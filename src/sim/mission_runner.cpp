#include "sim/mission_runner.hpp"
#include "sim/tutorial_coordinates.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

namespace ss2 {
namespace {
template<class C> consteval bool isLockstepPlayerCommand() {
    return !std::is_same_v<C, cmd::SpawnInfantry> &&
           !std::is_same_v<C, cmd::SpawnVehicle> &&
           !std::is_same_v<C, cmd::EmbarkImmediate> &&
           !std::is_same_v<C, cmd::GroupMove> &&
           !std::is_same_v<C, cmd::AddReserveUnit> &&
           !std::is_same_v<C, cmd::TakeReserveUnits> &&
           !std::is_same_v<C, cmd::LoadSnapshot> &&
           !std::is_same_v<C, cmd::Acknowledge>;
}

std::array<int, 64> extractReinforcementWaveOwners(const std::vector<std::vector<std::uint32_t>>& scripts) {
    std::array<int, 64> owners;
    owners.fill(-1);
    for (const auto& sc : scripts) {
        for (std::size_t k = 5; k < sc.size(); ++k) {
            if (sc[k] != 40u) continue;
            if ((sc[k - 5] & 0x80000000u) == 0u || (sc[k - 4] & 0x80000000u) == 0u) continue;
            const std::uint32_t own = sc[k - 5] & 0x7fffffffu;
            const std::uint32_t wi = sc[k - 4] & 0x7fffffffu;
            if (wi < 64u && own < 4u && owners[wi] < 0) {
                owners[wi] = static_cast<int>(own);
            }
        }
    }
    return owners;
}

cmd::Ids ids(std::span<const std::size_t> indices) { return {indices.begin(), indices.end()}; }
}

template<class C> typename C::Result MissionRunner::submit(const C& c) {
    std::lock_guard lock(mutex_);
    return submitLocked(c);
}
// Logged before applying, even when modal_ makes applyLocked reject the
// command, so replay re-evaluates the same gate at the same tick.
template<class C> typename C::Result MissionRunner::submitLocked(const C& c) {
    if constexpr (isLockstepPlayerCommand<C>()) {
        if (commandSink_ && !inLockstepApply_) {
            commandSink_(cmd::Command{c});
            if constexpr (std::is_same_v<C, cmd::CallAirSupport>) {
                const auto slots = units_.airSupportFor(units_.viewerOwner());
                const auto k = static_cast<std::size_t>(c.kind);
                return k < slots.size() && slots[k].ready > 0;
            } else if constexpr (std::is_same_v<C, cmd::TogglePause>) {
                return paused_;
            } else if constexpr (std::is_same_v<typename C::Result, std::optional<bool>>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<typename C::Result, std::optional<size_t>>) {
                return size_t{1};
            } else if constexpr (std::is_same_v<typename C::Result, size_t>) {
                return size_t{1};
            } else if constexpr (std::is_same_v<typename C::Result, bool>) {
                return true;
            } else {
                return typename C::Result{};
            }
        }
    }
    if (recording_) recording_->push_back({units_.tick_, c});
    return applyLocked(c);
}
void MissionRunner::apply(const cmd::Command& command) {
    std::visit([this](const auto& c) { (void)submit(c); }, command);
}
void MissionRunner::applyAs(int actorPlayer, const cmd::Command& command) {
    std::lock_guard lock(mutex_);
    TutorialUnits::ScopedCommandActor actorGuard(units_, actorPlayer);
    const bool prevApply = inLockstepApply_;
    inLockstepApply_ = true;
    std::visit([this](const auto& c) { (void)submitLocked(c); }, command);
    inLockstepApply_ = prevApply;
}
void MissionRunner::startRecording() {
    std::lock_guard lock(mutex_);
    recording_.emplace();
}
std::vector<cmd::RecordedCommand> MissionRunner::takeRecording() {
    std::lock_guard lock(mutex_);
    auto log = std::move(recording_).value_or(std::vector<cmd::RecordedCommand>{});
    recording_.reset();
    return log;
}

MissionRunner::MissionRunner(assets::TutorialData data, unsigned delayMs, TutorialUnits units, bool startRecordingNow,
                             std::map<unsigned, std::vector<assets::ReinforcementUnit>> initialSavedPools)
    : MissionRunner(ManualClock{}, std::move(data), std::move(units), std::move(initialSavedPools)) {
    if (startRecordingNow) recording_.emplace();
    worker_ = std::jthread([this, delayMs](std::stop_token stop) { run(stop, delayMs); });
}
MissionRunner::MissionRunner(ManualClock, assets::TutorialData data, TutorialUnits units,
                             std::map<unsigned, std::vector<assets::ReinforcementUnit>> initialSavedPools)
    : units_(std::move(units)),
      spawnedReinforcementWaves_{},
      reinforcementWaveOwner_(extractReinforcementWaveOwners(data.scripts)),
      savedUnitPools_(std::move(initialSavedPools)),
      tutorial_(data, makeQueriesLocked(data.zones, data.reinforcements, data.markers)) {
    units_.setWeather(data.weather);
    fog_ = units_.takeFogSnapshot();
    collect();
    collectVehiclePoses();
}
void MissionRunner::stopWorker() {
    worker_.request_stop();
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
MissionRunner::~MissionRunner() {
    stopWorker();
}

void MissionRunner::collect() {
    for (const auto e : tutorial_.takeEvents()) {
        if (e.kind == MissionEvent::Dialog) modal_ = true;
        if (e.kind == MissionEvent::Reinforcement) {
            const auto* wave = reinforcementWaveLocked(e.index);
            if (!wave || wave->empty()) continue;
            if (e.index < 64u && e.owner < 4u) {
                reinforcementWaveOwner_[e.index] = static_cast<int>(e.owner);
            }
            if (e.delaySec > 0) {
                scheduledReinforcements_.push_back({e, tutorial_.tick() + e.delaySec * 25u});
                continue;
            }
            if (e.index < 64u) spawnedReinforcementWaves_[e.index] = true;
        }
        if (e.kind == MissionEvent::GroupOrder) {
            if ((e.gate & (1u << 22)) != 0u) {
                units_.setGroupHoldFire(e.index, true, true);
            }
            const auto& locs = tutorial_.data().locations;
            if (e.destZone < locs.size()) {
                const auto [gx, gy] = locs[e.destZone];
                (void)units_.orderGroupMove(e.index, gx, gy, true);
            }
            continue;
        }
        if (e.kind == MissionEvent::FogReveal) {
            const auto& marks = tutorial_.data().markers;
            if (e.index < marks.size()) {
                const auto [mx, my] = marks[e.index];
                units_.revealFogAt(mx, my, int(e.destZone), int(e.owner));
                collectVehiclePoses();
                if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
            }
            continue;
        }
        if (e.kind == MissionEvent::PlayerTeam) {
            // Gold OP(17) stores nextCampaignMissionId = packed >> 8, subIndex = packed & 0xff.
            nextMissionSignal_ = {e.owner, e.gate};
            continue;
        }
        if (e.kind == MissionEvent::AirSupportAddPlanes) {
            units_.addAirSupportPlanes(int(e.owner), static_cast<assets::AirMissionKind>(e.index), int(e.destZone));
            continue;
        }
        if (e.kind == MissionEvent::AirSupportAddSorties) {
            units_.addAirSupportSorties(int(e.owner), static_cast<assets::AirMissionKind>(e.index), int(e.destZone));
            continue;
        }
        if (e.kind == MissionEvent::AirStrike) {
            const auto& marks = tutorial_.data().markers;
            if (e.index < marks.size()) {
                const auto [mx, my] = marks[e.index];
                const unsigned cnt = std::min<unsigned>(e.destZone, 16u);
                bool anyLaunched = false;
                for (unsigned k = 0; k < cnt; ++k) {
                    const int offset = (static_cast<int>(k) - static_cast<int>(cnt / 2u)) * 32;
                    if (units_.callAirSupport(static_cast<assets::AirMissionKind>(e.gate), mx + offset, my, int(e.owner))) {
                        anyLaunched = true;
                    }
                }
                if (anyLaunched) collectAirSorties();
            }
            continue;
        }
        if (e.kind == MissionEvent::AirStrikeZone) {
            const auto& locs = tutorial_.data().locations;
            if (e.index < locs.size()) {
                int gx = locs[e.index].first;
                int gy = locs[e.index].second;
                if (gx == 0 && gy == 0) {
                    gx = units_.mapPixelW_ / 2;
                    gy = units_.mapPixelH_ / 2;
                }
                const unsigned cnt = std::min<unsigned>(e.destZone, 16u);
                bool anyLaunched = false;
                for (unsigned k = 0; k < cnt; ++k) {
                    const int offset = (static_cast<int>(k) - static_cast<int>(cnt / 2u)) * 32;
                    if (units_.callAirSupport(static_cast<assets::AirMissionKind>(e.gate), gx + offset, gy, int(e.owner))) {
                        anyLaunched = true;
                    }
                }
                if (anyLaunched) collectAirSorties();
            }
            continue;
        }
        if (e.kind == MissionEvent::DeployReserve) {
            const unsigned hpPct = std::max(1u, e.owner & 0xffu);
            const unsigned ammoPct = (e.owner >> 8) & 0xffu;
            const unsigned expaPct = (e.owner >> 16) & 0xffu;
            auto taken = units_.takeReserveUnits(e.index);
            if (!taken.empty()) {
                std::array<std::vector<assets::ReinforcementUnit>, 4> byOwner;
                for (const auto& r : taken) {
                    const unsigned o = (r.owner >= 0 && r.owner < 4) ? static_cast<unsigned>(r.owner) : 0u;
                    assets::ReinforcementUnit ru;
                    ru.id = r.id;
                    ru.group = r.group;
                    ru.hp = std::clamp((r.hp * static_cast<int>(hpPct)) / 100, 1, 100);
                    ru.ammo = std::clamp((r.ammo * static_cast<int>(ammoPct)) / 100, 0, 100);
                    ru.expa = expaPct > 0u ? std::clamp(static_cast<int>(expaPct), 0, 100) : std::clamp(r.expa, 0, 100);
                    byOwner[o].push_back(std::move(ru));
                }
                for (unsigned o = 0; o < 4u; ++o) {
                    if (byOwner[o].empty()) continue;
                    const unsigned dynIdx = nextDynamicWaveId_++;
                    dynamicWaves_[dynIdx] = std::move(byOwner[o]);
                    MissionEvent spawnEv{MissionEvent::Reinforcement, dynIdx, o, e.gate, e.destZone, e.delaySec};
                    if (e.delaySec > 0u) {
                        scheduledReinforcements_.push_back({spawnEv, tutorial_.tick() + e.delaySec * 25u});
                    } else {
                        pending_.push_back(spawnEv);
                    }
                }
            }
            continue;
        }
        if (e.kind == MissionEvent::UnitPoolClear) {
            savedUnitPools_.erase(e.index);
            continue;
        }
        if (e.kind == MissionEvent::UnitPoolSaveGroup || e.kind == MissionEvent::UnitPoolSavePlayer) {
            auto live = units_.collectLiveUnitsForPool(e.kind == MissionEvent::UnitPoolSaveGroup, e.owner);
            if (!live.empty()) {
                auto& pool = savedUnitPools_[e.index];
                pool.insert(pool.end(), std::make_move_iterator(live.begin()), std::make_move_iterator(live.end()));
            }
            continue;
        }
        if (e.kind == MissionEvent::UnitPoolToWave) {
            const auto it = savedUnitPools_.find(e.owner);
            if (it == savedUnitPools_.end() || it->second.empty()) {
                dynamicWaves_.erase(e.index);
            } else {
                auto built = it->second;
                if (e.gate != 0xffffffffu) {
                    const int newGroup = static_cast<int>(e.gate);
                    for (auto& u : built) {
                        u.group = newGroup;
                        for (auto& p : u.passengers) p.group = newGroup;
                    }
                }
                dynamicWaves_[e.index] = std::move(built);
                if (e.index < 64u) spawnedReinforcementWaves_[e.index] = false;
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupSetStat) {
            std::vector<SoldierPose> changed;
            if (units_.setGroupStat(e.index, e.owner, e.gate, e.destZone, &changed) > 0u) {
                mergeSoldierPoses(changed);
                collectVehiclePoses();
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupEvacuate || e.kind == MissionEvent::ZoneEvacuate) {
            const auto& data = tutorial_.data();
            int gx = 0, gy = 0;
            bool foundGate = false;
            if (e.gate < data.gates.size() && (data.gates[e.gate].first != 0 || data.gates[e.gate].second != 0)) {
                gx = data.gates[e.gate].first;
                gy = data.gates[e.gate].second;
                foundGate = true;
            } else if (e.gate < data.gateTargets.size() &&
                       (data.gateTargets[e.gate].first != 0 || data.gateTargets[e.gate].second != 0)) {
                gx = data.gateTargets[e.gate].first;
                gy = data.gateTargets[e.gate].second;
                foundGate = true;
            } else if (e.gate < data.markers.size() &&
                       (data.markers[e.gate].first != 0 || data.markers[e.gate].second != 0)) {
                gx = data.markers[e.gate].first;
                gy = data.markers[e.gate].second;
                foundGate = true;
            }
            if (foundGate) {
                if (e.kind == MissionEvent::GroupEvacuate) {
                    (void)units_.orderGroupEvacuate(e.index, gx, gy);
                } else {
                    (void)units_.orderZoneEvacuate(data.zones, e.index, gx, gy);
                }
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupAirfieldOrder) {
            const auto& data = tutorial_.data();
            int tx = units_.mapPixelW_ / 2;
            int ty = units_.mapPixelH_ / 2;
            const std::size_t locIdx = static_cast<std::size_t>(e.gate) + 1u;
            if (locIdx < data.locations.size() && (data.locations[locIdx].first != 0 || data.locations[locIdx].second != 0)) {
                tx = data.locations[locIdx].first;
                ty = data.locations[locIdx].second;
            } else if (e.gate < data.markers.size() &&
                       (data.markers[e.gate].first != 0 || data.markers[e.gate].second != 0)) {
                tx = data.markers[e.gate].first;
                ty = data.markers[e.gate].second;
            } else if (e.gate < data.gates.size() && (data.gates[e.gate].first != 0 || data.gates[e.gate].second != 0)) {
                tx = data.gates[e.gate].first;
                ty = data.gates[e.gate].second;
            }
            (void)units_.orderGroupMoveLimited(e.index, e.destZone, tx, ty);
            continue;
        }
        if (e.kind == MissionEvent::GroupTransferOwner) {
            std::vector<SoldierPose> changed;
            if (units_.transferGroupOwner(e.index, int(e.owner), &changed) > 0u) {
                mergeSoldierPoses(changed);
                collectVehiclePoses();
                if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupReassignInZone) {
            std::vector<SoldierPose> changed;
            if (units_.reassignGroupInZone(tutorial_.data().zones, e.destZone, e.index, e.owner, &changed) > 0u) {
                mergeSoldierPoses(changed);
                collectVehiclePoses();
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupBombard) {
            if (e.owner == 1u) {
                const auto& marks = tutorial_.data().markers;
                if (e.destZone < marks.size()) {
                    const auto [mx, my] = marks[e.destZone];
                    (void)units_.orderGroupAttackGround(e.index, mx, my, true);
                }
            } else {
                const auto& locs = tutorial_.data().locations;
                if (e.destZone < locs.size()) {
                    const auto [gx, gy] = locs[e.destZone];
                    (void)units_.orderGroupAttackGround(e.index, gx, gy, true);
                }
            }
            continue;
        }
        if (e.kind == MissionEvent::MultiplayerWave ||
            e.kind == MissionEvent::MultiplayerZeppelinWave ||
            e.kind == MissionEvent::MultiplayerZeppelinPlanes ||
            e.kind == MissionEvent::MultiplayerZeppelinSorties) {
            if (mpSkirmish_) handleMpScriptEventLocked(e);
            else deferredMpScriptEvents_.push_back(e);
            continue;
        }
        pending_.push_back(e);
    }
}

void MissionRunner::configureMultiplayerSkirmish(const MultiplayerSkirmishConfig& cfg) {
    std::lock_guard lock(mutex_);
    MpSkirmishState mp{};
    mp.config = cfg;
    mp.config.playerCount = std::clamp(cfg.playerCount, 2, 4);
    for (int s = 0; s < 4; ++s) {
        if (mp.config.playerNations[s] > 4u) mp.config.playerNations[s] = 0u;
        units_.setPlayerTeam(static_cast<unsigned>(s), mp.config.playerTeams[s]);
    }
    std::vector<unsigned> activeGates;
    const auto& gates = tutorial_.data().gates;
    for (unsigned gi = 0; gi < gates.size(); ++gi) {
        if (gates[gi].first != 0 || gates[gi].second != 0) {
            activeGates.push_back(gi);
        }
    }
    for (int s = 0; s < 4; ++s) {
        mp.assignedGates[s] = activeGates.empty()
            ? static_cast<unsigned>(s)
            : activeGates[static_cast<std::size_t>(s) % activeGates.size()];
    }
    mpSkirmish_ = std::move(mp);
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    auto deferred = std::exchange(deferredMpScriptEvents_, {});
    for (const auto& ev : deferred) {
        handleMpScriptEventLocked(ev);
    }
}

void MissionRunner::handleMpScriptEventLocked(const MissionEvent& e) {
    if (!mpSkirmish_) return;
    auto& mp = *mpSkirmish_;
    const auto& waves = tutorial_.data().reinforcements;
    if (e.kind == MissionEvent::MultiplayerWave) {
        for (int s = 0; s < mp.config.playerCount; ++s) {
            if ((mp.resignedSeatsMask & (1u << s)) != 0u) continue;
            const unsigned waveIdx = unpackMpNationWave(e.owner, e.destZone, mp.config.playerNations[s]);
            if (waveIdx >= waves.size() || waves[waveIdx].empty()) continue;
            MissionEvent spawnEv{MissionEvent::Reinforcement, waveIdx, static_cast<unsigned>(s),
                                 mp.assignedGates[s], 0u, e.delaySec};
            if (e.delaySec > 0u) {
                scheduledReinforcements_.push_back({spawnEv, tutorial_.tick() + e.delaySec * 25u});
            } else {
                pending_.push_back(spawnEv);
            }
        }
        return;
    }

    auto findOrAddGroup = [&](std::uint32_t mask) -> MpZeppelinGroup* {
        if (mask == 0u) return nullptr;
        for (auto& g : mp.groups) {
            if (g.mask == mask) return &g;
        }
        MpZeppelinGroup g{};
        g.mask = mask;
        mp.groups.push_back(std::move(g));
        return &mp.groups.back();
    };

    if (e.kind == MissionEvent::MultiplayerZeppelinWave) {
        if (auto* g = findOrAddGroup(e.index)) {
            g->waves.push_back({e.owner, e.destZone, e.delaySec});
        }
    } else if (e.kind == MissionEvent::MultiplayerZeppelinPlanes) {
        if (auto* g = findOrAddGroup(e.index); g && e.gate < 5u && e.destZone > 0u) {
            g->airPlanes.push_back({e.gate, e.destZone});
        }
    } else if (e.kind == MissionEvent::MultiplayerZeppelinSorties) {
        if (auto* g = findOrAddGroup(e.index); g && e.gate < 5u && e.destZone > 0u) {
            g->airSorties.push_back({e.gate, e.destZone});
        }
    }
}

void MissionRunner::stepMpSkirmishLocked() {
    if (!mpSkirmish_) return;
    auto& mp = *mpSkirmish_;
    const std::uint32_t tick = tutorial_.tick();

    if ((tick & 0xfu) == 0xeu || tick == 0u) {
        std::uint32_t activeMask = 0u;
        for (const auto& g : mp.groups) activeMask |= g.mask;

        mp.teamPresentMask.fill(0u);
        std::vector<std::tuple<int, int, int>> tileUnits;
        units_.collectUnitTilePositions(tileUnits);
        const auto& markers = tutorial_.data().markers;
        const int originX = units_.originX();

        for (int z = 0; z < 32 && static_cast<std::size_t>(z) < markers.size(); ++z) {
            if ((activeMask & (1u << z)) == 0u) continue;
            const auto [mx, my] = markers[z];
            if (mx == originX && my == 16) continue;
            const auto [zu, zv] = tutorialFootWorld(mx, my, originX);
            const int zx = zu >> 5;
            const int zy = zv >> 5;
            for (const auto& [owner, ux, uy] : tileUnits) {
                if (owner < 0 || owner >= mp.config.playerCount) continue;
                if ((mp.resignedSeatsMask & (1u << owner)) != 0u) continue;
                const int dx = std::abs(ux - zx);
                const int dy = std::abs(uy - zy);
                const int octDist = std::max(dx, dy) + std::min(dx, dy) / 2;
                if (octDist <= mp.config.captureRadiusTiles) {
                    const int team = std::clamp(mp.config.playerTeams[owner], 0, 3);
                    mp.teamPresentMask[team] |= (1u << z);
                }
            }
            int teamsPresent = 0;
            for (int t = 0; t < 4; ++t) {
                if ((mp.teamPresentMask[t] & (1u << z)) != 0u) ++teamsPresent;
            }
            if (teamsPresent > 1) {
                mp.zeppelinContested[z] = true;
                mp.zeppelinHolder[z] = -1;
                for (int t = 0; t < 4; ++t) {
                    mp.teamPresentMask[t] &= ~(1u << z);
                }
            } else {
                mp.zeppelinContested[z] = false;
            }
        }
    }

    const auto& waves = tutorial_.data().reinforcements;
    for (auto& g : mp.groups) {
        int holdingTeam = -1;
        if (g.mask != 0u) {
            for (int t = 0; t < 4; ++t) {
                if ((mp.teamPresentMask[t] & g.mask) == g.mask) {
                    if (holdingTeam >= 0) {
                        holdingTeam = -1;
                        break;
                    }
                    holdingTeam = t;
                }
            }
        }
        if (holdingTeam < 0) {
            g.holdTicks.fill(0u);
            continue;
        }
        for (int t = 0; t < 4; ++t) {
            if (t != holdingTeam) g.holdTicks[t] = 0u;
        }
        if (tick < mp.config.initialWaitTicks) continue;
        if (g.holdTicks[holdingTeam] < mp.config.captureHoldTicks) {
            ++g.holdTicks[holdingTeam];
        }
        if (g.holdTicks[holdingTeam] < mp.config.captureHoldTicks) continue;

        for (int z = 0; z < 32; ++z) {
            if ((g.mask & (1u << z)) != 0u && !mp.zeppelinContested[z]) {
                mp.zeppelinHolder[z] = holdingTeam;
            }
        }

        const bool grantAir = ((g.airGrantedTeamsMask & (1u << holdingTeam)) == 0u);
        if (grantAir) {
            g.airGrantedTeamsMask |= static_cast<std::uint8_t>(1u << holdingTeam);
        }

        for (int s = 0; s < mp.config.playerCount; ++s) {
            if ((mp.resignedSeatsMask & (1u << s)) != 0u) continue;
            if (mp.config.playerTeams[s] != holdingTeam) continue;
            if (grantAir) {
                for (const auto& ap : g.airPlanes) {
                    units_.addAirSupportPlanes(s, static_cast<assets::AirMissionKind>(ap.slot),
                                               static_cast<int>(ap.count));
                }
                for (const auto& as : g.airSorties) {
                    units_.addAirSupportSorties(s, static_cast<assets::AirMissionKind>(as.slot),
                                                static_cast<int>(as.count));
                }
            }
        }

        for (auto& w : g.waves) {
            if (tick < w.nextRepeatTick[holdingTeam]) continue;
            w.nextRepeatTick[holdingTeam] = tick + std::max<std::uint32_t>(25u, w.periodSec * 25u);
            for (int s = 0; s < mp.config.playerCount; ++s) {
                if ((mp.resignedSeatsMask & (1u << s)) != 0u) continue;
                if (mp.config.playerTeams[s] != holdingTeam) continue;
                const unsigned waveIdx = unpackMpNationWave(w.packed4, w.jpWave, mp.config.playerNations[s]);
                if (waveIdx < waves.size() && !waves[waveIdx].empty()) {
                    pending_.push_back({MissionEvent::Reinforcement, waveIdx, static_cast<unsigned>(s),
                                        mp.assignedGates[s], 0u, 0u});
                }
            }
        }
    }

    if ((tick & 0xfu) == 0xeu && tick >= 50u) {
        evaluateMpVictoryLocked();
    }
}

void MissionRunner::evaluateMpVictoryLocked() {
    if (!mpSkirmish_ || mpSkirmish_->matchOutcomeTeam != -1) return;
    auto& mp = *mpSkirmish_;

    auto seatHasPendingSpawn = [&](int s) {
        for (const auto& r : scheduledReinforcements_) {
            if (r.event.owner == static_cast<unsigned>(s)) return true;
        }
        for (const auto& ev : pending_) {
            if (ev.kind == MissionEvent::Reinforcement && ev.owner == static_cast<unsigned>(s)) return true;
        }
        return false;
    };

    std::vector<int> allTeams;
    std::vector<int> survivingTeams;
    for (int t = 0; t < 4; ++t) {
        bool hasSeat = false;
        bool hasLiving = false;
        for (int s = 0; s < mp.config.playerCount; ++s) {
            if (mp.config.playerTeams[s] != t) continue;
            hasSeat = true;
            if ((mp.resignedSeatsMask & (1u << s)) == 0u &&
                (units_.countAliveByPlayer(s) > 0u || seatHasPendingSpawn(s))) {
                hasLiving = true;
                break;
            }
        }
        if (hasSeat) allTeams.push_back(t);
        if (hasLiving) survivingTeams.push_back(t);
    }

    if (allTeams.size() < 2u || survivingTeams.size() > 1u) return;
    mp.matchOutcomeTeam = survivingTeams.empty() ? -2 : survivingTeams.front();
    for (int t : allTeams) {
        if (t != mp.matchOutcomeTeam) mp.defeatedTeamsMask |= static_cast<std::uint8_t>(1u << t);
    }
    const int viewerTeam = (units_.viewerOwner() >= 0 && units_.viewerOwner() < 4)
        ? mp.config.playerTeams[units_.viewerOwner()] : -1;
    const bool won = (mp.matchOutcomeTeam >= 0 && viewerTeam == mp.matchOutcomeTeam);
    pending_.push_back({won ? MissionEvent::Victory : MissionEvent::Defeat, 0u});
}

void MissionRunner::flushScheduledReinforcements() {
    const std::uint32_t tick = tutorial_.tick();
    for (auto it = scheduledReinforcements_.begin(); it != scheduledReinforcements_.end();) {
        if (it->dueTick <= tick) {
            auto ready = it->event;
            ready.delaySec = 0;
            if (ready.index < 64u) spawnedReinforcementWaves_[ready.index] = true;
            pending_.push_back(ready);
            it = scheduledReinforcements_.erase(it);
        } else {
            ++it;
        }
    }
}

void MissionRunner::mergeSoldierPoses(std::span<const SoldierPose> incoming) {
    for (const auto& pose : incoming) {
        const auto old = std::ranges::find(poses_, pose.index, &SoldierPose::index);
        if (old == poses_.end()) {
            poses_.push_back(pose);
            continue;
        }
        const bool wasFired = old->fired;
        const bool wasGrenade = old->threwGrenade;
        const bool wasFlamer = old->firedFlamer;
        const bool wasExplosive = old->firedExplosive;
        const int oldAmmo = old->ammo;
        const std::int8_t oldCrouch = old->crouchedState;
        const int oldTx = old->targetX;
        const int oldTy = old->targetY;
        *old = pose;
        if (wasFired && !pose.fired) {
            old->fired = true;
            old->targetX = oldTx;
            old->targetY = oldTy;
        }
        old->threwGrenade = old->threwGrenade || wasGrenade;
        old->firedFlamer = old->firedFlamer || wasFlamer;
        old->firedExplosive = old->firedExplosive || wasExplosive;
        if (pose.ammo < 0 && oldAmmo >= 0) old->ammo = oldAmmo;
        if (pose.crouchedState < 0 && oldCrouch >= 0) old->crouchedState = oldCrouch;
    }
}

void MissionRunner::collectVehiclePoses() {
    for (const auto& pose : units_.takeVehiclePoses()) {
        const auto old = std::ranges::find(vehiclePoses_, pose.index, &VehiclePose::index);
        if (old == vehiclePoses_.end()) {
            vehiclePoses_.push_back(pose);
            continue;
        }
        const bool wasFired = old->fired;
        const int oldTx = old->targetX;
        const int oldTy = old->targetY;
        const bool oldSec = old->firedSecondary;
        const bool oldHeavy = old->heavyImpact;
        const bool oldHitVeh = old->hitVehicle;
        const bool oldRocket = old->firedRocket;
        const bool oldFlamer = old->firedFlamer;
        const std::uint8_t oldCrater = old->craterScale;
        const bool wasRepaired = old->repaired;
        const bool wasResupplied = old->resupplied;
        const bool wasCaptured = old->captured;
        const bool wasDecrewed = old->decrewed;
        std::string oldSnd = std::move(old->fireSound);
        *old = pose;
        if (wasFired && !pose.fired) {
            old->fired = true;
            old->targetX = oldTx;
            old->targetY = oldTy;
            old->firedSecondary = oldSec;
            old->heavyImpact = oldHeavy;
            old->hitVehicle = oldHitVeh;
            old->firedRocket = oldRocket;
            old->firedFlamer = oldFlamer;
            old->craterScale = oldCrater;
            old->fireSound = std::move(oldSnd);
        }
        old->repaired = old->repaired || wasRepaired;
        old->resupplied = old->resupplied || wasResupplied;
        old->captured = old->captured || wasCaptured;
        old->decrewed = old->decrewed || wasDecrewed;
    }
}

void MissionRunner::collectBuildingUpdates() {
    for (const auto& b : units_.takeBuildingUpdates()) {
        const auto old = std::ranges::find(buildingUpdates_, b.index, &BuildingSnapshot::index);
        if (old == buildingUpdates_.end()) {
            buildingUpdates_.push_back(b);
            continue;
        }
        const bool wasDamaged = old->damagedThisTick;
        const bool wasCollapsed = old->collapsedThisTick;
        *old = b;
        old->damagedThisTick = old->damagedThisTick || wasDamaged;
        old->collapsedThisTick = old->collapsedThisTick || wasCollapsed;
    }
}

void MissionRunner::collectBridgeUpdates() {
    for (const auto& br : units_.takeBridgeUpdates()) {
        const auto old = std::ranges::find(bridgeUpdates_, br.index, &BridgeSnapshot::index);
        if (old == bridgeUpdates_.end()) {
            bridgeUpdates_.push_back(br);
            continue;
        }
        const bool wasDamaged = old->damagedThisTick;
        const bool wasCollapsed = old->collapsedThisTick;
        const bool wasRepaired = old->repairedThisTick;
        *old = br;
        old->damagedThisTick = old->damagedThisTick || wasDamaged;
        old->collapsedThisTick = old->collapsedThisTick || wasCollapsed;
        old->repairedThisTick = old->repairedThisTick || wasRepaired;
    }
}

void MissionRunner::collectAirSorties() {
    for (const auto& pose : units_.takeAirSorties()) {
        const auto old = std::ranges::find(airSorties_, pose.id, &AirSortiePose::id);
        if (old == airSorties_.end()) {
            airSorties_.push_back(pose);
            continue;
        }
        const bool hadAa = old->aaFired;
        const int aaX = old->aaOriginX;
        const int aaY = old->aaOriginY;
        std::string aaSnd = std::move(old->aaSoundGroup);
        const bool wasAlive = old->alive;
        const bool wasActive = old->active;
        *old = pose;
        if (hadAa && !pose.aaFired) {
            old->aaFired = true;
            old->aaOriginX = aaX;
            old->aaOriginY = aaY;
            old->aaSoundGroup = std::move(aaSnd);
        }
        old->alive = wasAlive && pose.alive;
        old->active = wasActive && pose.active;
    }
}

void MissionRunner::run(std::stop_token stop, unsigned delayMs) {
    using namespace std::chrono;
    using Clock = steady_clock;
    const auto start = Clock::now();
    const auto computePeriod = [&] {
        const unsigned pct = static_cast<unsigned>(std::clamp(speedPercent_.load(), 50, 300));
        return milliseconds(std::max(8u, (delayMs * 100u) / pct));
    };
    auto deadline = computePeriod();
    unsigned phase = 0;
    std::unique_lock lock(mutex_);
    try {
        while (!stop.stop_requested()) {
            changed_.wait_until(lock, stop, start + deadline, [] { return false; });
            if (stop.stop_requested()) break;
            const auto period = computePeriod();
            const auto now = duration_cast<milliseconds>(Clock::now() - start);
            // Gold SP accepts one update on a due dispatch. On the fourth phase
            // it resynchronizes to now; the intervening phases average the two times.
            deadline = (phase == 3 ? now : (deadline + now) / 2) + period;
            phase = (phase + 1) % 4;
            stepIfRunningLocked();
        }
    } catch (...) { error_ = std::current_exception(); }
}

void MissionRunner::stepGroupAiLocked(bool fastOnly) {
    const auto& data = tutorial_.data();
    const auto& locs = data.locations;
    const auto& zones = data.zones;
    const auto& groups = tutorial_.groupAi();
    const unsigned tick = simTick_.load(std::memory_order_relaxed);
    std::vector<SoldierPose> aiSoldierPoses;

    for (unsigned g = 0u; g < groups.size(); ++g) {
        const auto& ai = groups[g];
        if (ai.behavior == igroups::kAiNone) continue;
        if (fastOnly &&
            ai.behavior != igroups::kAiRecon &&
            ai.behavior != igroups::kAiKatyaMove &&
            ai.behavior != igroups::kAiFurgTransport) {
            continue;
        }
        if ((ai.aiFlags & igroups::kAifHoldFire) != 0u) {
            units_.setGroupHoldFire(g, true, true);
        }

        const bool hasZone1 = (ai.zone1 > 0u && ai.zone1 < locs.size());
        const bool hasZone2 = (ai.zone2 > 0u && ai.zone2 < locs.size());
        const int z1x = hasZone1 ? locs[ai.zone1].first : 0;
        const int z1y = hasZone1 ? locs[ai.zone1].second : 0;
        const int z2x = hasZone2 ? locs[ai.zone2].first : 0;
        const int z2y = hasZone2 ? locs[ai.zone2].second : 0;
        const unsigned targetGrp = ai.aiFlag0 > 0u ? ai.aiFlag0 : ai.aiFlag1;

        if (ai.behavior == igroups::kAiSoldArt) {
            (void)units_.orderGroupSoldArt(g, ai.aiFlags, zones,
                                           ai.zone1, z1x, z1y, hasZone1,
                                           ai.zone2, z2x, z2y, hasZone2, true);
            continue;
        }
        if (ai.behavior == igroups::kAiRecon) {
            (void)units_.orderGroupRecon(g, ai.aiFlags, zones,
                                         ai.zone1, z1x, z1y, hasZone1,
                                         ai.zone2, z2x, z2y, hasZone2, true);
            continue;
        }
        if (ai.behavior == igroups::kAiKatyaMove) {
            (void)units_.orderGroupKatya(g, ai.aiFlags, zones,
                                         ai.zone1, z1x, z1y, hasZone1,
                                         ai.zone2, z2x, z2y, hasZone2, true);
            continue;
        }
        if (ai.behavior == igroups::kAiTrainMove) {
            (void)units_.orderGroupTrain(g, ai.aiFlags, zones,
                                         ai.zone1, z1x, z1y, hasZone1,
                                         ai.zone2, z2x, z2y, hasZone2, tick, true);
            continue;
        }
        if (ai.behavior == igroups::kAiFurgTransport) {
            (void)units_.orderGroupTransport(g, ai.aiFlags, targetGrp, zones,
                                             ai.zone1, z1x, z1y, hasZone1,
                                             ai.zone2, z2x, z2y, hasZone2,
                                             &aiSoldierPoses, true);
            continue;
        }

        const bool hasZoneGateFlags =
            (ai.aiFlags & (igroups::kAifZoneHaveOwn | igroups::kAifZoneNoEnemy)) != 0u;
        const auto ownerOpt = hasZoneGateFlags ? units_.groupOwner(g, true) : std::nullopt;
        const auto zoneOpen = [&](unsigned zIdx, int zx, int zy) {
            if (!hasZoneGateFlags || !ownerOpt) return true;
            return units_.checkZoneGate(zones, zIdx, zx, zy, *ownerOpt, ai.aiFlags,
                                        igroups::kAifZoneHaveOwn, igroups::kAifZoneNoEnemy);
        };

        std::uint32_t z = 0u;
        if (hasZone1 && hasZone2 && ai.zone1 != ai.zone2 && (ai.aiFlags & igroups::kAifRndMove) != 0u) {
            const bool preferZ2 = ((tick / 128u) & 1u) != 0u;
            const bool z1Ok = zoneOpen(ai.zone1, z1x, z1y);
            const bool z2Ok = zoneOpen(ai.zone2, z2x, z2y);
            if (!z1Ok) units_.stopGroupUnitsMovingTo(g, z1x, z1y, 224.0, true);
            if (!z2Ok) units_.stopGroupUnitsMovingTo(g, z2x, z2y, 224.0, true);
            if (preferZ2 && z2Ok) z = ai.zone2;
            else if (z1Ok) z = ai.zone1;
            else if (z2Ok) z = ai.zone2;
        } else if (hasZone1) {
            if (zoneOpen(ai.zone1, z1x, z1y)) {
                z = ai.zone1;
            } else {
                units_.stopGroupUnitsMovingTo(g, z1x, z1y, 224.0, true);
                if (hasZone2 && ai.zone2 != ai.zone1 && zoneOpen(ai.zone2, z2x, z2y)) {
                    z = ai.zone2;
                }
            }
        } else if (hasZone2) {
            if (zoneOpen(ai.zone2, z2x, z2y)) {
                z = ai.zone2;
            } else {
                units_.stopGroupUnitsMovingTo(g, z2x, z2y, 224.0, true);
            }
        }
        const bool hasZone = (z > 0u && z < locs.size());
        const int zx = hasZone ? locs[z].first : 0;
        const int zy = hasZone ? locs[z].second : 0;
        if (ai.behavior == igroups::kAiFurgHelp || ai.behavior == igroups::kAiGruzReload) {
            if (targetGrp > 0u || hasZone) {
                (void)units_.orderGroupSupport(g, targetGrp, zx, zy, hasZone, true);
            }
        } else if (ai.behavior == igroups::kAiSoldFollow || ai.behavior == igroups::kAiTankFollow) {
            std::optional<std::pair<int, int>> target;
            if (targetGrp > 0u) target = units_.groupCentroid(targetGrp, true);
            if (!target && hasZone) target = std::pair<int, int>{zx, zy};
            if (target) {
                (void)units_.orderGroupZoneBehavior(g, ai.behavior, ai.aiFlags, target->first, target->second, true);
            }
        } else if (hasZone) {
            (void)units_.orderGroupZoneBehavior(g, ai.behavior, ai.aiFlags, zx, zy, true);
        }
    }

    if (units_.fogDirty_ && units_.fog_) {
        units_.refreshSpotting(&aiSoldierPoses, true);
    }
    if (!aiSoldierPoses.empty()) {
        mergeSoldierPoses(aiSoldierPoses);
    }
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) {
        fog_ = std::move(snap);
    }
}

void MissionRunner::stepIfRunningLocked() {
    if (!modal_ && !paused_) stepTickLocked();
}

void MissionRunner::stepTickLocked() {
    mergeSoldierPoses(units_.step());
    collectVehiclePoses();
    collectBuildingUpdates();
    collectBridgeUpdates();
    collectAirSorties();
    auto newAir = units_.takeAirStrikeEvents();
    if (!newAir.empty()) {
        airEvents_.insert(airEvents_.end(), newAir.begin(), newAir.end());
    }
    simTick_.store(units_.tick_, std::memory_order_relaxed);
    auto newMines = units_.takeMineEvents();
    if (!newMines.empty()) {
        mineEvents_.insert(mineEvents_.end(), newMines.begin(), newMines.end());
    }
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    tutorial_.step(makeQueriesLocked(tutorial_.data().zones, tutorial_.data().reinforcements,
                                    tutorial_.data().markers));
    collect();
    flushScheduledReinforcements();
    if (mpSkirmish_) stepMpSkirmishLocked();
    if (!mpSkirmish_) {
        if (simTick_ == 1u || (simTick_ & 63u) == 19u) {
            stepGroupAiLocked(false);
        } else if ((simTick_ & 15u) == 3u) {
            stepGroupAiLocked(true);
        }
    }
}

void MissionRunner::advanceTicks(unsigned n) {
    std::lock_guard lock(mutex_);
    if (error_) std::rethrow_exception(error_);
    for (unsigned i = 0; i < n; ++i) stepIfRunningLocked();
}

// Each step advances units_.tick_ by exactly one, so stepping until the tick
// matches reproduces the worker iterations that preceded a recorded command.
bool MissionRunner::advanceToTickLocked(std::uint32_t tick) {
    while (units_.tick_ < tick) {
        if (modal_ || paused_) return false;
        stepTickLocked();
    }
    return units_.tick_ == tick;
}

bool MissionRunner::replay(std::span<const cmd::RecordedCommand> log, std::uint32_t untilTick) {
    std::lock_guard lock(mutex_);
    for (const auto& rc : log) {
        if (!advanceToTickLocked(rc.tick)) return false;
        std::visit([this](const auto& c) { (void)submitLocked(c); }, rc.command);
    }
    return advanceToTickLocked(untilTick);
}

std::vector<MissionEvent> MissionRunner::poll() {
    std::lock_guard lock(mutex_);
    if (error_) std::rethrow_exception(error_);
    return std::exchange(pending_, {});
}
std::vector<SoldierPose> MissionRunner::pollPoses() {
    std::lock_guard lock(mutex_);
    return std::exchange(poses_, {});
}
std::vector<VehiclePose> MissionRunner::pollVehiclePoses() {
    std::lock_guard lock(mutex_);
    return std::exchange(vehiclePoses_, {});
}
std::optional<std::vector<std::uint8_t>> MissionRunner::pollFog() {
    std::lock_guard lock(mutex_);
    return std::exchange(fog_, std::nullopt);
}
bool MissionRunner::canMove(std::span<const size_t> indices, int x, int y) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canMoveSelection(indices, x, y);
}
std::optional<size_t> MissionRunner::move(std::span<const size_t> indices, int x, int y, bool append) {
    return submit(cmd::Move{ids(indices), x, y, append});
}
cmd::Move::Result MissionRunner::applyLocked(const cmd::Move& c) {
    if (modal_) return {};
    return units_.orderSelection(c.ids, c.x, c.y, c.append);
}
std::optional<size_t> MissionRunner::attackMove(std::span<const size_t> indices, int x, int y, bool append) {
    return submit(cmd::AttackMove{ids(indices), x, y, append});
}
cmd::AttackMove::Result MissionRunner::applyLocked(const cmd::AttackMove& c) {
    if (modal_) return {};
    return units_.orderAttackMove(c.ids, c.x, c.y, c.append);
}
std::optional<size_t> MissionRunner::patrol(std::span<const size_t> indices, int mapX, int mapY) {
    return submit(cmd::Patrol{ids(indices), mapX, mapY});
}
cmd::Patrol::Result MissionRunner::applyLocked(const cmd::Patrol& c) {
    if (modal_) return {};
    return units_.orderPatrol(c.ids, c.x, c.y);
}
size_t MissionRunner::scatter(std::span<const size_t> indices) {
    return submit(cmd::Scatter{ids(indices)});
}
cmd::Scatter::Result MissionRunner::applyLocked(const cmd::Scatter& c) {
    if (modal_) return 0;
    return units_.orderScatter(c.ids);
}
bool MissionRunner::isValidAttackTarget(size_t targetIndex) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.isValidAttackTarget(targetIndex, units_.viewerOwner());
}
std::optional<size_t> MissionRunner::attack(std::span<const size_t> indices, size_t targetIndex) {
    return submit(cmd::Attack{ids(indices), targetIndex});
}
cmd::Attack::Result MissionRunner::applyLocked(const cmd::Attack& c) {
    if (modal_) return {};
    return units_.orderAttack(c.ids, c.target);
}
size_t MissionRunner::stop(std::span<const size_t> indices) {
    return submit(cmd::Stop{ids(indices)});
}
cmd::Stop::Result MissionRunner::applyLocked(const cmd::Stop& c) {
    if (modal_) return 0;
    return units_.stopSelection(c.ids);
}
std::optional<bool> MissionRunner::toggleHoldFire(std::span<const size_t> indices) {
    return submit(cmd::ToggleHoldFire{ids(indices)});
}
cmd::ToggleHoldFire::Result MissionRunner::applyLocked(const cmd::ToggleHoldFire& c) {
    if (modal_ || c.ids.empty()) return std::nullopt;
    return units_.toggleHoldFire(c.ids);
}
std::optional<bool> MissionRunner::toggleCrouch(std::span<const size_t> indices) {
    return submit(cmd::ToggleCrouch{ids(indices)});
}
cmd::ToggleCrouch::Result MissionRunner::applyLocked(const cmd::ToggleCrouch& c) {
    if (modal_ || c.ids.empty()) return std::nullopt;
    const bool crouched = units_.toggleCrouch(c.ids);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return crouched;
}
std::optional<bool> MissionRunner::toggleBinoculars(std::span<const size_t> indices) {
    return submit(cmd::ToggleBinoculars{ids(indices)});
}
cmd::ToggleBinoculars::Result MissionRunner::applyLocked(const cmd::ToggleBinoculars& c) {
    if (modal_ || c.ids.empty()) return std::nullopt;
    std::vector<SoldierPose> changed;
    const bool active = units_.toggleBinocularsWithPoses(c.ids, &changed);
    mergeSoldierPoses(changed);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return active;
}
size_t MissionRunner::spawnInfantry(const assets::Infantry& unit, int expa) {
    return submit(cmd::SpawnInfantry{unit, expa});
}
cmd::SpawnInfantry::Result MissionRunner::applyLocked(const cmd::SpawnInfantry& c) {
    const size_t index = units_.spawnInfantry(c.unit, c.expa);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return index;
}
std::optional<size_t> MissionRunner::moveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    return submit(cmd::MoveVehicles{ids(indices), mapX, mapY, append});
}
cmd::MoveVehicles::Result MissionRunner::applyLocked(const cmd::MoveVehicles& c) {
    if (modal_) return {};
    return units_.moveVehicles(c.ids, c.x, c.y, c.append);
}
std::optional<size_t> MissionRunner::reverseMoveVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    return submit(cmd::ReverseMoveVehicles{ids(indices), mapX, mapY});
}
cmd::ReverseMoveVehicles::Result MissionRunner::applyLocked(const cmd::ReverseMoveVehicles& c) {
    if (modal_) return {};
    return units_.reverseMoveVehicles(c.ids, c.x, c.y);
}
std::optional<size_t> MissionRunner::faceVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    return submit(cmd::FaceVehicles{ids(indices), mapX, mapY});
}
cmd::FaceVehicles::Result MissionRunner::applyLocked(const cmd::FaceVehicles& c) {
    if (modal_) return {};
    const size_t res = units_.faceVehicles(c.ids, c.x, c.y);
    collectVehiclePoses();
    return res;
}
std::optional<size_t> MissionRunner::attackMoveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    return submit(cmd::AttackMoveVehicles{ids(indices), mapX, mapY, append});
}
cmd::AttackMoveVehicles::Result MissionRunner::applyLocked(const cmd::AttackMoveVehicles& c) {
    if (modal_) return {};
    return units_.attackMoveVehicles(c.ids, c.x, c.y, c.append);
}
std::optional<size_t> MissionRunner::patrolVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    return submit(cmd::PatrolVehicles{ids(indices), mapX, mapY});
}
cmd::PatrolVehicles::Result MissionRunner::applyLocked(const cmd::PatrolVehicles& c) {
    if (modal_) return {};
    return units_.patrolVehicles(c.ids, c.x, c.y);
}
std::optional<size_t> MissionRunner::attackWithVehicles(std::span<const size_t> indices, bool targetIsVehicle, size_t targetIdx) {
    return submit(cmd::AttackWithVehicles{ids(indices), targetIsVehicle, targetIdx});
}
cmd::AttackWithVehicles::Result MissionRunner::applyLocked(const cmd::AttackWithVehicles& c) {
    if (modal_) return {};
    return units_.attackWithVehicles(c.ids, c.targetIsVehicle, c.target);
}
std::optional<size_t> MissionRunner::attackVehicle(std::span<const size_t> indices, size_t targetVehicleIdx) {
    return submit(cmd::AttackVehicle{ids(indices), targetVehicleIdx});
}
cmd::AttackVehicle::Result MissionRunner::applyLocked(const cmd::AttackVehicle& c) {
    if (modal_) return {};
    return units_.attackVehicle(c.ids, c.targetVehicle);
}
size_t MissionRunner::stopVehicles(std::span<const size_t> indices) {
    return submit(cmd::StopVehicles{ids(indices)});
}
cmd::StopVehicles::Result MissionRunner::applyLocked(const cmd::StopVehicles& c) {
    if (modal_) return 0;
    const size_t stopped = units_.stopVehicles(c.ids);
    collectVehiclePoses();
    return stopped;
}
bool MissionRunner::canSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canSupportVehicle(supporterVehicleIds, targetVehicleIdx);
}
std::optional<size_t> MissionRunner::orderSupportVehicle(std::span<const size_t> supporterVehicleIds,
                                                           size_t targetVehicleIdx) {
    return submit(cmd::SupportVehicle{ids(supporterVehicleIds), targetVehicleIdx});
}
cmd::SupportVehicle::Result MissionRunner::applyLocked(const cmd::SupportVehicle& c) {
    if (modal_) return {};
    const size_t res = units_.orderSupportVehicle(c.supporters, c.targetVehicle);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canSupportBuilding(supporterVehicleIds, buildingIdx);
}
std::optional<size_t> MissionRunner::orderSupportBuilding(std::span<const size_t> supporterVehicleIds,
                                                            size_t buildingIdx) {
    return submit(cmd::SupportBuilding{ids(supporterVehicleIds), buildingIdx});
}
cmd::SupportBuilding::Result MissionRunner::applyLocked(const cmd::SupportBuilding& c) {
    if (modal_) return {};
    const size_t res = units_.orderSupportBuilding(c.supporters, c.building);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canSupportBridge(std::span<const size_t> supporterVehicleIds, size_t bridgeIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canSupportBridge(supporterVehicleIds, bridgeIdx);
}
std::optional<size_t> MissionRunner::orderSupportBridge(std::span<const size_t> supporterVehicleIds,
                                                          size_t bridgeIdx) {
    return submit(cmd::SupportBridge{ids(supporterVehicleIds), bridgeIdx});
}
cmd::SupportBridge::Result MissionRunner::applyLocked(const cmd::SupportBridge& c) {
    if (modal_) return {};
    const size_t res = units_.orderSupportBridge(c.supporters, c.bridge);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canHealSoldier(std::span<const size_t> medicSoldierIds,
                                   std::span<const size_t> ambulanceVehicleIds,
                                   size_t targetSoldierIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canHealSoldier(medicSoldierIds, ambulanceVehicleIds, targetSoldierIdx);
}
std::optional<size_t> MissionRunner::orderHealSoldier(std::span<const size_t> medicSoldierIds,
                                                        std::span<const size_t> ambulanceVehicleIds,
                                                        size_t targetSoldierIdx) {
    return submit(cmd::HealSoldier{ids(medicSoldierIds), ids(ambulanceVehicleIds), targetSoldierIdx});
}
cmd::HealSoldier::Result MissionRunner::applyLocked(const cmd::HealSoldier& c) {
    if (modal_) return {};
    const size_t res = units_.orderHealSoldier(c.medics, c.ambulances, c.targetSoldier);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetVehicleIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canEscortVehicle(soldierIds, vehicleIds, targetVehicleIdx);
}
std::optional<size_t> MissionRunner::orderEscortVehicle(std::span<const size_t> soldierIds,
                                                          std::span<const size_t> vehicleIds,
                                                          size_t targetVehicleIdx) {
    return submit(cmd::EscortVehicle{ids(soldierIds), ids(vehicleIds), targetVehicleIdx});
}
cmd::EscortVehicle::Result MissionRunner::applyLocked(const cmd::EscortVehicle& c) {
    if (modal_) return {};
    const size_t res = units_.orderEscortVehicle(c.soldiers, c.vehicles, c.targetVehicle);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetSoldierIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canEscortSoldier(soldierIds, vehicleIds, targetSoldierIdx);
}
std::optional<size_t> MissionRunner::orderEscortSoldier(std::span<const size_t> soldierIds,
                                                          std::span<const size_t> vehicleIds,
                                                          size_t targetSoldierIdx) {
    return submit(cmd::EscortSoldier{ids(soldierIds), ids(vehicleIds), targetSoldierIdx});
}
cmd::EscortSoldier::Result MissionRunner::applyLocked(const cmd::EscortSoldier& c) {
    if (modal_) return {};
    const size_t res = units_.orderEscortSoldier(c.soldiers, c.vehicles, c.targetSoldier);
    collectVehiclePoses();
    return res;
}
std::optional<bool> MissionRunner::toggleVehicleHoldFire(std::span<const size_t> indices) {
    return submit(cmd::ToggleVehicleHoldFire{ids(indices)});
}
cmd::ToggleVehicleHoldFire::Result MissionRunner::applyLocked(const cmd::ToggleVehicleHoldFire& c) {
    if (modal_ || c.ids.empty()) return std::nullopt;
    return units_.toggleVehicleHoldFire(c.ids);
}
std::optional<bool> MissionRunner::toggleMarchMode(std::span<const size_t> vehicleIds) {
    return submit(cmd::ToggleMarchMode{ids(vehicleIds)});
}
cmd::ToggleMarchMode::Result MissionRunner::applyLocked(const cmd::ToggleMarchMode& c) {
    if (modal_ || c.vehicles.empty()) return std::nullopt;
    const auto toggled = units_.toggleMarchMode(c.vehicles);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return toggled;
}
bool MissionRunner::isValidVehicleAttackTarget(size_t targetIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.isValidVehicleAttackTarget(targetIdx, units_.viewerOwner());
}
bool MissionRunner::canBoardVehicle(size_t vehicleIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canBoardVehicle(vehicleIdx, units_.viewerOwner());
}
std::optional<size_t> MissionRunner::boardVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    return submit(cmd::BoardVehicle{ids(soldierIds), vehicleIdx});
}
cmd::BoardVehicle::Result MissionRunner::applyLocked(const cmd::BoardVehicle& c) {
    if (modal_) return {};
    return units_.orderBoardVehicle(c.soldiers, c.vehicle);
}
size_t MissionRunner::unloadVehicles(std::span<const size_t> vehicleIds) {
    return submit(cmd::UnloadVehicles{ids(vehicleIds)});
}
cmd::UnloadVehicles::Result MissionRunner::applyLocked(const cmd::UnloadVehicles& c) {
    if (modal_) return 0;
    std::vector<SoldierPose> outPoses;
    const size_t count = units_.unloadVehicles(c.vehicles, &outPoses);
    mergeSoldierPoses(outPoses);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return count;
}
bool MissionRunner::canCaptureVehicle(size_t vehicleIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canCaptureVehicle(vehicleIdx, units_.viewerOwner());
}
std::optional<size_t> MissionRunner::captureVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    return submit(cmd::CaptureVehicle{ids(soldierIds), vehicleIdx});
}
cmd::CaptureVehicle::Result MissionRunner::applyLocked(const cmd::CaptureVehicle& c) {
    if (modal_) return {};
    return units_.orderCaptureVehicle(c.soldiers, c.vehicle);
}
std::optional<size_t> MissionRunner::attackGroundVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    return submit(cmd::AttackGroundVehicles{ids(indices), mapX, mapY});
}
cmd::AttackGroundVehicles::Result MissionRunner::applyLocked(const cmd::AttackGroundVehicles& c) {
    if (modal_) return {};
    return units_.orderAttackGroundVehicles(c.ids, c.x, c.y);
}
std::optional<size_t> MissionRunner::attackGroundSoldiers(std::span<const size_t> indices, int mapX, int mapY) {
    return submit(cmd::AttackGroundSoldiers{ids(indices), mapX, mapY});
}
cmd::AttackGroundSoldiers::Result MissionRunner::applyLocked(const cmd::AttackGroundSoldiers& c) {
    if (modal_) return {};
    return units_.orderAttackGroundSoldiers(c.ids, c.x, c.y);
}
size_t MissionRunner::layMine(std::span<const size_t> soldierIds) {
    return submit(cmd::LayMine{ids(soldierIds)});
}
cmd::LayMine::Result MissionRunner::applyLocked(const cmd::LayMine& c) {
    if (modal_) return 0;
    std::vector<SoldierPose> changed;
    const size_t n = units_.orderLayMine(c.soldiers, &changed);
    mergeSoldierPoses(changed);
    auto evs = units_.takeMineEvents();
    if (!evs.empty()) mineEvents_.insert(mineEvents_.end(), evs.begin(), evs.end());
    return n;
}
bool MissionRunner::canDisarmMineAt(std::span<const size_t> soldierIds, int mapX, int mapY) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canDisarmMineAt(soldierIds, mapX, mapY);
}
std::optional<size_t> MissionRunner::orderDisarmMine(std::span<const size_t> soldierIds, int mapX, int mapY) {
    return submit(cmd::DisarmMine{ids(soldierIds), mapX, mapY});
}
cmd::DisarmMine::Result MissionRunner::applyLocked(const cmd::DisarmMine& c) {
    if (modal_) return {};
    return units_.orderDisarmMine(c.soldiers, c.x, c.y);
}
size_t MissionRunner::orderGroupMove(unsigned group, int mapX, int mapY, bool onlyNonPlayer) {
    return submit(cmd::GroupMove{group, mapX, mapY, onlyNonPlayer});
}
cmd::GroupMove::Result MissionRunner::applyLocked(const cmd::GroupMove& c) {
    return units_.orderGroupMove(c.group, c.x, c.y, c.onlyNonPlayer);
}
std::vector<MineEvent> MissionRunner::pollMineEvents() {
    std::lock_guard lock(mutex_);
    return std::exchange(mineEvents_, {});
}
std::vector<VisibleMine> MissionRunner::visibleMines() const {
    std::lock_guard lock(mutex_);
    return units_.visibleMines(units_.viewerOwner());
}
std::vector<IdleUnit> MissionRunner::idlePlayerUnits() const {
    std::lock_guard lock(mutex_);
    return units_.idlePlayerUnits();
}
bool MissionRunner::canTowVehicle(size_t tractorIdx, size_t gunIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canTowVehicle(tractorIdx, gunIdx, units_.viewerOwner());
}
std::optional<size_t> MissionRunner::towVehicle(size_t tractorIdx, size_t gunIdx) {
    return submit(cmd::TowVehicle{tractorIdx, gunIdx});
}
cmd::TowVehicle::Result MissionRunner::applyLocked(const cmd::TowVehicle& c) {
    if (modal_) return {};
    const size_t res = units_.orderTowVehicle(c.tractor, c.gun);
    collectVehiclePoses();
    return res;
}
size_t MissionRunner::unhitchVehicles(std::span<const size_t> vehicleIndices) {
    return submit(cmd::UnhitchVehicles{ids(vehicleIndices)});
}
cmd::UnhitchVehicles::Result MissionRunner::applyLocked(const cmd::UnhitchVehicles& c) {
    if (modal_) return 0;
    const size_t n = units_.unhitchVehicles(c.vehicles);
    collectVehiclePoses();
    return n;
}
bool MissionRunner::canGarrisonBuilding(size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canGarrisonBuilding(buildingIdx, units_.viewerOwner());
}
std::optional<size_t> MissionRunner::garrisonBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    return submit(cmd::GarrisonBuilding{ids(soldierIds), buildingIdx});
}
cmd::GarrisonBuilding::Result MissionRunner::applyLocked(const cmd::GarrisonBuilding& c) {
    if (modal_) return {};
    return units_.orderGarrisonBuilding(c.soldiers, c.building);
}
size_t MissionRunner::evacuateBuilding(size_t buildingIdx) {
    return submit(cmd::EvacuateBuilding{buildingIdx});
}
cmd::EvacuateBuilding::Result MissionRunner::applyLocked(const cmd::EvacuateBuilding& c) {
    if (modal_) return 0;
    std::vector<SoldierPose> outPoses;
    const size_t count = units_.evacuateBuilding(c.building, &outPoses);
    mergeSoldierPoses(outPoses);
    collectBuildingUpdates();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return count;
}
std::optional<bool> MissionRunner::toggleBuildingHoldFire(size_t buildingIdx) {
    return submit(cmd::ToggleBuildingHoldFire{buildingIdx});
}
cmd::ToggleBuildingHoldFire::Result MissionRunner::applyLocked(const cmd::ToggleBuildingHoldFire& c) {
    if (modal_) return {};
    return units_.toggleBuildingHoldFire(c.building);
}
std::optional<bool> MissionRunner::toggleBuildingBinoculars(size_t buildingIdx) {
    return submit(cmd::ToggleBuildingBinoculars{buildingIdx});
}
cmd::ToggleBuildingBinoculars::Result MissionRunner::applyLocked(const cmd::ToggleBuildingBinoculars& c) {
    if (modal_) return {};
    std::vector<SoldierPose> outPoses;
    const auto active = units_.toggleBuildingBinoculars(c.building, &outPoses);
    mergeSoldierPoses(outPoses);
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return active;
}
std::optional<size_t> MissionRunner::orderBuildingAttack(size_t buildingIdx, bool targetIsVehicle, size_t targetIdx) {
    return submit(cmd::BuildingAttack{buildingIdx, targetIsVehicle, targetIdx});
}
cmd::BuildingAttack::Result MissionRunner::applyLocked(const cmd::BuildingAttack& c) {
    if (modal_) return {};
    return units_.orderBuildingAttack(c.building, c.targetIsVehicle, c.target);
}
std::optional<size_t> MissionRunner::orderBuildingAttackBuilding(size_t buildingIdx, size_t targetBuildingIdx) {
    return submit(cmd::BuildingAttackBuilding{buildingIdx, targetBuildingIdx});
}
cmd::BuildingAttackBuilding::Result MissionRunner::applyLocked(const cmd::BuildingAttackBuilding& c) {
    if (modal_) return {};
    return units_.orderBuildingAttackBuilding(c.building, c.targetBuilding);
}
std::optional<size_t> MissionRunner::attackBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    return submit(cmd::AttackBuilding{ids(soldierIds), buildingIdx});
}
cmd::AttackBuilding::Result MissionRunner::applyLocked(const cmd::AttackBuilding& c) {
    if (modal_) return {};
    return units_.orderAttackBuilding(c.soldiers, c.building);
}
std::optional<size_t> MissionRunner::attackBuildingWithVehicles(std::span<const size_t> vehicleIds, size_t buildingIdx) {
    return submit(cmd::AttackBuildingWithVehicles{ids(vehicleIds), buildingIdx});
}
cmd::AttackBuildingWithVehicles::Result MissionRunner::applyLocked(const cmd::AttackBuildingWithVehicles& c) {
    if (modal_) return {};
    return units_.attackBuildingWithVehicles(c.vehicles, c.building);
}
std::vector<BuildingSnapshot> MissionRunner::pollBuildingUpdates() {
    std::lock_guard lock(mutex_);
    return std::exchange(buildingUpdates_, {});
}
std::vector<BridgeSnapshot> MissionRunner::pollBridgeUpdates() {
    std::lock_guard lock(mutex_);
    return std::exchange(bridgeUpdates_, {});
}
std::array<assets::AirSupportSlot, 5> MissionRunner::airSupportFor(int owner) const {
    std::lock_guard lock(mutex_);
    return units_.airSupportFor(owner);
}
std::array<assets::AirSupportSlot, 5> MissionRunner::playerAirSupport() {
    std::lock_guard lock(mutex_);
    return units_.playerAirSupport();
}
bool MissionRunner::callAirSupport(assets::AirMissionKind kind, int targetX, int targetY) {
    return submit(cmd::CallAirSupport{kind, targetX, targetY});
}
cmd::CallAirSupport::Result MissionRunner::applyLocked(const cmd::CallAirSupport& c) {
    if (modal_) return false;
    const bool ok = units_.callAirSupport(c.kind, c.x, c.y, units_.actorOwner());
    if (ok) collectAirSorties();
    return ok;
}
std::vector<AirSortiePose> MissionRunner::pollAirSorties() {
    std::lock_guard lock(mutex_);
    return std::exchange(airSorties_, {});
}
std::vector<AirStrikeEvent> MissionRunner::pollAirStrikeEvents() {
    std::lock_guard lock(mutex_);
    return std::exchange(airEvents_, {});
}
TutorialUnits::Snapshot MissionRunner::takeSnapshot() {
    std::lock_guard lock(mutex_);
    return units_.takeSnapshot();
}
bool MissionRunner::loadSnapshot(const TutorialUnits::Snapshot& snap) {
    return submit(cmd::LoadSnapshot{snap});
}
MissionRunner::FullSnapshot MissionRunner::takeFullSnapshot() {
    std::lock_guard lock(mutex_);
    return FullSnapshot{
        units_.takeSnapshot(),
        units_.takeExtraState(),
        tutorial_.takeState(),
        spawnedReinforcementWaves_,
        reinforcementWaveOwner_,
        scheduledReinforcements_,
        savedUnitPools_,
        dynamicWaves_,
        nextDynamicWaveId_,
        nextMissionSignal_
    };
}
bool MissionRunner::loadFullSnapshot(const FullSnapshot& snap) {
    std::lock_guard lock(mutex_);
    if (snap.tutorial.active.size() != tutorial_.data().scripts.size() ||
        snap.tutorial.airRouteWaypoints.size() > 60u) {
        return false;
    }
    for (const auto& g : snap.tutorial.groupAi) {
        if (g.waypoints.size() > 16u) return false;
        if (g.waypoints.empty() ? g.waypointIndex != 0u : g.waypointIndex >= g.waypoints.size()) return false;
    }
    std::vector<SoldierPose> restoredPoses;
    if (!units_.loadSnapshotWithExtra(snap.units, &snap.unitsExtra, &restoredPoses)) return false;
    if (!tutorial_.loadState(snap.tutorial)) return false;
    poses_ = std::move(restoredPoses);
    vehiclePoses_.clear();
    mineEvents_.clear();
    buildingUpdates_.clear();
    bridgeUpdates_.clear();
    airSorties_.clear();
    airEvents_.clear();
    pending_.clear();
    pendingCrewSpawns_.clear();
    deferredMpScriptEvents_.clear();
    modal_ = false;
    spawnedReinforcementWaves_ = snap.spawnedReinforcementWaves;
    reinforcementWaveOwner_ = snap.reinforcementWaveOwner;
    scheduledReinforcements_ = snap.scheduledReinforcements;
    savedUnitPools_ = snap.savedUnitPools;
    dynamicWaves_ = snap.dynamicWaves;
    nextDynamicWaveId_ = snap.nextDynamicWaveId;
    nextMissionSignal_ = snap.nextMissionSignal;
    collectVehiclePoses();
    collectBuildingUpdates();
    collectBridgeUpdates();
    simTick_.store(units_.tick_, std::memory_order_relaxed);
    if (auto f = units_.takeFogSnapshot()) fog_ = std::move(f);
    changed_.notify_all();
    return true;
}
cmd::LoadSnapshot::Result MissionRunner::applyLocked(const cmd::LoadSnapshot& c) {
    std::vector<SoldierPose> restoredPoses;
    if (!units_.loadSnapshot(c.snapshot, &restoredPoses)) return false;
    poses_ = std::move(restoredPoses);
    vehiclePoses_.clear();
    mineEvents_.clear();
    buildingUpdates_.clear();
    bridgeUpdates_.clear();
    airSorties_.clear();
    airEvents_.clear();
    scheduledReinforcements_.clear();
    pending_.clear();
    modal_ = false;
    collectVehiclePoses();
    collectBuildingUpdates();
    collectBridgeUpdates();
    simTick_.store(units_.tick_, std::memory_order_relaxed);
    if (auto f = units_.takeFogSnapshot()) fog_ = std::move(f);
    return true;
}
void MissionRunner::setSpeedPercent(int percent) {
    speedPercent_.store(std::clamp(percent, 50, 300));
    changed_.notify_all();
}
size_t MissionRunner::spawnVehicle(const assets::VehicleUnit& unit) {
    return submit(cmd::SpawnVehicle{unit});
}
cmd::SpawnVehicle::Result MissionRunner::applyLocked(const cmd::SpawnVehicle& c) {
    const size_t index = units_.spawnVehicle(c.unit);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return index;
}
bool MissionRunner::embarkImmediate(size_t soldierIdx, size_t vehicleIdx) {
    return submit(cmd::EmbarkImmediate{soldierIdx, vehicleIdx});
}
cmd::EmbarkImmediate::Result MissionRunner::applyLocked(const cmd::EmbarkImmediate& c) {
    const bool ok = units_.embarkImmediate(c.soldier, c.vehicle);
    if (ok) {
        collectVehiclePoses();
        if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    }
    return ok;
}
std::vector<TacticalOrderLine> MissionRunner::selectionOrderLines(std::span<const size_t> soldierIds,
                                                                    std::span<const size_t> vehicleIds) {
    std::lock_guard lock(mutex_);
    return units_.selectionOrderLines(soldierIds, vehicleIds);
}
std::vector<VehicleRangePreview> MissionRunner::selectionRangePreviews(std::span<const size_t> vehicleIds,
                                                                         std::span<const size_t> soldierIds) const {
    std::lock_guard lock(mutex_);
    return units_.selectionRangePreviews(vehicleIds, soldierIds);
}
std::vector<assets::Placement> MissionRunner::abandonVehicles(std::span<const size_t> vehicleIds) {
    return submit(cmd::AbandonVehicles{ids(vehicleIds)});
}
cmd::AbandonVehicles::Result MissionRunner::applyLocked(const cmd::AbandonVehicles& c) {
    if (modal_ || c.vehicles.empty()) return {};
    std::vector<SoldierPose> changed;
    auto spawns = units_.abandonVehicles(c.vehicles, &changed);
    if (units_.multiplayerSymmetry() && !spawns.empty()) {
        pendingCrewSpawns_.insert(pendingCrewSpawns_.end(), spawns.begin(), spawns.end());
    }
    mergeSoldierPoses(changed);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return spawns;
}
void MissionRunner::addReserveUnit(const TutorialUnits::ReserveUnit& entry) {
    submit(cmd::AddReserveUnit{entry});
}
cmd::AddReserveUnit::Result MissionRunner::applyLocked(const cmd::AddReserveUnit& c) {
    units_.addReserveUnit(c.entry);
}
std::vector<TutorialUnits::ReserveUnit> MissionRunner::takeReserveUnits(unsigned group, int pool, std::size_t maxCount) {
    return submit(cmd::TakeReserveUnits{group, pool, maxCount});
}
cmd::TakeReserveUnits::Result MissionRunner::applyLocked(const cmd::TakeReserveUnits& c) {
    return units_.takeReserveUnits(c.group, c.pool, c.maxCount);
}
bool MissionRunner::isElevatedMapPoint(int mapX, int mapY) const {
    std::lock_guard lock(mutex_);
    return units_.isElevatedMapPoint(mapX, mapY);
}
void MissionRunner::acknowledge() {
    submit(cmd::Acknowledge{});
}
cmd::Acknowledge::Result MissionRunner::applyLocked(const cmd::Acknowledge&) {
    modal_ = false;
}
bool MissionRunner::togglePause() {
    return submit(cmd::TogglePause{});
}
cmd::TogglePause::Result MissionRunner::applyLocked(const cmd::TogglePause&) {
    paused_ = !paused_;
    return paused_;
}
void MissionRunner::resign(std::uint8_t seat) {
    submit(cmd::Resign{seat});
}
cmd::Resign::Result MissionRunner::applyLocked(const cmd::Resign& c) {
    if (c.seat >= 4u) return;
    if (units_.commandActorActive() && static_cast<int>(c.seat) != units_.actorOwner()) return;
    if (!mpSkirmish_ || c.seat >= static_cast<std::uint8_t>(mpSkirmish_->config.playerCount)) return;
    mpSkirmish_->resignedSeatsMask |= static_cast<std::uint8_t>(1u << c.seat);
    evaluateMpVictoryLocked();
}
std::vector<ZeppelinMarkerStatus> MissionRunner::zeppelinStatuses() const {
    std::lock_guard lock(mutex_);
    std::vector<ZeppelinMarkerStatus> out;
    if (!mpSkirmish_) return out;
    const auto& mp = *mpSkirmish_;
    std::uint32_t activeMask = 0u;
    for (const auto& g : mp.groups) activeMask |= g.mask;
    const auto& markers = tutorial_.data().markers;
    const int originX = units_.originX();
    const std::uint32_t capTicks = std::max<std::uint32_t>(1u, mp.config.captureHoldTicks);
    for (int z = 0; z < 32 && static_cast<std::size_t>(z) < markers.size(); ++z) {
        if ((activeMask & (1u << z)) == 0u) continue;
        const auto [mx, my] = markers[z];
        if (mx == originX && my == 16) continue;
        float progress = 0.0f;
        int capturingTeam = -1;
        for (const auto& g : mp.groups) {
            if ((g.mask & (1u << z)) == 0u) continue;
            for (int t = 0; t < 4; ++t) {
                if (g.holdTicks[t] > 0u) {
                    const float p = std::clamp(static_cast<float>(g.holdTicks[t]) / static_cast<float>(capTicks),
                                               0.0f, 1.0f);
                    if (p > progress) {
                        progress = p;
                        capturingTeam = t;
                    }
                }
            }
        }
        out.push_back({static_cast<std::uint8_t>(z), mx, my, mp.zeppelinHolder[z], mp.zeppelinContested[z],
                       progress, capturingTeam});
    }
    return out;
}
int MissionRunner::assignedGateForSeat(int seat) const {
    std::lock_guard lock(mutex_);
    if (!mpSkirmish_ || seat < 0 || seat >= 4) return std::max(0, seat);
    return static_cast<int>(mpSkirmish_->assignedGates[seat]);
}
bool MissionRunner::mpSkirmishConfigured() const {
    std::lock_guard lock(mutex_);
    return mpSkirmish_.has_value();
}
bool MissionRunner::isPaused() const {
    std::lock_guard lock(mutex_);
    return paused_;
}
void MissionRunner::setViewerOwner(int owner) {
    std::lock_guard lock(mutex_);
    units_.setViewerOwner(owner);
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
}
int MissionRunner::viewerOwner() const {
    std::lock_guard lock(mutex_);
    return units_.viewerOwner();
}
void MissionRunner::setMultiplayerSymmetry(bool enabled) {
    std::lock_guard lock(mutex_);
    units_.setMultiplayerSymmetry(enabled);
}
bool MissionRunner::multiplayerSymmetry() const {
    std::lock_guard lock(mutex_);
    return units_.multiplayerSymmetry();
}
void MissionRunner::setCommandSink(std::function<void(cmd::Command)> sink) {
    std::lock_guard lock(mutex_);
    commandSink_ = std::move(sink);
}
std::vector<assets::Placement> MissionRunner::pollCrewSpawns() {
    std::lock_guard lock(mutex_);
    return std::exchange(pendingCrewSpawns_, {});
}
std::optional<std::pair<std::uint32_t, std::uint32_t>> MissionRunner::nextMissionSignal() const {
    std::lock_guard lock(mutex_);
    return nextMissionSignal_;
}
const std::vector<assets::ReinforcementUnit>* MissionRunner::reinforcementWaveLocked(unsigned waveIdx) const {
    if (const auto it = dynamicWaves_.find(waveIdx); it != dynamicWaves_.end()) {
        return &it->second;
    }
    const auto& staticWaves = tutorial_.data().reinforcements;
    if (waveIdx < staticWaves.size()) return &staticWaves[waveIdx];
    return nullptr;
}
std::vector<assets::ReinforcementUnit> MissionRunner::reinforcementWave(unsigned waveIdx) const {
    std::lock_guard lock(mutex_);
    const auto* w = reinforcementWaveLocked(waveIdx);
    return w ? *w : std::vector<assets::ReinforcementUnit>{};
}
std::map<unsigned, std::vector<assets::ReinforcementUnit>> MissionRunner::savedUnitPools() const {
    std::lock_guard lock(mutex_);
    return savedUnitPools_;
}
unsigned MissionRunner::countPendingReinforcementsByGroup(
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group) const {
    unsigned total = 0;
    for (std::size_t wi = 0; wi < waves.size(); ++wi) {
        if (spawnedReinforcementWaves_[wi]) continue;
        const auto dynIt = dynamicWaves_.find(static_cast<unsigned>(wi));
        const auto& waveVec = (dynIt != dynamicWaves_.end()) ? dynIt->second : waves[wi];
        for (const auto& u : waveVec) {
            if (u.group >= 0 && static_cast<unsigned>(u.group) == group) {
                ++total;
            }
            for (const auto& pax : u.passengers) {
                if (pax.group >= 0 && static_cast<unsigned>(pax.group) == group) {
                    ++total;
                }
            }
        }
    }
    return total;
}
unsigned MissionRunner::countPendingReinforcementsByGroupTeam(
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group, int team) const {
    unsigned total = 0;
    for (std::size_t wi = 0; wi < waves.size(); ++wi) {
        if (spawnedReinforcementWaves_[wi]) continue;
        const int owner = reinforcementWaveOwner_[wi];
        if (owner < 0 || units_.playerTeam(owner) != team) continue;
        const auto dynIt = dynamicWaves_.find(static_cast<unsigned>(wi));
        const auto& waveVec = (dynIt != dynamicWaves_.end()) ? dynIt->second : waves[wi];
        for (const auto& u : waveVec) {
            if (u.group >= 0 && static_cast<unsigned>(u.group) == group) {
                ++total;
            }
            for (const auto& pax : u.passengers) {
                if (pax.group >= 0 && static_cast<unsigned>(pax.group) == group) {
                    ++total;
                }
            }
        }
    }
    return total;
}
unsigned MissionRunner::countPendingReinforcementsByPlayer(
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, int owner) const {
    if (owner < 0 || owner >= 4) return 0u;
    unsigned total = 0;
    for (std::size_t wi = 0; wi < waves.size(); ++wi) {
        if (spawnedReinforcementWaves_[wi]) continue;
        if (reinforcementWaveOwner_[wi] == owner) {
            const auto dynIt = dynamicWaves_.find(static_cast<unsigned>(wi));
            const auto& waveVec = (dynIt != dynamicWaves_.end()) ? dynIt->second : waves[wi];
            for (const auto& u : waveVec) {
                total += 1u + static_cast<unsigned>(u.passengers.size());
            }
        }
    }
    return total;
}
MissionQueries MissionRunner::makeQueriesLocked(
    std::span<const std::uint8_t> zones,
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves,
    std::span<const std::pair<int, int>> markers) const {
    return {
        [this, zones](unsigned zone, unsigned player) {
            return units_.countInZone(zones, zone, int(player));
        },
        [this](unsigned group) {
            return units_.countAliveByGroup(group);
        },
        [this](unsigned player) {
            return units_.countAliveByPlayer(int(player));
        },
        [this, zones](unsigned zone, unsigned group) {
            return units_.countGroupInZone(zones, zone, group);
        },
        [this](unsigned id) -> std::optional<unsigned> {
            if (auto g = units_.lastGroupHitTick(id)) return g;
            return units_.lastHitTick(id);
        },
        [this, &waves](unsigned group, unsigned mask) {
            unsigned total = units_.countByGroupMask(group, mask);
            if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroup(waves, group);
            return total;
        },
        [this, &waves](int owner, unsigned mask) {
            unsigned total = units_.countByPlayerMask(owner, mask);
            if ((mask & 8u) != 0u) total += countPendingReinforcementsByPlayer(waves, owner);
            return total;
        },
        [this, markers](unsigned marker) -> bool {
            if (marker >= markers.size()) return true;
            const auto [mx, my] = markers[marker];
            return units_.isObjectDestroyedAt(mx, my);
        },
        [this, &waves](unsigned group, int team, unsigned mask) {
            unsigned total = units_.countByGroupTeamMask(group, team, mask);
            if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroupTeam(waves, group, team);
            return total;
        },
        [this](unsigned owner, unsigned slot) {
            return units_.countAirPlanes(int(owner), slot);
        },
        [this](unsigned poolIdx) -> unsigned {
            const auto it = savedUnitPools_.find(poolIdx);
            if (it == savedUnitPools_.end()) return 0u;
            unsigned total = 0u;
            for (const auto& u : it->second) {
                total += 1u + static_cast<unsigned>(u.passengers.size());
            }
            return total;
        },
        [this](unsigned group, unsigned slot) {
            return units_.groupAmmoPercent(group, slot);
        },
        [this](unsigned waveIdx) -> bool {
            const auto it = dynamicWaves_.find(waveIdx);
            return it != dynamicWaves_.end() && !it->second.empty();
        }
    };
}
unsigned MissionRunner::countByGroupMask(unsigned group, unsigned mask) const {
    std::lock_guard lock(mutex_);
    unsigned total = units_.countByGroupMask(group, mask);
    if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroup(tutorial_.data().reinforcements, group);
    return total;
}
unsigned MissionRunner::countByGroupTeamMask(unsigned group, int team, unsigned mask) const {
    std::lock_guard lock(mutex_);
    unsigned total = units_.countByGroupTeamMask(group, team, mask);
    if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroupTeam(tutorial_.data().reinforcements, group, team);
    return total;
}
unsigned MissionRunner::countByPlayerMask(int owner, unsigned mask) const {
    std::lock_guard lock(mutex_);
    unsigned total = units_.countByPlayerMask(owner, mask);
    if ((mask & 8u) != 0u) total += countPendingReinforcementsByPlayer(tutorial_.data().reinforcements, owner);
    return total;
}
}
