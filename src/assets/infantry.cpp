#include "assets/infantry.hpp"
#include "assets/indexed.hpp"
#include "assets/infantry_scale.hpp"
#include "assets/hd_pack.hpp"
#include <cmath>
#include "assets/sprite.hpp"
#include <algorithm>
#include <map>
#include <set>
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

InfantryScene::InfantryScene(MapImage background, const std::filesystem::path& gameDir, std::span<const Placement> source,
                             bool originalSize, MapImage bakedHd, const std::filesystem::path& atlases, bool cleanGround,
                             SceneryDepth depth, bool gpuSprites)
    : resolution_(bakedHd.width ? 2 : 1), ground_(resolution_ == 2 ? std::move(bakedHd) : std::move(background)),
      depth_(std::move(depth)), gpuSprites_(gpuSprites) {
    if (resolution_ == 2 && (ground_.width != background.width*2 || ground_.height != background.height*2))
        throw std::runtime_error("HD pack dimensions do not match 2x Tutorial I map");
    if (!atlases.empty() && (resolution_ != 2 || !cleanGround))
        throw std::runtime_error("Painted infantry require a clean 2x ground layer");
    std::map<std::string, std::array<int,4>> bakedBounds;
    std::set<std::string> paintedTypes;
    const auto scaled = [originalSize](int edge) { return originalSize ? edge : int(std::lround(edge * 1.25)); };
    // Original UNITS definitions in desc_common.sue give the file stem for each ID.
    const std::map<std::string, std::string> names{
        {"aguner", "A-2CASQ.RS2"}, {"aofficer", "AOfice.rs2"}, {"cmedic", "ALRUSMED.RS2"},
        {"dflamer", "A-4.RS2"}, {"dguner", "DGuner.rs2"}, {"dofficer", "DOficer.rs2"},
        {"dsmguner", "DMGun.rs2"}, {"rsmguner", "RMGun.rs2"}};
    const auto archive = gameDir / "game_common.sue";
    std::vector<std::uint8_t> palette;
    // mapunits is the opening live-list order. Simulation, formation dispatch
    // and equal-distance picking use it. The paint pass sorts its visible units.
    for (const auto& unit : source) {
        if (!names.contains(unit.id)) continue;
        if (!poses_.contains(unit.id) && !originalSize && !atlases.empty()) {
            const auto path = atlases / (unit.id + ".png");
            if (std::filesystem::is_regular_file(path)) {
                poses_.emplace(unit.id, loadInfantryAtlas(path));
                paintedTypes.insert(unit.id);
            }
        }
        if (paintedTypes.contains(unit.id)) ++painted_;
        if (!poses_.contains(unit.id)) {
            if (palette.empty()) palette = readSueEntry(archive, "SOLD.PL");
            const auto file = readSueEntry(archive, names.at(unit.id));
            View rest = file;
            auto& set = poses_[unit.id];
            // Gold indexes its group table by (2*animationState + stance)*8 + facing.
            for (unsigned group = 0; group < 40; ++group) {
                const auto count = u32(rest, 0), size = u32(rest, 4);
                rest = rest.subspan(8);
                if (count > 256 || size < count*4 || size > rest.size())
                    throw std::runtime_error("Invalid RS2 group");
                auto payload = rest.first(size);
                rest = rest.subspan(size);
                if (group >= 8 && group < 32) continue;
                if (!count) throw std::runtime_error("Missing standing infantry pose");
                auto& frames = set[group < 8 ? group : group - 24];
                for (unsigned i = 0; i < count; ++i) {
                    const auto begin = u32(payload, i*4), end = i+1 == count ? size : u32(payload, (i+1)*4);
                    if (begin < count*4 || end < begin || end > size) throw std::runtime_error("Invalid RS2 frame");
                    const auto sprite = payload.subspan(begin, end-begin);
                    const int x = std::int16_t(u16(sprite,0)), y = std::int16_t(u16(sprite,2));
                    const int w = u16(sprite,4), h = u16(sprite,6);
                    if (!w || !h || w > 2048 || h > 2048) throw std::runtime_error("Invalid infantry frame dimensions");
                    if (group == 0 && i == 0) bakedBounds[unit.id] = {x,y,w,h};
                    MapImage decoded{unsigned(w), unsigned(h), std::vector<std::uint8_t>(size_t(w)*h*4)};
                    drawIndexedFrame(decoded, -x, -y, sprite, palette);
                    InfantryPose pose{scaled(x), scaled(y), scaled(x+w)-scaled(x), scaled(y+h)-scaled(y), {}};
                    if (originalSize && resolution_ == 1) pose.pixels = std::move(decoded);
                    else pose.pixels = resampleInfantry(decoded, pose.width*resolution_, pose.height*resolution_);
                    frames.push_back(std::move(pose));
                }
            }
        }
        units_.push_back({unit.id, unit.x, unit.y, unit.hp, unit.ammo, unit.group, unit.owner,
                          0,0,0,0, unsigned(unit.direction) & 7u});
        bounds(units_.back());
        if (resolution_ == 2 && !cleanGround) {
            const auto [x,y,w,h] = bakedBounds.at(unit.id);
            restoreFootprint(ground_, background, unit.x+x, unit.y+y, w, h);
        }
    }
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
}

std::vector<MapPatch> InfantryScene::setInspection(const std::set<std::size_t>& selected, Cursor cursor, int checkerPhase) {
    if (gpuSprites_) return {};
    if (selected == selected_ && cursor == cursor_ && checkerPhase == checkerPhase_) return {};
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < units_.size(); ++i) {
        const auto& u = units_[i];
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

void InfantryScene::paint(MapImage& target, int x, int y) const {
    if (gpuSprites_) return;
    std::vector<const Infantry*> visible;
    for (const auto& unit : units_)
        if (unit.hp > 0 && unit.visible &&
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

MapPatch InfantryScene::updateFog(std::span<const std::uint8_t> fog128x128) {
    if (fog128x128.size() != 128u * 128u) return {0, 0, {}};
    if (fog_.empty()) {
        fog_.assign(fog128x128.begin(), fog128x128.end());
        return repaint(0, 0, int(ground_.width), int(ground_.height));
    }
    int minX = int(ground_.width), minY = int(ground_.height), maxX = 0, maxY = 0;
    for (int ty = 0; ty < 128; ++ty) {
        for (int tx = 0; tx < 128; ++tx) {
            const std::size_t idx = std::size_t(ty) * 128u + std::size_t(tx);
            if ((fog_[idx] & 0x11u) == (fog128x128[idx] & 0x11u)) continue;
            fog_[idx] = fog128x128[idx];
            const int cx = 4096 + (tx - ty) * 32;
            const int cy = (tx + ty) * 16 + 16;
            minX = std::min(minX, (cx - 32) * resolution_);
            maxX = std::max(maxX, (cx + 32) * resolution_);
            minY = std::min(minY, (cy - 16) * resolution_);
            maxY = std::max(maxY, (cy + 16) * resolution_);
        }
    }
    if (maxX <= minX || maxY <= minY) return {0, 0, {}};
    return repaint(minX, minY, maxX, maxY);
}

MapPatch InfantryScene::update(size_t index, int x, int y, unsigned direction, unsigned progress, bool walking, int hp, int visible) {
    auto& unit = units_.at(index);
    auto next = unit;
    next.x = x;
    next.y = y;
    next.direction = direction;
    next.progress = progress;
    next.walking = walking;
    if (hp >= 0) next.hp = hp;
    if (visible >= 0) next.visible = visible != 0;
    bounds(next);
    const int left = std::min(unit.left,next.left)*resolution_, top = std::min(unit.top,next.top)*resolution_;
    const int right = std::max(unit.left+unit.width,next.left+next.width)*resolution_;
    const int bottom = std::max(unit.top+unit.height,next.top+next.height)*resolution_;
    unit = std::move(next);
    if (gpuSprites_) return {0, 0, {}};
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
        if (!fog_.empty()) {
            const int my = (int(row) + top) / resolution_;
            for (unsigned col = 0; col < patch.width; ++col) {
                const int mx = (int(col) + left) / resolution_;
                const int halfX = (mx - 4096) >> 1;
                const int tx = (my + halfX) >> 5;
                const int ty = (my - halfX) >> 5;
                unsigned scale = 112u; // unexplored shroud (~44%)
                if (tx >= 0 && tx < 128 && ty >= 0 && ty < 128) {
                    const std::uint8_t f = fog_[std::size_t(ty) * 128u + std::size_t(tx)];
                    if ((f & 0x01u) != 0) scale = 256u;      // visible
                    else if ((f & 0x10u) != 0) scale = 178u; // explored fog (~70%)
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
    paint(patch, left, top);
    return {left, top, std::move(patch)};
}
}
