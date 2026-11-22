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
constexpr std::array<std::uint8_t, 8> kSnapshotMagicV1{'S', 'S', '2', 'S', 'N', 'A', 'P', '1'};
constexpr std::array<std::uint8_t, 8> kSnapshotMagicV2{'S', 'S', '2', 'S', 'N', 'A', 'P', '2'};
constexpr std::array<std::uint8_t, 8> kSnapshotMagicV3{'S', 'S', '2', 'S', 'N', 'A', 'P', '3'};

void appendU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void appendI32(std::vector<std::uint8_t>& out, std::int32_t v) {
    appendU32(out, static_cast<std::uint32_t>(v));
}

struct SnapshotReader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos{0};

    bool readU8(std::uint8_t& out) {
        if (pos >= bytes.size()) return false;
        out = bytes[pos++];
        return true;
    }
    bool readBool(bool& out) {
        std::uint8_t v = 0;
        if (!readU8(v)) return false;
        out = v != 0;
        return true;
    }
    bool readU32(std::uint32_t& out) {
        if (pos + 4 > bytes.size()) return false;
        out = static_cast<std::uint32_t>(bytes[pos]) |
              (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
        pos += 4;
        return true;
    }
    bool readI32(std::int32_t& out) {
        std::uint32_t raw = 0;
        if (!readU32(raw)) return false;
        out = static_cast<std::int32_t>(raw);
        return true;
    }
    bool readBytes(std::vector<std::uint8_t>& out, std::uint32_t len) {
        if (len > bytes.size() - pos) return false;
        out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
        return true;
    }
};
}
TutorialUnits::Snapshot TutorialUnits::takeSnapshot() const {
    Snapshot snap{};
    snap.tick = tick_;
    snap.playerTeams = playerTeams_;
    snap.mines = mines_;
    if (fog_) {
        const auto raw = fog_->raw();
        snap.fog.assign(raw.begin(), raw.end());
    }
    snap.soldiers.reserve(soldiers_.size());
    for (const auto& s : soldiers_) {
        snap.soldiers.push_back(Snapshot::SoldierState{
            s.cell.x, s.cell.y,
            s.hp, s.maxHp, s.ammo, s.expa, s.kills, s.atGrenades, s.mineAmmo,
            s.owner, s.group,
            s.direction,
            s.crouched, s.holdFire, s.binoculars, s.embarked,
            s.garrisonBuilding ? static_cast<std::int32_t>(*s.garrisonBuilding) : -1,
            s.suppressionTicks
        });
    }
    snap.vehicles.reserve(vehicles_.size());
    for (const auto& v : vehicles_) {
        Snapshot::VehicleState vs{};
        vs.x = v.x;
        vs.y = v.y;
        vs.hullDir = v.hullDir;
        vs.turretDir = v.turretDir;
        vs.hp = v.hp;
        vs.maxHp = v.maxHp;
        vs.ammo = v.ammo;
        vs.ammo2 = v.ammo2;
        vs.expa = v.expa;
        vs.kills = v.kills;
        vs.owner = v.owner;
        vs.group = v.group;
        vs.alive = v.alive;
        vs.holdFire = v.holdFire;
        vs.passengers.reserve(v.passengers.size());
        for (size_t pIdx : v.passengers) {
            vs.passengers.push_back(static_cast<std::uint32_t>(pIdx));
        }
        vs.towedVehicle = v.towedVehicle ? static_cast<std::int32_t>(*v.towedVehicle) : -1;
        vs.towedBy = v.towedBy ? static_cast<std::int32_t>(*v.towedBy) : -1;
        vs.hasCrew = v.hasCrew;
        vs.crewHp = v.crewHp;
        snap.vehicles.push_back(std::move(vs));
    }
    snap.buildings.reserve(buildings_.size());
    for (const auto& b : buildings_) {
        Snapshot::BuildingState bs{};
        bs.hp = b.hp;
        bs.owner = b.owner;
        bs.destroyed = b.destroyed;
        bs.occupants.reserve(b.occupants.size());
        for (size_t sIdx : b.occupants) {
            bs.occupants.push_back(static_cast<std::uint32_t>(sIdx));
        }
        snap.buildings.push_back(std::move(bs));
    }
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) {
            snap.airReady[p][k] = airSupport_[p][k].ready;
        }
    }
    return snap;
}

bool TutorialUnits::loadSnapshot(const Snapshot& snap, std::vector<SoldierPose>* outSoldiers) {
    if (snap.soldiers.size() > soldiers_.size() || snap.vehicles.size() > vehicles_.size()) {
        return false;
    }
    if (!snap.buildings.empty() && snap.buildings.size() != buildings_.size()) {
        return false;
    }
    if (fog_ && !snap.fog.empty() && snap.fog.size() != fog_->raw().size()) {
        return false;
    }
    if (!mines_.empty() && !snap.mines.empty() && snap.mines.size() != mines_.size()) {
        return false;
    }
    for (const auto& ss : snap.soldiers) {
        if (ss.garrisonBuilding >= 0 &&
            static_cast<size_t>(ss.garrisonBuilding) >= buildings_.size()) {
            return false;
        }
    }
    for (const auto& vs : snap.vehicles) {
        if (vs.towedVehicle >= 0 && static_cast<size_t>(vs.towedVehicle) >= snap.vehicles.size()) return false;
        if (vs.towedBy >= 0 && static_cast<size_t>(vs.towedBy) >= snap.vehicles.size()) return false;
        for (std::uint32_t p : vs.passengers) {
            if (static_cast<size_t>(p) >= snap.soldiers.size()) return false;
        }
    }
    for (const auto& bs : snap.buildings) {
        for (std::uint32_t occ : bs.occupants) {
            if (static_cast<size_t>(occ) >= snap.soldiers.size()) return false;
        }
    }

    grid_.clearOccupancy();
    projectiles_.clear();
    mineEvents_.clear();
    buildingUpdates_.clear();
    vehiclePoses_.clear();
    sorties_.clear();
    sortiePoses_.clear();
    airEvents_.clear();
    lastHitTick_.fill(std::nullopt);
    lastGroupHitTick_.fill(std::nullopt);
    reserveUnits_.clear();
    tick_ = snap.tick;
    playerTeams_ = snap.playerTeams;
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) {
            airSupport_[p][k].ready = snap.airReady[p][k];
        }
    }
    if (!snap.mines.empty() || mines_.empty()) {
        mines_ = snap.mines;
    } else {
        std::ranges::fill(mines_, std::uint8_t{0});
    }

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        s.route.clear();
        s.cursor = 0;
        s.motion.reset();
        s.movingSightPenalty = false;
        s.goal.reset();
        s.planning = {};
        s.needsRoute = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.attackReload = 0;
        s.attackBurst = 0;
        s.firing = false;
        s.fireAnim = 0;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.attackBuildingTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.attackMove = false;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.patrolToDest = true;
        s.lastReactTick = 0;
        s.lastFireTick = 0;
        s.wasCamouflaged = false;
        s.suppressionTicks = 0;
        s.wasSuppressed = false;
        s.spottedMask = 0;
        if (i >= snap.soldiers.size()) {
            s.hp = 0;
            s.embarked = false;
            s.garrisonBuilding.reset();
            continue;
        }
        const auto& ss = snap.soldiers[i];
        s.cell = {ss.cellX, ss.cellY};
        s.hp = ss.hp;
        s.maxHp = std::max(1, ss.maxHp);
        s.ammo = ss.ammo;
        s.expa = ss.expa;
        s.kills = ss.kills;
        s.atGrenades = ss.atGrenades;
        s.mineAmmo = ss.mineAmmo;
        s.owner = ss.owner;
        s.group = ss.group;
        s.direction = static_cast<std::uint8_t>(ss.direction & 7u);
        s.crouched = ss.crouched;
        s.holdFire = ss.holdFire;
        s.binoculars = ss.binoculars;
        s.embarked = ss.embarked;
        s.garrisonBuilding = ss.garrisonBuilding >= 0
            ? std::optional<size_t>(static_cast<size_t>(ss.garrisonBuilding))
            : std::nullopt;
        s.suppressionTicks = (s.hp > 0 && !s.embarked && !s.garrisonBuilding)
            ? std::clamp(ss.suppressionTicks, 0, 35) : 0;
        s.wasSuppressed = s.hp > 0 && s.isSuppressed();
        if (s.hp > 0 && !s.embarked && !s.garrisonBuilding) {
            grid_.setOccupied(s.cell, true);
        }
    }

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.pendingTowGun.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackGround.reset();
        v.attackTarget.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.wasMoving = false;
        v.fireCooldown = 0;
        v.burstCount1 = 0;
        v.burstCount2 = 0;
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.patrolToDest = true;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.lastReactTick = 0;
        v.aaCooldown = 0;
        v.spottedMask = 0;
        v.muzzleRevealTicks = 0u;
        v.lastCombatTick = 0u;
        v.hatchOpen = v.marchEnabled;
        v.lastPanicTick = 0u;
        v.suppressionTicks = 0;
        v.wasSuppressed = false;
        if (i >= snap.vehicles.size()) {
            v.alive = false;
            v.hp = 0;
            v.visible = false;
            v.passengers.clear();
            v.towedVehicle.reset();
            v.towedBy.reset();
            continue;
        }
        const auto& vs = snap.vehicles[i];
        v.x = vs.x;
        v.y = vs.y;
        v.fx = static_cast<double>(vs.x);
        v.fy = static_cast<double>(vs.y);
        v.cell = placementCell(v.x, v.y, originX_);
        v.hullDir = vs.hullDir & 31u;
        v.turretDir = vs.turretDir & 31u;
        v.hp = vs.hp;
        v.maxHp = std::max(1, vs.maxHp);
        v.ammo = vs.ammo;
        v.ammo2 = vs.ammo2;
        v.expa = vs.expa;
        v.kills = vs.kills;
        v.owner = vs.owner;
        v.group = vs.group;
        v.alive = vs.alive;
        v.holdFire = vs.holdFire;
        v.visible = true;
        v.passengers.clear();
        v.passengers.reserve(vs.passengers.size());
        for (std::uint32_t pIdx : vs.passengers) {
            v.passengers.push_back(static_cast<size_t>(pIdx));
        }
        if (vs.crewHp >= 0) {
            v.hasCrew = vs.hasCrew;
            v.crewHp = v.hasCrew ? std::clamp(vs.crewHp, 0, v.maxCrewHp) : 0;
        } else {
            v.hasCrew = (v.owner != 3) || !v.passengers.empty();
            v.crewHp = v.hasCrew ? v.maxCrewHp : 0;
        }
        v.towedVehicle = vs.towedVehicle >= 0
            ? std::optional<size_t>(static_cast<size_t>(vs.towedVehicle))
            : std::nullopt;
        v.towedBy = vs.towedBy >= 0
            ? std::optional<size_t>(static_cast<size_t>(vs.towedBy))
            : std::nullopt;
        if (v.alive && v.hp > 0) {
            grid_.setOccupied(v.cell, true);
        }
    }

    for (size_t i = 0; i < snap.buildings.size(); ++i) {
        auto& b = buildings_[i];
        const auto& bs = snap.buildings[i];
        b.hp = bs.hp;
        b.owner = bs.owner;
        b.destroyed = bs.destroyed;
        b.occupants.clear();
        b.occupants.reserve(bs.occupants.size());
        for (std::uint32_t sIdx : bs.occupants) {
            b.occupants.push_back(static_cast<size_t>(sIdx));
        }
        emitBuildingUpdate(i, false, false);
    }

    if (fog_) {
        if (!snap.fog.empty()) {
            fog_->clearAll();
            const int tileW = fog_->width();
            const int tileH = fog_->height();
            for (int ty = 0; ty < tileH; ++ty) {
                for (int tx = 0; tx < tileW; ++tx) {
                    const std::uint8_t bits = snap.fog[static_cast<size_t>(ty * tileW + tx)];
                    if (bits != 0) fog_->castRay(tx, ty, 0, 0, bits);
                }
            }
        }
        fogDirty_ = true;
        refreshSpotting(nullptr, false);
    }

    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        emitVehiclePose(vi);
    }
    if (outSoldiers) {
        outSoldiers->clear();
        outSoldiers->reserve(soldiers_.size());
        for (size_t si = 0; si < soldiers_.size(); ++si) {
            const auto& s = soldiers_[si];
            const auto [u, v] = soldierWorld(s);
            const bool isGar = s.garrisonBuilding.has_value();
            const bool alive = s.hp > 0;
            const bool vis = alive && !s.embarked && !isGar && isVisibleToViewer(s.owner, s.spottedMask);
            const bool camo = isSoldierCamouflaged(s);
            const bool sup = alive && s.isSuppressed();
            soldiers_[si].wasCamouflaged = camo;
            soldiers_[si].wasSuppressed = sup;
            outSoldiers->push_back(SoldierPose{
                si, originX_ + u - v, (u + v) / 2, s.direction, 0u, false,
                s.hp, alive, s.owner, s.group, false, vis, 0, 0, isGar,
                s.expa, s.kills, false, s.atGrenades, false, camo, s.mineAmmo, s.ammo, false, sup,
                static_cast<std::int8_t>(s.crouched ? 1 : 0)
            });
        }
    }
    return true;
}

std::vector<std::uint8_t> TutorialUnits::serializeSnapshot(const Snapshot& snap) {
    std::vector<std::uint8_t> out;
    out.reserve(160 + snap.mines.size() + snap.fog.size() +
                snap.soldiers.size() * 56 + snap.vehicles.size() * 73 + snap.buildings.size() * 16);
    out.insert(out.end(), kSnapshotMagicV3.begin(), kSnapshotMagicV3.end());
    appendU32(out, snap.tick);
    for (int team : snap.playerTeams) {
        appendI32(out, static_cast<std::int32_t>(team));
    }
    appendU32(out, static_cast<std::uint32_t>(snap.mines.size()));
    out.insert(out.end(), snap.mines.begin(), snap.mines.end());
    appendU32(out, static_cast<std::uint32_t>(snap.fog.size()));
    out.insert(out.end(), snap.fog.begin(), snap.fog.end());

    appendU32(out, static_cast<std::uint32_t>(snap.soldiers.size()));
    for (const auto& s : snap.soldiers) {
        appendI32(out, s.cellX);
        appendI32(out, s.cellY);
        appendI32(out, s.hp);
        appendI32(out, s.maxHp);
        appendI32(out, s.ammo);
        appendI32(out, s.expa);
        appendI32(out, s.kills);
        appendI32(out, s.atGrenades);
        appendI32(out, s.mineAmmo);
        appendI32(out, s.owner);
        appendI32(out, s.group);
        appendU8(out, s.direction);
        appendU8(out, s.crouched ? 1u : 0u);
        appendU8(out, s.holdFire ? 1u : 0u);
        appendU8(out, s.binoculars ? 1u : 0u);
        appendU8(out, s.embarked ? 1u : 0u);
        appendI32(out, s.garrisonBuilding);
    }

    appendU32(out, static_cast<std::uint32_t>(snap.vehicles.size()));
    for (const auto& v : snap.vehicles) {
        appendI32(out, v.x);
        appendI32(out, v.y);
        appendU32(out, v.hullDir);
        appendU32(out, v.turretDir);
        appendI32(out, v.hp);
        appendI32(out, v.maxHp);
        appendI32(out, v.ammo);
        appendI32(out, v.ammo2);
        appendI32(out, v.expa);
        appendI32(out, v.kills);
        appendI32(out, v.owner);
        appendI32(out, v.group);
        appendU8(out, v.alive ? 1u : 0u);
        appendU8(out, v.holdFire ? 1u : 0u);
        appendI32(out, v.towedVehicle);
        appendI32(out, v.towedBy);
        appendU32(out, static_cast<std::uint32_t>(v.passengers.size()));
        for (std::uint32_t p : v.passengers) {
            appendU32(out, p);
        }
    }

    appendU32(out, static_cast<std::uint32_t>(snap.buildings.size()));
    for (const auto& b : snap.buildings) {
        appendI32(out, b.hp);
        appendI32(out, b.owner);
        appendU8(out, b.destroyed ? 1u : 0u);
        appendU32(out, static_cast<std::uint32_t>(b.occupants.size()));
        for (std::uint32_t occ : b.occupants) {
            appendU32(out, occ);
        }
    }
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) {
            appendI32(out, static_cast<std::int32_t>(snap.airReady[p][k]));
        }
    }
    for (const auto& s : snap.soldiers) {
        appendI32(out, static_cast<std::int32_t>(s.suppressionTicks));
    }
    for (const auto& v : snap.vehicles) {
        appendU8(out, v.hasCrew ? 1u : 0u);
        appendI32(out, static_cast<std::int32_t>(v.crewHp));
    }
    return out;
}

std::optional<TutorialUnits::Snapshot> TutorialUnits::deserializeSnapshot(std::span<const std::uint8_t> bytes) {
    int version = 0;
    if (bytes.size() >= kSnapshotMagicV3.size() &&
        std::equal(kSnapshotMagicV3.begin(), kSnapshotMagicV3.end(), bytes.begin())) {
        version = 3;
    } else if (bytes.size() >= kSnapshotMagicV2.size() &&
               std::equal(kSnapshotMagicV2.begin(), kSnapshotMagicV2.end(), bytes.begin())) {
        version = 2;
    } else if (bytes.size() >= kSnapshotMagicV1.size() &&
               std::equal(kSnapshotMagicV1.begin(), kSnapshotMagicV1.end(), bytes.begin())) {
        version = 1;
    } else {
        return std::nullopt;
    }
    SnapshotReader r{bytes, kSnapshotMagicV1.size()};
    Snapshot snap{};
    std::uint32_t tickU32 = 0;
    if (!r.readU32(tickU32)) return std::nullopt;
    snap.tick = tickU32;
    for (int& team : snap.playerTeams) {
        std::int32_t tv = 0;
        if (!r.readI32(tv)) return std::nullopt;
        team = tv;
    }
    std::uint32_t mineLen = 0;
    if (!r.readU32(mineLen) || !r.readBytes(snap.mines, mineLen)) return std::nullopt;
    std::uint32_t fogLen = 0;
    if (!r.readU32(fogLen) || !r.readBytes(snap.fog, fogLen)) return std::nullopt;

    std::uint32_t soldierCount = 0;
    if (!r.readU32(soldierCount) || soldierCount > (bytes.size() - r.pos) / 53u) return std::nullopt;
    snap.soldiers.resize(soldierCount);
    for (auto& s : snap.soldiers) {
        if (!r.readI32(s.cellX) || !r.readI32(s.cellY) ||
            !r.readI32(s.hp) || !r.readI32(s.maxHp) || !r.readI32(s.ammo) ||
            !r.readI32(s.expa) || !r.readI32(s.kills) || !r.readI32(s.atGrenades) ||
            !r.readI32(s.mineAmmo) || !r.readI32(s.owner) || !r.readI32(s.group) ||
            !r.readU8(s.direction) || !r.readBool(s.crouched) || !r.readBool(s.holdFire) ||
            !r.readBool(s.binoculars) || !r.readBool(s.embarked) || !r.readI32(s.garrisonBuilding)) {
            return std::nullopt;
        }
    }

    std::uint32_t vehicleCount = 0;
    if (!r.readU32(vehicleCount) || vehicleCount > (bytes.size() - r.pos) / 62u) return std::nullopt;
    snap.vehicles.resize(vehicleCount);
    for (auto& v : snap.vehicles) {
        std::uint32_t hull = 0, turret = 0, passCount = 0;
        if (!r.readI32(v.x) || !r.readI32(v.y) ||
            !r.readU32(hull) || !r.readU32(turret) ||
            !r.readI32(v.hp) || !r.readI32(v.maxHp) || !r.readI32(v.ammo) || !r.readI32(v.ammo2) ||
            !r.readI32(v.expa) || !r.readI32(v.kills) || !r.readI32(v.owner) || !r.readI32(v.group) ||
            !r.readBool(v.alive) || !r.readBool(v.holdFire) ||
            !r.readI32(v.towedVehicle) || !r.readI32(v.towedBy) || !r.readU32(passCount)) {
            return std::nullopt;
        }
        v.hullDir = hull;
        v.turretDir = turret;
        if (passCount > (bytes.size() - r.pos) / 4u) return std::nullopt;
        v.passengers.resize(passCount);
        for (auto& p : v.passengers) {
            if (!r.readU32(p)) return std::nullopt;
        }
    }

    std::uint32_t bldCount = 0;
    if (!r.readU32(bldCount) || bldCount > (bytes.size() - r.pos) / 13u) return std::nullopt;
    snap.buildings.resize(bldCount);
    for (auto& b : snap.buildings) {
        std::uint32_t occCount = 0;
        if (!r.readI32(b.hp) || !r.readI32(b.owner) || !r.readBool(b.destroyed) || !r.readU32(occCount)) {
            return std::nullopt;
        }
        if (occCount > (bytes.size() - r.pos) / 4u) return std::nullopt;
        b.occupants.resize(occCount);
        for (auto& occ : b.occupants) {
            if (!r.readU32(occ)) return std::nullopt;
        }
    }

    if (version >= 2) {
        for (std::size_t p = 0; p < 4u; ++p) {
            for (std::size_t k = 0; k < 5u; ++k) {
                std::int32_t rv = 0;
                if (!r.readI32(rv)) return std::nullopt;
                snap.airReady[p][k] = rv;
            }
        }
    }
    if (version >= 3) {
        for (auto& s : snap.soldiers) {
            std::int32_t st = 0;
            if (!r.readI32(st)) return std::nullopt;
            s.suppressionTicks = st;
        }
        for (auto& v : snap.vehicles) {
            std::int32_t ch = 0;
            if (!r.readBool(v.hasCrew) || !r.readI32(ch)) return std::nullopt;
            v.crewHp = ch;
        }
    }

    if (r.pos != bytes.size()) return std::nullopt;
    return snap;
}
}
