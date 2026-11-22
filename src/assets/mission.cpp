#include "assets/mission.hpp"
#include "assets/sprite.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <jpeglib.h>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <zlib.h>

namespace ss2::assets {
namespace {
using BytesView = std::span<const std::uint8_t>;

constexpr std::array<std::uint16_t, 32> kCp1252Extra{
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 0x80..0x87
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, // 0x88..0x8F
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 0x90..0x97
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178, // 0x98..0x9F
};

bool isAsciiSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

std::string_view trimAscii(std::string_view s) {
    while (!s.empty() && isAsciiSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isAsciiSpace(s.back())) s.remove_suffix(1);
    return s;
}

char asciiLowerChar(char ch) {
    const auto u = static_cast<unsigned char>(ch);
    return (u >= 'A' && u <= 'Z') ? static_cast<char>(u + ('a' - 'A')) : ch;
}

std::string asciiLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) out.push_back(asciiLowerChar(ch));
    return out;
}

bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLowerChar(a[i]) != asciiLowerChar(b[i])) return false;
    }
    return true;
}

std::string_view cStringView(BytesView bytes) {
    const std::string_view raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto nul = raw.find('\0');
    return nul == std::string_view::npos ? raw : raw.substr(0, nul);
}

std::string firstLineTrimmed(std::string_view text) {
    text = trimAscii(text);
    const auto newline = text.find_first_of("\r\n");
    if (newline != std::string_view::npos) text = text.substr(0, newline);
    return std::string(trimAscii(text));
}

void appendUtf8CodePoint(std::string& out, std::uint16_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::size_t validUtf8Length(std::string_view s) {
    if (s.empty()) return 0;
    const auto b0 = static_cast<unsigned char>(s[0]);
    if (b0 < 0x80u) return 1;

    const auto isCont = [](unsigned char c) { return (c & 0xC0u) == 0x80u; };
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        if (s.size() >= 2 && isCont(static_cast<unsigned char>(s[1]))) return 2;
        return 0;
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        if (s.size() < 3) return 0;
        const auto b1 = static_cast<unsigned char>(s[1]);
        const auto b2 = static_cast<unsigned char>(s[2]);
        if (!isCont(b2)) return 0;
        if (b0 == 0xE0u) return (b1 >= 0xA0u && b1 <= 0xBFu) ? 3 : 0;
        if (b0 == 0xEDu) return (b1 >= 0x80u && b1 <= 0x9Fu) ? 3 : 0;
        return isCont(b1) ? 3 : 0;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u) {
        if (s.size() < 4) return 0;
        const auto b1 = static_cast<unsigned char>(s[1]);
        const auto b2 = static_cast<unsigned char>(s[2]);
        const auto b3 = static_cast<unsigned char>(s[3]);
        if (!isCont(b2) || !isCont(b3)) return 0;
        if (b0 == 0xF0u) return (b1 >= 0x90u && b1 <= 0xBFu) ? 4 : 0;
        if (b0 == 0xF4u) return (b1 >= 0x80u && b1 <= 0x8Fu) ? 4 : 0;
        return isCont(b1) ? 4 : 0;
    }
    return 0;
}

std::uint16_t little16(BytesView bytes) {
    if (bytes.size() < 2) throw std::runtime_error("Short mission field");
    return static_cast<std::uint16_t>(bytes[0] | (unsigned(bytes[1]) << 8));
}

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
    std::vector<JSAMPLE> row;
    ~Jpeg() { jpeg_destroy_decompress(&decoder); }
};

void decompressImpl(Jpeg& jpg, BytesView data, MapImage& image, bool allocateFromHeader) {
    auto& c = jpg.decoder;
    c.err = jpeg_std_error(&jpg.error);
    c.client_data = &jpg;
    jpg.error.error_exit = [](j_common_ptr cinfo) {
        std::longjmp(static_cast<Jpeg*>(cinfo->client_data)->failure, 1);
    };
    if (setjmp(jpg.failure)) throw std::runtime_error("Invalid mission JPEG");
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, data.data(), static_cast<unsigned long>(data.size()));
    jpeg_read_header(&c, TRUE);
    if (allocateFromHeader) {
        if (!c.image_width || !c.image_height || c.image_width > 8192 || c.image_height > 8192)
            throw std::runtime_error("Unsupported JPEG dimensions");
        image.width = c.image_width;
        image.height = c.image_height;
        image.rgba.resize(std::size_t(image.width) * image.height * 4);
        jpg.row.resize(std::size_t(image.width) * 3);
    } else if (c.image_width != image.width || c.image_height != image.height) {
        throw std::runtime_error("Mission overview dimensions disagree");
    }
    c.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c);
    if (c.output_width != image.width || c.output_height != image.height || c.output_components != 3)
        throw std::runtime_error("Unsupported mission JPEG color layout");
    while (c.output_scanline < c.output_height) {
        auto* out = image.rgba.data() + std::size_t(c.output_scanline) * image.width * 4;
        JSAMPROW line = jpg.row.data();
        if (jpeg_read_scanlines(&c, &line, 1) != 1) throw std::runtime_error("Short mission JPEG");
        for (unsigned x = 0; x < image.width; ++x, out += 4) {
            std::memcpy(out, jpg.row.data() + std::size_t(3) * x, 3);
            out[3] = 255;
        }
    }
    jpeg_finish_decompress(&c);
    if (jpg.error.num_warnings) throw std::runtime_error("Damaged mission JPEG");
}

void decompress(Jpeg& jpg, BytesView data, MapImage& image) {
    decompressImpl(jpg, data, image, false);
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
        if (bytes.size() + static_cast<std::size_t>(size) > (32u << 20))
            throw std::runtime_error("Mission exceeds size limit");
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + size);
    }
    if (gzclose(file.release()) != Z_OK) throw std::runtime_error("Incomplete compressed mission");
    return bytes;
}

std::string_view trimRecordName(std::string_view name) {
    if (const auto nul = name.find('\0'); nul != std::string_view::npos)
        name = name.substr(0, nul);
    while (!name.empty() && name.back() == ' ')
        name.remove_suffix(1);
    return name;
}

std::optional<BytesView> findRecordIn(BytesView rest, std::string_view expectedMagic, std::string_view wanted) {
    if (rest.size() < 8 || std::memcmp(rest.data(), expectedMagic.data(), 4) != 0 || little32(rest.subspan(4)) != 0x222u)
        throw std::runtime_error("Unsupported mission header");
    const std::string_view target = trimRecordName(wanted);
    rest = rest.subspan(8);
    std::optional<BytesView> found;
    while (!rest.empty()) {
        if (rest.size() < 16) throw std::runtime_error("Short mission record");
        const std::string_view name = trimRecordName(std::string_view(reinterpret_cast<const char*>(rest.data()), 12));
        const auto size = little32(rest.subspan(12));
        rest = rest.subspan(16);
        if (size > rest.size()) throw std::runtime_error("Invalid mission record size");
        if (name == target) found = rest.first(size);
        rest = rest.subspan(size);
    }
    return found;
}

std::string_view standaloneArchiveMagic(BytesView bytes) {
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "MM2S", 4) == 0) return "MM2S";
    return "MS2S";
}

std::optional<BytesView> findRecord(BytesView rest, std::string_view wanted) {
    return findRecordIn(rest, standaloneArchiveMagic(rest), wanted);
}

BytesView requireRecordIn(BytesView rest, std::string_view expectedMagic, std::string_view wanted) {
    const auto found = findRecordIn(rest, expectedMagic, wanted);
    if (!found) throw std::runtime_error("Missing mission record " + std::string(trimRecordName(wanted)));
    return *found;
}

struct MissionInfo {
    unsigned scheme{};
    unsigned width{};
    unsigned height{};
};

MissionInfo parseInfoIn(BytesView bytes, std::string_view magic) {
    const auto info = requireRecordIn(bytes, magic, "P-INFO");
    if (info.size() < 12) throw std::runtime_error("Invalid mission info record");
    const unsigned scheme = little32(info);
    const unsigned width = little32(info.subspan(4));
    const unsigned height = little32(info.subspan(8));
    if (width < 64 || width > 512 || (width % 64) != 0 ||
        height < 64 || height > 512 || (height % 64) != 0)
        throw std::runtime_error("Unsupported mission map dimensions");
    return {scheme, width, height};
}

MissionInfo parseInfo(BytesView bytes) {
    return parseInfoIn(bytes, standaloneArchiveMagic(bytes));
}

std::optional<std::filesystem::path> findChildFileIgnoreCase(const std::filesystem::path& dir, std::string_view wanted) {
    std::error_code ec;
    const auto exact = dir / wanted;
    if (std::filesystem::is_regular_file(exact, ec)) return exact;
    std::vector<std::filesystem::path> matches;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc)) continue;
        if (asciiEqualsIgnoreCase(it->path().filename().string(), wanted))
            matches.push_back(it->path());
    }
    if (matches.empty()) return std::nullopt;
    std::ranges::sort(matches);
    return matches.front();
}

std::string parseDescriptionIn(const MissionArchive& archive, bool firstLineOnly) {
    const auto desc = archive.findMissionRecord("I-DESC");
    if (!desc) return {};
    const std::string decoded = decodeMissionText(cStringView(*desc));
    if (firstLineOnly) return firstLineTrimmed(decoded);
    return std::string(trimAscii(decoded));
}

std::string parseDescription(BytesView bytes, bool firstLineOnly) {
    const auto desc = findRecord(bytes, "I-DESC");
    if (!desc) return {};
    const std::string decoded = decodeMissionText(cStringView(*desc));
    if (firstLineOnly) return firstLineTrimmed(decoded);
    return std::string(trimAscii(decoded));
}

std::string displayNameFromStem(std::string_view stem) {
    std::string_view s = trimAscii(stem);
    if (!s.empty() && s.front() == '\'') {
        s.remove_prefix(1);
        s = trimAscii(s);
    }
    constexpr std::string_view kTotalVictory = "[total victory]";
    if (s.size() >= kTotalVictory.size() &&
        asciiEqualsIgnoreCase(s.substr(s.size() - kTotalVictory.size()), kTotalVictory)) {
        s.remove_suffix(kTotalVictory.size());
        s = trimAscii(s);
    }
    while (!s.empty() && (s.front() == '-' || isAsciiSpace(s.front()))) s.remove_prefix(1);
    while (!s.empty() && (s.back() == '-' || isAsciiSpace(s.back()))) s.remove_suffix(1);
    return s.empty() ? std::string(stem) : std::string(s);
}

std::optional<std::filesystem::path> findChildDirIgnoreCase(const std::filesystem::path& dir, std::string_view wanted) {
    std::error_code ec;
    const auto exact = dir / wanted;
    if (std::filesystem::is_directory(exact, ec)) return exact;
    std::vector<std::filesystem::path> matches;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        if (asciiEqualsIgnoreCase(it->path().filename().string(), wanted))
            matches.push_back(it->path());
    }
    if (matches.empty()) return std::nullopt;
    std::ranges::sort(matches);
    return matches.front();
}

std::string joinCandidateNames(const std::vector<const MissionSummary*>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += items[i]->name;
    }
    return out;
}

void parseReinforcements(BytesView unamesRaw, BytesView support,
                         std::array<std::vector<ReinforcementUnit>, 64>& out) {
    if ((unamesRaw.size() % 16u) != 0)
        throw std::runtime_error("Invalid mission unit name table");
    std::vector<std::string_view> unames;
    unames.reserve(unamesRaw.size() / 16u);
    for (std::size_t off = 0; off < unamesRaw.size(); off += 16u)
        unames.push_back(cStringView(unamesRaw.subspan(off, 16u)));

    const auto resolveName = [&](std::uint16_t idx) -> std::string {
        if (idx >= unames.size() || unames[idx].empty())
            throw std::runtime_error("Invalid mission reinforcement unit name index");
        return std::string(unames[idx]);
    };

    for (auto& wave : out) {
        const auto unitCount = little32(support);
        support = support.subspan(4);
        if (std::size_t(unitCount) > support.size() / 12u)
            throw std::runtime_error("Invalid mission reinforcement unit count");
        wave.reserve(unitCount);
        for (std::uint32_t u = 0; u < unitCount; ++u) {
            if (support.size() < 12u)
                throw std::runtime_error("Truncated mission reinforcement unit");
            const int grp = support[0];
            const int hp = support[1];
            const int ammo = support[2];
            const int expa = support[3];
            const auto unameIdx = little16(support.subspan(6));
            const std::size_t nRiders = support[10];
            const std::size_t nCrew = support[11];
            const std::size_t passengerCount = nRiders + nCrew;
            support = support.subspan(12);

            if (passengerCount > support.size() / 10u)
                throw std::runtime_error("Truncated mission reinforcement passengers");
            std::vector<PassengerPlacement> passengers;
            passengers.reserve(passengerCount);
            for (std::size_t p = 0; p < passengerCount; ++p) {
                const int pgrp = support[0];
                const int php = support[1];
                const int pammo = support[2];
                const int pexpa = support[3];
                const auto puidx = little16(support.subspan(6));
                support = support.subspan(10);
                // Retail I-SUPPORT stores nCrew crew slots first (0..nCrew-1), followed by riders, matching I-UNITS.
                passengers.push_back({resolveName(puidx), pgrp, php, pammo, pexpa, p < nCrew});
            }
            wave.push_back({resolveName(unameIdx), grp, hp, ammo, expa, std::move(passengers)});
        }
    }
    if (!support.empty())
        throw std::runtime_error("Trailing mission reinforcement bytes");
}
std::optional<CampaignMissionSummary> findCampaignMissionInList(const std::vector<CampaignSummary>& campaigns,
                                                                std::string_view query,
                                                                bool easy) {
    const std::string_view q = trimAscii(query);
    if (q.empty() || campaigns.empty()) return std::nullopt;

    const auto applyDifficulty = [easy](CampaignMissionSummary m) {
        if (easy && !m.easyPath.empty()) m.path = m.easyPath;
        return m;
    };

    if (const auto colon = q.find(':'); colon != std::string_view::npos) {
        std::string campQ = asciiLower(trimAscii(q.substr(0, colon)));
        if (campQ == "american" || campQ == "usa") campQ = "us";
        else if (campQ == "soviet" || campQ == "ussr") campQ = "russian";
        else if (campQ == "uk") campQ = "british";
        const std::string_view idxStr = trimAscii(q.substr(colon + 1));
        std::size_t idx = 0;
        const auto [ptr, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
        if (ec != std::errc{} || ptr != idxStr.data() + idxStr.size() || idx == 0 || campQ.empty()) {
            return std::nullopt;
        }
        std::vector<const CampaignSummary*> prefixMatches;
        std::vector<const CampaignSummary*> substrMatches;
        for (const auto& c : campaigns) {
            const std::string cLower = asciiLower(c.name);
            if (cLower == campQ || cLower.starts_with(campQ)) {
                prefixMatches.push_back(&c);
            } else if (cLower.find(campQ) != std::string::npos) {
                substrMatches.push_back(&c);
            }
        }
        const auto& chosen = !prefixMatches.empty() ? prefixMatches : substrMatches;
        if (chosen.size() == 1 && idx <= chosen.front()->missions.size()) {
            return applyDifficulty(chosen.front()->missions[idx - 1]);
        }
        return std::nullopt;
    }

    const std::string qLower = asciiLower(q);
    for (const auto& c : campaigns) {
        for (const auto& m : c.missions) {
            if (asciiLower(m.code) == qLower || asciiLower(m.title) == qLower) {
                return applyDifficulty(m);
            }
        }
    }

    std::vector<const CampaignMissionSummary*> sub;
    for (const auto& c : campaigns) {
        for (const auto& m : c.missions) {
            if (asciiLower(m.title).find(qLower) != std::string::npos ||
                asciiLower(c.name).find(qLower) != std::string::npos) {
                sub.push_back(&m);
            }
        }
    }
    if (sub.empty()) return std::nullopt;
    if (sub.size() == 1) return applyDifficulty(*sub.front());
    return std::nullopt;
}
} // namespace

std::string resolveAmbientWooferSound(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (ch == '\0') break;
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch - 'A' + 'a');
        key.push_back(static_cast<char>(ch));
    }
    if (key.empty()) return {};
    if (key == "@wind_bunker") return "@wind_bunker_loop";
    if (key == "@wind_emptybuilding") return "@wind_emptyhouse";
    if (key == "@wind_hangar") return "@wind_hangar_loop";
    if (key == "@wind_grass") return "@wind_grass_loop";
    if (key == "@misc_birds") return "@birds";
    if (key == "@sea" || key == "@wind_plant") return key;
    if (key.front() != '@') return "@" + key;
    return key;
}

std::string decodeMissionText(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        if (const std::size_t len = validUtf8Length(raw.substr(i)); len > 0) {
            out.append(raw.substr(i, len));
            i += len;
            continue;
        }
        const auto byte = static_cast<unsigned char>(raw[i]);
        const std::uint16_t cp = byte < 0xA0u ? kCp1252Extra[byte - 0x80u] : static_cast<std::uint16_t>(byte);
        appendUtf8CodePoint(out, cp);
        ++i;
    }
    return out;
}

std::optional<BytesView> MissionArchive::findMapRecord(std::string_view wanted) const {
    return findRecordIn(mapBytes, isCampaign ? "P2SS" : (isMultiplayer ? "MM2S" : "MS2S"), wanted);
}

std::optional<BytesView> MissionArchive::findMissionRecord(std::string_view wanted) const {
    if (isCampaign) return findRecordIn(missionBytes, "I2SS", wanted);
    return findRecordIn(mapBytes, isMultiplayer ? "MM2S" : "MS2S", wanted);
}

BytesView MissionArchive::requireMapRecord(std::string_view wanted) const {
    return requireRecordIn(mapBytes, isCampaign ? "P2SS" : (isMultiplayer ? "MM2S" : "MS2S"), wanted);
}

BytesView MissionArchive::requireMissionRecord(std::string_view wanted) const {
    if (isCampaign) return requireRecordIn(missionBytes, "I2SS", wanted);
    return requireRecordIn(mapBytes, isMultiplayer ? "MM2S" : "MS2S", wanted);
}

MissionArchive loadMissionArchive(const std::filesystem::path& path) {
    auto bytes = readMission(path);
    if (bytes.size() < 8 || little32(BytesView(bytes).subspan(4)) != 0x222u)
        throw std::runtime_error("Unsupported mission header");

    MissionArchive out;
    if (std::memcmp(bytes.data(), "MS2S", 4) == 0 || std::memcmp(bytes.data(), "MM2S", 4) == 0) {
        const auto magic = standaloneArchiveMagic(bytes);
        const auto info = parseInfoIn(bytes, magic);
        out.isMultiplayer = (magic == "MM2S");
        out.scheme = info.scheme;
        out.baseWidth = info.width;
        out.baseHeight = info.height;
        out.width = info.width;
        out.height = info.height;
        out.mapBytes = std::move(bytes);
        return out;
    }

    if (std::memcmp(bytes.data(), "I2SS", 4) == 0) {
        out.isCampaign = true;
        out.missionBytes = std::move(bytes);

        const std::string stem = path.stem().string();
        if (stem.size() < 3) throw std::runtime_error("Invalid campaign mission filename");
        const std::string baseName = stem.substr(0, 3) + ".ssc";
        const auto parent = path.parent_path();
        auto basePath = findChildFileIgnoreCase(parent, baseName);
        if (!basePath && parent.has_parent_path()) {
            basePath = findChildFileIgnoreCase(parent.parent_path(), baseName);
        }
        if (!basePath) throw std::runtime_error("Missing campaign base map " + baseName);

        out.mapBytes = readMission(*basePath);
        const auto pInfo = parseInfoIn(out.mapBytes, "P2SS");
        out.scheme = pInfo.scheme;
        out.baseWidth = pInfo.width;
        out.baseHeight = pInfo.height;

        const auto cInfo = out.requireMissionRecord("I-CINFO");
        if (cInfo.size() < 16) throw std::runtime_error("Invalid campaign crop info record");
        out.offsetX = little32(cInfo);
        out.offsetY = little32(cInfo.subspan(4));
        out.width = little32(cInfo.subspan(8));
        out.height = little32(cInfo.subspan(12));
        if (out.width < 64 || out.width > 512 || (out.width % 64) != 0 ||
            out.height < 64 || out.height > 512 || (out.height % 64) != 0 ||
            out.offsetX + out.width > out.baseWidth ||
            out.offsetY + out.height > out.baseHeight) {
            throw std::runtime_error("Campaign crop region out of range");
        }
        return out;
    }

    throw std::runtime_error("Unsupported mission header");
}

MapImage decodeJpeg(std::span<const std::uint8_t> data) {
    if (data.empty()) throw std::runtime_error("Empty JPEG buffer");
    MapImage image;
    Jpeg jpg;
    decompressImpl(jpg, data, image, true);
    return image;
}

MapImage readMissionOverview(const std::filesystem::path& mission) {
    const auto archive = loadMissionArchive(mission);
    const auto overview = archive.requireMissionRecord("I-JPG1024");
    if (overview.size() <= 12) throw std::runtime_error("Missing mission overview header");
    const auto width = little32(overview);
    const auto height = little32(overview.subspan(4));
    const auto stride = little32(overview.subspan(8));
    if (!width || !height || width > 8192 || height > 8192 || (stride != 0 && stride < width * 3))
        throw std::runtime_error("Unsupported mission overview dimensions");
    MapImage image{width, height, std::vector<std::uint8_t>(std::size_t(width) * height * 4)};
    Jpeg jpg;
    jpg.row.resize(std::size_t(width) * 3);
    decompress(jpg, overview.subspan(12), image);
    return image;
}

MissionData readMissionData(const std::filesystem::path& mission) {
    const auto archive = loadMissionArchive(mission);
    const std::size_t baseCells = std::size_t(archive.baseWidth) * std::size_t(archive.baseHeight);
    const std::size_t cellCount = std::size_t(archive.width) * std::size_t(archive.height);

    const auto flags = archive.requireMapRecord("P-FLAGS");
    const auto obstacles = archive.requireMapRecord("P-OBSTACLES");
    if (flags.size() != baseCells * 4 || obstacles.size() != baseCells * 2)
        throw std::runtime_error("Invalid mission navigation layers");

    MissionData data;
    data.scheme = archive.scheme;
    data.width = archive.width;
    data.height = archive.height;
    data.description = parseDescriptionIn(archive, false);

    data.flags.reserve(cellCount);
    data.obstacles.reserve(cellCount);
    for (unsigned y = 0; y < archive.height; ++y) {
        const std::size_t rowOff = std::size_t(y + archive.offsetY) * archive.baseWidth + archive.offsetX;
        for (unsigned x = 0; x < archive.width; ++x) {
            const std::size_t srcIdx = rowOff + x;
            data.flags.push_back(little32(flags.subspan(4 * srcIdx)));
            data.obstacles.push_back(static_cast<std::uint16_t>(
                obstacles[2 * srcIdx] | (unsigned(obstacles[2 * srcIdx + 1]) << 8)));
        }
    }

    auto phrases = archive.requireMissionRecord("I-PHRASE2");
    const auto count = little32(phrases);
    phrases = phrases.subspan(4);
    if (count > 4096) throw std::runtime_error("Too many mission phrases");
    data.phrases.reserve(count);
    data.phraseSounds.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto soundLen = little32(phrases);
        phrases = phrases.subspan(4);
        if (soundLen > phrases.size() || soundLen > 65536) throw std::runtime_error("Invalid mission phrase sound");
        const std::string_view soundRaw(reinterpret_cast<const char*>(phrases.data()), soundLen);
        phrases = phrases.subspan(soundLen);

        const auto textLen = little32(phrases);
        phrases = phrases.subspan(4);
        if (textLen > phrases.size() || textLen > 65536) throw std::runtime_error("Invalid mission phrase");
        const std::string_view textRaw(reinterpret_cast<const char*>(phrases.data()), textLen);
        phrases = phrases.subspan(textLen);

        data.phraseSounds.push_back(decodeMissionText(soundRaw));
        data.phrases.push_back(decodeMissionText(textRaw));
    }

    auto scripts = archive.requireMissionRecord("I-SCRIPTS");
    const auto triggers = little32(scripts);
    if (triggers < 1 || triggers > 4096) throw std::runtime_error("Unsupported mission trigger count");
    scripts = scripts.subspan(4);
    data.scripts.reserve(triggers);
    for (unsigned i = 0; i < triggers; ++i) {
        const auto size = little32(scripts);
        scripts = scripts.subspan(4);
        if (size < 12 || size > scripts.size() || (size % 4) != 0 || std::memcmp(scripts.data(), "CAOF", 4) != 0)
            throw std::runtime_error("Invalid compiled trigger");
        std::vector<std::uint32_t> code;
        code.reserve((size - 4) / 4);
        for (std::size_t j = 4; j < size; j += 4) code.push_back(little32(scripts.subspan(j)));
        if (code.back() != 0x7fffffffu) throw std::runtime_error("Unterminated compiled trigger");
        data.scripts.push_back(std::move(code));
        scripts = scripts.subspan(size);
    }
    if (!scripts.empty()) throw std::runtime_error("Trailing trigger records");

    const auto zones = archive.requireMissionRecord("I-ZONES");
    if (zones.size() != cellCount) throw std::runtime_error("Invalid mission zone map size");
    data.zones.assign(zones.begin(), zones.end());

    if (const auto mines = archive.findMissionRecord("I-MINES"); mines && mines->size() == cellCount) {
        data.mines.assign(mines->begin(), mines->end());
    }
    if (const auto players = archive.findMissionRecord("I-PLAYERS"); players && players->size() >= 4u + 4u * 397u) {
        for (std::size_t p = 0; p < 4u; ++p) {
            const auto slot = players->subspan(4u + p * 397u, 397u);
            data.playerTeams[p] = static_cast<int>(slot[32u]);
            for (std::size_t k = 0; k < 5u; ++k) {
                const std::size_t off = 37u + k * 44u;
                std::string plane(cStringView(slot.subspan(off, 32u)));
                const int tot = std::clamp(static_cast<int>(little32(slot.subspan(off + 32u))), 0, 999);
                const int rdy = std::clamp(static_cast<int>(little32(slot.subspan(off + 36u))), 0, 999);
                data.airSupport[p][k] = {std::move(plane), tot, rdy};
            }
            data.paratrooperGroup[p] = static_cast<int>(little16(slot.subspan(257u)));
            data.paratrooperRoster[p].clear();
            for (std::size_t j = 0; j < 4u; ++j) {
                std::string unitId(cStringView(slot.subspan(259u + j * 16u, 16u)));
                const int cnt = static_cast<int>(slot[323u + j]);
                if (!unitId.empty() && cnt > 0) {
                    data.paratrooperRoster[p].push_back({std::move(unitId), cnt});
                }
            }
        }
    }

    std::array<std::int64_t, 64> sumsX{};
    std::array<std::int64_t, 64> sumsY{};
    std::array<int, 64> counts{};
    const int mapW = int(archive.width);
    const int mapH = int(archive.height);
    for (int y = 4; y < mapH - 4; ++y) {
        for (int x = 4; x < mapW - 4; ++x) {
            const auto zone = zones[std::size_t(y) * archive.width + std::size_t(x)] & 63u;
            ++counts[zone];
            sumsX[zone] += x;
            sumsY[zone] += y;
        }
    }
    const int originX = mapH * 32;
    for (std::size_t zone = 0; zone < counts.size(); ++zone) {
        const int x = zone && counts[zone] ? int(sumsX[zone] / counts[zone]) : mapW / 2;
        const int y = zone && counts[zone] ? int(sumsY[zone] / counts[zone]) : mapH / 2;
        data.locations[zone] = {originX + 32 * (x - y), 16 * (x + y + 1)};
    }

    const auto markers = archive.requireMissionRecord("I-OBJECTS");
    if (markers.size() != 400) throw std::runtime_error("Invalid mission marker table");
    for (std::size_t i = 0; i < 100; ++i) {
        const auto marker = little32(markers.subspan(i * 4));
        const int x = std::int16_t(marker & 0xffffu);
        const int y = std::int16_t(marker >> 16);
        data.markers[i] = {originX + 32 * (x - y), 16 * (x + y + 1)};
    }
    if (const auto gates = archive.findMissionRecord("I-FLAGS")) {
        for (std::size_t i = 0; i < data.gates.size() && (i + 1) * 20 <= gates->size(); ++i) {
            const int tx = static_cast<std::int32_t>(little32(gates->subspan(i * 20)));
            const int ty = static_cast<std::int32_t>(little32(gates->subspan(i * 20 + 4)));
            if (tx >= 0 && tx < mapW && ty >= 0 && ty < mapH) {
                data.gates[i] = {originX + 32 * (tx - ty), 16 * (tx + ty + 1)};
            }
            const int rx = static_cast<std::int32_t>(little32(gates->subspan(i * 20 + 8)));
            const int ry = static_cast<std::int32_t>(little32(gates->subspan(i * 20 + 12)));
            if (rx >= 0 && rx < mapW && ry >= 0 && ry < mapH) {
                data.gateTargets[i] = {originX + 32 * (rx - ry), 16 * (rx + ry + 1)};
            }
        }
    }
    if (const auto unames = archive.findMissionRecord("I-UNAMES")) {
        if (const auto support = archive.findMissionRecord("I-SUPPORT")) {
            parseReinforcements(*unames, *support, data.reinforcements);
        }
    }
    if (const auto woof = archive.findMissionRecord("I-WOOFERS"); woof && woof->size() >= 4u) {
        const std::size_t declared = little32(*woof);
        const std::size_t count = std::min(declared, (woof->size() - 4u) / 78u);
        data.woofers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t off = 4u + i * 78u;
            std::string name(cStringView(woof->subspan(off, 64u)));
            if (name.empty()) continue;
            const int u = little16(woof->subspan(off + 64u));
            const int v = little16(woof->subspan(off + 66u));
            const int rad = little16(woof->subspan(off + 68u));
            const std::uint32_t volBits = little32(woof->subspan(off + 70u));
            float vol = 0.2f;
            std::memcpy(&vol, &volBits, sizeof(float));
            if (!std::isfinite(vol) || vol <= 0.0f) vol = 0.2f;
            else vol = std::clamp(vol, 0.05f, 1.0f);
            const unsigned minMs = little16(woof->subspan(off + 74u));
            const unsigned maxMs = little16(woof->subspan(off + 76u));
            const int mx = originX + u - v;
            const int my = (u + v) / 2;
            data.woofers.push_back({std::move(name), mx, my, rad > 0 ? rad : 32, vol, minMs, maxMs});
        }
    }
    if (const auto xinf = archive.findMissionRecord("I-MISXINF"); xinf && xinf->size() >= 4u) {
        const std::uint32_t rawWeather = little32(*xinf);
        data.weather = (rawWeather < 4u) ? static_cast<WeatherKind>(rawWeather) : WeatherKind::Clear;
    }
    return data;
}

namespace {
std::vector<MissionSummary> listMissionsInSubdir(const std::filesystem::path& gameDir,
                                                 std::string_view subdirName,
                                                 std::initializer_list<std::string_view> extensions) {
    const auto mapsDir = findChildDirIgnoreCase(gameDir, "maps");
    if (!mapsDir) return {};
    const auto targetDir = findChildDirIgnoreCase(*mapsDir, subdirName);
    if (!targetDir) return {};

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(*targetDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;
        const std::string ext = it->path().extension().string();
        if (std::ranges::any_of(extensions, [&](std::string_view want) { return asciiEqualsIgnoreCase(ext, want); }))
            files.push_back(it->path());
    }
    std::ranges::sort(files);

    std::vector<MissionSummary> out;
    for (const auto& file : files) {
        try {
            const auto bytes = readMission(file);
            const auto info = parseInfo(bytes);
            out.push_back({
                file,
                displayNameFromStem(file.stem().string()),
                parseDescription(bytes, true),
                info.width,
                info.height,
                info.scheme,
            });
        } catch (const std::exception&) {
            // Skip unreadable or malformed mission archives in the directory scan.
        }
    }

    std::ranges::sort(out, [](const MissionSummary& a, const MissionSummary& b) {
        const auto nameA = asciiLower(a.name);
        const auto nameB = asciiLower(b.name);
        if (nameA != nameB) return nameA < nameB;
        const auto fileA = asciiLower(a.path.filename().string());
        const auto fileB = asciiLower(b.path.filename().string());
        if (fileA != fileB) return fileA < fileB;
        return a.path.filename().string() < b.path.filename().string();
    });

    std::map<std::string, std::size_t> nameCounts;
    for (const auto& m : out) ++nameCounts[asciiLower(m.name)];

    std::map<std::pair<std::string, std::string>, std::size_t> nameStemCounts;
    for (const auto& m : out) {
        if (nameCounts[asciiLower(m.name)] > 1)
            ++nameStemCounts[{asciiLower(m.name), asciiLower(m.path.stem().string())}];
    }

    std::map<std::string, std::size_t> finalNameCounts;
    for (auto& m : out) {
        const auto key = asciiLower(m.name);
        if (nameCounts[key] > 1) {
            const auto stem = m.path.stem().string();
            if (nameStemCounts[{key, asciiLower(stem)}] == 1) {
                m.name += " (" + stem + ")";
            } else {
                m.name += " [" + std::to_string(m.width) + "x" + std::to_string(m.height) + "]";
            }
        }
        ++finalNameCounts[asciiLower(m.name)];
    }
    for (auto& m : out) {
        if (finalNameCounts[asciiLower(m.name)] > 1)
            m.name += " (" + m.path.filename().string() + ")";
    }

    return out;
}

std::optional<MissionSummary> findMissionInList(const std::vector<MissionSummary>& missions, std::string_view query) {
    const std::string_view q = trimAscii(query);
    if (q.empty() || missions.empty()) return std::nullopt;

    const bool allDigits = std::ranges::all_of(q, [](char c) { return c >= '0' && c <= '9'; });
    if (allDigits) {
        std::size_t index = 0;
        const auto [ptr, ec] = std::from_chars(q.data(), q.data() + q.size(), index);
        if (ec != std::errc{} || ptr != q.data() + q.size() || index == 0 || index > missions.size())
            return std::nullopt;
        return missions[index - 1];
    }

    const std::string qLower = asciiLower(q);
    std::vector<const MissionSummary*> exact;
    for (const auto& m : missions) {
        const std::string stem = m.path.stem().string();
        const std::string file = m.path.filename().string();
        if (asciiLower(m.name) == qLower || asciiLower(stem) == qLower || asciiLower(file) == qLower)
            exact.push_back(&m);
    }
    if (exact.size() == 1) return *exact.front();
    if (exact.size() > 1) {
        std::vector<const MissionSummary*> exactCase;
        for (const auto* m : exact) {
            if (m->name == q || m->path.stem().string() == q || m->path.filename().string() == q)
                exactCase.push_back(m);
        }
        if (exactCase.size() == 1) return *exactCase.front();
        throw std::invalid_argument("Ambiguous mission query '" + std::string(q) + "': " + joinCandidateNames(exact));
    }

    std::vector<const MissionSummary*> sub;
    for (const auto& m : missions) {
        const std::string stemLower = asciiLower(m.path.stem().string());
        if (asciiLower(m.name).find(qLower) != std::string::npos || stemLower.find(qLower) != std::string::npos)
            sub.push_back(&m);
    }
    if (sub.empty()) return std::nullopt;
    if (sub.size() == 1) return *sub.front();
    throw std::invalid_argument("Ambiguous mission query '" + std::string(q) + "': " + joinCandidateNames(sub));
}
} // namespace

std::vector<MissionSummary> listSinglePlayerMissions(const std::filesystem::path& gameDir) {
    return listMissionsInSubdir(gameDir, "single", {".ssm"});
}

std::optional<MissionSummary> findSinglePlayerMission(const std::filesystem::path& gameDir, std::string_view query) {
    return findMissionInList(listSinglePlayerMissions(gameDir), query);
}

std::vector<MissionSummary> listMultiplayerMissions(const std::filesystem::path& gameDir) {
    return listMissionsInSubdir(gameDir, "mplay", {".smm", ".ssm"});
}

std::optional<MissionSummary> findMultiplayerMission(const std::filesystem::path& gameDir, std::string_view query) {
    return findMissionInList(listMultiplayerMissions(gameDir), query);
}

std::vector<CampaignSummary> listCampaigns(const std::filesystem::path& gameDir) {
    const auto mapsDir = findChildDirIgnoreCase(gameDir, "maps");
    if (!mapsDir) return {};
    const auto campaignDir = findChildDirIgnoreCase(*mapsDir, "campaign");
    if (!campaignDir) return {};
    const auto easyDir = findChildDirIgnoreCase(*campaignDir, "EASY");

    const auto loadSummary = [&](const std::string& code, const std::string& title)
        -> std::optional<CampaignMissionSummary> {
        const auto sscPath = findChildFileIgnoreCase(*campaignDir, code + ".ssc");
        if (!sscPath) return std::nullopt;
        try {
            const auto archive = loadMissionArchive(*sscPath);
            std::filesystem::path easyPath;
            if (easyDir) {
                if (const auto ep = findChildFileIgnoreCase(*easyDir, code + ".ssc")) {
                    easyPath = *ep;
                }
            }
            std::string description = parseDescriptionIn(archive, false);
            return CampaignMissionSummary{
                code, title.empty() ? code : title, *sscPath, std::move(easyPath),
                archive.width, archive.height, archive.scheme, std::move(description)
            };
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    std::vector<CampaignSummary> campaigns;
    try {
        const auto langArchive = resolveCaseInsensitivePath(gameDir, "lang.sue");
        std::error_code ec;
        if (std::filesystem::is_regular_file(langArchive, ec)) {
            const auto seqBytes = readSueEntry(langArchive, "CAMPAIGN.SEQ");
            const std::string seqText = decodeMissionText(
                std::string_view(reinterpret_cast<const char*>(seqBytes.data()), seqBytes.size()));
            std::size_t pos = 0;
            while (pos < seqText.size()) {
                std::size_t eol = seqText.find('\n', pos);
                if (eol == std::string_view::npos) eol = seqText.size();
                const std::string_view line = trimAscii(std::string_view(seqText).substr(pos, eol - pos));
                pos = eol + 1;
                if (line.empty()) continue;
                if (line.front() == '>') {
                    const std::string cname(trimAscii(line.substr(1)));
                    if (!cname.empty()) campaigns.push_back({cname, {}});
                    continue;
                }
                const auto gt = line.find('>');
                if (gt != std::string_view::npos && !campaigns.empty()) {
                    const std::string code(trimAscii(line.substr(0, gt)));
                    const std::string title(trimAscii(line.substr(gt + 1)));
                    if (code.size() == 6) {
                        if (auto summary = loadSummary(code, title)) {
                            campaigns.back().missions.push_back(std::move(*summary));
                        }
                    }
                }
            }
            std::erase_if(campaigns, [](const CampaignSummary& c) { return c.missions.empty(); });
            if (!campaigns.empty()) return campaigns;
        }
    } catch (const std::exception&) {
        campaigns.clear();
    }

    // Fallback when CAMPAIGN.SEQ is unavailable: enumerate NNNMMM.ssc files directly.
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(*campaignDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;
        if (asciiEqualsIgnoreCase(it->path().extension().string(), ".ssc") &&
            it->path().stem().string().size() == 6) {
            files.push_back(it->path());
        }
    }
    std::ranges::sort(files);
    if (files.empty()) return {};

    CampaignSummary fallback{"Campaign", {}};
    for (const auto& f : files) {
        const std::string code = f.stem().string();
        try {
            const auto archive = loadMissionArchive(f);
            std::string title = parseDescriptionIn(archive, true);
            if (title.empty()) title = code;
            std::filesystem::path easyPath;
            if (easyDir) {
                if (const auto ep = findChildFileIgnoreCase(*easyDir, code + ".ssc")) {
                    easyPath = *ep;
                }
            }
            std::string description = parseDescriptionIn(archive, false);
            fallback.missions.push_back({
                code, std::move(title), f, std::move(easyPath),
                archive.width, archive.height, archive.scheme, std::move(description)
            });
        } catch (const std::exception&) {}
    }
    if (!fallback.missions.empty()) campaigns.push_back(std::move(fallback));
    return campaigns;
}

std::optional<CampaignMissionSummary> findCampaignMission(const std::filesystem::path& gameDir,
                                                          std::string_view query,
                                                          bool easy) {
    return findCampaignMissionInList(listCampaigns(gameDir), query, easy);
}

std::vector<CampaignSummary> listCustomCampaigns(const std::filesystem::path& gameDir,
                                                 const std::filesystem::path& userDir) {
    std::vector<std::filesystem::path> candidateRoots;
    std::error_code ec;
    if (!gameDir.empty()) {
        if (const auto mapsDir = findChildDirIgnoreCase(gameDir, "maps")) {
            if (const auto ccDir = findChildDirIgnoreCase(*mapsDir, "custom_campaign");
                ccDir && std::filesystem::is_directory(*ccDir, ec) && !ec) {
                candidateRoots.push_back(*ccDir);
            }
        }
    }
    if (!userDir.empty() && std::filesystem::is_directory(userDir, ec) && !ec) {
        candidateRoots.push_back(userDir);
    }

    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> canonicalSeen;
    for (const auto& r : candidateRoots) {
        std::error_code cEc;
        auto can = std::filesystem::weakly_canonical(r, cEc);
        if (cEc) can = r;
        if (std::ranges::find(canonicalSeen, can) == canonicalSeen.end()) {
            canonicalSeen.push_back(std::move(can));
            roots.push_back(r);
        }
    }

    auto loadFileSummary = [](const std::filesystem::path& filePath,
                              const std::optional<std::filesystem::path>& easyDir,
                              const std::string& codeOverride = {},
                              const std::string& titleOverride = {})
        -> std::optional<CampaignMissionSummary> {
        try {
            const auto archive = loadMissionArchive(filePath);
            std::filesystem::path easyPath;
            if (easyDir) {
                if (const auto ep = findChildFileIgnoreCase(*easyDir, filePath.filename().string())) {
                    easyPath = *ep;
                }
            }
            std::string title = titleOverride;
            if (title.empty()) title = parseDescriptionIn(archive, true);
            if (title.empty()) title = displayNameFromStem(filePath.stem().string());
            std::string code = codeOverride.empty() ? filePath.stem().string() : codeOverride;
            std::string description = parseDescriptionIn(archive, false);
            return CampaignMissionSummary{
                std::move(code), std::move(title), filePath, std::move(easyPath),
                archive.width, archive.height, archive.scheme, std::move(description)
            };
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    auto scanMissionDir = [&](const std::filesystem::path& dir, const std::string& fallbackName) {
        std::vector<CampaignSummary> found;
        const auto easyDir = findChildDirIgnoreCase(dir, "EASY");

        if (const auto seqPath = findChildFileIgnoreCase(dir, "campaign.seq")) {
            std::ifstream seqIn(*seqPath, std::ios::binary);
            if (seqIn) {
                std::ostringstream ss;
                ss << seqIn.rdbuf();
                const std::string seqText = decodeMissionText(ss.str());
                std::size_t pos = 0;
                while (pos < seqText.size()) {
                    std::size_t eol = seqText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = seqText.size();
                    const std::string_view line = trimAscii(std::string_view(seqText).substr(pos, eol - pos));
                    pos = eol + 1;
                    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
                    if (line.front() == '>') {
                        const std::string cname(trimAscii(line.substr(1)));
                        if (!cname.empty()) found.push_back({cname, {}});
                        continue;
                    }
                    const auto gt = line.find('>');
                    const bool hasTitle = (gt != std::string_view::npos);
                    const std::string code(trimAscii(hasTitle ? line.substr(0, gt) : line));
                    const std::string title = hasTitle ? std::string(trimAscii(line.substr(gt + 1))) : std::string{};
                    if (code.empty()) continue;
                    if (found.empty()) found.push_back({fallbackName, {}});
                    std::optional<std::filesystem::path> mFile;
                    if (hasTitle) {
                        mFile = findChildFileIgnoreCase(dir, code + ".ssc");
                        if (!mFile) mFile = findChildFileIgnoreCase(dir, code + ".ssm");
                        if (!mFile) mFile = findChildFileIgnoreCase(dir, code);
                    } else {
                        mFile = findChildFileIgnoreCase(dir, code);
                        if (!mFile) mFile = findChildFileIgnoreCase(dir, code + ".ssc");
                        if (!mFile) mFile = findChildFileIgnoreCase(dir, code + ".ssm");
                    }
                    if (mFile) {
                        const std::string resolvedCode = hasTitle ? code : mFile->stem().string();
                        if (auto sum = loadFileSummary(*mFile, easyDir, resolvedCode, title)) {
                            found.back().missions.push_back(std::move(*sum));
                        }
                    }
                }
                std::erase_if(found, [](const CampaignSummary& c) { return c.missions.empty(); });
                if (!found.empty()) return found;
            }
        }

        std::vector<std::filesystem::path> files;
        std::error_code fEc;
        for (std::filesystem::directory_iterator it(dir, fEc), end; !fEc && it != end; it.increment(fEc)) {
            std::error_code regEc;
            if (!it->is_regular_file(regEc)) continue;
            const std::string ext = it->path().extension().string();
            if (asciiEqualsIgnoreCase(ext, ".ssc") || asciiEqualsIgnoreCase(ext, ".ssm")) {
                files.push_back(it->path());
            }
        }
        std::ranges::sort(files);
        if (!files.empty()) {
            CampaignSummary cs{fallbackName, {}};
            for (const auto& f : files) {
                if (auto sum = loadFileSummary(f, easyDir)) {
                    cs.missions.push_back(std::move(*sum));
                }
            }
            if (!cs.missions.empty()) found.push_back(std::move(cs));
        }
        return found;
    };

    std::vector<CampaignSummary> out;
    for (const auto& root : roots) {
        std::vector<std::filesystem::path> subdirs;
        std::error_code dEc;
        for (std::filesystem::directory_iterator it(root, dEc), end; !dEc && it != end; it.increment(dEc)) {
            std::error_code isDirEc;
            if (!it->is_directory(isDirEc)) continue;
            const std::string dirName = it->path().filename().string();
            if (asciiEqualsIgnoreCase(dirName, "EASY")) continue;
            subdirs.push_back(it->path());
        }
        std::ranges::sort(subdirs);
        for (const auto& sub : subdirs) {
            auto camps = scanMissionDir(sub, sub.filename().string());
            for (auto& c : camps) out.push_back(std::move(c));
        }

        // Also include loose .ssc/.ssm missions placed directly under root as "Custom Campaign".
        const auto rootEasyDir = findChildDirIgnoreCase(root, "EASY");
        std::vector<std::filesystem::path> rootFiles;
        std::error_code rfEc;
        for (std::filesystem::directory_iterator it(root, rfEc), end; !rfEc && it != end; it.increment(rfEc)) {
            std::error_code regEc;
            if (!it->is_regular_file(regEc)) continue;
            const std::string ext = it->path().extension().string();
            if (asciiEqualsIgnoreCase(ext, ".ssc") || asciiEqualsIgnoreCase(ext, ".ssm")) {
                rootFiles.push_back(it->path());
            }
        }
        std::ranges::sort(rootFiles);
        if (!rootFiles.empty()) {
            CampaignSummary loose{"Custom Campaign", {}};
            for (const auto& f : rootFiles) {
                if (auto sum = loadFileSummary(f, rootEasyDir)) {
                    loose.missions.push_back(std::move(*sum));
                }
            }
            if (!loose.missions.empty()) out.push_back(std::move(loose));
        }
    }

    std::ranges::sort(out, [](const CampaignSummary& a, const CampaignSummary& b) {
        const auto la = asciiLower(a.name);
        const auto lb = asciiLower(b.name);
        if (la != lb) return la < lb;
        return a.name < b.name;
    });
    return out;
}

std::optional<CampaignMissionSummary> findCustomCampaignMission(const std::filesystem::path& gameDir,
                                                                std::string_view query,
                                                                bool easy,
                                                                const std::filesystem::path& userDir) {
    return findCampaignMissionInList(listCustomCampaigns(gameDir, userDir), query, easy);
}

}
