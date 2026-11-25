#include "assets/briefing_media.hpp"
#include "assets/infantry.hpp"
#include "assets/mission.hpp"
#include "assets/placement.hpp"
#include "assets/scenery.hpp"
#include "assets/sound_bank.hpp"
#include "assets/sprite.hpp"
#include "assets/terrain.hpp"
#include "assets/unit_catalog.hpp"
#include "assets/vehicles.hpp"
#include <jpeglib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <zlib.h>

namespace {

using Bytes = ss2::assets::Bytes;

void check(bool condition, const std::string& what) {
    if (!condition) {
        throw std::runtime_error(what);
    }
}

bool sameExistingPath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    return !a.empty() && !b.empty() && std::filesystem::equivalent(a, b, ec) && !ec;
}

template <class Fn>
void checkThrows(Fn&& fn, std::string_view expectedWhat, const std::string& label) {
    try {
        fn();
    } catch (const std::invalid_argument& e) {
        if (!expectedWhat.empty() && std::string_view(e.what()).find(expectedWhat) == std::string_view::npos) {
            throw std::runtime_error(label + ": unexpected invalid_argument message: " + e.what());
        }
        return;
    } catch (const std::exception& e) {
        throw std::runtime_error(label + ": expected std::invalid_argument, got: " + e.what());
    }
    throw std::runtime_error(label + ": expected exception, but none was thrown");
}

template <class Ex, class Fn>
void checkThrowsType(Fn&& fn, const std::string& label) {
    try {
        fn();
    } catch (const Ex&) {
        return;
    } catch (const std::exception& e) {
        throw std::runtime_error(label + ": unexpected exception type: " + e.what());
    }
    throw std::runtime_error(label + ": expected exception, but none was thrown");
}

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(std::string_view prefix) {
        std::random_device rd;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                                   (std::string(prefix) + "_" + std::to_string(stamp) + "_" + std::to_string(rd()));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("Failed to create temporary directory");
    }

    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void appendU16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void appendU32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void appendStr(Bytes& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
}

void appendRecord(Bytes& out, std::string_view name, std::span<const std::uint8_t> payload) {
    if (name.size() > 12) {
        throw std::runtime_error("Mission record name exceeds 12 bytes");
    }
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), 12 - name.size(), static_cast<std::uint8_t>(' '));
    appendU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

struct SyntheticPassenger {
    std::uint8_t group{0}, hp{100}, ammo{100}, expa{0}, lives{1}, morale{100};
    std::uint16_t unameIdx{0};
};

struct SyntheticSupportUnit {
    std::uint8_t group{0}, hp{100}, ammo{100}, expa{0}, lives{1}, morale{100};
    std::uint16_t unameIdx{0};
    std::uint8_t riders{0}, crew{0};
    std::vector<SyntheticPassenger> passengers{};
};

struct SupportSpec {
    std::vector<std::string> unames;
    std::optional<std::array<std::vector<SyntheticSupportUnit>, 64>> supportWaves;
    std::optional<Bytes> rawSupportOverride;
};

struct MissionSpec {
    std::uint32_t scheme{0};
    std::uint32_t width{128};
    std::uint32_t height{128};
    std::optional<std::string> rawDescription;
    std::vector<std::pair<std::string, std::string>> phrases;
    std::size_t phraseTrailingZeros{0};
    std::uint32_t triggerCount{1};
    bool emitFlags20{false};
    std::optional<std::uint32_t> rawMisxinfWeather{};
};

Bytes buildSyntheticMissionBytes(const MissionSpec& spec, const SupportSpec* supportSpec = nullptr) {
    Bytes out;
    appendStr(out, "MS2S");
    appendU32(out, 0x222u);

    Bytes info;
    appendU32(info, spec.scheme);
    appendU32(info, spec.width);
    appendU32(info, spec.height);
    appendRecord(out, "P-INFO", info);

    if (spec.rawDescription.has_value()) {
        const auto& desc = *spec.rawDescription;
        appendRecord(out, "I-DESC",
                     std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(desc.data()), desc.size()));
    }

    const std::size_t cellCount = std::size_t(spec.width) * std::size_t(spec.height);

    Bytes flags;
    flags.reserve(cellCount * 4);
    Bytes obstacles;
    obstacles.reserve(cellCount * 2);
    Bytes zones(cellCount, 0);
    for (std::size_t i = 0; i < cellCount; ++i) {
        const auto flagVal = static_cast<std::uint32_t>((i * 2654435761u) ^ (spec.scheme + 1u));
        const auto obsVal = static_cast<std::uint16_t>((i * 73u + spec.width) & 0xFFFFu);
        appendU32(flags, flagVal);
        appendU16(obstacles, obsVal);
    }
    if (spec.width >= 64 && spec.height >= 64) {
        zones[std::size_t(20) * spec.width + 10] = 3;
        zones[std::size_t(22) * spec.width + 12] = 3;
    }
    appendRecord(out, "P-FLAGS", flags);
    appendRecord(out, "P-OBSTACLES", obstacles);
    appendRecord(out, "I-ZONES", zones);

    Bytes phraseRec;
    appendU32(phraseRec, static_cast<std::uint32_t>(spec.phrases.size()));
    for (const auto& [sound, text] : spec.phrases) {
        appendU32(phraseRec, static_cast<std::uint32_t>(sound.size()));
        appendStr(phraseRec, sound);
        appendU32(phraseRec, static_cast<std::uint32_t>(text.size()));
        appendStr(phraseRec, text);
    }
    phraseRec.insert(phraseRec.end(), spec.phraseTrailingZeros, 0);
    appendRecord(out, "I-PHRASE2", phraseRec);

    Bytes scriptsRec;
    appendU32(scriptsRec, spec.triggerCount);
    for (std::uint32_t t = 0; t < spec.triggerCount; ++t) {
        const std::uint32_t extraWords = t % 3u;
        const std::uint32_t size = 12u + 4u * extraWords;
        appendU32(scriptsRec, size);
        appendStr(scriptsRec, "CAOF");
        appendU32(scriptsRec, 0x100u + t);
        for (std::uint32_t w = 0; w < extraWords; ++w) {
            appendU32(scriptsRec, 0x2000u + t * 16u + w);
        }
        appendU32(scriptsRec, 0x7fffffffu);
    }
    appendRecord(out, "I-SCRIPTS", scriptsRec);

    Bytes objects(400, 0);
    // Marker 0: (10, 20), Marker 1: (-5, 12)
    objects[0] = 10;
    objects[1] = 0;
    objects[2] = 20;
    objects[3] = 0;
    const std::uint16_t neg5 = static_cast<std::uint16_t>(static_cast<std::int16_t>(-5));
    objects[4] = static_cast<std::uint8_t>(neg5 & 0xFFu);
    objects[5] = static_cast<std::uint8_t>((neg5 >> 8) & 0xFFu);
    objects[6] = 12;
    objects[7] = 0;
    appendRecord(out, "I-OBJECTS", objects);

    if (spec.emitFlags20) {
        Bytes iflags(16u * 20u, 0xFFu);
        const auto putI32 = [&](std::size_t off, std::int32_t v) {
            appendU32(iflags, 0);
            iflags.resize(iflags.size() - 4);
            const std::uint32_t u = static_cast<std::uint32_t>(v);
            iflags[off + 0] = static_cast<std::uint8_t>(u & 0xFFu);
            iflags[off + 1] = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
            iflags[off + 2] = static_cast<std::uint8_t>((u >> 16) & 0xFFu);
            iflags[off + 3] = static_cast<std::uint8_t>((u >> 24) & 0xFFu);
        };
        // Gate 0: entry (10, 20), rally (14, 24); Gate 1: entry (30, 40), rally (34, 44).
        putI32(0, 10);
        putI32(4, 20);
        putI32(8, 14);
        putI32(12, 24);
        putI32(16, 1);
        putI32(20, 30);
        putI32(24, 40);
        putI32(28, 34);
        putI32(32, 44);
        putI32(36, 2);
        appendRecord(out, "I-FLAGS", iflags);
    }

    if (supportSpec) {
        if (!supportSpec->unames.empty()) {
            Bytes unamesRec(supportSpec->unames.size() * 16u, 0);
            for (std::size_t i = 0; i < supportSpec->unames.size(); ++i) {
                const auto& name = supportSpec->unames[i];
                const std::size_t copyLen = std::min<std::size_t>(name.size(), 16u);
                std::copy_n(name.data(), copyLen, reinterpret_cast<char*>(unamesRec.data() + i * 16u));
            }
            appendRecord(out, "I-UNAMES", unamesRec);
        }

        if (supportSpec->rawSupportOverride.has_value()) {
            appendRecord(out, "I-SUPPORT", *supportSpec->rawSupportOverride);
        } else if (supportSpec->supportWaves.has_value()) {
            Bytes supportRec;
            for (const auto& wave : *supportSpec->supportWaves) {
                appendU32(supportRec, static_cast<std::uint32_t>(wave.size()));
                for (const auto& unit : wave) {
                    supportRec.push_back(unit.group);
                    supportRec.push_back(unit.hp);
                    supportRec.push_back(unit.ammo);
                    supportRec.push_back(unit.expa);
                    supportRec.push_back(unit.lives);
                    supportRec.push_back(unit.morale);
                    appendU16(supportRec, unit.unameIdx);
                    appendU16(supportRec, 0);
                    supportRec.push_back(unit.riders);
                    supportRec.push_back(unit.crew);
                    for (const auto& pax : unit.passengers) {
                        supportRec.push_back(pax.group);
                        supportRec.push_back(pax.hp);
                        supportRec.push_back(pax.ammo);
                        supportRec.push_back(pax.expa);
                        supportRec.push_back(pax.lives);
                        supportRec.push_back(pax.morale);
                        appendU16(supportRec, pax.unameIdx);
                        appendU16(supportRec, 0);
                    }
                }
            }
            appendRecord(out, "I-SUPPORT", supportRec);
        }
    }

    if (spec.rawMisxinfWeather.has_value()) {
        Bytes xinf(392u, 0u);
        const std::uint32_t wv = *spec.rawMisxinfWeather;
        xinf[0] = static_cast<std::uint8_t>(wv & 0xFFu);
        xinf[1] = static_cast<std::uint8_t>((wv >> 8) & 0xFFu);
        xinf[2] = static_cast<std::uint8_t>((wv >> 16) & 0xFFu);
        xinf[3] = static_cast<std::uint8_t>((wv >> 24) & 0xFFu);
        appendRecord(out, "I-MISXINF", xinf);
    }

    return out;
}

void writeGzipFile(const std::filesystem::path& path, std::span<const std::uint8_t> payload) {
    std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
    std::unique_ptr<gzFile_s, decltype(&gzclose)> gz(gzopen_w(path.c_str(), "wb"), gzclose);
#else
    std::unique_ptr<gzFile_s, decltype(&gzclose)> gz(gzopen(path.c_str(), "wb"), gzclose);
#endif
    if (!gz) {
        throw std::runtime_error("Failed to open gzip file for writing: " + path.string());
    }
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const unsigned chunk = static_cast<unsigned>(std::min<std::size_t>(payload.size() - offset, 1u << 20));
        const int written = gzwrite(gz.get(), payload.data() + offset, chunk);
        if (written <= 0 || static_cast<unsigned>(written) != chunk) {
            throw std::runtime_error("Failed writing gzip file: " + path.string());
        }
        offset += chunk;
    }
    if (gzclose(gz.release()) != Z_OK) {
        throw std::runtime_error("Failed closing gzip file: " + path.string());
    }
}

void writeMissionFile(const std::filesystem::path& path, const MissionSpec& spec,
                      const SupportSpec* supportSpec = nullptr) {
    const auto raw = buildSyntheticMissionBytes(spec, supportSpec);
    writeGzipFile(path, raw);
}

Bytes zlibCompress(std::span<const std::uint8_t> input) {
    uLongf outSize = compressBound(static_cast<uLong>(input.size()));
    Bytes output(outSize);
    std::array<std::uint8_t, 1> dummy{};
    const Bytef* src = input.empty() ? dummy.data() : input.data();
    const int rc = compress2(output.data(), &outSize, src, static_cast<uLong>(input.size()), Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) {
        throw std::runtime_error("zlib compress2 failed");
    }
    output.resize(outSize);
    return output;
}

void appendSueTable(Bytes& out, std::span<const std::uint8_t> raw, std::uint32_t count, std::uint32_t width) {
    const auto packed = zlibCompress(raw);
    appendU32(out, static_cast<std::uint32_t>(packed.size()));
    appendU32(out, count);
    appendU32(out, width);
    out.insert(out.end(), packed.begin(), packed.end());
}

struct SueEntrySpec {
    std::string name;
    Bytes data;
    bool chunked{false};
    std::uint32_t chunkSize{0};
};

void writeSueArchive(const std::filesystem::path& path, const std::vector<SueEntrySpec>& entries) {
    std::filesystem::create_directories(path.parent_path());

    Bytes dataSection;
    Bytes items;
    Bytes names;
    std::vector<std::uint32_t> offsets;

    for (const auto& entry : entries) {
        const auto nameOffset = static_cast<std::uint32_t>(names.size());
        appendStr(names, entry.name);
        names.push_back(0);

        std::uint32_t kind = 2;
        std::uint32_t first = 0;
        const auto size = static_cast<std::uint32_t>(entry.data.size());
        std::uint32_t chunk = 0;

        if (!entry.chunked) {
            first = static_cast<std::uint32_t>(8 + dataSection.size());
            dataSection.insert(dataSection.end(), entry.data.begin(), entry.data.end());
        } else {
            if (entry.chunkSize == 0) {
                throw std::runtime_error("Chunked SUE entry requires non-zero chunk size");
            }
            kind = 8;
            first = static_cast<std::uint32_t>(offsets.size());
            chunk = entry.chunkSize;
            offsets.push_back(static_cast<std::uint32_t>(8 + dataSection.size()));
            for (std::size_t pos = 0; pos < entry.data.size(); pos += chunk) {
                const std::size_t len = std::min<std::size_t>(chunk, entry.data.size() - pos);
                const auto compressed = zlibCompress(std::span<const std::uint8_t>(entry.data.data() + pos, len));
                dataSection.insert(dataSection.end(), compressed.begin(), compressed.end());
                offsets.push_back(static_cast<std::uint32_t>(8 + dataSection.size()));
            }
        }

        appendU32(items, nameOffset);
        appendU32(items, kind);
        appendU32(items, first);
        appendU32(items, size);
        appendU32(items, chunk);
    }

    Bytes offsetBytes;
    offsetBytes.reserve(offsets.size() * 4);
    for (std::uint32_t off : offsets) {
        appendU32(offsetBytes, off);
    }

    Bytes archive;
    appendStr(archive, "FZFS");
    appendU32(archive, static_cast<std::uint32_t>(dataSection.size()));
    archive.insert(archive.end(), dataSection.begin(), dataSection.end());
    appendSueTable(archive, items, static_cast<std::uint32_t>(entries.size()), 20);
    appendSueTable(archive, names, static_cast<std::uint32_t>(names.size()), 1);
    appendSueTable(archive, offsetBytes, static_cast<std::uint32_t>(offsets.size()), 4);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()))) {
        throw std::runtime_error("Failed writing SUE archive: " + path.string());
    }
}

void testDecodeMissionText() {
    using ss2::assets::decodeMissionText;

    check(decodeMissionText("").empty(), "empty text stays empty");

    const std::string ascii = "Sudden Strike II: Mission 01 - Hold the bridge!\r\n\t100%";
    check(decodeMissionText(ascii) == ascii, "pure ASCII unchanged");

    // Valid UTF-8 sequences must pass through verbatim without double-encoding.
    check(decodeMissionText("\xC2\xB4") == "\xC2\xB4", "UTF-8 acute accent verbatim");
    check(decodeMissionText("B\xC3\xA4r") == "B\xC3\xA4r", "UTF-8 German umlaut verbatim");
    check(decodeMissionText("\xE2\x82\xAC") == "\xE2\x82\xAC", "UTF-8 Euro verbatim");
    check(decodeMissionText("\xF0\x9F\x9A\x97") == "\xF0\x9F\x9A\x97", "4-byte UTF-8 verbatim");

    // Windows-1252 bytes decoded to UTF-8.
    check(decodeMissionText("\x80") == "\xE2\x82\xAC", "CP1252 0x80 Euro -> UTF-8");
    check(decodeMissionText("\x93Hello\x94") == "\xE2\x80\x9CHello\xE2\x80\x9D", "CP1252 smart quotes -> UTF-8");
    check(decodeMissionText("Caf\xE9") == "Caf\xC3\xA9", "CP1252 0xE9 -> UTF-8");
    check(decodeMissionText("\x96\x97\x99") == "\xE2\x80\x93\xE2\x80\x94\xE2\x84\xA2",
          "CP1252 dashes and trademark -> UTF-8");
    check(decodeMissionText("\x81") == "\xEF\xBF\xBD", "CP1252 undefined byte -> U+FFFD");

    // Mixed valid UTF-8 and raw CP1252 in the same string.
    check(decodeMissionText("M\xC3\xBCnchen \x80 \x93 Caf\xE9") ==
              "M\xC3\xBCnchen \xE2\x82\xAC \xE2\x80\x9C Caf\xC3\xA9",
          "mixed UTF-8 and CP1252 decoded cleanly");

    // Invalid UTF-8 prefix falls back per-byte to CP1252.
    check(decodeMissionText("\xC3" "a") == "\xC3\x83" "a", "lone 0xC3 falls back to CP1252");
}

void testSyntheticMissions() {
    using namespace ss2::assets;
    using namespace std::string_literals;

    TempDir tmp("ss2_mission_test");

    // Verify 128x128, 256x256, 512x512, scheme 0 & 2, variable triggers, I-PHRASE2 with padding,
    // I-DESC NUL truncation + CP1252 + missing I-DESC.
    const std::array<MissionSpec, 3> specs{{
        MissionSpec{
            .scheme = 0,
            .width = 128,
            .height = 128,
            .rawDescription = "  \x93" "Caf\xE9" "\x94 \x80\nSecond line\0ignored trailing bytes"s,
            .phrases = {
                {"snd_intro\xE9", "Move to \x93Sector A\x94 (\x80" "50)!"},
                {"snd_utf8_\xC3\xA4", "R\xC3\xBC" "ckzug"},
            },
            .phraseTrailingZeros = 8,
            .triggerCount = 1,
        },
        MissionSpec{
            .scheme = 2,
            .width = 256,
            .height = 256,
            .rawDescription = std::nullopt,
            .phrases = {
                {"", "Silent alert"},
            },
            .phraseTrailingZeros = 4,
            .triggerCount = 5,
        },
        MissionSpec{
            .scheme = 0,
            .width = 512,
            .height = 512,
            .rawDescription = std::string("Big Map\r\nLine2"),
            .phrases = {},
            .phraseTrailingZeros = 0,
            .triggerCount = 3,
        },
    }};

    for (std::size_t idx = 0; idx < specs.size(); ++idx) {
        const auto& spec = specs[idx];
        const auto missionPath = tmp.path / ("unit_" + std::to_string(idx) + ".ssm");
        writeMissionFile(missionPath, spec);

        const auto data = readMissionData(missionPath);
        check(data.scheme == spec.scheme, "mission scheme matches");
        check(data.width == spec.width && data.height == spec.height, "mission dimensions match");
        check(data.weather == WeatherKind::Clear, "missing I-MISXINF defaults to Clear");

        const std::size_t cellCount = std::size_t(spec.width) * std::size_t(spec.height);
        check(data.flags.size() == cellCount, "P-FLAGS cell count matches");
        check(data.obstacles.size() == cellCount, "P-OBSTACLES cell count matches");
        check(data.zones.size() == cellCount, "I-ZONES cell count matches");

        for (std::size_t i = 0; i < cellCount; ++i) {
            const auto expectedFlag = static_cast<std::uint32_t>((i * 2654435761u) ^ (spec.scheme + 1u));
            const auto expectedObs = static_cast<std::uint16_t>((i * 73u + spec.width) & 0xFFFFu);
            if (data.flags[i] != expectedFlag || data.obstacles[i] != expectedObs) {
                throw std::runtime_error("P-FLAGS/P-OBSTACLES payload mismatch at cell " + std::to_string(i));
            }
        }

        // Verify I-ZONES centroid location math and default center fallback.
        const int originX = static_cast<int>(spec.height) * 32;
        const std::pair<int, int> expectedZone3{originX + 32 * (11 - 21), 16 * (11 + 21 + 1)};
        const std::pair<int, int> expectedDefault{originX, 16 * (static_cast<int>(spec.width / 2) + static_cast<int>(spec.height / 2) + 1)};
        check(data.locations[3] == expectedZone3, "I-ZONES centroid location matches");
        check(data.locations[0] == expectedDefault, "I-ZONES default zone 0 location matches");
        check(data.locations[7] == expectedDefault, "I-ZONES empty zone location matches");

        // Verify I-OBJECTS marker coordinates (positive and negative int16).
        const std::pair<int, int> expectedMarker0{originX + 32 * (10 - 20), 16 * (10 + 20 + 1)};
        const std::pair<int, int> expectedMarker1{originX + 32 * (-5 - 12), 16 * (-5 + 12 + 1)};
        check(data.markers[0] == expectedMarker0, "I-OBJECTS positive marker matches");
        check(data.markers[1] == expectedMarker1, "I-OBJECTS negative marker matches");

        // Verify I-PHRASE2 strings and sounds (including trailing zero padding).
        check(data.phrases.size() == spec.phrases.size(), "I-PHRASE2 phrase count matches");
        check(data.phraseSounds.size() == spec.phrases.size(), "I-PHRASE2 sound count matches");
        for (std::size_t p = 0; p < spec.phrases.size(); ++p) {
            check(data.phraseSounds[p] == decodeMissionText(spec.phrases[p].first), "I-PHRASE2 sound decoded");
            check(data.phrases[p] == decodeMissionText(spec.phrases[p].second), "I-PHRASE2 text decoded");
        }

        // Verify I-SCRIPTS variable trigger counts.
        check(data.scripts.size() == spec.triggerCount, "I-SCRIPTS trigger count matches");
        for (std::uint32_t t = 0; t < spec.triggerCount; ++t) {
            const auto& code = data.scripts[t];
            check(code.size() == 2u + (t % 3u), "I-SCRIPTS trigger word count matches");
            check(code.front() == 0x100u + t, "I-SCRIPTS trigger first word matches");
            check(code.back() == 0x7fffffffu, "I-SCRIPTS trigger terminator matches");
        }

        // Verify omitted I-SUPPORT leaves reinforcement waves empty.
        for (const auto& wave : data.reinforcements) {
            check(wave.empty(), "omitted I-SUPPORT produces empty reinforcement waves");
        }
    }

    // Verify synthetic I-UNAMES + I-SUPPORT parsing (parent + crew + riders + error paths, crew-first order).
    {
        std::array<std::vector<SyntheticSupportUnit>, 64> waves{};
        waves[0].push_back(SyntheticSupportUnit{
            2, 95, 40, 7, 1, 100, 0, 2, 1,
            {
                SyntheticPassenger{4, 75, 50, 3, 1, 100, 3},
                SyntheticPassenger{3, 100, 80, 1, 1, 100, 1},
                SyntheticPassenger{3, 90, 60, 2, 1, 100, 2},
            },
        });
        waves[63].push_back(SyntheticSupportUnit{9, 110, 120, 15, 1, 100, 1, 0, 0, {}});

        SupportSpec okSupport;
        okSupport.unames = {"maultier", "dguner", "dsmguner", "exact_sixteen_ch"};
        okSupport.supportWaves = waves;
        const auto supportMission = tmp.path / "support_ok.ssm";
        writeMissionFile(supportMission, MissionSpec{}, &okSupport);
        const auto data = readMissionData(supportMission);
        check(data.reinforcements[0].size() == 1 && data.reinforcements[0][0].passengers.size() == 3,
              "wave 0 nests 1 crew + 2 riders on parent reinforcement vehicle");
        check(data.reinforcements[0][0].id == "maultier" && data.reinforcements[0][0].group == 2 &&
                  data.reinforcements[0][0].hp == 95 && data.reinforcements[0][0].ammo == 40 &&
                  data.reinforcements[0][0].expa == 7,
              "wave 0 parent reinforcement matches");
        check(data.reinforcements[0][0].passengers[0].id == "exact_sixteen_ch" &&
                  data.reinforcements[0][0].passengers[0].group == 4 &&
                  data.reinforcements[0][0].passengers[0].hp == 75 &&
                  data.reinforcements[0][0].passengers[0].ammo == 50 &&
                  data.reinforcements[0][0].passengers[0].expa == 3 &&
                  data.reinforcements[0][0].passengers[0].isCrew,
              "wave 0 crew 16-char entry without NUL terminator matches first slot (isCrew=true)");
        check(data.reinforcements[0][0].passengers[1].id == "dguner" &&
                  data.reinforcements[0][0].passengers[1].group == 3 &&
                  data.reinforcements[0][0].passengers[1].hp == 100 &&
                  data.reinforcements[0][0].passengers[1].ammo == 80 &&
                  data.reinforcements[0][0].passengers[1].expa == 1 &&
                  !data.reinforcements[0][0].passengers[1].isCrew,
              "wave 0 rider 1 matches (isCrew=false)");
        check(data.reinforcements[0][0].passengers[2].id == "dsmguner" &&
                  data.reinforcements[0][0].passengers[2].group == 3 &&
                  data.reinforcements[0][0].passengers[2].hp == 90 &&
                  data.reinforcements[0][0].passengers[2].ammo == 60 &&
                  data.reinforcements[0][0].passengers[2].expa == 2 &&
                  !data.reinforcements[0][0].passengers[2].isCrew,
              "wave 0 rider 2 matches (isCrew=false)");
        for (std::size_t w = 1; w < 63; ++w) {
            check(data.reinforcements[w].empty(), "middle empty wave stays empty");
        }
        check(data.reinforcements[63].size() == 1 && data.reinforcements[63][0].id == "dguner" &&
                  data.reinforcements[63][0].group == 9 && data.reinforcements[63][0].hp == 110 &&
                  data.reinforcements[63][0].ammo == 120 && data.reinforcements[63][0].expa == 15,
              "wave 63 reinforcement matches");

        // Error paths: invalid unameIdx, truncated wave, trailing bytes.
        std::array<std::vector<SyntheticSupportUnit>, 64> badIdxWaves{};
        badIdxWaves[0].push_back(SyntheticSupportUnit{0, 100, 100, 0, 1, 100, 9, 0, 0, {}});
        SupportSpec badIdxSupport;
        badIdxSupport.unames = {"dguner"};
        badIdxSupport.supportWaves = badIdxWaves;
        const auto badIdxPath = tmp.path / "support_bad_idx.ssm";
        writeMissionFile(badIdxPath, MissionSpec{}, &badIdxSupport);
        checkThrowsType<std::runtime_error>([&] { (void)readMissionData(badIdxPath); },
                                            "I-SUPPORT out-of-range unameIdx throws");

        SupportSpec truncSupport;
        truncSupport.unames = {"dguner"};
        truncSupport.rawSupportOverride = Bytes(64 * 4 - 1, 0);
        const auto truncPath = tmp.path / "support_trunc.ssm";
        writeMissionFile(truncPath, MissionSpec{}, &truncSupport);
        checkThrowsType<std::runtime_error>([&] { (void)readMissionData(truncPath); },
                                            "truncated I-SUPPORT throws");

        SupportSpec trailSupport;
        trailSupport.unames = {"dguner"};
        trailSupport.rawSupportOverride = Bytes(64 * 4 + 1, 0);
        const auto trailPath = tmp.path / "support_trail.ssm";
        writeMissionFile(trailPath, MissionSpec{}, &trailSupport);
        checkThrowsType<std::runtime_error>([&] { (void)readMissionData(trailPath); },
                                            "trailing I-SUPPORT bytes throw");
    }

    // Verify 20-byte I-FLAGS gate & rally target decoding on synthetic mission.
    {
        MissionSpec fSpec;
        fSpec.width = 128;
        fSpec.height = 128;
        fSpec.emitFlags20 = true;
        const auto fPath = tmp.path / "gates20.ssm";
        writeMissionFile(fPath, fSpec);
        const auto data = readMissionData(fPath);
        const int origX = 128 * 32;
        check(data.gates[0] == std::pair<int, int>{origX + 32 * (10 - 20), 16 * (10 + 20 + 1)},
              "20-byte I-FLAGS gate 0 decoded");
        check(data.gateTargets[0] == std::pair<int, int>{origX + 32 * (14 - 24), 16 * (14 + 24 + 1)},
              "20-byte I-FLAGS gate 0 rally target decoded");
        check(data.gates[1] == std::pair<int, int>{origX + 32 * (30 - 40), 16 * (30 + 40 + 1)},
              "20-byte I-FLAGS gate 1 decoded (proves 20-byte stride)");
        check(data.gateTargets[1] == std::pair<int, int>{origX + 32 * (34 - 44), 16 * (34 + 44 + 1)},
              "20-byte I-FLAGS gate 1 rally target decoded");
    }

    // Verify paired campaign `.ssc` (P2SS + I2SS + I-CINFO cropping), overview JPEG, placements, and UnitCatalog parsing.
    {
        const auto campDir = tmp.path / "CampGame" / "maps" / "campaign";
        std::filesystem::create_directories(campDir);
        const std::uint32_t baseW = 128, baseH = 128;
        const std::uint32_t offX = 32, offY = 16, subW = 64, subH = 64;

        Bytes p2ss;
        appendStr(p2ss, "P2SS");
        appendU32(p2ss, 0x222u);
        Bytes pInfo;
        appendU32(pInfo, 2u);
        appendU32(pInfo, baseW);
        appendU32(pInfo, baseH);
        appendRecord(p2ss, "P-INFO", pInfo);
        Bytes bFlags, bObs;
        for (std::uint32_t y = 0; y < baseH; ++y) {
            for (std::uint32_t x = 0; x < baseW; ++x) {
                appendU32(bFlags, y * 1000u + x);
                appendU16(bObs, static_cast<std::uint16_t>((y + x) & 0xFFFFu));
            }
        }
        appendRecord(p2ss, "P-FLAGS", bFlags);
        appendRecord(p2ss, "P-OBSTACLES", bObs);
        writeGzipFile(campDir / "099.ssc", p2ss);

        Bytes i2ss;
        appendStr(i2ss, "I2SS");
        appendU32(i2ss, 0x222u);
        Bytes cInfo;
        appendU32(cInfo, offX);
        appendU32(cInfo, offY);
        appendU32(cInfo, subW);
        appendU32(cInfo, subH);
        appendRecord(i2ss, "I-CINFO", cInfo);
        const std::string cdesc = "Operation Cropped\nDetails";
        appendRecord(i2ss, "I-DESC",
                     std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(cdesc.data()), cdesc.size()));
        Bytes cPhr;
        appendU32(cPhr, 0u);
        appendRecord(i2ss, "I-PHRASE2", cPhr);
        Bytes cScr;
        appendU32(cScr, 1u);
        appendU32(cScr, 12u);
        appendStr(cScr, "CAOF");
        appendU32(cScr, 0x7fffffffu);
        appendU32(cScr, 0x7fffffffu);
        appendRecord(i2ss, "I-SCRIPTS", cScr);
        Bytes cZones(std::size_t(subW) * subH, 0);
        appendRecord(i2ss, "I-ZONES", cZones);
        appendRecord(i2ss, "I-OBJECTS", Bytes(400, 0));

        // 1 unit "tiger" in cropped pixel bounds (u=320, v=480) with 1 crew + 2 riders.
        Bytes unames(32, 0);
        std::copy_n("tiger", 5, reinterpret_cast<char*>(unames.data()));
        std::copy_n("dguner", 6, reinterpret_cast<char*>(unames.data() + 16));
        appendRecord(i2ss, "I-UNAMES", unames);
        Bytes iunits;
        appendU32(iunits, 1u);
        iunits.push_back(7);   // group
        iunits.push_back(100); // hp
        iunits.push_back(90);  // ammo
        iunits.push_back(0);   // expa
        iunits.push_back(1);   // lives
        iunits.push_back(100); // morale
        appendU16(iunits, 0u); // unameIdx ("tiger")
        appendU16(iunits, 0u); // pad
        iunits.push_back(2);   // nRiders
        iunits.push_back(1);   // nCrew
        appendU16(iunits, 320u);
        appendU16(iunits, 480u);
        iunits.push_back(4);   // dir
        iunits.push_back(0);   // owner
        // Crew record (first nCrew = 1) + 2 Rider records (next nRiders = 2)
        for (int p = 0; p < 3; ++p) {
            iunits.push_back(7 + p); // group
            iunits.push_back(100);   // hp
            iunits.push_back(80);    // ammo
            iunits.push_back(p);     // expa
            iunits.push_back(1);     // lives
            iunits.push_back(100);   // morale
            appendU16(iunits, 1u);   // unameIdx ("dguner")
            appendU16(iunits, 0u);   // pad
        }
        appendRecord(i2ss, "I-UNITS", iunits);

        // Encode 4x4 RGB overview JPEG for I-JPG1024.
        Bytes jpgData;
        {
            unsigned char* rawBuf = nullptr;
            unsigned long rawLen = 0;
            jpeg_compress_struct cinfo{};
            jpeg_error_mgr jerr{};
            cinfo.err = jpeg_std_error(&jerr);
            jpeg_create_compress(&cinfo);
            jpeg_mem_dest(&cinfo, &rawBuf, &rawLen);
            cinfo.image_width = 4;
            cinfo.image_height = 4;
            cinfo.input_components = 3;
            cinfo.in_color_space = JCS_RGB;
            jpeg_set_defaults(&cinfo);
            jpeg_start_compress(&cinfo, TRUE);
            std::array<JSAMPLE, 12> row{};
            row.fill(200);
            while (cinfo.next_scanline < cinfo.image_height) {
                JSAMPROW ptr = row.data();
                jpeg_write_scanlines(&cinfo, &ptr, 1);
            }
            jpeg_finish_compress(&cinfo);
            jpeg_destroy_compress(&cinfo);
            jpgData.assign(rawBuf, rawBuf + rawLen);
            std::free(rawBuf);
        }
        Bytes jpgRec;
        appendU32(jpgRec, 4u);
        appendU32(jpgRec, 4u);
        appendU32(jpgRec, 12u);
        jpgRec.insert(jpgRec.end(), jpgData.begin(), jpgData.end());
        appendRecord(i2ss, "I-JPG1024", jpgRec);
        writeGzipFile(campDir / "099000.ssc", i2ss);

        const auto cData = readMissionData(campDir / "099000.ssc");
        check(cData.scheme == 2u && cData.width == subW && cData.height == subH,
              "campaign ssc cropped dimensions & scheme match");
        check(cData.flags.size() == std::size_t(subW) * subH &&
              cData.flags[0] == offY * 1000u + offX &&
              cData.flags[std::size_t(subH - 1) * subW + (subW - 1)] == (offY + subH - 1) * 1000u + (offX + subW - 1),
              "campaign ssc sliced P-FLAGS matches exact I-CINFO offset");
        const auto cPlaced = readMissionPlacements(campDir / "099000.ssc");
        check(cPlaced.size() == 1 && cPlaced[0].id == "tiger" &&
              cPlaced[0].x == int(subH) * 32 + 320 - 480 && cPlaced[0].y == (320 + 480) / 2,
              "campaign ssc I-UNITS origin uses cropped height");
        check(cPlaced[0].passengers.size() == 3 &&
              cPlaced[0].passengers[0].isCrew && !cPlaced[0].passengers[1].isCrew &&
              !cPlaced[0].passengers[2].isCrew && cPlaced[0].passengers[1].id == "dguner",
              "campaign ssc nested I-UNITS crew and riders decoded in retail order");
        const auto cOverview = readMissionOverview(campDir / "099000.ssc");
        check(cOverview.width == 4 && cOverview.height == 4, "campaign ssc I-JPG1024 decoded");
        const auto camps = listCampaigns(tmp.path / "CampGame");
        check(camps.size() == 1 && camps[0].missions.size() == 1 && camps[0].missions[0].code == "099000",
              "campaign fallback discovery listed synthetic .ssc");

        // UnitCatalog::parseUnitText synthetic check.
        const std::string_view tigerTxt =
            "name $UNIT_TIGER_NAME\n"
            "*name PzKpfw.VI E Tiger\n"
            "shortname Tiger\n"
            "health 1160\n"
            "mech 1\n"
            "ammo1 94\n"
            "ammo2 5100\n"
            "movespeed 1.12 1.45\n"
            "sight 7 8\n"
            "bonus_sight 3 3\n"
            "marchsightbonus 4\n"
            "shot1_id pierce8-10k\n"
            "shot1_damage 629 704\n"
            "shot1_range 638 701\n"
            "shot1_deadzone 80 80\n"
            "shot1_reloadtime 211 179\n"
            "shot1_sound gunshot 0.3 perm 0 1\n"
            "shot2_id man_machine\n"
            "shot2_damage 46 51\n"
            "shot2_range 607 667\n"
            "shot2_reloadtime 2 2\n"
            "shot2_burstshots 8 8\n"
            "shot2_burstreloadtime 52 44\n"
            "shot2_sound mg_tank_burst 0.3 perm 0 1\n"
            "armor PIERCE 100 105 82 86 82 86\n"
            "armor MACHINE 256 256 256 256 256 256\n";
        const auto parsed = UnitCatalog::parseUnitText("TIGER", tigerTxt, {}, [](std::string_view shot) {
            if (shot == "pierce8-10k") return HitType::Pierce;
            if (shot == "man_machine") return HitType::Machine;
            return HitType::None;
        });
        check(parsed.id == "tiger" && parsed.displayName == "PzKpfw.VI E Tiger" && parsed.shortName == "Tiger" &&
              parsed.health == 1160 && parsed.isVehicle && parsed.sight == 7 && parsed.bonusSight == 3 &&
              parsed.marchSightBonus == 4 &&
              parsed.shot1.hitType == HitType::Pierce && parsed.shot1.damage == 629 &&
              parsed.shot2.hitType == HitType::Machine && parsed.shot2.damage == 46 &&
              parsed.armor[static_cast<std::size_t>(HitType::Pierce)].front == 100 &&
              parsed.armor[static_cast<std::size_t>(HitType::Pierce)].rear == 82 &&
              parsed.armor[static_cast<std::size_t>(HitType::Machine)].front == 256,
              "UnitCatalog::parseUnitText parsed weapons and directional armor");
    }

    // Check I-DESC NUL truncation + CP1252 + missing I-DESC.
    const auto descMission = readMissionData(tmp.path / "unit_0.ssm");
    check(descMission.description == "\xE2\x80\x9C" "Caf\xC3\xA9" "\xE2\x80\x9D \xE2\x82\xAC\nSecond line",
          "I-DESC truncated at NUL and decoded as CP1252");
    const auto noDescMission = readMissionData(tmp.path / "unit_1.ssm");
    check(noDescMission.description.empty(), "missing I-DESC yields empty description");

    // Test listSinglePlayerMissions & findSinglePlayerMission with case-insensitive Maps/Single.
    const auto gameDir = tmp.path / "GameRoot";
    const auto singleDir = gameDir / "mApS" / "sInGlE";
    writeMissionFile(singleDir / "'V2 [Total Victory].ssm", MissionSpec{
        .scheme = 0,
        .width = 128,
        .height = 128,
        .rawDescription = "  \x93" "Caf\xE9" "\x94 \x80\nDetails\0hidden"s,
        .phrases = {{"s1", "t1"}},
        .phraseTrailingZeros = 0,
        .triggerCount = 1,
    });
    writeMissionFile(singleDir / "V2.SSM", MissionSpec{
        .scheme = 2,
        .width = 256,
        .height = 256,
        .rawDescription = std::nullopt,
        .phrases = {{"s2", "t2"}},
        .phraseTrailingZeros = 0,
        .triggerCount = 2,
    });
    writeMissionFile(singleDir / "Alpha.SsM", MissionSpec{
        .scheme = 0,
        .width = 128,
        .height = 128,
        .rawDescription = std::string("First line\r\nSecond line"),
        .phrases = {},
        .phraseTrailingZeros = 0,
        .triggerCount = 1,
    });
    // Non-mission and corrupt files should be ignored cleanly by listSinglePlayerMissions.
    {
        std::ofstream txt(singleDir / "notes.txt");
        txt << "ignore me\n";
        std::ofstream bad(singleDir / "broken.ssm", std::ios::binary);
        bad << "not gzip";
    }

    const auto listed = listSinglePlayerMissions(gameDir);
    check(listed.size() == 3, "listSinglePlayerMissions listed 3 valid mixed-case .ssm/.SSM missions");
    check(listed[0].name == "Alpha", "Alpha sorted first");
    check(listed[0].description == "First line", "summary description uses first line only");
    check(listed[1].name == "V2 ('V2 [Total Victory])", "duplicate display name disambiguated with stem (1)");
    check(listed[1].description == "\xE2\x80\x9C" "Caf\xC3\xA9" "\xE2\x80\x9D \xE2\x82\xAC",
          "summary description NUL-truncated, CP1252-decoded, and first-line trimmed");
    check(listed[2].name == "V2 (V2)", "duplicate display name disambiguated with stem (2)");
    check(listed[2].description.empty(), "summary without I-DESC has empty description");
    check(listed[2].width == 256 && listed[2].height == 256 && listed[2].scheme == 2,
          "summary captures dimensions and scheme");

    // findSinglePlayerMission queries.
    check(!findSinglePlayerMission(gameDir, "").has_value(), "empty query -> nullopt");
    check(!findSinglePlayerMission(gameDir, "   \t\n").has_value(), "whitespace query -> nullopt");
    check(!findSinglePlayerMission(gameDir, "0").has_value(), "0 index -> nullopt");
    check(!findSinglePlayerMission(gameDir, "4").has_value(), "out-of-range index -> nullopt");
    check(!findSinglePlayerMission(gameDir, "18446744073709551617").has_value(), "overflow index -> nullopt");
    check(!findSinglePlayerMission(gameDir, "nonexistent").has_value(), "unmatched query -> nullopt");

    const auto byIndex1 = findSinglePlayerMission(gameDir, "1");
    check(byIndex1.has_value() && byIndex1->name == "Alpha", "1-based index 1 -> Alpha");
    const auto byIndex2 = findSinglePlayerMission(gameDir, " 2 ");
    check(byIndex2.has_value() && byIndex2->name == "V2 ('V2 [Total Victory])", "1-based index 2 -> V2 Total Victory");
    const auto byExactName = findSinglePlayerMission(gameDir, "v2 (v2)");
    check(byExactName.has_value() && byExactName->name == "V2 (V2)", "exact case-insensitive name match");
    const auto byExactStem = findSinglePlayerMission(gameDir, "'v2 [total victory]");
    check(byExactStem.has_value() && byExactStem->name == "V2 ('V2 [Total Victory])", "exact stem match");
    const auto byExactFile = findSinglePlayerMission(gameDir, "v2.ssm");
    check(byExactFile.has_value() && byExactFile->name == "V2 (V2)", "exact filename match");
    const auto byUniqueSub = findSinglePlayerMission(gameDir, "total victory");
    check(byUniqueSub.has_value() && byUniqueSub->name == "V2 ('V2 [Total Victory])", "unique substring match");

    const auto byExactV2Stem = findSinglePlayerMission(gameDir, "V2");
    check(byExactV2Stem.has_value() && byExactV2Stem->name == "V2 (V2)", "exact stem V2 resolves V2.SSM");
    checkThrows([&] { (void)findSinglePlayerMission(gameDir, "v2 ("); },
                "Ambiguous mission query",
                "ambiguous substring throws invalid_argument");

    // Synthetic I-MISXINF dword[0] weather selector parsing and out-of-range fallback to Clear.
    const std::array<std::pair<std::uint32_t, WeatherKind>, 7> weatherCases{{
        {0u, WeatherKind::Clear},
        {1u, WeatherKind::Fog},
        {2u, WeatherKind::Rain},
        {3u, WeatherKind::Snow},
        {4u, WeatherKind::Clear},
        {99u, WeatherKind::Clear},
        {0xffffffffu, WeatherKind::Clear},
    }};
    for (const auto& [rawCode, expectedWeather] : weatherCases) {
        MissionSpec ws{};
        ws.rawMisxinfWeather = rawCode;
        const auto wp = tmp.path / ("misxinf_" + std::to_string(rawCode) + ".ssm");
        writeMissionFile(wp, ws);
        check(readMissionData(wp).weather == expectedWeather, "synthetic I-MISXINF weather selector matches");
    }

    // Synthetic MM2S multiplayer mission under Maps/mPlay/ should appear in listMultiplayerMissions only.
    {
        MissionSpec mpSpec{};
        mpSpec.scheme = 1;
        mpSpec.width = 128;
        mpSpec.height = 128;
        mpSpec.rawDescription = std::string("River crossing\nMore details");
        Bytes mm2s = buildSyntheticMissionBytes(mpSpec);
        mm2s[0] = 'M';
        mm2s[1] = 'M';
        mm2s[2] = '2';
        mm2s[3] = 'S';
        const auto mplayDir = gameDir / "mApS" / "mPlay";
        writeGzipFile(mplayDir / "Skirmish_2p.smm", mm2s);
        {
            std::ofstream txt(mplayDir / "ignored.txt");
            txt << "ignore";
            std::ofstream bad(mplayDir / "broken.smm", std::ios::binary);
            bad << "not gzip";
        }
        const auto mpData = readMissionData(mplayDir / "Skirmish_2p.smm");
        check(mpData.width == 128 && mpData.height == 128 && mpData.scheme == 1 &&
                  mpData.description == "River crossing\nMore details",
              "readMissionData parses synthetic MM2S multiplayer archive");
        const auto mpListed = listMultiplayerMissions(gameDir);
        check(mpListed.size() == 1 && mpListed[0].name == "Skirmish_2p" &&
                  mpListed[0].description == "River crossing",
              "listMultiplayerMissions lists synthetic MM2S .smm cleanly");
        const auto mpByIdx = findMultiplayerMission(gameDir, "1");
        const auto mpByStem = findMultiplayerMission(gameDir, "skirmish_2p");
        check(mpByIdx.has_value() && mpByStem.has_value() && mpByIdx->path == mpListed[0].path &&
                  mpByStem->path == mpListed[0].path,
              "findMultiplayerMission resolves by 1-based index and stem");
        check(listSinglePlayerMissions(gameDir).size() == 3,
              "listSinglePlayerMissions remains unaffected by Maps/mPlay archives");
    }

    // Synthetic Custom Campaigns: Maps/Custom_Campaign/ + userDir, campaign.seq ordering, EASY/, and ignored dummy.txt.
    {
        auto makeSpec = [](std::uint8_t scheme, std::uint16_t w, std::uint16_t h, std::string desc) {
            MissionSpec s{};
            s.scheme = scheme;
            s.width = w;
            s.height = h;
            s.rawDescription = std::move(desc);
            return s;
        };

        const auto ccDir = gameDir / "mApS" / "Custom_Campaign";
        const auto userCcDir = tmp.path / "user_custom_campaigns";
        std::filesystem::create_directories(ccDir);
        {
            std::ofstream dummy(ccDir / "dummy.txt");
            dummy << "dummy";
        }
        check(listCustomCampaigns(gameDir, userCcDir).empty(),
              "dummy.txt alone produces zero custom campaigns");

        writeMissionFile(ccDir / "Loose_Op.ssc", makeSpec(0, 128, 128, "Loose briefing"));

        const auto seaLionDir = ccDir / "Operation_SeaLion";
        std::filesystem::create_directories(seaLionDir / "EASY");
        {
            std::ofstream seq(seaLionDir / "campaign.seq");
            seq << "# ordered custom sequence\nsecond.ssc\nfirst.ssc\n";
        }
        writeMissionFile(seaLionDir / "first.ssc", makeSpec(1, 256, 256, "Normal SeaLion 1"));
        writeMissionFile(seaLionDir / "EASY" / "first.ssc", makeSpec(1, 256, 256, "Easy SeaLion 1"));
        writeMissionFile(seaLionDir / "second.ssc", makeSpec(1, 256, 256, "Normal SeaLion 2"));

        const auto desertDir = userCcDir / "Desert_Fox";
        writeMissionFile(desertDir / "01_tobruk.ssm", makeSpec(2, 128, 128, "Tobruk assault"));
        writeMissionFile(desertDir / "02_alamein.ssc", makeSpec(2, 128, 128, "El Alamein"));

        const auto retailSeqDir = userCcDir / "Retail_Seq";
        std::filesystem::create_directories(retailSeqDir);
        {
            std::ofstream seq(retailSeqDir / "campaign.seq");
            seq << ">Retail Styled\nm1>1. Custom Title\n";
        }
        writeMissionFile(retailSeqDir / "m1.ssc", makeSpec(0, 128, 128, "Ignored title"));

        const auto customCamps = listCustomCampaigns(gameDir, userCcDir);
        check(customCamps.size() == 4, "listCustomCampaigns finds root + game folder + user folders");
        check(customCamps[0].name == "Custom Campaign" && customCamps[0].missions.size() == 1,
              "loose root custom .ssc grouped under Custom Campaign");
        check(customCamps[1].name == "Desert_Fox" && customCamps[1].missions.size() == 2 &&
              customCamps[1].missions[0].code == "01_tobruk",
              "userDir custom folder missions listed in stem order");
        check(customCamps[2].name == "Operation_SeaLion" && customCamps[2].missions.size() == 2 &&
              customCamps[2].missions[0].code == "second" &&
              customCamps[2].missions[1].code == "first" &&
              sameExistingPath(customCamps[2].missions[1].easyPath, seaLionDir / "EASY" / "first.ssc") &&
              customCamps[2].missions[0].easyPath.empty(),
              "campaign.seq overrides file sort and binds sibling EASY path");
        check(customCamps[3].name == "Retail Styled" && customCamps[3].missions.size() == 1 &&
              customCamps[3].missions[0].code == "m1" &&
              customCamps[3].missions[0].title == "1. Custom Title",
              "retail-style >Name and code>Title campaign.seq overrides campaign and mission titles");

        const auto byCustomIdx = findCustomCampaignMission(gameDir, "Operation_SeaLion:2", true, userCcDir);
        check(byCustomIdx.has_value() && sameExistingPath(byCustomIdx->path, seaLionDir / "EASY" / "first.ssc"),
              "findCustomCampaignMission resolves campaign:N easy variant");
        const auto byCustomFallback = findCustomCampaignMission(gameDir, "Operation_SeaLion:1", true, userCcDir);
        check(byCustomFallback.has_value() && sameExistingPath(byCustomFallback->path, seaLionDir / "second.ssc"),
              "findCustomCampaignMission falls back to normal .ssc when EASY variant missing");
        const auto byCustomStem = findCustomCampaignMission(gameDir, "01_tobruk", false, userCcDir);
        check(byCustomStem.has_value() && sameExistingPath(byCustomStem->path, desertDir / "01_tobruk.ssm"),
              "findCustomCampaignMission resolves stem match across userDir");
    }
}

void testSueAndPaths() {
    using namespace ss2::assets;

    TempDir tmp("ss2_sue_test");

    // resolveCaseInsensitivePath tests.
    const auto actualSub = tmp.path / "GameData" / "Lng" / "Addons";
    std::filesystem::create_directories(actualSub);
    const auto actualFile = actualSub / "Extra_Unit.SUE";
    {
        std::ofstream f(actualFile, std::ios::binary);
        f << "placeholder";
    }

    const auto resolvedExact = resolveCaseInsensitivePath(tmp.path, "GameData/Lng/Addons/Extra_Unit.SUE");
    check(sameExistingPath(resolvedExact, actualFile), "resolveCaseInsensitivePath exact match");

    const auto resolvedCI = resolveCaseInsensitivePath(tmp.path, "gamedata/LNG/addons/extra_unit.sue");
    check(sameExistingPath(resolvedCI, actualFile), "resolveCaseInsensitivePath case-insensitive multi-component match");

    const auto missingRel = std::filesystem::path("gamedata/LNG/addons/missing.sue");
    const auto resolvedMissing = resolveCaseInsensitivePath(tmp.path, missingRel);
    check(resolvedMissing == tmp.path / missingRel, "resolveCaseInsensitivePath missing returns direct path");

    // SUE archive fixtures (both kind=2 stored and kind=8 zlib-chunked).
    const Bytes storedExact{'e', 'x', 'a', 'c', 't', 1, 2, 3};
    const Bytes storedUpper{'U', 'P', 'P', 'E', 'R', 4, 5, 6};
    Bytes chunkedPayload(5000);
    for (std::size_t i = 0; i < chunkedPayload.size(); ++i) {
        chunkedPayload[i] = static_cast<std::uint8_t>((i * 17u + 3u) & 0xFFu);
    }

    const auto archiveA = tmp.path / "Archives" / "First.SUE";
    writeSueArchive(archiveA, {
        {"Data/Mixed.txt", storedUpper, false, 0},
        {"data/mixed.txt", storedExact, false, 0},
        {"Sprites/Hero.pck", chunkedPayload, true, 1024},
    });

    const Bytes fallbackPayload{'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k'};
    const auto archiveB = tmp.path / "Archives" / "Second.Sue";
    writeSueArchive(archiveB, {
        {"Voice/Order.wav", fallbackPayload, false, 0},
        {"data/mixed.txt", Bytes{'s', 'h', 'a', 'd', 'o', 'w'}, false, 0},
    });

    // Exact match beats earlier case-insensitive match in the same archive.
    check(readSueEntry(archiveA, "data/mixed.txt") == storedExact, "readSueEntry prefers exact case match");
    check(readSueEntry(archiveA, "DATA/MIXED.TXT") == storedUpper, "readSueEntry falls back to case-insensitive match");
    check(readSueEntry(archiveA, "sprites/hero.pck") == chunkedPayload,
          "readSueEntry decodes multi-chunk kind=8 entry with case-insensitive lookup");
    checkThrowsType<std::runtime_error>([&] { (void)readSueEntry(archiveA, "does/not/exist"); },
                                        "readSueEntry throws on missing entry");

    // readSueEntryFallback: skips missing archive, resolves case-insensitive archive path, falls through to second archive.
    const std::vector<std::filesystem::path> chain{
        tmp.path / "archives" / "nonexistent.sue",
        tmp.path / "archives" / "first.sue",
        tmp.path / "archives" / "second.sue",
    };
    check(readSueEntryFallback(chain, "DATA/MIXED.TXT") == storedUpper,
          "readSueEntryFallback resolves case-insensitive path and returns first archive hit");
    check(readSueEntryFallback(chain, "voice/order.wav") == fallbackPayload,
          "readSueEntryFallback falls through to second archive");
    checkThrowsType<std::runtime_error>([&] { (void)readSueEntryFallback(chain, "missing/entry.bin"); },
                                        "readSueEntryFallback throws when entry absent in all archives");

    // sueEntrySize & readSueEntryRange across stored (kind=2) and multi-chunk (kind=8) entries.
    check(sueEntrySize(archiveA, "data/mixed.txt") == storedExact.size(), "sueEntrySize stored entry");
    check(sueEntrySize(archiveA, "SPRITES/HERO.PCK") == chunkedPayload.size(), "sueEntrySize case-insensitive kind=8");
    check(!sueEntrySize(archiveA, "does/not/exist").has_value(), "sueEntrySize missing entry returns nullopt");
    check(!sueEntrySize(tmp.path / "missing.sue", "data/mixed.txt").has_value(),
          "sueEntrySize missing archive returns nullopt");
    check(readSueEntryRange(archiveA, "data/mixed.txt", 2, 4) ==
              Bytes(storedExact.begin() + 2, storedExact.begin() + 6),
          "readSueEntryRange kind=2 slice");
    check(readSueEntryRange(archiveA, "sprites/hero.pck", 900, 1500) ==
              Bytes(chunkedPayload.begin() + 900, chunkedPayload.begin() + 2400),
          "readSueEntryRange kind=8 cross-chunk slice");
    check(readSueEntryRange(archiveA, "sprites/hero.pck", 4950, 200) ==
              Bytes(chunkedPayload.begin() + 4950, chunkedPayload.end()),
          "readSueEntryRange kind=8 clamps at EOF");
    check(readSueEntryRange(archiveA, "sprites/hero.pck", chunkedPayload.size(), 64).empty(),
          "readSueEntryRange kind=8 past EOF returns empty");
}

void testOptionalRetailSweep() {
    const char* env = std::getenv("SS2_PRIVATE_GAME");
    if (!env || !*env) {
        return;
    }
    const std::filesystem::path gameDir(env);
    std::error_code ec;
    if (!std::filesystem::is_directory(gameDir, ec)) {
        return;
    }

    const auto missions = ss2::assets::listSinglePlayerMissions(gameDir);
    check(missions.size() >= 39, "retail sweep expected at least 39 single-player missions");

    bool sawBasf = false;
    bool sawTutorial1 = false;
    for (const auto& m : missions) {
        const auto data = ss2::assets::readMissionData(m.path);
        check(data.width == m.width && data.height == m.height && data.scheme == m.scheme,
              "retail mission metadata matches summary: " + m.path.filename().string());
        const std::size_t cells = std::size_t(data.width) * std::size_t(data.height);
        check(data.flags.size() == cells && data.obstacles.size() == cells && data.zones.size() == cells,
              "retail mission cell layers complete: " + m.path.filename().string());
        check(!data.scripts.empty(), "retail mission has scripts: " + m.path.filename().string());

        const auto fileName = m.path.filename().string();
        const auto countWaveUnits = [](const std::vector<ss2::assets::ReinforcementUnit>& wave) {
            std::size_t n = 0;
            for (const auto& u : wave) n += 1u + u.passengers.size();
            return n;
        };
        if (fileName == "'BASF [Total Victory].ssm") {
            sawBasf = true;
            check(countWaveUnits(data.reinforcements[0]) >= 18, "BASF wave 0 expected at least 18 units");
            check(data.gates[0] == std::pair<int, int>{3936, 3904}, "BASF gate 0 expected at (3936, 3904)");
            check(data.gates[1] == std::pair<int, int>{2720, 800}, "BASF gate 1 expected at (2720, 800)");
        } else if (fileName == "--- TUTORIAL I ---.SSM") {
            sawTutorial1 = true;
            check(countWaveUnits(data.reinforcements[1]) >= 34, "Tutorial I wave 1 expected at least 34 units");
            check(data.gates[0] == std::pair<int, int>{1024, 2448}, "Tutorial I gate 0 expected at (1024, 2448)");
        }

        const auto overview = ss2::assets::readMissionOverview(m.path);
        check(overview.width > 0 && overview.height > 0,
              "retail mission overview non-empty: " + m.path.filename().string());
        check(overview.rgba.size() == std::size_t(overview.width) * std::size_t(overview.height) * 4,
              "retail mission overview RGBA size matches: " + m.path.filename().string());
    }
    check(sawBasf, "retail sweep expected BASF mission");
    check(sawTutorial1, "retail sweep expected Tutorial I mission");

    const auto mpMissions = ss2::assets::listMultiplayerMissions(gameDir);
    check(mpMissions.size() == 35u, "retail sweep expected 35 multiplayer missions");
    for (const auto& mp : mpMissions) {
        const auto mpData = ss2::assets::readMissionData(mp.path);
        check(mpData.width == mp.width && mpData.height == mp.height && mpData.scheme == mp.scheme,
              "retail multiplayer mission metadata matches summary: " + mp.path.filename().string());
        const auto mpOverview = ss2::assets::readMissionOverview(mp.path);
        check(mpOverview.width > 0 && mpOverview.height > 0 &&
                  mpOverview.rgba.size() == std::size_t(mpOverview.width) * std::size_t(mpOverview.height) * 4u,
              "retail multiplayer overview non-empty: " + mp.path.filename().string());
    }

    // Verify retail UnitCatalog across iconic WWII armor, soft vehicles & infantry.
    {
        const auto cat = ss2::assets::UnitCatalog::load(gameDir);
        const auto* tiger = cat->find("tiger");
        check(tiger && tiger->health == 1160 && tiger->isVehicle &&
              tiger->shot1.hitType == ss2::assets::HitType::Pierce &&
              tiger->shot2.hitType == ss2::assets::HitType::Machine &&
              tiger->armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)].front == 100 &&
              tiger->armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)].rear == 82 &&
              tiger->armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)].front == 256,
              "retail UnitCatalog Tiger stats match desc_common.sue");
        const auto* sherman = cat->find("sherman.v");
        check(sherman && sherman->health == 660 &&
              sherman->armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)].front == 63 &&
              sherman->armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)].side == 38,
              "retail UnitCatalog Sherman V stats match desc_common.sue");
        const auto* jeep = cat->find("wyllis.mb.jeep");
        check(jeep && jeep->shot1.damage == 0 && jeep->health == 70,
              "retail UnitCatalog Wyllis MB Jeep stats match desc_common.sue");
        const auto* dguner = cat->find("dguner");
        check(dguner && !dguner->isVehicle && dguner->health == 100,
              "retail UnitCatalog German rifleman stats match desc_common.sue");
        const auto* pak40 = cat->find("pak40");
        check(pak40 && pak40->isArtillery() && pak40->shot1.damage > 0 && pak40->shot1.range > 0 &&
                  !pak40->shot1.soundGroup.empty(),
              "retail UnitCatalog PaK 40 bare shot_* weapon parsed");
        const auto* flak88 = cat->find("88mmflak");
        check(flak88 && flak88->shot1.damage > 0 && flak88->shot1.range > 0,
              "retail UnitCatalog 88mm Flak bare shot_* weapon parsed");
        const auto* how150 = cat->find("150mm.japanese");
        check(how150 && how150->shot1.soundGroup == "howitzer" && how150->shot1.damage > 0,
              "retail UnitCatalog 150mm howitzer shot1_sound overrides shot_sound");
        const auto* supply = cat->find("sdkfz.11");
        check(supply && supply->isSupplyTruck() && supply->ammoRegenDelay > 0,
              "retail UnitCatalog Sd.Kfz.11 detected as supply truck");
        const auto* medTruck = cat->find("gmc.353.med");
        check(medTruck && medTruck->isAmbulance() && medTruck->healDelay > 0,
              "retail UnitCatalog GMC 353 Med detected as ambulance");
        const auto* t34 = cat->find("t-34");
        check(t34 && t34->nation == "russian" && t34->roleClass == "tank" && t34->moveDamageHp == 220 &&
                  t34->canMoveBackward && t34->backMoveSpeed > 0.0,
              "retail UnitCatalog T-34 nation/roleClass/moveDamageHp/reverse match");
        const auto* tiger4v = cat->find("tiger");
        const auto* rmortar = cat->find("rmortar");
        const auto* dhmg = cat->find("dhmg");
        const auto* dgeneral = cat->find("dgeneral");
        const auto* cmedic = cat->find("cmedic");
        const auto* bazooka4v = cat->find("bazooka");
        check(tiger4v && tiger4v->turnDelay >= 12 && t34->turnDelay <= 6,
              "retail UnitCatalog turndelay parses Tiger >= 12 and T-34 <= 6");
        check(rmortar && rmortar->isMortarInfantry() &&
                  dhmg && dhmg->isHeavyMgInfantry() &&
                  dgeneral && dgeneral->isOfficerInfantry() &&
                  cmedic && cmedic->isMedicInfantry() &&
                  bazooka4v && bazooka4v->isAntiTankInfantry(),
              "retail UnitCatalog specialist infantry helpers match rmortar/dhmg/dgeneral/cmedic/bazooka");
        const auto us1 = ss2::assets::findCampaignMission(gameDir, "us:1");
        const auto amer1 = ss2::assets::findCampaignMission(gameDir, "american:1");
        const auto sov1 = ss2::assets::findCampaignMission(gameDir, "soviet:1");
        const auto uk1 = ss2::assets::findCampaignMission(gameDir, "uk:1");
        const auto brit1 = ss2::assets::findCampaignMission(gameDir, "british:1");
        check(us1.has_value() && us1->code == "016000" && amer1.has_value() && amer1->path == us1->path,
              "findCampaignMission us:1 and american:1 resolve US #1 (016000)");
        check(sov1.has_value() && sov1->code == "000000",
              "findCampaignMission soviet:1 resolves Soviet #1 (000000)");
        check(uk1.has_value() && brit1.has_value() && uk1->path == brit1->path,
              "findCampaignMission uk:1 matches british:1");
        const auto retailCampaigns = ss2::assets::listCampaigns(gameDir);
        check(retailCampaigns.size() == 5, "listCampaigns returns all 5 Gold campaigns");
        check(!retailCampaigns[0].missions.empty() &&
              !retailCampaigns[0].missions[0].description.empty() &&
              !retailCampaigns[0].missions[0].easyPath.empty(),
              "listCampaigns populates mission I-DESC descriptions and EASY paths");
        check(ss2::assets::listCustomCampaigns(gameDir, {}).empty(),
              "listCustomCampaigns ignores retail maps/custom_campaign/dummy.txt");
        const auto arnhem = ss2::assets::findCampaignMission(gameDir, "031000");
        check(arnhem.has_value(), "findCampaignMission finds 031000");
        const auto arnhemData = ss2::assets::readMissionData(arnhem->path);
        check(arnhemData.playerTeams == std::array<int, 4>{0, 1, 0, 3},
              "retail 031000.ssc I-PLAYERS teams ally slot 2 with player");
        check(pak40->isTowable() && pak40->towWeightClass == "handy",
              "retail UnitCatalog PaK 40 canbetowed handy parsed");
        check(flak88->isTowable() && flak88->towWeightClass == "heavy",
              "retail UnitCatalog 88mm Flak canbetowed heavy parsed");
        const auto* long150 = cat->find("150mm.longrange");
        check(long150 && !long150->isTowable() && long150->towWeightClass == "static",
              "retail UnitCatalog static 150mm gun is not towable");
        const auto* zis5 = cat->find("zis-5");
        check(zis5 && zis5->canTowAny() && zis5->towSpeedFactor("handy") > 0.5,
              "retail UnitCatalog ZiS-5 truck can tow handy guns");
        {
            ss2::assets::MapImage bldMap{
                (arnhemData.width + arnhemData.height) * 32u,
                (arnhemData.width + arnhemData.height) * 16u,
                std::vector<std::uint8_t>(std::size_t((arnhemData.width + arnhemData.height) * 32u) *
                                         ((arnhemData.width + arnhemData.height) * 16u) * 4u, 0)
            };
            std::vector<ss2::assets::BuildingPlacement> blds;
            (void)ss2::assets::drawMissionScenery(bldMap, gameDir, arnhem->path, nullptr, &blds);
            check(blds.size() >= 50, "retail 031000.ssc drawMissionScenery extracts >= 50 houses");
            std::size_t ruinCount = 0;
            std::size_t progressiveCount = 0;
            bool anyRuinOpaque = false;
            const auto hashRgba = [](const std::vector<std::uint8_t>& bytes) {
                std::uint64_t h = 14695981039346656037ull;
                for (std::uint8_t v : bytes) {
                    h ^= v;
                    h *= 1099511628211ull;
                }
                return h;
            };
            for (const auto& b : blds) {
                check(b.capacity >= 2 && b.capacity <= 16 && b.maxHp >= 250 && b.width > 0 && b.height > 0,
                      "retail 031000.ssc house capacity/bounds valid");
                if (b.ruinedPatch.width > 0 && b.ruinedPatch.height > 0) {
                    ++ruinCount;
                    check(b.basePatch.width == b.ruinedPatch.width && b.basePatch.height == b.ruinedPatch.height,
                          "retail 031000.ssc house basePatch matches ruinedPatch size");
                    for (std::size_t pi = 3; pi < b.ruinedPatch.rgba.size(); pi += 4u) {
                        if (b.ruinedPatch.rgba[pi] > 0) {
                            anyRuinOpaque = true;
                            break;
                        }
                    }
                    const auto s0 = ss2::assets::renderHouseStage(
                        b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 0);
                    const auto s1 = ss2::assets::renderHouseStage(
                        b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 1);
                    const auto s2 = ss2::assets::renderHouseStage(
                        b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 2);
                    const auto s3 = ss2::assets::renderHouseStage(
                        b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 3);
                    const auto h0 = hashRgba(s0.image.rgba);
                    const auto h1 = hashRgba(s1.image.rgba);
                    const auto h2 = hashRgba(s2.image.rgba);
                    const auto h3 = hashRgba(s3.image.rgba);
                    if (h0 != h1 && h1 != h2 && h2 != h3 && h0 != h3 &&
                        s0.bodyCount > 0u && s1.bodyCount > 0u && s2.bodyCount > 0u && s3.bodyCount == 0u) {
                        ++progressiveCount;
                    }
                }
            }
            check(ruinCount >= 40 && anyRuinOpaque,
                  "retail 031000.ssc pre-renders dom.res ruinedPatch on houses");
            check(progressiveCount == ruinCount && progressiveCount >= 40,
                  "retail 031000.ssc renders distinct 4-stage dom.res damage states on houses");
        }
        const auto beach = ss2::assets::findCampaignMission(gameDir, "002000");
        check(beach.has_value(), "findCampaignMission finds 002000");
        const auto beachData = ss2::assets::readMissionData(beach->path);
        check(beachData.mines.size() == std::size_t(beachData.width) * beachData.height,
              "retail 002000.ssc I-MINES matches tile grid size");
        const auto mineCount = std::ranges::count_if(beachData.mines, [](std::uint8_t b) { return (b & 0x01u) != 0; });
        check(mineCount >= 100, "retail 002000.ssc contains >= 100 I-MINES tiles");
    }

    // Verify all 5 retail campaigns (31 Normal + EASY/.ssc missions) load clean across I-CINFO crops.
    {
        const auto campaigns = ss2::assets::listCampaigns(gameDir);
        check(campaigns.size() == 5, "retail sweep expected 5 campaigns");
        std::size_t totalCampaignMissions = 0;
        for (const auto& c : campaigns) {
            for (const auto& cm : c.missions) {
                ++totalCampaignMissions;
                const auto cData = ss2::assets::readMissionData(cm.path);
                check(cData.width == cm.width && cData.height == cm.height && cData.scheme == cm.scheme,
                      "retail campaign metadata matches: " + cm.code);
                const auto cOv = ss2::assets::readMissionOverview(cm.path);
                check(cOv.width > 0 && cOv.height > 0, "retail campaign overview non-empty: " + cm.code);
                const auto cPl = ss2::assets::readMissionPlacements(cm.path);
                check(!cPl.empty(), "retail campaign placements non-empty: " + cm.code);
                if (cm.code == "022000") {
                    std::size_t paxTotal = 0;
                    for (const auto& u : cPl) paxTotal += u.passengers.size();
                    check(paxTotal >= 40, "retail 022000.ssc expected at least 40 crew/riders");
                }
                if (!cm.easyPath.empty()) {
                    const auto ePl = ss2::assets::readMissionPlacements(cm.easyPath);
                    check(!ePl.empty(), "retail EASY campaign placements non-empty: " + cm.code);
                }
            }
        }
        check(totalCampaignMissions == 31, "retail sweep expected 31 campaign missions");
    }

    // Verify dynamic runtime infantry spawn in InfantryScene against retail SUE archives.
    {
        const ss2::assets::MapImage bg{128, 128, std::vector<std::uint8_t>(128u * 128u * 4u, 42u)};
        ss2::assets::InfantryScene scene(bg, gameDir, {});
        check(scene.units().empty(), "empty initial placement starts with 0 units");
        check(!scene.spawn({"pz4", 64, 64, 0, 100, 100, 0, 0}).has_value(),
              "non-infantry vehicle spawn returns nullopt");
        const auto s0 = scene.spawn({"dguner", 64, 64, 2, 100, 90, 1, 0});
        check(s0.has_value() && s0->first == 0, "dynamic dguner spawn returns index 0");
        check(s0->second.pixels.width > 0 && s0->second.pixels.height > 0, "dguner spawn patch non-empty");
        const auto s1 = scene.spawn({"dofficer", 80, 64, 0, 100, 90, 1, 0});
        check(s1.has_value() && s1->first == 1, "dynamic dofficer spawn returns index 1");
        check(scene.units().size() == 2 && scene.units()[0].id == "dguner" && scene.units()[1].id == "dofficer",
              "spawned infantry appended to InfantryScene units");

        const auto infPortrait = scene.renderUnitPortrait("dguner", 48u);
        check(infPortrait.has_value() && infPortrait->width == 48u && infPortrait->height == 48u,
              "InfantryScene::renderUnitPortrait builds 48x48 retail sprite portrait");
        {
            std::uint8_t minR = 255u, maxR = 0u;
            for (std::size_t i = 0; i < infPortrait->rgba.size(); i += 4u) {
                minR = std::min(minR, infPortrait->rgba[i]);
                maxR = std::max(maxR, infPortrait->rgba[i]);
            }
            check(maxR - minR > 12, "Infantry portrait has non-uniform sprite detail");
        }

        auto patched = bg;
        const auto applyPatch = [](ss2::assets::MapImage& dst, const ss2::assets::MapPatch& patch) {
            for (unsigned r = 0; r < patch.pixels.height; ++r) {
                std::copy_n(patch.pixels.rgba.data() + r * std::size_t(patch.pixels.width) * 4,
                            patch.pixels.width * 4,
                            dst.rgba.data() + ((patch.y + r) * std::size_t(dst.width) + patch.x) * 4);
            }
        };
        applyPatch(patched, s0->second);
        applyPatch(patched, s1->second);
        check(patched.rgba != bg.rgba, "spawned infantry patches paint pixels");
        check(patched.rgba == scene.compose().rgba, "spawned infantry patches match compose()");

        // Verify copyGroundRect, stampGroundPatch restore, and stampAlphaOverlay round-trip cleanly.
        const auto savedRect = scene.copyGroundRect(16, 16, 24, 24);
        check(savedRect.width == 24 && savedRect.height == 24 && savedRect.rgba.size() == 24u * 24u * 4u,
              "copyGroundRect returns exact RGBA dimensions");
        ss2::assets::MapImage stamp{16, 16, std::vector<std::uint8_t>(16u * 16u * 4u, 0)};
        for (std::size_t i = 0; i < stamp.rgba.size(); i += 4u) {
            stamp.rgba[i] = 200;
            stamp.rgba[i + 1] = 100;
            stamp.rgba[i + 2] = 50;
            stamp.rgba[i + 3] = 255;
        }
        const auto craterPatch = scene.stampAlphaOverlay(28, 28, stamp);
        check(craterPatch.pixels.width > 0 && scene.compose().rgba != patched.rgba,
              "stampAlphaOverlay modifies ground composition");
        (void)scene.stampGroundPatch(16, 16, savedRect);
        check(scene.compose().rgba == patched.rgba,
              "stampGroundPatch restores copied intact ground rect pixel-for-pixel");
    }

    // Verify retail Phase 4E I-PLAYERS air support, aircraft PCK sprites & vor.res shell craters.
    {
        const auto r1 = ss2::assets::findCampaignMission(gameDir, "000000");
        const auto r2 = ss2::assets::findCampaignMission(gameDir, "011000");
        const auto r3 = ss2::assets::findCampaignMission(gameDir, "024000");
        check(r1 && r2 && r3, "findCampaignMission finds 000000/011000/024000");
        const auto d1 = ss2::assets::readMissionData(r1->path);
        const auto d2 = ss2::assets::readMissionData(r2->path);
        const auto d3 = ss2::assets::readMissionData(r3->path);
        check(d1.airSupport[0][1].planeId == "audax" && d1.airSupport[0][1].ready == 3,
              "000000.ssc player 0 Recon slot is audax x3");
        check(d2.airSupport[0][4].planeId == "il-2" && d2.airSupport[0][4].ready == 10,
              "011000.ssc player 0 Fighter slot is il-2 x10");
        check(d3.airSupport[0][2].planeId == "ju-52" && d3.airSupport[0][2].ready == 1 &&
                  !d3.paratrooperRoster[0].empty() && d3.paratrooperRoster[0][0].unitId == "dsmguner",
              "024000.ssc player 0 Paratrooper slot & roster parsed");
        const auto barb = ss2::assets::findSinglePlayerMission(gameDir, "Barbarossa");
        check(barb.has_value(), "findSinglePlayerMission finds Barbarossa");
        const auto dBarb = ss2::assets::readMissionData(barb->path);
        check(dBarb.airSupport[0][0].planeId == "ju-87" && dBarb.airSupport[0][0].ready == 5 &&
                  dBarb.airSupport[0][4].planeId == "bf-109" && dBarb.airSupport[0][4].ready == 3,
              "Barbarossa player 0 Ju-87 Bomber & Bf-109 Fighter sorties parsed");

        const auto ju87 = ss2::assets::loadAircraftSprite(gameDir, "ju-87", 8);
        const auto b17 = ss2::assets::loadAircraftSprite(gameDir, "b-17c", 0);
        auto hasOpaque = [](const ss2::assets::MapImage& im) {
            for (std::size_t i = 3; i < im.rgba.size(); i += 4u) {
                if (im.rgba[i] > 0) return true;
            }
            return false;
        };
        check(ju87 && ju87->width > 20 && hasOpaque(*ju87), "loadAircraftSprite ju-87 decoded");
        check(b17 && b17->width > 40 && hasOpaque(*b17), "loadAircraftSprite b-17c decoded");

        const auto craters = ss2::assets::loadCraterCatalog(gameDir, 0);
        check(!craters.smallCraters.empty() && !craters.largeCraters.empty() &&
                  hasOpaque(craters.smallCraters.front()) && hasOpaque(craters.largeCraters.front()),
              "loadCraterCatalog loads retail vor.res small & large shell craters");

        // Verify Phase 4F retail active-crew flag: 000000.ssc group 98 has 4 unmanned owner=3 sturmtigers (hasCrew=false).
        ss2::assets::VehicleScene scene0(gameDir, ss2::assets::readMissionPlacements(r1->path));
        unsigned emptySturmtigers = 0;
        for (const auto& v : scene0.vehicles()) {
            if (v.id == "sturmtiger" && v.owner == 3 && v.group == 98 && !v.hasCrew) {
                ++emptySturmtigers;
            }
        }
        check(emptySturmtigers == 4u,
              "000000.ssc VehicleScene marks all 4 unmanned group-98 sturmtigers with hasCrew=false");
        if (!scene0.vehicles().empty()) {
            const auto& v0 = scene0.vehicles().at(0);
            (void)scene0.update(0, v0.x, v0.y, v0.hullDir, v0.turretDir, -1, -1, 17);
            check(scene0.vehicles().at(0).ammo == 17,
                  "VehicleScene::update synchronizes live vehicle ammo count");
        }

        // Verify Phase 4G retail vehicle portraits and rocket artillery UnitCatalog parsing.
        for (const char* vid : {"sturmtiger", "t-34"}) {
            const auto vp = scene0.renderUnitPortrait(vid, 48u);
            check(vp.has_value() && vp->width == 48u && vp->height == 48u,
                  "VehicleScene::renderUnitPortrait builds 48x48 retail sprite portrait");
            std::uint8_t minR = 255u, maxR = 0u;
            for (std::size_t i = 0; i < vp->rgba.size(); i += 4u) {
                minR = std::min(minR, vp->rgba[i]);
                maxR = std::max(maxR, vp->rgba[i]);
            }
            check(maxR - minR > 12, "Vehicle portrait has non-uniform sprite detail");
        }
        const auto cat4g = ss2::assets::UnitCatalog::load(gameDir);
        const auto* bm31 = cat4g ? cat4g->find("bm-31") : nullptr;
        const auto* nebel = cat4g ? cat4g->find("nebelwerfer.42") : nullptr;
        check(bm31 && bm31->isRocketArtillery() && bm31->isArtillery() && bm31->shot1.burstShots > 1,
              "Retail UnitCatalog bm-31 detected as multi-shot rocket artillery");
        check(nebel && nebel->isRocketArtillery() && nebel->isArtillery() && nebel->shot1.burstShots > 1,
              "Retail UnitCatalog nebelwerfer.42 detected as multi-shot rocket artillery");
        const auto* dflamer = cat4g ? cat4g->find("dflamer") : nullptr;
        const auto* ot34 = cat4g ? cat4g->find("ot-34") : nullptr;
        const auto* sniper = cat4g ? cat4g->find("sniper") : nullptr;
        check(dflamer && dflamer->hasFlamethrower(), "Retail UnitCatalog dflamer detected as flamethrower");
        check(ot34 && ot34->shot2.isFlamethrower() && ot34->hasFlamethrower() && ot34->marchSightBonus == 4,
              "Retail UnitCatalog ot-34 detected with secondary flamethrower & marchsightbonus");
        check(sniper && sniper->isSniper(), "Retail UnitCatalog sniper detected as sniper");
        const auto* tiger = cat4g ? cat4g->find("tiger") : nullptr;
        const auto* pak40 = cat4g ? cat4g->find("pak40") : nullptr;
        check(tiger && tiger->marchSightBonus == 4 && tiger->ammo1 == 94 && tiger->ammo2 == 5100,
              "Retail UnitCatalog tiger parses marchsightbonus=4, ammo1=94, ammo2=5100");
        check(pak40 && pak40->ammo1 == 24 && pak40->isArtillery(),
              "Retail UnitCatalog pak40 parses ammo1=24");
        const auto* dgen = cat4g ? cat4g->find("dgeneral") : nullptr;
        const auto* doff = cat4g ? cat4g->find("dofficer") : nullptr;
        const auto* kami = cat4g ? cat4g->find("kamikaze") : nullptr;
        const auto* goliath = cat4g ? cat4g->find("sdkfz.303") : nullptr;
        check(sniper->binocularSight == 16 && sniper->hasBinoculars(),
              "Retail UnitCatalog sniper parses binocular=16");
        check(dgen && dgen->binocularSight == 32 && dgen->hasBinoculars(),
              "Retail UnitCatalog dgeneral parses binocular=32");
        check(doff && doff->binocularSight == 19 && doff->hasBinoculars(),
              "Retail UnitCatalog dofficer parses binocular=19");
        check(kami && kami->kamikazePct == 100 && kami->isKamikaze() && !kami->isVehicle,
              "Retail UnitCatalog kamikaze parses kamikazepctg=100");
        check(goliath && goliath->kamikazePct == 101 && goliath->isKamikaze() && goliath->isVehicle,
              "Retail UnitCatalog sdkfz.303 parses kamikazepctg=101");
        check(pak40 && pak40->hasCrewProtection && pak40->hasExposedCrew() && pak40->canSelfRepair &&
                  pak40->crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)].rear == 0 &&
                  pak40->crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)].front == 45,
              "Retail UnitCatalog pak40 parses directional crew protection & repair");
        check(tiger && tiger->hasCrewProtection && !tiger->hasExposedCrew() && tiger->canSelfRepair &&
                  tiger->crewUnit == "dtankist" &&
                  tiger->crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Fire)].front == 1 &&
                  tiger->crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)].front == 256,
              "Retail UnitCatalog tiger parses closed crew protection, dtankist crew & repair");
        const auto* jlanding = cat4g ? cat4g->find("japan.landing") : nullptr;
        check(tiger && tiger->dieExplosion == "dead_tank" && tiger->scoreValue == 1180 &&
                  tiger->scoreType == "tank" && tiger->attackCrew == 3 && tiger->moveCrew == 1 &&
                  tiger->cookOffDamage() == 160,
              "Retail UnitCatalog tiger parses dieexplosion, scorevalue/type & attackcrew/movecrew");
        check(pak40 && pak40->dieExplosion == "mine4-5k" && pak40->scoreValue == 770 &&
                  pak40->scoreType == "gun" && pak40->cookOffDamage() == 160,
              "Retail UnitCatalog pak40 parses dieexplosion & scorevalue/type");
        check(jlanding && jlanding->isNavalCraft() && jlanding->walkOnWater &&
                  !jlanding->walkOnGround && jlanding->walkOnShallows &&
                  jlanding->forbiddenTerrainMask() == 0x000Fu,
              "Retail UnitCatalog japan.landing parses naval walkonwater terrain mask");
        const auto* ammoBox = cat4g ? cat4g->find("ammo.box") : nullptr;
        const auto* spareParts = cat4g ? cat4g->find("spare.parts") : nullptr;
        const auto* sdkfz11 = cat4g ? cat4g->find("sdkfz.11") : nullptr;
        check(ammoBox && ammoBox->isSupplyCrate() && ammoBox->isSupplyTruck() &&
                  ammoBox->ammo1 == 1000 && ammoBox->ammo2 == 0,
              "Retail UnitCatalog ammo.box parses specialized ammo crate");
        check(spareParts && spareParts->isSupplyCrate() && spareParts->isSupplyTruck() &&
                  spareParts->ammo1 == 0 && spareParts->ammo2 == 1000,
              "Retail UnitCatalog spare.parts parses specialized spare parts crate");
        check(sdkfz11 && sdkfz11->isSupplyTruck() && !sdkfz11->isSupplyCrate() &&
                  sdkfz11->ammo1 == 15000 && sdkfz11->ammo2 == 5000,
              "Retail UnitCatalog sdkfz.11 parses finite supply truck stocks");
        const auto* dhmg = cat4g ? cat4g->find("dhmg") : nullptr;
        const auto* sdkfz251 = cat4g ? cat4g->find("sdkfz.251") : nullptr;
        const auto* locomotive = cat4g ? cat4g->find("locomotive") : nullptr;
        check(dhmg && dhmg->hasAttackPref && dhmg->attackPreferenceFor("gun") == 253 &&
                  dhmg->attackPreferenceFor("car") == 250 && dhmg->attackPreferenceFor("man") == 249 &&
                  dhmg->attackPreferenceFor("heavytank") == 1,
              "Retail UnitCatalog dhmg parses 14-class attackpref weights");
        check(sdkfz251 && sdkfz251->hasAttackPref && sdkfz251->attackPreferenceFor("gun") == 255 &&
                  sdkfz251->attackPreferenceFor("man") == 251 && sdkfz251->attackPreferenceFor("heavytank") == 1 &&
                  sdkfz251->isTracked() && !sdkfz251->isWheeled(),
              "Retail UnitCatalog sdkfz.251 parses attackpref and tracked engine");
        check(tiger && tiger->hasAttackPref && tiger->attackPreferenceFor("heavytank") == 255 &&
                  tiger->isTracked() && !tiger->isWheeled(),
              "Retail UnitCatalog tiger parses attackpref HEAVYTANK 255 and track engine");
        check(bm31 && bm31->isWheeled() && !bm31->isTracked(),
              "Retail UnitCatalog bm-31 parses Gold 2.2 engine whell typo");
        check(locomotive && locomotive->isTrain(),
              "Retail UnitCatalog locomotive parses train seltype/targettype");
        const auto* planeB17c = cat4g ? cat4g->findPlane("b-17c") : nullptr;
        const auto* planeJu87 = cat4g ? cat4g->findPlane("ju-87") : nullptr;
        const auto* planeBf109 = cat4g ? cat4g->findPlane("bf-109") : nullptr;
        const auto* planeAudax = cat4g ? cat4g->findPlane("audax") : nullptr;
        const auto* planeC47 = cat4g ? cat4g->findPlane("c47") : nullptr;
        check(planeB17c && planeB17c->health == 2050 && planeB17c->scoreValue == 7100 &&
                  planeB17c->bombsNumber == 20 && planeB17c->bombDamage == 3000 && planeB17c->sight == 24,
              "Retail UnitCatalog findPlane b-17c parses bomber HP/score/bombs/sight");
        check(planeJu87 && planeJu87->health == 550 && planeJu87->scoreValue == 4100 && planeJu87->bombsNumber == 4,
              "Retail UnitCatalog findPlane ju-87 parses dive-bomber stats");
        check(planeBf109 && planeBf109->health == 300 && planeBf109->scoreValue == 600 &&
                  planeBf109->groundDamage == 45 && planeBf109->airDamage == 45 && planeBf109->flySpeed > 12.0,
              "Retail UnitCatalog findPlane bf-109 parses fighter weapons/speed/score");
        check(planeAudax && planeAudax->health == 450 && planeAudax->scoreValue == 900 && planeAudax->sight == 20,
              "Retail UnitCatalog findPlane audax parses recon sight/score");
        check(planeC47 && planeC47->health == 800 && planeC47->scoreValue == 1600,
              "Retail UnitCatalog findPlane c47 parses transport stats");
        const auto* kv2 = cat4g ? cat4g->find("kv-2") : nullptr;
        const auto* sdkfz222 = cat4g ? cat4g->find("sdkfz.222") : nullptr;
        const auto* mortar82 = cat4g ? cat4g->find("82mm") : nullptr;
        const auto* willyJeep = cat4g ? cat4g->find("wyllis.mb.jeep") : nullptr;
        check(tiger && tiger->hasGunTurnDelay && tiger->gunTurnDelay == 13 && tiger->hasCrushRange,
              "Retail UnitCatalog tiger parses gunturndelay=13 & davirange");
        check(kv2 && kv2->hasGunTurnDelay && kv2->gunTurnDelay == 14 && kv2->hasCrushRange,
              "Retail UnitCatalog kv-2 parses gunturndelay=14 & davirange");
        check(sdkfz222 && sdkfz222->hasGunTurnDelay && sdkfz222->gunTurnDelay == 0,
              "Retail UnitCatalog sdkfz.222 parses fast turret gunturndelay=0");
        check(mortar82 && mortar82->canBeCrushed,
              "Retail UnitCatalog 82mm parses canbecrushed=1");
        check(willyJeep && willyJeep->passengersCanFire,
              "Retail UnitCatalog wyllis.mb.jeep parses passcanfire=1");
        const auto* wirbelwind = cat4g ? cat4g->find("wirbelwind") : nullptr;
        const auto* gun52k = cat4g ? cat4g->find("52-k") : nullptr;
        const auto* ba64Agit = cat4g ? cat4g->find("ba64.agit") : nullptr;
        const auto* germanCutter = cat4g ? cat4g->find("german.cutter") : nullptr;
        check(tiger && tiger->shot1.accuracyBase == 33 && tiger->shot1.accuracyMin == 29 &&
                  tiger->shot1.expaPerShot == 88,
              "Retail UnitCatalog tiger parses shot1_accuracy=33/29 and shot1_expa=88");
        check(kv2 && kv2->shot1.accuracyBase == 73 && kv2->shot1.accuracyMin == 65 &&
                  kv2->shot1.expaPerShot == 152,
              "Retail UnitCatalog kv-2 parses shot1_accuracy=73/65 and shot1_expa=152");
        check(willyJeep && willyJeep->bonusShotRange == 20,
              "Retail UnitCatalog wyllis.mb.jeep parses bonus_shotrange=20");
        check(wirbelwind && wirbelwind->selType == "aagun" && wirbelwind->isAntiAir() &&
                  gun52k && gun52k->selType == "mobairdef" && gun52k->isAntiAir(),
              "Retail UnitCatalog wirbelwind (aagun) & 52-k (mobairdef) detected as isAntiAir()");
        check(ba64Agit && ba64Agit->isPropaganda(),
              "Retail UnitCatalog ba64.agit parses agit propaganda role");
        check(germanCutter && germanCutter->isGunboat() && germanCutter->isNavalCraft(),
              "Retail UnitCatalog german.cutter parses gunboat + naval craft flags");

        // Phase 5F: soldonarmor Tank Desant, General/Officer aura & moraleresist,
        // reload1/2 resupply cost, shot*_endburstsound & AVIA sounds.
        const auto* commandoSpec = cat4g ? cat4g->find("commando") : nullptr;
        const auto* longRange150 = cat4g ? cat4g->find("150mm.longrange") : nullptr;
        const auto* dsmgunerSpec = cat4g ? cat4g->find("dsmguner") : nullptr;
        const auto* planeIl2 = cat4g ? cat4g->findPlane("il-2") : nullptr;
        check(tiger && tiger->soldiersOnArmor && sdkfz251 && !sdkfz251->soldiersOnArmor,
              "Retail UnitCatalog tiger parses soldonarmor while sdkfz.251 does not");
        check(doff && doff->officerRadius == 160 && doff->moraleAutoIncrease == 5 &&
                  doff->moraleResist == 1000 && doff->shot1.endBurstSound == "rifle_reload",
              "Retail UnitCatalog dofficer parses officerradius/morale/endburstsound");
        check(dgen && dgen->officerRadius == 350 && dgen->moraleAutoIncrease == 10 &&
                  dgen->moraleResist == 10000,
              "Retail UnitCatalog dgeneral parses wide officerradius=350 & high moraleresist=10000");
        check(commandoSpec && commandoSpec->moraleResist == 1000 &&
                  commandoSpec->shot1.resupplyCost == 10,
              "Retail UnitCatalog commando parses moraleresist=1000 & reload1=10");
        check(tiger->shot1.resupplyCost == 96 && tiger->shot2.resupplyCost == 2 &&
                  pak40->shot1.resupplyCost == 80 && longRange150 &&
                  longRange150->shot1.resupplyCost == 500 && dsmgunerSpec &&
                  dsmgunerSpec->shot1.resupplyCost == 1,
              "Retail UnitCatalog reload1/reload2 ammo resupply cost parsed accurately");
        check(sdkfz251->shot1.endBurstSound == "gilse" && dhmg &&
                  dhmg->shot1.endBurstSound == "gilse",
              "Retail UnitCatalog sdkfz.251 & dhmg parse shot1_endburstsound=gilse");
        check(planeB17c->idleSound == "fly_heavy" && planeB17c->bombSound == "bomb" &&
                  planeB17c->fallSound == "falling_heavy",
              "Retail UnitCatalog findPlane b-17c parses idle/bomb/fall audio");
        check(planeBf109->idleSound == "fly_small" && planeBf109->fallSound == "fall_small" &&
                  planeBf109->groundShotSound == "mg2_aa" && planeBf109->airShotSound == "mg_aa",
              "Retail UnitCatalog findPlane bf-109 parses fighter audio");
        check(planeIl2 && planeIl2->groundShotSound == "rocket" &&
                  planeIl2->idleSound == "fly_small" && planeIl2->fallSound == "fall_small",
              "Retail UnitCatalog findPlane il-2 parses rocket ground-attack audio");

        // Phase 5G: retail repair cost, gunshotwait stabilization, radius, canbecrew & bombid=fsheet.
        const auto* dtankistSpec = cat4g ? cat4g->find("dtankist") : nullptr;
        const auto* dpilotSpec = cat4g ? cat4g->find("dpilot") : nullptr;
        const auto* flak88Spec = cat4g ? cat4g->find("88mmflak") : nullptr;
        const auto* planeB17Agit = cat4g ? cat4g->findPlane("b-17c_agit") : nullptr;
        const auto* planeHe111Agit = cat4g ? cat4g->findPlane("he_111_agit") : nullptr;
        check(tiger->repairCost == 36 && kv2->repairCost == 33 && longRange150->repairCost == 16 &&
                  flak88Spec && flak88Spec->repairCost == 7 && sdkfz251->repairCost == 6 &&
                  pak40->repairCost == 4 && mortar82->repairCost == 2,
              "Retail UnitCatalog repair parses spare-parts cost per HP");
        check(tiger->gunShotWait == 8 && kv2->gunShotWait == 15 && sdkfz251->gunShotWait == 0,
              "Retail UnitCatalog gunshotwait parses heavy main-gun stabilization delay");
        check(tiger->unitRadius == 23 && kv2->unitRadius == 20 && sdkfz251->unitRadius == 9 &&
                  pak40->unitRadius == 8 && dsmgunerSpec->unitRadius == 5,
              "Retail UnitCatalog radius parses physical unit collision footprint");
        check(dtankistSpec && dtankistSpec->canBeCrew == 1 && dtankistSpec->isQualifiedVehicleCrew() &&
                  doff->canBeCrew == 1 && dpilotSpec && dpilotSpec->canBeCrew == 2 &&
                  commandoSpec->canBeCrew == 3 && dsmgunerSpec->canBeCrew == 0 &&
                  !dsmgunerSpec->isQualifiedVehicleCrew(),
              "Retail UnitCatalog canbecrew distinguishes crew specialists from standard infantry");
        check(planeB17c->bombId == "bomb" && !planeB17c->isLeafletBomber() &&
                  planeB17Agit && planeB17Agit->isLeafletBomber() &&
                  planeHe111Agit && planeHe111Agit->isLeafletBomber(),
              "Retail UnitCatalog findPlane parses bombid=fsheet on propaganda leaflet bombers");

        // Phase 5H: canfirefrominside (PIAT vs backblast), crouchtofire, crouchsight,
        // shot*_useammo, and static fortifications / kamikaze.boat.
        const auto* piatSpec = cat4g ? cat4g->find("piat") : nullptr;
        const auto* pfaustSpec = cat4g ? cat4g->find("panzerfaust") : nullptr;
        const auto* gatsSpec = cat4g ? cat4g->find("gats") : nullptr;
        const auto* ot34Spec = cat4g ? cat4g->find("ot-34") : nullptr;
        const auto* is2Spec = cat4g ? cat4g->find("is-2") : nullptr;
        const auto* mg34Spec = cat4g ? cat4g->find("mg34") : nullptr;
        const auto* kamiBoatSpec = cat4g ? cat4g->find("kamikaze.boat") : nullptr;
        check(piatSpec && piatSpec->canFireFromInside && piatSpec->hasExplicitInsideRule &&
                  pfaustSpec && !pfaustSpec->canFireFromInside && pfaustSpec->hasExplicitInsideRule,
              "Retail UnitCatalog piat vs panzerfaust parses canfirefrominside backblast rules");
        check(gatsSpec && gatsSpec->crouchToFire && !gatsSpec->canFireFromInside &&
                  gatsSpec->crouchSight == 8 && gatsSpec->crouchSightMax == 9,
              "Retail UnitCatalog gats parses crouchtofire & crouchsight 8..9");
        check(doff->crouchSight == 9 && doff->crouchSightMax == 10 &&
                  sniper->crouchSight == 9 && dsmgunerSpec->crouchSight == 8 &&
                  dsmgunerSpec->crouchSightMax == 9,
              "Retail UnitCatalog parses retail crouchsight across infantry descriptors");
        check(ot34Spec && ot34Spec->shot2.ammoPerShot == 10 && is2Spec &&
                  is2Spec->shot2.ammoPerShot == 3 && kv2->shot2.ammoPerShot == 3 &&
                  ba64Agit->shot1.ammoPerShot == 0 && tiger->shot1.ammoPerShot == 1,
              "Retail UnitCatalog shot*_useammo parses multi-round and zero-ammo consumption");
        check(mg34Spec && mg34Spec->isFortification() && kamiBoatSpec &&
                  kamiBoatSpec->isKamikaze() && kamiBoatSpec->isGunboat() &&
                  kamiBoatSpec->isNavalCraft(),
              "Retail UnitCatalog mg34 fortification & kamikaze.boat naval kamikaze detected");

        // Phase 5I: crew_ver crew vulnerability, expa_loosehp combat XP under fire,
        // soldtomove gun-crew immobility & movesmoke exhaust flag.
        const auto* agit2593Spec = cat4g ? cat4g->find("2593.agit") : nullptr;
        const auto* ozu2589Spec = cat4g ? cat4g->find("2589.ozu") : nullptr;
        const auto* gun25pSpec = cat4g ? cat4g->find("25p") : nullptr;
        const auto* cmedicSpec = cat4g ? cat4g->find("cmedic") : nullptr;
        check(agit2593Spec && agit2593Spec->crewVulnerability > 1.8 &&
                  sdkfz251->crewVulnerability > 0.5 && sdkfz251->crewVulnerability < 0.6 &&
                  ozu2589Spec && ozu2589Spec->crewVulnerability > 0.0 &&
                  ozu2589Spec->crewVulnerability < 0.1,
              "Retail UnitCatalog crew_ver parses crew vulnerability multipliers");
        check(tiger->crewVulnerability > 0.85 && kv2->crewVulnerability > 0.55 &&
                  kv2->crewVulnerability < 0.70,
              "Retail UnitCatalog tiger & kv-2 parse heavy tank crew_ver");
        check(dsmgunerSpec->expaOnHit == 100 && doff->expaOnHit == 100 &&
                  tiger->expaOnHit == 1 && kv2->expaOnHit == 1 &&
                  cmedicSpec && cmedicSpec->expaOnHit == 0 && pak40->expaOnHit == 0,
              "Retail UnitCatalog expa_loosehp parses combat XP gained under fire");
        check(pak40->soldToMove == 2 && gun25pSpec && gun25pSpec->soldToMove == 2 &&
                  flak88Spec->soldToMove == 3 && longRange150->soldToMove == 3,
              "Retail UnitCatalog soldtomove parses field-gun relocation crew minimum");
        check(sdkfz251->hasMoveSmoke && !pak40->hasMoveSmoke,
              "Retail UnitCatalog movesmoke detects vehicle exhaust emitters");

        // Phase 5J: idlesnd_idle standby engine audio, vehicle moraleresist shock resistance,
        // and AVIA shot_air_sound dogfight audio.
        const auto* t3485Spec = cat4g ? cat4g->find("t-34-85") : nullptr;
        const auto* horchSpec = cat4g ? cat4g->find("horch") : nullptr;
        const auto* bf109Plane = cat4g ? cat4g->findPlane("bf-109") : nullptr;
        const auto* spitPlane = cat4g ? cat4g->findPlane("spitfire.mk-xvl") : nullptr;
        check(tiger->idleSound == "heavy_idle" && t3485Spec && t3485Spec->idleSound == "diesel_idle" &&
                  horchSpec && horchSpec->idleSound == "carb_standby" && pak40->idleSound.empty(),
              "Retail UnitCatalog idlesnd_idle parses vehicle standby engine groups");
        check(kv2->moraleResist == 12288 && tiger->moraleResist == 10240 &&
                  sdkfz251->moraleResist == 4096 && agit2593Spec->moraleResist == 2048,
              "Retail UnitCatalog moraleresist parses vehicle crew shock resistance");
        check(bf109Plane && bf109Plane->airShotSound == "mg_aa" &&
                  spitPlane && spitPlane->airShotSound == "mg_aa",
              "Retail UnitCatalog AVIA shot_air_sound parses fighter dogfight audio");

        // Phase 5K: retail soundtype, crewcanbehealed open-cab flag, and SHOTS/EXPLOSIONS resolution
        // (useh indirectArc, vor craterScale, impact sound).
        const auto* dingoSpec = cat4g ? cat4g->find("dingo.jeep") : nullptr;
        const auto* sturmtigerSpec = cat4g ? cat4g->find("sturmtiger") : nullptr;
        check(tiger->soundType == "tank" && pak40->soundType == "gun" &&
                  sdkfz251->soundType == "transport" && doff->soundType == "officer" &&
                  dsmgunerSpec->soundType == "soldier",
              "Retail UnitCatalog soundtype parses voice role mapping across units");
        check(horchSpec && horchSpec->crewCanBeHealed && dingoSpec && dingoSpec->crewCanBeHealed &&
                  !tiger->crewCanBeHealed && !kv2->crewCanBeHealed && !sdkfz251->crewCanBeHealed,
              "Retail UnitCatalog crewcanbehealed distinguishes open-cab jeeps from closed armor");
        check(longRange150->shot1.indirectArc && longRange150->shot1.craterScale == 2 &&
                  longRange150->shot1.impactSound == "howitzerexplosion",
              "Retail SHOTS\\ballistic & EXPLOSIONS\\howitzer resolve indirectArc, vor=2 & howitzerexplosion");
        check(sturmtigerSpec && sturmtigerSpec->shot1.indirectArc &&
                  sturmtigerSpec->shot1.craterScale == 3 &&
                  sturmtigerSpec->shot1.impactSound == "defaultexplosion",
              "Retail SHOTS\\sturmtiger resolves indirectArc, vor=3 & defaultexplosion");
        check(!tiger->shot1.indirectArc && tiger->shot1.craterScale == 4 &&
                  tiger->shot1.impactSound == "defaultexplosion",
              "Retail SHOTS\\pierce8-10k resolves direct trajectory, vor=4 & defaultexplosion");
        check(!gatsSpec->shot1.indirectArc && gatsSpec->shot1.craterScale == 0 &&
                  gatsSpec->shot1.impactSound == "ricochet_ground",
              "Retail SHOTS\\atrifle resolves useh=0, vor=NONE & ricochet_ground");

        // Phase 5L: retail LAND\DEFDOM building armor & protection, MISC\MISC StarScore table,
        // MISC\FRENCH fsold/ftank soundType, and AVIA fallsmoke/flysmoke.
        const auto& defDom = cat4g->buildingDefense();
        check(defDom.loaded && defDom.armorFor(ss2::assets::HitType::Machine) == 50 &&
                  defDom.armorFor(ss2::assets::HitType::Explosive) == 38 &&
                  defDom.armorFor(ss2::assets::HitType::Pierce) == 38 &&
                  defDom.armorFor(ss2::assets::HitType::Fire) == 0 &&
                  defDom.protectionFor(ss2::assets::HitType::Explosive) == 44 &&
                  defDom.protectionFor(ss2::assets::HitType::Mine) == 32 &&
                  defDom.protectionFor(ss2::assets::HitType::Machine) == 37 &&
                  defDom.protectionFor(ss2::assets::HitType::Fire) == 0,
              "Retail LAND\\DEFDOM parses building armor & occupant cover");
        check(cat4g->starScores().size() == 10u &&
                  cat4g->starRatingForScore(10000) == 0 &&
                  cat4g->starRatingForScore(25001) == 1 &&
                  cat4g->starRatingForScore(100001) == 3 &&
                  cat4g->starRatingForScore(400001) == 10,
              "Retail MISC\\MISC parses StarScore 1..10 rating thresholds");
        const auto* s35Spec = cat4g->find("s35");
        const auto* b1Spec = cat4g->find("b1");
        const auto* fatgSpec = cat4g->find("fatg");
        check(s35Spec && s35Spec->soundType == "ftank" &&
                  b1Spec && b1Spec->soundType == "ftank" &&
                  fatgSpec && fatgSpec->soundType == "fsold",
              "Retail MISC\\FRENCH units parse fsold/ftank voice routing");
        check(planeBf109->hasFallSmoke && planeB17c->hasFallSmoke,
              "Retail AVIA descriptors parse flysmoke/fallsmoke damaged aircraft trails");

        // Phase 5M: retail LAND\DOM garrison parameters, AVIA altitude & shot_grn_ammo,
        // and heavy vs light AA classification.
        const auto* p51Plane = cat4g ? cat4g->findPlane("p-51") : nullptr;
        const auto* wirbelSpec = cat4g ? cat4g->find("wirbelwind") : nullptr;
        check(defDom.unloadHp == 150 && defDom.bonusShotRange == 150,
              "Retail LAND\\DOM parses UnloadHP=150 & bonus_ShotRange=150");
        check(planeIl2->altitude == 200 && planeIl2->groundAmmo == 6 && planeIl2->isRocketAttacker() &&
                  planeB17c->altitude == 356 &&
                  planeBf109->altitude == 366 && planeBf109->groundAmmo == 550 && !planeBf109->isRocketAttacker() &&
                  p51Plane && p51Plane->altitude == 426,
              "Retail AVIA descriptors parse altitude and shot_grn_ammo rocket/cannon loadouts");
        check(flak88Spec && flak88Spec->isHeavyAntiAir() &&
                  wirbelSpec && wirbelSpec->isAntiAir() && !wirbelSpec->isHeavyAntiAir(),
              "Retail UnitSpec distinguishes heavy 88mm flak from light 20mm AA");

        // Phase 5N: retail MISC\MINES parameters, LAND\DOM bonus_ShotDeadZone,
        // MISC\MISC tanksonarmorprotection, and units/* canbepara flags.
        const auto& mine5n = cat4g->mineSpec();
        check(mine5n.loaded && mine5n.damage == 50 && mine5n.radius == 29 &&
                  mine5n.explDelayTicks == 1000 && mine5n.atShot == "mine_at" &&
                  mine5n.apShot == "mine_claymore" && mine5n.atSound == "defaultexplosion" &&
                  mine5n.apSound == "smallexplosion",
              "Retail MISC\\MINES parses damage/radius/expldelay/shot/shotsold & impact audio");
        check(defDom.bonusShotDeadZone == 150,
              "Retail LAND\\DOM parses bonus_ShotDeadZone=150");
        check(cat4g->tankArmorProtection(ss2::assets::HitType::Pierce) == 3 &&
                  cat4g->tankArmorProtection(ss2::assets::HitType::Explosive) == 3,
              "Retail MISC\\MISC parses tanksonarmorprotection=3 for Pierce & Explosive");
        const auto* para53k = cat4g ? cat4g->find("53-k") : nullptr;
        const auto* para8cm = cat4g ? cat4g->find("8cm.mortar") : nullptr;
        const auto* paraCrate = cat4g ? cat4g->find("ammo.box") : nullptr;
        const auto* paraKubel = cat4g ? cat4g->find("vwcubel.jeep") : nullptr;
        check(para53k && para53k->canBeParadropped &&
                  para8cm && para8cm->canBeParadropped &&
                  paraCrate && paraCrate->canBeParadropped &&
                  paraKubel && paraKubel->canBeParadropped &&
                  dsmgunerSpec && dsmgunerSpec->canBeParadropped &&
                  !tiger->canBeParadropped && !kv2->canBeParadropped,
              "Retail UnitCatalog canbepara identifies airborne infantry, light guns, mortars, crates & jeeps");

        // Phase 5O: retail AVIA shot_grn_id ground-attack HitType, warship/train turret_type
        // AA/artillery turrets, *guncrewunit bailout crews & moralenoattack fearless units.
        const auto* shipAaSpec = cat4g ? cat4g->find("ship_e_turret2") : nullptr;
        const auto* trainLightAaSpec = cat4g ? cat4g->find("steyerzug1t") : nullptr;
        const auto* trainHeavyAaSpec = cat4g ? cat4g->find("87t2") : nullptr;
        const auto* shipAtAaSpec = cat4g ? cat4g->find("ship_l_turret2") : nullptr;
        const auto* shipArtSpec = cat4g ? cat4g->find("ship_e_turret") : nullptr;
        const auto* us105Spec = cat4g ? cat4g->find("m1a1") : nullptr;
        const auto* jp150Spec = cat4g ? cat4g->find("150mm.japanese") : nullptr;
        check(planeBf109->groundShotId == "man_machine" &&
                  planeBf109->groundHitType == ss2::assets::HitType::Machine &&
                  planeBf109->effectiveGroundHitType() == ss2::assets::HitType::Machine &&
                  planeIl2->groundShotId == "rs" &&
                  planeIl2->groundHitType == ss2::assets::HitType::Explosive &&
                  planeIl2->effectiveGroundHitType() == ss2::assets::HitType::Explosive,
              "Retail AVIA shot_grn_id resolves Machine fighter MG vs Explosive il-2 rockets");
        check(shipAaSpec && shipAaSpec->turretType == "zenit" && shipAaSpec->isAntiAir() &&
                  shipAaSpec->isHeavyAntiAir() &&
                  trainLightAaSpec && trainLightAaSpec->turretType == "zenit" &&
                  trainLightAaSpec->isAntiAir() && !trainLightAaSpec->isHeavyAntiAir() &&
                  trainHeavyAaSpec && trainHeavyAaSpec->turretType == "zenit" &&
                  trainHeavyAaSpec->isAntiAir() && trainHeavyAaSpec->isHeavyAntiAir() &&
                  shipAtAaSpec && shipAtAaSpec->turretType == "atg_zenit" &&
                  shipAtAaSpec->isAntiAir() && shipAtAaSpec->isHeavyAntiAir() &&
                  shipArtSpec && shipArtSpec->turretType == "gaub" &&
                  shipArtSpec->isArtillery() && !shipArtSpec->isAntiAir(),
              "Retail turret_type classifies warship and armored-train AA & artillery turrets");
        check(pak40->crewUnit == "dguner" && flak88Spec->crewUnit == "dguner" &&
                  para53k->crewUnit == "rguner" && gun25pSpec && gun25pSpec->crewUnit == "eguner" &&
                  us105Spec && us105Spec->crewUnit == "aguner" &&
                  jp150Spec && jp150Spec->crewUnit == "jguner",
              "Retail *guncrewunit parses national artillery crew replacements");
        check(doff->moraleNoAttack == -32767 && doff->isFearless() &&
                  commandoSpec && commandoSpec->moraleNoAttack == -32767 && commandoSpec->isFearless() &&
                  !dsmgunerSpec->isFearless(),
              "Retail moralenoattack identifies fearless officers and commandos");

        // Phase 5P: retail MISC\INTERFACE UnderAttackInterval alert cadence.
        check(cat4g->underAttackIntervalSec() == 15,
              "Retail MISC\\INTERFACE parses UnderAttackInterval=15");

        // Phase 5Q: retail I-SUPPORT crew-first order, landing-craft stock=20 capacity,
        // mission-authored expa decoding, and movespeed/crouchmovespeed veterancy ranges.
        const auto* dgunerSpec5q = cat4g ? cat4g->find("dguner") : nullptr;
        const auto* jpLandingSpec = cat4g ? cat4g->find("japan.landing") : nullptr;
        check(commandoSpec && std::abs(commandoSpec->moveSpeed - 0.62) < 0.02 &&
                  std::abs(commandoSpec->moveSpeedMax - 0.68) < 0.02 &&
                  std::abs(commandoSpec->crouchMoveSpeed - 0.41) < 0.02 &&
                  std::abs(commandoSpec->crouchMoveSpeedMax - 0.45) < 0.02,
              "Retail commando parses movespeed 0.62..0.68 & crouchmovespeed 0.41..0.45");
        check(dhmg && dgunerSpec5q && dhmg->moveSpeed < dgunerSpec5q->moveSpeed &&
                  dhmg->crouchMoveSpeed < dgunerSpec5q->crouchMoveSpeed,
              "Retail dhmg (0.40/0.26) moves slower than dguner (0.50/0.33)");
        check(tiger && tiger->moveSpeed > 0.5 && tiger->moveSpeedMax > tiger->moveSpeed,
              "Retail tiger parses veterancy moveSpeedMax > moveSpeed");
        check(jpLandingSpec && jpLandingSpec->stock == 20,
              "Retail japan.landing parses stock=20 landing craft capacity");

        // Verify retail 031000.ssc, 000000.ssc, and 002000.ssc I-SUPPORT crew-first order & I-UNITS expa.
        if (const auto arnhem = ss2::assets::findCampaignMission(gameDir, "british:5")) {
            const auto arnData = ss2::assets::readMissionData(arnhem->path);
            bool sawCrewFirstTank = false;
            for (const auto& ru : arnData.reinforcements[0]) {
                if ((ru.id == "pzkpfw.lv.j" || ru.id == "tiger" || ru.id == "maultier") &&
                    ru.passengers.size() >= 2 &&
                    ru.passengers.front().isCrew && !ru.passengers.back().isCrew) {
                    if (ru.id == "pzkpfw.lv.j" && ru.passengers.front().id == "etankist") {
                        sawCrewFirstTank = true;
                        break;
                    }
                }
            }
            check(sawCrewFirstTank, "031000.ssc I-SUPPORT pzkpfw.lv.j puts etankist crew first (isCrew=true) & riders second");
        }
        if (const auto rus1 = ss2::assets::findCampaignMission(gameDir, "000000")) {
            const auto rusData = ss2::assets::readMissionData(rus1->path);
            bool sawJeepCrewFirst = false;
            for (const auto& ru : rusData.reinforcements[1]) {
                if (ru.id == "gaz.67.jeep" && ru.passengers.size() == 4 &&
                    ru.passengers[0].isCrew && ru.passengers[0].id == "rtankist" &&
                    !ru.passengers[1].isCrew && !ru.passengers[2].isCrew && !ru.passengers[3].isCrew) {
                    sawJeepCrewFirst = true;
                    break;
                }
            }
            check(sawJeepCrewFirst, "000000.ssc I-SUPPORT gaz.67.jeep puts rtankist crew first & 3 sniper/officer riders second");
            const auto rusPlaced = ss2::assets::readMissionPlacements(rus1->path);
            const bool sawNonzeroExpa = std::ranges::any_of(rusPlaced, [](const auto& p) {
                return (p.id == "8cm.mortar" || p.id == "dhmg") && p.expa == 100;
            });
            check(sawNonzeroExpa, "000000.ssc I-UNITS decodes mission-authored expa=100 on veteran initial units");
        }
        if (const auto jap1 = ss2::assets::findCampaignMission(gameDir, "002000")) {
            const auto japData = ss2::assets::readMissionData(jap1->path);
            bool sawLandingCrewFirst = false;
            for (const auto& ru : japData.reinforcements[2]) {
                if (ru.id == "japan.landing" && ru.passengers.size() == 22 &&
                    ru.passengers[0].isCrew && ru.passengers[1].isCrew &&
                    ru.passengers[0].id == "jtankist" && !ru.passengers[2].isCrew &&
                    !ru.passengers.back().isCrew) {
                    sawLandingCrewFirst = true;
                    break;
                }
            }
            check(sawLandingCrewFirst, "002000.ssc I-SUPPORT japan.landing puts 2 jtankist crew first + 20 riders second");
        }

        // Phase 5R: retail MISC\WEATHER, canattackpoint, bonus_shotdeadzone, scandelay,
        // expa_crush, and I-WOOFERS ambient sound emitters + SOUNDS.HDR verification.
        const auto* ba64Spec5r = cat4g ? cat4g->find("ba64") : nullptr;
        const auto* pak35Spec5r = cat4g ? cat4g->find("pak35-36") : nullptr;
        const auto* trainSpec5r = cat4g ? cat4g->find("steyerzug1") : nullptr;
        const auto& ws5r = cat4g->weather();
        check(ws5r.loaded && ws5r.rainVisibilityPct == 60 &&
                  ws5r.fogVisibilityPct == 70 && ws5r.snowVisibilityPct == 60,
              "Retail MISC\\WEATHER parses rain=60%, fog=70%, snow=60% visibility");
        check(dsmgunerSpec && dsmgunerSpec->canAttackPoint == 2 &&
                  dgunerSpec5q && dgunerSpec5q->canAttackPoint == 1,
              "Retail canattackpoint distinguishes SMG (2) from rifleman (1)");
        check(willyJeep && willyJeep->bonusShotDeadZone == 5 &&
                  horchSpec && horchSpec->bonusShotDeadZone == 5 &&
                  tiger && tiger->bonusShotDeadZone == 0,
              "Retail bonus_shotdeadzone parses open-top jeep deadzone (5) vs tank (0)");
        check(ba64Spec5r && pak35Spec5r && longRange150 &&
                  ba64Spec5r->scanDelay == 30 && pak35Spec5r->scanDelay == 50 &&
                  longRange150->scanDelay == 150 && tiger->scanDelay == 160,
              "Retail scandelay scales from fast scout (30) to heavy artillery/tiger (150..160)");
        check(trainSpec5r && trainSpec5r->expaCrush == 100,
              "Retail expa_crush parses armored-train crush XP bonus (100)");

        const auto bank5r = ss2::assets::SoundBank::load(gameDir);
        for (const char* ambGrp : {"@fire_loop", "@cicades_loop", "@birds"}) {
            check(bank5r.variantCount(ambGrp) > 0,
                  std::string("SOUNDS.HDR contains @ ambient group ") + ambGrp);
        }
        if (const auto m15 = ss2::assets::findCampaignMission(gameDir, "015000")) {
            const auto d15 = ss2::assets::readMissionData(m15->path);
            check(d15.woofers.size() == 4u, "015000.ssc decodes 4 I-WOOFERS spatial emitters");
            bool sawBirds = false;
            for (const auto& w : d15.woofers) {
                const std::string resolved = ss2::assets::resolveAmbientWooferSound(w.sound);
                check(bank5r.variantCount(resolved) > 0,
                      "015000.ssc woofer sound exists in SOUNDS.HDR: " + resolved);
                if (w.sound == "@misc_birds" && w.minPeriodMs == 1000u &&
                    w.maxPeriodMs == 6000u && resolved == "@birds") {
                    sawBirds = true;
                }
            }
            check(sawBirds, "015000.ssc @misc_birds maps period [1000,6000]ms -> @birds");
        }
        if (const auto m16 = ss2::assets::findCampaignMission(gameDir, "016000")) {
            const auto d16 = ss2::assets::readMissionData(m16->path);
            check(d16.woofers.size() == 12u, "016000.ssc decodes 12 I-WOOFERS spatial emitters");
            bool sawSea = false, sawGrass = false;
            for (const auto& w : d16.woofers) {
                const std::string resolved = ss2::assets::resolveAmbientWooferSound(w.sound);
                check(bank5r.variantCount(resolved) > 0,
                      "016000.ssc woofer sound exists in SOUNDS.HDR: " + resolved);
                if (resolved == "@sea") sawSea = true;
                if (resolved == "@wind_grass_loop") sawGrass = true;
            }
            check(sawSea && sawGrass, "016000.ssc resolves @sea and @wind_grass -> @wind_grass_loop");
        }

        // Phase 5S: retail marchenabled, expa_reload/ammoregendelay/healdelay pairs, mineammo/layminetime/getminetime,
        // moralerndmove/moralerage, and death/low-HP SOUNDS.HDR groups.
        const auto* sdkfz11Spec5s = cat4g ? cat4g->find("sdkfz.11") : nullptr;
        const auto* zisMedSpec5s = cat4g ? cat4g->find("zis-5.med") : nullptr;
        const auto* eminerSpec5s = cat4g ? cat4g->find("eminer") : nullptr;
        const auto* gatsSpec5s = cat4g ? cat4g->find("gats") : nullptr;
        const auto* cmedicSpec5s = cat4g ? cat4g->find("cmedic") : nullptr;
        check(tiger && tiger->marchEnabled && tiger->marchSightBonus == 4 &&
                  tiger->moraleRndMove == -21504 && tiger->moraleRage == 21845,
              "Retail tiger parses marchenabled=1, marchsightbonus=4, moralerndmove=-21504, moralerage=21845");
        check(sdkfz11Spec5s && sdkfz11Spec5s->ammoRegenDelay == 8 && sdkfz11Spec5s->ammoRegenDelayMax == 6 &&
                  sdkfz11Spec5s->expaReload == 3 && sdkfz11Spec5s->expaReloadMax == 1 &&
                  sdkfz11Spec5s->moraleRndMove == -18432 && sdkfz11Spec5s->moraleRage == 21845,
              "Retail sdkfz.11 parses ammoregendelay(8,6), expa_reload(3,1), moralerndmove(-18432) & moralerage");
        check(zisMedSpec5s && zisMedSpec5s->healDelay == 10 && zisMedSpec5s->healDelayMax == 8,
              "Retail zis-5.med parses healdelay(10,8)");
        check(eminerSpec5s && eminerSpec5s->mineAmmo == 1 && eminerSpec5s->layMineTime == 40 &&
                  eminerSpec5s->layMineTimeMax == 40 && eminerSpec5s->getMineTime == 80,
              "Retail eminer parses mineammo=1, layminetime=40, getminetime=80");
        check(gatsSpec5s && gatsSpec5s->mineAmmo == 1 && gatsSpec5s->layMineTime == 100 &&
                  gatsSpec5s->layMineTimeMax == 85 && gatsSpec5s->getMineTime == 80 &&
                  gatsSpec5s->getMineTimeMax == 68,
              "Retail gats parses mineammo=1, layminetime(100,85), getminetime(80,68)");
        check(cmedicSpec5s && cmedicSpec5s->getMineTime == 50 && cmedicSpec5s->getMineTimeMax == 40,
              "Retail cmedic parses getminetime(50,40)");
        for (const char* deathOrCloseGrp : {"screamofpain", "d_close", "a_sold_close", "j_sold_close", "i_sold_close"}) {
            check(bank5r.variantCount(deathOrCloseGrp) > 0,
                  std::string("SOUNDS.HDR contains infantry death/close group ") + deathOrCloseGrp);
        }

        // Phase 5T: retail MISC\WEATHER tints/showalways, dynamic bonus_sight, and I-MISXINF per-mission weather.
        check(ws5r.showAlways &&
                  ws5r.fogTint.r == 128 && ws5r.fogTint.g == 128 && ws5r.fogTint.b == 128 &&
                  ws5r.fogTint.alpha > 0.49f && ws5r.fogTint.alpha < 0.51f &&
                  ws5r.rainTint.r == 32 && ws5r.rainTint.alpha > 0.49f && ws5r.rainTint.alpha < 0.51f &&
                  ws5r.snowTint.r == 32 && ws5r.snowTint.alpha > 0.29f && ws5r.snowTint.alpha < 0.31f,
              "Retail MISC\\WEATHER parses showalways=1 and fog/rain/snow RGBC tints");
        check(tiger && tiger->sight == 7 && tiger->bonusSight == 3 &&
                  longRange150 && longRange150->sight == 9 && longRange150->bonusSight == -4,
              "Retail bonus_sight keeps signed modifier separate from base sight");
        const std::array<std::pair<const char*, ss2::assets::WeatherKind>, 4> retailSingleWeather{{
            {"v2", ss2::assets::WeatherKind::Clear},
            {"'dieppe [total victory]", ss2::assets::WeatherKind::Rain},
            {"'leningrad [total victory]", ss2::assets::WeatherKind::Snow},
            {"'stavelot [total victory]", ss2::assets::WeatherKind::Snow},
        }};
        unsigned resolvedSingleWeather = 0u;
        for (const auto& [query, expected] : retailSingleWeather) {
            const auto m = ss2::assets::findSinglePlayerMission(gameDir, query);
            check(m.has_value(), std::string("Retail single mission missing for weather check: ") + query);
            if (m) {
                ++resolvedSingleWeather;
                const auto md = ss2::assets::readMissionData(m->path);
                check(md.weather == expected, std::string("Retail single mission I-MISXINF weather for ") + query);
            }
        }
        check(resolvedSingleWeather == retailSingleWeather.size(), "All 4 retail single-player weather missions resolved");
        const std::array<std::pair<const char*, ss2::assets::WeatherKind>, 4> retailCampWeather{{
            {"001000", ss2::assets::WeatherKind::Rain},
            {"011001", ss2::assets::WeatherKind::Rain},
            {"023000", ss2::assets::WeatherKind::Snow},
            {"023001", ss2::assets::WeatherKind::Clear},
        }};
        unsigned resolvedCampWeather = 0u;
        for (const auto& [code, expected] : retailCampWeather) {
            const auto m = ss2::assets::findCampaignMission(gameDir, code);
            check(m.has_value(), std::string("Retail campaign mission missing for weather check: ") + code);
            if (m) {
                ++resolvedCampWeather;
                const auto md = ss2::assets::readMissionData(m->path);
                check(md.weather == expected, std::string("Retail campaign mission I-MISXINF weather for ") + code);
            }
        }
        check(resolvedCampWeather == retailCampWeather.size(), "All 4 retail campaign weather missions resolved");
    }
}

void testUnitCatalogPhase4A() {
    const std::string artDesc =
        "*name 75mm PaK-40\n"
        "*nationtype german at\n"
        "mech 1\n"
        "health 120\n"
        "ammo1 24\n"
        "seltype atg\n"
        "idlesnd_move rollgun 0.25 perm 0 0\n"
        "movedamagehp 60\n"
        "shot_sound gunshot 0.3 perm 0 1\n"
        "shot_damage 422 472\n"
        "shot_range 725 797\n"
        "shot_reloadtime 150 127\n"
        "canbetowed handy\n";
    const auto spec = ss2::assets::UnitCatalog::parseUnitText("pak40", artDesc);
    check(spec.nation == "german" && spec.roleClass == "at" && spec.isArtillery(),
          "synthetic artillery spec parsed nation/role/isArtillery");
    check(spec.moveSound == "rollgun" && spec.moveDamageHp == 60,
          "synthetic spec parsed moveSound and moveDamageHp");
    check(spec.shot1.damage == 422 && spec.shot1.range == 725 && spec.shot1.soundGroup == "gunshot",
          "synthetic bare shot_* fields parsed into shot1");
    check(spec.isTowable() && spec.towWeightClass == "handy",
          "synthetic canbetowed handy parsed");

    const std::string towDesc =
        "*nationtype russian car\n"
        "mech 1\n"
        "seltype transport\n"
        "canmovebackward 1\n"
        "backmovespeed 0.45\n"
        "havepricep heavy handy light weightless\n"
        "pricepspeed handy 0.59\n";
    const auto tow = ss2::assets::UnitCatalog::parseUnitText("zis-5", towDesc);
    check(tow.canTowAny() && tow.towSpeedFactor("handy") > 0.58 && tow.towSpeedFactor("heavy") > 0.4,
          "synthetic havepricep & pricepspeed parsed");
    check(tow.canMoveBackward && tow.backMoveSpeed > 0.44 && tow.backMoveSpeed < 0.46,
          "synthetic canmovebackward & backmovespeed parsed");

    const std::string supDesc =
        "*nationtype german furg\n"
        "mech 1\n"
        "seltype sappers\n"
        "ammoregendelay 8 6\n";
    const auto sup = ss2::assets::UnitCatalog::parseUnitText("sdkfz.11", supDesc);
    check(sup.isSupplyTruck() && !sup.isAmbulance(), "synthetic supply truck flags match");

    const auto flamerWreck = ss2::assets::UnitCatalog::parseUnitText("flame_tank_5p", "dieexplosion dead_flamer\n");
    const auto demoWreck = ss2::assets::UnitCatalog::parseUnitText("demo_veh_5p", "dieexplosion selfdestruct\n");
    check(flamerWreck.cookOffDamage() == 180 && demoWreck.cookOffDamage() == 280,
          "synthetic dead_flamer & selfdestruct cookOffDamage tiers match");

    const std::string ambDesc =
        "*nationtype american furg\n"
        "mech 1\n"
        "seltype transport\n"
        "healdelay 10 8\n";
    const auto amb = ss2::assets::UnitCatalog::parseUnitText("gmc.353.med", ambDesc);
    check(amb.isAmbulance(), "synthetic ambulance healdelay flag match");
}

void testBriefingMedia() {
    {
        const auto paras = ss2::assets::splitBriefingParagraphs("  Overview line.\r\n~1. First step\r\n~2. Second step ");
        check(paras.size() == 3u && paras[0] == "Overview line." &&
                  paras[1] == "1. First step" && paras[2] == "2. Second step",
              "splitBriefingParagraphs splits and trims on '~'");
        const auto plain = ss2::assets::splitBriefingParagraphs("  Single block\n ");
        check(plain.size() == 1u && plain[0] == "Single block", "splitBriefingParagraphs single block");
        check(ss2::assets::splitBriefingParagraphs("  \r\n\t ").empty(), "splitBriefingParagraphs blank is empty");
    }

    {
        // Synthetic "2D" Dialogic OKI ADPCM stream (2 bytes -> 4 samples)
        const std::array<std::uint8_t, 8> apm{{'2', 'D', 2u, 0u, 0u, 0u, 0x7Fu, 0x80u}};
        const auto pcm = ss2::assets::decodeApmAudio(apm);
        check(pcm.size() == 4u && pcm[0] == 28 && pcm[1] == -31,
              "decodeApmAudio decodes synthetic OKI ADPCM nibbles cleanly");
        const std::array<std::uint8_t, 6> badApm{{'R', 'I', 0u, 0u, 0u, 0u}};
        check(ss2::assets::decodeApmAudio(badApm).empty(), "decodeApmAudio rejects bad magic");
    }

    {
        // Synthetic 0xA1 compact RLE sprite (left=2, top=1, w=4, h=2) layered red (alpha 255) + blue (alpha 128)
        std::vector<std::uint8_t> dat{
            2u, 0u, 1u, 0u, 4u, 0u, 2u, 0u, 0xA1u,
            5u, 0u, 0x82u, 255u, 0x02u, 128u, 0u,
            0u, 0u,
        };
        const std::string offStr = "0 0 255 255\r\n1 0 16711680 128\r\n";
        std::vector<ss2::assets::BriefingLayer> layers;
        std::vector<ss2::assets::BriefingFrame> frames;
        check(ss2::assets::parseBriefingOff(
                  std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(offStr.data()), offStr.size()),
                  dat.size(), layers, frames),
              "parseBriefingOff parses synthetic 2-layer frame");
        check(layers.size() == 2u && frames.size() == 1u && frames[0].layerCount == 2u,
              "synthetic .off grouped into 1 frame with 2 layers");

        ss2::assets::BriefingPart part;
        part.dat = std::move(dat);
        part.layers = std::move(layers);
        part.frames = std::move(frames);

        std::vector<std::uint32_t> canvas(16u * 8u, 0xFFFFFFFFu);
        ss2::assets::renderBriefingFrameArgb32(part, 0u, canvas.data(), 16, 8, 16);
        check(canvas[0] == 0u, "renderBriefingFrameArgb32 clears transparent pixels");
        // (x=2, y=1): layer 0 writes opaque red (255,0,0), layer 1 (alpha=128) composites blue over it
        const std::uint32_t px = canvas[1u * 16u + 2u];
        const std::uint32_t a = px >> 24;
        const std::uint32_t r = (px >> 16) & 0xFFu;
        const std::uint32_t b = px & 0xFFu;
        check(a == 255u && r == 127u && b == 128u,
              "renderBriefingFrameArgb32 premultiplied SRC_OVER blend matches");
        check(canvas[1u * 16u + 5u] == 0u, "transparent index 0 leaves pixel transparent");
    }

    if (const char* env = std::getenv("SS2_PRIVATE_GAME"); env && *env) {
        const std::filesystem::path gameDir(env);
        if (std::filesystem::is_directory(gameDir)) {
            const auto camp001 = ss2::assets::findCampaignMission(gameDir, "001000");
            check(camp001.has_value(), "001000.ssc found for briefing_media check");
            const auto bm001 = ss2::assets::readMissionBriefingMedia(camp001->path);
            check(bm001.parts.size() == 3u && bm001.paragraphs.size() == 4u &&
                      !bm001.overviewVoicePcm.empty() &&
                      !bm001.parts[0].frames.empty() && !bm001.parts[0].voicePcm.empty(),
                  "retail 001000.ssc loads 3 animated parts, 4 ~ paragraphs, and 4 ADPCM voiceovers");
            std::vector<std::uint32_t> frameBuf(
                std::size_t(ss2::assets::kBriefingOverlayWidth) * ss2::assets::kBriefingOverlayHeight, 0u);
            ss2::assets::renderBriefingFrameArgb32(
                bm001.parts[0], 0u, frameBuf.data(),
                ss2::assets::kBriefingOverlayWidth, ss2::assets::kBriefingOverlayHeight,
                ss2::assets::kBriefingOverlayWidth);
            check(std::ranges::any_of(frameBuf, [](std::uint32_t c) { return c != 0u; }),
                  "retail 001000.ssc renders non-empty briefing overlay pixels");

            const auto camp002 = ss2::assets::findCampaignMission(gameDir, "002000");
            check(camp002.has_value(), "002000.ssc found for 4-part briefing_media check");
            const auto bm002 = ss2::assets::readMissionBriefingMedia(camp002->path);
            check(bm002.parts.size() == 4u && bm002.paragraphs.size() == 5u && bm002.hasVoice(),
                  "retail 002000.ssc loads 4 parts and 5 paragraphs");

            const auto tut = ss2::assets::findSinglePlayerMission(gameDir, "tutorial i");
            check(tut.has_value(), "Tutorial I found for non-animated briefing_media check");
            const auto bmTut = ss2::assets::readMissionBriefingMedia(tut->path);
            check(bmTut.empty() && !bmTut.hasAnimation() && !bmTut.hasVoice(),
                  "Tutorial I reports empty animated/voice briefing media");

            std::size_t animatedMissions = 0;
            std::size_t voicedMissions = 0;
            std::size_t totalFrames = 0;
            for (const auto& c : ss2::assets::listCampaigns(gameDir)) {
                for (const auto& m : c.missions) {
                    const auto bm = ss2::assets::readMissionBriefingMedia(m.path);
                    if (bm.hasAnimation()) ++animatedMissions;
                    if (bm.hasVoice()) ++voicedMissions;
                    for (const auto& pt : bm.parts) {
                        totalFrames += pt.frames.size();
                        ss2::assets::renderBriefingFrameArgb32(
                            pt, 0u, frameBuf.data(),
                            ss2::assets::kBriefingOverlayWidth, ss2::assets::kBriefingOverlayHeight,
                            ss2::assets::kBriefingOverlayWidth);
                    }
                }
            }
            for (const auto& m : ss2::assets::listSinglePlayerMissions(gameDir)) {
                const auto bm = ss2::assets::readMissionBriefingMedia(m.path);
                if (bm.hasAnimation()) ++animatedMissions;
                if (bm.hasVoice()) ++voicedMissions;
                for (const auto& pt : bm.parts) {
                    totalFrames += pt.frames.size();
                    ss2::assets::renderBriefingFrameArgb32(
                        pt, 0u, frameBuf.data(),
                        ss2::assets::kBriefingOverlayWidth, ss2::assets::kBriefingOverlayHeight,
                        ss2::assets::kBriefingOverlayWidth);
                }
            }
            check(animatedMissions >= 50u && voicedMissions >= 30u && totalFrames >= 3000u,
                  "retail SP/campaign sweep decoded all animated briefing streams");
        }
    }
}

void testVehiclesAndShadows() {
    const char* env = std::getenv("SS2_PRIVATE_GAME");
    if (!env || !*env) return;
    const std::filesystem::path gameDir(env);
    if (!std::filesystem::is_directory(gameDir)) return;

    const auto makeFlat = [](unsigned w, unsigned h) {
        ss2::assets::MapImage img{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4u, 255u)};
        for (std::size_t i = 0; i < img.rgba.size(); i += 4u) {
            img.rgba[i + 0u] = 140u;
            img.rgba[i + 1u] = 130u;
            img.rgba[i + 2u] = 80u;
            img.rgba[i + 3u] = 255u;
        }
        return img;
    };
    // (140>>1, (130>>1)+16, (80>>1)+41) == (70, 81, 81)
    const auto countBodyAndShadow = [](const ss2::assets::MapImage& img) {
        std::size_t body = 0, shadow = 0;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4u) {
            const auto r = img.rgba[i + 0u], g = img.rgba[i + 1u], b = img.rgba[i + 2u];
            if (r == 140u && g == 130u && b == 80u) continue;
            if (r == 70u && g == 81u && b == 81u) ++shadow;
            else ++body;
        }
        return std::pair{body, shadow};
    };

    // 1. FrameCount == 7 trains & warships + fc == 128 turret railcar (intact, wreck, portrait, shadow).
    {
        const std::vector<std::string> ids{"locomotive", "tm3.12", "m1.t-34", "ship_e", "ship_j", "tm1.180t"};
        for (std::size_t idx = 0; idx < ids.size(); ++idx) {
            const std::string& id = ids[idx];
            const ss2::assets::Placement pl{id, 512, 384, 4, 100, 50, 1, 0};
            ss2::assets::VehicleScene scene(gameDir, std::span<const ss2::assets::Placement>(&pl, 1u));
            check(scene.vehicles().size() == 1u, "7-frame/128-frame unit spawned: " + id);
            const auto& u = scene.vehicles().front();
            if (id == "ship_e" || id == "ship_j") {
                check(u.width > 400 && u.height > 200, "Warship bounds non-empty: " + id);
            } else {
                check(u.width > 50 && u.height > 30, "Train/railcar bounds non-empty: " + id);
            }

            auto intactImg = makeFlat(1024u, 768u);
            scene.drawAll(intactImg);
            const auto [bodyPx, shadowPx] = countBodyAndShadow(intactImg);
            check(bodyPx > 100u && shadowPx > 100u,
                  "Intact 7-frame/128-frame unit draws both body and 0xa3 shadow: " + id);

            if (id == "locomotive" || id == "ship_e" || id == "tm1.180t") {
                (void)scene.update(0, 512, 384, 4u, 4u, 0);
                auto wreckImg = makeFlat(1024u, 768u);
                scene.drawAll(wreckImg);
                const auto [wBody, wShadow] = countBodyAndShadow(wreckImg);
                check(wBody > 100u && wShadow > 100u && wreckImg.rgba != intactImg.rgba,
                      "Wreck state renders altered wreck body and 0xa3 shadow: " + id);
            }

            if (id == "locomotive" || id == "ship_e") {
                const auto portrait = scene.renderUnitPortrait(id, 48u);
                check(portrait.has_value() && portrait->width == 48u && portrait->height == 48u,
                      "7-frame portrait builds 48x48 image: " + id);
                std::uint8_t minR = 255u, maxR = 0u;
                for (std::size_t p = 0; p < portrait->rgba.size(); p += 4u) {
                    minR = std::min(minR, portrait->rgba[p]);
                    maxR = std::max(maxR, portrait->rgba[p]);
                }
                check(maxR - minR > 12, "7-frame portrait has non-uniform sprite detail: " + id);
            }
        }
    }

    // 2. All major vehicle frameCount shapes (66, 160, 192, 256, 288, 352, 448, 512) cast 0xa3 shadows,
    //    respect SceneryDepth::hasShadow deduplication, and move cleanly via DirtyRect.
    {
        for (const char* vid : {"ds39", "kurogan", "zundapp.ks750", "2598.so-da", "panther", "zis-3", "type.88", "us.landing"}) {
            const ss2::assets::Placement pl{vid, 128, 128, 6, 100, 50, 1, 0};
            ss2::assets::VehicleScene vs(gameDir, std::span<const ss2::assets::Placement>(&pl, 1u));
            auto img = makeFlat(256u, 256u);
            vs.drawAll(img);
            const auto [bPx, sPx] = countBodyAndShadow(img);
            check(bPx > 10u && sPx > 10u, std::string("Vehicle casts 0xa3 ground shadow: ") + vid);
        }

        const ss2::assets::Placement tankPl{"panther", 128, 128, 6, 100, 50, 1, 0};
        ss2::assets::VehicleScene tankScene(gameDir, std::span<const ss2::assets::Placement>(&tankPl, 1u));
        ss2::assets::SceneryDepth depth(256, 256);
        depth.addShadow({100, 100, 56, 56, std::vector<std::uint8_t>(56u * 56u, 1u)});
        auto maskedImg = makeFlat(256u, 256u);
        tankScene.drawAll(maskedImg, &depth);
        for (int y = 100; y < 156; ++y) {
            for (int x = 100; x < 156; ++x) {
                const std::size_t p = (std::size_t(y) * 256u + std::size_t(x)) * 4u;
                check(!(maskedImg.rgba[p] == 70u && maskedImg.rgba[p + 1u] == 81u && maskedImg.rgba[p + 2u] == 81u),
                      "Vehicle shadow skips SceneryDepth::hasShadow pixels");
            }
        }

        auto patched = makeFlat(256u, 256u);
        tankScene.drawAll(patched);
        const auto dirty = tankScene.update(0u, 188, 158, 8u, 10u);
        check(dirty.valid(), "Moving panther returns valid DirtyRect including shadow");
        const int x0 = std::clamp(dirty.left, 0, 256);
        const int y0 = std::clamp(dirty.top, 0, 256);
        const int x1 = std::clamp(dirty.right, x0, 256);
        const int y1 = std::clamp(dirty.bottom, y0, 256);
        auto patch = makeFlat(static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0));
        tankScene.drawRegion(patch, x0, y0);
        for (int row = y0; row < y1; ++row) {
            std::copy_n(patch.rgba.data() + std::size_t(row - y0) * patch.width * 4u,
                        std::size_t(patch.width) * 4u,
                        patched.rgba.data() + (std::size_t(row) * 256u + std::size_t(x0)) * 4u);
        }
        auto fullRedraw = makeFlat(256u, 256u);
        tankScene.drawAll(fullRedraw);
        check(patched.rgba == fullRedraw.rgba,
              "DirtyRect region redraw clears old shadow and matches full drawAll");

        // Retail track/wheel layers across fc=288 & fc=192 + walking-track animation on motion.
        auto trackLive0 = makeFlat(256u, 256u);
        ss2::assets::VehicleScene phaseScene(gameDir, std::span<const ss2::assets::Placement>(&tankPl, 1u));
        phaseScene.drawAll(trackLive0);
        const auto [pBody0, pSh0] = countBodyAndShadow(trackLive0);
        check(pBody0 > 1400u && pSh0 > 200u,
              "Panther intact render draws hull, tracks (block 32), and turret");

        (void)phaseScene.update(0u, 134, 128, 6u, 6u);
        auto trackLive1 = makeFlat(256u, 256u);
        phaseScene.drawRegion(trackLive1, 6, 0);
        check(trackLive1.rgba != trackLive0.rgba,
              "Moving panther toggles retail track animation phase (block 64 vs 32)");

        const ss2::assets::Placement ferdPl{"ferdinand", 128, 128, 12, 100, 50, 1, 0};
        ss2::assets::VehicleScene ferdScene(gameDir, std::span<const ss2::assets::Placement>(&ferdPl, 1u));
        auto ferdImg = makeFlat(256u, 256u);
        ferdScene.drawAll(ferdImg);
        const auto [fBody, fSh] = countBodyAndShadow(ferdImg);
        check(fBody > 1600u && fSh > 200u,
              "fc=192 SPG (ferdinand) draws both upper hull (block 0) and tracks (block 32/64)");

        // Dedicated retail charred wrecks & shadows across all remaining vehicle frameCount shapes.
        for (const char* wid : {"ds39", "horch", "ferdinand", "bmw.r-75", "gmc.cckw.353", "hummel", "panther", "17p", "88mmflak", "us.landing"}) {
            const ss2::assets::Placement wpl{wid, 160, 160, 6, 100, 50, 1, 0};
            ss2::assets::VehicleScene wScene(gameDir, std::span<const ss2::assets::Placement>(&wpl, 1u));
            auto intactW = makeFlat(320u, 320u);
            wScene.drawAll(intactW);
            (void)wScene.update(0u, 160, 160, 6u, 6u, 0);
            auto wreckW = makeFlat(320u, 320u);
            wScene.drawAll(wreckW);
            const auto [wbPx, wsPx] = countBodyAndShadow(wreckW);
            std::uint8_t maxChan = 0u;
            for (std::size_t p = 0; p < wreckW.rgba.size(); p += 4u) {
                const auto r = wreckW.rgba[p], g = wreckW.rgba[p + 1u], b = wreckW.rgba[p + 2u];
                if ((r == 140u && g == 130u && b == 110u) || (r == 70u && g == 81u && b == 81u)) continue;
                maxChan = std::max({maxChan, r, g, b});
            }
            check(wbPx > 10u && wsPx > 10u && wreckW.rgba != intactW.rgba && maxChan > 70u,
                  std::string("Dedicated retail charred wreck body & shadow rendered with true palette: ") + wid);
        }
    }

    // 3. Retail Tutorial I scenery draws 0xa3 shadows onto ground pixels matching n2Cad800 formula.
    {
        const auto rawTut = ss2::assets::loadTutorialTerrain(gameDir, 0u);
        auto sceneTut = rawTut;
        ss2::assets::SceneryDepth tutDepth(static_cast<int>(rawTut.width), static_cast<int>(rawTut.height));
        std::vector<ss2::assets::BuildingPlacement> tutBlds;
        std::vector<ss2::assets::BridgePlacement> tutBridges;
        (void)ss2::assets::drawTutorialScenery(sceneTut, gameDir, &tutDepth, 0u, &tutBlds, &tutBridges);
        check(!tutBlds.empty() && !tutBridges.empty(), "Tutorial I scenery loads houses and bridges");
        std::size_t exactShadowPixels = 0;
        for (unsigned y = 0; y < rawTut.height; ++y) {
            for (unsigned x = 0; x < rawTut.width; ++x) {
                const int ix = static_cast<int>(x), iy = static_cast<int>(y);
                if (!tutDepth.hasShadow(ix, iy) || tutDepth.occludes(ix, iy, 0)) continue;
                const std::size_t i = (std::size_t(y) * rawTut.width + x) * 4u;
                if (rawTut.rgba[i + 3u] != 255u) continue;
                const std::uint8_t er = static_cast<std::uint8_t>(rawTut.rgba[i + 0u] >> 1);
                const std::uint8_t eg = static_cast<std::uint8_t>((rawTut.rgba[i + 1u] >> 1) + 16u);
                const std::uint8_t eb = static_cast<std::uint8_t>((rawTut.rgba[i + 2u] >> 1) + 41u);
                if (sceneTut.rgba[i + 0u] == er && sceneTut.rgba[i + 1u] == eg &&
                    sceneTut.rgba[i + 2u] == eb && sceneTut.rgba[i + 3u] == 255u) {
                    ++exactShadowPixels;
                }
            }
        }
        check(exactShadowPixels > 10000u,
              "Tutorial I scenery paints >10000 exact 0xa3 tree/stand ground shadow pixels");
        std::size_t tutProg = 0u;
        for (const auto& b : tutBlds) {
            if (b.basePatch.width == 0u || b.basePatch.height == 0u) continue;
            const auto s0 = ss2::assets::renderHouseStage(
                b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 0);
            const auto s1 = ss2::assets::renderHouseStage(
                b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 1);
            const auto s2 = ss2::assets::renderHouseStage(
                b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 2);
            const auto s3 = ss2::assets::renderHouseStage(
                b.x, b.y, b.ruinLeft, b.ruinTop, b.basePatch, b.palette, b.stageBlocks, 3);
            if (s0.image.rgba != s1.image.rgba && s1.image.rgba != s2.image.rgba &&
                s2.image.rgba != s3.image.rgba && s3.bodyCount == 0u) {
                ++tutProg;
            }
        }
        check(tutProg == tutBlds.size(), "Tutorial I houses support all 4 progressive dom.res damage stages");
    }

    // 4. Retail MISC\MISC SoldClr infantry uniform tinting across shared .RS2 sprites & portraits.
    {
        const ss2::assets::MapImage bg{96, 96, std::vector<std::uint8_t>(96u * 96u * 4u, 40u)};
        const auto renderOne = [&](const char* uid) {
            const ss2::assets::Placement pl{uid, 48, 48, 3, 100, 50, 1, 0};
            ss2::assets::InfantryScene sc(bg, gameDir, std::span<const ss2::assets::Placement>(&pl, 1u));
            check(sc.units().size() == 1u, std::string("Infantry spawned for SoldClr check: ") + uid);
            return std::pair{sc.compose(), sc.renderUnitPortrait(uid, 48u)};
        };

        const auto [dhmgImg, dhmgPort] = renderOne("dhmg");
        const auto [ahmgImg, ahmgPort] = renderOne("ahmg");
        check(dhmgImg.rgba != bg.rgba && ahmgImg.rgba != bg.rgba && dhmgImg.rgba != ahmgImg.rgba,
              "A-3.RS2 dhmg (German) vs ahmg (American) uniforms differ via SoldClr");

        const auto [aoffImg, aoffPort] = renderOne("aofficer");
        const auto [eoffImg, eoffPort] = renderOne("eofficer");
        check(aoffImg.rgba != eoffImg.rgba && aoffPort && eoffPort && aoffPort->rgba != eoffPort->rgba,
              "AOFICE.RS2 aofficer (American) vs eofficer (British) world & portrait uniforms differ");

        const auto [dgenImg, dgenPort] = renderOne("dgeneral");
        const auto [rgenImg, rgenPort] = renderOne("rgeneral");
        check(dgenImg.rgba != rgenImg.rgba && dgenPort && rgenPort && dgenPort->rgba != rgenPort->rgba,
              "GENERAL.RS2 dgeneral (German) vs rgeneral (Russian) uniforms differ");

        // RUSALTAN.RS2 shared across 4 nations: Russian, American, British, French (SoldClr 7).
        const auto [rtk, _1] = renderOne("rtankist");
        const auto [atk, _2] = renderOne("atankist");
        const auto [etk, _3] = renderOne("etankist");
        const auto [ftk, _4] = renderOne("ftankist");
        check(rtk.rgba != atk.rgba && rtk.rgba != etk.rgba && rtk.rgba != ftk.rgba &&
                  atk.rgba != etk.rgba && atk.rgba != ftk.rgba && etk.rgba != ftk.rgba,
              "RUSALTAN.RS2 renders 4 distinct national tank-crew uniforms");

        // DATG.RS2 shared across German, Japanese, and French (SoldClr 7); plus cmedic white smock (SoldClr 2).
        const auto [gatsImg, _5] = renderOne("gats");
        const auto [jatsImg, _6] = renderOne("jats");
        const auto [fatgImg, _7] = renderOne("fatg");
        const auto [medImg, medPort] = renderOne("cmedic");
        check(gatsImg.rgba != jatsImg.rgba && gatsImg.rgba != fatgImg.rgba && jatsImg.rgba != fatgImg.rgba,
              "DATG.RS2 renders 3 distinct German/Japanese/French AT-rifle uniforms");
        check(medImg.rgba != bg.rgba && medPort.has_value(),
              "ALRUSMED.RS2 cmedic renders SoldClr 2 medic uniform and portrait");
    }

    // Verify retail 0xCA000001..4 .ANI containers & 0xa9 alpha-blended tactical cursor sequences.
    {
        const auto gameCommon = ss2::assets::resolveCaseInsensitivePath(gameDir, "game_common.sue");
        for (const char* stem : {
                 "C1FIELD", "C2OWN", "C3FRIEND", "C4ENEMY", "C5SCROLL", "C6NO", "C7UNLOAD",
                 "C8MOVE", "C9ATTACK", "C10AGRND", "C11AMOVE", "C12KATFR", "C13HOWFR", "C14RELD",
                 "C15ROTAT", "C16PCKUG", "C16PCKUP", "C17GOLN", "C18FLY", "C19BOMB", "C20PARA",
                 "C21SPY", "C26CURE", "C27MINE", "C28BINOC", "C29FIXBR", "VZRV_TNK", "a_rain"}) {
            const auto ani = ss2::assets::readSueEntry(gameCommon, std::string(stem) + ".ANI");
            const auto col = ss2::assets::readSueEntry(gameCommon, std::string(stem) + ".COL");
            const auto seq = ss2::assets::decodeAniSequence(ani, col);
            check(!seq.frames.empty() && seq.width >= 10u && seq.height >= 10u &&
                      seq.hotX >= 0 && seq.hotX < int(seq.width) &&
                      seq.hotY >= 0 && seq.hotY < int(seq.height),
                  std::string("decodeAniSequence valid bounds & hotspot for ") + stem);
            bool anyVisible = false;
            for (const auto& fr : seq.frames) {
                for (std::size_t p = 3u; p < fr.rgba.size(); p += 4u) {
                    if (fr.rgba[p] > 0u) {
                        anyVisible = true;
                        break;
                    }
                }
                if (anyVisible) break;
            }
            check(anyVisible, std::string("decodeAniSequence decodes visible pixels for ") + stem);
        }

        // Retail menu_common.sue\MENU_CUR.ANI + MENU_CUR.COL (0xa1 + 1024-byte premul .COL) and
        // game_common.sue\AIRFIRE.ANI + AIRFIRE.COL must preserve both transparent and semi-transparent pixels.
        const auto menuCommon = ss2::assets::resolveCaseInsensitivePath(gameDir, "menu_common.sue");
        const auto mCurAni = ss2::assets::readSueEntry(menuCommon, "MENU_CUR.ANI");
        const auto mCurCol = ss2::assets::readSueEntry(menuCommon, "MENU_CUR.COL");
        const auto mCurSeq = ss2::assets::decodeAniSequence(mCurAni, mCurCol);
        check(mCurSeq.frames.size() == 21u && mCurSeq.width == 14u && mCurSeq.height == 19u &&
                  mCurSeq.hotX == 1 && mCurSeq.hotY == 3,
              "MENU_CUR.ANI decodes 21 14x19 frames with hotspot (1, 3)");
        bool curHasTrans = false, curHasSemi = false;
        for (std::size_t p = 3u; p < mCurSeq.frames[0].rgba.size(); p += 4u) {
            const auto a = mCurSeq.frames[0].rgba[p];
            if (a == 0u) curHasTrans = true;
            else if (a < 255u) curHasSemi = true;
        }
        check(curHasTrans && curHasSemi && mCurSeq.frames[0].rgba != mCurSeq.frames[10].rgba,
              "MENU_CUR.ANI preserves transparent background, soft alpha fringe, and frame animation");

        const auto airFireAni = ss2::assets::readSueEntry(gameCommon, "AIRFIRE.ANI");
        const auto airFireCol = ss2::assets::readSueEntry(gameCommon, "AIRFIRE.COL");
        const auto airFireSeq = ss2::assets::decodeAniSequence(airFireAni, airFireCol);
        check(airFireSeq.frames.size() == 25u, "AIRFIRE.ANI decodes 25 frames");
        bool airHasTrans = false, airHasSemi = false;
        for (const auto& fr : airFireSeq.frames) {
            for (std::size_t p = 3u; p < fr.rgba.size(); p += 4u) {
                const auto a = fr.rgba[p];
                if (a == 0u) airHasTrans = true;
                else if (a < 255u) airHasSemi = true;
            }
        }
        check(airHasTrans && airHasSemi,
              "AIRFIRE.ANI (0xa1 + 1024B .COL) preserves transparent background and soft flame alpha");
    }

    // Retail Scheme3 (Beach) descriptors: desc_scheme3.sue + lang_desc_scheme3.sue LAND\DESCDOM.
    {
        const auto beachCat = ss2::assets::UnitCatalog::load(gameDir, 2u);
        check(beachCat->buildingName(0u) == "HUT",
              "retail UnitCatalog Scheme3 resolves lang_desc_scheme3.sue LAND\\DESCDOM name obj 0 = HUT");
        const auto* beachTiger = beachCat->find("tiger");
        check(beachTiger && beachTiger->health > 0,
              "retail UnitCatalog Scheme3 still resolves UNITS\\tiger from desc_common.sue");
    }

    // Retail TotalVictoryTools unpacked mycommon\ camouflage .COL overlay (without modifying ss2-private).
    {
        const auto tvtCol = ss2::assets::resolveCaseInsensitivePath(
            gameDir, "TotalVictoryTools/Camouflages/Early production TIGER.COL");
        std::error_code ec;
        if (std::filesystem::is_regular_file(tvtCol, ec)) {
            struct ActiveModReset {
                ~ActiveModReset() { ss2::assets::setActiveMod(""); }
            } resetMod;
            TempDir tvtMod("ss2_tvt_camo");
            const auto myCommon = tvtMod.path / "mycommon";
            std::filesystem::create_directories(myCommon);
            std::filesystem::copy_file(tvtCol, myCommon / "TIGER.COL",
                                       std::filesystem::copy_options::overwrite_existing);

            const std::array<ss2::assets::Placement, 1> tigerPlacement{{{"tiger", 96, 96, 4, 100, 10, 0, 0}}};
            auto renderTiger = [&] {
                ss2::assets::MapImage canvas{192u, 192u, std::vector<std::uint8_t>(192u * 192u * 4u, 0u)};
                ss2::assets::VehicleScene scene(gameDir, tigerPlacement);
                scene.drawAll(canvas);
                return canvas;
            };
            const auto baseTiger = renderTiger();
            ss2::assets::setActiveMod(tvtMod.path.string());
            const auto moddedTiger = renderTiger();
            ss2::assets::setActiveMod("");

            const bool anyPixel = std::any_of(moddedTiger.rgba.begin(), moddedTiger.rgba.end(),
                                              [](std::uint8_t b) { return b != 0u; });
            check(anyPixel && baseTiger.rgba != moddedTiger.rgba,
                  "TotalVictoryTools unpacked mycommon/TIGER.COL overrides VehicleScene camo over retail game_common.sue");
        }
    }
}

void testModVfsAndAddons() {
    using namespace ss2::assets;
    struct ActiveModReset {
        ~ActiveModReset() { setActiveMod(""); }
    } resetMod;

    TempDir tmp("ss2_mod_vfs");
    const auto gameDir = tmp.path / "game";
    std::filesystem::create_directories(gameDir / "maps" / "single");
    std::filesystem::create_directories(gameDir / "addons");
    std::filesystem::create_directories(gameDir / "mycommon" / "sub");
    std::filesystem::create_directories(gameDir / "winter_scheme2");

    // 1. Unpacked folder mount: raw loose file + 0x1f 0x8b gzip loose file + traversal guard.
    const Bytes rawCol(512u, 0x5Au);
    {
        std::ofstream f(gameDir / "mycommon" / "Tiger.col", std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(rawCol.data()), static_cast<std::streamsize>(rawCol.size()));
    }
    Bytes loosePayload(1024u, 0u);
    for (std::size_t i = 0; i < loosePayload.size(); ++i) {
        loosePayload[i] = static_cast<std::uint8_t>((i * 29u + 7u) & 0xFFu);
    }
    writeGzipFile(gameDir / "mycommon" / "sub" / "Gzipped.bin", loosePayload);

    check(sueEntrySize(gameDir / "mycommon", "TIGER.COL") == 512u, "unpacked dir sueEntrySize raw");
    check(sueEntrySize(gameDir / "mycommon", "SUB\\GZIPPED.BIN") == 1024u, "unpacked dir sueEntrySize gzip");
    check(readSueEntry(gameDir / "mycommon", "tiger.col") == rawCol, "unpacked dir readSueEntry raw");
    check(readSueEntry(gameDir / "mycommon", "sub\\gzipped.bin") == loosePayload, "unpacked dir readSueEntry gzip");
    check(readSueEntryRange(gameDir / "mycommon", "SUB\\GZIPPED.BIN", 100u, 200u) ==
              Bytes(loosePayload.begin() + 100, loosePayload.begin() + 300),
          "unpacked dir readSueEntryRange gzip slice");
    check(readSueEntryRange(gameDir / "mycommon", "tiger.col", 10u, 32u) ==
              Bytes(rawCol.begin() + 10, rawCol.begin() + 42),
          "unpacked dir readSueEntryRange raw slice");
    check(!tryReadSueEntry(gameDir / "mycommon", "..\\secret.bin").has_value(),
          "unpacked dir rejects parent traversal");

    // 2. Base archives + sudtest.ini [Data] & [Game] + Scheme2 winter + Sounds=mod_snd.
    const std::string baseDescText = "health 100\nsight 10\n";
    const std::string baseDescdom = "name obj 0 \"Base Hut\"\n";
    writeSueArchive(gameDir / "desc_common.sue", {
        {"UNITS\\officer", Bytes(baseDescText.begin(), baseDescText.end()), false, 0u},
        {"LAND\\DESCDOM", Bytes(baseDescdom.begin(), baseDescdom.end()), false, 0u},
    });
    writeSueArchive(gameDir / "game_common.sue", {
        {"dummy.txt", {'o', 'k'}, false, 0u},
    });
    writeSueArchive(gameDir / "game_scheme1.sue", {
        {"rhombs.res", {'s', '1'}, false, 0u},
    });
    {
        std::ofstream w(gameDir / "winter_scheme2" / "rhombs.res", std::ios::binary | std::ios::trunc);
        w << "w2";
        std::ofstream sh(gameDir / "mod_snd.hdr", std::ios::binary | std::ios::trunc);
        sh << "HDR";
        std::ofstream sr(gameDir / "mod_snd.rus", std::ios::binary | std::ios::trunc);
        sr << "RUS";
        std::ofstream ini(gameDir / "sudtest.ini", std::ios::binary | std::ios::trunc);
        ini << "[Data]\n"
            << "Common=lang.sue;[_single\\]maps\\single\\;[_camp000\\]maps\\campaign\\\n"
            << "Game=mycommon\\;game_common.sue\n"
            << "Sounds=mod_snd.\n"
            << "Scheme1=lang.sue;desc_common.sue;game_scheme1.sue\n"
            << "Scheme2=lang.sue;desc_common.sue;winter_scheme2\\\n"
            << "Scheme4=lang.sue;desc_common.sue\n"
            << "Addons=addons\\\n"
            << "[Game]\n"
            << "TurnDelay=28\n";
    }

    check(readSueEntryChain(resolveGameArchives(gameDir), "TIGER.COL") == rawCol,
          "sudtest.ini Game=mycommon\\;game_common.sue chain resolves unpacked entry");
    check(readSueEntryChain(resolveSchemeArchives(gameDir, 1u), "rhombs.res") == Bytes{'w', '2'},
          "sudtest.ini Scheme2 chain resolves winter folder");
    check(readSueEntryChain(resolveSchemeArchives(gameDir, 3u), "rhombs.res") == Bytes{'s', '1'},
          "Scheme4 without desert tiles falls back cleanly to Scheme1");
    check(schemeDisplayName(0u) == "Summer" && schemeDisplayName(1u) == "Winter" &&
              schemeDisplayName(2u) == "Beach" && schemeDisplayName(3u) == "Desert",
          "schemeDisplayName labels Summer/Winter/Beach/Desert");
    const auto snd = resolveSoundBankFiles(gameDir);
    check(snd.hdr.filename() == "mod_snd.hdr" && snd.rus.filename() == "mod_snd.rus",
          "sudtest.ini Sounds=mod_snd. resolves mod sound bank files");
    check(resolveIniTurnDelay(gameDir) == 28u, "resolveIniTurnDelay reads TurnDelay=28");

    // 3. addons/*.ssx (FZFS SUE pack + standalone .ssx mission) + maps/single/*.ssm.
    MissionSpec mSpecBase{0u, 64u, 64u, "Base Skirmish\nHold line", {}, 0u, 1u, false, std::nullopt};
    writeMissionFile(gameDir / "maps" / "single" / "Base_One.ssm", mSpecBase);

    MissionSpec mSpecAlpha{1u, 128u, 64u, "Alpha Pack\nWinter raid", {}, 0u, 1u, false, std::nullopt};
    writeMissionFile(tmp.path / "tmp_alpha.ssm", mSpecAlpha);
    Bytes alphaGz;
    {
        std::ifstream f(tmp.path / "tmp_alpha.ssm", std::ios::binary | std::ios::ate);
        const auto sz = f.tellg();
        f.seekg(0);
        alphaGz.resize(static_cast<std::size_t>(sz));
        f.read(reinterpret_cast<char*>(alphaGz.data()), sz);
    }
    MissionSpec mSpecBravo{2u, 64u, 128u, "Bravo Pack\nBeach landing", {}, 0u, 1u, false, std::nullopt};
    const Bytes bravoRaw = buildSyntheticMissionBytes(mSpecBravo);
    writeSueArchive(gameDir / "addons" / "pack01.ssx", {
        {"Alpha Strike.ssm", alphaGz, false, 0u},
        {"Bravo Raid.ssm", bravoRaw, true, 512u},
    });
    MissionSpec mSpecSolo{3u, 64u, 64u, "Solo SSX\nDesert sortie", {}, 0u, 1u, false, std::nullopt};
    writeMissionFile(gameDir / "addons" / "Solo_Addon.ssx", mSpecSolo);

    const auto spMissions = listSinglePlayerMissions(gameDir);
    check(spMissions.size() == 4u, "listSinglePlayerMissions discovers maps/single + FZFS .ssx pack + standalone .ssx");
    const auto alphaFound = findSinglePlayerMission(gameDir, "Alpha Strike");
    const auto bravoFound = findSinglePlayerMission(gameDir, "Bravo Raid");
    const auto soloFound = findSinglePlayerMission(gameDir, "Solo_Addon");
    check(alphaFound && bravoFound && soloFound, "findSinglePlayerMission finds .ssx pack members and standalone .ssx");
    const auto alphaData = readMissionData(alphaFound->path);
    const auto bravoData = readMissionData(bravoFound->path);
    const auto soloData = readMissionData(soloFound->path);
    check(alphaData.scheme == 1u && bravoData.scheme == 2u && soloData.scheme == 3u,
          "readMissionData decodes gzipped and raw FZFS .ssx entries plus standalone .ssx");

    // 4. Drop-in mods/<Name>/ and Run/<Name>/ discovery + live UnitCatalog override & revert.
    const auto balDir = gameDir / "mods" / "BalanceMod";
    std::filesystem::create_directories(balDir / "UNITS");
    std::filesystem::create_directories(balDir / "LAND");
    {
        std::ofstream uo(balDir / "UNITS" / "officer", std::ios::binary | std::ios::trunc);
        uo << "health 250\nsight 14\n";
        std::ofstream dd(balDir / "LAND" / "DESCDOM", std::ios::binary | std::ios::trunc);
        dd << "name obj 0 \"Mod Bunker\"\n";
    }
    const auto runDir = gameDir / "Run" / "RwmSwitch";
    std::filesystem::create_directories(runDir);
    {
        std::ofstream ri(runDir / "sudtest.ini", std::ios::binary | std::ios::trunc);
        ri << "[Game]\nTurnDelay=32\n";
    }

    const auto mods = listInstalledMods(gameDir, tmp.path / "empty_user_mods");
    check(mods.size() == 2u, "listInstalledMods discovers both mods/ and Run/ mods");

    setActiveMod("BalanceMod");
    const auto modCat = UnitCatalog::load(gameDir, 0u);
    const auto* modOff = modCat->find("officer");
    check(modOff && modOff->health == 250 && modCat->buildingName(0u) == "Mod Bunker",
          "activeMod BalanceMod overrides UnitCatalog UNITS\\officer and LAND\\DESCDOM");

    setActiveMod("RwmSwitch");
    check(resolveIniTurnDelay(gameDir) == 32u, "activeMod Run/RwmSwitch overrides TurnDelay=32");

    setActiveMod("vanilla");
    const auto baseCat = UnitCatalog::load(gameDir, 0u);
    const auto* baseOff = baseCat->find("officer");
    check(baseOff && baseOff->health == 100 && baseCat->buildingName(0u) == "Base Hut",
          "setActiveMod(\"vanilla\") restores base UnitCatalog stats");

    // Verify retail GOG/CD Tutorial I fallback when maps.src/map.020 is absent.
    {
        const TempDir tutOnly("ss2_test_tut1_ssm_fallback");
        const auto singleDir = tutOnly.path / "maps" / "single";
        std::filesystem::create_directories(singleDir);
        MissionSpec spec{};
        spec.rawDescription = "Fallback Tut";
        SupportSpec sup{};
        sup.unames = {"rguner", "bt7"};
        auto raw = buildSyntheticMissionBytes(spec, &sup);
        Bytes iUnits;
        appendU32(iUnits, 2u);
        auto appendTopUnit = [&](std::uint16_t unameIdx, std::uint16_t u, std::uint16_t v) {
            iUnits.push_back(1);
            iUnits.push_back(100);
            iUnits.push_back(100);
            iUnits.push_back(0);
            iUnits.push_back(0);
            iUnits.push_back(50);
            appendU16(iUnits, unameIdx);
            appendU16(iUnits, 0);
            iUnits.push_back(0);
            iUnits.push_back(0);
            appendU16(iUnits, u);
            appendU16(iUnits, v);
            iUnits.push_back(0);
            iUnits.push_back(0);
        };
        appendTopUnit(0u, 16u, 16u);
        appendTopUnit(1u, 32u, 32u);
        appendRecord(raw, "I-UNITS", iUnits);
        writeGzipFile(singleDir / "--- TUTORIAL I ---.SSM", raw);

        check(resolveTutorialMissionPath(tutOnly.path) == singleDir / "--- TUTORIAL I ---.SSM",
              "resolveTutorialMissionPath locates --- TUTORIAL I ---.SSM without maps.src");
        const auto tutUnits = readTutorialPlacements(tutOnly.path);
        check(tutUnits.size() == 2u && tutUnits[0].id == "rguner" && tutUnits[1].id == "bt7",
              "readTutorialPlacements falls back to --- TUTORIAL I ---.SSM when maps.src/map.020 is absent");
    }

    if (const char* retailEnv = std::getenv("SS2_PRIVATE_GAME"); retailEnv && *retailEnv) {
        const std::filesystem::path retailDir(retailEnv);
        const auto tutSsm = resolveTutorialMissionPath(retailDir);
        if (!tutSsm.empty() && std::filesystem::is_regular_file(retailDir / "maps.src/map.020/info")) {
            const TempDir gogSim("ss2_test_gog_tut1_only");
            std::filesystem::create_directories(gogSim.path / "maps" / "single");
            std::filesystem::copy_file(tutSsm, gogSim.path / "maps" / "single" / "--- TUTORIAL I ---.SSM");
            for (const char* f : {"sudtest.ini", "desc_common.sue", "game_common.sue", "game_scheme1.sue", "lang.sue"}) {
                const auto src = resolveCaseInsensitivePath(retailDir, f);
                if (!src.empty()) std::filesystem::create_symlink(src, gogSim.path / src.filename());
            }
            const auto devMap = loadTutorialTerrain(retailDir);
            const auto gogMap = loadTutorialTerrain(gogSim.path);
            check(gogMap.width == devMap.width && gogMap.height == devMap.height,
                  "loadTutorialTerrain matches dimensions via --- TUTORIAL I ---.SSM without maps.src");
            std::vector<BuildingPlacement> devB, gogB;
            std::vector<BridgePlacement> devBr, gogBr;
            auto devCanvas = devMap;
            auto gogCanvas = gogMap;
            const auto devObj = drawTutorialScenery(devCanvas, retailDir, nullptr, 0, &devB, &devBr);
            const auto gogObj = drawTutorialScenery(gogCanvas, gogSim.path, nullptr, 0, &gogB, &gogBr);
            check(devObj > 1000u && gogObj > 1000u && !gogB.empty() && gogBr.size() == devBr.size(),
                  "drawTutorialScenery without maps.src decodes Tutorial I scenery, buildings, and bridges");
            const auto devPl = readTutorialPlacements(retailDir);
            const auto gogPl = readTutorialPlacements(gogSim.path);
            check(devPl.size() == gogPl.size() && gogPl.size() > 50u,
                  "readTutorialPlacements without maps.src matches retail Tutorial I unit count");
        }
    }
}

} // namespace

int main() {
    try {
        testDecodeMissionText();
        testSyntheticMissions();
        testSueAndPaths();
        testModVfsAndAddons();
        testUnitCatalogPhase4A();
        testBriefingMedia();
        testVehiclesAndShadows();
        testOptionalRetailSweep();
    } catch (const std::exception& e) {
        std::cerr << "mission_test failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
