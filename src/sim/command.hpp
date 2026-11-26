#pragma once
#include "sim/tutorial_units.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Value types for every simulation mutation that crosses MissionRunner. A
// command is applied between ticks; a log of {tick, command} pairs replayed on
// a manual-clock runner reproduces the live simulation without a wall clock.
//
// Each struct carries a stable serialization tag. Fields are listed in the
// same order by the serializer in command.cpp; append new fields at the end
// and add new commands at the end of the Command variant.
namespace ss2::cmd {
using Ids = std::vector<std::size_t>;

// Infantry orders.
struct Move {
    static constexpr std::string_view tag = "move";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool append{};
    bool operator==(const Move&) const = default;
};
struct AttackMove {
    static constexpr std::string_view tag = "attack-move";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool append{};
    bool operator==(const AttackMove&) const = default;
};
struct Patrol {
    static constexpr std::string_view tag = "patrol";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool operator==(const Patrol&) const = default;
};
struct Scatter {
    static constexpr std::string_view tag = "scatter";
    using Result = std::size_t;
    Ids ids;
    bool operator==(const Scatter&) const = default;
};
struct Attack {
    static constexpr std::string_view tag = "attack";
    using Result = std::optional<std::size_t>;
    Ids ids;
    std::size_t target{};
    bool operator==(const Attack&) const = default;
};
struct Stop {
    static constexpr std::string_view tag = "stop";
    using Result = std::size_t;
    Ids ids;
    bool operator==(const Stop&) const = default;
};
struct ToggleHoldFire {
    static constexpr std::string_view tag = "toggle-hold-fire";
    using Result = std::optional<bool>;
    Ids ids;
    bool operator==(const ToggleHoldFire&) const = default;
};
struct ToggleCrouch {
    static constexpr std::string_view tag = "toggle-crouch";
    using Result = std::optional<bool>;
    Ids ids;
    bool operator==(const ToggleCrouch&) const = default;
};
struct ToggleBinoculars {
    static constexpr std::string_view tag = "toggle-binoculars";
    using Result = std::optional<bool>;
    Ids ids;
    bool operator==(const ToggleBinoculars&) const = default;
};
struct SpawnInfantry {
    static constexpr std::string_view tag = "spawn-infantry";
    using Result = std::size_t;
    assets::Infantry unit{};
    int expa{};
    bool operator==(const SpawnInfantry&) const = default;
};

// Vehicle orders.
struct MoveVehicles {
    static constexpr std::string_view tag = "move-vehicles";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool append{};
    bool operator==(const MoveVehicles&) const = default;
};
struct ReverseMoveVehicles {
    static constexpr std::string_view tag = "reverse-move-vehicles";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool operator==(const ReverseMoveVehicles&) const = default;
};
struct FaceVehicles {
    static constexpr std::string_view tag = "face-vehicles";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool operator==(const FaceVehicles&) const = default;
};
struct AttackMoveVehicles {
    static constexpr std::string_view tag = "attack-move-vehicles";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool append{};
    bool operator==(const AttackMoveVehicles&) const = default;
};
struct PatrolVehicles {
    static constexpr std::string_view tag = "patrol-vehicles";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool operator==(const PatrolVehicles&) const = default;
};
struct AttackWithVehicles {
    static constexpr std::string_view tag = "attack-with-vehicles";
    using Result = std::optional<std::size_t>;
    Ids ids;
    bool targetIsVehicle{};
    std::size_t target{};
    bool operator==(const AttackWithVehicles&) const = default;
};
struct AttackVehicle {
    static constexpr std::string_view tag = "attack-vehicle";
    using Result = std::optional<std::size_t>;
    Ids ids;
    std::size_t targetVehicle{};
    bool operator==(const AttackVehicle&) const = default;
};
struct StopVehicles {
    static constexpr std::string_view tag = "stop-vehicles";
    using Result = std::size_t;
    Ids ids;
    bool operator==(const StopVehicles&) const = default;
};
struct SupportVehicle {
    static constexpr std::string_view tag = "support-vehicle";
    using Result = std::optional<std::size_t>;
    Ids supporters;
    std::size_t targetVehicle{};
    bool operator==(const SupportVehicle&) const = default;
};
struct SupportBuilding {
    static constexpr std::string_view tag = "support-building";
    using Result = std::optional<std::size_t>;
    Ids supporters;
    std::size_t building{};
    bool operator==(const SupportBuilding&) const = default;
};
struct HealSoldier {
    static constexpr std::string_view tag = "heal-soldier";
    using Result = std::optional<std::size_t>;
    Ids medics, ambulances;
    std::size_t targetSoldier{};
    bool operator==(const HealSoldier&) const = default;
};
struct EscortVehicle {
    static constexpr std::string_view tag = "escort-vehicle";
    using Result = std::optional<std::size_t>;
    Ids soldiers, vehicles;
    std::size_t targetVehicle{};
    bool operator==(const EscortVehicle&) const = default;
};
struct EscortSoldier {
    static constexpr std::string_view tag = "escort-soldier";
    using Result = std::optional<std::size_t>;
    Ids soldiers, vehicles;
    std::size_t targetSoldier{};
    bool operator==(const EscortSoldier&) const = default;
};
struct ToggleVehicleHoldFire {
    static constexpr std::string_view tag = "toggle-vehicle-hold-fire";
    using Result = std::optional<bool>;
    Ids ids;
    bool operator==(const ToggleVehicleHoldFire&) const = default;
};
struct ToggleMarchMode {
    static constexpr std::string_view tag = "toggle-march-mode";
    using Result = std::optional<bool>;
    Ids vehicles;
    bool operator==(const ToggleMarchMode&) const = default;
};
struct BoardVehicle {
    static constexpr std::string_view tag = "board-vehicle";
    using Result = std::optional<std::size_t>;
    Ids soldiers;
    std::size_t vehicle{};
    bool operator==(const BoardVehicle&) const = default;
};
struct EmbarkImmediate {
    static constexpr std::string_view tag = "embark-immediate";
    using Result = bool;
    std::size_t soldier{}, vehicle{};
    bool operator==(const EmbarkImmediate&) const = default;
};
struct UnloadVehicles {
    static constexpr std::string_view tag = "unload-vehicles";
    using Result = std::size_t;
    Ids vehicles;
    bool operator==(const UnloadVehicles&) const = default;
};
struct CaptureVehicle {
    static constexpr std::string_view tag = "capture-vehicle";
    using Result = std::optional<std::size_t>;
    Ids soldiers;
    std::size_t vehicle{};
    bool operator==(const CaptureVehicle&) const = default;
};
struct AbandonVehicles {
    static constexpr std::string_view tag = "abandon-vehicles";
    using Result = std::vector<assets::Placement>;
    Ids vehicles;
    bool operator==(const AbandonVehicles&) const = default;
};
struct TowVehicle {
    static constexpr std::string_view tag = "tow-vehicle";
    using Result = std::optional<std::size_t>;
    std::size_t tractor{}, gun{};
    bool operator==(const TowVehicle&) const = default;
};
struct UnhitchVehicles {
    static constexpr std::string_view tag = "unhitch-vehicles";
    using Result = std::size_t;
    Ids vehicles;
    bool operator==(const UnhitchVehicles&) const = default;
};
struct SpawnVehicle {
    static constexpr std::string_view tag = "spawn-vehicle";
    using Result = std::size_t;
    assets::VehicleUnit unit{};
    bool operator==(const SpawnVehicle&) const = default;
};

// Reserves, ground fire, mines and group orders.
struct AddReserveUnit {
    static constexpr std::string_view tag = "add-reserve-unit";
    using Result = void;
    TutorialUnits::ReserveUnit entry{};
    bool operator==(const AddReserveUnit&) const = default;
};
struct TakeReserveUnits {
    static constexpr std::string_view tag = "take-reserve-units";
    using Result = std::vector<TutorialUnits::ReserveUnit>;
    unsigned group{};
    int pool{-1};
    std::size_t maxCount{static_cast<std::size_t>(-1)};
    bool operator==(const TakeReserveUnits&) const = default;
};
struct AttackGroundVehicles {
    static constexpr std::string_view tag = "attack-ground-vehicles";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool operator==(const AttackGroundVehicles&) const = default;
};
struct AttackGroundSoldiers {
    static constexpr std::string_view tag = "attack-ground-soldiers";
    using Result = std::optional<std::size_t>;
    Ids ids;
    int x{}, y{};
    bool operator==(const AttackGroundSoldiers&) const = default;
};
struct LayMine {
    static constexpr std::string_view tag = "lay-mine";
    using Result = std::size_t;
    Ids soldiers;
    bool operator==(const LayMine&) const = default;
};
struct DisarmMine {
    static constexpr std::string_view tag = "disarm-mine";
    using Result = std::optional<std::size_t>;
    Ids soldiers;
    int x{}, y{};
    bool operator==(const DisarmMine&) const = default;
};
struct GroupMove {
    static constexpr std::string_view tag = "group-move";
    using Result = std::size_t;
    unsigned group{};
    int x{}, y{};
    bool onlyNonPlayer{};
    bool operator==(const GroupMove&) const = default;
};

// Buildings.
struct GarrisonBuilding {
    static constexpr std::string_view tag = "garrison-building";
    using Result = std::optional<std::size_t>;
    Ids soldiers;
    std::size_t building{};
    bool operator==(const GarrisonBuilding&) const = default;
};
struct EvacuateBuilding {
    static constexpr std::string_view tag = "evacuate-building";
    using Result = std::size_t;
    std::size_t building{};
    bool operator==(const EvacuateBuilding&) const = default;
};
struct ToggleBuildingHoldFire {
    static constexpr std::string_view tag = "toggle-building-hold-fire";
    using Result = std::optional<bool>;
    std::size_t building{};
    bool operator==(const ToggleBuildingHoldFire&) const = default;
};
struct ToggleBuildingBinoculars {
    static constexpr std::string_view tag = "toggle-building-binoculars";
    using Result = std::optional<bool>;
    std::size_t building{};
    bool operator==(const ToggleBuildingBinoculars&) const = default;
};
struct BuildingAttack {
    static constexpr std::string_view tag = "building-attack";
    using Result = std::optional<std::size_t>;
    std::size_t building{};
    bool targetIsVehicle{};
    std::size_t target{};
    bool operator==(const BuildingAttack&) const = default;
};
struct BuildingAttackBuilding {
    static constexpr std::string_view tag = "building-attack-building";
    using Result = std::optional<std::size_t>;
    std::size_t building{}, targetBuilding{};
    bool operator==(const BuildingAttackBuilding&) const = default;
};
struct AttackBuilding {
    static constexpr std::string_view tag = "attack-building";
    using Result = std::optional<std::size_t>;
    Ids soldiers;
    std::size_t building{};
    bool operator==(const AttackBuilding&) const = default;
};
struct AttackBuildingWithVehicles {
    static constexpr std::string_view tag = "attack-building-with-vehicles";
    using Result = std::optional<std::size_t>;
    Ids vehicles;
    std::size_t building{};
    bool operator==(const AttackBuildingWithVehicles&) const = default;
};

// Air support, save state and mission flow.
struct CallAirSupport {
    static constexpr std::string_view tag = "call-air-support";
    using Result = bool;
    assets::AirMissionKind kind{assets::AirMissionKind::Bomber};
    int x{}, y{};
    bool operator==(const CallAirSupport&) const = default;
};
struct LoadSnapshot {
    static constexpr std::string_view tag = "load-snapshot";
    using Result = bool;
    TutorialUnits::Snapshot snapshot{};
    bool operator==(const LoadSnapshot&) const = default;
};
// Dismisses a modal dialog; while modal_ is set the runner neither steps nor
// accepts most orders.
struct Acknowledge {
    static constexpr std::string_view tag = "acknowledge";
    using Result = void;
    bool operator==(const Acknowledge&) const = default;
};
// Flips paused_, which gates stepping exactly like modal_.
struct TogglePause {
    static constexpr std::string_view tag = "toggle-pause";
    using Result = bool;
    bool operator==(const TogglePause&) const = default;
};
// Resigns a multiplayer seat (verified against the acting seat in lockstep applyAs).
struct Resign {
    static constexpr std::string_view tag = "resign";
    using Result = void;
    std::uint8_t seat{0};
    bool operator==(const Resign&) const = default;
};
struct SupportBridge {
    static constexpr std::string_view tag = "support-bridge";
    using Result = std::optional<std::size_t>;
    Ids supporters;
    std::size_t bridge{};
    bool operator==(const SupportBridge&) const = default;
};
struct UnloadSingleSoldier {
    static constexpr std::string_view tag = "unload-single-soldier";
    using Result = bool;
    std::size_t vehicle{}, soldier{};
    bool operator==(const UnloadSingleSoldier&) const = default;
};
struct EvacuateSingleSoldier {
    static constexpr std::string_view tag = "evacuate-single-soldier";
    using Result = bool;
    std::size_t building{}, soldier{};
    bool operator==(const EvacuateSingleSoldier&) const = default;
};

struct QueueSoldierOrder {
    static constexpr std::string_view tag = "queue-soldier-order";
    using Result = size_t;
    Ids soldiers;
    std::uint8_t op{0};
    int targetX{}, targetY{};
    std::size_t targetIndex{0};
    bool operator==(const QueueSoldierOrder&) const = default;
};

struct QueueVehicleOrder {
    static constexpr std::string_view tag = "queue-vehicle-order";
    using Result = size_t;
    Ids vehicles;
    std::uint8_t op{0};
    int targetX{}, targetY{};
    unsigned dir32{0};
    bool operator==(const QueueVehicleOrder&) const = default;
};

using Command = std::variant<
    Move, AttackMove, Patrol, Scatter, Attack, Stop, ToggleHoldFire, ToggleCrouch, ToggleBinoculars, SpawnInfantry,
    MoveVehicles, ReverseMoveVehicles, FaceVehicles, AttackMoveVehicles, PatrolVehicles, AttackWithVehicles,
    AttackVehicle, StopVehicles, SupportVehicle, SupportBuilding, HealSoldier, EscortVehicle, EscortSoldier,
    ToggleVehicleHoldFire, ToggleMarchMode, BoardVehicle, EmbarkImmediate, UnloadVehicles, CaptureVehicle,
    AbandonVehicles, TowVehicle, UnhitchVehicles, SpawnVehicle, AddReserveUnit, TakeReserveUnits,
    AttackGroundVehicles, AttackGroundSoldiers, LayMine, DisarmMine, GroupMove, GarrisonBuilding, EvacuateBuilding,
    ToggleBuildingHoldFire, ToggleBuildingBinoculars, BuildingAttack, BuildingAttackBuilding, AttackBuilding,
    AttackBuildingWithVehicles, CallAirSupport, LoadSnapshot, Acknowledge, TogglePause, Resign, SupportBridge,
    UnloadSingleSoldier, EvacuateSingleSoldier, QueueSoldierOrder, QueueVehicleOrder>;

// `tick` is the simulation tick at which the command was applied: after the
// step that produced `tick` and before the next one.
struct RecordedCommand {
    std::uint32_t tick{};
    Command command;
    bool operator==(const RecordedCommand&) const = default;
};

std::string_view tagOf(const Command&);

// One line per command: `<tag> <tick> <fields...>`, space separated, no
// trailing newline. Integers are decimal, bools 0/1, vectors are a count
// followed by the elements, and strings are a `'` followed by the bytes with
// space, `%` and non-printable bytes percent-encoded. The round trip is exact.
std::string toLine(const RecordedCommand&);
std::optional<RecordedCommand> fromLine(std::string_view line);

// A log is one toLine() per line, each terminated by '\n'. fromText ignores
// blank lines and lines starting with '#', and fails on any malformed line.
std::string toText(std::span<const RecordedCommand> log);
std::optional<std::vector<RecordedCommand>> fromText(std::string_view text);

// Self-contained `.ss2rep` replay file: optional `# key: value` metadata header
// lines (ignored by plain fromText) followed by the command lines.
struct ReplayFile {
    std::string mission;
    std::optional<int> difficulty;
    std::optional<bool> autoEngage;
    std::uint32_t endTick{0};
    std::optional<std::uint64_t> endStateHash;
    std::map<unsigned, std::vector<assets::ReinforcementUnit>> initialSavedPools;
    std::vector<RecordedCommand> commands;
    bool operator==(const ReplayFile&) const = default;
};

std::string toReplayFileText(const ReplayFile&);
std::optional<ReplayFile> fromReplayFileText(std::string_view text);

// Interactive transport controller for watching a ReplayFile tick-by-tick on a
// ManualClock runner without perturbing the simulation's internal stateHash().
struct ReplayTransport {
    static constexpr std::array<int, 5> kSpeedSteps{{50, 100, 200, 400, 800}};
    bool paused{false};
    int speedIndex{1};      // index into kSpeedSteps (default 100% = 1x)
    int viewerSeat{0};      // 0..3 = Player 1..4 fog perspective, -1 = All Vision
    std::size_t cursor{0};
    double tickAccumulator{0.0};
    bool finished{false};
    bool hashVerified{false};
    bool hashMismatch{false};

    int speedPercent() const noexcept { return kSpeedSteps[static_cast<std::size_t>(std::clamp(speedIndex, 0, 4))]; }
    std::string_view speedLabel() const noexcept;
    void togglePause() noexcept { paused = !paused; }
    void stepFaster() noexcept { speedIndex = std::min(4, speedIndex + 1); }
    void stepSlower() noexcept { speedIndex = std::max(0, speedIndex - 1); }
    void cycleViewerSeat(int maxSeats = 2) noexcept;
    unsigned consumeTicks(double elapsedSeconds, double baseTickPeriodSec = 0.036) noexcept;
    std::string formatBanner(std::uint32_t currentTick, std::uint32_t endTick) const;
};
}
