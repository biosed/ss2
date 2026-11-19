#include "sim/tutorial_units.hpp"
#include <algorithm>
#include <stdexcept>

namespace ss2 {
namespace {
SubCell fromMap(int x, int y) {
    return InfantryPathGrid::fromWorld((2*y + x - 4096) / 2, (2*y - x + 4096) / 2);
}
}
TutorialUnits::TutorialUnits(InfantryPathGrid grid, std::span<const assets::Infantry> units,
                             std::span<const assets::Placement> vehicles) : grid_(std::move(grid)) {
    for (const auto& unit : units) {
        const auto cell = fromMap(unit.x, unit.y);
        soldiers_.push_back({cell, unit.owner, {}, 0, {}});
        grid_.setOccupied(cell, true);
    }
    for (const auto& unit : vehicles) {
        const auto cell = fromMap(unit.x, unit.y);
        grid_.setOccupied(cell, true);
        if (unit.owner == 0) playerVehicles_.push_back(cell);
    }
}

std::vector<std::uint8_t> TutorialUnits::directRoute(size_t index, int x, int y) const {
    if (index >= soldiers_.size() || x < 0 || x >= 8192 || y < 0 || y >= 4096) return {};
    const int u2 = 2*y + x - 4096, v2 = 2*y - x + 4096;
    if (u2 < 0 || v2 < 0 || u2 >= 8192 || v2 >= 8192) return {};
    const auto& soldier = soldiers_[index];
    if (soldier.owner != 0 || soldier.motion || soldier.cursor < soldier.route.size()) return {};
    const auto dest = fromMap(x, y);
    int tx = soldier.cell.x/2, ty = soldier.cell.y/2;
    if (std::max(std::abs(dest.x/2-tx), std::abs(dest.y/2-ty)) > 63) return {};
    // At each tile Gold takes the direction of the signs of the remaining deltas.
    // Conservatively exclude routes requiring detours, occupied tiles or arbitration.
    while (tx != dest.x/2 || ty != dest.y/2) {
        const int sx = (dest.x/2 > tx) - (dest.x/2 < tx);
        const int sy = (dest.y/2 > ty) - (dest.y/2 < ty);
        if (sx && sy && (!grid_.clearRouteTile(tx+sx,ty) || !grid_.clearRouteTile(tx,ty+sy))) return {};
        tx += sx;
        ty += sy;
        if (!grid_.clearRouteTile(tx, ty)) return {};
    }
    return grid_.findDirections(soldier.cell, dest);
}

bool TutorialUnits::canMove(size_t index, int x, int y) const {
    return !directRoute(index, x, y).empty();
}

bool TutorialUnits::order(size_t index, int x, int y) {
    auto route = directRoute(index, x, y);
    if (route.empty()) return false;
    auto& soldier = soldiers_[index];
    soldier.route = std::move(route);
    soldier.cursor = 0;
    return true;
}

bool TutorialUnits::startNext(Soldier& soldier) {
    if (soldier.cursor == soldier.route.size()) return false;
    auto direction = soldier.route[soldier.cursor];
    auto next = InfantryPathGrid::step(soldier.cell, direction);
    if (!grid_.walkable(next) || grid_.occupied(next)) {
        soldier.cursor = soldier.route.size();
        return false;
    }
    ++soldier.cursor;
    grid_.setOccupied(soldier.cell, false);
    grid_.setOccupied(next, true);
    soldier.cell = next;
    soldier.motion = InfantryStep{next.x / 2, next.y / 2, unsigned(next.x % 2 + 2*(next.y % 2)), direction};
    return true;
}

std::vector<SoldierPose> TutorialUnits::step() {
    std::vector<SoldierPose> changed;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& soldier = soldiers_[i];
        if (!soldier.motion) {
            if (!startNext(soldier)) continue;
        } else {
            soldier.motion->advance(false, randomState_);
            if (!soldier.motion->running) startNext(soldier);
        }
        const auto& motion = *soldier.motion;
        const auto [u,v] = motion.position();
        changed.push_back({i,4096+u-v,(u+v)/2,motion.direction,motion.progress,motion.running});
        if (!motion.running) soldier.motion.reset();
    }
    return changed;
}

unsigned TutorialUnits::countInZone(std::span<const std::uint8_t> zones, unsigned zone) const {
    if (zones.size() != 128*128) throw std::invalid_argument("Expected Tutorial I location grid");
    const auto inside = [&](SubCell cell) { return (zones[(cell.y / 2)*128 + cell.x / 2] & 63u) == zone; };
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) { return s.owner == 0 && inside(s.cell); })
                  + std::ranges::count_if(playerVehicles_, inside));
}
}
