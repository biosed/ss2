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
    return airSupport_[0];
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
    const double dist = std::hypot(dx, dy);
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
    const int altitude = (pSpec && pSpec->altitude > 0) ? pSpec->altitude : 280;
    const int payloads = (kind == assets::AirMissionKind::Bomber)
        ? (pSpec && pSpec->bombsNumber > 0 ? std::clamp(pSpec->bombsNumber, 3, 12) : 5)
        : (kind == assets::AirMissionKind::Fighter)
            ? (isRocketAttacker ? (pSpec->groundAmmo > 0 ? std::clamp(pSpec->groundAmmo, 3, 6) : 6) : 3)
        : (kind == assets::AirMissionKind::Paratrooper || kind == assets::AirMissionKind::Supply) ? 1
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
        static_cast<int>(std::lround(st.fx)), static_cast<int>(std::lround(st.fy)),
        st.dir32, st.hp, st.maxHp, true, true, false, 0, 0
    });
    return true;
}

std::vector<AirSortiePose> TutorialUnits::takeAirSorties() {
    return std::exchange(sortiePoses_, {});
}

std::vector<AirStrikeEvent> TutorialUnits::takeAirStrikeEvents() {
    return std::exchange(airEvents_, {});
}

void TutorialUnits::stepAirSupport(std::vector<SoldierPose>& changed) {
    for (auto& v : vehicles_) {
        if (v.aaCooldown > 0u) --v.aaCooldown;
    }
    if (sorties_.empty()) return;

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
            if (!tv.alive || tv.hp <= 0 || !isHostile(owner, tv.owner)) continue;
            const double d = std::hypot(tv.fx - static_cast<double>(impactX), tv.fy - static_cast<double>(impactY));
            if (d > radius) continue;
            const int scaledDmg = std::max(20, static_cast<int>(std::lround(
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
            const double sd = std::hypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
            if (sd > radius) continue;
            const int splashDmg = std::max(10, static_cast<int>(std::lround(
                static_cast<double>(dmgBase) * 0.65 * (1.0 - sd / (radius + 1.0)))));
            ss.hp = std::max(0, ss.hp - splashDmg);
            if (ss.hp > 0) {
                applySuppression(k, hitType == assets::HitType::Explosive ? 30 : 18);
            }
            recordHit(ss.owner, ss.group);
            if (ss.hp == 0) {
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
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            const auto& tb = buildings_[bi];
            if (tb.destroyed) continue;
            const double bd = std::max(0.0, std::hypot(double(tb.x - impactX), double(tb.y - impactY)) -
                                            std::min(tb.width, tb.height) * 0.3);
            if (bd <= radius + 16.0) {
                const int bldDmg = hitType == assets::HitType::Explosive
                    ? std::max(25, dmgBase / 2)
                    : std::max(10, dmgBase / 4);
                damageBuilding(bi, bldDmg, hitType, owner, changed);
            }
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
        st.fx += st.vx;
        st.fy += st.vy;
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
            const double d = std::hypot(other.fx - fighter.fx, other.fy - fighter.fy);
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
                static_cast<int>(std::lround(fighter.fx)),
                static_cast<int>(std::lround(fighter.fy)),
                std::move(fSound)
            };
        }
    }

    std::vector<ActiveSortie> aliveSorties;
    aliveSorties.reserve(sorties_.size());
    for (size_t si = 0; si < sorties_.size(); ++si) {
        auto& st = sorties_[si];
        const int ix = static_cast<int>(std::lround(st.fx));
        const int iy = static_cast<int>(std::lround(st.fy));

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
            const double ad = std::hypot(st.fx - av.fx, st.fy - av.fy);
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

        const double spd = std::max(1e-4, std::hypot(st.vx, st.vy));
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
                        if (std::hypot(static_cast<double>(sx - ix), static_cast<double>(sy - iy)) <= 180.0) {
                            applySuppression(k, 35);
                        }
                    }
                    for (size_t k = 0; k < vehicles_.size(); ++k) {
                        auto& tv = vehicles_[k];
                        if (!tv.alive || tv.hp <= 0 || !tv.hasCrew || !isHostile(st.owner, tv.owner)) continue;
                        if (std::hypot(tv.fx - static_cast<double>(ix), tv.fy - static_cast<double>(iy)) <= 180.0) {
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
            if (st.payloadsLeft > 0 && along >= 0.0) {
                st.payloadsLeft = 0;
                const auto& rVec = paraRosters_[static_cast<std::size_t>(st.owner)];
                std::vector<assets::ParatrooperEntry> dropRoster = rVec;
                if (dropRoster.empty()) dropRoster.push_back({"dsmguner", 6});
                airEvents_.push_back(AirStrikeEvent{
                    AirStrikeEvent::ParatrooperDrop, st.targetX, st.targetY, st.owner,
                    paraGroups_[static_cast<std::size_t>(st.owner)], std::move(dropRoster),
                    st.scoreValue, st.planeId
                });
            }
        } else if (st.kind == assets::AirMissionKind::Supply) {
            if (st.payloadsLeft > 0 && along >= 0.0) {
                st.payloadsLeft = 0;
                for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
                    auto& tv = vehicles_[vi];
                    if (!tv.alive || tv.hp <= 0 || isHostile(st.owner, tv.owner) || tv.owner >= 3) continue;
                    if (std::hypot(tv.fx - static_cast<double>(st.targetX),
                                   tv.fy - static_cast<double>(st.targetY)) > 220.0) {
                        continue;
                    }
                    bool didRep = false;
                    bool didRes = false;
                    if (tv.hp < tv.maxHp) {
                        tv.hp = std::min(tv.maxHp, tv.hp + 80);
                        didRep = true;
                    }
                    if (tv.hasCrew && tv.crewHp < tv.maxCrewHp) {
                        tv.crewHp = tv.maxCrewHp;
                        didRep = true;
                    }
                    if (tv.maxAmmo > 0 && tv.ammo < tv.maxAmmo) {
                        tv.ammo = std::min(tv.maxAmmo, tv.ammo + std::max(1, tv.maxAmmo / 2));
                        didRes = true;
                    }
                    if (tv.maxAmmo2 > 0 && tv.ammo2 < tv.maxAmmo2) {
                        tv.ammo2 = std::min(tv.maxAmmo2, tv.ammo2 + std::max(10, tv.maxAmmo2 / 2));
                        didRes = true;
                    }
                    if (didRep || didRes) {
                        emitVehiclePose(vi, false, 0, 0, false, {}, false, false, didRep, didRes);
                    }
                }
                for (size_t si = 0; si < soldiers_.size(); ++si) {
                    auto& ss = soldiers_[si];
                    if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding ||
                        isHostile(st.owner, ss.owner) || ss.owner >= 3) {
                        continue;
                    }
                    const auto [su, sv] = soldierWorld(ss);
                    const int sx = originX_ + su - sv;
                    const int sy = (su + sv) / 2;
                    if (std::hypot(static_cast<double>(sx - st.targetX),
                                   static_cast<double>(sy - st.targetY)) > 220.0) {
                        continue;
                    }
                    const int maxInfAmmo = std::max(1, ss.desc.maxAmmo);
                    if (ss.hp < ss.maxHp || ss.ammo < maxInfAmmo) {
                        ss.hp = std::min(ss.maxHp, ss.hp + 40);
                        ss.ammo = std::max(ss.ammo, std::min(maxInfAmmo, ss.ammo + std::max(1, maxInfAmmo / 2)));
                        const bool vis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
                        pushOrUpdateSoldierPose(changed, {
                            si, sx, sy, ss.direction, 0, false, ss.hp, true, ss.owner, ss.group, false, vis
                        });
                    }
                }
                airEvents_.push_back(AirStrikeEvent{
                    AirStrikeEvent::SupplyDrop, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId
                });
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
}
}
