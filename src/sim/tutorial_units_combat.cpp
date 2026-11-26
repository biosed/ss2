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
constexpr double kInfantryVsVehicleRangePx = 240.0;
std::uint8_t directionFromDelta(int du, int dv) {
    return det::detDir8(du, dv);
}
int sniperHitPercent(int armor) {
    switch (armor) {
    case 0: return 100;
    case 1: return 75;
    case 2: return 50;
    case 3: return 25;
    default: return 10;
    }
}
double distToBuildingPerimeter(int left, int top, int width, int height, int bx, int by, int tx, int ty) {
    if (width > 0 && height > 0) {
        const double cx = std::clamp(static_cast<double>(tx), static_cast<double>(left),
                                     static_cast<double>(left + width));
        const double cy = std::clamp(static_cast<double>(ty), static_cast<double>(top),
                                     static_cast<double>(top + height));
        return det::detHypot(static_cast<double>(tx) - cx, static_cast<double>(ty) - cy);
    }
    return det::detHypot(static_cast<double>(tx - bx), static_cast<double>(ty - by));
}
}
bool TutorialUnits::isValidAttackTarget(size_t targetIndex, int viewerOwner) const {
    if (!fog_ || targetIndex >= soldiers_.size()) return false;
    const auto& target = soldiers_[targetIndex];
    if (target.hp <= 0 || target.embarked || target.garrisonBuilding || !isHostile(viewerOwner, target.owner)) return false;
    return (target.spottedMask & (1u << viewerOwner)) != 0;
}

size_t TutorialUnits::orderAttack(std::span<const size_t> ids, size_t targetIndex) {
    const int actor = actorOwner();
    if (!isValidAttackTarget(targetIndex, actor)) return 0;
    size_t accepted = 0;
    for (size_t id : ids) {
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
        s.attackTarget = targetIndex;
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::isSoldierCamouflaged(const Soldier& s) const {
    return s.isSniper && s.hp > 0 && !s.embarked && !s.garrisonBuilding &&
           s.crouched && !s.motion && s.route.empty() &&
           (s.lastFireTick == 0 || (tick_ >= s.lastFireTick && tick_ - s.lastFireTick >= 35u));
}

bool TutorialUnits::isSoldierCamouflaged(size_t index) const {
    return index < soldiers_.size() && isSoldierCamouflaged(soldiers_[index]);
}

int TutorialUnits::officerAuraDecayAt(int owner, double sx, double sy) const {
    if (owner < 0 || owner >= 4) return 1;
    int best = 1;
    for (size_t j = 0; j < soldiers_.size(); ++j) {
        const auto& other = soldiers_[j];
        if (other.hp <= 0 || other.embarked || other.owner != owner || !other.isOfficer) {
            continue;
        }
        double ox = 0.0;
        double oy = 0.0;
        if (other.garrisonBuilding) {
            const size_t ob = *other.garrisonBuilding;
            if (ob >= buildings_.size() || buildings_[ob].destroyed) continue;
            ox = static_cast<double>(buildings_[ob].x);
            oy = static_cast<double>(buildings_[ob].y);
        } else {
            const auto [ou, ov] = soldierWorld(other);
            ox = static_cast<double>(originX_ + ou - ov);
            oy = static_cast<double>((ou + ov) / 2);
        }
        if (det::detHypot(ox - sx, oy - sy) <= static_cast<double>(other.officerAuraRadius)) {
            best = std::max(best, other.moraleRallyRate);
        }
    }
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.owner != owner || !v.isPropaganda) {
            continue;
        }
        if (det::detHypot(v.fx - sx, v.fy - sy) <= 200.0) {
            best = std::max(best, 3);
        }
    }
    return best;
}

int TutorialUnits::officerAuraDecay(size_t soldierIdx) const {
    if (soldierIdx >= soldiers_.size()) return 1;
    const auto& s = soldiers_[soldierIdx];
    if (s.hp <= 0 || s.embarked) return 1;
    if (s.garrisonBuilding) {
        const size_t bIdx = *s.garrisonBuilding;
        if (bIdx >= buildings_.size() || buildings_[bIdx].destroyed) return 1;
        return officerAuraDecayAt(s.owner, static_cast<double>(buildings_[bIdx].x),
                                  static_cast<double>(buildings_[bIdx].y));
    }
    const auto [su, sv] = soldierWorld(s);
    const double sx = static_cast<double>(originX_ + su - sv);
    const double sy = static_cast<double>((su + sv) / 2);
    return officerAuraDecayAt(s.owner, sx, sy);
}

bool TutorialUnits::hasNearbyFriendlyOfficer(size_t soldierIdx) const {
    return officerAuraDecay(soldierIdx) > 1;
}

void TutorialUnits::applySuppression(size_t soldierIdx, int addTicks, bool allowGarrisoned) {
    if (soldierIdx >= soldiers_.size() || addTicks <= 0) return;
    auto& tgt = soldiers_[soldierIdx];
    if (tgt.hp <= 0 || tgt.embarked || (!allowGarrisoned && tgt.garrisonBuilding)) return;
    int effective = addTicks;
    if (tgt.moraleResist >= 5000) {
        effective = std::max(1, addTicks / 4);
    } else if (tgt.moraleResist >= 1000) {
        effective = std::max(1, addTicks / 2);
    }
    tgt.suppressionTicks = std::min(35, tgt.suppressionTicks + effective);
}

void TutorialUnits::applyVehicleSuppression(size_t vehicleIdx, int addTicks, int threatX, int threatY) {
    if (vehicleIdx >= vehicles_.size() || addTicks <= 0) return;
    auto& tv = vehicles_[vehicleIdx];
    if (!tv.alive || tv.hp <= 0 || !tv.hasCrew || (!multiplayerSymmetry_ && tv.owner == 3)) return;
    int effective = addTicks;
    if (tv.moraleResist >= 10000) {
        effective = std::max(1, addTicks / 4);
    } else if (tv.moraleResist >= 4000) {
        effective = std::max(1, addTicks / 2);
    }
    const bool wasSuppressed = tv.suppressionTicks > 0;
    const int cap = (tv.moraleRndMove < 0) ? 60 : 35;
    tv.suppressionTicks = std::min(cap, tv.suppressionTicks + effective);
    if (!wasSuppressed && tv.suppressionTicks > 0 && tv.marchEnabled && tv.hatchOpen) {
        fogDirty_ = true;
    }
    if (tv.moraleRndMove < 0 && tv.moraleResist < 10000 && tv.speedPx > 0.0 &&
        !tv.towedBy && !tv.isMoveImpaired() &&
        (tv.lastPanicTick == 0u || tick_ >= tv.lastPanicTick + 60u)) {
        const int panicThresh = std::clamp((-tv.moraleRndMove) / 400, 38, 56);
        if (tv.suppressionTicks >= panicThresh) {
            double dx = (threatX >= 0 && threatY >= 0) ? (tv.fx - static_cast<double>(threatX)) : 0.0;
            double dy = (threatX >= 0 && threatY >= 0) ? (tv.fy - static_cast<double>(threatY)) : 0.0;
            double len = det::detHypot(dx, dy);
            if (len < 1e-3) {
                const double ang = (static_cast<double>((tv.hullDir + 16u) & 31u) / 32.0) * kTwoPi;
                const auto sc = det::detSinCos(ang);
                const double du = sc.c;
                const double dv = sc.s;
                dx = du - dv;
                dy = 0.5 * (du + dv);
                len = std::max(1e-3, det::detHypot(dx, dy));
            }
            const int fleeX = std::clamp(static_cast<int>(det::detRoundToInt(tv.fx + (dx / len) * 72.0)),
                                         32, std::max(32, mapPixelW_ - 32));
            const int fleeY = std::clamp(static_cast<int>(det::detRoundToInt(tv.fy + (dy / len) * 72.0)),
                                         32, std::max(32, mapPixelH_ - 32));
            tv.pendingTowGun.reset();
            tv.queuedOrders.clear();
            tv.attackMove = false;
            tv.attackMoveEngaging = false;
            moveVehicleInternal(tv, fleeX, fleeY);
            if (tv.canMoveBackward) tv.reverseMove = true;
            tv.lastPanicTick = tick_ > 0u ? tick_ : 1u;
        }
    }
}

void TutorialUnits::applySoldierShotReload(Soldier& s) {
    ++s.attackBurst;
    int reloadVal = scaleByExpa(s.desc.reloadBase, s.desc.reloadDelta, s.expa);
    if (s.attackBurst >= s.desc.burstCount) {
        s.attackBurst = 0;
        reloadVal = scaleByExpa(s.desc.burstReloadBase, s.desc.burstReloadDelta, s.expa);
    }
    if (s.isSuppressed() && !s.fearless) {
        reloadVal = (reloadVal * 135 + 50) / 100;
    }
    s.attackReload = static_cast<std::int16_t>(std::max(1, reloadVal));
}

size_t TutorialUnits::orderAttackGroundSoldiers(std::span<const size_t> indices, int mapX, int mapY) {
    const int actor = actorOwner();
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        const bool mortarOrFlamer = (s.isMortar || s.isFlamer) && s.ammo > 0;
        const bool smgGrenade = s.canGrenadeGround && s.atGrenades > 0;
        if (s.owner != actor || s.hp <= 0 || s.embarked || s.garrisonBuilding ||
            (!mortarOrFlamer && !smgGrenade)) {
            continue;
        }
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
        s.attackMove = false;
        s.attackGround = std::pair{clampedX, clampedY};
        ++accepted;
    }
    return accepted;
}

std::optional<unsigned> TutorialUnits::lastHitTick(unsigned player) const {
    if (player >= lastHitTick_.size()) return std::nullopt;
    return lastHitTick_[player];
}

std::optional<unsigned> TutorialUnits::lastGroupHitTick(unsigned group) const {
    if (group >= lastGroupHitTick_.size()) return std::nullopt;
    return lastGroupHitTick_[group];
}

void TutorialUnits::recordHit(int owner, int group) {
    if (owner >= 0 && owner < static_cast<int>(lastHitTick_.size())) {
        lastHitTick_[static_cast<std::size_t>(owner)] = tick_;
    }
    if (group >= 0 && group < static_cast<int>(lastGroupHitTick_.size())) {
        lastGroupHitTick_[static_cast<std::size_t>(group)] = tick_;
    }
}

std::uint32_t TutorialUnits::nextCombatRandom(std::uint32_t mask) {
    combatRandomState_ = combatRandomState_ * 0x41c64e6du + 0x3039u;
    const auto shifted = static_cast<std::int32_t>(combatRandomState_) >> 16;
    return static_cast<std::uint32_t>(shifted) & mask;
}

void TutorialUnits::detonateKamikazeSoldier(size_t index, int impactX, int impactY,
                                            std::optional<size_t> directVehIdx,
                                            std::optional<size_t> directBldIdx,
                                            std::vector<SoldierPose>& changed) {
    if (index >= soldiers_.size()) return;
    auto& s = soldiers_[index];
    if (s.hp <= 0) return;
    const auto [su, sv] = soldierWorld(s);
    const int sx = originX_ + su - sv;
    const int sy = (su + sv) / 2;
    const int blastDmg = std::max(300, scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa));
    if (directVehIdx && *directVehIdx < vehicles_.size()) {
        auto& tv = vehicles_[*directVehIdx];
        if (tv.alive && tv.hp > 0 && isHostile(s.owner, tv.owner)) {
            const int dmg = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             sx, sy, blastDmg, assets::HitType::Explosive));
            const bool wasAlive = tv.alive && tv.hp > 0;
            tv.hp = std::max(0, tv.hp - dmg);
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            reactToAttack(tv.owner, tv.group, sx, sy, *directVehIdx);
            if (wasAlive && tv.hp == 0) {
                destroyVehicle(*directVehIdx, changed);
                ++s.kills;
                s.expa = std::min(0x8000, s.expa + 4096);
            } else if (tv.hp > 0) {
                applyVehicleCrewDamage(*directVehIdx, sx, sy, blastDmg, assets::HitType::Explosive,
                                       changed, static_cast<int>(index), -1);
            }
            emitVehiclePose(*directVehIdx);
        }
    }
    if (directBldIdx && *directBldIdx < buildings_.size() && !buildings_[*directBldIdx].destroyed) {
        damageBuilding(*directBldIdx, blastDmg, assets::HitType::Explosive, s.owner, changed);
    }
    applyMortarSplash(impactX, impactY, blastDmg, s.owner, static_cast<int>(index),
                      static_cast<int>(index), changed,
                      directVehIdx ? static_cast<int>(*directVehIdx) : -1);
    if (!mines_.empty()) {
        const SubCell impCell = pointerCell(impactX, impactY, originX_);
        triggerMineAtTile(impCell.x / 2, impCell.y / 2, changed, true);
    }
    if (!vehicleAtCell(s.cell)) grid_.setOccupied(s.cell, false);
    s.hp = 0;
    s.ammo = 0;
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
    s.binoculars = false;
    s.spottedMask = 0;
    fogDirty_ = true;
    const bool vis = !s.garrisonBuilding && isVisibleToViewer(s.owner, s.spottedMask);
    pushOrUpdateSoldierPose(changed, {
        index, sx, sy, s.direction, 0, false, 0, false, s.owner, s.group,
        true, vis, impactX, impactY, false, s.expa, s.kills, false,
        s.atGrenades, false, false, s.mineAmmo, 0, true
    });
}

void TutorialUnits::damageTankDesantRiders(size_t vehicleIdx, int rawDamage, assets::HitType hitType,
                                           std::vector<SoldierPose>& changed,
                                           int attackerSoldierIdx, int attackerVehicleIdx) {
    if (vehicleIdx >= vehicles_.size() || rawDamage <= 0) return;
    auto& tv = vehicles_[vehicleIdx];
    if (!tv.alive || tv.hp <= 0 || !tv.soldiersOnArmor || tv.passengers.empty()) return;

    const int riderProt = catalog_
        ? catalog_->tankArmorProtection(hitType)
        : ((hitType == assets::HitType::Pierce || hitType == assets::HitType::Transpierce ||
            hitType == assets::HitType::Explosive || hitType == assets::HitType::Mine) ? 3 : 0);
    if (rawDamage <= riderProt) return;

    const bool splashAll = (hitType == assets::HitType::Explosive ||
                            hitType == assets::HitType::Fire ||
                            hitType == assets::HitType::Mine);
    int riderDmg = 0;
    if (splashAll) {
        riderDmg = std::clamp(rawDamage / 4, 12, 65);
    } else if (hitType == assets::HitType::Machine || hitType == assets::HitType::Sniper) {
        riderDmg = std::clamp(rawDamage / 2, 10, 45);
    } else {
        riderDmg = std::clamp(rawDamage / 8, 6, 25);
    }
    riderDmg = std::max(1, riderDmg - riderProt);

    std::vector<size_t> remaining;
    remaining.reserve(tv.passengers.size());
    bool anyKilled = false;
    const size_t nPass = tv.passengers.size();
    for (size_t idx = 0; idx < nPass; ++idx) {
        const size_t pIdx = tv.passengers[idx];
        const bool targeted = splashAll || (idx == nPass - 1);
        if (!targeted || pIdx >= soldiers_.size() || soldiers_[pIdx].hp <= 0) {
            remaining.push_back(pIdx);
            continue;
        }
        auto& ps = soldiers_[pIdx];
        const int scaledRiderDmg = scaleVehicleDamageForDifficulty(ps.owner, riderDmg);
        ps.hp = std::max(0, ps.hp - scaledRiderDmg);
        recordHit(ps.owner, ps.group);
        if (ps.hp > 0) {
            const int addSup = splashAll ? 25 : 18;
            const int effSup = ps.moraleResist >= 5000
                ? std::max(1, addSup / 4)
                : (ps.moraleResist >= 1000 ? std::max(1, addSup / 2) : addSup);
            ps.suppressionTicks = std::min(35, ps.suppressionTicks + effSup);
            remaining.push_back(pIdx);
            continue;
        }
        ps.embarked = false;
        ps.cell = tv.cell;
        ps.motion.reset();
        clearMovingSightPenalty(ps);
        ps.goal.reset();
        ps.route.clear();
        ps.cursor = 0;
        ps.needsRoute = false;
        ps.queuedGoals.clear();
        ps.patrolOrigin.reset();
        ps.patrolDest.reset();
        ps.attackMove = false;
        ps.boardVehicleTarget.reset();
        ps.captureVehicleTarget.reset();
        ps.garrisonTarget.reset();
        ps.attackTarget.reset();
        ps.attackVehicleTarget.reset();
        ps.attackBuildingTarget.reset();
        ps.attackGround.reset();
        ps.autoTarget.reset();
        ps.healTarget.reset();
        ps.escortVehicleTarget.reset();
        ps.escortSoldierTarget.reset();
        ps.disarmMineTarget.reset();
        ps.binoculars = false;
        ps.suppressionTicks = 0;
        ps.spottedMask = 0;
        fogDirty_ = true;
        if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size() &&
            isHostile(soldiers_[static_cast<size_t>(attackerSoldierIdx)].owner, ps.owner)) {
            auto& attS = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
            ++attS.kills;
            attS.expa = std::min(0x8000, attS.expa + 1500);
        } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size() &&
                   isHostile(vehicles_[static_cast<size_t>(attackerVehicleIdx)].owner, ps.owner)) {
            auto& attV = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
            ++attV.kills;
            attV.expa = std::min(0x8000, attV.expa + 1500);
        }
        const bool vis = !fog_ || isAlliedWithPlayer(ps.owner) || tv.visible;
        pushOrUpdateSoldierPose(changed, {
            pIdx, tv.x, tv.y, ps.direction, 0, false, 0, false, ps.owner, ps.group, false, vis
        });
        anyKilled = true;
    }
    tv.passengers = std::move(remaining);
    if (anyKilled) {
        emitVehiclePose(vehicleIdx);
    }
}

void TutorialUnits::applyVehicleCrewDamage(size_t vehicleIdx, int attackerX, int attackerY,
                                           int rawDamage, assets::HitType hitType,
                                           std::vector<SoldierPose>& changed,
                                           int attackerSoldierIdx, int attackerVehicleIdx) {
    if (vehicleIdx >= vehicles_.size() || rawDamage <= 0) return;
    auto& tv = vehicles_[vehicleIdx];
    if (!tv.alive || tv.hp <= 0 || (!multiplayerSymmetry_ && tv.owner == 3)) return;
    damageTankDesantRiders(vehicleIdx, rawDamage, hitType, changed, attackerSoldierIdx, attackerVehicleIdx);
    const bool unbuttonedCommander = (hitType == assets::HitType::Sniper) && isVehicleUnbuttoned(tv);
    if (!tv.hasCrew || (!tv.hasCrewProtection && !unbuttonedCommander)) return;
    int rawCrewDmg = tv.hasCrewProtection
        ? applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.crewProtection, true,
                             attackerX, attackerY, rawDamage, hitType)
        : 0;
    if (rawCrewDmg <= 0 && unbuttonedCommander) {
        // Gold 0x1001ceb0 / 0x100014c9: unbuttoned crew uses MISC\MISC tanksonarmorprotection ((256 - val) * dmg) >> 8;
        // sniper keeps default 0x100 (0 protection -> full 100% rawDamage).
        const int prot = catalog_ ? std::clamp(catalog_->tankArmorProtection(hitType), 0, 256) : 0;
        rawCrewDmg = std::max(1, ((256 - prot) * rawDamage) >> 8);
    }
    if (rawCrewDmg <= 0) return;

    int fracDmg = rawCrewDmg;
    if (hitType == assets::HitType::Explosive) {
        fracDmg = std::max(1, rawCrewDmg / 2);
    } else if (hitType == assets::HitType::Pierce ||
               hitType == assets::HitType::Transpierce ||
               hitType == assets::HitType::Mine) {
        fracDmg = std::max(1, rawCrewDmg / 4);
    }
    if (tv.crewVulnerability > 0.0) {
        const double mult = std::clamp(tv.crewVulnerability, 0.10, 2.0);
        fracDmg = std::max(1, static_cast<int>(det::detRoundToInt(static_cast<double>(fracDmg) * mult)));
    }
    const int scaled = scaleVehicleDamageForDifficulty(tv.owner, fracDmg);
    if (scaled <= 0) return;
    tv.crewHp = std::max(0, tv.crewHp - scaled);
    if (tv.crewHp > 0) {
        applyVehicleSuppression(vehicleIdx,
                                (hitType == assets::HitType::Explosive || hitType == assets::HitType::Fire ||
                                 hitType == assets::HitType::Mine)
                                    ? 24 : 16,
                                attackerX, attackerY);
        return;
    }

    if (!tv.passengers.empty()) {
        bailoutPassengers(vehicleIdx, changed);
    }
    if (tv.towedVehicle) detachTowedGun(vehicleIdx);
    else if (tv.towedBy) detachTowedGun(*tv.towedBy);

    tv.hasCrew = false;
    tv.suppressionTicks = 0;
    tv.wasSuppressed = false;
    tv.owner = 3;
    tv.moveTarget.reset();
    tv.waypoints.clear();
    tv.queuedOrders.clear();
    tv.patrolOrigin.reset();
    tv.patrolDest.reset();
    tv.patrolToDest = true;
    tv.attackTarget.reset();
    tv.attackGround.reset();
    tv.attackBuildingTarget.reset();
    tv.pendingTowGun.reset();
    tv.chasingAttackTarget = false;
    tv.attackMove = false;
    tv.attackMoveEngaging = false;
    tv.reverseMove = false;
    tv.facingTargetDir.reset();
    tv.supportTargetVehicle.reset();
    tv.supportTargetBuilding.reset();
    tv.supportTargetBridge.reset();
    tv.supportTargetSoldier.reset();
    tv.escortVehicleTarget.reset();
    tv.escortSoldierTarget.reset();
    tv.wasMoving = false;
    tv.fireCooldown = 0;
    fogDirty_ = true;

    for (auto& ss : soldiers_) {
        if (ss.attackVehicleTarget && *ss.attackVehicleTarget == vehicleIdx) {
            ss.attackVehicleTarget.reset();
        }
    }
    for (auto& ov : vehicles_) {
        if (ov.attackTarget && ov.attackTarget->first && ov.attackTarget->second == vehicleIdx) {
            ov.attackTarget.reset();
        }
    }
    if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
        auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
        ++att.kills;
        att.expa = std::min(0x8000, att.expa + 3072);
    } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size()) {
        auto& av = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
        ++av.kills;
        av.expa = std::min(0x8000, av.expa + 3500);
    }
    emitVehiclePose(vehicleIdx, false, 0, 0, false, {}, false, false, false, false, false,
                    false, false, true);
}

void TutorialUnits::stepSoldierAttackVehicle(Soldier& s, size_t index, std::vector<SoldierPose>& changed) {
    constexpr double kAtGrenadeRangePx = 88.0;
    const bool canGrenade = !s.isAntiVehicle && !s.isKamikaze && !s.isSniper && !s.isFlamer && s.atGrenades > 0;
    if (s.ammo <= 0 && !canGrenade) {
        s.attackVehicleTarget.reset();
        return;
    }
    std::optional<size_t> targetVid;
    bool explicitOrder = false;
    const bool isGar = s.garrisonBuilding.has_value() &&
                       *s.garrisonBuilding < buildings_.size() &&
                       !buildings_[*s.garrisonBuilding].destroyed;
    int sx = 0;
    int sy = 0;
    int su = 0;
    int sv = 0;
    if (isGar) {
        const auto& gb = buildings_[*s.garrisonBuilding];
        sx = gb.x;
        sy = gb.y;
        const int duv = sx - originX_;
        const int suv = 2 * sy;
        su = (suv + duv) / 2;
        sv = (suv - duv) / 2;
    } else {
        std::tie(su, sv) = soldierWorld(s);
        sx = originX_ + su - sv;
        sy = (su + sv) / 2;
    }

    const int garRangeBonus = catalog_
        ? std::clamp(catalog_->buildingDefense().bonusShotRange / 3, 32, 64)
        : 48;
    const double sniperReach = std::max(
        kInfantryVsVehicleRangePx,
        static_cast<double>(scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa)) + (isGar ? garRangeBonus : 0.0));
    const double atCatalogRange =
        static_cast<double>(scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa)) + (isGar ? garRangeBonus : 0.0);
    const double atReach = s.hasCatalogAtShot ? atCatalogRange : kInfantryVsVehicleRangePx;
    const double atMinReach = (s.hasCatalogAtShot && s.desc.deadzoneBase > 0)
        ? static_cast<double>(scaleByExpa(s.desc.deadzoneBase, s.desc.deadzoneDelta, s.expa))
        : 0.0;
    const double atGrenadeReach = kAtGrenadeRangePx + (isGar ? 24.0 : 0.0);
    const auto distToVehicle = [&](const Vehicle& tv) {
        if (isGar) {
            const auto& gb = buildings_[*s.garrisonBuilding];
            return distToBuildingPerimeter(gb.left, gb.top, gb.width, gb.height, gb.x, gb.y, tv.x, tv.y);
        }
        return det::detHypot(double(tv.x - sx), double(tv.y - sy));
    };
    if (s.attackVehicleTarget) {
        const size_t vid = *s.attackVehicleTarget;
        if (!isVehicleTargetValidFor(s.owner, true, vid)) {
            s.attackVehicleTarget.reset();
            return;
        }
        targetVid = vid;
        explicitOrder = true;
    } else if (fog_ && (autoEngage_ || s.attackMove) && !s.holdFire &&
               (s.isAntiVehicle || s.isKamikaze || s.isSniper || canGrenade) && !vehicles_.empty()) {
        const double autoRange = s.isSniper
            ? sniperReach
            : (s.isAntiVehicle ? atReach : (s.isKamikaze ? kInfantryVsVehicleRangePx : atGrenadeReach));
        const int sniperRaw = std::max(85, scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa));
        int bestTier = 99;
        double bestDist = autoRange + 1.0;
        for (size_t j = 0; j < vehicles_.size(); ++j) {
            if (!isVehicleTargetValidFor(s.owner, true, j)) continue;
            const auto& tv = vehicles_[j];
            const double d = distToVehicle(tv);
            if (d > autoRange || (s.isAntiVehicle && d < atMinReach)) continue;
            if (s.isSniper && !isVehicleUnbuttoned(tv) &&
                (!tv.hasCrewProtection || !tv.hasCrew ||
                 applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.crewProtection, true,
                                    sx, sy, sniperRaw, assets::HitType::Sniper) <= 0)) {
                continue;
            }
            int tier = 0;
            if (s.hasAttackPref) {
                const int cls = assets::UnitSpec::targetClassIndex(tv.effectiveTargetType());
                const int pref = (cls >= 0) ? s.attackPref[static_cast<std::size_t>(cls)] : 0;
                if (pref == 1) continue;
                tier = (pref >= 2) ? (256 - pref) : 300;
            } else {
                const bool targetArmed = tv.armed && (tv.ammo > 0 || tv.ammo2 > 0);
                tier = targetArmed ? 0 : 1;
            }
            if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                bestTier = tier;
                bestDist = d;
                targetVid = j;
            }
        }
    }
    if (!targetVid) return;

    auto& tv = vehicles_[*targetVid];
    const double distMap = distToVehicle(tv);
    const double engageRange = s.isKamikaze
        ? 56.0
        : (s.isSniper ? sniperReach : (canGrenade ? atGrenadeReach : atReach));
    if ((explicitOrder || s.isKamikaze) && distMap > engageRange) {
        if (!isGar && (!s.goal || !sameTile(*s.goal, tv.cell))) {
            s.goal = tv.cell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
        return;
    }

    if (!isGar && (explicitOrder || s.isKamikaze)) {
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
    }

    const double minEngage = (!canGrenade && s.isAntiVehicle) ? atMinReach : 0.0;
    if (!s.motion && distMap >= minEngage && distMap <= engageRange && s.attackReload <= 0) {
        if (s.isKamikaze) {
            const auto [vu, vv] = tutorialFootWorld(tv.x, tv.y, originX_);
            s.direction = directionFromDelta(vu - su, vv - sv);
            detonateKamikazeSoldier(index, tv.x, tv.y, *targetVid, std::nullopt, changed);
            return;
        }
        const bool throwGrenade = canGrenade && distMap <= atGrenadeReach;
        if (!throwGrenade && s.ammo <= 0) return;
        if (!throwGrenade && !isGar && s.crouchToFire && !s.crouched) {
            const auto [vu, vv] = tutorialFootWorld(tv.x, tv.y, originX_);
            s.crouched = true;
            s.direction = directionFromDelta(vu - su, vv - sv);
            s.attackReload = 4;
            fogDirty_ = true;
            const bool vis = isVisibleToViewer(s.owner, s.spottedMask);
            pushOrUpdateSoldierPose(changed, {
                index, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, false, vis,
                0, 0, false, s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s),
                s.mineAmmo, s.ammo, false, s.isSuppressed(), std::int8_t{1}
            });
            return;
        }

        const auto [vu, vv] = tutorialFootWorld(tv.x, tv.y, originX_);
        s.direction = directionFromDelta(vu - su, vv - sv);
        s.firing = true;
        s.fireAnim = 0;
        s.lastFireTick = tick_;
        if (s.isSniper) fogDirty_ = true;

        const int atCatalogDmg = s.hasCatalogAtShot
            ? std::max(1, scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa))
            : 420;
        const int rawDmg = throwGrenade
            ? 260
            : (tv.hasCatalogArmor
                   ? (s.isFlamer ? 140
                                 : (s.isAntiVehicle ? atCatalogDmg
                                                    : (s.isSniper ? std::max(85, scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa)) : 30)))
                   : (s.isAntiVehicle ? (s.hasCatalogAtShot ? std::max(1, atCatalogDmg / 10) : 35)
                                      : (s.isSniper ? 15 : 5)));
        const auto hitType = throwGrenade
            ? assets::HitType::Explosive
            : (s.isFlamer ? assets::HitType::Fire
                          : (s.isAntiVehicle
                                 ? ((s.hasCatalogAtShot && s.primaryHitType != assets::HitType::None)
                                        ? s.primaryHitType
                                        : assets::HitType::Pierce)
                                 : (s.isSniper ? assets::HitType::Sniper : assets::HitType::Machine)));
        bool hitLanded = true;
        if (!throwGrenade && s.hasCatalogAtShot && s.desc.accuracyBase > 0) {
            const int effAcc = std::max(0, scaleByExpa(s.desc.accuracyBase, s.desc.accuracyDelta, s.expa));
            if (effAcc > 0) {
                const int hitPct = std::clamp(100 - effAcc, 35, 98);
                const int roll = static_cast<int>((nextCombatRandom(0x7fffu) * 100u) >> 15);
                hitLanded = (roll <= hitPct);
            }
        }
        if (hitLanded) {
            const int dmg = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor, sx, sy, rawDmg, hitType));
            const auto armorHit = computeArmorHitOutcome(
                tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor, sx, sy, rawDmg, hitType, dmg);
            const bool targetWasAlive = tv.alive && tv.hp > 0;
            tv.hp = std::max(0, tv.hp - dmg);
            if (dmg > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
                tv.expa = std::min(0x8000, tv.expa + std::min(200, dmg * tv.expaOnHit));
            }
            recordHit(tv.owner, tv.group);
            if (targetWasAlive && tv.hp == 0) {
                destroyVehicle(*targetVid, changed);
                ++s.kills;
                s.expa = std::min(0x8000, s.expa + 4096);
            } else if (tv.hp > 0) {
                applyVehicleCrewDamage(*targetVid, sx, sy, rawDmg, hitType, changed, static_cast<int>(index), -1);
            }
            markVehicleCombat(tv);
            reactToAttack(tv.owner, tv.group, sx, sy, *targetVid);
            emitVehiclePose(*targetVid, false, 0, 0, false, {}, false, false, false, false, false,
                            false, false, false, armorHit.outcome, armorHit.impactDir32);
            if (!throwGrenade && s.isFlamer) {
                applyFlameSplash(tv.x, tv.y, rawDmg, s.owner, static_cast<int>(index), -1, -1, changed,
                                 static_cast<int>(*targetVid));
            }
        }

        if (throwGrenade) {
            --s.atGrenades;
        } else {
            --s.ammo;
        }
        applySoldierShotReload(s);
        const bool vis = !isGar && isVisibleToViewer(s.owner, s.spottedMask);
        pushOrUpdateSoldierPose(changed, {
            index, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, true, vis, tv.x, tv.y,
            isGar, s.expa, s.kills, throwGrenade, s.atGrenades, !throwGrenade && s.isFlamer,
            isSoldierCamouflaged(s), s.mineAmmo, s.ammo, !throwGrenade && s.isAntiVehicle && !s.isFlamer
        });
    }
}

void TutorialUnits::applyFlameSplash(int impactX, int impactY, int rawDamage, int attackerOwner,
                                     int attackerSoldierIdx, int attackerVehicleIdx, int skipSoldierIdx,
                                     std::vector<SoldierPose>& changed, int skipVehicleIdx) {
    constexpr double kFlameSplashR = 44.0;
    for (size_t k = 0; k < soldiers_.size(); ++k) {
        if (skipSoldierIdx >= 0 && k == static_cast<size_t>(skipSoldierIdx)) continue;
        auto& ss = soldiers_[k];
        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(attackerOwner, ss.owner)) continue;
        const auto [su, sv] = soldierWorld(ss);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double sd = det::detHypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
        if (sd > kFlameSplashR) continue;
        const int splashDmg = scaleVehicleDamageForDifficulty(
            ss.owner, std::max(1, static_cast<int>(det::detRoundToInt(
                static_cast<double>(rawDamage) * 0.45 * (1.0 - sd / 45.0)))));
        ss.hp = std::max(0, ss.hp - splashDmg);
        recordHit(ss.owner, ss.group);
        reactToAttack(ss.owner, ss.group, impactX, impactY);
        if (ss.hp == 0) {
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 2048);
            } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size()) {
                auto& av = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
                ++av.kills;
                av.expa = std::min(0x8000, av.expa + 1500);
            }
            if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
            ss.motion.reset();
            clearMovingSightPenalty(ss);
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
        const bool sVis = isVisibleToViewer(ss.owner, ss.spottedMask);
        pushOrUpdateSoldierPose(changed, {
            k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis
        });
    }
    if (skipVehicleIdx == -2) return;
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        if (skipVehicleIdx >= 0 && vi == static_cast<size_t>(skipVehicleIdx)) continue;
        auto& tv = vehicles_[vi];
        if (!tv.alive || tv.hp <= 0 || !isHostile(attackerOwner, tv.owner)) continue;
        const double vd = det::detHypot(tv.fx - static_cast<double>(impactX), tv.fy - static_cast<double>(impactY));
        if (vd > kFlameSplashR) continue;
        const int rawSplash = std::max(1, static_cast<int>(det::detRoundToInt(
            static_cast<double>(rawDamage) * 0.45 * (1.0 - vd / 45.0))));
        const int dmg = scaleVehicleDamageForDifficulty(
            tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                           impactX, impactY, rawSplash, assets::HitType::Fire));
        const int prevCrewHp = tv.crewHp;
        const int prevOwner = tv.owner;
        const bool wasAlive = tv.alive && tv.hp > 0;
        if (dmg > 0) {
            tv.hp = std::max(0, tv.hp - dmg);
        }
        if (wasAlive && tv.hp == 0) {
            destroyVehicle(vi, changed);
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 4096);
            } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size()) {
                auto& av = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
                ++av.kills;
                av.expa = std::min(0x8000, av.expa + 4500);
            }
        } else if (tv.hp > 0) {
            applyVehicleCrewDamage(vi, impactX, impactY, rawSplash, assets::HitType::Fire,
                                   changed, attackerSoldierIdx, attackerVehicleIdx);
        }
        if (dmg > 0 || tv.crewHp != prevCrewHp) {
            recordHit(prevOwner, tv.group);
            markVehicleCombat(tv);
            if (tv.hasCrew) {
                reactToAttack(prevOwner, tv.group, impactX, impactY, vi);
            }
            emitVehiclePose(vi);
        }
    }
    damageBridgesAtImpact(impactX, impactY, 56.0, rawDamage, assets::HitType::Fire, attackerOwner, changed);
}

void TutorialUnits::applyMortarSplash(int impactX, int impactY, int rawDamage, int attackerOwner,
                                      int attackerSoldierIdx, int skipSoldierIdx,
                                      std::vector<SoldierPose>& changed, int skipVehicleIdx) {
    constexpr double kMortarSplashR = 52.0;
    for (size_t k = 0; k < soldiers_.size(); ++k) {
        if (skipSoldierIdx >= 0 && k == static_cast<size_t>(skipSoldierIdx)) continue;
        auto& ss = soldiers_[k];
        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(attackerOwner, ss.owner)) continue;
        const auto [su, sv] = soldierWorld(ss);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double sd = det::detHypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
        if (sd > kMortarSplashR) continue;
        const double falloff = 1.0 - 0.55 * (sd / kMortarSplashR);
        const int splashDmg = scaleVehicleDamageForDifficulty(
            ss.owner, std::max(1, static_cast<int>(det::detRoundToInt(
                static_cast<double>(rawDamage) * 0.65 * falloff))));
        ss.hp = std::max(0, ss.hp - splashDmg);
        if (ss.hp > 0) applySuppression(k, 25);
        recordHit(ss.owner, ss.group);
        reactToAttack(ss.owner, ss.group, impactX, impactY);
        if (ss.hp == 0) {
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 2048);
            }
            if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
            ss.motion.reset();
            clearMovingSightPenalty(ss);
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
        const bool sVis = isVisibleToViewer(ss.owner, ss.spottedMask);
        pushOrUpdateSoldierPose(changed, {
            k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis
        });
    }
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        if (skipVehicleIdx >= 0 && vi == static_cast<size_t>(skipVehicleIdx)) continue;
        auto& tv = vehicles_[vi];
        if (!tv.alive || tv.hp <= 0 || !isHostile(attackerOwner, tv.owner)) continue;
        const double vd = det::detHypot(tv.fx - static_cast<double>(impactX), tv.fy - static_cast<double>(impactY));
        if (vd > kMortarSplashR) continue;
        const double falloff = 1.0 - 0.55 * (vd / kMortarSplashR);
        const int rawSplash = std::max(1, static_cast<int>(det::detRoundToInt(
            static_cast<double>(rawDamage) * 0.65 * falloff)));
        const int dmg = scaleVehicleDamageForDifficulty(
            tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                           impactX, impactY, rawSplash, assets::HitType::Explosive));
        const int prevCrewHp = tv.crewHp;
        const int prevOwner = tv.owner;
        const bool wasAlive = tv.alive && tv.hp > 0;
        if (dmg > 0) {
            tv.hp = std::max(0, tv.hp - dmg);
        }
        if (wasAlive && tv.hp == 0) {
            destroyVehicle(vi, changed);
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 4096);
            }
        } else if (tv.hp > 0) {
            applyVehicleCrewDamage(vi, impactX, impactY, rawSplash, assets::HitType::Explosive,
                                   changed, attackerSoldierIdx, -1);
        }
        if (dmg > 0 || tv.crewHp != prevCrewHp) {
            recordHit(prevOwner, tv.group);
            markVehicleCombat(tv);
            if (tv.hasCrew) {
                reactToAttack(prevOwner, tv.group, impactX, impactY, vi);
            }
            emitVehiclePose(vi);
        }
    }
    damageBridgesAtImpact(impactX, impactY, 72.0, rawDamage, assets::HitType::Explosive, attackerOwner, changed);
}

void TutorialUnits::stepCombat(std::vector<SoldierPose>& changed) {
    if (!fog_) return;
    if (fogDirty_) refreshSpotting(&changed);

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked) continue;
        if (s.attackReload > 0) --s.attackReload;
        if (s.firing) {
            if (s.fireAnim == 255) {
                s.firing = false;
                s.fireAnim = 0;
            } else {
                s.fireAnim = static_cast<std::uint8_t>(std::min(255, int(s.fireAnim) + 15));
            }
        }

        const bool isGar = s.garrisonBuilding.has_value();
        if (isGar && !s.canFireFromInside) {
            s.attackTarget.reset();
            s.attackVehicleTarget.reset();
            s.attackBuildingTarget.reset();
            s.autoTarget.reset();
            continue;
        }
        const int effX = isGar ? buildings_[*s.garrisonBuilding].x : 0;
        const int effY = isGar ? buildings_[*s.garrisonBuilding].y : 0;
        const auto [su, sv] = isGar ? tutorialFootWorld(effX, effY, originX_) : soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const int garRangeBonus = catalog_
            ? std::clamp(catalog_->buildingDefense().bonusShotRange / 3, 32, 64)
            : 48;
        const int maxRange = scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa) + (isGar ? garRangeBonus : 0);
        const int baseDeadzone = scaleByExpa(s.desc.deadzoneBase, s.desc.deadzoneDelta, s.expa);
        const int garDeadzoneBonus = (isGar && baseDeadzone >= 40)
            ? std::clamp((catalog_ ? catalog_->buildingDefense().bonusShotDeadZone : 150) / 5, 16, 48)
            : 0;
        const int minRange = isGar ? (baseDeadzone >= 40 ? baseDeadzone + garDeadzoneBonus : 0) : baseDeadzone;

        // Explicit attack order takes precedence over transient auto-engage.
        std::optional<size_t> activeTarget;
        bool explicitOrder = false;
        if (s.attackTarget) {
            const size_t tid = *s.attackTarget;
            if (tid >= soldiers_.size() || !isValidAttackTarget(tid, s.owner) || s.ammo <= 0) {
                s.attackTarget.reset();
            } else {
                activeTarget = tid;
                explicitOrder = true;
                s.autoTarget.reset();
            }
        }
        if (!activeTarget && s.attackVehicleTarget) {
            s.autoTarget.reset();
            stepSoldierAttackVehicle(s, i, changed);
            continue;
        }
        std::optional<size_t> activeBldTarget;
        bool explicitBldOrder = false;
        if (!activeTarget && s.attackBuildingTarget) {
            const size_t bid = *s.attackBuildingTarget;
            if (bid >= buildings_.size() || buildings_[bid].destroyed ||
                buildings_[bid].owner < 0 || !isHostile(s.owner, buildings_[bid].owner) || s.ammo <= 0) {
                s.attackBuildingTarget.reset();
            } else {
                activeBldTarget = bid;
                explicitBldOrder = true;
            }
        }
        if (!activeTarget && !activeBldTarget && !s.attackGround && (autoEngage_ || s.attackMove) && !s.holdFire &&
            (s.isAntiVehicle || s.isKamikaze) && s.ammo > 0 && !vehicles_.empty()) {
            const double atReach = s.hasCatalogAtShot
                ? static_cast<double>(scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa)) + (isGar ? garRangeBonus : 0.0)
                : kInfantryVsVehicleRangePx;
            const double atMinReach = (s.hasCatalogAtShot && s.desc.deadzoneBase > 0)
                ? static_cast<double>(scaleByExpa(s.desc.deadzoneBase, s.desc.deadzoneDelta, s.expa))
                : 0.0;
            bool vehicleInReach = false;
            int maxVehPref = -1;
            for (size_t j = 0; j < vehicles_.size(); ++j) {
                if (!isVehicleTargetValidFor(s.owner, true, j)) continue;
                const double d = isGar
                    ? distToBuildingPerimeter(buildings_[*s.garrisonBuilding].left, buildings_[*s.garrisonBuilding].top,
                                              buildings_[*s.garrisonBuilding].width, buildings_[*s.garrisonBuilding].height,
                                              buildings_[*s.garrisonBuilding].x, buildings_[*s.garrisonBuilding].y,
                                              vehicles_[j].x, vehicles_[j].y)
                    : det::detHypot(double(vehicles_[j].x - sx), double(vehicles_[j].y - sy));
                if (d <= atReach && (!s.isAntiVehicle || d >= atMinReach)) {
                    if (s.hasAttackPref) {
                        const int cls = assets::UnitSpec::targetClassIndex(vehicles_[j].effectiveTargetType());
                        const int pref = (cls >= 0) ? s.attackPref[static_cast<std::size_t>(cls)] : 0;
                        if (pref == 1) continue;
                        maxVehPref = std::max(maxVehPref, pref);
                    }
                    vehicleInReach = true;
                }
            }
            if (vehicleInReach && s.hasAttackPref) {
                const int manPref = s.attackPref[static_cast<std::size_t>(assets::TargetClass::Man)];
                if (manPref >= 2 && manPref > maxVehPref) {
                    const double manLimit = double((maxRange * 112) / 100);
                    for (size_t j = 0; j < soldiers_.size(); ++j) {
                        if (j == i || !isValidAttackTarget(j, s.owner)) continue;
                        const auto [tu, tv] = soldierWorld(soldiers_[j]);
                        const double d = det::detHypot(double(tu - su), double(tv - sv));
                        if (d >= double(minRange) && d <= manLimit) {
                            vehicleInReach = false;
                            break;
                        }
                    }
                }
            }
            if (vehicleInReach) {
                s.autoTarget.reset();
                stepSoldierAttackVehicle(s, i, changed);
                continue;
            }
        }
        if (!activeTarget && !activeBldTarget && !isGar && s.attackGround &&
            (((s.isMortar || s.isFlamer) && s.ammo > 0) || (s.canGrenadeGround && s.atGrenades > 0))) {
            const bool mortarOrFlamer = (s.isMortar || s.isFlamer);
            const int groundMaxR = mortarOrFlamer ? maxRange : 140;
            const int groundMinR = mortarOrFlamer ? minRange : 0;
            const auto [gx, gy] = *s.attackGround;
            const auto [gu, gv] = tutorialFootWorld(gx, gy, originX_);
            const double dist = det::detHypot(double(gx - sx), double(gy - sy));
            if (dist > double(groundMaxR)) {
                const SubCell targetCell = pointerCell(gx, gy, originX_);
                if (!s.goal || !sameTile(*s.goal, targetCell)) {
                    s.goal = targetCell;
                    s.route.clear();
                    s.cursor = 0;
                    s.planning = {};
                    s.needsRoute = false;
                }
                continue;
            }
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            if (dist < double(groundMinR)) {
                continue;
            }
            if (!s.motion && s.attackReload <= 0) {
                s.direction = directionFromDelta(gu - su, gv - sv);
                s.firing = true;
                s.fireAnim = 0;
                s.lastFireTick = tick_;
                bool threwGrenade = false;
                if (s.isMortar) {
                    const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
                    applyMortarSplash(gx, gy, rawDamage, s.owner, static_cast<int>(i), -1, changed);
                    if (!mines_.empty()) {
                        const SubCell impCell = pointerCell(gx, gy, originX_);
                        const int mtx = impCell.x / 2;
                        const int mty = impCell.y / 2;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                triggerMineAtTile(mtx + dx, mty + dy, changed, true);
                            }
                        }
                    }
                    --s.ammo;
                } else if (s.isFlamer) {
                    const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
                    applyFlameSplash(gx, gy, rawDamage, s.owner, static_cast<int>(i), -1, -1, changed);
                    --s.ammo;
                } else {
                    threwGrenade = true;
                    --s.atGrenades;
                    applyMortarSplash(gx, gy, 85, s.owner, static_cast<int>(i), static_cast<int>(i), changed);
                    if (!mines_.empty()) {
                        const SubCell impCell = pointerCell(gx, gy, originX_);
                        triggerMineAtTile(impCell.x / 2, impCell.y / 2, changed, true);
                    }
                }
                applySoldierShotReload(s);
                const bool vis = isVisibleToViewer(s.owner, s.spottedMask);
                changed.push_back({
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group,
                    true, vis, gx, gy, false, s.expa, s.kills, threwGrenade, s.atGrenades,
                    s.isFlamer, isSoldierCamouflaged(s), s.mineAmmo, s.ammo, s.isMortar
                });
                if (threwGrenade || s.ammo <= 0) s.attackGround.reset();
            }
            continue;
        }
        const int acquireMaxRange = s.isKamikaze ? std::max(maxRange, 180) : maxRange;
        const bool attackerElevated = !isGar && fog_ && fog_->isElevated(s.cell.x / 2, s.cell.y / 2);
        const auto effectiveSoldRange = [&](int baseR, const Soldier& target) {
            if (!attackerElevated || fog_->isElevated(target.cell.x / 2, target.cell.y / 2)) {
                return baseR;
            }
            return (baseR * 112) / 100;
        };
        const auto soldierTargetDist = [&](const Soldier& target) {
            const auto [tu, tv] = soldierWorld(target);
            if (isGar) {
                const auto& gb = buildings_[*s.garrisonBuilding];
                return distToBuildingPerimeter(gb.left, gb.top, gb.width, gb.height, gb.x, gb.y,
                                               originX_ + tu - tv, (tu + tv) / 2);
            }
            return det::detHypot(double(tu - su), double(tv - sv));
        };
        const auto buildingTargetDist = [&](const Building& tb) {
            const double rawD = isGar
                ? distToBuildingPerimeter(buildings_[*s.garrisonBuilding].left, buildings_[*s.garrisonBuilding].top,
                                          buildings_[*s.garrisonBuilding].width, buildings_[*s.garrisonBuilding].height,
                                          buildings_[*s.garrisonBuilding].x, buildings_[*s.garrisonBuilding].y,
                                          tb.x, tb.y)
                : det::detHypot(double(tb.x - sx), double(tb.y - sy));
            return std::max(0.0, rawD - std::min(tb.width, tb.height) * 0.4);
        };
        if (!activeTarget && !activeBldTarget && (autoEngage_ || s.attackMove) && !s.holdFire && s.ammo > 0) {
            const int manPref = s.hasAttackPref
                ? s.attackPref[static_cast<std::size_t>(assets::TargetClass::Man)] : 0;
            const int bldPref = s.hasAttackPref
                ? s.attackPref[static_cast<std::size_t>(assets::TargetClass::Building)] : 0;
            const bool allowInfAuto = !s.hasAttackPref || manPref != 1;
            const bool allowBldAuto = !s.hasAttackPref || bldPref != 1;
            // Keep existing autoTarget while alive, spotted, and in range; otherwise acquire best target.
            if (s.autoTarget) {
                const size_t tid = *s.autoTarget;
                if (!allowInfAuto || tid >= soldiers_.size() || !isValidAttackTarget(tid, s.owner)) {
                    s.autoTarget.reset();
                } else {
                    const double d = soldierTargetDist(soldiers_[tid]);
                    if (d < double(minRange) || d > double(effectiveSoldRange(acquireMaxRange, soldiers_[tid]))) {
                        s.autoTarget.reset();
                    }
                }
            }
            if (!s.autoTarget && allowInfAuto) {
                double bestDist = double((acquireMaxRange * 112) / 100) + 1.0;
                std::optional<size_t> bestTarget;
                for (size_t j = 0; j < soldiers_.size(); ++j) {
                    if (j == i || !isValidAttackTarget(j, s.owner)) continue;
                    const double d = soldierTargetDist(soldiers_[j]);
                    if (d >= double(minRange) && d <= double(effectiveSoldRange(acquireMaxRange, soldiers_[j])) && d < bestDist) {
                        bestDist = d;
                        bestTarget = j;
                    }
                }
                s.autoTarget = bestTarget;
            }
            if (s.autoTarget) activeTarget = *s.autoTarget;
            const bool bldOutranksMan = s.hasAttackPref && bldPref >= 2 && (manPref < 2 || bldPref > manPref);
            if ((!activeTarget || bldOutranksMan) && allowBldAuto && !buildings_.empty()) {
                double bestBDist = double(acquireMaxRange) + 1.0;
                std::optional<size_t> bestBld;
                for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                    const auto& tb = buildings_[bi];
                    if (tb.destroyed || tb.owner < 0 || !isHostile(s.owner, tb.owner) || tb.occupants.empty()) continue;
                    if ((fog_->tile(tb.entryCell.x / 2, tb.entryCell.y / 2) & (1u << s.owner)) == 0) continue;
                    const double d = buildingTargetDist(tb);
                    if (d <= double(acquireMaxRange) && d < bestBDist) {
                        bestBDist = d;
                        bestBld = bi;
                    }
                }
                if (bestBld) {
                    if (bldOutranksMan) activeTarget.reset();
                    activeBldTarget = bestBld;
                }
            }
        }
        if (!activeTarget && activeBldTarget) {
            const auto& tb = buildings_[*activeBldTarget];
            const auto [bu, bv] = tutorialFootWorld(tb.x, tb.y, originX_);
            const double dist = buildingTargetDist(tb);
            const double bldReach = s.isKamikaze ? std::max(56.0, double(maxRange)) : double(maxRange);
            if ((explicitBldOrder || s.isKamikaze) && dist > bldReach) {
                if (!isGar && (!s.goal || !sameTile(*s.goal, tb.entryCell))) {
                    s.goal = tb.entryCell;
                    s.route.clear();
                    s.cursor = 0;
                    s.planning = {};
                    s.needsRoute = false;
                }
                continue;
            }
            if ((explicitBldOrder || s.isKamikaze) && !isGar) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                s.needsRoute = false;
            }
            if (!s.motion && dist <= bldReach && s.attackReload <= 0) {
                if (s.isKamikaze) {
                    s.direction = directionFromDelta(bu - su, bv - sv);
                    detonateKamikazeSoldier(i, tb.x, tb.y, std::nullopt, *activeBldTarget, changed);
                    continue;
                }
                s.direction = directionFromDelta(bu - su, bv - sv);
                s.firing = true;
                s.fireAnim = 0;
                s.lastFireTick = tick_;
                if (s.isSniper) fogDirty_ = true;
                const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
                const auto hitType = (s.isFlamer || s.isMortar)
                    ? (s.isFlamer ? assets::HitType::Fire : assets::HitType::Explosive)
                    : (s.isAntiVehicle ? assets::HitType::Pierce : assets::HitType::Machine);
                damageBuilding(*activeBldTarget, rawDamage, hitType, s.owner, changed);
                --s.ammo;
                applySoldierShotReload(s);
                const bool vis = !isGar && isVisibleToViewer(s.owner, s.spottedMask);
                changed.push_back({
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group,
                    true, vis, tb.x, tb.y, isGar, s.expa, s.kills, false, s.atGrenades,
                    s.isFlamer, isSoldierCamouflaged(s), s.mineAmmo, s.ammo, s.isMortar
                });
            }
            continue;
        }
        if (!activeTarget) {
            if ((s.isAntiVehicle || s.isKamikaze || s.isSniper || s.atGrenades > 0) && !vehicles_.empty()) {
                stepSoldierAttackVehicle(s, i, changed);
            }
            continue;
        }

        const size_t tid = *activeTarget;
        const auto& t = soldiers_[tid];
        const auto [tu, tv] = soldierWorld(t);
        const double dist = soldierTargetDist(t);
        const int effMaxRange = effectiveSoldRange(maxRange, t);
        const double infReach = s.isKamikaze ? std::max(56.0, double(effMaxRange)) : double(effMaxRange);

        if ((explicitOrder || s.isKamikaze) && dist > infReach) {
            if (!isGar && (!s.goal || !sameTile(*s.goal, t.cell))) {
                s.goal = t.cell;
                s.route.clear();
                s.cursor = 0;
                s.planning = {};
                s.needsRoute = false;
            }
            continue;
        }

        if ((explicitOrder || s.isKamikaze) && !isGar) {
            // Explicit attack in range: halt movement permanently.
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
        }

        if (!s.motion && dist >= double(minRange) && dist <= infReach && s.attackReload <= 0) {
            if (s.isKamikaze) {
                s.direction = directionFromDelta(tu - su, tv - sv);
                detonateKamikazeSoldier(i, originX_ + tu - tv, (tu + tv) / 2, std::nullopt, std::nullopt, changed);
                continue;
            }
            if (!isGar && s.crouchToFire && !s.crouched) {
                s.crouched = true;
                s.direction = directionFromDelta(tu - su, tv - sv);
                s.attackReload = 4;
                fogDirty_ = true;
                const bool vis = isVisibleToViewer(s.owner, s.spottedMask);
                pushOrUpdateSoldierPose(changed, {
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, false, vis,
                    0, 0, false, s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s),
                    s.mineAmmo, s.ammo, false, s.isSuppressed(), std::int8_t{1}
                });
                continue;
            }
            s.direction = directionFromDelta(tu - su, tv - sv);
            s.firing = true;
            s.fireAnim = 0;
            s.lastFireTick = tick_;
            if (s.isSniper) fogDirty_ = true;

            // Exact 0x10059df6..0x10059e59 two-draw scatter
            (void)nextCombatRandom(0x7fffu);
            (void)nextCombatRandom(0x7fffu);

            const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
            const int speed = std::max(1, s.desc.projectileSpeed);
            const int flightTicks = std::max(1, int(std::ceil(dist / double(speed))));
            const int baseHitPct = sniperHitPercent(t.desc.armorSniper);
            const int marksmanBonus = (s.isSniper && s.crouched && !s.motion) ? 20 : 0;
            const int hitPct = s.isSniper
                ? std::clamp(baseHitPct + marksmanBonus - (t.crouched ? 25 : 0), 10, 95)
                : (t.crouched ? std::max(10, baseHitPct - 25) : baseHitPct);
            projectiles_.push_back(Projectile{
                i, tid, rawDamage, hitPct, std::max(1, flightTicks - 1), s.isFlamer, s.isMortar, s.isHeavyMg
            });

            const int expaGain = scaleByExpa(s.desc.expaBase, s.desc.expaDelta, s.expa);
            s.expa = std::min(0x8000, s.expa + expaGain);
            --s.ammo;
            applySoldierShotReload(s);
            const bool vis = !isGar && isVisibleToViewer(s.owner, s.spottedMask);
            changed.push_back({
                i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, true, vis,
                originX_ + tu - tv, (tu + tv) / 2, isGar, s.expa, s.kills, false, s.atGrenades,
                s.isFlamer, isSoldierCamouflaged(s), s.mineAmmo, s.ammo, s.isMortar
            });
        }
    }

    stepOpenTopPassengerFire(changed);

    // Advance in-flight projectiles (0x10088100 -> 0x10057040 -> 0x100488f0)
    std::vector<Projectile> alive;
    alive.reserve(projectiles_.size());
    for (auto& p : projectiles_) {
        --p.ticksRemaining;
        if (p.ticksRemaining > 0) {
            alive.push_back(p);
            continue;
        }
        (void)nextCombatRandom(0xffu);
        const std::uint32_t rHit = nextCombatRandom(0x7fffu);
        const int roll = (int(rHit) * 100) >> 15;
        if (roll <= p.hitPct && p.target < soldiers_.size()) {
            auto& tgt = soldiers_[p.target];
            if (tgt.hp > 0) {
                const int dmg = scaleVehicleDamageForDifficulty(tgt.owner, p.rawDamage);
                tgt.hp = std::max(0, tgt.hp - dmg);
                if (tgt.hp > 0 && tgt.expaOnHit > 0) {
                    tgt.expa = std::min(0x8000, tgt.expa + tgt.expaOnHit);
                }
                if (tgt.hp > 0 && (p.isHeavyMg || p.isMortar)) {
                    applySuppression(p.target, p.isMortar ? 25 : 20);
                }
                recordHit(tgt.owner, tgt.group);
                if (p.attacker < soldiers_.size()) {
                    const auto [au, av] = soldierWorld(soldiers_[p.attacker]);
                    reactToAttack(tgt.owner, tgt.group, originX_ + au - av, (au + av) / 2);
                }
                if (tgt.hp > 0 && tgt.hp < (tgt.desc.maxHp >> 1) && p.rawDamage > (tgt.desc.maxHp >> 4)) {
                    (void)nextCombatRandom(1u);
                }
                const auto [tu, tv] = soldierWorld(tgt);
                const int impX = originX_ + tu - tv;
                const int impY = (tu + tv) / 2;
                const int attOwner = (p.attacker < soldiers_.size()) ? soldiers_[p.attacker].owner : -1;
                if (tgt.hp == 0) {
                    if (p.attacker < soldiers_.size()) {
                        auto& att = soldiers_[p.attacker];
                        ++att.kills;
                        att.expa = std::min(0x8000, att.expa + 2048);
                        if (att.hp > 0) {
                            const auto [au, av] = soldierWorld(att);
                            const bool aVis = !att.embarked && !att.garrisonBuilding &&
                                isVisibleToViewer(att.owner, att.spottedMask);
                            pushOrUpdateSoldierPose(changed, {
                                p.attacker, originX_ + au - av, (au + av) / 2,
                                att.motion ? att.motion->direction : att.direction,
                                att.motion ? att.motion->progress : 0u,
                                att.motion ? att.motion->running : false,
                                att.hp, true, att.owner, att.group, false, aVis, 0, 0,
                                att.garrisonBuilding.has_value(), att.expa, att.kills, false, att.atGrenades,
                                false, isSoldierCamouflaged(att)
                            });
                        }
                    }
                    if (!vehicleAtCell(tgt.cell)) grid_.setOccupied(tgt.cell, false);
                    tgt.motion.reset();
                    clearMovingSightPenalty(tgt);
                    tgt.goal.reset();
                    tgt.route.clear();
                    tgt.cursor = 0;
                    tgt.attackTarget.reset();
                    tgt.attackVehicleTarget.reset();
                    tgt.autoTarget.reset();
                    tgt.binoculars = false;
                    tgt.spottedMask = 0;
                    fogDirty_ = true;
                }
                const bool tVis = !tgt.garrisonBuilding &&
                    isVisibleToViewer(tgt.owner, tgt.spottedMask);
                changed.push_back({
                    p.target, impX, impY, tgt.direction, 0, false, tgt.hp, tgt.hp > 0, tgt.owner, tgt.group,
                    false, tVis, 0, 0, tgt.garrisonBuilding.has_value(), tgt.expa, tgt.kills, false,
                    tgt.atGrenades, false, isSoldierCamouflaged(tgt)
                });
                if (p.isFlamer && attOwner >= 0) {
                    applyFlameSplash(impX, impY, p.rawDamage, attOwner,
                                     static_cast<int>(p.attacker), -1, static_cast<int>(p.target), changed);
                } else if (p.isMortar && attOwner >= 0) {
                    applyMortarSplash(impX, impY, p.rawDamage, attOwner,
                                      static_cast<int>(p.attacker), static_cast<int>(p.target), changed);
                }
            }
        } else if (p.isMortar && p.target < soldiers_.size() && p.attacker < soldiers_.size()) {
            const auto& tgt = soldiers_[p.target];
            const int attOwner = soldiers_[p.attacker].owner;
            if (tgt.hp > 0 && !tgt.embarked && !tgt.garrisonBuilding && attOwner >= 0) {
                const auto [tu, tv] = soldierWorld(tgt);
                applyMortarSplash(originX_ + tu - tv, (tu + tv) / 2, std::max(1, p.rawDamage / 2),
                                  attOwner, static_cast<int>(p.attacker), -1, changed);
            }
        }
    }
    projectiles_ = std::move(alive);
    if (fogDirty_) refreshSpotting(&changed);
}

void TutorialUnits::stepOpenTopPassengerFire(std::vector<SoldierPose>& changed) {
    (void)changed;
    if (!fog_) return;
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        const auto& v = vehicles_[vi];
        if (!v.alive || v.hp <= 0 || !v.passengersCanFire || v.passengers.empty() || v.holdFire) {
            continue;
        }
        const auto [su, sv] = tutorialFootWorld(v.x, v.y, originX_);
        const bool vehicleWantsCombat = autoEngage_ || v.attackMove || v.attackTarget.has_value();
        for (size_t pIdx : v.passengers) {
            if (pIdx >= soldiers_.size()) continue;
            auto& p = soldiers_[pIdx];
            if (p.hp <= 0 || !p.embarked) continue;
            if (p.attackReload > 0) --p.attackReload;
            if (p.holdFire || p.ammo <= 0 || p.isMedic || p.isOfficer || p.isKamikaze ||
                p.attackReload > 0 || !vehicleWantsCombat) {
                continue;
            }
            const int maxR = std::min(
                260, scaleByExpa(p.desc.rangeBase, p.desc.rangeDelta, p.expa) + v.bonusShotRange);
            const int minR = std::max(
                0, scaleByExpa(p.desc.deadzoneBase, p.desc.deadzoneDelta, p.expa) + v.bonusShotDeadZone);
            double bestDist = static_cast<double>(maxR) + 1.0;
            std::optional<size_t> bestTarget;
            for (size_t j = 0; j < soldiers_.size(); ++j) {
                if (!isValidAttackTarget(j, p.owner)) continue;
                const auto [tu, tv] = soldierWorld(soldiers_[j]);
                const double d = det::detHypot(double(tu - su), double(tv - sv));
                if (d >= static_cast<double>(minR) && d <= static_cast<double>(maxR) && d < bestDist) {
                    bestDist = d;
                    bestTarget = j;
                }
            }
            if (!bestTarget) continue;
            const size_t tid = *bestTarget;
            const auto& t = soldiers_[tid];
            const auto [tu, tv] = soldierWorld(t);
            const int tx = originX_ + tu - tv;
            const int ty = (tu + tv) / 2;
            p.direction = directionFromDelta(tu - su, tv - sv);
            p.lastFireTick = tick_;

            (void)nextCombatRandom(0x7fffu);
            (void)nextCombatRandom(0x7fffu);
            const int rawDamage = scaleByExpa(p.desc.damageBase, p.desc.damageDelta, p.expa);
            const int speed = std::max(1, p.desc.projectileSpeed);
            const int flightTicks = std::max(1, int(std::ceil(bestDist / double(speed))));
            const int baseHitPct = sniperHitPercent(t.desc.armorSniper);
            const int hitPct = t.crouched ? std::max(10, baseHitPct - 25) : baseHitPct;
            projectiles_.push_back(Projectile{
                pIdx, tid, rawDamage, hitPct, std::max(1, flightTicks - 1), p.isFlamer, p.isMortar, p.isHeavyMg
            });
            const int expaGain = scaleByExpa(p.desc.expaBase, p.desc.expaDelta, p.expa);
            p.expa = std::min(0x8000, p.expa + expaGain);
            --p.ammo;
            applySoldierShotReload(p);
            emitVehiclePose(vi, true, tx, ty, true, "fire", false, false);
        }
    }
}

void TutorialUnits::reactToAttack(int victimOwner, int victimGroup, int attackerX, int attackerY,
                                  std::optional<size_t> victimVehicleIdx) {
    if (!autoEngage_ || victimOwner < 0 || victimOwner == 0 || (!multiplayerSymmetry_ && victimOwner == 3) ||
        attackerX < 0 || attackerY < 0) {
        return;
    }
    if (victimVehicleIdx && *victimVehicleIdx < vehicles_.size()) {
        auto& v = vehicles_[*victimVehicleIdx];
        if (v.alive && v.hp > 0 && !v.towedBy && !v.moveTarget && !v.attackTarget) {
            const unsigned desiredHull = vehicleDirFromMapDelta(
                static_cast<double>(attackerX) - v.fx, static_cast<double>(attackerY) - v.fy);
            const unsigned prevHull = v.hullDir;
            v.hullDir = turnTowardDir32(v.hullDir, desiredHull, 6u);
            if (!v.attackBuildingTarget && !v.attackGround) {
                v.turretDir = turnTowardDir32(v.turretDir, desiredHull, 6u);
            }
            if (v.hullDir != prevHull) emitVehiclePose(*victimVehicleIdx);
            if (v.armed && v.speedPx > 0.0 && (v.lastReactTick == 0 || tick_ - v.lastReactTick >= 50u)) {
                moveVehicleInternal(v, attackerX, attackerY);
                v.attackMove = true;
                v.lastReactTick = tick_;
            }
        }
    }
    if (victimGroup > 0) {
        int alerted = 0;
        for (size_t i = 0; i < soldiers_.size() && alerted < 6; ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.owner != victimOwner || s.group != victimGroup) continue;
            if (s.embarked || s.garrisonBuilding || s.motion || s.goal ||
                s.attackTarget || s.autoTarget || s.holdFire) {
                continue;
            }
            if (s.lastReactTick != 0 && tick_ - s.lastReactTick < 50u) continue;
            const auto [su, sv] = soldierWorld(s);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            if (det::detHypot(static_cast<double>(attackerX - sx), static_cast<double>(attackerY - sy)) > 760.0) {
                continue;
            }
            if (orderSoldierInternal(i, attackerX, attackerY)) {
                s.attackMove = true;
                s.lastReactTick = tick_;
                ++alerted;
            }
        }
        int vehAlerted = 0;
        for (size_t i = 0; i < vehicles_.size() && vehAlerted < 2; ++i) {
            if (victimVehicleIdx && i == *victimVehicleIdx) continue;
            auto& v = vehicles_[i];
            if (!v.alive || v.hp <= 0 || v.owner != victimOwner || v.group != victimGroup) continue;
            if (!v.armed || v.speedPx <= 0.0 || v.towedBy || v.moveTarget || v.attackTarget || v.attackGround) continue;
            if (v.lastReactTick != 0 && tick_ - v.lastReactTick < 50u) continue;
            if (det::detHypot(static_cast<double>(attackerX) - v.fx, static_cast<double>(attackerY) - v.fy) > 560.0) {
                continue;
            }
            const unsigned desiredHull = vehicleDirFromMapDelta(
                static_cast<double>(attackerX) - v.fx, static_cast<double>(attackerY) - v.fy);
            v.hullDir = turnTowardDir32(v.hullDir, desiredHull, 6u);
            if (!v.attackBuildingTarget) v.turretDir = turnTowardDir32(v.turretDir, desiredHull, 6u);
            moveVehicleInternal(v, attackerX, attackerY);
            v.attackMove = true;
            v.lastReactTick = tick_;
            emitVehiclePose(i);
            ++vehAlerted;
        }
    }
}
}
