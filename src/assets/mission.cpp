#include "assets/mission.hpp"

#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <jpeglib.h>
#include <memory>
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
}

MapImage readMissionOverview(const std::filesystem::path& mission) {
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
    BytesView rest(bytes);
    if (rest.size() < 8 || std::memcmp(rest.data(), "MS2S", 4) || little32(rest.subspan(4)) != 0x222)
        throw std::runtime_error("Unsupported mission header");
    rest = rest.subspan(8);
    BytesView overview;
    while (!rest.empty()) {
        if (rest.size() < 16) throw std::runtime_error("Short mission record");
        const std::string_view name(reinterpret_cast<const char*>(rest.data()), 12);
        const auto size = little32(rest.subspan(12));
        rest = rest.subspan(16);
        if (size > rest.size()) throw std::runtime_error("Invalid mission record size");
        if (name == "I-JPG1024   ") overview = rest.first(size);
        rest = rest.subspan(size);
    }
    if (overview.size() <= 12 || little32(overview) != 1024 || little32(overview.subspan(4)) != 512 ||
        little32(overview.subspan(8)) != 3072)
        throw std::runtime_error("Mission has no supported 1024 x 512 overview");
    MapImage image{1024, 512, std::vector<std::uint8_t>(1024 * 512 * 4)};
    Jpeg jpg;
    decompress(jpg, overview.subspan(12), image);
    return image;
}
}
