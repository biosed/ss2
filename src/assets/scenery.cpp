#include "assets/scenery.hpp"
#include "assets/mission.hpp"
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;
constexpr int maxGridCoord = 512 * 32;

unsigned word16(View bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2) throw std::runtime_error("Short scenery word");
    return unsigned(bytes[at]) | (unsigned(bytes[at + 1]) << 8);
}

unsigned word32(View bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("Short scenery index");
    return unsigned(bytes[at]) | unsigned(bytes[at + 1]) << 8 |
           unsigned(bytes[at + 2]) << 16 | unsigned(bytes[at + 3]) << 24;
}

enum class Layout { Plain, EmbeddedPalette, Bridge, House, Stand, Tree };

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
    int u{}, v{};
};
using AtlasMap = std::map<std::string, Atlas, std::less<>>;

AtlasMap loadSceneryAtlases(const std::filesystem::path& gameDir, unsigned scheme) {
    const auto scheme1Archive = resolveCaseInsensitivePath(gameDir, "game_scheme1.sue");
    std::vector<std::filesystem::path> beachArchives;
    if (scheme == 2) {
        beachArchives = {
            resolveCaseInsensitivePath(gameDir, "game_scheme3.sue"),
            scheme1Archive,
        };
    }
    const auto readEntry = [&](const char* name) -> Bytes {
        if (scheme == 2) return readSueEntryFallback(beachArchives, name);
        return readSueEntry(scheme1Archive, name);
    };
    AtlasMap atlases;
    const auto load = [&](const char* key, const char* res, const char* pl, size_t offset, Layout layout = Layout::Plain) {
        atlases.emplace(key, Atlas{readEntry(res), pl ? readEntry(pl) : Bytes{}, offset, layout});
    };
    load("tree", "tree.res", "tree.pl", 0, Layout::Tree);
    load("cliff", "cliff.res", "cliff.pl", 8);
    load("trop", "TROP.RES", "TROP.PL", 0);
    load("fre", "fre.res", "fre.pl", 0);
    load("lay", "LAY.RES", "lay.pl", 5);
    load("zabor", "zabor.res", "zabor.pl", 4);
    load("vor", "vor.res", "vor.pl", 0);
    // Bridge records have their own 256-entry palette, 38x38 passability grid, and 5 damage stages.
    load("most", "MOST.RES", nullptr, 2004, Layout::Bridge);
    // Houses (dom.res) carry an embedded 512B palette + 16 intact/damage blocks; empty 0-byte records are skipped.
    load("dom", "dom.res", nullptr, 0, Layout::House);
    // Stands (STAND.RES) select one of 8 palettes in STAND.PL and store 6 frame offsets at byte 205.
    load("stand", "STAND.RES", "STAND.PL", 0, Layout::Stand);
    return atlases;
}

void drawHouse(MapImage& map, int x, int y, View record, SceneryDepth* depth,
               BuildingPlacement* outBuilding = nullptr) {
    if (record.size() < 736) throw std::runtime_error("Truncated house scenery record");
    const auto palette = record.first(512);
    const unsigned ports = word32(record, 725);
    if (ports > 1024) throw std::runtime_error("House port count exceeds limit");
    size_t pos = 736u + size_t(ports) * 10u;
    if (pos > record.size()) throw std::runtime_error("House frame table out of bounds");
    // Blocks 0..2: intact left/right/base; 12..14: ruined left/right/base (blocks 3/7/11/15 are 0xa3 shadows).
    std::array<View, 16> blocks{};
    for (int i = 0; i < 16 && pos + 4 <= record.size(); ++i) {
        const size_t len = word32(record, pos);
        pos += 4;
        if (len > record.size() - pos) {
            if (i < 3) throw std::runtime_error("Truncated house sprite block");
            break;
        }
        blocks[static_cast<std::size_t>(i)] = record.subspan(pos, len);
        pos += len;
    }

    int ruinLeft = 0, ruinTop = 0;
    MapImage ruinedPatch;
    if (outBuilding && (!blocks[12].empty() || !blocks[13].empty() || !blocks[14].empty())) {
        bool haveRuinBounds = false;
        int rL = 0, rT = 0, rR = 0, rB = 0;
        for (int idx : {0, 1, 2, 12, 13, 14}) {
            const View blk = blocks[static_cast<std::size_t>(idx)];
            if (blk.size() < 9 || (blk[8] != 0xa1 && blk[8] != 0xa2)) continue;
            const int bx = x + static_cast<int>(static_cast<std::int16_t>(word16(blk, 0)));
            const int by = y + static_cast<int>(static_cast<std::int16_t>(word16(blk, 2)));
            const int bw = static_cast<int>(static_cast<std::int16_t>(word16(blk, 4)));
            const int bh = static_cast<int>(static_cast<std::int16_t>(word16(blk, 6)));
            if (bw <= 0 || bh <= 0) continue;
            if (!haveRuinBounds) {
                rL = bx; rT = by; rR = bx + bw; rB = by + bh;
                haveRuinBounds = true;
            } else {
                rL = std::min(rL, bx);
                rT = std::min(rT, by);
                rR = std::max(rR, bx + bw);
                rB = std::max(rB, by + bh);
            }
        }
        if (haveRuinBounds) {
            rL = std::clamp(rL, 0, static_cast<int>(map.width));
            rT = std::clamp(rT, 0, static_cast<int>(map.height));
            rR = std::clamp(rR, rL, static_cast<int>(map.width));
            rB = std::clamp(rB, rT, static_cast<int>(map.height));
            if (rR > rL && rB > rT) {
                ruinLeft = rL;
                ruinTop = rT;
                const unsigned rw = static_cast<unsigned>(rR - rL);
                const unsigned rh = static_cast<unsigned>(rB - rT);
                ruinedPatch = MapImage{rw, rh, std::vector<std::uint8_t>(std::size_t(rw) * rh * 4u, 0)};
                for (unsigned row = 0; row < rh; ++row) {
                    const auto srcBegin = map.rgba.begin() +
                        ((static_cast<std::size_t>(rT) + row) * map.width + static_cast<std::size_t>(rL)) * 4u;
                    std::copy_n(srcBegin, std::size_t(rw) * 4u,
                                ruinedPatch.rgba.data() + std::size_t(row) * rw * 4u);
                }
                for (int idx : {14, 12, 13}) {
                    const View blk = blocks[static_cast<std::size_t>(idx)];
                    if (blk.size() >= 9 && (blk[8] == 0xa1 || blk[8] == 0xa2)) {
                        drawIndexedFrame(ruinedPatch, x - rL, y - rT, blk, palette);
                    }
                }
            }
        }
    }

    if (!blocks[2].empty()) drawIndexedFrame(map, x, y, blocks[2], palette);
    bool haveBounds = false;
    int minL = 0, minT = 0, maxR = 0, maxB = 0;
    for (int i = 0; i < 2; ++i) {
        if (blocks[i].empty()) continue;
        SpriteCoverage coverage;
        const bool needCoverage = (depth != nullptr) || (outBuilding != nullptr);
        drawIndexedFrame(map, x, y, blocks[i], palette, needCoverage ? &coverage : nullptr);
        if (depth) depth->addBody(coverage, y);
        if (outBuilding && coverage.width > 0 && coverage.height > 0) {
            if (!haveBounds) {
                minL = coverage.left;
                minT = coverage.top;
                maxR = coverage.left + coverage.width;
                maxB = coverage.top + coverage.height;
                haveBounds = true;
            } else {
                minL = std::min(minL, coverage.left);
                minT = std::min(minT, coverage.top);
                maxR = std::max(maxR, coverage.left + coverage.width);
                maxB = std::max(maxB, coverage.top + coverage.height);
            }
        }
    }
    if (outBuilding) {
        const int cap = std::clamp<int>(static_cast<int>(ports), 2, 16);
        const int hp = 220 + cap * 35;
        if (haveBounds) {
            *outBuilding = {x, y, minL, minT, std::max(1, maxR - minL), std::max(1, maxB - minT),
                            cap, hp, ruinLeft, ruinTop, std::move(ruinedPatch)};
        } else {
            *outBuilding = {x, y, x - 36, y - 36, 72, 72,
                            cap, hp, ruinLeft, ruinTop, std::move(ruinedPatch)};
        }
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

void drawBridge(MapImage& map, const Object& obj, View record, BridgePlacement* outBridge = nullptr) {
    if (record.size() < 2004) throw std::runtime_error("Truncated bridge scenery record");
    const auto palette = record.first(512);
    std::array<std::size_t, 5> bodyAbs{};
    std::vector<std::size_t> cuts;
    cuts.reserve(11);
    for (int k = 0; k < 10; ++k) {
        const std::size_t rel = word32(record, 1960u + static_cast<std::size_t>(k) * 4u);
        if (rel == 0u) continue;
        const std::size_t absOff = 1960u + rel;
        if (absOff >= record.size()) continue;
        cuts.push_back(absOff);
        if ((k & 1) == 0) bodyAbs[static_cast<std::size_t>(k / 2)] = absOff;
    }
    if (bodyAbs[0] == 0u && 2004u < record.size()) {
        bodyAbs[0] = 2004u;
        cuts.push_back(2004u);
    }
    cuts.push_back(record.size());
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    std::array<View, 5> stageViews{};
    for (int s = 0; s < 5; ++s) {
        const std::size_t absOff = bodyAbs[static_cast<std::size_t>(s)];
        if (absOff == 0u || absOff >= record.size()) continue;
        const auto it = std::upper_bound(cuts.begin(), cuts.end(), absOff);
        const std::size_t endOff = (it != cuts.end()) ? *it : record.size();
        if (endOff > absOff) {
            stageViews[static_cast<std::size_t>(s)] = record.subspan(absOff, endOff - absOff);
        }
    }

    int patchLeft = 0, patchTop = 0;
    MapImage basePatch;
    if (outBridge) {
        bool haveUnion = false;
        int uL = 0, uT = 0, uR = 0, uB = 0;
        for (int s = 0; s < 5; ++s) {
            const View blk = stageViews[static_cast<std::size_t>(s)];
            if (blk.size() < 9u || (blk[8] != 0xa1u && blk[8] != 0xa2u)) continue;
            const int bx = obj.x + static_cast<int>(static_cast<std::int16_t>(word16(blk, 0)));
            const int by = obj.y + static_cast<int>(static_cast<std::int16_t>(word16(blk, 2)));
            const int bw = static_cast<int>(static_cast<std::int16_t>(word16(blk, 4)));
            const int bh = static_cast<int>(static_cast<std::int16_t>(word16(blk, 6)));
            if (bw <= 0 || bh <= 0) continue;
            if (!haveUnion) {
                uL = bx; uT = by; uR = bx + bw; uB = by + bh;
                haveUnion = true;
            } else {
                uL = std::min(uL, bx);
                uT = std::min(uT, by);
                uR = std::max(uR, bx + bw);
                uB = std::max(uB, by + bh);
            }
        }
        if (haveUnion) {
            uL = std::clamp(uL, 0, static_cast<int>(map.width));
            uT = std::clamp(uT, 0, static_cast<int>(map.height));
            uR = std::clamp(uR, uL, static_cast<int>(map.width));
            uB = std::clamp(uB, uT, static_cast<int>(map.height));
            if (uR > uL && uB > uT) {
                patchLeft = uL;
                patchTop = uT;
                const unsigned pw = static_cast<unsigned>(uR - uL);
                const unsigned ph = static_cast<unsigned>(uB - uT);
                basePatch = MapImage{pw, ph, std::vector<std::uint8_t>(std::size_t(pw) * ph * 4u, 0)};
                for (unsigned row = 0; row < ph; ++row) {
                    const auto srcBegin = map.rgba.begin() +
                        ((static_cast<std::size_t>(uT) + row) * map.width + static_cast<std::size_t>(uL)) * 4u;
                    std::copy_n(srcBegin, std::size_t(pw) * 4u,
                                basePatch.rgba.data() + std::size_t(row) * pw * 4u);
                }
            }
        }
    }

    SpriteCoverage coverage{};
    const View intact = stageViews[0].empty() ? record.subspan(2004) : stageViews[0];
    drawIndexedFrame(map, obj.x, obj.y, intact, palette, outBridge ? &coverage : nullptr);

    if (outBridge) {
        outBridge->x = obj.x;
        outBridge->y = obj.y;
        outBridge->u = obj.u;
        outBridge->v = obj.v;
        if (coverage.width > 0 && coverage.height > 0) {
            outBridge->left = coverage.left;
            outBridge->top = coverage.top;
            outBridge->width = coverage.width;
            outBridge->height = coverage.height;
        } else if (basePatch.width > 0 && basePatch.height > 0) {
            outBridge->left = patchLeft;
            outBridge->top = patchTop;
            outBridge->width = static_cast<int>(basePatch.width);
            outBridge->height = static_cast<int>(basePatch.height);
        } else {
            outBridge->left = obj.x - 64;
            outBridge->top = obj.y - 48;
            outBridge->width = 128;
            outBridge->height = 96;
        }
        outBridge->maxHp = 3500;
        outBridge->patchLeft = patchLeft;
        outBridge->patchTop = patchTop;
        outBridge->basePatch = std::move(basePatch);
        std::copy_n(palette.data(), 512u, outBridge->palette.data());
        for (int s = 0; s < 5; ++s) {
            const View sv = stageViews[static_cast<std::size_t>(s)];
            outBridge->stageFrames[static_cast<std::size_t>(s)].assign(sv.begin(), sv.end());
        }
        const int baseTx = (obj.u >> 5) - 9;
        const int baseTy = (obj.v >> 5) - 9;
        const auto maskGrid = record.subspan(516, 1444);
        for (int sy = 0; sy < 38; ++sy) {
            for (int sx = 0; sx < 38; ++sx) {
                const std::uint8_t m = maskGrid[static_cast<std::size_t>(sy) * 38u + static_cast<std::size_t>(sx)];
                if ((m & 0x05u) == 0u) continue;
                const int tx = baseTx + (sx >> 1);
                const int ty = baseTy + (sy >> 1);
                const int subX = tx * 2 + (sx & 1);
                const int subY = ty * 2 + (sy & 1);
                if (subX < 0 || subY < 0) continue;
                outBridge->cells.push_back({subX, subY, m});
            }
        }
    }
}

std::size_t renderSceneryObjects(MapImage& map, std::vector<Object>& objects, SceneryDepth* depth,
                                 std::vector<BuildingPlacement>* buildings = nullptr,
                                 std::vector<BridgePlacement>* bridges = nullptr) {
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
            BuildingPlacement bp{};
            drawHouse(map, obj.x, obj.y, record, depth, buildings ? &bp : nullptr);
            if (buildings) buildings->push_back(std::move(bp));
            ++drawn;
            continue;
        }
        if (a.layout == Layout::Bridge) {
            if (record.empty()) continue;
            BridgePlacement br{};
            drawBridge(map, obj, record, bridges ? &br : nullptr);
            if (bridges) bridges->push_back(std::move(br));
            ++drawn;
            continue;
        }
        if (a.layout == Layout::Stand || a.layout == Layout::Tree) {
            drawDepthBody(map, obj, record, depth);
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

void SceneryDepth::clearRect(int left, int top, int width, int height) {
    if (tiles_.empty() || width <= 0 || height <= 0) return;
    const int x0 = std::max(0, left), y0 = std::max(0, top);
    const int x1 = std::min(width_, left + width), y1 = std::min(height_, top + height);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            auto& tile = tiles_[std::size_t(y / side) * columns_ + x / side];
            if (tile) (*tile)[(y % side) * side + x % side] = 0;
        }
    }
}

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

std::size_t drawTutorialScenery(MapImage& map, const std::filesystem::path& gameDir,
                                SceneryDepth* depth, unsigned scheme,
                                std::vector<BuildingPlacement>* buildings,
                                std::vector<BridgePlacement>* bridges) {
    const auto path = resolveCaseInsensitivePath(gameDir, "maps.src/map.020/objects");
    if (std::filesystem::file_size(path) > (16u << 20)) throw std::runtime_error("Scenery list exceeds limit");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open tutorial scenery");
    const auto atlases = loadSceneryAtlases(gameDir, scheme);

    std::vector<Object> objects;
    for (std::string line; std::getline(input, line);) {
        std::istringstream row(line);
        std::string kind;
        int u{}, v{}, index{};
        if (!(row >> kind >> u >> v >> index) || u < 0 || v < 0 || u >= maxGridCoord || v >= maxGridCoord || index < 0)
            throw std::runtime_error("Invalid tutorial scenery placement");
        const auto atlas = atlases.find(kind);
        if (atlas != atlases.end())
            objects.push_back({&atlas->second, unsigned(index), int(map.width / 2) + u - v,
                               (u + v) / 2, kind == "trop" || kind == "lay" || kind == "vor", u, v});
    }
    return renderSceneryObjects(map, objects, depth, buildings, bridges);
}

std::size_t drawMissionScenery(MapImage& map, const std::filesystem::path& gameDir,
                               const std::filesystem::path& missionPath,
                               SceneryDepth* depth,
                               std::vector<BuildingPlacement>* buildings,
                               std::vector<BridgePlacement>* bridges) {
    const auto archive = loadMissionArchive(missionPath);
    const auto pLnames = archive.requireMapRecord("P-LNAMES");
    constexpr std::size_t kindSlots = 11;
    constexpr std::size_t kindSlotBytes = 16;
    if (pLnames.size() != kindSlots * kindSlotBytes)
        throw std::runtime_error("Invalid mission scenery kind table");
    std::array<std::string_view, kindSlots> kinds;
    for (std::size_t i = 0; i < kindSlots; ++i) {
        const auto slot = pLnames.subspan(i * kindSlotBytes, kindSlotBytes);
        const auto nul = std::find(slot.begin(), slot.end(), std::uint8_t{0});
        kinds[i] = std::string_view(reinterpret_cast<const char*>(slot.data()),
                                    static_cast<std::size_t>(nul - slot.begin()));
    }

    const auto pObjects = archive.requireMapRecord("P-OBJECTS");
    if ((pObjects.size() % 10u) != 0)
        throw std::runtime_error("Invalid mission scenery object table size");
    if (pObjects.empty()) return 0;

    const auto atlases = loadSceneryAtlases(gameDir, archive.scheme);
    const int originX = (archive.width == archive.height) ? int(map.width / 2) : int(archive.height * 32u);
    const int offU = int(archive.offsetX) * 32;
    const int offV = int(archive.offsetY) * 32;
    const int subMaxU = int(archive.width) * 32;
    const int subMaxV = int(archive.height) * 32;
    const bool cropped = archive.offsetX != 0 || archive.offsetY != 0 ||
                         archive.width != archive.baseWidth || archive.height != archive.baseHeight;

    std::vector<Object> objects;
    objects.reserve(pObjects.size() / 10u);
    for (std::size_t off = 0; off < pObjects.size(); off += 10u) {
        const int u = int(word16(pObjects, off));
        const int v = int(word16(pObjects, off + 2));
        const std::size_t kindIdx = pObjects[off + 4];
        const unsigned index = word16(pObjects, off + 8);
        if (u >= maxGridCoord || v >= maxGridCoord)
            throw std::runtime_error("Invalid mission scenery placement");
        if (kindIdx >= kinds.size())
            throw std::runtime_error("Invalid mission scenery kind index");
        const int su = u - offU;
        const int sv = v - offV;
        if (cropped && (su < -256 || sv < -256 || su >= subMaxU + 256 || sv >= subMaxV + 256))
            continue;
        const int px = originX + su - sv;
        const int py = (su + sv) / 2;
        if (py < 0 || py >= int(map.height) || py > 32766 || px < -512 || px >= int(map.width) + 512)
            continue;
        const auto kind = kinds[kindIdx];
        const auto atlas = atlases.find(kind);
        if (atlas != atlases.end()) {
            objects.push_back({&atlas->second, index, px, py,
                               kind == "trop" || kind == "lay" || kind == "vor", su, sv});
        }
    }
    return renderSceneryObjects(map, objects, depth, buildings, bridges);
}

CraterCatalog loadCraterCatalog(const std::filesystem::path& gameDir, unsigned scheme) {
    CraterCatalog out;
    if (gameDir.empty()) return out;
    try {
        const auto scheme1 = resolveCaseInsensitivePath(gameDir, "game_scheme1.sue");
        std::vector<std::filesystem::path> archives;
        if (scheme == 2) {
            archives = {resolveCaseInsensitivePath(gameDir, "game_scheme3.sue"), scheme1};
        } else {
            archives = {scheme1};
        }
        const auto vorRes = readSueEntryFallback(archives, "vor.res");
        const auto vorPl = readSueEntryFallback(archives, "vor.pl");
        if (vorRes.size() < 8u || vorPl.size() < 512u) return out;

        const View data(vorRes);
        const View pal(vorPl);
        const unsigned count = word32(data, 0);
        if (count == 0 || (std::uint64_t(count) + 2u) * 4u > data.size()) return out;

        for (unsigned i = 0; i < count; ++i) {
            const size_t start = word32(data, 4u + 4u * i);
            const size_t end = word32(data, 8u + 4u * i);
            if (end > data.size() || end <= start || (end - start) < 9u) continue;
            const auto rec = data.subspan(start, end - start);
            if (rec[8] != 0xa1u && rec[8] != 0xa2u) continue;
            const int ox = static_cast<std::int16_t>(word16(rec, 0));
            const int oy = static_cast<std::int16_t>(word16(rec, 2));
            const unsigned w = word16(rec, 4);
            const unsigned h = word16(rec, 6);
            if (w == 0 || h == 0 || w > 256 || h > 256) continue;

            MapImage img{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4u, 0)};
            drawIndexedFrame(img, -ox, -oy, rec, pal.first(512));
            for (std::size_t p = 0; p < img.rgba.size(); p += 4u) {
                const unsigned a = img.rgba[p + 3u];
                if (a > 0u && a < 255u) {
                    img.rgba[p] = static_cast<std::uint8_t>(std::min(255u, (unsigned(img.rgba[p]) * 255u) / a));
                    img.rgba[p + 1u] = static_cast<std::uint8_t>(std::min(255u, (unsigned(img.rgba[p + 1u]) * 255u) / a));
                    img.rgba[p + 2u] = static_cast<std::uint8_t>(std::min(255u, (unsigned(img.rgba[p + 2u]) * 255u) / a));
                }
            }
            if (w <= 70u) out.smallCraters.push_back(std::move(img));
            else out.largeCraters.push_back(std::move(img));
        }
    } catch (...) {
    }
    return out;
}
}
