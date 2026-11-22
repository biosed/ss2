#include "tutorial_vm_test_internal.hpp"
#include "assets/placement.hpp"
#include "assets/scenery.hpp"
#include "assets/terrain.hpp"
#include "sim/tutorial_coordinates.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ss2::vm_test {
namespace {

constexpr std::uint32_t lit(std::uint32_t v) { return 0x80000000u | v; }

void check(bool cond, const char* msg) {
    if (!cond) throw std::runtime_error(msg);
}

std::string lowerAscii(std::string_view sv) {
    std::string out(sv);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

ss2::assets::MissionData makeFlatMission(unsigned w = 64u, unsigned h = 64u) {
    ss2::assets::MissionData md{};
    md.width = w;
    md.height = h;
    md.flags.assign(static_cast<std::size_t>(w) * h, 0u);
    md.obstacles.assign(static_cast<std::size_t>(w) * h, 0x000Fu);
    md.zones.assign(static_cast<std::size_t>(w) * h, 0u);
    md.phrases.resize(16);
    const int originX = static_cast<int>(h) * 32;
    auto tilePx = [originX](int tx, int ty) -> std::pair<int, int> {
        return {originX + (tx - ty) * 32, (tx + ty) * 16 + 16};
    };
    md.locations[0] = tilePx(32, 32);
    md.locations[1] = tilePx(26, 26);
    md.locations[2] = tilePx(22, 22);
    md.locations[3] = tilePx(18, 18);
    md.markers[0] = tilePx(24, 24);
    md.markers[1] = tilePx(28, 28);
    md.markers[2] = tilePx(30, 30);
    md.gates[0] = tilePx(12, 12);
    md.gateTargets[0] = tilePx(14, 14);
    return md;
}

std::shared_ptr<const ss2::assets::UnitCatalog> makeCaofTestCatalog() {
    ss2::assets::UnitSpec rifle{};
    rifle.id = "rifle_caof";
    rifle.health = 100;
    rifle.ammo1 = 40;
    rifle.moveSpeed = 1.6;
    rifle.sight = 12;
    rifle.shot1 = {"rifle", ss2::assets::HitType::Machine, 20, 20, 220, 220, 0, 12, 1, 12, "rifle"};

    ss2::assets::UnitSpec tank{};
    tank.id = "tank_caof";
    tank.isVehicle = true;
    tank.health = 500;
    tank.ammo1 = 20;
    tank.ammo2 = 10;
    tank.crewNumber = 2;
    tank.stock = 4;
    tank.moveSpeed = 2.0;
    tank.sight = 14;
    tank.shot1 = {"ap75", ss2::assets::HitType::Pierce, 120, 120, 400, 400, 0, 15, 1, 15, "tankgun"};
    tank.shot2 = {"mg792", ss2::assets::HitType::Machine, 15, 15, 250, 250, 0, 8, 3, 20, "mg"};

    ss2::assets::UnitSpec truck{};
    truck.id = "truck_caof";
    truck.isVehicle = true;
    truck.health = 240;
    truck.stock = 6;
    truck.moveSpeed = 2.2;
    truck.sight = 10;

    ss2::assets::PlaneSpec bomber{};
    bomber.id = "he111";
    bomber.health = 250;
    bomber.flySpeed = 8.0;
    bomber.bombsNumber = 4;
    bomber.bombDamage = 1800;
    bomber.sight = 18;

    return ss2::assets::UnitCatalog::fromSpecs({rifle, tank, truck}, {bomber});
}

std::pair<int, int> px64(int tx, int ty) {
    return {64 * 32 + (tx - ty) * 32, (tx + ty) * 16 + 16};
}

bool hasMessageEvent(const std::vector<ss2::MissionEvent>& evs, unsigned phraseIdx) {
    for (const auto& e : evs) {
        if ((e.kind == ss2::MissionEvent::Message || e.kind == ss2::MissionEvent::Dialog) &&
            e.index == phraseIdx) {
            return true;
        }
    }
    return false;
}

}  // namespace

void runCaofCompleteOpcodes() {
    const auto cat = makeCaofTestCatalog();

    // 1. OP(8) air strike by zone + OP(9) ready+active plane count + OP(43/44/45/46) air routes.
    {
        auto md = makeFlatMission();
        md.airSupport[0][2] = {"he111", 4, 2};
        // Script 0: OP(9) owner=0, mode=0 (==), thresh=2, slot=2 -> Message 0; then OP(8) launches 1 bomber at zone 1.
        md.scripts.push_back({
            lit(0), lit(0), lit(2), lit(2), 9u,
            7u, 11u,
            lit(0), 15u,
            lit(1), lit(2), lit(0), lit(1), lit(0), 8u,
            0x7fffffffu
        });
        // Script 1: At tick > 50 (scan tick 67), OP(9) must STILL see 2 total planes (1 ready + 1 flying),
        // then OP(44)+OP(43)+OP(44)+OP(45)+OP(46) dispatches 1 bomber to marker 2.
        md.scripts.push_back({
            lit(1), lit(2), 29u, 7u, 11u,
            lit(0), lit(0), lit(2), lit(2), 9u, 7u, lit(1), 15u,
            lit(0), 44u, 43u, lit(1), 44u, lit(2), 45u,
            lit(1), lit(2), lit(0), lit(0), 46u,
            0x7fffffffu
        });

        std::vector<ss2::assets::Infantry> inf;
        std::vector<ss2::assets::Placement> veh;
        ss2::TutorialUnits units(
            ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
            inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, cat);
        units.setAirSupport(md.airSupport, md.paratrooperGroup, md.paratrooperRoster);

        ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));
        auto ev0 = runner.poll();
        check(hasMessageEvent(ev0, 0u), "OP(9) initial ready plane count == 2 failed");
        check(runner.airSupportFor(0)[2].ready == 1, "OP(8) did not launch 1 sortie at zone 1");
        check(runner.pollAirSorties().size() == 1u, "OP(8) sortie not active in pollAirSorties()");

        runner.advanceTicks(4u);
        runner.advanceTicks(64u);
        auto ev67 = runner.poll();
        check(hasMessageEvent(ev67, 1u), "OP(9) did not count ready+in-flight planes together");
        check(runner.airSupportFor(0)[2].ready == 0, "OP(46) did not launch queued air-route sortie");

        // Also test empty-route OP(46) zone fallback (extra = 2 -> zone 1).
        auto mdEmpty = makeFlatMission();
        mdEmpty.airSupport[0][2] = {"he111", 2, 1};
        mdEmpty.scripts.push_back({lit(1), lit(2), lit(0), lit(2), 46u, 0x7fffffffu});
        ss2::TutorialUnits uEmpty(
            ss2::InfantryPathGrid::fromSubCellMasks(mdEmpty.width, mdEmpty.height, mdEmpty.flags, mdEmpty.obstacles),
            inf, veh, ss2::FogGrid(mdEmpty.width, mdEmpty.height, mdEmpty.flags), 2, false, {}, cat);
        uEmpty.setAirSupport(mdEmpty.airSupport, mdEmpty.paratrooperGroup, mdEmpty.paratrooperRoster);
        ss2::MissionRunner mrEmpty(ss2::MissionRunner::ManualClock{}, std::move(mdEmpty), std::move(uEmpty));
        check(mrEmpty.airSupportFor(0)[2].ready == 0 && mrEmpty.pollAirSorties().size() == 1u,
              "OP(46) empty-route zone fallback did not launch sortie");
    }

    // 2. OP(31) & OP(33) percentage queries across active/unmanned masks.
    {
        auto md = makeFlatMission();
        for (int y = 16; y <= 20; ++y) {
            for (int x = 16; x <= 20; ++x) {
                md.zones[static_cast<std::size_t>(y) * md.width + static_cast<std::size_t>(x)] = 3u;
            }
        }
        // Owner 0: 4 soldiers total, 2 inside zone 3 -> 50%.
        // Owner 1: 2 soldiers (mask 1) + 1 unmanned truck (mask 2 -> 50% of 2).
        const auto [zx0, zy0] = px64(18, 18);
        const auto [zx1, zy1] = px64(19, 18);
        const auto [ox0, oy0] = px64(36, 36);
        const auto [ox1, oy1] = px64(37, 36);
        const auto [ex0, ey0] = px64(42, 42);
        const auto [ex1, ey1] = px64(43, 42);
        const auto [vx1, vy1] = px64(45, 45);
        std::vector<ss2::assets::Infantry> inf{
            {"rifle_caof", zx0, zy0, 100, 40, 1, 0},
            {"rifle_caof", zx1, zy1, 100, 40, 1, 0},
            {"rifle_caof", ox0, oy0, 100, 40, 1, 0},
            {"rifle_caof", ox1, oy1, 100, 40, 1, 0},
            {"rifle_caof", ex0, ey0, 100, 40, 2, 1},
            {"rifle_caof", ex1, ey1, 100, 40, 2, 1},
        };
        std::vector<ss2::assets::VehicleUnit> vehUnits;
        ss2::assets::VehicleUnit unm{};
        unm.id = "truck_caof";
        unm.x = vx1; unm.y = vy1; unm.hp = 240; unm.owner = 1; unm.group = 2; unm.alive = true; unm.hasCrew = false;
        vehUnits.push_back(unm);

        // Script 0: OP(31) mode=0, pct=50, player=0, zone=3 -> Message 0
        md.scripts.push_back({lit(0), lit(50), lit(0), lit(3), 31u, 7u, 11u, lit(0), 15u, 0x7fffffffu});
        // Script 1: OP(33) maskA=1, pA=1, mode=0, pct=50, maskB=1, pB=0 AND maskA=2, pA=1, mode=0, pct=50, maskB=1, pB=1 -> Message 1
        md.scripts.push_back({
            lit(1), lit(1), lit(0), lit(50), lit(1), lit(0), 33u,
            lit(2), lit(1), lit(0), lit(50), lit(1), lit(1), 33u,
            1u, 7u, 11u, lit(1), 15u, 0x7fffffffu
        });

        ss2::TutorialUnits units(
            ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
            inf, {}, ss2::FogGrid(md.width, md.height, md.flags), 2, false, std::move(vehUnits), cat);
        ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));
        const auto evs = runner.poll();
        check(hasMessageEvent(evs, 0u), "OP(31) active zone percentage failed");
        check(hasMessageEvent(evs, 1u), "OP(33) mask cross-player percentage failed");
    }

    // 3. OP(83) & OP(91) group stat setters + primary/secondary ammo % query.
    {
        auto md = makeFlatMission();
        const auto [sx, sy] = px64(24, 24);
        const auto [tx, ty] = px64(28, 28);
        std::vector<ss2::assets::Infantry> inf{{"rifle_caof", sx, sy, 100, 50, 15, 0}};
        ss2::assets::VehicleUnit tank{};
        tank.id = "tank_caof"; tank.x = tx; tank.y = ty; tank.hp = 100;
        tank.ammo = 50;
        tank.owner = 0; tank.group = 15; tank.alive = true; tank.hasCrew = true;

        // Script 0: check slot0==50% & slot1==50% -> raise ammo to 80%, lower HP to 40%, raise expa to 60%, suppress morale to 10% -> Message 0
        md.scripts.push_back({
            lit(15), lit(0), lit(0), lit(50), 91u,
            lit(15), lit(1), lit(0), lit(50), 91u,
            1u, 7u, 11u,
            lit(0), 15u,
            lit(1), lit(1), lit(80), lit(15), 83u,
            lit(0), lit(0), lit(40), lit(15), 83u,
            lit(2), lit(1), lit(60), lit(15), 83u,
            lit(3), lit(0), lit(10), lit(15), 83u,
            0x7fffffffu
        });
        // Script 1 (fires on tick 67): check slot0==80% & rally morale to 90% -> Message 1
        md.scripts.push_back({
            lit(1), lit(2), 29u, 7u, 11u,
            lit(15), lit(0), lit(0), lit(80), 91u,
            7u, lit(1), 15u,
            lit(3), lit(1), lit(90), lit(15), 83u,
            0x7fffffffu
        });

        ss2::TutorialUnits units(
            ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
            inf, {}, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {tank}, cat);
        ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));
        check(hasMessageEvent(runner.poll(), 0u), "OP(91) initial primary/secondary ammo percent mismatch");
        const auto vPoses0 = runner.pollVehiclePoses();
        check(!vPoses0.empty(), "OP(83) initial vehicle pose missing");
        check(vPoses0.back().hp == 200, "OP(83) cap HP 40% failed");
        check(vPoses0.back().expa == ss2::TutorialUnits::missionExpaToSim(60),
              "OP(83) floor Expa 60% failed");
        check(vPoses0.back().suppressed, "OP(83) cap morale 10% did not suppress vehicle");

        runner.advanceTicks(68u);
        check(hasMessageEvent(runner.poll(), 1u), "OP(91) did not reflect OP(83) 80% ammo resupply");
        const auto vPoses = runner.pollVehiclePoses();
        check(!vPoses.empty() && !vPoses.back().suppressed, "OP(83) floor morale 90% did not clear suppression");
    }

    // 4. OP(85) group evac, OP(86) zone evac, OP(39) delayed reserve deploy, OP(75) limited move,
    //    plus OP(62/63/71/72/84/90) pool save/load/clear and multi-mission carryover + stateHash parity.
    {
        auto makeScenarioMission = [&]() {
            auto md = makeFlatMission();
            for (int y = 11; y <= 14; ++y) {
                for (int x = 11; x <= 14; ++x) {
                    md.zones[static_cast<std::size_t>(y) * md.width + static_cast<std::size_t>(x)] = 2u;
                }
            }
            md.gates[0] = px64(12, 12);
            md.locations[2] = px64(26, 26); // OP(75) target=2 -> locations[1+1] = locations[2]

            const auto [sx20, sy20] = px64(13, 12); // group 20 soldier right next to gate 0
            const auto [vx20, vy20] = px64(12, 13); // group 20 empty truck next to gate 0
            const auto [sxZ2, syZ2] = px64(14, 14); // group 21 zone-2 soldier
            const auto [tx40, ty40] = px64(30, 30); // group 40 tank + embarked passenger
            const auto [rx41, ry41] = px64(31, 30); // group 41 extra player 0 soldier
            const auto [mx30a, my30a] = px64(34, 34);
            const auto [mx30b, my30b] = px64(35, 34);
            const auto [mx30c, my30c] = px64(36, 34);

            std::vector<ss2::assets::Infantry> inf{
                {"rifle_caof", sx20, sy20, 100, 100, 20, 0},
                {"rifle_caof", sxZ2, syZ2, 100, 100, 21, 0},
                {"rifle_caof", tx40, ty40, 100, 100, 40, 0}, // index 2 -> embark onto tank (veh 1)
                {"rifle_caof", rx41, ry41, 100, 100, 41, 0},
                {"rifle_caof", mx30a, my30a, 100, 100, 30, 0},
                {"rifle_caof", mx30b, my30b, 100, 100, 30, 0},
                {"rifle_caof", mx30c, my30c, 100, 100, 30, 0},
            };
            ss2::assets::VehicleUnit truck{};
            truck.id = "truck_caof"; truck.x = vx20; truck.y = vy20; truck.hp = 100; truck.owner = 0; truck.group = 20; truck.alive = true;
            ss2::assets::VehicleUnit tank{};
            tank.id = "tank_caof"; tank.x = tx40; tank.y = ty40; tank.hp = 100; tank.ammo = 100; tank.owner = 0; tank.group = 40; tank.alive = true;

            md.scripts.resize(4);
            // Script 0 (tick 0): evacuate group 20 & zone 2 to gate 0; OP(75) send 1 from group 30 to target=2;
            // save group 40 into pool 2; save player 0 into pool 3.
            md.scripts[0] = {
                11u,
                lit(20), lit(0), 85u,
                lit(2), lit(0), 86u,
                lit(1), lit(30), lit(2), 75u,
                lit(40), lit(2), 63u,
                lit(0), lit(3), 84u,
                0x7fffffffu
            };
            // Script 1 (scan tick 67): verify OP(90) pool 2 == 2 & pool 3 > 2 -> Message 0 + OP(62) clear pool 3.
            md.scripts[1] = {
                lit(2), lit(0), lit(2), 90u,
                lit(3), lit(1), lit(2), 90u,
                1u, 7u, 11u,
                lit(0), 15u,
                lit(3), 62u,
                0x7fffffffu
            };
            // Script 2 (scan tick 131, after OP(62) applied at tick 67): verify pool 3 == 0 -> Message 1.
            md.scripts[2] = {
                lit(1), lit(3), 29u,
                lit(3), lit(0), lit(0), 90u,
                1u, 7u, 11u,
                lit(1), 15u,
                0x7fffffffu
            };
            // Script 3 (scan tick 67 after evacuation): countByGroupMask(20, mask=4) == 2 -> OP(39) deploy + Message 2.
            md.scripts[3] = {
                lit(4), lit(20), lit(0), lit(2), 3u,
                7u, 11u,
                lit(20), lit(0), lit(1), lit(1), lit(80), lit(50), lit(40), lit(100), 39u,
                lit(2), 15u,
                0x7fffffffu
            };

            ss2::TutorialUnits units(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                inf, {}, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {truck, tank}, cat);
            check(units.embarkImmediate(2u, 1u), "test setup embarkImmediate failed");
            return std::pair{std::move(md), std::move(units)};
        };

        auto [mdA, uA] = makeScenarioMission();
        auto [mdB, uB] = makeScenarioMission();
        ss2::MissionRunner runnerA(ss2::MissionRunner::ManualClock{}, std::move(mdA), std::move(uA));
        ss2::MissionRunner runnerB(ss2::MissionRunner::ManualClock{}, std::move(mdB), std::move(uB));
        check(runnerA.stateHash() == runnerB.stateHash(), "CAOF complete twin initial stateHash mismatch");

        (void)runnerA.poll();
        (void)runnerB.poll();

        // Advance past scan ticks 67 & 131 and OP(39) 1s (25 tick) delay + evacuation arrival.
        runnerA.advanceTicks(140u);
        runnerB.advanceTicks(140u);
        check(runnerA.stateHash() == runnerB.stateHash(),
              "CAOF complete twin stateHash diverged after evac/reserve/OP(39)/OP(75)");

        std::vector<ss2::MissionEvent> allEvs = runnerA.poll();
        check(hasMessageEvent(allEvs, 0u), "OP(63/84/90) pool save + query check failed");
        check(hasMessageEvent(allEvs, 1u), "OP(62/90) pool clear check failed");
        check(hasMessageEvent(allEvs, 2u), "OP(85) evacuation to reserve failed to trigger OP(39) at tick 67");
        check(runnerA.countByGroupMask(20u, 1u) == 0u && runnerA.countByGroupMask(21u, 1u) == 0u,
              "OP(85)/OP(86) evacuated units still alive on map");
        check(runnerA.countByGroupMask(20u, 4u) == 0u, "OP(39) did not consume group 20 reserve");
        check(runnerA.countByGroupMask(21u, 4u) == 1u, "OP(86) zone-2 soldier did not enter reserve");

        bool sawDynWave = false;
        for (const auto& ev : allEvs) {
            if (ev.kind == ss2::MissionEvent::Reinforcement && ev.index >= 64u) {
                const auto wave = runnerA.reinforcementWave(ev.index);
                check(wave.size() == 2u, "OP(39) deployed dynamic wave size != 2");
                for (const auto& wu : wave) {
                    check(wu.hp == 80 && wu.ammo == 50 && wu.expa == 40,
                          "OP(39) HP/ammo/expa template scaling mismatch");
                }
                sawDynWave = true;
            }
        }
        check(sawDynWave, "OP(39) did not emit Reinforcement event for dynamic wave");

        // Check OP(75) moved only 1 of 3 group-30 soldiers from their initial cells around y=34.
        const auto poses30 = runnerA.pollPoses();
        int moved30 = 0;
        for (std::size_t idx : {4u, 5u, 6u}) {
            const auto it = std::ranges::find(poses30, idx, &ss2::SoldierPose::index);
            if (it != poses30.end() && (it->x != px64(static_cast<int>(30 + idx), 34).first ||
                                        it->y != px64(static_cast<int>(30 + idx), 34).second)) {
                ++moved30;
            }
        }
        check(moved30 == 1, "OP(75) should move only 1 soldier from group 30");

        // Multi-mission carryover: feed runnerA.savedUnitPools() into runnerC on tick 0.
        const auto carriedPools = runnerA.savedUnitPools();
        check(carriedPools.contains(2u) && !carriedPools.contains(3u),
              "savedUnitPools carryover map mismatch (pool 2 saved, pool 3 cleared)");

        auto mdC = makeFlatMission();
        mdC.scripts.push_back({
            lit(2), lit(1), lit(1), 90u, 7u, 11u,
            lit(4), lit(2), lit(77), 72u,
            lit(5), lit(2), 71u,
            lit(0), lit(4), lit(0), lit(0), lit(0), 40u,
            0x7fffffffu
        });
        ss2::TutorialUnits uC(
            ss2::InfantryPathGrid::fromSubCellMasks(mdC.width, mdC.height, mdC.flags, mdC.obstacles),
            {}, {}, ss2::FogGrid(mdC.width, mdC.height, mdC.flags), 2, false, {}, cat);
        ss2::MissionRunner runnerC(ss2::MissionRunner::ManualClock{}, std::move(mdC), std::move(uC), carriedPools);
        const auto evC = runnerC.poll();
        bool sawPoolReinf = false;
        for (const auto& ev : evC) {
            if (ev.kind == ss2::MissionEvent::Reinforcement && ev.index == 4u) sawPoolReinf = true;
        }
        check(sawPoolReinf, "Same-trigger OP(90)->OP(72)->OP(40) carryover wave spawn failed");
        const auto w4 = runnerC.reinforcementWave(4u);
        const auto w5 = runnerC.reinforcementWave(5u);
        check(w4.size() == 1u && w4[0].group == 77 && w4[0].passengers.size() == 1u &&
              w4[0].passengers[0].group == 77,
              "OP(72) wave group override failed");
        check(w5.size() == 1u && w5[0].group == 40 && w5[0].passengers.size() == 1u &&
              w5[0].passengers[0].group == 40,
              "OP(71) wave preserve-group failed");

        // Ensure saved/dynamic pool changes perturb stateHash sensitivity.
        auto [mdD, uD] = makeScenarioMission();
        ss2::MissionRunner runnerD(ss2::MissionRunner::ManualClock{}, std::move(mdD), std::move(uD), carriedPools);
        check(runnerD.stateHash() != runnerC.stateHash(), "MissionRunner::stateHash did not reflect mission diff");
    }

    // 5. Complete FullSnapshot mid-mission save/load parity across OP(40) reinforcements,
    //    VM timers/variables/AI/air-routes/pools, scheduled waves, evacuations, mines, fog & weather.
    {
        auto buildRichMission = [&](ss2::assets::WeatherKind initWeather)
            -> std::pair<ss2::assets::MissionData, ss2::TutorialUnits> {
            auto md = makeFlatMission();
            md.weather = initWeather;
            md.airSupport[0][0] = {"he111", 2, 1};
            md.reinforcements[0] = {
                {"rifle_caof", 31, 95, 90, 20, {}},
                {"tank_caof", 31, 100, 100, 40, {{"rifle_caof", 31, 100, 100, 20, true}}},
            };
            md.reinforcements[1] = {
                {"rifle_caof", 32, 100, 100, 0, {}},
            };
            md.scripts.push_back({
                51u, 7u, 11u,
                lit(1), lit(3), 12u,
                lit(0x7fffff02u), lit(77), 47u,
                lit(5), lit(11), 19u,
                lit(1), lit(2), lit(5), 79u,
                lit(1), lit(2), 44u,
                lit(0), lit(2), lit(0), 24u,
                lit(0), lit(0), lit(0), lit(1), lit(0), 40u,
                lit(0), lit(1), lit(0), lit(1), lit(2), 40u,
                lit(1), lit(2), 63u,
                lit(6), lit(2), lit(44), 72u,
                lit(2), lit(0), 85u,
                0x7fffffffu
            });
            md.scripts.push_back({
                lit(1), 27u, 7u,
                lit(10), 14u, 51u,
                0x7fffffffu
            });

            const auto [sx, sy] = px64(32, 32);
            const auto [mx, my] = px64(14, 14);
            std::vector<ss2::assets::Infantry> initInf{
                {"rifle_caof", sx, sy, 100, 40, 1, 0},
                {"rifle_caof", mx, my, 100, 40, 2, 0},
            };
            auto fog = ss2::FogGrid(md.width, md.height, md.flags);
            fog.revealCircle(32, 32, 8, 0);
            ss2::TutorialUnits units(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                initInf, {}, std::move(fog), 2, false, {}, cat);
            units.setWeather(initWeather);
            units.setAirSupport(md.airSupport, md.paratrooperGroup, md.paratrooperRoster);
            return {std::move(md), std::move(units)};
        };

        auto [mdA, uA] = buildRichMission(ss2::assets::WeatherKind::Snow);
        ss2::MissionRunner runnerA(ss2::MissionRunner::ManualClock{}, std::move(mdA), std::move(uA));
        const auto earlyFull = runnerA.takeFullSnapshot();
        check(earlyFull.units.soldiers.size() == 2u && earlyFull.units.vehicles.empty(),
              "Pre-reinforcement FullSnapshot initial counts mismatch");

        // Trigger tick-0 OP(51) fires and queues OP(40) wave 0 (immediate) + wave 1 (2s = 50 ticks delayed).
        const auto ev0 = runnerA.poll();
        bool sawWave0 = false;
        for (const auto& ev : ev0) {
            if (ev.kind == ss2::MissionEvent::Reinforcement && ev.index == 0u) sawWave0 = true;
        }
        check(sawWave0, "OP(40) wave 0 did not emit on tick 0");
        // Spawn reinforcement units matching Session behavior (1 rifleman + 1 tank).
        (void)runnerA.spawnInfantry({"rifle_caof", px64(14, 14).first, px64(14, 14).second, 95, 36, 31, 0}, 6553);
        ss2::assets::VehicleUnit reinfTank{};
        reinfTank.id = "tank_caof";
        reinfTank.x = px64(15, 14).first;
        reinfTank.y = px64(15, 14).second;
        reinfTank.hp = 500;
        reinfTank.ammo = 20;
        reinfTank.owner = 0;
        reinfTank.group = 31;
        reinfTank.alive = true;
        reinfTank.hasCrew = true;
        (void)runnerA.spawnVehicle(reinfTank);
        runnerA.advanceTicks(10u);

        const auto midFull = runnerA.takeFullSnapshot();
        check(midFull.units.soldiers.size() == 3u && midFull.units.vehicles.size() == 1u,
              "Post-reinforcement FullSnapshot counts mismatch");
        check(!midFull.tutorial.active[0], "Tick-0 OP(51) script should remain inactive after firing");
        check(midFull.scheduledReinforcements.size() == 1u &&
                  midFull.scheduledReinforcements[0].event.index == 1u,
              "Pending scheduled OP(40) wave missing from FullSnapshot");

        const auto serialized = ss2::MissionRunner::serializeFullSnapshot(midFull);
        const auto decoded = ss2::MissionRunner::deserializeFullSnapshot(serialized);
        check(decoded.has_value() && *decoded == midFull, "SS2FULL1 serialize/deserialize round-trip failed");

        // Load post-reinforcement save into two BRAND NEW pre-reinforcement runners (Clear weather).
        auto [mdB, uB] = buildRichMission(ss2::assets::WeatherKind::Clear);
        ss2::MissionRunner runnerB(ss2::MissionRunner::ManualClock{}, std::move(mdB), std::move(uB));
        auto [mdC, uC] = buildRichMission(ss2::assets::WeatherKind::Clear);
        ss2::MissionRunner runnerC(ss2::MissionRunner::ManualClock{}, std::move(mdC), std::move(uC));
        check(!runnerB.loadSnapshot(midFull.units),
              "Legacy loadSnapshot without ExtraState should still reject grown counts");
        check(runnerB.loadFullSnapshot(*decoded) && runnerC.loadFullSnapshot(*decoded),
              "loadFullSnapshot into pre-reinforcement runner failed");
        check(runnerB.simTick() == runnerA.simTick() && runnerB.takeFullSnapshot() == midFull,
              "Loaded runnerB FullSnapshot diverged from runnerA");
        check(runnerB.stateHash() == runnerC.stateHash(),
              "Two runners loaded from identical SS2FULL1 diverged in stateHash");
        check(runnerB.airSupportFor(0)[0].total == 2 && runnerB.airSupportFor(0)[0].ready == 3,
              "Air plane total/ready mismatch after loadFullSnapshot");
        check(runnerB.savedUnitPools().contains(2u) && !runnerB.reinforcementWave(6u).empty(),
              "Pools/dynamic waves missing after loadFullSnapshot");
        check(runnerB.countByPlayerMask(0, 4u) == runnerA.countByPlayerMask(0, 4u),
              "Reserve count mismatch after loadFullSnapshot");

        // Advance past scheduled wave 1 (tick 50) + timer (tick 75) on both loaded runners and confirm
        // deterministic parity, OP(85) evacuation completion, AND no duplicate wave 0.
        runnerB.advanceTicks(125u);
        runnerC.advanceTicks(125u);
        check(runnerB.stateHash() == runnerC.stateHash(),
              "Post-load evolution diverged between two identically loaded runners");
        check(runnerB.countByPlayerMask(0, 4u) >= 1u, "OP(85) evacuating unit did not reach reserve after load");
        const auto evB = runnerB.poll();
        int wave0Count = 0;
        int wave1Count = 0;
        for (const auto& ev : evB) {
            if (ev.kind == ss2::MissionEvent::Reinforcement && ev.index == 0u) ++wave0Count;
            if (ev.kind == ss2::MissionEvent::Reinforcement && ev.index == 1u) ++wave1Count;
        }
        check(wave0Count == 0 && wave1Count == 1,
              "Expected no duplicate wave 0 and exactly 1 scheduled wave 1 after loadFullSnapshot");

        // Now load the EARLIER pre-reinforcement save into runnerB and verify clean shrink-back!
        check(runnerB.loadFullSnapshot(earlyFull), "Shrink-back loadFullSnapshot(earlyFull) failed");
        const auto shrunkSnap = runnerB.takeFullSnapshot();
        check(shrunkSnap == earlyFull && shrunkSnap.units.soldiers.size() == 2u &&
                  shrunkSnap.units.vehicles.empty() && shrunkSnap.tutorial.active[1] &&
                  shrunkSnap.unitsExtra.weather == ss2::assets::WeatherKind::Snow,
              "Shrink-back loadFullSnapshot did not restore pre-reinforcement counts/VM state");
    }

    // 4. Destructible & Repairable Bridges: demolition, unit plunge, OP(41), supply-truck repair & FullSnapshot.
    {
        ss2::assets::TutorialData brData{};
        brData.width = 128;
        brData.height = 128;
        brData.flags.assign(128u * 128u, 0u);
        brData.obstacles.assign(128u * 128u, 0u);
        // Carve impassable water river along tileX = 20, except tileY == 20 where our bridge sits.
        for (int ty = 0; ty < 128; ++ty) {
            if (ty == 20) continue;
            brData.obstacles[static_cast<std::size_t>(ty) * 128u + 20u] = 0x00F0u;
        }
        brData.zones.assign(128u * 128u, 0u);
        brData.phrases.resize(1);
        const auto [brPx, brPy] = tc(20, 20);
        brData.markers[0] = {brPx, brPy}; // marker 0 = bridge center
        constexpr std::uint32_t kVarBase = 0x7fffff00u;
        // Trigger 0: OP(41) when bridge at marker 0 is destroyed -> set var[0] = 1
        brData.scripts.push_back({
            lit(0u), 41u, 7u,
            lit(kVarBase + 0u), lit(1u), 47u, 11u,
            0x7fffffffu
        });
        // Trigger 1: once bridge is repaired back (not destroyed && var[0] == 1) -> set var[1] = 1
        brData.scripts.push_back({
            lit(0u), 41u, 26u, 7u,
            lit(kVarBase + 0u), lit(0u), lit(1u), 50u, 7u,
            lit(kVarBase + 1u), lit(1u), 47u, 11u,
            0x7fffffffu
        });

        ss2::assets::UnitSpec infSpec{};
        infSpec.id = "dguner";
        infSpec.isVehicle = false;
        infSpec.health = 9000;
        infSpec.ammo1 = 40;
        infSpec.moveSpeed = 1.6;
        infSpec.sight = 10;

        ss2::assets::UnitSpec howSpec{};
        howSpec.id = "wespe";
        howSpec.isVehicle = true;
        howSpec.health = 500;
        howSpec.ammo1 = 40;
        howSpec.moveSpeed = 1.5;
        howSpec.sight = 14;
        howSpec.roleClass = "artillery";
        howSpec.shot1 = {"he105", ss2::assets::HitType::Explosive, 650, 1200, 600, 600, 0, 10, 1, 10, "howitzer"};
        howSpec.shot1.indirectArc = true;
        howSpec.shot1.accuracyBase = 4;
        howSpec.shot1.accuracyMin = 4;

        ss2::assets::UnitSpec supSpec{};
        supSpec.id = "ss";
        supSpec.isVehicle = true;
        supSpec.selType = "sappers";
        supSpec.ammoRegenDelay = 8;
        supSpec.health = 240;
        supSpec.ammo1 = 5000;
        supSpec.ammo2 = 5000;
        supSpec.moveSpeed = 2.0;
        supSpec.sight = 10;
        supSpec.roleClass = "supply";
        supSpec.stock = 44;

        ss2::assets::UnitSpec tankVictim{};
        tankVictim.id = "t34";
        tankVictim.isVehicle = true;
        tankVictim.health = 9000;
        tankVictim.ammo1 = 30;
        tankVictim.moveSpeed = 1.8;
        tankVictim.sight = 10;
        tankVictim.roleClass = "tank";
        tankVictim.stock = 4;

        const auto brCat = ss2::assets::UnitCatalog::fromSpecs({infSpec, howSpec, supSpec, tankVictim});
        const auto [howX, howY] = tc(17, 20);
        const auto [supX, supY] = tc(11, 20);
        const auto [westX, westY] = tc(16, 20);
        const auto [eastX, eastY] = tc(22, 20);
        const auto [onBrX, onBrY] = tc(20, 20);

        // Two infantry: one on bridge (will drown), one on west bank (tests route across bridge); plus passenger on bridge tank.
        std::vector<ss2::assets::Infantry> inf{
            {"dguner", onBrX, onBrY, 9000, 40, 1, 0},
            {"dguner", westX, westY, 100, 40, 2, 0},
            {"dguner", eastX, eastY, 100, 40, 3, 1},
        };
        std::vector<ss2::assets::Placement> veh{
            {"wespe", howX, howY, 0, 100, 100, 4, 0},
            {"ss", supX, supY, 0, 100, 100, 5, 0},
            {"t34", onBrX + 4, onBrY, 0, 100, 100, 6, 1},
        };

        ss2::assets::BridgePlacement bp{};
        bp.x = brPx;
        bp.y = brPy;
        bp.left = brPx - 32;
        bp.top = brPy - 24;
        bp.width = 64;
        bp.height = 48;
        bp.maxHp = 3500;
        // Passable at stages 0..3 (0xF0), blocked at stage 4 (0); bits 0x01|0x04 = passable+hit.
        bp.cells = {
            {40, 40, 0xF5u},
            {41, 40, 0xF5u},
            {40, 41, 0xF5u},
            {41, 41, 0xF5u},
        };

        auto brGrid = ss2::InfantryPathGrid::fromSubCellMasks(brData.width, brData.height, brData.flags, brData.obstacles);
        ss2::FogGrid brFog(brData.width, brData.height, brData.flags);
        ss2::TutorialUnits brUnits(std::move(brGrid), inf, veh, std::move(brFog), 2, false, {}, brCat);
        brUnits.setBridges({bp});
        check(brUnits.embarkImmediate(2, 2), "Expected soldier 2 to embark on bridge tank before demolition");
        check(brUnits.canMove(std::size_t{1}, eastX, eastY), "Expected west-bank soldier canMove across intact bridge");
        check(!brUnits.isObjectDestroyedAt(brPx, brPy), "OP(41) expected intact bridge -> not destroyed");

        ss2::MissionRunner brRunner(ss2::MissionRunner::ManualClock{}, brData, std::move(brUnits));
        check(brRunner.countByGroupMask(1u, 1u) == 1u, "Expected soldier on bridge alive before collapse");
        check(brRunner.countByGroupMask(6u, 3u) == 1u, "Expected tank on bridge alive before collapse");
        check(brRunner.countByGroupMask(3u, 1u) == 1u, "Expected embarked rider alive before collapse");
        check(brRunner.attackGroundVehicles(std::array<std::size_t, 1>{0u}, brPx, brPy).value_or(0u) == 1u,
              "Expected howitzer to accept attack-ground order on bridge");

        bool sawDamage = false;
        bool sawCollapse = false;
        for (int tick = 0; tick < 250; ++tick) {
            brRunner.advanceTicks(1u);
            for (const auto& u : brRunner.pollBridgeUpdates()) {
                if (u.damagedThisTick) sawDamage = true;
                if (u.collapsedThisTick && u.destroyed && u.stage == 4 && u.hp == 0) sawCollapse = true;
            }
            if (sawCollapse) break;
        }
        check(sawDamage && sawCollapse, "Expected artillery bombardment to damage and collapse bridge to stage 4");
        (void)brRunner.stopVehicles(std::array<std::size_t, 1>{0u});
        brRunner.advanceTicks(2u);
        for (int s = 0; s < 65 && brRunner.takeFullSnapshot().tutorial.variables[0] == 0u; ++s) {
            brRunner.advanceTicks(1u);
        }
        check(brRunner.countByGroupMask(1u, 1u) == 0u, "Expected soldier standing on bridge to plunge on collapse");
        check(brRunner.countByGroupMask(6u, 3u) == 0u, "Expected vehicle on bridge to plunge on collapse");
        check(brRunner.countByGroupMask(3u, 1u) == 0u, "Expected embarked rider in plunged tank to drown too");
        (void)brRunner.move(std::array<std::size_t, 1>{1u}, eastX, eastY);
        brRunner.advanceTicks(180u);
        const auto collapsedSnap = brRunner.takeFullSnapshot();
        check(collapsedSnap.units.soldiers[1].cellX / 2 <= 19,
              "Expected collapsed bridge to block soldier 1 on west bank");
        check(collapsedSnap.tutorial.variables[0] == 1u,
              "Expected OP(41) trigger to observe destroyed bridge");

        // Supply truck must repair collapsed bridge back to full HP and restore passability + OP(41).
        check(brRunner.canSupportBridge(std::array<std::size_t, 1>{1u}, 0u),
              "Expected supply truck with ammo2 to report canSupportBridge=true");
        check(brRunner.orderSupportBridge(std::array<std::size_t, 1>{1u}, 0u).value_or(0u) == 1u,
              "Expected supply truck to accept orderSupportBridge");

        bool sawRepair = false;
        int finalBridgeHp = 0;
        for (int tick = 0; tick < 1000; ++tick) {
            brRunner.advanceTicks(1u);
            for (const auto& u : brRunner.pollBridgeUpdates()) {
                if (u.repairedThisTick) sawRepair = true;
                finalBridgeHp = u.hp;
            }
            if (finalBridgeHp >= 3500) break;
        }
        check(sawRepair && finalBridgeHp == 3500, "Expected supply truck to fully repair bridge to 3500 HP");
        brRunner.advanceTicks(2u);
        for (int s = 0; s < 65 && brRunner.takeFullSnapshot().tutorial.variables[1] == 0u; ++s) {
            brRunner.advanceTicks(1u);
        }
        const auto postRepairFull = brRunner.takeFullSnapshot();
        check(postRepairFull.units.vehicles[1].ammo2 < 5000,
              "Expected supply truck repair to expend finite spare parts ammo2");
        check(postRepairFull.tutorial.variables[1] == 1u,
              "Expected OP(41) to detect bridge became intact again after repair");
        check(postRepairFull.unitsExtra.bridges.size() == 1u &&
                  postRepairFull.unitsExtra.bridges[0].hp == 3500 &&
                  !postRepairFull.unitsExtra.bridges[0].destroyed,
              "Expected FullSnapshot ExtraState to capture repaired bridge");
        (void)brRunner.move(std::array<std::size_t, 1>{1u}, eastX, eastY);
        brRunner.advanceTicks(180u);
        check(brRunner.takeFullSnapshot().units.soldiers[1].cellX / 2 >= 21,
              "Expected soldier 1 to cross repaired bridge to east bank");
        check(brRunner.loadFullSnapshot(collapsedSnap), "Expected loadFullSnapshot to restore collapsed bridge state");
        (void)brRunner.move(std::array<std::size_t, 1>{1u}, eastX, eastY);
        brRunner.advanceTicks(120u);
        const auto restoredCollapsed = brRunner.takeFullSnapshot();
        check(restoredCollapsed.unitsExtra.bridges[0].stage == 4 &&
                  restoredCollapsed.units.soldiers[1].cellX / 2 <= 19,
              "Expected restored collapsed bridge to re-block crossing");
        check(brRunner.loadFullSnapshot(postRepairFull), "Expected loadFullSnapshot to restore repaired bridge state");
        (void)brRunner.move(std::array<std::size_t, 1>{1u}, eastX, eastY);
        brRunner.advanceTicks(180u);
        const auto restoredRepaired = brRunner.takeFullSnapshot();
        check(restoredRepaired.unitsExtra.bridges[0].hp == 3500 &&
                  restoredRepaired.units.soldiers[1].cellX / 2 >= 21,
              "Expected restored repaired bridge to allow crossing");
    }

    // 5. Retail I-GROUPS initial AI: garrisoning, zone movement + aif_holdfire, follow, supply support,
    //    16-tick auto-reserve, OP(19) packed flags, and SS2FULL3 round-trip.
    {
        ss2::assets::MissionData igData{};
        igData.width = 128;
        igData.height = 128;
        igData.flags = emptyFlags128();
        igData.obstacles = emptyObstacles128();
        igData.zones.assign(128u * 128u, 0u);
        igData.phrases.resize(1);
        const auto [z1x, z1y] = tc(24, 20);
        const auto [z2x, z2y] = tc(28, 20);
        const auto [z3x, z3y] = tc(36, 20);
        const auto [z4x, z4y] = tc(16, 20);
        igData.locations[1] = {z1x, z1y};
        igData.locations[2] = {z2x, z2y};
        igData.locations[3] = {z3x, z3y};
        igData.locations[4] = {z4x, z4y};
        igData.gates[0] = tc(16, 20);
        igData.gateTargets[0] = tc(18, 20);
        igData.hasInitialGroups = true;
        // Group 1: ai_sold_guard (2) with aif_useobjects (1<<3) in zone 1 -> garrisons house in zone 1.
        igData.initialGroups[1].behavior = 2u;
        igData.initialGroups[1].aiFlags = (1u << 3);
        igData.initialGroups[1].zone1 = 1u;
        // Group 2: ai_tank_guard (5) with aif_holdfire (1<<22) in zone 2 -> moves & holds fire.
        igData.initialGroups[2].behavior = 5u;
        igData.initialGroups[2].aiFlags = (1u << 22);
        igData.initialGroups[2].zone1 = 2u;
        // Group 3: ai_tank_guard (5) advancing to zone 3 (leader for group 4).
        igData.initialGroups[3].behavior = 5u;
        igData.initialGroups[3].zone1 = 3u;
        // Group 4: ai_tank_follow (6) following group 3.
        igData.initialGroups[4].behavior = 6u;
        igData.initialGroups[4].group1 = 3u;
        // Group 5: ai_gruz_reload (9) supporting group 2's damaged tank.
        igData.initialGroups[5].behavior = 9u;
        igData.initialGroups[5].group1 = 2u;
        // Group 6: periodic auto-reserve trigger at tick 25 ((25 & 0xf) == 9).
        igData.initialGroups[6].reservGate = 0u;
        igData.initialGroups[6].reservZone = 4u;
        igData.initialGroups[6].reservArrivalDelayTicks = 0u;
        igData.initialGroups[6].reservMinDelayTicks = 25u;
        igData.initialGroups[6].reservMinUnits = 0u;
        igData.initialGroups[6].reservForceDelayTicks = 0x7fffffffu;
        igData.initialGroups[6].reservHpPct = 80u;
        igData.initialGroups[6].reservAmmoPct = 90u;
        igData.initialGroups[6].reservExpaPct = 30u;
        igData.initialGroups[6].reservMoralePct = 60u;

        // Script tests OP(19) with packed word (behavior=2 | ((1<<22 | 1)<<4) = 0x04000012u).
        igData.scripts.push_back({
            lit(7u), lit(0x04000012u), 19u, 11u, 0x7fffffffu
        });

        ss2::assets::UnitSpec rifle{};
        rifle.id = "dguner";
        rifle.isVehicle = false;
        rifle.health = 100;
        rifle.ammo1 = 40;
        rifle.moveSpeed = 1.6;
        rifle.sight = 10;

        ss2::assets::UnitSpec tank{};
        tank.id = "t34";
        tank.isVehicle = true;
        tank.health = 600;
        tank.ammo1 = 30;
        tank.moveSpeed = 2.0;
        tank.sight = 12;
        tank.roleClass = "tank";
        tank.shot1 = {"ap76", ss2::assets::HitType::Pierce, 100, 100, 350, 350, 0, 20, 1, 20, "tankgun"};

        ss2::assets::UnitSpec sup{};
        sup.id = "ss";
        sup.isVehicle = true;
        sup.selType = "sappers";
        sup.ammoRegenDelay = 8;
        sup.health = 240;
        sup.ammo1 = 5000;
        sup.ammo2 = 5000;
        sup.moveSpeed = 2.2;
        sup.sight = 10;
        sup.stock = 44;

        const auto igCat = ss2::assets::UnitCatalog::fromSpecs({rifle, tank, sup});
        const auto [s1x, s1y] = tc(21, 20);
        const auto [p0x, p0y] = tc(27, 20);
        const auto [t2x, t2y] = tc(20, 22);
        const auto [t3x, t3y] = tc(24, 24);
        const auto [t4x, t4y] = tc(24, 26);
        const auto [supStartX, supStartY] = tc(20, 24);

        std::vector<ss2::assets::Infantry> inf{
            {"dguner", s1x, s1y, 100, 40, 1, 1}, // group 1 garrisoner
            {"dguner", p0x, p0y, 100, 40, 0, 0}, // nearby player enemy to verify group 2 holdfire
        };
        std::vector<ss2::assets::Placement> veh{
            {"t34", t2x, t2y, 0, 50, 100, 2, 1},        // idx 0: damaged tank in group 2 (ai_tank_guard + holdfire)
            {"t34", t3x, t3y, 0, 100, 100, 3, 1},       // idx 1: group 3 leader tank moving to zone 3
            {"t34", t4x, t4y, 0, 100, 100, 4, 1},       // idx 2: group 4 follower tank (group1 = 3)
            {"ss", supStartX, supStartY, 0, 100, 100, 5, 1}, // idx 3: group 5 supply truck (ai_gruz_reload for group 2)
        };
        auto igGrid = ss2::InfantryPathGrid::fromSubCellMasks(igData.width, igData.height, igData.flags, igData.obstacles);
        ss2::FogGrid igFog(igData.width, igData.height, igData.flags);
        ss2::TutorialUnits igUnits(std::move(igGrid), inf, veh, std::move(igFog), 2, false, {}, igCat);
        ss2::assets::BuildingPlacement house{};
        house.x = z1x;
        house.y = z1y;
        house.left = z1x - 24;
        house.top = z1y - 24;
        house.width = 48;
        house.height = 48;
        house.capacity = 4;
        house.maxHp = 400;
        igUnits.setBuildings({house});
        igUnits.addReserveUnit({"dguner", 1, 6, 100, 100, 10, 0});

        ss2::MissionRunner igRunner(ss2::MissionRunner::ManualClock{}, std::move(igData), std::move(igUnits));
        bool sawReserveDeploy = false;
        for (unsigned t = 0u; t < 200u; ++t) {
            igRunner.advanceTicks(1u);
            for (const auto& ev : igRunner.poll()) {
                if (ev.kind == ss2::MissionEvent::Reinforcement &&
                    ev.owner == 1u && ev.gate == 0u && ev.destZone == 4u) {
                    sawReserveDeploy = true;
                }
            }
        }
        const auto igSnap = igRunner.takeFullSnapshot();
        check(igSnap.units.soldiers[0].garrisonBuilding == 0,
              "Expected I-GROUPS ai_sold_guard with aif_useobjects to garrison building in zone 1");
        check(igSnap.units.vehicles[0].holdFire,
              "Expected I-GROUPS aif_holdfire on group 2 to enforce holdFire=true");
        check(igSnap.units.vehicles[0].x > t2x,
              "Expected I-GROUPS ai_tank_guard to move group 2 toward zone 2");
        check(igSnap.units.vehicles[0].hp > 300,
              "Expected I-GROUPS ai_gruz_reload group 5 supply truck to repair group 2 tank");
        check(igSnap.units.vehicles[1].x > t3x && igSnap.units.vehicles[2].x > t4x,
              "Expected I-GROUPS ai_tank_follow group 4 to follow group 3 leader");
        check(sawReserveDeploy && igSnap.tutorial.groupAi[6].lastReserveTick == 25u,
              "Expected I-GROUPS 16-tick auto-reserve to deploy group 6 at tick 25");
        check(igSnap.tutorial.groupAi[7].behavior == 2u &&
                  igSnap.tutorial.groupAi[7].aiFlags == 0x00400001u,
              "Expected OP(19) to split packed behavior and aiFlags bits 4..31");

        const auto ser3 = ss2::MissionRunner::serializeFullSnapshot(igSnap);
        const auto dec3 = ss2::MissionRunner::deserializeFullSnapshot(ser3);
        check(dec3.has_value() && *dec3 == igSnap,
              "Expected SS2FULL3 round-trip to preserve group aiFlags and lastReserveTick");
    }

    // 6. Specialized retail I-GROUPS AI behaviors (ai_sold_art, ai_recon, ai_furg_transport,
    //    ai_katya_move, ai_train_move) and conditional zone gating (aif_zonehaveown, aif_zonenoenemy).
    {
        ss2::assets::UnitSpec crewInf{};
        crewInf.id = "dguner";
        crewInf.isVehicle = false;
        crewInf.health = 100;
        crewInf.ammo1 = 40;
        crewInf.moveSpeed = 1.8;
        crewInf.sight = 12;
        crewInf.canBeCrew = 1;
        crewInf.shot1 = {"rifle", ss2::assets::HitType::Machine, 18, 18, 220, 220, 0, 12, 1, 12, "rifle"};

        ss2::assets::UnitSpec scoutSpec{};
        scoutSpec.id = "sdkfz.222";
        scoutSpec.isVehicle = true;
        scoutSpec.health = 240;
        scoutSpec.ammo1 = 40;
        scoutSpec.moveSpeed = 2.4;
        scoutSpec.canMoveBackward = true;
        scoutSpec.backMoveSpeed = 1.6;
        scoutSpec.sight = 16;
        scoutSpec.marchEnabled = true;
        scoutSpec.marchSightBonus = 4;
        scoutSpec.shot1 = {"ac20", ss2::assets::HitType::Pierce, 35, 35, 320, 320, 0, 10, 2, 24, "ac20"};

        ss2::assets::UnitSpec tankSpec{};
        tankSpec.id = "t34";
        tankSpec.isVehicle = true;
        tankSpec.health = 600;
        tankSpec.ammo1 = 30;
        tankSpec.moveSpeed = 2.0;
        tankSpec.sight = 12;
        tankSpec.roleClass = "tank";
        tankSpec.shot1 = {"ap76", ss2::assets::HitType::Pierce, 110, 110, 360, 360, 0, 20, 1, 20, "tankgun"};

        ss2::assets::UnitSpec howitzerSpec{};
        howitzerSpec.id = "lefh18";
        howitzerSpec.isVehicle = true;
        howitzerSpec.selType = "howitzer";
        howitzerSpec.turretType = "gaub";
        howitzerSpec.towWeightClass = "medium";
        howitzerSpec.health = 260;
        howitzerSpec.ammo1 = 30;
        howitzerSpec.moveSpeed = 0.0;
        howitzerSpec.sight = 12;
        howitzerSpec.crewNumber = 2;
        howitzerSpec.attackCrew = 1;
        howitzerSpec.shot1 = {"he105", ss2::assets::HitType::Explosive, 220, 220, 700, 700, 120, 25, 1, 25, "howitzer"};
        howitzerSpec.shot1.indirectArc = true;

        ss2::assets::UnitSpec atgSpec{};
        atgSpec.id = "pak40";
        atgSpec.isVehicle = true;
        atgSpec.selType = "atg";
        atgSpec.roleClass = "at";
        atgSpec.turretType = "atg";
        atgSpec.towWeightClass = "light";
        atgSpec.health = 200;
        atgSpec.ammo1 = 30;
        atgSpec.moveSpeed = 0.0;
        atgSpec.sight = 12;
        atgSpec.crewNumber = 2;
        atgSpec.attackCrew = 1;
        atgSpec.shot1 = {"ap75", ss2::assets::HitType::Pierce, 140, 140, 520, 520, 0, 18, 1, 18, "pak"};

        ss2::assets::UnitSpec apcSpec{};
        apcSpec.id = "sdkfz.251";
        apcSpec.isVehicle = true;
        apcSpec.health = 300;
        apcSpec.moveSpeed = 2.4;
        apcSpec.sight = 11;
        apcSpec.stock = 4;

        ss2::assets::UnitSpec katyaSpec{};
        katyaSpec.id = "bm-13";
        katyaSpec.isVehicle = true;
        katyaSpec.selType = "rl";
        katyaSpec.health = 220;
        katyaSpec.ammo1 = 16;
        katyaSpec.moveSpeed = 2.2;
        katyaSpec.sight = 12;
        katyaSpec.shot1 = {"katy", ss2::assets::HitType::Explosive, 240, 240, 760, 760, 140, 5, 4, 180, "katyusha"};
        katyaSpec.shot1.indirectArc = true;

        ss2::assets::UnitSpec trainSpec{};
        trainSpec.id = "armored_train";
        trainSpec.isVehicle = true;
        trainSpec.selType = "train";
        trainSpec.targetType = "train";
        trainSpec.walkOnGround = false;
        trainSpec.walkOnWater = false;
        trainSpec.walkOnShallows = false;
        trainSpec.canMoveBackward = true;
        trainSpec.moveSpeed = 2.2;
        trainSpec.backMoveSpeed = 2.0;
        trainSpec.health = 1200;
        trainSpec.ammo1 = 50;
        trainSpec.sight = 14;
        trainSpec.shot1 = {"train76", ss2::assets::HitType::Explosive, 130, 130, 480, 480, 0, 18, 1, 18, "tankgun"};

        const auto specCat = ss2::assets::UnitCatalog::fromSpecs({
            crewInf, scoutSpec, tankSpec, howitzerSpec, atgSpec, apcSpec, katyaSpec, trainSpec
        });

        // 6a. Conditional zone flags: aif_zonehaveown (1<<1) and aif_zonenoenemy (1<<2) + mid-advance stop.
        {
            auto md = makeFlatMission();
            md.obstacles.assign(64u * 64u, 0u);
            md.scripts.push_back({0x7fffffffu});
            for (int y = 18; y <= 22; ++y) {
                for (int x = 28; x <= 32; ++x) md.zones[std::size_t(y) * 64u + std::size_t(x)] = 1u;
                for (int x = 38; x <= 42; ++x) md.zones[std::size_t(y) * 64u + std::size_t(x)] = 2u;
            }
            md.locations[1] = px64(30, 20);
            md.locations[2] = px64(40, 20);
            md.hasInitialGroups = true;
            // Group 1 requires friendly unit in zone 1 before advancing.
            md.initialGroups[1].behavior = ss2::igroups::kAiTankGuard;
            md.initialGroups[1].aiFlags = ss2::igroups::kAifZoneHaveOwn;
            md.initialGroups[1].zone1 = 1u;
            // Group 2 requires zero enemy units in zone 2 before advancing.
            md.initialGroups[2].behavior = ss2::igroups::kAiTankGuard;
            md.initialGroups[2].aiFlags = ss2::igroups::kAifZoneNoEnemy;
            md.initialGroups[2].zone1 = 2u;

            const auto [t1x, t1y] = px64(14, 20);
            const auto [t2x, t2y] = px64(8, 20);
            const auto [allyStartX, allyStartY] = px64(26, 20); // 2 tiles west of zone 1 (x=28..32)
            const auto [enemyInZ2X, enemyInZ2Y] = px64(42, 20); // inside zone 2 east edge (x=38..42)
            const auto [enemyOutX, enemyOutY] = px64(44, 20);   // 2 tiles outside zone 2

            std::vector<ss2::assets::Infantry> inf{
                {"dguner", enemyInZ2X, enemyInZ2Y, 100, 40, 0, 0}, // player 0 blocking zone 2
                {"dguner", allyStartX, allyStartY, 100, 40, 9, 1}, // friendly outside zone 1 initially
            };
            std::vector<ss2::assets::Placement> veh{
                {"t34", t1x, t1y, 0, 100, 100, 1, 1},
                {"t34", t2x, t2y, 0, 100, 100, 2, 1},
            };
            ss2::TutorialUnits units(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, specCat);
            ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));

            runner.advanceTicks(90u);
            auto snapBlocked = runner.takeFullSnapshot();
            check(snapBlocked.units.vehicles[0].x == t1x && snapBlocked.units.vehicles[0].y == t1y,
                  "aif_zonehaveown group 1 moved before friendly entered zone 1");
            check(snapBlocked.units.vehicles[1].x == t2x && snapBlocked.units.vehicles[1].y == t2y,
                  "aif_zonenoenemy group 2 moved while enemy occupied zone 2");

            // Move friendly soldier 1 into zone 1 and player soldier 0 just outside zone 2.
            (void)runner.orderGroupMove(9u, px64(30, 20).first, px64(30, 20).second, true);
            (void)runner.move(std::array<std::size_t, 1>{0u}, enemyOutX, enemyOutY);
            runner.advanceTicks(130u);
            auto snapOpen = runner.takeFullSnapshot();
            check(snapOpen.units.vehicles[0].x > t1x + 48,
                  "aif_zonehaveown group 1 did not advance after friendly entered zone 1");
            const int midX = snapOpen.units.vehicles[1].x;
            check(midX > t2x + 48,
                  "aif_zonenoenemy group 2 did not advance after enemy cleared zone 2");

            // Re-enter zone 2 with player unit while group 2 tank is still mid-map -> must halt mid-advance.
            (void)runner.move(std::array<std::size_t, 1>{0u}, enemyInZ2X, enemyInZ2Y);
            runner.advanceTicks(100u);
            const int stoppedX = runner.takeFullSnapshot().units.vehicles[1].x;
            runner.advanceTicks(80u);
            check(runner.takeFullSnapshot().units.vehicles[1].x == stoppedX &&
                      stoppedX < px64(36, 20).first,
                  "aif_zonenoenemy group 2 failed to halt mid-advance when enemy re-entered zone 2");
        }

        // 6b. ai_sold_art (4): infantry crew neutral howitzer & friendly decrewed AT gun, respect aif_atgnouse, and fire.
        {
            auto md = makeFlatMission();
            md.obstacles.assign(64u * 64u, 0u);
            md.scripts.push_back({0x7fffffffu});
            md.locations[1] = px64(22, 22);
            md.locations[2] = px64(18, 38);
            md.hasInitialGroups = true;
            md.initialGroups[3].behavior = ss2::igroups::kAiSoldArt;
            md.initialGroups[3].zone1 = 1u;
            md.initialGroups[4].behavior = ss2::igroups::kAiSoldArt;
            md.initialGroups[4].aiFlags = ss2::igroups::kAifAtgNoUse;
            md.initialGroups[4].zone1 = 2u;

            const auto [s3aX, s3aY] = px64(20, 22);
            const auto [s3bX, s3bY] = px64(20, 24);
            const auto [howX, howY] = px64(23, 22);
            const auto [atgX, atgY] = px64(23, 24);
            const auto [s4X, s4Y] = px64(16, 38);
            const auto [atgNoUseX, atgNoUseY] = px64(18, 38);
            const auto [targetX, targetY] = px64(30, 22);

            std::vector<ss2::assets::Infantry> inf{
                {"dguner", s3aX, s3aY, 100, 40, 3, 1},
                {"dguner", s3bX, s3bY, 100, 40, 3, 1},
                {"dguner", s4X, s4Y, 100, 40, 4, 1},
                {"dguner", targetX, targetY, 100, 40, 0, 0}, // spotted enemy in range of crewed guns
            };
            ss2::assets::VehicleUnit neutralHow{};
            neutralHow.id = "lefh18";
            neutralHow.x = howX; neutralHow.y = howY;
            neutralHow.hp = 100; neutralHow.ammo = 100;
            neutralHow.owner = 3; neutralHow.group = 0;
            neutralHow.alive = true; neutralHow.hasCrew = false;

            ss2::assets::VehicleUnit decrewedAtg{};
            decrewedAtg.id = "pak40";
            decrewedAtg.x = atgX; decrewedAtg.y = atgY;
            decrewedAtg.hp = 100; decrewedAtg.ammo = 100;
            decrewedAtg.owner = 1; decrewedAtg.group = 3;
            decrewedAtg.alive = true; decrewedAtg.hasCrew = false;

            ss2::assets::VehicleUnit forbiddenAtg{};
            forbiddenAtg.id = "pak40";
            forbiddenAtg.x = atgNoUseX; forbiddenAtg.y = atgNoUseY;
            forbiddenAtg.hp = 100; forbiddenAtg.ammo = 100;
            forbiddenAtg.owner = 3; forbiddenAtg.group = 0;
            forbiddenAtg.alive = true; forbiddenAtg.hasCrew = false;

            ss2::TutorialUnits units(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                inf, {}, ss2::FogGrid(md.width, md.height, md.flags), 2, false,
                {neutralHow, decrewedAtg, forbiddenAtg}, specCat);
            ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));

            bool gunFired = false;
            for (unsigned t = 0u; t < 200u; ++t) {
                runner.advanceTicks(1u);
                for (const auto& vp : runner.pollVehiclePoses()) {
                    if ((vp.index == 0u || vp.index == 1u) && vp.fired) gunFired = true;
                }
            }
            const auto snap = runner.takeFullSnapshot();
            check(snap.units.vehicles[0].hasCrew && snap.units.vehicles[0].owner == 1 &&
                      snap.units.vehicles[0].group == 3,
                  "ai_sold_art did not crew neutral howitzer and adopt group 3");
            check(snap.units.vehicles[1].hasCrew && snap.units.vehicles[1].owner == 1,
                  "ai_sold_art did not re-crew friendly decrewed AT gun");
            check(!snap.units.vehicles[2].hasCrew && snap.units.vehicles[2].owner == 3,
                  "ai_sold_art violated aif_atgnouse by crewing forbidden AT gun");
            check(gunFired, "ai_sold_art crewed artillery/AT gun did not engage spotted enemy in range");
        }

        // 6c. ai_recon (1): scout advances to zone 1 holding fire to spot enemy, then retreats to zone 2 when threatened.
        {
            auto md = makeFlatMission();
            md.obstacles.assign(64u * 64u, 0u);
            md.scripts.push_back({0x7fffffffu});
            md.locations[1] = px64(30, 20); // recon target zone
            md.locations[2] = px64(14, 20); // safe retreat zone
            md.hasInitialGroups = true;
            md.initialGroups[5].behavior = ss2::igroups::kAiRecon;
            md.initialGroups[5].zone1 = 1u;
            md.initialGroups[5].zone2 = 2u;

            const auto [scStartX, scStartY] = px64(18, 20);
            const auto [enemyFarX, enemyFarY] = px64(40, 20);   // ~358px from zone 1: spotted (>256px) without close threat
            const auto [enemyCloseX, enemyCloseY] = px64(33, 20); // ~107px from zone 1: triggers close-range retreat

            std::vector<ss2::assets::Infantry> inf{
                {"dguner", enemyFarX, enemyFarY, 100, 40, 0, 0},
            };
            std::vector<ss2::assets::Placement> veh{
                {"sdkfz.222", scStartX, scStartY, 0, 100, 100, 5, 1},
            };
            ss2::TutorialUnits units(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, true, {}, specCat);
            units.setGroupHoldFire(0u, true, false); // keep player target passive during spotting phase
            ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));

            bool scoutEverFired = false;
            for (unsigned t = 0u; t < 160u; ++t) {
                runner.advanceTicks(1u);
                for (const auto& vp : runner.pollVehiclePoses()) {
                    if (vp.index == 0u && vp.fired) scoutEverFired = true;
                }
            }
            const auto atReconSnap = runner.takeFullSnapshot();
            check(!scoutEverFired && atReconSnap.units.vehicles[0].holdFire,
                  "ai_recon scout failed to hold fire while advancing to zone 1");
            check(atReconSnap.units.vehicles[0].x > scStartX + 320,
                  "ai_recon scout did not advance into target zone 1");

            // Enemy closes to <256px of scout -> scout must retreat westward toward safe zone 2 without firing.
            const int peakX = atReconSnap.units.vehicles[0].x;
            (void)runner.move(std::array<std::size_t, 1>{0u}, enemyCloseX, enemyCloseY);
            for (unsigned t = 0u; t < 240u; ++t) {
                runner.advanceTicks(1u);
                for (const auto& vp : runner.pollVehiclePoses()) {
                    if (vp.index == 0u && vp.fired) scoutEverFired = true;
                }
            }
            const auto retreatedSnap = runner.takeFullSnapshot();
            check(!scoutEverFired, "ai_recon scout fired instead of retreating");
            check(retreatedSnap.units.vehicles[0].x < peakX - 180,
                  "ai_recon scout did not retreat toward safe zone 2 when enemy entered close range");
        }

        // 6d. ai_furg_transport (13): APC shuttles infantry both from zone1->zone2 and by targetGroup->zone3.
        {
            auto md = makeFlatMission();
            md.obstacles.assign(64u * 64u, 0u);
            md.scripts.push_back({0x7fffffffu});
            md.locations[1] = px64(20, 18); // pickup zone 1
            md.locations[2] = px64(38, 18); // dropoff zone 2
            md.locations[3] = px64(38, 32); // dropoff zone 3 for targetGroup shuttle
            md.hasInitialGroups = true;
            // Group 6 APC: zone1 pickup -> zone2 unload.
            md.initialGroups[6].behavior = ss2::igroups::kAiFurgTransport;
            md.initialGroups[6].zone1 = 1u;
            md.initialGroups[6].zone2 = 2u;
            // Group 7 APC: target group 8 pickup -> zone3 unload.
            md.initialGroups[7].behavior = ss2::igroups::kAiFurgTransport;
            md.initialGroups[7].group1 = 8u;
            md.initialGroups[7].zone2 = 3u;

            const auto [apc1X, apc1Y] = px64(14, 18);
            const auto [z1InfA_X, z1InfA_Y] = px64(20, 18);
            const auto [z1InfB_X, z1InfB_Y] = px64(21, 18);
            const auto [apc2X, apc2Y] = px64(14, 32);
            const auto [g8InfX, g8InfY] = px64(20, 32);

            std::vector<ss2::assets::Infantry> inf{
                {"dguner", z1InfA_X, z1InfA_Y, 100, 40, 11, 1},
                {"dguner", z1InfB_X, z1InfB_Y, 100, 40, 11, 1},
                {"dguner", g8InfX, g8InfY, 100, 40, 8, 1},
            };
            std::vector<ss2::assets::Placement> veh{
                {"sdkfz.251", apc1X, apc1Y, 0, 100, 100, 6, 1},
                {"sdkfz.251", apc2X, apc2Y, 0, 100, 100, 7, 1},
            };
            ss2::TutorialUnits units(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, specCat);
            ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));

            bool sawEmbarked1 = false;
            bool sawEmbarked2 = false;
            for (unsigned t = 0u; t < 420u; ++t) {
                runner.advanceTicks(1u);
                for (const auto& vp : runner.pollVehiclePoses()) {
                    if (vp.index == 0u && vp.passengerCount > 0) sawEmbarked1 = true;
                    if (vp.index == 1u && vp.passengerCount > 0) sawEmbarked2 = true;
                }
            }
            const auto snap = runner.takeFullSnapshot();
            check(sawEmbarked1 && snap.units.vehicles[0].passengers.empty(),
                  "ai_furg_transport APC 0 did not board and unload zone1 infantry");
            check(!snap.units.soldiers[0].embarked && !snap.units.soldiers[1].embarked &&
                      snap.units.soldiers[0].cellX / 2 >= 34 && snap.units.soldiers[1].cellX / 2 >= 34,
                  "ai_furg_transport APC 0 did not deliver infantry to destination zone 2");
            check(sawEmbarked2 && snap.units.vehicles[1].passengers.empty() &&
                      !snap.units.soldiers[2].embarked && snap.units.soldiers[2].cellX / 2 >= 34,
                  "ai_furg_transport APC 1 did not shuttle targetGroup=8 infantry to zone 3");
        }

        // 6e. ai_katya_move (10): rocket launcher advances to firing zone 1, avoids friendly-fire cluster,
        //     fires salvo at spotted enemy cluster, scoots to cover zone 2 while reloading, then returns.
        {
            auto md = makeFlatMission();
            md.obstacles.assign(64u * 64u, 0u);
            md.scripts.push_back({0x7fffffffu});
            md.locations[1] = px64(26, 20); // firing zone 1
            md.locations[2] = px64(14, 20); // cover/reload zone 2
            md.hasInitialGroups = true;
            md.initialGroups[9].behavior = ss2::igroups::kAiKatyaMove;
            md.initialGroups[9].zone1 = 1u;
            md.initialGroups[9].zone2 = 2u;

            const auto [katStartX, katStartY] = px64(14, 20);
            const auto [spotterX, spotterY] = px64(31, 20);
            const auto [mixedEnemyX, mixedEnemyY] = px64(31, 21); // close to friendly spotter -> -2 penalty!
            const auto [clearEnemyX, clearEnemyY] = px64(35, 20); // clear enemy cluster in range -> +2 score!

            std::vector<ss2::assets::Infantry> inf{
                {"dguner", spotterX, spotterY, 500, 40, 20, 1},       // friendly spotter near mixedEnemy
                {"dguner", mixedEnemyX, mixedEnemyY, 500, 40, 0, 0},  // enemy hugging friendly spotter
                {"dguner", clearEnemyX, clearEnemyY, 500, 40, 0, 0},  // clear enemy 1
                {"dguner", clearEnemyX + 16, clearEnemyY, 500, 40, 0, 0}, // clear enemy 2
            };
            std::vector<ss2::assets::Placement> veh{
                {"bm-13", katStartX, katStartY, 0, 100, 100, 9, 1},
            };
            ss2::TutorialUnits units(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, specCat);
            units.setGroupHoldFire(20u, true, false);
            units.setGroupHoldFire(0u, true, false);
            ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));

            int rocketsFired = 0;
            int lastTargetX = 0;
            int firingX = 0;
            bool scootedBack = false;
            bool returnedAfterReload = false;
            int minReloadX = 999999;

            for (unsigned t = 0u; t < 480u; ++t) {
                runner.advanceTicks(1u);
                for (const auto& vp : runner.pollVehiclePoses()) {
                    if (vp.index != 0u) continue;
                    if (vp.fired && vp.firedRocket) {
                        ++rocketsFired;
                        lastTargetX = vp.targetX;
                        firingX = std::max(firingX, vp.x);
                    }
                    if (rocketsFired >= 4 && vp.x < firingX - 200) {
                        scootedBack = true;
                        minReloadX = std::min(minReloadX, vp.x);
                    }
                    if (scootedBack && vp.x > minReloadX + 180) {
                        returnedAfterReload = true;
                    }
                }
            }
            check(rocketsFired >= 4, "ai_katya_move did not fire full rocket salvo from firing zone 1");
            check(lastTargetX > mixedEnemyX + 120,
                  "ai_katya_move failed friendly-fire cluster avoidance (-2 allied weight)");
            check(scootedBack,
                  "ai_katya_move did not scoot back to reload/cover zone 2 during burstReload cooldown");
            check(returnedAfterReload,
                  "ai_katya_move did not return toward firing zone 1 after reload completed");
        }

        // 6f. ai_train_move (12): armored train patrols along 1-tile rail track between off-track zone centroids
        //     with twin-runner determinism parity.
        {
            auto makeTrainMission = [&]() {
                auto md = makeFlatMission();
                md.scripts.push_back({0x7fffffffu});
                // Block ground nibble (0x000F) everywhere EXCEPT the straight railroad line at tileY == 24!
                md.obstacles.assign(64u * 64u, 0x000Fu);
                for (unsigned tx = 8u; tx <= 52u; ++tx) {
                    md.obstacles[24u * 64u + tx] = 0x0000u;
                }
                for (unsigned ty = 22u; ty <= 28u; ++ty) {
                    for (unsigned tx = 14u; tx <= 18u; ++tx) md.zones[ty * 64u + tx] = 1u;
                    for (unsigned tx = 36u; tx <= 40u; ++tx) md.zones[ty * 64u + tx] = 2u;
                }
                // Place zone centroids 3 tiles OFF the track (tileY=27, where obstacles=0x000F blocks trains!).
                md.locations[1] = px64(16, 27);
                md.locations[2] = px64(38, 27);
                md.hasInitialGroups = true;
                md.initialGroups[10].behavior = ss2::igroups::kAiTrainMove;
                md.initialGroups[10].zone1 = 1u;
                md.initialGroups[10].zone2 = 2u;

                const auto [trX, trY] = px64(26, 24);
                std::vector<ss2::assets::Placement> veh{
                    {"armored_train", trX, trY, 0, 100, 100, 10, 1},
                };
                ss2::TutorialUnits u(
                    ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                    {}, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, specCat);
                return std::pair{std::move(md), std::move(u)};
            };

            auto [mdA, uA] = makeTrainMission();
            auto [mdB, uB] = makeTrainMission();
            ss2::MissionRunner trainA(ss2::MissionRunner::ManualClock{}, std::move(mdA), std::move(uA));
            ss2::MissionRunner trainB(ss2::MissionRunner::ManualClock{}, std::move(mdB), std::move(uB));

            bool reachedZ1 = false;
            bool reachedZ2 = false;
            const int z1RailX = px64(16, 24).first;
            const int z2RailX = px64(38, 24).first;
            for (unsigned step = 0u; step < 16u; ++step) {
                trainA.advanceTicks(45u);
                trainB.advanceTicks(45u);
                check(trainA.stateHash() == trainB.stateHash(),
                      "ai_train_move twin runner stateHash diverged");
                const auto snap = trainA.takeFullSnapshot();
                const auto& tv = snap.units.vehicles[0];
                const int trainTileY = ss2::tutorialFootWorld(tv.x, tv.y, 64 * 32).second / 32;
                check(trainTileY == 24, "ai_train_move left railroad track tileY == 24");
                if (std::abs(tv.x - z1RailX) <= 96) reachedZ1 = true;
                if (std::abs(tv.x - z2RailX) <= 96) reachedZ2 = true;
            }
            check(reachedZ1 && reachedZ2,
                  "ai_train_move failed to snap off-rail zone centroids and patrol between zone 1 and zone 2");

            // Multi-car consist test (3 coupled railcars in group 10): unified patrol direction without head-on split.
            auto makeConsistMission = [&]() {
                auto md = makeFlatMission();
                md.scripts.push_back({0x7fffffffu});
                md.obstacles.assign(64u * 64u, 0x000Fu);
                for (unsigned tx = 8u; tx <= 52u; ++tx) {
                    md.obstacles[24u * 64u + tx] = 0x0000u;
                }
                for (unsigned ty = 23u; ty <= 25u; ++ty) {
                    for (unsigned tx = 14u; tx <= 18u; ++tx) md.zones[ty * 64u + tx] = 1u;
                    for (unsigned tx = 36u; tx <= 40u; ++tx) md.zones[ty * 64u + tx] = 2u;
                }
                md.locations[1] = px64(16, 24);
                md.locations[2] = px64(38, 24);
                md.hasInitialGroups = true;
                md.initialGroups[10].behavior = ss2::igroups::kAiTrainMove;
                md.initialGroups[10].zone1 = 1u;
                md.initialGroups[10].zone2 = 2u;

                const auto [c0x, c0y] = px64(25, 24);
                const auto [c1x, c1y] = px64(27, 24);
                const auto [c2x, c2y] = px64(29, 24);
                std::vector<ss2::assets::Placement> veh{
                    {"armored_train", c0x, c0y, 0, 100, 100, 10, 1},
                    {"armored_train", c1x, c1y, 0, 100, 100, 10, 1},
                    {"armored_train", c2x, c2y, 0, 100, 100, 10, 1},
                };
                ss2::TutorialUnits u(
                    ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                    {}, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, specCat);
                return std::pair{std::move(md), std::move(u)};
            };

            auto [cmdA, cuA] = makeConsistMission();
            auto [cmdB, cuB] = makeConsistMission();
            ss2::MissionRunner conA(ss2::MissionRunner::ManualClock{}, std::move(cmdA), std::move(cuA));
            ss2::MissionRunner conB(ss2::MissionRunner::ManualClock{}, std::move(cmdB), std::move(cuB));
            bool consistReachedZ1 = false;
            bool consistReachedZ2 = false;
            for (unsigned step = 0u; step < 20u; ++step) {
                conA.advanceTicks(45u);
                conB.advanceTicks(45u);
                check(conA.stateHash() == conB.stateHash(),
                      "Multi-car ai_train_move twin runner stateHash diverged");
                const auto snap = conA.takeFullSnapshot();
                int avgX = 0;
                for (std::size_t k = 0; k < 3u; ++k) {
                    const auto& cv = snap.units.vehicles[k];
                    const int ty = ss2::tutorialFootWorld(cv.x, cv.y, 64 * 32).second / 32;
                    check(ty == 24, "Multi-car train car left railroad track tileY == 24");
                    avgX += cv.x;
                }
                avgX /= 3;
                // Cars must remain spaced along the track rather than collapsing onto identical coordinates.
                const int span01 = std::abs(snap.units.vehicles[0].x - snap.units.vehicles[1].x);
                const int span12 = std::abs(snap.units.vehicles[1].x - snap.units.vehicles[2].x);
                check(span01 >= 16 || span12 >= 16,
                      "Multi-car train consist collapsed all 3 cars onto identical coordinates");
                if (std::abs(avgX - z1RailX) <= 128) consistReachedZ1 = true;
                if (std::abs(avgX - z2RailX) <= 128) consistReachedZ2 = true;
            }
            check(consistReachedZ1 && consistReachedZ2,
                  "Multi-car ai_train_move consist failed to patrol coherently between zone 1 and zone 2");
        }

        // 7. ai_plane_move (11, n2Game_Dll.dll 0x10053c50): >= 70% HP gate, aif_zonenoenemy halt, and airfield parking.
        {
            ss2::assets::UnitSpec parkedPlane{};
            parkedPlane.id = "yak_ground";
            parkedPlane.isVehicle = true;
            parkedPlane.health = 200;
            parkedPlane.moveSpeed = 1.8;
            parkedPlane.sight = 14;
            const auto planeCat = ss2::assets::UnitCatalog::fromSpecs({parkedPlane});

            auto md = makeFlatMission();
            md.obstacles.assign(64u * 64u, 0u);
            for (unsigned ty = 28u; ty <= 32u; ++ty) {
                for (unsigned tx = 36u; tx <= 40u; ++tx) md.zones[ty * 64u + tx] = 1u; // Airfield Zone 1
            }
            md.locations[1] = px64(38, 30);
            md.scripts.push_back({0x7fffffffu});
            md.hasInitialGroups = true;
            md.initialGroups[11].behavior = ss2::igroups::kAiPlaneMove;
            md.initialGroups[11].aiFlags = ss2::igroups::kAifZoneNoEnemy;
            md.initialGroups[11].zone1 = 1u;

            const auto [p0x, p0y] = px64(14, 30);
            const auto [p1x, p1y] = px64(14, 32);
            const auto [enX, enY] = px64(38, 30); // Hostile soldier blocking Airfield Zone 1 initially
            std::vector<ss2::assets::Placement> veh{
                {"yak_ground", p0x, p0y, 0, 100, 0, 11, 1}, // 100% HP (>= 70% threshold)
                {"yak_ground", p1x, p1y, 0, 50, 0, 11, 1},  // 50% HP (< 70% threshold -> stays grounded)
            };
            std::vector<ss2::assets::Infantry> inf{
                {"dguner", enX, enY, 100, 0, 1, 0},
            };
            ss2::TutorialUnits u(
                ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, planeCat);
            ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(u));

            // While enemy occupies Airfield Zone 1, neither plane may relocate.
            runner.advanceTicks(80u);
            auto snapBlocked = runner.takeFullSnapshot();
            check(std::abs(snapBlocked.units.vehicles[0].x - p0x) <= 16 &&
                      std::abs(snapBlocked.units.vehicles[1].x - p1x) <= 16,
                  "ai_plane_move relocated into enemy-occupied airfield despite aif_zonenoenemy");

            // Clear enemy from Airfield Zone 1 -> intact plane [0] relocates and parks; damaged plane [1] (50% HP) stays grounded.
            const auto [awayX, awayY] = px64(10, 10);
            (void)runner.move(std::array<std::size_t, 1>{0}, awayX, awayY);
            runner.advanceTicks(360u);
            auto snapArrived = runner.takeFullSnapshot();
            const auto [z1x, z1y] = px64(38, 30);
            check(std::hypot(snapArrived.units.vehicles[0].x - z1x, snapArrived.units.vehicles[0].y - z1y) <= 160.0,
                  "ai_plane_move intact plane failed to relocate to cleared Airfield Zone 1");
            check(std::abs(snapArrived.units.vehicles[1].x - p1x) <= 16 &&
                      std::abs(snapArrived.units.vehicles[1].y - p1y) <= 16,
                  "ai_plane_move relocated damaged plane (< 70% maxHp) contrary to 0x10053d1c");
        }

        // 8. Retail CPara::Step (0x1002e1a0..0x1002e3b6) parachute descent, mid-air SS2FULL4 save/reload,
        //    persistent neutral ammo.box supply crates, sympathetic cook-off, & deep-water splashdown drowning.
        {
            ss2::assets::PlaneSpec ju52{};
            ju52.id = "ju-52";
            ju52.health = 1100;
            ju52.flySpeed = 3.81;
            ju52.dropFlySpeed = 2.35;
            ju52.altitude = 120;
            ss2::assets::UnitSpec crateSpec{};
            crateSpec.id = "ammo.box";
            crateSpec.isVehicle = true;
            crateSpec.health = 100;
            crateSpec.ammo1 = 1000;
            crateSpec.selType = "crate";
            crateSpec.dieExplosion = "crate";
            ss2::assets::UnitSpec tankSpec{};
            tankSpec.id = "pz4";
            tankSpec.isVehicle = true;
            tankSpec.health = 400;
            tankSpec.ammo1 = 80;
            tankSpec.moveSpeed = 1.4;
            tankSpec.shot1.damage = 140;
            tankSpec.shot1.range = 420;
            tankSpec.shot1.reloadTicks = 20;
            tankSpec.shot1.hitType = ss2::assets::HitType::Explosive;
            const auto dropCat = ss2::assets::UnitCatalog::fromSpecs({crateSpec, tankSpec}, {ju52});

            auto makeDropMission = [&]() {
                auto md = makeFlatMission();
                md.obstacles.assign(64u * 64u, 0u);
                // Carve an unbridged deep-water pond around tile (44..52, 18..26): deep water nibble 0x00F0 set, ground nibble 0x000F clear.
                for (unsigned ty = 18u; ty <= 26u; ++ty) {
                    for (unsigned tx = 44u; tx <= 52u; ++tx) {
                        md.obstacles[ty * 64u + tx] = 0x00F0u;
                    }
                }
                md.scripts.push_back({0x7fffffffu});
                const auto [tkX, tkY] = px64(25, 22); // ~96px from (22, 22): inside 140px supply radius and clean firing stand-off
                const auto [solX, solY] = px64(24, 22);
                std::vector<ss2::assets::Infantry> inf{
                    {"dguner", solX, solY, 100, 5, 1, 0}, // low-ammo friendly soldier near supply drop zone
                };
                std::vector<ss2::assets::Placement> veh{
                    {"pz4", tkX, tkY, 0, 60, 10, 1, 0},   // damaged & low-ammo friendly tank near supply drop zone
                };
                ss2::TutorialUnits u(
                    ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
                    inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, dropCat);
                std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air{};
                air[0][static_cast<std::size_t>(ss2::assets::AirMissionKind::Paratrooper)] = {"ju-52", 2, 2};
                air[0][static_cast<std::size_t>(ss2::assets::AirMissionKind::Supply)] = {"ju-52", 1, 1};
                std::array<int, 4> pGrp{14, 0, 0, 0};
                std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos{};
                pRos[0] = {{"dsmguner", 4}};
                u.setAirSupport(air, pGrp, pRos);
                return std::pair{std::move(md), std::move(u)};
            };

            auto [md1, u1] = makeDropMission();
            auto [md2, u2] = makeDropMission();
            ss2::MissionRunner runA(ss2::MissionRunner::ManualClock{}, std::move(md1), std::move(u1));
            ss2::MissionRunner runB(ss2::MissionRunner::ManualClock{}, std::move(md2), std::move(u2));

            const auto [paraLandX, paraLandY] = px64(18, 22);
            const auto [supLandX, supLandY] = px64(22, 22);
            const auto [waterX, waterY] = px64(48, 22);
            check(runA.callAirSupport(ss2::assets::AirMissionKind::Paratrooper, paraLandX, paraLandY),
                  "Paratrooper sortie call failed");
            check(runA.callAirSupport(ss2::assets::AirMissionKind::Supply, supLandX, supLandY),
                  "Supply sortie call failed");
            check(runA.callAirSupport(ss2::assets::AirMissionKind::Paratrooper, waterX, waterY),
                  "Water Paratrooper sortie call failed");

            // Step until canopies are actively descending mid-air (before touchdown!).
            bool sawAirborneCanopy = false;
            for (unsigned t = 0u; t < 320u; ++t) {
                runA.advanceTicks(1u);
                for (const auto& pp : runA.pollParachutes()) {
                    if (pp.active && pp.altitudePx > 25) sawAirborneCanopy = true;
                }
                if (sawAirborneCanopy && !runA.takeFullSnapshot().unitsExtra.parachutes.empty()) break;
            }
            const auto midSnap = runA.takeFullSnapshot();
            check(sawAirborneCanopy && !midSnap.unitsExtra.parachutes.empty(),
                  "Paratrooper/Supply sorties must spawn airborne descending parachutes");
            check(midSnap.units.soldiers.size() == 1u,
                  "Airborne paratroopers must not materialize on ground before canopy touchdown");

            // Mid-descent SS2FULL4 serialize -> deserialize -> loadFullSnapshot onto runB, then verify bit-identical landing!
            const auto midBytes = ss2::MissionRunner::serializeFullSnapshot(midSnap);
            const auto midDecoded = ss2::MissionRunner::deserializeFullSnapshot(midBytes);
            check(midDecoded.has_value() && runB.loadFullSnapshot(*midDecoded),
                  "Mid-descent SS2FULL4 snapshot failed to round-trip into twin runner");
            check(runA.stateHash() == runB.stateHash(),
                  "Mid-descent SS2FULL4 reload stateHash diverged immediately");

            bool sawBoxDust = false;
            unsigned paraLandedEvents = 0u;
            unsigned crateLandedEvents = 0u;
            for (unsigned t = 0u; t < 320u; ++t) {
                runA.advanceTicks(1u);
                runB.advanceTicks(1u);
                for (const auto& ae : runA.pollAirStrikeEvents()) {
                    if (ae.kind == ss2::AirStrikeEvent::BoxDust) sawBoxDust = true;
                    if (ae.kind == ss2::AirStrikeEvent::ParatrooperLanded) ++paraLandedEvents;
                    if (ae.kind == ss2::AirStrikeEvent::SupplyCrateLanded) ++crateLandedEvents;
                }
                (void)runB.pollAirStrikeEvents();
            }
            check(runA.stateHash() == runB.stateHash(),
                  "Mid-descent SS2FULL4 twin runner stateHash diverged after canopy touchdown");
            check(sawBoxDust && paraLandedEvents == 4u && crateLandedEvents == 3u,
                  "Expected BoxDust + 4 dry-land paratroopers + 3 ammo.box crates (while 4 water jumpers drowned)");

            const auto landedSnap = runA.takeFullSnapshot();
            check(landedSnap.units.soldiers.size() == 5u,
                  "Exactly 4 dry-land paratroopers must join initial soldier (water jumpers must drown)");
            check(landedSnap.units.vehicles.size() == 4u,
                  "Supply sortie must materialize 3 persistent neutral ammo.box crates alongside initial tank");
            // Nearby low-ammo tank & soldier must have resupplied/repaired from the neutral (owner==3) ammo.box crates!
            check(landedSnap.units.vehicles[0].ammo > 10 && landedSnap.units.vehicles[0].hp > 240,
                  "Friendly tank failed to resupply/repair from airdropped neutral ammo.box crates");
            check(landedSnap.units.soldiers[0].ammo > 5,
                  "Friendly soldier failed to resupply from airdropped neutral ammo.box crates");

            // Sympathetic crate detonation: shell crate #1 -> destroys crate #1 & #2 and chain-splashes crate #3 (>48px away)!
            const int c1x = landedSnap.units.vehicles[1].x;
            const int c1y = landedSnap.units.vehicles[1].y;
            (void)runA.attackGroundVehicles(std::array<std::size_t, 1>{0u}, c1x, c1y);
            runA.advanceTicks(80u);
            const auto blownSnap = runA.takeFullSnapshot();
            unsigned deadCrates = 0u;
            for (std::size_t vi = 1u; vi < blownSnap.units.vehicles.size(); ++vi) {
                if (!blownSnap.units.vehicles[vi].alive || blownSnap.units.vehicles[vi].hp == 0) ++deadCrates;
            }
            check(deadCrates >= 2u && blownSnap.units.vehicles[3].hp < 100,
                  "Shelling airdropped ammo.box crate failed to trigger sympathetic cook-off chain across crate stick");
        }
    }

    std::cout << "CAOF complete 17-opcode + air-route + pool-carryover + bridges + specialized I-GROUPS + parachute/crates checks PASSED\n";
}

void runRetailCaofCorpusCheck(const std::filesystem::path& gameDir) {
    static constexpr std::array<std::uint32_t, 76> kImplementedOpcodes{{
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u,
        11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u,
        21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u,
        31u, 33u,
        39u, 40u, 41u, 42u, 43u, 44u, 45u, 46u,
        47u, 48u, 49u, 50u, 51u, 52u, 53u, 54u, 55u, 56u, 57u, 58u, 59u, 60u,
        62u, 63u, 71u, 72u, 75u, 76u, 77u, 78u, 79u, 80u,
        82u, 83u, 84u, 85u, 86u, 87u, 88u, 89u, 90u, 91u, 92u, 93u,
        0x7fffffffu, 0x7fffffffu
    }};

    std::vector<std::filesystem::path> missionFiles;
    const auto mapsDir = gameDir / "maps";
    if (!std::filesystem::is_directory(mapsDir)) {
        throw std::runtime_error("Missing retail maps directory for CAOF corpus check");
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(mapsDir)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = lowerAscii(entry.path().extension().string());
        if ((ext == ".ssc" && entry.path().stem().string().size() >= 6u) ||
            ext == ".ssm" || ext == ".smm") {
            missionFiles.push_back(entry.path());
        }
    }
    std::ranges::sort(missionFiles);
    check(missionFiles.size() == 136u, "Expected 136 retail missions in maps/");

    std::set<std::uint32_t> unknownOps;
    std::size_t totalScripts = 0;
    std::size_t evaluatedMissions = 0;
    for (const auto& mp : missionFiles) {
        const auto md = ss2::assets::readMissionData(mp);
        for (const auto& sc : md.scripts) {
            ++totalScripts;
            for (const std::uint32_t word : sc) {
                if ((word & 0x80000000u) != 0u || word == 0x7fffffffu) continue;
                if (std::ranges::find(kImplementedOpcodes, word) == kImplementedOpcodes.end()) {
                    unknownOps.insert(word);
                }
            }
        }
        if (lowerAscii(mp.extension().string()) != ".smm") {
            check(md.hasInitialGroups, "Expected SP/campaign mission to carry 2800-byte I-GROUPS");
        }
        if (!md.scripts.empty()) {
            ss2::MissionQueries zeroQ{};
            try {
                ss2::Tutorial vm(md, zeroQ);
                for (unsigned t = 0; t < 64u; ++t) vm.step(zeroQ);
                (void)vm.takeEvents();
            } catch (const std::exception& ex) {
                throw std::runtime_error("Retail CAOF VM eval failed in " + mp.filename().string() + ": " + ex.what());
            }
            ++evaluatedMissions;
        }
    }
    if (!unknownOps.empty()) {
        throw std::runtime_error("Retail CAOF corpus contains unhandled opcode OP(" +
                                 std::to_string(*unknownOps.begin()) + ")");
    }
    check(evaluatedMissions == 136u, "Expected 136 retail VM evaluations");

    // Verify exact retail I-GROUPS values on Tutorial I, campaign/001000.ssc, and campaign/010007.ssc.
    {
        const auto tutMd = ss2::assets::readMissionData(gameDir / "maps/single/--- TUTORIAL I ---.SSM");
        check(tutMd.hasInitialGroups &&
                  tutMd.initialGroups[13].behavior == 4u &&
                  tutMd.initialGroups[13].aiFlags == 1u &&
                  tutMd.initialGroups[13].zone1 == 0u,
              "Expected Tutorial I group 13 I-GROUPS behavior=4, aiFlags=1");

        const auto c001Md = ss2::assets::readMissionData(gameDir / "maps/campaign/001000.ssc");
        check(c001Md.hasInitialGroups &&
                  c001Md.initialGroups[1].behavior == 12u &&
                  c001Md.initialGroups[1].aiFlags == 1u &&
                  c001Md.initialGroups[1].zone1 == 1u &&
                  c001Md.initialGroups[1].zone2 == 1u,
              "Expected campaign/001000.ssc group 1 I-GROUPS behavior=12, aiFlags=1, zone1=zone2=1");

        const auto c010Md = ss2::assets::readMissionData(gameDir / "maps/campaign/010007.ssc");
        check(c010Md.hasInitialGroups &&
                  c010Md.initialGroups[70].reservMinDelayTicks == 0u &&
                  c010Md.initialGroups[70].reservArrivalDelayTicks == 22500u &&
                  c010Md.initialGroups[70].reservMinUnits == 20u &&
                  c010Md.initialGroups[70].reservExpaPct == 30u,
              "Expected campaign/010007.ssc group 70 I-GROUPS reserve configuration");

        // Retail specialized I-GROUPS checks: ai_katya_move (10), ai_train_move (12), ai_furg_transport (13),
        // and conditional zone flags aif_zonehaveown (0x2) / aif_zonenoenemy (0x4).
        const auto fleeMd = ss2::assets::readMissionData(gameDir / "maps/single/FLEE TO THE FRONT.SSM");
        bool sawFleeKatya = false;
        bool sawFleeTrain = false;
        for (const auto& ig : fleeMd.initialGroups) {
            if (ig.behavior == ss2::igroups::kAiKatyaMove) sawFleeKatya = true;
            if (ig.behavior == ss2::igroups::kAiTrainMove) sawFleeTrain = true;
        }
        check(sawFleeKatya && sawFleeTrain,
              "Expected FLEE TO THE FRONT.SSM to contain ai_katya_move (10) and ai_train_move (12)");

        const auto c010004Md = ss2::assets::readMissionData(gameDir / "maps/campaign/010004.ssc");
        bool sawTransport = false;
        bool sawZoneCond = false;
        for (const auto& ig : c010004Md.initialGroups) {
            if (ig.behavior == ss2::igroups::kAiFurgTransport) sawTransport = true;
            if ((ig.aiFlags & (ss2::igroups::kAifZoneHaveOwn | ss2::igroups::kAifZoneNoEnemy)) != 0u) {
                sawZoneCond = true;
            }
        }
        check(sawTransport && sawZoneCond,
              "Expected campaign/010004.ssc to contain ai_furg_transport (13) and conditional zone flags");
    }

    // Smoke-step representative retail missions exercising OP(8/9/31/33/39/46/62/63/71/72/75/83/84/85/86/90/91)
    // and specialized I-GROUPS behaviors (ai_sold_art, ai_katya_move, ai_train_move, ai_furg_transport).
    const auto catalog = ss2::assets::UnitCatalog::load(gameDir);
    const std::array<const char*, 8> repRelPaths{{
        "maps/campaign/001000.ssc",
        "maps/campaign/006000.ssc",
        "maps/campaign/010004.ssc",
        "maps/campaign/024001.ssc",
        "maps/single/'Dieppe [Total Victory].ssm",
        "maps/single/'Crete [Total Victory].ssm",
        "maps/single/ALONE AGAINST THE WORLD.SSM",
        "maps/single/FLEE TO THE FRONT.SSM",
    }};
    for (const char* rel : repRelPaths) {
        const auto full = gameDir / rel;
        check(std::filesystem::is_regular_file(full), rel);
        auto md = ss2::assets::readMissionData(full);
        const auto placed = ss2::assets::readMissionPlacements(full);
        std::vector<ss2::assets::Infantry> inf;
        std::vector<ss2::assets::Placement> veh;
        for (const auto& p : placed) {
            const auto* spec = catalog ? catalog->find(p.id) : nullptr;
            if (spec && !spec->isVehicle) {
                inf.push_back({
                    p.id, p.x, p.y, p.hp, p.ammo, p.group, p.owner,
                    0, 0, 0, 0,
                    static_cast<unsigned>(p.direction), 0u, false, true, p.expa
                });
            } else {
                veh.push_back(p);
            }
        }
        ss2::TutorialUnits units(
            ss2::InfantryPathGrid::fromSubCellMasks(md.width, md.height, md.flags, md.obstacles),
            inf, veh, ss2::FogGrid(md.width, md.height, md.flags), 2, false, {}, catalog);
        units.setAirSupport(md.airSupport, md.paratrooperGroup, md.paratrooperRoster);
        ss2::MissionRunner runner(ss2::MissionRunner::ManualClock{}, std::move(md), std::move(units));
        runner.advanceTicks(64u);
        (void)runner.poll();
    }
    // Verify retail MOST.RES extraction on Tutorial I (4 bridges) and mission 006000.ssc (>= 1 bridge).
    {
        auto tutMap = ss2::assets::loadTutorialTerrain(gameDir);
        std::vector<ss2::assets::BuildingPlacement> tutBlds;
        std::vector<ss2::assets::BridgePlacement> tutBridges;
        (void)ss2::assets::drawTutorialScenery(tutMap, gameDir, nullptr, 0, &tutBlds, &tutBridges);
        check(tutBridges.size() == 4u, "Expected 4 MOST.RES bridges on Tutorial I");
        for (const auto& br : tutBridges) {
            check(br.maxHp == 3500 && br.basePatch.width > 0u && br.basePatch.height > 0u && !br.cells.empty(),
                  "Expected valid Tutorial I BridgePlacement dimensions and 38x38 cells");
            for (int st = 0; st < 5; ++st) {
                check(!br.stageFrames[static_cast<std::size_t>(st)].empty(),
                      "Expected non-empty MOST.RES sprite frame for every bridge damage stage 0..4");
            }
        }
        const auto m006 = gameDir / "maps/campaign/006000.ssc";
        auto m006Map = ss2::assets::loadMissionTerrain(gameDir, m006);
        std::vector<ss2::assets::BuildingPlacement> m006Blds;
        std::vector<ss2::assets::BridgePlacement> m006Bridges;
        (void)ss2::assets::drawMissionScenery(m006Map, gameDir, m006, nullptr, &m006Blds, &m006Bridges);
        check(!m006Bridges.empty(), "Expected retail campaign 006000.ssc to contain at least 1 bridge");
    }
    std::cout << "Retail CAOF 136-mission corpus check (" << missionFiles.size() << " files, "
              << totalScripts << " triggers, 0 unhandled opcodes, " << evaluatedMissions
              << " VM evals, 6 smoke sims, retail MOST.RES bridges) PASSED\n";
}

}  // namespace ss2::vm_test
