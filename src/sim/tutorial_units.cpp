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
namespace {
std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}
bool isAntiVehicleSoldierId(std::string_view lowId) {
    static constexpr std::array<std::string_view, 8> kTags{{
        "bazooka", "panzerfaust", "piat", "flamer", "ats", "fatg", "antitank", "anti_tank"
    }};
    return std::ranges::any_of(kTags, [&](std::string_view tag) {
        return lowId.find(tag) != std::string_view::npos;
    });
}
bool isUnarmedVehicleId(std::string_view id) {
    const std::string low = toLowerAscii(id);
    static constexpr std::array<std::string_view, 21> kUnarmedTags{{
        "jeep", "kubel", "truck", "opel", "blitz", "studeb", "zis5", "zis-5",
        "maultier", "mack", "med", "supply", "ambul", "tractor",
        "horch", "bedford", "gmc", "cckw", "morris", "sdkfz.11", "type.94"
    }};
    return std::ranges::any_of(kUnarmedTags, [&](std::string_view tag) {
        return low.find(tag) != std::string_view::npos;
    });
}
}
TutorialUnits::TutorialUnits(InfantryPathGrid grid, std::span<const assets::Infantry> units,
                             std::span<const assets::Placement> vehicles,
                             std::optional<FogGrid> fog, int difficulty, bool autoEngage,
                             std::vector<assets::VehicleUnit> vehicleUnits,
                             std::shared_ptr<const assets::UnitCatalog> catalog)
    : grid_(std::move(grid)), fog_(std::move(fog)), difficulty_(difficulty),
      autoEngage_(autoEngage), catalog_(std::move(catalog)) {
    const int tileW = grid_.width() / 2, tileH = grid_.height() / 2;
    originX_ = tileH * 32;
    mapPixelW_ = (tileW + tileH) * 32;
    mapPixelH_ = (tileW + tileH) * 16;
    for (const auto& unit : units) {
        (void)appendSoldier(unit, placementCell(unit.x, unit.y, originX_), missionExpaToSim(unit.expa));
    }
    if (!vehicleUnits.empty()) {
        for (const auto& vu : vehicleUnits) {
            (void)appendVehicle(vu);
        }
    } else {
        for (const auto& p : vehicles) {
            assets::VehicleUnit vu{};
            vu.id = p.id;
            vu.x = p.x;
            vu.y = p.y;
            vu.hp = p.hp;
            vu.ammo = p.ammo;
            vu.group = p.group;
            vu.owner = p.owner;
            vu.hullDir = static_cast<unsigned>(p.direction) & 31u;
            vu.turretDir = vu.hullDir;
            vu.alive = p.hp > 0;
            vu.visible = true;
            vu.hasCrew = (p.owner != 3) || !p.passengers.empty();
            vu.expa = p.expa;
            (void)appendVehicle(vu);
        }
    }
    if (fog_) refreshSpotting(nullptr, false);
}

bool TutorialUnits::mapBounds(int x, int y) const {
    return x >= 0 && x < mapPixelW_ && y >= 0 && y < mapPixelH_;
}

bool TutorialUnits::isElevatedMapPoint(int mapX, int mapY) const {
    if (!fog_) return false;
    const SubCell cell = placementCell(mapX, mapY, originX_);
    return fog_->isElevated(cell.x / 2, cell.y / 2);
}

bool TutorialUnits::canMove(size_t index, int x, int y) const {
    if (!mapBounds(x,y) || index >= soldiers_.size()) return false;
    const auto& soldier = soldiers_[index];
    if (soldier.owner != effectiveQueryOwner() || soldier.hp <= 0 || soldier.embarked || soldier.garrisonBuilding) return false;
    const auto dest = pointerCell(x,y,originX_);
    if (!grid_.inBounds(dest)) return false;
    return (soldier.goal && sameTile(dest, *soldier.goal)) || grid_.hasTerrainRoute(soldier.cell,dest);
}

bool TutorialUnits::order(size_t index, int x, int y) {
    if (!canMove(index,x,y)) return false;
    auto& soldier = soldiers_[index];
    soldier.boardVehicleTarget.reset();
    soldier.captureVehicleTarget.reset();
    soldier.garrisonTarget.reset();
    soldier.attackTarget.reset();
    soldier.attackVehicleTarget.reset();
    soldier.attackBuildingTarget.reset();
    soldier.attackGround.reset();
    soldier.healTarget.reset();
    soldier.escortVehicleTarget.reset();
    soldier.escortSoldierTarget.reset();
    soldier.disarmMineTarget.reset();
    soldier.queuedGoals.clear();
    soldier.pendingActions.clear();
    soldier.attackMove = false;
    soldier.patrolOrigin.reset();
    soldier.patrolDest.reset();
    const auto dest = pointerCell(x,y,originX_);
    // 0x100369d0 compares tile goals and preserves the current RUN substep.
    if (soldier.goal && sameTile(dest, *soldier.goal)) return true;
    soldier.goal = dest;
    soldier.route.clear();
    soldier.cursor = 0;
    soldier.planning = {};
    // Gold clears the pending request at 0x100369d0. The unit asks for
    // another route at 0x10021a50 once its current RUN substep finishes.
    soldier.needsRoute = false;
    return true;
}

bool TutorialUnits::orderSoldierInternal(size_t index, int x, int y) {
    if (!mapBounds(x, y) || index >= soldiers_.size()) return false;
    auto& soldier = soldiers_[index];
    if (soldier.hp <= 0 || soldier.embarked || soldier.garrisonBuilding) return false;
    const auto dest = findSpawnCell(grid_, pointerCell(x, y, originX_));
    if (!grid_.inBounds(dest) || !grid_.hasTerrainRoute(soldier.cell, dest)) return false;
    soldier.boardVehicleTarget.reset();
    soldier.captureVehicleTarget.reset();
    soldier.garrisonTarget.reset();
    soldier.attackTarget.reset();
    soldier.attackVehicleTarget.reset();
    soldier.attackBuildingTarget.reset();
    soldier.attackGround.reset();
    soldier.healTarget.reset();
    soldier.escortVehicleTarget.reset();
    soldier.escortSoldierTarget.reset();
    soldier.disarmMineTarget.reset();
    if (soldier.goal && sameTile(dest, *soldier.goal)) return true;
    soldier.goal = dest;
    soldier.route.clear();
    soldier.cursor = 0;
    soldier.planning = {};
    soldier.needsRoute = false;
    return true;
}

std::vector<TutorialUnits::Destination> TutorialUnits::formationTargets(
        std::span<const size_t> ids, int mapX, int mapY) const {
    if (ids.empty() || ids.size() > soldiers_.size() || !mapBounds(mapX, mapY))
        return {};
    std::vector<SubCell> sources;
    sources.reserve(ids.size());
    const int owner = effectiveQueryOwner();
    for (size_t id : ids) {
        if (id >= soldiers_.size() || soldiers_[id].owner != owner || soldiers_[id].hp <= 0 ||
            soldiers_[id].embarked || soldiers_[id].garrisonBuilding) return {};
        sources.push_back(soldiers_[id].cell);
    }
    const auto tiles = grid_.formationDestinations(sources, pointerCell(mapX,mapY,originX_));
    std::vector<Destination> result;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const int x = tiles[i].x/2, y = tiles[i].y/2;
        result.push_back({ids[i],originX_+(x-y)*32,(x+y)*16+16});
    }
    return result;
}

bool TutorialUnits::canMoveSelection(std::span<const size_t> ids, int x, int y) const {
    const auto targets = formationTargets(ids,x,y);
    return std::ranges::any_of(targets, [&](const auto& t) { return canMove(t.soldier,t.mapX,t.mapY); });
}

size_t TutorialUnits::orderSelection(std::span<const size_t> ids, int x, int y, bool append) {
    const int actor = actorOwner();
    size_t accepted = 0;
    for (const auto& t : formationTargets(ids, x, y)) {
        auto& s = soldiers_[t.soldier];
        const bool busy = s.motion || s.goal || !s.route.empty();
        if (append && busy && s.owner == actor && s.hp > 0 && !s.embarked && mapBounds(t.mapX, t.mapY)) {
            const auto dest = pointerCell(t.mapX, t.mapY, originX_);
            if (grid_.inBounds(dest) && grid_.hasTerrainRoute(s.cell, dest)) {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.escortVehicleTarget.reset();
                s.escortSoldierTarget.reset();
                s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
                s.attackGround.reset();
                if (!s.pendingActions.empty()) {
                    if (s.pendingActions.size() < 16u) {
                        s.pendingActions.push_back({QueuedSoldierOp::Move, t.mapX, t.mapY, 0});
                    }
                    ++accepted;
                    continue;
                }
                s.queuedGoals.emplace_back(t.mapX, t.mapY);
                ++accepted;
                continue;
            }
        }
        if (order(t.soldier, t.mapX, t.mapY)) ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderAttackMove(std::span<const size_t> ids, int mapX, int mapY, bool append) {
    const int actor = actorOwner();
    size_t accepted = 0;
    for (const auto& t : formationTargets(ids, mapX, mapY)) {
        auto& s = soldiers_[t.soldier];
        const bool busy = s.motion || s.goal || !s.route.empty();
        if (append && busy && s.owner == actor && s.hp > 0 && !s.embarked && mapBounds(t.mapX, t.mapY)) {
            const auto dest = pointerCell(t.mapX, t.mapY, originX_);
            if (grid_.inBounds(dest) && grid_.hasTerrainRoute(s.cell, dest)) {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.escortVehicleTarget.reset();
                s.escortSoldierTarget.reset();
                s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
                s.attackGround.reset();
                if (!s.pendingActions.empty()) {
                    if (s.pendingActions.size() < 16u) {
                        s.pendingActions.push_back({QueuedSoldierOp::AttackMove, t.mapX, t.mapY, 0});
                    }
                    ++accepted;
                    continue;
                }
                s.queuedGoals.emplace_back(t.mapX, t.mapY);
                s.attackMove = true;
                ++accepted;
                continue;
            }
        }
        if (order(t.soldier, t.mapX, t.mapY)) {
            s.attackMove = true;
            ++accepted;
        }
    }
    return accepted;
}

size_t TutorialUnits::orderPatrol(std::span<const size_t> ids, int mapX, int mapY) {
    size_t accepted = 0;
    for (const auto& t : formationTargets(ids, mapX, mapY)) {
        auto& s = soldiers_[t.soldier];
        const auto [su, sv] = soldierWorld(s);
        const std::pair origin{originX_ + su - sv, (su + sv) / 2};
        const std::pair dest{t.mapX, t.mapY};
        if (orderSoldierInternal(t.soldier, t.mapX, t.mapY)) {
            s.queuedGoals.clear();
            s.pendingActions.clear();
            if (det::detHypot(static_cast<double>(dest.first - origin.first),
                           static_cast<double>(dest.second - origin.second)) >= 16.0) {
                s.patrolOrigin = origin;
                s.patrolDest = dest;
            } else {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
            }
            s.patrolToDest = true;
            s.attackMove = true;
            ++accepted;
        }
    }
    return accepted;
}

size_t TutorialUnits::orderScatter(std::span<const size_t> ids) {
    struct Entry {
        size_t id;
        double sx;
        double sy;
    };
    std::vector<Entry> eligible;
    eligible.reserve(ids.size());
    std::vector<size_t> seen;
    seen.reserve(ids.size());
    double sumX = 0.0, sumY = 0.0;
    const int actor = actorOwner();
    for (const size_t id : ids) {
        if (id >= soldiers_.size() || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        const auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        const auto [su, sw] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sw);
        const double sy = static_cast<double>(su + sw) * 0.5;
        eligible.push_back({id, sx, sy});
        sumX += sx;
        sumY += sy;
    }
    if (eligible.empty()) return 0;
    const double cx = sumX / static_cast<double>(eligible.size());
    const double cy = sumY / static_cast<double>(eligible.size());
    std::vector<SubCell> claimedDest;
    claimedDest.reserve(eligible.size());
    static constexpr double kRadii[] = {88.0, 68.0, 52.0, 36.0};
    static constexpr double kAngles[] = {0.0, 0.45, -0.45, 0.90, -0.90, 1.50, -1.50};
    size_t accepted = 0;
    for (size_t k = 0; k < eligible.size(); ++k) {
        const auto& e = eligible[k];
        auto& s = soldiers_[e.id];
        double vx = e.sx - cx;
        double vy = e.sy - cy;
        double len = det::detHypot(vx, vy);
        if (len < 6.0) {
            const double theta = 2.399963229728653 * static_cast<double>(k + 1u);
            const auto tsc = det::detSinCos(theta);
            vx = tsc.c;
            vy = tsc.s;
            len = 1.0;
        }
        const double ux = vx / len;
        const double uy = vy / len;
        bool placed = false;
        for (const double rad : kRadii) {
            for (const double ang : kAngles) {
                const auto asc = det::detSinCos(ang);
                const double rx = ux * asc.c - uy * asc.s;
                const double ry = ux * asc.s + uy * asc.c;
                const int tx = det::detRoundToInt(e.sx + rx * rad);
                const int ty = det::detRoundToInt(e.sy + ry * rad);
                if (!mapBounds(tx, ty)) continue;
                const SubCell dest = findSpawnCell(grid_, pointerCell(tx, ty, originX_));
                if (!grid_.inBounds(dest) || sameTile(dest, s.cell)) continue;
                if (std::ranges::any_of(claimedDest, [&](const SubCell& d) { return sameTile(d, dest); })) continue;
                if (!grid_.hasTerrainRoute(s.cell, dest)) continue;
                if (orderSoldierInternal(e.id, tx, ty)) {
                    s.queuedGoals.clear();
                    s.pendingActions.clear();
                    s.patrolOrigin.reset();
                    s.patrolDest.reset();
                    s.attackMove = false;
                    claimedDest.push_back(dest);
                    ++accepted;
                    placed = true;
                    break;
                }
            }
            if (placed) break;
        }
    }
    return accepted;
}

size_t TutorialUnits::stopSelection(std::span<const size_t> ids) {
    const int actor = actorOwner();
    size_t stopped = 0;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0 || s.embarked) continue;
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        s.queuedGoals.clear();
        s.pendingActions.clear();
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
        if (!s.motion) {
            clearMovingSightPenalty(s);
        }
        ++stopped;
    }
    if (fog_ && fogDirty_) refreshSpotting();
    return stopped;
}

bool TutorialUnits::toggleHoldFire(std::span<const size_t> ids) {
    const int actor = actorOwner();
    bool anyFree = false;
    for (size_t id : ids) {
        if (id < soldiers_.size() && soldiers_[id].owner == actor && soldiers_[id].hp > 0 && !soldiers_[id].holdFire)
            anyFree = true;
    }
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0) continue;
        s.holdFire = anyFree;
        if (anyFree) s.autoTarget.reset();
    }
    return anyFree;
}

bool TutorialUnits::toggleCrouch(std::span<const size_t> ids) {
    const int actor = actorOwner();
    bool anyStanding = false;
    for (size_t id : ids) {
        if (id < soldiers_.size() && soldiers_[id].owner == actor && soldiers_[id].hp > 0 && !soldiers_[id].crouched)
            anyStanding = true;
    }
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0) continue;
        s.crouched = anyStanding;
    }
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting();
    }
    return anyStanding;
}

bool TutorialUnits::toggleBinoculars(std::span<const size_t> ids) {
    return toggleBinocularsWithPoses(ids, nullptr);
}

bool TutorialUnits::toggleBinocularsWithPoses(std::span<const size_t> ids, std::vector<SoldierPose>* changed) {
    const int actor = actorOwner();
    bool anyOfficer = false;
    bool anyOff = false;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        const auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0 || !s.hasBinoculars) continue;
        anyOfficer = true;
        if (!s.binoculars) anyOff = true;
    }
    if (!anyOfficer) return false;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0 || !s.hasBinoculars) continue;
        s.binoculars = anyOff;
    }
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(changed);
    }
    if (changed) {
        for (auto& pose : *changed) {
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
    return anyOff;
}

size_t TutorialUnits::appendSoldier(const assets::Infantry& unit, SubCell cell, int expa) {
    const auto desc = InfantryCombatDesc::forUnitId(unit.id);
    Soldier s{};
    s.cell = cell;
    s.owner = unit.owner;
    s.expa = std::clamp(expa, 0, 0x8000);
    s.group = unit.group;
    s.direction = static_cast<std::uint8_t>(unit.direction & 7u);
    const std::string lowId = toLowerAscii(unit.id);
    const auto* spec = catalog_ ? catalog_->find(unit.id) : nullptr;
    s.desc = desc;
    if (spec && !spec->isVehicle) {
        s.attackPref = spec->attackPref;
        s.hasAttackPref = spec->hasAttackPref;
        if (spec->health > 0) s.desc.maxHp = spec->health;
        if (spec->ammo1 > 0) s.desc.maxAmmo = spec->ammo1;
        if (spec->sight > 0) {
            s.desc.sightBase = std::clamp(spec->sight, 6, 20);
            s.desc.crouchSightBase = std::max(4, s.desc.sightBase - 3);
        }
        if (spec->crouchSight > 0) {
            s.desc.crouchSightBase = std::clamp(spec->crouchSight, 4, 20);
            s.desc.crouchSightDelta = std::max(0, spec->crouchSightMax - spec->crouchSight);
        }
        if (spec->shot1.damage > 0 && spec->shot1.range > 0) {
            s.desc.damageBase = spec->shot1.damage;
            s.desc.damageDelta = std::max(0, spec->shot1.damageMax - spec->shot1.damage);
            s.desc.rangeBase = spec->shot1.range;
            s.desc.rangeDelta = std::max(0, spec->shot1.rangeMax - spec->shot1.range);
            if (spec->shot1.deadzone > 0) s.desc.deadzoneBase = spec->shot1.deadzone;
            if (spec->shot1.accuracyBase > 0) {
                s.desc.accuracyBase = spec->shot1.accuracyBase;
                s.desc.accuracyDelta = spec->shot1.accuracyMin - spec->shot1.accuracyBase;
            }
            if (spec->shot1.reloadTicks > 0) {
                s.desc.reloadBase = spec->shot1.reloadTicks;
                s.desc.reloadDelta = -std::max(0, s.desc.reloadBase / 6);
            }
            if (spec->shot1.burstShots > 0) s.desc.burstCount = spec->shot1.burstShots;
            if (spec->shot1.burstReloadTicks > 0) {
                s.desc.burstReloadBase = spec->shot1.burstReloadTicks;
                s.desc.burstReloadDelta = -std::max(0, s.desc.burstReloadBase / 6);
            }
        }
        if (spec->shot1.hitType != assets::HitType::None) {
            s.primaryHitType = spec->shot1.hitType;
        }
    }
    s.hp = unit.hp > 0 ? unit.hp : s.desc.maxHp;
    s.maxHp = std::max(1, std::max(s.desc.maxHp, s.hp));
    if (spec && !spec->isVehicle && spec->ammo1 > 0) {
        s.ammo = unit.ammo > 0 ? std::max(1, (spec->ammo1 * std::clamp(unit.ammo, 1, 100)) / 100) : spec->ammo1;
    } else {
        s.ammo = unit.ammo > 0 ? unit.ammo : s.desc.maxAmmo;
    }
    s.isMedic = lowId.find("medic") != std::string::npos || (spec && spec->isMedicInfantry());
    s.isOfficer = lowId.find("officer") != std::string::npos || lowId.find("general") != std::string::npos ||
                  (spec && spec->isOfficerInfantry());
    s.moraleResist = (spec && !spec->isVehicle) ? std::max(0, spec->moraleResist) : 0;
    s.fearless = (spec && !spec->isVehicle) ? spec->isFearless() : false;
    s.resupplyCost = (spec && !spec->isVehicle) ? std::max(0, spec->shot1.resupplyCost) : 0;
    s.canBeCrew = (spec && !spec->isVehicle) ? std::max(0, spec->canBeCrew) : 0;
    s.unitRadius = (spec && !spec->isVehicle && spec->unitRadius > 0) ? spec->unitRadius : 5;
    s.canFireFromInside = (spec && !spec->isVehicle) ? spec->canFireFromInside : true;
    s.crouchToFire = (spec && !spec->isVehicle) ? spec->crouchToFire : false;
    s.canCrouchExpa = (spec && !spec->isVehicle) ? spec->canCrouchExpa : false;
    s.lastEmittedCrouch = s.crouched;
    s.expaOnHit = (spec && !spec->isVehicle) ? std::max(0, spec->expaOnHit) : 0;
    if (spec && !spec->isVehicle) {
        if (spec->moveSpeed > 0.01) {
            s.standStepBase = static_cast<unsigned>(std::clamp(det::detRoundToInt(spec->moveSpeed * 16.0), 4, 14));
            const double msMax = spec->moveSpeedMax >= spec->moveSpeed ? spec->moveSpeedMax : spec->moveSpeed;
            s.standStepMax = static_cast<unsigned>(
                std::clamp(det::detRoundToInt(msMax * 16.0), static_cast<int>(s.standStepBase), 14));
        }
        if (spec->crouchMoveSpeed > 0.01) {
            s.crouchStepBase = static_cast<unsigned>(
                std::clamp(det::detRoundToInt(spec->crouchMoveSpeed * 16.0), 3, 12));
            const double cmsMax = spec->crouchMoveSpeedMax >= spec->crouchMoveSpeed
                ? spec->crouchMoveSpeedMax : spec->crouchMoveSpeed;
            s.crouchStepMax = static_cast<unsigned>(
                std::clamp(det::detRoundToInt(cmsMax * 16.0), static_cast<int>(s.crouchStepBase), 12));
        }
    }
    if (s.isOfficer) {
        const bool isGeneral = lowId.find("general") != std::string::npos ||
                               (spec && spec->selType == "general");
        s.officerAuraRadius = (spec && spec->officerRadius > 0) ? spec->officerRadius
                                                                : (isGeneral ? 350 : 160);
        const int retailRate = (spec && spec->moraleAutoIncrease > 0) ? spec->moraleAutoIncrease
                                                                      : (isGeneral ? 10 : 5);
        s.moraleRallyRate = std::clamp((retailRate + 1) / 2, 2, 6);
    }
    s.isAntiVehicle = isAntiVehicleSoldierId(lowId) || (spec && spec->isAntiTankInfantry());
    s.hasCatalogAtShot = (spec && !spec->isVehicle && s.isAntiVehicle &&
                          spec->shot1.damage > 0 && spec->shot1.range > 0);
    s.isFlamer = lowId.find("flam") != std::string::npos || (spec && spec->hasFlamethrower());
    s.isSniper = lowId.find("sniper") != std::string::npos || (spec && spec->isSniper());
    s.isSapper = lowId.find("sapper") != std::string::npos || lowId.find("miner") != std::string::npos ||
                 (spec && (spec->selType == "sapper" || spec->selType == "sappers"));
    s.isMortar = lowId.find("mortar") != std::string::npos || (spec && spec->isMortarInfantry());
    s.isHeavyMg = lowId.find("hmg") != std::string::npos || (spec && spec->isHeavyMgInfantry());
    s.hasBinoculars = s.isOfficer || s.isSniper || (spec && spec->binocularSight > 0);
    if (spec && !spec->isVehicle && spec->binocularSight > 0) {
        s.binoSightBonus = std::clamp(spec->binocularSight - s.desc.sightBase, 4, 14);
    } else {
        s.binoSightBonus = s.isSniper ? 4 : 6;
    }
    s.isKamikaze = lowId.find("kamikaze") != std::string::npos || (spec && spec->isKamikaze());
    s.canGrenadeGround = (spec && !spec->isVehicle && spec->canAttackPoint == 2) ||
                         lowId.find("smguner") != std::string::npos;
    if (s.isSapper) {
        s.canLayMines = true;
        s.maxMineAmmo = 5;
        s.mineAmmo = 5;
    } else if (spec && !spec->isVehicle && !s.isMedic && spec->layMineTime > 0 && spec->mineAmmo > 0) {
        s.canLayMines = true;
        s.maxMineAmmo = std::clamp(spec->mineAmmo, 1, 5);
        s.mineAmmo = s.maxMineAmmo;
    }
    s.getMineTime = (spec && !spec->isVehicle) ? std::max(0, spec->getMineTime) : 0;
    s.getMineTimeMax = (spec && !spec->isVehicle && spec->getMineTimeMax > 0)
        ? std::min(spec->getMineTimeMax, s.getMineTime)
        : s.getMineTime;
    if (!s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar && !s.isKamikaze) s.atGrenades = 2;
    soldiers_.push_back(std::move(s));
    soldierIds_.push_back(unit.id);
    grid_.setOccupied(cell, true);
    return soldiers_.size() - 1;
}

size_t TutorialUnits::appendVehicle(const assets::VehicleUnit& unit) {
    const SubCell cell = placementCell(unit.x, unit.y, originX_);
    Vehicle v{};
    v.id = unit.id;
    v.cell = cell;
    v.x = unit.x;
    v.y = unit.y;
    v.fx = static_cast<double>(unit.x);
    v.fy = static_cast<double>(unit.y);
    v.hullDir = unit.hullDir & 31u;
    v.turretDir = unit.turretDir & 31u;
    v.alive = unit.alive && unit.hp > 0;
    v.owner = unit.owner;
    v.group = unit.group;
    v.hasCrew = unit.hasCrew;
    v.expa = missionExpaToSim(unit.expa);
    v.visible = fog_ ? (unit.owner == 0) : unit.visible;
    const std::string lowId = toLowerAscii(unit.id);
    if (const auto* spec = catalog_ ? catalog_->find(unit.id) : nullptr) {
        v.maxHp = std::max(1, spec->health);
        v.hp = v.alive ? std::max(1, (v.maxHp * std::clamp(unit.hp, 1, 100)) / 100) : 0;
        v.speedPx = spec->moveSpeed > 0.01 ? std::clamp(spec->moveSpeed * 2.5, 0.8, 5.5) : 0.0;
        const double vMsMax = spec->moveSpeedMax >= spec->moveSpeed ? spec->moveSpeedMax : spec->moveSpeed;
        v.speedPxMax = v.speedPx > 0.0 ? std::clamp(vMsMax * 2.5, v.speedPx, 6.8) : 0.0;
        v.turnStep32 = (v.speedPx <= 0.0 || spec->turnDelay >= 12) ? 1u : (spec->turnDelay <= 6 ? 3u : 2u);
        v.sightTiles = std::clamp(spec->sight, 6, 22);
        v.bonusSight = spec->bonusSight;
        v.marchSightBonus = std::clamp(spec->marchSightBonus, 0, 8);
        v.marchEnabled = spec->marchEnabled || (spec->marchSightBonus > 0);
        v.hatchOpen = v.marchEnabled;
        v.isKamikaze = spec->isKamikaze() || lowId.find("sdkfz.303") != std::string::npos ||
                       lowId.find("goliath") != std::string::npos;
        if (spec->shot1.damage > 0 && spec->shot1.range > 0) {
            v.hasPrimary = true;
            v.damage = spec->shot1.damage;
            v.range = v.isKamikaze
                ? std::clamp(static_cast<double>(spec->shot1.range), 48.0, 96.0)
                : std::clamp(spec->shot1.range * 0.65, 140.0, 560.0);
            v.deadzone = std::max(0.0, spec->shot1.deadzone * 0.5);
            v.burstShots1 = std::clamp(spec->shot1.burstShots, 1, 24);
            v.burstReload1 = std::clamp(spec->shot1.burstReloadTicks > 0 ? spec->shot1.burstReloadTicks
                                        : (spec->shot1.reloadTicks > 0 ? spec->shot1.reloadTicks : 60),
                                        10, 360);
            v.cooldown = (v.burstShots1 > 1)
                ? std::clamp(spec->shot1.reloadTicks > 0 ? spec->shot1.reloadTicks : 6, 3, 60)
                : std::clamp(spec->shot1.reloadTicks > 0 ? spec->shot1.reloadTicks : 60, 10, 260);
            v.hitType = spec->shot1.hitType != assets::HitType::None ? spec->shot1.hitType : assets::HitType::Pierce;
            v.sound1 = spec->shot1.soundGroup;
            v.weapon1Flamer = spec->shot1.isFlamethrower();
            if (v.weapon1Flamer && spec->shot1.hitType == assets::HitType::None) {
                v.hitType = assets::HitType::Fire;
            }
            if (v.isKamikaze) {
                v.hitType = assets::HitType::Explosive;
            }
        }
        if (spec->shot2.damage > 0 && spec->shot2.range > 0) {
            v.hasSecondary = true;
            v.damage2 = spec->shot2.damage;
            v.range2 = std::clamp(spec->shot2.range * 0.65, 140.0, 520.0);
            v.cooldown2 = std::clamp(spec->shot2.reloadTicks > 0 ? spec->shot2.reloadTicks : 4, 2, 40);
            v.burstShots2 = std::clamp(spec->shot2.burstShots, 1, 12);
            v.burstReload2 = std::clamp(spec->shot2.burstReloadTicks > 0 ? spec->shot2.burstReloadTicks : 45, 10, 120);
            v.hitType2 = spec->shot2.hitType != assets::HitType::None ? spec->shot2.hitType : assets::HitType::Machine;
            v.sound2 = spec->shot2.soundGroup;
            v.weapon2Flamer = spec->shot2.isFlamethrower();
            if (v.weapon2Flamer && spec->shot2.hitType == assets::HitType::None) {
                v.hitType2 = assets::HitType::Fire;
            }
        }
        v.armed = v.hasPrimary || v.hasSecondary;
        v.ammo = spec->ammo1 > 0
            ? std::max(0, (spec->ammo1 * std::clamp(unit.ammo, 0, 100)) / 100)
            : (v.hasPrimary ? std::max(1, unit.ammo) : 0);
        v.ammo2 = spec->ammo2 > 0
            ? std::max(0, (spec->ammo2 * std::clamp(unit.ammo, 0, 100)) / 100)
            : (v.hasSecondary ? std::max(1, unit.ammo) : 0);
        v.armor = spec->armor;
        v.hasCatalogArmor = true;
        v.crewProtection = spec->crewProtection;
        v.hasCrewProtection = spec->hasCrewProtection;
        v.crewUnit = spec->crewUnit;
        v.canSelfRepair = spec->canSelfRepair;
        const int crewCount = std::clamp(spec->crewNumber > 0 ? spec->crewNumber : 2, 1, 8);
        v.crewNumber = crewCount;
        v.attackCrew = std::clamp(spec->attackCrew > 0 ? spec->attackCrew : 1, 1, crewCount);
        const int baseMoveCrew = std::clamp(spec->moveCrew > 0 ? spec->moveCrew : 1, 1, crewCount);
        v.moveCrew = std::max(baseMoveCrew, std::max(0, spec->soldToMove));
        v.crewVulnerability = spec->crewVulnerability > 0.0 ? spec->crewVulnerability : 0.0;
        v.expaOnHit = std::max(0, spec->expaOnHit);
        v.moraleResist = std::max(0, spec->moraleResist);
        v.crewCanBeHealed = spec->crewCanBeHealed;
        v.indirectArc1 = spec->shot1.indirectArc;
        v.craterScale1 = std::max(0, spec->shot1.craterScale);
        v.cookOffDamage = spec->cookOffDamage();
        v.cookOffRadius = spec->cookOffRadius();
        v.forbiddenTerrainMask = spec->forbiddenTerrainMask();
        v.isNaval = spec->isNavalCraft();
        v.targetType = spec->targetType;
        v.attackPref = spec->attackPref;
        v.hasAttackPref = spec->hasAttackPref;
        v.isWheeled = spec->isWheeled();
        v.isTracked = spec->isTracked();
        v.isTrain = spec->isTrain();
        v.maxCrewHp = 70 + crewCount * 35;
        v.crewHp = v.hasCrew ? v.maxCrewHp : 0;
        v.passengerCapacity = std::clamp(spec->stock, 0, 44);
        v.maxAmmo = std::max(v.ammo, spec->ammo1);
        v.maxAmmo2 = std::max(v.ammo2, spec->ammo2);
        v.moveDamageHp = spec->moveDamageHp;
        v.isSupply = spec->isSupplyTruck();
        v.isCrate = spec->isSupplyCrate();
        v.hasFiniteSupply = v.isSupply && (v.maxAmmo > 0 || v.maxAmmo2 > 0);
        v.isAmbulance = spec->isAmbulance();
        v.isArtillery = spec->isArtillery();
        v.isRocketArtillery = spec->isRocketArtillery();
        v.isCasemate = spec->isCasemate();
        v.canBeCrushed = spec->canBeCrushed;
        v.passengersCanFire = spec->passengersCanFire;
        v.accuracy1 = spec->shot1.accuracyBase;
        v.accuracy1Min = spec->shot1.accuracyMin;
        v.shotExpa1 = spec->shot1.expaPerShot;
        v.shotExpa2 = spec->shot2.expaPerShot;
        v.bonusShotRange = std::max(0, spec->bonusShotRange);
        v.bonusShotDeadZone = std::max(0, spec->bonusShotDeadZone);
        v.scanDelayTicks = spec->scanDelay > 0 ? spec->scanDelay : 0;
        v.crushExpaBonus = std::max(0, spec->expaCrush);
        v.expaReload = std::max(0, spec->expaReload);
        v.expaReloadMax = std::max(0, spec->expaReloadMax);
        v.ammoRegenDelay = std::max(0, spec->ammoRegenDelay);
        v.ammoRegenDelayMax = std::max(0, spec->ammoRegenDelayMax);
        v.healDelay = std::max(0, spec->healDelay);
        v.healDelayMax = std::max(0, spec->healDelayMax);
        v.moraleRndMove = spec->moraleRndMove;
        v.moraleRage = std::max(0, spec->moraleRage);
        v.isPropaganda = spec->isPropaganda();
        v.soldiersOnArmor = spec->soldiersOnArmor;
        v.resupplyCost1 = std::max(0, spec->shot1.resupplyCost);
        v.resupplyCost2 = std::max(0, spec->shot2.resupplyCost);
        v.repairCost = std::max(0, spec->repairCost);
        v.gunShotWait = std::max(0, spec->gunShotWait);
        v.unitRadius = std::max(0, spec->unitRadius);
        v.ammoPerShot1 = (spec->shot1.ammoPerShot >= 1000) ? 1 : std::clamp(spec->shot1.ammoPerShot, 0, 20);
        v.ammoPerShot2 = (spec->shot2.ammoPerShot >= 1000) ? 1 : std::clamp(spec->shot2.ammoPerShot, 0, 20);
        v.hasGunTurnDelay = spec->hasGunTurnDelay;
        if (spec->hasGunTurnDelay) {
            if (spec->gunTurnDelay <= 2) v.turretStep32 = 6u;
            else if (spec->gunTurnDelay <= 6) v.turretStep32 = 3u;
            else if (spec->gunTurnDelay <= 10) v.turretStep32 = 2u;
            else v.turretStep32 = 1u;
        } else {
            v.turretStep32 = 4u;
        }
        const int frontPierce = spec->armor[static_cast<std::size_t>(assets::HitType::Pierce)].front;
        v.canCrushInfantry = !v.isKamikaze && !spec->isTowable() &&
            (spec->hasCrushRange || spec->roleClass == "tank" || spec->selType == "tank" ||
             spec->selType == "spg" || frontPierce >= 25 || spec->health >= 250);
        v.towWeightClass = spec->towWeightClass;
        v.towSpeedByClass = spec->towSpeedByClass;
        v.canMoveBackward = spec->canMoveBackward && v.speedPx > 0.0;
        v.backSpeedPx = v.canMoveBackward
            ? (spec->backMoveSpeed > 0.01 ? std::clamp(spec->backMoveSpeed * 2.5, 0.4, 3.5)
                                          : std::max(0.4, v.speedPx * 0.5))
            : 0.0;
    } else {
        if (lowId == "ammo.box") {
            v.maxHp = 100;
            v.hp = v.alive ? std::clamp(unit.hp > 0 ? unit.hp : 100, 1, 100) : 0;
            v.hasCrew = false;
            v.crewHp = 0;
            v.ammo = (unit.ammo == 100) ? 1000 : std::max(0, unit.ammo);
            v.maxAmmo = std::max(v.ammo, 1000);
            v.armed = false;
            v.hasPrimary = false;
            v.range = 0.0;
            v.cooldown = 0;
            v.damage = 0;
            v.speedPx = 0.0;
            v.speedPxMax = 0.0;
            v.canMoveBackward = false;
            v.backSpeedPx = 0.0;
            v.canCrushInfantry = false;
            v.isSupply = true;
            v.isCrate = true;
            v.hasFiniteSupply = true;
            v.cookOffDamage = 90;
            v.cookOffRadius = 44.0;
        } else {
            v.hp = v.alive ? unit.hp : std::max(0, unit.hp);
            v.maxHp = std::max(1, v.hp);
            v.crewHp = v.hasCrew ? v.maxCrewHp : 0;
            v.ammo = std::max(0, unit.ammo);
            v.maxAmmo = v.ammo;
            v.isKamikaze = lowId.find("kamikaze") != std::string::npos ||
                           lowId.find("sdkfz.303") != std::string::npos ||
                           lowId.find("goliath") != std::string::npos;
            v.armed = v.isKamikaze || !isUnarmedVehicleId(unit.id);
            v.hasPrimary = v.armed;
            if (v.isKamikaze) {
                v.range = 64.0;
                v.damage = 1875;
                v.cooldown = 1;
                v.hitType = assets::HitType::Explosive;
            }
            if (!v.armed) {
                v.range = 0.0;
                v.cooldown = 0;
                v.damage = 0;
            }
            v.isRocketArtillery = lowId.find("katy") != std::string::npos ||
                                  lowId.find("nebel") != std::string::npos ||
                                  lowId.find("bm-31") != std::string::npos ||
                                  lowId.find("bm-13") != std::string::npos ||
                                  lowId.find("calliope") != std::string::npos;
            v.isCasemate = v.isRocketArtillery ||
                           lowId.find("stug") != std::string::npos ||
                           lowId.find("jagd") != std::string::npos ||
                           lowId.find("hetzer") != std::string::npos ||
                           lowId.find("ferdinand") != std::string::npos ||
                           lowId.find("hummel") != std::string::npos ||
                           lowId.find("su-85") != std::string::npos ||
                           lowId.find("su-100") != std::string::npos ||
                           lowId.find("isu-") != std::string::npos;
            if (lowId.find("truck") != std::string::npos || lowId.find("opel") != std::string::npos ||
                lowId.find("maultier") != std::string::npos || lowId.find("halftrack") != std::string::npos ||
                lowId.find("m16") != std::string::npos || lowId.find("sdkfz") != std::string::npos) {
                v.passengerCapacity = 8;
            } else if (lowId.find("jeep") != std::string::npos || lowId.find("kubel") != std::string::npos ||
                       lowId.find("wyllis") != std::string::npos) {
                v.passengerCapacity = 3;
            }
            v.isSupply = lowId.find("sdkfz.11") != std::string::npos || lowId.find("morris.c8") != std::string::npos ||
                         lowId.find("supply") != std::string::npos ||
                         lowId == "m4" || lowId == "m5" || lowId == "ss";
            v.isAmbulance = lowId.size() >= 4 && lowId.ends_with(".med");
            v.canCrushInfantry = !v.isKamikaze && (v.maxHp >= 250) && (v.speedPx > 0.0);
            v.speedPxMax = v.speedPx;
            v.canMoveBackward = v.speedPx > 0.0;
            v.backSpeedPx = v.canMoveBackward ? std::max(0.4, v.speedPx * 0.5) : 0.0;
        }
    }
    {
        const auto* spec = catalog_ ? catalog_->find(unit.id) : nullptr;
        v.isAntiAir = (spec && spec->isAntiAir()) ||
                      lowId.find("flak") != std::string::npos ||
                      lowId.find("wirbel") != std::string::npos ||
                      lowId.find("ostwind") != std::string::npos ||
                      lowId.find("bofors") != std::string::npos ||
                      lowId.find("m16") != std::string::npos ||
                      lowId.find("zsu") != std::string::npos ||
                      lowId.find("quad") != std::string::npos ||
                      ((lowId.find("20mm") != std::string::npos ||
                        lowId.find("37mm") != std::string::npos ||
                        lowId.find("40mm") != std::string::npos) &&
                       (lowId.find("aa") != std::string::npos || lowId.find("flak") != std::string::npos));
        v.isHeavyAntiAir = (spec && spec->isHeavyAntiAir()) ||
                           lowId.find("88") != std::string::npos ||
                           lowId.find("52-k") != std::string::npos ||
                           lowId.find("type.88") != std::string::npos;
    }
    if (v.alive) grid_.setOccupied(cell, true);
    vehicles_.push_back(std::move(v));
    const size_t idx = vehicles_.size() - 1;
    if (catalog_ && (vehicles_[idx].hp != unit.hp || vehicles_[idx].maxHp != std::max(1, unit.hp) ||
                     vehicles_[idx].ammo != unit.ammo)) {
        emitVehiclePose(idx);
    }
    return idx;
}

size_t TutorialUnits::spawnInfantry(const assets::Infantry& unit, int expa) {
    const SubCell cell = findSpawnCell(grid_, placementCell(unit.x, unit.y, originX_));
    const int initExpa = expa > 0 ? std::clamp(expa, 0, 0x8000) : missionExpaToSim(unit.expa);
    const size_t index = appendSoldier(unit, cell, initExpa);
    if (fog_) refreshSpotting();
    return index;
}

bool TutorialUnits::isObjectDestroyedAt(int mapX, int mapY) const {
    if (mapX == 0 && mapY == 0) return true;
    std::optional<bool> bestDestroyed;
    double bestDist2 = 1e18;
    for (const auto& b : buildings_) {
        const bool inBox = (mapX >= b.left - 32 && mapX <= b.left + b.width + 32 &&
                            mapY >= b.top - 32 && mapY <= b.top + b.height + 32);
        const double dx = static_cast<double>(mapX - b.x);
        const double dy = static_cast<double>(mapY - b.y);
        const double dist2 = dx * dx + dy * dy;
        if (!inBox && dist2 > 96.0 * 96.0) continue;
        if (!bestDestroyed || dist2 < bestDist2) {
            bestDestroyed = b.destroyed;
            bestDist2 = dist2;
        }
    }
    for (const auto& br : bridges_) {
        const double dCell = distanceToBridge(br, static_cast<double>(mapX), static_cast<double>(mapY));
        const double dx = static_cast<double>(mapX - br.x);
        const double dy = static_cast<double>(mapY - br.y);
        const double dist2 = dx * dx + dy * dy;
        if (dCell > 72.0 && dist2 > 96.0 * 96.0) continue;
        if (!bestDestroyed || dist2 < bestDist2) {
            bestDestroyed = br.destroyed;
            bestDist2 = dist2;
        }
    }
    if (!bestDestroyed) return true;
    return *bestDestroyed;
}

void TutorialUnits::revealFogAt(int mapX, int mapY, int radiusTiles, int player) {
    if (!fog_ || player < 0 || player >= 4) return;
    const SubCell cell = placementCell(mapX, mapY, originX_);
    fog_->revealCircle(cell.x / 2, cell.y / 2, std::clamp(radiusTiles, 1, 32), player);
    fogDirty_ = true;
    refreshSpotting(nullptr, true);
}

void TutorialUnits::setPlayerTeams(std::array<int, 4> teams) {
    playerTeams_ = teams;
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(nullptr, true);
    }
}

bool TutorialUnits::isHostile(int ownerA, int ownerB) const {
    if (ownerA == ownerB) return false;
    if (!multiplayerSymmetry_ && (ownerA == 3 || ownerB == 3)) return false;
    if (ownerA >= 0 && ownerA < 4 && ownerB >= 0 && ownerB < 4) {
        const int teamA = playerTeams_[static_cast<std::size_t>(ownerA)];
        const int teamB = playerTeams_[static_cast<std::size_t>(ownerB)];
        if (!multiplayerSymmetry_ && (teamA == 3 || teamB == 3)) return false;
        if (teamA == teamB) return false;
    }
    return true;
}

bool TutorialUnits::isAlliedFor(int viewer, int owner) const {
    if (viewer < 0 || viewer >= 4) return owner == viewer;
    if (owner == viewer) return true;
    if (owner < 0 || owner >= 4 || (!multiplayerSymmetry_ && owner == 3)) return false;
    const int team = playerTeams_[static_cast<std::size_t>(owner)];
    return (multiplayerSymmetry_ || team != 3) && team == playerTeams_[static_cast<std::size_t>(viewer)];
}

bool TutorialUnits::isAlliedWithPlayer(int owner) const {
    return isAlliedFor(0, owner);
}

void TutorialUnits::setViewerOwner(int owner) {
    const int clamped = std::clamp(owner, 0, 3);
    if (viewerOwner_ != clamped) {
        viewerOwner_ = clamped;
        fogSnapshotDirty_ = true;
    }
}

void TutorialUnits::setMultiplayerSymmetry(bool enabled) {
    multiplayerSymmetry_ = enabled;
    if (enabled) difficulty_ = 2;
}

bool TutorialUnits::isVisibleToViewer(int owner, std::uint8_t spottedMask) const {
    if (!fog_) return true;
    if (isAlliedFor(viewerOwner_, owner)) return true;
    return (spottedMask & static_cast<std::uint8_t>(1u << viewerOwner_)) != 0u;
}

std::vector<IdleUnit> TutorialUnits::idlePlayerUnits() const {
    std::vector<IdleUnit> out;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.owner == viewerOwner_ && s.hp > 0 && !s.embarked && !s.garrisonBuilding && !s.motion && !s.goal &&
            s.route.empty() && !s.attackTarget && !s.attackVehicleTarget && !s.attackBuildingTarget &&
            !s.attackGround && !s.autoTarget && !s.boardVehicleTarget && !s.captureVehicleTarget &&
            !s.garrisonTarget && !s.healTarget && !s.escortVehicleTarget && !s.escortSoldierTarget &&
            !s.disarmMineTarget && !s.patrolOrigin) {
            out.push_back({false, i});
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (v.alive && v.hp > 0 && v.owner == viewerOwner_ && v.hasCrew && !v.towedBy && !v.moveTarget &&
            v.waypoints.empty() && !v.attackTarget && !v.attackGround && !v.attackBuildingTarget &&
            !v.pendingTowGun && !v.facingTargetDir && !v.supportTargetVehicle &&
            !v.supportTargetBuilding && !v.supportTargetBridge && !v.supportTargetSoldier &&
            !v.escortVehicleTarget && !v.escortSoldierTarget && !v.patrolOrigin) {
            out.push_back({true, i});
        }
    }
    return out;
}

std::vector<TacticalOrderLine> TutorialUnits::selectionOrderLines(std::span<const size_t> soldierIds,
                                                                    std::span<const size_t> vehicleIds) const {
    std::vector<TacticalOrderLine> lines;
    const auto pushSeg = [&](int x0, int y0, int x1, int y1, TacticalOrderLine::Kind kind) {
        if (lines.size() >= 48u) return;
        const int dx = x1 - x0;
        const int dy = y1 - y0;
        if (dx * dx + dy * dy < 64) return;
        for (const auto& ex : lines) {
            if (ex.kind == kind &&
                std::abs(ex.x0 - x0) + std::abs(ex.y0 - y0) + std::abs(ex.x1 - x1) + std::abs(ex.y1 - y1) < 16) {
                return;
            }
        }
        lines.push_back({x0, y0, x1, y1, kind});
    };
    const auto cellToMap = [&](SubCell c) -> std::pair<int, int> {
        const auto [u, v] = InfantryPathGrid::toWorldCenter(c);
        return {originX_ + u - v, (u + v) / 2};
    };

    for (size_t id : soldierIds) {
        if (lines.size() >= 48u) break;
        if (id >= soldiers_.size()) continue;
        const auto& s = soldiers_[id];
        if (s.hp <= 0 || s.embarked || s.owner != viewerOwner_) continue;
        const auto [su, sv] = soldierWorld(s);
        const int sx = s.garrisonBuilding ? buildings_[*s.garrisonBuilding].x : (originX_ + su - sv);
        const int sy = s.garrisonBuilding ? buildings_[*s.garrisonBuilding].y : ((su + sv) / 2);

        if (s.patrolOrigin && s.patrolDest) {
            const auto active = s.patrolToDest ? *s.patrolDest : *s.patrolOrigin;
            pushSeg(sx, sy, active.first, active.second, TacticalOrderLine::Kind::Patrol);
            pushSeg(s.patrolOrigin->first, s.patrolOrigin->second, s.patrolDest->first, s.patrolDest->second,
                    TacticalOrderLine::Kind::Patrol);
        } else if (s.attackTarget && *s.attackTarget < soldiers_.size()) {
            const auto& t = soldiers_[*s.attackTarget];
            if (t.hp > 0 && !t.embarked && !t.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(t);
                pushSeg(sx, sy, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Attack);
            }
        } else if (s.attackVehicleTarget && *s.attackVehicleTarget < vehicles_.size()) {
            const auto& tv = vehicles_[*s.attackVehicleTarget];
            if (tv.alive && tv.hp > 0) pushSeg(sx, sy, tv.x, tv.y, TacticalOrderLine::Kind::Attack);
        } else if (s.attackBuildingTarget && *s.attackBuildingTarget < buildings_.size()) {
            const auto& tb = buildings_[*s.attackBuildingTarget];
            if (!tb.destroyed) pushSeg(sx, sy, tb.x, tb.y, TacticalOrderLine::Kind::Attack);
        } else if (s.healTarget && *s.healTarget < soldiers_.size()) {
            const auto& th = soldiers_[*s.healTarget];
            if (th.hp > 0 && !th.embarked && !th.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(th);
                pushSeg(sx, sy, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (s.escortVehicleTarget && *s.escortVehicleTarget < vehicles_.size()) {
            const auto& tv = vehicles_[*s.escortVehicleTarget];
            if (tv.alive && tv.hp > 0) pushSeg(sx, sy, tv.x, tv.y, TacticalOrderLine::Kind::Support);
        } else if (s.escortSoldierTarget && *s.escortSoldierTarget < soldiers_.size()) {
            const auto& ts = soldiers_[*s.escortSoldierTarget];
            if (ts.hp > 0 && !ts.embarked && !ts.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(ts);
                pushSeg(sx, sy, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (s.disarmMineTarget) {
            const int wu = s.disarmMineTarget->first * 32 + 16;
            const int wv = s.disarmMineTarget->second * 32 + 16;
            pushSeg(sx, sy, originX_ + wu - wv, (wu + wv) / 2, TacticalOrderLine::Kind::Support);
        } else if (s.attackGround) {
            pushSeg(sx, sy, s.attackGround->first, s.attackGround->second, TacticalOrderLine::Kind::AttackGround);
        } else if (s.garrisonTarget && *s.garrisonTarget < buildings_.size() && !buildings_[*s.garrisonTarget].destroyed) {
            pushSeg(sx, sy, buildings_[*s.garrisonTarget].x, buildings_[*s.garrisonTarget].y,
                    TacticalOrderLine::Kind::Support);
        } else if (s.boardVehicleTarget && *s.boardVehicleTarget < vehicles_.size() &&
                   vehicles_[*s.boardVehicleTarget].alive) {
            pushSeg(sx, sy, vehicles_[*s.boardVehicleTarget].x, vehicles_[*s.boardVehicleTarget].y,
                    TacticalOrderLine::Kind::Support);
        } else if (s.captureVehicleTarget && *s.captureVehicleTarget < vehicles_.size() &&
                   vehicles_[*s.captureVehicleTarget].alive) {
            pushSeg(sx, sy, vehicles_[*s.captureVehicleTarget].x, vehicles_[*s.captureVehicleTarget].y,
                    TacticalOrderLine::Kind::Support);
        } else if (s.goal) {
            const auto kind = s.attackMove ? TacticalOrderLine::Kind::AttackMove : TacticalOrderLine::Kind::Move;
            auto prev = cellToMap(*s.goal);
            pushSeg(sx, sy, prev.first, prev.second, kind);
            for (const auto& q : s.queuedGoals) {
                pushSeg(prev.first, prev.second, q.first, q.second, kind);
                prev = q;
            }
        }
        if (!s.pendingActions.empty()) {
            std::pair<int, int> prev{sx, sy};
            if (!s.queuedGoals.empty()) {
                prev = s.queuedGoals.back();
            } else if (s.attackGround) {
                prev = *s.attackGround;
            } else if (s.disarmMineTarget) {
                const int wu = s.disarmMineTarget->first * 32 + 16;
                const int wv = s.disarmMineTarget->second * 32 + 16;
                prev = {originX_ + wu - wv, (wu + wv) / 2};
            } else if (s.garrisonTarget && *s.garrisonTarget < buildings_.size()) {
                prev = {buildings_[*s.garrisonTarget].x, buildings_[*s.garrisonTarget].y};
            } else if (s.boardVehicleTarget && *s.boardVehicleTarget < vehicles_.size()) {
                prev = {vehicles_[*s.boardVehicleTarget].x, vehicles_[*s.boardVehicleTarget].y};
            } else if (s.captureVehicleTarget && *s.captureVehicleTarget < vehicles_.size()) {
                prev = {vehicles_[*s.captureVehicleTarget].x, vehicles_[*s.captureVehicleTarget].y};
            } else if (s.goal) {
                prev = cellToMap(*s.goal);
            }
            for (const auto& qa : s.pendingActions) {
                std::pair<int, int> dst = prev;
                auto kind = TacticalOrderLine::Kind::Move;
                switch (qa.op) {
                case QueuedSoldierOp::Move:
                    dst = {qa.x, qa.y};
                    kind = TacticalOrderLine::Kind::Move;
                    break;
                case QueuedSoldierOp::AttackMove:
                    dst = {qa.x, qa.y};
                    kind = TacticalOrderLine::Kind::AttackMove;
                    break;
                case QueuedSoldierOp::AttackGround:
                    dst = {qa.x, qa.y};
                    kind = TacticalOrderLine::Kind::AttackGround;
                    break;
                case QueuedSoldierOp::GarrisonBuilding:
                    if (qa.targetIndex < buildings_.size() && !buildings_[qa.targetIndex].destroyed) {
                        dst = {buildings_[qa.targetIndex].x, buildings_[qa.targetIndex].y};
                    }
                    kind = TacticalOrderLine::Kind::Support;
                    break;
                case QueuedSoldierOp::BoardVehicle:
                case QueuedSoldierOp::CaptureVehicle:
                    if (qa.targetIndex < vehicles_.size() && vehicles_[qa.targetIndex].alive) {
                        dst = {vehicles_[qa.targetIndex].x, vehicles_[qa.targetIndex].y};
                    }
                    kind = TacticalOrderLine::Kind::Support;
                    break;
                case QueuedSoldierOp::LayMine:
                    kind = TacticalOrderLine::Kind::Support;
                    break;
                case QueuedSoldierOp::DisarmMine:
                    dst = {qa.x, qa.y};
                    kind = TacticalOrderLine::Kind::Support;
                    break;
                }
                if (dst != prev) {
                    pushSeg(prev.first, prev.second, dst.first, dst.second, kind);
                    prev = dst;
                }
            }
        }
    }

    for (size_t id : vehicleIds) {
        if (lines.size() >= 48u) break;
        if (id >= vehicles_.size()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != viewerOwner_) continue;

        if (v.patrolOrigin && v.patrolDest) {
            const auto active = v.patrolToDest ? *v.patrolDest : *v.patrolOrigin;
            pushSeg(v.x, v.y, active.first, active.second, TacticalOrderLine::Kind::Patrol);
            pushSeg(v.patrolOrigin->first, v.patrolOrigin->second, v.patrolDest->first, v.patrolDest->second,
                    TacticalOrderLine::Kind::Patrol);
        } else if (v.attackGround) {
            pushSeg(v.x, v.y, v.attackGround->first, v.attackGround->second, TacticalOrderLine::Kind::AttackGround);
        } else if (v.attackTarget) {
            const auto [isVeh, tid] = *v.attackTarget;
            if (isVeh && tid < vehicles_.size() && vehicles_[tid].alive && vehicles_[tid].hp > 0) {
                pushSeg(v.x, v.y, vehicles_[tid].x, vehicles_[tid].y, TacticalOrderLine::Kind::Attack);
            } else if (!isVeh && tid < soldiers_.size() && soldiers_[tid].hp > 0 &&
                       !soldiers_[tid].embarked && !soldiers_[tid].garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(soldiers_[tid]);
                pushSeg(v.x, v.y, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Attack);
            }
        } else if (v.attackBuildingTarget && *v.attackBuildingTarget < buildings_.size()) {
            const auto& tb = buildings_[*v.attackBuildingTarget];
            if (!tb.destroyed) pushSeg(v.x, v.y, tb.x, tb.y, TacticalOrderLine::Kind::Attack);
        } else if (v.supportTargetVehicle && *v.supportTargetVehicle < vehicles_.size()) {
            const auto& tv = vehicles_[*v.supportTargetVehicle];
            if (tv.alive && tv.hp > 0) pushSeg(v.x, v.y, tv.x, tv.y, TacticalOrderLine::Kind::Support);
        } else if (v.supportTargetBuilding && *v.supportTargetBuilding < buildings_.size()) {
            const auto& tb = buildings_[*v.supportTargetBuilding];
            if (!tb.destroyed) pushSeg(v.x, v.y, tb.x, tb.y, TacticalOrderLine::Kind::Support);
        } else if (v.supportTargetBridge && *v.supportTargetBridge < bridges_.size()) {
            const auto& tbr = bridges_[*v.supportTargetBridge];
            if (tbr.hp < tbr.maxHp) pushSeg(v.x, v.y, tbr.x, tbr.y, TacticalOrderLine::Kind::Support);
        } else if (v.supportTargetSoldier && *v.supportTargetSoldier < soldiers_.size()) {
            const auto& ts = soldiers_[*v.supportTargetSoldier];
            if (ts.hp > 0 && !ts.embarked && !ts.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(ts);
                pushSeg(v.x, v.y, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (v.escortVehicleTarget && *v.escortVehicleTarget < vehicles_.size()) {
            const auto& tv = vehicles_[*v.escortVehicleTarget];
            if (tv.alive && tv.hp > 0) pushSeg(v.x, v.y, tv.x, tv.y, TacticalOrderLine::Kind::Support);
        } else if (v.escortSoldierTarget && *v.escortSoldierTarget < soldiers_.size()) {
            const auto& ts = soldiers_[*v.escortSoldierTarget];
            if (ts.hp > 0 && !ts.embarked && !ts.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(ts);
                pushSeg(v.x, v.y, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (v.moveTarget) {
            const auto kind = v.attackMove ? TacticalOrderLine::Kind::AttackMove : TacticalOrderLine::Kind::Move;
            auto prev = *v.moveTarget;
            pushSeg(v.x, v.y, prev.first, prev.second, kind);
            for (const auto& q : v.queuedOrders) {
                pushSeg(prev.first, prev.second, q.first, q.second, kind);
                prev = q;
            }
        }
        if (!v.pendingActions.empty()) {
            std::pair<int, int> prev{v.x, v.y};
            if (!v.queuedOrders.empty()) {
                prev = v.queuedOrders.back();
            } else if (v.attackGround) {
                prev = *v.attackGround;
            } else if (v.moveTarget) {
                prev = *v.moveTarget;
            }
            for (const auto& qv : v.pendingActions) {
                std::pair<int, int> dst = prev;
                auto kind = TacticalOrderLine::Kind::Move;
                switch (qv.op) {
                case QueuedVehicleOp::Move:
                case QueuedVehicleOp::ReverseMove:
                    dst = {qv.x, qv.y};
                    kind = TacticalOrderLine::Kind::Move;
                    break;
                case QueuedVehicleOp::AttackMove:
                    dst = {qv.x, qv.y};
                    kind = TacticalOrderLine::Kind::AttackMove;
                    break;
                case QueuedVehicleOp::AttackGround:
                    dst = {qv.x, qv.y};
                    kind = TacticalOrderLine::Kind::AttackGround;
                    break;
                case QueuedVehicleOp::FaceDir:
                case QueuedVehicleOp::Unload:
                case QueuedVehicleOp::Unhitch:
                    kind = TacticalOrderLine::Kind::Support;
                    break;
                }
                if (dst != prev) {
                    pushSeg(prev.first, prev.second, dst.first, dst.second, kind);
                    prev = dst;
                }
            }
        }
    }
    return lines;
}

bool TutorialUnits::executeQueuedSoldierAction(size_t id, const QueuedSoldierAction& act,
                                               std::vector<SoldierPose>* outPoses) {
    if (id >= soldiers_.size()) return false;
    auto saved = std::move(soldiers_[id].pendingActions);
    soldiers_[id].pendingActions.clear();
    const std::array<size_t, 1> one{id};
    ScopedCommandActor actorGuard(*this, soldiers_[id].owner);
    bool ok = false;
    switch (act.op) {
    case QueuedSoldierOp::Move:
        ok = orderSelection(one, act.x, act.y, false) > 0;
        break;
    case QueuedSoldierOp::AttackMove:
        ok = orderAttackMove(one, act.x, act.y, false) > 0;
        break;
    case QueuedSoldierOp::AttackGround:
        ok = orderAttackGroundSoldiers(one, act.x, act.y) > 0;
        break;
    case QueuedSoldierOp::GarrisonBuilding:
        ok = orderGarrisonBuilding(one, act.targetIndex) > 0;
        break;
    case QueuedSoldierOp::BoardVehicle:
        ok = orderBoardVehicle(one, act.targetIndex) > 0;
        break;
    case QueuedSoldierOp::CaptureVehicle:
        ok = orderCaptureVehicle(one, act.targetIndex) > 0;
        break;
    case QueuedSoldierOp::LayMine:
        ok = orderLayMine(one, outPoses) > 0;
        break;
    case QueuedSoldierOp::DisarmMine:
        ok = orderDisarmMine(one, act.x, act.y) > 0;
        break;
    }
    soldiers_[id].pendingActions = std::move(saved);
    return ok;
}

size_t TutorialUnits::queueSoldierOrder(std::span<const size_t> soldierIds, QueuedSoldierOp op,
                                          int targetX, int targetY, std::size_t targetIndex,
                                          std::vector<SoldierPose>* outPoses) {
    const int actor = actorOwner();
    const bool groundOp = (op == QueuedSoldierOp::Move || op == QueuedSoldierOp::AttackMove);
    std::vector<size_t> eligible;
    eligible.reserve(soldierIds.size());
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& s = soldiers_[id];
        if (s.owner != actor || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    const auto queueOne = [&](size_t id, int px, int py) {
        auto& s = soldiers_[id];
        const QueuedSoldierAction act{op, px, py, targetIndex};
        const bool isBusy = s.motion.has_value() || s.goal.has_value() || !s.route.empty() ||
                            !s.queuedGoals.empty() || !s.pendingActions.empty() ||
                            s.attackGround.has_value() || s.disarmMineTarget.has_value() ||
                            s.garrisonTarget.has_value() || s.boardVehicleTarget.has_value() ||
                            s.captureVehicleTarget.has_value();
        if (!isBusy) {
            if (executeQueuedSoldierAction(id, act, outPoses)) ++accepted;
            return;
        }
        if (!s.queuedGoals.empty()) {
            const QueuedSoldierOp qOp = s.attackMove ? QueuedSoldierOp::AttackMove : QueuedSoldierOp::Move;
            while (!s.queuedGoals.empty() && s.pendingActions.size() < 16u) {
                const auto [qx, qy] = s.queuedGoals.front();
                s.queuedGoals.pop_front();
                s.pendingActions.push_back({qOp, qx, qy, 0});
            }
            s.queuedGoals.clear();
        }
        if (s.pendingActions.size() < 16u) {
            s.pendingActions.push_back(act);
            ++accepted;
        }
    };
    if (groundOp && !eligible.empty()) {
        for (const auto& ft : formationTargets(eligible, targetX, targetY)) {
            queueOne(ft.soldier, ft.mapX, ft.mapY);
        }
    } else {
        for (const size_t id : eligible) {
            queueOne(id, targetX, targetY);
        }
    }
    return accepted;
}

void TutorialUnits::markShellCraterAt(int mapX, int mapY) {
    if (!catalog_ || !catalog_->hasRetailCatalog() || !mapBounds(mapX, mapY)) return;
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tileW <= 0 || tileH <= 0) return;
    const SubCell c = pointerCell(mapX, mapY, originX_);
    if (!grid_.inBounds(c) || grid_.isShallows(c)) return;
    const int tx = c.x / 2;
    const int ty = c.y / 2;
    if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) return;
    if (craterTiles_.empty()) {
        craterTiles_.assign(static_cast<std::size_t>(tileW) * static_cast<std::size_t>(tileH), std::uint8_t{0});
    }
    craterTiles_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx)] = 1u;
}

int TutorialUnits::infantryCoverKindAtTile(int tx, int ty) const {
    if (!catalog_ || !catalog_->hasRetailCatalog()) return 0;
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) return 0;
    if (!craterTiles_.empty() &&
        craterTiles_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx)] != 0u) {
        return 2;
    }
    if (fog_ && fog_->isForestCover(tx, ty)) return 1;
    return 0;
}

int TutorialUnits::infantryCoverAtMapPoint(int mapX, int mapY) const {
    if (!mapBounds(mapX, mapY)) return 0;
    const SubCell c = pointerCell(mapX, mapY, originX_);
    return infantryCoverKindAtTile(c.x / 2, c.y / 2);
}

void TutorialUnits::addReserveUnit(const ReserveUnit& entry) {
    reserveUnits_.push_back(entry);
}

std::vector<TutorialUnits::ReserveUnit> TutorialUnits::takeReserveUnits(unsigned group, int pool, std::size_t maxCount) {
    std::vector<ReserveUnit> deployed;
    std::erase_if(reserveUnits_, [&](const ReserveUnit& r) {
        if (deployed.size() >= maxCount) return false;
        if (r.group < 0 || static_cast<unsigned>(r.group) != group) return false;
        if (pool >= 0 && r.pool != pool) return false;
        deployed.push_back(r);
        return true;
    });
    return deployed;
}

unsigned TutorialUnits::countReserveByGroup(unsigned group) const {
    const int g = int(group);
    return unsigned(std::ranges::count_if(reserveUnits_, [&](const ReserveUnit& r) { return r.group == g; }));
}

unsigned TutorialUnits::countReserveByPlayer(int owner) const {
    return unsigned(std::ranges::count_if(reserveUnits_, [&](const ReserveUnit& r) { return r.owner == owner; }));
}

unsigned TutorialUnits::groupAmmoPercent(unsigned group, unsigned slot) const {
    const int g = int(group);
    unsigned sum = 0;
    unsigned count = 0;
    if (slot == 0u) {
        for (const auto& s : soldiers_) {
            if (s.hp <= 0 || s.group != g || s.desc.maxAmmo <= 0) continue;
            sum += static_cast<unsigned>(std::clamp((s.ammo * 100) / s.desc.maxAmmo, 0, 100));
            ++count;
        }
    }
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || v.group != g) continue;
        if (slot == 0u && v.maxAmmo > 0) {
            sum += static_cast<unsigned>(std::clamp((v.ammo * 100) / v.maxAmmo, 0, 100));
            ++count;
        } else if (slot == 1u && v.maxAmmo2 > 0) {
            sum += static_cast<unsigned>(std::clamp((v.ammo2 * 100) / v.maxAmmo2, 0, 100));
            ++count;
        }
    }
    return count > 0u ? (sum / count) : 0u;
}

std::size_t TutorialUnits::setGroupStat(unsigned group, unsigned statKind, unsigned mode, unsigned pct,
                                        std::vector<SoldierPose>* outSoldiers) {
    const int g = int(group);
    const int clampedPct = static_cast<int>(std::min(pct, 100u));
    std::size_t modified = 0;
    const auto shouldSet = [&](int current, int target) {
        return mode == 0u ? (current > target) : (current < target);
    };

    for (std::size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.group != g) continue;
        bool changedAny = false;
        if (statKind == 0u) {
            const int target = std::clamp((s.desc.maxHp * clampedPct) / 100, 1, std::max(1, s.desc.maxHp));
            if (shouldSet(s.hp, target)) { s.hp = target; changedAny = true; }
        } else if (statKind == 1u) {
            const int maxA = std::max(0, s.desc.maxAmmo);
            const int target = std::clamp((maxA * clampedPct) / 100, 0, maxA);
            if (shouldSet(s.ammo, target)) { s.ammo = target; changedAny = true; }
        } else if (statKind == 2u) {
            const int target = missionExpaToSim(clampedPct);
            if (shouldSet(s.expa, target)) { s.expa = target; changedAny = true; }
        } else if (statKind == 3u) {
            if (mode == 0u && clampedPct < 50) {
                const int targetSup = (50 - clampedPct) * 4;
                if (s.suppressionTicks < targetSup) { s.suppressionTicks = targetSup; changedAny = true; }
            } else if (mode != 0u && clampedPct >= 50 && s.suppressionTicks > 0) {
                s.suppressionTicks = 0;
                changedAny = true;
            }
        }
        if (changedAny) {
            ++modified;
            if (outSoldiers) {
                const auto [su, sv] = s.garrisonBuilding
                    ? tutorialFootWorld(buildings_[*s.garrisonBuilding].x, buildings_[*s.garrisonBuilding].y, originX_)
                    : soldierWorld(s);
                const bool vis = !s.embarked && isVisibleToViewer(s.owner, s.spottedMask);
                pushOrUpdateSoldierPose(*outSoldiers, {
                    i, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false,
                    s.hp, true, s.owner, s.group, false, vis, 0, 0, s.garrisonBuilding.has_value(),
                    s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo, s.ammo,
                    false, s.isSuppressed()
                });
            }
        }
    }

    for (std::size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.group != g) continue;
        bool changedAny = false;
        if (statKind == 0u) {
            const int target = std::clamp((v.maxHp * clampedPct) / 100, 1, std::max(1, v.maxHp));
            if (shouldSet(v.hp, target)) { v.hp = target; changedAny = true; }
        } else if (statKind == 1u) {
            const int target1 = std::clamp((v.maxAmmo * clampedPct) / 100, 0, std::max(0, v.maxAmmo));
            if (v.maxAmmo > 0 && shouldSet(v.ammo, target1)) { v.ammo = target1; changedAny = true; }
            const int target2 = std::clamp((v.maxAmmo2 * clampedPct) / 100, 0, std::max(0, v.maxAmmo2));
            if (v.maxAmmo2 > 0 && shouldSet(v.ammo2, target2)) { v.ammo2 = target2; changedAny = true; }
        } else if (statKind == 2u) {
            const int target = missionExpaToSim(clampedPct);
            if (shouldSet(v.expa, target)) { v.expa = target; changedAny = true; }
        } else if (statKind == 3u) {
            if (mode == 0u && clampedPct < 50) {
                const int targetSup = (50 - clampedPct) * 4;
                if (v.suppressionTicks < targetSup) { v.suppressionTicks = targetSup; changedAny = true; }
            } else if (mode != 0u && clampedPct >= 50 && v.suppressionTicks > 0) {
                v.suppressionTicks = 0;
                changedAny = true;
            }
        }
        if (changedAny) {
            emitVehiclePose(i);
            ++modified;
        }
    }
    return modified;
}

std::vector<assets::ReinforcementUnit> TutorialUnits::collectLiveUnitsForPool(bool byGroup,
                                                                               unsigned selector) const {
    std::vector<assets::ReinforcementUnit> out;
    const int sel = int(selector);
    const auto soldierToPctAmmo = [](const Soldier& s) {
        return s.desc.maxAmmo > 0 ? std::clamp((s.ammo * 100) / s.desc.maxAmmo, 0, 100) : 100;
    };
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0) continue;
        if ((byGroup ? v.group : v.owner) != sel) continue;
        assets::ReinforcementUnit ru;
        ru.id = v.id;
        ru.group = v.group;
        ru.hp = std::clamp((v.hp * 100) / std::max(1, v.maxHp), 1, 100);
        ru.ammo = v.maxAmmo > 0 ? std::clamp((v.ammo * 100) / v.maxAmmo, 0, 100) : 100;
        ru.expa = std::clamp((v.expa * 100) / 0x8000, 0, 100);
        for (std::uint32_t pi : v.passengers) {
            if (pi >= soldiers_.size()) continue;
            const auto& p = soldiers_[pi];
            if (p.hp <= 0) continue;
            const std::string pid = (pi < soldierIds_.size() && !soldierIds_[pi].empty())
                ? soldierIds_[pi] : std::string("dguner");
            ru.passengers.push_back({
                pid, p.group,
                std::clamp((p.hp * 100) / std::max(1, p.desc.maxHp), 1, 100),
                soldierToPctAmmo(p),
                std::clamp((p.expa * 100) / 0x8000, 0, 100),
                false
            });
        }
        out.push_back(std::move(ru));
    }
    for (std::size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked) continue;
        if ((byGroup ? s.group : s.owner) != sel) continue;
        const std::string sid = (i < soldierIds_.size() && !soldierIds_[i].empty())
            ? soldierIds_[i] : std::string("dguner");
        out.push_back({
            sid, s.group,
            std::clamp((s.hp * 100) / std::max(1, s.desc.maxHp), 1, 100),
            soldierToPctAmmo(s),
            std::clamp((s.expa * 100) / 0x8000, 0, 100),
            {}
        });
    }
    return out;
}

void TutorialUnits::setPlayerTeam(unsigned player, int team) {
    if (player >= playerTeams_.size()) return;
    playerTeams_[player] = team;
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(nullptr, true);
    }
}

int TutorialUnits::playerTeam(int owner) const noexcept {
    if (owner < 0 || owner >= 4) return -1;
    return playerTeams_[static_cast<std::size_t>(owner)];
}

int TutorialUnits::weatherVisibilityPct() const noexcept {
    if (weather_ == WeatherKind::Clear) return 100;
    const assets::WeatherSpec def{};
    const auto& ws = catalog_ ? catalog_->weather() : def;
    switch (weather_) {
    case WeatherKind::Fog: return std::clamp(ws.fogVisibilityPct, 0, 100);
    case WeatherKind::Rain: return std::clamp(ws.rainVisibilityPct, 0, 100);
    case WeatherKind::Snow: return std::clamp(ws.snowVisibilityPct, 0, 100);
    case WeatherKind::Clear: break;
    }
    return 100;
}

void TutorialUnits::setWeather(WeatherKind w) {
    if (weather_ == w) return;
    weather_ = w;
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(nullptr, true);
    }
}

unsigned TutorialUnits::countAliveInGroup(int owner, int group) const {
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.owner == owner && s.group == group;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.owner == owner && v.group == group;
    }));
}

unsigned TutorialUnits::countAliveByGroup(unsigned group) const {
    const int g = int(group);
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.group == g;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.group == g && (v.hasCrew || !v.passengers.empty());
    }));
}

unsigned TutorialUnits::countAliveByPlayer(int owner) const {
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.owner == owner;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.owner == owner && (v.hasCrew || !v.passengers.empty());
    }));
}

unsigned TutorialUnits::countByGroupMask(unsigned group, unsigned mask) const {
    const int g = int(group);
    unsigned total = 0;
    if ((mask & 1u) != 0u) {
        total += countAliveByGroup(group);
    }
    if ((mask & 2u) != 0u) {
        total += unsigned(std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.group == g && !v.hasCrew && v.passengers.empty();
        }));
    }
    if ((mask & 4u) != 0u) {
        total += countReserveByGroup(group);
    }
    return total;
}

unsigned TutorialUnits::countByGroupTeamMask(unsigned group, int team, unsigned mask) const {
    const int g = int(group);
    const auto matchesTeam = [&](int owner) {
        return owner >= 0 && owner < 4 && playerTeams_[static_cast<std::size_t>(owner)] == team;
    };
    unsigned total = 0;
    if ((mask & 1u) != 0u) {
        total += unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
            return s.hp > 0 && s.group == g && matchesTeam(s.owner);
        }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.group == g && (v.hasCrew || !v.passengers.empty()) &&
                   matchesTeam(v.owner);
        }));
    }
    if ((mask & 2u) != 0u) {
        total += unsigned(std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.group == g && !v.hasCrew && v.passengers.empty() &&
                   matchesTeam(v.owner);
        }));
    }
    if ((mask & 4u) != 0u) {
        total += unsigned(std::ranges::count_if(reserveUnits_, [&](const ReserveUnit& r) {
            return r.group == g && matchesTeam(r.owner);
        }));
    }
    return total;
}

unsigned TutorialUnits::countByPlayerMask(int owner, unsigned mask) const {
    unsigned total = 0;
    if ((mask & 1u) != 0u) {
        total += countAliveByPlayer(owner);
    }
    if ((mask & 2u) != 0u) {
        total += unsigned(std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.owner == owner && !v.hasCrew && v.passengers.empty();
        }));
    }
    if ((mask & 4u) != 0u) {
        total += countReserveByPlayer(owner);
    }
    return total;
}

void TutorialUnits::collectUnitTilePositions(std::vector<std::tuple<int, int, int>>& out) const {
    out.clear();
    out.reserve(soldiers_.size() + vehicles_.size());
    for (const auto& s : soldiers_) {
        if (s.hp <= 0 || s.embarked) continue;
        SubCell c = s.cell;
        if (s.garrisonBuilding && *s.garrisonBuilding < buildings_.size()) {
            c = buildings_[*s.garrisonBuilding].entryCell;
        }
        out.emplace_back(s.owner, c.x / 2, c.y / 2);
    }
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || (!v.hasCrew && v.passengers.empty())) continue;
        out.emplace_back(v.owner, v.cell.x / 2, v.cell.y / 2);
    }
}

std::pair<int, int> TutorialUnits::soldierWorld(const Soldier& s) const {
    if (s.motion) return s.motion->position();
    return InfantryPathGrid::toWorldCenter(s.cell);
}

bool TutorialUnits::soldierAtCell(SubCell cell) const {
    return std::ranges::any_of(soldiers_, [&](const auto& s) {
        return s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.cell == cell;
    });
}

bool TutorialUnits::vehicleAtCell(SubCell cell) const {
    return std::ranges::any_of(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.cell == cell;
    });
}

void TutorialUnits::refreshSpotting(std::vector<SoldierPose>* changed, bool emitVehiclePoses) {
    if (!fog_) return;
    fog_->clearCurrentVisibility();
    const auto revealWithAllies = [&](int tileX, int tileY, int r, int owner) {
        if (owner < 0 || owner >= 4) return;
        for (int p = 0; p < 4; ++p) {
            if (p == owner || (!isHostile(owner, p) && (multiplayerSymmetry_ || playerTeams_[p] != 3))) {
                fog_->revealCircle(tileX, tileY, r, p);
            }
        }
    };
    const int visPct = weatherVisibilityPct();
    for (const auto& s : soldiers_) {
        if (s.hp <= 0 || s.embarked) continue;
        if (s.garrisonBuilding) {
            // Gold CHouse::GetSightRadius (0x100290e0..0x100291fc): standing occupant sight +
            // building sight bonus pre-weather, weather truncation clamped to [1, 31], then Easy 1.3x.
            const auto& b = buildings_[*s.garrisonBuilding];
            if (b.destroyed) {
                revealWithAllies(b.entryCell.x / 2, b.entryCell.y / 2, 1, s.owner);
                continue;
            }
            const int occSight = scaleByExpa(s.desc.sightBase, s.desc.sightDelta, s.expa);
            const int garBonus = b.sightBonus + (s.hasBinoculars && s.binoculars ? s.binoSightBonus : 0);
            const int preWeather = occSight + garBonus;
            int r = std::clamp(visPct >= 100 ? preWeather : (preWeather * visPct) / 100, 1, 31);
            if (difficulty_ <= 1 && s.owner == 0 && !multiplayerSymmetry_) {
                r = std::clamp((r * 13) / 10, 1, 31);
            }
            revealWithAllies(b.entryCell.x / 2, b.entryCell.y / 2, r, s.owner);
            continue;
        }
        // Gold CSoldier::GetSightRadius (0x100212c0..0x100213b3): binocular sight enters pre-weather
        // alongside High Ground (+2), followed by moving -1, weather truncation, Easy 1.3x, and 31 cap.
        InfantryCombatDesc sightDesc = s.desc;
        if (s.hasBinoculars && s.binoculars && !s.motion && s.route.empty()) {
            if (s.crouched) sightDesc.crouchSightBase += s.binoSightBonus;
            else sightDesc.sightBase += s.binoSightBonus;
        }
        if (fog_->isElevated(s.cell.x / 2, s.cell.y / 2)) {
            if (s.crouched) sightDesc.crouchSightBase += 2;
            else sightDesc.sightBase += 2;
        }
        const int r = effectiveSightRadius(sightDesc, s.expa, s.crouched, s.owner,
                                           difficulty_, s.movingSightPenalty, visPct);
        revealWithAllies(s.cell.x / 2, s.cell.y / 2, r, s.owner);
    }
    const auto applyWeatherSight = [visPct](int r) {
        return visPct >= 100 ? r : std::max(1, (r * visPct) / 100);
    };
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || !v.hasCrew) continue;
        const bool buttonedUp = !v.hatchOpen || v.suppressionTicks > 0 ||
                                (v.lastCombatTick != 0u && tick_ < v.lastCombatTick + 125u);
        const int marchBonus = buttonedUp ? 0 : v.marchSightBonus;
        const int elevBonus = fog_->isElevated(v.cell.x / 2, v.cell.y / 2) ? 2 : 0;
        const int vr = std::min(31, v.sightTiles + elevBonus + (v.expa >= 16384 ? 2 : (v.expa >= 8192 ? 1 : 0)));
        int effVr = applyWeatherSight(vr);
        if (v.bonusSight != 0) {
            effVr = std::clamp(effVr + (v.bonusSight * visPct) / 100, 1, 31);
        }
        if (difficulty_ <= 1 && v.owner == 0 && !multiplayerSymmetry_) {
            effVr = std::clamp((effVr * 13) / 10, 1, 31);
        }
        if (marchBonus > 0) {
            // Gold 0x1001d3e0..0x1001d46f adds (marchsightbonus * visPct) / 100 after base vehicle sight,
            // re-applies Easy 1.3x for player units, clamps to 31 (0x1f), and takes max(base, result).
            int withMarch = effVr + (marchBonus * visPct) / 100;
            if (difficulty_ <= 1 && v.owner == 0 && !multiplayerSymmetry_) {
                withMarch = (withMarch * 13) / 10;
            }
            effVr = std::max(effVr, std::min(31, withMarch));
        }
        revealWithAllies(v.cell.x / 2, v.cell.y / 2, effVr, v.owner);
    }
    for (const auto& st : sorties_) {
        if (st.kind != assets::AirMissionKind::Recon) continue;
        const SubCell cell = placementCell(static_cast<int>(det::detRoundToInt(st.fx)),
                                           static_cast<int>(det::detRoundToInt(st.fy)), originX_);
        revealWithAllies(cell.x / 2, cell.y / 2, applyWeatherSight(std::clamp(st.sightTiles, 12, 28)), st.owner);
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding) {
            s.spottedMask = 0;
            continue;
        }
        const bool wasVisible = isVisibleToViewer(s.owner, s.spottedMask);
        const std::uint8_t tileFog = fog_->tile(s.cell.x / 2, s.cell.y / 2) & 0x0fu;
        std::uint8_t mask = static_cast<std::uint8_t>(tileFog & ~(1u << s.owner));
        const bool camo = isSoldierCamouflaged(s);
        if (camo && mask != 0u) {
            const auto [su, sv] = soldierWorld(s);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            for (int p = 0; p < 4; ++p) {
                const std::uint8_t pBit = static_cast<std::uint8_t>(1u << p);
                if ((mask & pBit) == 0u || !isHostile(s.owner, p)) continue;
                bool detected = false;
                for (size_t j = 0; j < soldiers_.size() && !detected; ++j) {
                    const auto& d = soldiers_[j];
                    if (d.hp <= 0 || d.embarked || isHostile(p, d.owner)) continue;
                    const auto [du, dv] = d.garrisonBuilding
                        ? tutorialFootWorld(buildings_[*d.garrisonBuilding].x, buildings_[*d.garrisonBuilding].y, originX_)
                        : soldierWorld(d);
                    const int dx = originX_ + du - dv;
                    const int dy = (du + dv) / 2;
                    const double dist = det::detHypot(static_cast<double>(dx - sx), static_cast<double>(dy - sy));
                    if (dist <= 140.0 ||
                        (d.hasBinoculars && d.binoculars && !d.motion && d.route.empty() && dist <= 640.0)) {
                        detected = true;
                    }
                }
                for (size_t j = 0; j < vehicles_.size() && !detected; ++j) {
                    const auto& dv = vehicles_[j];
                    if (!dv.alive || dv.hp <= 0 || isHostile(p, dv.owner)) continue;
                    if (det::detHypot(static_cast<double>(dv.x - sx), static_cast<double>(dv.y - sy)) <= 140.0) {
                        detected = true;
                    }
                }
                if (!detected) mask = static_cast<std::uint8_t>(mask & ~pBit);
            }
        }
        s.spottedMask = mask;
        const bool nowVisible = isVisibleToViewer(s.owner, s.spottedMask);
        if (changed && nowVisible != wasVisible) {
            const auto [u, v] = soldierWorld(s);
            changed->push_back({i, originX_ + u - v, (u + v) / 2, s.direction, 0, false, s.hp, true,
                                s.owner, s.group, false, nowVisible, 0, 0, false,
                                s.expa, s.kills, false, s.atGrenades, false, camo});
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        const bool wasViewerVis = fog_ ? isVisibleToViewer(v.owner, v.spottedMask) : v.visible;
        if (!v.alive || v.hp <= 0) {
            v.spottedMask = 0;
            continue;
        }
        const std::uint8_t tileFog = fog_->tile(v.cell.x / 2, v.cell.y / 2) & 0x0fu;
        v.spottedMask = static_cast<std::uint8_t>(tileFog & ~(1u << v.owner));
        if (v.muzzleRevealTicks > 0u) {
            if (multiplayerSymmetry_) {
                for (int p = 0; p < 4; ++p) {
                    if (isHostile(p, v.owner)) {
                        v.spottedMask = static_cast<std::uint8_t>(v.spottedMask | (1u << p));
                    }
                }
            } else if (isHostile(0, v.owner)) {
                v.spottedMask = static_cast<std::uint8_t>(v.spottedMask | 1u);
            }
        }
        v.visible = isAlliedWithPlayer(v.owner) || ((v.spottedMask & 1u) != 0);
        const bool nowViewerVis = isVisibleToViewer(v.owner, v.spottedMask);
        if (emitVehiclePoses && nowViewerVis != wasViewerVis) {
            emitVehiclePose(i);
        }
    }
    fogDirty_ = false;
    fogSnapshotDirty_ = true;
}

std::optional<std::vector<std::uint8_t>> TutorialUnits::takeFogSnapshot() {
    if (!fog_ || !fogSnapshotDirty_) return std::nullopt;
    fogSnapshotDirty_ = false;
    const auto raw = fog_->raw();
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

int TutorialUnits::Soldier::movementPriority() const {
    // 0x10021cb0, standing Tutorial I player infantry: movespeed 0.5 * 256, then >> 3.
    if (hp <= 0 || !goal || (!motion && needsRoute && planning.backoff)) return 0;
    return 16;
}

void TutorialUnits::refreshRoutes() {
    // The retail path scheduler stamps lower-priority footprints before searching
    // the next bucket. Equally ranked soldiers can follow one another's routes.
    if (std::ranges::none_of(soldiers_, [](const auto& s) {
        return s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.goal && s.needsRoute && !s.planning.backoff;
    })) return;
    std::map<int, std::vector<size_t>> requests;
    std::vector<int> priorities;
    priorities.reserve(soldiers_.size());
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        const int base = (s.hp > 0 && !s.embarked && !s.garrisonBuilding) ? s.movementPriority() : 0;
        priorities.push_back(base);
        if (s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.goal && s.needsRoute && !s.planning.backoff)
            requests[std::min(255, base + (s.planning.wait >> 4))].push_back(i);
    }
    for (const auto& [priority, ids] : requests) {
        auto shadow = grid_.liveSlots();
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            if (soldiers_[i].hp <= 0 || soldiers_[i].embarked || soldiers_[i].garrisonBuilding || priorities[i] < priority) continue;
            const auto cell = soldiers_[i].cell;
            const unsigned slot = (cell.x&1) + 2*(cell.y&1);
            shadow[(cell.y/2)*(grid_.width()/2) + cell.x/2] &= ~(1u << slot);
        }
        // Gold prepends requests to each bucket before draining it.
        for (auto id = ids.rbegin(); id != ids.rend(); ++id) {
            auto& s = soldiers_[*id];
            if (sameTile(s.cell, *s.goal)) {
                s.needsRoute = false;
                continue;
            }
            std::optional<std::uint8_t> next;
            if (s.cursor < s.route.size()) next = s.route[s.cursor];
            auto plan = grid_.plan(s.cell, *s.goal, next, shadow, s.planning,
                                   std::uint8_t(infantryRandom(randomState_,255)));
            using Action = InfantryPathGrid::PlanAction;
            s.needsRoute = plan.action == Action::wait;
            if (plan.action == Action::keepQueue) continue;
            s.route = std::move(plan.directions);
            s.cursor = 0;
            if (plan.action == Action::giveUp) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                if (!s.motion) {
                    clearMovingSightPenalty(s);
                }
            }
        }
    }
}

void TutorialUnits::clearMovingSightPenalty(Soldier& s) {
    if (s.movingSightPenalty) {
        s.movingSightPenalty = false;
        fogDirty_ = true;
    }
}

bool TutorialUnits::startNext(Soldier& soldier) {
    if (soldier.hp <= 0 || !soldier.goal || soldier.needsRoute) return false;
    if (soldier.cursor == soldier.route.size()) {
        clearMovingSightPenalty(soldier);
        if (sameTile(soldier.cell, *soldier.goal)) {
            soldier.goal.reset();
            soldier.route.clear();
            soldier.cursor = 0;
        } else {
            soldier.needsRoute = true;
        }
        return false;
    }
    const auto next = grid_.acceptStep(soldier.cell,soldier.route[soldier.cursor]);
    if (!next) {
        if (sameTile(soldier.cell, *soldier.goal)) {
            clearMovingSightPenalty(soldier);
            soldier.goal.reset();
            soldier.route.clear();
            soldier.cursor = 0;
        } else {
            soldier.needsRoute = true;
        }
        return false;
    }
    if (next->crossesTile) {
        ++soldier.cursor;
        // 0x10021b70 always draws this bit on crossing a tile for its visibility update.
        infantryRandom(randomState_, 1);
        fogDirty_ = true;
    }
    grid_.setOccupied(soldier.cell,false);
    grid_.setOccupied(next->cell,true);
    soldier.cell = next->cell;
    soldier.direction = next->direction;
    soldier.motion = InfantryStep{next->cell.x/2, next->cell.y/2,
        unsigned(next->cell.x%2 + 2*(next->cell.y%2)), next->direction};
    if (!soldier.movingSightPenalty) {
        soldier.movingSightPenalty = true;
        fogDirty_ = true;
    }
    if (soldier.binoculars) {
        soldier.binoculars = false;
        fogDirty_ = true;
    }
    return true;
}

std::vector<SoldierPose> TutorialUnits::step() {
    ++tick_;
    std::vector<SoldierPose> changed;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& soldier = soldiers_[i];
        if (soldier.hp <= 0 || soldier.embarked || soldier.garrisonBuilding) continue;
        if (soldier.planning.backoff) --soldier.planning.backoff;
        if (!soldier.motion) {
            if (soldier.autoTarget) continue;
            if (!startNext(soldier)) continue;
        } else {
            soldier.motion->advance(soldier.crouched, randomState_, soldier.stepProgressBase());
            if (!soldier.motion->running && !soldier.autoTarget) startNext(soldier);
        }
        const auto& motion = *soldier.motion;
        const auto [u,v] = motion.position();
        changed.push_back({i,originX_+u-v,(u+v)/2,motion.direction,motion.progress,motion.running,soldier.hp,true,soldier.owner,soldier.group});
        if (!motion.running) {
            soldier.motion.reset();
            if (soldier.autoTarget || soldier.route.empty()) {
                clearMovingSightPenalty(soldier);
            }
            if (soldier.cursor >= soldier.route.size()) {
                soldier.route.clear();
                soldier.cursor = 0;
            }
        }
    }
    // The unit dispatcher 0x10055ff0 performs the pathfinding sweep after the unit updates.
    refreshRoutes();
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.motion || s.goal || !s.route.empty()) continue;
        if (!s.queuedGoals.empty()) {
            const bool keepAttackMove = s.attackMove;
            while (!s.queuedGoals.empty()) {
                const auto [gx, gy] = s.queuedGoals.front();
                s.queuedGoals.pop_front();
                if (orderSoldierInternal(i, gx, gy)) {
                    s.attackMove = keepAttackMove;
                    break;
                }
            }
            if (!s.goal && s.queuedGoals.empty()) s.attackMove = false;
        } else if (!s.pendingActions.empty() && !s.attackGround && !s.disarmMineTarget &&
                   !s.garrisonTarget && !s.boardVehicleTarget && !s.captureVehicleTarget &&
                   !s.attackTarget && !s.attackVehicleTarget && !s.attackBuildingTarget) {
            while (!s.pendingActions.empty()) {
                const auto act = s.pendingActions.front();
                s.pendingActions.pop_front();
                if (executeQueuedSoldierAction(i, act, &changed)) break;
            }
        } else if (s.patrolOrigin && s.patrolDest && !s.autoTarget) {
            s.patrolToDest = !s.patrolToDest;
            const auto nextPt = s.patrolToDest ? *s.patrolDest : *s.patrolOrigin;
            if (orderSoldierInternal(i, nextPt.first, nextPt.second)) {
                s.attackMove = true;
            } else {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.attackMove = false;
            }
        } else if (s.attackMove && !s.autoTarget && !s.escortVehicleTarget && !s.escortSoldierTarget) {
            s.attackMove = false;
        }
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.binoculars && (s.motion || !s.route.empty())) {
            s.binoculars = false;
            fogDirty_ = true;
        }
        if (s.isSniper && s.crouched && s.lastFireTick > 0 && tick_ >= s.lastFireTick &&
            tick_ - s.lastFireTick == 35u) {
            fogDirty_ = true;
        }
        if (s.suppressionTicks > 0) {
            const int decay = (s.hp > 0) ? officerAuraDecay(i) : 1;
            s.suppressionTicks = std::max(0, s.suppressionTicks - decay);
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (v.suppressionTicks > 0) {
            const int decay = (v.alive && v.hp > 0 && v.hasCrew)
                ? officerAuraDecayAt(v.owner, v.fx, v.fy) : 1;
            v.suppressionTicks = std::max(0, v.suppressionTicks - decay);
            if (v.suppressionTicks == 0 && v.marchEnabled && v.hatchOpen) {
                fogDirty_ = true;
                emitVehiclePose(i);
            }
        }
    }
    stepSupportApproach();
    stepEscort();
    stepTowing();
    stepVehicles(changed);
    stepMines(changed);
    stepBoarding(changed);
    stepCaptures(changed);
    stepGarrisons(changed);
    if (fog_) {
        stepCombat(changed);
    } else {
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
            if (s.attackReload > 0) --s.attackReload;
            if (s.firing) {
                if (s.fireAnim == 255) {
                    s.firing = false;
                    s.fireAnim = 0;
                } else {
                    s.fireAnim = static_cast<std::uint8_t>(std::min(255, int(s.fireAnim) + 15));
                }
            }
            if (s.attackVehicleTarget) {
                stepSoldierAttackVehicle(s, i, changed);
            }
        }
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding) {
                s.lastEmittedCrouch = s.crouched;
                continue;
            }
            if (s.crouched != s.lastEmittedCrouch) {
                s.lastEmittedCrouch = s.crouched;
                const auto [su, sv] = soldierWorld(s);
                const bool vis = isVisibleToViewer(s.owner, s.spottedMask);
                pushOrUpdateSoldierPose(changed, {
                    i, originX_ + su - sv, (su + sv) / 2, s.direction, s.stepProgressBase(), s.motion.has_value(),
                    s.hp, true, s.owner, s.group, false, vis, 0, 0, false, s.expa, s.kills,
                    false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo, s.ammo,
                    false, s.suppressionTicks > 0, static_cast<std::int8_t>(s.crouched ? 1 : 0)
                });
            }
        }
    }
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        auto& v = vehicles_[vi];
        if (v.muzzleRevealTicks > 0u) {
            --v.muzzleRevealTicks;
            if (v.muzzleRevealTicks == 0u) fogDirty_ = true;
        }
        if (v.lastCombatTick != 0u && tick_ >= v.lastCombatTick + 125u) {
            v.lastCombatTick = 0u;
            if (v.marchEnabled && v.hatchOpen) {
                fogDirty_ = true;
                emitVehiclePose(vi);
            }
        }
    }
    stepVehicleCombat(changed);
    stepAirSupport(changed);
    if (!evacuatingSoldiers_.empty() || !evacuatingVehicles_.empty()) {
        for (auto it = evacuatingSoldiers_.begin(); it != evacuatingSoldiers_.end();) {
            if (it->index >= soldiers_.size()) { it = evacuatingSoldiers_.erase(it); continue; }
            auto& s = soldiers_[it->index];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding) { it = evacuatingSoldiers_.erase(it); continue; }
            const auto [su, sv] = soldierWorld(s);
            const int px = originX_ + su - sv;
            const int py = (su + sv) / 2;
            const double dx = static_cast<double>(px - it->targetX);
            const double dy = static_cast<double>(py - it->targetY);
            const double dist2 = dx * dx + dy * dy;
            const bool stopped = !s.motion && s.route.empty() && !s.goal;
            if (dist2 <= 96.0 * 96.0 || (stopped && dist2 <= 192.0 * 192.0)) {
                const std::string sid = (it->index < soldierIds_.size() && !soldierIds_[it->index].empty())
                    ? soldierIds_[it->index] : std::string("dguner");
                const int hpPct = std::clamp((s.hp * 100) / std::max(1, s.desc.maxHp), 1, 100);
                const int ammoPct = s.desc.maxAmmo > 0 ? std::clamp((s.ammo * 100) / s.desc.maxAmmo, 0, 100) : 100;
                const int expaPct = std::clamp((s.expa * 100) / 0x8000, 0, 100);
                reserveUnits_.push_back({sid, s.owner, s.group, hpPct, ammoPct, expaPct, 0});
                const SubCell oldCell = s.cell;
                s.hp = 0;
                s.motion.reset();
                clearMovingSightPenalty(s);
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                s.needsRoute = false;
                s.queuedGoals.clear();
                s.pendingActions.clear();
                s.attackTarget.reset();
                s.attackVehicleTarget.reset();
                s.attackBuildingTarget.reset();
                s.autoTarget.reset();
                if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) grid_.setOccupied(oldCell, false);
                fogDirty_ = true;
                pushOrUpdateSoldierPose(changed, {
                    it->index, px, py, s.direction, 0, false, 0, false, s.owner, s.group, false, false
                });
                it = evacuatingSoldiers_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = evacuatingVehicles_.begin(); it != evacuatingVehicles_.end();) {
            if (it->index >= vehicles_.size()) { it = evacuatingVehicles_.erase(it); continue; }
            auto& v = vehicles_[it->index];
            if (!v.alive || v.hp <= 0) { it = evacuatingVehicles_.erase(it); continue; }
            const double dx = static_cast<double>(v.x - it->targetX);
            const double dy = static_cast<double>(v.y - it->targetY);
            const double dist2 = dx * dx + dy * dy;
            const bool stopped = !v.moveTarget && v.waypoints.empty();
            if (dist2 <= 112.0 * 112.0 || (stopped && dist2 <= 200.0 * 200.0)) {
                if (v.towedVehicle) detachTowedGun(it->index);
                const int hpPct = std::clamp((v.hp * 100) / std::max(1, v.maxHp), 1, 100);
                const int ammoPct = v.maxAmmo > 0 ? std::clamp((v.ammo * 100) / v.maxAmmo, 0, 100) : 100;
                const int expaPct = std::clamp((v.expa * 100) / 0x8000, 0, 100);
                reserveUnits_.push_back({v.id, v.owner, v.group, hpPct, ammoPct, expaPct, 0});
                for (std::uint32_t pi : v.passengers) {
                    if (pi >= soldiers_.size()) continue;
                    auto& ps = soldiers_[pi];
                    if (ps.hp <= 0) continue;
                    const std::string pid = (pi < soldierIds_.size() && !soldierIds_[pi].empty())
                        ? soldierIds_[pi] : std::string("dguner");
                    const int pHpPct = std::clamp((ps.hp * 100) / std::max(1, ps.desc.maxHp), 1, 100);
                    const int pAmmoPct = ps.desc.maxAmmo > 0 ? std::clamp((ps.ammo * 100) / ps.desc.maxAmmo, 0, 100) : 100;
                    const int pExpaPct = std::clamp((ps.expa * 100) / 0x8000, 0, 100);
                    reserveUnits_.push_back({pid, ps.owner, ps.group, pHpPct, pAmmoPct, pExpaPct, 0});
                    ps.hp = 0;
                    ps.embarked = false;
                    pushOrUpdateSoldierPose(changed, {
                        pi, v.x, v.y, ps.direction, 0, false, 0, false, ps.owner, ps.group, false, false
                    });
                }
                const SubCell oldCell = v.cell;
                v.passengers.clear();
                v.moveTarget.reset();
                v.waypoints.clear();
                v.queuedOrders.clear();
                v.alive = false;
                v.hp = 0;
                v.visible = false;
                if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) grid_.setOccupied(oldCell, false);
                fogDirty_ = true;
                emitVehiclePose(it->index);
                it = evacuatingVehicles_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (fog_ && fogDirty_) refreshSpotting(&changed);
    if (tick_ % 25 == 0) {
        stepMedicHeal(changed);
        stepSupplyAndRepair(changed);
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        const bool nowCamo = s.isSniper && isSoldierCamouflaged(s);
        const bool nowSup = s.hp > 0 && s.isSuppressed();
        if (nowCamo != s.wasCamouflaged || nowSup != s.wasSuppressed) {
            s.wasCamouflaged = nowCamo;
            s.wasSuppressed = nowSup;
            const auto [su, sv] = soldierWorld(s);
            const bool vis = s.hp > 0 && !s.embarked && !s.garrisonBuilding &&
                isVisibleToViewer(s.owner, s.spottedMask);
            pushOrUpdateSoldierPose(changed, {
                i, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false, s.hp, s.hp > 0,
                s.owner, s.group, false, vis, 0, 0, s.garrisonBuilding.has_value(),
                s.expa, s.kills, false, s.atGrenades, false, nowCamo, s.mineAmmo, s.ammo, false,
                nowSup
            });
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        const bool nowSup = v.alive && v.hp > 0 && v.isSuppressed();
        if (nowSup != v.wasSuppressed) {
            v.wasSuppressed = nowSup;
            emitVehiclePose(i);
        }
    }
    for (auto& pose : changed) {
        if (pose.index < soldiers_.size()) {
            auto& s = soldiers_[pose.index];
            pose.expa = s.expa;
            pose.kills = s.kills;
            pose.atGrenades = s.atGrenades;
            pose.camouflaged = isSoldierCamouflaged(s);
            pose.mineAmmo = s.mineAmmo;
            pose.ammo = s.ammo;
            pose.suppressed = s.hp > 0 && s.isSuppressed();
            s.wasCamouflaged = pose.camouflaged;
            s.wasSuppressed = pose.suppressed;
        }
    }
    return changed;
}

unsigned TutorialUnits::countInZone(std::span<const std::uint8_t> zones, unsigned zone, int owner) const {
    const std::size_t tileW = std::size_t(grid_.width() / 2), tileH = std::size_t(grid_.height() / 2);
    if (zones.size() != tileW * tileH) throw std::invalid_argument("Mission location grid size mismatch");
    const auto inside = [&](SubCell cell) {
        if (cell.x < 0 || cell.y < 0) return false;
        const std::size_t tx = std::size_t(cell.x / 2);
        const std::size_t ty = std::size_t(cell.y / 2);
        if (tx >= tileW || ty >= tileH) return false;
        return (zones[ty * tileW + tx] & 63u) == zone;
    };
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) { return s.hp > 0 && s.owner == owner && inside(s.cell); })
                  + std::ranges::count_if(vehicles_, [&](const auto& v) { return v.alive && v.hp > 0 && v.owner == owner && inside(v.cell); }));
}

unsigned TutorialUnits::countGroupInZone(std::span<const std::uint8_t> zones, unsigned zone, unsigned group) const {
    const std::size_t tileW = std::size_t(grid_.width() / 2), tileH = std::size_t(grid_.height() / 2);
    if (zones.size() != tileW * tileH) throw std::invalid_argument("Mission location grid size mismatch");
    const auto inside = [&](SubCell cell) {
        if (cell.x < 0 || cell.y < 0) return false;
        const std::size_t tx = std::size_t(cell.x / 2);
        const std::size_t ty = std::size_t(cell.y / 2);
        if (tx >= tileW || ty >= tileH) return false;
        return (zones[ty * tileW + tx] & 63u) == zone;
    };
    const int g = int(group);
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) { return s.hp > 0 && s.group == g && inside(s.cell); })
                  + std::ranges::count_if(vehicles_, [&](const auto& v) { return v.alive && v.hp > 0 && v.group == g && inside(v.cell); }));
}
}
