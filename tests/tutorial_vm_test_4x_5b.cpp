#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runPhase4Xto5B() {
    const auto flags = emptyFlags128();
    const auto obstacles = emptyObstacles128();
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
                        if (crewSpawns.size() != 5u || crewSpawns[0].id != "dtankist") {
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

                }
}
}  // namespace ss2::vm_test
