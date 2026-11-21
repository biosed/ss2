#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runPhase5Gto5J() {
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

}
}  // namespace ss2::vm_test
