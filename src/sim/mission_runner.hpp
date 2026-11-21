#pragma once
#include "sim/command.hpp"
#include "sim/tutorial.hpp"
#include "sim/tutorial_units.hpp"
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace ss2 {
// The mission worker owns script execution. Only presentation events and input
// cross between this clock and SDL's main thread.
//
// Every simulation mutation below is a thin wrapper that submits a cmd::Command.
// Commands are applied between ticks under mutex_ and, while recording, logged
// with the tick at which they were applied.
class MissionRunner {
public:
    MissionRunner(assets::TutorialData, unsigned delayMs, TutorialUnits);
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
    std::array<assets::AirSupportSlot, 5> airSupportFor(int owner) const;
    std::array<assets::AirSupportSlot, 5> playerAirSupport();
    bool callAirSupport(assets::AirMissionKind kind, int targetX, int targetY);
    std::vector<AirSortiePose> pollAirSorties();
    std::vector<AirStrikeEvent> pollAirStrikeEvents();
    TutorialUnits::Snapshot takeSnapshot();
    bool loadSnapshot(const TutorialUnits::Snapshot& snap);
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
    const assets::TutorialData& data() const { return tutorial_.data(); }

    // Applies a command exactly as the matching wrapper would, discarding its result.
    void apply(const cmd::Command& command);
    // Starts a fresh in-memory log of every submitted command.
    void startRecording();
    // Stops recording and returns the log.
    std::vector<cmd::RecordedCommand> takeRecording();
private:
    struct ScheduledReinforcement {
        MissionEvent event;
        std::uint32_t dueTick{0};
    };
    void collect();
    void flushScheduledReinforcements();
    void mergeSoldierPoses(std::span<const SoldierPose> incoming);
    void collectVehiclePoses();
    void collectBuildingUpdates();
    void collectAirSorties();
    unsigned countPendingReinforcementsByGroup(
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group) const;
    unsigned countPendingReinforcementsByGroupTeam(
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group, int team) const;
    unsigned countPendingReinforcementsByPlayer(
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, int owner) const;
    MissionQueries makeQueriesLocked(
        std::span<const std::uint8_t> zones,
        const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves,
        std::span<const std::pair<int, int>> markers) const;
    void run(std::stop_token, unsigned delayMs);
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
    TutorialUnits units_;
    std::array<bool, 64> spawnedReinforcementWaves_{};
    std::array<int, 64> reinforcementWaveOwner_{};
    Tutorial tutorial_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::vector<SoldierPose> poses_;
    std::vector<VehiclePose> vehiclePoses_;
    std::vector<MineEvent> mineEvents_;
    std::vector<BuildingSnapshot> buildingUpdates_;
    std::vector<AirSortiePose> airSorties_;
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
    std::jthread worker_;
};
}
