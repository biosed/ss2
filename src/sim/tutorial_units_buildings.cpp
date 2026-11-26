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
SubCell TutorialUnits::findBuildingEntryCell(int mapX, int mapY) const {
    const int w = grid_.width(), h = grid_.height();
    const SubCell raw = pointerCell(mapX, mapY, originX_);
    if (w <= 0 || h <= 0) return raw;
    const int cx = std::clamp(raw.x, 0, w - 1);
    const int cy = std::clamp(raw.y, 0, h - 1);
    std::optional<SubCell> walkableFallback;
    for (int r = 0; r <= 24; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                const SubCell cand{cx + dx, cy + dy};
                if (!grid_.inBounds(cand)) continue;
                if (grid_.canEnter(cand)) return cand;
                if (!walkableFallback && grid_.walkable(cand)) walkableFallback = cand;
            }
        }
    }
    return walkableFallback.value_or(SubCell{cx, cy});
}

void TutorialUnits::emitBuildingUpdate(size_t bIdx, bool damaged, bool collapsed) {
    if (bIdx >= buildings_.size()) return;
    const auto& b = buildings_[bIdx];
    BuildingSnapshot snap{
        bIdx, b.x, b.y, b.left, b.top, b.width, b.height,
        b.hp, b.maxHp, b.capacity, static_cast<int>(b.occupants.size()),
        b.owner, b.destroyed, damaged, collapsed, b.occupants
    };
    const auto it = std::ranges::find(buildingUpdates_, bIdx, &BuildingSnapshot::index);
    if (it == buildingUpdates_.end()) {
        buildingUpdates_.push_back(snap);
        return;
    }
    const bool prevDamaged = it->damagedThisTick;
    const bool prevCollapsed = it->collapsedThisTick;
    *it = snap;
    it->damagedThisTick = it->damagedThisTick || prevDamaged;
    it->collapsedThisTick = it->collapsedThisTick || prevCollapsed;
}

void TutorialUnits::setBuildings(std::vector<assets::BuildingPlacement> buildings) {
    buildings_.clear();
    buildingUpdates_.clear();
    buildings_.reserve(buildings.size());
    for (const auto& bp : buildings) {
        Building b{};
        b.x = bp.x;
        b.y = bp.y;
        b.left = bp.left;
        b.top = bp.top;
        b.width = bp.width;
        b.height = bp.height;
        b.maxHp = std::max(100, bp.maxHp);
        b.hp = b.maxHp;
        b.capacity = std::clamp(bp.capacity, 2, 16);
        b.sightBonus = bp.sightBonus > 0 ? bp.sightBonus : 3;
        b.entryCell = findBuildingEntryCell(bp.x, bp.y);
        buildings_.push_back(std::move(b));
    }
}

bool TutorialUnits::canGarrisonBuilding(size_t buildingIdx, int viewerOwner) const {
    if (buildingIdx >= buildings_.size()) return false;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || int(b.occupants.size()) >= b.capacity) return false;
    return b.owner < 0 || !isHostile(b.owner, viewerOwner);
}

size_t TutorialUnits::orderGarrisonBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    const int actor = actorOwner();
    if (!canGarrisonBuilding(buildingIdx, actor)) return 0;
    const auto& b = buildings_[buildingIdx];
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.garrisonTarget = buildingIdx;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
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
        s.goal = b.entryCell;
        s.route.clear();
        s.cursor = 0;
        s.planning = {};
        s.needsRoute = false;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::evacuateBuilding(size_t buildingIdx, std::vector<SoldierPose>* outPoses) {
    if (buildingIdx >= buildings_.size()) return 0;
    auto& b = buildings_[buildingIdx];
    if (commandActorActive_ && b.owner >= 0 && b.owner != actorOwner() && !isAlliedFor(actorOwner(), b.owner)) {
        return 0;
    }
    size_t evacuated = 0;
    for (size_t sid : b.occupants) {
        if (sid >= soldiers_.size()) continue;
        auto& s = soldiers_[sid];
        if (s.hp <= 0) continue;
        const SubCell spot = findSpawnCell(grid_, b.entryCell);
        s.garrisonBuilding.reset();
        s.garrisonTarget.reset();
        const bool vis = !fog_ || isAlliedFor(viewerOwner_, s.owner) ||
                         ((fog_->tile(spot.x / 2, spot.y / 2) & static_cast<std::uint8_t>(1u << viewerOwner_)) != 0);
        placeDisembarkedSoldier(sid, spot, vis, outPoses);
        ++evacuated;
    }
    b.occupants.clear();
    b.owner = -1;
    fogDirty_ = true;
    emitBuildingUpdate(buildingIdx);
    if (evacuated > 0 && fog_) refreshSpotting(outPoses, true);
    populateSoldierPoseStats(outPoses);
    return evacuated;
}

bool TutorialUnits::evacuateSingleOccupant(std::size_t buildingIdx, std::size_t soldierIdx,
                                           std::vector<SoldierPose>* outPoses) {
    if (buildingIdx >= buildings_.size() || soldierIdx >= soldiers_.size()) return false;
    auto& b = buildings_[buildingIdx];
    const int actor = actorOwner();
    if (b.destroyed || b.owner < 0 || (b.owner != actor && !isAlliedFor(actor, b.owner))) {
        return false;
    }
    const auto it = std::ranges::find(b.occupants, soldierIdx);
    if (it == b.occupants.end()) return false;
    auto& s = soldiers_[soldierIdx];
    if (s.hp <= 0) return false;
    if (s.owner != actor && !isAlliedFor(actor, s.owner)) return false;
    const SubCell spot = findSpawnCell(grid_, b.entryCell);
    s.garrisonBuilding.reset();
    s.garrisonTarget.reset();
    const bool vis = !fog_ || isAlliedFor(viewerOwner_, s.owner) ||
                     ((fog_->tile(spot.x / 2, spot.y / 2) & static_cast<std::uint8_t>(1u << viewerOwner_)) != 0);
    placeDisembarkedSoldier(soldierIdx, spot, vis, outPoses);
    b.occupants.erase(it);
    if (b.occupants.empty()) {
        b.owner = -1;
    }
    fogDirty_ = true;
    emitBuildingUpdate(buildingIdx);
    if (fog_) refreshSpotting(outPoses, true);
    populateSoldierPoseStats(outPoses);
    return true;
}

std::optional<bool> TutorialUnits::toggleBuildingHoldFire(size_t buildingIdx) {
    const int actor = actorOwner();
    if (buildingIdx >= buildings_.size()) return std::nullopt;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != actor || b.occupants.empty()) return std::nullopt;
    return toggleHoldFire(b.occupants);
}

std::optional<bool> TutorialUnits::toggleBuildingBinoculars(size_t buildingIdx, std::vector<SoldierPose>* outPoses) {
    const int actor = actorOwner();
    if (buildingIdx >= buildings_.size()) return std::nullopt;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != actor || b.occupants.empty()) return std::nullopt;
    const bool hasSpotter = std::ranges::any_of(b.occupants, [&](size_t sid) {
        return sid < soldiers_.size() && soldiers_[sid].owner == actor &&
               soldiers_[sid].hp > 0 && soldiers_[sid].hasBinoculars;
    });
    if (!hasSpotter) return std::nullopt;
    return toggleBinocularsWithPoses(b.occupants, outPoses);
}

size_t TutorialUnits::orderBuildingAttack(size_t buildingIdx, bool targetIsVehicle, size_t targetIdx) {
    const int actor = actorOwner();
    if (buildingIdx >= buildings_.size()) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != actor || b.occupants.empty()) return 0;
    std::vector<size_t> eligible;
    eligible.reserve(b.occupants.size());
    for (size_t occId : b.occupants) {
        if (occId < soldiers_.size() && soldiers_[occId].canFireFromInside) {
            eligible.push_back(occId);
        }
    }
    if (eligible.empty()) return 0;
    return targetIsVehicle ? attackVehicle(eligible, targetIdx) : orderAttack(eligible, targetIdx);
}

size_t TutorialUnits::orderBuildingAttackBuilding(size_t buildingIdx, size_t targetBuildingIdx) {
    const int actor = actorOwner();
    if (buildingIdx >= buildings_.size() || buildingIdx == targetBuildingIdx) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != actor || b.occupants.empty()) return 0;
    std::vector<size_t> eligible;
    eligible.reserve(b.occupants.size());
    for (size_t occId : b.occupants) {
        if (occId < soldiers_.size() && soldiers_[occId].canFireFromInside) {
            eligible.push_back(occId);
        }
    }
    if (eligible.empty()) return 0;
    return orderAttackBuilding(eligible, targetBuildingIdx);
}

size_t TutorialUnits::orderAttackBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    const int actor = actorOwner();
    if (buildingIdx >= buildings_.size()) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner < 0 || !isHostile(actor, b.owner)) return 0;
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0 || s.embarked || s.ammo <= 0) continue;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.attackBuildingTarget = buildingIdx;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::attackBuildingWithVehicles(std::span<const size_t> vehicleIds, size_t buildingIdx) {
    const int actor = actorOwner();
    if (buildingIdx >= buildings_.size()) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner < 0 || !isHostile(actor, b.owner)) return 0;
    size_t accepted = 0;
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != actor || v.towedBy || !v.armed ||
            (v.ammo <= 0 && (!v.hasSecondary || v.ammo2 <= 0)))
            continue;
        v.pendingTowGun.reset();
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget = buildingIdx;
        v.chasingAttackTarget = false;
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
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
    return accepted;
}

std::vector<BuildingSnapshot> TutorialUnits::takeBuildingUpdates() {
    return std::exchange(buildingUpdates_, {});
}

void TutorialUnits::stepGarrisons(std::vector<SoldierPose>& changed) {
    if (buildings_.empty()) return;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.garrisonTarget) continue;
        const size_t bIdx = *s.garrisonTarget;
        if (!canGarrisonBuilding(bIdx, s.owner)) {
            s.garrisonTarget.reset();
            continue;
        }
        auto& b = buildings_[bIdx];
        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const auto [eu, ev] = InfantryPathGrid::toWorldCenter(b.entryCell);
        const int ex = originX_ + eu - ev;
        const int ey = (eu + ev) / 2;
        const bool inRect = sx >= b.left - 32 && sx <= b.left + b.width + 32 &&
                            sy >= b.top - 32 && sy <= b.top + b.height + 32;
        const double distEntry = det::detHypot(double(ex - sx), double(ey - sy));
        const double distCenter = det::detHypot(double(b.x - sx), double(b.y - sy));
        const double reach = std::max(60.0, std::min(double(b.width), double(b.height)) * 0.55);
        if (inRect || distEntry <= 60.0 || distCenter <= reach) {
            const SubCell oldCell = s.cell;
            s.garrisonBuilding = bIdx;
            s.garrisonTarget.reset();
            s.motion.reset();
            clearMovingSightPenalty(s);
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.crouched = false;
            s.binoculars = false;
            s.spottedMask = 0;
            s.cell = b.entryCell;
            if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) {
                grid_.setOccupied(oldCell, false);
            }
            b.occupants.push_back(i);
            if (b.owner < 0) b.owner = s.owner;
            fogDirty_ = true;
            pushOrUpdateSoldierPose(changed, {
                i, b.x, b.y, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false, 0, 0, true
            });
            emitBuildingUpdate(bIdx);
            continue;
        }
        if (!s.goal || !sameTile(*s.goal, b.entryCell)) {
            s.goal = b.entryCell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
    }
}

void TutorialUnits::damageBuilding(size_t bIdx, int rawDamage, assets::HitType hitType, int attackerOwner,
                                   std::vector<SoldierPose>& changed) {
    (void)attackerOwner;
    if (bIdx >= buildings_.size()) return;
    auto& b = buildings_[bIdx];
    if (b.destroyed) return;
    if (b.owner >= 0) recordHit(b.owner);

    const auto bldDef = catalog_ ? catalog_->buildingDefense() : assets::BuildingDefense::retailDefault();
    const int bldArmor = bldDef.armorFor(hitType);
    const int bldProt = bldDef.protectionFor(hitType);
    int structDmg = 0;
    int splashHits = 0;
    int splashDmg = 0;
    if (hitType == assets::HitType::Fire) {
        // LAND\DEFDOM: ARMOR FIRE 0, PROTECTION FIRE 0 — incendiary weapons bypass stone cover.
        structDmg = std::max(6, rawDamage);
        splashHits = 2;
        splashDmg = std::max(10, rawDamage);
    } else if (hitType == assets::HitType::Explosive || hitType == assets::HitType::Pierce ||
               hitType == assets::HitType::Transpierce || hitType == assets::HitType::Mine) {
        if (rawDamage > bldArmor) {
            structDmg = std::max(12, rawDamage * 3 / 2);
            splashHits = 2;
            splashDmg = std::max(4, rawDamage / 3);
        } else {
            structDmg = std::max(8, rawDamage);
            splashHits = 2;
            splashDmg = std::max(3, rawDamage / 4);
        }
    } else {
        // Small arms (Machine/Sniper): LAND\DEFDOM ARMOR MACHINE 50 prevents rifles from demolishing masonry.
        if (rawDamage > bldArmor) {
            structDmg = std::max(2, (rawDamage - bldArmor) / 2);
        }
        if (nextCombatRandom(3u) == 0u) {
            if (rawDamage > bldProt) {
                splashHits = 1;
                splashDmg = std::max(2, (rawDamage - bldProt) / 2 + 2);
            } else if (rawDamage >= 20) {
                splashHits = 1;
                splashDmg = 1;
            }
        }
    }

    if (splashHits > 0 && splashDmg > 0) {
        int left = splashHits;
        for (size_t sid : b.occupants) {
            if (left <= 0) break;
            if (sid >= soldiers_.size()) continue;
            auto& occ = soldiers_[sid];
            if (occ.hp <= 0) continue;
            occ.hp = std::max(0, occ.hp - splashDmg);
            recordHit(occ.owner, occ.group);
            --left;
            if (occ.hp == 0) {
                occ.garrisonBuilding.reset();
                occ.spottedMask = 0;
                fogDirty_ = true;
                pushOrUpdateSoldierPose(changed, {
                    sid, b.x, b.y, occ.direction, 0, false, 0, false, occ.owner, occ.group, false, false, 0, 0, true
                });
            }
        }
        std::erase_if(b.occupants, [&](size_t sid) { return sid >= soldiers_.size() || soldiers_[sid].hp <= 0; });
        if (b.occupants.empty()) b.owner = -1;
    }
    if (!b.occupants.empty() &&
        (hitType == assets::HitType::Fire || hitType == assets::HitType::Explosive ||
         hitType == assets::HitType::Pierce || hitType == assets::HitType::Transpierce ||
         hitType == assets::HitType::Mine)) {
        const int bldSup = (hitType == assets::HitType::Fire) ? 24 : 18;
        for (size_t sid : b.occupants) {
            applySuppression(sid, bldSup, true);
        }
    }

    b.hp = std::max(0, b.hp - structDmg);
    if (b.hp == 0) {
        b.destroyed = true;
        for (size_t sid : b.occupants) {
            if (sid >= soldiers_.size()) continue;
            auto& occ = soldiers_[sid];
            if (occ.hp <= 0) continue;
            occ.hp = std::max(0, occ.hp - 35);
            if (occ.hp == 0) {
                occ.garrisonBuilding.reset();
                occ.spottedMask = 0;
                pushOrUpdateSoldierPose(changed, {
                    sid, b.x, b.y, occ.direction, 0, false, 0, false, occ.owner, occ.group, false, false, 0, 0, true
                });
            }
        }
        std::erase_if(b.occupants, [&](size_t sid) { return sid >= soldiers_.size() || soldiers_[sid].hp <= 0; });
        for (size_t sid : b.occupants) {
            applySuppression(sid, 30, true);
        }
        (void)evacuateBuilding(bIdx, &changed);
        emitBuildingUpdate(bIdx, true, true);
    } else {
        const int effectiveUnloadHp = std::min(bldDef.unloadHp, std::max(1, b.maxHp / 4));
        if (catalog_ != nullptr && !b.occupants.empty() && b.hp <= effectiveUnloadHp) {
            (void)evacuateBuilding(bIdx, &changed);
        }
        emitBuildingUpdate(bIdx, structDmg > 0, false);
    }
}

int TutorialUnits::bridgeStageForHp(int hp, int maxHp) noexcept {
    if (hp <= 0) return 4;
    // Retail LAND\MOST stages: 1000 1500 500 500 -> thresholds scaled cleanly if maxHp != 3500.
    const int effMax = std::max(100, maxHp);
    if (effMax == 3500) {
        if (hp > 2500) return 0;
        if (hp > 1000) return 1;
        if (hp > 500) return 2;
        return 3;
    }
    const double frac = static_cast<double>(hp) / static_cast<double>(effMax);
    if (frac > 2500.0 / 3500.0) return 0;
    if (frac > 1000.0 / 3500.0) return 1;
    if (frac > 500.0 / 3500.0) return 2;
    return 3;
}

void TutorialUnits::emitBridgeUpdate(size_t brIdx, bool damaged, bool collapsed, bool repaired) {
    if (brIdx >= bridges_.size()) return;
    const auto& br = bridges_[brIdx];
    BridgeSnapshot snap{
        brIdx, br.x, br.y, br.left, br.top, br.width, br.height,
        br.hp, br.maxHp, br.stage, br.destroyed, damaged, collapsed, repaired
    };
    const auto it = std::ranges::find(bridgeUpdates_, brIdx, &BridgeSnapshot::index);
    if (it == bridgeUpdates_.end()) {
        bridgeUpdates_.push_back(snap);
        return;
    }
    const bool prevDamaged = it->damagedThisTick;
    const bool prevCollapsed = it->collapsedThisTick;
    const bool prevRepaired = it->repairedThisTick;
    *it = snap;
    it->damagedThisTick = it->damagedThisTick || prevDamaged;
    it->collapsedThisTick = it->collapsedThisTick || prevCollapsed;
    it->repairedThisTick = it->repairedThisTick || prevRepaired;
}

void TutorialUnits::setBridges(std::vector<assets::BridgePlacement> bridges) {
    bridges_.clear();
    bridgeUpdates_.clear();
    bridges_.reserve(bridges.size());
    for (auto& bp : bridges) {
        Bridge br{};
        br.x = bp.x;
        br.y = bp.y;
        br.left = bp.left;
        br.top = bp.top;
        br.width = bp.width;
        br.height = bp.height;
        br.maxHp = std::max(100, bp.maxHp);
        br.hp = br.maxHp;
        br.stage = 0;
        br.destroyed = false;
        br.cells = std::move(bp.cells);
        bridges_.push_back(std::move(br));
    }
}

std::vector<BridgeSnapshot> TutorialUnits::takeBridgeUpdates() {
    return std::exchange(bridgeUpdates_, {});
}

double TutorialUnits::distanceToBridge(const Bridge& br, double px, double py) const {
    const double cx = std::clamp(px, static_cast<double>(br.left), static_cast<double>(br.left + br.width));
    const double cy = std::clamp(py, static_cast<double>(br.top), static_cast<double>(br.top + br.height));
    const double boxDist = det::detHypot(px - cx, py - cy);
    if (br.cells.empty() || boxDist > 256.0) return boxDist;
    double best = 1e18;
    bool anyCell = false;
    for (const auto& bc : br.cells) {
        if ((bc.mask & 0x05u) == 0u) continue;
        anyCell = true;
        const int wu = bc.subX * 16 + 8;
        const int wv = bc.subY * 16 + 8;
        const double mx = static_cast<double>(originX_ + wu - wv);
        const double my = static_cast<double>(wu + wv) * 0.5;
        best = std::min(best, det::detHypot(px - mx, py - my));
    }
    return anyCell ? best : boxDist;
}

std::pair<int, int> TutorialUnits::bridgeRepairApproachPoint(const Bridge& br, const Vehicle& v) const {
    double bestAnchorX = static_cast<double>(br.x);
    double bestAnchorY = static_cast<double>(br.y);
    double bestAnchorDist = 1e18;
    for (const auto& bc : br.cells) {
        if ((bc.mask & 0x01u) == 0u) continue;
        const int wu = bc.subX * 16 + 8;
        const int wv = bc.subY * 16 + 8;
        const double mx = static_cast<double>(originX_ + wu - wv);
        const double my = static_cast<double>(wu + wv) * 0.5;
        const double d = det::detHypot(v.fx - mx, v.fy - my);
        if (d < bestAnchorDist) {
            bestAnchorDist = d;
            bestAnchorX = mx;
            bestAnchorY = my;
        }
    }
    const double dx = v.fx - bestAnchorX;
    const double dy = v.fy - bestAnchorY;
    const double len = det::detHypot(dx, dy);
    if (len >= 1.0) {
        const double ux = dx / len;
        const double uy = dy / len;
        for (int r = 48; r <= 144; r += 16) {
            const int px = std::clamp(static_cast<int>(det::detRoundToInt(bestAnchorX + ux * static_cast<double>(r))),
                                      0, std::max(0, mapPixelW_ - 1));
            const int py = std::clamp(static_cast<int>(det::detRoundToInt(bestAnchorY + uy * static_cast<double>(r))),
                                      0, std::max(0, mapPixelH_ - 1));
            const SubCell c = pointerCell(px, py, originX_);
            if (grid_.inBounds(c) && grid_.walkableMask(c, v.forbiddenTerrainMask)) {
                return {px, py};
            }
        }
    }
    return approachPointNear(v.fx, v.fy, bestAnchorX, bestAnchorY, 72.0, mapPixelW_, mapPixelH_);
}

void TutorialUnits::applyBridgeStageTransition(size_t brIdx, int prevStage, int newStage,
                                               bool killPlungedUnits, std::vector<SoldierPose>* changed) {
    if (brIdx >= bridges_.size()) return;
    const auto& br = bridges_[brIdx];
    const std::uint8_t prevBit = (prevStage >= 0 && prevStage < 4)
        ? static_cast<std::uint8_t>(0x10u << prevStage) : 0u;
    const std::uint8_t newBit = (newStage >= 0 && newStage < 4)
        ? static_cast<std::uint8_t>(0x10u << newStage) : 0u;

    std::vector<SubCell> newlyBlocked;
    for (const auto& bc : br.cells) {
        if ((bc.mask & 0x01u) == 0u) continue;
        const bool wasPassable = prevBit != 0u && (bc.mask & prevBit) != 0u;
        const bool nowPassable = newBit != 0u && (bc.mask & newBit) != 0u;
        if (wasPassable == nowPassable) continue;
        const SubCell cell{bc.subX, bc.subY};
        grid_.setSubCellBlocked(cell, !nowPassable);
        if (!nowPassable && killPlungedUnits) {
            newlyBlocked.push_back(cell);
        }
    }
    if (newlyBlocked.empty() || !changed) return;

    auto isCollapsedCell = [&](SubCell c) {
        for (const SubCell& nb : newlyBlocked) {
            if (nb == c) return true;
            if ((nb.x >> 1) == (c.x >> 1) && (nb.y >> 1) == (c.y >> 1) && !grid_.walkable(c)) {
                return true;
            }
        }
        return false;
    };

    for (size_t si = 0; si < soldiers_.size(); ++si) {
        auto& s = soldiers_[si];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        if (isCollapsedCell(s.cell)) {
            if (!vehicleAtCell(s.cell)) grid_.setOccupied(s.cell, false);
            s.hp = 0;
            s.motion.reset();
            clearMovingSightPenalty(s);
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.queuedGoals.clear();
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
            s.binoculars = false;
            s.spottedMask = 0;
            fogDirty_ = true;
            const auto [su, sv] = soldierWorld(s);
            pushOrUpdateSoldierPose(*changed, {
                si, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false, 0, false, s.owner, s.group,
                false, false
            });
        } else if (!s.route.empty() && s.goal) {
            s.needsRoute = true;
        }
    }

    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        auto& v = vehicles_[vi];
        if (!v.alive || v.hp <= 0 || v.isNaval) continue;
        if (!isCollapsedCell(v.cell)) continue;
        // Drown embarked passengers inside the submerged vehicle instead of bailing onto banks.
        for (size_t pIdx : v.passengers) {
            if (pIdx >= soldiers_.size()) continue;
            auto& ps = soldiers_[pIdx];
            if (ps.hp <= 0) continue;
            ps.embarked = false;
            ps.hp = 0;
            ps.motion.reset();
            clearMovingSightPenalty(ps);
            ps.goal.reset();
            ps.route.clear();
            ps.cursor = 0;
            ps.needsRoute = false;
            ps.queuedGoals.clear();
            ps.attackMove = false;
            ps.attackTarget.reset();
            ps.attackVehicleTarget.reset();
            ps.attackBuildingTarget.reset();
            ps.attackGround.reset();
            ps.autoTarget.reset();
            ps.boardVehicleTarget.reset();
            ps.captureVehicleTarget.reset();
            ps.garrisonTarget.reset();
            ps.healTarget.reset();
            ps.escortVehicleTarget.reset();
            ps.escortSoldierTarget.reset();
            ps.disarmMineTarget.reset();
            ps.binoculars = false;
            ps.spottedMask = 0;
            fogDirty_ = true;
            pushOrUpdateSoldierPose(*changed, {
                pIdx, v.x, v.y, ps.direction, 0, false, 0, false, ps.owner, ps.group, false, false
            });
        }
        v.passengers.clear();
        destroyVehicle(vi, *changed);
        emitVehiclePose(vi);
    }
}

void TutorialUnits::damageBridge(size_t brIdx, int rawDamage, assets::HitType hitType, int attackerOwner,
                                 std::vector<SoldierPose>& changed) {
    (void)attackerOwner;
    if (brIdx >= bridges_.size() || rawDamage <= 0) return;
    auto& br = bridges_[brIdx];
    if (br.destroyed || br.hp <= 0) return;

    int structDmg = 0;
    if (hitType == assets::HitType::Explosive || hitType == assets::HitType::Mine) {
        structDmg = std::max(25, rawDamage * 2);
    } else if (hitType == assets::HitType::Pierce || hitType == assets::HitType::Transpierce) {
        structDmg = std::max(12, rawDamage);
    } else if (hitType == assets::HitType::Fire) {
        structDmg = std::max(6, rawDamage / 2);
    } else {
        return;
    }

    const int prevStage = br.stage;
    br.hp = std::max(0, br.hp - structDmg);
    const int newStage = bridgeStageForHp(br.hp, br.maxHp);
    br.stage = newStage;
    br.destroyed = (newStage >= 4 || br.hp == 0);
    if (newStage != prevStage) {
        applyBridgeStageTransition(brIdx, prevStage, newStage, true, &changed);
    }
    emitBridgeUpdate(brIdx, true, prevStage < 4 && br.destroyed, false);
}

void TutorialUnits::damageBridgesAtImpact(int impactX, int impactY, double radius, int rawDamage,
                                          assets::HitType hitType, int attackerOwner,
                                          std::vector<SoldierPose>& changed) {
    if (bridges_.empty() || rawDamage <= 0) return;
    if (hitType == assets::HitType::Machine || hitType == assets::HitType::Sniper) return;
    for (size_t bi = 0; bi < bridges_.size(); ++bi) {
        const auto& br = bridges_[bi];
        if (br.destroyed || br.hp <= 0) continue;
        if (distanceToBridge(br, static_cast<double>(impactX), static_cast<double>(impactY)) <= radius) {
            damageBridge(bi, rawDamage, hitType, attackerOwner, changed);
        }
    }
}
}
