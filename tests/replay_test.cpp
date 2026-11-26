// Command log round trip, manual-clock determinism and live-vs-replay parity
// for MissionRunner. Runs on synthetic data; pass a Gold 2.2 directory to also
// replay a Tutorial I session with its real script and dialogs.
#include "assets/mission.hpp"
#include "assets/unit_catalog.hpp"
#include "assets/vehicles.hpp"
#include "sim/command.hpp"
#include "sim/mission_runner.hpp"
#include "sim/tutorial_units.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace cmd = ss2::cmd;
using Ids = cmd::Ids;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

// One of every command, with every field set away from its default so a
// field missing from the serializer shows up as a round-trip mismatch.
std::vector<cmd::Command> everyCommand() {
    const Ids a{3, 1, 4}, b{1, 5}, none{};
    ss2::assets::Infantry inf{"a b%c\xff\t'", -4100, 2050, 77, 33, 9, 2, 1, 2, 3, 4, 5u, 6u, true, false, 42};
    ss2::assets::VehicleUnit veh{"tiger heavy", 4100, -2050, 66, 22, 8, 3, 7u, 9u, 1, 2, 3, 4, false, false, false, 11};
    ss2::TutorialUnits::ReserveUnit reserve{"", 3, 7, 50, 25, 12, 2};
    ss2::TutorialUnits::Snapshot snap;
    snap.tick = 1234;
    snap.playerTeams = {1, 0, 3, 2};
    snap.mines = {0, 1, 255};
    snap.fog = {7, 0, 9, 128};
    snap.soldiers.push_back({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, true, false, true, false, 13, 14});
    snap.soldiers.push_back({-1, -2, 0, 100, 0, 0, 0, 0, 0, 1, 2, 255, false, true, false, true, -1, 0});
    snap.vehicles.push_back({100, 200, 3u, 4u, 5, 6, 7, 8, 9, 10, 11, 12, true, true, {1, 2, 3}, 4, 5, false, 6});
    snap.buildings.push_back({300, 2, true, {9, 8}});
    snap.buildings.push_back({0, -1, false, {}});
    snap.airReady[1] = {1, 2, 3, 4, 5};
    snap.airReady[3] = {-5, 0, 0, 0, 7};
    return {
        cmd::Move{a, 10, -20, true},
        cmd::AttackMove{b, -1, 2, true},
        cmd::Patrol{a, 5, 6},
        cmd::Scatter{b},
        cmd::Attack{a, 17},
        cmd::Stop{none},
        cmd::ToggleHoldFire{a},
        cmd::ToggleCrouch{b},
        cmd::ToggleBinoculars{a},
        cmd::SpawnInfantry{inf, 31},
        cmd::MoveVehicles{a, 7, 8, true},
        cmd::ReverseMoveVehicles{b, 9, 10},
        cmd::FaceVehicles{a, 11, 12},
        cmd::AttackMoveVehicles{b, 13, 14, true},
        cmd::PatrolVehicles{a, 15, 16},
        cmd::AttackWithVehicles{b, true, 18},
        cmd::AttackVehicle{a, 19},
        cmd::StopVehicles{b},
        cmd::SupportVehicle{a, 20},
        cmd::SupportBuilding{b, 21},
        cmd::HealSoldier{a, b, 22},
        cmd::EscortVehicle{b, a, 23},
        cmd::EscortSoldier{a, b, 24},
        cmd::ToggleVehicleHoldFire{b},
        cmd::ToggleMarchMode{a},
        cmd::BoardVehicle{b, 25},
        cmd::EmbarkImmediate{26, 27},
        cmd::UnloadVehicles{a},
        cmd::CaptureVehicle{b, 28},
        cmd::AbandonVehicles{a},
        cmd::TowVehicle{29, 30},
        cmd::UnhitchVehicles{b},
        cmd::SpawnVehicle{veh},
        cmd::AddReserveUnit{reserve},
        cmd::TakeReserveUnits{7, 2, 5},
        cmd::AttackGroundVehicles{a, 31, 32},
        cmd::AttackGroundSoldiers{b, 33, 34},
        cmd::LayMine{a},
        cmd::DisarmMine{b, 35, 36},
        cmd::GroupMove{37, 38, 39, true},
        cmd::GarrisonBuilding{a, 40},
        cmd::EvacuateBuilding{41},
        cmd::ToggleBuildingHoldFire{42},
        cmd::ToggleBuildingBinoculars{43},
        cmd::BuildingAttack{44, true, 45},
        cmd::BuildingAttackBuilding{46, 47},
        cmd::AttackBuilding{b, 48},
        cmd::AttackBuildingWithVehicles{a, 49},
        cmd::CallAirSupport{ss2::assets::AirMissionKind::Paratrooper, 50, 51},
        cmd::LoadSnapshot{snap},
        cmd::Acknowledge{},
        cmd::TogglePause{},
        cmd::Resign{0},
        cmd::SupportBridge{b, 22},
        cmd::UnloadSingleSoldier{52, 53},
        cmd::EvacuateSingleSoldier{54, 55},
    };
}

void testSerialization() {
    const auto commands = everyCommand();
    std::set<std::size_t> covered;
    std::vector<cmd::RecordedCommand> log;
    std::uint32_t tick = 0;
    for (const auto& c : commands) {
        covered.insert(c.index());
        const cmd::RecordedCommand rc{tick, c};
        tick = tick * 3u + 7u;
        const auto line = cmd::toLine(rc);
        require(line.starts_with(std::string(cmd::tagOf(c)) + ' '), "line must start with its tag: " + line);
        require(line.find('\n') == std::string::npos, "line must not contain a newline: " + line);
        const auto back = cmd::fromLine(line);
        require(back.has_value(), "fromLine rejected its own output: " + line);
        require(*back == rc, "round trip changed the command: " + line);
        require(cmd::toLine(*back) == line, "re-serialization differs: " + line);
        log.push_back(rc);
    }
    require(covered.size() == std::variant_size_v<cmd::Command>, "round trip must cover every command type");
    log.push_back({0xffffffffu, cmd::Move{{static_cast<std::size_t>(-1)}, -2147483647 - 1, 2147483647, false}});

    const auto text = "# replay\n\n" + cmd::toText(log);
    const auto parsed = cmd::fromText(text);
    require(parsed && *parsed == log, "whole-log round trip failed");

    cmd::ReplayFile repFile;
    repFile.mission = "maps/single/--- TUTORIAL I ---.SSM";
    repFile.difficulty = 3;
    repFile.autoEngage = false;
    repFile.endTick = 0xffffffffu;
    repFile.endStateHash = 0xdeadbeefcafef00dull;
    repFile.initialSavedPools[0u] = {
        {"sdkfz 251", 70, 95, 80, 1200, {
            {"dcrew", 70, 100, 40, 800, true},
            {"dofficer 'a%b", 70, 88, 30, 1500, false},
        }},
        {"dguner", 70, 92, 45, 400, {}},
    };
    repFile.initialSavedPools[3u] = {
        {"sherman.v", 12, 100, 71, 2400, {{"acrew", 12, 100, 20, 600, true}}},
    };
    repFile.commands = {
        {0u, cmd::Acknowledge{}},
        {12u, cmd::Move{{0, 1}, 2960, 1776, false}},
        {0xffffffffu, cmd::TogglePause{}},
    };
    const auto repText = cmd::toReplayFileText(repFile);
    require(cmd::fromText(repText) == std::optional<std::vector<cmd::RecordedCommand>>(repFile.commands),
            "plain fromText must still read a ReplayFile body");
    const auto parsedRep = cmd::fromReplayFileText(repText);
    require(parsedRep && *parsedRep == repFile, "ReplayFile round trip failed");
    require(!cmd::fromReplayFileText("# saved_pool:\n"), "fromReplayFileText must reject empty saved_pool");
    require(!cmd::fromReplayFileText("# saved_pool: 0 0\n"), "fromReplayFileText must reject empty saved_pool wave");
    require(!cmd::fromReplayFileText("# saved_pool: 0 1 'rifle 1 100\n"),
            "fromReplayFileText must reject truncated saved_pool unit");
    require(!cmd::fromReplayFileText("# saved_pool: 0 1 'rifle 1 100 50 0 0 extra\n"),
            "fromReplayFileText must reject trailing saved_pool tokens");
    require(!cmd::fromReplayFileText("# saved_pool: 0 1 'rifle 1 100 50 0 0\n# saved_pool: 0 1 'rifle 1 100 50 0 0\n"),
            "fromReplayFileText must reject duplicate saved_pool index");
    require(!cmd::fromReplayFileText("# difficulty: 4\n"), "fromReplayFileText must reject out-of-range difficulty");
    require(!cmd::fromReplayFileText("# auto_engage: maybe\n"), "fromReplayFileText must reject bad auto_engage");
    require(!cmd::fromReplayFileText("# end_state_hash: 0xzz\n"), "fromReplayFileText must reject bad hex hash");
    require(!cmd::fromReplayFileText("# end_tick: 5\nacknowledge 6\n"),
            "fromReplayFileText must reject end_tick earlier than last command");
    require(!cmd::fromReplayFileText("acknowledge 5\nacknowledge 4\n"),
            "fromReplayFileText must reject out-of-order command ticks");

    for (const char* bad : {"", "move", "move 1", "move 1 1 0 2 3", "move 1 1 0 2 3 0 0 extra",
                            "move 1 1 0 2 3 2", "move -1 0 0 0 0", "move 1 5 0 0 0", "nope 0",
                            "toggle-pause 0 ", "toggle-pause  0", "spawn-vehicle 0 tiger", "spawn-vehicle 0 '%G0"}) {
        require(!cmd::fromLine(bad), std::string("fromLine accepted malformed input: ") + bad);
    }
    require(!cmd::fromText("toggle-pause 0\nbogus\n"), "fromText must reject a log with a bad line");
    std::cout << "Command serialization round trip (" << covered.size() << " types) PASSED\n";
}

// Two armies close enough to fight within a few dozen ticks.
struct World {
    ss2::assets::TutorialData data;
    std::vector<ss2::assets::Infantry> infantry;
    std::vector<ss2::assets::VehicleUnit> vehicles;
    std::shared_ptr<const ss2::assets::UnitCatalog> catalog;

    World() {
        data.width = 128;
        data.height = 128;
        data.flags.assign(128u * 128u, 0);
        data.obstacles.assign(128u * 128u, 0);
        data.zones.assign(128u * 128u, 0);
        data.phrases.resize(1);
        data.scripts.resize(1, {0x7fffffffu});

        ss2::assets::UnitSpec tiger{};
        tiger.id = "tiger";
        tiger.isVehicle = true;
        tiger.health = 1160;
        tiger.ammo1 = 94;
        tiger.moveSpeed = 1.12;
        tiger.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {100, 82, 50};
        tiger.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {256, 256, 256};
        ss2::assets::UnitSpec sherman{};
        sherman.id = "sherman.v";
        sherman.isVehicle = true;
        sherman.health = 660;
        sherman.ammo1 = 71;
        sherman.ammo2 = 500;
        sherman.moveSpeed = 1.15;
        const auto weapon = [](std::string id, ss2::assets::HitType hit, int damage, int range, int reload, int burst,
                               int burstReload, std::string sound) {
            ss2::assets::WeaponSpec w{};
            w.shotId = std::move(id);
            w.hitType = hit;
            w.damage = w.damageMax = damage;
            w.range = w.rangeMax = range;
            w.reloadTicks = reload;
            w.burstShots = burst;
            w.burstReloadTicks = burstReload;
            w.soundGroup = std::move(sound);
            return w;
        };
        sherman.shot1 = weapon("pierce6-7k", ss2::assets::HitType::Pierce, 500, 600, 5, 1, 5, "gunshot");
        sherman.shot2 = weapon("man_machine", ss2::assets::HitType::Machine, 54, 550, 2, 4, 12, "mg_tank_burst");
        ss2::assets::UnitSpec jeep{};
        jeep.id = "wyllis.mb.jeep";
        jeep.isVehicle = true;
        jeep.health = 70;
        jeep.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {0, 0, 0};
        catalog = ss2::assets::UnitCatalog::fromSpecs({tiger, sherman, jeep});

        infantry = {
            {"dofficer", 4064, 2032, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
            {"dguner", 4000, 2064, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
            {"aguner", 4128, 2064, 100, 40, 2, 1, 0, 0, 0, 0, 4u, 0u, false, true},
            {"aguner", 4192, 2096, 100, 40, 2, 1, 0, 0, 0, 0, 4u, 0u, false, true},
        };
        vehicles = {
            {"sherman.v", 4000, 2000, 100, 100, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
            {"tiger", 4128, 2032, 100, 100, 2, 1, 0u, 0u, 0, 0, 0, 0, true, true},
            {"wyllis.mb.jeep", 4128, 2080, 100, 0, 3, 1, 0u, 0u, 0, 0, 0, 0, true, true},
        };
    }
    ss2::TutorialUnits units() const {
        ss2::FogGrid fog(128, 128, data.flags);
        return ss2::TutorialUnits(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, data.flags, data.obstacles),
                                  infantry, {}, std::move(fog), 2, true, vehicles, catalog);
    }
    std::unique_ptr<ss2::MissionRunner> manualRunner() const {
        return std::make_unique<ss2::MissionRunner>(ss2::MissionRunner::ManualClock{}, data, units());
    }
};

struct Step {
    std::uint32_t tick;
    cmd::Command command;
};

// Orders that move and fight, plus a pause/resume and a mid-game spawn.
std::vector<Step> script() {
    return {
        {0, cmd::Move{{0, 1}, 4300, 2200, false}},
        {0, cmd::AttackVehicle{{1}, 2}},
        {4, cmd::AttackWithVehicles{{0}, true, 1}},
        {9, cmd::TogglePause{}},
        {9, cmd::Move{{0}, 4000, 2000, false}},
        {9, cmd::TogglePause{}},
        {15, cmd::Attack{{0}, 2}},
        {22, cmd::SpawnInfantry{{"dguner", 3968, 2000, 100, 40, 1, 0, 0, 0, 0, 0, 2u, 0u, false, true}, 0}},
        {30, cmd::MoveVehicles{{2}, 4400, 2300, true}},
        {31, cmd::ToggleCrouch{{4}}},
        {45, cmd::StopVehicles{{0}}},
        {46, cmd::AttackMove{{4}, 4200, 2100, true}},
    };
}

void testDeterminism(const World& world) {
    constexpr std::uint32_t endTick = 90;
    const auto play = [&](ss2::MissionRunner& runner) {
        runner.startRecording();
        for (const auto& [tick, command] : script()) {
            while (runner.simTick() < tick) runner.advanceTicks(1);
            runner.apply(command);
        }
        runner.advanceTicks(endTick - runner.simTick());
        return runner.takeRecording();
    };
    auto a = world.manualRunner();
    auto b = world.manualRunner();
    const auto initial = a->takeSnapshot();
    const auto initialHash = a->stateHash();
    require(initialHash == b->stateHash(), "fresh ManualClock runners must start with equal stateHash");
    const auto logA = play(*a);
    const auto logB = play(*b);
    require(a->simTick() == endTick && b->simTick() == endTick, "manual runners must reach the end tick");
    const auto snapA = a->takeSnapshot();
    const auto hashA = a->stateHash();
    require(snapA == b->takeSnapshot(), "identical scripts on two ManualClock runners diverged");
    require(hashA == b->stateHash(), "identical scripts produced different stateHash values");
    require(snapA != initial && hashA != initialHash, "the scripted session must change the simulation");
    require(logA == logB && logA.size() == script().size(), "recordings of identical scripts differ");
    for (std::size_t i = 0; i < logA.size(); ++i) {
        require(logA[i].tick == script()[i].tick && logA[i].command == script()[i].command,
                "recording must hold each command with the tick it was applied at");
    }

    // The same log, through text and replay(), on a third runner.
    const auto parsed = cmd::fromText(cmd::toText(logA));
    require(parsed && *parsed == logA, "recorded log text round trip failed");
    auto c = world.manualRunner();
    require(c->replay(*parsed, endTick), "replay rejected a recorded log");
    require(c->simTick() == endTick && c->takeSnapshot() == snapA, "replay diverged from the scripted run");
    require(c->stateHash() == hashA, "replay stateHash diverged from the scripted run");

    // Advancing is gated by pause like the worker loop.
    auto d = world.manualRunner();
    require(d->togglePause(), "togglePause must report paused");
    d->advanceTicks(10);
    require(d->simTick() == 0, "a paused ManualClock runner must not step");
    require(!d->replay(std::vector<cmd::RecordedCommand>{}, 5), "replay must fail when the clock is held");
    std::cout << "ManualClock determinism and replay PASSED\n";
}

void testLiveReplay(const World& world) {
    using namespace std::chrono_literals;
    ss2::TutorialUnits::Snapshot initial;
    ss2::TutorialUnits::Snapshot live;
    std::uint64_t initialHash = 0;
    std::uint64_t liveHash = 0;
    std::uint32_t liveTick = 0;
    std::vector<cmd::RecordedCommand> log;
    {
        ss2::MissionRunner runner(world.data, 2u, world.units());
        runner.startRecording();
        initial = runner.takeSnapshot();
        initialHash = runner.stateHash();
        (void)runner.move(std::array<std::size_t, 2>{0, 1}, 4300, 2200);
        (void)runner.attackVehicle(std::array<std::size_t, 1>{1}, 2);
        std::this_thread::sleep_for(40ms);
        (void)runner.attackWithVehicles(std::array<std::size_t, 1>{0}, true, 1);
        std::this_thread::sleep_for(25ms);
        (void)runner.togglePause();
        (void)runner.move(std::array<std::size_t, 1>{0}, 4000, 2000);
        std::this_thread::sleep_for(20ms);
        (void)runner.togglePause();
        std::this_thread::sleep_for(30ms);
        (void)runner.spawnInfantry({"dguner", 3968, 2000, 100, 40, 1, 0, 0, 0, 0, 0, 2u, 0u, false, true});
        (void)runner.attack(std::array<std::size_t, 1>{4}, 2);
        std::this_thread::sleep_for(35ms);
        (void)runner.moveVehicles(std::array<std::size_t, 1>{2}, 4400, 2300, true);
        std::this_thread::sleep_for(60ms);
        // Under heavy load the worker may lag; give it time to cover some ground.
        for (int i = 0; i < 200 && runner.simTick() < 20u; ++i) std::this_thread::sleep_for(5ms);
        require(runner.togglePause(), "final togglePause must pause the runner");
        // Paused: the worker keeps waking but cannot step any more.
        std::this_thread::sleep_for(30ms);
        live = runner.takeSnapshot();
        liveHash = runner.stateHash();
        liveTick = runner.simTick();
        log = runner.takeRecording();
        require(runner.simTick() == liveTick && runner.stateHash() == liveHash, "a paused runner must not advance");
    }
    require(log.size() == 10, "every live order, including pauses, must be recorded");
    require(liveTick == live.tick && liveTick >= 10, "the live session must run for several ticks");
    require(live != initial && liveHash != initialHash, "the live session must change the simulation");
    for (std::size_t i = 1; i < log.size(); ++i) {
        require(log[i - 1].tick <= log[i].tick, "live ticks must be non-decreasing");
    }
    require(log.back().tick == liveTick, "the final pause must be recorded at the last tick");

    const auto parsed = cmd::fromText(cmd::toText(log));
    require(parsed && *parsed == log, "live log text round trip failed");
    auto replayed = world.manualRunner();
    require(replayed->replay(*parsed, liveTick), "replay rejected the live log");
    require(replayed->simTick() == liveTick, "replay must stop at the live tick");
    require(replayed->takeSnapshot() == live, "replaying the live log diverged from the live simulation");
    require(replayed->stateHash() == liveHash, "replaying the live log produced a different stateHash");
    std::cout << "Live-vs-replay parity (" << log.size() << " commands over " << liveTick << " ticks) PASSED\n";
}

// Tutorial I with its real script: dialogs hold the clock until acknowledged.
void testGoldTutorial(const std::filesystem::path& game) {
    const auto data = ss2::assets::readTutorialData(game / "maps/single/--- TUTORIAL I ---.SSM");
    std::vector<ss2::assets::Infantry> infantry;
    std::vector<ss2::assets::Placement> vehicles;
    for (const auto& p : ss2::assets::readTutorialPlacements(game)) {
        if (p.id == "zis-3" || p.id == "m3a1" || p.id == "ds39" || p.id == "panther") vehicles.push_back(p);
        else infantry.push_back({p.id, p.x, p.y, p.hp, p.ammo, p.group, p.owner, 0, 0, 0, 0, unsigned(p.direction)});
    }
    const auto makeRunner = [&] {
        return std::make_unique<ss2::MissionRunner>(
            ss2::MissionRunner::ManualClock{}, data,
            ss2::TutorialUnits(ss2::InfantryPathGrid::loadTutorial(game), infantry, vehicles));
    };
    std::vector<std::size_t> squad;
    for (std::size_t i = 0; i < infantry.size(); ++i) {
        if (infantry[i].owner == 0) squad.push_back(i);
    }
    auto live = makeRunner();
    live->startRecording();
    const auto initial = live->takeSnapshot();
    unsigned dialogs = 0, rejectedWhileModal = 0;
    for (int iteration = 0; iteration < 1200; ++iteration) {
        for (const auto& e : live->poll()) {
            if (e.kind != ss2::MissionEvent::Dialog) continue;
            ++dialogs;
            // Orders during a dialog are rejected, yet recorded for replay.
            if (!live->move(squad, 2960, 1776)) ++rejectedWhileModal;
            live->acknowledge();
        }
        if (live->simTick() == 520u) (void)live->move(squad, 2960, 1776);
        if (live->simTick() == 700u) (void)live->stop(squad);
        live->advanceTicks(1);
    }
    const auto liveTick = live->simTick();
    const auto snap = live->takeSnapshot();
    const auto liveHash = live->stateHash();
    const auto log = live->takeRecording();
    require(dialogs >= 2 && rejectedWhileModal == dialogs, "Tutorial I must raise dialogs that hold orders");
    require(snap != initial, "Tutorial I session must change the simulation");
    const auto parsed = cmd::fromText(cmd::toText(log));
    require(parsed && *parsed == log, "Tutorial I log text round trip failed");
    auto replayed = makeRunner();
    require(replayed->replay(*parsed, liveTick), "Tutorial I replay rejected its log");
    require(replayed->takeSnapshot() == snap, "Tutorial I replay diverged");
    require(replayed->stateHash() == liveHash, "Tutorial I replay stateHash diverged");
    std::cout << "Gold Tutorial I replay (" << dialogs << " dialogs, " << log.size() << " commands, " << liveTick
              << " ticks) PASSED\n";
}

// Replays a command log while sampling stateHash every stride ticks. Returns
// the first sample tick whose hash differs from expectedTrace, or nullopt.
std::optional<std::uint32_t> replayAndVerifyTrace(
    ss2::MissionRunner& runner, std::span<const cmd::RecordedCommand> log, std::uint32_t untilTick,
    std::uint32_t stride, std::span<const std::pair<std::uint32_t, std::uint64_t>> expectedTrace) {
    std::size_t cmdIdx = 0;
    std::size_t traceIdx = 0;
    for (std::uint32_t target = 0; target <= untilTick; ++target) {
        while (runner.simTick() < target) {
            const auto before = runner.simTick();
            runner.advanceTicks(1);
            if (runner.simTick() == before) return target;
        }
        while (cmdIdx < log.size() && log[cmdIdx].tick == target) {
            runner.apply(log[cmdIdx].command);
            ++cmdIdx;
        }
        if (stride > 0 && (target % stride == 0 || target == untilTick)) {
            if (traceIdx >= expectedTrace.size() || expectedTrace[traceIdx].first != target ||
                expectedTrace[traceIdx].second != runner.stateHash()) {
                return target;
            }
            ++traceIdx;
        }
    }
    if (traceIdx != expectedTrace.size()) return untilTick;
    return std::nullopt;
}

// Recorded full Tutorial I route regression through P[7] village briefing:
// opening dialogs -> wreck march -> fields briefing (Dialog 5) -> group-3
// elimination (Phrase 6) -> P[7] village briefing (Dialog 7), then full
// ReplayFile round-trip with 250-tick periodic stateHash trace parity.
void testGoldTutorialFullRoute(const std::filesystem::path& game) {
    const auto data = ss2::assets::readMissionData(game / "maps/single/--- TUTORIAL I ---.SSM");
    const auto placements = ss2::assets::readTutorialPlacements(game);
    const auto catalog = ss2::assets::UnitCatalog::load(game);

    std::vector<ss2::assets::Infantry> infantry;
    std::vector<ss2::assets::Placement> vehicles;
    for (const auto& p : placements) {
        if (p.id == "zis-3" || p.id == "m3a1" || p.id == "ds39" || p.id == "panther") vehicles.push_back(p);
        else infantry.push_back({p.id, p.x, p.y, p.hp, p.ammo, p.group, p.owner, 0, 0, 0, 0, unsigned(p.direction)});
    }

    const auto makeFullRunner = [&] {
        ss2::FogGrid fog(data.width, data.height, data.flags);
        fog.applyMapPerimeterFlags();
        require(fog.loadCirc64FromGame(game), "CIRC64.DAT must load for Tutorial I full route test");
        ss2::TutorialUnits units(ss2::InfantryPathGrid::loadTutorial(game), infantry, vehicles,
                                 std::move(fog), 2, true, {}, catalog);
        units.setPlayerTeams(data.playerTeams);
        units.setMines(data.mines);
        units.setAirSupport(data.airSupport, data.paratrooperGroup, data.paratrooperRoster);
        return std::make_unique<ss2::MissionRunner>(
            ss2::MissionRunner::ManualClock{}, data, std::move(units));
    };

    std::vector<std::size_t> squad;
    for (std::size_t i = 0; i < infantry.size(); ++i) {
        if (infantry[i].owner == 0) squad.push_back(i);
    }
    require(squad.size() == 29u, "Tutorial I player squad must contain 29 infantry");

    auto live = makeFullRunner();
    const auto livingSquad = [&] {
        const auto snap = live->takeSnapshot();
        std::vector<std::size_t> alive;
        for (const auto id : squad) {
            if (id < snap.soldiers.size() && snap.soldiers[id].hp > 0) alive.push_back(id);
        }
        return alive;
    };
    live->startRecording();
    constexpr std::uint32_t kTraceStride = 250u;
    constexpr int kMaxTicks = 10000;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> trace;

    unsigned dialogs = 0;
    bool sawPhrase4 = false;
    bool sawDialog5 = false;
    bool sawPhrase6 = false;
    bool sawDialog7 = false;
    std::optional<std::uint32_t> fieldsOrderTick;
    int sweepStage = 0;
    std::uint32_t trailingTicks = 0;

    for (int step = 0; step < kMaxTicks; ++step) {
        for (const auto& e : live->poll()) {
            if (e.kind == ss2::MissionEvent::Message && e.index == 4) sawPhrase4 = true;
            if (e.kind == ss2::MissionEvent::Message && e.index == 6) sawPhrase6 = true;
            if (e.kind == ss2::MissionEvent::Dialog) {
                ++dialogs;
                if (e.index == 5) sawDialog5 = true;
                if (e.index == 7) sawDialog7 = true;
                live->acknowledge();
            }
        }

        const auto curTick = live->simTick();

        if (curTick == 520u && !sawDialog5) {
            const auto ids = livingSquad();
            const auto accepted = live->move(ids, 2960, 1776);
            require(accepted && *accepted == ids.size(), "Wreck march order failed on Tutorial I");
        }
        if (sawDialog5 && !fieldsOrderTick) {
            const auto ids = livingSquad();
            require(!ids.empty(), "No living player infantry remaining after wreck objective");
            const auto accepted = live->attackMove(ids, 3712, 1520);
            require(accepted && *accepted > 0u, "Fields attack-move order failed after Dialog 5");
            fieldsOrderTick = curTick;
        }
        if (fieldsOrderTick && !sawPhrase6) {
            if (sweepStage == 0 && curTick >= *fieldsOrderTick + 1500u) {
                const auto ids = livingSquad();
                if (!ids.empty()) (void)live->attackMove(ids, 3744, 1536);
                sweepStage = 1;
            } else if (sweepStage == 1 && curTick >= *fieldsOrderTick + 2500u) {
                const auto ids = livingSquad();
                if (!ids.empty()) (void)live->attackMove(ids, 3680, 1504);
                sweepStage = 2;
            }
        }

        if ((curTick % kTraceStride) == 0u) {
            const auto h = live->stateHash();
            if (!trace.empty() && trace.back().first == curTick) trace.back().second = h;
            else trace.emplace_back(curTick, h);
        }

        if (sawDialog7 && ++trailingTicks >= 20u) break;

        live->advanceTicks(1);
    }

    require(sawPhrase4, "Tutorial I full route must reach Phrase 4 (wreck complete)");
    require(sawDialog5, "Tutorial I full route must reach Dialog 5 (fields briefing)");
    require(sawPhrase6, "Tutorial I full route must reach Phrase 6 (group-3 cleared)");
    require(sawDialog7, "Tutorial I full route must reach Dialog 7 (P[7] village briefing)");

    const auto endTick = live->simTick();
    const auto endHash = live->stateHash();
    const auto endSnap = live->takeSnapshot();
    const auto log = live->takeRecording();
    if (trace.empty() || trace.back().first != endTick) {
        trace.emplace_back(endTick, endHash);
    }

    cmd::ReplayFile rep;
    rep.mission = "maps/single/--- TUTORIAL I ---.SSM";
    rep.difficulty = 2;
    rep.autoEngage = true;
    rep.endTick = endTick;
    rep.endStateHash = endHash;
    rep.commands = log;
    const auto parsed = cmd::fromReplayFileText(cmd::toReplayFileText(rep));
    require(parsed && *parsed == rep, "Full Tutorial I ReplayFile round trip failed");

    auto replayed = makeFullRunner();
    const auto badTick = replayAndVerifyTrace(*replayed, parsed->commands, endTick, kTraceStride, trace);
    require(!badTick.has_value(),
            "Full Tutorial I route replay diverged at sample tick " + std::to_string(badTick.value_or(0)));
    require(replayed->simTick() == endTick && replayed->takeSnapshot() == endSnap,
            "Full Tutorial I replay final snapshot diverged");
    require(replayed->stateHash() == endHash,
            "Full Tutorial I replay final stateHash diverged");

    std::cout << "Gold Tutorial I full route P[7] replay (" << dialogs << " dialogs, "
              << log.size() << " commands, " << endTick << " ticks, stateHash 0x"
              << std::hex << endHash << std::dec << ") PASSED\n";
}

void testStateHashSensitivityAndTrace(const World& world) {
    // (1) Issuing an order before any tick leaves Snapshot unchanged (positions,
    // HP and ammo have not moved yet) while changing stateHash immediately.
    auto r = world.manualRunner();
    const auto snap0 = r->takeSnapshot();
    const auto hash0 = r->stateHash();
    require(r->move(std::array<std::size_t, 1>{0}, 4300, 2200).has_value(), "move order must be accepted");
    const auto snapOrdered = r->takeSnapshot();
    const auto hashOrdered = r->stateHash();
    require(snapOrdered == snap0, "an unstepped move order does not alter Snapshot");
    require(hashOrdered != hash0, "stateHash must change immediately when an order is queued");

    // (2) Stepping one tick of movement changes stateHash again, and draining
    // presentation buffers via poll*() does not change stateHash.
    r->advanceTicks(1);
    const auto hashTick1 = r->stateHash();
    require(hashTick1 != hashOrdered && hashTick1 != hash0, "stateHash must change after one tick of movement");
    (void)r->poll();
    (void)r->pollPoses();
    (void)r->pollVehiclePoses();
    (void)r->pollFog();
    require(r->stateHash() == hashTick1, "draining presentation buffers must not change stateHash");

    // (3) Divergence detection: two runners given the same vehicle move order
    // one tick apart diverge while the vehicle is in flight.
    auto early = world.manualRunner();
    auto late = world.manualRunner();
    require(early->replay(std::array{cmd::RecordedCommand{1u, cmd::MoveVehicles{{0}, 3500, 1800, false}}}, 12u),
            "early replay failed");
    require(late->replay(std::array{cmd::RecordedCommand{2u, cmd::MoveVehicles{{0}, 3500, 1800, false}}}, 12u),
            "late replay failed");
    require(early->stateHash() != late->stateHash(),
            "orders issued one tick apart must produce distinct stateHash values while in flight");

    // (4) Periodic hash trace pinpoints the exact sample window where a
    // perturbed command log diverges.
    constexpr std::uint32_t endTick = 50;
    constexpr std::uint32_t stride = 5;
    std::vector<cmd::RecordedCommand> baseLog{
        {0u, cmd::Move{{0, 1}, 4300, 2200, false}},
        {20u, cmd::MoveVehicles{{0}, 3500, 1800, false}},
        {35u, cmd::StopVehicles{{0}}},
    };
    auto ref = world.manualRunner();
    std::vector<std::pair<std::uint32_t, std::uint64_t>> trace;
    std::size_t idx = 0;
    for (std::uint32_t t = 0; t <= endTick; ++t) {
        while (ref->simTick() < t) ref->advanceTicks(1);
        while (idx < baseLog.size() && baseLog[idx].tick == t) {
            ref->apply(baseLog[idx].command);
            ++idx;
        }
        if (t % stride == 0 || t == endTick) trace.emplace_back(t, ref->stateHash());
    }

    auto verifyOk = world.manualRunner();
    require(!replayAndVerifyTrace(*verifyOk, baseLog, endTick, stride, trace).has_value(),
            "periodic hash trace must match an identical replay");

    auto perturbedLog = baseLog;
    perturbedLog[1].tick = 21u; // shift tick-20 command to tick 21 -> diverges at sample tick 20
    auto verifyPerturbed = world.manualRunner();
    const auto badTick = replayAndVerifyTrace(*verifyPerturbed, perturbedLog, endTick, stride, trace);
    require(badTick == 20u, "periodic hash trace must report tick 20 as the first divergent sample");

    std::cout << "StateHash sensitivity, divergence and periodic trace PASSED\n";
}

void testInteractiveReplayTransport(const World& world) {
    constexpr std::uint32_t endTick = 90;
    auto recorder = world.manualRunner();
    recorder->startRecording();
    for (const auto& [tick, command] : script()) {
        while (recorder->simTick() < tick) recorder->advanceTicks(1);
        recorder->apply(command);
    }
    recorder->advanceTicks(endTick - recorder->simTick());
    const auto log = recorder->takeRecording();
    const auto expectedHash = recorder->stateHash();
    const auto expectedSnap = recorder->takeSnapshot();

    auto live = world.manualRunner();
    cmd::ReplayTransport tr{};
    require(tr.speedPercent() == 100 && tr.speedLabel() == "1x", "default ReplayTransport speed must be 1x");
    tr.stepFaster();
    tr.stepFaster();
    require(tr.speedPercent() == 400 && tr.speedLabel() == "4x", "two stepFaster calls must reach 4x");
    tr.stepSlower();
    tr.stepSlower();
    tr.stepSlower();
    require(tr.speedPercent() == 50 && tr.speedLabel() == "0.5x", "stepSlower must clamp down to 0.5x");

    tr.togglePause();
    require(tr.paused && tr.consumeTicks(1.0) == 0u, "paused ReplayTransport must consume 0 ticks");
    require(tr.formatBanner(0, endTick).find("PAUSED") != std::string::npos, "paused banner must report PAUSED");
    tr.togglePause();
    tr.speedIndex = 2; // 2x

    while (live->simTick() < endTick) {
        // Exercise viewer-seat cycling mid-replay: must NOT alter simulation stateHash().
        tr.cycleViewerSeat(2);
        if (tr.viewerSeat >= 0) live->setViewerOwner(tr.viewerSeat);
        const unsigned budget = std::max(1u, tr.consumeTicks(0.020));
        for (unsigned k = 0; k < budget && live->simTick() < endTick; ++k) {
            require(live->replayStep(log, tr.cursor, endTick), "replayStep failed before endTick");
        }
    }
    require(tr.cursor == log.size(), "replayStep must consume all commands by endTick");
    tr.finished = true;
    tr.hashVerified = (live->stateHash() == expectedHash);
    require(tr.hashVerified && live->takeSnapshot() == expectedSnap,
            "interactive replayStep with viewerSeat/speed changes diverged from batch replay");
    require(tr.formatBanner(live->simTick(), endTick).find("END OK") != std::string::npos,
            "finished ReplayTransport banner must report END OK");
    std::cout << "Interactive ReplayTransport & replayStep parity PASSED\n";
}
}

int main(int argc, char** argv) try {
    testSerialization();
    const World world;
    testDeterminism(world);
    testStateHashSensitivityAndTrace(world);
    testInteractiveReplayTransport(world);
    testLiveReplay(world);
    if (argc == 2) {
        testGoldTutorial(argv[1]);
        testGoldTutorialFullRoute(argv[1]);
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
