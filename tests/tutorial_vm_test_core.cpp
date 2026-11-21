#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runRetailChecks(const std::filesystem::path& gameDir) {
    const auto root = gameDir;
    const auto file = root / "maps/single/--- TUTORIAL I ---.SSM";
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

    const auto basfFile = root / "maps/single/'BASF [Total Victory].ssm";
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
}

int runCoreAnd4Ato4E() {
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
            const auto cat4e = makePhase4ECatalog();
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
        }
    return 0;
}
}  // namespace ss2::vm_test
