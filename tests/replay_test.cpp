// Command log round trip, manual-clock determinism and live-vs-replay parity
// for MissionRunner. Runs on synthetic data; pass a Gold 2.2 directory to also
// replay a Tutorial I session with its real script and dialogs.
#include "assets/mission.hpp"
#include "assets/unit_catalog.hpp"
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
    const auto logA = play(*a);
    const auto logB = play(*b);
    require(a->simTick() == endTick && b->simTick() == endTick, "manual runners must reach the end tick");
    const auto snapA = a->takeSnapshot();
    require(snapA == b->takeSnapshot(), "identical scripts on two ManualClock runners diverged");
    require(snapA != initial, "the scripted session must change the simulation");
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
    std::uint32_t liveTick = 0;
    std::vector<cmd::RecordedCommand> log;
    {
        ss2::MissionRunner runner(world.data, 2u, world.units());
        runner.startRecording();
        initial = runner.takeSnapshot();
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
        liveTick = runner.simTick();
        log = runner.takeRecording();
        require(runner.simTick() == liveTick, "a paused runner must not advance");
    }
    require(log.size() == 10, "every live order, including pauses, must be recorded");
    require(liveTick == live.tick && liveTick >= 10, "the live session must run for several ticks");
    require(live != initial, "the live session must change the simulation");
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
    const auto log = live->takeRecording();
    require(dialogs >= 2 && rejectedWhileModal == dialogs, "Tutorial I must raise dialogs that hold orders");
    require(snap != initial, "Tutorial I session must change the simulation");
    const auto parsed = cmd::fromText(cmd::toText(log));
    require(parsed && *parsed == log, "Tutorial I log text round trip failed");
    auto replayed = makeRunner();
    require(replayed->replay(*parsed, liveTick), "Tutorial I replay rejected its log");
    require(replayed->takeSnapshot() == snap, "Tutorial I replay diverged");
    std::cout << "Gold Tutorial I replay (" << dialogs << " dialogs, " << log.size() << " commands, " << liveTick
              << " ticks) PASSED\n";
}
}

int main(int argc, char** argv) try {
    testSerialization();
    const World world;
    testDeterminism(world);
    testLiveReplay(world);
    if (argc == 2) testGoldTutorial(argv[1]);
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
