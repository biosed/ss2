#include "assets/infantry.hpp"
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <map>
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

}

InfantryScene::InfantryScene(MapImage background, const std::filesystem::path& gameDir, std::span<const Placement> source)
    : ground_(std::move(background)) {
    // Original UNITS definitions in desc_common.sue give the file stem for each ID.
    const std::map<std::string, std::string> names{
        {"aguner", "A-2CASQ.RS2"}, {"aofficer", "AOfice.rs2"}, {"cmedic", "ALRUSMED.RS2"},
        {"dflamer", "A-4.RS2"}, {"dguner", "DGuner.rs2"}, {"dofficer", "DOficer.rs2"},
        {"dsmguner", "DMGun.rs2"}, {"rsmguner", "RMGun.rs2"}};
    const auto archive = gameDir / "game_common.sue";
    palette_ = readSueEntry(archive, "SOLD.PL");
    for (const auto& unit : source) {
        if (!names.contains(unit.id)) continue;
        if (!poses_.contains(unit.id)) {
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
                    frames.emplace_back(payload.begin()+begin, payload.begin()+end);
                }
            }
        }
        units_.push_back({unit.id, unit.x, unit.y, unit.hp, unit.ammo, unit.group, unit.owner,
                          0,0,0,0, unsigned(unit.direction) & 7u});
        bounds(units_.back());
    }
    std::stable_sort(units_.begin(), units_.end(), [](const auto& a, const auto& b) { return a.y < b.y; });
}

View InfantryScene::frame(const Infantry& unit) const {
    const auto& frames = poses_.at(unit.id).at((unit.walking ? 8 : 0) + unit.direction);
    return frames.at((frames.size() * unit.progress) >> 8);
}

void InfantryScene::bounds(Infantry& unit) {
    const auto sprite = frame(unit);
    unit.left = unit.x + std::int16_t(u16(sprite, 0));
    unit.top = unit.y + std::int16_t(u16(sprite, 2));
    unit.width = u16(sprite, 4);
    unit.height = u16(sprite, 6);
}

void InfantryScene::paint(MapImage& target, int x, int y) const {
    std::vector<const Infantry*> visible;
    for (const auto& unit : units_)
        if (unit.left < x + int(target.width) && unit.top < y + int(target.height) &&
            unit.left + unit.width > x && unit.top + unit.height > y) visible.push_back(&unit);
    std::stable_sort(visible.begin(), visible.end(), [](const auto* a, const auto* b) { return a->y < b->y; });
    for (const auto* unit : visible) drawIndexedFrame(target, unit->x-x, unit->y-y, frame(*unit), palette_);
}

MapImage InfantryScene::compose() const {
    auto result = ground_;
    paint(result, 0, 0);
    return result;
}

MapPatch InfantryScene::update(size_t index, int x, int y, unsigned direction, unsigned progress, bool walking) {
    auto& unit = units_.at(index);
    auto next = unit;
    next.x = x;
    next.y = y;
    next.direction = direction;
    next.progress = progress;
    next.walking = walking;
    bounds(next);
    const int left = std::clamp(std::min(unit.left, next.left), 0, int(ground_.width));
    const int top = std::clamp(std::min(unit.top, next.top), 0, int(ground_.height));
    const int right = std::clamp(std::max(unit.left+unit.width, next.left+next.width), left, int(ground_.width));
    const int bottom = std::clamp(std::max(unit.top+unit.height, next.top+next.height), top, int(ground_.height));
    MapImage patch{unsigned(right-left), unsigned(bottom-top), {}};
    patch.rgba.resize(std::size_t(patch.width)*patch.height*4);
    for (unsigned row = 0; row < patch.height; ++row) {
        const auto begin = ground_.rgba.begin() + ((row+top)*std::size_t(ground_.width)+left)*4;
        std::copy_n(begin, patch.width*4, patch.rgba.begin()+row*std::size_t(patch.width)*4);
    }
    unit = std::move(next);
    paint(patch, left, top);
    return {left, top, std::move(patch)};
}
}
