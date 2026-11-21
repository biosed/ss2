#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runPhase5Kto5O() {
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

}
}  // namespace ss2::vm_test
