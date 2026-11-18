#include "assets/infantry.hpp"
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;

int u16(View data, size_t pos) {
    if (pos > data.size() || data.size() - pos < 2) throw std::runtime_error("Truncated infantry sprite");
    return data[pos] | (int(data[pos + 1]) << 8);
}
std::uint32_t u32(View data, size_t pos) {
    return unsigned(u16(data, pos)) | (unsigned(u16(data, pos + 2)) << 16);
}

View firstPose(View rs2) {
    const auto count = u32(rs2, 0), size = u32(rs2, 4);
    if (!count || count > 256 || size < count * 4 || size > rs2.size() - 8)
        throw std::runtime_error("Invalid RS2 pose group");
    const auto group = rs2.subspan(8, size);
    const auto offset = u32(group, 0), end = count == 1 ? size : u32(group, 4);
    if (offset != count * 4 || end < offset || end > size) throw std::runtime_error("Invalid RS2 pose");
    return group.subspan(offset, end - offset);
}

int number(const std::map<std::string, std::string>& fields, const char* key) {
    const auto& field = fields.at(key);
    int value{};
    const auto [end, err] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (err != std::errc{} || end != field.data() + field.size())
        throw std::runtime_error("Invalid numeric infantry field");
    return value;
}
}

std::vector<Infantry> placeTutorialInfantry(MapImage& map, const std::filesystem::path& gameDir) {
    const auto path = gameDir / "maps.src/map.020/mis.000/mapunits";
    if (std::filesystem::file_size(path) > (1u << 20)) throw std::runtime_error("Unit list exceeds size limit");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read Tutorial I unit list");
    // Original UNITS definitions in desc_common.sue give the file stem for each ID.
    const std::map<std::string, std::string> names{
        {"aguner", "A-2CASQ.RS2"}, {"aofficer", "AOfice.rs2"}, {"cmedic", "ALRUSMED.RS2"},
        {"dflamer", "A-4.RS2"}, {"dguner", "DGuner.rs2"}, {"dofficer", "DOficer.rs2"},
        {"dsmguner", "DMGun.rs2"}, {"rsmguner", "RMGun.rs2"}};
    const auto archive = gameDir / "game_common.sue";
    const auto palette = readSueEntry(archive, "SOLD.PL");
    std::map<std::string, Bytes> poses;
    std::vector<Infantry> units;
    for (std::string line; std::getline(input, line);) {
        std::istringstream stream(line);
        std::map<std::string, std::string> fields;
        for (std::string token; stream >> token;) {
            const auto pos = token.find('=');
            if (pos != std::string::npos) fields.emplace(token.substr(0, pos), token.substr(pos + 1));
        }
        if (!fields.contains("U") || !fields.contains("V")) continue;
        const auto& id = fields.at("ID");
        if (!names.contains(id)) continue;
        const int u = number(fields, "U"), v = number(fields, "V");
        if (u < 0 || v < 0 || u >= 4096 || v >= 4096) throw std::runtime_error("Invalid tutorial placement");
        if (!poses.contains(id)) {
            const auto file = readSueEntry(archive, names.at(id));
            const auto frame = firstPose(file);
            poses.emplace(id, Bytes(frame.begin(), frame.end()));
        }
        const auto& sprite = poses.at(id);
        const int x = int(map.width / 2) + u - v;
        const int y = (u + v) / 2;
        units.push_back({id, x, y, number(fields, "HP"), number(fields, "Ammo"), number(fields, "Grp"), number(fields, "Owner"),
            x + std::int16_t(u16(sprite, 0)), y + std::int16_t(u16(sprite, 2)), u16(sprite, 4), u16(sprite, 6)});
    }
    std::stable_sort(units.begin(), units.end(), [](const auto& a, const auto& b) { return a.y < b.y; });
    for (const auto& unit : units) drawIndexedFrame(map, unit.x, unit.y, poses.at(unit.id), palette);
    return units;
}
}
