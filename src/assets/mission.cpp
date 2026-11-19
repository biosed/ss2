#include "assets/mission.hpp"

#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <jpeglib.h>
#include <memory>
#include <algorithm>
#include <span>
#include <stdexcept>
#include <string_view>
#include <zlib.h>

namespace ss2::assets {
namespace {
using BytesView = std::span<const std::uint8_t>;

std::uint32_t little32(BytesView bytes) {
    if (bytes.size() < 4) throw std::runtime_error("Short mission field");
    return bytes[0] | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) |
           (std::uint32_t(bytes[3]) << 24);
}

// libjpeg reports corrupt input via a C callback. Keep decoder state and output
// outside the setjmp frame, and bound the JPEG dimensions before decoding pixels.
struct Jpeg {
    jpeg_error_mgr error{};
    jpeg_decompress_struct decoder{};
    std::jmp_buf failure;
    ~Jpeg() { jpeg_destroy_decompress(&decoder); }
};

void decompress(Jpeg& jpg, BytesView data, MapImage& image) {
    auto& c = jpg.decoder;
    c.err = jpeg_std_error(&jpg.error);
    c.client_data = &jpg;
    jpg.error.error_exit = [](j_common_ptr cinfo) {
        std::longjmp(static_cast<Jpeg*>(cinfo->client_data)->failure, 1);
    };
    if (setjmp(jpg.failure)) throw std::runtime_error("Invalid mission JPEG");
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, data.data(), data.size());
    jpeg_read_header(&c, TRUE);
    if (c.image_width != image.width || c.image_height != image.height)
        throw std::runtime_error("Mission overview dimensions disagree");
    c.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c);
    std::array<JSAMPLE, 1024 * 3> row{};
    while (c.output_scanline < c.output_height) {
        auto* out = image.rgba.data() + std::size_t(c.output_scanline) * image.width * 4;
        JSAMPROW line = row.data();
        if (jpeg_read_scanlines(&c, &line, 1) != 1) throw std::runtime_error("Short mission JPEG");
        for (unsigned x = 0; x < image.width; ++x, out += 4) {
            std::memcpy(out, row.data() + 3 * x, 3);
            out[3] = 255;
        }
    }
    jpeg_finish_decompress(&c);
    if (jpg.error.num_warnings) throw std::runtime_error("Damaged mission JPEG");
}

std::vector<std::uint8_t> readMission(const std::filesystem::path& mission) {
    std::unique_ptr<gzFile_s, decltype(&gzclose)> file(gzopen(mission.c_str(), "rb"), gzclose);
    if (!file || gzdirect(file.get())) throw std::runtime_error("Expected a gzip-compressed mission");
    std::array<std::uint8_t, 65536> buffer;
    std::vector<std::uint8_t> bytes;
    for (;;) {
        const int size = gzread(file.get(), buffer.data(), buffer.size());
        if (size < 0) throw std::runtime_error("Invalid compressed mission");
        if (!size) break;
        if (bytes.size() + size > (32u << 20)) throw std::runtime_error("Mission exceeds size limit");
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + size);
    }
    if (gzclose(file.release()) != Z_OK) throw std::runtime_error("Incomplete compressed mission");
    return bytes;
}

BytesView record(BytesView rest, std::string_view wanted) {
    if (rest.size() < 8 || std::memcmp(rest.data(), "MS2S", 4) || little32(rest.subspan(4)) != 0x222)
        throw std::runtime_error("Unsupported mission header");
    rest = rest.subspan(8);
    BytesView found;
    while (!rest.empty()) {
        if (rest.size() < 16) throw std::runtime_error("Short mission record");
        const std::string_view name(reinterpret_cast<const char*>(rest.data()), 12);
        const auto size = little32(rest.subspan(12));
        rest = rest.subspan(16);
        if (size > rest.size()) throw std::runtime_error("Invalid mission record size");
        if (name == wanted) found = rest.first(size);
        rest = rest.subspan(size);
    }
    if (found.empty()) throw std::runtime_error("Missing mission record " + std::string(wanted));
    return found;
}
}

MapImage readMissionOverview(const std::filesystem::path& mission) {
    const auto bytes = readMission(mission);
    const auto overview = record(bytes, "I-JPG1024   ");
    if (overview.size() <= 12 || little32(overview) != 1024 || little32(overview.subspan(4)) != 512 ||
        little32(overview.subspan(8)) != 3072)
        throw std::runtime_error("Mission has no supported 1024 x 512 overview");
    MapImage image{1024, 512, std::vector<std::uint8_t>(1024 * 512 * 4)};
    Jpeg jpg;
    decompress(jpg, overview.subspan(12), image);
    return image;
}
TutorialData readTutorialData(const std::filesystem::path& mission) {
    const auto bytes = readMission(mission);
    TutorialData data;
    const auto info = record(bytes, "P-INFO      ");
    const auto flags = record(bytes, "P-FLAGS     ");
    const auto obstacles = record(bytes, "P-OBSTACLES ");
    if (info.size() < 12 || little32(info.subspan(4)) != 128 || little32(info.subspan(8)) != 128 ||
        flags.size() != 128*128*4 || obstacles.size() != 128*128*2)
        throw std::runtime_error("Invalid Tutorial I navigation layers");
    for (size_t i = 0; i < 128*128; ++i) {
        data.flags.push_back(little32(flags.subspan(4*i)));
        data.obstacles.push_back(obstacles[2*i] | (unsigned(obstacles[2*i+1]) << 8));
    }
    auto phrases = record(bytes, "I-PHRASE2   ");
    const auto count = little32(phrases);
    phrases = phrases.subspan(4);
    if (count > 1024) throw std::runtime_error("Too many mission phrases");
    for (unsigned i = 0; i < count; ++i) {
        if (phrases.size() < 8) throw std::runtime_error("Short phrase header");
        const auto size = little32(phrases.subspan(4));
        phrases = phrases.subspan(8);
        if (size > phrases.size() || size > 65536) throw std::runtime_error("Invalid mission phrase");
        data.phrases.emplace_back(reinterpret_cast<const char*>(phrases.data()), size);
        phrases = phrases.subspan(size);
    }
    auto scripts = record(bytes, "I-SCRIPTS   ");
    const auto triggers = little32(scripts);
    if (triggers != 30) throw std::runtime_error("Expected Gold Tutorial I trigger table");
    scripts = scripts.subspan(4);
    for (unsigned i = 0; i < triggers; ++i) {
        const auto size = little32(scripts);
        scripts = scripts.subspan(4);
        if (size < 12 || size > scripts.size() || size % 4 || std::memcmp(scripts.data(), "CAOF", 4))
            throw std::runtime_error("Invalid compiled trigger");
        std::vector<std::uint32_t> code;
        for (size_t j = 4; j < size; j += 4) code.push_back(little32(scripts.subspan(j)));
        if (code.back() != 0x7fffffff) throw std::runtime_error("Unterminated compiled trigger");
        data.scripts.push_back(std::move(code));
        scripts = scripts.subspan(size);
    }
    if (!scripts.empty()) throw std::runtime_error("Trailing trigger records");
    const auto zones = record(bytes, "I-ZONES     ");
    if (zones.size() != 128 * 128) throw std::runtime_error("Expected 128 x 128 tutorial zones");
    std::ranges::copy(zones, data.zones.begin());
    std::array<int, 64> counts{};
    for (int y = 4; y < 124; ++y) {
        for (int x = 4; x < 124; ++x) {
            const auto zone = zones[y * 128 + x] & 63;
            ++counts[zone];
            data.locations[zone].first += x;
            data.locations[zone].second += y;
        }
    }
    for (size_t zone = 0; zone < counts.size(); ++zone) {
        const auto [sumX, sumY] = data.locations[zone];
        const int x = zone && counts[zone] ? sumX / counts[zone] : 64;
        const int y = zone && counts[zone] ? sumY / counts[zone] : 64;
        data.locations[zone] = {4096 + 32*(x-y), 16*(x+y+1)};
    }
    const auto markers = record(bytes, "I-OBJECTS   ");
    if (markers.size() != 400) throw std::runtime_error("Invalid tutorial marker table");
    for (size_t i = 0; i < 100; ++i) {
        const auto marker = little32(markers.subspan(i*4));
        const int x = std::int16_t(marker & 0xffff), y = std::int16_t(marker >> 16);
        data.markers[i] = {4096 + 32*(x-y), 16*(x+y+1)};
    }
    return data;
}

}
