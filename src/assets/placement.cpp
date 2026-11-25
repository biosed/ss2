#include "assets/placement.hpp"
#include "assets/mission.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <zlib.h>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;

std::uint16_t u16(View bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2)
        throw std::runtime_error("Truncated mission placement word");
    return static_cast<std::uint16_t>(bytes[at] | (unsigned(bytes[at + 1]) << 8));
}

std::uint32_t u32(View bytes, std::size_t at) {
    return std::uint32_t(u16(bytes, at)) | (std::uint32_t(u16(bytes, at + 2)) << 16);
}
} // namespace

std::vector<Placement> readTutorialPlacements(const std::filesystem::path& gameDir) {
    const auto path = resolveCaseInsensitivePath(gameDir, "maps.src/map.020/mis.000/mapunits");
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
        const auto tut = resolveTutorialMissionPath(gameDir);
        if (!tut.empty()) return readMissionPlacements(tut);
        throw std::runtime_error("Cannot read Tutorial I unit list (neither maps.src/map.020 nor --- TUTORIAL I ---.SSM)");
    }
    if (std::filesystem::file_size(path) > (1u << 20)) throw std::runtime_error("Unit list exceeds size limit");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read Tutorial I unit list");
    std::vector<Placement> units;
    for (std::string line; std::getline(input, line);) {
        std::istringstream stream(line);
        std::map<std::string, std::string> fields;
        for (std::string token; stream >> token;) {
            const auto pos = token.find('=');
            if (pos != std::string::npos) fields.emplace(token.substr(0, pos), token.substr(pos + 1));
        }
        // Nested passengers have no independent map coordinates.
        if (!fields.contains("U") && !fields.contains("V")) continue;
        const auto number = [&](const char* key) {
            const auto it = fields.find(key);
            if (it == fields.end()) throw std::runtime_error("Missing unit field " + std::string(key));
            const auto& field = it->second;
            int value{};
            const auto [end, err] = std::from_chars(field.data(), field.data() + field.size(), value);
            if (err != std::errc{} || end != field.data() + field.size())
                throw std::runtime_error("Invalid unit field " + std::string(key));
            return value;
        };
        const int u = number("U"), v = number("V"), dir = number("Dir");
        const int expa = fields.contains("Expa") ? std::clamp(number("Expa"), 0, 100) : 0;
        if (u < 0 || v < 0 || u >= 4096 || v >= 4096 || dir < 0 || dir >= 32 || !fields.contains("ID"))
            throw std::runtime_error("Invalid Tutorial I unit placement");
        units.push_back({fields.at("ID"), 4096 + u - v, (u + v) / 2, dir, number("HP"),
                         number("Ammo"), number("Grp"), number("Owner"), {}, expa});
    }
    return units;
}

std::vector<Placement> readMissionPlacements(const std::filesystem::path& missionPath) {
    const auto archive = loadMissionArchive(missionPath);
    const std::uint32_t gridW = archive.width;
    const std::uint32_t gridH = archive.height;

    const auto iUnames = archive.requireMissionRecord("I-UNAMES");
    if ((iUnames.size() % 16u) != 0)
        throw std::runtime_error("Invalid mission unit name table");
    std::vector<std::string_view> unames;
    unames.reserve(iUnames.size() / 16u);
    for (std::size_t off = 0; off < iUnames.size(); off += 16u) {
        const auto slot = iUnames.subspan(off, 16u);
        const auto nul = std::find(slot.begin(), slot.end(), std::uint8_t{0});
        unames.emplace_back(reinterpret_cast<const char*>(slot.data()),
                            static_cast<std::size_t>(nul - slot.begin()));
    }

    const auto iUnits = archive.requireMissionRecord("I-UNITS");
    if (iUnits.size() < 4u) throw std::runtime_error("Truncated mission unit table");
    const std::uint32_t topCount = u32(iUnits, 0);

    const int originX = int(gridH) * 32;
    const std::uint32_t maxU = gridW * 32u;
    const std::uint32_t maxV = gridH * 32u;
    std::vector<Placement> units;
    units.reserve(std::min<std::size_t>(topCount, (iUnits.size() - 4u) / 18u));
    std::size_t pos = 4u;
    // Retail campaign 023001.ssc declares topCount one higher than actual records while ending cleanly at EOF.
    for (std::uint32_t i = 0; i < topCount && pos < iUnits.size(); ++i) {
        if (pos + 18u > iUnits.size())
            throw std::runtime_error("Truncated mission unit block");
        const int grp = iUnits[pos + 0];
        const int hp = iUnits[pos + 1];
        const int ammo = iUnits[pos + 2];
        const int expa = std::clamp<int>(iUnits[pos + 3], 0, 100);
        // Bytes @4..@5 store lives, morale; @8..@9 store padding.
        const std::uint16_t unameIdx = u16(iUnits, pos + 6);
        const std::uint8_t nRiders = iUnits[pos + 10];
        const std::uint8_t nCrew = iUnits[pos + 11];
        const std::uint16_t u = u16(iUnits, pos + 12);
        const std::uint16_t v = u16(iUnits, pos + 14);
        const int dir = iUnits[pos + 16];
        const int owner = iUnits[pos + 17];
        if (unameIdx >= unames.size() || unames[unameIdx].empty())
            throw std::runtime_error("Invalid mission unit name index");
        if (u >= maxU || v >= maxV || dir >= 32 || owner >= 4)
            throw std::runtime_error("Invalid mission unit placement");
        pos += 18u;
        const std::size_t totalPassengers = std::size_t(nRiders) + std::size_t(nCrew);
        const std::size_t passengerBytes = totalPassengers * 10u;
        if (passengerBytes > iUnits.size() - pos)
            throw std::runtime_error("Truncated mission unit passengers");
        std::vector<PassengerPlacement> passengers;
        passengers.reserve(totalPassengers);
        for (std::size_t p = 0; p < totalPassengers; ++p) {
            const int pGrp = iUnits[pos + 0];
            const int pHp = iUnits[pos + 1];
            const int pAmmo = iUnits[pos + 2];
            const int pExpa = iUnits[pos + 3];
            const std::uint16_t pUnameIdx = u16(iUnits, pos + 6);
            if (pUnameIdx >= unames.size() || unames[pUnameIdx].empty())
                throw std::runtime_error("Invalid mission passenger unit name index");
            passengers.push_back({
                std::string(unames[pUnameIdx]), pGrp, pHp, pAmmo, pExpa, p < std::size_t(nCrew)
            });
            pos += 10u;
        }
        units.push_back({std::string(unames[unameIdx]), originX + int(u) - int(v),
                         (int(u) + int(v)) / 2, dir, hp, ammo, grp, owner, std::move(passengers), expa});
    }
    if (pos != iUnits.size())
        throw std::runtime_error("Trailing mission unit bytes");
    return units;
}
}
