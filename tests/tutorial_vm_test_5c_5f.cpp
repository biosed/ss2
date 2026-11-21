#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runPhase5Cto5F() {
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

}
}  // namespace ss2::vm_test
