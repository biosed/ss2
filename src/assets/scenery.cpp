#include "assets/scenery.hpp"
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;
unsigned word32(View bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("Short scenery index");
    return unsigned(bytes[at]) | unsigned(bytes[at + 1]) << 8 |
           unsigned(bytes[at + 2]) << 16 | unsigned(bytes[at + 3]) << 24;
}

enum class Layout { Plain, EmbeddedPalette, House, Stand };

struct Atlas {
    Bytes res, palette;
    size_t frameOffset;
    Layout layout;
};
struct Object {
    const Atlas* atlas;
    unsigned index;
    int x, y;
    bool onGround;
};

void drawHouse(MapImage& map, int x, int y, View record) {
    if (record.size() < 736) throw std::runtime_error("Truncated house scenery record");
    const auto palette = record.first(512);
    const unsigned ports = word32(record, 725);
    if (ports > 1024) throw std::runtime_error("House port count exceeds limit");
    size_t pos = 736u + size_t(ports) * 10u;
    if (pos > record.size()) throw std::runtime_error("House frame table out of bounds");
    // Blocks 0..2: intact left (0xa1), intact right (0xa1), intact base (0xa2).
    View blocks[3];
    for (int i = 0; i < 3; ++i) {
        const size_t len = word32(record, pos);
        pos += 4;
        if (len > record.size() - pos) throw std::runtime_error("Truncated house sprite block");
        blocks[i] = record.subspan(pos, len);
        pos += len;
    }
    if (!blocks[2].empty()) drawIndexedFrame(map, x, y, blocks[2], palette);
    if (!blocks[0].empty()) drawIndexedFrame(map, x, y, blocks[0], palette);
    if (!blocks[1].empty()) drawIndexedFrame(map, x, y, blocks[1], palette);
}

void drawStand(MapImage& map, int x, int y, View record, View palettes) {
    if (record.size() < 229) throw std::runtime_error("Truncated stand scenery record");
    const size_t palIndex = word32(record, 0);
    if (palIndex >= palettes.size() / 512) throw std::runtime_error("Stand palette index out of range");
    const size_t frameOffset = word32(record, 205);
    if (!frameOffset || frameOffset >= record.size()) throw std::runtime_error("Invalid stand frame offset");
    drawIndexedFrame(map, x, y, record.subspan(frameOffset), palettes.subspan(palIndex * 512, 512));
}
}

std::size_t drawTutorialScenery(MapImage& map, const std::filesystem::path& gameDir) {
    const auto path = gameDir / "maps.src/map.020/objects";
    if (std::filesystem::file_size(path) > (1u << 20)) throw std::runtime_error("Scenery list exceeds limit");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open tutorial scenery");
    const auto archive = gameDir / "game_scheme1.sue";
    std::map<std::string, Atlas> atlases;
    const auto load = [&](const char* key, const char* res, const char* pl, size_t offset, Layout layout = Layout::Plain) {
        atlases.emplace(key, Atlas{readSueEntry(archive, res), pl ? readSueEntry(archive, pl) : Bytes{}, offset, layout});
    };
    load("tree", "tree.res", "tree.pl", 29);
    load("cliff", "cliff.res", "cliff.pl", 8);
    load("trop", "TROP.RES", "TROP.PL", 0);
    load("fre", "fre.res", "fre.pl", 0);
    load("lay", "LAY.RES", "lay.pl", 5);
    load("zabor", "zabor.res", "zabor.pl", 4);
    load("vor", "vor.res", "vor.pl", 0);
    // Bridge records have their own 256-entry palette and a first undamaged frame.
    load("most", "MOST.RES", nullptr, 2004, Layout::EmbeddedPalette);
    // Houses (dom.res) carry an embedded 512B palette + 16 intact/damage blocks; empty 0-byte records are skipped.
    load("dom", "dom.res", nullptr, 0, Layout::House);
    // Stands (STAND.RES) select one of 8 palettes in STAND.PL and store 6 frame offsets at byte 205.
    load("stand", "STAND.RES", "STAND.PL", 0, Layout::Stand);

    std::vector<Object> objects;
    for (std::string line; std::getline(input, line);) {
        std::istringstream row(line);
        std::string kind;
        int u{}, v{}, index{};
        if (!(row >> kind >> u >> v >> index) || u < 0 || v < 0 || u >= 4096 || v >= 4096 || index < 0)
            throw std::runtime_error("Invalid tutorial scenery placement");
        const auto atlas = atlases.find(kind);
        if (atlas != atlases.end())
            objects.push_back({&atlas->second, unsigned(index), int(map.width / 2) + u - v,
                               (u + v) / 2, kind == "trop" || kind == "lay" || kind == "vor"});
    }
    std::stable_sort(objects.begin(), objects.end(), [](const Object& a, const Object& b) {
        return a.onGround != b.onGround ? a.onGround : a.y < b.y;
    });
    std::size_t drawn = 0;
    for (const auto& obj : objects) {
        const auto& a = *obj.atlas;
        const View data(a.res);
        const unsigned count = word32(data, 0);
        if (obj.index >= count || (std::uint64_t(count) + 2) * 4 > data.size())
            throw std::runtime_error("Invalid scenery atlas index");
        const auto start = word32(data, 4 + 4 * obj.index), end = word32(data, 8 + 4 * obj.index);
        if (end > data.size() || end < start)
            throw std::runtime_error("Invalid scenery record range");
        const auto record = data.subspan(start, end - start);
        if (a.layout == Layout::House) {
            if (record.empty()) continue;
            drawHouse(map, obj.x, obj.y, record);
            ++drawn;
            continue;
        }
        if (a.layout == Layout::Stand) {
            drawStand(map, obj.x, obj.y, record, a.palette);
            ++drawn;
            continue;
        }
        if (a.frameOffset > record.size())
            throw std::runtime_error("Invalid scenery frame offset");
        const auto palette = a.layout == Layout::EmbeddedPalette ? record.first(512) : View(a.palette);
        drawIndexedFrame(map, obj.x, obj.y, record.subspan(a.frameOffset), palette);
        ++drawn;
    }
    return drawn;
}
}
