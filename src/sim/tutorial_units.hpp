#pragma once
#include "assets/infantry.hpp"
#include "assets/mission.hpp"
#include "assets/scenery.hpp"
#include "assets/unit_catalog.hpp"
#include "assets/vehicles.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/infantry_path.hpp"
#include "sim/infantry_step.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ss2 {
struct SoldierPose {
    size_t index;
    int x, y;
    unsigned direction, progress;
    bool walking;
    int hp{100};
    bool alive{true};
    int owner{0};
    int group{0};
    bool fired{false};
    bool visible{true};
    int targetX{0};
    int targetY{0};
    bool garrisoned{false};
    int expa{0};
    int kills{0};
    bool threwGrenade{false};
    int atGrenades{0};
    bool firedFlamer{false};
    bool camouflaged{false};
    int mineAmmo{0};
    int ammo{-1};
    bool firedExplosive{false};
    bool suppressed{false};
    std::int8_t crouchedState{-1}; // -1 = unchanged, 0 = standing, 1 = crouched
};

struct VehiclePose {
    size_t index{};
    int x{}, y{};
    unsigned hullDir{}, turretDir{};
    int hp{}, ammo{};
    bool alive{true}, visible{true}, moving{false}, fired{false};
    int targetX{}, targetY{};
    int owner{}, group{};
    int maxHp{100};
    bool firedSecondary{false};
    std::string fireSound{};
    int passengerCount{0};
    int passengerCapacity{0};
    bool heavyImpact{false};
    bool hitVehicle{false};
    bool repaired{false};
    bool resupplied{false};
    bool captured{false};
    int towedIndex{-1};
    int towedByIndex{-1};
    int expa{0};
    int kills{0};
    bool firedRocket{false};
    bool firedFlamer{false};
    int ammo2{0};
    bool decrewed{false};
    bool crewImpaired{false};
    bool moveImpaired{false};
    bool suppressed{false};
    std::uint8_t craterScale{0};
    bool hatchOpen{false};
};

struct BuildingSnapshot {
    std::size_t index{};
    int x{}, y{};
    int left{}, top{}, width{}, height{};
    int hp{}, maxHp{};
    int capacity{};
    int occupantCount{};
    int owner{-1};
    bool destroyed{false};
    bool damagedThisTick{false};
    bool collapsedThisTick{false};
    std::vector<size_t> occupantIds{};
};

struct BridgeSnapshot {
    std::size_t index{};
    int x{}, y{};
    int left{}, top{}, width{}, height{};
    int hp{}, maxHp{};
    int stage{0};
    bool destroyed{false};
    bool damagedThisTick{false};
    bool collapsedThisTick{false};
    bool repairedThisTick{false};
};

struct MineEvent {
    enum Kind { Detonated, Detected, Disarmed, Laid } kind{Detonated};
    int x{};
    int y{};
    bool antiTank{true};
};

struct VisibleMine {
    int x{}, y{};
    bool antiTank{true};
    bool hostile{false};
};

struct IdleUnit {
    bool isVehicle{false};
    size_t index{0};
};

struct VehicleRangePreview {
    int x{}, y{};
    float rangePx{0.0f};
    float deadzonePx{0.0f};
};

struct AirSortiePose {
    std::uint32_t id{};
    assets::AirMissionKind kind{assets::AirMissionKind::Bomber};
    std::string planeId;
    int owner{0};
    int x{}, y{};
    unsigned dir32{0};
    int hp{100}, maxHp{100};
    bool alive{true};
    bool active{true};
    bool aaFired{false};
    int aaOriginX{}, aaOriginY{};
    std::string aaSoundGroup{};
};

struct AirStrikeEvent {
    enum Kind { BombImpact, StrafeImpact, ParatrooperDrop, SupplyDrop, PlaneShotDown } kind{BombImpact};
    int x{}, y{};
    int owner{0};
    int group{0};
    std::vector<assets::ParatrooperEntry> paratroopers;
    int scoreValue{500};
    std::string planeId{};
    bool isLeaflet{false};
};

struct TacticalOrderLine {
    enum class Kind : std::uint8_t { Move, AttackMove, Patrol, Attack, AttackGround, Support };
    int x0{}, y0{}, x1{}, y1{};
    Kind kind{Kind::Move};
};

class TutorialUnits {
public:
    struct ReserveUnit {
        std::string id;
        int owner{0}, group{0}, hp{100}, ammo{100}, expa{0}, pool{0};
        bool operator==(const ReserveUnit&) const = default;
    };
    struct Snapshot {
        struct SoldierState {
            int cellX{}, cellY{};
            int hp{}, maxHp{}, ammo{}, expa{}, kills{}, atGrenades{}, mineAmmo{};
            int owner{}, group{};
            std::uint8_t direction{};
            bool crouched{}, holdFire{}, binoculars{}, embarked{};
            std::int32_t garrisonBuilding{-1};
            int suppressionTicks{0};
            bool operator==(const SoldierState&) const = default;
        };
        struct VehicleState {
            int x{}, y{};
            unsigned hullDir{}, turretDir{};
            int hp{}, maxHp{}, ammo{}, ammo2{}, expa{}, kills{};
            int owner{}, group{};
            bool alive{}, holdFire{};
            std::vector<std::uint32_t> passengers;
            std::int32_t towedVehicle{-1};
            std::int32_t towedBy{-1};
            bool hasCrew{true};
            int crewHp{-1};
            bool operator==(const VehicleState&) const = default;
        };
        struct BuildingState {
            int hp{}, owner{-1};
            bool destroyed{};
            std::vector<std::uint32_t> occupants;
            bool operator==(const BuildingState&) const = default;
        };
        unsigned tick{0};
        std::array<int, 4> playerTeams{0, 1, 2, 3};
        std::vector<std::uint8_t> mines;
        std::vector<std::uint8_t> fog;
        std::vector<SoldierState> soldiers;
        std::vector<VehicleState> vehicles;
        std::vector<BuildingState> buildings;
        std::array<std::array<int, 5>, 4> airReady{};
        bool operator==(const Snapshot&) const = default;
    };
    struct EvacuationOrder {
        std::size_t index{0};
        int targetX{0};
        int targetY{0};
        bool operator==(const EvacuationOrder&) const = default;
    };
    struct ExtraState {
        struct VehicleExtra {
            int suppressionTicks{0};
            bool hatchOpen{false};
            bool operator==(const VehicleExtra&) const = default;
        };
        struct BridgeState {
            int hp{3500};
            int stage{0};
            bool destroyed{false};
            bool operator==(const BridgeState&) const = default;
        };
        static constexpr std::array<std::int32_t, 100> makeDefaultGroupHitTicks() noexcept {
            std::array<std::int32_t, 100> a{};
            a.fill(-1);
            return a;
        }
        std::vector<std::string> soldierIds;
        std::vector<std::string> vehicleIds;
        std::vector<VehicleExtra> vehicleExtras;
        std::uint32_t randomState{0x76321fu};
        std::uint32_t combatRandomState{0x12345678u};
        assets::WeatherKind weather{assets::WeatherKind::Clear};
        std::array<std::int32_t, 4> lastHitTick{-1, -1, -1, -1};
        std::array<std::int32_t, 100> lastGroupHitTick{makeDefaultGroupHitTicks()};
        std::array<std::array<std::int32_t, 5>, 4> airPlanes{};
        std::uint32_t nextSortieId{1u};
        std::vector<ReserveUnit> reserveUnits;
        std::vector<EvacuationOrder> evacuatingSoldiers;
        std::vector<EvacuationOrder> evacuatingVehicles;
        std::vector<BridgeState> bridges;
        bool operator==(const ExtraState&) const = default;
    };

    static constexpr int missionExpaToSim(int expaPct) noexcept {
        if (expaPct <= 0) return 0;
        if (expaPct >= 100) return 0x8000;
        return (expaPct * 0x8000) / 100;
    }

    TutorialUnits(InfantryPathGrid, std::span<const assets::Infantry>, std::span<const assets::Placement> vehicles,
                  std::optional<FogGrid> fog = std::nullopt, int difficulty = 2, bool autoEngage = false,
                  std::vector<assets::VehicleUnit> vehicleUnits = {},
                  std::shared_ptr<const assets::UnitCatalog> catalog = {});
    bool canMove(size_t index, int mapX, int mapY) const;
    bool order(size_t index, int mapX, int mapY);
    bool canMoveSelection(std::span<const size_t> ids, int mapX, int mapY) const;
    size_t orderSelection(std::span<const size_t> ids, int mapX, int mapY, bool append = false);
    size_t orderAttackMove(std::span<const size_t> ids, int mapX, int mapY, bool append = false);
    size_t orderPatrol(std::span<const size_t> ids, int mapX, int mapY);
    size_t orderScatter(std::span<const size_t> ids);

    bool isValidAttackTarget(size_t targetIndex, int viewerOwner = 0) const;
    bool isElevatedMapPoint(int mapX, int mapY) const;
    size_t orderAttack(std::span<const size_t> ids, size_t targetIndex);
    size_t stopSelection(std::span<const size_t> ids);
    bool toggleHoldFire(std::span<const size_t> ids);
    bool toggleCrouch(std::span<const size_t> ids);
    bool isSoldierCamouflaged(size_t index) const;
    bool toggleBinoculars(std::span<const size_t> ids);
    size_t spawnInfantry(const assets::Infantry& unit, int expa = 0);

    size_t moveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append = false);
    size_t reverseMoveVehicles(std::span<const size_t> indices, int mapX, int mapY);
    size_t faceVehicles(std::span<const size_t> indices, int mapX, int mapY);
    size_t attackMoveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append = false);
    size_t patrolVehicles(std::span<const size_t> indices, int mapX, int mapY);
    size_t stopVehicles(std::span<const size_t> indices);
    bool canSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) const;
    size_t orderSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx);
    bool canSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) const;
    size_t orderSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx);
    bool canHealSoldier(std::span<const size_t> medicSoldierIds, std::span<const size_t> ambulanceVehicleIds,
                        size_t targetSoldierIdx) const;
    size_t orderHealSoldier(std::span<const size_t> medicSoldierIds, std::span<const size_t> ambulanceVehicleIds,
                            size_t targetSoldierIdx);
    bool canEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                          size_t targetVehicleIdx) const;
    size_t orderEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                              size_t targetVehicleIdx);
    bool canEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                          size_t targetSoldierIdx) const;
    size_t orderEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                              size_t targetSoldierIdx);
    std::optional<bool> toggleVehicleHoldFire(std::span<const size_t> indices);
    std::optional<bool> toggleMarchMode(std::span<const size_t> vehicleIds);
    size_t attackWithVehicles(std::span<const size_t> indices, bool targetIsVehicle, size_t targetIdx);
    size_t attackVehicle(std::span<const size_t> indices, size_t targetVehicleIdx);
    bool isValidVehicleAttackTarget(size_t targetIdx, int viewerOwner = 0) const;
    size_t spawnVehicle(const assets::VehicleUnit& unit);
    std::vector<VehiclePose> takeVehiclePoses();

    size_t orderGroupMove(unsigned group, int mapX, int mapY, bool onlyNonPlayer = true);
    size_t orderGroupAttackGround(unsigned group, int mapX, int mapY, bool onlyNonPlayer = true);
    size_t transferGroupOwner(unsigned group, int newOwner, std::vector<SoldierPose>* outSoldiers = nullptr);
    size_t reassignGroupInZone(std::span<const std::uint8_t> zones, unsigned zone, unsigned oldGroup,
                                 unsigned newGroup, std::vector<SoldierPose>* outSoldiers = nullptr);
    bool isObjectDestroyedAt(int mapX, int mapY) const;
    void revealFogAt(int mapX, int mapY, int radiusTiles, int player = 0);
    bool canBoardVehicle(size_t vehicleIdx, int viewerOwner = 0) const;
    bool embarkImmediate(size_t soldierIdx, size_t vehicleIdx);
    size_t orderBoardVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx);
    size_t unloadVehicles(std::span<const size_t> vehicleIds, std::vector<SoldierPose>* outPoses = nullptr);

    void setPlayerTeams(std::array<int, 4> teams);
    int playerTeam(int owner) const noexcept;
    bool isHostile(int ownerA, int ownerB) const;
    bool isAlliedFor(int viewer, int owner) const;
    bool isAlliedWithPlayer(int owner) const;
    void setViewerOwner(int owner);
    int viewerOwner() const noexcept { return viewerOwner_; }
    void setMultiplayerSymmetry(bool enabled);
    bool multiplayerSymmetry() const noexcept { return multiplayerSymmetry_; }
    bool isVisibleToViewer(int owner, std::uint8_t spottedMask) const;
    int actorOwner() const noexcept { return commandActor_; }
    bool commandActorActive() const noexcept { return commandActorActive_; }
    int originX() const noexcept { return originX_; }
    int effectiveQueryOwner() const noexcept { return commandActorActive_ ? commandActor_ : viewerOwner_; }
    struct ScopedCommandActor {
        TutorialUnits& units;
        int previous{0};
        bool prevActive{false};
        ScopedCommandActor(TutorialUnits& u, int actor) noexcept
            : units(u), previous(u.commandActor_), prevActive(u.commandActorActive_) {
            u.commandActor_ = (actor >= 0 && actor < 4) ? actor : 0;
            u.commandActorActive_ = true;
        }
        ~ScopedCommandActor() {
            units.commandActor_ = previous;
            units.commandActorActive_ = prevActive;
        }
        ScopedCommandActor(const ScopedCommandActor&) = delete;
        ScopedCommandActor& operator=(const ScopedCommandActor&) = delete;
    };
    void setMines(std::vector<std::uint8_t> mines);
    std::vector<MineEvent> takeMineEvents();
    size_t orderLayMine(std::span<const size_t> soldierIds, std::vector<SoldierPose>* outPoses = nullptr);
    bool canDisarmMineAt(std::span<const size_t> soldierIds, int mapX, int mapY) const;
    size_t orderDisarmMine(std::span<const size_t> soldierIds, int mapX, int mapY);
    std::vector<VisibleMine> visibleMines(int viewerOwner = 0) const;
    std::vector<IdleUnit> idlePlayerUnits() const;
    bool canCaptureVehicle(size_t vehicleIdx, int viewerOwner = 0) const;
    size_t orderCaptureVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx);
    std::vector<TacticalOrderLine> selectionOrderLines(std::span<const size_t> soldierIds,
                                                       std::span<const size_t> vehicleIds) const;
    std::vector<VehicleRangePreview> selectionRangePreviews(std::span<const size_t> vehicleIds,
                                                            std::span<const size_t> soldierIds = {}) const;
    std::vector<assets::Placement> abandonVehicles(std::span<const size_t> vehicleIds,
                                                   std::vector<SoldierPose>* outPoses = nullptr);
    void addReserveUnit(const ReserveUnit& entry);
    std::vector<ReserveUnit> takeReserveUnits(unsigned group, int pool = -1,
                                              std::size_t maxCount = static_cast<std::size_t>(-1));
    unsigned countReserveByGroup(unsigned group) const;
    unsigned countReserveByPlayer(int owner) const;
    unsigned countAirPlanes(int owner, unsigned slot) const;
    unsigned groupAmmoPercent(unsigned group, unsigned slot) const;
    std::size_t setGroupStat(unsigned group, unsigned statKind, unsigned mode, unsigned pct,
                             std::vector<SoldierPose>* outSoldiers = nullptr);
    std::vector<assets::ReinforcementUnit> collectLiveUnitsForPool(bool byGroup, unsigned selector) const;
    std::size_t orderGroupEvacuate(unsigned group, int mapX, int mapY);
    std::size_t orderZoneEvacuate(std::span<const std::uint8_t> zones, unsigned zone, int mapX, int mapY);
    std::size_t orderGroupMoveLimited(unsigned group, unsigned maxCount, int mapX, int mapY);
    size_t orderAttackGroundVehicles(std::span<const size_t> indices, int mapX, int mapY);
    size_t orderAttackGroundSoldiers(std::span<const size_t> indices, int mapX, int mapY);
    std::optional<unsigned> lastHitTick(unsigned player) const;
    void setPlayerTeam(unsigned player, int team);
    std::optional<unsigned> lastGroupHitTick(unsigned group) const;

    using WeatherKind = assets::WeatherKind;
    void setWeather(WeatherKind w);
    WeatherKind weather() const noexcept { return weather_; }
    int weatherVisibilityPct() const noexcept;

    bool canTowVehicle(size_t tractorIdx, size_t gunIdx, int viewerOwner = 0) const;
    size_t orderTowVehicle(size_t tractorIdx, size_t gunIdx);
    size_t unhitchVehicles(std::span<const size_t> vehicleIndices);

    void setBuildings(std::vector<assets::BuildingPlacement> buildings);
    bool canGarrisonBuilding(size_t buildingIdx, int viewerOwner = 0) const;
    size_t orderGarrisonBuilding(std::span<const size_t> soldierIds, size_t buildingIdx);
    size_t evacuateBuilding(size_t buildingIdx, std::vector<SoldierPose>* outPoses = nullptr);
    std::optional<bool> toggleBuildingHoldFire(size_t buildingIdx);
    std::optional<bool> toggleBuildingBinoculars(size_t buildingIdx, std::vector<SoldierPose>* outPoses = nullptr);
    size_t orderBuildingAttack(size_t buildingIdx, bool targetIsVehicle, size_t targetIdx);
    size_t orderBuildingAttackBuilding(size_t buildingIdx, size_t targetBuildingIdx);
    size_t orderAttackBuilding(std::span<const size_t> soldierIds, size_t buildingIdx);
    size_t attackBuildingWithVehicles(std::span<const size_t> vehicleIds, size_t buildingIdx);
    std::vector<BuildingSnapshot> takeBuildingUpdates();
    void setBridges(std::vector<assets::BridgePlacement> bridges);
    std::vector<BridgeSnapshot> takeBridgeUpdates();
    bool canSupportBridge(std::span<const size_t> supporterVehicleIds, size_t bridgeIdx) const;
    size_t orderSupportBridge(std::span<const size_t> supporterVehicleIds, size_t bridgeIdx);
    static int bridgeStageForHp(int hp, int maxHp) noexcept;

    Snapshot takeSnapshot() const;
    bool loadSnapshot(const Snapshot& snap, std::vector<SoldierPose>* outSoldiers = nullptr);
    ExtraState takeExtraState() const;
    bool loadSnapshotWithExtra(const Snapshot& snap, const ExtraState* extra,
                               std::vector<SoldierPose>* outSoldiers = nullptr);
    static std::vector<std::uint8_t> serializeSnapshot(const Snapshot& snap);
    static std::optional<Snapshot> deserializeSnapshot(std::span<const std::uint8_t> bytes);

    void setAirSupport(std::array<std::array<assets::AirSupportSlot, 5>, 4> slots,
                       std::array<int, 4> paraGroups,
                       std::array<std::vector<assets::ParatrooperEntry>, 4> paraRosters);
    void addAirSupportPlanes(int owner, assets::AirMissionKind kind, int count);
    void addAirSupportSorties(int owner, assets::AirMissionKind kind, int count);
    std::array<assets::AirSupportSlot, 5> airSupportFor(int owner) const;
    std::array<assets::AirSupportSlot, 5> playerAirSupport() const;
    bool callAirSupport(assets::AirMissionKind kind, int targetX, int targetY, int owner = 0);
    std::vector<AirSortiePose> takeAirSorties();
    std::vector<AirStrikeEvent> takeAirStrikeEvents();

    unsigned countAliveInGroup(int owner, int group) const;
    unsigned countAliveByGroup(unsigned group) const;
    unsigned countAliveByPlayer(int owner) const;
    unsigned countByGroupMask(unsigned group, unsigned mask) const;
    unsigned countByGroupTeamMask(unsigned group, int team, unsigned mask) const;
    unsigned countByPlayerMask(int owner, unsigned mask) const;
    void collectUnitTilePositions(std::vector<std::tuple<int, int, int>>& out) const;

    std::vector<SoldierPose> step();
    std::optional<std::vector<std::uint8_t>> takeFogSnapshot();
    unsigned countInZone(std::span<const std::uint8_t> zones, unsigned zone, int owner = 0) const;
    unsigned countGroupInZone(std::span<const std::uint8_t> zones, unsigned zone, unsigned group) const;
    // 64-bit hash of every member that can influence future simulation
    // (tutorial_units_hash.cpp). Unlike Snapshot it covers orders, paths,
    // queues, targets, timers, cooldowns and RNG state.
    std::uint64_t stateHash() const;
private:
    friend class MissionRunner;
    struct Soldier {
        SubCell cell;
        int owner;
        std::vector<std::uint8_t> route;
        size_t cursor{};
        std::optional<InfantryStep> motion;
        std::optional<SubCell> goal;
        InfantryPathGrid::RouteState planning;
        bool needsRoute{};
        int hp{100};
        int maxHp{100};
        int ammo{40};
        int expa{0};
        int group{0};
        InfantryCombatDesc desc{};
        std::uint8_t spottedMask{0};
        std::optional<size_t> attackTarget;
        std::optional<size_t> attackVehicleTarget;
        std::optional<size_t> autoTarget;
        std::int16_t attackReload{0};
        std::uint8_t attackBurst{0};
        std::uint8_t direction{0};
        bool firing{false};
        std::uint8_t fireAnim{0};
        bool crouched{false};
        bool holdFire{false};
        bool isMedic{false};
        bool isOfficer{false};
        bool isAntiVehicle{false};
        bool isFlamer{false};
        bool isSniper{false};
        bool isSapper{false};
        bool isMortar{false};
        bool isHeavyMg{false};
        bool hasBinoculars{false};
        int binoSightBonus{6};
        bool isKamikaze{false};
        int mineAmmo{0};
        int kills{0};
        int atGrenades{0};
        bool binoculars{false};
        bool embarked{false};
        std::uint64_t lastFireTick{0};
        bool wasCamouflaged{false};
        std::optional<size_t> boardVehicleTarget;
        std::optional<size_t> captureVehicleTarget;
        std::optional<size_t> garrisonBuilding;
        std::optional<size_t> garrisonTarget;
        std::optional<size_t> attackBuildingTarget;
        std::optional<std::pair<int, int>> attackGround;
        std::optional<size_t> healTarget;
        std::optional<size_t> escortVehicleTarget;
        std::optional<size_t> escortSoldierTarget;
        std::optional<std::pair<int, int>> disarmMineTarget;
        bool attackMove{false};
        std::deque<std::pair<int, int>> queuedGoals;
        std::optional<std::pair<int, int>> patrolOrigin;
        std::optional<std::pair<int, int>> patrolDest;
        bool patrolToDest{true};
        unsigned lastReactTick{0};
        std::array<int, static_cast<std::size_t>(assets::TargetClass::Count)>
            attackPref{assets::makeDefaultAttackPref()};
        bool hasAttackPref{false};
        int suppressionTicks{0};
        bool wasSuppressed{false};
        int officerAuraRadius{160};
        int moraleRallyRate{3};
        int moraleResist{0};
        bool fearless{false};
        int resupplyCost{0};
        int canBeCrew{0};
        int unitRadius{5};
        bool canFireFromInside{true};
        bool crouchToFire{false};
        int expaOnHit{0};
        bool canGrenadeGround{false};
        bool canLayMines{false};
        bool movingSightPenalty{false};
        int maxMineAmmo{0};
        int getMineTime{0};
        int getMineTimeMax{0};
        int disarmWorkTicks{0};
        unsigned standStepBase{8u};
        unsigned standStepMax{8u};
        unsigned crouchStepBase{5u};
        unsigned crouchStepMax{5u};
        unsigned stepProgressBase() const {
            const unsigned lo = crouched ? crouchStepBase : standStepBase;
            const unsigned hi = crouched ? crouchStepMax : standStepMax;
            if (hi <= lo || expa <= 0) return lo;
            const unsigned bonus = static_cast<unsigned>(
                ((static_cast<std::uint64_t>(std::clamp(expa, 0, 0x8000)) * (hi - lo)) + 16384u) >> 15);
            return std::min(hi, lo + bonus);
        }
        bool isSuppressed() const { return suppressionTicks > 0; }
        int movementPriority() const;
    };
    struct Projectile {
        size_t attacker{};
        size_t target{};
        int rawDamage{};
        int hitPct{};
        int ticksRemaining{};
        bool isFlamer{false};
        bool isMortar{false};
        bool isHeavyMg{false};
    };
    struct Vehicle {
        std::string id;
        SubCell cell;
        int x{0}, y{0};
        double fx{0.0}, fy{0.0};
        unsigned hullDir{0}, turretDir{0};
        int hp{100}, maxHp{100}, ammo{100};
        int owner{0}, group{0};
        bool alive{true}, visible{true}, holdFire{false};
        bool armed{false};
        double speedPx{3.5};
        double speedPxMax{3.5};
        int sightTiles{14};
        int ammo2{0};
        bool hasPrimary{false};
        bool hasSecondary{false};
        double range{320.0};
        double deadzone{0.0};
        int cooldown{35};
        int damage{40};
        assets::HitType hitType{assets::HitType::Pierce};
        std::string sound1;
        double range2{0.0};
        int cooldown2{4};
        int burstShots2{8};
        int burstReload2{52};
        int burstCount2{0};
        int damage2{0};
        assets::HitType hitType2{assets::HitType::Machine};
        std::string sound2;
        std::array<assets::DirectionalArmor, static_cast<std::size_t>(assets::HitType::Count)> armor{};
        bool hasCatalogArmor{false};
        int passengerCapacity{0};
        std::vector<size_t> passengers;
        std::uint8_t spottedMask{0};
        std::optional<std::pair<int, int>> moveTarget;
        std::deque<std::pair<int, int>> waypoints;
        std::optional<std::pair<bool, size_t>> attackTarget;
        int fireCooldown{0};
        bool chasingAttackTarget{false};
        bool wasMoving{false};
        int maxAmmo{0};
        int maxAmmo2{0};
        int moveDamageHp{0};
        bool isSupply{false};
        bool isAmbulance{false};
        bool isArtillery{false};
        bool attackMove{false};
        bool attackMoveEngaging{false};
        std::deque<std::pair<int, int>> queuedOrders;
        std::optional<std::pair<int, int>> attackGround;
        std::string towWeightClass;
        std::array<double, 6> towSpeedByClass{};
        std::optional<size_t> towedVehicle;
        std::optional<size_t> towedBy;
        std::optional<size_t> pendingTowGun;
        std::optional<size_t> attackBuildingTarget;
        bool canMoveBackward{false};
        double backSpeedPx{0.0};
        bool reverseMove{false};
        int expa{0};
        int kills{0};
        unsigned lastReactTick{0};
        bool isAntiAir{false};
        unsigned aaCooldown{0};
        bool hasCrew{true};
        int burstShots1{1};
        int burstReload1{60};
        int burstCount1{0};
        bool isRocketArtillery{false};
        bool isCasemate{false};
        bool isKamikaze{false};
        unsigned turnStep32{2u};
        bool canCrushInfantry{false};
        bool weapon1Flamer{false};
        bool weapon2Flamer{false};
        std::optional<std::pair<int, int>> patrolOrigin;
        std::optional<std::pair<int, int>> patrolDest;
        bool patrolToDest{true};
        std::optional<unsigned> facingTargetDir;
        std::optional<size_t> supportTargetVehicle;
        std::optional<size_t> supportTargetBuilding;
        std::optional<size_t> supportTargetBridge;
        std::optional<size_t> supportTargetSoldier;
        std::optional<size_t> escortVehicleTarget;
        std::optional<size_t> escortSoldierTarget;
        unsigned muzzleRevealTicks{0};
        int bonusSight{0};
        int marchSightBonus{0};
        bool marchEnabled{false};
        bool hatchOpen{false};
        unsigned lastCombatTick{0};
        std::array<assets::DirectionalArmor, static_cast<std::size_t>(assets::HitType::Count)>
            crewProtection{assets::makeDefaultCrewProtection()};
        bool hasCrewProtection{false};
        int crewHp{140};
        int maxCrewHp{140};
        std::string crewUnit;
        bool canSelfRepair{false};
        int crewNumber{2};
        int attackCrew{1};
        int moveCrew{1};
        int cookOffDamage{0};
        double cookOffRadius{0.0};
        std::uint16_t forbiddenTerrainMask{0x00F0u};
        bool isNaval{false};
        bool isCrate{false};
        bool hasFiniteSupply{false};
        std::string targetType;
        std::array<int, static_cast<std::size_t>(assets::TargetClass::Count)>
            attackPref{assets::makeDefaultAttackPref()};
        bool hasAttackPref{false};
        bool isWheeled{false};
        bool isTracked{false};
        bool isTrain{false};
        unsigned turretStep32{4};
        bool hasGunTurnDelay{false};
        bool canBeCrushed{false};
        bool passengersCanFire{false};
        int accuracy1{0};
        int accuracy1Min{0};
        int shotExpa1{0};
        int shotExpa2{0};
        int bonusShotRange{0};
        bool isPropaganda{false};
        bool soldiersOnArmor{false};
        int resupplyCost1{0};
        int resupplyCost2{0};
        int repairCost{0};
        int gunShotWait{0};
        int gunSettleTicks{0};
        int unitRadius{0};
        int ammoPerShot1{1};
        int ammoPerShot2{1};
        double crewVulnerability{0.0};
        int expaOnHit{0};
        int moraleResist{0};
        int suppressionTicks{0};
        bool wasSuppressed{false};
        bool crewCanBeHealed{false};
        bool indirectArc1{false};
        int craterScale1{0};
        bool isHeavyAntiAir{false};
        int bonusShotDeadZone{0};
        int scanDelayTicks{0};
        unsigned lastAutoScanTick{0};
        int crushExpaBonus{0};
        int expaReload{0};
        int expaReloadMax{0};
        int ammoRegenDelay{0};
        int ammoRegenDelayMax{0};
        int healDelay{0};
        int healDelayMax{0};
        int moraleRndMove{0};
        int moraleRage{0};
        unsigned lastPanicTick{0};

        std::string_view effectiveTargetType() const {
            if (!targetType.empty()) return targetType;
            return canCrushInfantry ? (maxHp >= 500 ? "heavytank" : "medtank") : (armed ? "car" : "soft");
        }
        int activeCrewCount() const {
            if (!hasCrew) return 0;
            const int c = std::clamp(crewNumber, 1, 8);
            const int mh = std::max(1, maxCrewHp);
            return std::clamp((crewHp * c + mh - 1) / mh, 1, c);
        }
        bool isCrewImpaired() const {
            return hasCrew && attackCrew > 1 && activeCrewCount() < attackCrew;
        }
        bool isMoveImpaired() const {
            return hasCrew && moveCrew > 1 && activeCrewCount() < moveCrew;
        }
        bool isSuppressed() const {
            return hasCrew && suppressionTicks >= 6;
        }
    };
    struct Building {
        int x{}, y{};
        int left{}, top{}, width{}, height{};
        int hp{300}, maxHp{300}, capacity{4};
        int owner{-1};
        bool destroyed{false};
        SubCell entryCell{};
        std::vector<size_t> occupants;
    };
    struct Bridge {
        int x{}, y{};
        int left{}, top{}, width{}, height{};
        int hp{3500}, maxHp{3500};
        int stage{0};
        bool destroyed{false};
        std::vector<assets::BridgeSubCell> cells;
    };
    InfantryPathGrid grid_;
    int originX_{4096};
    int mapPixelW_{8192};
    int mapPixelH_{4096};
    std::vector<Soldier> soldiers_;
    std::vector<Vehicle> vehicles_;
    std::vector<VehiclePose> vehiclePoses_;
    std::uint32_t randomState_{0x76321f};
    std::optional<FogGrid> fog_;
    int difficulty_{2};
    bool autoEngage_{false};
    std::shared_ptr<const assets::UnitCatalog> catalog_;
    bool fogDirty_{false};
    bool fogSnapshotDirty_{false};
    std::uint32_t combatRandomState_{0x12345678u};
    unsigned tick_{0};
    std::vector<Projectile> projectiles_;
    std::array<int, 4> playerTeams_{0, 1, 2, 3};
    std::array<std::optional<unsigned>, 4> lastHitTick_{};
    std::array<std::optional<unsigned>, 100> lastGroupHitTick_{};
    std::vector<std::uint8_t> mines_;
    std::vector<MineEvent> mineEvents_;
    std::vector<Building> buildings_;
    std::vector<BuildingSnapshot> buildingUpdates_;
    std::vector<Bridge> bridges_;
    std::vector<BridgeSnapshot> bridgeUpdates_;
    struct ActiveSortie {
        std::uint32_t id{};
        assets::AirMissionKind kind{assets::AirMissionKind::Bomber};
        std::string planeId;
        int owner{0};
        double fx{}, fy{}, vx{}, vy{};
        int targetX{}, targetY{};
        unsigned dir32{0};
        int hp{200}, maxHp{200};
        int payloadsLeft{0};
        int totalPayloads{0};
        int bombDamage{380};
        int strafeDamage{180};
        int airToAirDamage{75};
        int sightTiles{18};
        int scoreValue{500};
        bool isLeafletBomber{false};
        int altitude{280};
        bool isRocketAttacker{false};
        assets::HitType strafeHitType{assets::HitType::Machine};
        unsigned reconTick{0};
    };
    std::array<std::array<assets::AirSupportSlot, 5>, 4> airSupport_{};
    std::array<int, 4> paraGroups_{};
    std::array<std::vector<assets::ParatrooperEntry>, 4> paraRosters_{};
    std::vector<ActiveSortie> sorties_;
    std::vector<AirSortiePose> sortiePoses_;
    std::vector<AirStrikeEvent> airEvents_;
    std::uint32_t nextSortieId_{1};
    std::vector<ReserveUnit> reserveUnits_;
    std::vector<std::string> soldierIds_;
    std::vector<EvacuationOrder> evacuatingSoldiers_;
    std::vector<EvacuationOrder> evacuatingVehicles_;
    int cookOffDepth_{0};
    WeatherKind weather_{WeatherKind::Clear};
    // View-only presentation seat (0..3); NEVER read by simulation logic or included in stateHash().
    int viewerOwner_{0};
    // Transient command executor seat (0..3) set via ScopedCommandActor; always 0 at rest.
    int commandActor_{0};
    bool commandActorActive_{false};
    bool multiplayerSymmetry_{false};
    struct Destination { size_t soldier; int mapX, mapY; };
    std::vector<Destination> formationTargets(std::span<const size_t>, int mapX, int mapY) const;
    std::vector<std::pair<size_t, std::pair<int, int>>> vehicleFormationTargets(
        std::span<const size_t> eligibleIds, int mapX, int mapY) const;
    bool mapBounds(int x, int y) const;
    size_t appendSoldier(const assets::Infantry& unit, SubCell cell, int expa = 0);
    size_t appendVehicle(const assets::VehicleUnit& unit);
    bool toggleBinocularsWithPoses(std::span<const size_t> ids, std::vector<SoldierPose>* changed);
    void refreshRoutes();
    void refreshSpotting(std::vector<SoldierPose>* changed = nullptr, bool emitVehiclePoses = true);
    bool startNext(Soldier&);
    void clearMovingSightPenalty(Soldier&);
    std::uint32_t nextCombatRandom(std::uint32_t mask);
    int scaleVehicleDamageForDifficulty(int targetOwner, int rawDamage) const;
    std::pair<int, int> soldierWorld(const Soldier&) const;
    bool soldierAtCell(SubCell cell) const;
    bool vehicleAtCell(SubCell cell) const;
    void emitVehiclePose(size_t index, bool fired = false, int targetX = 0, int targetY = 0,
                         bool firedSecondary = false, std::string_view fireSound = {},
                         bool heavyImpact = false, bool hitVehicle = false,
                         bool repaired = false, bool resupplied = false, bool captured = false,
                         bool firedRocket = false, bool firedFlamer = false,
                         bool decrewed = false);
    bool isSoldierCamouflaged(const Soldier& s) const;
    int officerAuraDecayAt(int owner, double x, double y) const;
    int officerAuraDecay(size_t soldierIdx) const;
    bool hasNearbyFriendlyOfficer(size_t soldierIdx) const;
    bool isVehicleUnbuttoned(const Vehicle& v) const;
    void applySuppression(size_t soldierIdx, int addTicks, bool allowGarrisoned = false);
    void applyVehicleSuppression(size_t vehicleIdx, int addTicks, int threatX = -1, int threatY = -1);
    void applySoldierShotReload(Soldier& s);
    void applyFlameSplash(int impactX, int impactY, int rawDamage, int attackerOwner,
                          int attackerSoldierIdx, int attackerVehicleIdx, int skipSoldierIdx,
                          std::vector<SoldierPose>& changed, int skipVehicleIdx = -1);
    void damageTankDesantRiders(size_t vehicleIdx, int rawDamage, assets::HitType hitType,
                                std::vector<SoldierPose>& changed,
                                int attackerSoldierIdx = -1, int attackerVehicleIdx = -1);
    void applyVehicleCrewDamage(size_t vehicleIdx, int attackerX, int attackerY,
                                int rawDamage, assets::HitType hitType,
                                std::vector<SoldierPose>& changed,
                                int attackerSoldierIdx = -1, int attackerVehicleIdx = -1);
    void applyMortarSplash(int impactX, int impactY, int rawDamage, int attackerOwner,
                           int attackerSoldierIdx, int skipSoldierIdx,
                           std::vector<SoldierPose>& changed, int skipVehicleIdx = -1);
    void detonateKamikazeSoldier(size_t index, int impactX, int impactY,
                                 std::optional<size_t> directVehIdx,
                                 std::optional<size_t> directBldIdx,
                                 std::vector<SoldierPose>& changed);
    bool canVehicleStepTo(const Vehicle& v, double nextFx, double nextFy, SubCell& outCell) const;
    bool isVehicleTargetValidFor(int attackerOwner, bool targetIsVehicle, size_t targetIdx) const;
    bool orderSoldierInternal(size_t index, int mapX, int mapY);
    void moveVehicleInternal(Vehicle& v, int mapX, int mapY);
    std::size_t dispatchEvacuationOrders(std::vector<size_t> soldierIds,
                                         std::vector<SubCell> soldierSources,
                                         std::vector<size_t> vehIds,
                                         int clampedX, int clampedY);
    void detachTowedGun(size_t tractorIdx);
    void destroyVehicle(size_t vehicleIdx, std::vector<SoldierPose>& changed);
    void stepTowing();
    SubCell findBuildingEntryCell(int mapX, int mapY) const;
    void emitBuildingUpdate(size_t bIdx, bool damaged = false, bool collapsed = false);
    void stepGarrisons(std::vector<SoldierPose>& changed);
    void damageBuilding(size_t bIdx, int rawDamage, assets::HitType hitType, int attackerOwner,
                        std::vector<SoldierPose>& changed);
    void emitBridgeUpdate(size_t brIdx, bool damaged = false, bool collapsed = false, bool repaired = false);
    void applyBridgeStageTransition(size_t brIdx, int prevStage, int newStage, bool killPlungedUnits,
                                    std::vector<SoldierPose>* changed);
    void damageBridge(size_t brIdx, int rawDamage, assets::HitType hitType, int attackerOwner,
                      std::vector<SoldierPose>& changed);
    void damageBridgesAtImpact(int impactX, int impactY, double radius, int rawDamage,
                               assets::HitType hitType, int attackerOwner,
                               std::vector<SoldierPose>& changed);
    double distanceToBridge(const Bridge& br, double px, double py) const;
    std::pair<int, int> bridgeRepairApproachPoint(const Bridge& br, const Vehicle& v) const;
    void bailoutPassengers(size_t vehicleIdx, std::vector<SoldierPose>& changed);
    void stepBoarding(std::vector<SoldierPose>& changed);
    void stepCaptures(std::vector<SoldierPose>& changed);
    void recordHit(int owner, int group = -1);
    std::optional<std::pair<int, int>> findDetectedHostileMineNear(int mapX, int mapY, double maxDistPx,
                                                                   int viewerOwner = 0) const;
    void triggerMineAtTile(int tx, int ty, std::vector<SoldierPose>& changed, bool forceAny = false);
    void stepMines(std::vector<SoldierPose>& changed);
    void stepSoldierAttackVehicle(Soldier& s, size_t index, std::vector<SoldierPose>& changed);
    void stepCombat(std::vector<SoldierPose>& changed);
    void stepOpenTopPassengerFire(std::vector<SoldierPose>& changed);
    void stepVehicles(std::vector<SoldierPose>& changed);
    void markVehicleCombat(Vehicle& veh);
    void stepVehicleCombat(std::vector<SoldierPose>& changed);
    void stepAirSupport(std::vector<SoldierPose>& changed);
    void reactToAttack(int victimOwner, int victimGroup, int attackerX, int attackerY,
                       std::optional<size_t> victimVehicleIdx = std::nullopt);
    void stepMedicHeal(std::vector<SoldierPose>& changed);
    void stepSupplyAndRepair(std::vector<SoldierPose>& changed);
    void stepSupportApproach();
    void stepEscort();
    size_t dispatchEscort(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                          double targetX, double targetY, std::optional<size_t> targetVehicleIdx,
                          std::optional<size_t> targetSoldierIdx);
};
}
