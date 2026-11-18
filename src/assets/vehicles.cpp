#include "assets/vehicles.hpp"
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;

std::uint32_t u32(View data, std::size_t pos) {
    if (pos > data.size() || data.size() - pos < 4)
        throw std::runtime_error("Truncated vehicle asset record");
    return std::uint32_t(data[pos]) | (std::uint32_t(data[pos + 1]) << 8) |
           (std::uint32_t(data[pos + 2]) << 16) | (std::uint32_t(data[pos + 3]) << 24);
}

std::int32_t i32(View data, std::size_t pos) {
    return static_cast<std::int32_t>(u32(data, pos));
}

View pckFrame(View pck, std::size_t index) {
    const std::size_t first = u32(pck, 0);
    if (first < 8 || (first % 4) != 0 || first > pck.size())
        throw std::runtime_error("Invalid vehicle PCK frame table");
    const std::size_t count = first / 4 - 1;
    if (index >= count)
        throw std::runtime_error("Vehicle PCK frame index out of range");
    const std::size_t start = u32(pck, index * 4);
    const std::size_t end = u32(pck, (index + 1) * 4);
    if (start < first || end < start || end > pck.size())
        throw std::runtime_error("Invalid vehicle PCK frame range");
    return pck.subspan(start, end - start);
}



enum class Kind { Tank, Artillery, HeavyMg };

struct Spec {
    const char* pck;
    const char* col;
    const char* hot;
    Kind kind;
};

struct LoadedAsset {
    Bytes pck;
    Bytes col;
    Bytes hot;
    Kind kind;
};

struct Vehicle {
    const LoadedAsset* asset;
    int x, y;
    unsigned dir;
};
}

std::vector<Placement> drawTutorialVehicles(MapImage& map, const std::filesystem::path& gameDir, std::span<const Placement> source) {
    // Original UNITS definitions in desc_common.sue -> game_common.sue exact case-sensitive entry names.
    const std::map<std::string, Spec> specs{
        {"panther", {"PANTHER.PCK", "PANTHER.COL", "panther.hot", Kind::Tank}},
        {"zis-3",   {"GUN45RUS.PCK", "GUN45RUS.COL", nullptr, Kind::Artillery}},
        {"m3a1",    {"GUN100.PCK", "GUN100.COL", nullptr, Kind::Artillery}},
        {"ds39",    {"MACHGUN.PCK", "MACHGUN.COL", nullptr, Kind::HeavyMg}},
    };

    const auto archive = gameDir / "game_common.sue";
    std::map<std::string, LoadedAsset> loaded;
    std::vector<Vehicle> placements;
    std::vector<Placement> units;

    for (const auto& unit : source) {
        const auto& id = unit.id;
        const auto specIt = specs.find(id);
        if (specIt == specs.end()) continue;

        auto [it, inserted] = loaded.try_emplace(id);
        if (inserted) {
            const auto& spec = specIt->second;
            it->second.pck = readSueEntry(archive, spec.pck);
            it->second.col = readSueEntry(archive, spec.col);
            if (it->second.col.size() != 512)
                throw std::runtime_error("Invalid vehicle COL palette size");
            if (spec.hot) {
                it->second.hot = readSueEntry(archive, spec.hot);
                if (it->second.hot.size() < 32u * 8u)
                    throw std::runtime_error("Invalid vehicle HOT table size");
            }
            it->second.kind = spec.kind;
        }
        units.push_back(unit);
        placements.push_back({&it->second, unit.x, unit.y, unsigned(unit.direction)});
    }

    std::stable_sort(placements.begin(), placements.end(),
                     [](const Vehicle& a, const Vehicle& b) { return a.y < b.y; });

    for (const auto& p : placements) {
        const auto& a = *p.asset;
        switch (a.kind) {
        case Kind::Tank: {
            // Block 0: intact hull (0..31); Block 6: intact turret (192..223) offset by hot0[dir].
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, p.dir), a.col);
            const int hotDx = i32(a.hot, std::size_t(p.dir) * 8u);
            const int hotDy = i32(a.hot, std::size_t(p.dir) * 8u + 4u);
            if (hotDx < -2048 || hotDx > 2048 || hotDy < -2048 || hotDy > 2048)
                throw std::runtime_error("Vehicle turret offset exceeds sprite bounds");
            drawIndexedFrame(map, p.x + hotDx, p.y + hotDy, pckFrame(a.pck, 192u + p.dir), a.col);
            break;
        }
        case Kind::Artillery:
            // Block 0: wheels (0..31), Block 1: deployed carriage (32..63), Block 5: shield+barrel (160..191).
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, p.dir), a.col);
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, 32u + p.dir), a.col);
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, 160u + p.dir), a.col);
            break;
        case Kind::HeavyMg:
            // Frames 0..31: deployed heavy MG directions.
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, p.dir), a.col);
            break;
        }
    }
    return units;
}
}
