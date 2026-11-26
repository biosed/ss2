#include "sim/tutorial.hpp"
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
size_t TutorialUnits::orderGroupMove(unsigned group, int mapX, int mapY, bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;

    std::vector<size_t> soldierIds;
    std::vector<SubCell> soldierSources;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        soldierIds.push_back(i);
        soldierSources.push_back(s.cell);
    }
    if (!soldierIds.empty()) {
        const auto destCell = findSpawnCell(grid_, pointerCell(clampedX, clampedY, originX_));
        const auto tiles = grid_.formationDestinations(soldierSources, destCell);
        for (size_t i = 0; i < soldierIds.size(); ++i) {
            const int tx = tiles[i].x / 2, ty = tiles[i].y / 2;
            const int px = originX_ + (tx - ty) * 32;
            const int py = (tx + ty) * 16 + 16;
            if (orderSoldierInternal(soldierIds[i], px, py)) {
                soldiers_[soldierIds[i]].attackMove = true;
                ++accepted;
            }
        }
    }

    std::vector<size_t> vehIds;
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        vehIds.push_back(i);
    }
    for (const auto& [vid, pt] : vehicleFormationTargets(vehIds, clampedX, clampedY)) {
        auto& v = vehicles_[vid];
        moveVehicleInternal(v, pt.first, pt.second);
        v.attackMove = true;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderGroupAttackGround(unsigned group, int mapX, int mapY, bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (!v.armed || !v.hasPrimary || (v.ammoPerShot1 != 0 && v.ammo <= 0)) continue;
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
        v.supportTargetBridge.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        ++accepted;
    }
    if (accepted == 0) {
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
            if (onlyNonPlayer && s.owner == 0) continue;
            const bool mortarOrFlamer = (s.isMortar || s.isFlamer) && s.ammo > 0;
            const bool smgGrenade = s.canGrenadeGround && s.atGrenades > 0;
            if (!mortarOrFlamer && !smgGrenade) continue;
            s.attackTarget.reset();
            s.attackVehicleTarget.reset();
            s.attackBuildingTarget.reset();
            s.autoTarget.reset();
            s.boardVehicleTarget.reset();
            s.captureVehicleTarget.reset();
            s.garrisonTarget.reset();
            s.healTarget.reset();
            s.escortVehicleTarget.reset();
            s.escortSoldierTarget.reset();
            s.disarmMineTarget.reset();
            s.disarmWorkTicks = 0;
            s.patrolOrigin.reset();
            s.patrolDest.reset();
            s.queuedGoals.clear();
            s.motion.reset();
            clearMovingSightPenalty(s);
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.attackMove = false;
            s.attackGround = std::pair{clampedX, clampedY};
            ++accepted;
        }
    }
    return accepted;
}

void TutorialUnits::setGroupHoldFire(unsigned group, bool holdFire, bool onlyNonPlayer) {
    const int g = int(group);
    for (auto& s : soldiers_) {
        if (s.hp <= 0 || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        s.holdFire = holdFire;
    }
    for (auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        v.holdFire = holdFire;
    }
}

std::optional<int> TutorialUnits::groupOwner(unsigned group, bool onlyNonPlayer) const {
    const int g = int(group);
    for (const auto& s : soldiers_) {
        if (s.hp <= 0 || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        if (s.owner >= 0 && s.owner < 4 && (multiplayerSymmetry_ || s.owner != 3)) return s.owner;
    }
    std::optional<int> fallback;
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (v.owner >= 0 && v.owner < 4) {
            if (multiplayerSymmetry_ || v.owner != 3) return v.owner;
            if (!fallback) fallback = v.owner;
        }
    }
    return fallback;
}

bool TutorialUnits::checkZoneGate(std::span<const std::uint8_t> zones, unsigned zone, int zoneX, int zoneY,
                                  int owner, std::uint32_t aiFlags,
                                  std::uint32_t haveOwnBit, std::uint32_t noEnemyBit) const {
    const bool reqOwn = (aiFlags & haveOwnBit) != 0u;
    const bool reqNoEnemy = (aiFlags & noEnemyBit) != 0u;
    if (!reqOwn && !reqNoEnemy) return true;
    if (zone == 0u || owner < 0 || owner >= 4) return true;

    const std::size_t tileW = std::size_t(grid_.width() / 2);
    const std::size_t tileH = std::size_t(grid_.height() / 2);
    bool hasPaintedTiles = false;
    if (zones.size() == tileW * tileH) {
        for (const std::uint8_t zByte : zones) {
            if ((zByte & 63u) == zone) {
                hasPaintedTiles = true;
                break;
            }
        }
    }

    const auto cellInZone = [&](SubCell c, int px, int py) -> bool {
        if (hasPaintedTiles) {
            if (c.x < 0 || c.y < 0) return false;
            const std::size_t tx = std::size_t(c.x / 2);
            const std::size_t ty = std::size_t(c.y / 2);
            return tx < tileW && ty < tileH && (zones[ty * tileW + tx] & 63u) == zone;
        }
        return det::detHypot(static_cast<double>(px - zoneX), static_cast<double>(py - zoneY)) <= 224.0;
    };

    unsigned ownCount = 0u;
    unsigned enemyCount = 0u;
    for (const auto& s : soldiers_) {
        if (s.hp <= 0 || s.embarked) continue;
        SubCell c = s.cell;
        int sx = 0, sy = 0;
        if (s.garrisonBuilding && *s.garrisonBuilding < buildings_.size()) {
            const auto& b = buildings_[*s.garrisonBuilding];
            c = b.entryCell;
            sx = b.x;
            sy = b.y;
        } else {
            const auto [su, sv] = soldierWorld(s);
            sx = originX_ + su - sv;
            sy = (su + sv) / 2;
        }
        if (!cellInZone(c, sx, sy)) continue;
        if (isAlliedFor(owner, s.owner)) ++ownCount;
        else if (isHostile(owner, s.owner)) ++enemyCount;
    }
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || v.isCrate || (!v.hasCrew && v.passengers.empty())) continue;
        if (!cellInZone(v.cell, v.x, v.y)) continue;
        if (isAlliedFor(owner, v.owner)) ++ownCount;
        else if (isHostile(owner, v.owner)) ++enemyCount;
    }

    if (reqOwn && ownCount == 0u) return false;
    if (reqNoEnemy && enemyCount > 0u) return false;
    return true;
}

void TutorialUnits::stopGroupUnitsMovingTo(unsigned group, int targetX, int targetY, double radiusPx,
                                           bool onlyNonPlayer) {
    const int g = int(group);
    for (auto& s : soldiers_) {
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        if (!s.goal) continue;
        const auto [gu, gv] = InfantryPathGrid::toWorldCenter(*s.goal);
        const int gx = originX_ + gu - gv;
        const int gy = (gu + gv) / 2;
        if (det::detHypot(static_cast<double>(gx - targetX), static_cast<double>(gy - targetY)) <= radiusPx) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.queuedGoals.clear();
            s.attackMove = false;
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (!v.moveTarget) continue;
        if (det::detHypot(static_cast<double>(v.moveTarget->first - targetX),
                          static_cast<double>(v.moveTarget->second - targetY)) <= radiusPx) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.queuedOrders.clear();
            v.attackMove = false;
            v.attackMoveEngaging = false;
            v.reverseMove = false;
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(i);
            }
        }
    }
}

std::optional<std::pair<int, int>> TutorialUnits::groupCentroid(unsigned group, bool onlyNonPlayer) const {
    const int g = int(group);
    std::int64_t sumX = 0;
    std::int64_t sumY = 0;
    std::int64_t n = 0;
    for (const auto& s : soldiers_) {
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        const auto [su, sv] = soldierWorld(s);
        sumX += originX_ + su - sv;
        sumY += (su + sv) / 2;
        ++n;
    }
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        sumX += v.x;
        sumY += v.y;
        ++n;
    }
    if (n == 0) return std::nullopt;
    return std::pair<int, int>{static_cast<int>(sumX / n), static_cast<int>(sumY / n)};
}

size_t TutorialUnits::orderGroupZoneBehavior(unsigned group, std::uint32_t behavior, std::uint32_t aiFlags,
                                             int mapX, int mapY, bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    const bool holdFire = (aiFlags & (1u << 22)) != 0u;
    if (holdFire) setGroupHoldFire(group, true, onlyNonPlayer);

    const bool wantGarrison = (behavior == 2u) &&
        ((aiFlags & (1u << 19)) == 0u) &&
        ((aiFlags & ((1u << 3) | (1u << 20) | (1u << 21))) != 0u) &&
        !buildings_.empty();

    std::vector<int> pendingGarrisons;
    if (wantGarrison) {
        pendingGarrisons.assign(buildings_.size(), 0);
        for (const auto& s : soldiers_) {
            if (s.hp > 0 && s.garrisonTarget && *s.garrisonTarget < buildings_.size()) {
                ++pendingGarrisons[*s.garrisonTarget];
            }
        }
    }

    auto isEvacSoldier = [&](size_t idx) {
        return std::ranges::any_of(evacuatingSoldiers_, [&](const auto& eo) { return eo.index == idx; });
    };
    auto isEvacVehicle = [&](size_t idx) {
        return std::ranges::any_of(evacuatingVehicles_, [&](const auto& eo) { return eo.index == idx; });
    };

    size_t accepted = 0;
    std::vector<size_t> soldierIds;
    std::vector<SubCell> soldierSources;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        if (s.motion || s.goal || !s.route.empty() ||
            s.attackTarget || s.attackVehicleTarget || s.attackBuildingTarget ||
            s.garrisonTarget || s.boardVehicleTarget || isEvacSoldier(i)) {
            continue;
        }
        if (wantGarrison) {
            std::optional<size_t> bestBld;
            double bestDist = 240.0 + 1.0;
            for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                const auto& b = buildings_[bi];
                if (b.destroyed || (b.owner >= 0 && b.owner != s.owner)) continue;
                if (static_cast<int>(b.occupants.size()) + pendingGarrisons[bi] >= b.capacity) continue;
                const double d = det::detHypot(static_cast<double>(b.x - clampedX),
                                               static_cast<double>(b.y - clampedY));
                if (d <= 240.0 && d < bestDist) {
                    bestDist = d;
                    bestBld = bi;
                }
            }
            if (bestBld) {
                const auto& b = buildings_[*bestBld];
                if (orderSoldierInternal(i, b.x, b.y)) {
                    soldiers_[i].garrisonTarget = *bestBld;
                    ++pendingGarrisons[*bestBld];
                    ++accepted;
                    continue;
                }
            }
        }
        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        if (det::detHypot(static_cast<double>(clampedX - sx), static_cast<double>(clampedY - sy)) > 192.0) {
            soldierIds.push_back(i);
            soldierSources.push_back(s.cell);
        }
    }
    if (!soldierIds.empty()) {
        const auto destCell = findSpawnCell(grid_, pointerCell(clampedX, clampedY, originX_));
        const auto tiles = grid_.formationDestinations(soldierSources, destCell);
        for (size_t i = 0; i < soldierIds.size(); ++i) {
            const int tx = tiles[i].x / 2, ty = tiles[i].y / 2;
            const int px = originX_ + (tx - ty) * 32;
            const int py = (tx + ty) * 16 + 16;
            if (orderSoldierInternal(soldierIds[i], px, py)) {
                soldiers_[soldierIds[i]].attackMove = !holdFire;
                ++accepted;
            }
        }
    }

    std::vector<size_t> vehIds;
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (v.moveTarget || !v.waypoints.empty() || v.attackTarget || v.attackBuildingTarget ||
            v.attackGround || v.supportTargetVehicle || v.supportTargetBuilding ||
            v.supportTargetBridge || v.supportTargetSoldier || v.escortVehicleTarget ||
            v.escortSoldierTarget || isEvacVehicle(i)) {
            continue;
        }
        if (det::detHypot(static_cast<double>(clampedX) - v.fx, static_cast<double>(clampedY) - v.fy) > 192.0) {
            vehIds.push_back(i);
        }
    }
    for (const auto& [vid, pt] : vehicleFormationTargets(vehIds, clampedX, clampedY)) {
        auto& v = vehicles_[vid];
        moveVehicleInternal(v, pt.first, pt.second);
        v.attackMove = !holdFire;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderGroupSupport(unsigned group, unsigned targetGroup, int fallbackX, int fallbackY,
                                        bool hasFallback, bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedX = std::clamp(fallbackX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(fallbackY, 0, std::max(0, mapPixelH_ - 1));
    const auto fallbackAnchor = targetGroup > 0u
        ? groupCentroid(targetGroup, onlyNonPlayer)
        : (hasFallback ? std::optional<std::pair<int, int>>{std::pair{clampedX, clampedY}} : std::nullopt);
    size_t accepted = 0;

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        const bool isSup = v.isSupply && !v.isCrate;
        if (!isSup && !v.isAmbulance) continue;
        if (v.moveTarget || !v.waypoints.empty() || v.supportTargetVehicle || v.supportTargetBuilding ||
            v.supportTargetBridge || v.supportTargetSoldier || v.escortVehicleTarget || v.escortSoldierTarget) {
            continue;
        }
        if (isSup) {
            std::optional<size_t> bestVeh;
            double bestDist = 1e18;
            for (size_t j = 0; j < vehicles_.size(); ++j) {
                if (j == i) continue;
                const auto& tv = vehicles_[j];
                if (!tv.alive || tv.hp <= 0 || tv.owner != v.owner || tv.isCrate) continue;
                if (targetGroup > 0u) {
                    if (tv.group != static_cast<int>(targetGroup)) continue;
                } else if (!hasFallback ||
                           det::detHypot(tv.fx - static_cast<double>(clampedX),
                                         tv.fy - static_cast<double>(clampedY)) > 480.0) {
                    continue;
                }
                const bool needsFix = tv.hp < tv.maxHp && (!v.hasFiniteSupply || v.ammo2 > 0);
                const bool needsAmmo = ((tv.maxAmmo > 0 && tv.ammo < tv.maxAmmo) ||
                                        (tv.maxAmmo2 > 0 && tv.ammo2 < tv.maxAmmo2)) &&
                                       (!v.hasFiniteSupply || v.ammo > 0);
                if (!needsFix && !needsAmmo) continue;
                const double d = det::detHypot(tv.fx - v.fx, tv.fy - v.fy);
                if (d < bestDist) {
                    bestDist = d;
                    bestVeh = j;
                }
            }
            if (bestVeh) {
                const auto& tv = vehicles_[*bestVeh];
                const auto approach = approachPointNear(v.fx, v.fy, tv.fx, tv.fy, 64.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, approach.first, approach.second);
                v.supportTargetVehicle = *bestVeh;
                ++accepted;
                continue;
            }
            if (hasFallback && (!v.hasFiniteSupply || v.ammo2 > 0)) {
                std::optional<size_t> bestBr;
                double bestBrDist = 1e18;
                for (size_t bi = 0; bi < bridges_.size(); ++bi) {
                    const auto& br = bridges_[bi];
                    if (br.hp >= br.maxHp) continue;
                    if (distanceToBridge(br, static_cast<double>(clampedX), static_cast<double>(clampedY)) > 480.0) {
                        continue;
                    }
                    const double d = distanceToBridge(br, v.fx, v.fy);
                    if (d < bestBrDist) {
                        bestBrDist = d;
                        bestBr = bi;
                    }
                }
                if (bestBr) {
                    const auto approach = bridgeRepairApproachPoint(bridges_[*bestBr], v);
                    moveVehicleInternal(v, approach.first, approach.second);
                    v.supportTargetBridge = *bestBr;
                    ++accepted;
                    continue;
                }
            }
        } else if (v.isAmbulance) {
            std::optional<size_t> bestSol;
            double bestDist = 1e18;
            int bestTx = 0, bestTy = 0;
            for (size_t si = 0; si < soldiers_.size(); ++si) {
                const auto& ts = soldiers_[si];
                if (ts.owner != v.owner || ts.hp <= 0 || ts.embarked || ts.garrisonBuilding || ts.hp >= ts.maxHp) {
                    continue;
                }
                const auto [tu, tv] = soldierWorld(ts);
                const int tx = originX_ + tu - tv;
                const int ty = (tu + tv) / 2;
                if (targetGroup > 0u) {
                    if (ts.group != static_cast<int>(targetGroup)) continue;
                } else if (!hasFallback ||
                           det::detHypot(static_cast<double>(tx - clampedX),
                                         static_cast<double>(ty - clampedY)) > 480.0) {
                    continue;
                }
                const double d = det::detHypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
                if (d < bestDist) {
                    bestDist = d;
                    bestSol = si;
                    bestTx = tx;
                    bestTy = ty;
                }
            }
            if (bestSol) {
                const auto approach = approachPointNear(
                    v.fx, v.fy, static_cast<double>(bestTx), static_cast<double>(bestTy),
                    56.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, approach.first, approach.second);
                v.supportTargetSoldier = *bestSol;
                ++accepted;
                continue;
            }
        }
        const auto anchor = fallbackAnchor ? fallbackAnchor
            : (hasFallback ? std::optional<std::pair<int, int>>{std::pair{clampedX, clampedY}} : std::nullopt);
        if (anchor &&
            det::detHypot(static_cast<double>(anchor->first) - v.fx,
                          static_cast<double>(anchor->second) - v.fy) > 180.0) {
            const auto approach = approachPointNear(
                v.fx, v.fy, static_cast<double>(anchor->first), static_cast<double>(anchor->second),
                64.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
            ++accepted;
        }
    }
    return accepted;
}

size_t TutorialUnits::orderGroupSoldArt(unsigned group, std::uint32_t aiFlags,
                                        std::span<const std::uint8_t> zones,
                                        unsigned zone1, int z1x, int z1y, bool hasZone1,
                                        unsigned zone2, int z2x, int z2y, bool hasZone2,
                                        bool onlyNonPlayer) {
    (void)zone2;
    (void)z2x;
    (void)z2y;
    (void)hasZone2;
    const int g = int(group);
    const int clampedZ1X = hasZone1 ? std::clamp(z1x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ1Y = hasZone1 ? std::clamp(z1y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const bool holdFire = (aiFlags & igroups::kAifHoldFire) != 0u;
    if (holdFire) setGroupHoldFire(group, true, onlyNonPlayer);

    const std::size_t tileW = std::size_t(grid_.width() / 2);
    const std::size_t tileH = std::size_t(grid_.height() / 2);
    bool z1Painted = false;
    if (hasZone1 && zones.size() == tileW * tileH) {
        for (const std::uint8_t zb : zones) {
            if ((zb & 63u) == zone1) {
                z1Painted = true;
                break;
            }
        }
    }
    const auto inZone1 = [&](SubCell c, int px, int py) {
        if (!hasZone1) return false;
        if (z1Painted) {
            if (c.x < 0 || c.y < 0) return false;
            const std::size_t tx = std::size_t(c.x / 2);
            const std::size_t ty = std::size_t(c.y / 2);
            if (tx < tileW && ty < tileH && (zones[ty * tileW + tx] & 63u) == zone1) return true;
        }
        return det::detHypot(static_cast<double>(px - clampedZ1X),
                             static_cast<double>(py - clampedZ1Y)) <= 384.0;
    };

    const auto isEvacSoldier = [&](size_t idx) {
        return std::ranges::any_of(evacuatingSoldiers_, [&](const auto& eo) { return eo.index == idx; });
    };
    const auto isEvacVehicle = [&](size_t idx) {
        return std::ranges::any_of(evacuatingVehicles_, [&](const auto& eo) { return eo.index == idx; });
    };

    const auto classifyGun = [this](const Vehicle& v, bool& outIsGaub, bool& outIsAtg) {
        const auto* spec = catalog_ ? catalog_->find(v.id) : nullptr;
        outIsGaub = v.indirectArc1 || v.deadzone > 0.0 ||
                    (spec && (spec->selType == "howitzer" || spec->selType == "mortar" ||
                              spec->selType == "mobmortar" || spec->roleClass == "mortar" ||
                              spec->turretType == "gaub" || spec->turretType == "atg_gaub"));
        outIsAtg = !outIsGaub && v.armed &&
                   (v.speedPx <= 0.0 || !v.towWeightClass.empty() ||
                    (spec && (spec->selType == "atg" || spec->roleClass == "at" ||
                              spec->turretType == "atg" || spec->isAntiAir())));
    };

    size_t accepted = 0;

    // 1. Infantry crewing unmanned artillery/guns/vehicles (0x1004b7e0 -> order 0x0c) & zone1 guard.
    bool hasGroupSoldiers = false;
    for (const auto& s : soldiers_) {
        if (s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.group == g && (!onlyNonPlayer || s.owner != 0)) {
            hasGroupSoldiers = true;
            break;
        }
    }
    if (hasGroupSoldiers) {
        std::vector<bool> claimedVeh(vehicles_.size(), false);
        for (const auto& s : soldiers_) {
            if (s.hp > 0 && s.captureVehicleTarget && *s.captureVehicleTarget < vehicles_.size()) {
                claimedVeh[*s.captureVehicleTarget] = true;
            }
        }

        const bool wantGarrison = hasZone1 &&
            ((aiFlags & igroups::kAifDomNoUse) == 0u) &&
            ((aiFlags & (igroups::kAifUseObjects | igroups::kAifDomAmbush | igroups::kAifDomHide)) != 0u) &&
            !buildings_.empty();
        std::vector<int> pendingGarrisons;
        if (wantGarrison) {
            pendingGarrisons.assign(buildings_.size(), 0);
            for (const auto& s : soldiers_) {
                if (s.hp > 0 && s.garrisonTarget && *s.garrisonTarget < buildings_.size()) {
                    ++pendingGarrisons[*s.garrisonTarget];
                }
            }
        }

        std::vector<size_t> moveSoldierIds;
        std::vector<SubCell> moveSoldierSources;
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
            if (onlyNonPlayer && s.owner == 0) continue;
            if (s.captureVehicleTarget || s.boardVehicleTarget || s.garrisonTarget ||
                s.attackTarget || s.attackVehicleTarget || s.attackBuildingTarget || isEvacSoldier(i)) {
                continue;
            }

            const auto [su, sv] = soldierWorld(s);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;

            std::optional<size_t> bestGun;
            int bestTier = 99;
            double bestDist = 1e18;
            for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
                if (claimedVeh[vid]) continue;
                const auto& v = vehicles_[vid];
                if (!v.alive || v.hp <= 0 || v.hasCrew || v.isCrate || v.isNaval || v.isTrain) continue;
                const bool ownerOk = isAlliedFor(s.owner, v.owner) ||
                                     (v.owner == 3 && !multiplayerSymmetry_);
                if (!ownerOk) continue;

                bool isGaub = false;
                bool isAtg = false;
                classifyGun(v, isGaub, isAtg);
                const bool isMobile = !isGaub && !isAtg && v.speedPx > 0.0;
                if (isAtg && (aiFlags & igroups::kAifAtgNoUse) != 0u) continue;
                if (isGaub && (aiFlags & igroups::kAifGaubNoUse) != 0u) continue;
                if (isMobile && (aiFlags & igroups::kAifEzdNoUse) != 0u) continue;
                if (!isGaub && !isAtg && !isMobile) continue;

                if (hasZone1) {
                    const std::uint32_t ownBit = igroups::kAifZoneHaveOwn |
                        (isAtg ? igroups::kAifAtgZoneHaveOwn : (isGaub ? igroups::kAifGaubZoneHaveOwn : 0u));
                    const std::uint32_t noEnBit = igroups::kAifZoneNoEnemy |
                        (isAtg ? igroups::kAifAtgZoneNoEnemy : (isGaub ? igroups::kAifGaubZoneNoEnemy : 0u));
                    if (!checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, s.owner, aiFlags, ownBit, noEnBit)) {
                        continue;
                    }
                }

                const double distSoldier = det::detHypot(static_cast<double>(v.x - sx),
                                                         static_cast<double>(v.y - sy));
                const bool inScope = (v.group == g) ||
                                     (hasZone1 ? inZone1(v.cell, v.x, v.y) : (distSoldier <= 384.0));
                if (!inScope) continue;
                if (!sameTile(s.cell, v.cell) && !grid_.hasTerrainRoute(s.cell, v.cell)) continue;

                const int tier = (isGaub || isAtg) ? ((v.group == g) ? 0 : 1) : 2;
                if (tier < bestTier || (tier == bestTier && distSoldier < bestDist)) {
                    bestTier = tier;
                    bestDist = distSoldier;
                    bestGun = vid;
                }
            }

            if (bestGun) {
                const auto& gv = vehicles_[*bestGun];
                if (sameTile(s.cell, gv.cell) || orderSoldierInternal(i, gv.x, gv.y)) {
                    s.captureVehicleTarget = *bestGun;
                    s.attackMove = false;
                    claimedVeh[*bestGun] = true;
                    ++accepted;
                    continue;
                }
            }

            if (!hasZone1) continue;
            if (!checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, s.owner, aiFlags,
                               igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 224.0, onlyNonPlayer);
                continue;
            }
            if (s.motion || s.goal || !s.route.empty()) continue;

            if (wantGarrison) {
                std::optional<size_t> bestBld;
                double bestBldDist = 241.0;
                for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                    const auto& b = buildings_[bi];
                    if (b.destroyed || (b.owner >= 0 && b.owner != s.owner)) continue;
                    if (static_cast<int>(b.occupants.size()) + pendingGarrisons[bi] >= b.capacity) continue;
                    const double d = det::detHypot(static_cast<double>(b.x - clampedZ1X),
                                                   static_cast<double>(b.y - clampedZ1Y));
                    if (d <= 240.0 && d < bestBldDist) {
                        bestBldDist = d;
                        bestBld = bi;
                    }
                }
                if (bestBld) {
                    const auto& b = buildings_[*bestBld];
                    if (orderSoldierInternal(i, b.x, b.y)) {
                        s.garrisonTarget = *bestBld;
                        ++pendingGarrisons[*bestBld];
                        ++accepted;
                        continue;
                    }
                }
            }

            if (det::detHypot(static_cast<double>(clampedZ1X - sx),
                              static_cast<double>(clampedZ1Y - sy)) > 192.0) {
                moveSoldierIds.push_back(i);
                moveSoldierSources.push_back(s.cell);
            }
        }

        if (!moveSoldierIds.empty()) {
            const auto destCell = findSpawnCell(grid_, pointerCell(clampedZ1X, clampedZ1Y, originX_));
            const auto tiles = grid_.formationDestinations(moveSoldierSources, destCell);
            for (size_t k = 0; k < moveSoldierIds.size(); ++k) {
                const int tx = tiles[k].x / 2, ty = tiles[k].y / 2;
                const int px = originX_ + (tx - ty) * 32;
                const int py = (tx + ty) * 16 + 16;
                if (orderSoldierInternal(moveSoldierIds[k], px, py)) {
                    soldiers_[moveSoldierIds[k]].attackMove = !holdFire;
                    ++accepted;
                }
            }
        }
    }

    // 2. Crewed guns / artillery / mobile vehicles in group (0x10050af0 unit_gaub_AI & 0x100501f0 unit_gun_AI).
    std::vector<size_t> moveVehIds;
    for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (isEvacVehicle(vid)) continue;

        bool isGaub = false;
        bool isAtg = false;
        classifyGun(v, isGaub, isAtg);

        if (v.speedPx > 0.0 && hasZone1) {
            const bool noMove = (isAtg && (aiFlags & igroups::kAifAtgNoMove) != 0u) ||
                                (isGaub && (aiFlags & igroups::kAifGaubNoMove) != 0u);
            if (!noMove) {
                const std::uint32_t ownBit = igroups::kAifZoneHaveOwn |
                    (isAtg ? igroups::kAifAtgZoneHaveOwn : (isGaub ? igroups::kAifGaubZoneHaveOwn : 0u));
                const std::uint32_t noEnBit = igroups::kAifZoneNoEnemy |
                    (isAtg ? igroups::kAifAtgZoneNoEnemy : (isGaub ? igroups::kAifGaubZoneNoEnemy : 0u));
                if (!checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, v.owner, aiFlags, ownBit, noEnBit)) {
                    stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 224.0, onlyNonPlayer);
                } else if (!v.moveTarget && v.waypoints.empty() && !v.attackTarget && !v.attackGround &&
                           !v.attackBuildingTarget &&
                           det::detHypot(static_cast<double>(clampedZ1X) - v.fx,
                                         static_cast<double>(clampedZ1Y) - v.fy) > 192.0) {
                    moveVehIds.push_back(vid);
                }
            }
        }

        // Engage spotted hostiles in [deadzone, range] when stationary and free to fire.
        if (fog_ && v.armed && !v.holdFire && !holdFire && !v.moveTarget &&
            !v.attackTarget && !v.attackGround && !v.attackBuildingTarget &&
            ((v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0)) ||
             (v.hasSecondary && (v.ammoPerShot2 == 0 || v.ammo2 > 0)))) {
            const double maxR = std::max(v.hasPrimary ? v.range : 0.0, v.hasSecondary ? v.range2 : 0.0);
            const double minR = v.hasPrimary ? v.deadzone : 0.0;
            std::optional<std::pair<bool, size_t>> bestTgt;
            double bestDist = 1e18;
            for (size_t j = 0; j < vehicles_.size(); ++j) {
                if (j == vid || !isVehicleTargetValidFor(v.owner, true, j)) continue;
                const double d = det::detHypot(vehicles_[j].fx - v.fx, vehicles_[j].fy - v.fy);
                if (d >= minR && d <= maxR && d < bestDist) {
                    bestDist = d;
                    bestTgt = std::pair{true, j};
                }
            }
            for (size_t j = 0; j < soldiers_.size(); ++j) {
                if (!isVehicleTargetValidFor(v.owner, false, j)) continue;
                const auto [tu, tv] = soldierWorld(soldiers_[j]);
                const int tx = originX_ + tu - tv;
                const int ty = (tu + tv) / 2;
                const double d = det::detHypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
                if (d >= minR && d <= maxR && d < bestDist) {
                    bestDist = d;
                    bestTgt = std::pair{false, j};
                }
            }
            if (bestTgt) {
                v.attackTarget = bestTgt;
                ++accepted;
            } else if (isGaub && v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0)) {
                // Counter-battery: shell enemy artillery revealing its muzzle flash in range.
                std::optional<std::pair<int, int>> cbTarget;
                double cbDist = 1e18;
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == vid) continue;
                    const auto& tv = vehicles_[j];
                    if (!tv.alive || tv.hp <= 0 || !isHostile(v.owner, tv.owner) || tv.muzzleRevealTicks == 0u) continue;
                    const double d = det::detHypot(tv.fx - v.fx, tv.fy - v.fy);
                    if (d < minR || d > maxR || d >= cbDist) continue;
                    bool nearAlly = false;
                    for (size_t k = 0; k < vehicles_.size() && !nearAlly; ++k) {
                        const auto& av = vehicles_[k];
                        if (av.alive && av.hp > 0 && isAlliedFor(v.owner, av.owner) &&
                            det::detHypot(av.fx - tv.fx, av.fy - tv.fy) <= 128.0) {
                            nearAlly = true;
                        }
                    }
                    if (!nearAlly) {
                        cbDist = d;
                        cbTarget = std::pair{tv.x, tv.y};
                    }
                }
                if (cbTarget) {
                    v.attackGround = cbTarget;
                    ++accepted;
                }
            }
        }
        // Clear stale AI counter-battery attackGround once target flash expires & no hostiles remain near it.
        if (isGaub && v.attackGround && v.owner > 0) {
            const auto [gx, gy] = *v.attackGround;
            const bool activeThreat = std::ranges::any_of(vehicles_, [&](const Vehicle& tv) {
                return tv.alive && tv.hp > 0 && isHostile(v.owner, tv.owner) &&
                       (tv.muzzleRevealTicks > 0u || (tv.spottedMask & (1u << v.owner)) != 0u) &&
                       det::detHypot(tv.fx - static_cast<double>(gx), tv.fy - static_cast<double>(gy)) <= 96.0;
            });
            if (!activeThreat) {
                v.attackGround.reset();
            }
        }
    }

    for (const auto& [vid, pt] : vehicleFormationTargets(moveVehIds, clampedZ1X, clampedZ1Y)) {
        auto& v = vehicles_[vid];
        moveVehicleInternal(v, pt.first, pt.second);
        v.attackMove = !holdFire;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderGroupRecon(unsigned group, std::uint32_t aiFlags,
                                      std::span<const std::uint8_t> zones,
                                      unsigned zone1, int z1x, int z1y, bool hasZone1,
                                      unsigned zone2, int z2x, int z2y, bool hasZone2,
                                      bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedZ1X = hasZone1 ? std::clamp(z1x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ1Y = hasZone1 ? std::clamp(z1y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const int clampedZ2X = hasZone2 ? std::clamp(z2x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ2Y = hasZone2 ? std::clamp(z2y, 0, std::max(0, mapPixelH_ - 1)) : 0;

    // Recon units hold fire so they spot without exposing themselves or stopping to duel.
    setGroupHoldFire(group, true, onlyNonPlayer);

    const auto hitTick = lastGroupHitTick(group);
    const bool groupRecentlyHit = hitTick.has_value() && tick_ <= *hitTick + 125u;

    const auto nearestSpottedEnemy = [&](int viewer, double ux, double uy,
                                         int& outEx, int& outEy) -> double {
        double bestD = 1e18;
        for (size_t j = 0; j < soldiers_.size(); ++j) {
            const auto& es = soldiers_[j];
            if (es.hp <= 0 || es.embarked || es.garrisonBuilding || !isHostile(viewer, es.owner)) continue;
            if (fog_ && !isValidAttackTarget(j, viewer)) continue;
            const auto [eu, ev] = soldierWorld(es);
            const int ex = originX_ + eu - ev;
            const int ey = (eu + ev) / 2;
            const double d = det::detHypot(static_cast<double>(ex) - ux, static_cast<double>(ey) - uy);
            if (d < bestD) {
                bestD = d;
                outEx = ex;
                outEy = ey;
            }
        }
        for (size_t j = 0; j < vehicles_.size(); ++j) {
            const auto& ev = vehicles_[j];
            if (!ev.alive || ev.hp <= 0 || (!ev.hasCrew && ev.passengers.empty()) ||
                !isHostile(viewer, ev.owner)) {
                continue;
            }
            if (fog_ && !isVehicleTargetValidFor(viewer, true, j)) continue;
            const double d = det::detHypot(ev.fx - ux, ev.fy - uy);
            if (d < bestD) {
                bestD = d;
                outEx = ev.x;
                outEy = ev.y;
            }
        }
        return bestD;
    };

    const auto computeRetreatPoint = [&](int owner, double ux, double uy,
                                         double threatDist, int threatX, int threatY) -> std::pair<int, int> {
        if (hasZone2 && (!hasZone1 || zone2 != zone1) &&
            checkZoneGate(zones, zone2, clampedZ2X, clampedZ2Y, owner, aiFlags,
                          igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
            return {clampedZ2X, clampedZ2Y};
        }
        if (threatDist < 1e8) {
            const double dx = ux - static_cast<double>(threatX);
            const double dy = uy - static_cast<double>(threatY);
            const double len = det::detHypot(dx, dy);
            const double nx = (len > 1e-3) ? (dx / len) : -1.0;
            const double ny = (len > 1e-3) ? (dy / len) : 0.0;
            const int rx = std::clamp(det::detRoundToInt(ux + nx * 224.0), 32, std::max(32, mapPixelW_ - 32));
            const int ry = std::clamp(det::detRoundToInt(uy + ny * 224.0), 32, std::max(32, mapPixelH_ - 32));
            return {rx, ry};
        }
        if (hasZone2) return {clampedZ2X, clampedZ2Y};
        return {det::detRoundToInt(ux), det::detRoundToInt(uy)};
    };

    size_t accepted = 0;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        s.holdFire = true;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.autoTarget.reset();

        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        int ex = 0, ey = 0;
        const double threatDist = nearestSpottedEnemy(s.owner, static_cast<double>(sx), static_cast<double>(sy), ex, ey);
        const bool threatened = s.isSuppressed() || groupRecentlyHit || (threatDist <= 256.0);

        if (threatened) {
            if (s.binoculars) {
                s.binoculars = false;
                fogDirty_ = true;
            }
            const auto [rx, ry] = computeRetreatPoint(s.owner, static_cast<double>(sx), static_cast<double>(sy),
                                                      threatDist, ex, ey);
            if (det::detHypot(static_cast<double>(rx - sx), static_cast<double>(ry - sy)) > 48.0) {
                if (orderSoldierInternal(i, rx, ry)) {
                    s.attackMove = false;
                    ++accepted;
                }
            }
            continue;
        }

        if (hasZone1) {
            if (!checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, s.owner, aiFlags,
                               igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 224.0, onlyNonPlayer);
                continue;
            }
            const double distZ1 = det::detHypot(static_cast<double>(clampedZ1X - sx),
                                                static_cast<double>(clampedZ1Y - sy));
            if (distZ1 > 96.0) {
                if (!s.goal && !s.motion && s.route.empty()) {
                    if (orderSoldierInternal(i, clampedZ1X, clampedZ1Y)) {
                        s.attackMove = false;
                        ++accepted;
                    }
                }
                continue;
            }
        }
        if (!s.motion && !s.goal && s.route.empty() && s.hasBinoculars && !s.binoculars) {
            s.binoculars = true;
            fogDirty_ = true;
        }
    }

    for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.speedPx <= 0.0 || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        v.holdFire = true;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;

        int ex = 0, ey = 0;
        const double threatDist = nearestSpottedEnemy(v.owner, v.fx, v.fy, ex, ey);
        const bool threatened = v.isSuppressed() || groupRecentlyHit || (threatDist <= 256.0);

        if (threatened) {
            const auto [rx, ry] = computeRetreatPoint(v.owner, v.fx, v.fy, threatDist, ex, ey);
            if (det::detHypot(static_cast<double>(rx) - v.fx, static_cast<double>(ry) - v.fy) > 48.0) {
                if (!v.moveTarget ||
                    det::detHypot(static_cast<double>(v.moveTarget->first - rx),
                                  static_cast<double>(v.moveTarget->second - ry)) > 64.0) {
                    moveVehicleInternal(v, rx, ry);
                    v.attackMove = false;
                    if (v.canMoveBackward && v.backSpeedPx > 0.0 && threatDist <= 280.0) {
                        v.reverseMove = true;
                    }
                    ++accepted;
                }
            }
            continue;
        }

        if (v.marchEnabled && !v.hatchOpen && v.suppressionTicks == 0) {
            v.hatchOpen = true;
            fogDirty_ = true;
            emitVehiclePose(vid);
        }
        if (hasZone1) {
            if (!checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, v.owner, aiFlags,
                               igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 224.0, onlyNonPlayer);
                continue;
            }
            if (det::detHypot(static_cast<double>(clampedZ1X) - v.fx,
                              static_cast<double>(clampedZ1Y) - v.fy) > 96.0 && !v.moveTarget) {
                moveVehicleInternal(v, clampedZ1X, clampedZ1Y);
                v.attackMove = false;
                ++accepted;
            }
        }
    }
    return accepted;
}

size_t TutorialUnits::orderGroupTransport(unsigned group, std::uint32_t aiFlags, unsigned targetGroup,
                                          std::span<const std::uint8_t> zones,
                                          unsigned zone1, int z1x, int z1y, bool hasZone1,
                                          unsigned zone2, int z2x, int z2y, bool hasZone2,
                                          std::vector<SoldierPose>* outUnloaded,
                                          bool onlyNonPlayer) {
    const int g = int(group);
    const bool holdFire = (aiFlags & igroups::kAifHoldFire) != 0u;
    if (holdFire) setGroupHoldFire(group, true, onlyNonPlayer);

    const bool hasSeparateZones = hasZone1 && hasZone2 && zone1 != zone2;
    const bool hasDropZone = hasSeparateZones ? true : (hasZone2 || hasZone1);
    const unsigned dropZone = hasSeparateZones ? zone2 : (hasZone2 ? zone2 : zone1);
    const int dropX = hasDropZone
        ? std::clamp(hasSeparateZones ? z2x : (hasZone2 ? z2x : z1x), 0, std::max(0, mapPixelW_ - 1))
        : 0;
    const int dropY = hasDropZone
        ? std::clamp(hasSeparateZones ? z2y : (hasZone2 ? z2y : z1y), 0, std::max(0, mapPixelH_ - 1))
        : 0;
    const int pickZ1X = hasZone1 ? std::clamp(z1x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int pickZ1Y = hasZone1 ? std::clamp(z1y, 0, std::max(0, mapPixelH_ - 1)) : 0;

    const std::size_t tileW = std::size_t(grid_.width() / 2);
    const std::size_t tileH = std::size_t(grid_.height() / 2);
    bool z1Painted = false;
    bool dropPainted = false;
    if (zones.size() == tileW * tileH) {
        for (const std::uint8_t zb : zones) {
            const unsigned zid = zb & 63u;
            if (hasZone1 && zid == zone1) z1Painted = true;
            if (hasDropZone && zid == dropZone) dropPainted = true;
            if ((!hasZone1 || z1Painted) && (!hasDropZone || dropPainted)) break;
        }
    }

    const auto inPickupZone1 = [&](SubCell c, int px, int py) {
        if (!hasSeparateZones) return false;
        if (z1Painted && c.x >= 0 && c.y >= 0) {
            const std::size_t tx = std::size_t(c.x / 2);
            const std::size_t ty = std::size_t(c.y / 2);
            if (tx < tileW && ty < tileH && (zones[ty * tileW + tx] & 63u) == zone1) return true;
        }
        return det::detHypot(static_cast<double>(px - pickZ1X),
                             static_cast<double>(py - pickZ1Y)) <= 256.0;
    };
    const auto inDropZone = [&](SubCell c, int px, int py) {
        if (!hasDropZone) return false;
        if (dropPainted && c.x >= 0 && c.y >= 0) {
            const std::size_t tx = std::size_t(c.x / 2);
            const std::size_t ty = std::size_t(c.y / 2);
            if (tx < tileW && ty < tileH && (zones[ty * tileW + tx] & 63u) == dropZone) return true;
        }
        return det::detHypot(static_cast<double>(px - dropX),
                             static_cast<double>(py - dropY)) <= 160.0;
    };

    std::vector<size_t> transports;
    for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
        const auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.speedPx <= 0.0 || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (v.passengerCapacity <= 0) continue;
        transports.push_back(vid);
    }

    if (transports.empty()) {
        if (hasDropZone) {
            return orderGroupZoneBehavior(group, igroups::kAiFurgTransport, aiFlags, dropX, dropY, onlyNonPlayer);
        }
        return 0u;
    }

    size_t accepted = 0;
    for (const size_t vid : transports) {
        auto& v = vehicles_[vid];
        const int cap = v.passengerCapacity;
        const int loaded = static_cast<int>(v.passengers.size());

        // Eligible friendly infantry waiting for pickup outside the destination zone.
        std::vector<size_t> candidates;
        int activeBoardingHere = 0;
        for (size_t sid = 0; sid < soldiers_.size(); ++sid) {
            const auto& s = soldiers_[sid];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.owner != v.owner) continue;
            const auto [su, sv] = soldierWorld(s);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            if (hasDropZone && inDropZone(s.cell, sx, sy)) continue;

            if (targetGroup > 0u) {
                if (s.group != static_cast<int>(targetGroup)) continue;
            } else if (hasSeparateZones) {
                if (!inPickupZone1(s.cell, sx, sy)) continue;
            } else {
                continue;
            }

            if (s.boardVehicleTarget) {
                if (*s.boardVehicleTarget == vid) {
                    ++activeBoardingHere;
                    candidates.push_back(sid);
                }
                continue;
            }
            candidates.push_back(sid);
        }

        const int retailReadyThreshold = std::max(1, (cap * 70) / 100);
        const bool readyToShuttle =
            loaded > 0 && hasDropZone &&
            (loaded >= cap ||
             (loaded >= retailReadyThreshold && activeBoardingHere == 0) ||
             candidates.empty());

        if (readyToShuttle) {
            if (!checkZoneGate(zones, dropZone, dropX, dropY, v.owner, aiFlags,
                               igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                stopGroupUnitsMovingTo(group, dropX, dropY, 224.0, onlyNonPlayer);
                continue;
            }
            const double distDrop = det::detHypot(static_cast<double>(dropX) - v.fx,
                                                  static_cast<double>(dropY) - v.fy);
            if (distDrop <= 128.0 || (inDropZone(v.cell, v.x, v.y) && distDrop <= 192.0)) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.queuedOrders.clear();
                v.attackMove = false;
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(vid);
                }
                ScopedCommandActor scope(*this, v.owner);
                accepted += unloadVehicles(std::array<size_t, 1>{vid}, outUnloaded);
            } else if (!v.moveTarget ||
                       det::detHypot(static_cast<double>(v.moveTarget->first - dropX),
                                     static_cast<double>(v.moveTarget->second - dropY)) > 128.0) {
                moveVehicleInternal(v, dropX, dropY);
                v.attackMove = false;
                ++accepted;
            }
            continue;
        }

        if (!candidates.empty() && loaded < cap) {
            if (hasSeparateZones &&
                !checkZoneGate(zones, zone1, pickZ1X, pickZ1Y, v.owner, aiFlags,
                               igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                stopGroupUnitsMovingTo(group, pickZ1X, pickZ1Y, 224.0, onlyNonPlayer);
                continue;
            }
            std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
                const auto [au, av] = soldierWorld(soldiers_[a]);
                const auto [bu, bv] = soldierWorld(soldiers_[b]);
                const double da = det::detHypot(static_cast<double>(originX_ + au - av) - v.fx,
                                                static_cast<double>((au + av) / 2) - v.fy);
                const double db = det::detHypot(static_cast<double>(originX_ + bu - bv) - v.fx,
                                                static_cast<double>((bu + bv) / 2) - v.fy);
                if (std::abs(da - db) > 1e-6) return da < db;
                return a < b;
            });

            const int slotsLeft = cap - loaded;
            const size_t assignCount = std::min(candidates.size(), static_cast<size_t>(slotsLeft));
            const auto [nu, nv] = soldierWorld(soldiers_[candidates[0]]);
            const int nearX = originX_ + nu - nv;
            const int nearY = (nu + nv) / 2;
            const double distNearest = det::detHypot(static_cast<double>(nearX) - v.fx,
                                                     static_cast<double>(nearY) - v.fy);

            if (distNearest > 80.0) {
                if (!v.moveTarget ||
                    det::detHypot(static_cast<double>(v.moveTarget->first - nearX),
                                  static_cast<double>(v.moveTarget->second - nearY)) > 80.0) {
                    moveVehicleInternal(v, nearX, nearY);
                    v.attackMove = false;
                    ++accepted;
                }
            } else if (v.moveTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.queuedOrders.clear();
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(vid);
                }
            }

            if (distNearest <= 192.0) {
                for (size_t k = 0; k < assignCount; ++k) {
                    auto& s = soldiers_[candidates[k]];
                    if (s.boardVehicleTarget == vid) continue;
                    s.boardVehicleTarget = vid;
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
            }
            continue;
        }

        // Empty transport with no waiting infantry returns to pickup zone1 (0x100511e9).
        if (loaded == 0 && hasSeparateZones) {
            if (!checkZoneGate(zones, zone1, pickZ1X, pickZ1Y, v.owner, aiFlags,
                               igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                stopGroupUnitsMovingTo(group, pickZ1X, pickZ1Y, 224.0, onlyNonPlayer);
                continue;
            }
            if (!v.moveTarget &&
                det::detHypot(static_cast<double>(pickZ1X) - v.fx,
                              static_cast<double>(pickZ1Y) - v.fy) > 144.0) {
                moveVehicleInternal(v, pickZ1X, pickZ1Y);
                v.attackMove = false;
                ++accepted;
            }
        }
    }
    return accepted;
}

size_t TutorialUnits::orderGroupKatya(unsigned group, std::uint32_t aiFlags,
                                      std::span<const std::uint8_t> zones,
                                      unsigned zone1, int z1x, int z1y, bool hasZone1,
                                      unsigned zone2, int z2x, int z2y, bool hasZone2,
                                      bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedZ1X = hasZone1 ? std::clamp(z1x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ1Y = hasZone1 ? std::clamp(z1y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const int clampedZ2X = hasZone2 ? std::clamp(z2x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ2Y = hasZone2 ? std::clamp(z2y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const bool holdFire = (aiFlags & igroups::kAifHoldFire) != 0u;
    if (holdFire) setGroupHoldFire(group, true, onlyNonPlayer);

    size_t accepted = 0;
    for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;

        // Never interrupt an active rocket salvo mid-burst.
        if (v.burstCount1 > 0 && (v.ammoPerShot1 == 0 || v.ammo > 0)) {
            continue;
        }

        const bool outOfAmmo = (v.ammoPerShot1 > 0 && v.ammo <= 0);
        const bool reloading = (v.fireCooldown > 0) || outOfAmmo;

        if (reloading) {
            // Clear attackGround first so stepVehicleCombat does not cancel moveTarget.
            v.attackGround.reset();
            v.attackTarget.reset();
            v.attackBuildingTarget.reset();
            if (v.speedPx <= 0.0) continue;

            std::optional<std::pair<int, int>> coverPt;
            if (hasZone2 && (!hasZone1 || zone2 != zone1)) {
                if (!checkZoneGate(zones, zone2, clampedZ2X, clampedZ2Y, v.owner, aiFlags,
                                   igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                    stopGroupUnitsMovingTo(group, clampedZ2X, clampedZ2Y, 224.0, onlyNonPlayer);
                    continue;
                }
                coverPt = std::pair{clampedZ2X, clampedZ2Y};
            } else if ((aiFlags & igroups::kAifRndMove) != 0u) {
                static constexpr std::array<std::pair<int, int>, 4> kScootOffsets{{
                    {160, 0}, {-160, 0}, {0, 128}, {0, -128}
                }};
                const int baseX = hasZone1 ? clampedZ1X : v.x;
                const int baseY = hasZone1 ? clampedZ1Y : v.y;
                for (unsigned k = 0u; k < 4u; ++k) {
                    const auto [ox, oy] = kScootOffsets[((tick_ / 64u) + vid + k) & 3u];
                    const int cx = std::clamp(baseX + ox, 32, std::max(32, mapPixelW_ - 32));
                    const int cy = std::clamp(baseY + oy, 32, std::max(32, mapPixelH_ - 32));
                    const SubCell cc = placementCell(cx, cy, originX_);
                    if (grid_.inBounds(cc) && grid_.walkableMask(cc, v.forbiddenTerrainMask)) {
                        coverPt = std::pair{cx, cy};
                        break;
                    }
                }
            }

            if (coverPt &&
                det::detHypot(static_cast<double>(coverPt->first) - v.fx,
                              static_cast<double>(coverPt->second) - v.fy) > 88.0) {
                if (!v.moveTarget ||
                    det::detHypot(static_cast<double>(v.moveTarget->first - coverPt->first),
                                  static_cast<double>(v.moveTarget->second - coverPt->second)) > 96.0) {
                    moveVehicleInternal(v, coverPt->first, coverPt->second);
                    v.attackMove = false;
                    ++accepted;
                }
            }
            continue;
        }

        // Loaded & ready: advance into firing zone1 (0x10053752..0x1005388a).
        if (hasZone1) {
            if (!checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, v.owner, aiFlags,
                               igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy)) {
                stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 224.0, onlyNonPlayer);
                continue;
            }
            if (v.speedPx > 0.0 &&
                det::detHypot(static_cast<double>(clampedZ1X) - v.fx,
                              static_cast<double>(clampedZ1Y) - v.fy) > 160.0) {
                if (!v.moveTarget ||
                    det::detHypot(static_cast<double>(v.moveTarget->first - clampedZ1X),
                                  static_cast<double>(v.moveTarget->second - clampedZ1Y)) > 96.0) {
                    moveVehicleInternal(v, clampedZ1X, clampedZ1Y);
                    v.attackMove = false;
                    ++accepted;
                }
                continue;
            }
        }

        // In firing position and ready to fire salvo (0x100539c0..0x10053c35: +1 enemy, -2 allied density score).
        if (!v.hasPrimary || v.holdFire || holdFire) continue;
        if (v.attackGround) {
            const double gd = det::detHypot(static_cast<double>(v.attackGround->first) - v.fx,
                                            static_cast<double>(v.attackGround->second) - v.fy);
            if (gd >= v.deadzone && gd <= v.range) continue;
            v.attackGround.reset();
        }

        std::vector<std::tuple<int, int, int>> hostilePts;
        std::vector<std::pair<int, int>> alliedPts;
        for (size_t j = 0; j < soldiers_.size(); ++j) {
            const auto& s = soldiers_[j];
            if (s.hp <= 0 || s.embarked) continue;
            int sx = 0, sy = 0;
            if (s.garrisonBuilding && *s.garrisonBuilding < buildings_.size()) {
                sx = buildings_[*s.garrisonBuilding].x;
                sy = buildings_[*s.garrisonBuilding].y;
            } else {
                const auto [su, sv] = soldierWorld(s);
                sx = originX_ + su - sv;
                sy = (su + sv) / 2;
            }
            if (isAlliedFor(v.owner, s.owner)) {
                alliedPts.emplace_back(sx, sy);
            } else if (isHostile(v.owner, s.owner)) {
                if (!fog_ || isVehicleTargetValidFor(v.owner, false, j)) {
                    hostilePts.emplace_back(sx, sy, 0);
                }
            }
        }
        for (size_t j = 0; j < vehicles_.size(); ++j) {
            if (j == vid) continue;
            const auto& tv = vehicles_[j];
            if (!tv.alive || tv.hp <= 0 || tv.isCrate) continue;
            if (isAlliedFor(v.owner, tv.owner)) {
                alliedPts.emplace_back(tv.x, tv.y);
            } else if (isHostile(v.owner, tv.owner) && (tv.hasCrew || !tv.passengers.empty())) {
                const bool revealedBattery = tv.muzzleRevealTicks > 0u;
                if (!fog_ || isVehicleTargetValidFor(v.owner, true, j) || revealedBattery) {
                    const int cbBonus = (revealedBattery && (tv.isArtillery || tv.isRocketArtillery || tv.damage >= 180)) ? 4 : (revealedBattery ? 2 : 0);
                    hostilePts.emplace_back(tv.x, tv.y, cbBonus);
                }
            }
        }

        int bestScore = 0;
        double bestDist = 1e18;
        std::optional<std::pair<int, int>> bestTargetPt;
        for (const auto& [cx, cy, cbBonus] : hostilePts) {
            const double d = det::detHypot(static_cast<double>(cx) - v.fx,
                                           static_cast<double>(cy) - v.fy);
            if (d < v.deadzone || d > v.range) continue;
            int score = cbBonus;
            for (const auto& [hx, hy, hCb] : hostilePts) {
                (void)hCb;
                if (det::detHypot(static_cast<double>(hx - cx), static_cast<double>(hy - cy)) <= 160.0) {
                    ++score;
                }
            }
            for (const auto& [ax, ay] : alliedPts) {
                if (det::detHypot(static_cast<double>(ax - cx), static_cast<double>(ay - cy)) <= 128.0) {
                    score -= 2;
                }
            }
            if (score > bestScore || (score == bestScore && score > 0 && d < bestDist)) {
                bestScore = score;
                bestDist = d;
                bestTargetPt = std::pair{cx, cy};
            }
        }

        if (bestTargetPt) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.queuedOrders.clear();
            v.attackTarget.reset();
            v.attackBuildingTarget.reset();
            v.attackMove = false;
            v.attackMoveEngaging = false;
            v.reverseMove = false;
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(vid);
            }
            v.attackGround = bestTargetPt;
            ++accepted;
        }
    }
    return accepted;
}

size_t TutorialUnits::orderGroupTrain(unsigned group, std::uint32_t aiFlags,
                                      std::span<const std::uint8_t> zones,
                                      unsigned zone1, int z1x, int z1y, bool hasZone1,
                                      unsigned zone2, int z2x, int z2y, bool hasZone2,
                                      unsigned tick, bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedZ1X = hasZone1 ? std::clamp(z1x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ1Y = hasZone1 ? std::clamp(z1y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const int clampedZ2X = hasZone2 ? std::clamp(z2x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ2Y = hasZone2 ? std::clamp(z2y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const bool holdFire = (aiFlags & igroups::kAifHoldFire) != 0u;
    if (holdFire) setGroupHoldFire(group, true, onlyNonPlayer);

    const std::size_t tileW = std::size_t(grid_.width() / 2);
    const std::size_t tileH = std::size_t(grid_.height() / 2);
    const bool validZoneGrid = (zones.size() == tileW * tileH);

    // Snap an arbitrary zone centroid onto a passable, connected rail cell for `v`.
    const auto snapRailPoint = [&](const Vehicle& v, unsigned zId, int desiredX, int desiredY)
        -> std::optional<std::pair<int, int>> {
        const SubCell desiredSub = pointerCell(desiredX, desiredY, originX_);
        const int centerTx = std::clamp(desiredSub.x / 2, 0, std::max(0, static_cast<int>(tileW) - 1));
        const int centerTy = std::clamp(desiredSub.y / 2, 0, std::max(0, static_cast<int>(tileH) - 1));

        bool hasZoneTiles = false;
        if (validZoneGrid && zId > 0u) {
            for (const std::uint8_t zb : zones) {
                if ((zb & 63u) == zId) {
                    hasZoneTiles = true;
                    break;
                }
            }
        }

        const auto searchPass = [&](bool requireInZone) -> std::optional<std::pair<int, int>> {
            std::optional<std::pair<int, int>> bestPt;
            std::int64_t bestD2 = std::numeric_limits<std::int64_t>::max();
            for (int r = 0; r <= 24; ++r) {
                if (bestPt && std::int64_t(r) * 16 * std::int64_t(r) * 16 > bestD2 * 4) break;
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                        const int tx = centerTx + dx;
                        const int ty = centerTy + dy;
                        if (tx < 0 || ty < 0 || std::size_t(tx) >= tileW || std::size_t(ty) >= tileH) continue;
                        if (requireInZone &&
                            (zones[std::size_t(ty) * tileW + std::size_t(tx)] & 63u) != zId) {
                            continue;
                        }
                        for (int sy = 0; sy < 2; ++sy) {
                            for (int sx = 0; sx < 2; ++sx) {
                                const SubCell cand{tx * 2 + sx, ty * 2 + sy};
                                if (!grid_.inBounds(cand) || !grid_.walkableMask(cand, v.forbiddenTerrainMask)) {
                                    continue;
                                }
                                if (!sameTile(v.cell, cand) &&
                                    grid_.findTileDirectionsForMask(v.cell, cand, v.forbiddenTerrainMask, 256).empty()) {
                                    continue;
                                }
                                const auto [wu, wv] = InfantryPathGrid::toWorldCenter(cand);
                                const int px = std::clamp(originX_ + wu - wv, 0, std::max(0, mapPixelW_ - 1));
                                const int py = std::clamp((wu + wv) / 2, 0, std::max(0, mapPixelH_ - 1));
                                const std::int64_t ox = std::int64_t(px) - desiredX;
                                const std::int64_t oy = std::int64_t(py) - desiredY;
                                const std::int64_t d2 = ox * ox + oy * oy;
                                if (d2 < bestD2) {
                                    bestD2 = d2;
                                    bestPt = std::pair{px, py};
                                }
                            }
                        }
                    }
                }
            }
            return bestPt;
        };

        if (hasZoneTiles) {
            if (auto pt = searchPass(true)) return pt;
        }
        return searchPass(false);
    };

    size_t accepted = 0;
    std::vector<size_t> mobileTrainIds;
    for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
        const auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (v.speedPx > 0.0) mobileTrainIds.push_back(vid);
    }

    if (mobileTrainIds.size() > 1u && (hasZone1 || hasZone2)) {
        // Multi-car armored train consist: share a single patrol direction from the consist centroid
        // and space coupled cars sequentially along connected rail tiles so cars never split head-on.
        const auto& leadRef = vehicles_[mobileTrainIds.front()];
        const bool z1Ok = hasZone1 &&
            checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, leadRef.owner, aiFlags,
                          igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy);
        const bool z2Ok = hasZone2 &&
            checkZoneGate(zones, zone2, clampedZ2X, clampedZ2Y, leadRef.owner, aiFlags,
                          igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy);
        if (hasZone1 && !z1Ok) stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 256.0, onlyNonPlayer);
        if (hasZone2 && !z2Ok) stopGroupUnitsMovingTo(group, clampedZ2X, clampedZ2Y, 256.0, onlyNonPlayer);

        const bool allIdle = std::ranges::all_of(mobileTrainIds, [&](size_t vid) {
            return !vehicles_[vid].moveTarget;
        });
        if (allIdle) {
            double sumFx = 0.0, sumFy = 0.0;
            for (const size_t vid : mobileTrainIds) {
                sumFx += vehicles_[vid].fx;
                sumFy += vehicles_[vid].fy;
            }
            const double centX = sumFx / static_cast<double>(mobileTrainIds.size());
            const double centY = sumFy / static_cast<double>(mobileTrainIds.size());

            std::optional<std::pair<int, int>> r1 = z1Ok ? snapRailPoint(leadRef, zone1, clampedZ1X, clampedZ1Y)
                                                         : std::nullopt;
            std::optional<std::pair<int, int>> r2 = (z2Ok && (!hasZone1 || zone2 != zone1))
                                                         ? snapRailPoint(leadRef, zone2, clampedZ2X, clampedZ2Y)
                                                         : std::nullopt;
            std::optional<std::pair<int, int>> chosen;
            const double consistHalfSpan = 64.0 + static_cast<double>(mobileTrainIds.size() - 1u) * 36.0;
            if (r1 && r2) {
                const double d1 = det::detHypot(static_cast<double>(r1->first) - centX,
                                                static_cast<double>(r1->second) - centY);
                const double d2 = det::detHypot(static_cast<double>(r2->first) - centX,
                                                static_cast<double>(r2->second) - centY);
                if (d1 <= consistHalfSpan && d2 > consistHalfSpan) {
                    chosen = r2;
                } else if (d2 <= consistHalfSpan && d1 > consistHalfSpan) {
                    chosen = r1;
                } else if ((aiFlags & igroups::kAifRndMove) != 0u) {
                    chosen = (((tick / 128u) & 1u) != 0u) ? r2 : r1;
                } else {
                    chosen = (d1 >= d2) ? r1 : r2;
                }
            } else if (r1) {
                if (det::detHypot(static_cast<double>(r1->first) - centX,
                                  static_cast<double>(r1->second) - centY) > consistHalfSpan) {
                    chosen = r1;
                }
            } else if (r2) {
                if (det::detHypot(static_cast<double>(r2->first) - centX,
                                  static_cast<double>(r2->second) - centY) > consistHalfSpan) {
                    chosen = r2;
                }
            }

            if (chosen) {
                // Sort cars from closest-to-destination (lead car for this leg) to farthest (trailing car),
                // then space each car's target cell back along the rail direction toward the consist.
                std::vector<size_t> ordered = mobileTrainIds;
                std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
                    const double da = det::detHypot(static_cast<double>(chosen->first) - vehicles_[a].fx,
                                                    static_cast<double>(chosen->second) - vehicles_[a].fy);
                    const double db = det::detHypot(static_cast<double>(chosen->first) - vehicles_[b].fx,
                                                    static_cast<double>(chosen->second) - vehicles_[b].fy);
                    if (std::abs(da - db) > 1e-6) return da < db;
                    return a < b;
                });

                const double backDx = centX - static_cast<double>(chosen->first);
                const double backDy = centY - static_cast<double>(chosen->second);
                const double backLen = det::detHypot(backDx, backDy);
                const double ux = (backLen > 1e-3) ? (backDx / backLen) : 0.0;
                const double uy = (backLen > 1e-3) ? (backDy / backLen) : 0.0;

                for (size_t rank = 0; rank < ordered.size(); ++rank) {
                    auto& v = vehicles_[ordered[rank]];
                    const int desiredX = det::detRoundToInt(static_cast<double>(chosen->first) + ux * (48.0 * static_cast<double>(rank)));
                    const int desiredY = det::detRoundToInt(static_cast<double>(chosen->second) + uy * (48.0 * static_cast<double>(rank)));
                    const auto carPt = (rank == 0u) ? chosen : snapRailPoint(v, 0u, desiredX, desiredY);
                    const auto finalPt = carPt ? *carPt : *chosen;
                    moveVehicleInternal(v, finalPt.first, finalPt.second);
                    v.attackMove = !holdFire;
                    if (v.canMoveBackward && v.backSpeedPx > 0.0) {
                        const unsigned moveDir = vehicleDirFromMapDelta(
                            static_cast<double>(finalPt.first) - v.fx,
                            static_cast<double>(finalPt.second) - v.fy);
                        const unsigned delta = std::min((moveDir - v.hullDir) & 31u, (v.hullDir - moveDir) & 31u);
                        if (delta >= 12u) {
                            v.reverseMove = true;
                        }
                    }
                    ++accepted;
                }
            }
        }
    }

    for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;

        if (mobileTrainIds.size() <= 1u && v.speedPx > 0.0 && (hasZone1 || hasZone2)) {
            const bool z1Ok = hasZone1 &&
                checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, v.owner, aiFlags,
                              igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy);
            const bool z2Ok = hasZone2 &&
                checkZoneGate(zones, zone2, clampedZ2X, clampedZ2Y, v.owner, aiFlags,
                              igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy);
            if (hasZone1 && !z1Ok) stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 224.0, onlyNonPlayer);
            if (hasZone2 && !z2Ok) stopGroupUnitsMovingTo(group, clampedZ2X, clampedZ2Y, 224.0, onlyNonPlayer);

            if (!v.moveTarget) {
                std::optional<std::pair<int, int>> r1 = z1Ok ? snapRailPoint(v, zone1, clampedZ1X, clampedZ1Y)
                                                             : std::nullopt;
                std::optional<std::pair<int, int>> r2 = (z2Ok && (!hasZone1 || zone2 != zone1))
                                                             ? snapRailPoint(v, zone2, clampedZ2X, clampedZ2Y)
                                                             : std::nullopt;
                std::optional<std::pair<int, int>> chosen;
                if (r1 && r2) {
                    const double d1 = det::detHypot(static_cast<double>(r1->first) - v.fx,
                                                    static_cast<double>(r1->second) - v.fy);
                    const double d2 = det::detHypot(static_cast<double>(r2->first) - v.fx,
                                                    static_cast<double>(r2->second) - v.fy);
                    if (d1 <= 96.0 && d2 > 96.0) {
                        chosen = r2;
                    } else if (d2 <= 96.0 && d1 > 96.0) {
                        chosen = r1;
                    } else if ((aiFlags & igroups::kAifRndMove) != 0u) {
                        chosen = (((tick / 128u) & 1u) != 0u) ? r2 : r1;
                    } else {
                        chosen = (d1 >= d2) ? r1 : r2;
                    }
                } else if (r1) {
                    if (det::detHypot(static_cast<double>(r1->first) - v.fx,
                                      static_cast<double>(r1->second) - v.fy) > 64.0) {
                        chosen = r1;
                    }
                } else if (r2) {
                    if (det::detHypot(static_cast<double>(r2->first) - v.fx,
                                      static_cast<double>(r2->second) - v.fy) > 64.0) {
                        chosen = r2;
                    }
                }

                if (chosen) {
                    moveVehicleInternal(v, chosen->first, chosen->second);
                    v.attackMove = !holdFire;
                    if (v.canMoveBackward && v.backSpeedPx > 0.0) {
                        const unsigned moveDir = vehicleDirFromMapDelta(
                            static_cast<double>(chosen->first) - v.fx,
                            static_cast<double>(chosen->second) - v.fy);
                        const unsigned delta = std::min((moveDir - v.hullDir) & 31u, (v.hullDir - moveDir) & 31u);
                        if (delta >= 12u) {
                            v.reverseMove = true;
                        }
                    }
                    ++accepted;
                }
            }
        }

        // Engage spotted enemies in range when stationary on the rails and allowed to fire.
        if (fog_ && v.armed && !v.holdFire && !holdFire && !v.moveTarget &&
            !v.attackTarget && !v.attackGround && !v.attackBuildingTarget &&
            ((v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0)) ||
             (v.hasSecondary && (v.ammoPerShot2 == 0 || v.ammo2 > 0)))) {
            const double maxR = std::max(v.hasPrimary ? v.range : 0.0, v.hasSecondary ? v.range2 : 0.0);
            const double minR = v.hasPrimary ? v.deadzone : 0.0;
            std::optional<std::pair<bool, size_t>> bestTgt;
            double bestDist = 1e18;
            for (size_t j = 0; j < vehicles_.size(); ++j) {
                if (j == vid || !isVehicleTargetValidFor(v.owner, true, j)) continue;
                const double d = det::detHypot(vehicles_[j].fx - v.fx, vehicles_[j].fy - v.fy);
                if (d >= minR && d <= maxR && d < bestDist) {
                    bestDist = d;
                    bestTgt = std::pair{true, j};
                }
            }
            for (size_t j = 0; j < soldiers_.size(); ++j) {
                if (!isVehicleTargetValidFor(v.owner, false, j)) continue;
                const auto [tu, tv] = soldierWorld(soldiers_[j]);
                const int tx = originX_ + tu - tv;
                const int ty = (tu + tv) / 2;
                const double d = det::detHypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
                if (d >= minR && d <= maxR && d < bestDist) {
                    bestDist = d;
                    bestTgt = std::pair{false, j};
                }
            }
            if (bestTgt) {
                v.attackTarget = bestTgt;
                ++accepted;
            }
        }
    }
    return accepted;
}

size_t TutorialUnits::transferGroupOwner(unsigned group, int newOwner, std::vector<SoldierPose>* outSoldiers) {
    if (newOwner < 0 || newOwner >= 4) return 0;
    const int g = int(group);
    size_t transferred = 0;
    std::vector<size_t> movedSoldiers;
    std::vector<size_t> movedVehicles;

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.group != g || s.owner == newOwner) continue;
        s.owner = newOwner;
        s.motion.reset();
        clearMovingSightPenalty(s);
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        movedSoldiers.push_back(i);
        ++transferred;
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.group != g || v.owner == newOwner) continue;
        v.owner = newOwner;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
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
        v.supportTargetBridge.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.pendingTowGun.reset();
        movedVehicles.push_back(i);
        ++transferred;
    }
    if (transferred > 0) {
        fogDirty_ = true;
        if (fog_) refreshSpotting(nullptr, false);
        for (size_t vid : movedVehicles) emitVehiclePose(vid);
        if (outSoldiers) {
            for (size_t sid : movedSoldiers) {
                const auto& s = soldiers_[sid];
                const auto [su, sv] = s.garrisonBuilding
                    ? tutorialFootWorld(buildings_[*s.garrisonBuilding].x, buildings_[*s.garrisonBuilding].y, originX_)
                    : soldierWorld(s);
                const bool vis = isVisibleToViewer(s.owner, s.spottedMask);
                pushOrUpdateSoldierPose(*outSoldiers, {
                    sid, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false,
                    s.hp, true, s.owner, s.group, false, vis, 0, 0, s.garrisonBuilding.has_value(),
                    s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo
                });
            }
        }
    }
    return transferred;
}

size_t TutorialUnits::reassignGroupInZone(std::span<const std::uint8_t> zones, unsigned zone,
                                            unsigned oldGroup, unsigned newGroup,
                                            std::vector<SoldierPose>* outSoldiers) {
    const std::size_t tileW = std::size_t(grid_.width() / 2), tileH = std::size_t(grid_.height() / 2);
    if (zones.size() != tileW * tileH) return 0;
    const auto inside = [&](SubCell cell) {
        if (cell.x < 0 || cell.y < 0) return false;
        const std::size_t tx = std::size_t(cell.x / 2);
        const std::size_t ty = std::size_t(cell.y / 2);
        if (tx >= tileW || ty >= tileH) return false;
        return (zones[ty * tileW + tx] & 63u) == zone;
    };
    const int og = int(oldGroup);
    const int ng = int(newGroup);
    size_t reassigned = 0;

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.group != og) continue;
        const SubCell locCell = s.garrisonBuilding ? buildings_[*s.garrisonBuilding].entryCell : s.cell;
        if (!inside(locCell)) continue;
        s.group = ng;
        s.motion.reset();
        clearMovingSightPenalty(s);
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++reassigned;
        if (outSoldiers) {
            const auto [su, sv] = s.garrisonBuilding
                ? tutorialFootWorld(buildings_[*s.garrisonBuilding].x, buildings_[*s.garrisonBuilding].y, originX_)
                : soldierWorld(s);
            const bool vis = isVisibleToViewer(s.owner, s.spottedMask);
            pushOrUpdateSoldierPose(*outSoldiers, {
                i, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false,
                s.hp, true, s.owner, s.group, false, vis, 0, 0, s.garrisonBuilding.has_value(),
                s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo
            });
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.group != og || !inside(v.cell)) continue;
        v.group = ng;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
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
        v.supportTargetBridge.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.pendingTowGun.reset();
        emitVehiclePose(i);
        ++reassigned;
    }
    return reassigned;
}

std::size_t TutorialUnits::dispatchEvacuationOrders(std::vector<size_t> soldierIds,
                                                    std::vector<SubCell> soldierSources,
                                                    std::vector<size_t> vehIds,
                                                    int clampedX, int clampedY) {
    std::size_t ordered = 0;
    const auto upsertOrder = [](std::vector<EvacuationOrder>& vec, std::size_t idx, int tx, int ty) {
        for (auto& o : vec) {
            if (o.index == idx) { o.targetX = tx; o.targetY = ty; return; }
        }
        vec.push_back({idx, tx, ty});
    };
    if (!soldierIds.empty()) {
        const auto destCell = findSpawnCell(grid_, pointerCell(clampedX, clampedY, originX_));
        const auto tiles = grid_.formationDestinations(soldierSources, destCell);
        for (std::size_t k = 0; k < soldierIds.size(); ++k) {
            const int tx = tiles[k].x / 2, ty = tiles[k].y / 2;
            const int px = originX_ + (tx - ty) * 32;
            const int py = (tx + ty) * 16 + 16;
            if (orderSoldierInternal(soldierIds[k], px, py)) {
                soldiers_[soldierIds[k]].attackMove = false;
                upsertOrder(evacuatingSoldiers_, soldierIds[k], clampedX, clampedY);
                ++ordered;
            }
        }
    }
    for (const auto& [vid, pt] : vehicleFormationTargets(vehIds, clampedX, clampedY)) {
        moveVehicleInternal(vehicles_[vid], pt.first, pt.second);
        vehicles_[vid].attackMove = false;
        upsertOrder(evacuatingVehicles_, vid, clampedX, clampedY);
        ++ordered;
    }
    return ordered;
}

std::size_t TutorialUnits::orderGroupEvacuate(unsigned group, int mapX, int mapY) {
    const int g = int(group);
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    std::vector<size_t> soldierIds;
    std::vector<SubCell> soldierSources;
    for (std::size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        soldierIds.push_back(i);
        soldierSources.push_back(s.cell);
    }
    std::vector<size_t> vehIds;
    for (std::size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy || v.group != g) continue;
        vehIds.push_back(i);
    }
    return dispatchEvacuationOrders(std::move(soldierIds), std::move(soldierSources),
                                    std::move(vehIds), clampedX, clampedY);
}

std::size_t TutorialUnits::orderZoneEvacuate(std::span<const std::uint8_t> zones, unsigned zone, int mapX, int mapY) {
    const std::size_t tileW = std::size_t(grid_.width() / 2), tileH = std::size_t(grid_.height() / 2);
    if (zones.size() != tileW * tileH) return 0u;
    const auto inside = [&](SubCell cell) {
        if (cell.x < 0 || cell.y < 0) return false;
        const std::size_t tx = std::size_t(cell.x / 2), ty = std::size_t(cell.y / 2);
        return tx < tileW && ty < tileH && (zones[ty * tileW + tx] & 63u) == zone;
    };
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    std::vector<size_t> soldierIds;
    std::vector<SubCell> soldierSources;
    for (std::size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || !inside(s.cell)) continue;
        soldierIds.push_back(i);
        soldierSources.push_back(s.cell);
    }
    std::vector<size_t> vehIds;
    for (std::size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy || !inside(v.cell)) continue;
        vehIds.push_back(i);
    }
    return dispatchEvacuationOrders(std::move(soldierIds), std::move(soldierSources),
                                    std::move(vehIds), clampedX, clampedY);
}

std::size_t TutorialUnits::orderGroupMoveLimited(unsigned group, unsigned maxCount, int mapX, int mapY) {
    if (maxCount == 0u) return 0u;
    const int g = int(group);
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    std::size_t remaining = maxCount;
    std::size_t accepted = 0;

    std::vector<size_t> soldierIds;
    std::vector<SubCell> soldierSources;
    for (std::size_t i = 0; i < soldiers_.size() && remaining > 0u; ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        soldierIds.push_back(i);
        soldierSources.push_back(s.cell);
        --remaining;
    }
    if (!soldierIds.empty()) {
        const auto destCell = findSpawnCell(grid_, pointerCell(clampedX, clampedY, originX_));
        const auto tiles = grid_.formationDestinations(soldierSources, destCell);
        for (std::size_t k = 0; k < soldierIds.size(); ++k) {
            const int tx = tiles[k].x / 2, ty = tiles[k].y / 2;
            const int px = originX_ + (tx - ty) * 32;
            const int py = (tx + ty) * 16 + 16;
            if (orderSoldierInternal(soldierIds[k], px, py)) {
                soldiers_[soldierIds[k]].attackMove = true;
                ++accepted;
            }
        }
    }
    std::vector<size_t> vehIds;
    for (std::size_t i = 0; i < vehicles_.size() && remaining > 0u; ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy || v.group != g) continue;
        vehIds.push_back(i);
        --remaining;
    }
    for (const auto& [vid, pt] : vehicleFormationTargets(vehIds, clampedX, clampedY)) {
        moveVehicleInternal(vehicles_[vid], pt.first, pt.second);
        vehicles_[vid].attackMove = true;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderGroupPlane(unsigned group, std::uint32_t aiFlags,
                                      std::span<const std::uint8_t> zones,
                                      unsigned zone1, int z1x, int z1y, bool hasZone1,
                                      unsigned zone2, int z2x, int z2y, bool hasZone2,
                                      unsigned tick, bool onlyNonPlayer) {
    // Retail unit_plane AI (n2Game_Dll.dll 0x10053c50..0x10053e7e, behavior == 11 ai_plane_move):
    //   - 0x10053c96..0x10053cb5: stays parked once inside zone1 or zone2 (unless aif_rndmove alternates)
    //   - 0x10053d1c..0x10053d4c: requires hp >= (maxHp * 70) / 100 before relocating
    //   - 0x10053d52..0x10053e73: gates relocation on aif_zonehaveown / aif_zonenoenemy and halts mid-move
    const int g = int(group);
    const int clampedZ1X = hasZone1 ? std::clamp(z1x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ1Y = hasZone1 ? std::clamp(z1y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const int clampedZ2X = hasZone2 ? std::clamp(z2x, 0, std::max(0, mapPixelW_ - 1)) : 0;
    const int clampedZ2Y = hasZone2 ? std::clamp(z2y, 0, std::max(0, mapPixelH_ - 1)) : 0;
    const bool holdFire = (aiFlags & igroups::kAifHoldFire) != 0u;
    if (holdFire) setGroupHoldFire(group, true, onlyNonPlayer);
    if (!hasZone1 && !hasZone2) return 0u;

    const std::size_t tileW = std::size_t(grid_.width() / 2);
    const std::size_t tileH = std::size_t(grid_.height() / 2);
    bool z1Painted = false;
    bool z2Painted = false;
    if (zones.size() == tileW * tileH) {
        for (const std::uint8_t zb : zones) {
            const unsigned zid = zb & 63u;
            if (hasZone1 && zid == zone1) z1Painted = true;
            if (hasZone2 && zid == zone2) z2Painted = true;
            if ((!hasZone1 || z1Painted) && (!hasZone2 || z2Painted)) break;
        }
    }

    const auto inZone = [&](unsigned zId, bool painted, int zx, int zy, SubCell c, int px, int py) {
        if (zId == 0u) return false;
        if (painted && c.x >= 0 && c.y >= 0) {
            const std::size_t tx = std::size_t(c.x / 2);
            const std::size_t ty = std::size_t(c.y / 2);
            if (tx < tileW && ty < tileH && (zones[ty * tileW + tx] & 63u) == zId) return true;
        }
        return det::detHypot(static_cast<double>(px - zx), static_cast<double>(py - zy)) <= 160.0;
    };

    std::vector<size_t> relocateIds;
    int targetX = 0, targetY = 0;
    bool hasTarget = false;

    for (size_t vid = 0; vid < vehicles_.size(); ++vid) {
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.speedPx <= 0.0 || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;

        const bool atZ1 = hasZone1 && inZone(zone1, z1Painted, clampedZ1X, clampedZ1Y, v.cell, v.x, v.y);
        const bool atZ2 = hasZone2 && inZone(zone2, z2Painted, clampedZ2X, clampedZ2Y, v.cell, v.x, v.y);
        const bool z1Ok = hasZone1 &&
            checkZoneGate(zones, zone1, clampedZ1X, clampedZ1Y, v.owner, aiFlags,
                          igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy);
        const bool z2Ok = hasZone2 &&
            checkZoneGate(zones, zone2, clampedZ2X, clampedZ2Y, v.owner, aiFlags,
                          igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy);

        // 0x10053e3e..0x10053e73: if destination airfield becomes unsafe before arrival, abort relocation.
        if (v.moveTarget) {
            if (!atZ1 && !atZ2) {
                if (hasZone1 && !z1Ok &&
                    det::detHypot(static_cast<double>(v.moveTarget->first - clampedZ1X),
                                  static_cast<double>(v.moveTarget->second - clampedZ1Y)) <= 224.0) {
                    stopGroupUnitsMovingTo(group, clampedZ1X, clampedZ1Y, 224.0, onlyNonPlayer);
                }
                if (hasZone2 && !z2Ok &&
                    det::detHypot(static_cast<double>(v.moveTarget->first - clampedZ2X),
                                  static_cast<double>(v.moveTarget->second - clampedZ2Y)) <= 224.0) {
                    stopGroupUnitsMovingTo(group, clampedZ2X, clampedZ2Y, 224.0, onlyNonPlayer);
                }
            }
            continue;
        }

        // 0x10053d1c..0x10053d4c: damaged planes (< 70% maxHp) remain grounded.
        if (v.hp < (std::max(1, v.maxHp) * 70) / 100) continue;

        // 0x10053c96..0x10053cb5: stay parked once inside zone1 or zone2 unless aif_rndmove alternates.
        const bool rndAlt = (aiFlags & igroups::kAifRndMove) != 0u &&
                            hasZone1 && hasZone2 && zone1 != zone2;
        if ((atZ1 || atZ2) && !rndAlt) continue;

        std::optional<std::pair<int, int>> chosen;
        if (rndAlt) {
            const bool preferZ2 = (((tick / 128u) & 1u) != 0u);
            if (preferZ2 && z2Ok && !atZ2) chosen = std::pair{clampedZ2X, clampedZ2Y};
            else if (!preferZ2 && z1Ok && !atZ1) chosen = std::pair{clampedZ1X, clampedZ1Y};
        } else if (z1Ok && !atZ1) {
            chosen = std::pair{clampedZ1X, clampedZ1Y};
        } else if (z2Ok && !atZ2) {
            chosen = std::pair{clampedZ2X, clampedZ2Y};
        }

        if (chosen) {
            if (!hasTarget) {
                targetX = chosen->first;
                targetY = chosen->second;
                hasTarget = true;
            }
            if (chosen->first == targetX && chosen->second == targetY) {
                relocateIds.push_back(vid);
            } else {
                moveVehicleInternal(v, chosen->first, chosen->second);
                v.attackMove = false;
            }
        }
    }

    size_t accepted = 0;
    if (!relocateIds.empty()) {
        for (const auto& [vid, pt] : vehicleFormationTargets(relocateIds, targetX, targetY)) {
            moveVehicleInternal(vehicles_[vid], pt.first, pt.second);
            vehicles_[vid].attackMove = false;
            ++accepted;
        }
    }
    return accepted;
}
}  // namespace ss2
