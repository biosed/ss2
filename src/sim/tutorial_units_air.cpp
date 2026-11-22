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
void TutorialUnits::setAirSupport(std::array<std::array<assets::AirSupportSlot, 5>, 4> slots,
                                  std::array<int, 4> paraGroups,
                                  std::array<std::vector<assets::ParatrooperEntry>, 4> paraRosters) {
    airSupport_ = std::move(slots);
    paraGroups_ = std::move(paraGroups);
    paraRosters_ = std::move(paraRosters);
}

void TutorialUnits::addAirSupportPlanes(int owner, assets::AirMissionKind kind, int count) {
    if (owner < 0 || owner >= 4 || count <= 0) return;
    const std::size_t k = static_cast<std::size_t>(kind);
    if (k >= 5u) return;
    auto& slot = airSupport_[static_cast<std::size_t>(owner)][k];
    slot.ready = std::clamp(slot.ready + count, 0, 999);
}

void TutorialUnits::addAirSupportSorties(int owner, assets::AirMissionKind kind, int count) {
    if (owner < 0 || owner >= 4 || count <= 0) return;
    const std::size_t k = static_cast<std::size_t>(kind);
    if (k >= 5u) return;
    auto& slot = airSupport_[static_cast<std::size_t>(owner)][k];
    slot.total = std::clamp(slot.total + count, 0, 999);
}

std::array<assets::AirSupportSlot, 5> TutorialUnits::airSupportFor(int owner) const {
    if (owner < 0 || owner >= 4) return {};
    return airSupport_[static_cast<std::size_t>(owner)];
}

std::array<assets::AirSupportSlot, 5> TutorialUnits::playerAirSupport() const {
    return airSupport_[static_cast<std::size_t>(viewerOwner_)];
}

bool TutorialUnits::callAirSupport(assets::AirMissionKind kind, int targetX, int targetY, int owner) {
    if (owner < 0 || owner >= 4) return false;
    const std::size_t k = static_cast<std::size_t>(kind);
    if (k >= 5u) return false;
    auto& slot = airSupport_[static_cast<std::size_t>(owner)][k];
    if (slot.ready <= 0) return false;
    --slot.ready;
    if (slot.total > 0) --slot.total;

    const int tx = std::clamp(targetX, 0, std::max(0, mapPixelW_ - 1));
    const int ty = std::clamp(targetY, 0, std::max(0, mapPixelH_ - 1));
    const bool fromLeft = tx <= mapPixelW_ / 2;
    const double entryX = fromLeft ? 0.0 : static_cast<double>(std::max(1, mapPixelW_ - 1));
    const double entryY = static_cast<double>(
        std::clamp(ty - 120, std::min(64, std::max(0, mapPixelH_ - 1)), std::max(64, mapPixelH_ - 64)));
    const double dx = static_cast<double>(tx) - entryX;
    const double dy = static_cast<double>(ty) - entryY;
    const double dist = det::detHypot(dx, dy);
    const double ux = dist < 1.0 ? (fromLeft ? 1.0 : -1.0) : (dx / dist);
    const double uy = dist < 1.0 ? 0.0 : (dy / dist);
    const std::string resolvedPlaneId = slot.planeId.empty() ? "ju-87" : slot.planeId;
    const auto* pSpec = catalog_ ? catalog_->findPlane(resolvedPlaneId) : nullptr;
    const double speed = pSpec && pSpec->flySpeed > 0.1
        ? std::clamp(pSpec->flySpeed * 1.45, 5.5, 19.0)
        : ((kind == assets::AirMissionKind::Bomber) ? 9.5
           : (kind == assets::AirMissionKind::Paratrooper || kind == assets::AirMissionKind::Supply) ? 9.0
           : 12.5);
    const bool isRocketAttacker = (pSpec && pSpec->isRocketAttacker());
    const int altitude = (pSpec && pSpec->altitude > 0)
        ? pSpec->altitude
        : ((kind == assets::AirMissionKind::Paratrooper || kind == assets::AirMissionKind::Supply) ? 196 : 280);
    int paraPayloadCount = 6;
    if (kind == assets::AirMissionKind::Paratrooper) {
        int sum = 0;
        for (const auto& pe : paraRosters_[static_cast<std::size_t>(owner)]) {
            if (pe.count > 0) sum += pe.count;
        }
        if (sum > 0) paraPayloadCount = std::clamp(sum, 1, 24);
    }
    const int payloads = (kind == assets::AirMissionKind::Bomber)
        ? (pSpec && pSpec->bombsNumber > 0 ? std::clamp(pSpec->bombsNumber, 3, 12) : 5)
        : (kind == assets::AirMissionKind::Fighter)
            ? (isRocketAttacker ? (pSpec->groundAmmo > 0 ? std::clamp(pSpec->groundAmmo, 3, 6) : 6) : 3)
        : (kind == assets::AirMissionKind::Paratrooper) ? paraPayloadCount
        : (kind == assets::AirMissionKind::Supply) ? 3
        : 0;
    const int maxHp = pSpec && pSpec->health > 0
        ? std::clamp(pSpec->health, 120, 2500)
        : ((kind == assets::AirMissionKind::Bomber || kind == assets::AirMissionKind::Paratrooper ||
            kind == assets::AirMissionKind::Supply)
               ? 220
               : 150);
    const bool isLeafletBomber = (pSpec && pSpec->isLeafletBomber());
    const int bombDamage = isLeafletBomber ? 0
        : ((pSpec && pSpec->bombDamage > 0) ? std::clamp(pSpec->bombDamage / 6, 280, 650) : 380);
    const int strafeDamage = (pSpec && pSpec->groundDamage > 0) ? std::clamp(pSpec->groundDamage * 4, 120, 260) : 180;
    const int airToAirDamage = (pSpec && pSpec->airDamage > 0) ? std::clamp(pSpec->airDamage * 2, 55, 140) : 75;
    const int sightTiles = (pSpec && pSpec->sight > 0) ? std::clamp(pSpec->sight, 12, 28) : 18;
    const int scoreValue = pSpec ? pSpec->effectiveScoreValue() : 500;

    ActiveSortie st{};
    st.id = nextSortieId_++;
    st.kind = kind;
    st.planeId = resolvedPlaneId;
    st.owner = owner;
    st.fx = entryX;
    st.fy = entryY;
    st.vx = ux * speed;
    st.vy = uy * speed;
    st.targetX = tx;
    st.targetY = ty;
    st.dir32 = vehicleDirFromMapDelta(st.vx, st.vy);
    st.hp = maxHp;
    st.maxHp = maxHp;
    st.payloadsLeft = payloads;
    st.totalPayloads = payloads;
    st.bombDamage = bombDamage;
    st.strafeDamage = strafeDamage;
    st.airToAirDamage = airToAirDamage;
    st.sightTiles = sightTiles;
    st.scoreValue = scoreValue;
    st.isLeafletBomber = isLeafletBomber;
    st.altitude = altitude;
    st.isRocketAttacker = isRocketAttacker;
    st.strafeHitType = pSpec ? pSpec->effectiveGroundHitType() : assets::HitType::Machine;
    sorties_.push_back(st);

    sortiePoses_.push_back(AirSortiePose{
        st.id, st.kind, st.planeId, st.owner,
        static_cast<int>(det::detRoundToInt(st.fx)), static_cast<int>(det::detRoundToInt(st.fy)),
        st.dir32, st.hp, st.maxHp, true, true, false, 0, 0
    });
    return true;
}

std::vector<AirSortiePose> TutorialUnits::takeAirSorties() {
    return std::exchange(sortiePoses_, {});
}

std::vector<ParachutePose> TutorialUnits::takeParachutes() {
    return std::exchange(parachutePoses_, {});
}

std::vector<AirStrikeEvent> TutorialUnits::takeAirStrikeEvents() {
    return std::exchange(airEvents_, {});
}

void TutorialUnits::stepAirSupport(std::vector<SoldierPose>& changed) {
    for (auto& v : vehicles_) {
        if (v.aaCooldown > 0u) --v.aaCooldown;
    }
    if (sorties_.empty()) {
        if (!parachutes_.empty()) stepParachutes(changed);
        return;
    }

    const auto pushSortiePose = [&](const AirSortiePose& pose) {
        const auto it = std::ranges::find(sortiePoses_, pose.id, &AirSortiePose::id);
        if (it == sortiePoses_.end()) {
            sortiePoses_.push_back(pose);
            return;
        }
        const bool hadAa = it->aaFired;
        const int aaX = it->aaOriginX;
        const int aaY = it->aaOriginY;
        std::string aaSnd = std::move(it->aaSoundGroup);
        *it = pose;
        if (hadAa && !pose.aaFired) {
            it->aaFired = true;
            it->aaOriginX = aaX;
            it->aaOriginY = aaY;
            it->aaSoundGroup = std::move(aaSnd);
        }
    };

    const auto applyAirSplash = [&](int impactX, int impactY, double radius, int dmgBase,
                                    assets::HitType hitType, int owner) {
        for (size_t k = 0; k < vehicles_.size(); ++k) {
            auto& tv = vehicles_[k];
            if (!tv.alive || tv.hp <= 0 || (!isHostile(owner, tv.owner) && !tv.isCrate)) continue;
            const double d = det::detHypot(tv.fx - static_cast<double>(impactX), tv.fy - static_cast<double>(impactY));
            if (d > radius) continue;
            const int scaledDmg = std::max(20, static_cast<int>(det::detRoundToInt(
                static_cast<double>(dmgBase) * (1.0 - 0.55 * (d / (radius + 1.0))))));
            const int dealt = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             impactX, impactY, scaledDmg, hitType));
            tv.hp = std::max(0, tv.hp - dealt);
            if (dealt > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
                tv.expa = std::min(0x8000, tv.expa + std::min(200, dealt * tv.expaOnHit));
            }
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            if (tv.hp == 0) destroyVehicle(k, changed);
            else {
                applyVehicleCrewDamage(k, impactX, impactY, scaledDmg, hitType, changed, -1, -1);
                if (dealt > 0 || hitType == assets::HitType::Explosive) {
                    applyVehicleSuppression(k, 24, impactX, impactY);
                }
            }
            emitVehiclePose(k);
        }
        for (size_t k = 0; k < soldiers_.size(); ++k) {
            auto& ss = soldiers_[k];
            if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(owner, ss.owner)) continue;
            const auto [su, sv] = soldierWorld(ss);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            const double sd = det::detHypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
            if (sd > radius) continue;
            const int splashDmg = std::max(10, static_cast<int>(det::detRoundToInt(
                static_cast<double>(dmgBase) * 0.65 * (1.0 - sd / (radius + 1.0)))));
            ss.hp = std::max(0, ss.hp - splashDmg);
            if (ss.hp > 0) {
                applySuppression(k, hitType == assets::HitType::Explosive ? 30 : 18);
            }
            recordHit(ss.owner, ss.group);
            if (ss.hp == 0) {
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
            SoldierPose sp{k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis};
            sp.suppressed = (ss.hp > 0 && ss.isSuppressed());
            pushOrUpdateSoldierPose(changed, sp);
        }
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            const auto& tb = buildings_[bi];
            if (tb.destroyed) continue;
            const double bd = std::max(0.0, det::detHypot(double(tb.x - impactX), double(tb.y - impactY)) -
                                            std::min(tb.width, tb.height) * 0.3);
            if (bd <= radius + 16.0) {
                const int bldDmg = hitType == assets::HitType::Explosive
                    ? std::max(25, dmgBase / 2)
                    : std::max(10, dmgBase / 4);
                damageBuilding(bi, bldDmg, hitType, owner, changed);
            }
        }
        if (hitType == assets::HitType::Explosive) {
            damageBridgesAtImpact(impactX, impactY, radius + 24.0, std::max(40, dmgBase), hitType, owner, changed);
        }
        if (hitType == assets::HitType::Explosive && !mines_.empty()) {
            const SubCell impCell = pointerCell(impactX, impactY, originX_);
            const int itx = impCell.x / 2;
            const int ity = impCell.y / 2;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    triggerMineAtTile(itx + dx, ity + dy, changed, true);
                }
            }
        }
    };

    for (auto& st : sorties_) {
        double stepScale = 1.0;
        if ((st.kind == assets::AirMissionKind::Paratrooper || st.kind == assets::AirMissionKind::Supply) &&
            st.payloadsLeft > 0) {
            const double spd = std::max(1e-4, det::detHypot(st.vx, st.vy));
            const double alongPre = (st.fx - static_cast<double>(st.targetX)) * (st.vx / spd) +
                                    (st.fy - static_cast<double>(st.targetY)) * (st.vy / spd);
            if (alongPre >= -112.0 && alongPre <= 96.0) {
                const auto* pSpec = catalog_ ? catalog_->findPlane(st.planeId) : nullptr;
                const double dropSpd = (pSpec && pSpec->dropFlySpeed > 0.1)
                    ? std::clamp(pSpec->dropFlySpeed * 1.45, 3.2, spd)
                    : std::min(spd, 5.5);
                stepScale = std::clamp(dropSpd / spd, 0.45, 1.0);
            }
        }
        st.fx += st.vx * stepScale;
        st.fy += st.vy * stepScale;
    }

    // Air-to-air interception: active Fighter sorties engage hostile aircraft within 420px.
    struct AirIntercept { int x{}; int y{}; std::string sound; };
    std::vector<std::optional<AirIntercept>> interceptedFrom(sorties_.size());
    for (size_t i = 0; i < sorties_.size(); ++i) {
        auto& fighter = sorties_[i];
        if (fighter.kind != assets::AirMissionKind::Fighter || fighter.hp <= 0) continue;
        if ((fighter.reconTick++ % 8u) != 0u) continue;
        std::optional<size_t> bestVictim;
        double bestDist = 420.0 + 1.0;
        for (size_t j = 0; j < sorties_.size(); ++j) {
            if (j == i) continue;
            const auto& other = sorties_[j];
            if (other.hp <= 0 || !isHostile(fighter.owner, other.owner)) continue;
            const double d = det::detHypot(other.fx - fighter.fx, other.fy - fighter.fy);
            if (d <= 420.0 && d < bestDist) {
                bestDist = d;
                bestVictim = j;
            }
        }
        if (bestVictim) {
            auto& victim = sorties_[*bestVictim];
            victim.hp = std::max(0, victim.hp - fighter.airToAirDamage);
            const auto* fSpec = catalog_ ? catalog_->findPlane(fighter.planeId) : nullptr;
            std::string fSound = (fSpec && !fSpec->airShotSound.empty()) ? fSpec->airShotSound : std::string("mg_aa");
            interceptedFrom[*bestVictim] = AirIntercept{
                static_cast<int>(det::detRoundToInt(fighter.fx)),
                static_cast<int>(det::detRoundToInt(fighter.fy)),
                std::move(fSound)
            };
        }
    }

    std::vector<ActiveSortie> aliveSorties;
    aliveSorties.reserve(sorties_.size());
    for (size_t si = 0; si < sorties_.size(); ++si) {
        auto& st = sorties_[si];
        const int ix = static_cast<int>(det::detRoundToInt(st.fx));
        const int iy = static_cast<int>(det::detRoundToInt(st.fy));

        if (st.kind == assets::AirMissionKind::Recon && fog_ && (st.reconTick++ % 3u) == 0u) {
            fogDirty_ = true;
        }

        bool aaFired = interceptedFrom[si].has_value();
        int aaOriginX = aaFired ? interceptedFrom[si]->x : 0;
        int aaOriginY = aaFired ? interceptedFrom[si]->y : 0;
        std::string aaSound = aaFired ? interceptedFrom[si]->sound : std::string{};
        const auto isAaSoundName = [](std::string_view snd) {
            return snd.find("flak") != std::string_view::npos || snd.find("aa") != std::string_view::npos;
        };
        for (size_t vi = 0; vi < vehicles_.size() && st.hp > 0; ++vi) {
            auto& av = vehicles_[vi];
            if (!av.alive || av.hp <= 0 || !av.isAntiAir || (av.ammo <= 0 && av.ammo2 <= 0) || av.towedBy) continue;
            if (!isHostile(av.owner, st.owner) || av.aaCooldown > 0u) continue;
            const double maxAaRange = av.isHeavyAntiAir ? 600.0
                                                        : (st.altitude >= 350 ? 440.0 : 520.0);
            const double ad = det::detHypot(st.fx - av.fx, st.fy - av.fy);
            if (ad > maxAaRange) continue;
            av.aaCooldown = 14u;
            if (av.ammo > 0) --av.ammo;
            else --av.ammo2;
            av.turretDir = vehicleDirFromMapDelta(st.fx - av.fx, st.fy - av.fy);
            emitVehiclePose(vi);
            if (!aaFired) {
                aaFired = true;
                aaOriginX = av.x;
                aaOriginY = av.y;
                if (isAaSoundName(av.sound2)) aaSound = av.sound2;
                else if (!av.sound1.empty()) aaSound = av.sound1;
                else if (!av.sound2.empty()) aaSound = av.sound2;
                else aaSound = "flak";
            }
            const int aaDmg = av.isHeavyAntiAir ? 55
                                                : (st.altitude >= 350 ? 20
                                                                      : (st.altitude <= 250 ? 40 : 35));
            st.hp = std::max(0, st.hp - aaDmg);
            if (st.hp == 0) break;
        }
        if (st.hp <= 0) {
            applyAirSplash(ix, iy, 60.0, 175, assets::HitType::Explosive, -1);
            airEvents_.push_back(AirStrikeEvent{
                AirStrikeEvent::PlaneShotDown, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId
            });
            pushSortiePose(AirSortiePose{
                st.id, st.kind, st.planeId, st.owner, ix, iy, st.dir32, 0, st.maxHp, false, false,
                aaFired, aaOriginX, aaOriginY, aaSound
            });
            continue;
        }

        const double spd = std::max(1e-4, det::detHypot(st.vx, st.vy));
        const double ux = st.vx / spd;
        const double uy = st.vy / spd;
        const double along = (st.fx - static_cast<double>(st.targetX)) * ux +
                             (st.fy - static_cast<double>(st.targetY)) * uy;

        if (st.kind == assets::AirMissionKind::Bomber) {
            const int totalDrops = std::max(1, st.totalPayloads);
            while (st.payloadsLeft > 0) {
                const int dropIdx = totalDrops - st.payloadsLeft;
                const double step = totalDrops > 1 ? (192.0 / static_cast<double>(totalDrops - 1)) : 0.0;
                const double threshold = -96.0 + step * static_cast<double>(dropIdx);
                if (along < threshold) break;
                --st.payloadsLeft;
                if (st.isLeafletBomber) {
                    for (size_t k = 0; k < soldiers_.size(); ++k) {
                        auto& ss = soldiers_[k];
                        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(st.owner, ss.owner)) continue;
                        const auto [su, sv] = soldierWorld(ss);
                        const int sx = originX_ + su - sv;
                        const int sy = (su + sv) / 2;
                        if (det::detHypot(static_cast<double>(sx - ix), static_cast<double>(sy - iy)) <= 180.0) {
                            applySuppression(k, 35);
                        }
                    }
                    for (size_t k = 0; k < vehicles_.size(); ++k) {
                        auto& tv = vehicles_[k];
                        if (!tv.alive || tv.hp <= 0 || !tv.hasCrew || !isHostile(st.owner, tv.owner)) continue;
                        if (det::detHypot(tv.fx - static_cast<double>(ix), tv.fy - static_cast<double>(iy)) <= 180.0) {
                            applyVehicleSuppression(k, 32, ix, iy);
                        }
                    }
                    airEvents_.push_back(AirStrikeEvent{
                        AirStrikeEvent::BombImpact, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId, true
                    });
                } else {
                    applyAirSplash(ix, iy, 100.0, st.bombDamage, assets::HitType::Explosive, st.owner);
                    airEvents_.push_back(AirStrikeEvent{
                        AirStrikeEvent::BombImpact, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId, false
                    });
                }
            }
        } else if (st.kind == assets::AirMissionKind::Fighter) {
            const int totalStrafes = std::max(1, st.totalPayloads);
            const double spanHalf = st.isRocketAttacker ? 96.0 : 64.0;
            while (st.payloadsLeft > 0) {
                const int dropIdx = totalStrafes - st.payloadsLeft;
                const double step = totalStrafes > 1
                    ? ((spanHalf * 2.0) / static_cast<double>(totalStrafes - 1)) : 0.0;
                const double threshold = -spanHalf + step * static_cast<double>(dropIdx);
                if (along < threshold) break;
                --st.payloadsLeft;
                applyAirSplash(ix, iy, st.isRocketAttacker ? 96.0 : 80.0,
                               st.strafeDamage, st.strafeHitType, st.owner);
                airEvents_.push_back(AirStrikeEvent{
                    AirStrikeEvent::StrafeImpact, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId
                });
            }
        } else if (st.kind == assets::AirMissionKind::Paratrooper) {
            const auto& rVec = paraRosters_[static_cast<std::size_t>(st.owner)];
            std::vector<assets::ParatrooperEntry> dropRoster = rVec;
            if (dropRoster.empty()) dropRoster.push_back({"dsmguner", 6});
            std::vector<std::string> stickIds;
            stickIds.reserve(static_cast<std::size_t>(std::max(1, st.totalPayloads)));
            for (const auto& pe : dropRoster) {
                for (int c = 0; c < pe.count && static_cast<int>(stickIds.size()) < st.totalPayloads; ++c) {
                    stickIds.push_back(pe.unitId);
                }
            }
            while (static_cast<int>(stickIds.size()) < std::max(1, st.totalPayloads)) {
                stickIds.push_back("dsmguner");
            }
            const int totalDrops = std::max(1, st.totalPayloads);
            const double spanHalf = totalDrops > 1
                ? std::min(84.0, 14.0 * static_cast<double>(totalDrops - 1))
                : 0.0;
            const double fallSpd = catalog_ ? catalog_->paraSpec().fallSpeed : 1.2;
            const int fallSpeed256 = std::max(64, static_cast<int>(det::detRoundToInt(fallSpd * 256.0)));
            const int maxX = std::max(16, mapPixelW_ - 16);
            const int maxY = std::max(16, mapPixelH_ - 16);
            while (st.payloadsLeft > 0) {
                const int dropIdx = totalDrops - st.payloadsLeft;
                const double step = totalDrops > 1 ? ((spanHalf * 2.0) / static_cast<double>(totalDrops - 1)) : 0.0;
                const double threshold = -spanHalf + step * static_cast<double>(dropIdx);
                if (along < threshold) break;
                if (st.payloadsLeft == totalDrops) {
                    airEvents_.push_back(AirStrikeEvent{
                        AirStrikeEvent::ParatrooperDrop, st.targetX, st.targetY, st.owner,
                        paraGroups_[static_cast<std::size_t>(st.owner)], dropRoster,
                        st.scoreValue, st.planeId
                    });
                }
                --st.payloadsLeft;
                const double sideSign = (dropIdx % 2 == 0) ? -10.0 : 10.0;
                const int px = std::clamp(
                    static_cast<int>(det::detRoundToInt(static_cast<double>(st.targetX) + ux * threshold - uy * sideSign)),
                    16, maxX);
                const int py = std::clamp(
                    static_cast<int>(det::detRoundToInt(static_cast<double>(st.targetY) + uy * threshold + ux * sideSign)),
                    16, maxY);
                ActiveParachute para{};
                para.id = nextSortieId_++;
                para.unitId = stickIds[static_cast<std::size_t>(dropIdx)];
                para.owner = st.owner;
                para.group = paraGroups_[static_cast<std::size_t>(st.owner)];
                para.x = px;
                para.y = py;
                para.altitude256 = std::max(64, st.altitude) * 256;
                para.fallSpeed256 = fallSpeed256;
                para.hp = 40;
                para.maxHp = 40;
                para.isSupplyCrate = false;
                parachutes_.push_back(std::move(para));
            }
        } else if (st.kind == assets::AirMissionKind::Supply) {
            const int totalDrops = std::max(1, st.totalPayloads);
            const double spanHalf = totalDrops > 1 ? 24.0 : 0.0;
            const double fallSpd = catalog_ ? catalog_->paraSpec().fallSpeed : 1.2;
            const int fallSpeed256 = std::max(64, static_cast<int>(det::detRoundToInt(fallSpd * 256.0)));
            const int maxX = std::max(16, mapPixelW_ - 16);
            const int maxY = std::max(16, mapPixelH_ - 16);
            while (st.payloadsLeft > 0) {
                const int dropIdx = totalDrops - st.payloadsLeft;
                const double step = totalDrops > 1 ? ((spanHalf * 2.0) / static_cast<double>(totalDrops - 1)) : 0.0;
                const double threshold = -spanHalf + step * static_cast<double>(dropIdx);
                if (along < threshold) break;
                if (st.payloadsLeft == totalDrops) {
                    airEvents_.push_back(AirStrikeEvent{
                        AirStrikeEvent::SupplyDrop, st.targetX, st.targetY, st.owner, 0, {}, st.scoreValue, st.planeId
                    });
                }
                --st.payloadsLeft;
                const double sideSign = (dropIdx == 1) ? 0.0 : ((dropIdx % 2 == 0) ? -8.0 : 8.0);
                const int px = std::clamp(
                    static_cast<int>(det::detRoundToInt(static_cast<double>(st.targetX) + ux * threshold - uy * sideSign)),
                    16, maxX);
                const int py = std::clamp(
                    static_cast<int>(det::detRoundToInt(static_cast<double>(st.targetY) + uy * threshold + ux * sideSign)),
                    16, maxY);
                ActiveParachute para{};
                para.id = nextSortieId_++;
                para.unitId = "ammo.box";
                para.owner = st.owner;
                para.group = 0;
                para.x = px;
                para.y = py;
                para.altitude256 = std::max(64, st.altitude) * 256;
                para.fallSpeed256 = fallSpeed256;
                para.hp = 50;
                para.maxHp = 50;
                para.isSupplyCrate = true;
                parachutes_.push_back(std::move(para));
            }
        }

        const bool outOfBounds = st.fx < -160.0 || st.fx > static_cast<double>(mapPixelW_ + 160) ||
                                 st.fy < -160.0 || st.fy > static_cast<double>(mapPixelH_ + 160);
        pushSortiePose(AirSortiePose{
            st.id, st.kind, st.planeId, st.owner, ix, iy, st.dir32, st.hp, st.maxHp, true, !outOfBounds,
            aaFired, aaOriginX, aaOriginY, aaSound
        });
        if (!outOfBounds) aliveSorties.push_back(std::move(st));
    }
    sorties_ = std::move(aliveSorties);
    if (!parachutes_.empty()) stepParachutes(changed);
}

void TutorialUnits::stepParachutes(std::vector<SoldierPose>& changed) {
    if (parachutes_.empty()) return;

    const auto pushParaPose = [&](const ParachutePose& pose) {
        const auto it = std::ranges::find(parachutePoses_, pose.id, &ParachutePose::id);
        if (it == parachutePoses_.end()) {
            parachutePoses_.push_back(pose);
            return;
        }
        *it = pose;
    };

    const int frameDelay = std::clamp(catalog_ ? catalog_->paraSpec().paraFrameDelay : 3, 1, 8);
    const int collapseDuration = 4 * frameDelay;

    std::vector<ActiveParachute> remaining;
    remaining.reserve(parachutes_.size());
    for (auto& p : parachutes_) {
        // Descending canopies (altitude > 0) are vulnerable to nearby hostile AA and ground small-arms fire.
        if (p.altitude256 > 0 && p.hp > 0) {
            for (size_t vi = 0; vi < vehicles_.size() && p.hp > 0; ++vi) {
                auto& av = vehicles_[vi];
                if (!av.alive || av.hp <= 0 || !av.isAntiAir || (av.ammo <= 0 && av.ammo2 <= 0) || av.towedBy) {
                    continue;
                }
                if (!isHostile(av.owner, p.owner) || av.aaCooldown > 0u) continue;
                const double d = det::detHypot(static_cast<double>(p.x) - av.fx, static_cast<double>(p.y) - av.fy);
                if (d > 360.0) continue;
                av.aaCooldown = 14u;
                if (av.ammo > 0) --av.ammo;
                else --av.ammo2;
                av.turretDir = vehicleDirFromMapDelta(static_cast<double>(p.x) - av.fx,
                                                      static_cast<double>(p.y) - av.fy);
                emitVehiclePose(vi, true, p.x, p.y - (p.altitude256 >> 8));
                p.hp = std::max(0, p.hp - (av.isHeavyAntiAir ? 35 : 22));
            }
            if (p.hp > 0 && (tick_ & 7u) == 0u) {
                for (size_t si = 0; si < soldiers_.size() && p.hp > 0; ++si) {
                    auto& s = soldiers_[si];
                    if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.holdFire || s.ammo <= 0) continue;
                    if (!isHostile(s.owner, p.owner) || s.attackReload > 0) continue;
                    if (s.isMedic || s.isMortar || s.isFlamer || s.isAntiVehicle) continue;
                    const auto [su, sv] = soldierWorld(s);
                    const int sx = originX_ + su - sv;
                    const int sy = (su + sv) / 2;
                    if (det::detHypot(static_cast<double>(p.x - sx), static_cast<double>(p.y - sy)) > 192.0) continue;
                    --s.ammo;
                    applySoldierShotReload(s);
                    p.hp = std::max(0, p.hp - 14);
                }
            }
            if (p.hp <= 0) {
                pushParaPose(ParachutePose{
                    p.id, p.unitId, p.owner, p.group, p.x, p.y, p.altitude256 >> 8, 0, false, false, p.isSupplyCrate
                });
                continue;
            }

            // CPara::Step (n2Game_Dll.dll 0x1002e2cc..0x1002e32c): 8.8 fixed-point descent + 3*fallSpeed BOXDUST.
            const int prevAlt = p.altitude256;
            p.altitude256 = std::max(0, p.altitude256 - p.fallSpeed256);
            if (p.isSupplyCrate && !p.emittedBoxDust &&
                prevAlt > 3 * p.fallSpeed256 && p.altitude256 <= 3 * p.fallSpeed256) {
                p.emittedBoxDust = true;
                airEvents_.push_back(AirStrikeEvent{
                    AirStrikeEvent::BoxDust, p.x, p.y, p.owner, p.group, {}, 0, p.unitId
                });
            }
            pushParaPose(ParachutePose{
                p.id, p.unitId, p.owner, p.group, p.x, p.y, p.altitude256 >> 8, 0, true, true, p.isSupplyCrate
            });
            remaining.push_back(std::move(p));
            continue;
        }

        // Canopy collapse on ground (0x1002e245..0x1002e346: 4 collapse frames * paraframedelay).
        ++p.collapseTicks;
        if (p.collapseTicks < collapseDuration) {
            pushParaPose(ParachutePose{
                p.id, p.unitId, p.owner, p.group, p.x, p.y, 0, p.collapseTicks, true, true, p.isSupplyCrate
            });
            remaining.push_back(std::move(p));
            continue;
        }

        // Touchdown materialization (0x1002e348..0x1002e3b3).
        const SubCell rawCell = placementCell(p.x, p.y, originX_);
        const auto* uSpec = catalog_ ? catalog_->find(p.unitId) : nullptr;
        const bool isNaval = uSpec && uSpec->isNavalCraft();
        const bool isUnbridgedWater =
            grid_.inBounds(rawCell) && !grid_.walkable(rawCell) && grid_.walkableMask(rawCell, 0x000Fu);
        if (isUnbridgedWater && !isNaval) {
            // Landed in deep water without a passable bridge -> unit/crate drowns on splashdown.
            pushParaPose(ParachutePose{
                p.id, p.unitId, p.owner, p.group, p.x, p.y, 0, p.collapseTicks, false, false, p.isSupplyCrate
            });
            continue;
        }

        const unsigned rand3 = nextCombatRandom(7u);
        if (p.isSupplyCrate) {
            const SubCell landCell = findSpawnCell(grid_, rawCell);
            const auto [lu, lv] = InfantryPathGrid::toWorldCenter(landCell);
            const int lx = originX_ + lu - lv;
            const int ly = (lu + lv) / 2;
            assets::VehicleUnit vu{};
            vu.id = "ammo.box";
            vu.x = lx;
            vu.y = ly;
            vu.hp = 100;
            vu.ammo = 100;
            vu.group = p.group;
            // Retail 0x1002e396 sets landed ammo.box owner to neutral (owner == 3) so any unit can resupply.
            vu.owner = 3;
            vu.hullDir = ((rand3 * 4u) + 2u) & 31u;
            vu.turretDir = vu.hullDir;
            vu.alive = true;
            vu.visible = true;
            vu.hasCrew = false;
            const std::size_t vIdx = appendVehicle(vu);
            // Supply crates also carry spare parts (ammo2) for field vehicle repairs alongside 1000 ammo.
            vehicles_[vIdx].ammo2 = std::max(vehicles_[vIdx].ammo2, 500);
            vehicles_[vIdx].maxAmmo2 = std::max(vehicles_[vIdx].maxAmmo2, 500);
            vehicles_[vIdx].hasFiniteSupply = true;
            fogDirty_ = true;
            emitVehiclePose(vIdx);
            airEvents_.push_back(AirStrikeEvent{
                AirStrikeEvent::SupplyCrateLanded, lx, ly, vu.owner, p.group, {}, 10,
                "ammo.box", false, vIdx, true
            });
        } else if (uSpec && uSpec->isVehicle) {
            const SubCell landCell = findSpawnCell(grid_, rawCell);
            const auto [lu, lv] = InfantryPathGrid::toWorldCenter(landCell);
            const int lx = originX_ + lu - lv;
            const int ly = (lu + lv) / 2;
            assets::VehicleUnit vu{};
            vu.id = p.unitId;
            vu.x = lx;
            vu.y = ly;
            vu.hp = 100;
            vu.ammo = 100;
            vu.group = p.group;
            vu.owner = p.owner;
            vu.hullDir = ((rand3 * 4u) + 2u) & 31u;
            vu.turretDir = vu.hullDir;
            vu.alive = true;
            vu.visible = true;
            vu.hasCrew = (p.owner != 3) || multiplayerSymmetry_;
            const std::size_t vIdx = appendVehicle(vu);
            fogDirty_ = true;
            emitVehiclePose(vIdx);
            airEvents_.push_back(AirStrikeEvent{
                AirStrikeEvent::ParatrooperLanded, lx, ly, p.owner, p.group, {}, 0,
                p.unitId, false, vIdx, true
            });
        } else {
            const SubCell landCell = findSpawnCell(grid_, rawCell);
            const auto [lu, lv] = InfantryPathGrid::toWorldCenter(landCell);
            const int lx = originX_ + lu - lv;
            const int ly = (lu + lv) / 2;
            assets::Infantry inf{};
            inf.id = p.unitId;
            inf.x = lx;
            inf.y = ly;
            inf.hp = 100;
            inf.ammo = 100;
            inf.group = p.group;
            inf.owner = p.owner;
            inf.direction = rand3 & 7u;
            inf.visible = true;
            const std::size_t sIdx = appendSoldier(inf, landCell, 0);
            const auto& s = soldiers_[sIdx];
            fogDirty_ = true;
            const bool vis = isVisibleToViewer(s.owner, s.spottedMask);
            SoldierPose sp{
                sIdx, lx, ly, s.direction, 0u, false, s.hp, true, s.owner, s.group, false, vis,
                0, 0, false, s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s),
                s.mineAmmo, s.ammo
            };
            pushOrUpdateSoldierPose(changed, sp);
            airEvents_.push_back(AirStrikeEvent{
                AirStrikeEvent::ParatrooperLanded, lx, ly, p.owner, p.group, {}, 0,
                p.unitId, false, sIdx, false
            });
        }
        pushParaPose(ParachutePose{
            p.id, p.unitId, p.owner, p.group, p.x, p.y, 0, p.collapseTicks, true, false, p.isSupplyCrate
        });
    }
    parachutes_ = std::move(remaining);
}

unsigned TutorialUnits::countAirPlanes(int owner, unsigned slot) const {
    if (owner < 0 || owner >= 4 || slot >= 5u) return 0u;
    unsigned total = static_cast<unsigned>(std::max(0, airSupport_[static_cast<std::size_t>(owner)][slot].ready));
    for (const auto& st : sorties_) {
        if (st.hp > 0 && st.owner == owner && static_cast<unsigned>(st.kind) == slot) {
            ++total;
        }
    }
    return total;
}
}
