#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runPhase4Fto4O() {
    const auto flags = emptyFlags128();
    const auto obstacles = emptyObstacles128();
    const auto pTankSpec = makePhase4ETigerSpec();
    const auto cat4e = makePhase4ECatalog();
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
                    // Gold CSoldier::GetSightRadius (0x10021381: cmp [ecx+8], 1; jg) scales Easy player sight by 1.3x
                    // and leaves Hard AI sight equal to Normal (Hard scales combat damage instead).
                    if (playerSightEasy <= playerSightNorm || enemySightHard != enemySightNorm) {
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

            }
}
}  // namespace ss2::vm_test
