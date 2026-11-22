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

bool needsSupplyService(int hp, int maxHp, int ammo, int maxAmmo, int ammo2, int maxAmmo2) {
    return hp < maxHp || (maxAmmo > 0 && ammo < maxAmmo) || (maxAmmo2 > 0 && ammo2 < maxAmmo2);
}
}
bool TutorialUnits::canSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) const {
    if (targetVehicleIdx >= vehicles_.size()) return false;
    const auto& target = vehicles_[targetVehicleIdx];
    if (!target.alive || target.hp <= 0) return false;
    const bool targetIsAccessibleCrate = target.isCrate && (target.owner == 0 || target.owner == 3);
    if (!targetIsAccessibleCrate && target.owner != 0) return false;

    const bool targetNeedsRepair = !target.isCrate && target.hp < target.maxHp;
    const bool targetNeedsAmmo1 = !target.isSupply && target.maxAmmo > 0 && target.ammo < target.maxAmmo;
    const bool targetNeedsAmmo2 = !target.isSupply && target.maxAmmo2 > 0 && target.ammo2 < target.maxAmmo2;
    if (!targetIsAccessibleCrate && !targetNeedsRepair && !targetNeedsAmmo1 && !targetNeedsAmmo2) {
        return false;
    }
    return std::ranges::any_of(supporterVehicleIds, [&](size_t id) {
        if (id >= vehicles_.size() || id == targetVehicleIdx) return false;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.isSupply || v.isCrate || v.speedPx <= 0.0 || v.towedBy) {
            return false;
        }
        if (targetIsAccessibleCrate) {
            return (v.ammo < v.maxAmmo && target.ammo > 0) || (v.ammo2 < v.maxAmmo2 && target.ammo2 > 0);
        }
        const bool canDoRepair = targetNeedsRepair && (!v.hasFiniteSupply || v.ammo2 > 0);
        const bool canDoAmmo = (targetNeedsAmmo1 || targetNeedsAmmo2) && (!v.hasFiniteSupply || v.ammo > 0);
        return canDoRepair || canDoAmmo;
    });
}

size_t TutorialUnits::orderSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) {
    if (!canSupportVehicle(supporterVehicleIds, targetVehicleIdx)) return 0;
    const auto& target = vehicles_[targetVehicleIdx];
    size_t accepted = 0;
    std::vector<size_t> seen;
    for (size_t id : supporterVehicleIds) {
        if (id >= vehicles_.size() || id == targetVehicleIdx || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.isSupply || v.isCrate || v.speedPx <= 0.0 || v.towedBy) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        const double dist = det::detHypot(target.fx - v.fx, target.fy - v.fy);
        if (dist <= 120.0) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, target.fx, target.fy, 64.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
            v.supportTargetVehicle = targetVehicleIdx;
        }
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::canSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) const {
    if (buildingIdx >= buildings_.size()) return false;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || (b.owner >= 0 && b.owner != 0)) return false;
    bool needsSupply = b.hp < b.maxHp;
    bool needsMedic = false;
    if (b.owner == 0) {
        for (size_t sIdx : b.occupants) {
            if (sIdx >= soldiers_.size()) continue;
            const auto& s = soldiers_[sIdx];
            if (s.owner != 0 || s.hp <= 0) continue;
            if (s.hp < s.maxHp) needsMedic = true;
            const bool canCarryGrenades = !s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar;
            const int maxInfAmmo = std::max(1, s.desc.maxAmmo);
            const int maxMines = s.maxMineAmmo > 0 ? s.maxMineAmmo : 5;
            if (s.ammo < maxInfAmmo || (canCarryGrenades && s.atGrenades < 2) ||
                (s.canLayMines && s.mineAmmo < maxMines)) {
                needsSupply = true;
            }
        }
    }
    if (!needsSupply && !needsMedic) return false;
    return std::ranges::any_of(supporterVehicleIds, [&](size_t id) {
        if (id >= vehicles_.size()) return false;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) return false;
        return (needsSupply && v.isSupply) || (needsMedic && v.isAmbulance);
    });
}

size_t TutorialUnits::orderSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) {
    if (!canSupportBuilding(supporterVehicleIds, buildingIdx)) return 0;
    const auto& b = buildings_[buildingIdx];
    bool needsSupply = b.hp < b.maxHp;
    bool needsMedic = false;
    if (b.owner == 0) {
        for (size_t sIdx : b.occupants) {
            if (sIdx >= soldiers_.size()) continue;
            const auto& s = soldiers_[sIdx];
            if (s.owner != 0 || s.hp <= 0) continue;
            if (s.hp < s.maxHp) needsMedic = true;
            const bool canCarryGrenades = !s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar;
            const int maxInfAmmo = std::max(1, s.desc.maxAmmo);
            const int maxMines = s.maxMineAmmo > 0 ? s.maxMineAmmo : 5;
            if (s.ammo < maxInfAmmo || (canCarryGrenades && s.atGrenades < 2) ||
                (s.canLayMines && s.mineAmmo < maxMines)) {
                needsSupply = true;
            }
        }
    }
    const double bRadius = static_cast<double>(std::max(b.width, b.height)) * 0.5;
    const double bReach = 125.0 + bRadius;
    size_t accepted = 0;
    std::vector<size_t> seen;
    for (size_t id : supporterVehicleIds) {
        if (id >= vehicles_.size() || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        if (!((needsSupply && v.isSupply) || (needsMedic && v.isAmbulance))) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        const double dist = det::detHypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy);
        if (dist <= bReach) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, static_cast<double>(b.x), static_cast<double>(b.y),
                                                    bRadius + 32.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
            v.supportTargetBuilding = buildingIdx;
        }
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::canHealSoldier(std::span<const size_t> medicSoldierIds, std::span<const size_t> ambulanceVehicleIds,
                                   size_t targetSoldierIdx) const {
    if (targetSoldierIdx >= soldiers_.size()) return false;
    const auto& t = soldiers_[targetSoldierIdx];
    if (t.owner != 0 || t.hp <= 0 || t.embarked || t.garrisonBuilding || t.hp >= t.maxHp) return false;
    const bool hasMedic = std::ranges::any_of(medicSoldierIds, [&](size_t id) {
        if (id >= soldiers_.size() || id == targetSoldierIdx) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.isMedic;
    });
    if (hasMedic) return true;
    return std::ranges::any_of(ambulanceVehicleIds, [&](size_t id) {
        if (id >= vehicles_.size()) return false;
        const auto& v = vehicles_[id];
        return v.alive && v.hp > 0 && v.owner == 0 && v.isAmbulance && v.speedPx > 0.0 && !v.towedBy;
    });
}

size_t TutorialUnits::orderHealSoldier(std::span<const size_t> medicSoldierIds, std::span<const size_t> ambulanceVehicleIds,
                                       size_t targetSoldierIdx) {
    if (!canHealSoldier(medicSoldierIds, ambulanceVehicleIds, targetSoldierIdx)) return 0;
    const auto& t = soldiers_[targetSoldierIdx];
    const auto [tu, tv] = soldierWorld(t);
    const int tx = originX_ + tu - tv;
    const int ty = (tu + tv) / 2;
    size_t accepted = 0;

    std::vector<size_t> seenMedics;
    for (size_t id : medicSoldierIds) {
        if (id >= soldiers_.size() || id == targetSoldierIdx || std::ranges::find(seenMedics, id) != seenMedics.end()) {
            continue;
        }
        seenMedics.push_back(id);
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isMedic) continue;
        const auto [su, sv] = soldierWorld(s);
        const double worldDist = det::detHypot(static_cast<double>(su - tu), static_cast<double>(sv - tv));
        if (worldDist <= 84.0) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
            s.attackMove = false;
            s.boardVehicleTarget.reset();
            s.captureVehicleTarget.reset();
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
            ++accepted;
        } else if (orderSoldierInternal(id, tx, ty)) {
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
            s.attackMove = false;
            s.autoTarget.reset();
            s.healTarget = targetSoldierIdx;
            ++accepted;
        }
    }

    std::vector<size_t> seenAmbs;
    for (size_t id : ambulanceVehicleIds) {
        if (id >= vehicles_.size() || std::ranges::find(seenAmbs, id) != seenAmbs.end()) continue;
        seenAmbs.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.isAmbulance || v.speedPx <= 0.0 || v.towedBy) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        const double dist = det::detHypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
        if (dist <= 120.0) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, static_cast<double>(tx), static_cast<double>(ty),
                                                    56.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
            v.supportTargetSoldier = targetSoldierIdx;
        }
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::canEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetVehicleIdx) const {
    if (targetVehicleIdx >= vehicles_.size()) return false;
    const auto& t = vehicles_[targetVehicleIdx];
    if (!t.alive || t.hp <= 0 || t.owner != 0) return false;
    const bool hasSol = std::ranges::any_of(soldierIds, [&](size_t id) {
        if (id >= soldiers_.size()) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding;
    });
    if (hasSol) return true;
    return std::ranges::any_of(vehicleIds, [&](size_t id) {
        if (id >= vehicles_.size() || id == targetVehicleIdx) return false;
        const auto& v = vehicles_[id];
        return v.alive && v.hp > 0 && v.owner == 0 && v.speedPx > 0.0 && !v.towedBy &&
               v.towedVehicle != targetVehicleIdx;
    });
}

bool TutorialUnits::canEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetSoldierIdx) const {
    if (targetSoldierIdx >= soldiers_.size()) return false;
    const auto& t = soldiers_[targetSoldierIdx];
    if (t.owner != 0 || t.hp <= 0 || t.embarked || t.garrisonBuilding) return false;
    const bool hasSol = std::ranges::any_of(soldierIds, [&](size_t id) {
        if (id >= soldiers_.size() || id == targetSoldierIdx) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding;
    });
    if (hasSol) return true;
    return std::ranges::any_of(vehicleIds, [&](size_t id) {
        if (id >= vehicles_.size()) return false;
        const auto& v = vehicles_[id];
        return v.alive && v.hp > 0 && v.owner == 0 && v.speedPx > 0.0 && !v.towedBy;
    });
}

size_t TutorialUnits::dispatchEscort(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     double targetX, double targetY, std::optional<size_t> targetVehicleIdx,
                                     std::optional<size_t> targetSoldierIdx) {
    size_t accepted = 0;
    std::vector<size_t> seenSoldiers;
    seenSoldiers.reserve(soldierIds.size());
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size() || (targetSoldierIdx && id == *targetSoldierIdx) ||
            std::ranges::find(seenSoldiers, id) != seenSoldiers.end()) {
            continue;
        }
        seenSoldiers.push_back(id);
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
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
        const auto [su, sv] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sv);
        const double sy = static_cast<double>(su + sv) * 0.5;
        if (det::detHypot(targetX - sx, targetY - sy) <= 90.0) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        } else {
            const auto approach = approachPointNear(sx, sy, targetX, targetY, 52.0, mapPixelW_, mapPixelH_);
            (void)orderSoldierInternal(id, approach.first, approach.second);
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
        }
        s.escortVehicleTarget = targetVehicleIdx;
        s.escortSoldierTarget = targetSoldierIdx;
        s.attackMove = true;
        ++accepted;
    }

    std::vector<size_t> seenVehicles;
    seenVehicles.reserve(vehicleIds.size());
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size() || (targetVehicleIdx && id == *targetVehicleIdx) ||
            std::ranges::find(seenVehicles, id) != seenVehicles.end()) {
            continue;
        }
        seenVehicles.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        if (targetVehicleIdx && v.towedVehicle == targetVehicleIdx) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.facingTargetDir.reset();
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.reverseMove = false;
        v.attackMoveEngaging = false;
        if (det::detHypot(targetX - v.fx, targetY - v.fy) <= 110.0) {
            v.moveTarget.reset();
            v.waypoints.clear();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, targetX, targetY, 76.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
        }
        v.escortVehicleTarget = targetVehicleIdx;
        v.escortSoldierTarget = targetSoldierIdx;
        v.attackMove = true;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                         size_t targetVehicleIdx) {
    if (!canEscortVehicle(soldierIds, vehicleIds, targetVehicleIdx)) return 0;
    const auto& target = vehicles_[targetVehicleIdx];
    return dispatchEscort(soldierIds, vehicleIds, target.fx, target.fy, targetVehicleIdx, std::nullopt);
}

size_t TutorialUnits::orderEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                         size_t targetSoldierIdx) {
    if (!canEscortSoldier(soldierIds, vehicleIds, targetSoldierIdx)) return 0;
    const auto [tu, tv] = soldierWorld(soldiers_[targetSoldierIdx]);
    const double tx = static_cast<double>(originX_ + tu - tv);
    const double ty = static_cast<double>(tu + tv) * 0.5;
    return dispatchEscort(soldierIds, vehicleIds, tx, ty, std::nullopt, targetSoldierIdx);
}

void TutorialUnits::stepMedicHeal(std::vector<SoldierPose>& changed) {
    constexpr std::int64_t kHealRadiusSq = 96 * 96;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (!s.isMedic || s.hp <= 0 || s.embarked) continue;
        if (s.garrisonBuilding) {
            const size_t bIdx = *s.garrisonBuilding;
            if (bIdx < buildings_.size() && !buildings_[bIdx].destroyed) {
                const auto& b = buildings_[bIdx];
                std::optional<size_t> bestOcc;
                int lowestHp = 1000000;
                for (size_t occId : b.occupants) {
                    if (occId >= soldiers_.size()) continue;
                    const auto& other = soldiers_[occId];
                    if (other.owner != s.owner || other.hp <= 0 || other.hp >= other.maxHp) continue;
                    if (other.hp < lowestHp) {
                        lowestHp = other.hp;
                        bestOcc = occId;
                    }
                }
                if (bestOcc) {
                    auto& target = soldiers_[*bestOcc];
                    target.hp = std::min(target.maxHp, target.hp + 8);
                    pushOrUpdateSoldierPose(changed, {
                        *bestOcc, b.x, b.y, target.direction, 0, false, target.hp, true,
                        target.owner, target.group, false, false, 0, 0, true
                    });
                }
            }
            continue;
        }
        if (s.motion || !s.route.empty()) continue;
        const auto [su, sv] = soldierWorld(s);
        std::optional<size_t> bestIdx;
        std::int64_t bestD2 = kHealRadiusSq + 1;
        for (size_t j = 0; j < soldiers_.size(); ++j) {
            const auto& other = soldiers_[j];
            if (other.owner != s.owner || other.hp <= 0 || other.embarked || other.garrisonBuilding || other.hp >= other.maxHp) continue;
            const auto [ou, ov] = soldierWorld(other);
            const std::int64_t du = std::int64_t(ou) - su;
            const std::int64_t dv = std::int64_t(ov) - sv;
            const std::int64_t d2 = du * du + dv * dv;
            if (d2 <= kHealRadiusSq && d2 < bestD2) {
                bestD2 = d2;
                bestIdx = j;
            }
        }
        if (!bestIdx) {
            if (autoEngage_ && !s.holdFire && !s.goal && !s.healTarget &&
                !s.boardVehicleTarget && !s.captureVehicleTarget && !s.garrisonTarget &&
                !s.escortVehicleTarget && !s.escortSoldierTarget && !s.patrolOrigin &&
                !s.disarmMineTarget && s.queuedGoals.empty()) {
                constexpr std::int64_t kAutoTriageRadiusSq = 280 * 280;
                std::optional<size_t> autoIdx;
                std::int64_t autoBestD2 = kAutoTriageRadiusSq + 1;
                int autoTx = 0;
                int autoTy = 0;
                for (size_t j = 0; j < soldiers_.size(); ++j) {
                    if (j == i) continue;
                    const auto& other = soldiers_[j];
                    if (other.owner != s.owner || other.hp <= 0 || other.embarked ||
                        other.garrisonBuilding || other.hp >= other.maxHp) {
                        continue;
                    }
                    const auto [ou, ov] = soldierWorld(other);
                    const std::int64_t du = std::int64_t(ou) - su;
                    const std::int64_t dv = std::int64_t(ov) - sv;
                    const std::int64_t d2 = du * du + dv * dv;
                    if (d2 <= kAutoTriageRadiusSq && d2 < autoBestD2) {
                        autoBestD2 = d2;
                        autoIdx = j;
                        autoTx = originX_ + ou - ov;
                        autoTy = (ou + ov) / 2;
                    }
                }
                if (autoIdx && orderSoldierInternal(i, autoTx, autoTy) &&
                    soldiers_[i].goal && !sameTile(*soldiers_[i].goal, soldiers_[i].cell)) {
                    soldiers_[i].healTarget = *autoIdx;
                }
            }
            const int msx = originX_ + su - sv;
            const int msy = (su + sv) / 2;
            for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
                auto& tv = vehicles_[vi];
                if (!tv.alive || tv.hp <= 0 || !tv.hasCrew || tv.owner != s.owner || tv.crewHp >= tv.maxCrewHp) {
                    continue;
                }
                if (det::detHypot(tv.fx - static_cast<double>(msx), tv.fy - static_cast<double>(msy)) <= 96.0) {
                    const int healAmt = tv.crewCanBeHealed ? 70 : 35;
                    tv.crewHp = std::min(tv.maxCrewHp, tv.crewHp + healAmt);
                    emitVehiclePose(vi, false, 0, 0, false, {}, false, false, true, false);
                    break;
                }
            }
            continue;
        }
        auto& target = soldiers_[*bestIdx];
        target.hp = std::min(target.maxHp, target.hp + 8);
        bool updatedExisting = false;
        for (auto& pose : changed) {
            if (pose.index == *bestIdx) {
                pose.hp = target.hp;
                updatedExisting = true;
            }
        }
        if (!updatedExisting) {
            const auto [tu, tv] = soldierWorld(target);
            const unsigned dir = target.motion ? target.motion->direction : target.direction;
            const unsigned prog = target.motion ? target.motion->progress : 0u;
            const bool walking = target.motion ? target.motion->running : false;
            const bool vis = !fog_ || target.owner == 0 || (target.spottedMask & 1u) != 0;
            changed.push_back({*bestIdx, originX_ + tu - tv, (tu + tv) / 2, dir, prog, walking,
                               target.hp, true, target.owner, target.group, false, vis});
        }
    }
}

void TutorialUnits::stepSupplyAndRepair(std::vector<SoldierPose>& changed) {
    constexpr double kSupportRadius = 140.0;
    const auto pushHealed = [&](size_t idx, const Soldier& s) {
        for (auto& pose : changed) {
            if (pose.index == idx) {
                pose.hp = s.hp;
                pose.ammo = s.ammo;
                pose.atGrenades = s.atGrenades;
                pose.mineAmmo = s.mineAmmo;
                return;
            }
        }
        const auto [tu, tv] = soldierWorld(s);
        const unsigned dir = s.motion ? s.motion->direction : s.direction;
        const unsigned prog = s.motion ? s.motion->progress : 0u;
        const bool walking = s.motion ? s.motion->running : false;
        const bool vis = !fog_ || s.owner == 0 || (s.spottedMask & 1u) != 0;
        changed.push_back({idx, originX_ + tu - tv, (tu + tv) / 2, dir, prog, walking,
                           s.hp, true, s.owner, s.group, false, vis, 0, 0, false,
                           s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s),
                           s.mineAmmo, s.ammo, false});
    };

    const auto veteranRateMult = [](int baseDelay, int maxDelay, int expa) {
        if (baseDelay <= 0 || maxDelay <= 0 || maxDelay >= baseDelay) return 1.0;
        const double vetFrac = static_cast<double>(std::clamp(expa, 0, 0x8000)) / 32768.0;
        const double effDelay = static_cast<double>(baseDelay) -
                                vetFrac * static_cast<double>(baseDelay - maxDelay);
        return static_cast<double>(baseDelay) / std::max(1.0, effDelay);
    };
    constexpr double kAutoSupportRadius = 280.0;
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || (v.moveTarget && !v.patrolOrigin)) continue;
        if (v.hasCrew && v.owner != 3 && v.canSelfRepair && !v.isKamikaze && !v.towedBy &&
            !v.moveTarget && (v.lastCombatTick == 0u || tick_ >= v.lastCombatTick + 125u)) {
            const int minMobilityHp = v.moveDamageHp > 0 ? std::min(v.maxHp, v.moveDamageHp + 1) : 0;
            const int emergencyCap = std::clamp(std::max(minMobilityHp, (v.maxHp * 35) / 100), 1, v.maxHp);
            if (v.hp < emergencyCap) {
                v.hp = std::min(emergencyCap, v.hp + std::max(6, v.maxHp / 45));
                emitVehiclePose(i, false, 0, 0, false, {}, false, false, true, false);
            }
        }
        const bool canAutoDispatch =
            autoEngage_ && !v.holdFire && v.hasCrew && v.speedPx > 0.0 && !v.towedBy &&
            !v.moveTarget && v.waypoints.empty() && v.queuedOrders.empty() &&
            !v.supportTargetVehicle && !v.supportTargetBuilding && !v.supportTargetSoldier &&
            !v.escortVehicleTarget && !v.escortSoldierTarget && !v.patrolOrigin &&
            !v.pendingTowGun && !v.facingTargetDir;
        if (v.isSupply) {
            int repairsLeft = 1;
            int resuppliesLeft = 2;
            const bool canResupplyAmmo = !v.hasFiniteSupply || v.ammo > 0;
            const bool canRepairParts = !v.hasFiniteSupply || v.ammo2 > 0;
            const double supplyMult = veteranRateMult(v.ammoRegenDelay, v.ammoRegenDelayMax, v.expa);
            bool truckStockChanged = false;
            bool didAnyService = false;
            if (!v.isCrate && v.hasFiniteSupply) {
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == i) continue;
                    auto& crate = vehicles_[j];
                    if (!crate.alive || crate.hp <= 0 || !crate.isCrate) continue;
                    if (crate.owner != v.owner && crate.owner != 3) continue;
                    if (det::detHypot(crate.fx - v.fx, crate.fy - v.fy) > kSupportRadius) continue;
                    bool didCrateTransfer = false;
                    if (v.ammo < v.maxAmmo && crate.ammo > 0) {
                        const int xfer = std::min({500, crate.ammo, v.maxAmmo - v.ammo});
                        crate.ammo -= xfer;
                        v.ammo += xfer;
                        didCrateTransfer = true;
                    }
                    if (v.ammo2 < v.maxAmmo2 && crate.ammo2 > 0) {
                        const int xfer2 = std::min({500, crate.ammo2, v.maxAmmo2 - v.ammo2});
                        crate.ammo2 -= xfer2;
                        v.ammo2 += xfer2;
                        didCrateTransfer = true;
                    }
                    if (didCrateTransfer) {
                        truckStockChanged = true;
                        emitVehiclePose(j, false, 0, 0, false, {}, false, false, false, true);
                        emitVehiclePose(i, false, 0, 0, false, {}, false, false, false, true);
                    }
                }
            }
            for (size_t j = 0; j < vehicles_.size() && (repairsLeft > 0 || resuppliesLeft > 0); ++j) {
                if (j == i) continue;
                auto& other = vehicles_[j];
                if (!other.alive || other.hp <= 0 || other.owner != v.owner) continue;
                if (det::detHypot(other.fx - v.fx, other.fy - v.fy) > kSupportRadius) continue;
                bool didRepair = false;
                bool didResupply = false;
                if (repairsLeft > 0 && !other.isCrate && other.hp < other.maxHp &&
                    (!v.hasFiniteSupply || v.ammo2 > 0)) {
                    const int maxRepairStep = std::max(1, static_cast<int>(det::detRoundToInt(18.0 * supplyMult)));
                    int gain = std::min(maxRepairStep, other.maxHp - other.hp);
                    if (v.hasFiniteSupply) {
                        const int partsPerHp = other.repairCost > 0 ? other.repairCost : 10;
                        if (v.ammo2 < partsPerHp) {
                            gain = 1;
                            v.ammo2 = 0;
                        } else {
                            gain = std::min(gain, v.ammo2 / partsPerHp);
                            v.ammo2 -= gain * partsPerHp;
                        }
                        truckStockChanged = true;
                    }
                    other.hp += gain;
                    --repairsLeft;
                    didRepair = true;
                    didAnyService = true;
                }
                const bool needsAmmo1 = other.maxAmmo > 0 && other.ammo < other.maxAmmo;
                const bool needsAmmo2 = other.maxAmmo2 > 0 && other.ammo2 < other.maxAmmo2;
                if (resuppliesLeft > 0 && !other.isSupply && (needsAmmo1 || needsAmmo2) &&
                    (!v.hasFiniteSupply || v.ammo > 0)) {
                    const int prevA1 = other.ammo;
                    const int prevA2 = other.ammo2;
                    const int unitCost1 = other.resupplyCost1 > 0 ? other.resupplyCost1 : 15;
                    const int unitCost2 = other.resupplyCost2 > 0 ? other.resupplyCost2 : 2;
                    const int baseStep1 = std::max(1, other.maxAmmo / 6);
                    const int scaledStep1 = std::max(1, static_cast<int>(det::detRoundToInt(static_cast<double>(baseStep1) * supplyMult)));
                    const int scaledStep2 = std::max(5, static_cast<int>(det::detRoundToInt(static_cast<double>(std::max(5, other.maxAmmo2 / 6)) * supplyMult)));
                    int add1 = needsAmmo1 ? std::min(other.maxAmmo - other.ammo, scaledStep1) : 0;
                    int add2 = needsAmmo2 ? std::min(other.maxAmmo2 - other.ammo2, scaledStep2) : 0;
                    if (v.hasFiniteSupply && other.resupplyCost1 > 0 && add1 > 1 && v.ammo < add1 * unitCost1) {
                        add1 = std::max(1, v.ammo / unitCost1);
                    }
                    if (add1 > 0) other.ammo += add1;
                    if (add2 > 0) other.ammo2 += add2;
                    const int d1 = std::max(0, other.ammo - prevA1);
                    const int d2 = std::max(0, other.ammo2 - prevA2);
                    if (v.hasFiniteSupply) {
                        const int cost = std::max(1, d1 * unitCost1 + d2 * unitCost2);
                        v.ammo = std::max(0, v.ammo - cost);
                        truckStockChanged = true;
                    }
                    --resuppliesLeft;
                    didResupply = true;
                    didAnyService = true;
                }
                if (didRepair || didResupply) {
                    emitVehiclePose(j, false, 0, 0, false, {}, false, false, didRepair, didResupply);
                }
            }
            if (repairsLeft > 0 && canRepairParts && (!v.hasFiniteSupply || v.ammo2 > 0)) {
                for (size_t bi = 0; bi < buildings_.size() && repairsLeft > 0 && (!v.hasFiniteSupply || v.ammo2 > 0); ++bi) {
                    auto& b = buildings_[bi];
                    if (b.destroyed || b.hp >= b.maxHp) continue;
                    if (b.owner >= 0 && b.owner != v.owner) continue;
                    const double bReach = kSupportRadius + static_cast<double>(std::max(b.width, b.height)) * 0.5;
                    if (det::detHypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) > bReach) continue;
                    const int maxBldStep = std::max(1, static_cast<int>(det::detRoundToInt(25.0 * supplyMult)));
                    int gain = std::min(maxBldStep, b.maxHp - b.hp);
                    if (v.hasFiniteSupply) {
                        if (v.ammo2 < 10) {
                            gain = 1;
                            v.ammo2 = 0;
                        } else {
                            gain = std::min(gain, v.ammo2 / 10);
                            v.ammo2 -= gain * 10;
                        }
                        truckStockChanged = true;
                    }
                    b.hp += gain;
                    didAnyService = true;
                    emitBuildingUpdate(bi);
                    emitVehiclePose(i, false, 0, 0, false, {}, false, false, true, false);
                    --repairsLeft;
                }
            }
            auto resupplySoldier = [&](Soldier& s) -> std::pair<bool, bool> {
                if (v.hasFiniteSupply && v.ammo <= 0) return {false, false};
                const bool canCarryGrenades = !s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar;
                const int maxInfAmmo = std::max(1, s.desc.maxAmmo);
                const int ammoStep = std::max(1, (maxInfAmmo + 3) / 4);
                const bool needsAmmo = s.ammo < maxInfAmmo;
                const bool needsGrenade = canCarryGrenades && s.atGrenades < 2;
                const int maxMines = s.maxMineAmmo > 0 ? s.maxMineAmmo : 5;
                const bool needsMine = s.canLayMines && s.mineAmmo < maxMines;
                if (!needsAmmo && !needsGrenade && !needsMine) return {false, false};
                const int prevSA = s.ammo;
                if (needsAmmo) s.ammo = std::min(maxInfAmmo, s.ammo + ammoStep);
                if (needsGrenade) s.atGrenades = std::min(2, s.atGrenades + 1);
                if (needsMine) s.mineAmmo = std::min(maxMines, s.mineAmmo + 1);
                if (v.hasFiniteSupply) {
                    const int da = std::max(0, s.ammo - prevSA);
                    const int unitCost = s.resupplyCost > 0 ? s.resupplyCost : 3;
                    const int cost = std::max(1, da * unitCost + ((needsGrenade || needsMine) ? 25 : 0));
                    v.ammo = std::max(0, v.ammo - cost);
                    truckStockChanged = true;
                }
                return {true, needsGrenade || needsMine};
            };
            int infLeft = 3;
            bool didInfResupply = false;
            if (canResupplyAmmo) {
                for (size_t sIdx = 0; sIdx < soldiers_.size() && infLeft > 0; ++sIdx) {
                    auto& s = soldiers_[sIdx];
                    if (s.owner != v.owner || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
                    const auto [su, sv] = soldierWorld(s);
                    const int sx = originX_ + su - sv;
                    const int sy = (su + sv) / 2;
                    if (det::detHypot(double(sx) - v.fx, double(sy) - v.fy) > kSupportRadius) continue;
                    const auto [didRes, gearChanged] = resupplySoldier(s);
                    (void)gearChanged;
                    if (!didRes) continue;
                    pushHealed(sIdx, s);
                    didInfResupply = true;
                    didAnyService = true;
                    --infLeft;
                }
                for (size_t bi = 0; bi < buildings_.size() && infLeft > 0; ++bi) {
                    const auto& b = buildings_[bi];
                    if (b.destroyed || b.owner != v.owner || b.occupants.empty()) continue;
                    const double bReach = kSupportRadius + static_cast<double>(std::max(b.width, b.height)) * 0.5;
                    if (det::detHypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) > bReach) continue;
                    for (size_t sIdx : b.occupants) {
                        if (infLeft <= 0) break;
                        if (sIdx >= soldiers_.size()) continue;
                        auto& s = soldiers_[sIdx];
                        if (s.owner != v.owner || s.hp <= 0) continue;
                        const auto [didRes, gearChanged] = resupplySoldier(s);
                        (void)gearChanged;
                        if (!didRes) continue;
                        pushOrUpdateSoldierPose(changed, {
                            sIdx, b.x, b.y, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false,
                            0, 0, true, s.expa, s.kills, false, s.atGrenades, false, false, s.mineAmmo, s.ammo
                        });
                        didInfResupply = true;
                        didAnyService = true;
                        --infLeft;
                    }
                }
            }
            if (didAnyService && v.expaReload > 0) {
                const double vetFrac = static_cast<double>(std::clamp(v.expa, 0, 0x8000)) / 32768.0;
                const double xpUnits = static_cast<double>(v.expaReload) +
                                       vetFrac * static_cast<double>(v.expaReloadMax - v.expaReload);
                v.expa = std::min(0x8000, v.expa + std::max(1, static_cast<int>(det::detRoundToInt(xpUnits * 64.0))));
                truckStockChanged = true;
            }
            if (didInfResupply) {
                emitVehiclePose(i, false, 0, 0, false, {}, false, false, false, true);
            } else if (truckStockChanged) {
                emitVehiclePose(i);
            }
            if (canAutoDispatch && repairsLeft == 1 && resuppliesLeft == 2 && !didInfResupply) {
                std::optional<size_t> bestVeh;
                double bestDist = kAutoSupportRadius + 1.0;
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == i) continue;
                    const auto& other = vehicles_[j];
                    if (!other.alive || other.hp <= 0 || other.owner != v.owner) continue;
                    const bool canFixOther = !other.isCrate && other.hp < other.maxHp && (!v.hasFiniteSupply || v.ammo2 > 0);
                    const bool otherNeedsAmmo = !other.isSupply &&
                        ((other.maxAmmo > 0 && other.ammo < other.maxAmmo) ||
                         (other.maxAmmo2 > 0 && other.ammo2 < other.maxAmmo2));
                    const bool canFeedOther = otherNeedsAmmo && (!v.hasFiniteSupply || v.ammo > 0);
                    if (!canFixOther && !canFeedOther) {
                        continue;
                    }
                    const double d = det::detHypot(other.fx - v.fx, other.fy - v.fy);
                    if (d <= kAutoSupportRadius && d < bestDist) {
                        bestDist = d;
                        bestVeh = j;
                    }
                }
                if (bestVeh) {
                    const auto& t = vehicles_[*bestVeh];
                    const auto approach = approachPointNear(v.fx, v.fy, t.fx, t.fy, 64.0, mapPixelW_, mapPixelH_);
                    moveVehicleInternal(v, approach.first, approach.second);
                    v.supportTargetVehicle = *bestVeh;
                }
            }
        }
        if (v.isAmbulance) {
            int healsLeft = 2;
            const double healMult = veteranRateMult(v.healDelay, v.healDelayMax, v.expa);
            const int infHeal = std::max(1, static_cast<int>(det::detRoundToInt(12.0 * healMult)));
            const int crewHeal = std::max(1, static_cast<int>(det::detRoundToInt(45.0 * healMult)));
            bool didAmbulanceHeal = false;
            for (size_t sIdx = 0; sIdx < soldiers_.size() && healsLeft > 0; ++sIdx) {
                auto& s = soldiers_[sIdx];
                if (s.owner != v.owner || s.hp <= 0 || s.embarked || s.garrisonBuilding || s.hp >= s.maxHp) continue;
                const auto [su, sv] = soldierWorld(s);
                const int sx = originX_ + su - sv;
                const int sy = (su + sv) / 2;
                if (det::detHypot(double(sx) - v.fx, double(sy) - v.fy) > kSupportRadius) continue;
                s.hp = std::min(s.maxHp, s.hp + infHeal);
                pushHealed(sIdx, s);
                --healsLeft;
                didAmbulanceHeal = true;
            }
            for (size_t bi = 0; bi < buildings_.size() && healsLeft > 0; ++bi) {
                const auto& b = buildings_[bi];
                if (b.destroyed || b.owner != v.owner || b.occupants.empty()) continue;
                const double bReach = kSupportRadius + static_cast<double>(std::max(b.width, b.height)) * 0.5;
                if (det::detHypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) > bReach) continue;
                for (size_t sIdx : b.occupants) {
                    if (healsLeft <= 0) break;
                    if (sIdx >= soldiers_.size()) continue;
                    auto& s = soldiers_[sIdx];
                    if (s.owner != v.owner || s.hp <= 0 || s.hp >= s.maxHp) continue;
                    s.hp = std::min(s.maxHp, s.hp + infHeal);
                    pushOrUpdateSoldierPose(changed, {
                        sIdx, b.x, b.y, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false,
                        0, 0, true
                    });
                    --healsLeft;
                    didAmbulanceHeal = true;
                }
            }
            if (healsLeft > 0) {
                for (size_t j = 0; j < vehicles_.size() && healsLeft > 0; ++j) {
                    if (j == i) continue;
                    auto& other = vehicles_[j];
                    if (!other.alive || other.hp <= 0 || !other.hasCrew || other.owner != v.owner ||
                        other.crewHp >= other.maxCrewHp) {
                        continue;
                    }
                    if (det::detHypot(other.fx - v.fx, other.fy - v.fy) > kSupportRadius) continue;
                    other.crewHp = std::min(other.maxCrewHp, other.crewHp + crewHeal);
                    emitVehiclePose(j, false, 0, 0, false, {}, false, false, true, false);
                    --healsLeft;
                    didAmbulanceHeal = true;
                }
            }
            if (didAmbulanceHeal) {
                v.expa = std::min(0x8000, v.expa + 128);
                emitVehiclePose(i);
            }
            if (canAutoDispatch && healsLeft == 2) {
                std::optional<size_t> bestSoldier;
                double bestDist = kAutoSupportRadius + 1.0;
                double bestX = 0.0;
                double bestY = 0.0;
                for (size_t sIdx = 0; sIdx < soldiers_.size(); ++sIdx) {
                    const auto& s = soldiers_[sIdx];
                    if (s.owner != v.owner || s.hp <= 0 || s.embarked || s.garrisonBuilding || s.hp >= s.maxHp) {
                        continue;
                    }
                    const auto [su, sv] = soldierWorld(s);
                    const double sx = static_cast<double>(originX_ + su - sv);
                    const double sy = static_cast<double>(su + sv) * 0.5;
                    const double d = det::detHypot(sx - v.fx, sy - v.fy);
                    if (d <= kAutoSupportRadius && d < bestDist) {
                        bestDist = d;
                        bestSoldier = sIdx;
                        bestX = sx;
                        bestY = sy;
                    }
                }
                if (bestSoldier) {
                    const auto approach = approachPointNear(v.fx, v.fy, bestX, bestY, 56.0, mapPixelW_, mapPixelH_);
                    moveVehicleInternal(v, approach.first, approach.second);
                    v.supportTargetSoldier = *bestSoldier;
                }
            }
        }
    }
}

void TutorialUnits::stepSupportApproach() {
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (!s.healTarget) continue;
        const size_t tIdx = *s.healTarget;
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isMedic || tIdx >= soldiers_.size()) {
            s.healTarget.reset();
            continue;
        }
        const auto& t = soldiers_[tIdx];
        if (t.owner != s.owner || t.hp <= 0 || t.embarked || t.garrisonBuilding || t.hp >= t.maxHp) {
            s.healTarget.reset();
            continue;
        }
        const auto [su, sv] = soldierWorld(s);
        const auto [tu, tv] = soldierWorld(t);
        const double worldDist = det::detHypot(static_cast<double>(su - tu), static_cast<double>(sv - tv));
        if (worldDist <= 84.0) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
            s.healTarget.reset();
            continue;
        }
        if (!s.motion &&
            (!s.goal || (std::abs(s.goal->x / 2 - t.cell.x / 2) + std::abs(s.goal->y / 2 - t.cell.y / 2) > 3))) {
            const int tx = originX_ + tu - tv;
            const int ty = (tu + tv) / 2;
            if (orderSoldierInternal(i, tx, ty) && s.goal && !sameTile(*s.goal, s.cell)) {
                s.healTarget = tIdx;
            } else {
                s.healTarget.reset();
            }
        }
    }

    const auto haltVehicleSupport = [&](size_t idx, Vehicle& veh) {
        veh.moveTarget.reset();
        veh.waypoints.clear();
        veh.queuedOrders.clear();
        veh.supportTargetVehicle.reset();
        veh.supportTargetBuilding.reset();
        veh.supportTargetSoldier.reset();
        if (veh.wasMoving) {
            veh.wasMoving = false;
            emitVehiclePose(idx);
        }
    };

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.supportTargetVehicle && !v.supportTargetBuilding && !v.supportTargetSoldier) continue;
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy) {
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            continue;
        }
        if (v.supportTargetVehicle) {
            const size_t tIdx = *v.supportTargetVehicle;
            if (!v.isSupply || tIdx >= vehicles_.size()) {
                v.supportTargetVehicle.reset();
                continue;
            }
            const auto& t = vehicles_[tIdx];
            if (!t.alive || t.hp <= 0 || t.owner != v.owner ||
                !needsSupplyService(t.hp, t.maxHp, t.ammo, t.maxAmmo, t.ammo2, t.maxAmmo2)) {
                v.supportTargetVehicle.reset();
                continue;
            }
            const double dist = det::detHypot(t.fx - v.fx, t.fy - v.fy);
            if (dist <= 120.0) {
                haltVehicleSupport(i, v);
                continue;
            }
            if (!v.moveTarget ||
                det::detHypot(t.fx - static_cast<double>(v.moveTarget->first),
                           t.fy - static_cast<double>(v.moveTarget->second)) > 96.0) {
                const auto approach = approachPointNear(v.fx, v.fy, t.fx, t.fy, 64.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, approach.first, approach.second);
                v.supportTargetVehicle = tIdx;
            }
        } else if (v.supportTargetBuilding) {
            const size_t bIdx = *v.supportTargetBuilding;
            if ((!v.isSupply && !v.isAmbulance) || bIdx >= buildings_.size()) {
                v.supportTargetBuilding.reset();
                continue;
            }
            const auto& b = buildings_[bIdx];
            if (b.destroyed || (b.owner >= 0 && b.owner != v.owner)) {
                v.supportTargetBuilding.reset();
                continue;
            }
            bool needsBld = v.isSupply && (b.hp < b.maxHp);
            if (b.owner == v.owner) {
                for (size_t sIdx : b.occupants) {
                    if (sIdx >= soldiers_.size()) continue;
                    const auto& occ = soldiers_[sIdx];
                    if (occ.owner != v.owner || occ.hp <= 0) continue;
                    if (v.isAmbulance && occ.hp < occ.maxHp) needsBld = true;
                    if (v.isSupply) {
                        const bool canCarryGrenades = !occ.isAntiVehicle && !occ.isMedic && !occ.isOfficer && !occ.isMortar;
                        const int maxInfAmmo = std::max(1, occ.desc.maxAmmo);
                        const int occMaxMines = occ.maxMineAmmo > 0 ? occ.maxMineAmmo : 5;
                        if (occ.ammo < maxInfAmmo || (canCarryGrenades && occ.atGrenades < 2) ||
                            (occ.canLayMines && occ.mineAmmo < occMaxMines)) {
                            needsBld = true;
                        }
                    }
                }
            }
            if (!needsBld) {
                v.supportTargetBuilding.reset();
                continue;
            }
            const double bReach = 125.0 + static_cast<double>(std::max(b.width, b.height)) * 0.5;
            if (det::detHypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) <= bReach) {
                haltVehicleSupport(i, v);
            }
        } else if (v.supportTargetSoldier) {
            const size_t tIdx = *v.supportTargetSoldier;
            if (!v.isAmbulance || tIdx >= soldiers_.size()) {
                v.supportTargetSoldier.reset();
                continue;
            }
            const auto& t = soldiers_[tIdx];
            if (t.owner != v.owner || t.hp <= 0 || t.embarked || t.garrisonBuilding || t.hp >= t.maxHp) {
                v.supportTargetSoldier.reset();
                continue;
            }
            const auto [tu, tv] = soldierWorld(t);
            const double tx = static_cast<double>(originX_ + tu - tv);
            const double ty = static_cast<double>(tu + tv) * 0.5;
            if (det::detHypot(tx - v.fx, ty - v.fy) <= 120.0) {
                haltVehicleSupport(i, v);
                continue;
            }
            if (!v.moveTarget ||
                det::detHypot(tx - static_cast<double>(v.moveTarget->first),
                           ty - static_cast<double>(v.moveTarget->second)) > 84.0) {
                const auto approach = approachPointNear(v.fx, v.fy, tx, ty, 56.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, approach.first, approach.second);
                v.supportTargetSoldier = tIdx;
            }
        }
    }
}

void TutorialUnits::stepEscort() {
    const auto resolveEscortPoint = [&](int owner, std::optional<size_t> vehIdx, std::optional<size_t> solIdx,
                                        double& outX, double& outY) -> bool {
        if (vehIdx && *vehIdx < vehicles_.size()) {
            const auto& tv = vehicles_[*vehIdx];
            if (!tv.alive || tv.hp <= 0 || tv.owner != owner) return false;
            outX = tv.fx;
            outY = tv.fy;
            return true;
        }
        if (solIdx && *solIdx < soldiers_.size()) {
            const auto& ts = soldiers_[*solIdx];
            if (ts.owner != owner || ts.hp <= 0 || ts.embarked || ts.garrisonBuilding) return false;
            const auto [tu, tv] = soldierWorld(ts);
            outX = static_cast<double>(originX_ + tu - tv);
            outY = static_cast<double>(tu + tv) * 0.5;
            return true;
        }
        return false;
    };

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (!s.escortVehicleTarget && !s.escortSoldierTarget) continue;
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding) {
            s.escortVehicleTarget.reset();
            s.escortSoldierTarget.reset();
            s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            continue;
        }
        double tx = 0.0, ty = 0.0;
        if (!resolveEscortPoint(s.owner, s.escortVehicleTarget, s.escortSoldierTarget, tx, ty) ||
            (s.escortSoldierTarget && *s.escortSoldierTarget == i)) {
            s.escortVehicleTarget.reset();
            s.escortSoldierTarget.reset();
            s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            if (!s.goal) s.attackMove = false;
            continue;
        }
        const auto [su, sv] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sv);
        const double sy = static_cast<double>(su + sv) * 0.5;
        const double dist = det::detHypot(tx - sx, ty - sy);
        if (s.attackTarget || s.attackVehicleTarget || s.attackBuildingTarget || s.autoTarget) {
            if (s.autoTarget && !s.attackTarget && !s.attackVehicleTarget && !s.attackBuildingTarget &&
                dist > 260.0) {
                s.autoTarget.reset();
            } else {
                continue;
            }
        }
        if (dist <= 82.0) {
            if (!s.motion && s.goal) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                s.planning = {};
                s.needsRoute = false;
            }
            continue;
        }
        if (dist > 115.0 && !s.motion) {
            bool goalStale = false;
            if (s.goal) {
                const auto [gu, gv] = InfantryPathGrid::toWorldCenter(*s.goal);
                const double gx = static_cast<double>(originX_ + gu - gv);
                const double gy = static_cast<double>(gu + gv) * 0.5;
                goalStale = det::detHypot(tx - gx, ty - gy) > 72.0;
            }
            if (goalStale || (!s.goal && ((tick_ + i) % 6u == 0u))) {
                const auto savedVeh = s.escortVehicleTarget;
                const auto savedSol = s.escortSoldierTarget;
                const auto ap = approachPointNear(sx, sy, tx, ty, 52.0, mapPixelW_, mapPixelH_);
                (void)orderSoldierInternal(i, ap.first, ap.second);
                s.queuedGoals.clear();
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.escortVehicleTarget = savedVeh;
                s.escortSoldierTarget = savedSol;
                s.attackMove = true;
            }
        }
    }

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.escortVehicleTarget && !v.escortSoldierTarget) continue;
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy) {
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            continue;
        }
        double tx = 0.0, ty = 0.0;
        if (!resolveEscortPoint(v.owner, v.escortVehicleTarget, v.escortSoldierTarget, tx, ty) ||
            (v.escortVehicleTarget && (*v.escortVehicleTarget == i || v.towedVehicle == v.escortVehicleTarget))) {
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (!v.moveTarget) v.attackMove = false;
            continue;
        }
        const double dist = det::detHypot(tx - v.fx, ty - v.fy);
        if (v.attackMoveEngaging) {
            if (dist > 300.0) {
                v.attackMoveEngaging = false;
            } else {
                continue;
            }
        }
        if (dist <= 92.0) {
            if (v.moveTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(i);
                }
            }
            continue;
        }
        if (dist > 125.0) {
            const bool goalStale = v.moveTarget &&
                det::detHypot(tx - static_cast<double>(v.moveTarget->first),
                           ty - static_cast<double>(v.moveTarget->second)) > 96.0;
            if (goalStale || (!v.moveTarget && ((tick_ + i) % 6u == 0u))) {
                const auto savedVeh = v.escortVehicleTarget;
                const auto savedSol = v.escortSoldierTarget;
                const auto ap = approachPointNear(v.fx, v.fy, tx, ty, 76.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, ap.first, ap.second);
                v.escortVehicleTarget = savedVeh;
                v.escortSoldierTarget = savedSol;
                v.attackMove = true;
            }
        }
    }
}
}
