#include "assets/infantry.hpp"
#include "assets/indexed.hpp"
#include "assets/infantry_scale.hpp"
#include "assets/hd_pack.hpp"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include "assets/sprite.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

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

std::optional<Bytes> tryReadSue(std::span<const std::filesystem::path> archives, std::string_view entry) {
    return tryReadSueEntryChain(archives, entry);
}

std::string_view nextToken(std::string_view& line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
        line.remove_prefix(1);
    const auto end = line.find_first_of(" \t\r\n");
    const auto tok = (end == std::string_view::npos) ? line : line.substr(0, end);
    line = (end == std::string_view::npos) ? std::string_view{} : line.substr(end);
    return tok;
}

bool parseSignedInt(std::string_view tok, int& out) {
    if (tok.empty()) return false;
    const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), out);
    return ec == std::errc{} && ptr == tok.data() + tok.size();
}

// Retail n2Game_Dll.dll 0x100665d8..0x10066719 / 0x100675d0..0x100676f5 camouflage slot mapping:
// 0=german, 1=japanese, 2=british, 3=american, 4=russian, 5..15 = numeric SoldClr 1..11.
int parseCamouflageSlot(std::string_view tok) {
    if (tok.empty()) return -1;
    if (equalsIgnoreCaseAscii(tok, "german")) return 0;
    if (equalsIgnoreCaseAscii(tok, "japanese")) return 1;
    if (equalsIgnoreCaseAscii(tok, "british")) return 2;
    if (equalsIgnoreCaseAscii(tok, "american")) return 3;
    if (equalsIgnoreCaseAscii(tok, "russian")) return 4;
    int num = 0;
    if (parseSignedInt(tok, num) && num >= 1 && num <= 11) return 4 + num;
    return -1;
}

std::array<std::array<int, 3>, 16> loadSoldClrTable(std::span<const std::filesystem::path> descArchives, bool hasDesc) {
    std::array<std::array<int, 3>, 16> table{};
    for (auto& entry : table) entry = {-1, -1, -1};
    if (!hasDesc) return table;
    const auto bytes = tryReadSue(descArchives, "MISC\\MISC");
    if (!bytes) return table;
    std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    while (!text.empty()) {
        const auto eol = text.find_first_of("\r\n");
        std::string_view line = (eol == std::string_view::npos) ? text : text.substr(0, eol);
        text = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);
        const auto key = nextToken(line);
        if (!equalsIgnoreCaseAscii(key, "SoldClr")) continue;
        const auto slotTok = nextToken(line);
        int r = -1, g = -1, b = -1;
        if (!parseSignedInt(nextToken(line), r) ||
            !parseSignedInt(nextToken(line), g) ||
            !parseSignedInt(nextToken(line), b)) {
            continue;
        }
        const int slot = parseCamouflageSlot(slotTok);
        if (slot >= 0 && slot < 16) {
            table[static_cast<std::size_t>(slot)] = {r, g, b};
        }
    }
    return table;
}

struct UnitVisualMeta {
    std::string stem;
    int camoSlot{-1};
};

std::optional<UnitVisualMeta> resolveUnitVisualMeta(std::span<const std::filesystem::path> descArchives, bool hasDesc,
                                                    std::string_view unitId) {
    UnitVisualMeta meta{std::string(unitId), -1};
    if (hasDesc) {
        if (const auto bytes = tryReadSue(descArchives, "UNITS\\" + std::string(unitId))) {
            std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            while (!text.empty()) {
                const auto eol = text.find_first_of("\r\n");
                std::string_view line = (eol == std::string_view::npos) ? text : text.substr(0, eol);
                text = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);
                const auto key = nextToken(line);
                if (equalsIgnoreCaseAscii(key, "file")) {
                    const auto stem = nextToken(line);
                    if (stem.empty() || equalsIgnoreCaseAscii(stem, "nofile")) return std::nullopt;
                    meta.stem = std::string(stem);
                } else if (equalsIgnoreCaseAscii(key, "camouflage")) {
                    meta.camoSlot = parseCamouflageSlot(nextToken(line));
                }
            }
        }
    }
    return meta;
}

struct LoadedInfantryRs2 {
    Bytes rs2;
    int camoSlot{-1};
};

std::optional<LoadedInfantryRs2> tryLoadInfantryRs2(std::span<const std::filesystem::path> descArchives, bool hasDesc,
                                                    std::span<const std::filesystem::path> gameArchives,
                                                    std::string_view unitId) {
    const auto meta = resolveUnitVisualMeta(descArchives, hasDesc, unitId);
    if (!meta) return std::nullopt;
    auto rs2 = tryReadSue(gameArchives, meta->stem + ".RS2");
    if (!rs2) return std::nullopt;
    return LoadedInfantryRs2{std::move(*rs2), meta->camoSlot};
}

InfantryPoses decodeRs2Poses(View rest, View palette, bool originalSize, int resolution,
                             std::array<int, 4>* firstBounds = nullptr) {
    const auto scaled = [originalSize](int edge) { return originalSize ? edge : int(std::lround(edge * 1.25)); };
    InfantryPoses set;
    // Gold indexes its group table by (2*animationState + stance)*8 + facing.
    for (unsigned group = 0; group < 40; ++group) {
        const auto count = u32(rest, 0), size = u32(rest, 4);
        rest = rest.subspan(8);
        if (count > 256 || size < count * 4 || size > rest.size())
            throw std::runtime_error("Invalid RS2 group");
        auto payload = rest.first(size);
        rest = rest.subspan(size);
        if (group >= 8 && group < 32) continue;
        if (!count) {
            if (group < 8) throw std::runtime_error("Missing standing infantry pose");
            set[group - 24] = set[group - 32];
            continue;
        }
        auto& frames = set[group < 8 ? group : group - 24];
        for (unsigned i = 0; i < count; ++i) {
            const auto begin = u32(payload, i * 4);
            const auto end = i + 1 == count ? size : u32(payload, (i + 1) * 4);
            if (begin < count * 4 || end < begin || end > size)
                throw std::runtime_error("Invalid RS2 frame");
            const auto sprite = payload.subspan(begin, end - begin);
            const int x = std::int16_t(u16(sprite, 0)), y = std::int16_t(u16(sprite, 2));
            const int w = u16(sprite, 4), h = u16(sprite, 6);
            if (!w || !h || w > 2048 || h > 2048)
                throw std::runtime_error("Invalid infantry frame dimensions");
            if (firstBounds && group == 0 && i == 0) *firstBounds = {x, y, w, h};
            MapImage decoded{unsigned(w), unsigned(h), std::vector<std::uint8_t>(size_t(w) * h * 4)};
            drawIndexedFrame(decoded, -x, -y, sprite, palette);
            InfantryPose pose{scaled(x), scaled(y), scaled(x + w) - scaled(x), scaled(y + h) - scaled(y), {}};
            if (originalSize && resolution == 1) pose.pixels = std::move(decoded);
            else pose.pixels = resampleInfantry(decoded, pose.width * resolution, pose.height * resolution);
            frames.push_back(std::move(pose));
        }
    }
    return set;
}

// The v1 HD pack bakes the first standing frame of every soldier into its map.
// Replace just those footprints from the clean native ground before painting any live poses.
void restoreFootprint(MapImage& hd, const MapImage& ground, int x, int y, int w, int h) {
    // Scale2x reads the neighbors of each source pixel. Two adjacent soldiers
    // can change even a ground pixel just outside both original rectangles.
    --x;
    --y;
    w += 2;
    h += 2;
    const int left = std::max(0, x-1), top = std::max(0, y-1);
    const int right = std::min(int(ground.width), x+w+1), bottom = std::min(int(ground.height), y+h+1);
    if (right <= left || bottom <= top) return;
    MapImage patch{unsigned(right-left), unsigned(bottom-top), {}};
    patch.rgba.resize(size_t(patch.width)*patch.height*4);
    for (unsigned row = 0; row < patch.height; ++row)
        std::copy_n(ground.rgba.data() + ((top+row)*size_t(ground.width)+left)*4, patch.width*4,
                    patch.rgba.data()+row*size_t(patch.width)*4);
    const auto scaled = upscalePatch2x(patch);
    for (int row = std::max(0,y)*2; row < std::min(int(ground.height),y+h)*2; ++row) {
        const int start = std::max(0,x)*2, stop = std::min(int(ground.width),x+w)*2;
        if (start < stop)
            std::copy_n(scaled.rgba.data()+((row-2*top)*size_t(scaled.width)+start-2*left)*4, (stop-start)*4,
                        hd.rgba.data()+(row*size_t(hd.width)+start)*4);
    }
}

}

std::vector<std::uint8_t> buildSoldClrPalette(std::span<const std::uint8_t> baseSoldPl,
                                              int scaleR, int scaleG, int scaleB) {
    if (baseSoldPl.size() < 512u) throw std::runtime_error("Invalid SOLD.PL palette size");
    std::vector<std::uint8_t> out(baseSoldPl.begin(), baseSoldPl.begin() + 512);
    // Retail n2Game_Dll.dll 0x1007bc3d: scaleR == -1 copies SOLD.PL verbatim (e.g. German).
    if (scaleR < 0) return out;
    // Retail n2Game_Dll.dll 0x1007ba80..0x1007bbde: tint achromatic uniform entries, preserve skin/wood/fire.
    for (std::size_t i = 0; i < 256u; ++i) {
        const unsigned w = unsigned(baseSoldPl[2u * i]) | (unsigned(baseSoldPl[2u * i + 1u]) << 8);
        const int r = static_cast<int>((w >> 8) & 0xF8u);
        const int g = static_cast<int>((w >> 3) & 0xFCu);
        const int b = static_cast<int>((w & 0x1Fu) << 3);
        const int maxDiff = std::max({std::abs(r - b), std::abs(b - g), std::abs(r - g)});
        const int avg = (r + g + b) / 3;
        if (maxDiff > avg / 5) continue;
        const int nr = std::clamp((avg * scaleR) / 250, 0, 255);
        const int ng = std::clamp((avg * scaleG) / 250, 0, 255);
        const int nb = std::clamp((avg * scaleB) / 250, 0, 255);
        const auto nw = static_cast<std::uint16_t>(
            ((nr & 0xF8) << 8) | ((ng & 0xFC) << 3) | ((nb >> 3) & 0x1F));
        out[2u * i] = static_cast<std::uint8_t>(nw & 0xFFu);
        out[2u * i + 1u] = static_cast<std::uint8_t>((nw >> 8) & 0xFFu);
    }
    return out;
}

InfantryScene::InfantryScene(MapImage background, const std::filesystem::path& gameDir, std::span<const Placement> source,
                             bool originalSize, MapImage bakedHd, const std::filesystem::path& atlases, bool cleanGround,
                             SceneryDepth depth, const std::map<std::string, unsigned>* gpuSlots, unsigned fogW, unsigned fogH)
    : gameDir_(gameDir), originalSize_(originalSize), atlases_(atlases),
      resolution_(bakedHd.width ? 2 : 1), ground_(resolution_ == 2 ? std::move(bakedHd) : std::move(background)),
      depth_(std::move(depth)), fogW_(fogW), fogH_(fogH), gpuSlots_(gpuSlots) {
    if (!fogW_ || !fogH_ || fogW_ > 512u || fogH_ > 512u)
        throw std::runtime_error("Fog grid dimensions out of range");
    if (resolution_ == 2 && (ground_.width != background.width*2 || ground_.height != background.height*2))
        throw std::runtime_error("HD pack dimensions do not match 2x Tutorial I map");
    if (!atlases_.empty() && (resolution_ != 2 || !cleanGround))
        throw std::runtime_error("Painted infantry require a clean 2x ground layer");
    if (!gameDir_.empty()) {
        gameArchives_ = resolveGameArchives(gameDir_);
        descArchives_ = resolveDescArchives(gameDir_, 0);
        hasArchive_ = !gameArchives_.empty();
        hasDesc_ = !descArchives_.empty();
    }
    std::map<std::string, std::array<int,4>> bakedBounds;
    // mapunits is the opening live-list order. Simulation, formation dispatch
    // and equal-distance picking use it. The paint pass sorts its visible units.
    for (const auto& unit : source) {
        std::array<int, 4> firstBounds{};
        const bool hadPose = poses_.contains(unit.id);
        if (!ensureClassLoaded(unit.id, &firstBounds)) continue;
        if (!hadPose && !paintedTypes_.contains(unit.id)) {
            bakedBounds[unit.id] = firstBounds;
        }
        units_.push_back({unit.id, unit.x, unit.y, unit.hp, unit.ammo, unit.group, unit.owner,
                          0,0,0,0, unsigned(unit.direction) & 7u});
        units_.back().expa = unit.expa;
        bounds(units_.back());
        if (resolution_ == 2 && !cleanGround) {
            const auto [x,y,w,h] = bakedBounds.at(unit.id);
            restoreFootprint(ground_, background, unit.x+x, unit.y+y, w, h);
        }
    }
}

bool InfantryScene::ensureClassLoaded(const std::string& id, std::array<int, 4>* firstBounds) {
    if (skippedTypes_.contains(id)) return false;
    if (!poses_.contains(id)) {
        if (!originalSize_ && !atlases_.empty()) {
            const auto path = resolveCaseInsensitivePath(atlases_, id + ".png");
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec)) {
                poses_.emplace(id, loadInfantryAtlas(path));
                paintedTypes_.insert(id);
            }
        }
        if (!poses_.contains(id)) {
            if (!hasArchive_) {
                if (gameDir_.empty() && !atlases_.empty()) {
                    skippedTypes_.insert(id);
                    return false;
                }
                throw std::runtime_error("Cannot open SUE archive");
            }
            const auto loaded = tryLoadInfantryRs2(descArchives_, hasDesc_, gameArchives_, id);
            if (!loaded) {
                skippedTypes_.insert(id);
                return false;
            }
            if (palette_.empty()) palette_ = readSueEntryChain(gameArchives_, "SOLD.PL");
            if (!soldClrLoaded_) {
                soldClr_ = loadSoldClrTable(descArchives_, hasDesc_);
                soldClrLoaded_ = true;
            }
            View activePalette(palette_);
            if (loaded->camoSlot >= 0 && loaded->camoSlot < 16) {
                auto& cached = camoPalettes_[static_cast<std::size_t>(loaded->camoSlot)];
                if (cached.empty()) {
                    const auto [sr, sg, sb] = soldClr_[static_cast<std::size_t>(loaded->camoSlot)];
                    cached = buildSoldClrPalette(palette_, sr, sg, sb);
                }
                activePalette = View(cached);
            }
            std::array<int, 4> fb{};
            poses_.emplace(id, decodeRs2Poses(loaded->rs2, activePalette, originalSize_, resolution_, &fb));
            if (firstBounds) *firstBounds = fb;
        }
    }
    if (paintedTypes_.contains(id)) ++painted_;
    return true;
}

std::optional<std::pair<size_t, MapPatch>> InfantryScene::spawn(const Placement& p) {
    if (!ensureClassLoaded(p.id)) return std::nullopt;
    const size_t index = units_.size();
    units_.push_back({p.id, p.x, p.y, p.hp, p.ammo, p.group, p.owner,
                      0, 0, 0, 0, unsigned(p.direction) & 7u});
    units_.back().expa = p.expa;
    bounds(units_.back());
    if (gpuDrawn(p.id)) return std::make_pair(index, MapPatch{0, 0, {}});
    const auto& u = units_.back();
    return std::make_pair(index, repaint(u.left * resolution_, u.top * resolution_,
                                         (u.left + u.width) * resolution_,
                                         (u.top + u.height) * resolution_));
}

const InfantryPose& InfantryScene::frame(const Infantry& unit) const {
    const auto& frames = poses_.at(unit.id).at((unit.walking ? 8 : 0) + unit.direction);
    return frames.at((frames.size() * unit.progress) >> 8);
}

void InfantryScene::bounds(Infantry& unit) {
    const auto& pose = frame(unit);
    unit.left = unit.x + pose.left;
    unit.top = unit.y + pose.top;
    unit.width = pose.width;
    unit.height = pose.height;
}

namespace {
bool revealed(const Infantry& unit, std::size_t index, const std::set<std::size_t>& selected, InfantryScene::Cursor cursor) {
    if (selected.contains(index)) return true;
    if (!cursor) return false;
    const int dx = std::abs(unit.x-cursor->first), dy = 2*std::abs(unit.y-cursor->second);
    return std::max(dx,dy) + std::min(dx,dy)/2 < 256;
}

MapImage upscaleOverlay2xEpx(const MapImage& src) {
    const int w = int(src.width);
    const int h = int(src.height);
    const std::size_t dstW = std::size_t(src.width) * 2u;
    const std::size_t dstH = std::size_t(src.height) * 2u;
    MapImage dst{src.width * 2u, src.height * 2u, std::vector<std::uint8_t>(dstW * dstH * 4u, 0u)};
    const auto readPx = [&](int x, int y) -> std::array<std::uint8_t, 4> {
        if (x < 0 || y < 0 || x >= w || y >= h) return {0u, 0u, 0u, 0u};
        const auto* p = src.rgba.data() + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4u;
        if (p[3] == 0u) return {0u, 0u, 0u, 0u};
        return {p[0], p[1], p[2], p[3]};
    };
    const auto eq = [](const std::array<std::uint8_t, 4>& a, const std::array<std::uint8_t, 4>& b) -> bool {
        if (a[3] == 0u && b[3] == 0u) return true;
        return a == b;
    };
    const auto putPx = [&](std::size_t x, std::size_t y, const std::array<std::uint8_t, 4>& c) {
        auto* p = &dst.rgba[(y * dstW + x) * 4u];
        p[0] = c[0];
        p[1] = c[1];
        p[2] = c[2];
        p[3] = c[3];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto P = readPx(x, y);
            const auto A = readPx(x, y - 1);
            const auto B = readPx(x + 1, y);
            const auto C = readPx(x - 1, y);
            const auto D = readPx(x, y + 1);
            auto p00 = P, p10 = P, p01 = P, p11 = P;
            if (!eq(C, B) && !eq(A, D)) {
                if (eq(C, A)) p00 = A;
                if (eq(A, B)) p10 = B;
                if (eq(D, C)) p01 = C;
                if (eq(B, D)) p11 = D;
            }
            const std::size_t ox = std::size_t(x) * 2u;
            const std::size_t oy = std::size_t(y) * 2u;
            putPx(ox, oy, p00);
            putPx(ox + 1u, oy, p10);
            putPx(ox, oy + 1u, p01);
            putPx(ox + 1u, oy + 1u, p11);
        }
    }
    return dst;
}
}

std::vector<MapPatch> InfantryScene::setInspection(const std::set<std::size_t>& selected, Cursor cursor, int checkerPhase) {
    if (selected == selected_ && cursor == cursor_ && checkerPhase == checkerPhase_) return {};
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < units_.size(); ++i) {
        const auto& u = units_[i];
        if (gpuDrawn(u.id)) continue;
        const bool after = revealed(u,i,selected,cursor);
        if ((after != revealed(u,i,selected_,cursor_) || (after && checkerPhase != checkerPhase_)) &&
            depth_.intersects(u.left,u.top,u.width,u.height)) changed.push_back(i);
    }
    selected_ = selected;
    cursor_ = cursor;
    checkerPhase_ = checkerPhase;
    std::vector<MapPatch> result;
    result.reserve(changed.size());
    for (auto i : changed) {
        const auto& u = units_[i];
        result.push_back(repaint(u.left*resolution_,u.top*resolution_,
                                 (u.left+u.width)*resolution_,(u.top+u.height)*resolution_));
    }
    return result;
}

void InfantryScene::setVehicleLayer(VehicleLayer layer) {
    vehicleLayer_ = std::move(layer);
}

MapPatch InfantryScene::repaintRegion(int left, int top, int right, int bottom) const {
    return repaint(left, top, right, bottom);
}

MapPatch InfantryScene::blitGroundPatch(int x, int y, const MapImage& patch) {
    const int dstLeft = std::clamp(x * resolution_, 0, int(ground_.width));
    const int dstTop = std::clamp(y * resolution_, 0, int(ground_.height));
    const int dstRight = std::clamp((x + int(patch.width)) * resolution_, dstLeft, int(ground_.width));
    const int dstBottom = std::clamp((y + int(patch.height)) * resolution_, dstTop, int(ground_.height));
    for (int dy = dstTop; dy < dstBottom; ++dy) {
        const int sy = (dy / resolution_) - y;
        if (sy < 0 || sy >= int(patch.height)) continue;
        for (int dx = dstLeft; dx < dstRight; ++dx) {
            const int sx = (dx / resolution_) - x;
            if (sx < 0 || sx >= int(patch.width)) continue;
            const auto* src = patch.rgba.data() + (std::size_t(sy) * patch.width + static_cast<std::size_t>(sx)) * 4u;
            auto* dst = ground_.rgba.data() + (std::size_t(dy) * ground_.width + static_cast<std::size_t>(dx)) * 4u;
            std::copy_n(src, 4u, dst);
        }
    }
    return repaint(dstLeft, dstTop, dstRight, dstBottom);
}

MapPatch InfantryScene::stampGroundPatch(int x, int y, const MapImage& patch, bool clearDepth) {
    if (!patch.width || !patch.height || patch.rgba.size() < std::size_t(patch.width) * patch.height * 4u) {
        return {0, 0, {}};
    }
    if (clearDepth) depth_.clearRect(x, y, int(patch.width), int(patch.height));
    return blitGroundPatch(x, y, patch);
}

MapPatch InfantryScene::updateBuildingPatch(int patchLeft, int patchTop, const MapImage& patch, int sortY,
                                            std::span<const SpriteCoverage> newBodies,
                                            const SpriteCoverage* oldShadow,
                                            const SpriteCoverage* newShadow) {
    if (!patch.width || !patch.height || patch.rgba.size() < std::size_t(patch.width) * patch.height * 4u) {
        return {0, 0, {}};
    }
    depth_.clearBodyDepth(patchLeft, patchTop, int(patch.width), int(patch.height), sortY);
    if (oldShadow && oldShadow->width > 0 && oldShadow->height > 0) {
        depth_.clearShadow(*oldShadow);
    }
    if (sortY >= 0 && sortY <= 32766) {
        for (const auto& cov : newBodies) {
            if (cov.width > 0 && cov.height > 0) {
                depth_.addBody(cov, sortY);
            }
        }
    }
    if (newShadow && newShadow->width > 0 && newShadow->height > 0) {
        depth_.addShadow(*newShadow);
    }
    return blitGroundPatch(patchLeft, patchTop, patch);
}

MapImage InfantryScene::copyGroundRect(int x, int y, int width, int height) const {
    if (width <= 0 || height <= 0 || ground_.width == 0 || ground_.height == 0) return {};
    MapImage out{
        static_cast<unsigned>(width),
        static_cast<unsigned>(height),
        std::vector<std::uint8_t>(std::size_t(width) * static_cast<std::size_t>(height) * 4u, 0)
    };
    for (int dy = 0; dy < height; ++dy) {
        const int gy = (y + dy) * resolution_;
        if (gy < 0 || gy >= int(ground_.height)) continue;
        for (int dx = 0; dx < width; ++dx) {
            const int gx = (x + dx) * resolution_;
            if (gx < 0 || gx >= int(ground_.width)) continue;
            const auto* src = ground_.rgba.data() + (std::size_t(gy) * ground_.width + static_cast<std::size_t>(gx)) * 4u;
            auto* dst = out.rgba.data() + (std::size_t(dy) * out.width + static_cast<std::size_t>(dx)) * 4u;
            std::copy_n(src, 4u, dst);
        }
    }
    return out;
}

MapPatch InfantryScene::stampAlphaOverlay(int centerX, int centerY, const MapImage& sprite) {
    if (!sprite.width || !sprite.height ||
        sprite.rgba.size() < std::size_t(sprite.width) * sprite.height * 4u ||
        !ground_.width || !ground_.height) {
        return {0, 0, {}};
    }
    const int ox = centerX - int(sprite.width / 2);
    const int oy = centerY - int(sprite.height / 2);
    const int dstLeft = std::clamp(ox * resolution_, 0, int(ground_.width));
    const int dstTop = std::clamp(oy * resolution_, 0, int(ground_.height));
    const int dstRight = std::clamp((ox + int(sprite.width)) * resolution_, dstLeft, int(ground_.width));
    const int dstBottom = std::clamp((oy + int(sprite.height)) * resolution_, dstTop, int(ground_.height));
    if (dstLeft >= dstRight || dstTop >= dstBottom) return {0, 0, {}};
    if (resolution_ == 2) {
        const MapImage upscaled = upscaleOverlay2xEpx(sprite);
        const int uox = ox * 2;
        const int uoy = oy * 2;
        const float cx = 0.5f * float(upscaled.width - 1u);
        const float cy = 0.5f * float(upscaled.height - 1u);
        const float invRx = 1.0f / std::max(1.0f, 0.5f * float(upscaled.width));
        const float invRy = 1.0f / std::max(1.0f, 0.5f * float(upscaled.height));
        for (int dy = dstTop; dy < dstBottom; ++dy) {
            const int sy = dy - uoy;
            if (sy < 0 || sy >= int(upscaled.height)) continue;
            for (int dx = dstLeft; dx < dstRight; ++dx) {
                const int sx = dx - uox;
                if (sx < 0 || sx >= int(upscaled.width)) continue;
                const auto* src = upscaled.rgba.data() + (std::size_t(sy) * upscaled.width + static_cast<std::size_t>(sx)) * 4u;
                const unsigned a = src[3];
                if (a == 0u) continue;
                const float nd = std::hypot((float(sx) - cx) * invRx, (float(sy) - cy) * invRy);
                const float t = std::clamp(1.0f - nd, 0.0f, 1.0f);
                const float scorch = 1.0f - 0.08f * (t * t * (3.0f - 2.0f * t));
                const unsigned sr = static_cast<unsigned>(std::lround(float(src[0]) * scorch));
                const unsigned sg = static_cast<unsigned>(std::lround(float(src[1]) * scorch));
                const unsigned sb = static_cast<unsigned>(std::lround(float(src[2]) * scorch));
                auto* dst = ground_.rgba.data() + (std::size_t(dy) * ground_.width + static_cast<std::size_t>(dx)) * 4u;
                if (a == 255u) {
                    dst[0] = static_cast<std::uint8_t>(sr);
                    dst[1] = static_cast<std::uint8_t>(sg);
                    dst[2] = static_cast<std::uint8_t>(sb);
                    dst[3] = 255u;
                } else {
                    const unsigned invA = 255u - a;
                    dst[0] = static_cast<std::uint8_t>((sr * a + unsigned(dst[0]) * invA + 127u) / 255u);
                    dst[1] = static_cast<std::uint8_t>((sg * a + unsigned(dst[1]) * invA + 127u) / 255u);
                    dst[2] = static_cast<std::uint8_t>((sb * a + unsigned(dst[2]) * invA + 127u) / 255u);
                    dst[3] = 255u;
                }
            }
        }
        return repaint(dstLeft, dstTop, dstRight, dstBottom);
    }
    for (int dy = dstTop; dy < dstBottom; ++dy) {
        const int sy = (dy / resolution_) - oy;
        if (sy < 0 || sy >= int(sprite.height)) continue;
        for (int dx = dstLeft; dx < dstRight; ++dx) {
            const int sx = (dx / resolution_) - ox;
            if (sx < 0 || sx >= int(sprite.width)) continue;
            const auto* src = sprite.rgba.data() + (std::size_t(sy) * sprite.width + static_cast<std::size_t>(sx)) * 4u;
            const unsigned a = src[3];
            if (a == 0u) continue;
            auto* dst = ground_.rgba.data() + (std::size_t(dy) * ground_.width + static_cast<std::size_t>(dx)) * 4u;
            if (a == 255u) {
                std::copy_n(src, 4u, dst);
            } else {
                const unsigned invA = 255u - a;
                for (unsigned c = 0; c < 3u; ++c) {
                    dst[c] = static_cast<std::uint8_t>((unsigned(src[c]) * a + unsigned(dst[c]) * invA + 127u) / 255u);
                }
                dst[3] = 255u;
            }
        }
    }
    return repaint(dstLeft, dstTop, dstRight, dstBottom);
}

void InfantryScene::paint(MapImage& target, int x, int y) const {
    if (!target.width || !target.height) return;
    if (vehicleLayer_) vehicleLayer_(target, x, y, &depth_);
    std::vector<const Infantry*> visible;
    for (const auto& unit : units_)
        if (unit.hp > 0 && unit.visible && !gpuDrawn(unit.id) &&
            unit.left*resolution_ < x + int(target.width) && unit.top*resolution_ < y + int(target.height) &&
            (unit.left+unit.width)*resolution_ > x && (unit.top+unit.height)*resolution_ > y) visible.push_back(&unit);
    std::stable_sort(visible.begin(), visible.end(), [](const auto* a, const auto* b) { return a->y < b->y; });
    for (const auto* unit : visible) {
        const auto& raster = frame(*unit).pixels;
        const int left = unit->left*resolution_-x, top = unit->top*resolution_-y;
        const bool testDepth = depth_.intersects(unit->left,unit->top,unit->width,unit->height);
        const bool highlight = testDepth && revealed(*unit,unit-units_.data(),selected_,cursor_);
        for (int row = std::max(0,-top); row < std::min(int(raster.height),int(target.height)-top); ++row) {
            for (int col = std::max(0,-left); col < std::min(int(raster.width),int(target.width)-left); ++col) {
                const auto* src = raster.rgba.data()+(row*size_t(raster.width)+col)*4;
                if (!src[3]) continue;
                if (testDepth && depth_.occludes((x+left+col)/resolution_, (y+top+row)/resolution_,
                                                 unit->y,highlight,checkerPhase_)) continue;
                auto* dst = target.rgba.data()+((top+row)*size_t(target.width)+left+col)*4;
                const unsigned inv = 255-src[3];
                for (int channel=0; channel<4; ++channel)
                    dst[channel] = src[channel] + (dst[channel]*inv+127)/255;
            }
        }
    }
}

MapImage InfantryScene::compose() const {
    if (!fog_.empty()) return repaint(0, 0, int(ground_.width), int(ground_.height)).pixels;
    auto result = ground_;
    paint(result, 0, 0);
    return result;
}

void InfantryScene::setFogViewerOwner(int owner) {
    fogViewerOwner_ = std::clamp(owner, 0, 3);
}

MapPatch InfantryScene::updateFog(std::span<const std::uint8_t> fog) {
    const std::size_t expectedSize = std::size_t(fogW_) * std::size_t(fogH_);
    if (fog.size() != expectedSize) return {0, 0, {}};
    if (fog_.empty()) {
        fog_.assign(fog.begin(), fog.end());
        return repaint(0, 0, int(ground_.width), int(ground_.height));
    }
    const int originX = int(fogH_) * 32;
    const auto fogMask = static_cast<std::uint8_t>(0x11u << fogViewerOwner_);
    int minX = int(ground_.width), minY = int(ground_.height), maxX = 0, maxY = 0;
    for (unsigned ty = 0; ty < fogH_; ++ty) {
        for (unsigned tx = 0; tx < fogW_; ++tx) {
            const std::size_t idx = std::size_t(ty) * std::size_t(fogW_) + std::size_t(tx);
            const std::uint8_t prev = fog_[idx];
            fog_[idx] = fog[idx];
            if ((prev & fogMask) == (fog[idx] & fogMask)) continue;
            const int cx = originX + (int(tx) - int(ty)) * 32;
            const int cy = (int(tx) + int(ty)) * 16 + 16;
            minX = std::min(minX, (cx - 32) * resolution_);
            maxX = std::max(maxX, (cx + 32) * resolution_);
            minY = std::min(minY, (cy - 16) * resolution_);
            maxY = std::max(maxY, (cy + 16) * resolution_);
        }
    }
    if (maxX <= minX || maxY <= minY) return {0, 0, {}};
    return repaint(minX, minY, maxX, maxY);
}

MapPatch InfantryScene::update(size_t index, int x, int y, unsigned direction, unsigned progress, bool walking, int hp, int visible, int ammo) {
    auto& unit = units_.at(index);
    auto next = unit;
    next.x = x;
    next.y = y;
    next.direction = direction;
    next.progress = progress;
    next.walking = walking;
    if (hp >= 0) next.hp = hp;
    if (visible >= 0) next.visible = visible != 0;
    if (ammo >= 0) next.ammo = ammo;
    bounds(next);
    const int left = std::min(unit.left,next.left)*resolution_, top = std::min(unit.top,next.top)*resolution_;
    const int right = std::max(unit.left+unit.width,next.left+next.width)*resolution_;
    const int bottom = std::max(unit.top+unit.height,next.top+next.height)*resolution_;
    unit = std::move(next);
    // GPU units are overlay-only (paint/compose/spawn skip them) and slotted pre-spawn, so a move dirties no ground_ pixels.
    if (gpuDrawn(unit.id)) return {0, 0, {}};
    return repaint(left,top,right,bottom);
}

MapPatch InfantryScene::repaint(int left, int top, int right, int bottom) const {
    left = std::clamp(left,0,int(ground_.width));
    top = std::clamp(top,0,int(ground_.height));
    right = std::clamp(right,left,int(ground_.width));
    bottom = std::clamp(bottom,top,int(ground_.height));
    MapImage patch{unsigned(right-left), unsigned(bottom-top), {}};
    patch.rgba.resize(std::size_t(patch.width)*patch.height*4);
    for (unsigned row = 0; row < patch.height; ++row) {
        const auto begin = ground_.rgba.begin() + ((row+top)*std::size_t(ground_.width)+left)*4;
        auto* dst = patch.rgba.data() + row * std::size_t(patch.width) * 4;
        std::copy_n(begin, patch.width*4, dst);
    }
    paint(patch, left, top);
    if (!fog_.empty()) {
        const int originX = int(fogH_) * 32;
        for (unsigned row = 0; row < patch.height; ++row) {
            const int my = (int(row) + top) / resolution_;
            auto* dst = patch.rgba.data() + row * std::size_t(patch.width) * 4;
            for (unsigned col = 0; col < patch.width; ++col) {
                const int mx = (int(col) + left) / resolution_;
                const int halfX = (mx - originX) >> 1;
                const int tx = (my + halfX) >> 5;
                const int ty = (my - halfX) >> 5;
                unsigned scale = 112u; // unexplored shroud (~44%)
                if (tx >= 0 && tx < int(fogW_) && ty >= 0 && ty < int(fogH_)) {
                    const std::uint8_t f = fog_[std::size_t(ty) * std::size_t(fogW_) + std::size_t(tx)];
                    const auto curBit = static_cast<std::uint8_t>(1u << fogViewerOwner_);
                    const auto expBit = static_cast<std::uint8_t>(0x10u << fogViewerOwner_);
                    if ((f & curBit) != 0) scale = 256u;      // visible
                    else if ((f & expBit) != 0) scale = 178u; // explored fog (~70%)
                }
                if (scale < 256u) {
                    auto* p = dst + col * 4;
                    p[0] = std::uint8_t((unsigned(p[0]) * scale) >> 8);
                    p[1] = std::uint8_t((unsigned(p[1]) * scale) >> 8);
                    p[2] = std::uint8_t((unsigned(p[2]) * scale) >> 8);
                }
            }
        }
    }
    return {left, top, std::move(patch)};
}

std::vector<Infantry> loadMissionInfantry(const std::filesystem::path& gameDir,
                                          const std::filesystem::path& missionPath) {
    const auto placements = readMissionPlacements(missionPath);
    if (placements.empty()) return {};
    const auto gameArchives = resolveGameArchives(gameDir);
    if (gameArchives.empty())
        throw std::runtime_error("Cannot open SUE archive");
    const auto descArchives = resolveDescArchives(gameDir, 0);
    const bool hasDesc = !descArchives.empty();

    std::vector<std::uint8_t> palette;
    const auto soldClr = loadSoldClrTable(descArchives, hasDesc);
    std::array<std::vector<std::uint8_t>, 16> camoPalettes{};
    std::map<std::string, std::optional<InfantryPoses>> cache;
    std::vector<Infantry> units;
    for (const auto& unit : placements) {
        auto it = cache.find(unit.id);
        if (it == cache.end()) {
            std::optional<InfantryPoses> decoded;
            if (const auto loaded = tryLoadInfantryRs2(descArchives, hasDesc, gameArchives, unit.id)) {
                if (palette.empty()) palette = readSueEntryChain(gameArchives, "SOLD.PL");
                View activePalette(palette);
                if (loaded->camoSlot >= 0 && loaded->camoSlot < 16) {
                    auto& cached = camoPalettes[static_cast<std::size_t>(loaded->camoSlot)];
                    if (cached.empty()) {
                        const auto [sr, sg, sb] = soldClr[static_cast<std::size_t>(loaded->camoSlot)];
                        cached = buildSoldClrPalette(palette, sr, sg, sb);
                    }
                    activePalette = View(cached);
                }
                decoded = decodeRs2Poses(loaded->rs2, activePalette, false, 1);
            }
            it = cache.emplace(unit.id, std::move(decoded)).first;
        }
        if (!it->second) continue;
        const unsigned dir = unsigned(unit.direction) & 7u;
        const auto& pose = (*it->second)[dir].at(0);
        units.push_back({unit.id, unit.x, unit.y, unit.hp, unit.ammo, unit.group, unit.owner,
                         unit.x + pose.left, unit.y + pose.top, pose.width, pose.height, dir});
    }
    return units;
}

std::optional<MapImage> InfantryScene::renderUnitPortrait(const std::string& id, unsigned size) const {
    if (size == 0u || id.empty()) return std::nullopt;
    const auto it = poses_.find(id);
    if (it == poses_.end()) return std::nullopt;

    const InfantryPose* picked = nullptr;
    if (!it->second[3].empty() && it->second[3][0].pixels.width > 0 && it->second[3][0].pixels.height > 0) {
        picked = &it->second[3][0];
    } else {
        for (unsigned d = 0; d < 8u; ++d) {
            if (!it->second[d].empty() && it->second[d][0].pixels.width > 0 && it->second[d][0].pixels.height > 0) {
                picked = &it->second[d][0];
                break;
            }
        }
    }
    if (!picked) return std::nullopt;

    const auto& src = picked->pixels;
    int minX = static_cast<int>(src.width), minY = static_cast<int>(src.height), maxX = -1, maxY = -1;
    for (int y = 0; y < static_cast<int>(src.height); ++y) {
        for (int x = 0; x < static_cast<int>(src.width); ++x) {
            const auto a = src.rgba[(static_cast<std::size_t>(y) * src.width + static_cast<std::size_t>(x)) * 4u + 3u];
            if (a > 8u) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < minY) {
        minX = 0;
        minY = 0;
        maxX = static_cast<int>(src.width) - 1;
        maxY = static_cast<int>(src.height) - 1;
    }
    const int cw = maxX - minX + 1;
    const int ch = maxY - minY + 1;
    if (cw <= 0 || ch <= 0) return std::nullopt;

    MapImage out{size, size, std::vector<std::uint8_t>(static_cast<std::size_t>(size) * size * 4u, 255u)};
    const double half = static_cast<double>(size) * 0.5;
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            const double nx = (static_cast<double>(x) + 0.5 - half) / std::max(1.0, half);
            const double ny = (static_cast<double>(y) + 0.5 - half) / std::max(1.0, half);
            const double t = std::clamp(std::sqrt(nx * nx + ny * ny) * 0.72, 0.0, 1.0);
            const std::size_t di = (static_cast<std::size_t>(y) * size + x) * 4u;
            out.rgba[di + 0u] = static_cast<std::uint8_t>(std::lround(54.0 - 30.0 * t));
            out.rgba[di + 1u] = static_cast<std::uint8_t>(std::lround(62.0 - 33.0 * t));
            out.rgba[di + 2u] = static_cast<std::uint8_t>(std::lround(50.0 - 26.0 * t));
            out.rgba[di + 3u] = 255u;
        }
    }

    const unsigned pad = std::min(3u, size / 8u);
    const unsigned inner = std::max(4u, size > 2u * pad ? size - 2u * pad : size);
    const double scale = std::min(static_cast<double>(inner) / static_cast<double>(cw),
                                  static_cast<double>(inner) / static_cast<double>(ch));
    const unsigned dstW = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(cw) * scale)));
    const unsigned dstH = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(ch) * scale)));
    const int offX = (static_cast<int>(size) - static_cast<int>(dstW)) / 2;
    const int offY = (static_cast<int>(size) - static_cast<int>(dstH)) / 2;
    for (unsigned dy = 0; dy < dstH; ++dy) {
        const int py = offY + static_cast<int>(dy);
        if (py < 0 || py >= static_cast<int>(size)) continue;
        const int sy = minY + std::min<int>(static_cast<int>((dy * static_cast<unsigned>(ch)) / dstH), ch - 1);
        for (unsigned dx = 0; dx < dstW; ++dx) {
            const int px = offX + static_cast<int>(dx);
            if (px < 0 || px >= static_cast<int>(size)) continue;
            const int sx = minX + std::min<int>(static_cast<int>((dx * static_cast<unsigned>(cw)) / dstW), cw - 1);
            const std::size_t si = (static_cast<std::size_t>(sy) * src.width + static_cast<std::size_t>(sx)) * 4u;
            const unsigned sa = src.rgba[si + 3u];
            if (sa == 0u) continue;
            const std::size_t di = (static_cast<std::size_t>(py) * size + static_cast<unsigned>(px)) * 4u;
            if (sa == 255u) {
                out.rgba[di + 0u] = src.rgba[si + 0u];
                out.rgba[di + 1u] = src.rgba[si + 1u];
                out.rgba[di + 2u] = src.rgba[si + 2u];
            } else {
                const unsigned inv = 255u - sa;
                out.rgba[di + 0u] = static_cast<std::uint8_t>(std::min(255u, src.rgba[si + 0u] + (out.rgba[di + 0u] * inv) / 255u));
                out.rgba[di + 1u] = static_cast<std::uint8_t>(std::min(255u, src.rgba[si + 1u] + (out.rgba[di + 1u] * inv) / 255u));
                out.rgba[di + 2u] = static_cast<std::uint8_t>(std::min(255u, src.rgba[si + 2u] + (out.rgba[di + 2u] * inv) / 255u));
            }
            out.rgba[di + 3u] = 255u;
        }
    }
    return out;
}
}
