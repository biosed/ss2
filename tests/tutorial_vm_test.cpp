#include "sim/tutorial.hpp"
#include "sim/mission_runner.hpp"
#include "sim/tutorial_units.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>
#include <thread>

int main(int argc, char** argv) try {
    using Event = ss2::MissionEvent;
    if (argc == 2) {
        const auto file = std::filesystem::path(argv[1]) / "maps/single/--- TUTORIAL I ---.SSM";
        ss2::Tutorial vm(ss2::assets::readTutorialData(file), 0);
        std::vector<std::pair<unsigned, unsigned>> observed;
        for (unsigned i = 0; i <= 835; ++i) {
            if (i) vm.step(i < 579 ? 0 : i < 643 ? 1 : 2);
            for (auto event : vm.takeEvents())
                if (event.kind == Event::Dialog || event.kind == Event::Message) observed.emplace_back(i, event.index);
        }
        using P = std::pair<unsigned, unsigned>;
        const std::vector<P> expected{{0,19},{67,0},{131,28},{195,1},{259,2},{387,20},{451,3},{515,29},{643,4},{835,5}};
        if (observed != expected) throw std::runtime_error("Native mission trace differs from the Gold interpreter");
        std::cout << observed.size() << " mission callbacks match the retail reference\n";

        const auto basfFile = std::filesystem::path(argv[1]) / "maps/single/'BASF [Total Victory].ssm";
        const auto basfData = ss2::assets::readMissionData(basfFile);
        for (const unsigned chosenZone : {1u, 2u}) {
            ss2::MissionQueries bq{
                [](unsigned, unsigned) { return 0u; },
                [](unsigned) { return 0u; },
                [](unsigned) { return 20u; },
                [chosenZone](unsigned zone, unsigned group) {
                    return (zone == chosenZone && group == 99u) ? 1u : 0u;
                }
            };
            ss2::Tutorial basfVm(basfData, bq);
            unsigned playerReinf = 0;
            for (const auto& ev : basfVm.takeEvents()) {
                if (ev.kind == Event::Reinforcement && ev.owner == 0) ++playerReinf;
            }
            if (playerReinf < 10)
                throw std::runtime_error("BASF Jeep zone selection did not trigger player OP(40) reinforcements");
        }
        std::cout << "BASF OP(5)/OP(40) reinforcement unlock PASSED\n";
    } else {
        // Hand-authored input exercises strict comparison and one-shot retirement.
        ss2::assets::TutorialData data;
        data.phrases.resize(1);
        data.scripts.resize(12, {0x7fffffff});
        data.scripts[0] = {0x80000001,0x80000001,0x80000000,0x80000002,6,7,11,0x80000000,15,0x7fffffff};
        ss2::Tutorial vm(std::move(data), 1);
        for (unsigned i = 0; i <= 131; ++i) {
            if (i) vm.step(i >= 4 ? 2 : 1);
            const auto events = vm.takeEvents();
            if (i == 67) {
                if (events.size() != 1 || events[0].kind != Event::Message) return 1;
            } else if (!events.empty()) return 1;
        }
        // >30 triggers: dynamic active_ must activate index 35 (old array<30> could not).
        ss2::assets::TutorialData big;
        big.phrases.resize(1);
        big.scripts.resize(40, {0x7fffffff});
        big.scripts[35] = {0x80000001,0x80000001,0x80000000,0x80000002,6,7,11,0x80000000,15,0x7fffffff};
        ss2::MissionQueries queries{
            [](unsigned zone, unsigned player) -> unsigned { return (zone == 2 && player == 0) ? 2u : 0u; },
            [](unsigned) -> unsigned { return 0u; }};
        ss2::Tutorial bigVm(std::move(big), queries);
        const auto first = bigVm.takeEvents();
        if (first.size() != 1 || first[0].kind != Event::Message || first[0].index != 0) return 1;
        for (unsigned i = 0; i < 200; ++i) { bigVm.step(queries); if (!bigVm.takeEvents().empty()) return 1; }
        std::cout << "Dynamic >30-trigger mission VM PASSED\n";

        // Test op 4 (player alive count), op 5 (group in zone), op 18 (1=Victory, 2=Defeat), op 89 (fog reveal), and op 40 LIFO args.
        ss2::assets::TutorialData syn;
        syn.reinforcements[3].push_back({"dguner", 0, 100, 100, 0});
        syn.scripts.resize(5, {0x7fffffff});
        // Script 0: op 4 -> [mask=1, player=1, mode=2(<), thresh=5] => true (count=3 < 5) => op 18 (1 -> Victory)
        syn.scripts[0] = {0x80000001u, 0x80000001u, 0x80000002u, 0x80000005u, 4u, 7u, 11u, 0x80000001u, 18u, 0x7fffffffu};
        // Script 1: op 5 -> [mode=0(==), thresh=1, group=99, zone=2] => op 89 (7, 5, 1) + op 40 (owner=1, wave=3, gate=2, dest=4, delay=6)
        syn.scripts[1] = {0x80000000u, 0x80000001u, 0x80000063u, 0x80000002u, 5u, 7u, 11u,
                          0x80000007u, 0x80000005u, 0x80000001u, 89u,
                          0x80000001u, 0x80000003u, 0x80000002u, 0x80000004u, 0x80000006u, 40u, 0x7fffffffu};
        // Script 2: op 18 (2 -> Defeat)
        syn.scripts[2] = {11u, 0x80000002u, 18u, 0x7fffffffu};
        ss2::MissionQueries sq{
            [](unsigned, unsigned) { return 0u; },
            [](unsigned) { return 0u; },
            [](unsigned player) { return player == 1u ? 3u : 0u; },
            [](unsigned zone, unsigned group) { return (zone == 2u && group == 99u) ? 1u : 0u; }
        };
        ss2::Tutorial symVm(std::move(syn), sq);
        const auto evs = symVm.takeEvents();
        bool sawVictory = false, sawDefeat = false, sawReinf = false;
        for (const auto& ev : evs) {
            if (ev.kind == Event::Victory && ev.index == 1u) sawVictory = true;
            if (ev.kind == Event::Defeat && ev.index == 2u) sawDefeat = true;
            if (ev.kind == Event::Reinforcement && ev.owner == 1u && ev.index == 3u &&
                ev.gate == 2u && ev.destZone == 4u && ev.delaySec == 6u) sawReinf = true;
        }
        if (!sawVictory || !sawDefeat || !sawReinf) return 1;
        std::cout << "CAOF op 4/5/18(Victory+Defeat)/40/89 PASSED\n";

        // Synthetic UnitCatalog combat test: directional armor, small-arms immunity, & dual weapons.
        {
            ss2::assets::UnitSpec tigerSpec{};
            tigerSpec.id = "tiger";
            tigerSpec.isVehicle = true;
            tigerSpec.health = 1160;
            tigerSpec.ammo1 = 94;
            tigerSpec.moveSpeed = 1.12;
            tigerSpec.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {100, 82, 50};
            tigerSpec.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {256, 256, 256};

            ss2::assets::UnitSpec shermanSpec{};
            shermanSpec.id = "sherman.v";
            shermanSpec.isVehicle = true;
            shermanSpec.health = 660;
            shermanSpec.ammo1 = 71;
            shermanSpec.ammo2 = 500;
            shermanSpec.moveSpeed = 1.15;
            shermanSpec.shot1 = {"pierce6-7k", ss2::assets::HitType::Pierce, 500, 500, 600, 600, 0, 5, 1, 5, "gunshot"};
            shermanSpec.shot2 = {"man_machine", ss2::assets::HitType::Machine, 54, 54, 550, 550, 0, 2, 4, 12, "mg_tank_burst"};

            ss2::assets::UnitSpec jeepSpec{};
            jeepSpec.id = "wyllis.mb.jeep";
            jeepSpec.isVehicle = true;
            jeepSpec.health = 70;
            jeepSpec.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {0, 0, 0};

            auto catalog = ss2::assets::UnitCatalog::fromSpecs({tigerSpec, shermanSpec, jeepSpec});
            const auto runAngleTest = [&](int attackerX, int attackerY) -> int {
                std::vector<std::uint32_t> flags(128u * 128u, 0);
                std::vector<std::uint16_t> obs(128u * 128u, 0);
                ss2::FogGrid fog(128, 128, flags);
                auto grid = ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obs);
                std::vector<ss2::assets::VehicleUnit> vunits{
                    {"sherman.v", attackerX, attackerY, 100, 100, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
                    {"tiger", 4096, 2048, 100, 100, 2, 1, 0u, 0u, 0, 0, 0, 0, true, true}, // hullDir 0 = +U (East-Southeast screen: dx=+64, dy=+32)
                };
                ss2::TutorialUnits sim(std::move(grid), {}, {}, std::move(fog), 2, false, vunits, catalog);
                (void)sim.attackWithVehicles(std::array<size_t, 1>{0}, true, 1);
                int hpAfter = 1160;
                for (int t = 0; t < 15; ++t) {
                    (void)sim.step();
                    for (const auto& vp : sim.takeVehiclePoses()) {
                        if (vp.index == 1) hpAfter = vp.hp;
                    }
                }
                return 1160 - hpAfter;
            };
            // Target hullDir = 0 points along +U (+dx=64, +dy=32). Front is (4096+96, 2048+48), rear is (4096-96, 2048-48).
            const int frontDmg = runAngleTest(4192, 2096);
            const int rearDmg = runAngleTest(4000, 1952);
            if (frontDmg <= 0 || rearDmg <= frontDmg) {
                throw std::runtime_error("Directional armor test failed: rearDmg must exceed frontDmg");
            }

            // Test small-arms immunity (Tiger vs rifleman = 0 damage; Jeep vs rifleman > 0) AND dual-weapon coax MG.
            std::vector<std::uint32_t> flags(128u * 128u, 0);
            std::vector<std::uint16_t> obs(128u * 128u, 0);
            ss2::FogGrid fog(128, 128, flags);
            auto grid = ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obs);
            std::vector<ss2::assets::Infantry> inf{
                {"dofficer", 4064, 2032, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
                {"aguner", 4128, 2064, 100, 40, 2, 1, 0, 0, 0, 0, 4u, 0u, false, true},
            };
            std::vector<ss2::assets::VehicleUnit> vunits{
                {"sherman.v", 4000, 2000, 100, 100, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
                {"tiger", 4128, 2032, 100, 100, 2, 1, 0u, 0u, 0, 0, 0, 0, true, true},
                {"wyllis.mb.jeep", 4128, 2080, 100, 0, 3, 1, 0u, 0u, 0, 0, 0, 0, true, true},
            };
            ss2::TutorialUnits sim2(std::move(grid), inf, {}, std::move(fog), 2, false, vunits, catalog);
            (void)sim2.attackVehicle(std::array<size_t, 1>{0}, 1); // attack Tiger
            bool dgunerFiredAtTiger = false;
            for (int t = 0; t < 10; ++t) {
                for (const auto& sp : sim2.step()) {
                    if (sp.index == 0 && sp.fired) dgunerFiredAtTiger = true;
                }
            }
            int tigerHp = 1160;
            for (const auto& vp : sim2.takeVehiclePoses()) if (vp.index == 1) tigerHp = vp.hp;
            if (!dgunerFiredAtTiger || tigerHp != 1160) {
                throw std::runtime_error("Small arms must fire and deal 0 damage to Tiger armor 256");
            }

            (void)sim2.attackVehicle(std::array<size_t, 1>{0}, 2); // attack Wyllis Jeep
            for (int t = 0; t < 60; ++t) (void)sim2.step();
            int jeepHp = 70;
            for (const auto& vp : sim2.takeVehiclePoses()) if (vp.index == 2) jeepHp = vp.hp;
            if (jeepHp >= 70) throw std::runtime_error("Small arms must damage unarmored Wyllis MB Jeep");

            (void)sim2.attackWithVehicles(std::array<size_t, 1>{0}, false, 1); // Sherman vs enemy infantry -> coax MG!
            bool sawSecondary = false;
            int shermanMainAmmo = 71;
            for (int t = 0; t < 15; ++t) {
                (void)sim2.step();
                for (const auto& vp : sim2.takeVehiclePoses()) {
                    if (vp.index == 0) {
                        shermanMainAmmo = vp.ammo;
                        if (vp.fired && vp.firedSecondary && vp.fireSound == "mg_tank_burst") sawSecondary = true;
                    }
                }
            }
            if (!sawSecondary || shermanMainAmmo != 71) {
                throw std::runtime_error("Vehicle vs infantry must fire secondary coax MG without spending main cannon shells");
            }
            std::cout << "UnitCatalog directional armor, small-arms immunity & dual-weapon coax PASSED\n";
        }

        // Deterministic tick-based MissionRunner reinforcement delay (pauses cleanly when paused).
        {
            ss2::assets::TutorialData tdata;
            tdata.width = 64;
            tdata.height = 64;
            tdata.flags.assign(64u * 64u, 0);
            tdata.obstacles.assign(64u * 64u, 0);
            tdata.zones.assign(64u * 64u, 0);
            tdata.reinforcements[0].push_back({"dguner", 0, 100, 100, 0});
            tdata.scripts.resize(1);
            // Script 0: one-shot delaySec=1, destZone=0, gate=0, wave=0, owner=0 -> OP(40).
            tdata.scripts[0] = {11u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000001u, 40u, 0x7fffffffu};

            ss2::FogGrid fog(64, 64, tdata.flags);
            auto grid = ss2::InfantryPathGrid::fromSubCellMasks(64, 64, tdata.flags, tdata.obstacles);
            ss2::TutorialUnits units(std::move(grid), {}, {}, std::move(fog));
            ss2::MissionRunner runner(tdata, 2u, std::move(units));
            if (!runner.poll().empty()) {
                throw std::runtime_error("Delayed OP(40) wave must not arrive at tick 0");
            }
            // Pause immediately and verify delayed wave is held indefinitely while paused.
            (void)runner.togglePause();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!runner.poll().empty()) {
                throw std::runtime_error("Delayed OP(40) wave must not expire while MissionRunner is paused");
            }
            (void)runner.togglePause();
            bool delivered = false;
            for (int i = 0; i < 150 && !delivered; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                for (const auto& ev : runner.poll()) {
                    if (ev.kind == Event::Reinforcement && ev.delaySec == 0u) delivered = true;
                }
            }
            if (!delivered) {
                throw std::runtime_error("Delayed OP(40) wave did not deliver after 25 unpaused mission ticks");
            }
            std::cout << "MissionRunner deterministic tick reinforcement delay PASSED\n";
        }

        // 64/64 CAOF opcode balance + GroupOrder (19/20/21) + FogReveal (89).
        {
            ss2::assets::TutorialData caof;
            caof.width = 64;
            caof.height = 64;
            caof.phrases.resize(1);
            caof.markers[4] = {2048, 1024};
            caof.locations[7] = {2240, 1120};
            caof.scripts.resize(2);
            // Script 0: condition ops (41, 90, 91, 9, 31, 33, 93 all return 0), negated/ORed with op 51 (=1)
            // -> executes 89 (FogReveal), 19/20 (GroupOrder), 8, 22, 23, 24, 25, 39, 42, 46, 58, 59, 62, 63, 71, 72, 75, 77, 79, 80, 82, 83, 84, 85, 86, 92, and ends with 15 (Message 0)!
            caof.scripts[0] = {
                0x80000000u, 26u,
                0x80000000u, 41u,
                2u,
                0x80000001u, 0x80000002u, 0x80000003u, 90u, 2u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 9u, 2u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 31u, 2u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 91u, 2u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000001u, 0x80000005u, 93u, 2u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 0x80000005u, 0x80000006u, 33u, 2u,
                51u, 2u, 7u, 11u,
                // FogReveal(marker=4, radius=8, player=0)
                0x80000004u, 0x80000008u, 0x80000000u, 89u,
                // Group 5 behavior=18, zone1=7
                0x80000005u, 0x80000012u, 19u,
                0x80000005u, 0x80000007u, 20u,
                // Remaining stack-balanced actions
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 0x80000005u, 8u,
                0x80000001u, 0x80000002u, 22u,
                0x80000001u, 0x80000002u, 23u,
                0x80000001u, 0x80000002u, 0x80000003u, 24u,
                0x80000001u, 0x80000002u, 0x80000003u, 25u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 0x80000005u, 0x80000006u, 0x80000007u, 0x80000008u, 39u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 0x80000005u, 42u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 46u,
                0x80000001u, 62u,
                0x80000001u, 0x80000002u, 63u,
                0x80000001u, 0x80000002u, 71u,
                0x80000001u, 0x80000002u, 0x80000003u, 72u,
                0x80000001u, 0x80000002u, 0x80000003u, 75u,
                0x80000001u, 0x80000002u, 0x80000003u, 82u,
                0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 83u,
                0x80000001u, 0x80000002u, 84u,
                0x80000001u, 0x80000002u, 85u,
                0x80000001u, 0x80000002u, 86u,
                // Message 0 at end proves stack remained balanced to depth 0!
                0x80000000u, 15u,
                0x7fffffffu
            };
            // Script 1: zone2 fallback (21 then 19) for Group 6 -> loc 7
            caof.scripts[1] = {11u, 0x80000006u, 0x80000007u, 21u, 0x80000006u, 0x80000012u, 19u, 0x7fffffffu};

            ss2::MissionQueries q;
            ss2::Tutorial vm(std::move(caof), q);
            const auto evs = vm.takeEvents();
            bool sawMsg0 = false, sawFog = false;
            int groupOrders = 0;
            for (const auto& e : evs) {
                if (e.kind == Event::Message && e.index == 0u) sawMsg0 = true;
                if (e.kind == Event::FogReveal && e.index == 4u && e.destZone == 8u) sawFog = true;
                if (e.kind == Event::GroupOrder && (e.index == 5u || e.index == 6u) && e.destZone == 7u) ++groupOrders;
            }
            if (!sawMsg0 || !sawFog || groupOrders != 2) {
                throw std::runtime_error("64/64 CAOF opcode balance / GroupOrder / FogReveal test failed");
            }
            std::cout << "CAOF 64/64 opcode stack balance, GroupOrder & FogReveal PASSED\n";
        }

        // OP(79)/OP(80) patrol queue: a periodic timer cycles a group's waypoints.
        {
            ss2::assets::TutorialData pat;
            pat.phrases.resize(1);
            pat.locations[16] = {1000, 1000};
            pat.locations[17] = {2000, 2000};
            pat.scripts.resize(2, {0x7fffffffu});
            // Script 0 (one-shot): queue group 20 waypoints (17,17),(16,16); timer0 period 4s, due 4s.
            pat.scripts[0] = {11u,
                0x80000011u, 0x80000011u, 0x80000014u, 79u,
                0x80000010u, 0x80000010u, 0x80000014u, 79u,
                0x80000000u, 0x80000004u, 76u,
                0x80000000u, 0x80000004u, 12u, 0x7fffffffu};
            // Script 1 (periodic): when timer0 expires, advance group 20 (no OP(19) behavior set).
            pat.scripts[1] = {0x80000000u, 27u, 7u, 0x80000014u, 80u, 0x7fffffffu};
            ss2::Tutorial pvm(std::move(pat), ss2::MissionQueries{});
            (void)pvm.takeEvents();
            std::vector<unsigned> zones;
            for (unsigned t = 1; t <= 400; ++t) {
                pvm.step(ss2::MissionQueries{});
                for (const auto& e : pvm.takeEvents())
                    if (e.kind == Event::GroupOrder && e.index == 20u) zones.push_back(e.destZone);
            }
            // Gold pre-increments the cursor: first advance lands on waypoint index 1 (loc 16), then 17, then 16.
            if (zones.size() < 3 || zones[0] != 16u || zones[1] != 17u || zones[2] != 16u) {
                throw std::runtime_error("OP(79)/OP(80) patrol queue did not cycle waypoints");
            }
            std::cout << "OP(79)/OP(80) patrol waypoint cycling PASSED\n";
        }

        // Transport embark, unload & destruction bailout + orderGroupMove.
        {
            ss2::assets::UnitSpec truckSpec{};
            truckSpec.id = "opel.blitz";
            truckSpec.isVehicle = true;
            truckSpec.health = 80;
            truckSpec.moveSpeed = 1.2;
            truckSpec.stock = 4;

            ss2::assets::UnitSpec enemyTankSpec{};
            enemyTankSpec.id = "t-34";
            enemyTankSpec.isVehicle = true;
            enemyTankSpec.health = 600;
            enemyTankSpec.ammo1 = 50;
            enemyTankSpec.moveSpeed = 1.2;
            enemyTankSpec.shot1 = {"pierce6-7k", ss2::assets::HitType::Pierce, 400, 400, 500, 500, 0, 5, 1, 5, "gunshot"};

            auto catalog = ss2::assets::UnitCatalog::fromSpecs({truckSpec, enemyTankSpec});
            std::vector<std::uint32_t> flags(128u * 128u, 0);
            std::vector<std::uint16_t> obs(128u * 128u, 0);
            ss2::FogGrid fog(128, 128, flags);
            auto grid = ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obs);
            std::vector<ss2::assets::Infantry> inf{
                {"dguner", 4048, 2032, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
                {"dguner", 4048, 2048, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
                {"rguner", 4300, 2100, 100, 40, 9, 1, 0, 0, 0, 0, 4u, 0u, false, true},
            };
            std::vector<ss2::assets::VehicleUnit> vunits{
                {"opel.blitz", 4080, 2040, 100, 0, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
                {"t-34", 4340, 2120, 100, 100, 9, 1, 0u, 0u, 0, 0, 0, 0, true, true},
            };
            ss2::TutorialUnits sim(std::move(grid), inf, {}, std::move(fog), 2, false, vunits, catalog);
            if (!sim.canBoardVehicle(0, 0)) throw std::runtime_error("Truck must be boardable");
            if (sim.orderBoardVehicle(std::array<size_t, 2>{0, 1}, 0) != 2) {
                throw std::runtime_error("Expected 2 soldiers to accept board order");
            }
            int cargoCount = 0;
            for (int t = 0; t < 30 && cargoCount < 2; ++t) {
                (void)sim.step();
                for (const auto& vp : sim.takeVehiclePoses()) {
                    if (vp.index == 0) cargoCount = vp.passengerCount;
                }
            }
            if (cargoCount != 2) throw std::runtime_error("Soldiers did not embark into truck");

            // Move truck east, then unload -> soldiers reappear!
            (void)sim.moveVehicles(std::array<size_t, 1>{0}, 4160, 2040);
            for (int t = 0; t < 35; ++t) (void)sim.step();
            std::vector<ss2::SoldierPose> outPoses;
            if (sim.unloadVehicles(std::array<size_t, 1>{0}, &outPoses) != 2 || outPoses.size() < 2) {
                throw std::runtime_error("Truck unloadVehicles did not disembark 2 passengers");
            }

            // Order enemy Group 9 to advance on attack-move and verify movement starts.
            if (sim.orderGroupMove(9, 4120, 2040, true) == 0) {
                throw std::runtime_error("orderGroupMove did not move enemy Group 9");
            }
            bool enemyMoved = false;
            for (int t = 0; t < 15; ++t) {
                (void)sim.step();
                for (const auto& vp : sim.takeVehiclePoses()) {
                    if (vp.index == 1 && vp.moving) enemyMoved = true;
                }
            }
            if (!enemyMoved) throw std::runtime_error("Enemy group vehicle did not move after orderGroupMove");

            // Destruction bailout: embarked passenger bails out wounded when transport is destroyed.
            {
                std::vector<std::uint32_t> bFlags(128u * 128u, 0);
                std::vector<std::uint16_t> bObs(128u * 128u, 0);
                ss2::FogGrid bFog(128, 128, bFlags);
                auto bGrid = ss2::InfantryPathGrid::fromSubCellMasks(128, 128, bFlags, bObs);
                std::vector<ss2::assets::Infantry> bInf{
                    {"dguner", 4064, 2040, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
                };
                std::vector<ss2::assets::VehicleUnit> bVeh{
                    {"opel.blitz", 4080, 2040, 100, 0, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
                    {"t-34", 4180, 2040, 100, 100, 9, 1, 0u, 0u, 0, 0, 0, 0, true, true},
                };
                ss2::TutorialUnits bailSim(std::move(bGrid), bInf, {}, std::move(bFog), 2, true, bVeh, catalog);
                if (!bailSim.embarkImmediate(0, 0)) {
                    throw std::runtime_error("bailSim failed to embark soldier 0 into truck");
                }
                bool bailedOut = false;
                bool truckDead = false;
                for (int t = 0; t < 120 && !(bailedOut && truckDead); ++t) {
                    for (const auto& sp : bailSim.step()) {
                        if (sp.index == 0 && sp.visible && sp.alive && sp.hp < 100) bailedOut = true;
                    }
                    for (const auto& vp : bailSim.takeVehiclePoses()) {
                        if (vp.index == 0 && !vp.alive) truckDead = true;
                    }
                }
                if (!truckDead || !bailedOut) {
                    throw std::runtime_error("Transport destruction bailout did not leave wounded visible passenger");
                }
            }
            std::cout << "Transport embark, unload, bailout, and orderGroupMove PASSED\n";
        }

        // Phase 4A: HE splash, supply repair/resupply, ambulance heal, attack-move & Shift waypoints.
        {
            ss2::assets::UnitSpec howSpec{};
            howSpec.id = "hummel";
            howSpec.isVehicle = true;
            howSpec.health = 400;
            howSpec.ammo1 = 30;
            howSpec.moveSpeed = 1.5;
            howSpec.moveDamageHp = 200;
            howSpec.shot1 = {"howitzer", ss2::assets::HitType::Explosive, 200, 200, 500, 500, 0, 10, 1, 10, "howitzer"};

            ss2::assets::UnitSpec supSpec{};
            supSpec.id = "sdkfz.11";
            supSpec.isVehicle = true;
            supSpec.selType = "sappers";
            supSpec.ammoRegenDelay = 8;
            supSpec.health = 180;
            supSpec.moveSpeed = 1.2;

            ss2::assets::UnitSpec ambSpec{};
            ambSpec.id = "gmc.353.med";
            ambSpec.isVehicle = true;
            ambSpec.healDelay = 10;
            ambSpec.health = 170;
            ambSpec.moveSpeed = 1.2;

            auto cat = ss2::assets::UnitCatalog::fromSpecs({howSpec, supSpec, ambSpec});
            std::vector<std::uint32_t> flags(128u * 128u, 0);
            std::vector<std::uint16_t> obs(128u * 128u, 0);
            ss2::FogGrid fog(128, 128, flags);
            auto grid = ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obs);

            // Soldier 0 & 1 (player), Soldier 2 (enemy primary target), Soldier 3 (enemy adjacent in splash radius)
            std::vector<ss2::assets::Infantry> inf{
                {"dguner", 4000, 2040, 40, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},   // wounded friendly
                {"dguner", 3980, 2140, 100, 40, 2, 0, 0, 0, 0, 0, 0u, 0u, false, true},  // attack-move & waypoint test
                {"rguner", 4180, 2040, 100, 40, 5, 1, 0, 0, 0, 0, 4u, 0u, false, true},  // enemy direct target
                {"rguner", 4210, 2040, 100, 40, 5, 1, 0, 0, 0, 0, 4u, 0u, false, true},  // enemy inside 72px HE splash!
                {"rguner", 4120, 2140, 100, 40, 6, 1, 0, 0, 0, 0, 4u, 0u, false, true},  // enemy for attack-move test
            };
            // Vehicle 0: damaged low-ammo Hummel; Vehicle 1: Supply truck; Vehicle 2: Ambulance
            std::vector<ss2::assets::VehicleUnit> veh{
                {"hummel", 4040, 2040, 40, 30, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
                {"sdkfz.11", 4080, 2040, 100, 0, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
                {"gmc.353.med", 4020, 2040, 100, 0, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
            };
            // autoEngage = false so we explicitly test attackMove + manual orders + support trucks!
            ss2::TutorialUnits sim(std::move(grid), inf, {}, std::move(fog), 2, false, veh, cat);

            // Issue Shift-queued waypoints to Hummel and Soldier 1
            if (sim.moveVehicles(std::array<size_t, 1>{1}, 4090, 2040, false) != 1 ||
                sim.moveVehicles(std::array<size_t, 1>{1}, 4080, 2040, true) != 1) {
                throw std::runtime_error("Vehicle Shift-queued waypoint order failed");
            }
            if (sim.orderAttackMove(std::array<size_t, 1>{1}, 4140, 2140, false) != 1 ||
                sim.orderAttackMove(std::array<size_t, 1>{1}, 4180, 2140, true) != 1) {
                throw std::runtime_error("Infantry attackMove + Shift-queued waypoint failed");
            }
            // Also tell Hummel to fire HE shell at enemy soldier 2
            if (sim.attackWithVehicles(std::array<size_t, 1>{0}, false, 2) != 1) {
                throw std::runtime_error("Hummel failed to target enemy infantry");
            }

            bool splashWounded = false;
            bool hummelRepaired = false;
            bool hummelResupplied = false;
            bool woundedHealed = false;
            bool attackMoveEngaged = false;
            for (int t = 0; t < 160; ++t) {
                for (const auto& sp : sim.step()) {
                    if (sp.index == 3 && sp.hp < 100) splashWounded = true;
                    if (sp.index == 0 && sp.hp > 40) woundedHealed = true;
                    if (sp.index == 1 && sp.fired) attackMoveEngaged = true;
                }
                for (const auto& vp : sim.takeVehiclePoses()) {
                    if (vp.index == 0 && vp.repaired) hummelRepaired = true;
                    if (vp.index == 0 && vp.resupplied) hummelResupplied = true;
                }
            }
            if (!splashWounded) throw std::runtime_error("HE splash did not damage adjacent enemy infantry");
            if (!hummelRepaired || !hummelResupplied) throw std::runtime_error("Supply truck did not repair and resupply Hummel");
            if (!woundedHealed) throw std::runtime_error("Ambulance did not heal wounded friendly infantry");
            if (!attackMoveEngaged) throw std::runtime_error("Attack-move infantry did not engage enemy along route");
            std::cout << "Phase 4A HE splash, supply repair/resupply, ambulance heal, attack-move & waypoints PASSED\n";
        }

        // Phase 4B: Diplomacy + Allied shared vision, Neutral capture, I-MINES + Sapper, Attack-Ground, lastHitTick & speed.
        {
            ss2::assets::UnitSpec howSpec{};
            howSpec.id = "hummel";
            howSpec.isVehicle = true;
            howSpec.health = 400;
            howSpec.ammo1 = 30;
            howSpec.moveSpeed = 1.5;
            howSpec.shot1 = {"howitzer", ss2::assets::HitType::Explosive, 200, 200, 500, 500, 0, 10, 1, 10, "howitzer"};

            ss2::assets::UnitSpec neutSpec{};
            neutSpec.id = "opel.blitz";
            neutSpec.isVehicle = true;
            neutSpec.health = 100;
            neutSpec.moveSpeed = 1.2;

            ss2::assets::UnitSpec enemyTank{};
            enemyTank.id = "t-34";
            enemyTank.isVehicle = true;
            enemyTank.health = 400;
            enemyTank.moveSpeed = 2.0;

            auto cat = ss2::assets::UnitCatalog::fromSpecs({howSpec, neutSpec, enemyTank});
            std::vector<std::uint32_t> flags(128u * 128u, 0);
            std::vector<std::uint16_t> obs(128u * 128u, 0);
            ss2::FogGrid fog(128, 128, flags);
            auto grid = ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obs);

            // 128x128: originX = 4096. Tile (64,64) = (4096,2064); tile (65,64) = (4128,2080); tile (60,60) = (4096,1936).
            std::vector<ss2::assets::Infantry> inf{
                {"dsapper", 4096, 2064, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},   // 0: player sapper at (64,64)
                {"dguner", 4128, 1952, 100, 40, 2, 2, 0, 0, 0, 0, 0u, 0u, false, true},    // 1: allied owner 2
                {"rguner", 4288, 2128, 100, 40, 5, 1, 0, 0, 0, 0, 4u, 0u, false, true},    // 2: enemy soldier for attackGround
            };
            std::vector<ss2::assets::VehicleUnit> veh{
                {"hummel", 3980, 2080, 100, 100, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},    // 0: player artillery
                {"opel.blitz", 4032, 2000, 100, 0, 0, 3, 0u, 0u, 0, 0, 0, 0, true, true},  // 1: neutral truck (owner 3)
                {"t-34", 4096, 1904, 100, 0, 9, 1, 0u, 0u, 0, 0, 0, 0, true, true},        // 2: enemy tank at (59,60)->steps to (60,60)
            };
            ss2::TutorialUnits sim(std::move(grid), inf, {}, std::move(fog), 2, true, veh, cat);
            sim.setPlayerTeams({0, 1, 0, 3});

            // Place enemy AT mine at (65,64) near Sapper, and player AT mine at (60,60) in front of enemy tank!
            std::vector<std::uint8_t> mines(128u * 128u, 0u);
            mines[64u * 128u + 65u] = 0x45u; // enemy (owner 1) AT mine
            mines[60u * 128u + 60u] = 0x85u; // player (owner 0) AT mine
            sim.setMines(mines);

            if (sim.isHostile(0, 2) || !sim.isHostile(0, 1) || sim.isHostile(0, 3)) {
                throw std::runtime_error("setPlayerTeams diplomacy check failed");
            }
            if (!sim.canCaptureVehicle(1, 0) || sim.isValidVehicleAttackTarget(1)) {
                throw std::runtime_error("Neutral vehicle must be capturable and not attackable");
            }
            if (sim.orderLayMine(std::array<size_t, 1>{0}) != 1) {
                throw std::runtime_error("Sapper failed to lay mine at current tile");
            }
            if (sim.orderAttackGroundVehicles(std::array<size_t, 1>{0}, 4288, 2128) != 1) {
                throw std::runtime_error("orderAttackGroundVehicles failed");
            }
            (void)sim.orderGroupMove(9, 4096, 1968, true);

            bool detected = false, disarmed = false, detonated = false, laid = false;
            bool enemyHitByShell = false, neutralShot = false;
            for (int t = 0; t < 45; ++t) {
                for (const auto& sp : sim.step()) {
                    if (sp.index == 2 && sp.hp < 100) enemyHitByShell = true;
                }
                for (const auto& vp : sim.takeVehiclePoses()) {
                    if (vp.index == 1 && vp.hp < 100) neutralShot = true;
                }
                for (const auto& me : sim.takeMineEvents()) {
                    if (me.kind == ss2::MineEvent::Detected) detected = true;
                    if (me.kind == ss2::MineEvent::Disarmed) disarmed = true;
                    if (me.kind == ss2::MineEvent::Detonated) detonated = true;
                    if (me.kind == ss2::MineEvent::Laid) laid = true;
                }
            }
            if (neutralShot) throw std::runtime_error("Allied units fired on Neutral vehicle");
            if (!laid || !detected || !disarmed || !detonated) {
                throw std::runtime_error("Minefield lifecycle (Laid/Detected/Disarmed/Detonated) incomplete");
            }
            if (!enemyHitByShell || !sim.lastHitTick(1).has_value()) {
                throw std::runtime_error("Attack-ground shell did not hit enemy or record lastHitTick");
            }
            if (sim.orderCaptureVehicle(std::array<size_t, 1>{0}, 1) != 1) {
                throw std::runtime_error("orderCaptureVehicle did not accept sapper");
            }
            bool captured = false;
            for (int t = 0; t < 120 && !captured; ++t) {
                (void)sim.step();
                for (const auto& vp : sim.takeVehiclePoses()) {
                    if (vp.index == 1 && vp.captured && vp.owner == 0) captured = true;
                }
            }
            if (!captured) throw std::runtime_error("Neutral vehicle capture did not flip owner to 0");

            // Check MissionRunner dynamic speed scaling API cleanly.
            {
                ss2::assets::TutorialData mData{};
                mData.width = 128;
                mData.height = 128;
                mData.flags.assign(128u * 128u, 0);
                mData.obstacles.assign(128u * 128u, 0);
                mData.zones.assign(128u * 128u, 0);
                mData.phrases.resize(1);
                mData.scripts.resize(1, {0x7fffffffu});
                std::vector<std::uint32_t> rf(128u * 128u, 0);
                std::vector<std::uint16_t> ro(128u * 128u, 0);
                ss2::MissionRunner runner(mData, 20, ss2::TutorialUnits(
                    ss2::InfantryPathGrid::fromSubCellMasks(128, 128, rf, ro), {}, {}));
                runner.setSpeedPercent(200);
                if (runner.speedPercent() != 200) throw std::runtime_error("MissionRunner speedPercent mismatch");
            }
            std::cout << "Phase 4B diplomacy, neutral capture, mines, attack-ground, lastHitTick & speed PASSED\n";
        }

        // Phase 4C synthetic test: towed artillery hitch/follow/unhitch, garrisonable houses & urban warfare, OP49/OP17 wiring.
        {
            std::vector<std::uint32_t> flags(128u * 128u, 0);
            std::vector<std::uint16_t> obstacles(128u * 128u, 0);
            const auto tc = [](int tx, int ty) -> std::pair<int, int> {
                const int wu = tx * 32 + 16;
                const int wv = ty * 32 + 16;
                return {4096 + wu - wv, (wu + wv) / 2};
            };
            const auto [trX, trY] = tc(50, 50);
            const auto [gunX, gunY] = tc(51, 50);
            const auto [destX, destY] = tc(56, 56);

            ss2::assets::UnitSpec zis5{};
            zis5.id = "zis-5";
            zis5.isVehicle = true;
            zis5.health = 150;
            zis5.moveSpeed = 1.6;
            zis5.sight = 12;
            zis5.towSpeedByClass = {1.0, 0.70, 0.60, 0.52, 0.45, 0.35};

            ss2::assets::UnitSpec pak40{};
            pak40.id = "pak40";
            pak40.isVehicle = true;
            pak40.health = 120;
            pak40.ammo1 = 20;
            pak40.moveSpeed = 0.0;
            pak40.sight = 12;
            pak40.towWeightClass = "handy";
            pak40.shot1 = {"ap75", ss2::assets::HitType::Pierce, 400, 400, 520, 520, 0, 30, 1, 30, "gunshot"};

            auto towCat = ss2::assets::UnitCatalog::fromSpecs({zis5, pak40});
            std::vector<ss2::assets::Placement> towVeh{
                {"zis-5", trX, trY, 0, 100, 100, 1, 0},
                {"pak40", gunX, gunY, 0, 100, 100, 1, 0},
            };
            ss2::FogGrid towFog(128, 128, flags);
            ss2::TutorialUnits towSim(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                      {}, towVeh, std::move(towFog), 2, true, {}, towCat);
            if (!towSim.canTowVehicle(0, 1, 0)) throw std::runtime_error("ZiS-5 should be able to tow PaK 40");
            if (towSim.orderTowVehicle(0, 1) != 1) throw std::runtime_error("orderTowVehicle failed to hitch PaK 40");
            if (towSim.moveVehicles(std::array<size_t, 1>{0}, destX, destY) != 1) {
                throw std::runtime_error("Failed to move ZiS-5 tractor after hitching");
            }
            int gunMovedX = gunX;
            int gunMovedY = gunY;
            int towedBySeen = -1;
            bool gunFiredWhileTowed = false;
            for (int t = 0; t < 80; ++t) {
                (void)towSim.step();
                for (const auto& vp : towSim.takeVehiclePoses()) {
                    if (vp.index == 1) {
                        gunMovedX = vp.x;
                        gunMovedY = vp.y;
                        towedBySeen = vp.towedByIndex;
                        if (vp.fired) gunFiredWhileTowed = true;
                    }
                }
            }
            if (towedBySeen != 0 || std::hypot(double(gunMovedX - gunX), double(gunMovedY - gunY)) < 60.0 || gunFiredWhileTowed) {
                throw std::runtime_error("Towed PaK 40 did not follow tractor cleanly or fired while hitched");
            }
            if (towSim.unhitchVehicles(std::array<size_t, 1>{0}) != 1) {
                throw std::runtime_error("unhitchVehicles did not detach PaK 40");
            }
            int afterUnhitch = 99;
            for (const auto& vp : towSim.takeVehiclePoses()) {
                if (vp.index == 0) afterUnhitch = vp.towedIndex;
            }
            if (afterUnhitch != -1) throw std::runtime_error("Tractor towedIndex did not reset to -1 after unhitch");

            // Garrisonable house test: place a solid 2x2 obstacle footprint under House 0 (66, 60)
            // to verify findBuildingEntryCell locates a walkable doorstep nearby!
            const int hx0 = 66, hy0 = 60;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    obstacles[std::size_t(hy0 + dy) * 128u + std::size_t(hx0 + dx)] = 0x1111u;
                }
            }
            const auto [houseX, houseY] = tc(hx0, hy0);
            const auto [g1X, g1Y] = tc(64, 60);
            const auto [g2X, g2Y] = tc(65, 60);
            const auto [eneX, eneY] = tc(65, 61); // outside House 0 doorstep, hostile rifleman group 12
            const auto [flmX, flmY] = tc(64, 62); // enemy flamer group 13
            const auto [tnkX, tnkY] = tc(76, 66); // enemy tank with HE inside range to collapse House 0
            const auto [h2X, h2Y] = tc(75, 50);
            const auto [g3X, g3Y] = tc(74, 50);

            ss2::assets::UnitSpec t34{};
            t34.id = "t-34";
            t34.isVehicle = true;
            t34.health = 280;
            t34.ammo1 = 20;
            t34.moveSpeed = 1.6;
            t34.sight = 14;
            t34.shot1 = {"he76", ss2::assets::HitType::Explosive, 160, 160, 480, 480, 0, 20, 1, 20, "tankgun"};

            auto garCat = ss2::assets::UnitCatalog::fromSpecs({t34});
            std::vector<ss2::assets::Infantry> inf{
                {"dguner", g1X, g1Y, 200, 40, 1, 0},
                {"dguner", g2X, g2Y, 200, 40, 1, 0},
                {"dguner", eneX, eneY, 200, 40, 12, 1},
                {"dflamer", flmX, flmY, 100, 40, 13, 1},
                {"dguner", g3X, g3Y, 100, 40, 1, 0},
            };
            std::vector<ss2::assets::Placement> garVeh{
                {"t-34", tnkX, tnkY, 0, 100, 100, 14, 1},
            };
            ss2::FogGrid fog(128, 128, flags);
            ss2::TutorialUnits sim(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                   inf, garVeh, std::move(fog), 2, true, {}, garCat);
            sim.setBuildings({
                {houseX, houseY, houseX - 36, houseY - 36, 72, 72, 4, 400},
                {h2X, h2Y, h2X - 32, h2Y - 32, 64, 64, 4, 300},
            });
            sim.revealFogAt(houseX, houseY, 16, 0);
            sim.revealFogAt(eneX, eneY, 16, 0);

            if (!sim.canGarrisonBuilding(0, 0) || sim.orderGarrisonBuilding(std::array<size_t, 2>{0, 1}, 0) != 2) {
                throw std::runtime_error("orderGarrisonBuilding failed on House 0");
            }
            if (sim.orderGarrisonBuilding(std::array<size_t, 1>{4}, 1) != 1) {
                throw std::runtime_error("orderGarrisonBuilding failed on House 1");
            }
            bool garEntered = false;
            bool garFired = false;
            bool houseCollapsed = false;
            bool eneGroupHit = false;
            for (int t = 0; t < 180; ++t) {
                for (const auto& sp : sim.step()) {
                    if ((sp.index == 0 || sp.index == 1) && sp.fired && sp.garrisoned) garFired = true;
                }
                for (const auto& bu : sim.takeBuildingUpdates()) {
                    if (bu.index == 0 && bu.occupantCount == 2 && bu.owner == 0) garEntered = true;
                    if (bu.index == 0 && bu.collapsedThisTick && bu.destroyed) houseCollapsed = true;
                }
                if (sim.lastGroupHitTick(12).has_value()) eneGroupHit = true;
                if (houseCollapsed) break;
            }
            if (!garEntered) throw std::runtime_error("Soldiers did not enter garrisoned House 0");
            if (!garFired) throw std::runtime_error("Garrisoned soldiers did not fire out of House 0");
            if (!eneGroupHit) throw std::runtime_error("OP(49) group lastHitTick not recorded on enemy group 12");
            if (!houseCollapsed) throw std::runtime_error("Enemy flamer/HE tank did not collapse occupied House 0");

            // Evacuate House 1 cleanly & check dynamic OP(17) setPlayerTeam(2, 0).
            std::vector<ss2::SoldierPose> evacPoses;
            if (sim.evacuateBuilding(1, &evacPoses) != 1 || evacPoses.empty() || !evacPoses[0].alive) {
                throw std::runtime_error("evacuateBuilding did not emerge survivor cleanly");
            }
            if (!sim.isHostile(0, 2)) throw std::runtime_error("Slot 2 should initially be hostile to 0");
            sim.setPlayerTeam(2, 0);
            if (sim.isHostile(0, 2)) throw std::runtime_error("setPlayerTeam(2, 0) did not switch team alliance");

            std::cout << "Phase 4C towed guns, garrisonable houses & OP49/OP17 wiring PASSED\n";
        }

        // Phase 4D synthetic test: tank reverse gear, close-range AT grenades, unit veterancy/kills, and Snapshot save/load.
        {
            std::vector<std::uint32_t> flags(128u * 128u, 0);
            std::vector<std::uint16_t> obstacles(128u * 128u, 0);
            const auto tc = [](int tx, int ty) {
                return std::pair{4096 + (tx - ty) * 32, (tx + ty) * 16 + 16};
            };

            ss2::assets::UnitSpec tankSpec{};
            tankSpec.id = "t-34";
            tankSpec.isVehicle = true;
            tankSpec.health = 280;
            tankSpec.ammo1 = 20;
            tankSpec.moveSpeed = 1.6;
            tankSpec.sight = 14;
            tankSpec.canMoveBackward = true;
            tankSpec.backMoveSpeed = 0.8;
            tankSpec.shot1 = {"he76", ss2::assets::HitType::Explosive, 160, 160, 480, 480, 0, 20, 1, 20, "tankgun"};

            ss2::assets::UnitSpec carSpec{};
            carSpec.id = "kubel";
            carSpec.isVehicle = true;
            carSpec.health = 180;
            carSpec.moveSpeed = 1.8;
            carSpec.sight = 10;

            auto cat4d = ss2::assets::UnitCatalog::fromSpecs({tankSpec, carSpec});

            const auto [pTankX, pTankY] = tc(60, 50);
            const auto [eCarX, eCarY] = tc(64, 70);
            const auto [greX, greY] = tc(63, 70); // ~35.8px from enemy kubel (<= 88px AT grenade range)
            const auto [eInfX, eInfY] = tc(65, 50); // ~179px from tank (inside 312px scaled gun range) for tank kill/expa check
            std::vector<ss2::assets::Infantry> inf4d{
                {"dguner", greX, greY, 100, 40, 1, 0},
            };
            std::vector<ss2::assets::Placement> veh4d{
                {"t-34", pTankX, pTankY, 28, 100, 100, 1, 0},
            };
            ss2::FogGrid fog4d(128, 128, flags);
            ss2::TutorialUnits sim4d(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                     inf4d, veh4d, std::move(fog4d), 2, true, {}, cat4d);
            sim4d.setBuildings({
                {tc(80, 80).first, tc(80, 80).second, tc(80, 80).first - 24, tc(80, 80).second - 24, 48, 48, 4, 300}
            });

            // 1) Tank reverse gear keeps front armor facing east (28) while moving west (-X).
            if (sim4d.moveVehicles(std::array<size_t, 1>{0}, pTankX + 120, pTankY) != 1) {
                throw std::runtime_error("moveVehicles east failed in Phase 4D test");
            }
            for (int t = 0; t < 20; ++t) (void)sim4d.step();
            int midTankX = pTankX;
            for (const auto& vp : sim4d.takeVehiclePoses()) {
                if (vp.index == 0) midTankX = vp.x;
            }
            if (midTankX <= pTankX + 20) throw std::runtime_error("Player tank did not advance east before reversing");

            if (sim4d.reverseMoveVehicles(std::array<size_t, 1>{0}, midTankX - 100, pTankY) != 1) {
                throw std::runtime_error("reverseMoveVehicles rejected capable tank");
            }
            unsigned revHullDir = 99u;
            int revEndX = midTankX;
            for (int t = 0; t < 35; ++t) {
                (void)sim4d.step();
                for (const auto& vp : sim4d.takeVehiclePoses()) {
                    if (vp.index == 0) {
                        revHullDir = vp.hullDir;
                        revEndX = vp.x;
                    }
                }
            }
            if (revEndX >= midTankX - 20 || revHullDir != 28u) {
                throw std::runtime_error("Reverse gear did not retreat west while preserving east-facing front armor (dir 28)");
            }

            // 2) Close-range AT grenade kill & tank veterancy/kills (spawned fresh after reverse test).
            (void)sim4d.spawnInfantry({"dguner", eInfX, eInfY, 60, 40, 11, 1});
            (void)sim4d.spawnVehicle({"kubel", eCarX, eCarY, 100, 0, 12, 1, 0u, 0u, 0, 0, 0, 0, true, true});
            sim4d.revealFogAt(eCarX, eCarY, 12, 0);
            sim4d.revealFogAt(eInfX, eInfY, 12, 0);
            bool threwGrenade = false;
            int greAfter = 99;
            int greKills = 0;
            int greExpa = 0;
            bool kubelDead = false;
            int tankKills = 0;
            int tankExpa = 0;
            for (int t = 0; t < 160; ++t) {
                for (const auto& sp : sim4d.step()) {
                    if (sp.index == 0) {
                        if (sp.threwGrenade) threwGrenade = true;
                        greAfter = sp.atGrenades;
                        greKills = std::max(greKills, sp.kills);
                        greExpa = std::max(greExpa, sp.expa);
                    }
                }
                for (const auto& vp : sim4d.takeVehiclePoses()) {
                    if (vp.index == 1 && !vp.alive) kubelDead = true;
                    if (vp.index == 0) {
                        tankKills = std::max(tankKills, vp.kills);
                        tankExpa = std::max(tankExpa, vp.expa);
                    }
                }
                if (threwGrenade && kubelDead && tankKills >= 1 && greKills >= 1) break;
            }
            if (!threwGrenade || greAfter >= 2 || !kubelDead || greKills < 1 || greExpa < 4096) {
                throw std::runtime_error("Infantry close-range AT grenade / kill / veterancy check failed");
            }
            if (tankKills < 1 || tankExpa < 1500) {
                throw std::runtime_error("Vehicle kill / veterancy counter did not increment");
            }

            // 3) Snapshot binary serialize/deserialize round-trip & live state restore.
            const auto savedSnap = sim4d.takeSnapshot();
            const auto encoded = ss2::TutorialUnits::serializeSnapshot(savedSnap);
            const auto decoded = ss2::TutorialUnits::deserializeSnapshot(encoded);
            if (!decoded || !(*decoded == savedSnap)) {
                throw std::runtime_error("Snapshot binary serialize/deserialize round-trip mismatch");
            }
            std::vector<std::uint8_t> corrupt(encoded.begin(), encoded.begin() + 12);
            if (ss2::TutorialUnits::deserializeSnapshot(corrupt).has_value()) {
                throw std::runtime_error("deserializeSnapshot accepted truncated buffer");
            }
            // Mutate live state before loading snapshot.
            sim4d.setPlayerTeam(1, 0);
            (void)sim4d.moveVehicles(std::array<size_t, 1>{0}, tc(40, 40).first, tc(40, 40).second);
            for (int t = 0; t < 15; ++t) (void)sim4d.step();
            if (sim4d.takeSnapshot() == savedSnap) {
                throw std::runtime_error("Live simulation state failed to diverge prior to loadSnapshot");
            }
            std::vector<ss2::SoldierPose> restoredPoses;
            if (!sim4d.loadSnapshot(*decoded, &restoredPoses) || restoredPoses.size() != 2) {
                throw std::runtime_error("loadSnapshot failed on valid decoded snapshot");
            }
            if (!(sim4d.takeSnapshot() == savedSnap)) {
                throw std::runtime_error("loadSnapshot did not restore exact Snapshot state");
            }

            std::cout << "Phase 4D reverse gear, AT grenades, veterancy & snapshot round-trip PASSED\n";
        }

        // Phase 4E synthetic test: air support (Recon/Bomber/Paratrooper/AA flak), tactical AI counter-attack, and SS2SNAP2/SS2SNAP1 compat.
        {
            std::vector<std::uint32_t> flags(128u * 128u, 0);
            std::vector<std::uint16_t> obstacles(128u * 128u, 0);
            const auto tc = [](int tx, int ty) {
                return std::pair{4096 + (tx - ty) * 32, (tx + ty) * 16 + 16};
            };

            ss2::assets::UnitSpec flakSpec{};
            flakSpec.id = "88mmflak";
            flakSpec.isVehicle = true;
            flakSpec.roleClass = "aa";
            flakSpec.health = 220;
            flakSpec.ammo1 = 40;
            flakSpec.moveSpeed = 0.0;
            flakSpec.sight = 14;
            flakSpec.shot1 = {"aa88", ss2::assets::HitType::Explosive, 120, 120, 600, 600, 0, 20, 1, 20, "gunshot"};

            ss2::assets::UnitSpec carSpec{};
            carSpec.id = "kubel";
            carSpec.isVehicle = true;
            carSpec.health = 180;
            carSpec.moveSpeed = 1.6;
            carSpec.sight = 10;

            ss2::assets::UnitSpec pTankSpec{};
            pTankSpec.id = "tiger";
            pTankSpec.isVehicle = true;
            pTankSpec.health = 800;
            pTankSpec.ammo1 = 30;
            pTankSpec.moveSpeed = 1.5;
            pTankSpec.sight = 16;
            pTankSpec.shot1 = {"ap88", ss2::assets::HitType::Pierce, 220, 220, 750, 750, 0, 15, 1, 15, "gunshot"};

            ss2::assets::UnitSpec eTankSpec{};
            eTankSpec.id = "t-34";
            eTankSpec.isVehicle = true;
            eTankSpec.health = 600;
            eTankSpec.ammo1 = 30;
            eTankSpec.moveSpeed = 1.6;
            eTankSpec.sight = 8; // 256px sight < 430px attack distance
            eTankSpec.shot1 = {"ap76", ss2::assets::HitType::Pierce, 140, 140, 450, 450, 0, 20, 1, 20, "tankgun"};

            auto cat4e = ss2::assets::UnitCatalog::fromSpecs({flakSpec, carSpec, pTankSpec, eTankSpec});
            const auto [targetX, targetY] = tc(10, 113); // mapX = 800, mapY = 1984 (west entry edge -> fast pass)
            const auto [pTankX, pTankY] = tc(48, 60);
            const auto [eTankX, eTankY] = tc(60, 60);
            const auto [pShootX, pShootY] = tc(45, 60);
            const auto [eS1X, eS1Y] = tc(58, 60);
            const auto [eS2X, eS2Y] = tc(59, 60);

            std::vector<ss2::assets::Infantry> inf4e{
                {"dguner", pShootX, pShootY, 100, 40, 1, 0},
                {"dguner", eS1X, eS1Y, 120, 40, 9, 1},
                {"dguner", eS2X, eS2Y, 120, 40, 9, 1},
            };
            std::vector<ss2::assets::Placement> veh4e{
                {"tiger", pTankX, pTankY, 0, 100, 100, 1, 0},
                {"t-34", eTankX, eTankY, 0, 100, 100, 9, 1}, // hullDir = 0
                {"kubel", targetX, targetY, 0, 100, 0, 12, 1},
            };
            ss2::FogGrid fog4e(128, 128, flags);
            ss2::TutorialUnits sim4e(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                     inf4e, veh4e, std::move(fog4e), 2, true, {}, cat4e);
            sim4e.revealFogAt(eTankX, eTankY, 12, 0);
            sim4e.revealFogAt(eS1X, eS1Y, 12, 0);

            std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air{};
            air[0][0] = {"ju-87", 2, 2};
            air[0][1] = {"audax", 1, 1};
            air[0][2] = {"ju-52", 1, 1};
            std::array<int, 4> paraGrp{7, 0, 0, 0};
            std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> paraRos{};
            paraRos[0] = {{"dsmguner", 6}};
            sim4e.setAirSupport(air, paraGrp, paraRos);

            if (!sim4e.callAirSupport(ss2::assets::AirMissionKind::Recon, targetX, targetY, 0) ||
                !sim4e.callAirSupport(ss2::assets::AirMissionKind::Bomber, targetX, targetY, 0) ||
                !sim4e.callAirSupport(ss2::assets::AirMissionKind::Paratrooper, targetX, targetY, 0)) {
                throw std::runtime_error("callAirSupport failed in Phase 4E test");
            }

            // Trigger tactical AI counter-attack on enemy tank & group 9 soldiers while air sorties approach target.
            (void)sim4e.attackWithVehicles(std::array<size_t, 1>{0}, true, 1);
            (void)sim4e.orderAttack(std::array<size_t, 1>{0}, 1);

            bool reconRevealed = false;
            bool bombHit = false;
            bool kubelBombed = false;
            bool paraDropped = false;
            bool aiTankReacted = false;
            bool aiSoldierReacted = false;
            for (int t = 0; t < 150; ++t) {
                for (const auto& sp : sim4e.step()) {
                    if (sp.index == 2 && sp.walking) aiSoldierReacted = true;
                }
                for (const auto& vp : sim4e.takeVehiclePoses()) {
                    if (vp.index == 2 && !vp.alive) kubelBombed = true;
                    if (vp.index == 1 && (vp.moving || vp.hullDir != 0u)) aiTankReacted = true;
                }
                for (const auto& ae : sim4e.takeAirStrikeEvents()) {
                    if (ae.kind == ss2::AirStrikeEvent::BombImpact) bombHit = true;
                    if (ae.kind == ss2::AirStrikeEvent::ParatrooperDrop && ae.group == 7 &&
                        !ae.paratroopers.empty() && ae.paratroopers[0].count == 6) {
                        paraDropped = true;
                    }
                }
                if (auto f = sim4e.takeFogSnapshot()) {
                    if (f->size() == 128u * 128u && ((*f)[std::size_t(113u) * 128u + 10u] & 1u) != 0u) {
                        reconRevealed = true;
                    }
                }
                if (reconRevealed && bombHit && kubelBombed && paraDropped && aiTankReacted && aiSoldierReacted) break;
            }
            if (!reconRevealed || !bombHit || !kubelBombed || !paraDropped) {
                throw std::runtime_error(
                    std::string("Phase 4E air support failed (recon=") + (reconRevealed ? "1" : "0") +
                    " bomb=" + (bombHit ? "1" : "0") +
                    " kubel=" + (kubelBombed ? "1" : "0") +
                    " para=" + (paraDropped ? "1" : "0") + ")");
            }
            if (!aiTankReacted || !aiSoldierReacted) {
                throw std::runtime_error(
                    std::string("Phase 4E tactical AI counter-attack reaction failed (tank=") +
                    (aiTankReacted ? "1" : "0") + " sol=" + (aiSoldierReacted ? "1" : "0") + ")");
            }

            // 2) Hostile AA flak intercepts second Bomber sortie flying over (targetX, targetY) toward (1200, targetY).
            for (int t = 0; t < 75; ++t) {
                (void)sim4e.step();
            }
            (void)sim4e.takeAirSorties();
            (void)sim4e.takeAirStrikeEvents();

            (void)sim4e.spawnVehicle({"88mmflak", targetX, targetY, 100, 100, 15, 1, 0u, 0u, 0, 0, 0, 0, true, true});
            const auto [aaGoalX, aaGoalY] = tc(16, 106); // ~1216 px East, forcing >100 ticks inside 520px AA ring
            if (!sim4e.callAirSupport(ss2::assets::AirMissionKind::Bomber, aaGoalX, aaGoalY, 0)) {
                throw std::runtime_error("Second Bomber call failed before AA flak test");
            }
            bool aaFired = false;
            bool planeDown = false;
            for (int t = 0; t < 160 && !(aaFired && planeDown); ++t) {
                (void)sim4e.step();
                for (const auto& sp : sim4e.takeAirSorties()) {
                    if (sp.aaFired) aaFired = true;
                }
                for (const auto& ae : sim4e.takeAirStrikeEvents()) {
                    if (ae.kind == ss2::AirStrikeEvent::PlaneShotDown) planeDown = true;
                }
            }
            if (!aaFired || !planeDown) {
                throw std::runtime_error(
                    std::string("Phase 4E hostile AA flak interception failed (aa=") +
                    (aaFired ? "1" : "0") + " down=" + (planeDown ? "1" : "0") + ")");
            }

            // 3) SS2SNAP2 airReady persistence & SS2SNAP1 backwards compatibility.
            // Replenish bomber count via snapshot edit test: set 1 ready left -> save -> consume? Wait: 0 left vs SS2SNAP1.
            // Let us verify current remaining counts (0,0,0) and custom synthetic SS2SNAP2 airReady round-trip!
            auto snap4e = sim4e.takeSnapshot();
            snap4e.airReady[0][0] = 2;
            const auto bytes4e = ss2::TutorialUnits::serializeSnapshot(snap4e);
            const auto decoded4e = ss2::TutorialUnits::deserializeSnapshot(bytes4e);
            if (!decoded4e || decoded4e->airReady[0][0] != 2 || !(*decoded4e == snap4e)) {
                throw std::runtime_error("SS2SNAP3 airReady serialize/deserialize mismatch");
            }
            if (!sim4e.loadSnapshot(*decoded4e) || sim4e.playerAirSupport()[0].ready != 2) {
                throw std::runtime_error("loadSnapshot did not restore SS2SNAP3 airReady count");
            }
            const std::size_t v3Ext = snap4e.soldiers.size() * 4u + snap4e.vehicles.size() * 5u;
            if (bytes4e.size() < v3Ext + 88u) throw std::runtime_error("Unexpected SS2SNAP3 size");
            std::vector<std::uint8_t> v2Bytes(bytes4e.begin(), bytes4e.end() - static_cast<std::ptrdiff_t>(v3Ext));
            v2Bytes[7] = '2'; // SS2SNAP2
            const auto v2Decoded = ss2::TutorialUnits::deserializeSnapshot(v2Bytes);
            if (!v2Decoded || v2Decoded->airReady[0][0] != 2) {
                throw std::runtime_error("Legacy SS2SNAP2 snapshot compatibility failed");
            }
            std::vector<std::uint8_t> legacyBytes(v2Bytes.begin(), v2Bytes.end() - 80u);
            legacyBytes[7] = '1'; // SS2SNAP1
            const auto legacyDecoded = ss2::TutorialUnits::deserializeSnapshot(legacyBytes);
            if (!legacyDecoded || legacyDecoded->airReady[0][0] != 0) {
                throw std::runtime_error("Legacy SS2SNAP1 snapshot compatibility failed");
            }

            std::cout << "Phase 4E air support, AA flak, tactical AI counter-attack & SS2SNAP2 PASSED\n";

            // Phase 4F: Active-crew OP(3) bitmask counting + OP(17) next-mission signal (never flipping owner=1 team).
            const auto [capX, capY] = tc(50, 60);
            const auto [veh4fX, veh4fY] = tc(51, 60);
            const auto [en4fX, en4fY] = tc(54, 60);
            std::vector<ss2::assets::Infantry> inf4f{
                {"dguner", capX, capY, 100, 40, 1, 0},
                {"dguner", en4fX, en4fY, 100, 40, 2, 1},
            };
            std::vector<ss2::assets::Placement> veh4f{
                {"kubel", veh4fX, veh4fY, 0, 100, 100, 98, 3},
            };
            ss2::TutorialUnits sim4f(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                     inf4f, veh4f, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4e);
            if (sim4f.countAliveByGroup(98) != 0u || sim4f.countByGroupMask(98, 1u) != 0u ||
                sim4f.countByGroupMask(98, 2u) != 1u) {
                throw std::runtime_error("Phase 4F initial unmanned owner=3 vehicle count mismatch");
            }
            if (sim4f.orderCaptureVehicle(std::array<size_t, 1>{0}, 0) != 1u) {
                throw std::runtime_error("Phase 4F orderCaptureVehicle failed");
            }
            for (int t = 0; t < 40 && sim4f.countAliveByGroup(98) == 0u; ++t) {
                (void)sim4f.step();
            }
            if (sim4f.countAliveByGroup(98) != 1u || sim4f.countByGroupMask(98, 1u) != 1u ||
                sim4f.countByGroupMask(98, 2u) != 0u) {
                throw std::runtime_error("Phase 4F captured vehicle active-crew count mismatch");
            }

            ss2::assets::TutorialData mrData;
            mrData.width = 128;
            mrData.height = 128;
            mrData.flags = flags;
            mrData.zones.assign(128u * 128u, 0u);
            mrData.scripts.resize(1);
            mrData.scripts[0] = {0x80000100u, 17u, 11u, 0x7fffffffu};
            ss2::TutorialUnits mrUnits(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                       inf4f, veh4f, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4e);
            ss2::MissionRunner mr(std::move(mrData), 50u, std::move(mrUnits));
            const auto sig = mr.nextMissionSignal();
            if (!sig || sig->first != 1u || sig->second != 0u) {
                throw std::runtime_error("Phase 4F OP(17)(0x100) nextMissionSignal mismatch");
            }
            if (!mr.isValidAttackTarget(1) || mr.countByGroupMask(98, 2u) != 1u || mr.countByGroupMask(98, 1u) != 0u) {
                throw std::runtime_error("Phase 4F OP(17)(0x100) altered hostility or group 98 active count");
            }
            std::cout << "Phase 4F active-crew OP(3) masks & OP(17) next-mission signal PASSED\n";

            // Phase 4G: Rocket salvo burst + scatter, player Patrol (infantry + vehicle), & bit3 pending reinforcements.
            ss2::assets::UnitSpec bmSpec{};
            bmSpec.id = "bm-31";
            bmSpec.isVehicle = true;
            bmSpec.roleClass = "mortar";
            bmSpec.selType = "mobmortar";
            bmSpec.health = 100;
            bmSpec.ammo1 = 12;
            bmSpec.moveSpeed = 1.7;
            bmSpec.sight = 10;
            bmSpec.shot1 = {"katyusha", ss2::assets::HitType::Explosive, 350, 350, 750, 750, 100, 5, 4, 80, "rocket"};

            auto cat4g = ss2::assets::UnitCatalog::fromSpecs({bmSpec, pTankSpec});
            const auto [bmX, bmY] = tc(50, 60);
            const auto [bmTgtX, bmTgtY] = tc(60, 60);
            const auto [patInfX, patInfY] = tc(50, 62);
            const auto [patInfDstX, patInfDstY] = tc(54, 62);
            const auto [patVehX, patVehY] = tc(52, 65);
            const auto [patVehDstX, patVehDstY] = tc(56, 65);
            std::vector<ss2::assets::Infantry> inf4g{
                {"dguner", patInfX, patInfY, 100, 40, 1, 0},
            };
            std::vector<ss2::assets::Placement> veh4g{
                {"bm-31", bmX, bmY, 0, 100, 100, 2, 0},
                {"tiger", patVehX, patVehY, 0, 100, 100, 3, 0},
            };
            ss2::TutorialUnits sim4g(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                     inf4g, veh4g, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4g);
            if (sim4g.orderAttackGroundVehicles(std::array<size_t, 1>{0}, bmTgtX, bmTgtY) != 1u ||
                sim4g.orderPatrol(std::array<size_t, 1>{0}, patInfDstX, patInfDstY) != 1u ||
                sim4g.patrolVehicles(std::array<size_t, 1>{1}, patVehDstX, patVehDstY) != 1u) {
                throw std::runtime_error("Phase 4G failed to issue rocket attack-ground or patrol orders");
            }
            int rocketShotsEarly = 0;
            int rocketShotsPause = 0;
            int maxInfX = patInfX, minInfAfterPeak = patInfDstX;
            int maxVehX = patVehX, minVehAfterPeak = patVehDstX;
            bool infReachedFar = false, vehReachedFar = false;
            for (int t = 0; t < 460; ++t) {
                for (const auto& sp : sim4g.step()) {
                    if (sp.index == 0) {
                        maxInfX = std::max(maxInfX, sp.x);
                        if (sp.x >= patInfX + 90) infReachedFar = true;
                        if (infReachedFar) minInfAfterPeak = std::min(minInfAfterPeak, sp.x);
                    }
                }
                for (const auto& vp : sim4g.takeVehiclePoses()) {
                    if (vp.index == 0 && vp.fired && vp.firedRocket) {
                        if (t < 28) ++rocketShotsEarly;
                        else if (t < 65) ++rocketShotsPause;
                    }
                    if (vp.index == 1) {
                        maxVehX = std::max(maxVehX, vp.x);
                        if (vp.x >= patVehX + 90) vehReachedFar = true;
                        if (vehReachedFar) minVehAfterPeak = std::min(minVehAfterPeak, vp.x);
                    }
                }
            }
            if (rocketShotsEarly < 4 || rocketShotsPause != 0) {
                throw std::runtime_error(
                    std::string("Phase 4G rocket salvo cadence failed (early=") + std::to_string(rocketShotsEarly) +
                    " pause=" + std::to_string(rocketShotsPause) + ")");
            }
            if (!infReachedFar || minInfAfterPeak > maxInfX - 40 || !vehReachedFar || minVehAfterPeak > maxVehX - 40) {
                throw std::runtime_error(
                    std::string("Phase 4G patrol turnaround failed (infFar=") + (infReachedFar ? "1" : "0") +
                    " maxInfX=" + std::to_string(maxInfX) + " minInfAfter=" + std::to_string(minInfAfterPeak) +
                    " vehFar=" + (vehReachedFar ? "1" : "0") + " maxVehX=" + std::to_string(maxVehX) +
                    " minVehAfter=" + std::to_string(minVehAfterPeak) + ")");
            }

            // Bit 3 (8) pending I-SUPPORT reinforcements in MissionRunner before and after OP(40).
            ss2::assets::TutorialData mr4gData;
            mr4gData.width = 128;
            mr4gData.height = 128;
            mr4gData.flags = flags;
            mr4gData.zones.assign(128u * 128u, 0u);
            mr4gData.reinforcements[0] = {
                {"dguner", 77, 100, 100, 0},
                {"dguner", 77, 100, 100, 0},
                {"dguner", 77, 100, 100, 0},
                {"tiger", 77, 100, 100, 0},
            };
            mr4gData.scripts.resize(2);
            mr4gData.scripts[0] = {0u | 0x80000000u, 1u | 0x80000000u, 12u, 11u, 0x7fffffffu};
            mr4gData.scripts[1] = {
                0u | 0x80000000u, 27u, 7u,
                0u | 0x80000000u, 0u | 0x80000000u, 0u | 0x80000000u, 0u | 0x80000000u, 0u | 0x80000000u,
                40u, 11u, 0x7fffffffu
            };
            ss2::TutorialUnits mr4gUnits(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                         {}, {}, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4g);
            ss2::MissionRunner mr4g(std::move(mr4gData), 8u, std::move(mr4gUnits));
            if (mr4g.countByGroupMask(77u, 8u) != 4u || mr4g.countByPlayerMask(0, 8u) != 4u) {
                throw std::runtime_error("Phase 4G bit3 pre-spawn pending reinforcement count mismatch");
            }
            bool gotWave = false;
            const auto waitStart = std::chrono::steady_clock::now();
            while (!gotWave && std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(2)) {
                for (const auto& ev : mr4g.poll()) {
                    if (ev.kind == Event::Reinforcement && ev.index == 0u) gotWave = true;
                }
                if (!gotWave) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!gotWave || mr4g.countByGroupMask(77u, 8u) != 0u || mr4g.countByPlayerMask(0, 8u) != 0u) {
                throw std::runtime_error("Phase 4G bit3 post-spawn reinforcement count did not clear to 0");
            }
            std::cout << "Phase 4G portraits, rocket salvos, player patrol & bit3 pending reinforcements PASSED\n";

            // Phase 4H: Flamethrower tank secondary fmw + incendiary cone AoE, & Sniper camouflage stealth.
            ss2::assets::UnitSpec ot34Spec{};
            ot34Spec.id = "ot-34";
            ot34Spec.isVehicle = true;
            ot34Spec.roleClass = "tank";
            ot34Spec.health = 650;
            ot34Spec.ammo1 = 40;
            ot34Spec.ammo2 = 50;
            ot34Spec.moveSpeed = 1.6;
            ot34Spec.sight = 14;
            ot34Spec.shot1 = {"pierce76", ss2::assets::HitType::Pierce, 190, 210, 600, 600, 0, 40, 1, 40, "gunshot"};
            ot34Spec.shot2 = {"flamethrower", ss2::assets::HitType::Fire, 150, 165, 160, 165, 0, 12, 1, 12, "fmw"};

            ss2::assets::UnitSpec sniperSpec{};
            sniperSpec.id = "sniper";
            sniperSpec.selType = "sniper";
            sniperSpec.health = 100;
            sniperSpec.ammo1 = 40;
            sniperSpec.sight = 14;
            sniperSpec.shot1 = {"professional", ss2::assets::HitType::Sniper, 120, 130, 420, 420, 0, 30, 1, 30, "rifle1"};

            auto cat4h = ss2::assets::UnitCatalog::fromSpecs({ot34Spec, sniperSpec});
            const auto [otX, otY] = tc(50, 60);
            const auto [fTgt1X, fTgt1Y] = tc(51, 59);
            const auto [fTgt2X, fTgt2Y] = tc(52, 59); // ~36px from fTgt1, <= 44px flame AoE & <= 140px from ot-34
            const auto [garX, garY] = tc(50, 57);
            const auto [garEnX, garEnY] = tc(51, 57);
            const auto [snpX, snpY] = tc(50, 80);     // player sniper
            const auto [detX, detY] = tc(53, 77);     // 192px east: inside tile sight (<384px) but > 140px camo reveal
            const auto [closeDetX, closeDetY] = tc(51, 79); // 64px east: inside 140px close detection
            std::vector<ss2::assets::Infantry> inf4h{
                {"dguner", fTgt1X, fTgt1Y, 200, 40, 10, 1}, // 0: primary flame target
                {"dguner", fTgt2X, fTgt2Y, 200, 40, 10, 1}, // 1: adjacent flame splash target
                {"sniper", snpX, snpY, 100, 40, 2, 0},      // 2: player sniper
                {"dguner", detX, detY, 200, 40, 11, 1},     // 3: enemy detector at 192px
                {"dguner", garEnX, garEnY, 120, 40, 12, 1}, // 4: house occupant
            };
            std::vector<ss2::assets::Placement> veh4h{
                {"ot-34", otX, otY, 0, 100, 100, 1, 0},
            };
            ss2::FogGrid fog4h(128, 128, flags);
            ss2::TutorialUnits sim4h(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                     inf4h, veh4h, std::move(fog4h), 2, false, {}, cat4h);
            ss2::assets::BuildingPlacement house4h{};
            house4h.x = garX; house4h.y = garY;
            house4h.left = garX - 24; house4h.top = garY - 24; house4h.width = 48; house4h.height = 48;
            house4h.capacity = 4; house4h.maxHp = 400;
            sim4h.setBuildings({house4h});
            sim4h.revealFogAt(snpX, snpY, 12, 1);

            // Crouched sniper starts hidden at 192px despite enemy fog visibility on tile.
            if (!sim4h.toggleCrouch(std::array<size_t, 1>{2}) ||
                !sim4h.isSoldierCamouflaged(2) || sim4h.isValidAttackTarget(2, 1)) {
                throw std::runtime_error("Phase 4H crouched sniper failed to stay camouflaged at 192px");
            }
            // Test ot-34 flame attack against infantry cluster + garrisoned building.
            sim4h.revealFogAt(otX, otY, 12, 0);
            if (sim4h.attackWithVehicles(std::array<size_t, 1>{0}, false, 0) != 1u) {
                throw std::runtime_error("Phase 4H ot-34 attackWithVehicles failed");
            }
            bool flameShotSeen = false;
            int adjHpAfterFlame = 200;
            for (int t = 0; t < 12; ++t) {
                for (const auto& sp : sim4h.step()) {
                    if (sp.index == 1) adjHpAfterFlame = sp.hp;
                }
                for (const auto& vp : sim4h.takeVehiclePoses()) {
                    if (vp.index == 0 && vp.fired && vp.firedFlamer) flameShotSeen = true;
                }
                if (flameShotSeen && adjHpAfterFlame < 200) break;
            }
            if (!flameShotSeen || adjHpAfterFlame >= 200) {
                throw std::runtime_error("Phase 4H ot-34 flamethrower or incendiary cone AoE failed");
            }
            // Burn hostile garrisoned house with ot-34 secondary flame (Fire hitType).
            {
                auto garSnap = sim4h.takeSnapshot();
                garSnap.buildings[0].owner = 1;
                garSnap.buildings[0].occupants = {4};
                garSnap.soldiers[4].garrisonBuilding = 0;
                if (!sim4h.loadSnapshot(garSnap)) {
                    throw std::runtime_error("Phase 4H failed to load garrisoned house snapshot");
                }
                if (sim4h.attackBuildingWithVehicles(std::array<size_t, 1>{0}, 0) != 1u) {
                    throw std::runtime_error("Phase 4H ot-34 attackBuildingWithVehicles failed");
                }
                bool bldFlamed = false;
                bool bldDamaged = false;
                for (int t = 0; t < 30; ++t) {
                    (void)sim4h.step();
                    for (const auto& vp : sim4h.takeVehiclePoses()) {
                        if (vp.index == 0 && vp.fired && vp.firedFlamer) bldFlamed = true;
                    }
                    for (const auto& bu : sim4h.takeBuildingUpdates()) {
                        if (bu.index == 0 && bu.damagedThisTick) bldDamaged = true;
                    }
                    if (bldFlamed && bldDamaged) break;
                }
                if (!bldFlamed || !bldDamaged) {
                    throw std::runtime_error("Phase 4H ot-34 failed to scorch hostile garrisoned building");
                }
            }
            // Firing breaks sniper camo; after 35 ticks without firing or close enemies, camo restores.
            sim4h.revealFogAt(detX, detY, 14, 0);
            if (!sim4h.isValidAttackTarget(3, 0)) {
                throw std::runtime_error("Phase 4H setup: enemy detector not visible to player");
            }
            if (sim4h.orderAttack(std::array<size_t, 1>{2}, 3) != 1u) {
                throw std::runtime_error("Phase 4H sniper orderAttack failed");
            }
            bool shotBrokeCamo = false;
            for (int t = 0; t < 15; ++t) {
                for (const auto& sp : sim4h.step()) {
                    if (sp.index == 2 && sp.fired) {
                        shotBrokeCamo = !sim4h.isSoldierCamouflaged(2) && !sp.camouflaged && sim4h.isValidAttackTarget(2, 1);
                    }
                }
                if (shotBrokeCamo) break;
            }
            if (!shotBrokeCamo) {
                throw std::runtime_error("Phase 4H sniper shot did not break camouflage");
            }
            (void)sim4h.toggleHoldFire(std::array<size_t, 1>{2});
            (void)sim4h.stopSelection(std::array<size_t, 1>{2});
            for (int t = 0; t < 40; ++t) (void)sim4h.step();
            if (!sim4h.isSoldierCamouflaged(2) || sim4h.isValidAttackTarget(2, 1)) {
                throw std::runtime_error("Phase 4H sniper did not re-enter camouflage 35 ticks after firing");
            }
            // Close-proximity enemy (<140px) exposes even crouched, hold-fire sniper.
            const size_t closeDet = sim4h.spawnInfantry({"dguner", closeDetX, closeDetY, 100, 40, 11, 1});
            (void)closeDet;
            (void)sim4h.step();
            if (!sim4h.isValidAttackTarget(2, 1)) {
                throw std::runtime_error("Phase 4H close enemy (<140px) failed to spot camouflaged sniper");
            }
            std::cout << "Phase 4H flamethrower tanks, incendiary AoE & sniper camouflage PASSED\n";

            // Phase 4I: Tactical order lines, vehicle crew abandon (K), bit2 off-map reserve pool & nested I-SUPPORT bit3.
            {
                std::vector<ss2::assets::Infantry> inf4i{
                    {"dguner", tc(40, 60).first, tc(40, 60).second, 100, 40, 5, 0},
                };
                std::vector<ss2::assets::Placement> veh4i{
                    {"tiger", tc(44, 60).first, tc(44, 60).second, 0, 100, 100, 5, 0},
                    {"t-34", tc(48, 60).first, tc(48, 60).second, 0, 100, 100, 6, 1},
                };
                ss2::TutorialUnits sim4i(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                         inf4i, veh4i, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4g);
                sim4i.revealFogAt(tc(48, 60).first, tc(48, 60).second, 10, 0);
                if (sim4i.orderSelection(std::array<size_t, 1>{0}, tc(42, 62).first, tc(42, 62).second) != 1u ||
                    sim4i.patrolVehicles(std::array<size_t, 1>{0}, tc(44, 63).first, tc(44, 63).second) != 1u) {
                    throw std::runtime_error("Phase 4I failed to issue order lines setup");
                }
                const auto olMovePat = sim4i.selectionOrderLines(std::array<size_t, 1>{0}, std::array<size_t, 1>{0});
                bool sawMove = false, sawPatrol = false;
                for (const auto& l : olMovePat) {
                    if (l.kind == ss2::TacticalOrderLine::Kind::Move) sawMove = true;
                    if (l.kind == ss2::TacticalOrderLine::Kind::Patrol) sawPatrol = true;
                }
                if (!sawMove || !sawPatrol) {
                    throw std::runtime_error("Phase 4I selectionOrderLines missing Move or Patrol");
                }
                if (sim4i.orderAttackMove(std::array<size_t, 1>{0}, tc(43, 62).first, tc(43, 62).second) != 1u ||
                    sim4i.orderAttackGroundVehicles(std::array<size_t, 1>{0}, tc(47, 60).first, tc(47, 60).second) != 1u) {
                    throw std::runtime_error("Phase 4I failed to issue AttackMove/AttackGround setup");
                }
                const auto olAmAg = sim4i.selectionOrderLines(std::array<size_t, 1>{0}, std::array<size_t, 1>{0});
                bool sawAm = false, sawAg = false;
                for (const auto& l : olAmAg) {
                    if (l.kind == ss2::TacticalOrderLine::Kind::AttackMove) sawAm = true;
                    if (l.kind == ss2::TacticalOrderLine::Kind::AttackGround) sawAg = true;
                }
                if (!sawAm || !sawAg) {
                    throw std::runtime_error("Phase 4I selectionOrderLines missing AttackMove or AttackGround");
                }

                // Abandon vehicle -> neutral unmanned (bit0 2->1, bit1 0->1), spawn crewman, re-capture -> crewed again!
                if (sim4i.countByGroupMask(5u, 1u) != 2u || sim4i.countByGroupMask(5u, 2u) != 0u) {
                    throw std::runtime_error("Phase 4I initial group-5 bit0/bit1 mismatch");
                }
                const auto abSpawns = sim4i.abandonVehicles(std::array<size_t, 1>{0});
                if (abSpawns.size() != 1u || abSpawns[0].owner != 0 ||
                    sim4i.countByGroupMask(5u, 1u) != 1u || sim4i.countByGroupMask(5u, 2u) != 1u) {
                    throw std::runtime_error("Phase 4I abandonVehicles bit0/bit1 transition mismatch");
                }
                const size_t crewIdx = sim4i.spawnInfantry(
                    {abSpawns[0].id, abSpawns[0].x, abSpawns[0].y, 100, 40, abSpawns[0].group, 0}, 0);
                if (sim4i.orderCaptureVehicle(std::array<size_t, 1>{crewIdx}, 0) != 1u) {
                    throw std::runtime_error("Phase 4I recapture after abandon failed");
                }
                for (int t = 0; t < 40 && sim4i.countByGroupMask(5u, 2u) != 0u; ++t) (void)sim4i.step();
                if (sim4i.countByGroupMask(5u, 1u) != 3u || sim4i.countByGroupMask(5u, 2u) != 0u) {
                    throw std::runtime_error("Phase 4I recaptured vehicle did not restore bit0 active crew");
                }

                // Off-map reserve bit2 (4) and combined bit0|bit2|bit3 (13).
                sim4i.addReserveUnit({"tiger", 0, 5, 100, 100, 0, 2});
                sim4i.addReserveUnit({"dguner", 0, 5, 100, 40, 0, 2});
                if (sim4i.countByGroupMask(5u, 4u) != 2u || sim4i.countByGroupMask(5u, 5u) != 5u ||
                    sim4i.countByPlayerMask(0, 4u) != 2u) {
                    throw std::runtime_error("Phase 4I bit2 reserve counting mismatch");
                }
                const auto deployed = sim4i.takeReserveUnits(5u, 2, 1);
                if (deployed.size() != 1u || sim4i.countByGroupMask(5u, 4u) != 1u) {
                    throw std::runtime_error("Phase 4I takeReserveUnits mismatch");
                }

                // Nested I-SUPPORT crew/rider bit3 (8) counting in MissionRunner (group 70 truck+crew, group 71 riders).
                ss2::assets::TutorialData mr4iData;
                mr4iData.width = 128; mr4iData.height = 128; mr4iData.flags = flags;
                mr4iData.zones.assign(128u * 128u, 0u);
                mr4iData.reinforcements[0] = {
                    {"maultier", 70, 100, 100, 0, {
                        {"dguner", 71, 100, 40, 0, false},
                        {"dsmguner", 71, 100, 40, 0, false},
                        {"dtankist", 70, 100, 40, 0, true},
                    }},
                };
                mr4iData.scripts.resize(2);
                mr4iData.scripts[0] = {0u | 0x80000000u, 1u | 0x80000000u, 12u, 11u, 0x7fffffffu};
                mr4iData.scripts[1] = {
                    0u | 0x80000000u, 27u, 7u,
                    0u | 0x80000000u, 0u | 0x80000000u, 0u | 0x80000000u, 0u | 0x80000000u, 0u | 0x80000000u,
                    40u, 11u, 0x7fffffffu
                };
                ss2::TutorialUnits mr4iUnits(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             {}, {}, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4g);
                ss2::MissionRunner mr4i(std::move(mr4iData), 8u, std::move(mr4iUnits));
                if (mr4i.countByGroupMask(70u, 8u) != 2u || mr4i.countByGroupMask(71u, 8u) != 2u ||
                    mr4i.countByPlayerMask(0, 8u) != 4u) {
                    throw std::runtime_error("Phase 4I nested I-SUPPORT bit3 pending count mismatch");
                }
                bool gotWave4i = false;
                const auto wait4i = std::chrono::steady_clock::now();
                while (!gotWave4i && std::chrono::steady_clock::now() - wait4i < std::chrono::seconds(2)) {
                    for (const auto& ev : mr4i.poll()) {
                        if (ev.kind == Event::Reinforcement && ev.index == 0u) gotWave4i = true;
                    }
                    if (!gotWave4i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!gotWave4i || mr4i.countByGroupMask(70u, 8u) != 0u || mr4i.countByGroupMask(71u, 8u) != 0u ||
                    mr4i.countByPlayerMask(0, 8u) != 0u) {
                    throw std::runtime_error("Phase 4I nested bit3 count did not clear after OP(40)");
                }
                std::cout << "Phase 4I reinforcement riders, tactical order lines, vehicle abandon & bit2 reserve PASSED\n";

                // Phase 4J: Infantry Emergency Scatter (D) disperses clustered soldiers outward onto walkable tiles.
                const auto [sc0x, sc0y] = tc(50, 60);
                const auto [sc1x, sc1y] = tc(50, 60);
                const auto [sc2x, sc2y] = tc(51, 60);
                const auto [sc3x, sc3y] = tc(50, 61);
                std::vector<ss2::assets::Infantry> inf4j{
                    {"dguner", sc0x, sc0y, 100, 40, 1, 0},
                    {"dguner", sc1x, sc1y, 100, 40, 1, 0},
                    {"dguner", sc2x, sc2y, 100, 40, 1, 0},
                    {"dguner", sc3x, sc3y, 100, 40, 1, 0},
                };
                ss2::TutorialUnits sim4j(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                         inf4j, {}, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4g);
                const std::array<size_t, 4> scIds{0, 1, 2, 3};
                std::array<std::pair<int, int>, 4> scPos{
                    std::pair{sc0x, sc0y}, std::pair{sc1x, sc1y}, std::pair{sc2x, sc2y}, std::pair{sc3x, sc3y}
                };
                const auto meanSpread = [&](const std::array<std::pair<int, int>, 4>& pts) -> double {
                    double mx = 0.0, my = 0.0;
                    for (const auto& [px, py] : pts) { mx += px; my += py; }
                    mx /= 4.0; my /= 4.0;
                    double sum = 0.0;
                    for (const auto& [px, py] : pts) sum += std::hypot(px - mx, py - my);
                    return sum / 4.0;
                };
                const double spreadBefore = meanSpread(scPos);
                if (sim4j.orderScatter(scIds) != 4u) {
                    throw std::runtime_error("Phase 4J orderScatter did not accept all 4 clustered infantry");
                }
                for (int t = 0; t < 140; ++t) {
                    for (const auto& p : sim4j.step()) {
                        if (p.index < 4u) {
                            if (!p.alive || p.hp <= 0) throw std::runtime_error("Phase 4J scattering soldier died unexpectedly");
                            scPos[p.index] = {p.x, p.y};
                        }
                    }
                }
                const double spreadAfter = meanSpread(scPos);
                if (spreadAfter <= spreadBefore + 25.0) {
                    throw std::runtime_error("Phase 4J orderScatter did not sufficiently disperse infantry (before=" +
                                             std::to_string(spreadBefore) + " after=" + std::to_string(spreadAfter) + ")");
                }
                std::cout << "Phase 4J infantry emergency scatter PASSED\n";

                // Phase 4K: Visible mines, safe Sapper AP disarm, supply truck building repair & ordnance resupply, idlePlayerUnits().
                {
                    ss2::assets::UnitSpec supSpec{};
                    supSpec.id = "sdkfz.11";
                    supSpec.isVehicle = true;
                    supSpec.selType = "truck";
                    supSpec.health = 100;
                    supSpec.moveSpeed = 1.5;
                    supSpec.ammoRegenDelay = 30;

                    auto cat4k = ss2::assets::UnitCatalog::fromSpecs({supSpec});
                    const auto [sapX, sapY] = tc(50, 60);
                    const auto [rifX, rifY] = tc(50, 62);
                    const auto [supX, supY] = tc(50, 61);
                    const auto [bldX, bldY] = tc(51, 61);
                    std::vector<ss2::assets::Infantry> inf4k{
                        {"dsapper", sapX, sapY, 100, 40, 1, 0},
                        {"dguner", rifX, rifY, 100, 40, 1, 0},
                    };
                    std::vector<ss2::assets::Placement> veh4k{
                        {"sdkfz.11", supX, supY, 0, 100, 0, 1, 0},
                    };
                    ss2::TutorialUnits sim4k(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4k, veh4k, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4k);
                    sim4k.setBuildings({
                        {bldX, bldY, bldX - 16, bldY - 16, 32, 32, 4, 400}
                    });
                    std::vector<std::uint8_t> mines4k(128u * 128u, 0u);
                    mines4k[60u * 128u + 48u] = 0x05u; // friendly owner-0 AT mine (visible immediately)
                    mines4k[60u * 128u + 53u] = 0x41u; // undetected hostile owner-1 AP mine
                    sim4k.setMines(mines4k);

                    auto vm0 = sim4k.visibleMines(0);
                    if (vm0.size() != 1u || !vm0[0].antiTank || vm0[0].hostile) {
                        throw std::runtime_error("Phase 4K initial visibleMines expected 1 friendly AT mine");
                    }
                    std::vector<ss2::SoldierPose> layPoses;
                    if (sim4k.orderLayMine(std::array<size_t, 1>{0}, &layPoses) != 1u ||
                        layPoses.empty() || layPoses[0].mineAmmo != 4 || sim4k.visibleMines(0).size() != 2u) {
                        throw std::runtime_error("Phase 4K orderLayMine pose/visibleMines mismatch");
                    }
                    for (int t = 0; t < 20; ++t) (void)sim4k.step();
                    auto vmDetected = sim4k.visibleMines(0);
                    int hostileCount = 0;
                    for (const auto& m : vmDetected) {
                        if (m.hostile) ++hostileCount;
                    }
                    if (vmDetected.size() != 3u || hostileCount != 1) {
                        throw std::runtime_error("Phase 4K Sapper did not detect hostile AP mine into visibleMines");
                    }
                    const auto [apX, apY] = tc(53, 60);
                    if (!sim4k.order(0, apX, apY)) {
                        throw std::runtime_error("Phase 4K order Sapper onto detected AP mine failed");
                    }
                    bool sapDisarmed = false;
                    for (int t = 0; t < 160; ++t) {
                        (void)sim4k.step();
                        for (const auto& me : sim4k.takeMineEvents()) {
                            if (me.kind == ss2::MineEvent::Disarmed && !me.antiTank) sapDisarmed = true;
                        }
                    }
                    auto snapMid = sim4k.takeSnapshot();
                    if (!sapDisarmed || snapMid.soldiers[0].hp != 100 || sim4k.visibleMines(0).size() != 2u) {
                        throw std::runtime_error("Phase 4K safe Sapper disarm of detected AP mine failed");
                    }

                    // Damage building & set player owner, then step supply pulses for building repair + mine resupply.
                    snapMid.buildings[0].hp = 200;
                    snapMid.buildings[0].owner = 0;
                    if (!sim4k.loadSnapshot(snapMid)) {
                        throw std::runtime_error("Phase 4K loadSnapshot failed");
                    }
                    (void)sim4k.takeBuildingUpdates();
                    for (int t = 0; t < 65; ++t) (void)sim4k.step();
                    const auto snapAfter = sim4k.takeSnapshot();
                    if (snapAfter.buildings[0].hp <= 200 || sim4k.takeBuildingUpdates().empty()) {
                        throw std::runtime_error("Phase 4K supply truck did not repair damaged building");
                    }
                    if (snapAfter.soldiers[0].mineAmmo != 5) {
                        throw std::runtime_error("Phase 4K supply truck did not replenish Sapper mineAmmo back to 5");
                    }

                    // Move soldier 1 away and check idlePlayerUnits excludes the busy unit.
                    const auto [m1x, m1y] = tc(60, 62);
                    if (!sim4k.order(1, m1x, m1y)) {
                        throw std::runtime_error("Phase 4K busy order failed");
                    }
                    (void)sim4k.step();
                    const auto idle = sim4k.idlePlayerUnits();
                    bool hasInf0 = false, hasInf1 = false, hasVeh0 = false;
                    for (const auto& iu : idle) {
                        if (!iu.isVehicle && iu.index == 0) hasInf0 = true;
                        if (!iu.isVehicle && iu.index == 1) hasInf1 = true;
                        if (iu.isVehicle && iu.index == 0) hasVeh0 = true;
                    }
                    if (!hasInf0 || hasInf1 || !hasVeh0) {
                        throw std::runtime_error("Phase 4K idlePlayerUnits filter mismatch");
                    }
                    std::cout << "Phase 4K visible mines, safe sapper disarm, supply building repair/resupply & idle cycling PASSED\n";
                }

                // Phase 4L: Heavy tank infantry crushing, garrisoned medic aid station & weapon range previews.
                {
                    ss2::assets::UnitSpec crushTank{};
                    crushTank.id = "tiger";
                    crushTank.isVehicle = true;
                    crushTank.roleClass = "tank";
                    crushTank.selType = "tank";
                    crushTank.health = 450;
                    crushTank.ammo1 = 0;
                    crushTank.moveSpeed = 1.6;
                    crushTank.sight = 12;

                    ss2::assets::UnitSpec softCar{};
                    softCar.id = "kubel";
                    softCar.isVehicle = true;
                    softCar.selType = "car";
                    softCar.health = 90;
                    softCar.ammo1 = 0;
                    softCar.moveSpeed = 1.8;
                    softCar.sight = 10;

                    ss2::assets::UnitSpec previewArty{};
                    previewArty.id = "bm-31";
                    previewArty.isVehicle = true;
                    previewArty.roleClass = "mortar";
                    previewArty.selType = "mobmortar";
                    previewArty.health = 100;
                    previewArty.ammo1 = 12;
                    previewArty.moveSpeed = 1.7;
                    previewArty.sight = 10;
                    previewArty.shot1 = {"katyusha", ss2::assets::HitType::Explosive, 350, 350, 750, 750, 100, 5, 4, 80, "rocket"};

                    auto cat4l = ss2::assets::UnitCatalog::fromSpecs({crushTank, softCar, previewArty});
                    const auto [tX, tY] = tc(50, 60);
                    const auto [tTgtX, tTgtY] = tc(54, 60);
                    const auto [e0X, e0Y] = tc(52, 60);
                    const auto [cX, cY] = tc(50, 64);
                    const auto [cTgtX, cTgtY] = tc(54, 64);
                    const auto [e1X, e1Y] = tc(52, 64);
                    const auto [garX, garY] = tc(60, 60);
                    const auto [medX, medY] = tc(60, 61);
                    const auto [patX, patY] = tc(61, 61);
                    const auto [artX, artY] = tc(46, 60);

                    std::vector<ss2::assets::Infantry> inf4l{
                        {"rguner", e0X, e0Y, 100, 0, 5, 1},   // 0: in Tiger's crush path
                        {"rguner", e1X, e1Y, 100, 0, 5, 1},   // 1: in Kubel's non-crush path
                        {"cmedic", medX, medY, 100, 0, 1, 0}, // 2: player medic to garrison
                        {"dguner", patX, patY, 100, 0, 1, 0}, // 3: player patient to garrison
                    };
                    std::vector<ss2::assets::Placement> veh4l{
                        {"tiger", tX, tY, 0, 100, 0, 1, 0},
                        {"kubel", cX, cY, 0, 100, 0, 1, 0},
                        {"bm-31", artX, artY, 0, 100, 12, 1, 0},
                    };
                    ss2::TutorialUnits sim4l(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4l, veh4l, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4l);
                    sim4l.setBuildings({
                        {garX, garY, garX - 16, garY - 16, 32, 32, 6, 400}
                    });

                    // Selection range previews should return the armed artillery with range + deadzone, skipping ammo=0 cars.
                    const auto rp = sim4l.selectionRangePreviews(std::array<size_t, 3>{0, 1, 2});
                    if (rp.size() != 1u || rp[0].rangePx <= 140.0f || rp[0].deadzonePx <= 20.0f) {
                        throw std::runtime_error("Phase 4L selectionRangePreviews mismatch");
                    }

                    // Order Tiger and Kubel through their respective enemy infantry targets (both ammo=0) + garrison medic & patient.
                    if (sim4l.moveVehicles(std::array<size_t, 1>{0}, tTgtX, tTgtY) != 1u ||
                        sim4l.moveVehicles(std::array<size_t, 1>{1}, cTgtX, cTgtY) != 1u ||
                        sim4l.orderGarrisonBuilding(std::array<size_t, 2>{2, 3}, 0) != 2u) {
                        throw std::runtime_error("Phase 4L setup orders failed");
                    }
                    for (int t = 0; t < 90; ++t) (void)sim4l.step();
                    auto snap4l = sim4l.takeSnapshot();
                    if (snap4l.soldiers[0].hp != 0 || snap4l.vehicles[0].kills < 1 || snap4l.vehicles[0].expa < 1500) {
                        throw std::runtime_error("Phase 4L heavy tank did not crush enemy infantry or award kill/expa");
                    }
                    if (snap4l.soldiers[1].hp <= 0) {
                        throw std::runtime_error("Phase 4L soft car unexpectedly crushed enemy infantry");
                    }
                    if (snap4l.soldiers[2].garrisonBuilding != 0 || snap4l.soldiers[3].garrisonBuilding != 0) {
                        throw std::runtime_error("Phase 4L medic and patient did not garrison building");
                    }
                    snap4l.soldiers[3].hp = 40;
                    if (!sim4l.loadSnapshot(snap4l)) {
                        throw std::runtime_error("Phase 4L loadSnapshot failed");
                    }
                    for (int t = 0; t < 60; ++t) (void)sim4l.step();
                    const auto snapHealed = sim4l.takeSnapshot();
                    if (snapHealed.soldiers[3].hp <= 40 || snapHealed.soldiers[3].garrisonBuilding != 0) {
                        throw std::runtime_error("Phase 4L garrisoned medic did not heal wounded garrisonmate");
                    }
                    std::cout << "Phase 4L tank infantry crush, garrison medic aid station & weapon range previews PASSED\n";
                }

                // Phase 4M: Gold difficulty (1=Easy, 2=Normal, 3=Hard) sight & vehicle damage scaling.
                {
                    const auto desc = ss2::InfantryCombatDesc::forUnitId("dguner");
                    const int playerSightEasy = ss2::effectiveSightRadius(desc, 0, false, 0, 1);
                    const int playerSightNorm = ss2::effectiveSightRadius(desc, 0, false, 0, 2);
                    const int enemySightNorm = ss2::effectiveSightRadius(desc, 0, false, 1, 2);
                    const int enemySightHard = ss2::effectiveSightRadius(desc, 0, false, 1, 3);
                    if (playerSightEasy <= playerSightNorm || enemySightHard <= enemySightNorm) {
                        throw std::runtime_error("Phase 4M effectiveSightRadius difficulty scaling mismatch");
                    }

                    ss2::assets::UnitSpec pArmor{};
                    pArmor.id = "panzer_tgt";
                    pArmor.isVehicle = true;
                    pArmor.health = 400;
                    pArmor.ammo1 = 0;
                    pArmor.sight = 12;
                    pArmor.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {20, 10, 5};

                    ss2::assets::UnitSpec eShooter{};
                    eShooter.id = "shooter_tk";
                    eShooter.isVehicle = true;
                    eShooter.health = 400;
                    eShooter.ammo1 = 20;
                    eShooter.sight = 14;
                    eShooter.shot1 = {"ap75", ss2::assets::HitType::Pierce, 100, 100, 450, 450, 0, 40, 1, 40, "tankgun"};

                    auto cat4m = ss2::assets::UnitCatalog::fromSpecs({pArmor, eShooter});
                    const auto runDifficultyDamage = [&](int diff) -> int {
                        const auto [ptX, ptY] = tc(50, 60);
                        const auto [etX, etY] = tc(54, 60);
                        std::vector<ss2::assets::Placement> veh4m{
                            {"panzer_tgt", ptX, ptY, 0, 100, 0, 1, 0},
                            {"shooter_tk", etX, etY, 0, 100, 20, 2, 1},
                        };
                        ss2::TutorialUnits sim4m(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                                 {}, veh4m, ss2::FogGrid(128, 128, flags), diff, true, {}, cat4m);
                        for (int t = 0; t < 25; ++t) (void)sim4m.step();
                        const auto snap = sim4m.takeSnapshot();
                        return 400 - snap.vehicles[0].hp;
                    };
                    const int dmgEasy = runDifficultyDamage(1);
                    const int dmgNorm = runDifficultyDamage(2);
                    const int dmgHard = runDifficultyDamage(3);
                    if (dmgEasy <= 0 || dmgEasy >= dmgNorm || dmgNorm >= dmgHard) {
                        throw std::runtime_error("Phase 4M vehicle damage difficulty scaling mismatch (easy=" +
                                                 std::to_string(dmgEasy) + " norm=" + std::to_string(dmgNorm) +
                                                 " hard=" + std::to_string(dmgHard) + ")");
                    }
                    std::cout << "Phase 4M Gold difficulty sight & vehicle damage scaling PASSED\n";
                }

                // Phase 4N: Tactical facing (T), smart logistics auto-approach & service, Support order lines.
                {
                    ss2::assets::UnitSpec tankSpec{};
                    tankSpec.id = "panzer_face";
                    tankSpec.isVehicle = true;
                    tankSpec.health = 400;
                    tankSpec.moveSpeed = 1.6;
                    tankSpec.sight = 14;
                    tankSpec.ammo1 = 20;
                    tankSpec.shot1 = {"ap75", ss2::assets::HitType::Pierce, 80, 80, 300, 300, 0, 40, 1, 40, "tankgun"};

                    ss2::assets::UnitSpec atGunSpec{};
                    atGunSpec.id = "pak_static";
                    atGunSpec.isVehicle = true;
                    atGunSpec.health = 180;
                    atGunSpec.moveSpeed = 0.0;
                    atGunSpec.sight = 14;
                    atGunSpec.ammo1 = 20;
                    atGunSpec.shot1 = {"ap50", ss2::assets::HitType::Pierce, 70, 70, 320, 320, 0, 35, 1, 35, "tankgun"};

                    ss2::assets::UnitSpec truckSpec{};
                    truckSpec.id = "supply_truck";
                    truckSpec.isVehicle = true;
                    truckSpec.health = 150;
                    truckSpec.moveSpeed = 2.0;
                    truckSpec.sight = 10;
                    truckSpec.ammoRegenDelay = 20;

                    auto cat4n = ss2::assets::UnitCatalog::fromSpecs({tankSpec, atGunSpec, truckSpec});

                    const auto [tkX, tkY] = tc(50, 60);
                    const auto [atX, atY] = tc(50, 64);
                    const auto [abX, abY] = tc(50, 68);
                    const auto [supX, supY] = tc(56, 60);
                    const auto [bldX, bldY] = tc(64, 60);
                    const auto [medX, medY] = tc(48, 70);
                    const auto [patX, patY] = tc(54, 70);
                    const auto [faceTgtX, faceTgtY] = tc(40, 60);

                    std::vector<ss2::assets::Infantry> inf4n{
                        {"dmedic", medX, medY, 100, 20, 1, 0},
                        {"drifle", patX, patY, 40, 40, 1, 0},
                    };
                    std::vector<ss2::assets::Placement> veh4n{
                        {"panzer_face", tkX, tkY, 0, 50, 100, 1, 0},  // idx 0: 200/400 HP
                        {"pak_static", atX, atY, 0, 100, 100, 1, 0},  // idx 1: stationary AT gun
                        {"panzer_face", abX, abY, 0, 100, 100, 1, 3}, // idx 2: abandoned neutral (uncrewed)
                        {"supply_truck", supX, supY, 0, 100, 0, 1, 0},// idx 3: player supply truck
                    };
                    ss2::TutorialUnits sim4n(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4n, veh4n, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4n);
                    sim4n.setBuildings({
                        {bldX, bldY, bldX - 20, bldY - 20, 40, 40, 4, 400}
                    });
                    auto initSnap4n = sim4n.takeSnapshot();
                    initSnap4n.soldiers[1].hp = 40;
                    initSnap4n.buildings[0].hp = 150;
                    if (!sim4n.loadSnapshot(initSnap4n)) {
                        throw std::runtime_error("Phase 4N initial loadSnapshot failed");
                    }

                    // 1) Tactical facing rotates both tank and stationary AT gun in place while rejecting owner=3.
                    const size_t faceAccepted = sim4n.faceVehicles(std::array<size_t, 3>{0, 1, 2}, faceTgtX, faceTgtY);
                    if (faceAccepted != 2u) {
                        throw std::runtime_error("Phase 4N expected 2 crewing vehicles accepted by faceVehicles, got " +
                                                 std::to_string(faceAccepted));
                    }
                    // Verify facingTargetDir makes turning units non-idle until rotation finishes.
                    const auto idleMidFace = sim4n.idlePlayerUnits();
                    for (const auto& iu : idleMidFace) {
                        if (iu.isVehicle && (iu.index == 0 || iu.index == 1)) {
                            throw std::runtime_error("Phase 4N vehicle marked idle while rotating front armor");
                        }
                    }
                    for (int t = 0; t < 20; ++t) (void)sim4n.step();
                    const auto afterFace = sim4n.takeSnapshot();
                    if (afterFace.vehicles[0].hullDir != 16u || afterFace.vehicles[0].turretDir != 16u ||
                        afterFace.vehicles[0].x != tkX || afterFace.vehicles[0].y != tkY) {
                        throw std::runtime_error("Phase 4N mobile tank facing mismatch");
                    }
                    if (afterFace.vehicles[1].hullDir != 18u || afterFace.vehicles[1].turretDir != 18u ||
                        afterFace.vehicles[1].x != atX || afterFace.vehicles[1].y != atY) {
                        throw std::runtime_error("Phase 4N stationary AT gun facing mismatch (hull=" +
                                                 std::to_string(afterFace.vehicles[1].hullDir) + " turret=" +
                                                 std::to_string(afterFace.vehicles[1].turretDir) + ")");
                    }
                    bool foundTankIdle = false, foundAtIdle = false;
                    for (const auto& iu : sim4n.idlePlayerUnits()) {
                        if (iu.isVehicle && iu.index == 0) foundTankIdle = true;
                        if (iu.isVehicle && iu.index == 1) foundAtIdle = true;
                    }
                    if (!foundTankIdle || !foundAtIdle) {
                        throw std::runtime_error("Phase 4N vehicles did not return to idle after completing face order");
                    }

                    // 2) Supply truck smart right-click support on damaged tank (~200px away).
                    if (!sim4n.canSupportVehicle(std::array<size_t, 1>{3}, 0)) {
                        throw std::runtime_error("Phase 4N expected canSupportVehicle=true for damaged tank");
                    }
                    if (sim4n.orderSupportVehicle(std::array<size_t, 1>{3}, 0) != 1u) {
                        throw std::runtime_error("Phase 4N orderSupportVehicle failed");
                    }
                    const auto vehSupportLines = sim4n.selectionOrderLines({}, std::array<size_t, 1>{3});
                    bool hasVehSupportLine = false;
                    for (const auto& ol : vehSupportLines) {
                        if (ol.kind == ss2::TacticalOrderLine::Kind::Support) hasVehSupportLine = true;
                    }
                    if (!hasVehSupportLine) {
                        throw std::runtime_error("Phase 4N missing TacticalOrderLine::Kind::Support for supply truck");
                    }
                    for (int t = 0; t < 80; ++t) (void)sim4n.step();
                    const auto afterVehRepair = sim4n.takeSnapshot();
                    if (afterVehRepair.vehicles[0].hp <= 200) {
                        throw std::runtime_error("Phase 4N supply truck did not approach and repair damaged tank (hp=" +
                                                 std::to_string(afterVehRepair.vehicles[0].hp) + ")");
                    }

                    // 3) Supply truck smart right-click repair on damaged building.
                    if (!sim4n.canSupportBuilding(std::array<size_t, 1>{3}, 0)) {
                        throw std::runtime_error("Phase 4N expected canSupportBuilding=true");
                    }
                    if (sim4n.orderSupportBuilding(std::array<size_t, 1>{3}, 0) != 1u) {
                        throw std::runtime_error("Phase 4N orderSupportBuilding failed");
                    }
                    for (int t = 0; t < 120; ++t) (void)sim4n.step();
                    const auto afterBldRepair = sim4n.takeSnapshot();
                    if (afterBldRepair.buildings[0].hp <= 150) {
                        throw std::runtime_error("Phase 4N supply truck did not approach and repair damaged building (hp=" +
                                                 std::to_string(afterBldRepair.buildings[0].hp) + ")");
                    }

                    // 4) Medic smart right-click heal on wounded infantryman (~190 world units away).
                    if (!sim4n.canHealSoldier(std::array<size_t, 1>{0}, {}, 1)) {
                        throw std::runtime_error("Phase 4N expected canHealSoldier=true");
                    }
                    if (sim4n.orderHealSoldier(std::array<size_t, 1>{0}, {}, 1) != 1u) {
                        throw std::runtime_error("Phase 4N orderHealSoldier failed");
                    }
                    const auto medSupportLines = sim4n.selectionOrderLines(std::array<size_t, 1>{0}, {});
                    bool hasMedSupportLine = false;
                    for (const auto& ol : medSupportLines) {
                        if (ol.kind == ss2::TacticalOrderLine::Kind::Support) hasMedSupportLine = true;
                    }
                    if (!hasMedSupportLine) {
                        throw std::runtime_error("Phase 4N missing TacticalOrderLine::Kind::Support for medic");
                    }
                    for (int t = 0; t < 320; ++t) (void)sim4n.step();
                    const auto afterHeal = sim4n.takeSnapshot();
                    if (afterHeal.soldiers[1].hp <= 40) {
                        throw std::runtime_error("Phase 4N medic did not approach and heal wounded soldier (hp=" +
                                                 std::to_string(afterHeal.soldiers[1].hp) + ")");
                    }
                    std::cout << "Phase 4N tactical facing, smart logistics dispatch & support order lines PASSED\n";
                }

                // Phase 4O: Multi-vehicle battle-line formations & single-vehicle exact destination.
                {
                    ss2::assets::UnitSpec formSpec{};
                    formSpec.id = "panzer_form";
                    formSpec.isVehicle = true;
                    formSpec.health = 300;
                    formSpec.moveSpeed = 1.8;
                    formSpec.sight = 14;
                    formSpec.ammo1 = 20;
                    auto cat4o = ss2::assets::UnitCatalog::fromSpecs({formSpec});

                    const auto [p0X, p0Y] = tc(48, 60);
                    const auto [p1X, p1Y] = tc(48, 62);
                    const auto [p2X, p2Y] = tc(50, 60);
                    const auto [p3X, p3Y] = tc(50, 62);
                    const auto [e0X, e0Y] = tc(52, 70);
                    const auto [e1X, e1Y] = tc(52, 72);
                    const auto [e2X, e2Y] = tc(54, 70);
                    const auto [destX, destY] = tc(56, 60);
                    const auto [singleX, singleY] = tc(56, 64);
                    const auto [enemyDestX, enemyDestY] = tc(60, 70);

                    std::vector<ss2::assets::Placement> veh4o{
                        {"panzer_form", p0X, p0Y, 0, 100, 100, 1, 0},
                        {"panzer_form", p1X, p1Y, 0, 100, 100, 1, 0},
                        {"panzer_form", p2X, p2Y, 0, 100, 100, 1, 0},
                        {"panzer_form", p3X, p3Y, 0, 100, 100, 1, 0},
                        {"panzer_form", e0X, e0Y, 0, 100, 100, 7, 1},
                        {"panzer_form", e1X, e1Y, 0, 100, 100, 7, 1},
                        {"panzer_form", e2X, e2Y, 0, 100, 100, 7, 1},
                    };
                    ss2::TutorialUnits sim4o(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             {}, veh4o, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4o);

                    if (sim4o.moveVehicles(std::array<size_t, 4>{0, 1, 2, 3}, destX, destY) != 4u) {
                        throw std::runtime_error("Phase 4O moveVehicles expected 4 vehicles accepted");
                    }
                    for (int t = 0; t < 90; ++t) (void)sim4o.step();
                    const auto snap4o = sim4o.takeSnapshot();
                    for (size_t a = 0; a < 4u; ++a) {
                        for (size_t b = a + 1u; b < 4u; ++b) {
                            const double dist = std::hypot(double(snap4o.vehicles[a].x - snap4o.vehicles[b].x),
                                                           double(snap4o.vehicles[a].y - snap4o.vehicles[b].y));
                            if (dist < 36.0) {
                                throw std::runtime_error("Phase 4O player vehicles overlapped in formation (dist=" +
                                                         std::to_string(dist) + ")");
                            }
                        }
                    }

                    // Single-vehicle move preserves exact destination compatibility.
                    if (sim4o.moveVehicles(std::array<size_t, 1>{0}, singleX, singleY) != 1u) {
                        throw std::runtime_error("Phase 4O single-vehicle move failed");
                    }
                    for (int t = 0; t < 75; ++t) (void)sim4o.step();
                    const auto snapSingle = sim4o.takeSnapshot();
                    const double singleErr = std::hypot(double(snapSingle.vehicles[0].x - singleX),
                                                        double(snapSingle.vehicles[0].y - singleY));
                    if (singleErr > 10.0) {
                        throw std::runtime_error("Phase 4O single vehicle missed exact destination (err=" +
                                                 std::to_string(singleErr) + ")");
                    }

                    // Enemy/scripted group vehicle move also fans into spread battle-line formation.
                    if (sim4o.orderGroupMove(7u, enemyDestX, enemyDestY, true) != 3u) {
                        throw std::runtime_error("Phase 4O orderGroupMove expected 3 enemy vehicles accepted");
                    }
                    for (int t = 0; t < 90; ++t) (void)sim4o.step();
                    const auto snapGrp = sim4o.takeSnapshot();
                    for (size_t a = 4u; a < 7u; ++a) {
                        for (size_t b = a + 1u; b < 7u; ++b) {
                            const double dist = std::hypot(double(snapGrp.vehicles[a].x - snapGrp.vehicles[b].x),
                                                           double(snapGrp.vehicles[a].y - snapGrp.vehicles[b].y));
                            if (dist < 36.0) {
                                throw std::runtime_error("Phase 4O group vehicles overlapped in formation (dist=" +
                                                         std::to_string(dist) + ")");
                            }
                        }
                    }
                    std::cout << "Phase 4O multi-vehicle battle-line formations PASSED\n";
                }

                // Phase 4P: Counter-battery muzzle-flash reveal in fog of war & timed fade.
                {
                    ss2::assets::UnitSpec blindTank{};
                    blindTank.id = "player_blind_tk";
                    blindTank.isVehicle = true;
                    blindTank.health = 500;
                    blindTank.sight = 6;
                    blindTank.ammo1 = 0;

                    ss2::assets::UnitSpec enemyHowitzer{};
                    enemyHowitzer.id = "enemy_howitzer";
                    enemyHowitzer.isVehicle = true;
                    enemyHowitzer.selType = "howitzer";
                    enemyHowitzer.health = 240;
                    enemyHowitzer.sight = 14;
                    enemyHowitzer.ammo1 = 1;
                    enemyHowitzer.shot1 = {"he105", ss2::assets::HitType::Explosive, 90, 90, 800, 800, 0, 50, 1, 50, "howitzer"};

                    auto cat4p = ss2::assets::UnitCatalog::fromSpecs({blindTank, enemyHowitzer});
                    const auto [ptX, ptY] = tc(48, 60);
                    const auto [ehX, ehY] = tc(58, 60);
                    std::vector<ss2::assets::Placement> veh4p{
                        {"player_blind_tk", ptX, ptY, 0, 100, 0, 1, 0},
                        {"enemy_howitzer", ehX, ehY, 0, 100, 100, 2, 1},
                    };
                    ss2::TutorialUnits sim4p(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             {}, veh4p, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4p);

                    if (sim4p.isValidVehicleAttackTarget(1u)) {
                        throw std::runtime_error("Phase 4P enemy howitzer should start hidden in player fog");
                    }
                    bool revealedOnFire = false;
                    for (int t = 0; t < 20; ++t) {
                        (void)sim4p.step();
                        if (sim4p.isValidVehicleAttackTarget(1u)) {
                            revealedOnFire = true;
                            break;
                        }
                    }
                    if (!revealedOnFire) {
                        throw std::runtime_error("Phase 4P enemy howitzer was not revealed by counter-battery muzzle flash");
                    }
                    for (int t = 0; t < 135; ++t) (void)sim4p.step();
                    if (sim4p.isValidVehicleAttackTarget(1u)) {
                        throw std::runtime_error("Phase 4P enemy howitzer stayed visible after muzzle-flash duration expired");
                    }
                    std::cout << "Phase 4P counter-battery muzzle-flash reveal & fade PASSED\n";
                }

                // Phase 4Q: Convoy escort/guard orders, auto-follow & idle exclusion.
                {
                    ss2::assets::UnitSpec truck4q{};
                    truck4q.id = "supply_4q";
                    truck4q.isVehicle = true;
                    truck4q.health = 180;
                    truck4q.moveSpeed = 2.0;
                    truck4q.sight = 10;

                    ss2::assets::UnitSpec tank4q{};
                    tank4q.id = "panzer_4q";
                    tank4q.isVehicle = true;
                    tank4q.health = 320;
                    tank4q.moveSpeed = 1.9;
                    tank4q.sight = 12;
                    tank4q.ammo1 = 20;
                    tank4q.shot1 = {"ap50", ss2::assets::HitType::Pierce, 70, 70, 320, 320, 0, 35, 1, 35, "tankgun"};

                    ss2::assets::UnitSpec at4q{};
                    at4q.id = "pak_4q";
                    at4q.isVehicle = true;
                    at4q.health = 160;
                    at4q.moveSpeed = 0.0;
                    at4q.sight = 12;
                    at4q.ammo1 = 20;

                    auto cat4q = ss2::assets::UnitCatalog::fromSpecs({truck4q, tank4q, at4q});
                    const auto [trX, trY] = tc(46, 74);
                    const auto [tkX, tkY] = tc(44, 74);
                    const auto [atX, atY] = tc(44, 78);
                    const auto [enX, enY] = tc(80, 80);
                    const auto [solX, solY] = tc(45, 75);
                    const auto [vipX, vipY] = tc(48, 78);
                    const auto [destX, destY] = tc(52, 74);

                    std::vector<ss2::assets::Infantry> inf4q{
                        {"drifle", solX, solY, 100, 40, 1, 0},
                        {"dofficer", vipX, vipY, 100, 40, 1, 0},
                    };
                    std::vector<ss2::assets::Placement> veh4q{
                        {"supply_4q", trX, trY, 0, 100, 0, 1, 0},  // idx 0: VIP convoy truck
                        {"panzer_4q", tkX, tkY, 0, 100, 20, 1, 0}, // idx 1: escort tank
                        {"pak_4q", atX, atY, 0, 100, 20, 1, 0},    // idx 2: stationary AT gun
                        {"panzer_4q", enX, enY, 0, 100, 20, 2, 1}, // idx 3: enemy vehicle
                    };
                    ss2::TutorialUnits sim4q(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4q, veh4q, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4q);

                    if (sim4q.canEscortVehicle({}, std::array<size_t, 1>{0}, 0) ||
                        sim4q.canEscortVehicle({}, std::array<size_t, 1>{2}, 0) ||
                        sim4q.canEscortVehicle(std::array<size_t, 1>{0}, std::array<size_t, 1>{1}, 3) ||
                        !sim4q.canEscortVehicle(std::array<size_t, 1>{0}, std::array<size_t, 1>{1}, 0)) {
                        throw std::runtime_error("Phase 4Q canEscortVehicle capability check mismatch");
                    }

                    if (sim4q.orderEscortVehicle(std::array<size_t, 1>{0}, std::array<size_t, 1>{1}, 0) != 2u) {
                        throw std::runtime_error("Phase 4Q orderEscortVehicle expected 2 escorts accepted");
                    }
                    const auto escortLines = sim4q.selectionOrderLines(std::array<size_t, 1>{0}, std::array<size_t, 1>{1});
                    size_t supportLines = 0;
                    for (const auto& ol : escortLines) {
                        if (ol.kind == ss2::TacticalOrderLine::Kind::Support) ++supportLines;
                    }
                    if (supportLines < 2u) {
                        throw std::runtime_error("Phase 4Q missing Support order lines for escorts");
                    }
                    for (const auto& iu : sim4q.idlePlayerUnits()) {
                        if ((!iu.isVehicle && iu.index == 0) || (iu.isVehicle && iu.index == 1)) {
                            throw std::runtime_error("Phase 4Q escorting units must be excluded from idlePlayerUnits");
                        }
                    }

                    if (sim4q.moveVehicles(std::array<size_t, 1>{0}, destX, destY) != 1u) {
                        throw std::runtime_error("Phase 4Q failed to move VIP truck");
                    }
                    double soldierStartDist = 0.0;
                    double soldierMinDist = 1e9;
                    {
                        const auto p0 = tc(45, 75);
                        soldierStartDist = std::hypot(double(destX - p0.first), double(destY - p0.second));
                    }
                    for (int t = 0; t < 300; ++t) {
                        for (const auto& sp : sim4q.step()) {
                            if (sp.index == 0) {
                                const double d = std::hypot(double(destX - sp.x), double(destY - sp.y));
                                soldierMinDist = std::min(soldierMinDist, d);
                            }
                        }
                    }
                    const auto snapFollow = sim4q.takeSnapshot();
                    const double truckErr = std::hypot(double(snapFollow.vehicles[0].x - destX),
                                                       double(snapFollow.vehicles[0].y - destY));
                    const double tankGap = std::hypot(double(snapFollow.vehicles[1].x - snapFollow.vehicles[0].x),
                                                      double(snapFollow.vehicles[1].y - snapFollow.vehicles[0].y));
                    if (truckErr > 20.0) {
                        throw std::runtime_error("Phase 4Q VIP truck did not reach destination (err=" +
                                                 std::to_string(truckErr) + ")");
                    }
                    if (tankGap > 140.0) {
                        throw std::runtime_error("Phase 4Q escort tank failed to follow VIP truck (gap=" +
                                                 std::to_string(tankGap) + ")");
                    }
                    if (soldierMinDist > soldierStartDist - 70.0) {
                        throw std::runtime_error("Phase 4Q escort soldier failed to follow VIP truck");
                    }

                    if (!sim4q.canEscortSoldier(std::array<size_t, 1>{0}, std::array<size_t, 1>{1}, 1) ||
                        sim4q.canEscortSoldier(std::array<size_t, 1>{1}, {}, 1) ||
                        sim4q.orderEscortSoldier(std::array<size_t, 1>{0}, std::array<size_t, 1>{1}, 1) != 2u) {
                        throw std::runtime_error("Phase 4Q orderEscortSoldier check failed");
                    }
                    sim4q.stopSelection(std::array<size_t, 1>{0});
                    sim4q.stopVehicles(std::array<size_t, 1>{1});
                    const auto stoppedLines = sim4q.selectionOrderLines(std::array<size_t, 1>{0}, std::array<size_t, 1>{1});
                    for (const auto& ol : stoppedLines) {
                        if (ol.kind == ss2::TacticalOrderLine::Kind::Support) {
                            throw std::runtime_error("Phase 4Q stop did not clear escort order line");
                        }
                    }
                    for (int t = 0; t < 40; ++t) (void)sim4q.step();
                    bool solIdle = false, vehIdle = false;
                    for (const auto& iu : sim4q.idlePlayerUnits()) {
                        if (!iu.isVehicle && iu.index == 0) solIdle = true;
                        if (iu.isVehicle && iu.index == 1) vehIdle = true;
                    }
                    if (!solIdle || !vehIdle) {
                        throw std::runtime_error("Phase 4Q stop did not clear escort idle exclusion");
                    }

                    if (sim4q.orderEscortVehicle({}, std::array<size_t, 1>{1}, 0) != 1u ||
                        sim4q.moveVehicles(std::array<size_t, 1>{1}, tkX, tkY) != 1u) {
                        throw std::runtime_error("Phase 4Q move override setup failed");
                    }
                    for (int t = 0; t < 200; ++t) (void)sim4q.step();
                    bool vehReturnedIdle = false;
                    for (const auto& iu : sim4q.idlePlayerUnits()) {
                        if (iu.isVehicle && iu.index == 1) vehReturnedIdle = true;
                    }
                    if (!vehReturnedIdle) {
                        throw std::runtime_error("Phase 4Q normal move did not clear escort target after arrival");
                    }
                    std::cout << "Phase 4Q convoy escort/guard orders, auto-follow & idle exclusion PASSED\n";
                }

                // Phase 4R: Sapper right-click minefield disarm + auto-chain, attack-move sweep & VehiclePose ammo2.
                {
                    ss2::assets::UnitSpec dualTank4r{};
                    dualTank4r.id = "panzer_4r";
                    dualTank4r.isVehicle = true;
                    dualTank4r.health = 320;
                    dualTank4r.moveSpeed = 1.8;
                    dualTank4r.sight = 12;
                    dualTank4r.ammo1 = 30;
                    dualTank4r.ammo2 = 120;
                    dualTank4r.shot1 = {"ap50", ss2::assets::HitType::Pierce, 70, 70, 320, 320, 0, 35, 1, 35, "tankgun"};
                    dualTank4r.shot2 = {"mg34", ss2::assets::HitType::Machine, 8, 8, 220, 220, 0, 4, 4, 40, "mgun"};

                    auto cat4r = ss2::assets::UnitCatalog::fromSpecs({dualTank4r});
                    const auto [sapX, sapY] = tc(50, 76);
                    const auto [rifX, rifY] = tc(50, 78);
                    const auto [swpX, swpY] = tc(50, 82);
                    const auto [swpDestX, swpDestY] = tc(56, 82);
                    const auto [tkX, tkY] = tc(46, 76);
                    const auto [mDet1X, mDet1Y] = tc(53, 76);
                    const auto [mHiddenX, mHiddenY] = tc(60, 76);

                    std::vector<ss2::assets::Infantry> inf4r{
                        {"dsapper", sapX, sapY, 100, 40, 1, 0},
                        {"drifle", rifX, rifY, 100, 40, 1, 0},
                        {"dsapper", swpX, swpY, 100, 40, 1, 0},
                    };
                    std::vector<ss2::assets::Placement> veh4r{
                        {"panzer_4r", tkX, tkY, 0, 100, 100, 1, 0},
                    };
                    ss2::TutorialUnits sim4r(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4r, veh4r, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4r);

                    std::vector<std::uint8_t> mines4r(128u * 128u, 0u);
                    mines4r[76u * 128u + 53u] = 0xC5u; // detected hostile AT mine (cluster 1)
                    mines4r[76u * 128u + 55u] = 0xC1u; // detected hostile AP mine (cluster 2, auto-chained)
                    mines4r[76u * 128u + 60u] = 0x45u; // undetected hostile AT mine (not right-clickable)
                    mines4r[82u * 128u + 52u] = 0xC1u; // detected hostile AP mine along attack-move path
                    sim4r.setMines(mines4r);

                    if (sim4r.canDisarmMineAt(std::array<size_t, 1>{0}, mHiddenX, mHiddenY)) {
                        throw std::runtime_error("Phase 4R undetected mine should not be targetable");
                    }
                    if (sim4r.canDisarmMineAt(std::array<size_t, 1>{1}, mDet1X, mDet1Y)) {
                        throw std::runtime_error("Phase 4R non-sapper must not be allowed to disarm mines");
                    }
                    if (!sim4r.canDisarmMineAt(std::array<size_t, 1>{0}, mDet1X, mDet1Y)) {
                        throw std::runtime_error("Phase 4R sapper should be able to target detected hostile mine");
                    }
                    if (sim4r.orderDisarmMine(std::array<size_t, 1>{0}, mDet1X, mDet1Y) != 1u) {
                        throw std::runtime_error("Phase 4R orderDisarmMine failed");
                    }
                    const auto sapLines = sim4r.selectionOrderLines(std::array<size_t, 1>{0}, {});
                    bool hasMineSupportLine = false;
                    for (const auto& ol : sapLines) {
                        if (ol.kind == ss2::TacticalOrderLine::Kind::Support) hasMineSupportLine = true;
                    }
                    if (!hasMineSupportLine) {
                        throw std::runtime_error("Phase 4R missing Support order line for disarming sapper");
                    }
                    for (const auto& iu : sim4r.idlePlayerUnits()) {
                        if (!iu.isVehicle && iu.index == 0) {
                            throw std::runtime_error("Phase 4R sapper en route to disarm must not be idle");
                        }
                    }

                    if (sim4r.orderAttackMove(std::array<size_t, 1>{2}, swpDestX, swpDestY) != 1u) {
                        throw std::runtime_error("Phase 4R sapper attack-move setup failed");
                    }

                    // Also trigger a VehiclePose via facing west to inspect coax ammo2 propagation.
                    if (sim4r.faceVehicles(std::array<size_t, 1>{0}, tkX - 100, tkY) != 1u) {
                        throw std::runtime_error("Phase 4R faceVehicles failed");
                    }
                    for (int t = 0; t < 340; ++t) (void)sim4r.step();

                    const auto mineEvs = sim4r.takeMineEvents();
                    size_t disarmedCount = 0;
                    size_t detonatedCount = 0;
                    for (const auto& me : mineEvs) {
                        if (me.kind == ss2::MineEvent::Disarmed) ++disarmedCount;
                        else if (me.kind == ss2::MineEvent::Detonated) ++detonatedCount;
                    }
                    if (disarmedCount < 3u || detonatedCount != 0u) {
                        throw std::runtime_error("Phase 4R expected >= 3 mines disarmed and 0 detonated, got disarmed=" +
                                                 std::to_string(disarmedCount) + " detonated=" +
                                                 std::to_string(detonatedCount));
                    }
                    bool sapIdle = false;
                    for (const auto& iu : sim4r.idlePlayerUnits()) {
                        if (!iu.isVehicle && iu.index == 0) sapIdle = true;
                    }
                    if (!sapIdle) {
                        throw std::runtime_error("Phase 4R sapper did not return to idle after clearing cluster");
                    }
                    const auto poses4r = sim4r.takeVehiclePoses();
                    bool foundAmmo2 = false;
                    for (const auto& vp : poses4r) {
                        if (vp.index == 0 && vp.ammo2 == 120) foundAmmo2 = true;
                    }
                    if (!foundAmmo2) {
                        throw std::runtime_error("Phase 4R VehiclePose did not carry ammo2=120");
                    }
                    std::cout << "Phase 4R sapper mine-sweep, auto-chain disarm, attack-move sweep & VehiclePose ammo2 PASSED\n";
                }

                // Phase 4S: Garrisoned building logistics (supply truck + ambulance) & cohesive armored platoon AI.
                {
                    ss2::assets::UnitSpec sup4s{};
                    sup4s.id = "sup_4s";
                    sup4s.isVehicle = true;
                    sup4s.health = 160;
                    sup4s.moveSpeed = 2.0;
                    sup4s.sight = 10;
                    sup4s.ammoRegenDelay = 20;

                    ss2::assets::UnitSpec amb4s{};
                    amb4s.id = "amb_4s";
                    amb4s.isVehicle = true;
                    amb4s.health = 150;
                    amb4s.moveSpeed = 2.0;
                    amb4s.sight = 10;
                    amb4s.healDelay = 20;

                    ss2::assets::UnitSpec tiger4s{};
                    tiger4s.id = "tiger_4s";
                    tiger4s.isVehicle = true;
                    tiger4s.health = 500;
                    tiger4s.moveSpeed = 1.6;
                    tiger4s.sight = 14;
                    tiger4s.ammo1 = 30;
                    tiger4s.shot1 = {"ap88", ss2::assets::HitType::Pierce, 90, 90, 420, 420, 0, 20, 1, 20, "tankgun"};

                    ss2::assets::UnitSpec t34_4s{};
                    t34_4s.id = "t34_4s";
                    t34_4s.isVehicle = true;
                    t34_4s.health = 400;
                    t34_4s.moveSpeed = 1.6;
                    t34_4s.sight = 6;
                    t34_4s.ammo1 = 30;
                    t34_4s.shot1 = {"ap76", ss2::assets::HitType::Pierce, 70, 70, 320, 320, 0, 25, 1, 25, "tankgun"};

                    auto cat4s = ss2::assets::UnitCatalog::fromSpecs({sup4s, amb4s, tiger4s, t34_4s});
                    const auto [bldX, bldY] = tc(52, 86);
                    const auto [supX, supY] = tc(58, 86);
                    const auto [ambX, ambY] = tc(58, 88);
                    const auto [ptX, ptY] = tc(48, 96);
                    const auto [eHitX, eHitY] = tc(58, 96);
                    const auto [eWingX, eWingY] = tc(58, 99);

                    std::vector<ss2::assets::Infantry> inf4s{
                        {"drifle", bldX, bldY, 100, 40, 1, 0},
                    };
                    std::vector<ss2::assets::Placement> veh4s{
                        {"sup_4s", supX, supY, 0, 100, 0, 1, 0},     // idx 0: supply truck
                        {"amb_4s", ambX, ambY, 0, 100, 0, 1, 0},     // idx 1: ambulance
                        {"tiger_4s", ptX, ptY, 0, 100, 100, 1, 0},   // idx 2: player tank attacker
                        {"t34_4s", eHitX, eHitY, 0, 100, 100, 11, 1}, // idx 3: enemy target tank (group 11)
                        {"t34_4s", eWingX, eWingY, 0, 100, 100, 11, 1}, // idx 4: enemy wingman tank (group 11)
                    };
                    ss2::TutorialUnits sim4s(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4s, veh4s, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4s);
                    sim4s.setBuildings({
                        {bldX, bldY, bldX - 20, bldY - 20, 40, 40, 4, 400}
                    });
                    if (sim4s.orderGarrisonBuilding(std::array<size_t, 1>{0}, 0) != 1u) {
                        throw std::runtime_error("Phase 4S garrison setup failed");
                    }
                    (void)sim4s.step();
                    auto snap4s = sim4s.takeSnapshot();
                    if (snap4s.soldiers[0].garrisonBuilding != 0) {
                        throw std::runtime_error("Phase 4S soldier did not enter intact building");
                    }
                    snap4s.soldiers[0].hp = 40;
                    snap4s.soldiers[0].ammo = 5;
                    snap4s.soldiers[0].atGrenades = 0;
                    if (!sim4s.loadSnapshot(snap4s)) {
                        throw std::runtime_error("Phase 4S loadSnapshot failed");
                    }

                    // Even at full building HP (400/400), both supply truck and ambulance can support garrisoned defenders.
                    if (!sim4s.canSupportBuilding(std::array<size_t, 1>{0}, 0) ||
                        !sim4s.canSupportBuilding(std::array<size_t, 1>{1}, 0)) {
                        throw std::runtime_error("Phase 4S canSupportBuilding failed for garrisoned need on intact building");
                    }
                    if (sim4s.orderSupportBuilding(std::array<size_t, 2>{0, 1}, 0) != 2u) {
                        throw std::runtime_error("Phase 4S orderSupportBuilding expected 2 vehicles dispatched");
                    }
                    if (sim4s.attackWithVehicles(std::array<size_t, 1>{2}, true, 3u) != 1u) {
                        throw std::runtime_error("Phase 4S attackWithVehicles target setup failed");
                    }

                    for (int t = 0; t < 160; ++t) (void)sim4s.step();

                    const auto after4s = sim4s.takeSnapshot();
                    const auto& occ = after4s.soldiers[0];
                    if (occ.ammo <= 5 || occ.atGrenades < 1 || occ.hp <= 40) {
                        throw std::runtime_error("Phase 4S garrisoned defender did not receive resupply & healing (hp=" +
                                                 std::to_string(occ.hp) + " ammo=" + std::to_string(occ.ammo) +
                                                 " gre=" + std::to_string(occ.atGrenades) + ")");
                    }
                    const auto& wing = after4s.vehicles[4];
                    if (wing.x == eWingX && wing.y == eWingY && wing.hullDir == 0u) {
                        throw std::runtime_error("Phase 4S enemy wingman tank did not react when platoon mate was hit");
                    }
                    std::cout << "Phase 4S garrisoned building logistics & cohesive armored platoon AI PASSED\n";
                }

                // Phase 4T: Garrisoned anti-tank urban ambush, stationary field-gun carriage traversal,
                // autonomous field logistics (respecting Hold Fire park lock), and on-patrol servicing.
                {
                    ss2::assets::UnitSpec sup4t{};
                    sup4t.id = "sup_4t";
                    sup4t.isVehicle = true;
                    sup4t.health = 160;
                    sup4t.moveSpeed = 2.0;
                    sup4t.sight = 10;
                    sup4t.ammoRegenDelay = 20;

                    ss2::assets::UnitSpec pak4t{};
                    pak4t.id = "pak_4t";
                    pak4t.isVehicle = true;
                    pak4t.health = 180;
                    pak4t.moveSpeed = 0.0;
                    pak4t.sight = 14;
                    pak4t.ammo1 = 20;
                    pak4t.shot1 = {"ap75", ss2::assets::HitType::Pierce, 80, 80, 320, 320, 0, 20, 1, 20, "tankgun"};

                    ss2::assets::UnitSpec t34_4t{};
                    t34_4t.id = "t34_4t";
                    t34_4t.isVehicle = true;
                    t34_4t.health = 400;
                    t34_4t.moveSpeed = 1.6;
                    t34_4t.sight = 4;
                    t34_4t.ammo1 = 30;
                    t34_4t.shot1 = {"ap76", ss2::assets::HitType::Pierce, 70, 70, 320, 320, 0, 25, 1, 25, "tankgun"};

                    auto cat4t = ss2::assets::UnitCatalog::fromSpecs({sup4t, pak4t, t34_4t});
                    const auto [bldX, bldY] = tc(46, 102);
                    const auto [eBldX, eBldY] = tc(49, 102);
                    const auto [pakX, pakY] = tc(64, 102);
                    const auto [ePakX, ePakY] = tc(60, 102);
                    const auto [m1X, m1Y] = tc(46, 108);
                    const auto [w1X, w1Y] = tc(51, 108);
                    const auto [m2X, m2Y] = tc(59, 108);
                    const auto [w2X, w2Y] = tc(64, 108);
                    const auto [s1X, s1Y] = tc(46, 113);
                    const auto [d1X, d1Y] = tc(51, 113);
                    const auto [s2X, s2Y] = tc(59, 113);
                    const auto [d2X, d2Y] = tc(64, 113);
                    const auto [patStartX, patStartY] = tc(42, 122);
                    const auto [patDestX, patDestY] = tc(50, 122);
                    const auto [d3X, d3Y] = tc(46, 122);

                    std::vector<ss2::assets::Infantry> inf4t{
                        {"dbazooka", bldX, bldY, 100, 40, 1, 0}, // idx 0: garrisoned AT ambush
                        {"dmedic", m1X, m1Y, 100, 0, 1, 0},     // idx 1: active idle medic
                        {"drifle", w1X, w1Y, 100, 40, 1, 0},    // idx 2: wounded friendly in 280px
                        {"dmedic", m2X, m2Y, 100, 0, 1, 0},     // idx 3: hold-fire medic
                        {"drifle", w2X, w2Y, 100, 40, 1, 0},    // idx 4: wounded friendly near hold-fire medic
                    };
                    std::vector<ss2::assets::Placement> veh4t{
                        {"t34_4t", eBldX, eBldY, 0, 100, 0, 11, 1},     // idx 0: enemy tank outside AT building
                        {"pak_4t", pakX, pakY, 0, 100, 100, 1, 0},      // idx 1: stationary field gun facing East (0u)
                        {"sup_4t", s1X, s1Y, 0, 100, 0, 1, 0},          // idx 2: idle supply truck
                        {"t34_4t", d1X, d1Y, 0, 50, 100, 1, 0},         // idx 3: damaged tank within 280px (200/400)
                        {"sup_4t", s2X, s2Y, 0, 100, 0, 1, 0},          // idx 4: hold-fire supply truck
                        {"t34_4t", d2X, d2Y, 0, 50, 100, 1, 0},         // idx 5: damaged tank near hold-fire truck
                        {"t34_4t", ePakX, ePakY, 0, 100, 0, 12, 1},     // idx 6: enemy tank West of field gun (16u)
                        {"sup_4t", patStartX, patStartY, 0, 100, 0, 1, 0}, // idx 7: patrolling supply truck
                        {"t34_4t", d3X, d3Y, 0, 50, 100, 1, 0},         // idx 8: damaged tank along patrol route
                    };
                    ss2::TutorialUnits sim4t(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4t, veh4t, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4t);
                    sim4t.setBuildings({
                        {bldX, bldY, bldX - 20, bldY - 20, 40, 40, 4, 400}
                    });
                    if (sim4t.orderGarrisonBuilding(std::array<size_t, 1>{0}, 0) != 1u) {
                        throw std::runtime_error("Phase 4T garrison setup failed");
                    }
                    (void)sim4t.step();
                    auto snap4t = sim4t.takeSnapshot();
                    if (snap4t.soldiers[0].garrisonBuilding != 0) {
                        throw std::runtime_error("Phase 4T dbazooka did not enter building");
                    }
                    snap4t.soldiers[2].hp = 40;
                    snap4t.soldiers[4].hp = 40;
                    if (!sim4t.loadSnapshot(snap4t)) {
                        throw std::runtime_error("Phase 4T loadSnapshot failed");
                    }
                    if (!sim4t.toggleHoldFire(std::array<size_t, 1>{3})) {
                        throw std::runtime_error("Phase 4T toggleHoldFire on medic 3 failed");
                    }
                    const auto vHoldRes = sim4t.toggleVehicleHoldFire(std::array<size_t, 1>{4});
                    if (!vHoldRes || !*vHoldRes) {
                        throw std::runtime_error("Phase 4T toggleVehicleHoldFire on supply 4 failed");
                    }
                    if (sim4t.patrolVehicles(std::array<size_t, 1>{7}, patDestX, patDestY) != 1u) {
                        throw std::runtime_error("Phase 4T patrolVehicles failed");
                    }

                    for (int t = 0; t < 200; ++t) (void)sim4t.step();

                    const auto after4t = sim4t.takeSnapshot();
                    if (after4t.vehicles[0].hp >= 400 || after4t.soldiers[0].garrisonBuilding != 0) {
                        throw std::runtime_error("Phase 4T garrisoned dbazooka ambush failed (vehHp=" +
                                                 std::to_string(after4t.vehicles[0].hp) + ")");
                    }
                    if (after4t.vehicles[1].hullDir != 16u || after4t.vehicles[6].hp >= 400) {
                        throw std::runtime_error("Phase 4T stationary field-gun carriage traversal failed (hullDir=" +
                                                 std::to_string(after4t.vehicles[1].hullDir) + " vehHp=" +
                                                 std::to_string(after4t.vehicles[6].hp) + ")");
                    }
                    if (after4t.soldiers[2].hp <= 40 || after4t.soldiers[4].hp != 40) {
                        throw std::runtime_error("Phase 4T autonomous medic triage failed (w1Hp=" +
                                                 std::to_string(after4t.soldiers[2].hp) + " w2Hp=" +
                                                 std::to_string(after4t.soldiers[4].hp) + ")");
                    }
                    if (after4t.vehicles[3].hp <= 200 || after4t.vehicles[5].hp != 200) {
                        throw std::runtime_error("Phase 4T autonomous supply truck failed (d1Hp=" +
                                                 std::to_string(after4t.vehicles[3].hp) + " d2Hp=" +
                                                 std::to_string(after4t.vehicles[5].hp) + ")");
                    }
                    if (after4t.vehicles[8].hp <= 200) {
                        throw std::runtime_error("Phase 4T on-patrol supply truck servicing failed (d3Hp=" +
                                                 std::to_string(after4t.vehicles[8].hp) + ")");
                    }
                    std::cout << "Phase 4T garrisoned AT ambush, field-gun carriage traversal & autonomous field logistics PASSED\n";
                }

                // Phase 4U: Casemate assault-gun hull pivoting, retail 'antiaircraft' class AA fix,
                // garrisoned officer binoculars (+3/+6 sight) & building command suite (Hold Fire / focus fire / occupantIds).
                {
                    ss2::assets::UnitSpec stug4u{};
                    stug4u.id = "stug_4u";
                    stug4u.isVehicle = true;
                    stug4u.health = 500;
                    stug4u.moveSpeed = 1.1;
                    stug4u.roleClass = "stug";
                    stug4u.selType = "spg";
                    stug4u.sight = 12;
                    stug4u.ammo1 = 30;
                    stug4u.shot1 = {"ap75", ss2::assets::HitType::Pierce, 90, 90, 320, 320, 0, 20, 1, 20, "tankgun"};

                    ss2::assets::UnitSpec aa52k4u{};
                    aa52k4u.id = "aa_52k_4u"; // intentionally lacks "flak" so roleClass == "antiaircraft" is tested
                    aa52k4u.isVehicle = true;
                    aa52k4u.health = 140;
                    aa52k4u.moveSpeed = 0.0;
                    aa52k4u.roleClass = "antiaircraft";
                    aa52k4u.sight = 14;
                    aa52k4u.ammo1 = 40;
                    aa52k4u.shot1 = {"aa85", ss2::assets::HitType::Air, 80, 80, 380, 380, 0, 10, 1, 10, "flak"};

                    ss2::assets::UnitSpec t34_4u{};
                    t34_4u.id = "t34_4u";
                    t34_4u.isVehicle = true;
                    t34_4u.health = 400;
                    t34_4u.moveSpeed = 1.6;
                    t34_4u.sight = 4;
                    t34_4u.ammo1 = 30;
                    t34_4u.shot1 = {"ap76", ss2::assets::HitType::Pierce, 70, 70, 320, 320, 0, 25, 1, 25, "tankgun"};

                    auto cat4u = ss2::assets::UnitCatalog::fromSpecs({stug4u, aa52k4u, t34_4u});
                    const auto [bld0X, bld0Y] = tc(46, 102);
                    const auto [obsX, obsY] = tc(28, 102); // 18 tiles West of Building 0 (>16 base+3, <=22 +6 bino)
                    const auto [bld1X, bld1Y] = tc(52, 108);
                    const auto [eBld1X, eBld1Y] = tc(55, 108);
                    const auto [stugX, stugY] = tc(64, 102);
                    const auto [eStugX, eStugY] = tc(60, 102);
                    const auto [aa1X, aa1Y] = tc(36, 122);
                    const auto [aa2X, aa2Y] = tc(38, 122);
                    const auto [aaGoalX, aaGoalY] = tc(40, 122);

                    std::vector<ss2::assets::Infantry> inf4u{
                        {"dofficer", bld0X, bld0Y, 100, 40, 1, 0}, // idx 0: officer in Building 0
                        {"dbazooka", bld1X, bld1Y, 100, 40, 1, 0}, // idx 1: bazooka in Building 1
                        {"drifle", obsX, obsY, 100, 0, 11, 1},     // idx 2: enemy soldier for bino sight test
                    };
                    std::vector<ss2::assets::Placement> veh4u{
                        {"stug_4u", stugX, stugY, 0, 100, 100, 1, 0},     // idx 0: mobile casemate facing East (0u)
                        {"t34_4u", eStugX, eStugY, 0, 100, 0, 11, 1},     // idx 1: enemy tank West (16u) of stug
                        {"aa_52k_4u", aa1X, aa1Y, 0, 100, 100, 1, 0},     // idx 2: AA battery #1
                        {"aa_52k_4u", aa2X, aa2Y, 0, 100, 100, 1, 0},     // idx 3: AA battery #2
                    };
                    ss2::TutorialUnits sim4u(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4u, veh4u, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4u);
                    sim4u.setBuildings({
                        {bld0X, bld0Y, bld0X - 20, bld0Y - 20, 40, 40, 4, 400},
                        {bld1X, bld1Y, bld1X - 20, bld1Y - 20, 40, 40, 4, 400},
                    });
                    if (sim4u.orderGarrisonBuilding(std::array<size_t, 1>{0}, 0) != 1u ||
                        sim4u.orderGarrisonBuilding(std::array<size_t, 1>{1}, 1) != 1u) {
                        throw std::runtime_error("Phase 4U garrison setup failed");
                    }
                    (void)sim4u.step();
                    auto snap4u = sim4u.takeSnapshot();
                    if (snap4u.soldiers[0].garrisonBuilding != 0 || snap4u.soldiers[1].garrisonBuilding != 1) {
                        throw std::runtime_error("Phase 4U soldiers did not enter buildings");
                    }
                    if (const auto f0 = sim4u.takeFogSnapshot();
                        !f0 || f0->size() != 128u * 128u || ((*f0)[std::size_t(102u) * 128u + 28u] & 1u) != 0u) {
                        throw std::runtime_error("Phase 4U enemy observer tile should start hidden before binoculars");
                    }

                    // Verify occupantIds propagated in building updates.
                    bool sawOcc1 = false;
                    for (const auto& bu : sim4u.takeBuildingUpdates()) {
                        if (bu.index == 1 && bu.occupantIds == std::vector<size_t>{1u}) sawOcc1 = true;
                    }
                    if (!sawOcc1) {
                        throw std::runtime_error("Phase 4U BuildingSnapshot did not carry occupantIds for Building 1");
                    }

                    // Enable Hold-Fire on Building 1, then spawn enemy tank 4 outside it to verify auto-fire suppression.
                    const auto bHold = sim4u.toggleBuildingHoldFire(1);
                    if (!bHold || !*bHold) {
                        throw std::runtime_error("Phase 4U toggleBuildingHoldFire(1) failed");
                    }
                    if (sim4u.toggleBuildingBinoculars(1).has_value()) {
                        throw std::runtime_error("Phase 4U toggleBuildingBinoculars(1) succeeded without an officer");
                    }
                    const size_t eBld1Tank = sim4u.spawnVehicle(
                        {"t34_4u", eBld1X, eBld1Y, 100, 0, 12, 1, 0u, 0u, 0, 0, 0, 0, true, true});

                    // Enemy bomber flies East from x=0 across AA battery at (36..38, 122) toward (40, 122).
                    std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air4u{};
                    air4u[1][0] = {"ju-87", 1, 1};
                    std::array<int, 4> pGrp4u{};
                    std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos4u{};
                    sim4u.setAirSupport(air4u, pGrp4u, pRos4u);
                    if (!sim4u.callAirSupport(ss2::assets::AirMissionKind::Bomber, aaGoalX, aaGoalY, 1)) {
                        throw std::runtime_error("Phase 4U enemy callAirSupport failed");
                    }

                    bool aaFired = false;
                    bool planeDown = false;
                    bool obsSpottedEarly = false;
                    for (int t = 0; t < 240; ++t) {
                        for (const auto& sp : sim4u.step()) {
                            if (sp.index == 2 && sp.visible) obsSpottedEarly = true;
                        }
                        for (const auto& sp : sim4u.takeAirSorties()) {
                            if (sp.aaFired) aaFired = true;
                        }
                        for (const auto& ae : sim4u.takeAirStrikeEvents()) {
                            if (ae.kind == ss2::AirStrikeEvent::PlaneShotDown) planeDown = true;
                        }
                    }

                    const auto mid4u = sim4u.takeSnapshot();
                    if (mid4u.vehicles[0].hullDir != 16u || mid4u.vehicles[1].hp >= 400) {
                        throw std::runtime_error("Phase 4U casemate StuG did not pivot hull to 16u / engage target (dir=" +
                                                 std::to_string(mid4u.vehicles[0].hullDir) + " hp=" +
                                                 std::to_string(mid4u.vehicles[1].hp) + ")");
                    }
                    if (!aaFired || !planeDown) {
                        throw std::runtime_error("Phase 4U retail antiaircraft class failed to shoot down bomber (aaFired=" +
                                                 std::string(aaFired ? "1" : "0") + " down=" +
                                                 std::string(planeDown ? "1" : "0") + ")");
                    }
                    if (obsSpottedEarly) {
                        throw std::runtime_error("Phase 4U observer spotted unexpectedly before building binoculars toggled on");
                    }
                    if (mid4u.vehicles[eBld1Tank].hp != 400) {
                        throw std::runtime_error("Phase 4U Building 1 Hold-Fire did not suppress garrison fire (hp=" +
                                                 std::to_string(mid4u.vehicles[eBld1Tank].hp) + ")");
                    }

                    // Toggle binoculars ON for Building 0 officer (+3 +6 elevated sight spots soldier 2 at 18 tiles)
                    // and focus-fire Building 1 onto enemy tank 4 while remaining garrisoned.
                    std::vector<ss2::SoldierPose> binoPoses;
                    const auto bOpt = sim4u.toggleBuildingBinoculars(0, &binoPoses);
                    if (!bOpt || !*bOpt) {
                        throw std::runtime_error("Phase 4U toggleBuildingBinoculars(0) failed");
                    }
                    bool obsPoseVisible = false;
                    for (const auto& sp : binoPoses) {
                        if (sp.index == 2 && sp.visible) obsPoseVisible = true;
                    }
                    const auto f1 = sim4u.takeFogSnapshot();
                    if (!obsPoseVisible || !f1 || f1->size() != 128u * 128u ||
                        ((*f1)[std::size_t(102u) * 128u + 28u] & 1u) == 0u) {
                        throw std::runtime_error("Phase 4U elevated garrisoned officer binoculars did not spot distant target");
                    }
                    if (sim4u.orderBuildingAttack(1, true, eBld1Tank) != 1u) {
                        throw std::runtime_error("Phase 4U orderBuildingAttack(1) failed");
                    }
                    for (int t = 0; t < 80; ++t) (void)sim4u.step();

                    const auto after4u = sim4u.takeSnapshot();
                    if (after4u.vehicles[eBld1Tank].hp >= 400 || after4u.soldiers[1].garrisonBuilding != 1) {
                        throw std::runtime_error("Phase 4U building focus-fire failed (hp=" +
                                                 std::to_string(after4u.vehicles[eBld1Tank].hp) + ")");
                    }
                    std::cout << "Phase 4U casemate hull pivot, antiaircraft class fix & garrison building command suite PASSED\n";
                }

                // Phase 4V: Infantry catalog stats (HMG/Mortar/General), mortar HE splash,
                // dynamic magazine resupply (s.desc.maxAmmo), and vehicle turndelay agility.
                {
                    ss2::assets::UnitSpec dhmg4v{};
                    dhmg4v.id = "dhmg_4v";
                    dhmg4v.selType = "hmg";
                    dhmg4v.health = 100;
                    dhmg4v.ammo1 = 200;
                    dhmg4v.sight = 11;
                    dhmg4v.shot1 = {"man_machine", ss2::assets::HitType::Machine, 46, 51, 650, 650, 8, 2, 8, 45, "mg"};

                    ss2::assets::UnitSpec dgen4v{};
                    dgen4v.id = "dgeneral_4v";
                    dgen4v.selType = "general";
                    dgen4v.health = 50;
                    dgen4v.ammo1 = 64;
                    dgen4v.sight = 11;
                    dgen4v.shot1 = {"man_machine", ss2::assets::HitType::Machine, 13, 14, 500, 500, 8, 29, 8, 90, "mg"};

                    ss2::assets::UnitSpec rmortar4v{};
                    rmortar4v.id = "rmortar_4v";
                    rmortar4v.selType = "mortar";
                    rmortar4v.health = 100;
                    rmortar4v.ammo1 = 6;
                    rmortar4v.sight = 11;
                    rmortar4v.shot1 = {"mortar", ss2::assets::HitType::Explosive, 95, 100, 520, 520, 40, 40, 1, 40, "mortar"};

                    ss2::assets::UnitSpec sup4v{};
                    sup4v.id = "sup_4v";
                    sup4v.isVehicle = true;
                    sup4v.health = 160;
                    sup4v.moveSpeed = 2.0;
                    sup4v.sight = 10;
                    sup4v.ammoRegenDelay = 20;

                    ss2::assets::UnitSpec fast4v{};
                    fast4v.id = "fast_4v";
                    fast4v.isVehicle = true;
                    fast4v.health = 120;
                    fast4v.moveSpeed = 2.5;
                    fast4v.sight = 10;
                    fast4v.turnDelay = 4;

                    ss2::assets::UnitSpec heavy4v{};
                    heavy4v.id = "heavy_4v";
                    heavy4v.isVehicle = true;
                    heavy4v.health = 1000;
                    heavy4v.moveSpeed = 1.0;
                    heavy4v.sight = 10;
                    heavy4v.turnDelay = 14;

                    auto cat4v = ss2::assets::UnitCatalog::fromSpecs({dhmg4v, dgen4v, rmortar4v, sup4v, fast4v, heavy4v});
                    const auto [hmgX, hmgY] = tc(8, 120);
                    const auto [supHX, supHY] = tc(9, 120);
                    const auto [eHmgX, eHmgY] = tc(12, 120);
                    const auto [genX, genY] = tc(10, 114);
                    const auto [morX, morY] = tc(18, 106);
                    const auto [supMX, supMY] = tc(19, 106);
                    const auto [eM1X, eM1Y] = tc(23, 106);
                    const auto [eM2X, eM2Y] = tc(24, 106);
                    const auto [fastX, fastY] = tc(28, 114);
                    const auto [fastFaceX, fastFaceY] = tc(24, 114);
                    const auto [heavyX, heavyY] = tc(28, 118);
                    const auto [heavyFaceX, heavyFaceY] = tc(24, 118);

                    std::vector<ss2::assets::Infantry> inf4v{
                        {"dhmg_4v", hmgX, hmgY, 100, 100, 1, 0},     // idx 0: HMG (200 ammo)
                        {"dgeneral_4v", genX, genY, 100, 100, 1, 0}, // idx 1: general (binoculars)
                        {"rmortar_4v", morX, morY, 400, 100, 1, 0},  // idx 2: mortar (6 ammo, HE splash)
                        {"drifle", eM1X, eM1Y, 120, 0, 11, 1},       // idx 3: mortar primary target
                        {"drifle", eM2X, eM2Y, 120, 0, 11, 1},       // idx 4: adjacent mortar splash target
                        {"drifle", eHmgX, eHmgY, 200, 0, 12, 1},     // idx 5: HMG target
                    };
                    std::vector<ss2::assets::Placement> veh4v{
                        {"sup_4v", supHX, supHY, 0, 100, 0, 1, 0},   // idx 0: near HMG
                        {"sup_4v", supMX, supMY, 0, 100, 0, 1, 0},   // idx 1: near mortar
                        {"fast_4v", fastX, fastY, 0, 100, 0, 1, 0},  // idx 2: agile vehicle (turnDelay=4 -> step 3)
                        {"heavy_4v", heavyX, heavyY, 0, 100, 0, 1, 0}, // idx 3: heavy vehicle (turnDelay=14 -> step 1)
                    };
                    ss2::TutorialUnits sim4v(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4v, veh4v, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4v);
                    const auto init4v = sim4v.takeSnapshot();
                    if (init4v.soldiers[0].ammo != 200 || init4v.soldiers[2].ammo != 6 ||
                        init4v.soldiers[2].atGrenades != 0) {
                        throw std::runtime_error("Phase 4V infantry catalog ammo/grenade init mismatch (hmg=" +
                                                 std::to_string(init4v.soldiers[0].ammo) + " mortar=" +
                                                 std::to_string(init4v.soldiers[2].ammo) + " gre=" +
                                                 std::to_string(init4v.soldiers[2].atGrenades) + ")");
                    }
                    if (!sim4v.toggleBinoculars(std::array<size_t, 1>{1})) {
                        throw std::runtime_error("Phase 4V dgeneral was not recognized for officer binoculars");
                    }
                    if (sim4v.faceVehicles(std::array<size_t, 1>{2}, fastFaceX, fastFaceY) != 1u ||
                        sim4v.faceVehicles(std::array<size_t, 1>{3}, heavyFaceX, heavyFaceY) != 1u) {
                        throw std::runtime_error("Phase 4V faceVehicles setup failed");
                    }
                    for (int t = 0; t < 6; ++t) (void)sim4v.step();
                    const auto turnSnap = sim4v.takeSnapshot();
                    if (turnSnap.vehicles[2].hullDir != 16u || turnSnap.vehicles[3].hullDir != 6u) {
                        throw std::runtime_error("Phase 4V turndelay agility mismatch (fast=" +
                                                 std::to_string(turnSnap.vehicles[2].hullDir) + " heavy=" +
                                                 std::to_string(turnSnap.vehicles[3].hullDir) + ")");
                    }

                    for (int t = 0; t < 50; ++t) (void)sim4v.step();
                    auto comb4v = sim4v.takeSnapshot();
                    if (comb4v.soldiers[3].hp >= 120 || comb4v.soldiers[4].hp >= 120 || comb4v.soldiers[5].hp >= 200) {
                        throw std::runtime_error("Phase 4V mortar splash / HMG burst combat failed (eM1=" +
                                                 std::to_string(comb4v.soldiers[3].hp) + " eM2=" +
                                                 std::to_string(comb4v.soldiers[4].hp) + " eHmg=" +
                                                 std::to_string(comb4v.soldiers[5].hp) + ")");
                    }

                    // Drain HMG and mortar magazines, halt enemies, and verify supply truck caps dynamically (200 vs 6).
                    comb4v.soldiers[0].ammo = 20;
                    comb4v.soldiers[2].ammo = 1;
                    comb4v.soldiers[3].hp = 0;
                    comb4v.soldiers[4].hp = 0;
                    comb4v.soldiers[5].hp = 0;
                    if (!sim4v.loadSnapshot(comb4v)) {
                        throw std::runtime_error("Phase 4V loadSnapshot failed before resupply check");
                    }
                    for (int t = 0; t < 100; ++t) (void)sim4v.step();
                    const auto res4v = sim4v.takeSnapshot();
                    if (res4v.soldiers[0].ammo != 200 || res4v.soldiers[2].ammo != 6) {
                        throw std::runtime_error("Phase 4V dynamic magazine resupply cap failed (hmg=" +
                                                 std::to_string(res4v.soldiers[0].ammo) + " mortar=" +
                                                 std::to_string(res4v.soldiers[2].ammo) + " morHp=" +
                                                 std::to_string(res4v.soldiers[2].hp) + " supX=" +
                                                 std::to_string(res4v.vehicles[1].x) + "," +
                                                 std::to_string(res4v.vehicles[1].y) + " vs mor=" +
                                                 std::to_string(morX) + "," + std::to_string(morY) + ")");
                    }
                    std::cout << "Phase 4V infantry catalog stats, mortar HE splash, dynamic magazine resupply & vehicle turndelay PASSED\n";
                }

                // Phase 4W: Live SoldierPose::ammo sync, mortar firedExplosive VFX/SFX flag & Fighter air-to-air interception.
                {
                    ss2::assets::UnitSpec rmortar4w{};
                    rmortar4w.id = "rmortar_4w";
                    rmortar4w.selType = "mortar";
                    rmortar4w.health = 100;
                    rmortar4w.ammo1 = 6;
                    rmortar4w.sight = 11;
                    rmortar4w.shot1 = {"mortar", ss2::assets::HitType::Explosive, 95, 100, 520, 520, 40, 40, 1, 40, "mortar"};

                    ss2::assets::UnitSpec sup4w{};
                    sup4w.id = "sup_4w";
                    sup4w.isVehicle = true;
                    sup4w.health = 160;
                    sup4w.moveSpeed = 2.0;
                    sup4w.sight = 10;
                    sup4w.ammoRegenDelay = 20;

                    auto cat4w = ss2::assets::UnitCatalog::fromSpecs({rmortar4w, sup4w});
                    const auto [mor4wX, mor4wY] = tc(30, 118);
                    const auto [sup4wX, sup4wY] = tc(29, 118);
                    const auto [e4wX, e4wY] = tc(34, 118);
                    const auto [airTargetX, airTargetY] = tc(40, 118);
                    const auto [fightTargetX, fightTargetY] = tc(44, 118);

                    std::vector<ss2::assets::Infantry> inf4w{
                        {"rmortar_4w", mor4wX, mor4wY, 200, 100, 1, 0},
                        {"drifle", e4wX, e4wY, 200, 0, 11, 1},
                    };
                    std::vector<ss2::assets::Placement> veh4w{
                        {"sup_4w", sup4wX, sup4wY, 0, 100, 0, 1, 0},
                    };
                    ss2::TutorialUnits sim4w(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                             inf4w, veh4w, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4w);

                    bool sawExplosiveFire = false;
                    bool sawResupplyPose = false;
                    for (int t = 0; t < 35 && !(sawExplosiveFire && sawResupplyPose); ++t) {
                        for (const auto& sp : sim4w.step()) {
                            if (sp.index == 0) {
                                if (sp.fired && sp.firedExplosive && sp.ammo == 5) {
                                    sawExplosiveFire = true;
                                }
                                if (!sp.fired && sawExplosiveFire && sp.ammo == 6) {
                                    sawResupplyPose = true;
                                }
                            }
                        }
                    }
                    if (!sawExplosiveFire || !sawResupplyPose) {
                        throw std::runtime_error(
                            std::string("Phase 4W live SoldierPose ammo/explosive check failed (fire=") +
                            (sawExplosiveFire ? "1" : "0") + " resupply=" + (sawResupplyPose ? "1" : "0") + ")");
                    }

                    // Fighter air-to-air interception with zero ground AA vehicles on map.
                    std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air4w{};
                    air4w[0][static_cast<std::size_t>(ss2::assets::AirMissionKind::Fighter)] = {"audax", 1, 1};
                    air4w[1][static_cast<std::size_t>(ss2::assets::AirMissionKind::Bomber)] = {"ju-87", 1, 1};
                    std::array<int, 4> pGrp4w{};
                    std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos4w{};
                    sim4w.setAirSupport(air4w, pGrp4w, pRos4w);
                    if (!sim4w.callAirSupport(ss2::assets::AirMissionKind::Bomber, airTargetX, airTargetY, 1) ||
                        !sim4w.callAirSupport(ss2::assets::AirMissionKind::Fighter, fightTargetX, fightTargetY, 0)) {
                        throw std::runtime_error("Phase 4W air support setup failed");
                    }
                    bool a2aTracer = false;
                    bool enemyBomberDown = false;
                    for (int t = 0; t < 200 && !(a2aTracer && enemyBomberDown); ++t) {
                        (void)sim4w.step();
                        for (const auto& sp : sim4w.takeAirSorties()) {
                            if (sp.owner == 1 && sp.kind == ss2::assets::AirMissionKind::Bomber && sp.aaFired) {
                                a2aTracer = true;
                            }
                        }
                        for (const auto& ae : sim4w.takeAirStrikeEvents()) {
                            if (ae.kind == ss2::AirStrikeEvent::PlaneShotDown && ae.owner == 1) {
                                enemyBomberDown = true;
                            }
                        }
                    }
                    if (!a2aTracer || !enemyBomberDown) {
                        throw std::runtime_error(
                            std::string("Phase 4W Fighter air-to-air interception failed (tracer=") +
                            (a2aTracer ? "1" : "0") + " down=" + (enemyBomberDown ? "1" : "0") + ")");
                    }
                    std::cout << "Phase 4W live SoldierPose ammo, mortar explosive flag & Fighter air-to-air interception PASSED\n";
                }

                // Phase 4X: Combined-arms auto-target priority, live VehiclePose ammo sync, AT infantry vehicle priority & marchsightbonus.
                {
                    // (A) AP AT gun prioritizes armed hostile tank over closer enemy rifleman and decrements live ammo.
                    {
                        ss2::assets::UnitSpec pak4x{};
                        pak4x.id = "pak40_4x";
                        pak4x.isVehicle = true;
                        pak4x.health = 160;
                        pak4x.ammo1 = 24;
                        pak4x.sight = 14;
                        pak4x.roleClass = "at";
                        pak4x.selType = "atg";
                        pak4x.shot1 = {"ap75", ss2::assets::HitType::Pierce, 400, 400, 760, 760, 0, 30, 1, 30, "gunshot"};

                        ss2::assets::UnitSpec t34_4x{};
                        t34_4x.id = "t34_4x";
                        t34_4x.isVehicle = true;
                        t34_4x.health = 500;
                        t34_4x.ammo1 = 60;
                        t34_4x.sight = 10;
                        t34_4x.shot1 = {"ap76", ss2::assets::HitType::Pierce, 250, 250, 600, 600, 0, 50, 1, 50, "gunshot"};

                        ss2::assets::UnitSpec blind4x{};
                        blind4x.id = "blind_4x";
                        blind4x.health = 120;
                        blind4x.ammo1 = 0;
                        blind4x.sight = 4;

                        auto cat4xA = ss2::assets::UnitCatalog::fromSpecs({pak4x, t34_4x, blind4x});
                        const auto [pakX, pakY] = tc(30, 30);
                        const auto [infX, infY] = tc(33, 30);  // closer (~107px)
                        const auto [tnkX, tnkY] = tc(35, 30);  // farther (~179px)
                        std::vector<ss2::assets::Infantry> inf4xA{{"blind_4x", infX, infY, 120, 0, 11, 1}};
                        std::vector<ss2::assets::Placement> veh4xA{
                            {"pak40_4x", pakX, pakY, 0, 100, 100, 1, 0},
                            {"t34_4x", tnkX, tnkY, 0, 100, 100, 11, 1},
                        };
                        ss2::TutorialUnits sim4xA(ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                                  inf4xA, veh4xA, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4xA);
                        bool sawInitAmmo24 = false;
                        for (const auto& vp : sim4xA.takeVehiclePoses()) {
                            if (vp.index == 0 && vp.ammo == 24) sawInitAmmo24 = true;
                        }
                        if (!sawInitAmmo24) {
                            throw std::runtime_error("Phase 4X initial catalog vehicle ammo=24 pose not emitted");
                        }
                        bool sawHitVeh = false;
                        int liveAmmoAfter = -1;
                        for (int t = 0; t < 15 && !sawHitVeh; ++t) {
                            (void)sim4xA.step();
                            for (const auto& vp : sim4xA.takeVehiclePoses()) {
                                if (vp.index == 0 && vp.fired) {
                                    sawHitVeh = vp.hitVehicle;
                                    liveAmmoAfter = vp.ammo;
                                }
                            }
                        }
                        const auto snap4xA = sim4xA.takeSnapshot();
                        if (!sawHitVeh || liveAmmoAfter != 23 ||
                            snap4xA.vehicles[1].hp >= 500 || snap4xA.soldiers[0].hp != 120) {
                            throw std::runtime_error("Phase 4X AP AT gun priority or live VehiclePose ammo sync failed");
                        }
                    }

                    // (B) MG vehicle prioritizes infantry over heavy armor, and flamer tank prioritizes occupied garrisoned building.
                    {
                        ss2::assets::UnitSpec mgcar4x{};
                        mgcar4x.id = "mgcar_4x";
                        mgcar4x.isVehicle = true;
                        mgcar4x.health = 140;
                        mgcar4x.ammo1 = 400;
                        mgcar4x.sight = 14;
                        mgcar4x.shot1 = {"man_machine", ss2::assets::HitType::Machine, 35, 35, 500, 500, 0, 10, 4, 30, "mg_tank_burst"};

                        ss2::assets::UnitSpec heavyTarget4x{};
                        heavyTarget4x.id = "heavy_target_4x";
                        heavyTarget4x.isVehicle = true;
                        heavyTarget4x.health = 900;
                        heavyTarget4x.ammo1 = 0;
                        heavyTarget4x.sight = 6;
                        heavyTarget4x.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {256, 256, 256};
                        heavyTarget4x.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {100, 80, 80};

                        ss2::assets::UnitSpec ot34_4x{};
                        ot34_4x.id = "ot34_4x";
                        ot34_4x.isVehicle = true;
                        ot34_4x.health = 560;
                        ot34_4x.ammo1 = 77;
                        ot34_4x.ammo2 = 50;
                        ot34_4x.sight = 14;
                        ot34_4x.shot1 = {"ap76", ss2::assets::HitType::Pierce, 196, 196, 600, 600, 0, 50, 1, 50, "gunshot"};
                        ot34_4x.shot2 = {"flamethrower", ss2::assets::HitType::Fire, 150, 150, 320, 320, 0, 30, 1, 30, "fmw"};

                        ss2::assets::UnitSpec blind4x{};
                        blind4x.id = "blind_4x";
                        blind4x.health = 120;
                        blind4x.ammo1 = 0;
                        blind4x.sight = 4;

                        auto cat4xB = ss2::assets::UnitCatalog::fromSpecs({mgcar4x, heavyTarget4x, ot34_4x, blind4x});

                        // MG car test
                        {
                            const auto [carX, carY] = tc(30, 50);
                            const auto [htX, htY] = tc(33, 50);   // closer (~107px) heavy tank
                            const auto [infX, infY] = tc(30, 55); // farther (~179px) rifleman
                            std::vector<ss2::assets::Infantry> mgInf{{"blind_4x", infX, infY, 120, 0, 11, 1}};
                            std::vector<ss2::assets::Placement> mgVeh{
                                {"mgcar_4x", carX, carY, 0, 100, 100, 1, 0},
                                {"heavy_target_4x", htX, htY, 0, 100, 100, 11, 1},
                            };
                            ss2::TutorialUnits simMg(
                                ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                mgInf, mgVeh, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4xB);
                            bool shotInf = false, shotVeh = false;
                            for (int t = 0; t < 15 && !shotInf; ++t) {
                                (void)simMg.step();
                                for (const auto& vp : simMg.takeVehiclePoses()) {
                                    if (vp.index == 0 && vp.fired) {
                                        if (vp.hitVehicle) shotVeh = true;
                                        else shotInf = true;
                                    }
                                }
                            }
                            const auto snapMg = simMg.takeSnapshot();
                            if (!shotInf || shotVeh || snapMg.soldiers[0].hp >= 120) {
                                throw std::runtime_error("Phase 4X MG vehicle anti-infantry priority failed");
                            }
                        }

                        // Flamethrower tank occupied hostile building priority
                        {
                            const auto [otX, otY] = tc(50, 50);
                            const auto [infX, infY] = tc(54, 50); // ~143px east
                            const auto [bldX, bldY] = tc(50, 53); // ~107px south (in 208px flame range)
                            std::vector<ss2::assets::Infantry> otInf{
                                {"blind_4x", infX, infY, 120, 0, 11, 1},
                                {"blind_4x", bldX, bldY, 120, 0, 12, 1},
                            };
                            std::vector<ss2::assets::Placement> otVeh{{"ot34_4x", otX, otY, 0, 100, 100, 1, 0}};
                            ss2::TutorialUnits simOt(
                                ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                                otInf, otVeh, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4xB);
                            ss2::assets::BuildingPlacement garHouse{};
                            garHouse.x = bldX; garHouse.y = bldY;
                            garHouse.left = bldX - 24; garHouse.top = bldY - 24;
                            garHouse.width = 48; garHouse.height = 48;
                            garHouse.capacity = 4; garHouse.maxHp = 400;
                            simOt.setBuildings({garHouse});
                            auto sOt = simOt.takeSnapshot();
                            sOt.buildings[0].owner = 1;
                            sOt.buildings[0].occupants = {1};
                            sOt.soldiers[1].garrisonBuilding = 0;
                            if (!simOt.loadSnapshot(sOt)) {
                                throw std::runtime_error("Phase 4X ot-34 building snapshot failed");
                            }
                            bool firedFlame = false;
                            for (int t = 0; t < 10 && !firedFlame; ++t) {
                                (void)simOt.step();
                                for (const auto& vp : simOt.takeVehiclePoses()) {
                                    if (vp.index == 0 && vp.fired && vp.firedFlamer) firedFlame = true;
                                }
                            }
                            const auto afterOt = simOt.takeSnapshot();
                            if (!firedFlame || afterOt.buildings[0].hp >= 400 || afterOt.soldiers[0].hp != 120) {
                                throw std::runtime_error("Phase 4X flamer tank garrison building priority failed");
                            }
                        }
                    }

                    // (C) AT infantry prioritizes hostile vehicle over closer hostile rifleman.
                    {
                        ss2::assets::UnitSpec baz4x{};
                        baz4x.id = "dbazooka_4x";
                        baz4x.selType = "atinf";
                        baz4x.health = 100;
                        baz4x.ammo1 = 4;
                        baz4x.sight = 10;
                        baz4x.shot1 = {"bazooka", ss2::assets::HitType::Pierce, 420, 420, 220, 220, 0, 50, 1, 50, "bazooka"};

                        ss2::assets::UnitSpec blind4x{};
                        blind4x.id = "blind_4x";
                        blind4x.health = 120;
                        blind4x.ammo1 = 0;
                        blind4x.sight = 4;

                        ss2::assets::UnitSpec t34_4x{};
                        t34_4x.id = "t34_4x";
                        t34_4x.isVehicle = true;
                        t34_4x.health = 600;
                        t34_4x.ammo1 = 0;
                        t34_4x.sight = 6;
                        t34_4x.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {40, 30, 20};

                        auto cat4xC = ss2::assets::UnitCatalog::fromSpecs({baz4x, blind4x, t34_4x});
                        const auto [bazX, bazY] = tc(30, 80);
                        const auto [infX, infY] = tc(32, 80); // ~72px east
                        const auto [tnkX, tnkY] = tc(30, 84); // ~143px south (< 240px kInfantryVsVehicleRangePx)
                        std::vector<ss2::assets::Infantry> bazInf{
                            {"dbazooka_4x", bazX, bazY, 100, 100, 1, 0},
                            {"blind_4x", infX, infY, 120, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> bazVeh{{"t34_4x", tnkX, tnkY, 0, 100, 100, 11, 1}};
                        ss2::TutorialUnits sim4xC(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            bazInf, bazVeh, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4xC);
                        (void)sim4xC.step();
                        const auto snap4xC = sim4xC.takeSnapshot();
                        if (snap4xC.soldiers[0].ammo != 3 || snap4xC.vehicles[0].hp >= 600 || snap4xC.soldiers[1].hp != 120) {
                            throw std::runtime_error("Phase 4X AT infantry vehicle priority failed");
                        }
                    }

                    // (D) Retail marchsightbonus button-up on firing & recovery after 125 calm ticks.
                    {
                        ss2::assets::UnitSpec tiger4x{};
                        tiger4x.id = "tiger_4x";
                        tiger4x.isVehicle = true;
                        tiger4x.health = 1160;
                        tiger4x.ammo1 = 94;
                        tiger4x.sight = 7;
                        tiger4x.marchSightBonus = 4; // 11t unbuttoned, 7t buttoned
                        tiger4x.shot1 = {"pierce8", ss2::assets::HitType::Pierce, 500, 500, 400, 400, 40, 60, 1, 60, "gunshot"};

                        ss2::assets::UnitSpec blind4x{};
                        blind4x.id = "blind_4x";
                        blind4x.health = 100;
                        blind4x.ammo1 = 0;
                        blind4x.sight = 4;

                        auto cat4xD = ss2::assets::UnitCatalog::fromSpecs({tiger4x, blind4x});
                        const auto [tigX, tigY] = tc(80, 90);
                        const auto [gndX, gndY] = tc(83, 90); // ~107px away (between deadzone=20 and range=260)
                        const auto [farX, farY] = tc(89, 90); // 9 tiles east: visible at 11t, hidden at 7t
                        std::vector<ss2::assets::Infantry> tigInf{{"blind_4x", farX, farY, 100, 0, 11, 1}};
                        std::vector<ss2::assets::Placement> tigVeh{{"tiger_4x", tigX, tigY, 0, 100, 100, 1, 0}};
                        ss2::TutorialUnits sim4xD(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            tigInf, tigVeh, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4xD);
                        if (!sim4xD.isValidAttackTarget(0, 0)) {
                            throw std::runtime_error("Phase 4X unbuttoned tiger should spot enemy at 9 tiles");
                        }
                        if (sim4xD.orderAttackGroundVehicles(std::array<size_t, 1>{0}, gndX, gndY) != 1u) {
                            throw std::runtime_error("Phase 4X tiger attack-ground order failed");
                        }
                        (void)sim4xD.step();
                        if (sim4xD.isValidAttackTarget(0, 0)) {
                            throw std::runtime_error("Phase 4X firing tiger should button up and lose marchsightbonus");
                        }
                        (void)sim4xD.stopVehicles(std::array<size_t, 1>{0});
                        for (int t = 0; t < 130; ++t) (void)sim4xD.step();
                        if (!sim4xD.isValidAttackTarget(0, 0)) {
                            throw std::runtime_error("Phase 4X tiger should regain marchsightbonus after 125 calm ticks");
                        }
                    }

                    std::cout << "Phase 4X combined-arms target priority, live VehiclePose ammo & marchsightbonus PASSED\n";
                }

                // Phase 4Y: Sniper/General retail binoculars, Infantry mortar attack-ground + range preview, and Kamikaze/Goliath self-destruct.
                {
                    // (A) Sniper crouched binoculars (preserves stealth) & General long-range retail binoculars (+14t).
                    {
                        ss2::assets::UnitSpec snp4y{};
                        snp4y.id = "sniper_4y";
                        snp4y.selType = "sniper";
                        snp4y.health = 100;
                        snp4y.ammo1 = 40;
                        snp4y.sight = 12;
                        snp4y.binocularSight = 16; // +4t bonus

                        ss2::assets::UnitSpec gen4y{};
                        gen4y.id = "dgeneral_4y";
                        gen4y.selType = "general";
                        gen4y.health = 50;
                        gen4y.ammo1 = 64;
                        gen4y.sight = 11;
                        gen4y.binocularSight = 32; // clamped +14t bonus -> 25t total

                        ss2::assets::UnitSpec blind4y{};
                        blind4y.id = "blind_4y";
                        blind4y.health = 100;
                        blind4y.ammo1 = 0;
                        blind4y.sight = 4;

                        auto cat4yA = ss2::assets::UnitCatalog::fromSpecs({snp4y, gen4y, blind4y});
                        const auto [snpX, snpY] = tc(30, 60);
                        const auto [esX, esY] = tc(43, 60); // 13 tiles east: hidden crouched (9t), spotted with bino (13t)
                        const auto [genX, genY] = tc(30, 80);
                        const auto [egX, egY] = tc(50, 80); // 20 tiles east: hidden (11t), spotted with general bino (25t)
                        std::vector<ss2::assets::Infantry> binoInf{
                            {"sniper_4y", snpX, snpY, 100, 100, 1, 0},
                            {"dgeneral_4y", genX, genY, 50, 100, 1, 0},
                            {"blind_4y", esX, esY, 100, 0, 11, 1},
                            {"blind_4y", egX, egY, 100, 0, 12, 1},
                        };
                        std::vector<ss2::assets::Placement> noVeh;
                        ss2::TutorialUnits sim4yA(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            binoInf, noVeh, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4yA);
                        if (!sim4yA.toggleCrouch(std::array<size_t, 1>{0}) ||
                            sim4yA.isValidAttackTarget(2, 0) || sim4yA.isValidAttackTarget(3, 0)) {
                            throw std::runtime_error("Phase 4Y binocular baseline visibility check failed");
                        }
                        if (!sim4yA.toggleBinoculars(std::array<size_t, 2>{0, 1}) ||
                            !sim4yA.isSoldierCamouflaged(0) ||
                            !sim4yA.isValidAttackTarget(2, 0) || !sim4yA.isValidAttackTarget(3, 0)) {
                            throw std::runtime_error("Phase 4Y sniper/general retail binoculars failed");
                        }
                    }

                    // (B) Infantry mortar attack-ground, range/deadzone preview, order line & mine-clearing HE splash.
                    {
                        ss2::assets::UnitSpec mor4y{};
                        mor4y.id = "rmortar_4y";
                        mor4y.selType = "mortar";
                        mor4y.health = 100;
                        mor4y.ammo1 = 6;
                        mor4y.sight = 11;
                        mor4y.shot1 = {"mortar", ss2::assets::HitType::Explosive, 95, 95, 512, 512, 179, 40, 1, 40, "mortar"};

                        ss2::assets::UnitSpec blind4y{};
                        blind4y.id = "blind_4y";
                        blind4y.health = 120;
                        blind4y.ammo1 = 0;
                        blind4y.sight = 4;

                        auto cat4yB = ss2::assets::UnitCatalog::fromSpecs({mor4y, blind4y});
                        const auto [morX, morY] = tc(30, 30);
                        const auto [tgtX, tgtY] = tc(37, 30); // ~250px east (inside 179..512 mortar band)
                        std::vector<ss2::assets::Infantry> morInf{
                            {"rmortar_4y", morX, morY, 100, 100, 1, 0},
                            {"blind_4y", tgtX, tgtY, 120, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> noVeh;
                        ss2::TutorialUnits sim4yB(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            morInf, noVeh, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4yB);
                        std::vector<std::uint8_t> mines4y(128u * 128u, 0u);
                        mines4y[30u * 128u + 37u] = 0x45u; // hostile player-1 AT mine under target
                        sim4yB.setMines(std::move(mines4y));

                        const auto previews = sim4yB.selectionRangePreviews({}, std::array<size_t, 1>{0});
                        if (previews.size() != 1u || previews[0].rangePx < 500.0f || previews[0].deadzonePx < 170.0f) {
                            throw std::runtime_error("Phase 4Y mortar selectionRangePreviews failed");
                        }
                        if (sim4yB.orderAttackGroundSoldiers(std::array<size_t, 1>{0}, tgtX, tgtY) != 1u) {
                            throw std::runtime_error("Phase 4Y orderAttackGroundSoldiers failed");
                        }
                        const auto lines = sim4yB.selectionOrderLines(std::array<size_t, 1>{0}, {});
                        const bool hasGroundLine = std::any_of(lines.begin(), lines.end(), [](const ss2::TacticalOrderLine& l) {
                            return l.kind == ss2::TacticalOrderLine::Kind::AttackGround;
                        });
                        if (!hasGroundLine) {
                            throw std::runtime_error("Phase 4Y mortar attack-ground tactical order line missing");
                        }
                        bool sawMortarBlast = false;
                        for (const auto& sp : sim4yB.step()) {
                            if (sp.index == 0 && sp.fired && sp.firedExplosive && sp.ammo == 5) sawMortarBlast = true;
                        }
                        bool sawMineDetonated = false;
                        for (const auto& me : sim4yB.takeMineEvents()) {
                            if (me.kind == ss2::MineEvent::Detonated) sawMineDetonated = true;
                        }
                        const auto snap4yB = sim4yB.takeSnapshot();
                        if (!sawMortarBlast || !sawMineDetonated || snap4yB.soldiers[1].hp >= 120) {
                            throw std::runtime_error("Phase 4Y infantry mortar attack-ground or mine clearing failed");
                        }
                    }

                    // (C) Retail Kamikaze soldier & SdKfz 303 Goliath self-destruct explosive assault.
                    {
                        ss2::assets::UnitSpec kami4y{};
                        kami4y.id = "kamikaze_4y";
                        kami4y.selType = "kamikaze";
                        kami4y.kamikazePct = 100;
                        kami4y.health = 100;
                        kami4y.ammo1 = 1;
                        kami4y.sight = 11;
                        kami4y.shot1 = {"kamikaze", ss2::assets::HitType::Explosive, 625, 625, 40, 40, 2, 1, 1, 1, ""};

                        ss2::assets::UnitSpec goliath4y{};
                        goliath4y.id = "sdkfz.303";
                        goliath4y.isVehicle = true;
                        goliath4y.kamikazePct = 101;
                        goliath4y.health = 30;
                        goliath4y.ammo1 = 1;
                        goliath4y.moveSpeed = 0.4;
                        goliath4y.sight = 10;
                        goliath4y.shot1 = {"kamikaze", ss2::assets::HitType::Explosive, 1875, 1875, 64, 64, 3, 1, 1, 1, ""};

                        ss2::assets::UnitSpec t34_4y{};
                        t34_4y.id = "t34_4y";
                        t34_4y.isVehicle = true;
                        t34_4y.health = 600;
                        t34_4y.ammo1 = 0;
                        t34_4y.sight = 6;
                        t34_4y.armor[static_cast<std::size_t>(ss2::assets::HitType::Explosive)] = {40, 30, 20};

                        ss2::assets::UnitSpec tiger4y{};
                        tiger4y.id = "tiger_4y";
                        tiger4y.isVehicle = true;
                        tiger4y.health = 1160;
                        tiger4y.ammo1 = 0;
                        tiger4y.sight = 6;
                        tiger4y.armor[static_cast<std::size_t>(ss2::assets::HitType::Explosive)] = {80, 60, 60};

                        auto cat4yC = ss2::assets::UnitCatalog::fromSpecs({kami4y, goliath4y, t34_4y, tiger4y});
                        const auto [kX, kY] = tc(30, 100);
                        const auto [t34X, t34Y] = tc(31, 100); // ~36px east (inside 56px melee charge reach)
                        const auto [golX, golY] = tc(60, 100);
                        const auto [tigX, tigY] = tc(61, 100); // ~36px east (inside 64px Goliath detonation range)
                        std::vector<ss2::assets::Infantry> kamInf{{"kamikaze_4y", kX, kY, 100, 100, 1, 0}};
                        std::vector<ss2::assets::Placement> kamVeh{
                            {"sdkfz.303", golX, golY, 0, 100, 100, 2, 0},
                            {"t34_4y", t34X, t34Y, 0, 100, 100, 11, 1},
                            {"tiger_4y", tigX, tigY, 0, 100, 100, 12, 1},
                        };
                        ss2::TutorialUnits sim4yC(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            kamInf, kamVeh, ss2::FogGrid(128, 128, flags), 2, true, {}, cat4yC);
                        bool sawKamiBlast = false;
                        for (const auto& sp : sim4yC.step()) {
                            if (sp.index == 0 && !sp.alive && sp.hp == 0 && sp.fired && sp.firedExplosive) {
                                sawKamiBlast = true;
                            }
                        }
                        bool sawGoliathBlast = false;
                        for (const auto& vp : sim4yC.takeVehiclePoses()) {
                            if (vp.index == 0 && !vp.alive && vp.hp == 0 && vp.fired && vp.heavyImpact) {
                                sawGoliathBlast = true;
                            }
                        }
                        const auto snap4yC = sim4yC.takeSnapshot();
                        if (!sawKamiBlast || snap4yC.vehicles[1].hp > 300 ||
                            !sawGoliathBlast || snap4yC.vehicles[2].hp >= 1160) {
                            throw std::runtime_error("Phase 4Y kamikaze or Goliath self-destruct assault failed");
                        }
                    }

                    std::cout << "Phase 4Y sniper/general binoculars, mortar attack-ground & kamikaze/Goliath PASSED\n";
                }

                // Phase 4Z: Retail directional crew protection, intact de-crewing, retail crew_unit spawns, and halted crew self-repair.
                {
                    // (A) Sniper shield block front vs rear de-crew of intact AT gun + unmanned lock + capture.
                    {
                        ss2::assets::UnitSpec pak4z{};
                        pak4z.id = "pak40_4z";
                        pak4z.isVehicle = true;
                        pak4z.selType = "atg";
                        pak4z.roleClass = "at";
                        pak4z.health = 240;
                        pak4z.ammo1 = 24;
                        pak4z.sight = 12;
                        pak4z.crewNumber = 2;
                        pak4z.hasCrewProtection = true;
                        pak4z.armor[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {256, 256, 256};
                        pak4z.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {40, 36, 36};
                        pak4z.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {45, 24, 0};
                        pak4z.shot1 = {"pierce8", ss2::assets::HitType::Pierce, 250, 250, 350, 350, 0, 20, 1, 20, "gunshot"};

                        ss2::assets::UnitSpec snp4z{};
                        snp4z.id = "sniper_4z";
                        snp4z.selType = "sniper";
                        snp4z.health = 100;
                        snp4z.ammo1 = 40;
                        snp4z.sight = 12;
                        snp4z.shot1 = {"prof", ss2::assets::HitType::Sniper, 88, 88, 420, 420, 0, 12, 1, 12, "rifle1"};

                        ss2::assets::UnitSpec rfl4z{};
                        rfl4z.id = "dguner_4z";
                        rfl4z.selType = "infantry";
                        rfl4z.health = 100;
                        rfl4z.ammo1 = 60;
                        rfl4z.sight = 10;

                        auto cat4zA = ss2::assets::UnitCatalog::fromSpecs({pak4z, snp4z, rfl4z});
                        const auto [gunX, gunY] = tc(40, 80);
                        const auto [frontX, frontY] = tc(44, 80);
                        const auto [rearX, rearY] = tc(36, 80);
                        const auto [capX, capY] = tc(40, 81);
                        std::vector<ss2::assets::Infantry> inf4zA{
                            {"sniper_4z", frontX, frontY, 100, 100, 1, 0},
                            {"sniper_4z", rearX, rearY, 100, 100, 1, 0},
                            {"dguner_4z", capX, capY, 100, 100, 1, 0},
                        };
                        std::vector<ss2::assets::Placement> veh4zA{
                            {"pak40_4z", gunX, gunY, 0, 100, 100, 11, 1},
                        };
                        ss2::TutorialUnits sim4zA(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            inf4zA, veh4zA, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4zA);

                        // Frontal sniper shot blocked by AT gun shield (protection.Sniper.front = 45).
                        if (sim4zA.attackVehicle(std::array<size_t, 1>{0}, 0) != 1u) {
                            throw std::runtime_error("Phase 4Z frontal sniper attackVehicle failed");
                        }
                        for (int t = 0; t < 20; ++t) (void)sim4zA.step();
                        const auto snapFront = sim4zA.takeSnapshot();
                        if (snapFront.vehicles[0].owner != 1 || snapFront.vehicles[0].hp != 240) {
                            throw std::runtime_error("Phase 4Z frontal gun shield failed to protect AT crew");
                        }

                        // Rear sniper shots de-crew intact AT gun cleanly (protection.Sniper.rear = 0).
                        if (sim4zA.attackVehicle(std::array<size_t, 1>{1}, 0) != 1u) {
                            throw std::runtime_error("Phase 4Z rear sniper attackVehicle failed");
                        }
                        bool sawDecrew = false;
                        for (int t = 0; t < 35; ++t) {
                            (void)sim4zA.step();
                            const auto poses = sim4zA.takeVehiclePoses();
                            if (std::any_of(poses.begin(), poses.end(), [](const ss2::VehiclePose& vp) {
                                    return vp.index == 0 && vp.decrewed && vp.owner == 3 && vp.alive && vp.hp == 240;
                                })) {
                                sawDecrew = true;
                                break;
                            }
                        }
                        const auto snapDecrew = sim4zA.takeSnapshot();
                        if (!sawDecrew || snapDecrew.vehicles[0].owner != 3 || snapDecrew.vehicles[0].hp != 240) {
                            throw std::runtime_error("Phase 4Z rear sniper failed to de-crew intact pak40");
                        }
                        // Capture de-crewed intact AT gun with rifleman.
                        if (sim4zA.orderCaptureVehicle(std::array<size_t, 1>{2}, 0) != 1u) {
                            throw std::runtime_error("Phase 4Z capture of de-crewed pak40 failed to queue");
                        }
                        for (int t = 0; t < 40; ++t) (void)sim4zA.step();
                        const auto snapCap = sim4zA.takeSnapshot();
                        if (snapCap.vehicles[0].owner != 0 || !snapCap.vehicles[0].alive || snapCap.vehicles[0].hp != 240) {
                            throw std::runtime_error("Phase 4Z capture of de-crewed pak40 failed");
                        }
                    }

                    // (B) Flamer crew-cooking sealed heavy tank into neutral state & retail crew_unit abandon spawn.
                    {
                        ss2::assets::UnitSpec tig4z{};
                        tig4z.id = "tiger_4z";
                        tig4z.isVehicle = true;
                        tig4z.nation = "german";
                        tig4z.crewUnit = "dtankist";
                        tig4z.crewNumber = 5;
                        tig4z.health = 900;
                        tig4z.ammo1 = 94;
                        tig4z.sight = 10;
                        tig4z.hasCrewProtection = true;
                        tig4z.armor[static_cast<std::size_t>(ss2::assets::HitType::Fire)] = {20, 16, 16};
                        tig4z.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Fire)] = {1, 1, 1};
                        tig4z.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {256, 256, 256};

                        ss2::assets::UnitSpec flm4z{};
                        flm4z.id = "dflamer_4z";
                        flm4z.health = 100;
                        flm4z.ammo1 = 60;
                        flm4z.sight = 10;
                        flm4z.shot1 = {"flame1", ss2::assets::HitType::Fire, 140, 140, 160, 160, 0, 8, 1, 8, "fmw"};

                        auto cat4zB = ss2::assets::UnitCatalog::fromSpecs({tig4z, flm4z});
                        const auto [tigX, tigY] = tc(50, 80);
                        const auto [flmX, flmY] = tc(50, 82);
                        const auto [ownX, ownY] = tc(56, 80);
                        std::vector<ss2::assets::Infantry> inf4zB{
                            {"dflamer_4z", flmX, flmY, 100, 100, 1, 0},
                        };
                        std::vector<ss2::assets::Placement> veh4zB{
                            {"tiger_4z", tigX, tigY, 0, 100, 100, 11, 1},
                            {"tiger_4z", ownX, ownY, 0, 100, 100, 1, 0},
                        };
                        ss2::TutorialUnits sim4zB(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            inf4zB, veh4zB, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4zB);

                        if (sim4zB.attackVehicle(std::array<size_t, 1>{0}, 0) != 1u) {
                            throw std::runtime_error("Phase 4Z flamer attackVehicle failed");
                        }
                        bool sawCookDecrew = false;
                        for (int t = 0; t < 40; ++t) {
                            (void)sim4zB.step();
                            const auto poses = sim4zB.takeVehiclePoses();
                            if (std::any_of(poses.begin(), poses.end(), [](const ss2::VehiclePose& vp) {
                                    return vp.index == 0 && vp.decrewed && vp.owner == 3;
                                })) {
                                sawCookDecrew = true;
                                break;
                            }
                        }
                        const auto snapB = sim4zB.takeSnapshot();
                        if (!sawCookDecrew || snapB.vehicles[0].owner != 3 || snapB.vehicles[0].hp <= 700) {
                            throw std::runtime_error("Phase 4Z flamer failed to crew-cook tiger while leaving hull intact");
                        }
                        const auto crewSpawns = sim4zB.abandonVehicles(std::array<size_t, 1>{1});
                        if (crewSpawns.size() != 1u || crewSpawns[0].id != "dtankist") {
                            throw std::runtime_error("Phase 4Z abandonVehicles did not spawn retail crew_unit dtankist");
                        }
                    }

                    // (C) Halted out-of-combat crew field self-repair capped at max(moveDamageHp + 1, 35% maxHp).
                    {
                        ss2::assets::UnitSpec selfRep{};
                        selfRep.id = "tiger_rep_4z";
                        selfRep.isVehicle = true;
                        selfRep.health = 900;
                        selfRep.moveDamageHp = 550;
                        selfRep.canSelfRepair = true;
                        selfRep.moveSpeed = 2.0;

                        ss2::assets::UnitSpec noRep{};
                        noRep.id = "norep_4z";
                        noRep.isVehicle = true;
                        noRep.health = 900;
                        noRep.moveDamageHp = 550;
                        noRep.canSelfRepair = false;
                        noRep.moveSpeed = 2.0;

                        auto cat4zC = ss2::assets::UnitCatalog::fromSpecs({selfRep, noRep});
                        const auto [rX, rY] = tc(64, 80);
                        const auto [nrX, nrY] = tc(68, 80);
                        std::vector<ss2::assets::Infantry> emptyInf{};
                        std::vector<ss2::assets::Placement> veh4zC{
                            {"tiger_rep_4z", rX, rY, 0, 40, 100, 1, 0}, // 360 HP < 551 cap
                            {"norep_4z", nrX, nrY, 0, 40, 100, 1, 0},
                        };
                        ss2::TutorialUnits sim4zC(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            emptyInf, veh4zC, ss2::FogGrid(128, 128, flags), 2, false, {}, cat4zC);
                        bool sawSelfRepaired = false;
                        for (int t = 0; t < 350; ++t) {
                            (void)sim4zC.step();
                            const auto poses = sim4zC.takeVehiclePoses();
                            if (std::any_of(poses.begin(), poses.end(), [](const ss2::VehiclePose& vp) {
                                    return vp.index == 0 && vp.repaired;
                                })) {
                                sawSelfRepaired = true;
                            }
                        }
                        const auto snapC = sim4zC.takeSnapshot();
                        if (!sawSelfRepaired || snapC.vehicles[0].hp != 551 || snapC.vehicles[1].hp != 360) {
                            throw std::runtime_error("Phase 4Z emergency crew self-repair ceiling mismatch");
                        }
                    }

                    std::cout << "Phase 4Z directional crew protection, intact de-crewing, retail crew_unit & emergency self-repair PASSED\n";
                }

                // Phase 5A: Secondary cook-off blasts (dieexplosion), partial crew degradation/medic recovery & naval terrain mobility.
                {
                    // (A) Vehicle dieexplosion cook-off & bounded chain reaction.
                    {
                        ss2::assets::UnitSpec killer5a{};
                        killer5a.id = "killer_5a";
                        killer5a.isVehicle = true;
                        killer5a.health = 600;
                        killer5a.ammo1 = 20;
                        killer5a.sight = 12;
                        killer5a.shot1 = {"pierce8", ss2::assets::HitType::Pierce, 200, 200, 300, 300, 0, 5, 1, 5, "gunshot"};

                        ss2::assets::UnitSpec victim5a{};
                        victim5a.id = "victim_5a";
                        victim5a.isVehicle = true;
                        victim5a.health = 80;
                        victim5a.dieExplosion = "dead_tank";
                        victim5a.sight = 6;

                        ss2::assets::UnitSpec crate5a{};
                        crate5a.id = "crate_5a";
                        crate5a.isVehicle = true;
                        crate5a.selType = "crate";
                        crate5a.health = 40;
                        crate5a.dieExplosion = "crate";
                        crate5a.sight = 4;

                        ss2::assets::UnitSpec obs5a{};
                        obs5a.id = "obs_5a";
                        obs5a.isVehicle = true;
                        obs5a.health = 500;
                        obs5a.sight = 4;

                        ss2::assets::UnitSpec sold5a{};
                        sold5a.id = "sold_5a";
                        sold5a.health = 100;
                        sold5a.sight = 6;

                        auto cat5aA = ss2::assets::UnitCatalog::fromSpecs({killer5a, victim5a, crate5a, obs5a, sold5a});
                        const auto [kX, kY] = tc(36, 80);
                        const auto [vX, vY] = tc(40, 80);
                        const auto [cX, cY] = tc(40, 81); // ~35px SE (< 56px cook-off radius)
                        const auto [oX, oY] = tc(39, 80); // ~35px W
                        const auto [sX, sY] = tc(41, 80); // ~35px E
                        std::vector<ss2::assets::Infantry> inf5aA{{"sold_5a", sX, sY, 100, 0, 11, 1}};
                        std::vector<ss2::assets::Placement> veh5aA{
                            {"killer_5a", kX, kY, 0, 100, 100, 1, 0},
                            {"victim_5a", vX, vY, 0, 100, 0, 11, 1},
                            {"crate_5a", cX, cY, 0, 100, 0, 11, 1},
                            {"obs_5a", oX, oY, 0, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits sim5aA(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            inf5aA, veh5aA, ss2::FogGrid(128, 128, flags), 2, false, {}, cat5aA);
                        if (sim5aA.attackWithVehicles(std::array<size_t, 1>{0}, true, 1) != 1u) {
                            throw std::runtime_error("Phase 5A killer attackWithVehicles failed");
                        }
                        for (int t = 0; t < 15; ++t) (void)sim5aA.step();
                        const auto snap5aA = sim5aA.takeSnapshot();
                        if (snap5aA.vehicles[1].hp != 0 || snap5aA.vehicles[2].hp != 0 ||
                            snap5aA.vehicles[3].hp >= 500 || snap5aA.soldiers[0].hp >= 100) {
                            throw std::runtime_error("Phase 5A dieexplosion secondary cook-off blast failed");
                        }
                    }

                    // (B) Partial crew degradation (attackcrew=3) slows reload, Medic crew healing restores readiness.
                    {
                        ss2::assets::UnitSpec he5a{};
                        he5a.id = "he_5a";
                        he5a.isVehicle = true;
                        he5a.health = 900;
                        he5a.ammo1 = 1;
                        he5a.sight = 12;
                        he5a.shot1 = {"he", ss2::assets::HitType::Explosive, 310, 310, 300, 300, 0, 5, 1, 5, "gunshot"};

                        ss2::assets::UnitSpec tig5a{};
                        tig5a.id = "tig_5a";
                        tig5a.isVehicle = true;
                        tig5a.health = 900;
                        tig5a.crewNumber = 5;
                        tig5a.attackCrew = 3;
                        tig5a.moveCrew = 1;
                        tig5a.hasCrewProtection = true;
                        tig5a.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Explosive)] = {0, 0, 0};
                        tig5a.armor[static_cast<std::size_t>(ss2::assets::HitType::Explosive)] = {256, 256, 256};
                        tig5a.ammo1 = 20;
                        tig5a.sight = 12;
                        tig5a.shot1 = {"pierce8", ss2::assets::HitType::Pierce, 20, 20, 300, 300, 0, 20, 1, 20, "gunshot"};

                        ss2::assets::UnitSpec med5a{};
                        med5a.id = "cmedic_5a";
                        med5a.selType = "medic";
                        med5a.health = 100;
                        med5a.sight = 8;

                        auto cat5aB = ss2::assets::UnitCatalog::fromSpecs({he5a, tig5a, med5a});
                        const auto [hX, hY] = tc(46, 80);
                        const auto [tX, tY] = tc(50, 80);
                        const auto [mX, mY] = tc(50, 81);
                        std::vector<ss2::assets::Infantry> emptyInf{};
                        std::vector<ss2::assets::Placement> veh5aB{
                            {"he_5a", hX, hY, 0, 100, 100, 11, 1},
                            {"tig_5a", tX, tY, 0, 100, 100, 1, 0},
                        };
                        ss2::TutorialUnits sim5aB(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            emptyInf, veh5aB, ss2::FogGrid(128, 128, flags), 2, true, {}, cat5aB);
                        bool sawImpaired = false;
                        for (int t = 0; t < 28; ++t) {
                            (void)sim5aB.step();
                            for (const auto& vp : sim5aB.takeVehiclePoses()) {
                                if (vp.index == 1 && vp.crewImpaired) sawImpaired = true;
                            }
                        }
                        const auto snapB1 = sim5aB.takeSnapshot();
                        if (!sawImpaired || snapB1.vehicles[1].ammo != 19) {
                            throw std::runtime_error("Phase 5A partial crew degradation / reload penalty failed");
                        }
                        // Spawn friendly medic adjacent to Tiger; medic heals crew -> clears crewImpaired.
                        (void)sim5aB.spawnInfantry({"cmedic_5a", mX, mY, 100, 0, 1, 0});
                        bool sawRecovered = false;
                        for (int t = 0; t < 30; ++t) {
                            (void)sim5aB.step();
                            for (const auto& vp : sim5aB.takeVehiclePoses()) {
                                if (vp.index == 1 && vp.repaired && !vp.crewImpaired) sawRecovered = true;
                            }
                        }
                        if (!sawRecovered) {
                            throw std::runtime_error("Phase 5A Medic failed to restore impaired vehicle crew");
                        }
                    }

                    // (C) Naval craft sails water into shallows while blocked from dry land (and land tank blocked from water).
                    {
                        ss2::assets::UnitSpec boat5a{};
                        boat5a.id = "japan.landing";
                        boat5a.isVehicle = true;
                        boat5a.health = 400;
                        boat5a.moveSpeed = 2.0;
                        boat5a.sight = 10;
                        boat5a.walkOnGround = false;
                        boat5a.walkOnWater = true;
                        boat5a.walkOnShallows = true;

                        ss2::assets::UnitSpec tank5a{};
                        tank5a.id = "land_5a";
                        tank5a.isVehicle = true;
                        tank5a.health = 400;
                        tank5a.moveSpeed = 2.0;
                        tank5a.sight = 10;
                        tank5a.walkOnGround = true;
                        tank5a.walkOnWater = false;
                        tank5a.walkOnShallows = true;

                        auto cat5aC = ss2::assets::UnitCatalog::fromSpecs({boat5a, tank5a});
                        std::vector<std::uint32_t> nflags(16 * 16, 0u);
                        std::vector<std::uint16_t> nobst(16 * 16, 0x000Fu); // dry ground default
                        for (int x = 5; x <= 7; ++x) nobst[8 * 16 + x] = 0x00F0u; // deep water
                        nobst[8 * 16 + 8] = 0x0F00u;                              // shallows
                        auto ntc = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{16 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [bStartX, bStartY] = ntc(5, 8);
                        const auto [shallX, shallY] = ntc(8, 8);
                        const auto [dryX, dryY] = ntc(9, 8);
                        const auto [tStartX, tStartY] = ntc(10, 8);
                        const auto [deepX, deepY] = ntc(6, 8);
                        std::vector<ss2::assets::Infantry> emptyInf{};
                        std::vector<ss2::assets::Placement> veh5aC{
                            {"japan.landing", bStartX, bStartY, 0, 100, 0, 1, 0},
                            {"land_5a", tStartX, tStartY, 16, 100, 0, 1, 0},
                        };
                        ss2::TutorialUnits sim5aC(
                            ss2::InfantryPathGrid::fromSubCellMasks(16, 16, nflags, nobst, 4),
                            emptyInf, veh5aC, std::nullopt, 2, false, {}, cat5aC);

                        if (sim5aC.moveVehicles(std::array<size_t, 1>{0}, shallX, shallY) != 1u) {
                            throw std::runtime_error("Phase 5A naval moveVehicles to shallows failed");
                        }
                        for (int t = 0; t < 45; ++t) (void)sim5aC.step();
                        const auto snapShallows = sim5aC.takeSnapshot();
                        if (std::hypot(double(snapShallows.vehicles[0].x - shallX),
                                       double(snapShallows.vehicles[0].y - shallY)) > 20.0) {
                            throw std::runtime_error("Phase 5A naval craft failed to reach shallows across water");
                        }
                        // Order naval craft onto dry land (9,8) -> blocked.
                        (void)sim5aC.moveVehicles(std::array<size_t, 1>{0}, dryX, dryY);
                        for (int t = 0; t < 15; ++t) (void)sim5aC.step();
                        const auto snapDry = sim5aC.takeSnapshot();
                        const int dWu = (snapDry.vehicles[0].x - 512) / 2 + snapDry.vehicles[0].y;
                        const int dWv = snapDry.vehicles[0].y - (snapDry.vehicles[0].x - 512) / 2;
                        const int dTx = dWu / 32;
                        const int dTy = dWv / 32;
                        if (nobst[static_cast<std::size_t>(dTy * 16 + dTx)] == 0x000Fu ||
                            std::hypot(double(snapDry.vehicles[0].x - dryX),
                                       double(snapDry.vehicles[0].y - dryY)) < 15.0) {
                            throw std::runtime_error("Phase 5A naval craft illegally entered dry ground");
                        }
                        // Order land tank toward deep water (6,8) -> must not enter deep water tile.
                        (void)sim5aC.moveVehicles(std::array<size_t, 1>{1}, deepX, deepY);
                        for (int t = 0; t < 35; ++t) (void)sim5aC.step();
                        const auto snapLand = sim5aC.takeSnapshot();
                        const int lWu = (snapLand.vehicles[1].x - 512) / 2 + snapLand.vehicles[1].y;
                        const int lWv = snapLand.vehicles[1].y - (snapLand.vehicles[1].x - 512) / 2;
                        const int lTx = lWu / 32;
                        const int lTy = lWv / 32;
                        if (nobst[static_cast<std::size_t>(lTy * 16 + lTx)] == 0x00F0u ||
                            std::hypot(double(snapLand.vehicles[1].x - deepX),
                                       double(snapLand.vehicles[1].y - deepY)) < 15.0) {
                            throw std::runtime_error("Phase 5A land tank illegally entered deep water");
                        }
                    }

                    std::cout << "Phase 5A cook-off blasts, partial crew degradation/medic recovery & naval terrain mobility PASSED\n";
                }

                // Phase 5B: Finite supply logistics (ammo.box/spare.parts crate specialization, crate-to-truck refill)
                // and P-FLAGS 0x2000u High Ground vision + downhill range bonus.
                {
                    // (A) Specialized ammo.box vs spare.parts crates with finite stockpile deduction.
                    {
                        ss2::assets::UnitSpec ammoCrate{};
                        ammoCrate.id = "ammo.box";
                        ammoCrate.isVehicle = true;
                        ammoCrate.selType = "crate";
                        ammoCrate.health = 100;
                        ammoCrate.ammo1 = 1000;
                        ammoCrate.ammo2 = 0;

                        ss2::assets::UnitSpec partsCrate{};
                        partsCrate.id = "spare.parts";
                        partsCrate.isVehicle = true;
                        partsCrate.selType = "crate";
                        partsCrate.health = 100;
                        partsCrate.ammo1 = 0;
                        partsCrate.ammo2 = 1000;

                        ss2::assets::UnitSpec tig5b{};
                        tig5b.id = "tiger_5b";
                        tig5b.isVehicle = true;
                        tig5b.health = 900;
                        tig5b.ammo1 = 94;
                        tig5b.ammo2 = 5100;

                        auto cat5bA = ss2::assets::UnitCatalog::fromSpecs({ammoCrate, partsCrate, tig5b});
                        const auto [cX, cY] = tc(36, 80);
                        const auto [tX, tY] = tc(37, 80);
                        std::vector<ss2::assets::Infantry> emptyInf{};
                        std::vector<ss2::assets::Placement> vehAmmoOnly{
                            {"ammo.box", cX, cY, 0, 100, 100, 1, 0},
                            {"tiger_5b", tX, tY, 0, 50, 50, 1, 0}, // 450 HP, 47 ammo
                        };
                        ss2::TutorialUnits simAmmo(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            emptyInf, vehAmmoOnly, ss2::FogGrid(128, 128, flags), 2, false, {}, cat5bA);
                        for (int t = 0; t < 30; ++t) (void)simAmmo.step();
                        const auto snapAmmo = simAmmo.takeSnapshot();
                        if (snapAmmo.vehicles[1].ammo <= 47 || snapAmmo.vehicles[1].hp != 450 ||
                            snapAmmo.vehicles[0].ammo >= 1000) {
                            throw std::runtime_error("Phase 5B ammo.box specialization or deduction failed");
                        }

                        std::vector<ss2::assets::Placement> vehPartsOnly{
                            {"spare.parts", cX, cY, 0, 100, 100, 1, 0},
                            {"tiger_5b", tX, tY, 0, 50, 50, 1, 0},
                        };
                        ss2::TutorialUnits simParts(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            emptyInf, vehPartsOnly, ss2::FogGrid(128, 128, flags), 2, false, {}, cat5bA);
                        for (int t = 0; t < 30; ++t) (void)simParts.step();
                        const auto snapParts = simParts.takeSnapshot();
                        if (snapParts.vehicles[1].hp != 468 || snapParts.vehicles[1].ammo != 47 ||
                            snapParts.vehicles[0].ammo2 != 820) {
                            throw std::runtime_error("Phase 5B spare.parts specialization or deduction failed");
                        }
                    }

                    // (B) Supply truck (sdkfz.11) refills from neutral ammo.box & spare.parts depot crates.
                    {
                        ss2::assets::UnitSpec truckSpec{};
                        truckSpec.id = "sdkfz.11";
                        truckSpec.isVehicle = true;
                        truckSpec.selType = "sappers";
                        truckSpec.ammoRegenDelay = 8;
                        truckSpec.moveSpeed = 1.6;
                        truckSpec.health = 100;
                        truckSpec.ammo1 = 15000;
                        truckSpec.ammo2 = 5000;

                        ss2::assets::UnitSpec ammoCrate{};
                        ammoCrate.id = "ammo.box";
                        ammoCrate.isVehicle = true;
                        ammoCrate.selType = "crate";
                        ammoCrate.health = 100;
                        ammoCrate.ammo1 = 1000;
                        ammoCrate.ammo2 = 0;

                        ss2::assets::UnitSpec partsCrate{};
                        partsCrate.id = "spare.parts";
                        partsCrate.isVehicle = true;
                        partsCrate.selType = "crate";
                        partsCrate.health = 100;
                        partsCrate.ammo1 = 0;
                        partsCrate.ammo2 = 1000;

                        auto cat5bB = ss2::assets::UnitCatalog::fromSpecs({truckSpec, ammoCrate, partsCrate});
                        const auto [trX, trY] = tc(40, 80);
                        const auto [aX, aY] = tc(41, 80);
                        const auto [pX, pY] = tc(40, 81);
                        std::vector<ss2::assets::Infantry> emptyInf{};
                        std::vector<ss2::assets::Placement> veh5bB{
                            {"sdkfz.11", trX, trY, 0, 100, 50, 1, 0},     // 7500 ammo, 2500 parts
                            {"ammo.box", aX, aY, 0, 100, 100, 0, 3},      // neutral depot crate
                            {"spare.parts", pX, pY, 0, 100, 100, 0, 3},   // neutral depot crate
                        };
                        ss2::TutorialUnits sim5bB(
                            ss2::InfantryPathGrid::fromSubCellMasks(128, 128, flags, obstacles),
                            emptyInf, veh5bB, ss2::FogGrid(128, 128, flags), 2, false, {}, cat5bB);
                        if (!sim5bB.canSupportVehicle(std::array<size_t, 1>{0}, 1) ||
                            !sim5bB.canSupportVehicle(std::array<size_t, 1>{0}, 2)) {
                            throw std::runtime_error("Phase 5B canSupportVehicle rejected accessible supply crates");
                        }
                        for (int t = 0; t < 30; ++t) (void)sim5bB.step();
                        const auto snap5bB = sim5bB.takeSnapshot();
                        if (snap5bB.vehicles[0].ammo != 8000 || snap5bB.vehicles[0].ammo2 != 3000 ||
                            snap5bB.vehicles[1].ammo != 500 || snap5bB.vehicles[2].ammo2 != 500) {
                            throw std::runtime_error("Phase 5B crate-to-truck depot refill mismatch");
                        }
                    }

                    // (C) High Ground (P-FLAGS 0x2000u) grants +2 sight tiles and +12% downhill firing range.
                    {
                        ss2::assets::UnitSpec spotter5b{};
                        spotter5b.id = "spotter_5b";
                        spotter5b.health = 100;
                        spotter5b.ammo1 = 40;
                        spotter5b.sight = 10;

                        ss2::assets::UnitSpec gun5b{};
                        gun5b.id = "gun_5b";
                        gun5b.isVehicle = true;
                        gun5b.health = 600;
                        gun5b.ammo1 = 20;
                        gun5b.sight = 9;
                        gun5b.shot1 = {"pierce8", ss2::assets::HitType::Pierce, 80, 80, 400, 400, 0, 10, 1, 10, "gunshot"}; // 260px flat, 291.2px elevated

                        ss2::assets::UnitSpec target5b{};
                        target5b.id = "target_5b";
                        target5b.isVehicle = true;
                        target5b.health = 600;
                        target5b.sight = 4;

                        auto cat5bC = ss2::assets::UnitCatalog::fromSpecs({spotter5b, gun5b, target5b});
                        std::vector<std::uint32_t> flatFlags(64u * 64u, 0u);
                        std::vector<std::uint32_t> highFlags(64u * 64u, 0u);
                        highFlags[20u * 64u + 20u] = 0x2000u; // elevated vantage at tile (20, 20)
                        std::vector<std::uint16_t> eObst(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [oX, oY] = tc64(20, 20);
                        const auto [sTgtX, sTgtY] = tc64(31, 20); // 11 tiles east: hidden to sight=10, spotted by sight=10+2
                        const auto [vTgtX, vTgtY] = tc64(28, 20); // ~286.2px east: >260px flat, <291.2px downhill

                        std::vector<ss2::assets::Infantry> inf5bC{
                            {"spotter_5b", oX, oY, 100, 40, 1, 0},
                            {"spotter_5b", sTgtX, sTgtY, 100, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> veh5bC{
                            {"gun_5b", oX, oY, 0, 100, 100, 1, 0},
                            {"target_5b", vTgtX, vTgtY, 16, 100, 0, 11, 1},
                        };

                        ss2::TutorialUnits simFlat(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flatFlags, eObst),
                            inf5bC, veh5bC, ss2::FogGrid(64, 64, flatFlags), 2, true, {}, cat5bC);
                        ss2::TutorialUnits simHigh(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, highFlags, eObst),
                            inf5bC, veh5bC, ss2::FogGrid(64, 64, highFlags), 2, true, {}, cat5bC);
                        if (simFlat.isValidAttackTarget(1, 0) || !simHigh.isValidAttackTarget(1, 0) ||
                            !simHigh.isElevatedMapPoint(oX, oY) || simFlat.isElevatedMapPoint(oX, oY)) {
                            throw std::runtime_error("Phase 5B high-ground +2 sight bonus check failed");
                        }
                        for (int t = 0; t < 15; ++t) {
                            (void)simFlat.step();
                            (void)simHigh.step();
                        }
                        const auto snapFlat = simFlat.takeSnapshot();
                        const auto snapHigh = simHigh.takeSnapshot();
                        if (snapFlat.vehicles[0].ammo != 20 || snapFlat.vehicles[1].hp != 600 ||
                            snapHigh.vehicles[0].ammo >= 20 || snapHigh.vehicles[1].hp >= 600) {
                            throw std::runtime_error("Phase 5B high-ground +12% downhill range check failed");
                        }
                    }

                    std::cout << "Phase 5B finite supply logistics, crate transfer & high-ground elevation bonus PASSED\n";

                    // Phase 5C: retail 14-class attackpref matrix, HMG/mortar suppression + Officer rally aura,
                    // and tracked vs wheeled (engine track/whell) shallows mobility.
                    // (A) 14-class attackpref prioritization & pref==1 auto-acquire suppression.
                    {
                        ss2::assets::UnitSpec att5c{};
                        att5c.id = "att_5c";
                        att5c.isVehicle = true;
                        att5c.health = 400;
                        att5c.ammo1 = 30;
                        att5c.sight = 14;
                        att5c.hasAttackPref = true;
                        att5c.attackPref[static_cast<std::size_t>(ss2::assets::TargetClass::Gun)] = 255;
                        att5c.attackPref[static_cast<std::size_t>(ss2::assets::TargetClass::Car)] = 180;
                        att5c.attackPref[static_cast<std::size_t>(ss2::assets::TargetClass::HeavyTank)] = 1;
                        att5c.shot1 = {"ap5c", ss2::assets::HitType::Pierce, 80, 80, 420, 420, 0, 8, 1, 8, "gunshot"};

                        ss2::assets::UnitSpec car5c{};
                        car5c.id = "car_5c";
                        car5c.isVehicle = true;
                        car5c.targetType = "car";
                        car5c.health = 200;
                        car5c.sight = 4;

                        ss2::assets::UnitSpec gun5c{};
                        gun5c.id = "gun_5c";
                        gun5c.isVehicle = true;
                        gun5c.targetType = "gun";
                        gun5c.health = 200;
                        gun5c.sight = 4;

                        ss2::assets::UnitSpec htank5c{};
                        htank5c.id = "htank_5c";
                        htank5c.isVehicle = true;
                        htank5c.targetType = "heavytank";
                        htank5c.health = 800;
                        htank5c.sight = 4;

                        auto cat5cA = ss2::assets::UnitCatalog::fromSpecs({att5c, car5c, gun5c, htank5c});
                        std::vector<std::uint32_t> flags5c(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5c(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [aX, aY] = tc64(20, 20);
                        const auto [cX, cY] = tc64(22, 20);
                        const auto [gX, gY] = tc64(24, 20);
                        std::vector<ss2::assets::Infantry> emptyInf5c{};
                        std::vector<ss2::assets::Placement> veh5cPrio{
                            {"att_5c", aX, aY, 0, 100, 100, 1, 0},
                            {"car_5c", cX, cY, 16, 100, 0, 11, 1},
                            {"gun_5c", gX, gY, 16, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits simPrio(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            emptyInf5c, veh5cPrio, ss2::FogGrid(64, 64, flags5c), 2, true, {}, cat5cA);
                        for (int t = 0; t < 10; ++t) (void)simPrio.step();
                        const auto snapPrio = simPrio.takeSnapshot();
                        if (snapPrio.vehicles[1].hp != 200 || snapPrio.vehicles[2].hp >= 200) {
                            throw std::runtime_error("Phase 5C attackpref priority failed to prefer GUN(255) over closer CAR(180)");
                        }

                        std::vector<ss2::assets::Placement> veh5cSkip{
                            {"att_5c", aX, aY, 0, 100, 100, 1, 0},
                            {"htank_5c", cX, cY, 16, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits simSkip(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            emptyInf5c, veh5cSkip, ss2::FogGrid(64, 64, flags5c), 2, true, {}, cat5cA);
                        for (int t = 0; t < 12; ++t) (void)simSkip.step();
                        const auto snapNoAuto = simSkip.takeSnapshot();
                        if (snapNoAuto.vehicles[0].ammo != 30 || snapNoAuto.vehicles[1].hp != 800) {
                            throw std::runtime_error("Phase 5C attackpref pref==1 failed to suppress auto-acquire");
                        }
                        const std::vector<size_t> attIds{0};
                        if (simSkip.attackWithVehicles(attIds, true, 1) != 1u) {
                            throw std::runtime_error("Phase 5C explicit attackWithVehicles rejected pref==1 target");
                        }
                        for (int t = 0; t < 10; ++t) (void)simSkip.step();
                        const auto snapManual = simSkip.takeSnapshot();
                        if (snapManual.vehicles[0].ammo >= 30 || snapManual.vehicles[1].hp >= 800) {
                            throw std::runtime_error("Phase 5C explicit attackVehicle failed to override pref==1");
                        }
                    }

                    // (B) HMG suppression, 1.35x reload penalty & Officer rally aura (3u/tick within 160px).
                    {
                        ss2::assets::UnitSpec hmg5c{};
                        hmg5c.id = "dhmg5c";
                        hmg5c.selType = "hmg";
                        hmg5c.health = 200;
                        hmg5c.ammo1 = 8; // fires bursts then stops so recovery duration can be measured
                        hmg5c.sight = 14;
                        hmg5c.shot1 = {"mg5c", ss2::assets::HitType::Machine, 2, 2, 260, 260, 0, 2, 1, 2, "gunshot"};

                        ss2::assets::UnitSpec rifle5c{};
                        rifle5c.id = "rifle_5c";
                        rifle5c.health = 500;
                        rifle5c.ammo1 = 60;
                        rifle5c.sight = 14;
                        rifle5c.shot1 = {"r5c", ss2::assets::HitType::Machine, 1, 1, 260, 260, 0, 10, 1, 10, "gunshot"};

                        ss2::assets::UnitSpec off5c{};
                        off5c.id = "officer_5c";
                        off5c.selType = "officer";
                        off5c.health = 300;
                        off5c.ammo1 = 0;
                        off5c.sight = 14;

                        auto cat5cB = ss2::assets::UnitCatalog::fromSpecs({hmg5c, rifle5c, off5c});
                        std::vector<std::uint32_t> flags5c(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5c(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [hmgX, hmgY] = tc64(20, 20);
                        const auto [rifX, rifY] = tc64(22, 20);
                        const auto [offX, offY] = tc64(22, 21);

                        std::vector<ss2::assets::Placement> emptyVeh5c{};
                        std::vector<ss2::assets::Infantry> infNoOff{
                            {"rifle_5c", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5c", hmgX, hmgY, 100, 100, 11, 1},
                        };
                        std::vector<ss2::assets::Infantry> infWithOff{
                            {"rifle_5c", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5c", hmgX, hmgY, 100, 100, 11, 1},
                            {"officer_5c", offX, offY, 100, 0, 1, 0},
                        };
                        ss2::TutorialUnits simNoOff(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            infNoOff, emptyVeh5c, ss2::FogGrid(64, 64, flags5c), 2, true, {}, cat5cB);
                        ss2::TutorialUnits simWithOff(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            infWithOff, emptyVeh5c, ss2::FogGrid(64, 64, flags5c), 2, true, {}, cat5cB);

                        // Force identical HMG target (index 0) and watch suppression begin & recover.
                        const std::vector<size_t> rifOnly{0};
                        (void)simNoOff.toggleHoldFire(rifOnly);
                        (void)simWithOff.toggleHoldFire(rifOnly);

                        bool noOffSupNow = false;
                        bool withOffSupNow = false;
                        bool sawNoOffSup = false;
                        bool sawWithOffSup = false;
                        int noOffClearTick = -1;
                        int withOffClearTick = -1;
                        for (int t = 1; t <= 75; ++t) {
                            for (const auto& p : simNoOff.step()) {
                                if (p.index == 0u) noOffSupNow = p.suppressed;
                            }
                            for (const auto& p : simWithOff.step()) {
                                if (p.index == 0u) withOffSupNow = p.suppressed;
                            }
                            if (noOffSupNow) sawNoOffSup = true;
                            else if (sawNoOffSup && noOffClearTick < 0) noOffClearTick = t;
                            if (withOffSupNow) sawWithOffSup = true;
                            else if (sawWithOffSup && withOffClearTick < 0) withOffClearTick = t;
                        }
                        if (!sawNoOffSup || !sawWithOffSup || withOffClearTick < 0 || noOffClearTick < 0 ||
                            withOffClearTick >= noOffClearTick) {
                            throw std::runtime_error("Phase 5C HMG suppression or Officer rally aura check failed");
                        }

                        // Verify 1.35x reload penalty slows suppressed infantry fire rate.
                        ss2::assets::UnitSpec sustainedHmg = hmg5c;
                        sustainedHmg.ammo1 = 60;
                        auto cat5cRate = ss2::assets::UnitCatalog::fromSpecs({sustainedHmg, rifle5c});
                        std::vector<ss2::assets::Infantry> infFree{
                            {"rifle_5c", rifX, rifY, 100, 100, 1, 0},
                            {"rifle_5c", hmgX, hmgY, 100, 100, 11, 1},
                        };
                        std::vector<ss2::assets::Infantry> infPinned{
                            {"rifle_5c", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5c", hmgX, hmgY, 100, 100, 11, 1},
                        };
                        ss2::TutorialUnits simFree(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            infFree, emptyVeh5c, ss2::FogGrid(64, 64, flags5c), 2, true, {}, cat5cRate);
                        ss2::TutorialUnits simPinned(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            infPinned, emptyVeh5c, ss2::FogGrid(64, 64, flags5c), 2, true, {}, cat5cRate);
                        for (int t = 0; t < 55; ++t) {
                            (void)simFree.step();
                            (void)simPinned.step();
                        }
                        const int freeShots = 60 - simFree.takeSnapshot().soldiers[0].ammo;
                        const int pinnedShots = 60 - simPinned.takeSnapshot().soldiers[0].ammo;
                        if (freeShots <= 0 || pinnedShots >= freeShots) {
                            throw std::runtime_error("Phase 5C suppressed reload penalty did not reduce fire rate");
                        }
                    }

                    // (C) Tracked vs wheeled (engine track / whell) shallows mobility penalty.
                    {
                        ss2::assets::UnitSpec track5c{};
                        track5c.id = "track_5c";
                        track5c.isVehicle = true;
                        track5c.engineType = "track";
                        track5c.health = 200;
                        track5c.moveSpeed = 10.0;
                        track5c.sight = 6;

                        ss2::assets::UnitSpec wheel5c{};
                        wheel5c.id = "wheel_5c";
                        wheel5c.isVehicle = true;
                        wheel5c.engineType = "whell";
                        wheel5c.health = 200;
                        wheel5c.moveSpeed = 10.0;
                        wheel5c.sight = 6;

                        auto cat5cC = ss2::assets::UnitCatalog::fromSpecs({track5c, wheel5c});
                        std::vector<std::uint32_t> flags5c(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5c(64u * 64u, 0x000Fu);
                        for (int tx = 18; tx <= 34; ++tx) {
                            obst5c[20u * 64u + static_cast<std::size_t>(tx)] = 0x0F00u; // shallows/ford corridor
                        }
                        auto grid5cC = ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c);
                        if (!grid5cC.isShallows({40, 40}) || grid5cC.isShallows({40, 60})) {
                            throw std::runtime_error("Phase 5C InfantryPathGrid::isShallows query failed");
                        }
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [sX, sY] = tc64(20, 20);
                        const auto [dX, dY] = tc64(30, 20);
                        std::vector<ss2::assets::Infantry> emptyInf5c{};
                        std::vector<ss2::assets::Placement> vehTrack{{"track_5c", sX, sY, 0, 100, 0, 1, 0}};
                        std::vector<ss2::assets::Placement> vehWheel{{"wheel_5c", sX, sY, 0, 100, 0, 1, 0}};
                        ss2::TutorialUnits simTrack(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            emptyInf5c, vehTrack, ss2::FogGrid(64, 64, flags5c), 2, false, {}, cat5cC);
                        ss2::TutorialUnits simWheel(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5c, obst5c),
                            emptyInf5c, vehWheel, ss2::FogGrid(64, 64, flags5c), 2, false, {}, cat5cC);
                        const std::vector<size_t> v0{0};
                        if (simTrack.moveVehicles(v0, dX, dY) != 1u || simWheel.moveVehicles(v0, dX, dY) != 1u) {
                            throw std::runtime_error("Phase 5C moveVehicles across shallows rejected");
                        }
                        for (int t = 0; t < 12; ++t) {
                            (void)simTrack.step();
                            (void)simWheel.step();
                        }
                        const auto stTrack = simTrack.takeSnapshot().vehicles[0];
                        const auto stWheel = simWheel.takeSnapshot().vehicles[0];
                        const double trackDist = std::hypot(double(stTrack.x - sX), double(stTrack.y - sY));
                        const double wheelDist = std::hypot(double(stWheel.x - sX), double(stWheel.y - sY));
                        if (wheelDist <= 0.0 || trackDist <= wheelDist * 1.25) {
                            throw std::runtime_error("Phase 5C tracked vs wheeled shallows speed penalty mismatch");
                        }
                    }

                    std::cout << "Phase 5C attackpref target-priority, HMG suppression/Officer rally & shallows mobility PASSED\n";

                    // Phase 5D: retail AVIA PlaneSpec sorties, gunturndelay turret traverse, canbecrushed
                    // light-gun crushing & passcanfire open-top passenger firing.
                    // (A) Retail PlaneSpec sortie HP/speed/bombs/suppression & real PlaneShotDown scoreValue.
                    {
                        ss2::assets::UnitSpec flak5d{};
                        flak5d.id = "flak5d";
                        flak5d.isVehicle = true;
                        flak5d.selType = "antiaircraft";
                        flak5d.health = 400;
                        flak5d.ammo1 = 60;
                        flak5d.sight = 14;
                        flak5d.shot1 = {"aa5d", ss2::assets::HitType::Explosive, 40, 40, 200, 200, 0, 10, 1, 10, "gunshot"};

                        ss2::assets::UnitSpec inf5d{};
                        inf5d.id = "inf5d";
                        inf5d.health = 2500;
                        inf5d.ammo1 = 10;
                        inf5d.sight = 8;

                        ss2::assets::PlaneSpec b17{};
                        b17.id = "b-17c";
                        b17.health = 2050;
                        b17.scoreValue = 7100;
                        b17.sight = 24;
                        b17.flySpeed = 6.97;
                        b17.bombDamage = 3000;
                        b17.bombsNumber = 8;

                        ss2::assets::PlaneSpec bf109{};
                        bf109.id = "bf-109";
                        bf109.health = 300;
                        bf109.scoreValue = 600;
                        bf109.sight = 16;
                        bf109.flySpeed = 13.0;
                        bf109.groundDamage = 45;
                        bf109.airDamage = 45;

                        auto cat5dA = ss2::assets::UnitCatalog::fromSpecs({flak5d, inf5d}, {b17, bf109});
                        std::vector<std::uint32_t> flags5d(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5d(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [tgtX, tgtY] = tc64(42, 20); // right-side corridor (~2752, ~1008)
                        std::vector<ss2::assets::Infantry> infBomb{{"inf5d", tgtX, tgtY, 100, 10, 1, 1}};
                        std::vector<ss2::assets::Placement> vehBomb{};
                        ss2::TutorialUnits simBomb(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5d, obst5d),
                            infBomb, vehBomb, ss2::FogGrid(64, 64, flags5d), 2, false, {}, cat5dA);
                        std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air5d{};
                        air5d[0][0] = {"b-17c", 1, 1};
                        air5d[1][4] = {"bf-109", 1, 1};
                        std::array<int, 4> pGrp5d{};
                        std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos5d{};
                        simBomb.setAirSupport(air5d, pGrp5d, pRos5d);
                        if (!simBomb.callAirSupport(ss2::assets::AirMissionKind::Bomber, tgtX, tgtY, 0)) {
                            throw std::runtime_error("Phase 5D b-17c callAirSupport failed");
                        }
                        const auto initSorties = simBomb.takeAirSorties();
                        if (initSorties.empty() || initSorties[0].maxHp != 2050) {
                            throw std::runtime_error("Phase 5D PlaneSpec b-17c maxHp mismatch");
                        }
                        int bombDrops = 0;
                        bool sawBombSuppressed = false;
                        for (int t = 0; t < 260; ++t) {
                            for (const auto& p : simBomb.step()) {
                                if (p.index == 0u && p.suppressed) sawBombSuppressed = true;
                            }
                            for (const auto& ae : simBomb.takeAirStrikeEvents()) {
                                if (ae.kind == ss2::AirStrikeEvent::BombImpact) ++bombDrops;
                            }
                        }
                        if (bombDrops != 8 || !sawBombSuppressed) {
                            throw std::runtime_error("Phase 5D PlaneSpec bomber drop count or suppression mismatch");
                        }

                        // Hostile bf-109 shot down by AA emits PlaneShotDown with real scoreValue=600.
                        const auto [fGoalX, fGoalY] = tc64(14, 48); // ~1312px southwest from (42, 20)
                        std::vector<ss2::assets::Infantry> emptyInf5d{};
                        const auto [aa1X, aa1Y] = tc64(36, 26);
                        const auto [aa2X, aa2Y] = tc64(33, 29);
                        const auto [aa3X, aa3Y] = tc64(30, 32);
                        std::vector<ss2::assets::Placement> vehAa{
                            {"flak5d", aa1X, aa1Y, 0, 100, 100, 1, 0},
                            {"flak5d", aa2X, aa2Y, 0, 100, 100, 1, 0},
                            {"flak5d", aa3X, aa3Y, 0, 100, 100, 1, 0},
                        };
                        ss2::TutorialUnits simAa(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5d, obst5d),
                            emptyInf5d, vehAa, ss2::FogGrid(64, 64, flags5d), 2, false, {}, cat5dA);
                        simAa.setAirSupport(air5d, pGrp5d, pRos5d);
                        if (!simAa.callAirSupport(ss2::assets::AirMissionKind::Fighter, fGoalX, fGoalY, 1)) {
                            throw std::runtime_error("Phase 5D bf-109 callAirSupport failed");
                        }
                        int downScore = 0;
                        std::string downId;
                        for (int t = 0; t < 180; ++t) {
                            (void)simAa.step();
                            for (const auto& ae : simAa.takeAirStrikeEvents()) {
                                if (ae.kind == ss2::AirStrikeEvent::PlaneShotDown) {
                                    downScore = ae.scoreValue;
                                    downId = ae.planeId;
                                }
                            }
                            if (downScore > 0) break;
                        }
                        if (downScore != 600 || downId != "bf-109") {
                            throw std::runtime_error("Phase 5D PlaneShotDown scoreValue/planeId mismatch");
                        }
                    }

                    // (B) gunturndelay turret traverse speed & aim-alignment gate before firing.
                    {
                        ss2::assets::UnitSpec fastTurret{};
                        fastTurret.id = "fast_5d";
                        fastTurret.isVehicle = true;
                        fastTurret.health = 500;
                        fastTurret.ammo1 = 20;
                        fastTurret.sight = 14;
                        fastTurret.hasGunTurnDelay = true;
                        fastTurret.gunTurnDelay = 0; // turretStep32 = 6
                        fastTurret.shot1 = {"ap5d", ss2::assets::HitType::Pierce, 60, 60, 400, 400, 0, 8, 1, 8, "gunshot"};

                        ss2::assets::UnitSpec slowTurret = fastTurret;
                        slowTurret.id = "slow_5d";
                        slowTurret.gunTurnDelay = 14; // turretStep32 = 1

                        ss2::assets::UnitSpec tgtVeh{};
                        tgtVeh.id = "tgt_5d";
                        tgtVeh.isVehicle = true;
                        tgtVeh.health = 800;
                        tgtVeh.sight = 4;

                        auto cat5dB = ss2::assets::UnitCatalog::fromSpecs({fastTurret, slowTurret, tgtVeh});
                        std::vector<std::uint32_t> flags5d(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5d(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [tkX, tkY] = tc64(20, 20);
                        const auto [enX, enY] = tc64(16, 20); // dir 16 (directly behind dir 0)
                        std::vector<ss2::assets::Infantry> emptyInf5d{};
                        std::vector<ss2::assets::Placement> vehFast{
                            {"fast_5d", tkX, tkY, 0, 100, 100, 1, 0},
                            {"tgt_5d", enX, enY, 0, 100, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> vehSlow{
                            {"slow_5d", tkX, tkY, 0, 100, 100, 1, 0},
                            {"tgt_5d", enX, enY, 0, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits simFast(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5d, obst5d),
                            emptyInf5d, vehFast, ss2::FogGrid(64, 64, flags5d), 2, true, {}, cat5dB);
                        ss2::TutorialUnits simSlow(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5d, obst5d),
                            emptyInf5d, vehSlow, ss2::FogGrid(64, 64, flags5d), 2, true, {}, cat5dB);

                        for (int t = 0; t < 6; ++t) {
                            (void)simFast.step();
                            (void)simSlow.step();
                        }
                        const auto snapFast6 = simFast.takeSnapshot();
                        const auto snapSlow6 = simSlow.takeSnapshot();
                        if (snapFast6.vehicles[0].ammo >= 20 || snapSlow6.vehicles[0].ammo != 20 ||
                            snapSlow6.vehicles[0].turretDir == 0u) {
                            throw std::runtime_error("Phase 5D gunturndelay fast vs slow early traverse check failed");
                        }
                        for (int t = 0; t < 16; ++t) (void)simSlow.step();
                        const auto snapSlow22 = simSlow.takeSnapshot();
                        if (snapSlow22.vehicles[0].ammo >= 20 || snapSlow22.vehicles[1].hp >= 800) {
                            throw std::runtime_error("Phase 5D slow turret never finished traversing & firing");
                        }
                    }

                    // (C) canbecrushed light-gun crush & passcanfire open-top passenger firing.
                    {
                        ss2::assets::UnitSpec crusher{};
                        crusher.id = "crush_5d";
                        crusher.isVehicle = true;
                        crusher.hasCrushRange = true;
                        crusher.health = 800;
                        crusher.moveSpeed = 10.0;
                        crusher.sight = 8;

                        ss2::assets::UnitSpec mortar5d{};
                        mortar5d.id = "mortar_5d";
                        mortar5d.isVehicle = true;
                        mortar5d.canBeCrushed = true;
                        mortar5d.health = 200;
                        mortar5d.sight = 4;

                        ss2::assets::UnitSpec jeep5d{};
                        jeep5d.id = "jeep_5d";
                        jeep5d.isVehicle = true;
                        jeep5d.passengersCanFire = true;
                        jeep5d.stock = 4;
                        jeep5d.health = 200;
                        jeep5d.sight = 10;

                        ss2::assets::UnitSpec rider5d{};
                        rider5d.id = "rider_5d";
                        rider5d.health = 100;
                        rider5d.ammo1 = 40;
                        rider5d.sight = 10;
                        rider5d.shot1 = {"r5d", ss2::assets::HitType::Machine, 15, 15, 220, 220, 0, 5, 1, 5, "gunshot"};

                        ss2::assets::UnitSpec enemyInf5d{};
                        enemyInf5d.id = "target_inf_5d";
                        enemyInf5d.health = 250;
                        enemyInf5d.ammo1 = 0;
                        enemyInf5d.sight = 4;

                        auto cat5dC = ss2::assets::UnitCatalog::fromSpecs({crusher, mortar5d, jeep5d, rider5d, enemyInf5d});
                        std::vector<std::uint32_t> flags5d(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5d(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [sX, sY] = tc64(20, 20);
                        const auto [mX, mY] = tc64(22, 20);
                        const auto [dX, dY] = tc64(26, 20);
                        const auto [eX, eY] = tc64(24, 20); // 128px east: >88px AT grenade range, <220px passenger fire
                        std::vector<ss2::assets::Infantry> emptyInf5d{};
                        std::vector<ss2::assets::Placement> vehCrush{
                            {"crush_5d", sX, sY, 0, 100, 0, 1, 0},
                            {"mortar_5d", mX, mY, 16, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits simCrush(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5d, obst5d),
                            emptyInf5d, vehCrush, ss2::FogGrid(64, 64, flags5d), 2, false, {}, cat5dC);
                        const std::vector<size_t> c0{0};
                        if (simCrush.moveVehicles(c0, dX, dY) != 1u) {
                            throw std::runtime_error("Phase 5D moveVehicles crusher failed");
                        }
                        for (int t = 0; t < 15; ++t) (void)simCrush.step();
                        const auto snapCrush = simCrush.takeSnapshot();
                        if (snapCrush.vehicles[1].alive || snapCrush.vehicles[0].kills < 1) {
                            throw std::runtime_error("Phase 5D heavy tank failed to crush canbecrushed mortar");
                        }

                        std::vector<ss2::assets::Infantry> infJeep{
                            {"rider_5d", sX, sY, 100, 100, 1, 0},
                            {"target_inf_5d", eX, eY, 250, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> vehJeep{{"jeep_5d", sX, sY, 0, 100, 0, 1, 0}};
                        ss2::TutorialUnits simJeep(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5d, obst5d),
                            infJeep, vehJeep, ss2::FogGrid(64, 64, flags5d), 2, true, {}, cat5dC);
                        if (!simJeep.embarkImmediate(0, 0)) {
                            throw std::runtime_error("Phase 5D embarkImmediate failed for open-top jeep");
                        }
                        for (int t = 0; t < 25; ++t) (void)simJeep.step();
                        const auto snapJeep = simJeep.takeSnapshot();
                        if (!snapJeep.soldiers[0].embarked || snapJeep.soldiers[0].ammo >= 40 ||
                            snapJeep.soldiers[1].hp >= 250) {
                            throw std::runtime_error(
                                "Phase 5D open-top passcanfire failed (emb=" +
                                std::to_string(snapJeep.soldiers[0].embarked) +
                                " ammo=" + std::to_string(snapJeep.soldiers[0].ammo) +
                                " tgtHp=" + std::to_string(snapJeep.soldiers[1].hp) + ")");
                        }
                    }

                    std::cout << "Phase 5D retail AVIA catalog, gunturndelay traverse, gun crushing & open-top firing PASSED\n";

                    // Phase 5E: retail shot*_accuracy bombardment scatter, shot*_expa vehicle XP,
                    // open-top bonus_shotrange, seltype agit rally aura & lossless SS2SNAP3 save/load.
                    // (A) shot1_expa XP award, shot1_accuracy attackGround scatter & open-top bonus_shotrange.
                    {
                        ss2::assets::UnitSpec xpTank{};
                        xpTank.id = "xp_5e";
                        xpTank.isVehicle = true;
                        xpTank.health = 500;
                        xpTank.ammo1 = 20;
                        xpTank.sight = 14;
                        xpTank.shot1 = {"ap5e", ss2::assets::HitType::Pierce, 40, 40, 400, 400, 0, 5, 1, 5, "gunshot", 33, 29, 88};

                        ss2::assets::UnitSpec how5e{};
                        how5e.id = "how_5e";
                        how5e.isVehicle = true;
                        how5e.selType = "howitzer";
                        how5e.health = 400;
                        how5e.ammo1 = 20;
                        how5e.sight = 14;
                        how5e.shot1 = {"he5e", ss2::assets::HitType::Explosive, 80, 80, 500, 500, 0, 2, 1, 2, "gunshot", 196, 176, 40};

                        ss2::assets::UnitSpec target5e{};
                        target5e.id = "tgt_5e";
                        target5e.isVehicle = true;
                        target5e.health = 2000;
                        target5e.sight = 4;

                        ss2::assets::UnitSpec jeepNoBonus{};
                        jeepNoBonus.id = "jeep_plain_5e";
                        jeepNoBonus.isVehicle = true;
                        jeepNoBonus.passengersCanFire = true;
                        jeepNoBonus.bonusShotRange = 0;
                        jeepNoBonus.stock = 4;
                        jeepNoBonus.health = 200;
                        jeepNoBonus.sight = 12;

                        ss2::assets::UnitSpec jeepBonus = jeepNoBonus;
                        jeepBonus.id = "jeep_bonus_5e";
                        jeepBonus.bonusShotRange = 20;

                        ss2::assets::UnitSpec shooter5e{};
                        shooter5e.id = "shoot_5e";
                        shooter5e.health = 100;
                        shooter5e.ammo1 = 40;
                        shooter5e.sight = 12;
                        shooter5e.shot1 = {"r5e", ss2::assets::HitType::Machine, 15, 15, 190, 190, 0, 5, 1, 5, "gunshot"};

                        ss2::assets::UnitSpec dummy5e{};
                        dummy5e.id = "dummy_5e";
                        dummy5e.health = 250;
                        dummy5e.ammo1 = 0;
                        dummy5e.sight = 4;

                        auto cat5eA = ss2::assets::UnitCatalog::fromSpecs({
                            xpTank, how5e, target5e, jeepNoBonus, jeepBonus, shooter5e, dummy5e
                        });
                        std::vector<std::uint32_t> flags5e(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5e(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [x0, y0] = tc64(20, 20);
                        const auto [x1, y1] = tc64(24, 20);
                        std::vector<ss2::assets::Infantry> emptyInf5e{};
                        std::vector<ss2::assets::Placement> vehXp{
                            {"xp_5e", x0, y0, 0, 100, 100, 1, 0},
                            {"tgt_5e", x1, y1, 16, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits simXp(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5e, obst5e),
                            emptyInf5e, vehXp, ss2::FogGrid(64, 64, flags5e), 2, true, {}, cat5eA);
                        for (int t = 0; t < 6; ++t) (void)simXp.step();
                        const auto snapXp = simXp.takeSnapshot();
                        if (snapXp.vehicles[0].expa < 88 || snapXp.vehicles[1].hp <= 0) {
                            throw std::runtime_error("Phase 5E shot1_expa did not award per-shot vehicle XP");
                        }

                        // Accuracy bombardment scatter check.
                        const auto [gx, gy] = tc64(28, 20);
                        std::vector<ss2::assets::Placement> vehHow{{"how_5e", x0, y0, 0, 100, 100, 1, 0}};
                        ss2::TutorialUnits simHow(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5e, obst5e),
                            emptyInf5e, vehHow, ss2::FogGrid(64, 64, flags5e), 2, false, {}, cat5eA);
                        const std::vector<size_t> howIds{0};
                        if (simHow.orderAttackGroundVehicles(howIds, gx, gy) != 1u) {
                            throw std::runtime_error("Phase 5E orderAttackGroundVehicles rejected");
                        }
                        bool sawScatteredShot = false;
                        for (int t = 0; t < 20; ++t) {
                            (void)simHow.step();
                            for (const auto& vp : simHow.takeVehiclePoses()) {
                                if (vp.fired && (vp.targetX != gx || vp.targetY != gy)) {
                                    const double off = std::hypot(double(vp.targetX - gx), double(vp.targetY - gy));
                                    if (off <= 72.0) sawScatteredShot = true;
                                }
                            }
                        }
                        if (!sawScatteredShot) {
                            throw std::runtime_error("Phase 5E shot1_accuracy bombardment did not scatter impacts");
                        }

                        // Open-top bonus_shotrange (+20) extends embarked passenger engagement reach to 192px.
                        const auto [exFar, eyFar] = tc64(26, 20); // 6*32 = 192px (>180 base, <=200 bonus)
                        std::vector<ss2::assets::Infantry> infPlain{
                            {"shoot_5e", x0, y0, 100, 100, 1, 0},
                            {"dummy_5e", exFar, eyFar, 250, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Infantry> infBonus = infPlain;
                        std::vector<ss2::assets::Placement> vehPlain{{"jeep_plain_5e", x0, y0, 0, 100, 0, 1, 0}};
                        std::vector<ss2::assets::Placement> vehBonus{{"jeep_bonus_5e", x0, y0, 0, 100, 0, 1, 0}};
                        ss2::TutorialUnits simPlain(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5e, obst5e),
                            infPlain, vehPlain, ss2::FogGrid(64, 64, flags5e), 2, true, {}, cat5eA);
                        ss2::TutorialUnits simBonus(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5e, obst5e),
                            infBonus, vehBonus, ss2::FogGrid(64, 64, flags5e), 2, true, {}, cat5eA);
                        (void)simPlain.embarkImmediate(0, 0);
                        (void)simBonus.embarkImmediate(0, 0);
                        for (int t = 0; t < 25; ++t) {
                            (void)simPlain.step();
                            (void)simBonus.step();
                        }
                        if (simPlain.takeSnapshot().soldiers[0].ammo != 40 ||
                            simBonus.takeSnapshot().soldiers[0].ammo >= 40) {
                            throw std::runtime_error("Phase 5E open-top bonus_shotrange failed to extend passenger reach");
                        }
                    }

                    // (B) seltype agit propaganda vehicle rally aura accelerates suppression recovery (3u/tick <= 200px).
                    {
                        ss2::assets::UnitSpec hmg5e{};
                        hmg5e.id = "dhmg5e";
                        hmg5e.selType = "hmg";
                        hmg5e.health = 200;
                        hmg5e.ammo1 = 8;
                        hmg5e.sight = 14;
                        hmg5e.shot1 = {"mg5e", ss2::assets::HitType::Machine, 2, 2, 260, 260, 0, 2, 1, 2, "gunshot"};

                        ss2::assets::UnitSpec rif5e{};
                        rif5e.id = "rif_5e";
                        rif5e.health = 500;
                        rif5e.ammo1 = 60;
                        rif5e.sight = 14;

                        ss2::assets::UnitSpec agit5e{};
                        agit5e.id = "agit_5e";
                        agit5e.isVehicle = true;
                        agit5e.selType = "agit";
                        agit5e.health = 200;
                        agit5e.sight = 10;

                        auto cat5eB = ss2::assets::UnitCatalog::fromSpecs({hmg5e, rif5e, agit5e});
                        std::vector<std::uint32_t> flags5e(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5e(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [hmgX, hmgY] = tc64(20, 20);
                        const auto [rifX, rifY] = tc64(22, 20);
                        const auto [agX, agY] = tc64(22, 22);
                        std::vector<ss2::assets::Infantry> inf5eB{
                            {"rif_5e", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5e", hmgX, hmgY, 100, 100, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> vehNone{};
                        std::vector<ss2::assets::Placement> vehAgit{{"agit_5e", agX, agY, 0, 100, 0, 1, 0}};
                        ss2::TutorialUnits simNoAgit(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5e, obst5e),
                            inf5eB, vehNone, ss2::FogGrid(64, 64, flags5e), 2, true, {}, cat5eB);
                        ss2::TutorialUnits simAgit(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5e, obst5e),
                            inf5eB, vehAgit, ss2::FogGrid(64, 64, flags5e), 2, true, {}, cat5eB);
                        const std::vector<size_t> r0{0};
                        (void)simNoAgit.toggleHoldFire(r0);
                        (void)simAgit.toggleHoldFire(r0);

                        bool noAgitSup = false, agitSup = false, sawNoAgit = false, sawAgit = false;
                        int noAgitClear = -1, agitClear = -1;
                        for (int t = 1; t <= 75; ++t) {
                            for (const auto& p : simNoAgit.step()) if (p.index == 0u) noAgitSup = p.suppressed;
                            for (const auto& p : simAgit.step()) if (p.index == 0u) agitSup = p.suppressed;
                            if (noAgitSup) sawNoAgit = true; else if (sawNoAgit && noAgitClear < 0) noAgitClear = t;
                            if (agitSup) sawAgit = true; else if (sawAgit && agitClear < 0) agitClear = t;
                        }
                        if (!sawNoAgit || !sawAgit || agitClear < 0 || noAgitClear < 0 || agitClear >= noAgitClear) {
                            throw std::runtime_error("Phase 5E agit propaganda vehicle failed to rally suppression");
                        }
                    }

                    // (C) Lossless SS2SNAP3 persistence of suppressionTicks, hasCrew & crewHp (+ crewImpaired).
                    {
                        ss2::assets::UnitSpec hmgS3{};
                        hmgS3.id = "dhmg_s3";
                        hmgS3.selType = "hmg";
                        hmgS3.health = 200;
                        hmgS3.ammo1 = 4;
                        hmgS3.sight = 14;
                        hmgS3.shot1 = {"mgs3", ss2::assets::HitType::Machine, 2, 2, 260, 260, 0, 2, 1, 2, "gunshot"};

                        ss2::assets::UnitSpec rifS3{};
                        rifS3.id = "rif_s3";
                        rifS3.health = 500;
                        rifS3.ammo1 = 40;
                        rifS3.sight = 14;

                        ss2::assets::UnitSpec snipS3{};
                        snipS3.id = "sniper_s3";
                        snipS3.selType = "sniper";
                        snipS3.health = 100;
                        snipS3.ammo1 = 1;
                        snipS3.sight = 14;
                        snipS3.shot1 = {"sn_s3", ss2::assets::HitType::Sniper, 125, 125, 320, 320, 0, 10, 1, 10, "gunshot"};

                        ss2::assets::UnitSpec crewedVeh{};
                        crewedVeh.id = "crew_5e";
                        crewedVeh.isVehicle = true;
                        crewedVeh.health = 1200;
                        crewedVeh.crewNumber = 4;
                        crewedVeh.attackCrew = 3;
                        crewedVeh.moveCrew = 1;
                        crewedVeh.hasCrewProtection = true;
                        crewedVeh.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {0, 0, 0};
                        crewedVeh.sight = 10;

                        auto cat5eC = ss2::assets::UnitCatalog::fromSpecs({hmgS3, rifS3, snipS3, crewedVeh});
                        std::vector<std::uint32_t> flags5e(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5e(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [aX, aY] = tc64(20, 20);
                        const auto [bX, bY] = tc64(22, 20);
                        const auto [cX, cY] = tc64(20, 28);
                        const auto [dX, dY] = tc64(22, 28);
                        std::vector<ss2::assets::Infantry> infS3{
                            {"rif_s3", bX, bY, 100, 100, 1, 0},     // soldier 0: gets suppressed by enemy HMG
                            {"dhmg_s3", aX, aY, 100, 100, 11, 1},   // soldier 1: hostile HMG
                            {"sniper_s3", cX, cY, 100, 100, 2, 0},  // soldier 2: friendly sniper
                        };
                        std::vector<ss2::assets::Placement> vehS3{
                            {"crew_5e", dX, dY, 0, 100, 0, 11, 1},  // vehicle 0: gets crew damaged (< attackCrew)
                            {"crew_5e", aX, aY, 0, 100, 0, 12, 3},  // vehicle 1: neutral unmanned (hasCrew=false)
                        };
                        ss2::TutorialUnits simS3(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5e, obst5e),
                            infS3, vehS3, ss2::FogGrid(64, 64, flags5e), 2, true, {}, cat5eC);
                        const std::vector<size_t> rHold{0};
                        const std::vector<size_t> snipIds{2};
                        (void)simS3.toggleHoldFire(rHold);
                        if (simS3.attackVehicle(snipIds, 0) != 1u) {
                            throw std::runtime_error("Phase 5E sniper attackVehicle setup failed");
                        }
                        for (int t = 0; t < 16; ++t) (void)simS3.step();

                        const auto snapSave = simS3.takeSnapshot();
                        if (snapSave.soldiers[0].suppressionTicks <= 0 ||
                            !snapSave.vehicles[0].hasCrew || snapSave.vehicles[0].crewHp >= 210 ||
                            snapSave.vehicles[1].hasCrew || snapSave.vehicles[1].crewHp != 0) {
                            throw std::runtime_error("Phase 5E SS2SNAP3 pre-save state setup incomplete");
                        }
                        const auto bytesS3 = ss2::TutorialUnits::serializeSnapshot(snapSave);
                        if (bytesS3.size() < 8u || std::string_view(reinterpret_cast<const char*>(bytesS3.data()), 8u) != "SS2SNAP3") {
                            throw std::runtime_error("Phase 5E snapshot magic header is not SS2SNAP3");
                        }
                        const auto decS3 = ss2::TutorialUnits::deserializeSnapshot(bytesS3);
                        if (!decS3 || !(*decS3 == snapSave)) {
                            throw std::runtime_error("Phase 5E SS2SNAP3 serialize/deserialize equality failed");
                        }
                        for (int t = 0; t < 40; ++t) (void)simS3.step(); // decay suppressionTicks in live sim
                        if (simS3.takeSnapshot().soldiers[0].suppressionTicks != 0) {
                            throw std::runtime_error("Phase 5E post-save suppressionTicks did not decay before load");
                        }
                        std::vector<ss2::SoldierPose> restoredPoses;
                        if (!simS3.loadSnapshot(*decS3, &restoredPoses) || !(simS3.takeSnapshot() == snapSave)) {
                            throw std::runtime_error("Phase 5E SS2SNAP3 loadSnapshot did not losslessly restore state");
                        }
                        bool sawRestoredSup = false;
                        for (const auto& p : restoredPoses) {
                            if (p.index == 0u && p.suppressed) sawRestoredSup = true;
                        }
                        bool sawImpairedPose = false;
                        for (const auto& vp : simS3.takeVehiclePoses()) {
                            if (vp.index == 0u && vp.crewImpaired) sawImpairedPose = true;
                        }
                        if (!sawRestoredSup || !sawImpairedPose) {
                            throw std::runtime_error("Phase 5E SS2SNAP3 restored poses missing suppressed or crewImpaired flag");
                        }
                    }

                    std::cout << "Phase 5E retail ballistics/expa, bonus_shotrange, agit rally, AA seltype & SS2SNAP3 PASSED\n";

                    // Phase 5F: soldonarmor Tank Desant hull-rider exposure, General wide aura & moraleresist,
                    // retail reload1/reload2 ammo resupply costs & AVIA AirStrikeEvent planeId audio propagation.
                    // (A) Tank Desant (soldonarmor=true) riders exposed to hostile fire vs protected closed APC.
                    {
                        ss2::assets::UnitSpec tankDesant{};
                        tankDesant.id = "desant_tank_5f";
                        tankDesant.isVehicle = true;
                        tankDesant.soldiersOnArmor = true;
                        tankDesant.stock = 5;
                        tankDesant.health = 1200;
                        tankDesant.sight = 14;
                        tankDesant.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {256, 256, 256};

                        ss2::assets::UnitSpec apcClosed = tankDesant;
                        apcClosed.id = "apc_closed_5f";
                        apcClosed.soldiersOnArmor = false;

                        ss2::assets::UnitSpec rider5f{};
                        rider5f.id = "rider_5f";
                        rider5f.health = 30;
                        rider5f.ammo1 = 0;
                        rider5f.sight = 8;

                        ss2::assets::UnitSpec mgVeh5f{};
                        mgVeh5f.id = "mgveh_5f";
                        mgVeh5f.isVehicle = true;
                        mgVeh5f.health = 400;
                        mgVeh5f.ammo1 = 100;
                        mgVeh5f.sight = 14;
                        mgVeh5f.shot1 = {"mg5f", ss2::assets::HitType::Machine, 40, 40, 260, 260, 0, 3, 8, 20, "gunshot"};

                        auto cat5fA = ss2::assets::UnitCatalog::fromSpecs({tankDesant, apcClosed, rider5f, mgVeh5f});
                        std::vector<std::uint32_t> flags5f(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5f(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [tX, tY] = tc64(20, 20);
                        const auto [eX, eY] = tc64(24, 20); // ~143px away (> 88px grenade range)
                        std::vector<ss2::assets::Infantry> inf5f{
                            {"rider_5f", tX, tY, 30, 0, 1, 0},
                            {"rider_5f", tX, tY, 30, 0, 1, 0},
                        };
                        std::vector<ss2::assets::Placement> vehDesant{
                            {"desant_tank_5f", tX, tY, 0, 100, 0, 1, 0},
                            {"mgveh_5f", eX, eY, 16, 100, 100, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> vehApc{
                            {"apc_closed_5f", tX, tY, 0, 100, 0, 1, 0},
                            {"mgveh_5f", eX, eY, 16, 100, 100, 11, 1},
                        };
                        ss2::TutorialUnits simDesant(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            inf5f, vehDesant, ss2::FogGrid(64, 64, flags5f), 2, true, {}, cat5fA);
                        ss2::TutorialUnits simApc(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            inf5f, vehApc, ss2::FogGrid(64, 64, flags5f), 2, true, {}, cat5fA);
                        if (!simDesant.embarkImmediate(0, 0) || !simDesant.embarkImmediate(1, 0) ||
                            !simApc.embarkImmediate(0, 0) || !simApc.embarkImmediate(1, 0)) {
                            throw std::runtime_error("Phase 5F embarkImmediate setup failed");
                        }
                        for (int t = 0; t < 20; ++t) {
                            (void)simDesant.step();
                            (void)simApc.step();
                        }
                        const auto snapDes = simDesant.takeSnapshot();
                        const auto snapApc = simApc.takeSnapshot();
                        if (snapDes.vehicles[0].hp != 1200 || snapDes.soldiers[1].hp != 0 ||
                            snapDes.soldiers[1].embarked || snapDes.vehicles[0].passengers.size() >= 2u ||
                            !snapApc.soldiers[0].embarked || !snapApc.soldiers[1].embarked ||
                            snapApc.soldiers[0].hp != 30 || snapApc.soldiers[1].hp != 30) {
                            throw std::runtime_error("Phase 5F Tank Desant soldonarmor exposure check failed");
                        }
                    }

                    // (B) General wide aura (officerradius 350, moraleautoincrease 10) vs Officer (160) & moraleresist.
                    {
                        ss2::assets::UnitSpec hmg5f{};
                        hmg5f.id = "dhmg5f";
                        hmg5f.selType = "hmg";
                        hmg5f.health = 200;
                        hmg5f.ammo1 = 8;
                        hmg5f.sight = 14;
                        hmg5f.shot1 = {"mg5f", ss2::assets::HitType::Machine, 2, 2, 100, 100, 0, 2, 1, 2, "gunshot"};

                        ss2::assets::UnitSpec rif5f{};
                        rif5f.id = "rif_5f";
                        rif5f.health = 500;
                        rif5f.ammo1 = 60;
                        rif5f.sight = 14;

                        ss2::assets::UnitSpec off5f{};
                        off5f.id = "dofficer_5f";
                        off5f.selType = "officer";
                        off5f.health = 300;
                        off5f.sight = 14;
                        off5f.officerRadius = 160;
                        off5f.moraleAutoIncrease = 5;
                        off5f.moraleResist = 1000;

                        ss2::assets::UnitSpec gen5f{};
                        gen5f.id = "dgeneral_5f";
                        gen5f.selType = "general";
                        gen5f.health = 300;
                        gen5f.sight = 14;
                        gen5f.officerRadius = 350;
                        gen5f.moraleAutoIncrease = 10;
                        gen5f.moraleResist = 10000;

                        auto cat5fB = ss2::assets::UnitCatalog::fromSpecs({hmg5f, rif5f, off5f, gen5f});
                        std::vector<std::uint32_t> flags5f(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5f(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [hmgX, hmgY] = tc64(20, 20);
                        const auto [rifX, rifY] = tc64(21, 20);
                        const auto [cmdX, cmdY] = tc64(25, 16); // du=+128, dv=-128 -> dx=256, dy=0 -> 256px from rif (>160, <=350)
                        std::vector<ss2::assets::Placement> emptyVeh5f{};
                        std::vector<ss2::assets::Infantry> infOff{
                            {"rif_5f", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5f", hmgX, hmgY, 100, 100, 11, 1},
                            {"dofficer_5f", cmdX, cmdY, 100, 0, 1, 0},
                        };
                        std::vector<ss2::assets::Infantry> infGen{
                            {"rif_5f", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5f", hmgX, hmgY, 100, 100, 11, 1},
                            {"dgeneral_5f", cmdX, cmdY, 100, 0, 1, 0},
                        };
                        ss2::TutorialUnits simOff(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            infOff, emptyVeh5f, ss2::FogGrid(64, 64, flags5f), 2, true, {}, cat5fB);
                        ss2::TutorialUnits simGen(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            infGen, emptyVeh5f, ss2::FogGrid(64, 64, flags5f), 2, true, {}, cat5fB);
                        (void)simOff.toggleHoldFire(std::array<size_t, 2>{0, 2});
                        (void)simGen.toggleHoldFire(std::array<size_t, 2>{0, 2});
                        bool offSup = false, genSup = false, sawOff = false, sawGen = false;
                        int offClear = -1, genClear = -1;
                        for (int t = 1; t <= 75; ++t) {
                            for (const auto& p : simOff.step()) if (p.index == 0u) offSup = p.suppressed;
                            for (const auto& p : simGen.step()) if (p.index == 0u) genSup = p.suppressed;
                            if (offSup) sawOff = true; else if (sawOff && offClear < 0) offClear = t;
                            if (genSup) sawGen = true; else if (sawGen && genClear < 0) genClear = t;
                        }
                        if (!sawOff || !sawGen || genClear < 0 || offClear < 0 || genClear >= offClear) {
                            throw std::runtime_error("Phase 5F General wide 350px aura failed to beat Officer 160px at 256px");
                        }

                        // moraleresist check: dgeneral vs rif under identical HMG burst
                        std::vector<ss2::assets::Infantry> infRifTarget{
                            {"rif_5f", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5f", hmgX, hmgY, 100, 100, 11, 1},
                        };
                        std::vector<ss2::assets::Infantry> infGenTarget{
                            {"dgeneral_5f", rifX, rifY, 100, 100, 1, 0},
                            {"dhmg5f", hmgX, hmgY, 100, 100, 11, 1},
                        };
                        ss2::TutorialUnits simRifRes(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            infRifTarget, emptyVeh5f, ss2::FogGrid(64, 64, flags5f), 2, true, {}, cat5fB);
                        ss2::TutorialUnits simGenRes(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            infGenTarget, emptyVeh5f, ss2::FogGrid(64, 64, flags5f), 2, true, {}, cat5fB);
                        (void)simRifRes.toggleHoldFire(std::array<size_t, 1>{0});
                        (void)simGenRes.toggleHoldFire(std::array<size_t, 1>{0});
                        for (int t = 0; t < 10; ++t) {
                            (void)simRifRes.step();
                            (void)simGenRes.step();
                        }
                        const int rifTicks = simRifRes.takeSnapshot().soldiers[0].suppressionTicks;
                        const int genTicks = simGenRes.takeSnapshot().soldiers[0].suppressionTicks;
                        if (rifTicks <= 0 || genTicks >= rifTicks) {
                            throw std::runtime_error("Phase 5F moraleresist failed to reduce suppression accumulation");
                        }
                    }

                    // (C) Retail reload1 ammo resupply cost & AirStrikeEvent::planeId propagation.
                    {
                        ss2::assets::UnitSpec truck5f{};
                        truck5f.id = "sdkfz.11";
                        truck5f.isVehicle = true;
                        truck5f.selType = "sappers";
                        truck5f.ammoRegenDelay = 8;
                        truck5f.health = 200;
                        truck5f.ammo1 = 1000;
                        truck5f.ammo2 = 500;
                        truck5f.sight = 10;

                        ss2::assets::UnitSpec heavyGun5f{};
                        heavyGun5f.id = "heavy_gun_5f";
                        heavyGun5f.isVehicle = true;
                        heavyGun5f.health = 200;
                        heavyGun5f.ammo1 = 20;
                        heavyGun5f.sight = 10;
                        heavyGun5f.shot1 = {"he5f", ss2::assets::HitType::Explosive, 100, 100, 500, 500, 0, 40, 1, 40, "gunshot", 0, 0, 0, 500, ""};

                        ss2::assets::UnitSpec lightVeh5f = heavyGun5f;
                        lightVeh5f.id = "light_veh_5f";
                        lightVeh5f.shot1.resupplyCost = 2;

                        ss2::assets::PlaneSpec il2{};
                        il2.id = "il-2";
                        il2.health = 350;
                        il2.scoreValue = 650;
                        il2.sight = 16;
                        il2.flySpeed = 10.0;
                        il2.groundDamage = 65;
                        il2.airDamage = 45;
                        il2.groundShotSound = "rocket";

                        auto cat5fC = ss2::assets::UnitCatalog::fromSpecs({truck5f, heavyGun5f, lightVeh5f}, {il2});
                        std::vector<std::uint32_t> flags5f(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5f(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [tX, tY] = tc64(20, 20);
                        const auto [gX, gY] = tc64(21, 20);
                        std::vector<ss2::assets::Infantry> emptyInf5f{};
                        std::vector<ss2::assets::Placement> vehHeavy{
                            {"sdkfz.11", tX, tY, 0, 100, 100, 1, 0},
                            {"heavy_gun_5f", gX, gY, 0, 100, 50, 1, 0},
                        };
                        std::vector<ss2::assets::Placement> vehLight{
                            {"sdkfz.11", tX, tY, 0, 100, 100, 1, 0},
                            {"light_veh_5f", gX, gY, 0, 100, 50, 1, 0},
                        };
                        ss2::TutorialUnits simHeavySup(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            emptyInf5f, vehHeavy, ss2::FogGrid(64, 64, flags5f), 2, false, {}, cat5fC);
                        ss2::TutorialUnits simLightSup(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5f, obst5f),
                            emptyInf5f, vehLight, ss2::FogGrid(64, 64, flags5f), 2, false, {}, cat5fC);
                        for (int t = 0; t < 26; ++t) {
                            (void)simHeavySup.step();
                            (void)simLightSup.step();
                        }
                        const auto snapH = simHeavySup.takeSnapshot();
                        const auto snapL = simLightSup.takeSnapshot();
                        if (snapH.vehicles[0].ammo != 0 || snapH.vehicles[1].ammo != 12 ||
                            snapL.vehicles[0].ammo != 994 || snapL.vehicles[1].ammo != 13) {
                            throw std::runtime_error("Phase 5F reload1 resupply cost scaling or stock clamp failed");
                        }

                        // AirStrikeEvent::planeId check for StrafeImpact
                        const auto [fX, fY] = tc64(36, 20);
                        std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air5f{};
                        air5f[0][4] = {"il-2", 1, 1};
                        std::array<int, 4> pGrp5f{};
                        std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos5f{};
                        simLightSup.setAirSupport(air5f, pGrp5f, pRos5f);
                        if (!simLightSup.callAirSupport(ss2::assets::AirMissionKind::Fighter, fX, fY, 0)) {
                            throw std::runtime_error("Phase 5F il-2 callAirSupport failed");
                        }
                        std::string strafePlaneId;
                        for (int t = 0; t < 220; ++t) {
                            (void)simLightSup.step();
                            for (const auto& ae : simLightSup.takeAirStrikeEvents()) {
                                if (ae.kind == ss2::AirStrikeEvent::StrafeImpact) {
                                    strafePlaneId = ae.planeId;
                                }
                            }
                            if (!strafePlaneId.empty()) break;
                        }
                        if (strafePlaneId != "il-2") {
                            throw std::runtime_error("Phase 5F StrafeImpact failed to carry AirStrikeEvent::planeId");
                        }
                    }

                    std::cout << "Phase 5F tank desant hull-rider exposure, General aura/moraleresist, reload1 logistics & AVIA audio PASSED\n";

                    // Phase 5G: retail repair spare-parts scaling, gunshotwait stabilization delay,
                    // canbecrew qualified vehicle capture, unitRadius crush footprint & fsheet leaflet bombers.
                    // (A) Retail repair spare-parts scaling (heavy tank 36/HP vs light mortar 2/HP).
                    {
                        ss2::assets::UnitSpec truck5g{};
                        truck5g.id = "sdkfz.11";
                        truck5g.isVehicle = true;
                        truck5g.selType = "sappers";
                        truck5g.ammoRegenDelay = 8;
                        truck5g.health = 200;
                        truck5g.ammo1 = 0;
                        truck5g.ammo2 = 360;
                        truck5g.sight = 10;

                        ss2::assets::UnitSpec heavyRep5g{};
                        heavyRep5g.id = "tiger_rep_5g";
                        heavyRep5g.isVehicle = true;
                        heavyRep5g.health = 200;
                        heavyRep5g.canSelfRepair = true;
                        heavyRep5g.repairCost = 36;
                        heavyRep5g.sight = 10;

                        ss2::assets::UnitSpec lightRep5g = heavyRep5g;
                        lightRep5g.id = "mortar_rep_5g";
                        lightRep5g.repairCost = 2;

                        auto cat5gA = ss2::assets::UnitCatalog::fromSpecs({truck5g, heavyRep5g, lightRep5g});
                        std::vector<std::uint32_t> flags5g(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5g(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [tX, tY] = tc64(20, 20);
                        const auto [vX, vY] = tc64(21, 20);
                        std::vector<ss2::assets::Infantry> emptyInf5g{};
                        std::vector<ss2::assets::Placement> vehHeavyRep{
                            {"sdkfz.11", tX, tY, 0, 100, 100, 1, 0},
                            {"tiger_rep_5g", vX, vY, 0, 50, 100, 1, 0},
                        };
                        std::vector<ss2::assets::Placement> vehLightRep{
                            {"sdkfz.11", tX, tY, 0, 100, 100, 1, 0},
                            {"mortar_rep_5g", vX, vY, 0, 50, 100, 1, 0},
                        };
                        ss2::TutorialUnits simHeavyRep(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            emptyInf5g, vehHeavyRep, ss2::FogGrid(64, 64, flags5g), 2, false, {}, cat5gA);
                        ss2::TutorialUnits simLightRep(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            emptyInf5g, vehLightRep, ss2::FogGrid(64, 64, flags5g), 2, false, {}, cat5gA);
                        for (int t = 0; t < 26; ++t) {
                            (void)simHeavyRep.step();
                            (void)simLightRep.step();
                        }
                        const auto snapHr = simHeavyRep.takeSnapshot();
                        const auto snapLr = simLightRep.takeSnapshot();
                        if (snapHr.vehicles[0].ammo2 != 0 || snapHr.vehicles[1].hp != 110 ||
                            snapLr.vehicles[0].ammo2 != 324 || snapLr.vehicles[1].hp != 118) {
                            throw std::runtime_error("Phase 5G retail repair spare-parts scaling check failed");
                        }
                    }

                    // (B) Retail gunshotwait main-gun stabilization delay after moving (primary held, coax unaffected).
                    {
                        ss2::assets::UnitSpec heavyStab{};
                        heavyStab.id = "heavy_stab_5g";
                        heavyStab.isVehicle = true;
                        heavyStab.health = 800;
                        heavyStab.moveSpeed = 6.0;
                        heavyStab.ammo1 = 20;
                        heavyStab.ammo2 = 40;
                        heavyStab.sight = 14;
                        heavyStab.gunShotWait = 8;
                        heavyStab.shot1 = {"ap5g", ss2::assets::HitType::Pierce, 80, 80, 400, 400, 0, 10, 1, 10, "gunshot"};
                        heavyStab.shot2 = {"mg5g", ss2::assets::HitType::Machine, 10, 10, 400, 400, 0, 3, 4, 15, "gunshot"};

                        ss2::assets::UnitSpec fastStab = heavyStab;
                        fastStab.id = "fast_stab_5g";
                        fastStab.gunShotWait = 0;

                        ss2::assets::UnitSpec tgtStab{};
                        tgtStab.id = "tgt_stab_5g";
                        tgtStab.isVehicle = true;
                        tgtStab.health = 1000;
                        tgtStab.sight = 4;

                        auto cat5gB = ss2::assets::UnitCatalog::fromSpecs({heavyStab, fastStab, tgtStab});
                        std::vector<std::uint32_t> flags5g(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5g(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [sX, sY] = tc64(20, 20);
                        const auto [dX, dY] = tc64(21, 20); // ~35.8px move -> 7 ticks
                        const auto [eX, eY] = tc64(25, 20); // ~178.9px target
                        std::vector<ss2::assets::Infantry> emptyInf5g{};
                        std::vector<ss2::assets::Placement> vehHeavy{
                            {"heavy_stab_5g", sX, sY, 0, 100, 100, 1, 0},
                            {"tgt_stab_5g", eX, eY, 16, 100, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> vehFast{
                            {"fast_stab_5g", sX, sY, 0, 100, 100, 1, 0},
                            {"tgt_stab_5g", eX, eY, 16, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits simHeavyStab(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            emptyInf5g, vehHeavy, ss2::FogGrid(64, 64, flags5g), 2, true, {}, cat5gB);
                        ss2::TutorialUnits simFastStab(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            emptyInf5g, vehFast, ss2::FogGrid(64, 64, flags5g), 2, true, {}, cat5gB);
                        const std::vector<size_t> v0{0};
                        if (simHeavyStab.moveVehicles(v0, dX, dY) != 1u || simFastStab.moveVehicles(v0, dX, dY) != 1u) {
                            throw std::runtime_error("Phase 5G gunshotwait moveVehicles failed");
                        }
                        for (int t = 0; t < 10; ++t) {
                            (void)simHeavyStab.step();
                            (void)simFastStab.step();
                        }
                        const auto snapH10 = simHeavyStab.takeSnapshot();
                        const auto snapF10 = simFastStab.takeSnapshot();
                        if (snapF10.vehicles[0].ammo >= 20 || snapH10.vehicles[0].ammo != 20) {
                            throw std::runtime_error("Phase 5G gunshotwait failed to hold primary fire while settling");
                        }
                        for (int t = 0; t < 12; ++t) (void)simHeavyStab.step();
                        const auto snapH22 = simHeavyStab.takeSnapshot();
                        if (snapH22.vehicles[0].ammo >= 20 || snapH22.vehicles[1].hp >= 1000) {
                            throw std::runtime_error("Phase 5G heavy gun never fired primary after stabilizing");
                        }
                    }

                    // (C) Retail canbecrew capture efficiency, radius crush footprint & fsheet leaflet bombers.
                    {
                        ss2::assets::UnitSpec crewedHeavy{};
                        crewedHeavy.id = "tiger_5g";
                        crewedHeavy.isVehicle = true;
                        crewedHeavy.health = 800;
                        crewedHeavy.crewNumber = 5;
                        crewedHeavy.attackCrew = 3;
                        crewedHeavy.hasCrewProtection = true;
                        crewedHeavy.sight = 10;

                        ss2::assets::UnitSpec rifle5g{};
                        rifle5g.id = "rifle_5g";
                        rifle5g.health = 100;
                        rifle5g.ammo1 = 20;
                        rifle5g.sight = 10;
                        rifle5g.canBeCrew = 0;

                        ss2::assets::UnitSpec tankist5g{};
                        tankist5g.id = "dtankist_5g";
                        tankist5g.health = 100;
                        tankist5g.ammo1 = 20;
                        tankist5g.sight = 10;
                        tankist5g.canBeCrew = 1;

                        ss2::assets::UnitSpec crushWide{};
                        crushWide.id = "crush_wide_5g";
                        crushWide.isVehicle = true;
                        crushWide.hasCrushRange = true;
                        crushWide.unitRadius = 23;
                        crushWide.health = 800;
                        crushWide.moveSpeed = 10.0;
                        crushWide.sight = 10;

                        ss2::assets::UnitSpec crushTiny = crushWide;
                        crushTiny.id = "crush_tiny_5g";
                        crushTiny.unitRadius = 5;

                        ss2::assets::UnitSpec lightGun5g{};
                        lightGun5g.id = "gun_5g";
                        lightGun5g.isVehicle = true;
                        lightGun5g.canBeCrushed = true;
                        lightGun5g.unitRadius = 10;
                        lightGun5g.health = 200;
                        lightGun5g.sight = 4;

                        ss2::assets::PlaneSpec leafletPlane{};
                        leafletPlane.id = "b-17c_agit";
                        leafletPlane.health = 2050;
                        leafletPlane.sight = 24;
                        leafletPlane.flySpeed = 6.97;
                        leafletPlane.bombDamage = 5000;
                        leafletPlane.bombsNumber = 3;
                        leafletPlane.bombId = "fsheet";

                        auto cat5gC = ss2::assets::UnitCatalog::fromSpecs(
                            {crewedHeavy, rifle5g, tankist5g, crushWide, crushTiny, lightGun5g},
                            {leafletPlane});
                        std::vector<std::uint32_t> flags5g(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5g(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [uX, uY] = tc64(20, 20);
                        const auto [vX, vY] = tc64(21, 20);
                        std::vector<ss2::assets::Infantry> infRif{{"rifle_5g", uX, uY, 100, 20, 1, 0}};
                        std::vector<ss2::assets::Infantry> infTnk{{"dtankist_5g", uX, uY, 100, 20, 1, 0}};
                        std::vector<ss2::assets::Placement> vehCap{{"tiger_5g", vX, vY, 0, 100, 0, 0, 3}};

                        ss2::TutorialUnits simCapRif(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            infRif, vehCap, ss2::FogGrid(64, 64, flags5g), 2, false, {}, cat5gC);
                        ss2::TutorialUnits simCapTnk(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            infTnk, vehCap, ss2::FogGrid(64, 64, flags5g), 2, false, {}, cat5gC);
                        auto preSnap = simCapTnk.takeSnapshot();
                        preSnap.soldiers[0].expa = 4000;
                        if (!simCapTnk.loadSnapshot(preSnap)) {
                            throw std::runtime_error("Phase 5G loadSnapshot for tankist expa failed");
                        }
                        if (simCapRif.orderCaptureVehicle(std::array<size_t, 1>{0}, 0) != 1u ||
                            simCapTnk.orderCaptureVehicle(std::array<size_t, 1>{0}, 0) != 1u) {
                            throw std::runtime_error("Phase 5G orderCaptureVehicle failed");
                        }
                        bool rifImpaired = false;
                        bool tnkClean = false;
                        for (int t = 0; t < 20; ++t) {
                            (void)simCapRif.step();
                            for (const auto& vp : simCapRif.takeVehiclePoses()) {
                                if (vp.index == 0u && vp.captured && vp.crewImpaired) rifImpaired = true;
                            }
                            (void)simCapTnk.step();
                            for (const auto& vp : simCapTnk.takeVehiclePoses()) {
                                if (vp.index == 0u && vp.captured && !vp.crewImpaired) tnkClean = true;
                            }
                        }
                        const auto snapRifCap = simCapRif.takeSnapshot();
                        const auto snapTnkCap = simCapTnk.takeSnapshot();
                        if (!rifImpaired || !tnkClean || snapRifCap.vehicles[0].owner != 0 ||
                            snapRifCap.vehicles[0].crewHp >= 245 || snapTnkCap.vehicles[0].crewHp != 245 ||
                            snapTnkCap.vehicles[0].expa < 4000) {
                            throw std::runtime_error("Phase 5G canbecrew capture efficiency or vet transfer failed");
                        }

                        // radius physical crush footprint (29px lateral offset: wide 23+10=33 crushes, tiny 5+5->24 misses)
                        const auto [cStartX, cStartY] = tc64(18, 22);
                        const auto [cEndX, cEndY] = tc64(22, 18);
                        const auto [cMidX, cMidY] = tc64(20, 20);
                        std::vector<ss2::assets::Infantry> emptyInf5g{};
                        std::vector<ss2::assets::Placement> vehWide{
                            {"crush_wide_5g", cStartX, cStartY, 0, 100, 0, 1, 0},
                            {"gun_5g", cMidX, cMidY + 29, 0, 100, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> vehTiny{
                            {"crush_tiny_5g", cStartX, cStartY, 0, 100, 0, 1, 0},
                            {"gun_5g", cMidX, cMidY + 29, 0, 100, 0, 11, 1},
                        };
                        ss2::TutorialUnits simWide(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            emptyInf5g, vehWide, ss2::FogGrid(64, 64, flags5g), 2, false, {}, cat5gC);
                        ss2::TutorialUnits simTiny(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            emptyInf5g, vehTiny, ss2::FogGrid(64, 64, flags5g), 2, false, {}, cat5gC);
                        (void)simWide.moveVehicles(std::array<size_t, 1>{0}, cEndX, cEndY);
                        (void)simTiny.moveVehicles(std::array<size_t, 1>{0}, cEndX, cEndY);
                        for (int t = 0; t < 55; ++t) {
                            (void)simWide.step();
                            (void)simTiny.step();
                        }
                        if (simWide.takeSnapshot().vehicles[1].alive || !simTiny.takeSnapshot().vehicles[1].alive) {
                            throw std::runtime_error("Phase 5G unitRadius physical crush footprint check failed");
                        }

                        // fsheet propaganda leaflet bomber: wide suppression, zero explosion damage
                        const auto [bTgtX, bTgtY] = tc64(40, 20);
                        std::vector<ss2::assets::Infantry> infLeaf{{"rifle_5g", bTgtX, bTgtY, 100, 0, 11, 1}};
                        std::vector<ss2::assets::Placement> vehLeaf{{"tiger_5g", bTgtX, bTgtY, 0, 100, 0, 11, 1}};
                        ss2::TutorialUnits simLeaf(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5g, obst5g),
                            infLeaf, vehLeaf, ss2::FogGrid(64, 64, flags5g), 2, false, {}, cat5gC);
                        std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air5g{};
                        air5g[0][0] = {"b-17c_agit", 1, 1};
                        std::array<int, 4> pGrp5g{};
                        std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos5g{};
                        simLeaf.setAirSupport(air5g, pGrp5g, pRos5g);
                        if (!simLeaf.callAirSupport(ss2::assets::AirMissionKind::Bomber, bTgtX, bTgtY, 0)) {
                            throw std::runtime_error("Phase 5G b-17c_agit callAirSupport failed");
                        }
                        bool sawLeafletDrop = false;
                        bool sawLeafSup = false;
                        for (int t = 0; t < 260; ++t) {
                            for (const auto& sp : simLeaf.step()) {
                                if (sp.index == 0u && sp.suppressed) sawLeafSup = true;
                            }
                            for (const auto& ae : simLeaf.takeAirStrikeEvents()) {
                                if (ae.kind == ss2::AirStrikeEvent::BombImpact && ae.isLeaflet) sawLeafletDrop = true;
                            }
                        }
                        const auto snapLeaf = simLeaf.takeSnapshot();
                        if (!sawLeafletDrop || !sawLeafSup || snapLeaf.soldiers[0].hp != 100 ||
                            snapLeaf.vehicles[0].hp != 800) {
                            throw std::runtime_error("Phase 5G bombid=fsheet leaflet bomber damaged HP or missed suppression");
                        }
                    }

                    std::cout << "Phase 5G retail repair parts scaling, gunshotwait stabilization, canbecrew capture, radius crush & fsheet leaflet bombers PASSED\n";

                    // Phase 5H: garrisoned canfirefrominside (PIAT vs backblast), AT-rifle crouchtofire & crouchsight,
                    // and multi-round shot*_useammo consumption (ot-34 10/shot, agit 0/shot).
                    // (A) Retail canfirefrominside: garrisoned PIAT fires out window while Panzerfaust is blocked.
                    {
                        ss2::assets::UnitSpec piat5h{};
                        piat5h.id = "piat_5h";
                        piat5h.selType = "atinf";
                        piat5h.health = 100;
                        piat5h.ammo1 = 6;
                        piat5h.sight = 11;
                        piat5h.canFireFromInside = true;
                        piat5h.hasExplicitInsideRule = true;
                        piat5h.shot1 = {"ap5h", ss2::assets::HitType::Pierce, 150, 150, 260, 260, 0, 8, 1, 8, "gunshot"};

                        ss2::assets::UnitSpec pfaust5h = piat5h;
                        pfaust5h.id = "pfaust_5h";
                        pfaust5h.ammo1 = 4;
                        pfaust5h.canFireFromInside = false;

                        ss2::assets::UnitSpec tgt5h{};
                        tgt5h.id = "tgt_5h";
                        tgt5h.isVehicle = true;
                        tgt5h.health = 600;
                        tgt5h.sight = 4;

                        auto cat5hA = ss2::assets::UnitCatalog::fromSpecs({piat5h, pfaust5h, tgt5h});
                        std::vector<std::uint32_t> flags5h(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5h(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [bX, bY] = tc64(20, 20);
                        const auto [tX, tY] = tc64(22, 20);
                        std::vector<ss2::assets::Infantry> infPiat{{"piat_5h", bX, bY, 100, 100, 1, 0}};
                        std::vector<ss2::assets::Infantry> infPfaust{{"pfaust_5h", bX, bY, 100, 100, 1, 0}};
                        std::vector<ss2::assets::Placement> vehTgt{{"tgt_5h", tX, tY, 16, 100, 0, 11, 1}};
                        ss2::assets::BuildingPlacement house5h{bX, bY, bX - 20, bY - 20, 40, 40, 4, 400};

                        ss2::TutorialUnits simPiat(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5h, obst5h),
                            infPiat, vehTgt, ss2::FogGrid(64, 64, flags5h), 2, true, {}, cat5hA);
                        ss2::TutorialUnits simPfaust(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5h, obst5h),
                            infPfaust, vehTgt, ss2::FogGrid(64, 64, flags5h), 2, true, {}, cat5hA);
                        simPiat.setBuildings({house5h});
                        simPfaust.setBuildings({house5h});
                        if (simPiat.orderGarrisonBuilding(std::array<size_t, 1>{0}, 0) != 1u ||
                            simPfaust.orderGarrisonBuilding(std::array<size_t, 1>{0}, 0) != 1u) {
                            throw std::runtime_error("Phase 5H garrison setup failed");
                        }
                        (void)simPiat.step();
                        (void)simPfaust.step();
                        if (simPiat.orderBuildingAttack(0, true, 0) != 1u ||
                            simPfaust.orderBuildingAttack(0, true, 0) != 0u) {
                            throw std::runtime_error("Phase 5H orderBuildingAttack canfirefrominside filter failed");
                        }
                        for (int t = 0; t < 20; ++t) {
                            (void)simPiat.step();
                            (void)simPfaust.step();
                        }
                        const auto snapPiat = simPiat.takeSnapshot();
                        const auto snapPf = simPfaust.takeSnapshot();
                        if (snapPiat.soldiers[0].ammo >= 6 || snapPiat.vehicles[0].hp >= 600 ||
                            snapPf.soldiers[0].ammo != 4 || snapPf.vehicles[0].hp != 600) {
                            throw std::runtime_error("Phase 5H PIAT vs Panzerfaust garrisoned firing check failed");
                        }
                    }

                    // (B) Retail crouchtofire AT-rifle prone deployment & exact crouchsight.
                    {
                        ss2::assets::UnitSpec gats5h{};
                        gats5h.id = "gats_5h";
                        gats5h.selType = "atinf";
                        gats5h.health = 100;
                        gats5h.ammo1 = 20;
                        gats5h.sight = 12;
                        gats5h.crouchSight = 8;
                        gats5h.crouchSightMax = 8;
                        gats5h.crouchToFire = true;
                        gats5h.shot1 = {"atr_5h", ss2::assets::HitType::Pierce, 80, 80, 260, 260, 0, 8, 1, 8, "gunshot"};

                        ss2::assets::UnitSpec scout5h{};
                        scout5h.id = "scout_5h";
                        scout5h.health = 100;
                        scout5h.sight = 4;

                        ss2::assets::UnitSpec tank5h{};
                        tank5h.id = "tank_5h";
                        tank5h.isVehicle = true;
                        tank5h.health = 600;
                        tank5h.sight = 4;

                        auto cat5hB = ss2::assets::UnitCatalog::fromSpecs({gats5h, scout5h, tank5h});
                        std::vector<std::uint32_t> flags5h(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5h(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [sX, sY] = tc64(20, 20);
                        const auto [vX, vY] = tc64(23, 20);
                        const auto [farX, farY] = tc64(30, 20); // 10 tiles: visible standing (12), hidden crouched (8)

                        std::vector<ss2::assets::Infantry> infGats{{"gats_5h", sX, sY, 100, 100, 1, 0}};
                        std::vector<ss2::assets::Placement> vehGats{{"tank_5h", vX, vY, 16, 100, 0, 11, 1}};
                        ss2::TutorialUnits simGats(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5h, obst5h),
                            infGats, vehGats, ss2::FogGrid(64, 64, flags5h), 2, true, {}, cat5hB);
                        (void)simGats.step(); // tick 1: crouch-deploy first before firing
                        const auto snapG1 = simGats.takeSnapshot();
                        if (!snapG1.soldiers[0].crouched || snapG1.soldiers[0].ammo != 20) {
                            throw std::runtime_error("Phase 5H crouchtofire did not auto-crouch before first shot");
                        }
                        for (int t = 0; t < 12; ++t) (void)simGats.step();
                        const auto snapG13 = simGats.takeSnapshot();
                        if (!snapG13.soldiers[0].crouched || snapG13.soldiers[0].ammo >= 20 ||
                            snapG13.vehicles[0].hp >= 600) {
                            throw std::runtime_error("Phase 5H crouchtofire did not fire after prone deployment");
                        }

                        // crouchsight verification (12 standing vs 8 crouched)
                        std::vector<ss2::assets::Infantry> infSight{
                            {"gats_5h", sX, sY, 100, 0, 1, 0},
                            {"scout_5h", farX, farY, 100, 0, 11, 1},
                        };
                        std::vector<ss2::assets::Placement> noVeh{};
                        ss2::TutorialUnits simSight(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5h, obst5h),
                            infSight, noVeh, ss2::FogGrid(64, 64, flags5h), 2, true, {}, cat5hB);
                        if (!simSight.isValidAttackTarget(1, 0)) {
                            throw std::runtime_error("Phase 5H standing gats should spot enemy at 10 tiles");
                        }
                        if (!simSight.toggleCrouch(std::array<size_t, 1>{0}) || simSight.isValidAttackTarget(1, 0)) {
                            throw std::runtime_error("Phase 5H crouched gats did not apply crouchsight=8");
                        }
                    }

                    // (C) Retail shot2_useammo multi-round consumption (ot-34 10 fuel/shot) & shot1_useammo=0.
                    {
                        ss2::assets::UnitSpec ot34_5h{};
                        ot34_5h.id = "ot34_5h";
                        ot34_5h.isVehicle = true;
                        ot34_5h.health = 500;
                        ot34_5h.ammo1 = 40;
                        ot34_5h.ammo2 = 50;
                        ot34_5h.sight = 12;
                        ot34_5h.shot1 = {"ap5h", ss2::assets::HitType::Pierce, 80, 80, 300, 300, 0, 30, 1, 30, "gunshot"};
                        ot34_5h.shot2 = {"flamer5h", ss2::assets::HitType::Fire, 100, 100, 260, 260, 0, 5, 1, 10, "fmw", 0, 0, 0, 0, "", 10};

                        ss2::assets::UnitSpec free5h{};
                        free5h.id = "free_5h";
                        free5h.isVehicle = true;
                        free5h.health = 200;
                        free5h.ammo1 = 0;
                        free5h.sight = 12;
                        free5h.shot1 = {"mg5h", ss2::assets::HitType::Machine, 25, 25, 300, 300, 0, 5, 1, 10, "gunshot", 0, 0, 0, 0, "", 0};

                        ss2::assets::UnitSpec dummy5h{};
                        dummy5h.id = "dum_5h";
                        dummy5h.health = 500;
                        dummy5h.sight = 4;

                        auto cat5hC = ss2::assets::UnitCatalog::fromSpecs({ot34_5h, free5h, dummy5h});
                        std::vector<std::uint32_t> flags5h(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5h(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [vX, vY] = tc64(20, 20);
                        const auto [eX, eY] = tc64(23, 20);
                        std::vector<ss2::assets::Infantry> infOt{{"dum_5h", eX, eY, 500, 0, 11, 1}};
                        std::vector<ss2::assets::Placement> vehOt{{"ot34_5h", vX, vY, 0, 100, 100, 1, 0}};
                        ss2::TutorialUnits simOt(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5h, obst5h),
                            infOt, vehOt, ss2::FogGrid(64, 64, flags5h), 2, true, {}, cat5hC);
                        (void)simOt.step();
                        const auto snapOt1 = simOt.takeSnapshot();
                        if (snapOt1.vehicles[0].ammo2 != 40 || snapOt1.soldiers[0].hp >= 500) {
                            throw std::runtime_error("Phase 5H ot-34 shot2_useammo=10 failed to consume 10 ammo2 per shot");
                        }

                        std::vector<ss2::assets::Placement> vehFree{{"free_5h", vX, vY, 0, 100, 0, 1, 0}};
                        ss2::TutorialUnits simFree(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5h, obst5h),
                            infOt, vehFree, ss2::FogGrid(64, 64, flags5h), 2, true, {}, cat5hC);
                        auto snapZ = simFree.takeSnapshot();
                        snapZ.vehicles[0].ammo = 0;
                        if (!simFree.loadSnapshot(snapZ)) {
                            throw std::runtime_error("Phase 5H free_5h zero-ammo loadSnapshot failed");
                        }
                        for (int t = 0; t < 15; ++t) (void)simFree.step();
                        const auto snapFreeAfter = simFree.takeSnapshot();
                        if (snapFreeAfter.vehicles[0].ammo != 0 || snapFreeAfter.soldiers[0].hp >= 500) {
                            throw std::runtime_error("Phase 5H shot1_useammo=0 failed to fire with 0 ammo");
                        }
                    }

                    std::cout << "Phase 5H garrison canfirefrominside (PIAT vs backblast), crouchtofire/crouchsight & shot*_useammo PASSED\n";

                    // Phase 5I: retail crew_ver vulnerability scaling, expa_loosehp XP under fire,
                    // soldtomove/movecrew immobility & live SoldierPose::crouchedState visual sync.
                    // (A) Retail crew_ver vulnerability scaling (1.84x vs 0.10x).
                    {
                        ss2::assets::UnitSpec snip5i{};
                        snip5i.id = "sniper_5i";
                        snip5i.selType = "sniper";
                        snip5i.health = 100;
                        snip5i.ammo1 = 1;
                        snip5i.sight = 14;
                        snip5i.shot1 = {"sn_5i", ss2::assets::HitType::Sniper, 100, 100, 300, 300, 0, 20, 1, 20, "gunshot"};

                        ss2::assets::UnitSpec highVuln{};
                        highVuln.id = "high_vuln_5i";
                        highVuln.isVehicle = true;
                        highVuln.health = 1000;
                        highVuln.crewNumber = 4;
                        highVuln.crewVulnerability = 1.84;
                        highVuln.hasCrewProtection = true;
                        highVuln.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {0, 0, 0};
                        highVuln.sight = 4;

                        ss2::assets::UnitSpec lowVuln = highVuln;
                        lowVuln.id = "low_vuln_5i";
                        lowVuln.crewVulnerability = 0.04;

                        auto cat5iA = ss2::assets::UnitCatalog::fromSpecs({snip5i, highVuln, lowVuln});
                        std::vector<std::uint32_t> flags5i(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5i(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [sX, sY] = tc64(20, 20);
                        const auto [vX, vY] = tc64(23, 20);
                        std::vector<ss2::assets::Infantry> inf5iA{{"sniper_5i", sX, sY, 100, 100, 1, 0}};
                        std::vector<ss2::assets::Placement> vehHigh{{"high_vuln_5i", vX, vY, 0, 100, 0, 11, 1}};
                        std::vector<ss2::assets::Placement> vehLow{{"low_vuln_5i", vX, vY, 0, 100, 0, 11, 1}};
                        ss2::TutorialUnits simHighV(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5i, obst5i),
                            inf5iA, vehHigh, ss2::FogGrid(64, 64, flags5i), 2, true, {}, cat5iA);
                        ss2::TutorialUnits simLowV(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5i, obst5i),
                            inf5iA, vehLow, ss2::FogGrid(64, 64, flags5i), 2, true, {}, cat5iA);
                        if (simHighV.attackVehicle(std::array<size_t, 1>{0}, 0) != 1u ||
                            simLowV.attackVehicle(std::array<size_t, 1>{0}, 0) != 1u) {
                            throw std::runtime_error("Phase 5I sniper attackVehicle setup failed");
                        }
                        for (int t = 0; t < 10; ++t) {
                            (void)simHighV.step();
                            (void)simLowV.step();
                        }
                        const auto snapH = simHighV.takeSnapshot();
                        const auto snapL = simLowV.takeSnapshot();
                        if (snapL.vehicles[0].crewHp >= 210 || snapH.vehicles[0].crewHp >= snapL.vehicles[0].crewHp) {
                            throw std::runtime_error("Phase 5I crew_ver vulnerability multiplier mismatch");
                        }
                    }

                    // (B) Retail expa_loosehp combat XP under fire (soldier +100/hit, heavy tank +dmg/hit).
                    {
                        ss2::assets::UnitSpec solHit{};
                        solHit.id = "sol_hit_5i";
                        solHit.health = 500;
                        solHit.ammo1 = 0;
                        solHit.expaOnHit = 100;
                        solHit.sight = 10;

                        ss2::assets::UnitSpec tankHit{};
                        tankHit.id = "tank_hit_5i";
                        tankHit.isVehicle = true;
                        tankHit.health = 1000;
                        tankHit.expaOnHit = 1;
                        tankHit.sight = 10;

                        ss2::assets::UnitSpec attackerVeh{};
                        attackerVeh.id = "att_5i";
                        attackerVeh.isVehicle = true;
                        attackerVeh.health = 400;
                        attackerVeh.ammo1 = 40;
                        attackerVeh.sight = 14;
                        attackerVeh.shot1 = {"at_5i", ss2::assets::HitType::Pierce, 40, 40, 300, 300, 0, 5, 1, 10, "gunshot"};
                        attackerVeh.shot2 = {"mg_5i", ss2::assets::HitType::Machine, 10, 10, 300, 300, 0, 5, 1, 10, "gunshot"};

                        auto cat5iB = ss2::assets::UnitCatalog::fromSpecs({solHit, tankHit, attackerVeh});
                        std::vector<std::uint32_t> flags5i(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5i(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [tX, tY] = tc64(20, 20);
                        const auto [sX, sY] = tc64(21, 20); // ~107px from att_5i (<= 195px effRange)
                        const auto [aX, aY] = tc64(24, 20); // ~143px from tank_hit_5i (>88px grenade range)
                        std::vector<ss2::assets::Infantry> inf5iB{{"sol_hit_5i", sX, sY, 500, 0, 1, 0}};
                        std::vector<ss2::assets::Placement> veh5iB{
                            {"tank_hit_5i", tX, tY, 0, 100, 0, 1, 0},
                            {"att_5i", aX, aY, 16, 100, 100, 11, 1},
                        };
                        ss2::TutorialUnits simExpa(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5i, obst5i),
                            inf5iB, veh5iB, ss2::FogGrid(64, 64, flags5i), 2, true, {}, cat5iB);
                        (void)simExpa.toggleHoldFire(std::array<size_t, 1>{0});
                        std::vector<ss2::assets::Infantry> infOnlyB{{"sol_hit_5i", sX, sY, 500, 0, 1, 0}};
                        std::vector<ss2::assets::Placement> vehOnlyAtt{{"att_5i", aX, aY, 16, 100, 100, 11, 1}};
                        ss2::TutorialUnits simSolExpa(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5i, obst5i),
                            infOnlyB, vehOnlyAtt, ss2::FogGrid(64, 64, flags5i), 2, true, {}, cat5iB);
                        (void)simSolExpa.toggleHoldFire(std::array<size_t, 1>{0});
                        for (int t = 0; t < 10; ++t) {
                            (void)simExpa.step();
                            (void)simSolExpa.step();
                        }
                        const auto snapVehEx = simExpa.takeSnapshot();
                        const auto snapSolEx = simSolExpa.takeSnapshot();
                        if (snapSolEx.soldiers[0].expa < 100 || snapSolEx.soldiers[0].kills != 0 ||
                            snapVehEx.vehicles[0].expa < 40 || snapVehEx.vehicles[0].kills != 0) {
                            throw std::runtime_error("Phase 5I expa_loosehp failed to award XP under fire");
                        }
                    }

                    // (C) Retail soldtomove immobility & live SoldierPose::crouchedState sync.
                    {
                        ss2::assets::UnitSpec pak5i{};
                        pak5i.id = "pak40_5i";
                        pak5i.isVehicle = true;
                        pak5i.health = 400;
                        pak5i.moveSpeed = 2.0;
                        pak5i.crewNumber = 2;
                        pak5i.attackCrew = 2;
                        pak5i.soldToMove = 2;
                        pak5i.hasCrewProtection = true;
                        pak5i.sight = 10;

                        ss2::assets::UnitSpec flak5i = pak5i;
                        flak5i.id = "flak88_5i";
                        flak5i.soldToMove = 3; // requires >2 crew, cannot relocate under own crew power

                        ss2::assets::UnitSpec gats5i{};
                        gats5i.id = "gats_5i";
                        gats5i.selType = "atinf";
                        gats5i.health = 100;
                        gats5i.ammo1 = 20;
                        gats5i.sight = 12;
                        gats5i.crouchToFire = true;
                        gats5i.shot1 = {"atr_5i", ss2::assets::HitType::Pierce, 80, 80, 260, 260, 0, 8, 1, 8, "gunshot"};

                        ss2::assets::UnitSpec dummy5i{};
                        dummy5i.id = "dum_5i";
                        dummy5i.isVehicle = true;
                        dummy5i.health = 600;
                        dummy5i.sight = 4;

                        auto cat5iC = ss2::assets::UnitCatalog::fromSpecs({pak5i, flak5i, gats5i, dummy5i});
                        std::vector<std::uint32_t> flags5i(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5i(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) {
                            const int wu = tx * 32 + 16;
                            const int wv = ty * 32 + 16;
                            return std::pair<int, int>{64 * 32 + wu - wv, (wu + wv) / 2};
                        };
                        const auto [pX, pY] = tc64(20, 20);
                        const auto [dX, dY] = tc64(24, 20);
                        const auto [eX, eY] = tc64(23, 20);
                        std::vector<ss2::assets::Infantry> emptyInf5i{};
                        std::vector<ss2::assets::Placement> vehPak{{"pak40_5i", pX, pY, 0, 100, 0, 1, 0}};
                        std::vector<ss2::assets::Placement> vehFlak{{"flak88_5i", pX, pY, 0, 100, 0, 1, 0}};
                        ss2::TutorialUnits simPak(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5i, obst5i),
                            emptyInf5i, vehPak, ss2::FogGrid(64, 64, flags5i), 2, false, {}, cat5iC);
                        ss2::TutorialUnits simFlak(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5i, obst5i),
                            emptyInf5i, vehFlak, ss2::FogGrid(64, 64, flags5i), 2, false, {}, cat5iC);
                        bool pakInitImp = true, flakInitImp = false;
                        for (const auto& vp : simPak.takeVehiclePoses()) if (vp.index == 0u) pakInitImp = vp.moveImpaired;
                        for (const auto& vp : simFlak.takeVehiclePoses()) if (vp.index == 0u) flakInitImp = vp.moveImpaired;
                        (void)simPak.moveVehicles(std::array<size_t, 1>{0}, dX, dY);
                        (void)simFlak.moveVehicles(std::array<size_t, 1>{0}, dX, dY);
                        for (int t = 0; t < 10; ++t) {
                            (void)simPak.step();
                            (void)simFlak.step();
                        }
                        const auto snapPak = simPak.takeSnapshot();
                        const auto snapFlak = simFlak.takeSnapshot();
                        if (pakInitImp || !flakInitImp ||
                            (snapPak.vehicles[0].x == pX && snapPak.vehicles[0].y == pY) ||
                            snapFlak.vehicles[0].x != pX || snapFlak.vehicles[0].y != pY) {
                            throw std::runtime_error("Phase 5I soldtomove full-crew vs heavy-gun immobility check failed");
                        }
                        // Reduce pak40 crewHp to 1 active crew -> becomes moveImpaired and halts
                        auto depPak = snapPak;
                        depPak.vehicles[0].crewHp = 50; // 50/140 -> 1 active crew < soldToMove(2)
                        const int depX = depPak.vehicles[0].x;
                        const int depY = depPak.vehicles[0].y;
                        if (!simPak.loadSnapshot(depPak)) {
                            throw std::runtime_error("Phase 5I depleted pak40 loadSnapshot failed");
                        }
                        bool pakDepImp = false;
                        for (const auto& vp : simPak.takeVehiclePoses()) if (vp.index == 0u) pakDepImp = vp.moveImpaired;
                        (void)simPak.moveVehicles(std::array<size_t, 1>{0}, pX, pY);
                        for (int t = 0; t < 10; ++t) (void)simPak.step();
                        const auto afterDep = simPak.takeSnapshot();
                        if (!pakDepImp || afterDep.vehicles[0].x != depX || afterDep.vehicles[0].y != depY) {
                            throw std::runtime_error("Phase 5I under-crewed pak40 failed to immobilize");
                        }

                        // Live SoldierPose::crouchedState == 1 emitted on crouchToFire auto-crouch
                        std::vector<ss2::assets::Infantry> infGats{{"gats_5i", pX, pY, 100, 100, 1, 0}};
                        std::vector<ss2::assets::Placement> vehDum{{"dum_5i", eX, eY, 16, 100, 0, 11, 1}};
                        ss2::TutorialUnits simCrouch(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5i, obst5i),
                            infGats, vehDum, ss2::FogGrid(64, 64, flags5i), 2, true, {}, cat5iC);
                        std::int8_t emittedCrouch = -1;
                        for (const auto& p : simCrouch.step()) {
                            if (p.index == 0u) emittedCrouch = p.crouchedState;
                        }
                        if (emittedCrouch != 1) {
                            throw std::runtime_error("Phase 5I crouchtofire failed to emit SoldierPose::crouchedState == 1");
                        }
                    }

                    std::cout << "Phase 5I retail crew_ver vulnerability, expa_loosehp XP, soldtomove immobility & crouchedState sync PASSED\n";

                    // Phase 5J: retail moraleresist vehicle crew shock & Officer rally, AVIA shot_air_sound
                    // vs Ground AA flak audio distinction, and building bombardment occupant suppression.
                    {
                        std::vector<std::uint32_t> flags5j(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5j(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        // (A) Vehicle moraleresist (light 2048 vs heavy 10240), suppressed reload penalty & Officer rally.
                        ss2::assets::UnitSpec light5j{};
                        light5j.id = "light_5j";
                        light5j.isVehicle = true;
                        light5j.health = 400;
                        light5j.crewNumber = 2;
                        light5j.attackCrew = 1;
                        light5j.moraleResist = 2048;
                        light5j.ammo1 = 60;
                        light5j.sight = 14;
                        light5j.shot1 = {"man_machine", ss2::assets::HitType::Machine, 10, 10, 400, 400, 0, 20, 1, 20, "gunshot"};

                        ss2::assets::UnitSpec heavy5j{};
                        heavy5j.id = "heavy_5j";
                        heavy5j.isVehicle = true;
                        heavy5j.health = 1000;
                        heavy5j.crewNumber = 5;
                        heavy5j.attackCrew = 1;
                        heavy5j.moraleResist = 10240;
                        heavy5j.ammo1 = 0;
                        heavy5j.sight = 8;

                        ss2::assets::UnitSpec tgt5j{};
                        tgt5j.id = "tgt_5j";
                        tgt5j.isVehicle = true;
                        tgt5j.health = 2000;
                        tgt5j.ammo1 = 0;
                        tgt5j.sight = 4;

                        ss2::assets::UnitSpec off5j{};
                        off5j.id = "off_5j";
                        off5j.selType = "officer";
                        off5j.health = 150;
                        off5j.ammo1 = 0;
                        off5j.sight = 8;
                        off5j.officerRadius = 160;
                        off5j.moraleAutoIncrease = 5;

                        ss2::assets::PlaneSpec leaf5j{};
                        leaf5j.id = "b-17c_agit";
                        leaf5j.health = 250;
                        leaf5j.flySpeed = 8.0;
                        leaf5j.bombsNumber = 3;
                        leaf5j.bombDamage = 0;
                        leaf5j.bombId = "fsheet";

                        ss2::assets::PlaneSpec bf109j{};
                        bf109j.id = "bf-109";
                        bf109j.health = 200;
                        bf109j.flySpeed = 10.0;
                        bf109j.airDamage = 60;
                        bf109j.airShotSound = "mg_aa";

                        ss2::assets::PlaneSpec ju87j{};
                        ju87j.id = "ju-87";
                        ju87j.health = 220;
                        ju87j.flySpeed = 8.0;
                        ju87j.bombsNumber = 3;
                        ju87j.bombDamage = 600;

                        ss2::assets::UnitSpec flak5j{};
                        flak5j.id = "flak88_5j";
                        flak5j.isVehicle = true;
                        flak5j.roleClass = "antiaircraft";
                        flak5j.selType = "mobairdef";
                        flak5j.health = 200;
                        flak5j.ammo1 = 40;
                        flak5j.sight = 14;
                        flak5j.shot1 = {"flak_he", ss2::assets::HitType::Explosive, 100, 100, 600, 600, 0, 20, 1, 20, "flak"};

                        ss2::assets::UnitSpec m16j{};
                        m16j.id = "m16_5j";
                        m16j.isVehicle = true;
                        m16j.selType = "aagun";
                        m16j.health = 200;
                        m16j.ammo1 = 200;
                        m16j.ammo2 = 200;
                        m16j.sight = 14;
                        m16j.shot1 = {"m2_hb", ss2::assets::HitType::Machine, 30, 30, 500, 500, 0, 10, 4, 20, "mg_tank_burst"};
                        m16j.shot2 = {"aa_mg", ss2::assets::HitType::Machine, 30, 30, 600, 600, 0, 10, 4, 20, "mg2_aa"};

                        ss2::assets::UnitSpec howi5j{};
                        howi5j.id = "howi_5j";
                        howi5j.isVehicle = true;
                        howi5j.health = 500;
                        howi5j.ammo1 = 20;
                        howi5j.sight = 14;
                        howi5j.shot1 = {"he_shell", ss2::assets::HitType::Explosive, 30, 30, 500, 500, 0, 18, 1, 18, "howitzer"};

                        ss2::assets::UnitSpec inf5j{};
                        inf5j.id = "inf_5j";
                        inf5j.health = 120;
                        inf5j.ammo1 = 40;
                        inf5j.sight = 10;
                        inf5j.moraleResist = 0;

                        auto cat5j = ss2::assets::UnitCatalog::fromSpecs(
                            {light5j, heavy5j, tgt5j, off5j, flak5j, m16j, howi5j, inf5j},
                            {leaf5j, bf109j, ju87j});

                        const auto [lx, ly] = tc64(40, 20);
                        const auto [hx, hy] = tc64(41, 20);
                        const auto [tx, ty] = tc64(43, 20);
                        std::vector<ss2::assets::Infantry> noInf5j;
                        std::vector<ss2::assets::Placement> vehShock{
                            {"light_5j", lx, ly, 0, 100, 100, 1, 1},
                            {"heavy_5j", hx, hy, 0, 100, 0, 1, 1},
                        };
                        std::vector<ss2::assets::Infantry> offRallyInf{
                            {"off_5j", lx, ly, 150, 0, 1, 1},
                        };
                        ss2::TutorialUnits simSoloShock(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5j, obst5j),
                            noInf5j, vehShock, ss2::FogGrid(64, 64, flags5j), 2, false, {}, cat5j);
                        ss2::TutorialUnits simRallyShock(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5j, obst5j),
                            offRallyInf, vehShock, ss2::FogGrid(64, 64, flags5j), 2, false, {}, cat5j);

                        std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air5j{};
                        air5j[0][0] = {"b-17c_agit", 1, 1};
                        std::array<int, 4> pGrp5j{};
                        std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos5j{};
                        simSoloShock.setAirSupport(air5j, pGrp5j, pRos5j);
                        simRallyShock.setAirSupport(air5j, pGrp5j, pRos5j);
                        if (!simSoloShock.callAirSupport(ss2::assets::AirMissionKind::Bomber, lx, ly, 0) ||
                            !simRallyShock.callAirSupport(ss2::assets::AirMissionKind::Bomber, lx, ly, 0)) {
                            throw std::runtime_error("Phase 5J leaflet setup failed");
                        }

                        bool lightShocked = false;
                        bool heavyClearedEarly = false;
                        bool soloStillSuppressedAfter8 = false;
                        bool rallyClearedBy8 = false;
                        int lastDropTick = -1;
                        bool curLightSup = false;
                        bool curHeavySup = false;
                        bool curRallySup = false;
                        for (int t = 0; t < 260; ++t) {
                            (void)simSoloShock.step();
                            (void)simRallyShock.step();
                            for (const auto& vp : simSoloShock.takeVehiclePoses()) {
                                if (vp.index == 0u) curLightSup = vp.suppressed;
                                if (vp.index == 1u) curHeavySup = vp.suppressed;
                            }
                            for (const auto& vp : simRallyShock.takeVehiclePoses()) {
                                if (vp.index == 0u) curRallySup = vp.suppressed;
                            }
                            for (const auto& ae : simSoloShock.takeAirStrikeEvents()) {
                                if (ae.kind == ss2::AirStrikeEvent::BombImpact && ae.isLeaflet) {
                                    lastDropTick = t;
                                    if (curLightSup) lightShocked = true;
                                }
                            }
                            if (curLightSup) lightShocked = true;
                            (void)simRallyShock.takeAirStrikeEvents();
                            if (lastDropTick >= 0 && t == lastDropTick + 12) {
                                soloStillSuppressedAfter8 = curLightSup;
                                rallyClearedBy8 = !curRallySup;
                            }
                            if (lastDropTick >= 0 && t == lastDropTick + 24) {
                                heavyClearedEarly = !curHeavySup && curLightSup;
                            }
                        }
                        if (!lightShocked || !heavyClearedEarly || !soloStillSuppressedAfter8 || !rallyClearedBy8) {
                            throw std::runtime_error("Phase 5J moraleresist vehicle shock or Officer rally check failed");
                        }

                        // Measure reload gap: suppressed light_5j (owner 0) vs clean light_5j (owner 0) attacking tgt_5j (owner 1)
                        std::vector<ss2::assets::Placement> vehReload{
                            {"light_5j", lx, ly, 0, 100, 100, 1, 0},
                            {"tgt_5j", tx, ty, 0, 100, 0, 1, 1},
                        };
                        ss2::TutorialUnits simCleanFire(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5j, obst5j),
                            noInf5j, vehReload, ss2::FogGrid(64, 64, flags5j), 2, false, {}, cat5j);
                        ss2::TutorialUnits simFireShocked(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5j, obst5j),
                            noInf5j, vehReload, ss2::FogGrid(64, 64, flags5j), 2, false, {}, cat5j);
                        std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> airEnemy5j{};
                        airEnemy5j[1][0] = {"b-17c_agit", 1, 1};
                        simFireShocked.setAirSupport(airEnemy5j, pGrp5j, pRos5j);
                        if (!simFireShocked.callAirSupport(ss2::assets::AirMissionKind::Bomber, lx, ly, 1)) {
                            throw std::runtime_error("Phase 5J shocked reload bomber setup failed");
                        }
                        int shockedDrops = 0;
                        for (int t = 0; t < 240 && shockedDrops < 3; ++t) {
                            (void)simFireShocked.step();
                            for (const auto& ae : simFireShocked.takeAirStrikeEvents()) {
                                if (ae.kind == ss2::AirStrikeEvent::BombImpact) ++shockedDrops;
                            }
                        }
                        simFireShocked.takeVehiclePoses();
                        if (simCleanFire.attackWithVehicles(std::array<size_t, 1>{0}, true, 1) != 1u ||
                            simFireShocked.attackWithVehicles(std::array<size_t, 1>{0}, true, 1) != 1u) {
                            throw std::runtime_error("Phase 5J attackWithVehicles setup failed");
                        }
                        int lastCleanShot = -1, cleanGap = -1;
                        int lastSupShot = -1, supGap = -1;
                        for (int t = 0; t < 65 && (cleanGap < 0 || supGap < 0); ++t) {
                            (void)simCleanFire.step();
                            for (const auto& vp : simCleanFire.takeVehiclePoses()) {
                                if (vp.index == 0u && vp.fired) {
                                    if (lastCleanShot >= 0 && cleanGap < 0) cleanGap = t - lastCleanShot;
                                    lastCleanShot = t;
                                }
                            }
                            (void)simFireShocked.step();
                            for (const auto& vp : simFireShocked.takeVehiclePoses()) {
                                if (vp.index == 0u && vp.fired) {
                                    if (lastSupShot >= 0 && supGap < 0) supGap = t - lastSupShot;
                                    lastSupShot = t;
                                }
                            }
                        }
                        if (cleanGap <= 0 || supGap <= cleanGap + 4) {
                            throw std::runtime_error("Phase 5J suppressed vehicle reload penalty check failed");
                        }

                        // (B) AVIA shot_air_sound (mg_aa) vs Ground AA flak vs mg2_aa distinction
                        const auto checkAaSound = [&](bool useFighter, const char* groundId, std::string_view expectedSnd) {
                            std::vector<ss2::assets::Placement> aaVehs;
                            if (!useFighter && groundId) {
                                aaVehs.push_back({groundId, lx, ly, 0, 100, 100, 1, 0});
                            }
                            ss2::TutorialUnits simAa(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5j, obst5j),
                                noInf5j, aaVehs, ss2::FogGrid(64, 64, flags5j), 2, false, {}, cat5j);
                            std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> slots{};
                            slots[1][static_cast<std::size_t>(ss2::assets::AirMissionKind::Bomber)] = {"ju-87", 1, 1};
                            if (useFighter) {
                                slots[0][static_cast<std::size_t>(ss2::assets::AirMissionKind::Fighter)] = {"bf-109", 1, 1};
                            }
                            simAa.setAirSupport(slots, pGrp5j, pRos5j);
                            if (!simAa.callAirSupport(ss2::assets::AirMissionKind::Bomber, lx, ly, 1)) {
                                throw std::runtime_error("Phase 5J AA bomber setup failed");
                            }
                            if (useFighter && !simAa.callAirSupport(ss2::assets::AirMissionKind::Fighter, lx, ly, 0)) {
                                throw std::runtime_error("Phase 5J AA fighter setup failed");
                            }
                            std::string gotSnd;
                            for (int t = 0; t < 240 && gotSnd.empty(); ++t) {
                                (void)simAa.step();
                                for (const auto& sp : simAa.takeAirSorties()) {
                                    if (sp.owner == 1 && sp.aaFired) {
                                        gotSnd = sp.aaSoundGroup;
                                    }
                                }
                            }
                            if (gotSnd != expectedSnd) {
                                throw std::runtime_error(std::string("Phase 5J AA audio distinction mismatch: expected ") +
                                                         std::string(expectedSnd) + ", got " + gotSnd);
                            }
                        };
                        checkAaSound(true, nullptr, "mg_aa");
                        checkAaSound(false, "flak88_5j", "flak");
                        checkAaSound(false, "m16_5j", "mg2_aa");

                        // (C) Building bombardment occupant suppression on heavy HE hit & evacuation
                        const auto [hX, hY] = tc64(30, 30);
                        const auto [gX, gY] = tc64(27, 30);
                        std::vector<ss2::assets::Infantry> bldInf{{"inf_5j", hX, hY, 120, 40, 1, 1}};
                        std::vector<ss2::assets::Placement> bldAtt{{"howi_5j", gX, gY, 0, 100, 100, 1, 0}};
                        ss2::TutorialUnits simBldSup(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5j, obst5j),
                            bldInf, bldAtt, ss2::FogGrid(64, 64, flags5j), 2, false, {}, cat5j);
                        ss2::assets::BuildingPlacement house5j{};
                        house5j.x = hX; house5j.y = hY;
                        house5j.left = hX - 24; house5j.top = hY - 24;
                        house5j.width = 48; house5j.height = 48;
                        house5j.capacity = 4; house5j.maxHp = 80;
                        simBldSup.setBuildings({house5j});
                        auto snapB = simBldSup.takeSnapshot();
                        snapB.buildings[0].owner = 1;
                        snapB.buildings[0].occupants = {0};
                        snapB.soldiers[0].garrisonBuilding = 0;
                        if (!simBldSup.loadSnapshot(snapB)) {
                            throw std::runtime_error("Phase 5J garrison snapshot load failed");
                        }
                        if (simBldSup.attackBuildingWithVehicles(std::array<size_t, 1>{0}, 0) != 1u) {
                            throw std::runtime_error("Phase 5J attackBuildingWithVehicles failed");
                        }
                        bool sawGarrisonSuppressed = false;
                        bool sawEvacSuppressed = false;
                        for (int t = 0; t < 80 && !(sawGarrisonSuppressed && sawEvacSuppressed); ++t) {
                            for (const auto& sp : simBldSup.step()) {
                                if (sp.index == 0u && sp.alive) {
                                    if (sp.garrisoned && sp.suppressed) sawGarrisonSuppressed = true;
                                    if (!sp.garrisoned && sp.suppressed) sawEvacSuppressed = true;
                                }
                            }
                        }
                        if (!sawGarrisonSuppressed || !sawEvacSuppressed) {
                            throw std::runtime_error("Phase 5J building bombardment occupant suppression check failed");
                        }

                        std::cout << "Phase 5J retail moraleresist vehicle shock, AVIA/flak AA audio distinction & building occupant suppression PASSED\n";
                    }

                    // Phase 5K: retail SHOTS/EXPLOSIONS indirectArc splash (+radius), craterScale pose
                    // & crewcanbehealed open-cab 2x medic triage.
                    {
                        std::vector<std::uint32_t> flags5k(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5k(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        // (A) Retail crewcanbehealed open-cab 2x field triage vs closed armor
                        ss2::assets::UnitSpec openJeep{};
                        openJeep.id = "jeep_5k";
                        openJeep.isVehicle = true;
                        openJeep.health = 200;
                        openJeep.crewNumber = 4;
                        openJeep.attackCrew = 1;
                        openJeep.crewCanBeHealed = true;
                        openJeep.sight = 8;

                        ss2::assets::UnitSpec closedTank{};
                        closedTank.id = "tank_5k";
                        closedTank.isVehicle = true;
                        closedTank.health = 600;
                        closedTank.crewNumber = 4;
                        closedTank.attackCrew = 1;
                        closedTank.crewCanBeHealed = false;
                        closedTank.sight = 8;

                        ss2::assets::UnitSpec medic5k{};
                        medic5k.id = "cmedic_5k";
                        medic5k.selType = "medic";
                        medic5k.health = 100;
                        medic5k.ammo1 = 0;
                        medic5k.sight = 8;

                        // (B) indirectArc howitzer vs direct HE tank
                        ss2::assets::UnitSpec howi5k{};
                        howi5k.id = "howi_5k";
                        howi5k.isVehicle = true;
                        howi5k.selType = "howitzer";
                        howi5k.health = 400;
                        howi5k.ammo1 = 30;
                        howi5k.sight = 14;
                        howi5k.shot1 = {"ballistic", ss2::assets::HitType::Explosive, 120, 120, 600, 600, 0, 20, 1, 20, "howitzer"};
                        howi5k.shot1.indirectArc = true;
                        howi5k.shot1.craterScale = 2;

                        ss2::assets::UnitSpec direct5k{};
                        direct5k.id = "direct_5k";
                        direct5k.isVehicle = true;
                        direct5k.health = 500;
                        direct5k.ammo1 = 30;
                        direct5k.sight = 14;
                        direct5k.shot1 = {"pierce8-10k", ss2::assets::HitType::Explosive, 120, 120, 600, 600, 0, 20, 1, 20, "gunshot"};
                        direct5k.shot1.indirectArc = false;
                        direct5k.shot1.craterScale = 4;

                        ss2::assets::UnitSpec targetInf5k{};
                        targetInf5k.id = "tgtinf_5k";
                        targetInf5k.health = 100;
                        targetInf5k.ammo1 = 0;
                        targetInf5k.sight = 4;

                        auto cat5k = ss2::assets::UnitCatalog::fromSpecs(
                            {openJeep, closedTank, medic5k, howi5k, direct5k, targetInf5k});

                        const auto [mx, my] = tc64(20, 20);
                        const auto [vx, vy] = tc64(21, 20); // ~36px < 96px medic triage radius
                        std::vector<ss2::assets::Infantry> infMed5k{{"cmedic_5k", mx, my, 100, 0, 1, 0}};
                        ss2::TutorialUnits simOpen(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5k, obst5k),
                            infMed5k, std::vector<ss2::assets::Placement>{{"jeep_5k", vx, vy, 0, 100, 0, 1, 0}},
                            ss2::FogGrid(64, 64, flags5k), 2, false, {}, cat5k);
                        ss2::TutorialUnits simClosed(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5k, obst5k),
                            infMed5k, std::vector<ss2::assets::Placement>{{"tank_5k", vx, vy, 0, 100, 0, 1, 0}},
                            ss2::FogGrid(64, 64, flags5k), 2, false, {}, cat5k);
                        auto snapOpen = simOpen.takeSnapshot();
                        auto snapClosed = simClosed.takeSnapshot();
                        snapOpen.vehicles[0].crewHp = 100;   // maxCrewHp = 70 + 4*35 = 210
                        snapClosed.vehicles[0].crewHp = 100;
                        if (!simOpen.loadSnapshot(snapOpen) || !simClosed.loadSnapshot(snapClosed)) {
                            throw std::runtime_error("Phase 5K loadSnapshot failed");
                        }
                        for (int t = 0; t < 26; ++t) {
                            (void)simOpen.step();
                            (void)simClosed.step();
                        }
                        const int openCrewAfter = simOpen.takeSnapshot().vehicles[0].crewHp;
                        const int closedCrewAfter = simClosed.takeSnapshot().vehicles[0].crewHp;
                        if (openCrewAfter != 170 || closedCrewAfter != 135) {
                            throw std::runtime_error("Phase 5K crewcanbehealed 2x medic triage check failed");
                        }

                        // (B) indirectArc splash radius (92px vs 72px) and VehiclePose::craterScale
                        const auto [sx5k, sy5k] = tc64(26, 20);
                        std::vector<ss2::assets::Infantry> tgtInfVec{{"tgtinf_5k", sx5k, sy5k, 100, 0, 11, 1}};
                        ss2::TutorialUnits simHowi(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5k, obst5k),
                            tgtInfVec, std::vector<ss2::assets::Placement>{{"howi_5k", mx, my, 0, 100, 100, 1, 0}},
                            ss2::FogGrid(64, 64, flags5k), 2, false, {}, cat5k);
                        ss2::TutorialUnits simDirect(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5k, obst5k),
                            tgtInfVec, std::vector<ss2::assets::Placement>{{"direct_5k", mx, my, 0, 100, 100, 1, 0}},
                            ss2::FogGrid(64, 64, flags5k), 2, false, {}, cat5k);
                        const int impX = sx5k - 82; // exactly 82px away: 72 < 82 <= 92
                        const int impY = sy5k;
                        if (simHowi.orderAttackGroundVehicles(std::array<size_t, 1>{0}, impX, impY) != 1u ||
                            simDirect.orderAttackGroundVehicles(std::array<size_t, 1>{0}, impX, impY) != 1u) {
                            throw std::runtime_error("Phase 5K orderAttackGroundVehicles failed");
                        }
                        std::uint8_t howiCrater = 0;
                        std::uint8_t directCrater = 0;
                        for (int t = 0; t < 15 && (howiCrater == 0 || directCrater == 0); ++t) {
                            (void)simHowi.step();
                            (void)simDirect.step();
                            for (const auto& vp : simHowi.takeVehiclePoses()) {
                                if (vp.index == 0u && vp.fired && vp.heavyImpact) howiCrater = vp.craterScale;
                            }
                            for (const auto& vp : simDirect.takeVehiclePoses()) {
                                if (vp.index == 0u && vp.fired && vp.heavyImpact) directCrater = vp.craterScale;
                            }
                        }
                        const int howiTgtHp = simHowi.takeSnapshot().soldiers[0].hp;
                        const int directTgtHp = simDirect.takeSnapshot().soldiers[0].hp;
                        if (howiCrater != 2u || directCrater != 4u ||
                            howiTgtHp >= 100 || directTgtHp != 100) {
                            throw std::runtime_error("Phase 5K indirectArc splash or craterScale pose check failed");
                        }

                        std::cout << "Phase 5K retail SHOTS/EXPLOSIONS indirectArc splash, craterScale pose & crewcanbehealed 2x triage PASSED\n";
                    }

                    // Phase 5L: retail LAND\DEFDOM building armor & Fire cover bypass.
                    {
                        std::vector<std::uint32_t> flags5l(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5l(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        ss2::assets::UnitSpec occ5l{};
                        occ5l.id = "occ_5l";
                        occ5l.health = 100;
                        occ5l.ammo1 = 0;
                        occ5l.sight = 6;

                        ss2::assets::UnitSpec rifle5l{};
                        rifle5l.id = "rifle_5l";
                        rifle5l.health = 100;
                        rifle5l.ammo1 = 60;
                        rifle5l.sight = 14;
                        rifle5l.shot1 = {"man_machine", ss2::assets::HitType::Machine, 18, 18, 400, 400, 0, 10, 1, 10, "gunshot"};

                        ss2::assets::UnitSpec flamer5l{};
                        flamer5l.id = "flamer_5l";
                        flamer5l.selType = "flameinf";
                        flamer5l.health = 100;
                        flamer5l.ammo1 = 60;
                        flamer5l.sight = 14;
                        flamer5l.shot1 = {"flamethrower", ss2::assets::HitType::Fire, 60, 60, 400, 400, 0, 10, 1, 10, "fmw"};

                        auto cat5l = ss2::assets::UnitCatalog::fromSpecs({occ5l, rifle5l, flamer5l});

                        const auto [bX5l, bY5l] = tc64(25, 20);
                        const auto [aX5l, aY5l] = tc64(23, 20);
                        ss2::assets::BuildingPlacement house5l{};
                        house5l.x = bX5l; house5l.y = bY5l;
                        house5l.left = bX5l - 24; house5l.top = bY5l - 24;
                        house5l.width = 48; house5l.height = 48;
                        house5l.capacity = 4; house5l.maxHp = 200;

                        ss2::TutorialUnits simRifleBld(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5l, obst5l),
                            std::vector<ss2::assets::Infantry>{
                                {"occ_5l", bX5l, bY5l, 100, 0, 11, 1},
                                {"rifle_5l", aX5l, aY5l, 100, 60, 1, 0},
                            },
                            std::vector<ss2::assets::Placement>{},
                            ss2::FogGrid(64, 64, flags5l), 2, false, {}, cat5l);
                        ss2::TutorialUnits simFlamerBld(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5l, obst5l),
                            std::vector<ss2::assets::Infantry>{
                                {"occ_5l", bX5l, bY5l, 100, 0, 11, 1},
                                {"flamer_5l", aX5l, aY5l, 100, 60, 1, 0},
                            },
                            std::vector<ss2::assets::Placement>{},
                            ss2::FogGrid(64, 64, flags5l), 2, false, {}, cat5l);
                        simRifleBld.setBuildings({house5l});
                        simFlamerBld.setBuildings({house5l});
                        auto loadGar = [](ss2::TutorialUnits& sim) {
                            auto sn = sim.takeSnapshot();
                            sn.buildings[0].owner = 1;
                            sn.buildings[0].occupants = {0};
                            sn.soldiers[0].garrisonBuilding = 0;
                            if (!sim.loadSnapshot(sn)) {
                                throw std::runtime_error("Phase 5L garrison snapshot load failed");
                            }
                        };
                        loadGar(simRifleBld);
                        loadGar(simFlamerBld);
                        simRifleBld.revealFogAt(bX5l, bY5l, 12, 0);
                        simFlamerBld.revealFogAt(bX5l, bY5l, 12, 0);

                        if (simRifleBld.orderAttackBuilding(std::array<size_t, 1>{1}, 0) != 1u ||
                            simFlamerBld.orderAttackBuilding(std::array<size_t, 1>{1}, 0) != 1u) {
                            throw std::runtime_error("Phase 5L orderAttackBuilding setup failed");
                        }
                        for (int t = 0; t < 30; ++t) {
                            (void)simRifleBld.step();
                            (void)simFlamerBld.step();
                        }
                        const auto snapRifle = simRifleBld.takeSnapshot();
                        const auto snapFlamer = simFlamerBld.takeSnapshot();
                        if (snapRifle.buildings[0].hp != 200 || snapRifle.soldiers[0].hp != 100 ||
                            snapFlamer.buildings[0].hp >= 200 || snapFlamer.soldiers[0].hp >= 100) {
                            throw std::runtime_error("Phase 5L LAND\\DEFDOM building armor or Fire bypass check failed");
                        }

                        std::cout << "Phase 5L retail LAND\\DEFDOM building armor & Fire cover bypass PASSED\n";
                    }

                    // Phase 5M: retail AVIA altitude-scaled AA flak, il-2 6-rocket salvo & LAND\DOM garrison range.
                    {
                        std::vector<std::uint32_t> flags5m(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5m(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        ss2::assets::UnitSpec flak88m{};
                        flak88m.id = "flak88_5m";
                        flak88m.isVehicle = true;
                        flak88m.roleClass = "antiaircraft";
                        flak88m.selType = "mobairdef";
                        flak88m.health = 200;
                        flak88m.ammo1 = 60;
                        flak88m.sight = 14;
                        flak88m.shot1 = {"flak_he", ss2::assets::HitType::Explosive, 180, 180, 600, 600, 0, 20, 1, 20, "flak"};

                        ss2::assets::UnitSpec flak20m{};
                        flak20m.id = "wirbel_5m";
                        flak20m.isVehicle = true;
                        flak20m.selType = "aagun";
                        flak20m.health = 240;
                        flak20m.ammo1 = 200;
                        flak20m.sight = 14;
                        flak20m.shot1 = {"autocannon", ss2::assets::HitType::Machine, 35, 35, 500, 500, 0, 10, 4, 20, "flak_20mm"};

                        ss2::assets::PlaneSpec b17m{};
                        b17m.id = "b17_5m";
                        b17m.health = 800;
                        b17m.flySpeed = 6.0;
                        b17m.altitude = 356;
                        b17m.bombsNumber = 3;
                        b17m.bombDamage = 0;

                        ss2::assets::PlaneSpec il2m{};
                        il2m.id = "il2_5m";
                        il2m.health = 800;
                        il2m.flySpeed = 6.0;
                        il2m.altitude = 200;
                        il2m.groundAmmo = 6;
                        il2m.groundDamage = 40;
                        il2m.groundShotSound = "rocket";

                        ss2::assets::PlaneSpec bf109m{};
                        bf109m.id = "bf109_5m";
                        bf109m.health = 800;
                        bf109m.flySpeed = 8.0;
                        bf109m.altitude = 366;
                        bf109m.groundAmmo = 550;
                        bf109m.groundDamage = 30;
                        bf109m.groundShotSound = "mg2_aa";

                        auto cat5m = ss2::assets::UnitCatalog::fromSpecs({flak88m, flak20m}, {b17m, il2m, bf109m});

                        const auto [cx, cy] = tc64(32, 32);
                        std::vector<ss2::assets::Infantry> noInf5m;
                        std::array<int, 4> pGrp5m{};
                        std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos5m{};

                        auto runAaPass = [&](const char* aaId, const char* planeId, ss2::assets::AirMissionKind kind) {
                            ss2::TutorialUnits simAa(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5m, obst5m),
                                noInf5m, std::vector<ss2::assets::Placement>{{aaId, cx, cy, 0, 100, 100, 1, 0}},
                                ss2::FogGrid(64, 64, flags5m), 2, false, {}, cat5m);
                            std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> slots{};
                            slots[1][static_cast<std::size_t>(kind)] = {planeId, 1, 1};
                            simAa.setAirSupport(slots, pGrp5m, pRos5m);
                            if (!simAa.callAirSupport(kind, cx, cy, 1)) {
                                throw std::runtime_error("Phase 5M callAirSupport AA setup failed");
                            }
                            int minHp = 9999;
                            for (int t = 0; t < 240; ++t) {
                                (void)simAa.step();
                                for (const auto& sp : simAa.takeAirSorties()) {
                                    if (sp.owner == 1) minHp = std::min(minHp, sp.hp);
                                }
                            }
                            return minHp;
                        };
                        const int hp88High = runAaPass("flak88_5m", "b17_5m", ss2::assets::AirMissionKind::Bomber);
                        const int hp20High = runAaPass("wirbel_5m", "b17_5m", ss2::assets::AirMissionKind::Bomber);
                        const int hp20Low = runAaPass("wirbel_5m", "il2_5m", ss2::assets::AirMissionKind::Fighter);
                        if (hp88High >= hp20High - 80 || hp20Low >= hp20High - 60) {
                            throw std::runtime_error("Phase 5M altitude-scaled heavy 88mm vs light 20mm AA check failed");
                        }

                        // il-2 6-rocket ground-attack salvo vs standard 3-pass fighter
                        auto countStrafes = [&](const char* planeId) {
                            ss2::TutorialUnits simStrafe(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5m, obst5m),
                                noInf5m, {}, ss2::FogGrid(64, 64, flags5m), 2, false, {}, cat5m);
                            std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> slots{};
                            slots[0][static_cast<std::size_t>(ss2::assets::AirMissionKind::Fighter)] = {planeId, 1, 1};
                            simStrafe.setAirSupport(slots, pGrp5m, pRos5m);
                            if (!simStrafe.callAirSupport(ss2::assets::AirMissionKind::Fighter, cx, cy, 0)) {
                                throw std::runtime_error("Phase 5M callAirSupport strafe setup failed");
                            }
                            int hits = 0;
                            for (int t = 0; t < 320; ++t) {
                                (void)simStrafe.step();
                                for (const auto& ae : simStrafe.takeAirStrikeEvents()) {
                                    if (ae.kind == ss2::AirStrikeEvent::StrafeImpact) ++hits;
                                }
                            }
                            return hits;
                        };
                        const int il2Hits = countStrafes("il2_5m");
                        const int bf109Hits = countStrafes("bf109_5m");
                        if (il2Hits != 6 || bf109Hits != 3) {
                            throw std::runtime_error("Phase 5M il-2 6-rocket salvo vs fighter 3-pass check failed");
                        }

                        std::cout << "Phase 5M retail AVIA altitude-scaled flak, il-2 6-rocket salvo & LAND\\DOM garrison range PASSED\n";
                    }

                    // Phase 5N: retail MISC\MINES warfare parity (sympathetic chain detonation, crew shock,
                    // building Mine damage, desant rider casualties with tanksonarmorprotection, AP vs AT distinction)
                    // and LAND\DOM bonus_ShotDeadZone for heavy-deadzone garrison weapons.
                    {
                        std::vector<std::uint32_t> flags5n(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5n(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        ss2::assets::UnitSpec tank5n{};
                        tank5n.id = "tank_5n";
                        tank5n.isVehicle = true;
                        tank5n.roleClass = "medtank";
                        tank5n.health = 800;
                        tank5n.ammo1 = 20;
                        tank5n.crewNumber = 4;
                        tank5n.attackCrew = 2;
                        tank5n.moveCrew = 1;
                        tank5n.stock = 4;
                        tank5n.soldiersOnArmor = true;
                        tank5n.moraleResist = 2048;
                        tank5n.sight = 14;
                        tank5n.moveSpeed = 1.0;

                        ss2::assets::UnitSpec rider5n{};
                        rider5n.id = "rider_5n";
                        rider5n.health = 180;
                        rider5n.ammo1 = 40;
                        rider5n.sight = 12;

                        ss2::assets::UnitSpec foot5n{};
                        foot5n.id = "foot_5n";
                        foot5n.health = 220;
                        foot5n.ammo1 = 40;
                        foot5n.sight = 12;

                        ss2::assets::UnitSpec heavyGar5n{};
                        heavyGar5n.id = "heavy_gar_5n";
                        heavyGar5n.health = 120;
                        heavyGar5n.ammo1 = 30;
                        heavyGar5n.sight = 14;
                        heavyGar5n.canFireFromInside = true;
                        heavyGar5n.hasExplicitInsideRule = true;
                        heavyGar5n.shot1 = {"mortar", ss2::assets::HitType::Explosive, 40, 40, 300, 300, 60, 10, 1, 10, "gunshot"};

                        ss2::assets::UnitSpec rifleGar5n{};
                        rifleGar5n.id = "rifle_gar_5n";
                        rifleGar5n.health = 120;
                        rifleGar5n.ammo1 = 60;
                        rifleGar5n.sight = 14;
                        rifleGar5n.shot1 = {"man_rifle", ss2::assets::HitType::Sniper, 25, 25, 300, 300, 8, 8, 1, 8, "gunshot"};

                        auto cat5n = ss2::assets::UnitCatalog::fromSpecs({tank5n, rider5n, foot5n, heavyGar5n, rifleGar5n});

                        const auto [atX0, atY0] = tc64(16, 16);
                        const auto [atX1, atY1] = tc64(17, 16);
                        const auto [bldX, bldY] = tc64(16, 17);
                        const auto [apTankX, apTankY] = tc64(30, 30);
                        const auto [apFootX, apFootY] = tc64(34, 30);

                        std::vector<ss2::assets::Infantry> inf5n{
                            {"rider_5n", atX0 + 48, atY0, 180, 40, 1, 0},
                            {"foot_5n", atX1, atY1, 220, 40, 2, 0},
                            {"foot_5n", apFootX, apFootY, 220, 40, 3, 0},
                        };
                        std::vector<ss2::assets::Placement> veh5n{
                            {"tank_5n", atX0, atY0, 0, 100, 100, 1, 0},
                            {"tank_5n", apTankX, apTankY, 0, 100, 100, 4, 0},
                        };
                        ss2::TutorialUnits simMine(
                            ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5n, obst5n),
                            inf5n, veh5n, ss2::FogGrid(64, 64, flags5n), 2, false, {}, cat5n);
                        simMine.setBuildings({
                            {bldX, bldY, bldX - 18, bldY - 18, 36, 36, 4, 500, 1}
                        });
                        if (!simMine.embarkImmediate(0, 0)) {
                            throw std::runtime_error("Phase 5N embarkImmediate rider onto tank failed");
                        }
                        std::vector<std::uint8_t> mines5n(64u * 64u, 0u);
                        mines5n[16u * 64u + 16u] = 0x45u; // enemy AT mine 0
                        mines5n[16u * 64u + 17u] = 0x45u; // adjacent enemy AT mine 1 (sympathetic chain)
                        mines5n[30u * 64u + 30u] = 0x41u; // enemy AP mine under tank 1 (should NOT detonate)
                        mines5n[30u * 64u + 34u] = 0x41u; // enemy AP mine under soldier 2 (should detonate)
                        simMine.setMines(std::move(mines5n));

                        const auto mineStepPoses = simMine.step();
                        const auto mineEvents5n = simMine.takeMineEvents();
                        const auto vehPoses5n = simMine.takeVehiclePoses();
                        const auto bldUpd5n = simMine.takeBuildingUpdates();

                        int atDets = 0;
                        int apDets = 0;
                        for (const auto& me : mineEvents5n) {
                            if (me.kind == ss2::MineEvent::Detonated) {
                                if (me.antiTank) ++atDets;
                                else ++apDets;
                            }
                        }
                        bool tank0ShockedAndDamaged = false;
                        bool tank1Unharmed = true;
                        for (const auto& vp : vehPoses5n) {
                            if (vp.index == 0u && vp.hp < 800 && vp.suppressed) tank0ShockedAndDamaged = true;
                            if (vp.index == 1u && vp.hp < 800) tank1Unharmed = false;
                        }
                        bool footDamagedAndSuppressed = false;
                        bool apFootDamagedAndSuppressed = false;
                        for (const auto& sp : mineStepPoses) {
                            if (sp.index == 1u && sp.hp < 220 && sp.suppressed) footDamagedAndSuppressed = true;
                            if (sp.index == 2u && sp.hp < 220 && sp.suppressed) apFootDamagedAndSuppressed = true;
                        }
                        bool bldDamaged = false;
                        for (const auto& bu : bldUpd5n) {
                            if (bu.index == 0u && bu.hp < 500) bldDamaged = true;
                        }
                        if (atDets != 2 || apDets != 1 || !tank0ShockedAndDamaged || !tank1Unharmed ||
                            !footDamagedAndSuppressed || !apFootDamagedAndSuppressed || !bldDamaged) {
                            throw std::runtime_error("Phase 5N mine chain/shock/building blast or AP vs AT check failed");
                        }
                        std::vector<ss2::SoldierPose> disembarkPoses;
                        (void)simMine.unloadVehicles(std::array<size_t, 1>{0}, &disembarkPoses);
                        bool riderHurtAndShocked = false;
                        for (const auto& sp : disembarkPoses) {
                            // Two mine hits: 2 * (clamp(260/4, 12, 65) - 3) = 2 * 62 = 124 -> hp == 56
                            if (sp.index == 0u && sp.hp == 56 && sp.suppressed) riderHurtAndShocked = true;
                        }
                        if (!riderHurtAndShocked) {
                            throw std::runtime_error("Phase 5N tanksonarmorprotection rider casualty/shock check failed");
                        }

                        // LAND\DOM bonus_ShotDeadZone: heavy-deadzone weapon respects deadzone + bonus from inside,
                        // whereas ordinary rifleman keeps point-blank self-defense.
                        auto testGarDeadzone = [&](const char* unitId, int distPx) {
                            const auto [hX, hY] = tc64(20, 20);
                            ss2::TutorialUnits simDz(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5n, obst5n),
                                std::vector<ss2::assets::Infantry>{
                                    {unitId, hX, hY, 120, 60, 1, 0},
                                    {"foot_5n", hX + distPx, hY, 220, 0, 2, 1},
                                },
                                {}, ss2::FogGrid(64, 64, flags5n), 2, true, {}, cat5n);
                            simDz.setBuildings({{hX, hY, hX - 20, hY - 20, 40, 40, 4, 400}});
                            simDz.revealFogAt(hX, hY, 18, 0);
                            if (simDz.orderGarrisonBuilding(std::array<size_t, 1>{0}, 0) != 1) {
                                throw std::runtime_error("Phase 5N orderGarrisonBuilding setup failed");
                            }
                            bool fired = false;
                            for (int t = 0; t < 40; ++t) {
                                for (const auto& sp : simDz.step()) {
                                    if (sp.index == 0u && sp.fired && sp.garrisoned) fired = true;
                                }
                            }
                            return fired;
                        };
                        if (testGarDeadzone("heavy_gar_5n", 75) ||
                            !testGarDeadzone("heavy_gar_5n", 180) ||
                            !testGarDeadzone("rifle_gar_5n", 60)) {
                            throw std::runtime_error("Phase 5N LAND\\DOM bonus_ShotDeadZone garrison check failed");
                        }

                        std::cout << "Phase 5N retail MISC\\MINES chain/shock/building blast, tanksonarmorprotection, LAND\\DOM deadzone & canbepara PASSED\n";
                    }

                    // Phase 5O: retail AVIA shot_grn_id Machine vs Explosive strafe parity,
                    // turret_type zenit shot2 AA turrets, *guncrewunit bailout, and moralenoattack fearless fire rate.
                    {
                        std::vector<std::uint32_t> flags5o(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5o(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        ss2::assets::UnitSpec heavyTank5o{};
                        heavyTank5o.id = "tank_5o";
                        heavyTank5o.isVehicle = true;
                        heavyTank5o.health = 600;
                        heavyTank5o.crewNumber = 4;
                        heavyTank5o.sight = 14;
                        heavyTank5o.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {256, 256, 256};
                        heavyTank5o.armor[static_cast<std::size_t>(ss2::assets::HitType::Explosive)] = {10, 10, 10};

                        ss2::assets::UnitSpec truck5o{};
                        truck5o.id = "truck_5o";
                        truck5o.isVehicle = true;
                        truck5o.health = 300;
                        truck5o.crewNumber = 2;
                        truck5o.sight = 12;

                        ss2::assets::UnitSpec turretAa5o{};
                        turretAa5o.id = "turret_aa_5o";
                        turretAa5o.isVehicle = true;
                        turretAa5o.turretType = "zenit";
                        turretAa5o.health = 400;
                        turretAa5o.ammo1 = 0;
                        turretAa5o.ammo2 = 100;
                        turretAa5o.sight = 16;
                        turretAa5o.shot2 = {"zenith_ac", ss2::assets::HitType::Air, 220, 246, 1200, 1300, 8, 4, 4, 80, "flak_37mm"};

                        ss2::assets::UnitSpec gun5o{};
                        gun5o.id = "gun_5o";
                        gun5o.isVehicle = true;
                        gun5o.roleClass = "at";
                        gun5o.health = 200;
                        gun5o.crewNumber = 2;
                        gun5o.crewUnit = "eguner";
                        gun5o.sight = 14;

                        ss2::assets::UnitSpec foot5o{};
                        foot5o.id = "foot_5o";
                        foot5o.health = 400;
                        foot5o.ammo1 = 40;
                        foot5o.sight = 14;

                        ss2::assets::UnitSpec hmg5o{};
                        hmg5o.id = "hmg_5o";
                        hmg5o.selType = "hmg";
                        hmg5o.health = 400;
                        hmg5o.ammo1 = 500;
                        hmg5o.sight = 14;
                        hmg5o.shot1 = {"man_machine", ss2::assets::HitType::Machine, 1, 1, 300, 300, 0, 2, 1, 2, "gunshot"};

                        ss2::assets::UnitSpec fearless5o{};
                        fearless5o.id = "fearless_5o";
                        fearless5o.health = 400;
                        fearless5o.ammo1 = 200;
                        fearless5o.sight = 14;
                        fearless5o.moraleNoAttack = -32767;
                        fearless5o.shot1 = {"man_rifle", ss2::assets::HitType::Sniper, 2, 2, 300, 300, 0, 10, 1, 10, "gunshot"};

                        ss2::assets::UnitSpec regular5o = fearless5o;
                        regular5o.id = "regular_5o";
                        regular5o.moraleNoAttack = 0;

                        ss2::assets::PlaneSpec bf109o{};
                        bf109o.id = "bf109_5o";
                        bf109o.health = 800;
                        bf109o.flySpeed = 8.0;
                        bf109o.altitude = 366;
                        bf109o.groundAmmo = 550;
                        bf109o.groundDamage = 45;
                        bf109o.groundShotId = "man_machine";
                        bf109o.groundHitType = ss2::assets::HitType::Machine;
                        bf109o.groundShotSound = "mg2_aa";

                        ss2::assets::PlaneSpec il2o{};
                        il2o.id = "il2_5o";
                        il2o.health = 800;
                        il2o.flySpeed = 6.0;
                        il2o.altitude = 200;
                        il2o.groundAmmo = 6;
                        il2o.groundDamage = 65;
                        il2o.groundShotId = "rs";
                        il2o.groundHitType = ss2::assets::HitType::Explosive;
                        il2o.groundShotSound = "rocket";

                        ss2::assets::PlaneSpec b17o{};
                        b17o.id = "b17_5o";
                        b17o.health = 900;
                        b17o.flySpeed = 7.0;
                        b17o.altitude = 360;

                        auto cat5o = ss2::assets::UnitCatalog::fromSpecs(
                            {heavyTank5o, truck5o, turretAa5o, gun5o, foot5o, hmg5o, fearless5o, regular5o},
                            {bf109o, il2o, b17o});

                        // 1. Fighter MG vs Sturmovik Rocket strafing HitType parity
                        const auto [cx, cy] = tc64(32, 32);
                        const auto [tx, ty] = tc64(33, 32);
                        const auto [fx, fy] = tc64(31, 32);
                        auto runStrafeParity = [&](const char* planeId) {
                            ss2::TutorialUnits simSt(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5o, obst5o),
                                std::vector<ss2::assets::Infantry>{{"foot_5o", fx, fy, 400, 40, 11, 1}},
                                std::vector<ss2::assets::Placement>{
                                    {"tank_5o", cx, cy, 0, 100, 100, 11, 1},
                                    {"truck_5o", tx, ty, 0, 100, 100, 12, 1},
                                },
                                ss2::FogGrid(64, 64, flags5o), 2, false, {}, cat5o);
                            simSt.setBuildings({{cx, cy, cx - 20, cy - 20, 40, 40, 4, 400, 1}});
                            std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> slots{};
                            slots[0][static_cast<std::size_t>(ss2::assets::AirMissionKind::Fighter)] = {planeId, 1, 1};
                            std::array<int, 4> pGrp{};
                            std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos{};
                            simSt.setAirSupport(slots, pGrp, pRos);
                            if (!simSt.callAirSupport(ss2::assets::AirMissionKind::Fighter, cx, cy, 0)) {
                                throw std::runtime_error("Phase 5O callAirSupport strafe failed");
                            }
                            bool tankSuppressed = false;
                            bool footSuppressed = false;
                            for (int t = 0; t < 320; ++t) {
                                for (const auto& sp : simSt.step()) {
                                    if (sp.index == 0u && sp.suppressed) footSuppressed = true;
                                }
                                for (const auto& vp : simSt.takeVehiclePoses()) {
                                    if (vp.index == 0u && vp.suppressed) tankSuppressed = true;
                                }
                            }
                            const auto snap = simSt.takeSnapshot();
                            struct Res { int tankHp; bool tankSup; int truckHp; int footHp; bool footSup; int bldHp; };
                            return Res{
                                snap.vehicles[0].hp, tankSuppressed, snap.vehicles[1].hp,
                                snap.soldiers[0].hp, footSuppressed, snap.buildings[0].hp
                            };
                        };
                        const auto mgRes = runStrafeParity("bf109_5o");
                        const auto rktRes = runStrafeParity("il2_5o");
                        if (mgRes.tankHp != 600 || mgRes.tankSup || mgRes.truckHp >= 300 ||
                            mgRes.footHp >= 400 || !mgRes.footSup || mgRes.bldHp != 400 ||
                            rktRes.tankHp >= 600 || rktRes.bldHp >= 400) {
                            throw std::runtime_error("Phase 5O Fighter MG vs Rocket strafe HitType check failed");
                        }

                        // 2. turret_type zenit secondary-only AA turret (ammo1=0, ammo2=100)
                        {
                            std::vector<ss2::assets::Infantry> noInf5o;
                            ss2::TutorialUnits simZenit(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5o, obst5o),
                                noInf5o,
                                std::vector<ss2::assets::Placement>{{"turret_aa_5o", cx, cy, 0, 100, 100, 1, 0}},
                                ss2::FogGrid(64, 64, flags5o), 2, false, {}, cat5o);
                            std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> slots{};
                            slots[1][static_cast<std::size_t>(ss2::assets::AirMissionKind::Bomber)] = {"b17_5o", 1, 1};
                            std::array<int, 4> pGrp{};
                            std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos{};
                            simZenit.setAirSupport(slots, pGrp, pRos);
                            if (!simZenit.callAirSupport(ss2::assets::AirMissionKind::Bomber, cx, cy, 1)) {
                                throw std::runtime_error("Phase 5O zenit bomber call failed");
                            }
                            int minB17Hp = 900;
                            int minTurretAmmo2 = 100;
                            for (int t = 0; t < 260; ++t) {
                                (void)simZenit.step();
                                for (const auto& sp : simZenit.takeAirSorties()) {
                                    if (sp.owner == 1) minB17Hp = std::min(minB17Hp, sp.hp);
                                }
                                for (const auto& vp : simZenit.takeVehiclePoses()) {
                                    if (vp.index == 0u) minTurretAmmo2 = std::min(minTurretAmmo2, vp.ammo2);
                                }
                            }
                            if (minB17Hp >= 900 || minTurretAmmo2 >= 100) {
                                throw std::runtime_error("Phase 5O zenit shot2 AA turret engagement failed");
                            }
                        }

                        // 3. *guncrewunit bailout & moralenoattack fearless fire rate under suppression
                        {
                            std::vector<ss2::assets::Infantry> noInf5o;
                            ss2::TutorialUnits simBail(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5o, obst5o),
                                noInf5o,
                                std::vector<ss2::assets::Placement>{{"gun_5o", cx, cy, 0, 100, 100, 1, 0}},
                                ss2::FogGrid(64, 64, flags5o), 2, false, {}, cat5o);
                            const auto bailCrew = simBail.abandonVehicles(std::array<size_t, 1>{0});
                            if (bailCrew.empty() || bailCrew[0].id != "eguner") {
                                throw std::runtime_error("Phase 5O *guncrewunit abandonVehicles eguner check failed");
                            }

                            auto countSuppressedShots = [&](const char* unitId) {
                                const auto [hx, hy] = tc64(20, 20);
                                const auto [sx, sy] = tc64(21, 20);
                                ss2::TutorialUnits simFr(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5o, obst5o),
                                    std::vector<ss2::assets::Infantry>{
                                        {unitId, sx, sy, 400, 200, 1, 0},
                                        {"hmg_5o", hx, hy, 400, 500, 11, 1},
                                    },
                                    {}, ss2::FogGrid(64, 64, flags5o), 2, true, {}, cat5o);
                                (void)simFr.orderAttack(std::array<size_t, 1>{0}, 1);
                                int shots = 0;
                                bool sawSup = false;
                                for (int t = 0; t < 100; ++t) {
                                    for (const auto& sp : simFr.step()) {
                                        if (sp.index == 0u) {
                                            if (sp.fired) ++shots;
                                            if (sp.suppressed) sawSup = true;
                                        }
                                    }
                                }
                                if (!sawSup) throw std::runtime_error("Phase 5O fearless test expected suppression");
                                return shots;
                            };
                            const int fearlessShots = countSuppressedShots("fearless_5o");
                            const int regularShots = countSuppressedShots("regular_5o");
                            if (fearlessShots <= regularShots) {
                                throw std::runtime_error("Phase 5O fearless unit did not fire faster than suppressed regular unit");
                            }
                        }

                        std::cout << "Phase 5O retail AVIA shot_grn_id Machine/Explosive strafe, zenit shot2 AA turrets, *guncrewunit & fearless fire rate PASSED\n";
                    }

                    // Phase 5P: end-to-end MissionRunner OP(3)/OP(4) 4-bit mask queries (incl. bit 3 pending),
                    // EXPLOSIONS vehicle cook-off building blast/suppression & PlaneShotDown crash blast.
                    {
                        std::vector<std::uint32_t> flags5p(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5p(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        // 1. Live MissionRunner OP(3)/OP(4) 4-bit mask evaluation (bits 0..3) on tick 0.
                        ss2::assets::UnitSpec truck5p{};
                        truck5p.id = "truck_5p";
                        truck5p.isVehicle = true;
                        truck5p.health = 300;
                        truck5p.dieExplosion = "dead_tank";
                        truck5p.sight = 12;

                        ss2::assets::UnitSpec tank5p{};
                        tank5p.id = "tank_5p";
                        tank5p.isVehicle = true;
                        tank5p.health = 500;
                        tank5p.crewNumber = 4;
                        tank5p.moraleResist = 2048;
                        tank5p.sight = 12;

                        ss2::assets::UnitSpec foot5p{};
                        foot5p.id = "foot_5p";
                        foot5p.health = 200;
                        foot5p.ammo1 = 40;
                        foot5p.sight = 12;

                        ss2::assets::UnitSpec flak88p{};
                        flak88p.id = "flak88_5p";
                        flak88p.isVehicle = true;
                        flak88p.roleClass = "antiaircraft";
                        flak88p.selType = "mobairdef";
                        flak88p.health = 500;
                        flak88p.ammo1 = 40;
                        flak88p.sight = 16;
                        flak88p.shot1 = {"tank_expl", ss2::assets::HitType::Explosive, 200, 220, 600, 650, 0, 12, 1, 12, "flak"};

                        ss2::assets::PlaneSpec b17p{};
                        b17p.id = "b17_5p";
                        b17p.health = 120;
                        b17p.flySpeed = 6.0;
                        b17p.altitude = 356;

                        auto cat5p = ss2::assets::UnitCatalog::fromSpecs(
                            {truck5p, tank5p, foot5p, flak88p}, {b17p});

                        {
                            ss2::assets::TutorialData m5p{};
                            m5p.width = 64;
                            m5p.height = 64;
                            m5p.flags = flags5p;
                            m5p.obstacles = obst5p;
                            m5p.zones.assign(64u * 64u, 0u);
                            m5p.phrases.resize(4);
                            m5p.reinforcements[0] = {
                                {"foot_5p", 42, 100, 100, 0},
                                {"foot_5p", 42, 100, 100, 0},
                            };
                            m5p.scripts.resize(5);
                            // Script 0: OP(3) mask=1(active crew), group=42, mode=0(==), thresh=0 => Message 0
                            m5p.scripts[0] = {0x80000001u, 0x8000002au, 0x80000000u, 0x80000000u, 3u,
                                              7u, 11u, 0x80000000u, 15u, 0x7fffffffu};
                            // Script 1: OP(3) mask=2(unmanned), group=42, mode=0(==), thresh=1 => Message 1
                            m5p.scripts[1] = {0x80000002u, 0x8000002au, 0x80000000u, 0x80000001u, 3u,
                                              7u, 11u, 0x80000001u, 15u, 0x7fffffffu};
                            // Script 2: OP(3) mask=10(2|8, unmanned + pending), group=42, mode=0(==), thresh=3 => Message 2
                            m5p.scripts[2] = {0x8000000au, 0x8000002au, 0x80000000u, 0x80000003u, 3u,
                                              7u, 11u, 0x80000002u, 15u, 0x7fffffffu};
                            // Script 3: OP(4) mask=8(pending), player=1, mode=0(==), thresh=2 => Message 3
                            m5p.scripts[3] = {0x80000008u, 0x80000001u, 0x80000000u, 0x80000002u, 4u,
                                              7u, 11u, 0x80000003u, 15u, 0x7fffffffu};
                            // Ensure OP(40) condition fails (time > 100s) so wave 0 stays pending while extract picks up owner=1
                            m5p.scripts[4] = {0x80000001u, 0x80000064u, 29u, 7u, 11u,
                                              0x80000001u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000064u,
                                              40u, 0x7fffffffu};

                            const auto [ux, uy] = tc64(20, 20);
                            std::vector<ss2::assets::Infantry> noInf5p;
                            ss2::TutorialUnits u5p(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5p, obst5p),
                                noInf5p,
                                std::vector<ss2::assets::Placement>{{"truck_5p", ux, uy, 0, 100, 100, 42, 3}},
                                ss2::FogGrid(64, 64, flags5p), 2, false, {}, cat5p);
                            ss2::MissionRunner mr5p(std::move(m5p), 50u, std::move(u5p));
                            std::array<bool, 4> sawMsg{};
                            for (const auto& ev : mr5p.poll()) {
                                if (ev.kind == ss2::MissionEvent::Message && ev.index < 4u) {
                                    sawMsg[ev.index] = true;
                                }
                            }
                            if (!sawMsg[0] || !sawMsg[1] || !sawMsg[2] || !sawMsg[3]) {
                                throw std::runtime_error("Phase 5P live MissionRunner OP(3)/OP(4) 4-bit mask query check failed");
                            }
                        }

                        // 2. Vehicle cook-off building blast & suppression
                        {
                            const auto [bx, by] = tc64(25, 25);
                            const auto [wx, wy] = tc64(25, 26); // ~35.8px away (<= 56px cookR)
                            const auto [kx, ky] = tc64(26, 25); // ~35.8px away from (25, 25)
                            const auto [ax, ay] = tc64(30, 25); // heavy anti-tank shooter outside blast radius

                            ss2::assets::UnitSpec at5p = flak88p;
                            at5p.id = "at_5p";
                            at5p.shot1 = {"pierce8-10k", ss2::assets::HitType::Pierce, 400, 400, 400, 400, 0, 10, 1, 10, "gunshot"};
                            auto catCook = ss2::assets::UnitCatalog::fromSpecs({truck5p, tank5p, foot5p, at5p});

                            ss2::TutorialUnits simCook(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5p, obst5p),
                                std::vector<ss2::assets::Infantry>{{"foot_5p", wx, wy, 200, 0, 11, 1}},
                                std::vector<ss2::assets::Placement>{
                                    {"truck_5p", bx, by, 0, 100, 100, 11, 1},
                                    {"tank_5p", kx, ky, 0, 100, 100, 12, 1},
                                    {"at_5p", ax, ay, 0, 100, 100, 1, 0},
                                },
                                ss2::FogGrid(64, 64, flags5p), 2, false, {}, catCook);
                            simCook.setBuildings({{wx, wy, wx - 18, wy - 18, 36, 36, 4, 400, 1}});
                            simCook.revealFogAt(bx, by, 18, 0);
                            (void)simCook.attackWithVehicles(std::array<size_t, 1>{2}, true, 0);

                            bool cookFootHurtAndSup = false;
                            bool cookTankSup = false;
                            bool cookBldDamaged = false;
                            for (int t = 0; t < 40; ++t) {
                                for (const auto& sp : simCook.step()) {
                                    if (sp.index == 0u && sp.hp < 200 && sp.suppressed) cookFootHurtAndSup = true;
                                }
                                for (const auto& vp : simCook.takeVehiclePoses()) {
                                    if (vp.index == 1u && vp.suppressed) cookTankSup = true;
                                }
                                for (const auto& bu : simCook.takeBuildingUpdates()) {
                                    if (bu.index == 0u && bu.hp < 400) cookBldDamaged = true;
                                }
                                if (cookFootHurtAndSup && cookTankSup && cookBldDamaged) break;
                            }
                            if (!cookFootHurtAndSup || !cookTankSup || !cookBldDamaged) {
                                throw std::runtime_error("Phase 5P vehicle cook-off building blast / suppression check failed");
                            }
                        }

                        // 3. Shot-down aircraft EXPLOSIONS\plane crash blast
                        {
                            const auto [aaX, aaY] = tc64(32, 32);    // (2048, 1040)
                            const auto [aa2X, aa2Y] = tc64(31, 31);  // (2048, 1008)
                            const auto [aa3X, aa3Y] = tc64(31, 32);  // (2016, 1024)
                            const auto [crashX, crashY] = tc64(22, 40); // (1472, 1008) beneath 600px intercept
                            ss2::TutorialUnits simCrash(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5p, obst5p),
                                std::vector<ss2::assets::Infantry>{{"foot_5p", crashX, crashY, 200, 0, 11, 1}},
                                std::vector<ss2::assets::Placement>{
                                    {"flak88_5p", aaX, aaY, 0, 100, 100, 1, 0},
                                    {"flak88_5p", aa2X, aa2Y, 0, 100, 100, 1, 0},
                                    {"flak88_5p", aa3X, aa3Y, 0, 100, 100, 1, 0},
                                },
                                ss2::FogGrid(64, 64, flags5p), 2, false, {}, cat5p);
                            simCrash.setBuildings({{crashX, crashY, crashX - 18, crashY - 18, 36, 36, 4, 400, 1}});
                            std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> slots{};
                            slots[1][static_cast<std::size_t>(ss2::assets::AirMissionKind::Bomber)] = {"b17_5p", 1, 1};
                            std::array<int, 4> pGrp{};
                            std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos{};
                            simCrash.setAirSupport(slots, pGrp, pRos);
                            if (!simCrash.callAirSupport(ss2::assets::AirMissionKind::Bomber, aaX, aaY, 1)) {
                                throw std::runtime_error("Phase 5P PlaneShotDown setup failed");
                            }
                            bool sawDown = false;
                            bool crashFootHit = false;
                            bool crashBldHit = false;
                            for (int t = 0; t < 240; ++t) {
                                for (const auto& sp : simCrash.step()) {
                                    if (sp.index == 0u && sp.hp < 200 && sp.suppressed) crashFootHit = true;
                                }
                                for (const auto& bu : simCrash.takeBuildingUpdates()) {
                                    if (bu.index == 0u && bu.hp < 400) crashBldHit = true;
                                }
                                for (const auto& ae : simCrash.takeAirStrikeEvents()) {
                                    if (ae.kind == ss2::AirStrikeEvent::PlaneShotDown) sawDown = true;
                                }
                                if (sawDown && crashFootHit && crashBldHit) break;
                            }
                            if (!sawDown || !crashFootHit || !crashBldHit) {
                                throw std::runtime_error("Phase 5P PlaneShotDown crash blast check failed");
                            }
                        }

                        std::cout << "Phase 5P MissionRunner OP(3)/OP(4) 4-bit masks, cook-off building blast/shock & PlaneShotDown crash blast PASSED\n";
                    }

                    // Phase 5Q: retail mission-authored expa scaling, landing-craft stock=20 capacity,
                    // and infantry/vehicle movespeed & crouchmovespeed veterancy mobility.
                    {
                        std::vector<std::uint32_t> flags5q(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5q(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        ss2::assets::UnitSpec rifle5q{};
                        rifle5q.id = "rifle_5q";
                        rifle5q.health = 200;
                        rifle5q.ammo1 = 40;
                        rifle5q.moveSpeed = 0.50;
                        rifle5q.moveSpeedMax = 0.58;
                        rifle5q.crouchMoveSpeed = 0.33;
                        rifle5q.crouchMoveSpeedMax = 0.38;

                        ss2::assets::UnitSpec commando5q{};
                        commando5q.id = "cmdo_5q";
                        commando5q.health = 250;
                        commando5q.ammo1 = 60;
                        commando5q.moveSpeed = 0.62;
                        commando5q.moveSpeedMax = 0.68;
                        commando5q.crouchMoveSpeed = 0.41;
                        commando5q.crouchMoveSpeedMax = 0.45;

                        ss2::assets::UnitSpec hmg5q{};
                        hmg5q.id = "hmg_5q";
                        hmg5q.health = 200;
                        hmg5q.ammo1 = 100;
                        hmg5q.moveSpeed = 0.40;
                        hmg5q.moveSpeedMax = 0.44;
                        hmg5q.crouchMoveSpeed = 0.26;
                        hmg5q.crouchMoveSpeedMax = 0.28;

                        ss2::assets::UnitSpec landing5q{};
                        landing5q.id = "landing_5q";
                        landing5q.isVehicle = true;
                        landing5q.health = 270;
                        landing5q.stock = 20;
                        landing5q.crewNumber = 2;
                        landing5q.attackCrew = 1;
                        landing5q.moveCrew = 1;
                        landing5q.moveSpeed = 1.6;
                        landing5q.moveSpeedMax = 1.6;

                        ss2::assets::UnitSpec tank5q{};
                        tank5q.id = "tank_5q";
                        tank5q.isVehicle = true;
                        tank5q.health = 500;
                        tank5q.ammo1 = 40;
                        tank5q.crewNumber = 4;
                        tank5q.attackCrew = 2;
                        tank5q.moveCrew = 1;
                        tank5q.moveSpeed = 1.2;
                        tank5q.moveSpeedMax = 1.56;

                        auto cat5q = ss2::assets::UnitCatalog::fromSpecs({rifle5q, commando5q, hmg5q, landing5q, tank5q});

                        // 1. missionExpaToSim & initial / spawnInfantry expa propagation
                        if (ss2::TutorialUnits::missionExpaToSim(0) != 0 ||
                            ss2::TutorialUnits::missionExpaToSim(50) != 16384 ||
                            ss2::TutorialUnits::missionExpaToSim(100) != 32768) {
                            throw std::runtime_error("Phase 5Q missionExpaToSim conversion check failed");
                        }
                        {
                            const auto [ix, iy] = tc64(15, 30);
                            const auto [vx, vy] = tc64(20, 30);
                            const auto [sx, sy] = tc64(18, 30);
                            std::vector<ss2::assets::Infantry> initInf{
                                {"rifle_5q", ix, iy, 200, 100, 1, 0, 0, 0, 0, 0, 0, 0, false, true, 100},
                            };
                            ss2::assets::VehicleUnit initVeh{};
                            initVeh.id = "tank_5q";
                            initVeh.x = vx;
                            initVeh.y = vy;
                            initVeh.hp = 100;
                            initVeh.ammo = 100;
                            initVeh.group = 2;
                            initVeh.owner = 0;
                            initVeh.hasCrew = true;
                            initVeh.expa = 50;
                            ss2::TutorialUnits simExpa(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5q, obst5q),
                                initInf, {}, ss2::FogGrid(64, 64, flags5q), 2, false, {initVeh}, cat5q);
                            ss2::assets::Infantry extraInf{"rifle_5q", sx, sy, 200, 100, 1, 0, 0, 0, 0, 0, 0, 0, false, true, 100};
                            (void)simExpa.spawnInfantry(extraInf, 0);
                            const auto snap = simExpa.takeSnapshot();
                            if (snap.soldiers.size() != 2u || snap.soldiers[0].expa != 32768 ||
                                snap.soldiers[1].expa != 32768 ||
                                snap.vehicles.size() != 1u || snap.vehicles[0].expa != 16384) {
                                throw std::runtime_error("Phase 5Q initial/spawned expa propagation check failed");
                            }
                        }

                        // 2. Landing craft stock=20 embarkation capacity (beyond legacy 16-slot limit)
                        {
                            const auto [bx, by] = tc64(20, 20);
                            std::vector<ss2::assets::Infantry> noInf;
                            ss2::TutorialUnits simBoat(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5q, obst5q),
                                noInf,
                                std::vector<ss2::assets::Placement>{{"landing_5q", bx, by, 0, 100, 100, 1, 0}},
                                ss2::FogGrid(64, 64, flags5q), 2, false, {}, cat5q);
                            for (int p = 0; p < 20; ++p) {
                                const auto [px, py] = tc64(22 + (p % 5), 22 + (p / 5));
                                const size_t sIdx = simBoat.spawnInfantry({"rifle_5q", px, py, 200, 100, 1, 0}, 0);
                                if (!simBoat.canBoardVehicle(0u, 0) || !simBoat.embarkImmediate(sIdx, 0u)) {
                                    throw std::runtime_error("Phase 5Q failed to embark rider #" + std::to_string(p) + " on stock=20 landing craft");
                                }
                            }
                            const auto snapBoat = simBoat.takeSnapshot();
                            if (snapBoat.vehicles.size() != 1u || snapBoat.vehicles[0].passengers.size() != 20u ||
                                simBoat.canBoardVehicle(0u, 0)) {
                                throw std::runtime_error("Phase 5Q landing craft capacity check failed (expected 20 passengers & full)");
                            }
                        }

                        // 3. Infantry movespeed / crouchmovespeed (commando vs hmg, plus veterancy speed gain)
                        auto measureEastDeltaX = [&](const std::vector<ss2::assets::Infantry>& units, bool crouch, int ticks) {
                            const auto [dst0X, dst0Y] = tc64(35, 10);
                            const auto [dst1X, dst1Y] = tc64(38, 13);
                            ss2::TutorialUnits simMob(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5q, obst5q),
                                units, {}, ss2::FogGrid(64, 64, flags5q), 2, false, {}, cat5q);
                            if (crouch) {
                                (void)simMob.toggleCrouch(std::array<size_t, 2>{0u, 1u});
                            }
                            if (!simMob.order(0u, dst0X, dst0Y) || !simMob.order(1u, dst1X, dst1Y)) {
                                throw std::runtime_error("Phase 5Q infantry mobility move order failed");
                            }
                            for (int t = 0; t < ticks; ++t) {
                                (void)simMob.step();
                            }
                            const auto snap = simMob.takeSnapshot();
                            const auto [s0x, s0y] = tc64(15, 30);
                            const auto [s1x, s1y] = tc64(18, 33);
                            const int end0X = 64 * 32 + (snap.soldiers[0].cellX * 16 + 8) - (snap.soldiers[0].cellY * 16 + 8);
                            const int end1X = 64 * 32 + (snap.soldiers[1].cellX * 16 + 8) - (snap.soldiers[1].cellY * 16 + 8);
                            (void)s0y;
                            (void)s1y;
                            return std::pair<int, int>{end0X - s0x, end1X - s1x};
                        };

                        {
                            const auto [s0x, s0y] = tc64(15, 30);
                            const auto [s1x, s1y] = tc64(18, 33);
                            std::vector<ss2::assets::Infantry> pairFastSlow{
                                {"cmdo_5q", s0x, s0y, 250, 100, 1, 0, 0, 0, 0, 0, 0, 0, false, true, 0},
                                {"hmg_5q", s1x, s1y, 200, 100, 1, 0, 0, 0, 0, 0, 0, 0, false, true, 0},
                            };
                            const auto [standCmdo, standHmg] = measureEastDeltaX(pairFastSlow, false, 60);
                            const auto [crouchCmdo, crouchHmg] = measureEastDeltaX(pairFastSlow, true, 80);
                            if (standCmdo <= standHmg || crouchCmdo <= crouchHmg) {
                                throw std::runtime_error("Phase 5Q commando vs hmg movespeed/crouchmovespeed check failed");
                            }

                            std::vector<ss2::assets::Infantry> pairVetRecruit{
                                {"rifle_5q", s0x, s0y, 200, 100, 1, 0, 0, 0, 0, 0, 0, 0, false, true, 100},
                                {"rifle_5q", s1x, s1y, 200, 100, 1, 0, 0, 0, 0, 0, 0, 0, false, true, 0},
                            };
                            const auto [vetDx, recDx] = measureEastDeltaX(pairVetRecruit, false, 120);
                            if (vetDx <= recDx) {
                                throw std::runtime_error("Phase 5Q infantry veterancy movespeed bonus check failed");
                            }
                        }

                        // 4. Vehicle movespeed veterancy speed bonus (vet expa=100 vs recruit expa=0)
                        {
                            const auto [v0x, v0y] = tc64(15, 30);
                            const auto [v1x, v1y] = tc64(18, 33);
                            const auto [d0x, d0y] = tc64(35, 10);
                            const auto [d1x, d1y] = tc64(38, 13);
                            ss2::assets::VehicleUnit vetTank{"tank_5q", v0x, v0y, 100, 100, 1, 0, 0, 0, 0, 0, 0, 0, true, true, true, 100};
                            ss2::assets::VehicleUnit recTank{"tank_5q", v1x, v1y, 100, 100, 1, 0, 0, 0, 0, 0, 0, 0, true, true, true, 0};
                            std::vector<ss2::assets::Infantry> noInf;
                            ss2::TutorialUnits simVehMob(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5q, obst5q),
                                noInf, {}, ss2::FogGrid(64, 64, flags5q), 2, false, {vetTank, recTank}, cat5q);
                            if (simVehMob.moveVehicles(std::array<size_t, 1>{0u}, d0x, d0y) != 1u ||
                                simVehMob.moveVehicles(std::array<size_t, 1>{1u}, d1x, d1y) != 1u) {
                                throw std::runtime_error("Phase 5Q vehicle mobility moveVehicles failed");
                            }
                            for (int t = 0; t < 40; ++t) {
                                (void)simVehMob.step();
                            }
                            const auto snapV = simVehMob.takeSnapshot();
                            const double vetDist = std::hypot(double(snapV.vehicles[0].x - v0x), double(snapV.vehicles[0].y - v0y));
                            const double recDist = std::hypot(double(snapV.vehicles[1].x - v1x), double(snapV.vehicles[1].y - v1y));
                            if (vetDist <= recDist + 4.0) {
                                throw std::runtime_error("Phase 5Q vehicle veterancy movespeed bonus check failed");
                            }
                        }

                        std::cout << "Phase 5Q I-SUPPORT crew-first order, landing-craft stock=20 capacity, mission expa scaling & movespeed/crouchmovespeed PASSED\n";
                    }

                    // Phase 5R: retail MISC\WEATHER visibility, canattackpoint=2 SMG grenade ground-attack,
                    // jeep bonus_shotdeadzone, scandelay auto-scan cadence & expa_crush train crush XP.
                    {
                        std::vector<std::uint32_t> flags5r(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5r(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        ss2::assets::UnitSpec obs5r{};
                        obs5r.id = "obs_5r";
                        obs5r.health = 200;
                        obs5r.ammo1 = 10;
                        obs5r.sight = 12;
                        obs5r.shot1 = {"dummy_5r", ss2::assets::HitType::Machine, 1, 1, 10, 10, 0, 100, 1, 100, "gunshot"};

                        ss2::assets::UnitSpec smg5r{};
                        smg5r.id = "smg_5r";
                        smg5r.health = 200;
                        smg5r.ammo1 = 40;
                        smg5r.sight = 12;
                        smg5r.canAttackPoint = 2;

                        ss2::assets::UnitSpec rifle5r{};
                        rifle5r.id = "rifle_5r";
                        rifle5r.health = 200;
                        rifle5r.ammo1 = 40;
                        rifle5r.sight = 12;
                        rifle5r.shot1 = {"r5r", ss2::assets::HitType::Machine, 20, 20, 240, 240, 0, 5, 1, 5, "gunshot"};

                        ss2::assets::UnitSpec jeep5r{};
                        jeep5r.id = "jeep_5r";
                        jeep5r.isVehicle = true;
                        jeep5r.health = 200;
                        jeep5r.sight = 12;
                        jeep5r.stock = 3;
                        jeep5r.crewNumber = 1;
                        jeep5r.attackCrew = 1;
                        jeep5r.moveCrew = 1;
                        jeep5r.passengersCanFire = true;
                        jeep5r.bonusShotDeadZone = 45;
                        jeep5r.armor[static_cast<std::size_t>(ss2::assets::HitType::Explosive)] = {256, 256, 256};
                        jeep5r.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {256, 256, 256};

                        ss2::assets::UnitSpec scout5r{};
                        scout5r.id = "scout_5r";
                        scout5r.isVehicle = true;
                        scout5r.health = 300;
                        scout5r.ammo1 = 40;
                        scout5r.sight = 14;
                        scout5r.crewNumber = 2;
                        scout5r.attackCrew = 1;
                        scout5r.moveCrew = 1;
                        scout5r.scanDelay = 30;
                        scout5r.gunShotWait = 0;
                        scout5r.shot1 = {"man_machine", ss2::assets::HitType::Machine, 25, 25, 380, 380, 0, 2, 1, 2, "gunshot"};

                        ss2::assets::UnitSpec slowTd5r = scout5r;
                        slowTd5r.id = "slowtd_5r";
                        slowTd5r.scanDelay = 150;

                        ss2::assets::UnitSpec train5r{};
                        train5r.id = "train_5r";
                        train5r.isVehicle = true;
                        train5r.selType = "train";
                        train5r.health = 1000;
                        train5r.crewNumber = 4;
                        train5r.attackCrew = 1;
                        train5r.moveCrew = 1;
                        train5r.moveSpeed = 2.0;
                        train5r.unitRadius = 24;
                        train5r.expaCrush = 100;

                        ss2::assets::UnitSpec stdTank5r = train5r;
                        stdTank5r.id = "stdtank_5r";
                        stdTank5r.selType = "heavytank";
                        stdTank5r.expaCrush = 0;

                        auto cat5r = ss2::assets::UnitCatalog::fromSpecs(
                            {obs5r, smg5r, rifle5r, jeep5r, scout5r, slowTd5r, train5r, stdTank5r});

                        // 1. MISC\WEATHER Rain (60%) drops sight from 12 tiles -> 7 tiles
                        {
                            const auto [px, py] = tc64(20, 20);
                            const auto [ex, ey] = tc64(30, 20); // 10 tiles away
                            ss2::TutorialUnits simW(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5r, obst5r),
                                std::vector<ss2::assets::Infantry>{
                                    {"obs_5r", px, py, 200, 0, 1, 0},
                                    {"obs_5r", ex, ey, 200, 0, 11, 1},
                                },
                                {}, ss2::FogGrid(64, 64, flags5r), 2, false, {}, cat5r);
                            if (!simW.isValidAttackTarget(1u, 0)) {
                                throw std::runtime_error("Phase 5R clear-weather sight check failed");
                            }
                            simW.setWeather(ss2::TutorialUnits::WeatherKind::Rain);
                            if (simW.weatherVisibilityPct() != 60 || simW.isValidAttackTarget(1u, 0)) {
                                throw std::runtime_error("Phase 5R Rain visibility reduction check failed");
                            }
                            simW.setWeather(ss2::TutorialUnits::WeatherKind::Clear);
                            if (!simW.isValidAttackTarget(1u, 0)) {
                                throw std::runtime_error("Phase 5R Clear restoration check failed");
                            }
                        }

                        // 2. canattackpoint=2 SMG grenade attack-ground damages/suppresses enemy & detonates mine
                        {
                            const auto [sx, sy] = tc64(20, 20);
                            const auto [gx, gy] = tc64(22, 20); // ~64px east (< 140px throw range)
                            ss2::TutorialUnits simG(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5r, obst5r),
                                std::vector<ss2::assets::Infantry>{
                                    {"smg_5r", sx, sy, 200, 40, 1, 0},
                                    {"obs_5r", gx, gy, 100, 0, 11, 1},
                                },
                                {}, ss2::FogGrid(64, 64, flags5r), 2, false, {}, cat5r);
                            std::vector<std::uint8_t> m5r(64u * 64u, 0u);
                            m5r[20u * 64u + 22u] = 0x81u | (1u << 2); // detected enemy mine at (22, 20)
                            simG.setMines(std::move(m5r));
                            if (simG.orderAttackGroundSoldiers(std::array<size_t, 1>{0u}, gx, gy) != 1u) {
                                throw std::runtime_error("Phase 5R SMG orderAttackGroundSoldiers rejected");
                            }
                            bool threw = false;
                            for (int t = 0; t < 20; ++t) {
                                for (const auto& sp : simG.step()) {
                                    if (sp.index == 0u && sp.threwGrenade) threw = true;
                                }
                            }
                            const auto snapG = simG.takeSnapshot();
                            const auto minesG = simG.takeMineEvents();
                            const bool mineBoom = std::ranges::any_of(minesG, [](const auto& ev) {
                                return ev.kind == ss2::MineEvent::Detonated;
                            });
                            if (!threw || snapG.soldiers[0].atGrenades != 1 ||
                                snapG.soldiers[1].hp >= 100 || snapG.soldiers[1].suppressionTicks <= 0 || !mineBoom) {
                                throw std::runtime_error("Phase 5R canattackpoint=2 SMG grenade ground-attack check failed");
                            }
                        }

                        // 3. Open-top jeep bonus_shotdeadzone (45px skips point-blank enemy, shoots 96px enemy)
                        {
                            const auto [jx, jy] = tc64(20, 20);
                            const auto [nearX, nearY] = tc64(20, 21); // 32px southwest (< 45px deadzone)
                            const auto [farX, farY] = tc64(23, 20);   // 96px east (> 45px deadzone)
                            ss2::TutorialUnits simDZ(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5r, obst5r),
                                std::vector<ss2::assets::Infantry>{
                                    {"rifle_5r", jx, jy, 200, 40, 1, 0},
                                    {"obs_5r", nearX, nearY, 200, 0, 11, 1},
                                    {"obs_5r", farX, farY, 200, 0, 11, 1},
                                },
                                std::vector<ss2::assets::Placement>{{"jeep_5r", jx, jy, 0, 100, 100, 1, 0}},
                                ss2::FogGrid(64, 64, flags5r), 2, true, {}, cat5r);
                            if (!simDZ.embarkImmediate(0u, 0u)) {
                                throw std::runtime_error("Phase 5R open-top jeep embark failed");
                            }
                            for (int t = 0; t < 40; ++t) {
                                (void)simDZ.step();
                            }
                            const auto snapDZ = simDZ.takeSnapshot();
                            if (!snapDZ.vehicles[0].alive || !snapDZ.soldiers[0].embarked ||
                                snapDZ.soldiers[1].hp != 200 || snapDZ.soldiers[2].hp >= 200) {
                                throw std::runtime_error(
                                    "Phase 5R jeep bonus_shotdeadzone rider target filtering failed (nearHp=" +
                                    std::to_string(snapDZ.soldiers[1].hp) +
                                    " farHp=" + std::to_string(snapDZ.soldiers[2].hp) +
                                    " riderAmmo=" + std::to_string(snapDZ.soldiers[0].ammo) + ")");
                            }
                        }

                        // 4. scandelay cadence (raw Gold ticks: scout=30 vs slowTD=150) & expa_crush train bonus
                        {
                            const auto [s0x, s0y] = tc64(20, 20);
                            const auto [s1x, s1y] = tc64(20, 22);
                            const auto [ex, ey] = tc64(25, 21);
                            std::vector<ss2::assets::Infantry> noInf;
                            ss2::TutorialUnits simScan(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5r, obst5r),
                                noInf,
                                std::vector<ss2::assets::Placement>{
                                    {"scout_5r", s0x, s0y, 0, 100, 100, 1, 0},
                                    {"slowtd_5r", s1x, s1y, 0, 100, 100, 1, 0},
                                },
                                ss2::FogGrid(64, 64, flags5r), 2, true, {}, cat5r);
                            (void)simScan.step(); // tick 1: both scan empty map -> lastAutoScanTick = 1
                            (void)simScan.spawnInfantry({"obs_5r", ex, ey, 500, 0, 11, 1}, 0);
                            bool scoutFiredEarly = false;
                            bool slowFiredEarly = false;
                            for (int t = 2; t <= 35; ++t) {
                                (void)simScan.step();
                                for (const auto& vp : simScan.takeVehiclePoses()) {
                                    if (vp.fired && vp.index == 0u) scoutFiredEarly = true;
                                    if (vp.fired && vp.index == 1u) slowFiredEarly = true;
                                }
                            }
                            bool slowFiredLater = false;
                            for (int t = 36; t <= 165; ++t) {
                                (void)simScan.step();
                                for (const auto& vp : simScan.takeVehiclePoses()) {
                                    if (vp.fired && vp.index == 1u) slowFiredLater = true;
                                }
                            }
                            if (!scoutFiredEarly || slowFiredEarly || !slowFiredLater) {
                                throw std::runtime_error("Phase 5R scandelay vehicle auto-scan cadence check failed");
                            }
                        }
                        {
                            const auto [t0x, t0y] = tc64(16, 20);
                            const auto [e0x, e0y] = tc64(18, 18);
                            const auto [t1x, t1y] = tc64(16, 24);
                            const auto [e1x, e1y] = tc64(18, 22);
                            ss2::TutorialUnits simCrush(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5r, obst5r),
                                std::vector<ss2::assets::Infantry>{
                                    {"obs_5r", e0x, e0y, 50, 0, 11, 1},
                                    {"obs_5r", e1x, e1y, 50, 0, 11, 1},
                                },
                                std::vector<ss2::assets::Placement>{
                                    {"train_5r", t0x, t0y, 0, 100, 100, 1, 0},
                                    {"stdtank_5r", t1x, t1y, 0, 100, 100, 1, 0},
                                },
                                ss2::FogGrid(64, 64, flags5r), 2, false, {}, cat5r);
                            const auto [dst0x, dst0y] = tc64(19, 17);
                            const auto [dst1x, dst1y] = tc64(19, 21);
                            (void)simCrush.moveVehicles(std::array<size_t, 1>{0u}, dst0x, dst0y);
                            (void)simCrush.moveVehicles(std::array<size_t, 1>{1u}, dst1x, dst1y);
                            for (int t = 0; t < 40; ++t) {
                                (void)simCrush.step();
                            }
                            const auto snapC = simCrush.takeSnapshot();
                            if (snapC.soldiers[0].hp != 0 || snapC.soldiers[1].hp != 0 ||
                                snapC.vehicles[0].expa != 2500 || snapC.vehicles[1].expa != 1500) {
                                throw std::runtime_error("Phase 5R expa_crush train crush XP bonus check failed");
                            }
                        }

                        std::cout << "Phase 5R MISC\\WEATHER visibility, canattackpoint=2 SMG grenade ground-attack, jeep bonus_shotdeadzone, scandelay & expa_crush PASSED\n";
                    }

                    // Phase 5S: retail marchenabled hatch toggle & sniper exposure, expa_reload/ammoregendelay/healdelay
                    // veterancy, AT-hunter layminetime/mineammo & timed getminetime, moralerndmove panic & moralerage fervor.
                    {
                        std::vector<std::uint32_t> flags5s(64u * 64u, 0u);
                        std::vector<std::uint16_t> obst5s(64u * 64u, 0x000Fu);
                        auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
                            const int u = tx * 32 + 16;
                            const int v = ty * 32 + 16;
                            return {64 * 32 + u - v, (u + v) / 2};
                        };

                        ss2::assets::UnitSpec tank5s{};
                        tank5s.id = "tank_5s";
                        tank5s.isVehicle = true;
                        tank5s.selType = "tank";
                        tank5s.health = 600;
                        tank5s.sight = 10;
                        tank5s.marchSightBonus = 4;
                        tank5s.marchEnabled = true;
                        tank5s.crewNumber = 4;
                        tank5s.hasCrewProtection = true;
                        tank5s.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {256, 256, 256};
                        tank5s.armor[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {256, 256, 256};

                        ss2::assets::UnitSpec sniper5s{};
                        sniper5s.id = "sniper_5s";
                        sniper5s.selType = "sniper";
                        sniper5s.health = 100;
                        sniper5s.ammo1 = 20;
                        sniper5s.sight = 14;
                        sniper5s.shot1 = {"man_sniper", ss2::assets::HitType::Sniper, 80, 80, 240, 240, 0, 5, 1, 5, "gunshot"};

                        ss2::assets::UnitSpec obs5s{};
                        obs5s.id = "obs_5s";
                        obs5s.health = 200;
                        obs5s.sight = 4;

                        ss2::assets::UnitSpec supply5s{};
                        supply5s.id = "supply_5s";
                        supply5s.isVehicle = true;
                        supply5s.selType = "sappers";
                        supply5s.health = 180;
                        supply5s.ammo1 = 15000;
                        supply5s.ammo2 = 5000;
                        supply5s.sight = 10;
                        supply5s.crewNumber = 2;
                        supply5s.ammoRegenDelay = 8;
                        supply5s.ammoRegenDelayMax = 6;
                        supply5s.expaReload = 3;
                        supply5s.expaReloadMax = 1;

                        ss2::assets::UnitSpec amb5s{};
                        amb5s.id = "zis-5.med_5s";
                        amb5s.isVehicle = true;
                        amb5s.selType = "transport";
                        amb5s.health = 130;
                        amb5s.sight = 10;
                        amb5s.crewNumber = 2;
                        amb5s.healDelay = 10;
                        amb5s.healDelayMax = 8;

                        ss2::assets::UnitSpec gats5s{};
                        gats5s.id = "gats_5s";
                        gats5s.selType = "atinf";
                        gats5s.health = 100;
                        gats5s.ammo1 = 40;
                        gats5s.mineAmmo = 1;
                        gats5s.layMineTime = 100;
                        gats5s.layMineTimeMax = 85;

                        ss2::assets::UnitSpec sap5s{};
                        sap5s.id = "sap_5s";
                        sap5s.selType = "sapper";
                        sap5s.health = 100;
                        sap5s.sight = 11;
                        sap5s.mineAmmo = 5;
                        sap5s.layMineTime = 40;
                        sap5s.getMineTime = 80;
                        sap5s.getMineTimeMax = 60;

                        ss2::assets::UnitSpec car5s{};
                        car5s.id = "car_5s";
                        car5s.isVehicle = true;
                        car5s.selType = "car";
                        car5s.health = 1000;
                        car5s.sight = 12;
                        car5s.crewNumber = 2;
                        car5s.moveSpeed = 2.0;
                        car5s.canMoveBackward = true;
                        car5s.backMoveSpeed = 1.0;
                        car5s.moraleResist = 2048;
                        car5s.moraleRndMove = -17408;
                        car5s.armor[static_cast<std::size_t>(ss2::assets::HitType::Explosive)] = {256, 256, 256};

                        ss2::assets::UnitSpec how5s{};
                        how5s.id = "how_5s";
                        how5s.isVehicle = true;
                        how5s.selType = "howitzer";
                        how5s.health = 500;
                        how5s.ammo1 = 30;
                        how5s.sight = 14;
                        how5s.crewNumber = 3;
                        how5s.gunShotWait = 0;
                        how5s.shot1 = {"h_he", ss2::assets::HitType::Explosive, 300, 300, 500, 500, 0, 3, 1, 3, "gunshot"};

                        ss2::assets::UnitSpec rageTank5s{};
                        rageTank5s.id = "ragetank_5s";
                        rageTank5s.isVehicle = true;
                        rageTank5s.selType = "tank";
                        rageTank5s.health = 1000;
                        rageTank5s.ammo1 = 40;
                        rageTank5s.sight = 14;
                        rageTank5s.crewNumber = 4;
                        rageTank5s.moraleRage = 21845;
                        rageTank5s.gunShotWait = 0;
                        rageTank5s.shot1 = {"r_ap", ss2::assets::HitType::Pierce, 10, 10, 500, 500, 0, 20, 1, 20, "gunshot"};

                        ss2::assets::UnitSpec officer5s{};
                        officer5s.id = "off_5s";
                        officer5s.selType = "officer";
                        officer5s.health = 200;
                        officer5s.sight = 13;
                        officer5s.officerRadius = 160;
                        officer5s.moraleAutoIncrease = 6;

                        ss2::assets::UnitSpec dummyVeh5s{};
                        dummyVeh5s.id = "tgtveh_5s";
                        dummyVeh5s.isVehicle = true;
                        dummyVeh5s.selType = "tank";
                        dummyVeh5s.health = 2500;
                        dummyVeh5s.sight = 6;
                        dummyVeh5s.crewNumber = 4;

                        auto cat5s = ss2::assets::UnitCatalog::fromSpecs({
                            tank5s, sniper5s, obs5s, supply5s, amb5s, gats5s, sap5s,
                            car5s, how5s, rageTank5s, officer5s, dummyVeh5s});

                        // 1. marchenabled hatch toggle controls +4 sight bonus and sniper commander exposure
                        {
                            const auto [tx, ty] = tc64(20, 20);
                            const auto [fx, fy] = tc64(33, 20); // 13 tiles away (14 open vs 10 closed)
                            const auto [sx, sy] = tc64(23, 20); // 96px east (< 240px sniper reach)
                            ss2::TutorialUnits simH(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5s, obst5s),
                                std::vector<ss2::assets::Infantry>{
                                    {"obs_5s", fx, fy, 200, 0, 11, 1},
                                },
                                std::vector<ss2::assets::Placement>{
                                    {"tank_5s", tx, ty, 0, 100, 0, 1, 0, {}, 0},
                                },
                                ss2::FogGrid(64, 64, flags5s), 2, true, {}, cat5s);
                            if (!simH.isValidAttackTarget(0u, 0)) {
                                throw std::runtime_error("Phase 5S open hatch +marchSightBonus initial spotting failed");
                            }
                            const auto closed = simH.toggleMarchMode(std::array<size_t, 1>{0u});
                            if (!closed || *closed) {
                                throw std::runtime_error("Phase 5S toggleMarchMode did not close open hatch");
                            }
                            (void)simH.step();
                            if (simH.isValidAttackTarget(0u, 0)) {
                                throw std::runtime_error("Phase 5S buttoned hatch failed to drop sight radius");
                            }
                            const auto reopened = simH.toggleMarchMode(std::array<size_t, 1>{0u});
                            if (!reopened || !*reopened) {
                                throw std::runtime_error("Phase 5S toggleMarchMode did not reopen hatch");
                            }
                            (void)simH.step();
                            if (!simH.isValidAttackTarget(0u, 0)) {
                                throw std::runtime_error("Phase 5S reopened hatch failed to restore +sight");
                            }
                            const auto closed2 = simH.toggleMarchMode(std::array<size_t, 1>{0u});
                            if (!closed2 || *closed2) {
                                throw std::runtime_error("Phase 5S toggleMarchMode did not re-close hatch");
                            }
                            (void)simH.spawnInfantry({"sniper_5s", sx, sy, 100, 20, 12, 1}, 0);
                            const auto snapH0 = simH.takeSnapshot();
                            for (int t = 0; t < 15; ++t) (void)simH.step();
                            const auto snapClosed = simH.takeSnapshot();
                            if (snapClosed.vehicles[0].crewHp != snapH0.vehicles[0].crewHp) {
                                throw std::runtime_error("Phase 5S buttoned commander should resist sniper fire");
                            }
                            const auto reopened2 = simH.toggleMarchMode(std::array<size_t, 1>{0u});
                            if (!reopened2 || !*reopened2) {
                                throw std::runtime_error("Phase 5S toggleMarchMode did not reopen hatch for exposure");
                            }
                            for (int t = 0; t < 10; ++t) (void)simH.step();
                            const auto snapOpen = simH.takeSnapshot();
                            if (snapOpen.vehicles[0].crewHp >= snapH0.vehicles[0].crewHp) {
                                throw std::runtime_error("Phase 5S unbuttoned commander was not exposed to sniper fire");
                            }
                        }

                        // 2. expa_reload, ammoregendelay & healdelay veterancy scaling
                        {
                            const auto [ax, ay] = tc64(20, 20);
                            const auto [bx, by] = tc64(22, 20);
                            auto runSupplyVetTest = [&](int supExpa) {
                                std::vector<ss2::assets::Infantry> noInf;
                                ss2::TutorialUnits simS(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5s, obst5s),
                                    noInf,
                                    std::vector<ss2::assets::Placement>{
                                        {"supply_5s", ax, ay, 0, 100, 100, 1, 0, {}, supExpa},
                                        {"tank_5s", bx, by, 0, 50, 0, 2, 0, {}, 0},
                                    },
                                    std::nullopt, 2, false, {}, cat5s);
                                for (int t = 0; t < 25; ++t) (void)simS.step();
                                return simS.takeSnapshot();
                            };
                            const auto rookieSup = runSupplyVetTest(0);
                            const auto vetSup = runSupplyVetTest(100);
                            if (rookieSup.vehicles[0].expa <= 0 || rookieSup.vehicles[1].hp != 318 ||
                                vetSup.vehicles[1].hp <= rookieSup.vehicles[1].hp) {
                                throw std::runtime_error("Phase 5S expa_reload / ammoregendelay veterancy check failed");
                            }

                            auto runAmbVetTest = [&](int ambExpa) {
                                ss2::TutorialUnits simA(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5s, obst5s),
                                    std::vector<ss2::assets::Infantry>{
                                        {"obs_5s", bx, by, 100, 0, 2, 0},
                                    },
                                    std::vector<ss2::assets::Placement>{
                                        {"zis-5.med_5s", ax, ay, 0, 100, 0, 1, 0, {}, ambExpa},
                                    },
                                    std::nullopt, 2, false, {}, cat5s);
                                for (int t = 0; t < 25; ++t) (void)simA.step();
                                return simA.takeSnapshot();
                            };
                            const auto rookieAmb = runAmbVetTest(0);
                            const auto vetAmb = runAmbVetTest(100);
                            if (rookieAmb.vehicles[0].expa <= 0 || rookieAmb.soldiers[0].hp != 112 ||
                                vetAmb.soldiers[0].hp <= rookieAmb.soldiers[0].hp) {
                                throw std::runtime_error("Phase 5S healdelay veterancy check failed");
                            }
                        }

                        // 3. AT-hunter mineammo/layminetime & sapper timed getminetime disarm
                        {
                            const auto [gx, gy] = tc64(20, 20);
                            const auto [sx, sy] = tc64(25, 20);
                            ss2::TutorialUnits simM(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5s, obst5s),
                                std::vector<ss2::assets::Infantry>{
                                    {"gats_5s", gx, gy, 100, 40, 1, 0},
                                    {"sap_5s", sx, sy, 100, 0, 2, 0},
                                },
                                {}, ss2::FogGrid(64, 64, flags5s), 2, false, {}, cat5s);
                            std::vector<std::uint8_t> m5s(64u * 64u, 0u);
                            m5s[20u * 64u + 26u] = 0xC5u; // hostile (0x40) detected (0x80) AT (0x04) mine 1 tile east of sap_5s
                            simM.setMines(std::move(m5s));
                            if (simM.orderLayMine(std::array<size_t, 1>{0u}) != 1u ||
                                simM.orderLayMine(std::array<size_t, 1>{0u}) != 0u) {
                                throw std::runtime_error("Phase 5S gats mineammo=1 lay check failed");
                            }
                            for (int t = 0; t < 5; ++t) (void)simM.step();
                            if ((simM.takeSnapshot().mines[20u * 64u + 26u] & 0x01u) == 0u) {
                                throw std::runtime_error("Phase 5S sapper disarmed getminetime=80 mine too early");
                            }
                            for (int t = 0; t < 6; ++t) (void)simM.step();
                            const auto snapM = simM.takeSnapshot();
                            const auto mev = simM.takeMineEvents();
                            const bool sawDisarmed = std::ranges::any_of(mev, [](const auto& ev) {
                                return ev.kind == ss2::MineEvent::Disarmed;
                            });
                            if (snapM.soldiers[0].mineAmmo != 0 ||
                                (snapM.mines[20u * 64u + 26u] & 0x01u) != 0u || !sawDisarmed) {
                                throw std::runtime_error("Phase 5S sapper timed getminetime disarm check failed");
                            }
                        }

                        // 4. moralerndmove panic reverse & moralerage officer fervor
                        {
                            const auto [cx, cy] = tc64(20, 20);
                            const auto [hx, hy] = tc64(26, 20);
                            std::vector<ss2::assets::Infantry> noInf;
                            ss2::TutorialUnits simP(
                                ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5s, obst5s),
                                noInf,
                                std::vector<ss2::assets::Placement>{
                                    {"car_5s", cx, cy, 0, 100, 0, 1, 0},
                                    {"how_5s", hx, hy, 16, 100, 100, 11, 1},
                                },
                                ss2::FogGrid(64, 64, flags5s), 2, true, {}, cat5s);
                            const auto snapP0 = simP.takeSnapshot();
                            for (int t = 0; t < 40; ++t) (void)simP.step();
                            const auto snapP1 = simP.takeSnapshot();
                            const double moved = std::hypot(double(snapP1.vehicles[0].x - snapP0.vehicles[0].x),
                                                            double(snapP1.vehicles[0].y - snapP0.vehicles[0].y));
                            if (moved <= 10.0 || snapP1.vehicles[0].x >= snapP0.vehicles[0].x) {
                                throw std::runtime_error("Phase 5S moralerndmove panic reverse check failed");
                            }

                            const auto [ox, oy] = tc64(21, 20);
                            const auto [tx, ty] = tc64(25, 20);
                            auto runRageTest = [&](bool withOfficer) {
                                std::vector<ss2::assets::Infantry> inf;
                                if (withOfficer) inf.push_back({"off_5s", ox, oy, 200, 0, 1, 0});
                                ss2::TutorialUnits simR(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5s, obst5s),
                                    inf,
                                    std::vector<ss2::assets::Placement>{
                                        {"ragetank_5s", cx, cy, 0, 100, 100, 1, 0},
                                        {"tgtveh_5s", tx, ty, 16, 100, 0, 11, 1},
                                    },
                                    ss2::FogGrid(64, 64, flags5s), 2, true, {}, cat5s);
                                for (int t = 0; t < 110; ++t) (void)simR.step();
                                return simR.takeSnapshot().vehicles[0].ammo;
                            };
                            const int ammoNoOff = runRageTest(false);
                            const int ammoWithOff = runRageTest(true);
                            if (ammoWithOff >= ammoNoOff) {
                                throw std::runtime_error("Phase 5S moralerage officer fervor reload check failed");
                            }
                        }

                        std::cout << "Phase 5S marchenabled hatch/sniper exposure, expa_reload/healdelay veterancy, mine logistics & moralerndmove/moralerage PASSED\n";

                        // Phase 5T: Gold I-MISXINF weather selector enum/propagation, truncation + floor 1, dynamic bonus_sight
                        {
                            static_assert(static_cast<int>(ss2::assets::WeatherKind::Clear) == 0);
                            static_assert(static_cast<int>(ss2::assets::WeatherKind::Fog) == 1);
                            static_assert(static_cast<int>(ss2::assets::WeatherKind::Rain) == 2);
                            static_assert(static_cast<int>(ss2::assets::WeatherKind::Snow) == 3);

                            std::vector<std::uint32_t> flags5t(64u * 64u, 0u);
                            std::vector<std::uint16_t> obst5t(64u * 64u, 0x000Fu);
                            auto tc5t = [](int tx, int ty) -> std::pair<int, int> {
                                const int u = tx * 32 + 16;
                                const int v = ty * 32 + 16;
                                return {64 * 32 + u - v, (u + v) / 2};
                            };

                            ss2::assets::UnitSpec obs6{};
                            obs6.id = "obs6_5t";
                            obs6.health = 200;
                            obs6.ammo1 = 10;
                            obs6.sight = 6;

                            ss2::assets::UnitSpec obs11 = obs6;
                            obs11.id = "obs11_5t";
                            obs11.sight = 11;

                            ss2::assets::UnitSpec obs12 = obs6;
                            obs12.id = "obs12_5t";
                            obs12.sight = 12;

                            ss2::assets::UnitSpec tank5t{};
                            tank5t.id = "tank_5t";
                            tank5t.isVehicle = true;
                            tank5t.health = 500;
                            tank5t.sight = 7;
                            tank5t.bonusSight = 3;
                            tank5t.crewNumber = 1;
                            tank5t.attackCrew = 1;
                            tank5t.moveCrew = 1;

                            ss2::assets::UnitSpec arty5t = tank5t;
                            arty5t.id = "arty_5t";
                            arty5t.sight = 9;
                            arty5t.bonusSight = -4;

                            auto cat5t = ss2::assets::UnitCatalog::fromSpecs({obs6, obs11, obs12, tank5t, arty5t});

                            const auto [px, py] = tc5t(20, 20);
                            const auto [e3x, e3y] = tc5t(23, 20);
                            const auto [e4x, e4y] = tc5t(24, 20);
                            const auto [e5x, e5y] = tc5t(25, 20);
                            const auto [e6x, e6y] = tc5t(26, 20);
                            const auto [e7x, e7y] = tc5t(27, 20);
                            const auto [e8x, e8y] = tc5t(28, 20);
                            const auto [e9x, e9y] = tc5t(29, 20);
                            const auto [e10x, e10y] = tc5t(30, 20);
                            const auto [e11x, e11y] = tc5t(31, 20);

                            // 1. Gold (r * visPct) / 100 truncation & floor 1:
                            // sight=6 in Rain (60%) -> 3 tiles (not old floor 4)
                            {
                                ss2::TutorialUnits simTrunc1(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5t, obst5t),
                                    std::vector<ss2::assets::Infantry>{
                                        {"obs6_5t", px, py, 200, 0, 1, 0},
                                        {"obs6_5t", e3x, e3y, 200, 0, 11, 1},
                                        {"obs6_5t", e4x, e4y, 200, 0, 12, 1},
                                    },
                                    {}, ss2::FogGrid(64, 64, flags5t), 2, false, {}, cat5t);
                                simTrunc1.setWeather(ss2::assets::WeatherKind::Rain);
                                if (!simTrunc1.isValidAttackTarget(1u, 0) || simTrunc1.isValidAttackTarget(2u, 0)) {
                                    throw std::runtime_error("Phase 5T Rain sight=6 truncation/floor check failed");
                                }
                            }
                            // sight=11 in Fog (70%) -> 7 tiles (truncates, not +50 rounding to 8)
                            {
                                ss2::TutorialUnits simTrunc2(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5t, obst5t),
                                    std::vector<ss2::assets::Infantry>{
                                        {"obs11_5t", px, py, 200, 0, 1, 0},
                                        {"obs11_5t", e7x, e7y, 200, 0, 11, 1},
                                        {"obs11_5t", e8x, e8y, 200, 0, 12, 1},
                                    },
                                    {}, ss2::FogGrid(64, 64, flags5t), 2, false, {}, cat5t);
                                simTrunc2.setWeather(ss2::assets::WeatherKind::Fog);
                                if (!simTrunc2.isValidAttackTarget(1u, 0) || simTrunc2.isValidAttackTarget(2u, 0)) {
                                    throw std::runtime_error("Phase 5T Fog sight=11 integer truncation check failed");
                                }
                            }

                            // 2. Dynamic occupied bonus_sight (+3 tank, -4 arty) under Clear and Rain:
                            {
                                ss2::TutorialUnits simTank(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5t, obst5t),
                                    std::vector<ss2::assets::Infantry>{
                                        {"obs6_5t", e5x, e5y, 200, 0, 11, 1},   // idx 0: dist 5
                                        {"obs6_5t", e6x, e6y, 200, 0, 12, 1},   // idx 1: dist 6
                                        {"obs6_5t", e10x, e10y, 200, 0, 13, 1}, // idx 2: dist 10
                                        {"obs6_5t", e11x, e11y, 200, 0, 14, 1}, // idx 3: dist 11
                                    },
                                    std::vector<ss2::assets::Placement>{{"tank_5t", px, py, 0, 100, 0, 1, 0}},
                                    ss2::FogGrid(64, 64, flags5t), 2, true, {}, cat5t);
                                if (!simTank.isValidAttackTarget(2u, 0) || simTank.isValidAttackTarget(3u, 0)) {
                                    throw std::runtime_error("Phase 5T Clear tank bonus_sight (+3) check failed");
                                }
                                simTank.setWeather(ss2::assets::WeatherKind::Rain);
                                if (!simTank.isValidAttackTarget(0u, 0) || simTank.isValidAttackTarget(1u, 0)) {
                                    throw std::runtime_error("Phase 5T Rain tank bonus_sight (4+1=5) check failed");
                                }
                            }
                            {
                                ss2::TutorialUnits simArty(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5t, obst5t),
                                    std::vector<ss2::assets::Infantry>{
                                        {"obs6_5t", e3x, e3y, 200, 0, 11, 1}, // idx 0: dist 3
                                        {"obs6_5t", e4x, e4y, 200, 0, 12, 1}, // idx 1: dist 4
                                        {"obs6_5t", e5x, e5y, 200, 0, 13, 1}, // idx 2: dist 5
                                        {"obs6_5t", e6x, e6y, 200, 0, 14, 1}, // idx 3: dist 6
                                    },
                                    std::vector<ss2::assets::Placement>{{"arty_5t", px, py, 0, 100, 0, 1, 0}},
                                    ss2::FogGrid(64, 64, flags5t), 2, true, {}, cat5t);
                                if (!simArty.isValidAttackTarget(2u, 0) || simArty.isValidAttackTarget(3u, 0)) {
                                    throw std::runtime_error("Phase 5T Clear arty bonus_sight (-4) check failed");
                                }
                                simArty.setWeather(ss2::assets::WeatherKind::Rain);
                                if (!simArty.isValidAttackTarget(0u, 0) || simArty.isValidAttackTarget(1u, 0)) {
                                    throw std::runtime_error("Phase 5T Rain arty bonus_sight (5-2=3) check failed");
                                }
                            }

                            // 3. MissionRunner applies MissionData::weather before initial fog snapshot
                            {
                                ss2::assets::TutorialData mdata5t;
                                mdata5t.width = 64;
                                mdata5t.height = 64;
                                mdata5t.flags = flags5t;
                                mdata5t.obstacles = obst5t;
                                mdata5t.scripts.resize(1);
                                mdata5t.scripts[0] = {0x7fffffffu};
                                mdata5t.weather = ss2::assets::WeatherKind::Rain;
                                ss2::TutorialUnits simMR(
                                    ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5t, obst5t),
                                    std::vector<ss2::assets::Infantry>{
                                        {"obs12_5t", px, py, 200, 0, 1, 0},
                                        {"obs12_5t", e7x, e7y, 200, 0, 11, 1},
                                        {"obs12_5t", e9x, e9y, 200, 0, 12, 1},
                                    },
                                    {}, ss2::FogGrid(64, 64, flags5t), 2, false, {}, cat5t);
                                ss2::MissionRunner runner5t(mdata5t, 50u, std::move(simMR));
                                if (!runner5t.isValidAttackTarget(1u) || runner5t.isValidAttackTarget(2u)) {
                                    throw std::runtime_error("Phase 5T MissionRunner initial I-MISXINF weather propagation check failed");
                                }
                            }

                            std::cout << "Phase 5T Gold I-MISXINF weather selector, weather sight/RGBC, dynamic bonus_sight & raw-tick scandelay PASSED\n";

                            // Phase 5U: Gold CAOF opcodes (22-26, 41-42, 58-60, 77-78, 82, 92-93) & weather marchsightbonus
                            {
                                constexpr std::uint32_t C = 0x80000000u;
                                constexpr std::uint32_t END = 0x7fffffffu;

                                std::vector<std::uint32_t> flags5u(64u * 64u, 0u);
                                std::vector<std::uint16_t> obst5u(64u * 64u, 0x000Fu);
                                auto tc5u = [](int tx, int ty) -> std::pair<int, int> {
                                    const int u = tx * 32 + 16;
                                    const int v = ty * 32 + 16;
                                    return {64 * 32 + u - v, (u + v) / 2};
                                };

                                ss2::assets::UnitSpec obs5u{};
                                obs5u.id = "obs_5u";
                                obs5u.health = 200;
                                obs5u.ammo1 = 10;
                                obs5u.sight = 10;

                                ss2::assets::UnitSpec inf5u = obs5u;
                                inf5u.id = "inf_5u";

                                ss2::assets::UnitSpec killer5u = obs5u;
                                killer5u.id = "killer_5u";
                                killer5u.isVehicle = true;
                                killer5u.selType = "tank";
                                killer5u.health = 500;
                                killer5u.ammo1 = 20;
                                killer5u.crewNumber = 1;
                                killer5u.shot1 = {"he5u", ss2::assets::HitType::Explosive, 5000, 5000, 400, 400, 2, 2, 1, 2, "gunshot"};

                                ss2::assets::UnitSpec how5u{};
                                how5u.id = "how_5u";
                                how5u.isVehicle = true;
                                how5u.selType = "arty";
                                how5u.health = 300;
                                how5u.ammo1 = 12;
                                how5u.sight = 10;
                                how5u.crewNumber = 1;
                                how5u.turnDelay = 2;
                                how5u.gunTurnDelay = 2;
                                how5u.hasGunTurnDelay = true;
                                how5u.shot1 = {"arty5u", ss2::assets::HitType::Explosive, 120, 120, 450, 450, 4, 4, 1, 4, "gunshot"};

                                ss2::assets::UnitSpec v2Spec = how5u;
                                v2Spec.id = "v2_5u";
                                v2Spec.selType = "rl";

                                ss2::assets::UnitSpec marchTank{};
                                marchTank.id = "march_5u";
                                marchTank.isVehicle = true;
                                marchTank.selType = "tank";
                                marchTank.health = 600;
                                marchTank.sight = 7;
                                marchTank.marchEnabled = true;
                                marchTank.marchSightBonus = 4;
                                marchTank.crewNumber = 1;

                                auto cat5u = ss2::assets::UnitCatalog::fromSpecs(
                                    {obs5u, inf5u, killer5u, how5u, v2Spec, marchTank});

                                // 1. OP(26) NOT + OP(41) marker object destroyed + OP(24)/OP(25)/OP(42) air strike
                                // + OP(58) owner transfer + OP(82) zone group reassign + OP(77)/OP(92) bombard + OP(22)/OP(23)
                                {
                                    const auto [bx, by] = tc5u(25, 25);
                                    const auto [kx, ky] = tc5u(22, 25);
                                    const auto [zInX, zInY] = tc5u(20, 20);
                                    const auto [zOutX, zOutY] = tc5u(30, 20);
                                    const auto [tX, tY] = tc5u(22, 22);
                                    const auto [bombX, bombY] = tc5u(25, 22);
                                    const auto [sX, sY] = tc5u(35, 20);

                                    ss2::assets::TutorialData d5u;
                                    d5u.width = 64;
                                    d5u.height = 64;
                                    d5u.flags = flags5u;
                                    d5u.obstacles = obst5u;
                                    d5u.zones.assign(64u * 64u, 0u);
                                    d5u.zones[20u * 64u + 20u] = 39u;
                                    d5u.zones[20u * 64u + 30u] = 5u;
                                    d5u.markers[0] = {bx, by};
                                    d5u.markers[1] = tc5u(40, 40);
                                    d5u.locations[2] = {bombX, bombY};
                                    d5u.phrases = {"b0_intact_via_not", "b0_destroyed"};
                                    d5u.playerTeams = {0, 1, 1, 2};
                                    d5u.reinforcements[0] = {
                                        {"inf_5u", 7, 100, 40, 0, {{"inf_5u", 7, 100, 40, 0, false}}},
                                    };

                                    d5u.scripts = {
                                        // s0 (tick 0): OP(41)(marker0)==0 -> OP(26) NOT == 1 -> msg 0; grant OP(24)+OP(25) 2 bombers to owner 1;
                                        // OP(58) transfer grp 60 -> owner 0; OP(82) reassign grp 63 -> grp 68 in zone 39; OP(22)/OP(23) AI flags;
                                        // OP(77) & OP(92) bombard loc 2.
                                        {
                                            0u | C, 41u, 26u, 7u,
                                            0u | C, 15u,
                                            1u | C, 2u | C, 0u | C, 24u,
                                            1u | C, 2u | C, 0u | C, 25u,
                                            60u | C, 0u | C, 58u,
                                            68u | C, 63u | C, 39u | C, 82u,
                                            10u | C, 1u | C, 22u,
                                            10u | C, 2u | C, 23u,
                                            11u | C, 2u | C, 77u,
                                            4u | C, 2u | C, 92u,
                                            11u, END
                                        },
                                        // s1 (when marker 0 building destroyed): OP(41)(0) == 1 -> msg 1
                                        {0u | C, 41u, 7u, 1u | C, 15u, 11u, END},
                                        // s2 (tick > 0, i.e. tick 3 scan): launch OP(42) 2-bomber strike for owner 1 at marker 1
                                        {1u | C, 0u | C, 29u, 7u, 2u | C, 0u | C, 1u | C, 1u | C, 0u | C, 42u, 11u, END},
                                        // s3..s5: OP(93) team-scoped group 7 tests (mask=1 team1==2, mask=8 team1==2, mask=9 team0==1)
                                        {1u | C, 1u | C, 7u | C, 0u | C, 2u | C, 93u, 7u, 10u | C, 15u, 11u, END},
                                        {8u | C, 1u | C, 7u | C, 0u | C, 2u | C, 93u, 7u, 11u | C, 15u, 11u, END},
                                        {9u | C, 0u | C, 7u | C, 0u | C, 1u | C, 93u, 7u, 12u | C, 15u, 11u, END},
                                        // s6: dormant OP(40) anchor so extractReinforcementWaveOwners binds wave 0 -> owner 1
                                        {99u | C, 27u, 7u, 1u | C, 0u | C, 0u | C, 0u | C, 0u | C, 40u, 11u, END},
                                    };
                                    d5u.phrases.resize(13);

                                    std::vector<ss2::assets::Infantry> initInf{
                                        {"inf_5u", sX, sY, 100, 40, 60, 1},   // idx 0: transfers grp 60 -> owner 0
                                        {"inf_5u", zInX, zInY, 100, 40, 63, 1}, // idx 1: inside zone 39 -> grp 68
                                        {"inf_5u", zOutX, zOutY, 100, 40, 63, 1}, // idx 2: stays grp 63
                                        {"inf_5u", sX, sY, 100, 40, 7, 0},    // idx 3: team 0 grp 7
                                        {"inf_5u", sX, sY, 100, 40, 7, 1},    // idx 4: team 1 grp 7
                                    };
                                    std::vector<ss2::assets::VehicleUnit> vehUnits;
                                    auto makeVeh = [&](std::string id, int x, int y, int group, int owner) {
                                        ss2::assets::VehicleUnit vu{};
                                        vu.id = std::move(id);
                                        vu.x = x; vu.y = y;
                                        vu.group = group; vu.owner = owner;
                                        vu.hp = 100; vu.ammo = 100;
                                        vu.alive = true; vu.visible = true; vu.hasCrew = true;
                                        return vu;
                                    };
                                    vehUnits.push_back(makeVeh("killer_5u", kx, ky, 5, 0));    // veh 0: destroys bld 0
                                    vehUnits.push_back(makeVeh("how_5u", tX, tY, 11, 1));      // veh 1: OP(77) bombard
                                    vehUnits.push_back(makeVeh("v2_5u", tX, tY, 4, 1));        // veh 2: OP(92) bombard
                                    vehUnits.push_back(makeVeh("how_5u", sX, sY, 60, 1));      // veh 3: OP(58) owner->0
                                    vehUnits.push_back(makeVeh("how_5u", sX, sY, 7, 2));       // veh 4: team 1 (owner 2) grp 7

                                    ss2::TutorialUnits sim5u(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5u, obst5u),
                                        initInf, {}, ss2::FogGrid(64, 64, flags5u), 2, false, std::move(vehUnits), cat5u);
                                    sim5u.setPlayerTeams(d5u.playerTeams);
                                    ss2::assets::BuildingPlacement bp{};
                                    bp.x = bx; bp.y = by; bp.left = bx - 32; bp.top = by - 16; bp.width = 64; bp.height = 32;
                                    bp.maxHp = 200; bp.capacity = 4;
                                    sim5u.setBuildings({bp});
                                    {
                                        auto snapB = sim5u.takeSnapshot();
                                        snapB.buildings[0].owner = 1;
                                        if (!sim5u.loadSnapshot(snapB)) {
                                            throw std::runtime_error("Phase 5U failed to mark building 0 hostile");
                                        }
                                    }

                                    ss2::MissionRunner runner5u(d5u, 8u, std::move(sim5u));

                                    // Check immediate tick-0 results collected in MissionRunner ctor
                                    const auto slotsAfter0 = runner5u.airSupportFor(1);
                                    if (slotsAfter0[0].ready != 2 || slotsAfter0[0].total != 2) {
                                        throw std::runtime_error("Phase 5U OP(24)/OP(25) enemy air grant failed");
                                    }
                                    if (runner5u.countByPlayerMask(0, 1u) != 4u || runner5u.countByPlayerMask(1, 1u) != 5u) {
                                        throw std::runtime_error("Phase 5U OP(58) initial owner transfer count mismatch");
                                    }
                                    if (runner5u.countByGroupMask(68u, 1u) != 1u || runner5u.countByGroupMask(63u, 1u) != 1u) {
                                        throw std::runtime_error("Phase 5U OP(82) zone group reassignment failed");
                                    }
                                    if (runner5u.countByGroupTeamMask(7u, 1, 1u) != 2u ||
                                        runner5u.countByGroupTeamMask(7u, 1, 8u) != 2u ||
                                        runner5u.countByGroupTeamMask(7u, 0, 1u) != 1u) {
                                        throw std::runtime_error("Phase 5U team-scoped group bitmask query failed");
                                    }
                                    if (runner5u.attackBuildingWithVehicles(std::array<size_t, 1>{0u}, 0u) != 1u) {
                                        throw std::runtime_error("Phase 5U failed to order killer_5u onto building 0");
                                    }

                                    bool sawMsg0 = false, sawMsg1 = false, sawMsg10 = false, sawMsg11 = false, sawMsg12 = false;
                                    bool veh1Fired = false, veh2Fired = false;
                                    std::vector<std::uint32_t> airSortieIds;
                                    const auto tStart5u = std::chrono::steady_clock::now();
                                    while (std::chrono::steady_clock::now() - tStart5u < std::chrono::seconds(3)) {
                                        for (const auto& ev : runner5u.poll()) {
                                            if (ev.kind == Event::Message) {
                                                if (ev.index == 0u) sawMsg0 = true;
                                                if (ev.index == 1u) sawMsg1 = true;
                                                if (ev.index == 10u) sawMsg10 = true;
                                                if (ev.index == 11u) sawMsg11 = true;
                                                if (ev.index == 12u) sawMsg12 = true;
                                            }
                                        }
                                        for (const auto& vp : runner5u.pollVehiclePoses()) {
                                            if (vp.index == 1u && vp.fired) veh1Fired = true;
                                            if (vp.index == 2u && vp.fired) veh2Fired = true;
                                        }
                                        for (const auto& sp : runner5u.pollAirSorties()) {
                                            if (sp.owner == 1 && sp.kind == ss2::assets::AirMissionKind::Bomber &&
                                                std::ranges::find(airSortieIds, sp.id) == airSortieIds.end()) {
                                                airSortieIds.push_back(sp.id);
                                            }
                                        }
                                        if (sawMsg0 && sawMsg1 && sawMsg10 && sawMsg11 && sawMsg12 &&
                                            veh1Fired && veh2Fired && airSortieIds.size() >= 2u) {
                                            break;
                                        }
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                    }
                                    const auto slotsAfterStrike = runner5u.airSupportFor(1);
                                    if (!sawMsg0 || !sawMsg1) {
                                        throw std::runtime_error("Phase 5U OP(26) NOT / OP(41) marker object destroyed failed");
                                    }
                                    if (!sawMsg10 || !sawMsg11 || !sawMsg12) {
                                        throw std::runtime_error("Phase 5U OP(93) team-scoped group count VM check failed");
                                    }
                                    if (airSortieIds.size() != 2u || slotsAfterStrike[0].ready != 0 || slotsAfterStrike[0].total != 0) {
                                        throw std::runtime_error("Phase 5U OP(42) scripted air strike dispatch failed");
                                    }
                                    if (!veh1Fired || !veh2Fired) {
                                        throw std::runtime_error("Phase 5U OP(77)/OP(92) scripted bombardment failed");
                                    }
                                    // Confirm grp 60 ownership changed from 1 -> 0
                                    if (runner5u.countByPlayerMask(0, 1u) != 4u || runner5u.countByPlayerMask(1, 1u) != 5u) {
                                        throw std::runtime_error("Phase 5U OP(58) group owner transfer count mismatch");
                                    }
                                }

                                // 2. Gold 0x1001d3e0 marchsightbonus weather scaling: (sight*visPct)/100 + (march*visPct)/100
                                {
                                    const auto [tx, ty] = tc5u(20, 20);
                                    const auto [d6x, d6y] = tc5u(26, 20);
                                    const auto [d7x, d7y] = tc5u(27, 20);
                                    const auto [d11x, d11y] = tc5u(31, 20);
                                    const auto [d12x, d12y] = tc5u(32, 20);
                                    ss2::TutorialUnits simMarchW(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5u, obst5u),
                                        std::vector<ss2::assets::Infantry>{
                                            {"obs_5u", d6x, d6y, 200, 0, 11, 1},   // idx 0: dist 6
                                            {"obs_5u", d7x, d7y, 200, 0, 12, 1},   // idx 1: dist 7
                                            {"obs_5u", d11x, d11y, 200, 0, 13, 1}, // idx 2: dist 11
                                            {"obs_5u", d12x, d12y, 200, 0, 14, 1}, // idx 3: dist 12
                                        },
                                        std::vector<ss2::assets::Placement>{{"march_5u", tx, ty, 0, 100, 0, 1, 0}},
                                        ss2::FogGrid(64, 64, flags5u), 2, false, {}, cat5u);
                                    if (!simMarchW.isValidAttackTarget(2u, 0) || simMarchW.isValidAttackTarget(3u, 0)) {
                                        throw std::runtime_error("Phase 5U Clear marchsightbonus open sight (7+4=11) check failed");
                                    }
                                    const auto closedW = simMarchW.toggleMarchMode(std::array<size_t, 1>{0u});
                                    (void)simMarchW.step();
                                    if (!closedW || *closedW || !simMarchW.isValidAttackTarget(1u, 0) ||
                                        simMarchW.isValidAttackTarget(2u, 0)) {
                                        throw std::runtime_error("Phase 5U Clear buttoned marchsightbonus sight (7) check failed");
                                    }
                                    const auto reopenedW = simMarchW.toggleMarchMode(std::array<size_t, 1>{0u});
                                    (void)simMarchW.step();
                                    if (!reopenedW || !*reopenedW || !simMarchW.isValidAttackTarget(2u, 0) ||
                                        simMarchW.isValidAttackTarget(3u, 0)) {
                                        throw std::runtime_error("Phase 5U Clear reopened marchsightbonus sight (7+4=11) check failed");
                                    }
                                    simMarchW.setWeather(ss2::assets::WeatherKind::Fog); // 70% -> (7*70)/100 + (4*70)/100 = 4 + 2 = 6
                                    if (!simMarchW.isValidAttackTarget(0u, 0) || simMarchW.isValidAttackTarget(1u, 0)) {
                                        throw std::runtime_error("Phase 5U Fog separate marchsightbonus scaling (4+2=6) check failed");
                                    }
                                }

                                std::cout << "Phase 5U Gold CAOF opcodes (22-26/41-42/58-60/77-78/82/92-93) & weather marchsightbonus PASSED\n";
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
