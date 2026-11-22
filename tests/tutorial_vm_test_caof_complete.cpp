#include "tutorial_vm_test_internal.hpp"
#include "assets/placement.hpp"
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

    std::cout << "CAOF complete 17-opcode + air-route + pool-carryover checks PASSED\n";
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

    // Smoke-step representative retail missions exercising OP(8/9/31/33/39/46/62/63/71/72/75/83/84/85/86/90/91).
    const auto catalog = ss2::assets::UnitCatalog::load(gameDir);
    const std::array<const char*, 6> repRelPaths{{
        "maps/campaign/001000.ssc",
        "maps/campaign/006000.ssc",
        "maps/campaign/024001.ssc",
        "maps/single/'Dieppe [Total Victory].ssm",
        "maps/single/'Crete [Total Victory].ssm",
        "maps/single/ALONE AGAINST THE WORLD.SSM",
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
    std::cout << "Retail CAOF 136-mission corpus check (" << missionFiles.size() << " files, "
              << totalScripts << " triggers, 0 unhandled opcodes, " << evaluatedMissions
              << " VM evals, 6 smoke sims) PASSED\n";
}

}  // namespace ss2::vm_test
