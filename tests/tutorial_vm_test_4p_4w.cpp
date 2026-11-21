#include "tutorial_vm_test_internal.hpp"

namespace ss2::vm_test {
void runPhase4Pto4W() {
    const auto flags = emptyFlags128();
    const auto obstacles = emptyObstacles128();
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

}
}  // namespace ss2::vm_test
