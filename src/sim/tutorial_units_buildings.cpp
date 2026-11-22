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
        s.cell = spot;
        s.motion.reset();
        clearMovingSightPenalty(s);
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        grid_.setOccupied(spot, true);
        if (outPoses) {
            const auto [u, w] = soldierWorld(s);
            const bool vis = !fog_ || isAlliedFor(viewerOwner_, s.owner) ||
                             ((fog_->tile(spot.x / 2, spot.y / 2) & static_cast<std::uint8_t>(1u << viewerOwner_)) != 0);
            pushOrUpdateSoldierPose(*outPoses, {
                sid, originX_ + u - w, (u + w) / 2, s.direction, 0, false,
                s.hp, true, s.owner, s.group, false, vis, 0, 0, false
            });
        }
        ++evacuated;
    }
    b.occupants.clear();
    b.owner = -1;
    fogDirty_ = true;
    emitBuildingUpdate(buildingIdx);
    if (evacuated > 0 && fog_) refreshSpotting(outPoses, true);
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
    return evacuated;
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
        emitBuildingUpdate(bIdx, structDmg > 0, false);
    }
}
}
