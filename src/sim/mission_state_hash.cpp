// Full-state hashes of the mission script VM and of MissionRunner.
//
// When you add a field to Tutorial or MissionRunner, hash it below (or
// document why it is excluded) and update the matching sizeof guard.
#include "sim/mission_runner.hpp"
#include "sim/state_hash.hpp"
#include "sim/tutorial.hpp"
#include <mutex>

namespace ss2 {
static void hashValue(StateHasher& h, const MissionEvent& e) {
    h.all(e.kind, e.index, e.owner, e.gate, e.destZone, e.delaySec);
}

// Completeness guards; see tutorial_units_hash.cpp for why the ones on types
// holding standard containers are limited to libstdc++ on LP64.
static_assert(sizeof(MissionEvent) == 24, "update hashValue(MissionEvent) for the new field");
#if defined(__GLIBCXX__) && defined(__LP64__)
#define SS2_STATE_HASH_SIZE_GUARDS 1
static_assert(sizeof(Tutorial) - sizeof(assets::TutorialData) == 6872,
              "update Tutorial::stateHash for the new member");
#endif

std::uint64_t Tutorial::stateHash() const {
#ifdef SS2_STATE_HASH_SIZE_GUARDS
    static_assert(sizeof(GroupAiState) == 56, "update the GroupAiState hash below for the new field");
#endif
    StateHasher h;
    // Excluded: data_ (scripts, zones, phrases, ...) is the immutable mission input; the VM never writes it.
    // Scripts are re-evaluated from their start on every scan, so active_ is the only per-script cursor.
    h.all(tick_, countdown_, active_, timers_, timerPeriods_, variables_);
    h.each(groupAi_, [&h](const GroupAiState& g) {
        h.all(g.behavior, g.zone1, g.zone2, g.aiFlag0, g.aiFlag1, g.waypoints, g.waypointIndex);
    });
    // Events raised but not yet taken by the runner still drive it on its next collect().
    h.add(events_);
    return h.value();
}

std::uint64_t MissionRunner::stateHash() const {
#ifdef SS2_STATE_HASH_SIZE_GUARDS
    static_assert(sizeof(ScheduledReinforcement) == 28, "update the ScheduledReinforcement hash below");
    static_assert(sizeof(MissionRunner) - sizeof(TutorialUnits) - sizeof(Tutorial) - sizeof(std::mutex) -
                      sizeof(std::condition_variable_any) ==
                      632 + sizeof(std::function<void(cmd::Command)>) + sizeof(std::vector<assets::Placement>) +
                          sizeof(std::vector<MissionEvent>) + sizeof(std::optional<MpSkirmishState>) +
                          sizeof(void*),
                  "update MissionRunner::stateHash for the new member");
    static_assert(sizeof(MultiplayerSkirmishConfig) == 36, "update MpSkirmishState hash for MultiplayerSkirmishConfig");
    static_assert(sizeof(MpZeppelinWave) == 28, "update MpSkirmishState hash for MpZeppelinWave");
    static_assert(sizeof(MpZeppelinAirGrant) == 8, "update MpSkirmishState hash for MpZeppelinAirGrant");
#endif
    std::lock_guard lock(mutex_);
    StateHasher h;
    h.add(units_.stateHash());
    h.all(spawnedReinforcementWaves_, reinforcementWaveOwner_);
    h.add(tutorial_.stateHash());
    // Excluded: mutex_, changed_, worker_ and error_ are threading machinery.
    // Excluded: poses_, vehiclePoses_, mineEvents_, buildingUpdates_, airSorties_, airEvents_, fog_ and
    // pending_ are presentation buffers drained by the poll*() calls; the sim never reads them back.
    h.all(modal_, paused_);
    // Excluded: speedPercent_ only paces the live worker's wall clock; it never changes a tick's outcome.
    // Excluded: simTick_ mirrors units_.tick_, which is hashed.
    h.each(scheduledReinforcements_, [&h](const ScheduledReinforcement& r) { h.all(r.event, r.dueTick); });
    h.add(nextMissionSignal_);
    if (mpSkirmish_ || !deferredMpScriptEvents_.empty()) {
        h.each(deferredMpScriptEvents_, [&h](const MissionEvent& e) { hashValue(h, e); });
        h.add(mpSkirmish_.has_value());
        if (mpSkirmish_) {
            const auto& mp = *mpSkirmish_;
            h.all(mp.config.playerCount, mp.config.playerNations, mp.config.playerTeams,
                  mp.config.captureHoldTicks, mp.config.initialWaitTicks, mp.config.captureRadiusTiles,
                  mp.assignedGates, mp.resignedSeatsMask, mp.defeatedTeamsMask, mp.matchOutcomeTeam,
                  mp.teamPresentMask, mp.zeppelinHolder, mp.zeppelinContested);
            h.each(mp.groups, [&h](const MpZeppelinGroup& g) {
                h.all(g.mask, g.holdTicks, g.airGrantedTeamsMask);
                h.each(g.waves, [&h](const MpZeppelinWave& w) { h.all(w.packed4, w.jpWave, w.periodSec, w.nextRepeatTick); });
                h.each(g.airPlanes, [&h](const MpZeppelinAirGrant& a) { h.all(a.slot, a.count); });
                h.each(g.airSorties, [&h](const MpZeppelinAirGrant& a) { h.all(a.slot, a.count); });
            });
        }
    }
    // Excluded: recording_, commandSink_, pendingCrewSpawns_, and inLockstepApply_ are transport/presentation helpers.
    return h.value();
}
} // namespace ss2
