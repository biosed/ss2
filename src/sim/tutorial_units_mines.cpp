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
void TutorialUnits::setMines(std::vector<std::uint8_t> mines) {
    const std::size_t expected = static_cast<std::size_t>(grid_.width() / 2) * static_cast<std::size_t>(grid_.height() / 2);
    if (mines.size() == expected) {
        mines_ = std::move(mines);
    }
}

std::vector<MineEvent> TutorialUnits::takeMineEvents() {
    return std::exchange(mineEvents_, {});
}

size_t TutorialUnits::orderLayMine(std::span<const size_t> soldierIds, std::vector<SoldierPose>* outPoses) {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tileW <= 0 || tileH <= 0) return 0;
    size_t laid = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.canLayMines || s.mineAmmo <= 0) continue;
        const int tx = s.cell.x / 2;
        const int ty = s.cell.y / 2;
        if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) continue;
        if (mines_.empty()) mines_.assign(static_cast<std::size_t>(tileW) * tileH, 0u);
        const std::size_t idx = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx);
        if ((mines_[idx] & 0x01u) != 0) continue;
        mines_[idx] = 0x85u; // present | anti-tank | player-0 | visible
        --s.mineAmmo;
        const int wu = tx * 32 + 16, wv = ty * 32 + 16;
        mineEvents_.push_back({MineEvent::Laid, originX_ + wu - wv, (wu + wv) / 2, true});
        if (outPoses) {
            const auto [su, sv] = soldierWorld(s);
            const bool vis = !fog_ || s.owner == 0 || (s.spottedMask & 1u) != 0;
            SoldierPose p{id, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false,
                          s.hp, true, s.owner, s.group, false, vis, 0, 0, false,
                          s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo};
            pushOrUpdateSoldierPose(*outPoses, p);
        }
        ++laid;
    }
    return laid;
}

std::optional<std::pair<int, int>> TutorialUnits::findDetectedHostileMineNear(int mapX, int mapY,
                                                                              double maxDistPx) const {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tileW <= 0 || tileH <= 0 || mines_.size() != static_cast<std::size_t>(tileW) * static_cast<std::size_t>(tileH) ||
        !mapBounds(mapX, mapY)) {
        return std::nullopt;
    }
    const auto mineOwner = [](std::uint8_t b) -> int {
        return (b & 0x40u) ? 1 : ((b & 0x20u) ? 2 : 0);
    };
    const SubCell center = pointerCell(mapX, mapY, originX_);
    const int cx = center.x / 2;
    const int cy = center.y / 2;
    std::optional<std::pair<int, int>> best;
    double bestDist = maxDistPx + 1e-6;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            const int tx = cx + dx;
            const int ty = cy + dy;
            if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) continue;
            const std::uint8_t m = mines_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) +
                                          static_cast<std::size_t>(tx)];
            if ((m & 0x81u) != 0x81u || !isHostile(mineOwner(m), 0)) continue;
            const int wu = tx * 32 + 16;
            const int wv = ty * 32 + 16;
            const int mx = originX_ + wu - wv;
            const int my = (wu + wv) / 2;
            const double dist = std::hypot(static_cast<double>(mx - mapX), static_cast<double>(my - mapY));
            if (dist <= bestDist) {
                bestDist = dist;
                best = std::pair{tx, ty};
            }
        }
    }
    return best;
}

bool TutorialUnits::canDisarmMineAt(std::span<const size_t> soldierIds, int mapX, int mapY) const {
    if (!findDetectedHostileMineNear(mapX, mapY, 28.0)) return false;
    return std::ranges::any_of(soldierIds, [&](size_t id) {
        if (id >= soldiers_.size()) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.isSapper;
    });
}

size_t TutorialUnits::orderDisarmMine(std::span<const size_t> soldierIds, int mapX, int mapY) {
    const auto mineTile = findDetectedHostileMineNear(mapX, mapY, 28.0);
    if (!mineTile) return 0;
    const int wu = mineTile->first * 32 + 16;
    const int wv = mineTile->second * 32 + 16;
    const int mx = originX_ + wu - wv;
    const int my = (wu + wv) / 2;
    size_t accepted = 0;
    std::vector<size_t> seen;
    seen.reserve(soldierIds.size());
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size() || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isSapper) continue;
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
        const int tileDist = std::max(std::abs(s.cell.x / 2 - mineTile->first),
                                      std::abs(s.cell.y / 2 - mineTile->second));
        if (tileDist <= 2) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        } else {
            const auto [su, sv] = soldierWorld(s);
            const double sx = static_cast<double>(originX_ + su - sv);
            const double sy = static_cast<double>(su + sv) * 0.5;
            const auto ap = approachPointNear(sx, sy, static_cast<double>(mx), static_cast<double>(my),
                                              40.0, mapPixelW_, mapPixelH_);
            if (!orderSoldierInternal(id, ap.first, ap.second)) {
                (void)orderSoldierInternal(id, mx, my);
            }
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
        }
        s.disarmMineTarget = *mineTile;
        s.attackMove = false;
        ++accepted;
    }
    return accepted;
}

std::vector<VisibleMine> TutorialUnits::visibleMines(int viewerOwner) const {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tileW <= 0 || tileH <= 0 || mines_.size() != static_cast<std::size_t>(tileW) * static_cast<std::size_t>(tileH)) {
        return {};
    }
    const auto mineOwner = [](std::uint8_t b) -> int {
        return (b & 0x40u) ? 1 : ((b & 0x20u) ? 2 : 0);
    };
    std::vector<VisibleMine> out;
    for (int ty = 0; ty < tileH; ++ty) {
        for (int tx = 0; tx < tileW; ++tx) {
            const std::uint8_t m = mines_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) +
                                          static_cast<std::size_t>(tx)];
            if ((m & 0x01u) == 0) continue;
            const int owner = mineOwner(m);
            const bool friendly = (owner == viewerOwner) || (viewerOwner == 0 && isAlliedWithPlayer(owner));
            const bool detected = (m & 0x80u) != 0;
            if (!friendly && !detected) continue;
            const int wu = tx * 32 + 16, wv = ty * 32 + 16;
            out.push_back({originX_ + wu - wv, (wu + wv) / 2, (m & 0x04u) != 0, isHostile(owner, viewerOwner)});
        }
    }
    return out;
}

void TutorialUnits::triggerMineAtTile(int tx, int ty, std::vector<SoldierPose>& changed, bool forceAny) {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (mines_.empty() || tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) return;
    const std::size_t idx = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx);
    const std::uint8_t m = mines_[idx];
    if ((m & 0x01u) == 0) return;
    const bool antiTank = (m & 0x04u) != 0;
    mines_[idx] = 0;
    const int wu = tx * 32 + 16, wv = ty * 32 + 16;
    const int mx = originX_ + wu - wv;
    const int my = (wu + wv) / 2;
    mineEvents_.push_back({MineEvent::Detonated, mx, my, antiTank});

    const auto mSpec = catalog_ ? catalog_->mineSpec() : assets::MineSpec{};
    if (antiTank || forceAny) {
        const double vehRadius = std::max(40.0, static_cast<double>(mSpec.radius) * 1.5);
        for (size_t j = 0; j < vehicles_.size(); ++j) {
            auto& tv = vehicles_[j];
            if (!tv.alive || tv.hp <= 0) continue;
            if (std::hypot(tv.fx - double(mx), tv.fy - double(my)) > vehRadius) continue;
            const int dmg = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             mx, my, 260, assets::HitType::Mine));
            tv.hp = std::max(0, tv.hp - dmg);
            if (dmg > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
                tv.expa = std::min(0x8000, tv.expa + std::min(200, dmg * tv.expaOnHit));
            }
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            if (tv.hp == 0) {
                destroyVehicle(j, changed);
            } else {
                applyVehicleCrewDamage(j, mx, my, 260, assets::HitType::Mine, changed);
                applyVehicleSuppression(j, 28, mx, my);
            }
            emitVehiclePose(j);
        }
    }
    const double infRadius = antiTank
        ? std::max(48.0, static_cast<double>(mSpec.radius) * 1.8)
        : std::max(52.0, static_cast<double>(mSpec.radius) * 2.0);
    const double baseInfDmg = antiTank
        ? 95.0
        : std::max(85.0, static_cast<double>(mSpec.damage) * 1.9);
    for (size_t k = 0; k < soldiers_.size(); ++k) {
        auto& ss = soldiers_[k];
        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding) continue;
        const auto [su, sv] = soldierWorld(ss);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double d = std::hypot(double(sx - mx), double(sy - my));
        if (d > infRadius) continue;
        const int sdmg = std::max(1, static_cast<int>(std::lround(baseInfDmg * (1.0 - d / (infRadius + 2.0)))));
        ss.hp = std::max(0, ss.hp - sdmg);
        recordHit(ss.owner, ss.group);
        if (ss.hp > 0) {
            applySuppression(k, antiTank ? 24 : 30);
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
    if (antiTank || forceAny) {
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            const auto& tb = buildings_[bi];
            if (tb.destroyed) continue;
            const double bd = std::max(0.0, std::hypot(double(tb.x - mx), double(tb.y - my)) -
                                            std::min(tb.width, tb.height) * 0.3);
            if (bd <= 56.0) {
                damageBuilding(bi, 140, assets::HitType::Mine, -1, changed);
            }
        }
    }
    if (antiTank) {
        for (const auto [dx, dy] : std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
            const int ntx = tx + dx;
            const int nty = ty + dy;
            if (ntx < 0 || nty < 0 || ntx >= tileW || nty >= tileH) continue;
            const std::size_t nIdx = static_cast<std::size_t>(nty) * static_cast<std::size_t>(tileW) +
                                     static_cast<std::size_t>(ntx);
            if ((mines_[nIdx] & 0x01u) != 0) {
                triggerMineAtTile(ntx, nty, changed, true);
            }
        }
    }
}

void TutorialUnits::stepMines(std::vector<SoldierPose>& changed) {
    if (mines_.empty()) return;
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    const auto mineOwner = [](std::uint8_t b) {
        return (b & 0x40u) ? 1 : ((b & 0x20u) ? 2 : 0);
    };
    const auto mineTileAt = [&](int tx, int ty) -> std::uint8_t* {
        if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) return nullptr;
        const std::size_t idx = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx);
        return idx < mines_.size() ? &mines_[idx] : nullptr;
    };
    const auto tileCenter = [&](int tx, int ty) -> std::pair<int, int> {
        const int wu = tx * 32 + 16, wv = ty * 32 + 16;
        return {originX_ + wu - wv, (wu + wv) / 2};
    };

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0) continue;
        const int tx = v.cell.x / 2, ty = v.cell.y / 2;
        if (const auto* mp = mineTileAt(tx, ty);
            mp && ((*mp & 0x05u) == 0x05u) && isHostile(mineOwner(*mp), v.owner)) {
            triggerMineAtTile(tx, ty, changed, false);
        }
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        const int tx = s.cell.x / 2, ty = s.cell.y / 2;
        if (const auto* mp = mineTileAt(tx, ty);
            mp && ((*mp & 0x05u) == 0x01u) && isHostile(mineOwner(*mp), s.owner) &&
            !(s.isSapper && (*mp & 0x80u) != 0)) {
            triggerMineAtTile(tx, ty, changed, false);
        }
    }

    const auto routeSapperToMine = [&](size_t sIdx, Soldier& s, int mtx, int mty) {
        const auto [su, sv] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sv);
        const double sy = static_cast<double>(su + sv) * 0.5;
        const auto [nmx, nmy] = tileCenter(mtx, mty);
        const auto ap = approachPointNear(sx, sy, static_cast<double>(nmx), static_cast<double>(nmy),
                                          40.0, mapPixelW_, mapPixelH_);
        if (!orderSoldierInternal(sIdx, ap.first, ap.second)) {
            (void)orderSoldierInternal(sIdx, nmx, nmy);
        }
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.disarmMineTarget = std::pair{mtx, mty};
        s.attackMove = false;
    };

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (!s.disarmMineTarget) continue;
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isSapper) {
            s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            continue;
        }
        const int tx = s.disarmMineTarget->first;
        const int ty = s.disarmMineTarget->second;
        const auto* mp = mineTileAt(tx, ty);
        const bool targetAlive = mp && ((*mp & 0x81u) == 0x81u) && isHostile(mineOwner(*mp), 0);
        if (!targetAlive) {
            const int cx = s.cell.x / 2;
            const int cy = s.cell.y / 2;
            std::optional<std::pair<int, int>> nextMine;
            int bestD2 = std::numeric_limits<int>::max();
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const int ntx = cx + dx;
                    const int nty = cy + dy;
                    const auto* cand = mineTileAt(ntx, nty);
                    if (!cand || ((*cand & 0x81u) != 0x81u) || !isHostile(mineOwner(*cand), 0)) continue;
                    const int d2 = dx * dx + dy * dy;
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        nextMine = std::pair{ntx, nty};
                    }
                }
            }
            if (nextMine) {
                s.disarmMineTarget = nextMine;
                const int td = std::max(std::abs(cx - nextMine->first), std::abs(cy - nextMine->second));
                if (td > 2 && !s.motion) {
                    routeSapperToMine(i, s, nextMine->first, nextMine->second);
                }
            } else {
                s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            }
            continue;
        }
        const int tileDist = std::max(std::abs(s.cell.x / 2 - tx), std::abs(s.cell.y / 2 - ty));
        if (tileDist <= 2) {
            if (!s.motion && s.goal) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                s.planning = {};
                s.needsRoute = false;
            }
        } else if (!s.motion && !s.goal && ((tick_ + i) % 6u == 0u)) {
            routeSapperToMine(i, s, tx, ty);
        }
    }

    if (tick_ % 15 == 0) {
        for (auto& s : soldiers_) {
            if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
            if (!s.isSapper && !s.isOfficer && !s.isMedic) continue;
            const int cx = s.cell.x / 2, cy = s.cell.y / 2;
            const bool canInstantDisarm = s.isSapper && s.getMineTime <= 0 &&
                ((!s.motion && s.route.empty()) || s.disarmMineTarget.has_value() || s.attackMove);
            bool disarmedOne = false;
            if (canInstantDisarm && s.disarmMineTarget) {
                const int tx = s.disarmMineTarget->first;
                const int ty = s.disarmMineTarget->second;
                if (std::abs(tx - cx) <= 2 && std::abs(ty - cy) <= 2) {
                    if (auto* tp = mineTileAt(tx, ty); tp && (*tp & 0x01u) != 0 && isHostile(mineOwner(*tp), 0)) {
                        const auto [mx, my] = tileCenter(tx, ty);
                        const bool antiTank = (*tp & 0x04u) != 0;
                        *tp = 0;
                        mineEvents_.push_back({MineEvent::Disarmed, mx, my, antiTank});
                        disarmedOne = true;
                    }
                }
            }
            for (int dy = -3; dy <= 3; ++dy) {
                for (int dx = -3; dx <= 3; ++dx) {
                    auto* mp = mineTileAt(cx + dx, cy + dy);
                    if (!mp || (*mp & 0x01u) == 0 || !isHostile(mineOwner(*mp), 0)) continue;
                    const auto [mx, my] = tileCenter(cx + dx, cy + dy);
                    if ((*mp & 0x80u) == 0) {
                        *mp |= 0x80u;
                        mineEvents_.push_back({MineEvent::Detected, mx, my, (*mp & 0x04u) != 0});
                    }
                    if (!disarmedOne && canInstantDisarm && std::abs(dx) <= 2 && std::abs(dy) <= 2) {
                        const bool antiTank = (*mp & 0x04u) != 0;
                        *mp = 0;
                        mineEvents_.push_back({MineEvent::Disarmed, mx, my, antiTank});
                        disarmedOne = true;
                    }
                }
            }
        }
    }

    for (auto& s : soldiers_) {
        if (!s.isSapper || s.getMineTime <= 0) continue;
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) {
            s.disarmWorkTicks = 0;
            continue;
        }
        const bool canDisarmNow = (!s.motion && s.route.empty()) || s.disarmMineTarget.has_value() || s.attackMove;
        if (!canDisarmNow) {
            s.disarmWorkTicks = 0;
            continue;
        }
        const int cx = s.cell.x / 2, cy = s.cell.y / 2;
        std::optional<std::pair<int, int>> targetTile;
        if (s.disarmMineTarget) {
            const int tx = s.disarmMineTarget->first;
            const int ty = s.disarmMineTarget->second;
            if (std::abs(tx - cx) <= 2 && std::abs(ty - cy) <= 2) {
                if (const auto* tp = mineTileAt(tx, ty); tp && (*tp & 0x01u) != 0 && isHostile(mineOwner(*tp), 0)) {
                    targetTile = std::pair{tx, ty};
                }
            }
        }
        if (!targetTile) {
            int bestD2 = std::numeric_limits<int>::max();
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const auto* mp = mineTileAt(cx + dx, cy + dy);
                    if (!mp || (*mp & 0x01u) == 0 || !isHostile(mineOwner(*mp), 0)) continue;
                    const int d2 = dx * dx + dy * dy;
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        targetTile = std::pair{cx + dx, cy + dy};
                    }
                }
            }
        }
        if (!targetTile) {
            s.disarmWorkTicks = 0;
            continue;
        }
        const double vetFrac = static_cast<double>(std::clamp(s.expa, 0, 0x8000)) / 32768.0;
        const double effTime = static_cast<double>(s.getMineTime) -
                               vetFrac * static_cast<double>(std::max(0, s.getMineTime - s.getMineTimeMax));
        const int needed = std::clamp(static_cast<int>(std::lround(effTime / 10.0)), 2, 10);
        ++s.disarmWorkTicks;
        if (s.disarmWorkTicks >= needed) {
            if (auto* mp = mineTileAt(targetTile->first, targetTile->second)) {
                const auto [mx, my] = tileCenter(targetTile->first, targetTile->second);
                const bool antiTank = (*mp & 0x04u) != 0;
                if ((*mp & 0x80u) == 0) {
                    mineEvents_.push_back({MineEvent::Detected, mx, my, antiTank});
                }
                *mp = 0;
                mineEvents_.push_back({MineEvent::Disarmed, mx, my, antiTank});
            }
            s.disarmWorkTicks = 0;
        }
    }
}
}
