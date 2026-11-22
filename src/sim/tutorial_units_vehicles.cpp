#include "sim/tutorial_units.hpp"
#include "sim/tutorial_coordinates.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "sim/tutorial_units_internal.hpp"

namespace ss2 {
namespace {
constexpr double kVehicleArrivePx = 8.0;
constexpr std::array<int, 8> kTileDx{1, 1, 0, -1, -1, -1, 0, 1};
constexpr std::array<int, 8> kTileDy{0, 1, 1, 1, 0, -1, -1, -1};
unsigned dirDelta32(unsigned a, unsigned b) {
    return std::min((a - b) & 31u, (b - a) & 31u);
}
}
int TutorialUnits::scaleVehicleDamageForDifficulty(int targetOwner, int rawDamage) const {
    if (rawDamage <= 0 || targetOwner != 0) return rawDamage;
    if (difficulty_ == 1) return std::max(1, (rawDamage * 80) / 100);
    if (difficulty_ == 3) return std::max(1, (rawDamage * 115) / 100);
    return rawDamage;
}

bool TutorialUnits::isVehicleUnbuttoned(const Vehicle& v) const {
    return v.hasCrew && v.marchEnabled && v.hatchOpen && v.suppressionTicks == 0 &&
           (v.lastCombatTick == 0u || tick_ >= v.lastCombatTick + 125u);
}

void TutorialUnits::markVehicleCombat(Vehicle& veh) {
    const unsigned ct = tick_ > 0u ? tick_ : 1u;
    const bool wasButtoned = veh.lastCombatTick != 0u && tick_ < veh.lastCombatTick + 125u;
    veh.lastCombatTick = ct;
    if ((veh.marchSightBonus > 0 || (veh.marchEnabled && veh.hatchOpen)) && !wasButtoned) {
        fogDirty_ = true;
    }
}

void TutorialUnits::moveVehicleInternal(Vehicle& v, int mapX, int mapY) {
    if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy) return;
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    v.moveTarget = std::pair{clampedX, clampedY};
    v.waypoints.clear();
    v.attackTarget.reset();
    v.attackGround.reset();
    v.attackBuildingTarget.reset();
    v.chasingAttackTarget = false;
    v.reverseMove = false;
    v.facingTargetDir.reset();
    v.supportTargetVehicle.reset();
    v.supportTargetBuilding.reset();
    v.supportTargetSoldier.reset();
    v.escortVehicleTarget.reset();
    v.escortSoldierTarget.reset();
    v.patrolOrigin.reset();
    v.patrolDest.reset();
}

std::vector<std::pair<size_t, std::pair<int, int>>> TutorialUnits::vehicleFormationTargets(
        std::span<const size_t> eligibleIds, int mapX, int mapY) const {
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    if (eligibleIds.empty()) return {};
    if (eligibleIds.size() == 1u) {
        return {{eligibleIds[0], {clampedX, clampedY}}};
    }

    double sumX = 0.0, sumY = 0.0;
    for (const size_t id : eligibleIds) {
        sumX += vehicles_[id].fx;
        sumY += vehicles_[id].fy;
    }
    const double invN = 1.0 / static_cast<double>(eligibleIds.size());
    const double cx = sumX * invN;
    const double cy = sumY * invN;
    const double dx = static_cast<double>(clampedX) - cx;
    const double dy = static_cast<double>(clampedY) - cy;
    const double len = std::hypot(dx, dy);
    const double fx = (len >= 8.0) ? (dx / len) : 1.0;
    const double fy = (len >= 8.0) ? (dy / len) : 0.0;
    const double rx = -fy;
    const double ry = fx;

    const auto isWalkableMapPt = [&](int px, int py) {
        if (!mapBounds(px, py)) return false;
        const SubCell c = placementCell(px, py, originX_);
        return grid_.inBounds(c) && grid_.walkable(c);
    };

    const size_t n = eligibleIds.size();
    const size_t cols = std::min<size_t>(4u, n);
    std::vector<std::pair<int, int>> slots;
    slots.reserve(n);
    static constexpr std::array<std::pair<int, int>, 8> kProbeOffsets{{
        {32, 0}, {-32, 0}, {0, 24}, {0, -24}, {32, 24}, {-32, 24}, {32, -24}, {-32, -24}
    }};
    for (size_t k = 0; k < n; ++k) {
        const size_t row = k / cols;
        const size_t col = k % cols;
        const size_t rowCount = std::min(cols, n - row * cols);
        const double lat = (static_cast<double>(col) - 0.5 * static_cast<double>(rowCount - 1)) * 56.0;
        const double depth = -static_cast<double>(row) * 48.0;
        const int candX = std::clamp(static_cast<int>(std::lround(clampedX + rx * lat + fx * depth)),
                                     0, std::max(0, mapPixelW_ - 1));
        const int candY = std::clamp(static_cast<int>(std::lround(clampedY + ry * lat + fy * depth)),
                                     0, std::max(0, mapPixelH_ - 1));
        std::pair<int, int> chosen{clampedX, clampedY};
        if (isWalkableMapPt(candX, candY)) {
            chosen = {candX, candY};
        } else {
            for (const auto& [ox, oy] : kProbeOffsets) {
                const int px = std::clamp(candX + ox, 0, std::max(0, mapPixelW_ - 1));
                const int py = std::clamp(candY + oy, 0, std::max(0, mapPixelH_ - 1));
                if (isWalkableMapPt(px, py)) {
                    chosen = {px, py};
                    break;
                }
            }
        }
        slots.push_back(chosen);
    }

    std::vector<size_t> ordered(eligibleIds.begin(), eligibleIds.end());
    std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        const double pa = (vehicles_[a].fx - cx) * fx + (vehicles_[a].fy - cy) * fy;
        const double pb = (vehicles_[b].fx - cx) * fx + (vehicles_[b].fy - cy) * fy;
        if (std::abs(pa - pb) > 1e-6) return pa > pb;
        return a < b;
    });
    for (size_t rowStart = 0; rowStart < ordered.size(); rowStart += cols) {
        const size_t rowEnd = std::min(ordered.size(), rowStart + cols);
        std::sort(ordered.begin() + static_cast<std::ptrdiff_t>(rowStart),
                  ordered.begin() + static_cast<std::ptrdiff_t>(rowEnd),
                  [&](size_t a, size_t b) {
            const double la = (vehicles_[a].fx - cx) * rx + (vehicles_[a].fy - cy) * ry;
            const double lb = (vehicles_[b].fx - cx) * rx + (vehicles_[b].fy - cy) * ry;
            if (std::abs(la - lb) > 1e-6) return la < lb;
            return a < b;
        });
    }

    std::vector<std::pair<size_t, std::pair<int, int>>> out;
    out.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        out.emplace_back(ordered[k], slots[k]);
    }
    return out;
}

size_t TutorialUnits::moveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        v.pendingTowGun.reset();
        if (append && v.moveTarget) {
            v.queuedOrders.emplace_back(pt.first, pt.second);
        } else {
            v.queuedOrders.clear();
            v.attackMove = false;
            v.attackMoveEngaging = false;
            moveVehicleInternal(v, pt.first, pt.second);
        }
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::reverseMoveVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.canMoveBackward ||
            v.backSpeedPx <= 0.0 || v.towedBy)
            continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        moveVehicleInternal(v, pt.first, pt.second);
        v.reverseMove = true;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::faceVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || v.towedBy) continue;
        const double dx = static_cast<double>(clampedX) - v.fx;
        const double dy = static_cast<double>(clampedY) - v.fy;
        if (std::hypot(dx, dy) < 4.0) continue;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.pendingTowGun.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir = vehicleDirFromMapDelta(dx, dy);
        if (v.wasMoving) {
            v.wasMoving = false;
            emitVehiclePose(id);
        }
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::attackMoveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        v.pendingTowGun.reset();
        if (append && v.moveTarget) {
            v.queuedOrders.emplace_back(pt.first, pt.second);
            v.attackMove = true;
        } else {
            v.queuedOrders.clear();
            moveVehicleInternal(v, pt.first, pt.second);
            v.attackMove = true;
            v.attackMoveEngaging = false;
        }
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::patrolVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        const std::pair origin{v.x, v.y};
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        moveVehicleInternal(v, pt.first, pt.second);
        if (std::hypot(static_cast<double>(pt.first - origin.first),
                       static_cast<double>(pt.second - origin.second)) >= 16.0) {
            v.patrolOrigin = origin;
            v.patrolDest = pt;
        } else {
            v.patrolOrigin.reset();
            v.patrolDest.reset();
        }
        v.patrolToDest = true;
        v.attackMove = true;
        v.attackMoveEngaging = false;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::stopVehicles(std::span<const size_t> indices) {
    size_t stopped = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        const bool wasMoving = v.moveTarget.has_value() || v.wasMoving;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.pendingTowGun.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.wasMoving = false;
        if (wasMoving) emitVehiclePose(id);
        ++stopped;
    }
    return stopped;
}

std::optional<bool> TutorialUnits::toggleVehicleHoldFire(std::span<const size_t> indices) {
    bool anyValid = false;
    bool anyFree = false;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        anyValid = true;
        if (!v.holdFire) anyFree = true;
    }
    if (!anyValid) return std::nullopt;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        v.holdFire = anyFree;
    }
    return anyFree;
}

std::optional<bool> TutorialUnits::toggleMarchMode(std::span<const size_t> vehicleIds) {
    bool anyValid = false;
    bool anyClosed = false;
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || !v.marchEnabled) continue;
        anyValid = true;
        if (!v.hatchOpen) anyClosed = true;
    }
    if (!anyValid) return std::nullopt;
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || !v.marchEnabled) continue;
        v.hatchOpen = anyClosed;
        emitVehiclePose(id);
    }
    fogDirty_ = true;
    return anyClosed;
}

size_t TutorialUnits::attackWithVehicles(std::span<const size_t> indices, bool targetIsVehicle, size_t targetIdx) {
    const bool valid = targetIsVehicle ? isValidVehicleAttackTarget(targetIdx)
                                       : isValidAttackTarget(targetIdx, 0);
    if (!valid) return 0;
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.towedBy || !v.armed ||
            (v.ammo <= 0 && (!v.hasSecondary || v.ammo2 <= 0)))
            continue;
        v.pendingTowGun.reset();
        v.attackTarget = std::pair{targetIsVehicle, targetIdx};
        v.chasingAttackTarget = false;
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::attackVehicle(std::span<const size_t> indices, size_t targetVehicleIdx) {
    if (!isValidVehicleAttackTarget(targetVehicleIdx)) return 0;
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked ||
            (s.ammo <= 0 && (s.isAntiVehicle || s.isSniper || s.isFlamer || s.atGrenades <= 0)))
            continue;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackVehicleTarget = targetVehicleIdx;
        s.attackTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::isValidVehicleAttackTarget(size_t targetIdx) const {
    if (targetIdx >= vehicles_.size()) return false;
    const auto& v = vehicles_[targetIdx];
    return v.alive && v.hp > 0 && isHostile(0, v.owner) && v.visible;
}

size_t TutorialUnits::spawnVehicle(const assets::VehicleUnit& unit) {
    const size_t index = appendVehicle(unit);
    if (fog_) refreshSpotting(nullptr, true);
    emitVehiclePose(index);
    return index;
}

std::vector<VehiclePose> TutorialUnits::takeVehiclePoses() {
    return std::exchange(vehiclePoses_, {});
}

bool TutorialUnits::canBoardVehicle(size_t vehicleIdx, int viewerOwner) const {
    if (vehicleIdx >= vehicles_.size()) return false;
    const auto& v = vehicles_[vehicleIdx];
    return v.alive && v.hp > 0 && v.owner == viewerOwner &&
           int(v.passengers.size()) < v.passengerCapacity;
}

bool TutorialUnits::embarkImmediate(size_t soldierIdx, size_t vehicleIdx) {
    if (soldierIdx >= soldiers_.size() || vehicleIdx >= vehicles_.size()) return false;
    auto& s = soldiers_[soldierIdx];
    auto& v = vehicles_[vehicleIdx];
    if (s.hp <= 0 || s.embarked || !v.alive || v.hp <= 0 || s.owner != v.owner) return false;
    if (v.passengers.size() >= 44u) return false;

    const SubCell oldCell = s.cell;
    s.embarked = true;
    s.motion.reset();
    s.goal.reset();
    s.route.clear();
    s.cursor = 0;
    s.needsRoute = false;
    s.attackTarget.reset();
    s.attackVehicleTarget.reset();
    s.attackBuildingTarget.reset();
    s.attackGround.reset();
    s.autoTarget.reset();
    s.boardVehicleTarget.reset();
    s.healTarget.reset();
    s.escortVehicleTarget.reset();
    s.escortSoldierTarget.reset();
    s.disarmMineTarget.reset();
    s.binoculars = false;
    s.spottedMask = 0;
    s.cell = v.cell;
    if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) {
        grid_.setOccupied(oldCell, false);
    }
    v.passengers.push_back(soldierIdx);
    fogDirty_ = true;
    emitVehiclePose(vehicleIdx);
    return true;
}

size_t TutorialUnits::orderBoardVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    if (!canBoardVehicle(vehicleIdx, 0)) return 0;
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.boardVehicleTarget = vehicleIdx;
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::unloadVehicles(std::span<const size_t> vehicleIds, std::vector<SoldierPose>* outPoses) {
    size_t unloaded = 0;
    for (size_t vid : vehicleIds) {
        if (vid >= vehicles_.size()) continue;
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.passengers.empty()) continue;
        for (size_t sIdx : v.passengers) {
            if (sIdx >= soldiers_.size()) continue;
            auto& s = soldiers_[sIdx];
            if (s.hp <= 0) continue;
            const SubCell spot = findSpawnCell(grid_, v.cell);
            s.embarked = false;
            s.cell = spot;
            s.motion.reset();
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.boardVehicleTarget.reset();
            grid_.setOccupied(spot, true);
            if (outPoses) {
                const auto [u, w] = soldierWorld(s);
                pushOrUpdateSoldierPose(*outPoses, {
                    sIdx, originX_ + u - w, (u + w) / 2, s.direction, 0, false,
                    s.hp, true, s.owner, s.group, false, true
                });
            }
            ++unloaded;
        }
        v.passengers.clear();
        emitVehiclePose(vid);
        fogDirty_ = true;
    }
    if (unloaded > 0 && fog_) refreshSpotting(outPoses, true);
    if (outPoses) {
        for (auto& pose : *outPoses) {
            if (pose.index < soldiers_.size()) {
                pose.expa = soldiers_[pose.index].expa;
                pose.kills = soldiers_[pose.index].kills;
                pose.atGrenades = soldiers_[pose.index].atGrenades;
                pose.mineAmmo = soldiers_[pose.index].mineAmmo;
                pose.ammo = soldiers_[pose.index].ammo;
                pose.suppressed = soldiers_[pose.index].isSuppressed();
            }
        }
    }
    return unloaded;
}

bool TutorialUnits::canCaptureVehicle(size_t vehicleIdx, int viewerOwner) const {
    if (vehicleIdx >= vehicles_.size()) return false;
    const auto& v = vehicles_[vehicleIdx];
    return viewerOwner == 0 && v.alive && v.hp > 0 && v.visible && v.owner == 3;
}

size_t TutorialUnits::orderCaptureVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    if (!canCaptureVehicle(vehicleIdx, 0)) return 0;
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.captureVehicleTarget = vehicleIdx;
        s.boardVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        ++accepted;
    }
    return accepted;
}

std::vector<VehicleRangePreview> TutorialUnits::selectionRangePreviews(std::span<const size_t> vehicleIds,
                                                                         std::span<const size_t> soldierIds) const {
    std::vector<VehicleRangePreview> out;
    std::vector<size_t> seen;
    for (const size_t vid : vehicleIds) {
        if (vid >= vehicles_.size()) continue;
        if (std::ranges::find(seen, vid) != seen.end()) continue;
        seen.push_back(vid);
        const auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || !v.hasPrimary || v.range <= 0.0) continue;
        out.push_back({v.x, v.y, static_cast<float>(v.range), static_cast<float>(v.deadzone)});
        if (out.size() >= 4u) break;
    }
    if (out.size() < 4u && !soldierIds.empty()) {
        std::vector<size_t> seenSoldiers;
        for (const size_t sid : soldierIds) {
            if (sid >= soldiers_.size()) continue;
            if (std::ranges::find(seenSoldiers, sid) != seenSoldiers.end()) continue;
            seenSoldiers.push_back(sid);
            const auto& s = soldiers_[sid];
            if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding ||
                (!s.isMortar && !s.isFlamer)) {
                continue;
            }
            const int maxRange = scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa);
            const int minRange = scaleByExpa(s.desc.deadzoneBase, s.desc.deadzoneDelta, s.expa);
            if (maxRange <= 0) continue;
            const auto [su, sv] = soldierWorld(s);
            out.push_back({
                originX_ + su - sv, (su + sv) / 2,
                static_cast<float>(maxRange), static_cast<float>(minRange)
            });
            if (out.size() >= 4u) break;
        }
    }
    return out;
}

std::vector<assets::Placement> TutorialUnits::abandonVehicles(std::span<const size_t> vehicleIds,
                                                              std::vector<SoldierPose>* outPoses) {
    std::vector<assets::Placement> crewPlacements;
    for (size_t vid : vehicleIds) {
        if (vid >= vehicles_.size()) continue;
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew) continue;
        if (!v.passengers.empty()) {
            (void)unloadVehicles(std::array<size_t, 1>{vid}, outPoses);
        }
        if (v.towedVehicle) detachTowedGun(vid);
        else if (v.towedBy) detachTowedGun(*v.towedBy);

        std::string crewId = "dguner";
        if (!v.crewUnit.empty()) {
            crewId = v.crewUnit;
        } else if (const auto* spec = catalog_ ? catalog_->find(v.id) : nullptr) {
            if (spec->nation == "russian") crewId = "rsmguner";
            else if (spec->nation == "american" || spec->nation == "british") crewId = "aguner";
            else if (spec->nation == "japanese") crewId = "jguner";
        }
        v.hasCrew = false;
        v.crewHp = 0;
        v.owner = 3;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.patrolToDest = true;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.pendingTowGun.reset();
        v.chasingAttackTarget = false;
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.wasMoving = false;
        fogDirty_ = true;
        emitVehiclePose(vid);

        const int cx = std::clamp(v.x + 18, 16, std::max(16, mapPixelW_ - 16));
        const int cy = std::clamp(v.y + 10, 16, std::max(16, mapPixelH_ - 16));
        crewPlacements.push_back({crewId, cx, cy, 0, 100, 40, v.group, 0});
    }
    if (!crewPlacements.empty() && fog_) {
        refreshSpotting(outPoses, true);
    }
    return crewPlacements;
}

size_t TutorialUnits::orderAttackGroundVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.towedBy || !v.hasPrimary || v.ammo <= 0) continue;
        v.pendingTowGun.reset();
        v.attackGround = std::pair{clampedX, clampedY};
        v.attackTarget.reset();
        v.attackBuildingTarget.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.moveTarget.reset();
        v.waypoints.clear();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        ++accepted;
    }
    return accepted;
}

void TutorialUnits::detachTowedGun(size_t tractorIdx) {
    if (tractorIdx >= vehicles_.size()) return;
    auto& tr = vehicles_[tractorIdx];
    tr.pendingTowGun.reset();
    if (!tr.towedVehicle) return;
    const size_t gunIdx = *tr.towedVehicle;
    tr.towedVehicle.reset();
    if (gunIdx < vehicles_.size()) {
        vehicles_[gunIdx].towedBy.reset();
        emitVehiclePose(gunIdx);
    }
    emitVehiclePose(tractorIdx);
}

void TutorialUnits::destroyVehicle(size_t vehicleIdx, std::vector<SoldierPose>& changed) {
    if (vehicleIdx >= vehicles_.size()) return;
    auto& tv = vehicles_[vehicleIdx];
    if (tv.towedBy) {
        const size_t tractorIdx = *tv.towedBy;
        tv.towedBy.reset();
        if (tractorIdx < vehicles_.size()) {
            vehicles_[tractorIdx].towedVehicle.reset();
            emitVehiclePose(tractorIdx);
        }
    }
    if (tv.towedVehicle) detachTowedGun(vehicleIdx);
    tv.alive = false;
    tv.hp = 0;
    tv.moveTarget.reset();
    tv.waypoints.clear();
    tv.queuedOrders.clear();
    tv.pendingTowGun.reset();
    tv.attackMove = false;
    tv.attackMoveEngaging = false;
    tv.attackGround.reset();
    tv.attackTarget.reset();
    tv.attackBuildingTarget.reset();
    tv.chasingAttackTarget = false;
    tv.reverseMove = false;
    tv.facingTargetDir.reset();
    tv.supportTargetVehicle.reset();
    tv.supportTargetBuilding.reset();
    tv.supportTargetSoldier.reset();
    tv.escortVehicleTarget.reset();
    tv.escortSoldierTarget.reset();
    tv.muzzleRevealTicks = 0u;
    tv.lastCombatTick = 0u;
    tv.wasMoving = false;
    tv.spottedMask = 0;
    tv.suppressionTicks = 0;
    tv.wasSuppressed = false;
    const int cookDmg = tv.cookOffDamage;
    const double cookR = tv.cookOffRadius;
    const int cookX = tv.x;
    const int cookY = tv.y;
    const SubCell cookCell = tv.cell;
    if (!soldierAtCell(tv.cell) && !vehicleAtCell(tv.cell)) {
        grid_.setOccupied(tv.cell, false);
    }
    if (cookDmg > 0 && cookR > 0.0 && cookOffDepth_ < 4) {
        ++cookOffDepth_;
        for (size_t k = 0; k < soldiers_.size(); ++k) {
            auto& ss = soldiers_[k];
            if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding) continue;
            const auto [su, sv] = soldierWorld(ss);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            const double sd = std::hypot(static_cast<double>(sx - cookX), static_cast<double>(sy - cookY));
            if (sd > cookR) continue;
            const double falloff = 1.0 - 0.55 * (sd / cookR);
            const int sdmg = std::max(1, static_cast<int>(std::lround(static_cast<double>(cookDmg) * 0.55 * falloff)));
            ss.hp = std::max(0, ss.hp - sdmg);
            recordHit(ss.owner, ss.group);
            if (ss.hp > 0) {
                applySuppression(k, 26);
            } else {
                if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
                ss.motion.reset();
                ss.goal.reset();
                ss.route.clear();
                ss.cursor = 0;
                ss.queuedGoals.clear();
                ss.attackMove = false;
                ss.attackTarget.reset();
                ss.attackVehicleTarget.reset();
                ss.attackBuildingTarget.reset();
                ss.autoTarget.reset();
                ss.binoculars = false;
                ss.spottedMask = 0;
                fogDirty_ = true;
            }
            const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
            SoldierPose sp{k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis};
            sp.suppressed = (ss.hp > 0 && ss.isSuppressed());
            pushOrUpdateSoldierPose(changed, sp);
        }
        for (size_t k = 0; k < vehicles_.size(); ++k) {
            if (k == vehicleIdx) continue;
            auto& tvOther = vehicles_[k];
            if (!tvOther.alive || tvOther.hp <= 0) continue;
            const double vd = std::hypot(tvOther.fx - static_cast<double>(cookX), tvOther.fy - static_cast<double>(cookY));
            if (vd > cookR) continue;
            const double falloff = 1.0 - 0.55 * (vd / cookR);
            const int rawSplash = std::max(1, static_cast<int>(std::lround(static_cast<double>(cookDmg) * falloff)));
            const int dmg = scaleVehicleDamageForDifficulty(
                tvOther.owner, applyVehicleDamage(tvOther.x, tvOther.y, tvOther.hullDir, tvOther.armor,
                                                    tvOther.hasCatalogArmor, cookX, cookY, rawSplash,
                                                    assets::HitType::Explosive));
            const int prevOwner = tvOther.owner;
            const int prevCrewHp = tvOther.crewHp;
            const bool wasAlive = tvOther.alive && tvOther.hp > 0;
            if (dmg > 0) {
                tvOther.hp = std::max(0, tvOther.hp - dmg);
            }
            if (wasAlive && tvOther.hp == 0) {
                destroyVehicle(k, changed);
            } else if (tvOther.hp > 0) {
                applyVehicleCrewDamage(k, cookX, cookY, rawSplash, assets::HitType::Explosive, changed, -1, -1);
                applyVehicleSuppression(k, 22, cookX, cookY);
            }
            if (dmg > 0 || tvOther.crewHp != prevCrewHp) {
                recordHit(prevOwner, tvOther.group);
                markVehicleCombat(tvOther);
                if (tvOther.hasCrew) {
                    reactToAttack(prevOwner, tvOther.group, cookX, cookY, k);
                }
                emitVehiclePose(k);
            } else if (tvOther.alive && tvOther.hp > 0 && tvOther.isSuppressed()) {
                emitVehiclePose(k);
            }
        }
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            const auto& tb = buildings_[bi];
            if (tb.destroyed) continue;
            const double bd = std::max(0.0, std::hypot(double(tb.x - cookX), double(tb.y - cookY)) -
                                            std::min(tb.width, tb.height) * 0.3);
            if (bd <= cookR + 12.0) {
                damageBuilding(bi, std::max(30, cookDmg / 2), assets::HitType::Explosive, -1, changed);
            }
        }
        if (!mines_.empty()) {
            triggerMineAtTile(cookCell.x / 2, cookCell.y / 2, changed, true);
        }
        --cookOffDepth_;
    }
    bailoutPassengers(vehicleIdx, changed);
    fogDirty_ = true;
}

bool TutorialUnits::canTowVehicle(size_t tractorIdx, size_t gunIdx, int viewerOwner) const {
    if (tractorIdx >= vehicles_.size() || gunIdx >= vehicles_.size() || tractorIdx == gunIdx) return false;
    const auto& tr = vehicles_[tractorIdx];
    const auto& gun = vehicles_[gunIdx];
    if (!tr.alive || tr.hp <= 0 || !gun.alive || gun.hp <= 0) return false;
    if (tr.owner != viewerOwner || isHostile(tr.owner, gun.owner)) return false;
    if (tr.towedVehicle || tr.towedBy || gun.towedVehicle || gun.towedBy) return false;
    if (assets::UnitSpec::towClassIndex(gun.towWeightClass) < 0) return false;
    return tr.towSpeedByClass[static_cast<std::size_t>(assets::UnitSpec::towClassIndex(gun.towWeightClass))] > 0.0;
}

size_t TutorialUnits::orderTowVehicle(size_t tractorIdx, size_t gunIdx) {
    if (tractorIdx >= vehicles_.size() || !canTowVehicle(tractorIdx, gunIdx, vehicles_[tractorIdx].owner)) return 0;
    auto& tr = vehicles_[tractorIdx];
    auto& gun = vehicles_[gunIdx];
    tr.attackTarget.reset();
    tr.attackGround.reset();
    tr.attackBuildingTarget.reset();
    tr.attackMove = false;
    tr.attackMoveEngaging = false;
    tr.queuedOrders.clear();
    tr.patrolOrigin.reset();
    tr.patrolDest.reset();
    tr.facingTargetDir.reset();
    tr.supportTargetVehicle.reset();
    tr.supportTargetBuilding.reset();
    tr.supportTargetSoldier.reset();
    tr.escortVehicleTarget.reset();
    tr.escortSoldierTarget.reset();
    const double dist = std::hypot(gun.fx - tr.fx, gun.fy - tr.fy);
    if (dist <= 68.0) {
        tr.pendingTowGun.reset();
        tr.moveTarget.reset();
        tr.waypoints.clear();
        tr.wasMoving = false;
        gun.moveTarget.reset();
        gun.waypoints.clear();
        gun.queuedOrders.clear();
        gun.attackTarget.reset();
        gun.attackGround.reset();
        gun.attackBuildingTarget.reset();
        gun.attackMove = false;
        gun.wasMoving = false;
        tr.towedVehicle = gunIdx;
        gun.towedBy = tractorIdx;
        emitVehiclePose(tractorIdx);
        emitVehiclePose(gunIdx);
        return 1;
    }
    tr.pendingTowGun = gunIdx;
    moveVehicleInternal(tr, gun.x, gun.y);
    return 1;
}

size_t TutorialUnits::unhitchVehicles(std::span<const size_t> vehicleIndices) {
    size_t unhitched = 0;
    for (size_t vid : vehicleIndices) {
        if (vid >= vehicles_.size()) continue;
        const auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        if (v.towedVehicle) {
            detachTowedGun(vid);
            ++unhitched;
        } else if (v.towedBy) {
            detachTowedGun(*v.towedBy);
            ++unhitched;
        }
    }
    return unhitched;
}

void TutorialUnits::stepTowing() {
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& tr = vehicles_[i];
        if (!tr.alive || tr.hp <= 0) {
            if (tr.towedVehicle) detachTowedGun(i);
            tr.pendingTowGun.reset();
            continue;
        }
        if (tr.towedVehicle) {
            const size_t gIdx = *tr.towedVehicle;
            if (gIdx >= vehicles_.size() || !vehicles_[gIdx].alive || vehicles_[gIdx].hp <= 0) {
                detachTowedGun(i);
            }
        }
        if (!tr.pendingTowGun) continue;
        const size_t gIdx = *tr.pendingTowGun;
        if (!canTowVehicle(i, gIdx, tr.owner)) {
            tr.pendingTowGun.reset();
            continue;
        }
        auto& gun = vehicles_[gIdx];
        if (std::hypot(gun.fx - tr.fx, gun.fy - tr.fy) <= 68.0) {
            tr.pendingTowGun.reset();
            tr.moveTarget.reset();
            tr.waypoints.clear();
            tr.wasMoving = false;
            gun.moveTarget.reset();
            gun.waypoints.clear();
            gun.queuedOrders.clear();
            gun.attackTarget.reset();
            gun.attackGround.reset();
            gun.attackBuildingTarget.reset();
            gun.attackMove = false;
            gun.wasMoving = false;
            tr.towedVehicle = gIdx;
            gun.towedBy = i;
            emitVehiclePose(i);
            emitVehiclePose(gIdx);
        } else if (!tr.moveTarget) {
            moveVehicleInternal(tr, gun.x, gun.y);
            if (!tr.moveTarget) tr.pendingTowGun.reset();
        }
    }
}

void TutorialUnits::emitVehiclePose(size_t index, bool fired, int targetX, int targetY,
                                    bool firedSecondary, std::string_view fireSound,
                                    bool heavyImpact, bool hitVehicle,
                                    bool repaired, bool resupplied, bool captured,
                                    bool firedRocket, bool firedFlamer, bool decrewed) {
    if (index >= vehicles_.size()) return;
    const auto& v = vehicles_[index];
    VehiclePose pose{
        index,
        v.x,
        v.y,
        v.hullDir,
        v.turretDir,
        v.hp,
        v.ammo,
        v.alive,
        v.visible,
        v.moveTarget.has_value(),
        fired,
        targetX,
        targetY,
        v.owner,
        v.group,
        v.maxHp,
        firedSecondary,
        fired ? std::string(fireSound) : std::string{},
        static_cast<int>(v.passengers.size()),
        v.passengerCapacity,
        heavyImpact,
        hitVehicle,
        repaired,
        resupplied,
        captured,
        v.towedVehicle ? int(*v.towedVehicle) : -1,
        v.towedBy ? int(*v.towedBy) : -1,
        v.expa,
        v.kills,
        firedRocket,
        firedFlamer,
        v.ammo2,
        decrewed,
        v.isCrewImpaired(),
        v.isMoveImpaired(),
        v.isSuppressed(),
        static_cast<std::uint8_t>((fired && !firedSecondary && heavyImpact) ? std::clamp(v.craterScale1, 0, 15) : 0),
        isVehicleUnbuttoned(v),
    };
    const auto it = std::ranges::find(vehiclePoses_, index, &VehiclePose::index);
    if (it == vehiclePoses_.end()) {
        vehiclePoses_.push_back(pose);
        return;
    }
    const bool wasFired = it->fired;
    const int oldTx = it->targetX;
    const int oldTy = it->targetY;
    const bool oldSec = it->firedSecondary;
    const bool oldHeavy = it->heavyImpact;
    const bool oldHitVeh = it->hitVehicle;
    const bool oldRocket = it->firedRocket;
    const bool oldFlamer = it->firedFlamer;
    const std::uint8_t oldCrater = it->craterScale;
    const bool wasRepaired = it->repaired;
    const bool wasResupplied = it->resupplied;
    const bool wasCaptured = it->captured;
    const bool wasDecrewed = it->decrewed;
    std::string oldSnd = std::move(it->fireSound);
    *it = pose;
    if (wasFired && !fired) {
        it->fired = true;
        it->targetX = oldTx;
        it->targetY = oldTy;
        it->firedSecondary = oldSec;
        it->heavyImpact = oldHeavy;
        it->hitVehicle = oldHitVeh;
        it->firedRocket = oldRocket;
        it->firedFlamer = oldFlamer;
        it->craterScale = oldCrater;
        it->fireSound = std::move(oldSnd);
    }
    it->repaired = it->repaired || wasRepaired;
    it->resupplied = it->resupplied || wasResupplied;
    it->captured = it->captured || wasCaptured;
    it->decrewed = it->decrewed || wasDecrewed;
}

bool TutorialUnits::canVehicleStepTo(const Vehicle& v, double nextFx, double nextFy, SubCell& outCell) const {
    const int ix = static_cast<int>(std::lround(nextFx));
    const int iy = static_cast<int>(std::lround(nextFy));
    if (!mapBounds(ix, iy)) return false;
    outCell = placementCell(ix, iy, originX_);
    if (outCell == v.cell) return true;
    if (!grid_.inBounds(v.cell)) {
        return outCell.x >= 0 && outCell.y >= 0 && outCell.x < grid_.width() && outCell.y < grid_.height();
    }
    return grid_.inBounds(outCell) && grid_.walkableMask(outCell, v.forbiddenTerrainMask);
}

bool TutorialUnits::isVehicleTargetValidFor(int attackerOwner, bool targetIsVehicle, size_t targetIdx) const {
    if (targetIsVehicle) {
        if (targetIdx >= vehicles_.size()) return false;
        const auto& tv = vehicles_[targetIdx];
        if (!tv.alive || tv.hp <= 0 || !isHostile(attackerOwner, tv.owner)) return false;
        if (!fog_) return tv.visible;
        return attackerOwner == 0 ? tv.visible : (tv.spottedMask & (1u << attackerOwner)) != 0;
    }
    if (targetIdx >= soldiers_.size()) return false;
    const auto& ts = soldiers_[targetIdx];
    if (ts.hp <= 0 || ts.embarked || !isHostile(attackerOwner, ts.owner)) return false;
    return !fog_ || (ts.spottedMask & (1u << attackerOwner)) != 0;
}

void TutorialUnits::bailoutPassengers(size_t vehicleIdx, std::vector<SoldierPose>& changed) {
    if (vehicleIdx >= vehicles_.size()) return;
    auto& tv = vehicles_[vehicleIdx];
    if (tv.passengers.empty()) return;
    for (size_t sIdx : tv.passengers) {
        if (sIdx >= soldiers_.size()) continue;
        auto& ps = soldiers_[sIdx];
        if (ps.hp <= 0) continue;
        const SubCell spot = findSpawnCell(grid_, tv.cell);
        ps.embarked = false;
        ps.cell = spot;
        ps.hp = std::max(1, (ps.hp * 60) / 100);
        ps.motion.reset();
        ps.goal.reset();
        ps.route.clear();
        ps.cursor = 0;
        ps.needsRoute = false;
        ps.boardVehicleTarget.reset();
        grid_.setOccupied(spot, true);
        const auto [pu, pv] = soldierWorld(ps);
        const bool pVis = !fog_ || ps.owner == 0 || (ps.spottedMask & 1u) != 0;
        pushOrUpdateSoldierPose(changed, {
            sIdx, originX_ + pu - pv, (pu + pv) / 2, ps.direction, 0, false,
            ps.hp, true, ps.owner, ps.group, false, pVis
        });
    }
    tv.passengers.clear();
}

void TutorialUnits::stepBoarding(std::vector<SoldierPose>& changed) {
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || !s.boardVehicleTarget) continue;
        const size_t vid = *s.boardVehicleTarget;
        if (!canBoardVehicle(vid, s.owner)) {
            s.boardVehicleTarget.reset();
            continue;
        }
        const auto& v = vehicles_[vid];
        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double dist = std::hypot(double(v.x - sx), double(v.y - sy));
        if (dist <= 56.0) {
            if (embarkImmediate(i, vid)) {
                pushOrUpdateSoldierPose(changed, {
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false
                });
            }
            continue;
        }
        if (!s.goal || !sameTile(*s.goal, v.cell)) {
            s.goal = v.cell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
    }
}

void TutorialUnits::stepCaptures(std::vector<SoldierPose>& changed) {
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || !s.captureVehicleTarget) continue;
        const size_t vid = *s.captureVehicleTarget;
        if (!canCaptureVehicle(vid, s.owner)) {
            s.captureVehicleTarget.reset();
            continue;
        }
        auto& v = vehicles_[vid];
        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        if (std::hypot(double(v.x - sx), double(v.y - sy)) <= 56.0) {
            v.owner = s.owner;
            v.hasCrew = true;
            const bool needsQualifiedCrew = v.hasCrewProtection && v.attackCrew >= 2;
            if (s.canBeCrew > 0 || !needsQualifiedCrew) {
                v.crewHp = v.maxCrewHp;
                if (s.canBeCrew > 0 && s.expa > v.expa) {
                    v.expa = s.expa;
                }
            } else {
                const int skeletonCrew = std::max(1, v.attackCrew - 1);
                v.crewHp = std::max(1, (v.maxCrewHp * skeletonCrew) / std::max(1, v.crewNumber));
            }
            s.captureVehicleTarget.reset();
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            fogDirty_ = true;
            if (fog_) refreshSpotting(&changed, false);
            emitVehiclePose(vid, false, 0, 0, false, {}, false, false, false, false, true);
            continue;
        }
        if (!s.goal || !sameTile(*s.goal, v.cell)) {
            s.goal = v.cell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
    }
}

void TutorialUnits::stepVehicles(std::vector<SoldierPose>& changed) {
    static constexpr std::array<double, 3> kSubtleSteer{{0.0, 0.55, -0.55}};
    const auto stepFacing = [&](size_t idx, Vehicle& veh) {
        if (!veh.facingTargetDir) return;
        const unsigned targetDir = *veh.facingTargetDir & 31u;
        const unsigned turnStep = std::max(1u, veh.turnStep32);
        const unsigned prevHull = veh.hullDir;
        const unsigned prevTurret = veh.turretDir;
        veh.hullDir = turnTowardDir32(veh.hullDir, targetDir, turnStep);
        if (!veh.attackTarget) {
            veh.turretDir = turnTowardDir32(veh.turretDir, targetDir, 2u);
        }
        if (veh.hullDir == targetDir && (veh.attackTarget || veh.turretDir == targetDir)) {
            veh.facingTargetDir.reset();
        }
        if (veh.hullDir != prevHull || veh.turretDir != prevTurret) {
            emitVehiclePose(idx);
        }
    };
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || !v.hasCrew) continue;
        if (v.towedBy) {
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.moveTarget || v.wasMoving) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.queuedOrders.clear();
                v.attackMove = false;
                v.attackMoveEngaging = false;
                v.chasingAttackTarget = false;
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            continue;
        }
        if (v.speedPx <= 0.0 || v.isMoveImpaired()) {
            if (v.moveTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.queuedOrders.clear();
                v.attackMove = false;
                v.attackMoveEngaging = false;
                v.chasingAttackTarget = false;
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(i);
                }
            }
            stepFacing(i, v);
            continue;
        }
        if (!v.moveTarget) {
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            stepFacing(i, v);
            continue;
        }
        if (v.attackMoveEngaging) {
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            continue;
        }
        const double vetFrac = static_cast<double>(std::clamp(v.expa, 0, 0x8000)) / 32768.0;
        const double fwdSpeed = (v.speedPxMax > v.speedPx)
            ? (v.speedPx + (v.speedPxMax - v.speedPx) * vetFrac)
            : v.speedPx;
        const double revSpeed = (v.backSpeedPx > 0.0 && v.speedPx > 0.0 && v.speedPxMax > v.speedPx)
            ? v.backSpeedPx * (fwdSpeed / v.speedPx)
            : v.backSpeedPx;
        const double rawSpeed = v.reverseMove ? revSpeed : fwdSpeed;
        const double baseSpeed = (v.moveDamageHp > 0 && v.hp < v.moveDamageHp)
            ? std::max(0.45, rawSpeed * 0.35)
            : rawSpeed;
        double towMult = 1.0;
        if (v.towedVehicle && *v.towedVehicle < vehicles_.size()) {
            const auto& gun = vehicles_[*v.towedVehicle];
            const int tIdx = assets::UnitSpec::towClassIndex(gun.towWeightClass);
            if (tIdx >= 0 && v.towSpeedByClass[static_cast<std::size_t>(tIdx)] > 0.0) {
                towMult = v.towSpeedByClass[static_cast<std::size_t>(tIdx)];
            } else {
                towMult = 0.6;
            }
        }
        const bool shallowBog = v.isWheeled && !v.isNaval && grid_.isShallows(v.cell);
        double stepSpeed = v.towedVehicle ? std::max(0.45, baseSpeed * towMult) : baseSpeed;
        if (shallowBog) {
            stepSpeed = std::max(0.35, stepSpeed * 0.70);
        }
        const auto advanceNextOrderOrPatrol = [&](Vehicle& veh) -> bool {
            if (!veh.queuedOrders.empty()) {
                const auto [nx, ny] = veh.queuedOrders.front();
                veh.queuedOrders.pop_front();
                moveVehicleInternal(veh, nx, ny);
                veh.wasMoving = true;
                return true;
            }
            if (veh.patrolOrigin && veh.patrolDest) {
                const auto origin = *veh.patrolOrigin;
                const auto dest = *veh.patrolDest;
                const bool nextToDest = !veh.patrolToDest;
                const auto nextPt = nextToDest ? dest : origin;
                moveVehicleInternal(veh, nextPt.first, nextPt.second);
                veh.patrolOrigin = origin;
                veh.patrolDest = dest;
                veh.patrolToDest = nextToDest;
                veh.attackMove = true;
                veh.wasMoving = true;
                return true;
            }
            return false;
        };
        const auto [tx, ty] = *v.moveTarget;
        const double finalDist = std::hypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
        if (finalDist <= kVehicleArrivePx) {
            if (!advanceNextOrderOrPatrol(v)) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                if (!v.escortVehicleTarget && !v.escortSoldierTarget) {
                    v.attackMove = false;
                }
                v.attackMoveEngaging = false;
                v.reverseMove = false;
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            continue;
        }

        while (v.waypoints.size() > 1) {
            const auto [wx, wy] = v.waypoints.front();
            if (std::hypot(static_cast<double>(wx) - v.fx, static_cast<double>(wy) - v.fy) > stepSpeed) {
                break;
            }
            v.waypoints.pop_front();
        }
        if (v.waypoints.size() == 1) {
            const auto [wx, wy] = v.waypoints.front();
            if (std::hypot(static_cast<double>(wx) - v.fx, static_cast<double>(wy) - v.fy) <= kVehicleArrivePx) {
                v.waypoints.pop_front();
            }
        }

        const auto tryStepToward = [&](int targetX, int targetY, bool allowSteer,
                                       double& outFx, double& outFy, SubCell& outCell) -> bool {
            const double dx = static_cast<double>(targetX) - v.fx;
            const double dy = static_cast<double>(targetY) - v.fy;
            const double dist = std::hypot(dx, dy);
            if (dist <= 1e-6) return false;
            const double stepLen = std::min(stepSpeed, dist);
            const double ux = dx / dist;
            const double uy = dy / dist;
            const std::size_t maxIdx = allowSteer ? kSubtleSteer.size() : 1u;
            for (std::size_t k = 0; k < maxIdx; ++k) {
                const double angle = kSubtleSteer[k];
                const double c = std::cos(angle);
                const double s = std::sin(angle);
                const double candFx = v.fx + (ux * c - uy * s) * stepLen;
                const double candFy = v.fy + (ux * s + uy * c) * stepLen;
                if (angle != 0.0) {
                    const double candDist = std::hypot(static_cast<double>(targetX) - candFx,
                                                       static_cast<double>(targetY) - candFy);
                    if (candDist > dist - 0.1) continue;
                }
                SubCell candCell{};
                if (canVehicleStepTo(v, candFx, candFy, candCell)) {
                    outFx = candFx;
                    outFy = candFy;
                    outCell = candCell;
                    return true;
                }
            }
            return false;
        };

        auto [stepTx, stepTy] = v.waypoints.empty() ? *v.moveTarget : v.waypoints.front();
        double nextFx = v.fx;
        double nextFy = v.fy;
        SubCell nextCell = v.cell;
        bool moved = tryStepToward(stepTx, stepTy, !v.waypoints.empty(), nextFx, nextFy, nextCell);

        if (!moved && v.waypoints.empty()) {
            const SubCell destCell = pointerCell(tx, ty, originX_);
            if (grid_.inBounds(destCell)) {
                const auto dirs = grid_.findTileDirectionsForMask(v.cell, destCell, v.forbiddenTerrainMask, 256);
                if (!dirs.empty()) {
                    int curTx = v.cell.x / 2;
                    int curTy = v.cell.y / 2;
                    const auto pushTileCenter = [&](int tileX, int tileY) {
                        const int wu = tileX * 32 + 16;
                        const int wv = tileY * 32 + 16;
                        v.waypoints.emplace_back(originX_ + wu - wv, (wu + wv) / 2);
                    };
                    for (std::uint8_t d : dirs) {
                        if (d >= 8u) break;
                        const int dxTile = kTileDx[d];
                        const int dyTile = kTileDy[d];
                        if (dxTile != 0 && dyTile != 0) {
                            const SubCell sideA{(curTx + dxTile) * 2, curTy * 2};
                            const SubCell sideB{curTx * 2, (curTy + dyTile) * 2};
                            if (!grid_.walkableMask(sideA, v.forbiddenTerrainMask) ||
                                !grid_.walkableMask(sideB, v.forbiddenTerrainMask)) {
                                if (grid_.walkableMask(sideA, v.forbiddenTerrainMask)) {
                                    pushTileCenter(curTx + dxTile, curTy);
                                } else if (grid_.walkableMask(sideB, v.forbiddenTerrainMask)) {
                                    pushTileCenter(curTx, curTy + dyTile);
                                }
                            }
                        }
                        curTx += dxTile;
                        curTy += dyTile;
                        pushTileCenter(curTx, curTy);
                    }
                    if (!v.waypoints.empty()) {
                        v.waypoints.back() = {tx, ty};
                        while (v.waypoints.size() > 1) {
                            const auto [wx, wy] = v.waypoints.front();
                            if (std::hypot(static_cast<double>(wx) - v.fx, static_cast<double>(wy) - v.fy) > stepSpeed) {
                                break;
                            }
                            v.waypoints.pop_front();
                        }
                        if (!v.waypoints.empty()) {
                            stepTx = v.waypoints.front().first;
                            stepTy = v.waypoints.front().second;
                            moved = tryStepToward(stepTx, stepTy, true, nextFx, nextFy, nextCell);
                        }
                    }
                }
            }
        }

        if (!moved) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.queuedOrders.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.attackMove = false;
            v.attackMoveEngaging = false;
            v.chasingAttackTarget = false;
            v.reverseMove = false;
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.wasMoving = false;
            emitVehiclePose(i);
            continue;
        }

        const double prevFx = v.fx;
        const double prevFy = v.fy;
        const unsigned moveDir = vehicleDirFromMapDelta(nextFx - v.fx, nextFy - v.fy);
        const unsigned desiredHull = v.reverseMove ? ((moveDir + 16u) & 31u) : moveDir;
        v.hullDir = turnTowardDir32(v.hullDir, desiredHull, std::max(1u, v.turnStep32));
        if (!v.attackTarget) {
            v.turretDir = turnTowardDir32(
                v.turretDir, v.hullDir, v.hasGunTurnDelay ? v.turretStep32 : std::max(2u, v.turnStep32));
        }
        v.fx = nextFx;
        v.fy = nextFy;
        v.x = static_cast<int>(std::lround(nextFx));
        v.y = static_cast<int>(std::lround(nextFy));
        if (v.gunShotWait > 0) {
            v.gunSettleTicks = v.gunShotWait;
        }

        if (nextCell != v.cell) {
            if (nextCell.x / 2 != v.cell.x / 2 || nextCell.y / 2 != v.cell.y / 2) {
                fogDirty_ = true;
            }
            const SubCell oldCell = v.cell;
            v.cell = nextCell;
            for (size_t pIdx : v.passengers) {
                if (pIdx < soldiers_.size()) soldiers_[pIdx].cell = nextCell;
            }
            if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) {
                grid_.setOccupied(oldCell, false);
            }
            grid_.setOccupied(nextCell, true);
        }

        if (v.canCrushInfantry && v.hasCrew) {
            for (size_t k = 0; k < soldiers_.size(); ++k) {
                auto& ss = soldiers_[k];
                if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(v.owner, ss.owner)) continue;
                const auto [su, sv] = soldierWorld(ss);
                const int sx = originX_ + su - sv;
                const int sy = (su + sv) / 2;
                const double infCrushR = (v.unitRadius > 0)
                    ? std::clamp(static_cast<double>(v.unitRadius + ss.unitRadius), 18.0, 32.0)
                    : 22.0;
                if (std::hypot(static_cast<double>(sx) - v.fx, static_cast<double>(sy) - v.fy) > infCrushR) continue;
                ss.hp = 0;
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 1500 + v.crushExpaBonus * 10);
                recordHit(ss.owner, ss.group);
                if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
                ss.motion.reset();
                ss.goal.reset();
                ss.route.clear();
                ss.cursor = 0;
                ss.queuedGoals.clear();
                ss.patrolOrigin.reset();
                ss.patrolDest.reset();
                ss.attackMove = false;
                ss.attackTarget.reset();
                ss.attackVehicleTarget.reset();
                ss.attackBuildingTarget.reset();
                ss.autoTarget.reset();
                ss.boardVehicleTarget.reset();
                ss.captureVehicleTarget.reset();
                ss.garrisonTarget.reset();
                ss.binoculars = false;
                ss.spottedMask = 0;
                fogDirty_ = true;
                const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
                pushOrUpdateSoldierPose(changed, {
                    k, sx, sy, ss.direction, 0, false, 0, false, ss.owner, ss.group, false, sVis
                });
            }
            for (size_t k = 0; k < vehicles_.size(); ++k) {
                if (k == i) continue;
                auto& tv = vehicles_[k];
                if (!tv.alive || tv.hp <= 0 || !tv.canBeCrushed || !isHostile(v.owner, tv.owner)) continue;
                const double gunCrushR = (v.unitRadius > 0 || tv.unitRadius > 0)
                    ? std::clamp(static_cast<double>((v.unitRadius > 0 ? v.unitRadius : 12) +
                                                     (tv.unitRadius > 0 ? tv.unitRadius : 12)),
                                 24.0, 38.0)
                    : 26.0;
                if (std::hypot(tv.fx - v.fx, tv.fy - v.fy) > gunCrushR) continue;
                tv.hp = 0;
                destroyVehicle(k, changed);
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 2500);
                emitVehiclePose(k);
            }
        }

        if (v.towedVehicle && *v.towedVehicle < vehicles_.size()) {
            const double mdx = v.fx - prevFx;
            const double mdy = v.fy - prevFy;
            const double mlen = std::hypot(mdx, mdy);
            if (mlen > 1e-4) {
                auto& gun = vehicles_[*v.towedVehicle];
                const double gx = std::clamp(v.fx - (mdx / mlen) * 36.0, 0.0, static_cast<double>(std::max(1, mapPixelW_ - 1)));
                const double gy = std::clamp(v.fy - (mdy / mlen) * 36.0, 0.0, static_cast<double>(std::max(1, mapPixelH_ - 1)));
                gun.fx = gx;
                gun.fy = gy;
                gun.x = static_cast<int>(std::lround(gx));
                gun.y = static_cast<int>(std::lround(gy));
                gun.hullDir = v.hullDir;
                gun.turretDir = v.hullDir;
                const SubCell newGunCell = placementCell(gun.x, gun.y, originX_);
                if (newGunCell != gun.cell) {
                    if (newGunCell.x / 2 != gun.cell.x / 2 || newGunCell.y / 2 != gun.cell.y / 2) {
                        fogDirty_ = true;
                    }
                    const SubCell oldGunCell = gun.cell;
                    gun.cell = newGunCell;
                    if (!soldierAtCell(oldGunCell) && !vehicleAtCell(oldGunCell)) {
                        grid_.setOccupied(oldGunCell, false);
                    }
                    grid_.setOccupied(newGunCell, true);
                }
                emitVehiclePose(*v.towedVehicle);
            }
        }

        const double newDist = std::hypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
        if (newDist <= kVehicleArrivePx) {
            if (!advanceNextOrderOrPatrol(v)) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                v.attackMove = false;
                v.attackMoveEngaging = false;
                v.reverseMove = false;
                v.wasMoving = false;
            }
        } else {
            v.wasMoving = true;
        }
        emitVehiclePose(i);
    }
}

void TutorialUnits::stepVehicleCombat(std::vector<SoldierPose>& changed) {
    struct ActiveWeapon {
        bool valid{false};
        bool secondary{false};
        double range{0.0};
        double deadzone{0.0};
        int damage{0};
        int cooldown{35};
        assets::HitType hitType{assets::HitType::Pierce};
        const std::string* sound{nullptr};
    };
    const auto selectWeapon = [](const Vehicle& v, bool targetIsVehicle) -> ActiveWeapon {
        const bool canPrimary = v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0);
        const bool canSecondary = v.hasSecondary && (v.ammoPerShot2 == 0 || v.ammo2 > 0);
        if (!targetIsVehicle) {
            if (canSecondary) {
                return {true, true, v.range2, 0.0, v.damage2, v.cooldown2, v.hitType2, &v.sound2};
            }
            if (canPrimary) {
                return {true, false, v.range, v.deadzone, v.damage, v.cooldown, v.hitType, &v.sound1};
            }
            return {};
        }
        if (canPrimary) {
            return {true, false, v.range, v.deadzone, v.damage, v.cooldown, v.hitType, &v.sound1};
        }
        if (canSecondary) {
            return {true, true, v.range2, 0.0, v.damage2, v.cooldown2, v.hitType2, &v.sound2};
        }
        return {};
    };
    const auto scaleVehCooldown = [&](const Vehicle& veh, int baseCd) {
        const double vet = static_cast<double>(std::clamp(veh.expa, 0, 0x8000)) / 32768.0;
        double mult = 1.0 - 0.25 * vet;
        if (veh.isCrewImpaired()) {
            mult *= 1.75;
        }
        if (veh.isSuppressed()) {
            mult *= 1.35;
        }
        if (veh.moraleRage > 0 && !veh.isSuppressed() && officerAuraDecayAt(veh.owner, veh.fx, veh.fy) >= 2) {
            mult *= 0.88;
        }
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(baseCd) * mult)));
    };
    const auto applyWeaponShotAndCooldown = [&](Vehicle& veh, bool secondary, int baseCd) {
        if (secondary) {
            veh.ammo2 = std::max(0, veh.ammo2 - veh.ammoPerShot2);
            ++veh.burstCount2;
            if (veh.burstCount2 >= veh.burstShots2) {
                veh.burstCount2 = 0;
                veh.fireCooldown = scaleVehCooldown(veh, veh.burstReload2);
            } else {
                veh.fireCooldown = scaleVehCooldown(veh, baseCd);
            }
            if (veh.shotExpa2 > 0) {
                veh.expa = std::min(0x8000, veh.expa + veh.shotExpa2);
            }
        } else {
            veh.ammo = std::max(0, veh.ammo - veh.ammoPerShot1);
            ++veh.burstCount1;
            if (veh.burstCount1 >= veh.burstShots1) {
                veh.burstCount1 = 0;
                veh.fireCooldown = scaleVehCooldown(veh, veh.burstReload1);
            } else {
                veh.fireCooldown = scaleVehCooldown(veh, baseCd);
            }
            if (veh.shotExpa1 > 0) {
                veh.expa = std::min(0x8000, veh.expa + veh.shotExpa1);
            }
        }
    };
    const auto markMuzzleFlashReveal = [&](Vehicle& veh, bool isSecondary, bool isHeavy, int dmg) {
        if (!fog_ || isSecondary || !isHostile(0, veh.owner)) return;
        if (!veh.isArtillery && !veh.isRocketArtillery && !isHeavy && dmg < 80) return;
        const unsigned dur = (veh.isArtillery || veh.isRocketArtillery) ? 125u : 75u;
        veh.muzzleRevealTicks = std::max(veh.muzzleRevealTicks, dur);
        if (!veh.visible) {
            veh.visible = true;
            veh.spottedMask = static_cast<std::uint8_t>(veh.spottedMask | 1u);
            fogDirty_ = true;
        }
    };

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        v.attackMoveEngaging = false;
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy) continue;
        if (v.fireCooldown > 0) --v.fireCooldown;
        if (!v.wasMoving && v.gunSettleTicks > 0) --v.gunSettleTicks;
        const bool anyAmmo = (v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0)) ||
                             (v.hasSecondary && (v.ammoPerShot2 == 0 || v.ammo2 > 0));
        if (!v.armed || !anyAmmo) {
            v.attackTarget.reset();
            v.attackBuildingTarget.reset();
            if (v.chasingAttackTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(i);
                }
            }
            continue;
        }

        std::optional<std::pair<bool, size_t>> activeTarget;
        bool explicitOrder = false;
        if (v.attackTarget) {
            const auto [isVeh, tid] = *v.attackTarget;
            if (!isVehicleTargetValidFor(v.owner, isVeh, tid) || !selectWeapon(v, isVeh).valid) {
                v.attackTarget.reset();
                if (v.chasingAttackTarget) {
                    v.moveTarget.reset();
                    v.waypoints.clear();
                    v.chasingAttackTarget = false;
                    if (v.wasMoving) {
                        v.wasMoving = false;
                        emitVehiclePose(i);
                    }
                }
            } else {
                activeTarget = *v.attackTarget;
                explicitOrder = true;
            }
        }

        std::optional<size_t> activeBldTarget;
        bool explicitBldOrder = false;
        if (!activeTarget && v.attackBuildingTarget) {
            const size_t bid = *v.attackBuildingTarget;
            if (bid >= buildings_.size() || buildings_[bid].destroyed ||
                buildings_[bid].owner < 0 || !isHostile(v.owner, buildings_[bid].owner)) {
                v.attackBuildingTarget.reset();
                if (v.chasingAttackTarget) {
                    v.moveTarget.reset();
                    v.waypoints.clear();
                    v.chasingAttackTarget = false;
                    if (v.wasMoving) {
                        v.wasMoving = false;
                        emitVehiclePose(i);
                    }
                }
            } else {
                activeBldTarget = bid;
                explicitBldOrder = true;
            }
        }

        const bool vehElevated = fog_ && fog_->isElevated(v.cell.x / 2, v.cell.y / 2);
        const auto effectiveVehRange = [&](double baseR, int targetMapX, int targetMapY) {
            if (!vehElevated) return baseR;
            const SubCell tCell = pointerCell(targetMapX, targetMapY, originX_);
            return fog_->isElevated(tCell.x / 2, tCell.y / 2) ? baseR : baseR * 1.12;
        };

        const bool scanReady = (v.scanDelayTicks <= 0) || (v.lastAutoScanTick == 0u) ||
                               (tick_ >= v.lastAutoScanTick + static_cast<unsigned>(v.scanDelayTicks));
        if (!activeTarget && !activeBldTarget && !v.attackGround && fog_ && (autoEngage_ || v.attackMove) &&
            !v.holdFire && scanReady) {
            v.lastAutoScanTick = tick_;
            const auto vehWeap = selectWeapon(v, true);
            const auto infWeap = selectWeapon(v, false);
            const bool primaryAP = vehWeap.valid && !vehWeap.secondary &&
                (vehWeap.hitType == assets::HitType::Pierce || vehWeap.hitType == assets::HitType::Transpierce);
            const bool primaryHeavyHE = vehWeap.valid && !vehWeap.secondary &&
                vehWeap.hitType == assets::HitType::Explosive && vehWeap.damage >= 120;
            const bool primaryAntiArmor = primaryAP || primaryHeavyHE;
            int bestTier = 99;
            double bestDist = 1e9;
            std::optional<std::pair<bool, size_t>> bestTarget;
            std::optional<size_t> bestBldTarget;
            if (vehWeap.valid) {
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == i || !isVehicleTargetValidFor(v.owner, true, j)) continue;
                    const auto& tv = vehicles_[j];
                    const double d = std::hypot(tv.fx - v.fx, tv.fy - v.fy);
                    if (d < vehWeap.deadzone || d > effectiveVehRange(vehWeap.range, tv.x, tv.y)) continue;
                    const bool targetArmed = tv.armed && (tv.ammo > 0 || tv.ammo2 > 0);
                    const bool targetArmored = tv.hasCatalogArmor
                        ? (tv.armor[static_cast<size_t>(assets::HitType::Machine)].front >= 20 ||
                           tv.armor[static_cast<size_t>(assets::HitType::Pierce)].front >= 20)
                        : (tv.maxHp >= 250);
                    int tier = 0;
                    if (v.hasAttackPref) {
                        const int cls = assets::UnitSpec::targetClassIndex(tv.effectiveTargetType());
                        const int pref = (cls >= 0) ? v.attackPref[static_cast<std::size_t>(cls)] : 0;
                        if (pref == 1 || (pref == 0 && targetArmored && !primaryAntiArmor)) continue;
                        tier = (pref >= 2) ? (256 - pref) : 300;
                    } else {
                        tier = primaryAntiArmor
                            ? (targetArmed ? 0 : 1)
                            : (targetArmored ? 3 : (targetArmed ? 1 : 2));
                    }
                    if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                        bestTier = tier;
                        bestDist = d;
                        bestTarget = std::pair{true, j};
                        bestBldTarget.reset();
                    }
                }
            }
            if (infWeap.valid) {
                const int manPref = v.hasAttackPref
                    ? v.attackPref[static_cast<std::size_t>(assets::TargetClass::Man)] : 0;
                if (!v.hasAttackPref || manPref != 1) {
                    const int infTierPref = v.hasAttackPref ? ((manPref >= 2) ? (256 - manPref) : 300) : 0;
                    for (size_t j = 0; j < soldiers_.size(); ++j) {
                        if (!isVehicleTargetValidFor(v.owner, false, j)) continue;
                        const auto [tu, tv] = soldierWorld(soldiers_[j]);
                        const int tx = originX_ + tu - tv;
                        const int ty = (tu + tv) / 2;
                        const double d = std::hypot(double(tx) - v.fx, double(ty) - v.fy);
                        if (d < infWeap.deadzone || d > effectiveVehRange(infWeap.range, tx, ty)) continue;
                        const int tier = v.hasAttackPref
                            ? infTierPref
                            : (primaryAntiArmor
                                   ? (soldiers_[j].isAntiVehicle ? 1 : 2)
                                   : (soldiers_[j].isAntiVehicle ? 0 : 1));
                        if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                            bestTier = tier;
                            bestDist = d;
                            bestTarget = std::pair{false, j};
                            bestBldTarget.reset();
                        }
                    }
                }
            }
            if ((vehWeap.valid || (v.weapon2Flamer && v.ammo2 > 0)) && !buildings_.empty()) {
                const int bldPref = v.hasAttackPref
                    ? v.attackPref[static_cast<std::size_t>(assets::TargetClass::Building)] : 0;
                for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                    const auto& tb = buildings_[bi];
                    if (tb.destroyed || tb.owner < 0 || !isHostile(v.owner, tb.owner) || tb.occupants.empty()) continue;
                    if ((fog_->tile(tb.entryCell.x / 2, tb.entryCell.y / 2) & (1u << v.owner)) == 0) continue;
                    const double d = std::max(0.0, std::hypot(double(tb.x) - v.fx, double(tb.y) - v.fy) -
                                                   std::min(tb.width, tb.height) * 0.35);
                    const bool canFlameBld = (v.weapon1Flamer && vehWeap.valid && !vehWeap.secondary &&
                                              d >= vehWeap.deadzone && d <= vehWeap.range) ||
                                             (v.weapon2Flamer && v.ammo2 > 0 && d <= v.range2);
                    const bool canVehWeapBld = vehWeap.valid && d >= vehWeap.deadzone && d <= vehWeap.range;
                    if (!canFlameBld && !canVehWeapBld) continue;
                    int tier = 0;
                    if (v.hasAttackPref) {
                        if (bldPref == 1 || (bldPref == 0 && !canFlameBld && !primaryHeavyHE)) continue;
                        tier = (bldPref >= 2) ? (256 - bldPref) : 302;
                    } else {
                        tier = canFlameBld ? 0 : (primaryHeavyHE ? 1 : 3);
                    }
                    if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                        bestTier = tier;
                        bestDist = d;
                        bestBldTarget = bi;
                        bestTarget.reset();
                    }
                }
            }
            activeTarget = bestTarget;
            activeBldTarget = bestBldTarget;
        }

        const auto applyExplosiveSplash = [&](int impactX, int impactY, int dmgBase, int skipSoldierIdx, int skipBuildingIdx) {
            const double splashR = v.indirectArc1 ? 92.0 : 72.0;
            const double bldSplashR = v.indirectArc1 ? 104.0 : 84.0;
            for (size_t k = 0; k < soldiers_.size(); ++k) {
                if (skipSoldierIdx >= 0 && k == static_cast<size_t>(skipSoldierIdx)) continue;
                auto& ss = soldiers_[k];
                if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(v.owner, ss.owner)) continue;
                const auto [su, sv] = soldierWorld(ss);
                const int sx = originX_ + su - sv;
                const int sy = (su + sv) / 2;
                const double sd = std::hypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
                if (sd > splashR) continue;
                const int splashDmg = std::max(1, static_cast<int>(std::lround(
                    static_cast<double>(dmgBase) * 0.55 * (1.0 - sd / (splashR + 1.0)))));
                ss.hp = std::max(0, ss.hp - splashDmg);
                if (ss.hp > 0) applySuppression(k, 25);
                recordHit(ss.owner, ss.group);
                reactToAttack(ss.owner, ss.group, v.x, v.y);
                if (ss.hp == 0) {
                    ++v.kills;
                    v.expa = std::min(0x8000, v.expa + 1500);
                    if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
                    ss.motion.reset();
                    ss.goal.reset();
                    ss.route.clear();
                    ss.cursor = 0;
                    ss.queuedGoals.clear();
                    ss.attackMove = false;
                    ss.attackTarget.reset();
                    ss.attackVehicleTarget.reset();
                    ss.attackBuildingTarget.reset();
                    ss.autoTarget.reset();
                    ss.binoculars = false;
                    ss.spottedMask = 0;
                    fogDirty_ = true;
                }
                const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
                pushOrUpdateSoldierPose(changed, {
                    k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis
                });
            }
            for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                if (skipBuildingIdx >= 0 && bi == static_cast<size_t>(skipBuildingIdx)) continue;
                const auto& tb = buildings_[bi];
                if (tb.destroyed) continue;
                const double bd = std::max(0.0, std::hypot(double(tb.x - impactX), double(tb.y - impactY)) -
                                                std::min(tb.width, tb.height) * 0.3);
                if (bd <= bldSplashR) {
                    damageBuilding(bi, std::max(10, dmgBase / 2), assets::HitType::Explosive, v.owner, changed);
                }
            }
            if (!mines_.empty()) {
                const SubCell impCell = pointerCell(impactX, impactY, originX_);
                const int itx = impCell.x / 2, ity = impCell.y / 2;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        triggerMineAtTile(itx + dx, ity + dy, changed, true);
                    }
                }
            }
        };

        const auto aimVehicleAt = [&](unsigned desiredDir, bool snap, bool forceEmit) {
            const unsigned prevTurret = v.turretDir;
            const unsigned prevHull = v.hullDir;
            const bool pivotHull = !v.towedBy && (v.speedPx <= 0.0 || (v.isCasemate && !v.moveTarget));
            if (snap) {
                v.turretDir = desiredDir;
                if (pivotHull) {
                    v.hullDir = desiredDir;
                }
                return;
            }
            v.turretDir = turnTowardDir32(v.turretDir, desiredDir, std::max(1u, v.turretStep32));
            if (pivotHull) {
                v.hullDir = turnTowardDir32(v.hullDir, desiredDir, std::max(1u, v.turnStep32));
            }
            if (v.turretDir != prevTurret || v.hullDir != prevHull || forceEmit) {
                emitVehiclePose(i);
            }
        };

        if (!activeTarget && activeBldTarget) {
            const size_t bid = *activeBldTarget;
            const auto& tb = buildings_[bid];
            const int tx = tb.x;
            const int ty = tb.y;
            const double dist = std::max(0.0, std::hypot(double(tx) - v.fx, double(ty) - v.fy) -
                                              std::min(tb.width, tb.height) * 0.35);
            auto weapon = selectWeapon(v, true);
            if (v.weapon2Flamer && v.ammo2 > 0 && dist <= v.range2) {
                weapon = {true, true, v.range2, 0.0, v.damage2, v.cooldown2, v.hitType2, &v.sound2};
            }
            if (!weapon.valid) {
                v.attackBuildingTarget.reset();
                continue;
            }
            const unsigned desiredTurret = vehicleDirFromMapDelta(double(tx) - v.fx, double(ty) - v.fy);
            if (explicitBldOrder && dist > weapon.range && v.speedPx > 0.0) {
                const int clampedX = std::clamp(tx, 0, std::max(0, mapPixelW_ - 1));
                const int clampedY = std::clamp(ty, 0, std::max(0, mapPixelH_ - 1));
                const auto nextMove = std::pair{clampedX, clampedY};
                if (v.moveTarget != nextMove) {
                    v.moveTarget = nextMove;
                    v.waypoints.clear();
                }
                v.chasingAttackTarget = true;
                const unsigned prevTurret = v.turretDir;
                v.turretDir = turnTowardDir32(v.turretDir, desiredTurret, std::max(1u, v.turretStep32));
                if (v.turretDir != prevTurret) emitVehiclePose(i);
                continue;
            }
            if (explicitBldOrder && v.chasingAttackTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                v.wasMoving = false;
            }
            if (dist > weapon.range || dist < weapon.deadzone) continue;
            if (v.attackMove && !explicitBldOrder) v.attackMoveEngaging = true;
            if (v.fireCooldown > 0) {
                aimVehicleAt(desiredTurret, false, explicitBldOrder);
                continue;
            }
            if (v.hasGunTurnDelay && !v.isCasemate &&
                dirDelta32(v.turretDir, desiredTurret) > v.turretStep32) {
                aimVehicleAt(desiredTurret, false, true);
                continue;
            }
            if (!weapon.secondary && v.gunShotWait > 0 && v.gunSettleTicks > 0) {
                aimVehicleAt(desiredTurret, false, explicitBldOrder);
                continue;
            }
            aimVehicleAt(desiredTurret, true, false);
            applyWeaponShotAndCooldown(v, weapon.secondary, weapon.cooldown);
            const bool isFlameShot = (weapon.secondary ? v.weapon2Flamer : v.weapon1Flamer) ||
                                     weapon.hitType == assets::HitType::Fire;
            const bool heavyShot = !weapon.secondary &&
                (weapon.hitType == assets::HitType::Explosive || weapon.damage >= 180);
            if (!weapon.secondary && weapon.hitType == assets::HitType::Explosive) {
                applyExplosiveSplash(tx, ty, weapon.damage, -1, static_cast<int>(bid));
            }
            damageBuilding(bid, weapon.damage, weapon.hitType, v.owner, changed);
            markVehicleCombat(v);
            markMuzzleFlashReveal(v, weapon.secondary, heavyShot, weapon.damage);
            emitVehiclePose(i, true, tx, ty, weapon.secondary,
                            weapon.sound ? *weapon.sound : std::string_view{},
                            heavyShot, false, false, false, false,
                            !weapon.secondary && v.isRocketArtillery, isFlameShot);
            if (v.isKamikaze) {
                destroyVehicle(i, changed);
                emitVehiclePose(i);
            }
            continue;
        }

        if (!activeTarget && v.attackGround && v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0)) {
            const auto [gx, gy] = *v.attackGround;
            const double gDist = std::hypot(static_cast<double>(gx) - v.fx, static_cast<double>(gy) - v.fy);
            const unsigned gTurret = vehicleDirFromMapDelta(static_cast<double>(gx) - v.fx, static_cast<double>(gy) - v.fy);
            if (gDist > v.range && v.speedPx > 0.0) {
                const auto nextMove = std::pair{gx, gy};
                if (v.moveTarget != nextMove) {
                    v.moveTarget = nextMove;
                    v.waypoints.clear();
                    v.reverseMove = false;
                }
                const unsigned prevTurret = v.turretDir;
                v.turretDir = turnTowardDir32(v.turretDir, gTurret, std::max(1u, v.turretStep32));
                if (v.turretDir != prevTurret) emitVehiclePose(i);
                continue;
            }
            v.moveTarget.reset();
            v.waypoints.clear();
            v.reverseMove = false;
            v.wasMoving = false;
            if (gDist >= v.deadzone && gDist <= v.range) {
                if (v.fireCooldown > 0) {
                    aimVehicleAt(gTurret, false, false);
                    continue;
                }
                if (v.hasGunTurnDelay && !v.isCasemate &&
                    dirDelta32(v.turretDir, gTurret) > v.turretStep32) {
                    aimVehicleAt(gTurret, false, true);
                    continue;
                }
                if (v.gunShotWait > 0 && v.gunSettleTicks > 0) {
                    aimVehicleAt(gTurret, false, false);
                    continue;
                }
                aimVehicleAt(gTurret, true, false);
                applyWeaponShotAndCooldown(v, false, v.cooldown);
                int impX = gx;
                int impY = gy;
                if (v.isRocketArtillery) {
                    const int ox = static_cast<int>(nextCombatRandom(73u)) - 36;
                    const int oy = static_cast<int>(nextCombatRandom(73u)) - 36;
                    impX = std::clamp(gx + ox, 0, std::max(0, mapPixelW_ - 1));
                    impY = std::clamp(gy + oy, 0, std::max(0, mapPixelH_ - 1));
                } else if (v.accuracy1 > 0) {
                    const double vet = static_cast<double>(std::clamp(v.expa, 0, 0x8000)) / 32768.0;
                    const double effAcc = static_cast<double>(v.accuracy1) -
                                          vet * static_cast<double>(std::max(0, v.accuracy1 - v.accuracy1Min));
                    const int supExtra = v.isSuppressed() ? 8 : 0;
                    const int scatterR = std::clamp(static_cast<int>(std::lround(effAcc * 0.25)) + supExtra, 4, 56);
                    const std::uint32_t span = static_cast<std::uint32_t>(scatterR * 2 + 1);
                    const int ox = static_cast<int>(nextCombatRandom(0x7fffu) % span) - scatterR;
                    const int oy = static_cast<int>(nextCombatRandom(0x7fffu) % span) - scatterR;
                    impX = std::clamp(gx + ox, 0, std::max(0, mapPixelW_ - 1));
                    impY = std::clamp(gy + oy, 0, std::max(0, mapPixelH_ - 1));
                }
                const bool isFlameGround = v.weapon1Flamer || v.hitType == assets::HitType::Fire;
                if (isFlameGround) {
                    applyFlameSplash(impX, impY, v.damage, v.owner, -1, static_cast<int>(i), -1, changed, -2);
                } else {
                    applyExplosiveSplash(impX, impY, v.damage, -1, -1);
                }
                for (size_t k = 0; k < vehicles_.size(); ++k) {
                    auto& tv = vehicles_[k];
                    if (!tv.alive || tv.hp <= 0 || !isHostile(v.owner, tv.owner)) continue;
                    const double vd = std::hypot(tv.fx - static_cast<double>(impX), tv.fy - static_cast<double>(impY));
                    if (vd > 48.0) continue;
                    const int dealt = scaleVehicleDamageForDifficulty(
                        tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                                     v.x, v.y, v.damage, v.hitType));
                    tv.hp = std::max(0, tv.hp - dealt);
                    recordHit(tv.owner, tv.group);
                    markVehicleCombat(tv);
                    reactToAttack(tv.owner, tv.group, v.x, v.y, k);
                    if (tv.hp == 0) {
                        destroyVehicle(k, changed);
                        ++v.kills;
                        v.expa = std::min(0x8000, v.expa + 4500);
                    } else {
                        applyVehicleCrewDamage(k, impX, impY, v.damage,
                                               isFlameGround ? assets::HitType::Fire : v.hitType,
                                               changed, -1, static_cast<int>(i));
                        applyVehicleSuppression(k, 24, impX, impY);
                    }
                    emitVehiclePose(k);
                }
                markVehicleCombat(v);
                markMuzzleFlashReveal(v, false, !isFlameGround, v.damage);
                emitVehiclePose(i, true, impX, impY, false, v.sound1, !isFlameGround, false, false, false, false,
                                v.isRocketArtillery, isFlameGround);
                if (v.isKamikaze) {
                    destroyVehicle(i, changed);
                    emitVehiclePose(i);
                }
                if (v.ammoPerShot1 > 0 && v.ammo <= 0) v.attackGround.reset();
            }
            continue;
        }

        if (!activeTarget) continue;

        const auto [targetIsVehicle, tid] = *activeTarget;
        const auto weapon = selectWeapon(v, targetIsVehicle);
        if (!weapon.valid) continue;

        double targetFx = 0.0;
        double targetFy = 0.0;
        int tx = 0;
        int ty = 0;
        if (targetIsVehicle) {
            targetFx = vehicles_[tid].fx;
            targetFy = vehicles_[tid].fy;
            tx = vehicles_[tid].x;
            ty = vehicles_[tid].y;
        } else {
            const auto [tu, tv] = soldierWorld(soldiers_[tid]);
            tx = originX_ + tu - tv;
            ty = (tu + tv) / 2;
            targetFx = static_cast<double>(tx);
            targetFy = static_cast<double>(ty);
        }
        const double dist = std::hypot(targetFx - v.fx, targetFy - v.fy);
        const double effRange = effectiveVehRange(weapon.range, tx, ty);
        const unsigned desiredTurret = vehicleDirFromMapDelta(targetFx - v.fx, targetFy - v.fy);

        if (explicitOrder && dist > effRange && v.speedPx > 0.0) {
            const int clampedX = std::clamp(tx, 0, std::max(0, mapPixelW_ - 1));
            const int clampedY = std::clamp(ty, 0, std::max(0, mapPixelH_ - 1));
            const auto nextMove = std::pair{clampedX, clampedY};
            if (v.moveTarget != nextMove) {
                v.moveTarget = nextMove;
                v.waypoints.clear();
                v.reverseMove = false;
            }
            v.chasingAttackTarget = true;
            const unsigned prevTurret = v.turretDir;
            v.turretDir = turnTowardDir32(v.turretDir, desiredTurret, std::max(1u, v.turretStep32));
            if (v.turretDir != prevTurret) emitVehiclePose(i);
            continue;
        }

        if (explicitOrder && v.chasingAttackTarget) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.chasingAttackTarget = false;
            v.wasMoving = false;
        }

        if (dist > effRange || dist < weapon.deadzone) continue;

        if (v.attackMove && !explicitOrder) {
            v.attackMoveEngaging = true;
        }

        if (v.fireCooldown > 0) {
            aimVehicleAt(desiredTurret, false, explicitOrder);
            continue;
        }
        if (v.hasGunTurnDelay && !v.isCasemate &&
            dirDelta32(v.turretDir, desiredTurret) > v.turretStep32) {
            aimVehicleAt(desiredTurret, false, true);
            continue;
        }
        if (!weapon.secondary && v.gunShotWait > 0 && v.gunSettleTicks > 0) {
            aimVehicleAt(desiredTurret, false, explicitOrder);
            continue;
        }

        aimVehicleAt(desiredTurret, true, false);
        applyWeaponShotAndCooldown(v, weapon.secondary, weapon.cooldown);

        const bool heavyShot = !weapon.secondary &&
            (weapon.hitType == assets::HitType::Explosive || weapon.damage >= 180);
        const bool isFlameShot = (weapon.secondary ? v.weapon2Flamer : v.weapon1Flamer) ||
                                 weapon.hitType == assets::HitType::Fire;
        const bool heSplash = !weapon.secondary && weapon.hitType == assets::HitType::Explosive;
        if (heSplash) {
            applyExplosiveSplash(tx, ty, weapon.damage, targetIsVehicle ? -1 : static_cast<int>(tid), -1);
        } else if (isFlameShot) {
            applyFlameSplash(tx, ty, weapon.damage, v.owner, -1, static_cast<int>(i),
                             targetIsVehicle ? -1 : static_cast<int>(tid), changed,
                             targetIsVehicle ? static_cast<int>(tid) : -1);
        }
        if (targetIsVehicle) {
            auto& tv = vehicles_[tid];
            const int dealt = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             v.x, v.y, weapon.damage, weapon.hitType));
            tv.hp = std::max(0, tv.hp - dealt);
            if (dealt > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
                tv.expa = std::min(0x8000, tv.expa + std::min(200, dealt * tv.expaOnHit));
            }
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            reactToAttack(tv.owner, tv.group, v.x, v.y, tid);
            if (tv.hp == 0) {
                destroyVehicle(tid, changed);
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 4500);
            } else {
                applyVehicleCrewDamage(tid, v.x, v.y, weapon.damage, weapon.hitType, changed, -1, static_cast<int>(i));
                if (weapon.hitType == assets::HitType::Explosive || weapon.hitType == assets::HitType::Fire ||
                    weapon.damage >= 180) {
                    applyVehicleSuppression(tid, weapon.hitType == assets::HitType::Explosive ? 24 : 16, v.x, v.y);
                }
            }
            emitVehiclePose(tid);
        } else {
            auto& ts = soldiers_[tid];
            ts.hp = std::max(0, ts.hp - weapon.damage);
            if (ts.hp > 0 && ts.expaOnHit > 0) {
                ts.expa = std::min(0x8000, ts.expa + ts.expaOnHit);
            }
            if (ts.hp > 0 && (weapon.hitType == assets::HitType::Machine ||
                              weapon.hitType == assets::HitType::Explosive)) {
                applySuppression(tid, weapon.hitType == assets::HitType::Explosive ? 25 : 20);
            }
            recordHit(ts.owner, ts.group);
            reactToAttack(ts.owner, ts.group, v.x, v.y);
            const auto [tu, tv] = soldierWorld(ts);
            if (ts.hp == 0) {
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 1500);
                if (!vehicleAtCell(ts.cell)) grid_.setOccupied(ts.cell, false);
                ts.motion.reset();
                ts.goal.reset();
                ts.route.clear();
                ts.cursor = 0;
                ts.queuedGoals.clear();
                ts.attackMove = false;
                ts.attackTarget.reset();
                ts.attackVehicleTarget.reset();
                ts.autoTarget.reset();
                ts.binoculars = false;
                ts.spottedMask = 0;
                fogDirty_ = true;
            }
            const bool tVis = !fog_ || isAlliedWithPlayer(ts.owner) || (ts.spottedMask & 1u) != 0;
            pushOrUpdateSoldierPose(changed, {
                tid, originX_ + tu - tv, (tu + tv) / 2, ts.direction, 0, false,
                ts.hp, ts.hp > 0, ts.owner, ts.group, false, tVis
            });
        }
        markVehicleCombat(v);
        markMuzzleFlashReveal(v, weapon.secondary, heavyShot, weapon.damage);
        emitVehiclePose(i, true, tx, ty, weapon.secondary,
                        weapon.sound ? *weapon.sound : std::string_view{},
                        heavyShot, targetIsVehicle, false, false, false,
                        !weapon.secondary && v.isRocketArtillery, isFlameShot);
        if (v.isKamikaze) {
            destroyVehicle(i, changed);
            emitVehiclePose(i);
        }
    }
}
}
