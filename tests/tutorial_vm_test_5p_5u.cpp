#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runPhase5Pto5U() {
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

                            // Phase 5V: Gold 0x1f sight cap, Easy 1.3x vehicle/march sight,
                            // tanksonarmorprotection unbuttoned sniper 100% crew damage & suppression auto-button-up.
                            {
                                std::vector<std::uint32_t> flags5v(64u * 64u, 0u);
                                std::vector<std::uint16_t> obst5v(64u * 64u, 0x000Fu);
                                auto tc5v = [](int tx, int ty) -> std::pair<int, int> {
                                    const int u = tx * 32 + 16;
                                    const int v = ty * 32 + 16;
                                    return {64 * 32 + u - v, (u + v) / 2};
                                };

                                ss2::assets::UnitSpec obs5v{};
                                obs5v.id = "obs_5v";
                                obs5v.health = 200;
                                obs5v.sight = 4;

                                ss2::assets::UnitSpec easy5v{};
                                easy5v.id = "easy_5v";
                                easy5v.isVehicle = true;
                                easy5v.selType = "tank";
                                easy5v.health = 600;
                                easy5v.sight = 10;
                                easy5v.marchSightBonus = 4;
                                easy5v.marchEnabled = true;
                                easy5v.crewNumber = 4;
                                easy5v.hasCrewProtection = true;
                                easy5v.crewProtection[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {256, 256, 256};
                                easy5v.armor[static_cast<std::size_t>(ss2::assets::HitType::Sniper)] = {256, 256, 256};

                                ss2::assets::UnitSpec cap5v = easy5v;
                                cap5v.id = "cap_5v";
                                cap5v.sight = 22;
                                cap5v.bonusSight = 6;
                                cap5v.marchSightBonus = 4;

                                ss2::assets::UnitSpec sniper5v{};
                                sniper5v.id = "sniper_5v";
                                sniper5v.selType = "sniper";
                                sniper5v.health = 100;
                                sniper5v.ammo1 = 20;
                                sniper5v.sight = 14;
                                sniper5v.shot1 = {"man_sniper", ss2::assets::HitType::Sniper, 90, 90, 240, 240, 0, 2, 1, 2, "gunshot"};

                                ss2::assets::PlaneSpec leaflet5v{};
                                leaflet5v.id = "b-17c_agit_5v";
                                leaflet5v.health = 2050;
                                leaflet5v.sight = 24;
                                leaflet5v.flySpeed = 6.97;
                                leaflet5v.bombDamage = 5000;
                                leaflet5v.bombsNumber = 3;
                                leaflet5v.bombId = "fsheet";

                                auto cat5v = ss2::assets::UnitCatalog::fromSpecs(
                                    {obs5v, easy5v, cap5v, sniper5v}, {leaflet5v});

                                // 1. Easy 1.3x vehicle/march sight (13 buttoned, 22 open) and 0x1f (31-tile) cap.
                                {
                                    const auto [vx, vy] = tc5v(10, 20);
                                    const auto [d13x, d13y] = tc5v(23, 20);
                                    const auto [d14x, d14y] = tc5v(24, 20);
                                    const auto [d22x, d22y] = tc5v(32, 20);
                                    const auto [d23x, d23y] = tc5v(33, 20);
                                    const auto [d31x, d31y] = tc5v(41, 20);
                                    const auto [d32x, d32y] = tc5v(42, 20);
                                    ss2::TutorialUnits simEasy(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5v, obst5v),
                                        std::vector<ss2::assets::Infantry>{
                                            {"obs_5v", d13x, d13y, 200, 0, 11, 1}, // idx 0: dist 13
                                            {"obs_5v", d14x, d14y, 200, 0, 12, 1}, // idx 1: dist 14
                                            {"obs_5v", d22x, d22y, 200, 0, 13, 1}, // idx 2: dist 22
                                            {"obs_5v", d23x, d23y, 200, 0, 14, 1}, // idx 3: dist 23
                                        },
                                        std::vector<ss2::assets::Placement>{{"easy_5v", vx, vy, 0, 100, 0, 1, 0}},
                                        ss2::FogGrid(64, 64, flags5v), 1, false, {}, cat5v);
                                    if (!simEasy.isValidAttackTarget(2u, 0) || simEasy.isValidAttackTarget(3u, 0)) {
                                        throw std::runtime_error("Phase 5V Easy open march sight ((13+4)*1.3=22) check failed");
                                    }
                                    (void)simEasy.toggleMarchMode(std::array<size_t, 1>{0u});
                                    (void)simEasy.step();
                                    if (!simEasy.isValidAttackTarget(0u, 0) || simEasy.isValidAttackTarget(1u, 0)) {
                                        throw std::runtime_error("Phase 5V Easy buttoned vehicle sight (10*1.3=13) check failed");
                                    }

                                    ss2::TutorialUnits simCap(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5v, obst5v),
                                        std::vector<ss2::assets::Infantry>{
                                            {"obs_5v", d31x, d31y, 200, 0, 11, 1}, // idx 0: dist 31
                                            {"obs_5v", d32x, d32y, 200, 0, 12, 1}, // idx 1: dist 32
                                        },
                                        std::vector<ss2::assets::Placement>{{"cap_5v", vx, vy, 0, 100, 0, 1, 0}},
                                        ss2::FogGrid(64, 64, flags5v), 1, false, {}, cat5v);
                                    if (!simCap.isValidAttackTarget(0u, 0) || simCap.isValidAttackTarget(1u, 0)) {
                                        throw std::runtime_error("Phase 5V Gold 0x1f (31-tile) vehicle sight cap check failed");
                                    }
                                }

                                // 2a. Gold tanksonarmorprotection unbuttoned sniper 100% crew damage & follow-up protection.
                                {
                                    const auto [tx, ty] = tc5v(20, 20);
                                    const auto [fx, fy] = tc5v(33, 20); // 13t away: visible open (14), hidden buttoned (10)
                                    const auto [sx, sy] = tc5v(23, 20); // 96px east (< 240px sniper reach)
                                    ss2::TutorialUnits simSnip(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5v, obst5v),
                                        std::vector<ss2::assets::Infantry>{
                                            {"obs_5v", fx, fy, 200, 0, 11, 1},
                                            {"sniper_5v", sx, sy, 100, 20, 12, 1},
                                        },
                                        std::vector<ss2::assets::Placement>{{"easy_5v", tx, ty, 0, 100, 0, 1, 0}},
                                        ss2::FogGrid(64, 64, flags5v), 2, true, {}, cat5v);
                                    const auto snap0 = simSnip.takeSnapshot();
                                    if (snap0.vehicles[0].crewHp != 210 || !simSnip.isValidAttackTarget(0u, 0)) {
                                        throw std::runtime_error("Phase 5V unbuttoned sniper baseline setup failed");
                                    }
                                    bool hitLanded = false;
                                    for (int t = 0; t < 10; ++t) {
                                        (void)simSnip.step();
                                        const auto snapNow = simSnip.takeSnapshot();
                                        if (snapNow.vehicles[0].crewHp < snap0.vehicles[0].crewHp) {
                                            hitLanded = true;
                                            if (snapNow.vehicles[0].crewHp != 120) {
                                                throw std::runtime_error("Phase 5V unbuttoned sniper should deal 100% rawDamage (210->120)");
                                            }
                                            if (simSnip.isValidAttackTarget(0u, 0)) {
                                                throw std::runtime_error("Phase 5V suppressed/hit tank failed to button up sight");
                                            }
                                            break;
                                        }
                                    }
                                    if (!hitLanded) {
                                        throw std::runtime_error("Phase 5V sniper failed to hit unbuttoned commander");
                                    }
                                    for (int t = 0; t < 10; ++t) (void)simSnip.step();
                                    if (simSnip.takeSnapshot().vehicles[0].crewHp != 120) {
                                        throw std::runtime_error("Phase 5V auto-buttoned tank should be immune to follow-up sniper fire");
                                    }
                                }

                                // 2b. Pure suppression auto-button-up & recovery (0x1001d0b7) without weapon combat timer.
                                {
                                    const auto [tx, ty] = tc5v(20, 20);
                                    const auto [fx, fy] = tc5v(33, 20); // 13t away: visible open (14), hidden suppressed (10)
                                    ss2::TutorialUnits simSup(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5v, obst5v),
                                        std::vector<ss2::assets::Infantry>{{"obs_5v", fx, fy, 200, 0, 11, 1}},
                                        std::vector<ss2::assets::Placement>{{"easy_5v", tx, ty, 0, 100, 0, 1, 0}},
                                        ss2::FogGrid(64, 64, flags5v), 2, false, {}, cat5v);
                                    std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air5v{};
                                    air5v[1][0] = {"b-17c_agit_5v", 1, 1};
                                    std::array<int, 4> pGrp5v{};
                                    std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos5v{};
                                    simSup.setAirSupport(air5v, pGrp5v, pRos5v);
                                    if (!simSup.isValidAttackTarget(0u, 0) ||
                                        !simSup.callAirSupport(ss2::assets::AirMissionKind::Bomber, tx, ty, 1)) {
                                        throw std::runtime_error("Phase 5V suppression button-up initial setup failed");
                                    }
                                    bool sawSuppressedButtonUp = false;
                                    for (int t = 0; t < 260; ++t) {
                                        (void)simSup.step();
                                        bool dropped = false;
                                        for (const auto& ae : simSup.takeAirStrikeEvents()) {
                                            if (ae.kind == ss2::AirStrikeEvent::BombImpact) dropped = true;
                                        }
                                        if (dropped && !simSup.isValidAttackTarget(0u, 0)) {
                                            sawSuppressedButtonUp = true;
                                            break;
                                        }
                                    }
                                    if (!sawSuppressedButtonUp || simSup.takeSnapshot().vehicles[0].hp != 600) {
                                        throw std::runtime_error("Phase 5V suppression failed to auto-button open hatch");
                                    }
                                    for (int t = 0; t < 70; ++t) (void)simSup.step();
                                    if (!simSup.isValidAttackTarget(0u, 0)) {
                                        throw std::runtime_error("Phase 5V tank failed to unbutton once suppression decayed");
                                    }
                                }

                                std::cout << "Phase 5V Gold 0x1f sight cap, Easy 1.3x vehicle sight, unbuttoned 100% sniper damage & suppression button-up PASSED\n";
                            }

                            // Phase 5W: Universal Shift-order queueing, retail AVIA*/UNITS* fidelity,
                            // forest/crater cover, and combined-arms AI (counter-battery + AI mine clearing).
                            {
                                auto tc5w = [](int tx, int ty) -> std::pair<int, int> {
                                    const int u = tx * 32 + 16;
                                    const int v = ty * 32 + 16;
                                    return {64 * 32 + u - v, (u + v) / 2};
                                };
                                std::vector<std::uint32_t> flags5w(64u * 64u, 0u);
                                std::vector<std::uint16_t> obst5w(64u * 64u, 0x000Fu);
                                flags5w[24u * 64u + 24u] |= 0x1000u; // forest cover tile

                                auto vetSpec = ss2::assets::UnitCatalog::parseUnitText(
                                    "vet_5w",
                                    "cancrouchexpa 1\nhealth 900\nsight 12\ncrouchsight 9\n"
                                    "shot1.id mg\nshot1.damage 10 12\nshot1.range 200 220\nshot1.reloadtime 20\n");
                                auto planeCap = ss2::assets::UnitCatalog::parsePlaneText(
                                    "yak_5w",
                                    "health 300\nsight 20\nflyspeed 10\nactiontime 400\nshot_grn_damage 80\nshot_air_damage 90\n");
                                auto planeBomb = ss2::assets::UnitCatalog::parsePlaneText(
                                    "he111_5w",
                                    "health 400\nsight 20\nflyspeed 8\nbombdamage 1800\nbombsnumber 2\nbombaccuracy 90\nbombreloadtime 12\n");
                                if (!vetSpec.canCrouchExpa || planeCap.actionTime != 400 ||
                                    planeBomb.bombAccuracy != 90 || planeBomb.bombReloadTime != 12) {
                                    throw std::runtime_error("Phase 5W retail catalog field parsing check failed");
                                }

                                ss2::assets::UnitSpec sapSpec5w{};
                                sapSpec5w.id = "sapper_5w";
                                sapSpec5w.selType = "sapper";
                                sapSpec5w.health = 100;
                                sapSpec5w.sight = 12;
                                sapSpec5w.moveSpeed = 0.6;
                                sapSpec5w.mineAmmo = 5;
                                sapSpec5w.ammo1 = 30;
                                sapSpec5w.shot1.shotId = "rifle";
                                sapSpec5w.shot1.hitType = ss2::assets::HitType::Machine;
                                sapSpec5w.shot1.damage = 10;
                                sapSpec5w.shot1.range = 180;
                                sapSpec5w.shot1.reloadTicks = 20;

                                ss2::assets::UnitSpec spgSpec5w{};
                                spgSpec5w.id = "spg_5w";
                                spgSpec5w.isVehicle = true;
                                spgSpec5w.health = 400;
                                spgSpec5w.sight = 12;
                                spgSpec5w.moveSpeed = 2.0;
                                spgSpec5w.backMoveSpeed = 1.6;
                                spgSpec5w.canMoveBackward = true;
                                spgSpec5w.ammo1 = 20;
                                spgSpec5w.shot1.shotId = "he_5w";
                                spgSpec5w.shot1.hitType = ss2::assets::HitType::Explosive;
                                spgSpec5w.shot1.damage = 220;
                                spgSpec5w.shot1.damageMax = 220;
                                spgSpec5w.shot1.range = 280;
                                spgSpec5w.shot1.rangeMax = 280;
                                spgSpec5w.shot1.deadzone = 32;
                                spgSpec5w.shot1.reloadTicks = 20;
                                spgSpec5w.shot1.burstShots = 1;
                                spgSpec5w.shot1.burstReloadTicks = 20;
                                spgSpec5w.shot1.craterScale = 2;

                                ss2::assets::UnitSpec tractorSpec5w{};
                                tractorSpec5w.id = "trac_5w";
                                tractorSpec5w.isVehicle = true;
                                tractorSpec5w.health = 200;
                                tractorSpec5w.sight = 10;
                                tractorSpec5w.moveSpeed = 2.2;
                                tractorSpec5w.towSpeedByClass = {0.8, 0.7, 0.6, 0.5, 0.4, 0.3};

                                ss2::assets::UnitSpec gunSpec5w{};
                                gunSpec5w.id = "gun_5w";
                                gunSpec5w.isVehicle = true;
                                gunSpec5w.selType = "howitzer";
                                gunSpec5w.health = 200;
                                gunSpec5w.sight = 12;
                                gunSpec5w.moveSpeed = 0.0;
                                gunSpec5w.towWeightClass = "light";
                                gunSpec5w.ammo1 = 20;
                                gunSpec5w.shot1.shotId = "he_5w";
                                gunSpec5w.shot1.hitType = ss2::assets::HitType::Explosive;
                                gunSpec5w.shot1.damage = 220;
                                gunSpec5w.shot1.range = 750;
                                gunSpec5w.shot1.deadzone = 40;
                                gunSpec5w.shot1.reloadTicks = 25;
                                gunSpec5w.shot1.burstShots = 1;
                                gunSpec5w.shot1.burstReloadTicks = 25;
                                gunSpec5w.shot1.indirectArc = true;

                                auto cat5w = ss2::assets::UnitCatalog::fromSpecs(
                                    {vetSpec, sapSpec5w, spgSpec5w, tractorSpec5w, gunSpec5w},
                                    {planeCap, planeBomb},
                                    /*retailCatalog=*/true);

                                // 1) Universal Shift queue + shoot-and-scoot + multi-segment selectionOrderLines.
                                {
                                    const auto [sx, sy] = tc5w(10, 20);
                                    const auto [wx, wy] = tc5w(12, 20);
                                    const auto [bx, by] = tc5w(14, 20);
                                    const auto [spgX, spgY] = tc5w(10, 14);
                                    const auto [tgtX, tgtY] = tc5w(15, 14);
                                    const auto [revX, revY] = tc5w(8, 14);
                                    const auto [trX, trY] = tc5w(20, 28);
                                    const auto [trDstX, trDstY] = tc5w(22, 28);
                                    const auto [gunX, gunY] = tc5w(21, 28);

                                    std::vector<ss2::assets::Infantry> infQ{
                                        {"sapper_5w", sx, sy, 100, 30, 1, 0},
                                        {"vet_5w", tc5w(24, 24).first, tc5w(24, 24).second, 900, 30, 2, 0},
                                    };
                                    infQ[1].expa = 6000;
                                    std::vector<ss2::assets::Placement> vehQ{
                                        {"spg_5w", spgX, spgY, 0, 100, 100, 1, 0},
                                        {"trac_5w", trX, trY, 0, 100, 0, 1, 0},
                                        {"gun_5w", gunX, gunY, 0, 100, 100, 1, 0},
                                    };
                                    ss2::TutorialUnits simQ(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5w, obst5w),
                                        infQ, vehQ, ss2::FogGrid(64, 64, flags5w), 2, false, {}, cat5w);
                                    simQ.setBuildings({{bx, by, bx - 16, by - 16, 32, 32, 4, 400}});
                                    simQ.setMines(std::vector<std::uint8_t>(64u * 64u, 0u));

                                    if (simQ.infantryCoverAtMapPoint(tc5w(24, 24).first, tc5w(24, 24).second) != 1) {
                                        throw std::runtime_error("Phase 5W forest tile should report forest cover");
                                    }

                                    // Sapper Shift chain: Move -> LayMine -> GarrisonBuilding
                                    if (simQ.orderSelection(std::array<size_t, 1>{0u}, wx, wy, false) != 1u ||
                                        simQ.queueSoldierOrder(std::array<size_t, 1>{0u}, ss2::QueuedSoldierOp::LayMine, 0, 0, 0) != 1u ||
                                        simQ.queueSoldierOrder(std::array<size_t, 1>{0u}, ss2::QueuedSoldierOp::GarrisonBuilding, 0, 0, 0) != 1u) {
                                        throw std::runtime_error("Phase 5W sapper Shift-queue chain setup failed");
                                    }
                                    const auto sapLines = simQ.selectionOrderLines(std::array<size_t, 1>{0u}, {});
                                    if (sapLines.size() < 2u) {
                                        throw std::runtime_error("Phase 5W multi-segment sapper order lines expected >= 2 segments");
                                    }

                                    // SPG shoot-and-scoot: AttackGround -> ReverseMove
                                    if (simQ.orderAttackGroundVehicles(std::array<size_t, 1>{0u}, tgtX, tgtY) != 1u ||
                                        simQ.queueVehicleOrder(std::array<size_t, 1>{0u}, ss2::QueuedVehicleOp::ReverseMove, revX, revY) != 1u) {
                                        throw std::runtime_error("Phase 5W shoot-and-scoot queue setup failed");
                                    }
                                    const auto spgLines = simQ.selectionOrderLines({}, std::array<size_t, 1>{0u});
                                    if (spgLines.size() < 2u ||
                                        spgLines[0].kind != ss2::TacticalOrderLine::Kind::AttackGround ||
                                        spgLines[1].kind != ss2::TacticalOrderLine::Kind::Move) {
                                        throw std::runtime_error("Phase 5W shoot-and-scoot preview lines check failed");
                                    }

                                    // Tractor hitch -> Move -> Unhitch
                                    if (simQ.orderTowVehicle(1u, 2u) != 1u) {
                                        throw std::runtime_error("Phase 5W tractor hitch failed");
                                    }
                                    for (int t = 0; t < 20 && simQ.takeSnapshot().vehicles[1].towedVehicle != 2; ++t) {
                                        (void)simQ.step();
                                    }
                                    if (simQ.moveVehicles(std::array<size_t, 1>{1u}, trDstX, trDstY, false) != 1u ||
                                        simQ.queueVehicleOrder(std::array<size_t, 1>{1u}, ss2::QueuedVehicleOp::Unhitch, 0, 0) != 1u) {
                                        throw std::runtime_error("Phase 5W tractor Move->Unhitch queue setup failed");
                                    }

                                    bool sawCrater = false;
                                    for (int t = 0; t < 160; ++t) {
                                        (void)simQ.step();
                                        if (simQ.infantryCoverAtMapPoint(tgtX, tgtY) == 2) sawCrater = true;
                                    }
                                    const auto snapQ = simQ.takeSnapshot();
                                    if (simQ.visibleMines(0).empty() || snapQ.buildings[0].occupants.size() != 1u) {
                                        throw std::runtime_error("Phase 5W Move->LayMine->GarrisonBuilding chain failed to complete");
                                    }
                                    if (snapQ.vehicles[0].ammo >= 20 || snapQ.vehicles[0].x >= spgX - 24) {
                                        throw std::runtime_error("Phase 5W SPG shoot-and-scoot failed to fire then reverse");
                                    }
                                    if (snapQ.vehicles[1].towedVehicle != -1) {
                                        throw std::runtime_error("Phase 5W Tractor Move->Unhitch failed to unhitch");
                                    }
                                    if (!sawCrater) {
                                        throw std::runtime_error("Phase 5W heavy HE shell impact failed to stamp crater cover");
                                    }
                                }

                                // 2) Fighter CAP loiter (actiontime), bomber scatter craters, & cancrouchexpa veteran prone.
                                {
                                    const auto [vx, vy] = tc5w(24, 24);
                                    const auto [tx, ty] = tc5w(30, 24);
                                    std::vector<ss2::assets::Infantry> infAir{
                                        {"vet_5w", vx, vy, 900, 30, 1, 0},
                                    };
                                    infAir[0].expa = 6000;
                                    ss2::TutorialUnits simAir(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5w, obst5w),
                                        infAir, {}, ss2::FogGrid(64, 64, flags5w), 2, false, {}, cat5w);
                                    std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air5w{};
                                    air5w[0][4] = {"yak_5w", 1, 1};
                                    air5w[1][0] = {"he111_5w", 1, 1};
                                    std::array<int, 4> pGrp5w{};
                                    std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos5w{};
                                    simAir.setAirSupport(air5w, pGrp5w, pRos5w);
                                    if (!simAir.callAirSupport(ss2::assets::AirMissionKind::Bomber, vx, vy, 1)) {
                                        throw std::runtime_error("Phase 5W bomber air support call failed");
                                    }
                                    bool sawBombDrop = false;
                                    for (int t = 0; t < 240 && !sawBombDrop; ++t) {
                                        (void)simAir.step();
                                        for (const auto& ae : simAir.takeAirStrikeEvents()) {
                                            if (ae.kind == ss2::AirStrikeEvent::BombImpact) {
                                                sawBombDrop = true;
                                            }
                                        }
                                    }
                                    if (!sawBombDrop) {
                                        throw std::runtime_error("Phase 5W scattered bomber failed to drop bombs");
                                    }
                                    if (!simAir.takeSnapshot().soldiers[0].crouched) {
                                        throw std::runtime_error("Phase 5W cancrouchexpa veteran failed to auto-prone under bomb suppression");
                                    }
                                    if (!simAir.callAirSupport(ss2::assets::AirMissionKind::Fighter, tx, ty, 0)) {
                                        throw std::runtime_error("Phase 5W fighter air support call failed");
                                    }
                                    int fighterTicksAlive = 0;
                                    for (int t = 0; t < 260; ++t) {
                                        (void)simAir.step();
                                        const auto poses = simAir.takeAirSorties();
                                        for (const auto& sp : poses) {
                                            if (sp.kind == ss2::assets::AirMissionKind::Fighter && sp.active && sp.alive) {
                                                ++fighterTicksAlive;
                                            }
                                        }
                                    }
                                    if (fighterTicksAlive < 110) {
                                        throw std::runtime_error("Phase 5W fighter actiontime CAP loiter too short");
                                    }
                                }

                                // 3) Combined-arms AI: Counter-battery on muzzleRevealTicks & AI Sapper mine clear.
                                {
                                    const auto [aiGunX, aiGunY] = tc5w(10, 20);
                                    const auto [pArtX, pArtY] = tc5w(19, 20);
                                    const auto [aiSapX, aiSapY] = tc5w(32, 20);
                                    std::vector<ss2::assets::Infantry> infAi{
                                        {"sapper_5w", aiSapX, aiSapY, 100, 30, 5, 1},
                                    };
                                    std::vector<ss2::assets::Placement> vehAi{
                                        {"gun_5w", aiGunX, aiGunY, 0, 100, 100, 6, 1},
                                        {"gun_5w", pArtX, pArtY, 0, 100, 100, 1, 0},
                                    };
                                    ss2::TutorialUnits simAi(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5w, obst5w),
                                        infAi, vehAi, ss2::FogGrid(64, 64, flags5w), 2, false, {}, cat5w);
                                    std::vector<std::uint8_t> minesAi(64u * 64u, 0u);
                                    minesAi[20u * 64u + 34u] = 0x05u; // Player 0 AT mine 2t east of AI Sapper
                                    simAi.setMines(minesAi);
                                    simAi.revealFogAt(aiGunX, aiGunY, 6, 1);

                                    // Player artillery fires once at distant empty ground to emit muzzleRevealTicks (> 0) without line-of-sight
                                    const auto [shellTargetX, shellTargetY] = tc5w(28, 20);
                                    (void)simAi.orderAttackGroundVehicles(std::array<size_t, 1>{1u}, shellTargetX, shellTargetY);
                                    std::vector<std::uint8_t> zonesAi(64u * 64u, 0u);
                                    for (int t = 0; t < 120; ++t) {
                                        (void)simAi.orderGroupSoldArt(6u, 0u, zonesAi, 0u, aiGunX, aiGunY, false, 0u, 0, 0, false, true);
                                        (void)simAi.step();
                                    }
                                    if (simAi.takeSnapshot().vehicles[1].hp >= 200) {
                                        throw std::runtime_error("Phase 5W AI counter-battery failed to hit firing unspotted player artillery");
                                    }
                                    if (!simAi.visibleMines(0).empty()) {
                                        throw std::runtime_error("Phase 5W AI Sapper failed to detect & disarm hostile player mine");
                                    }
                                }

                                std::cout << "Phase 5W universal Shift queues, shoot-and-scoot, CAP loiter, cancrouchexpa, forest/crater cover & combined-arms AI PASSED\n";
                            }

                            // Phase 5X: Wave 3 (Smoke LOS occlusion, Bottleneck Anti-Freeze, Convoy Pace, Safe Depot Logistics, Realism Doctrine).
                            {
                                ss2::assets::UnitSpec obsSpec5x{};
                                obsSpec5x.id = "officer_5x";
                                obsSpec5x.isVehicle = false;
                                obsSpec5x.health = 100;
                                obsSpec5x.sight = 14;
                                obsSpec5x.moveSpeed = 0.5;
                                obsSpec5x.ammo1 = 30;
                                obsSpec5x.selType = "officer";
                                obsSpec5x.shot1.damage = 10;
                                obsSpec5x.shot1.range = 180;
                                obsSpec5x.shot1.reloadTicks = 25;

                                ss2::assets::UnitSpec fastTank5x{};
                                fastTank5x.id = "tank_5x";
                                fastTank5x.isVehicle = true;
                                fastTank5x.health = 400;
                                fastTank5x.sight = 12;
                                fastTank5x.moveSpeed = 1.8;
                                fastTank5x.moveSpeedMax = 1.8;
                                fastTank5x.selType = "tank";
                                fastTank5x.crewNumber = 3;
                                fastTank5x.crewUnit = "officer_5x";
                                fastTank5x.ammo1 = 40;
                                fastTank5x.shot1.damage = 120;
                                fastTank5x.shot1.range = 400;
                                fastTank5x.shot1.reloadTicks = 30;
                                fastTank5x.shot1.hitType = ss2::assets::HitType::Pierce;

                                ss2::assets::UnitSpec supSpec5x{};
                                supSpec5x.id = "sup_5x";
                                supSpec5x.isVehicle = true;
                                supSpec5x.health = 180;
                                supSpec5x.sight = 10;
                                supSpec5x.moveSpeed = 1.1;
                                supSpec5x.moveSpeedMax = 1.1;
                                supSpec5x.selType = "sappers";
                                supSpec5x.ammo1 = 600;
                                supSpec5x.ammo2 = 400;
                                supSpec5x.ammoRegenDelay = 25;

                                ss2::assets::UnitSpec shipSpec5x{};
                                shipSpec5x.id = "ship_5x";
                                shipSpec5x.isVehicle = true;
                                shipSpec5x.health = 600;
                                shipSpec5x.sight = 14;
                                shipSpec5x.moveSpeed = 1.6;
                                shipSpec5x.moveSpeedMax = 1.6;
                                shipSpec5x.selType = "landing";
                                shipSpec5x.walkOnGround = false;
                                shipSpec5x.walkOnWater = true;

                                auto cat5x = ss2::assets::UnitCatalog::fromSpecs(
                                    {obsSpec5x, fastTank5x, supSpec5x, shipSpec5x}, {}, /*retailCatalog=*/true);

                                auto tc5x = [&](int tx, int ty) -> std::pair<int, int> {
                                    const int wu = tx * 32 + 16;
                                    const int wv = ty * 32 + 16;
                                    return {2048 + wu - wv, (wu + wv) / 2};
                                };
                                std::vector<std::uint32_t> flags5x(64u * 64u, 0u);
                                std::vector<std::uint16_t> obst5x(64u * 64u, 0u);

                                // 1) Smoke screen LOS occlusion & expiration restore.
                                {
                                    const auto [ox, oy] = tc5x(20, 20);
                                    const auto [sx, sy] = tc5x(23, 20);
                                    const auto [ex, ey] = tc5x(26, 20);
                                    std::vector<ss2::assets::Infantry> infSmk{
                                        {"officer_5x", ox, oy, 100, 20, 1, 0},
                                        {"officer_5x", ex, ey, 100, 20, 2, 1},
                                    };
                                    ss2::TutorialUnits simSmk(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5x, obst5x),
                                        infSmk, {}, ss2::FogGrid(64, 64, flags5x), 2, false, {}, cat5x);
                                    (void)simSmk.step();
                                    if (!simSmk.isValidAttackTarget(1u, 0)) {
                                        throw std::runtime_error("Phase 5X baseline expected clear LOS before smoke");
                                    }
                                    if (simSmk.orderDeploySmoke(std::array<size_t, 1>{0u}, {}, sx, sy) != 1u) {
                                        throw std::runtime_error("Phase 5X officer deploy smoke failed");
                                    }
                                    if (simSmk.isValidAttackTarget(1u, 0)) {
                                        throw std::runtime_error("Phase 5X active smoke cloud failed to block LOS");
                                    }
                                    for (int t = 0; t < 290; ++t) (void)simSmk.step();
                                    if (!simSmk.activeSmokeClouds().empty() || !simSmk.isValidAttackTarget(1u, 0)) {
                                        throw std::runtime_error("Phase 5X expired smoke cloud failed to restore LOS");
                                    }
                                }

                                // 2) Vehicle bottleneck anti-freeze + Convoy Pace Move.
                                {
                                    const auto [tx, ty] = tc5x(15, 20);
                                    const auto [bx, by] = tc5x(16, 20);
                                    const auto [dx, dy] = tc5x(22, 20);
                                    std::vector<ss2::assets::Infantry> infConv{
                                        {"officer_5x", tx, ty - 32, 100, 20, 1, 0},
                                    };
                                    std::vector<ss2::assets::Placement> vehConv{
                                        {"tank_5x", tx, ty, 0, 100, 100, 1, 0},
                                        {"tank_5x", bx, by, 0, 100, 100, 1, 0},
                                    };
                                    ss2::TutorialUnits simConv(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5x, obst5x),
                                        infConv, vehConv, ss2::FogGrid(64, 64, flags5x), 2, false, {}, cat5x);
                                    if (simConv.orderConvoyMove(std::array<size_t, 1>{0u}, std::array<size_t, 1>{0u},
                                                                dx, dy, false) != 2u) {
                                        throw std::runtime_error("Phase 5X convoy move setup failed");
                                    }
                                    const int startX = simConv.takeSnapshot().vehicles[0].x;
                                    // Temporary blocker sits ahead for 4 ticks, then moves out of the way.
                                    for (int t = 0; t < 4; ++t) (void)simConv.step();
                                    const int midX = simConv.takeSnapshot().vehicles[0].x;
                                    if (midX - startX > 22) {
                                        throw std::runtime_error("Phase 5X convoy speed cap exceeded infantry convoy pace");
                                    }
                                    const auto [clearX, clearY] = tc5x(16, 26);
                                    (void)simConv.moveVehicles(std::array<size_t, 1>{1u}, clearX, clearY, false);
                                    for (int t = 0; t < 240; ++t) (void)simConv.step();
                                    const int endX = simConv.takeSnapshot().vehicles[0].x;
                                    if (std::abs(endX - dx) > 48) {
                                        throw std::runtime_error("Phase 5X tank dropped order or stalled at temporary bottleneck");
                                    }
                                }

                                // 3) Safe logistics threat filter + Stationary Depot Mode + Realism Doctrine.
                                {
                                    const auto [sx, sy] = tc5x(12, 20);
                                    const auto [vx, vy] = tc5x(18, 20);
                                    const auto [ex, ey] = tc5x(20, 20);
                                    const auto [adjX, adjY] = tc5x(13, 20);
                                    std::vector<ss2::assets::Placement> vehLog{
                                        {"sup_5x", sx, sy, 0, 100, 100, 1, 0},
                                        {"tank_5x", vx, vy, 0, 85, 100, 1, 0},
                                        {"tank_5x", ex, ey, 0, 100, 100, 2, 1},
                                        {"tank_5x", adjX, adjY, 0, 60, 100, 1, 0},
                                    };
                                    ss2::TutorialUnits simLog(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags5x, obst5x),
                                        {}, vehLog, ss2::FogGrid(64, 64, flags5x), 2, true, {}, cat5x);
                                    (void)simLog.toggleVehicleHoldFire(std::array<size_t, 1>{0u});
                                    const int adjPreHp = simLog.takeSnapshot().vehicles[3].hp;
                                    for (int t = 0; t < 35; ++t) (void)simLog.step();
                                    const auto snapDepot = simLog.takeSnapshot();
                                    if (snapDepot.vehicles[0].x != sx || snapDepot.vehicles[0].y != sy ||
                                        snapDepot.vehicles[3].hp <= adjPreHp) {
                                        throw std::runtime_error("Phase 5X Depot Mode failed to stay put & repair adjacent ally");
                                    }
                                    (void)simLog.toggleVehicleHoldFire(std::array<size_t, 1>{0u});
                                    for (int t = 0; t < 35; ++t) (void)simLog.step();
                                    const auto snapSafe = simLog.takeSnapshot();
                                    if (snapSafe.vehicles[0].x != sx || snapSafe.vehicles[0].y != sy) {
                                        throw std::runtime_error("Phase 5X supply truck suicided toward tank next to spotted enemy");
                                    }
                                    // Verify Realism Doctrine increases tank engagement range & sight radius.
                                    const auto prevBefore = simLog.selectionRangePreviews(std::array<size_t, 1>{3u});
                                    simLog.setRealismDoctrine(true);
                                    const auto prevAfter = simLog.selectionRangePreviews(std::array<size_t, 1>{3u});
                                    if (prevBefore.empty() || prevAfter.empty() ||
                                        prevAfter.front().rangePx <= prevBefore.front().rangePx + 40.0f ||
                                        prevAfter.front().sightPx <= prevBefore.front().sightPx + 32.0f) {
                                        throw std::runtime_error("Phase 5X Realism Doctrine failed to expand standoff range & sight");
                                    }
                                }

                                // 4) Cherbourg partial-blocked 'o' hedgerow wall routing + spawn snap + naval edge navigation.
                                {
                                    std::vector<std::uint32_t> flagsHedge(64u * 64u, 0u);
                                    std::vector<std::uint16_t> obstHedge(64u * 64u, 0x000Fu); // ground tiles
                                    // Vertical hedgerow barrier at tile x = 24 with ONLY a 1-subcell 'o' gap at (24, 20)
                                    // where center slot 3 (bit 3) is blocked (0x0E blocks slots 1,2,3; only slot 0 open).
                                    for (int ty = 14; ty <= 26; ++ty) {
                                        flagsHedge[std::size_t(ty) * 64u + 24u] = (ty == 20) ? 0x0Eu : 0x0Fu;
                                    }
                                    // Blocked tile at (20, 22) to test spawnVehicle snapping off blocked spawn.
                                    flagsHedge[22u * 64u + 20u] = 0x0Fu;
                                    // Southern coastal water band for y = 38..48, x = 0..32 (ground at x >= 33).
                                    for (int ty = 38; ty <= 48; ++ty) {
                                        for (int tx = 0; tx <= 32; ++tx) {
                                            obstHedge[std::size_t(ty) * 64u + std::size_t(tx)] = 0x00F0u; // water only
                                        }
                                    }

                                    const auto [tStartX, tStartY] = tc5x(20, 20);
                                    const auto [tDstX, tDstY] = tc5x(28, 20);
                                    const auto [infX, infY] = tc5x(23, 20);
                                    const auto [shipStartX, shipStartY] = tc5x(2, 43);
                                    const auto [shipDstX, shipDstY] = tc5x(20, 43);

                                    std::vector<ss2::assets::Infantry> infHedge{
                                        {"officer_5x", infX, infY, 100, 0, 1, 0},
                                        {"officer_5x", infX + 4, infY, 100, 0, 1, 0},
                                        {"officer_5x", infX - 4, infY + 2, 100, 0, 1, 0},
                                    };
                                    std::vector<ss2::assets::Placement> vehHedge{
                                        {"tank_5x", tStartX, tStartY, 0, 100, 100, 1, 0},
                                        {"ship_5x", shipStartX, shipStartY, 0, 100, 100, 1, 0},
                                    };
                                    ss2::TutorialUnits simHedge(
                                        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flagsHedge, obstHedge),
                                        infHedge, vehHedge, ss2::FogGrid(64, 64, flagsHedge), 2, false, {}, cat5x);

                                    // Verify live spawnVehicle snaps out of fully blocked tile (20, 22).
                                    const auto [blkX, blkY] = tc5x(20, 22);
                                    ss2::assets::VehicleUnit reinUnit{};
                                    reinUnit.id = "tank_5x";
                                    reinUnit.x = blkX;
                                    reinUnit.y = blkY;
                                    reinUnit.hp = 100;
                                    reinUnit.ammo = 100;
                                    reinUnit.owner = 0;
                                    reinUnit.group = 1;
                                    reinUnit.hasCrew = true;
                                    reinUnit.visible = true;
                                    reinUnit.alive = true;
                                    const size_t snappedIdx = simHedge.spawnVehicle(reinUnit);

                                    if (simHedge.moveVehicles(std::array<size_t, 1>{0u}, tDstX, tDstY, false) != 1u ||
                                        simHedge.moveVehicles(std::array<size_t, 1>{1u}, shipDstX, shipDstY, false) != 1u) {
                                        throw std::runtime_error("Phase 5X Cherbourg hedgerow/naval move order rejected");
                                    }
                                    for (int t = 0; t < 260; ++t) {
                                        (void)simHedge.step();
                                    }
                                    const auto snapHedge = simHedge.takeSnapshot();
                                    const auto& tankEnd = snapHedge.vehicles[0];
                                    const auto& shipEnd = snapHedge.vehicles[1];
                                    const auto& snappedVeh = snapHedge.vehicles[snappedIdx];
                                    if (std::abs(tankEnd.x - tDstX) > 40 || std::abs(tankEnd.y - tDstY) > 40) {
                                        throw std::runtime_error("Phase 5X tank got stuck on partial-blocked hedgerow gap");
                                    }
                                    if (std::abs(shipEnd.x - shipDstX) > 48 || std::abs(shipEnd.y - shipDstY) > 48) {
                                        throw std::runtime_error("Phase 5X border ship failed to navigate from coastal edge");
                                    }
                                    if (snappedVeh.x == blkX && snappedVeh.y == blkY) {
                                        throw std::runtime_error("Phase 5X spawnVehicle failed to snap out of blocked tile");
                                    }
                                }

                                std::cout << "Phase 5X Smoke LOS, Bottleneck Repath, Convoy Pace, Safe Logistics & Realism Doctrine PASSED\n";
                            }
                        }
                    }
}
}  // namespace ss2::vm_test
