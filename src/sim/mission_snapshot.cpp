#include "sim/mission_runner.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace ss2 {
namespace {
constexpr std::array<std::uint8_t, 8> kFullMagicV1{'S', 'S', '2', 'F', 'U', 'L', 'L', '1'};
constexpr std::array<std::uint8_t, 8> kFullMagicV2{'S', 'S', '2', 'F', 'U', 'L', 'L', '2'};
constexpr std::array<std::uint8_t, 8> kFullMagicV3{'S', 'S', '2', 'F', 'U', 'L', 'L', '3'};
constexpr std::array<std::uint8_t, 8> kFullMagicV4{'S', 'S', '2', 'F', 'U', 'L', 'L', '4'};
constexpr std::uint32_t kMaxStringBytes = 4096u;

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

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xffu));
    }
}

void appendString(std::vector<std::uint8_t>& out, std::string_view s) {
    const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(s.size(), kMaxStringBytes));
    appendU32(out, len);
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(len));
}

void appendEvent(std::vector<std::uint8_t>& out, const MissionEvent& ev) {
    appendU8(out, static_cast<std::uint8_t>(ev.kind));
    appendU32(out, static_cast<std::uint32_t>(ev.index));
    appendU32(out, static_cast<std::uint32_t>(ev.owner));
    appendU32(out, static_cast<std::uint32_t>(ev.gate));
    appendU32(out, static_cast<std::uint32_t>(ev.destZone));
    appendU32(out, static_cast<std::uint32_t>(ev.delaySec));
}

void appendReinforcementWave(std::vector<std::uint8_t>& out,
                           const std::vector<assets::ReinforcementUnit>& wave) {
    appendU32(out, static_cast<std::uint32_t>(wave.size()));
    for (const auto& u : wave) {
        appendString(out, u.id);
        appendI32(out, static_cast<std::int32_t>(u.group));
        appendI32(out, static_cast<std::int32_t>(u.hp));
        appendI32(out, static_cast<std::int32_t>(u.ammo));
        appendI32(out, static_cast<std::int32_t>(u.expa));
        appendU32(out, static_cast<std::uint32_t>(u.passengers.size()));
        for (const auto& p : u.passengers) {
            appendString(out, p.id);
            appendI32(out, static_cast<std::int32_t>(p.group));
            appendI32(out, static_cast<std::int32_t>(p.hp));
            appendI32(out, static_cast<std::int32_t>(p.ammo));
            appendI32(out, static_cast<std::int32_t>(p.expa));
            appendU8(out, p.isCrew ? 1u : 0u);
        }
    }
}

struct FullReader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos{0};

    std::size_t remaining() const { return bytes.size() - pos; }

    bool readU8(std::uint8_t& out) {
        if (pos >= bytes.size()) return false;
        out = bytes[pos++];
        return true;
    }

    bool readBool(bool& out) {
        std::uint8_t v = 0;
        if (!readU8(v) || v > 1u) return false;
        out = v != 0u;
        return true;
    }

    bool readU32(std::uint32_t& out) {
        if (remaining() < 4u) return false;
        out = static_cast<std::uint32_t>(bytes[pos]) |
              (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
        pos += 4u;
        return true;
    }

    bool readI32(std::int32_t& out) {
        std::uint32_t raw = 0;
        if (!readU32(raw)) return false;
        out = static_cast<std::int32_t>(raw);
        return true;
    }

    bool readU64(std::uint64_t& out) {
        if (remaining() < 8u) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out |= static_cast<std::uint64_t>(bytes[pos + static_cast<std::size_t>(i)]) << (i * 8);
        }
        pos += 8u;
        return true;
    }

    bool readString(std::string& out) {
        std::uint32_t len = 0;
        if (!readU32(len) || len > kMaxStringBytes || len > remaining()) return false;
        out.assign(reinterpret_cast<const char*>(bytes.data() + pos), len);
        pos += len;
        return true;
    }

    bool readEvent(MissionEvent& ev) {
        std::uint8_t k = 0;
        std::uint32_t idx = 0, own = 0, gt = 0, dz = 0, ds = 0;
        if (!readU8(k) || k > static_cast<std::uint8_t>(MissionEvent::GroupAirfieldOrder)) return false;
        if (!readU32(idx) || !readU32(own) || !readU32(gt) || !readU32(dz) || !readU32(ds)) return false;
        ev.kind = static_cast<MissionEvent::Kind>(k);
        ev.index = idx;
        ev.owner = own;
        ev.gate = gt;
        ev.destZone = dz;
        ev.delaySec = ds;
        return true;
    }

    bool readReinforcementWave(std::vector<assets::ReinforcementUnit>& wave) {
        std::uint32_t cnt = 0;
        if (!readU32(cnt) || cnt > remaining() / 24u) return false;
        wave.resize(cnt);
        for (auto& u : wave) {
            std::int32_t grp = 0, hp = 0, ammo = 0, expa = 0;
            std::uint32_t paxCount = 0;
            if (!readString(u.id) || !readI32(grp) || !readI32(hp) || !readI32(ammo) || !readI32(expa) ||
                !readU32(paxCount) || paxCount > remaining() / 21u) {
                return false;
            }
            u.group = grp;
            u.hp = hp;
            u.ammo = ammo;
            u.expa = expa;
            u.passengers.resize(paxCount);
            for (auto& p : u.passengers) {
                std::int32_t pgrp = 0, php = 0, pammo = 0, pexpa = 0;
                bool isCrew = false;
                if (!readString(p.id) || !readI32(pgrp) || !readI32(php) || !readI32(pammo) ||
                    !readI32(pexpa) || !readBool(isCrew)) {
                    return false;
                }
                p.group = pgrp;
                p.hp = php;
                p.ammo = pammo;
                p.expa = pexpa;
                p.isCrew = isCrew;
            }
        }
        return true;
    }
};
} // namespace

std::vector<std::uint8_t> MissionRunner::serializeFullSnapshot(const FullSnapshot& snap) {
    const std::vector<std::uint8_t> unitBytes = TutorialUnits::serializeSnapshot(snap.units);
    std::vector<std::uint8_t> out;
    out.reserve(unitBytes.size() + 4096u);
    out.insert(out.end(), kFullMagicV4.begin(), kFullMagicV4.end());

    // 1. Core TutorialUnits snapshot.
    appendU32(out, static_cast<std::uint32_t>(unitBytes.size()));
    out.insert(out.end(), unitBytes.begin(), unitBytes.end());

    // 2. TutorialUnits::ExtraState.
    const auto& ex = snap.unitsExtra;
    appendU32(out, static_cast<std::uint32_t>(ex.soldierIds.size()));
    for (const auto& s : ex.soldierIds) appendString(out, s);
    appendU32(out, static_cast<std::uint32_t>(ex.vehicleIds.size()));
    for (const auto& s : ex.vehicleIds) appendString(out, s);
    appendU32(out, static_cast<std::uint32_t>(ex.vehicleExtras.size()));
    for (const auto& ve : ex.vehicleExtras) {
        appendI32(out, static_cast<std::int32_t>(ve.suppressionTicks));
        appendU8(out, ve.hatchOpen ? 1u : 0u);
    }
    appendU32(out, ex.randomState);
    appendU32(out, ex.combatRandomState);
    appendU8(out, static_cast<std::uint8_t>(ex.weather));
    for (const auto t : ex.lastHitTick) appendI32(out, t);
    for (const auto t : ex.lastGroupHitTick) appendI32(out, t);
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) appendI32(out, ex.airPlanes[p][k]);
    }
    appendU32(out, ex.nextSortieId);
    appendU32(out, static_cast<std::uint32_t>(ex.reserveUnits.size()));
    for (const auto& r : ex.reserveUnits) {
        appendString(out, r.id);
        appendI32(out, r.owner);
        appendI32(out, r.group);
        appendI32(out, r.hp);
        appendI32(out, r.ammo);
        appendI32(out, r.expa);
        appendI32(out, r.pool);
    }
    appendU32(out, static_cast<std::uint32_t>(ex.evacuatingSoldiers.size()));
    for (const auto& eo : ex.evacuatingSoldiers) {
        appendU64(out, static_cast<std::uint64_t>(eo.index));
        appendI32(out, eo.targetX);
        appendI32(out, eo.targetY);
    }
    appendU32(out, static_cast<std::uint32_t>(ex.evacuatingVehicles.size()));
    for (const auto& eo : ex.evacuatingVehicles) {
        appendU64(out, static_cast<std::uint64_t>(eo.index));
        appendI32(out, eo.targetX);
        appendI32(out, eo.targetY);
    }

    // 3. Tutorial::VmState.
    const auto& vm = snap.tutorial;
    appendU32(out, vm.tick);
    appendI32(out, vm.countdown);
    appendU32(out, static_cast<std::uint32_t>(vm.active.size()));
    for (const bool a : vm.active) appendU8(out, a ? 1u : 0u);
    for (const auto t : vm.timers) appendU32(out, t);
    for (const auto p : vm.timerPeriods) appendU32(out, p);
    for (const auto v : vm.variables) appendU32(out, v);
    for (const auto& g : vm.groupAi) {
        appendU32(out, g.behavior);
        appendU32(out, g.zone1);
        appendU32(out, g.zone2);
        appendU32(out, g.aiFlag0);
        appendU32(out, g.aiFlag1);
        appendU32(out, static_cast<std::uint32_t>(g.waypoints.size()));
        for (const auto& [lo, hi] : g.waypoints) {
            appendU32(out, lo);
            appendU32(out, hi);
        }
        appendU32(out, g.waypointIndex);
    }
    appendU32(out, static_cast<std::uint32_t>(vm.events.size()));
    for (const auto& ev : vm.events) appendEvent(out, ev);
    appendU32(out, static_cast<std::uint32_t>(vm.airRouteWaypoints.size()));
    for (const auto& [m, f] : vm.airRouteWaypoints) {
        appendU32(out, m);
        appendU32(out, f);
    }

    // 4. MissionRunner persistent state.
    for (const bool w : snap.spawnedReinforcementWaves) appendU8(out, w ? 1u : 0u);
    for (const int o : snap.reinforcementWaveOwner) appendI32(out, static_cast<std::int32_t>(o));
    appendU32(out, static_cast<std::uint32_t>(snap.scheduledReinforcements.size()));
    for (const auto& sr : snap.scheduledReinforcements) {
        appendEvent(out, sr.event);
        appendU32(out, sr.dueTick);
    }
    appendU32(out, static_cast<std::uint32_t>(snap.savedUnitPools.size()));
    for (const auto& [k, wave] : snap.savedUnitPools) {
        appendU32(out, static_cast<std::uint32_t>(k));
        appendReinforcementWave(out, wave);
    }
    appendU32(out, static_cast<std::uint32_t>(snap.dynamicWaves.size()));
    for (const auto& [k, wave] : snap.dynamicWaves) {
        appendU32(out, static_cast<std::uint32_t>(k));
        appendReinforcementWave(out, wave);
    }
    appendU32(out, static_cast<std::uint32_t>(snap.nextDynamicWaveId));
    appendU8(out, snap.nextMissionSignal ? 1u : 0u);
    if (snap.nextMissionSignal) {
        appendU32(out, snap.nextMissionSignal->first);
        appendU32(out, snap.nextMissionSignal->second);
    }

    // 5. SS2FULL2 extension: destructible bridge states.
    appendU32(out, static_cast<std::uint32_t>(ex.bridges.size()));
    for (const auto& bs : ex.bridges) {
        appendI32(out, bs.hp);
        appendI32(out, bs.stage);
        appendU8(out, bs.destroyed ? 1u : 0u);
    }

    // 6. SS2FULL3 extension: packed group aiFlags and auto-reserve timestamps.
    for (const auto& g : vm.groupAi) {
        appendU32(out, g.aiFlags);
        appendU32(out, g.lastReserveTick);
    }

    // 7. SS2FULL4 extension: airborne & collapsing parachute canopies + in-flight aircraft sorties.
    const auto appendF64 = [&](double d) {
        std::uint64_t bits = 0;
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&d), sizeof(double),
                    reinterpret_cast<std::uint8_t*>(&bits));
        appendU64(out, bits);
    };
    appendU32(out, static_cast<std::uint32_t>(ex.parachutes.size()));
    for (const auto& p : ex.parachutes) {
        appendU32(out, p.id);
        appendString(out, p.unitId);
        appendI32(out, p.owner);
        appendI32(out, p.group);
        appendI32(out, p.x);
        appendI32(out, p.y);
        appendI32(out, p.altitude256);
        appendI32(out, p.fallSpeed256);
        appendI32(out, p.collapseTicks);
        appendI32(out, p.hp);
        appendI32(out, p.maxHp);
        appendU8(out, p.isSupplyCrate ? 1u : 0u);
        appendU8(out, p.emittedBoxDust ? 1u : 0u);
    }
    appendU32(out, static_cast<std::uint32_t>(ex.sorties.size()));
    for (const auto& st : ex.sorties) {
        appendU32(out, st.id);
        appendU8(out, static_cast<std::uint8_t>(st.kind));
        appendString(out, st.planeId);
        appendI32(out, st.owner);
        appendF64(st.fx);
        appendF64(st.fy);
        appendF64(st.vx);
        appendF64(st.vy);
        appendI32(out, st.targetX);
        appendI32(out, st.targetY);
        appendU32(out, st.dir32);
        appendI32(out, st.hp);
        appendI32(out, st.maxHp);
        appendI32(out, st.payloadsLeft);
        appendI32(out, st.totalPayloads);
        appendI32(out, st.bombDamage);
        appendI32(out, st.strafeDamage);
        appendI32(out, st.airToAirDamage);
        appendI32(out, st.sightTiles);
        appendI32(out, st.scoreValue);
        appendU8(out, st.isLeafletBomber ? 1u : 0u);
        appendI32(out, st.altitude);
        appendU8(out, st.isRocketAttacker ? 1u : 0u);
        appendU8(out, static_cast<std::uint8_t>(st.strafeHitType));
        appendU32(out, st.reconTick);
    }
    return out;
}

std::optional<MissionRunner::FullSnapshot> MissionRunner::deserializeFullSnapshot(std::span<const std::uint8_t> bytes) {
    int version = 0;
    if (bytes.size() >= kFullMagicV4.size() &&
        std::equal(kFullMagicV4.begin(), kFullMagicV4.end(), bytes.begin())) {
        version = 4;
    } else if (bytes.size() >= kFullMagicV3.size() &&
               std::equal(kFullMagicV3.begin(), kFullMagicV3.end(), bytes.begin())) {
        version = 3;
    } else if (bytes.size() >= kFullMagicV2.size() &&
               std::equal(kFullMagicV2.begin(), kFullMagicV2.end(), bytes.begin())) {
        version = 2;
    } else if (bytes.size() >= kFullMagicV1.size() &&
               std::equal(kFullMagicV1.begin(), kFullMagicV1.end(), bytes.begin())) {
        version = 1;
    } else {
        return std::nullopt;
    }
    FullReader r{bytes, kFullMagicV1.size()};
    FullSnapshot out{};

    // 1. Core TutorialUnits snapshot.
    std::uint32_t unitLen = 0;
    if (!r.readU32(unitLen) || unitLen > r.remaining()) return std::nullopt;
    auto unitSnap = TutorialUnits::deserializeSnapshot(bytes.subspan(r.pos, unitLen));
    if (!unitSnap) return std::nullopt;
    r.pos += unitLen;
    out.units = std::move(*unitSnap);

    // 2. TutorialUnits::ExtraState.
    auto& ex = out.unitsExtra;
    std::uint32_t cnt = 0;
    if (!r.readU32(cnt) || cnt != out.units.soldiers.size() || cnt > r.remaining() / 4u) return std::nullopt;
    ex.soldierIds.resize(cnt);
    for (auto& s : ex.soldierIds) {
        if (!r.readString(s)) return std::nullopt;
    }
    if (!r.readU32(cnt) || cnt != out.units.vehicles.size() || cnt > r.remaining() / 4u) return std::nullopt;
    ex.vehicleIds.resize(cnt);
    for (auto& s : ex.vehicleIds) {
        if (!r.readString(s)) return std::nullopt;
    }
    if (!r.readU32(cnt) || cnt != out.units.vehicles.size() || cnt > r.remaining() / 5u) return std::nullopt;
    ex.vehicleExtras.resize(cnt);
    for (auto& ve : ex.vehicleExtras) {
        std::int32_t sup = 0;
        bool hatch = false;
        if (!r.readI32(sup) || !r.readBool(hatch)) return std::nullopt;
        ve.suppressionTicks = sup;
        ve.hatchOpen = hatch;
    }
    std::uint8_t weatherByte = 0;
    if (!r.readU32(ex.randomState) || !r.readU32(ex.combatRandomState) ||
        !r.readU8(weatherByte) || weatherByte > 3u) {
        return std::nullopt;
    }
    ex.weather = static_cast<assets::WeatherKind>(weatherByte);
    for (auto& t : ex.lastHitTick) {
        if (!r.readI32(t)) return std::nullopt;
    }
    for (auto& t : ex.lastGroupHitTick) {
        if (!r.readI32(t)) return std::nullopt;
    }
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) {
            if (!r.readI32(ex.airPlanes[p][k])) return std::nullopt;
        }
    }
    if (!r.readU32(ex.nextSortieId)) return std::nullopt;
    if (!r.readU32(cnt) || cnt > r.remaining() / 28u) return std::nullopt;
    ex.reserveUnits.resize(cnt);
    for (auto& ru : ex.reserveUnits) {
        if (!r.readString(ru.id) || !r.readI32(ru.owner) || !r.readI32(ru.group) ||
            !r.readI32(ru.hp) || !r.readI32(ru.ammo) || !r.readI32(ru.expa) || !r.readI32(ru.pool)) {
            return std::nullopt;
        }
    }
    if (!r.readU32(cnt) || cnt > r.remaining() / 16u) return std::nullopt;
    ex.evacuatingSoldiers.resize(cnt);
    for (auto& eo : ex.evacuatingSoldiers) {
        std::uint64_t idx = 0;
        if (!r.readU64(idx) || idx >= out.units.soldiers.size() ||
            !r.readI32(eo.targetX) || !r.readI32(eo.targetY)) {
            return std::nullopt;
        }
        eo.index = static_cast<std::size_t>(idx);
    }
    if (!r.readU32(cnt) || cnt > r.remaining() / 16u) return std::nullopt;
    ex.evacuatingVehicles.resize(cnt);
    for (auto& eo : ex.evacuatingVehicles) {
        std::uint64_t idx = 0;
        if (!r.readU64(idx) || idx >= out.units.vehicles.size() ||
            !r.readI32(eo.targetX) || !r.readI32(eo.targetY)) {
            return std::nullopt;
        }
        eo.index = static_cast<std::size_t>(idx);
    }

    // 3. Tutorial::VmState.
    auto& vm = out.tutorial;
    if (!r.readU32(vm.tick) || !r.readI32(vm.countdown)) return std::nullopt;
    if (!r.readU32(cnt) || cnt > r.remaining()) return std::nullopt;
    vm.active.resize(cnt);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        bool a = false;
        if (!r.readBool(a)) return std::nullopt;
        vm.active[i] = a;
    }
    for (auto& t : vm.timers) {
        if (!r.readU32(t)) return std::nullopt;
    }
    for (auto& p : vm.timerPeriods) {
        if (!r.readU32(p)) return std::nullopt;
    }
    for (auto& v : vm.variables) {
        if (!r.readU32(v)) return std::nullopt;
    }
    for (auto& g : vm.groupAi) {
        std::uint32_t wpCount = 0;
        if (!r.readU32(g.behavior) || !r.readU32(g.zone1) || !r.readU32(g.zone2) ||
            !r.readU32(g.aiFlag0) || !r.readU32(g.aiFlag1) ||
            !r.readU32(wpCount) || wpCount > 16u || wpCount > r.remaining() / 8u) {
            return std::nullopt;
        }
        g.waypoints.resize(wpCount);
        for (auto& [lo, hi] : g.waypoints) {
            if (!r.readU32(lo) || !r.readU32(hi)) return std::nullopt;
        }
        if (!r.readU32(g.waypointIndex)) return std::nullopt;
        if (g.waypoints.empty() ? g.waypointIndex != 0u : g.waypointIndex >= g.waypoints.size()) {
            return std::nullopt;
        }
    }
    if (!r.readU32(cnt) || cnt > r.remaining() / 21u) return std::nullopt;
    vm.events.resize(cnt);
    for (auto& ev : vm.events) {
        if (!r.readEvent(ev)) return std::nullopt;
    }
    if (!r.readU32(cnt) || cnt > 60u || cnt > r.remaining() / 8u) return std::nullopt;
    vm.airRouteWaypoints.resize(cnt);
    for (auto& [m, f] : vm.airRouteWaypoints) {
        if (!r.readU32(m) || !r.readU32(f)) return std::nullopt;
    }

    // 4. MissionRunner persistent state.
    for (auto& w : out.spawnedReinforcementWaves) {
        if (!r.readBool(w)) return std::nullopt;
    }
    for (auto& o : out.reinforcementWaveOwner) {
        std::int32_t ov = 0;
        if (!r.readI32(ov)) return std::nullopt;
        o = static_cast<int>(ov);
    }
    if (!r.readU32(cnt) || cnt > r.remaining() / 25u) return std::nullopt;
    out.scheduledReinforcements.resize(cnt);
    for (auto& sr : out.scheduledReinforcements) {
        if (!r.readEvent(sr.event) || !r.readU32(sr.dueTick)) return std::nullopt;
    }
    if (!r.readU32(cnt) || cnt > r.remaining() / 8u) return std::nullopt;
    for (std::uint32_t i = 0; i < cnt; ++i) {
        std::uint32_t key = 0;
        std::vector<assets::ReinforcementUnit> wave;
        if (!r.readU32(key) || !r.readReinforcementWave(wave)) return std::nullopt;
        out.savedUnitPools[key] = std::move(wave);
    }
    if (!r.readU32(cnt) || cnt > r.remaining() / 8u) return std::nullopt;
    for (std::uint32_t i = 0; i < cnt; ++i) {
        std::uint32_t key = 0;
        std::vector<assets::ReinforcementUnit> wave;
        if (!r.readU32(key) || !r.readReinforcementWave(wave)) return std::nullopt;
        out.dynamicWaves[key] = std::move(wave);
    }
    std::uint32_t nextDyn = 64u;
    bool hasSig = false;
    if (!r.readU32(nextDyn) || !r.readBool(hasSig)) return std::nullopt;
    out.nextDynamicWaveId = nextDyn;
    if (hasSig) {
        std::uint32_t a = 0, b = 0;
        if (!r.readU32(a) || !r.readU32(b)) return std::nullopt;
        out.nextMissionSignal = std::pair{a, b};
    }

    if (version >= 2) {
        if (!r.readU32(cnt) || cnt > r.remaining() / 9u) return std::nullopt;
        ex.bridges.resize(cnt);
        for (auto& bs : ex.bridges) {
            std::int32_t hp = 0, stage = 0;
            bool destroyed = false;
            if (!r.readI32(hp) || !r.readI32(stage) || !r.readBool(destroyed) || stage < 0 || stage > 4) {
                return std::nullopt;
            }
            bs.hp = hp;
            bs.stage = stage;
            bs.destroyed = destroyed;
        }
    }
    if (version >= 3) {
        for (auto& g : vm.groupAi) {
            if (!r.readU32(g.aiFlags) || !r.readU32(g.lastReserveTick)) return std::nullopt;
        }
    }
    if (version >= 4) {
        const auto readF64 = [&](double& d) -> bool {
            std::uint64_t bits = 0;
            if (!r.readU64(bits)) return false;
            std::copy_n(reinterpret_cast<const std::uint8_t*>(&bits), sizeof(double),
                        reinterpret_cast<std::uint8_t*>(&d));
            return std::isfinite(d);
        };
        if (!r.readU32(cnt) || cnt > 256u || cnt > r.remaining() / 46u) return std::nullopt;
        ex.parachutes.resize(cnt);
        for (auto& p : ex.parachutes) {
            if (!r.readU32(p.id) || !r.readString(p.unitId) || !r.readI32(p.owner) || !r.readI32(p.group) ||
                !r.readI32(p.x) || !r.readI32(p.y) || !r.readI32(p.altitude256) || !r.readI32(p.fallSpeed256) ||
                !r.readI32(p.collapseTicks) || !r.readI32(p.hp) || !r.readI32(p.maxHp) ||
                !r.readBool(p.isSupplyCrate) || !r.readBool(p.emittedBoxDust) ||
                p.owner < 0 || p.owner >= 4 || p.altitude256 < 0 || p.fallSpeed256 <= 0 ||
                p.collapseTicks < 0 || p.maxHp <= 0 || p.hp < 0 || p.hp > p.maxHp) {
                return std::nullopt;
            }
        }
        if (!r.readU32(cnt) || cnt > 128u || cnt > r.remaining() / 100u) return std::nullopt;
        ex.sorties.resize(cnt);
        for (auto& st : ex.sorties) {
            std::uint8_t kindByte = 0;
            std::uint8_t hitByte = 0;
            if (!r.readU32(st.id) || !r.readU8(kindByte) || kindByte >= 5u || !r.readString(st.planeId) ||
                !r.readI32(st.owner) || !readF64(st.fx) || !readF64(st.fy) || !readF64(st.vx) || !readF64(st.vy) ||
                !r.readI32(st.targetX) || !r.readI32(st.targetY) || !r.readU32(st.dir32) ||
                !r.readI32(st.hp) || !r.readI32(st.maxHp) || !r.readI32(st.payloadsLeft) ||
                !r.readI32(st.totalPayloads) || !r.readI32(st.bombDamage) || !r.readI32(st.strafeDamage) ||
                !r.readI32(st.airToAirDamage) || !r.readI32(st.sightTiles) || !r.readI32(st.scoreValue) ||
                !r.readBool(st.isLeafletBomber) || !r.readI32(st.altitude) || !r.readBool(st.isRocketAttacker) ||
                !r.readU8(hitByte) || hitByte >= static_cast<std::uint8_t>(assets::HitType::Count) ||
                !r.readU32(st.reconTick) ||
                st.owner < 0 || st.owner >= 4 || st.maxHp <= 0 || st.hp <= 0 || st.hp > st.maxHp ||
                st.payloadsLeft < 0 || st.totalPayloads < st.payloadsLeft) {
                return std::nullopt;
            }
            st.kind = static_cast<assets::AirMissionKind>(kindByte);
            st.strafeHitType = static_cast<assets::HitType>(hitByte);
            st.dir32 &= 31u;
        }
    }

    if (r.pos != bytes.size()) return std::nullopt;
    return out;
}
} // namespace ss2
