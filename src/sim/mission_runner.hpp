#pragma once
#include "sim/command.hpp"
#include "sim/tutorial.hpp"
#include "sim/tutorial_units.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace ss2 {
struct MultiplayerSkirmishConfig {
    int playerCount{2};
    std::array<std::uint8_t, 4> playerNations{1, 0, 2, 3};
    std::array<int, 4> playerTeams{0, 1, 2, 3};
    std::uint32_t captureHoldTicks{375u};
    std::uint32_t initialWaitTicks{0u};
    int captureRadiusTiles{4};
};

struct ZeppelinMarkerStatus {
    std::uint8_t index{0};
    int x{0};
    int y{0};
    int holderTeam{-1};
    bool contested{false};
    float captureProgress{0.0f};
    int capturingTeam{-1};
};

// The mission worker owns script execution. Only presentation events and input
// cross between this clock and SDL's main thread.
//
// Every simulation mutation below is a thin wrapper that submits a cmd::Command.
// Commands are applied between ticks under mutex_ and, while recording, logged
// with the tick at which they were applied, so replay() on a ManualClock runner
// reproduces a live session without any wall clock.
class MissionRunner {
public:
    // Tag for a runner without a worker thread; time only advances through
    // advanceTicks() or replay().
    struct ManualClock {
        explicit ManualClock() = default;
    };
    MissionRunner(assets::TutorialData, unsigned delayMs, TutorialUnits, bool startRecordingNow = false,
                  std::map<unsigned, std::vector<assets::ReinforcementUnit>> initialSavedPools = {});
    MissionRunner(ManualClock, assets::TutorialData, TutorialUnits,
                  std::map<unsigned, std::vector<assets::ReinforcementUnit>> initialSavedPools = {});
    bool canMove(std::span<const size_t> indices, int x, int y);
    std::optional<size_t> move(std::span<const size_t> indices, int mapX, int mapY, bool append = false);
    std::optional<size_t> attackMove(std::span<const size_t> indices, int mapX, int mapY, bool append = false);
    std::optional<size_t> patrol(std::span<const size_t> indices, int mapX, int mapY);
    size_t scatter(std::span<const size_t> indices);
    bool isValidAttackTarget(size_t targetIndex);
    bool isElevatedMapPoint(int mapX, int mapY) const;
    std::optional<size_t> attack(std::span<const size_t> indices, size_t targetIndex);
    size_t stop(std::span<const size_t> indices);
    std::optional<bool> toggleHoldFire(std::span<const size_t> indices);
    std::optional<bool> toggleCrouch(std::span<const size_t> indices);
    std::optional<bool> toggleBinoculars(std::span<const size_t> indices);
    size_t spawnInfantry(const assets::Infantry& unit, int expa = 0);
    std::optional<size_t> moveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append = false);
    std::optional<size_t> reverseMoveVehicles(std::span<const size_t> indices, int mapX, int mapY);
    std::optional<size_t> faceVehicles(std::span<const size_t> indices, int mapX, int mapY);
    std::optional<size_t> attackMoveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append = false);
    std::optional<size_t> patrolVehicles(std::span<const size_t> indices, int mapX, int mapY);
    std::optional<size_t> attackWithVehicles(std::span<const size_t> indices, bool targetIsVehicle, size_t targetIdx);
    std::optional<size_t> attackVehicle(std::span<const size_t> indices, size_t targetVehicleIdx);
    size_t stopVehicles(std::span<const size_t> indices);
    bool canSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) const;
    std::optional<size_t> orderSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx);
    bool canSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) const;
    std::optional<size_t> orderSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx);
    bool canHealSoldier(std::span<const size_t> medicSoldierIds, std::span<const size_t> ambulanceVehicleIds,
                        size_t targetSoldierIdx) const;
    std::optional<size_t> orderHealSoldier(std::span<const size_t> medicSoldierIds,
                                           std::span<const size_t> ambulanceVehicleIds, size_t targetSoldierIdx);
    bool canEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                          size_t targetVehicleIdx) const;
    std::optional<size_t> orderEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                             size_t targetVehicleIdx);
    bool canEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                          size_t targetSoldierIdx) const;
    std::optional<size_t> orderEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                             size_t targetSoldierIdx);
    std::optional<bool> toggleVehicleHoldFire(std::span<const size_t> indices);
    std::optional<bool> toggleMarchMode(std::span<const size_t> vehicleIds);
    bool isValidVehicleAttackTarget(size_t targetIdx);
    bool canBoardVehicle(size_t vehicleIdx);
    std::optional<size_t> boardVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx);
    bool embarkImmediate(size_t soldierIdx, size_t vehicleIdx);
    size_t unloadVehicles(std::span<const size_t> vehicleIds);
    bool canCaptureVehicle(size_t vehicleIdx);
    std::optional<size_t> captureVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx);
    std::vector<TacticalOrderLine> selectionOrderLines(std::span<const size_t> soldierIds,
                                                       std::span<const size_t> vehicleIds);
    std::vector<VehicleRangePreview> selectionRangePreviews(std::span<const size_t> vehicleIds,
                                                            std::span<const size_t> soldierIds = {}) const;
    std::vector<assets::Placement> abandonVehicles(std::span<const size_t> vehicleIds);
    void addReserveUnit(const TutorialUnits::ReserveUnit& entry);
    std::vector<TutorialUnits::ReserveUnit> takeReserveUnits(unsigned group, int pool = -1,
                                                             std::size_t maxCount = static_cast<std::size_t>(-1));
    std::vector<assets::ReinforcementUnit> reinforcementWave(unsigned waveIdx) const;
    std::map<unsigned, std::vector<assets::ReinforcementUnit>> savedUnitPools() const;
    std::optional<size_t> attackGroundVehicles(std::span<const size_t> indices, int mapX, int mapY);
    std::optional<size_t> attackGroundSoldiers(std::span<const size_t> indices, int mapX, int mapY);
    size_t layMine(std::span<const size_t> soldierIds);
    bool canDisarmMineAt(std::span<const size_t> soldierIds, int mapX, int mapY) const;
    std::optional<size_t> orderDisarmMine(std::span<const size_t> soldierIds, int mapX, int mapY);
    size_t orderGroupMove(unsigned group, int mapX, int mapY, bool onlyNonPlayer = false);
    std::vector<MineEvent> pollMineEvents();
    std::vector<VisibleMine> visibleMines() const;
    std::vector<IdleUnit> idlePlayerUnits() const;
    bool canTowVehicle(size_t tractorIdx, size_t gunIdx);
    std::optional<size_t> towVehicle(size_t tractorIdx, size_t gunIdx);
    size_t unhitchVehicles(std::span<const size_t> vehicleIndices);
    bool canGarrisonBuilding(size_t buildingIdx);
    std::optional<size_t> garrisonBuilding(std::span<const size_t> soldierIds, size_t buildingIdx);
    size_t evacuateBuilding(size_t buildingIdx);
    std::optional<bool> toggleBuildingHoldFire(size_t buildingIdx);
    std::optional<bool> toggleBuildingBinoculars(size_t buildingIdx);
    std::optional<size_t> orderBuildingAttack(size_t buildingIdx, bool targetIsVehicle, size_t targetIdx);
    std::optional<size_t> orderBuildingAttackBuilding(size_t buildingIdx, size_t targetBuildingIdx);
    std::optional<size_t> attackBuilding(std::span<const size_t> soldierIds, size_t buildingIdx);
    std::optional<size_t> attackBuildingWithVehicles(std::span<const size_t> vehicleIds, size_t buildingIdx);
    std::vector<BuildingSnapshot> pollBuildingUpdates();
    bool canSupportBridge(std::span<const size_t> supporterVehicleIds, size_t bridgeIdx) const;
    std::optional<size_t> orderSupportBridge(std::span<const size_t> supporterVehicleIds, size_t bridgeIdx);
    std::vector<BridgeSnapshot> pollBridgeUpdates();
    std::array<assets::AirSupportSlot, 5> airSupportFor(int owner) const;
    std::array<assets::AirSupportSlot, 5> playerAirSupport();
    bool callAirSupport(assets::AirMissionKind kind, int targetX, int targetY);
    std::vector<AirSortiePose> pollAirSorties();
    std::vector<ParachutePose> pollParachutes();
    std::vector<AirStrikeEvent> pollAirStrikeEvents();
    struct ScheduledReinforcement {
        MissionEvent event;
        std::uint32_t dueTick{0};
        bool operator==(const ScheduledReinforcement&) const = default;
    };
    struct FullSnapshot {
        TutorialUnits::Snapshot units;
        TutorialUnits::ExtraState unitsExtra;
        Tutorial::VmState tutorial;
        std::array<bool, 64> spawnedReinforcementWaves{};
        std::array<int, 64> reinforcementWaveOwner{};
        std::vector<ScheduledReinforcement> scheduledReinforcements;
        std::map<unsigned, std::vector<assets::ReinforcementUnit>> savedUnitPools;
        std::map<unsigned, std::vector<assets::ReinforcementUnit>> dynamicWaves;
        unsigned nextDynamicWaveId{64u};
        std::optional<std::pair<std::uint32_t, std::uint32_t>> nextMissionSignal;
        bool operator==(const FullSnapshot&) const = default;
    };
    TutorialUnits::Snapshot takeSnapshot();
    bool loadSnapshot(const TutorialUnits::Snapshot& snap);
    FullSnapshot takeFullSnapshot();
    bool loadFullSnapshot(const FullSnapshot& snap);
    static std::vector<std::uint8_t> serializeFullSnapshot(const FullSnapshot& snap);
    static std::optional<FullSnapshot> deserializeFullSnapshot(std::span<const std::uint8_t> bytes);
    unsigned simTick() const { return simTick_.load(std::memory_order_relaxed); }
    void setSpeedPercent(int percent);
    int speedPercent() const { return speedPercent_.load(); }
    size_t spawnVehicle(const assets::VehicleUnit& unit);
    std::vector<SoldierPose> pollPoses();
    std::vector<VehiclePose> pollVehiclePoses();
    std::optional<std::vector<std::uint8_t>> pollFog();
    ~MissionRunner();
    std::vector<MissionEvent> poll();
    std::optional<std::pair<std::uint32_t, std::uint32_t>> nextMissionSignal() const;
    unsigned countByGroupMask(unsigned group, unsigned mask) const;
    unsigned countByGroupTeamMask(unsigned group, int team, unsigned mask) const;
    unsigned countByPlayerMask(int owner, unsigned mask) const;
    void acknowledge();
    bool togglePause();
    bool isPaused() const;
    void configureMultiplayerSkirmish(const MultiplayerSkirmishConfig& cfg);
    void resign(std::uint8_t seat);
    std::vector<ZeppelinMarkerStatus> zeppelinStatuses() const;
    int assignedGateForSeat(int seat) const;
    bool mpSkirmishConfigured() const;
    void setViewerOwner(int owner);
    int viewerOwner() const;
    void setMultiplayerSymmetry(bool enabled);
    bool multiplayerSymmetry() const;
    void setCommandSink(std::function<void(cmd::Command)> sink);
    std::vector<assets::Placement> pollCrewSpawns();
    const assets::TutorialData& data() const { return tutorial_.data(); }

    // Applies a command exactly as the matching wrapper would, discarding its result.
    void apply(const cmd::Command& command);
    // Applies a command on behalf of seat `actorPlayer` (0..3), bypassing commandSink_.
    void applyAs(int actorPlayer, const cmd::Command& command);
    // Starts a fresh in-memory log of every submitted command.
    void startRecording();
    // Stops recording and returns the log.
    std::vector<cmd::RecordedCommand> takeRecording();
    // Stops and joins the background simulation worker thread, if running, so
    // final simTick(), stateHash() and takeRecording() form a consistent snapshot.
    void stopWorker();
    // Runs n worker iterations: each steps once unless a dialog or pause holds
    // the clock, as in live play. Meant for ManualClock runners.
    void advanceTicks(unsigned n);
    // Steps to each command's tick, applies it, then steps to untilTick.
    // Returns false if the log cannot be followed: a command lies in the past,
    // or the clock is held by a dialog or pause that the log never releases.
    bool replay(std::span<const cmd::RecordedCommand> log, std::uint32_t untilTick);
    // Advances a live/interactive replay by at most 1 simulation tick, applying
    // all recorded commands scheduled at the pre-step and post-step ticks so the
    // resulting trajectory and stateHash() match replay(log, endTick) bit-for-bit.
    bool replayStep(std::span<const cmd::RecordedCommand> log, std::size_t& cursor, std::uint32_t endTick);
    // 64-bit hash of the whole simulation: units, script VM and the runner's
    // own sim-affecting state (mission_state_hash.cpp). Two runners with equal
    // hashes evolve identically under the same commands.
    std::uint64_t stateHash() const;
private:
    void collect();
    void flushScheduledReinforcements();
    void handleMpScriptEventLocked(const MissionEvent& e);
    void stepMpSkirmishLocked();
    void evaluateMpVictoryLocked();
    void mergeSoldierPoses(std::span<const SoldierPose> incoming);
    void collectVehiclePoses();
    void collectBuildingUpdates();
    void collectBridgeUpdates();
    void collectAirSorties();
    void collectParachutes();
    unsigned countPendingReinforcementsByGroup(
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group) const;
    unsigned countPendingReinforcementsByGroupTeam(
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group, int team) const;
    unsigned countPendingReinforcementsByPlayer(
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, int owner) const;
    const std::vector<assets::ReinforcementUnit>* reinforcementWaveLocked(unsigned waveIdx) const;
    MissionQueries makeQueriesLocked(
        std::span<const std::uint8_t> zones,
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves,
        std::span<const std::pair<int, int>> markers) const;
    void run(std::stop_token, unsigned delayMs);
    void stepTickLocked();
    void stepIfRunningLocked();
    bool advanceToTickLocked(std::uint32_t tick);
    template<class C> typename C::Result submit(const C& c);
    template<class C> typename C::Result submitLocked(const C& c);
    cmd::Move::Result applyLocked(const cmd::Move&);
    cmd::AttackMove::Result applyLocked(const cmd::AttackMove&);
    cmd::Patrol::Result applyLocked(const cmd::Patrol&);
    cmd::Scatter::Result applyLocked(const cmd::Scatter&);
    cmd::Attack::Result applyLocked(const cmd::Attack&);
    cmd::Stop::Result applyLocked(const cmd::Stop&);
    cmd::ToggleHoldFire::Result applyLocked(const cmd::ToggleHoldFire&);
    cmd::ToggleCrouch::Result applyLocked(const cmd::ToggleCrouch&);
    cmd::ToggleBinoculars::Result applyLocked(const cmd::ToggleBinoculars&);
    cmd::SpawnInfantry::Result applyLocked(const cmd::SpawnInfantry&);
    cmd::MoveVehicles::Result applyLocked(const cmd::MoveVehicles&);
    cmd::ReverseMoveVehicles::Result applyLocked(const cmd::ReverseMoveVehicles&);
    cmd::FaceVehicles::Result applyLocked(const cmd::FaceVehicles&);
    cmd::AttackMoveVehicles::Result applyLocked(const cmd::AttackMoveVehicles&);
    cmd::PatrolVehicles::Result applyLocked(const cmd::PatrolVehicles&);
    cmd::AttackWithVehicles::Result applyLocked(const cmd::AttackWithVehicles&);
    cmd::AttackVehicle::Result applyLocked(const cmd::AttackVehicle&);
    cmd::StopVehicles::Result applyLocked(const cmd::StopVehicles&);
    cmd::SupportVehicle::Result applyLocked(const cmd::SupportVehicle&);
    cmd::SupportBuilding::Result applyLocked(const cmd::SupportBuilding&);
    cmd::HealSoldier::Result applyLocked(const cmd::HealSoldier&);
    cmd::EscortVehicle::Result applyLocked(const cmd::EscortVehicle&);
    cmd::EscortSoldier::Result applyLocked(const cmd::EscortSoldier&);
    cmd::ToggleVehicleHoldFire::Result applyLocked(const cmd::ToggleVehicleHoldFire&);
    cmd::ToggleMarchMode::Result applyLocked(const cmd::ToggleMarchMode&);
    cmd::BoardVehicle::Result applyLocked(const cmd::BoardVehicle&);
    cmd::EmbarkImmediate::Result applyLocked(const cmd::EmbarkImmediate&);
    cmd::UnloadVehicles::Result applyLocked(const cmd::UnloadVehicles&);
    cmd::CaptureVehicle::Result applyLocked(const cmd::CaptureVehicle&);
    cmd::AbandonVehicles::Result applyLocked(const cmd::AbandonVehicles&);
    cmd::TowVehicle::Result applyLocked(const cmd::TowVehicle&);
    cmd::UnhitchVehicles::Result applyLocked(const cmd::UnhitchVehicles&);
    cmd::SpawnVehicle::Result applyLocked(const cmd::SpawnVehicle&);
    cmd::AddReserveUnit::Result applyLocked(const cmd::AddReserveUnit&);
    cmd::TakeReserveUnits::Result applyLocked(const cmd::TakeReserveUnits&);
    cmd::AttackGroundVehicles::Result applyLocked(const cmd::AttackGroundVehicles&);
    cmd::AttackGroundSoldiers::Result applyLocked(const cmd::AttackGroundSoldiers&);
    cmd::LayMine::Result applyLocked(const cmd::LayMine&);
    cmd::DisarmMine::Result applyLocked(const cmd::DisarmMine&);
    cmd::GroupMove::Result applyLocked(const cmd::GroupMove&);
    cmd::GarrisonBuilding::Result applyLocked(const cmd::GarrisonBuilding&);
    cmd::EvacuateBuilding::Result applyLocked(const cmd::EvacuateBuilding&);
    cmd::ToggleBuildingHoldFire::Result applyLocked(const cmd::ToggleBuildingHoldFire&);
    cmd::ToggleBuildingBinoculars::Result applyLocked(const cmd::ToggleBuildingBinoculars&);
    cmd::BuildingAttack::Result applyLocked(const cmd::BuildingAttack&);
    cmd::BuildingAttackBuilding::Result applyLocked(const cmd::BuildingAttackBuilding&);
    cmd::AttackBuilding::Result applyLocked(const cmd::AttackBuilding&);
    cmd::AttackBuildingWithVehicles::Result applyLocked(const cmd::AttackBuildingWithVehicles&);
    cmd::CallAirSupport::Result applyLocked(const cmd::CallAirSupport&);
    cmd::LoadSnapshot::Result applyLocked(const cmd::LoadSnapshot&);
    cmd::Acknowledge::Result applyLocked(const cmd::Acknowledge&);
    cmd::TogglePause::Result applyLocked(const cmd::TogglePause&);
    cmd::Resign::Result applyLocked(const cmd::Resign&);
    cmd::SupportBridge::Result applyLocked(const cmd::SupportBridge&);
    void stepGroupAiLocked(bool fastOnly = false);
    struct MpZeppelinWave {
        std::uint32_t packed4{0};
        std::uint32_t jpWave{0};
        std::uint32_t periodSec{0};
        std::array<std::uint32_t, 4> nextRepeatTick{};
    };
    struct MpZeppelinAirGrant {
        unsigned slot{0};
        unsigned count{0};
    };
    struct MpZeppelinGroup {
        std::uint32_t mask{0};
        std::array<std::uint32_t, 4> holdTicks{};
        std::uint8_t airGrantedTeamsMask{0};
        std::vector<MpZeppelinWave> waves;
        std::vector<MpZeppelinAirGrant> airPlanes;
        std::vector<MpZeppelinAirGrant> airSorties;
    };
    struct MpSkirmishState {
        MultiplayerSkirmishConfig config{};
        std::array<unsigned, 4> assignedGates{};
        std::uint8_t resignedSeatsMask{0};
        std::uint8_t defeatedTeamsMask{0};
        int matchOutcomeTeam{-1};
        std::array<std::uint32_t, 4> teamPresentMask{};
        std::array<int, 32> zeppelinHolder{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
        std::array<bool, 32> zeppelinContested{};
        std::vector<MpZeppelinGroup> groups;
    };
    TutorialUnits units_;
    std::array<bool, 64> spawnedReinforcementWaves_{};
    std::array<int, 64> reinforcementWaveOwner_{};
    // Initialized before tutorial_ so tick-0 scan sees campaign-carried savedUnitPools_.
    std::map<unsigned, std::vector<assets::ReinforcementUnit>> savedUnitPools_;
    std::map<unsigned, std::vector<assets::ReinforcementUnit>> dynamicWaves_;
    Tutorial tutorial_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::vector<SoldierPose> poses_;
    std::vector<VehiclePose> vehiclePoses_;
    std::vector<MineEvent> mineEvents_;
    std::vector<BuildingSnapshot> buildingUpdates_;
    std::vector<BridgeSnapshot> bridgeUpdates_;
    std::vector<AirSortiePose> airSorties_;
    std::vector<ParachutePose> parachutes_;
    std::vector<AirStrikeEvent> airEvents_;
    std::optional<std::vector<std::uint8_t>> fog_;
    bool modal_{}, paused_{};
    std::atomic<int> speedPercent_{100};
    std::atomic<unsigned> simTick_{0};
    std::vector<ScheduledReinforcement> scheduledReinforcements_;
    std::vector<MissionEvent> pending_;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> nextMissionSignal_;
    std::exception_ptr error_;
    std::optional<std::vector<cmd::RecordedCommand>> recording_;
    std::function<void(cmd::Command)> commandSink_;
    std::vector<assets::Placement> pendingCrewSpawns_;
    bool inLockstepApply_{false};
    unsigned nextDynamicWaveId_{64u};
    std::vector<MissionEvent> deferredMpScriptEvents_;
    std::optional<MpSkirmishState> mpSkirmish_;
    std::jthread worker_;
};
}
