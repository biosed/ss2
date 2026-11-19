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

enum class Layout { Plain, EmbeddedPalette, House, Stand, Tree };

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

void drawHouse(MapImage& map, int x, int y, View record, SceneryDepth* depth) {
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
    for (int i = 0; i < 2; ++i) {
        if (blocks[i].empty()) continue;
        SpriteCoverage coverage;
        drawIndexedFrame(map, x, y, blocks[i], palette, depth ? &coverage : nullptr);
        if (depth) depth->addBody(coverage, y);
    }
}

void drawDepthBody(MapImage& map, const Object& object, View record, SceneryDepth* depth) {
    const bool stand = object.atlas->layout == Layout::Stand;
    const std::size_t table = stand ? 205 : 5;
    const auto palette = View(object.atlas->palette);
    const auto index = stand ? word32(record,0) : 0u;
    if (index >= palette.size()/512) throw std::runtime_error("Scenery palette index out of range");
    const auto frame = word32(record,table);
    if (!frame || frame >= record.size()) throw std::runtime_error("Invalid scenery body offset");
    SpriteCoverage coverage;
    drawIndexedFrame(map,object.x,object.y,record.subspan(frame),palette.subspan(index*512,512),
                     depth ? &coverage : nullptr);
    if (!depth) return;
    depth->addBody(coverage,object.y);
    const auto shadow = word32(record,table+8);
    if (shadow >= record.size()) throw std::runtime_error("Invalid scenery shadow offset");
    if (shadow) depth->addShadow(readShadowCoverage(object.x,object.y,record.subspan(shadow)));
}
}

SceneryDepth::SceneryDepth(int width, int height)
    : width_(width), height_(height), columns_((width+side-1)/side),
      tiles_(std::size_t(columns_)*((height+side-1)/side)) {}

void SceneryDepth::stamp(const SpriteCoverage& shape, std::uint16_t bodyDepth) {
    for (int y = std::max(0,shape.top); y < std::min(height_,shape.top+shape.height); ++y) {
        for (int x = std::max(0,shape.left); x < std::min(width_,shape.left+shape.width); ++x) {
            if (!shape.covers(x,y)) continue;
            auto& tile = tiles_[std::size_t(y/side)*columns_+x/side];
            if (!tile) tile = std::make_unique<Tile>();
            auto& value = (*tile)[(y%side)*side+x%side];
            if (bodyDepth) value = std::max<unsigned>(bodyDepth,value&~1u) | (value&1);
            else value |= 1;
        }
    }
}

void SceneryDepth::addBody(const SpriteCoverage& shape, int sortY) {
    if (sortY < 0 || sortY > 32766) throw std::runtime_error("Scenery depth out of range");
    stamp(shape,2*(sortY+1));
}
void SceneryDepth::addShadow(const SpriteCoverage& shape) { stamp(shape,0); }

std::uint16_t SceneryDepth::depth(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
    const auto& tile = tiles_[std::size_t(y/side)*columns_+x/side];
    return tile ? (*tile)[(y%side)*side+x%side] : 0;
}

bool SceneryDepth::occludes(int x, int y, int footY, bool revealed, int checkerPhase) const {
    if (revealed && ((x^y^checkerPhase)&1) == 0) return false;
    const auto current = depth(x,y);
    if (current < 2) return false;
    // The original compares a duplicated soldier depth against the previous and current
    // 16-bit stencil words. An unshadowed left edge survives an equal-depth comparison.
    const int soldier = 2*(footY+1);
    return current > soldier || (current == soldier && depth(x-1,y) >= soldier);
}

bool SceneryDepth::intersects(int left, int top, int width, int height) const {
    const int x0 = std::max(0,left), y0 = std::max(0,top);
    const int x1 = std::min(width_,left+width), y1 = std::min(height_,top+height);
    if (x0 >= x1 || y0 >= y1) return false;
    for (int y = y0/side; y <= (y1-1)/side; ++y)
        for (int x = x0/side; x <= (x1-1)/side; ++x)
            if (tiles_[std::size_t(y)*columns_+x]) return true;
    return false;
}

std::size_t drawTutorialScenery(MapImage& map, const std::filesystem::path& gameDir, SceneryDepth* depth) {
    const auto path = gameDir / "maps.src/map.020/objects";
    if (std::filesystem::file_size(path) > (1u << 20)) throw std::runtime_error("Scenery list exceeds limit");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open tutorial scenery");
    const auto archive = gameDir / "game_scheme1.sue";
    std::map<std::string, Atlas> atlases;
    const auto load = [&](const char* key, const char* res, const char* pl, size_t offset, Layout layout = Layout::Plain) {
        atlases.emplace(key, Atlas{readSueEntry(archive, res), pl ? readSueEntry(archive, pl) : Bytes{}, offset, layout});
    };
    load("tree", "tree.res", "tree.pl", 0, Layout::Tree);
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
            drawHouse(map, obj.x, obj.y, record, depth);
            ++drawn;
            continue;
        }
        if (a.layout == Layout::Stand || a.layout == Layout::Tree) {
            drawDepthBody(map,obj,record,depth);
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
