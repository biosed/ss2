#include "assets/placement.hpp"
#include "assets/scenery.hpp"
#include "assets/unit_catalog.hpp"
#include "sim/det_math.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/infantry_path.hpp"
#include "sim/state_hash.hpp"
#include "sim/tutorial_units.hpp"
#include "sim/tutorial_units_internal.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

void require(bool cond, const char* msg) {
    if (!cond) throw std::runtime_error(msg);
}

ss2::assets::WeaponSpec makeWeapon(std::string shotId, ss2::assets::HitType ht,
                                   int dmgMin, int dmgMax, int rangeMin, int rangeMax,
                                   int deadzone, int reload, int burstShots, int burstReload,
                                   std::string snd, int accBase = 0, int accMin = 0,
                                   int expa = 0, bool indirectArc = false, int craterScale = 0) {
    ss2::assets::WeaponSpec w{};
    w.shotId = std::move(shotId);
    w.hitType = ht;
    w.damage = dmgMin;
    w.damageMax = dmgMax;
    w.range = rangeMin;
    w.rangeMax = rangeMax;
    w.deadzone = deadzone;
    w.reloadTicks = reload;
    w.burstShots = burstShots;
    w.burstReloadTicks = burstReload;
    w.soundGroup = std::move(snd);
    w.accuracyBase = accBase;
    w.accuracyMin = accMin;
    w.expaPerShot = expa;
    w.indirectArc = indirectArc;
    w.craterScale = craterScale;
    return w;
}

ss2::assets::Infantry makeInf(std::string id, int x, int y, int hp, int ammo, int group, int owner) {
    ss2::assets::Infantry s{};
    s.id = std::move(id);
    s.x = x;
    s.y = y;
    s.hp = hp;
    s.ammo = ammo;
    s.group = group;
    s.owner = owner;
    return s;
}

ss2::assets::Placement makeVeh(std::string id, int x, int y, int dir, int hp, int ammo, int group, int owner) {
    ss2::assets::Placement v{};
    v.id = std::move(id);
    v.x = x;
    v.y = y;
    v.direction = dir;
    v.hp = hp;
    v.ammo = ammo;
    v.group = group;
    v.owner = owner;
    return v;
}

void testDetMathUnits() {
    // 1. detHypot
    require(ss2::det::detHypot(0.0, 0.0) == 0.0, "detHypot(0,0) failed");
    require(ss2::det::detHypot(-0.0, 0.0) == 0.0, "detHypot(-0,0) failed");
    require(ss2::det::detHypot(3.0, 4.0) == 5.0, "detHypot(3,4) failed");
    require(ss2::det::detHypot(-3.0, 4.0) == 5.0, "detHypot(-3,4) failed");
    const double bigH = ss2::det::detHypot(3e200, 4e200);
    require(std::isfinite(bigH) && std::abs(bigH / 5e200 - 1.0) < 1e-14, "detHypot overflow scaling failed");

    // 2. detRoundToInt64 / detRoundToInt
    require(ss2::det::detRoundToInt64(0.0) == 0, "detRound(0.0)");
    require(ss2::det::detRoundToInt64(-0.0) == 0, "detRound(-0.0)");
    require(ss2::det::detRoundToInt64(0.5) == 1, "detRound(0.5)");
    require(ss2::det::detRoundToInt64(-0.5) == -1, "detRound(-0.5)");
    require(ss2::det::detRoundToInt64(2.5) == 3, "detRound(2.5)");
    require(ss2::det::detRoundToInt64(-2.5) == -3, "detRound(-2.5)");
    require(ss2::det::detRoundToInt64(std::nextafter(0.5, 0.0)) == 0, "detRound(nextafter(0.5, 0))");
    require(ss2::det::detRoundToInt64(std::nextafter(-0.5, 0.0)) == 0, "detRound(nextafter(-0.5, 0))");

    // 3. detSinCos over all 32768 scatter/combat angles + wide sweep [-200..200]
    for (int r = 0; r < 32768; ++r) {
        const double ang = (static_cast<double>(r) / 32768.0) * ss2::det::kTwoPi;
        const auto sc = ss2::det::detSinCos(ang);
        require(std::abs(sc.s - std::sin(ang)) < 1e-15, "detSinCos s error >= 1e-15");
        require(std::abs(sc.c - std::cos(ang)) < 1e-15, "detSinCos c error >= 1e-15");
        require(std::abs(sc.s * sc.s + sc.c * sc.c - 1.0) < 1e-14, "detSinCos identity error");
    }
    for (int i = -200; i <= 200; ++i) {
        const double ang = static_cast<double>(i) * 0.7312345;
        const auto sc = ss2::det::detSinCos(ang);
        require(std::abs(sc.s - std::sin(ang)) < 1e-15, "detSinCos wide s error");
        require(std::abs(sc.c - std::cos(ang)) < 1e-15, "detSinCos wide c error");
    }

    // 4. detDir8 exact match vs atan2 reference across [-50..50]^2
    for (int du = -50; du <= 50; ++du) {
        for (int dv = -50; dv <= 50; ++dv) {
            std::uint8_t ref = 0u;
            if (du != 0 || dv != 0) {
                const double angle = std::atan2(static_cast<double>(dv), static_cast<double>(du));
                const int oct = static_cast<int>(std::llround(angle / (ss2::det::kTwoPi / 8.0)));
                ref = static_cast<std::uint8_t>((oct % 8 + 8) % 8);
            }
            require(ss2::det::detDir8(du, dv) == ref, "detDir8 mismatch vs atan2");
        }
    }

    // 5. detDir32 exact match vs atan2 reference across [-40..40]^2
    for (int du = -40; du <= 40; ++du) {
        for (int dv = -40; dv <= 40; ++dv) {
            unsigned ref = 0u;
            if (du != 0 || dv != 0) {
                const double angle = std::atan2(static_cast<double>(dv), static_cast<double>(du));
                const long long step32 = std::llround(angle / (ss2::det::kTwoPi / 32.0));
                ref = static_cast<unsigned>(((step32 % 32) + 32) % 32);
            }
            require(ss2::det::detDir32(static_cast<double>(du), static_cast<double>(dv)) == ref,
                    "detDir32 mismatch vs atan2");
        }
    }

    // 6. StateHasher -0.0 and NaN canonicalization
    {
        ss2::StateHasher hp, hn;
        hp.add(0.0f);
        hn.add(-0.0f);
        require(hp.value() == hn.value(), "StateHasher float -0.0 not canonicalized");
    }
    {
        ss2::StateHasher hp, hn;
        hp.add(0.0);
        hn.add(-0.0);
        require(hp.value() == hn.value(), "StateHasher double -0.0 not canonicalized");
    }
    {
        ss2::StateHasher hf1, hf2;
        hf1.add(std::bit_cast<float>(std::uint32_t{0x7fc00001u}));
        hf2.add(std::bit_cast<float>(std::uint32_t{0xffc00007u}));
        require(hf1.value() == hf2.value(), "StateHasher float NaN payloads not canonicalized");
    }
    {
        ss2::StateHasher hd1, hd2;
        hd1.add(std::bit_cast<double>(std::uint64_t{0x7ff8000000000001ull}));
        hd2.add(std::bit_cast<double>(std::uint64_t{0xfff8000000000007ull}));
        require(hd1.value() == hd2.value(), "StateHasher double NaN payloads not canonicalized");
    }
}

std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> runCombinedArmsOnce() {
    auto tc64 = [](int tx, int ty) -> std::pair<int, int> {
        const int u = tx * 32 + 16;
        const int v = ty * 32 + 16;
        return {64 * 32 + u - v, (u + v) / 2};
    };

    std::vector<std::uint32_t> flags(64u * 64u, 0u);
    std::vector<std::uint16_t> obstacles(64u * 64u, 0x000Fu);
    // Inject a small obstacle pillar near (25, 22) to exercise kSubtleSteer vehicle detouring.
    obstacles[22u * 64u + 25u] = 0x00F0u;
    flags[21u * 64u + 27u] = 0x0800u;

    ss2::assets::UnitSpec rifle{};
    rifle.id = "rifle_det";
    rifle.selType = "guner";
    rifle.health = 100;
    rifle.ammo1 = 60;
    rifle.sight = 12;
    rifle.moveSpeed = 0.5;
    rifle.shot1 = makeWeapon("m_rif", ss2::assets::HitType::Machine, 25, 35, 190, 210, 20, 8, 5, 18, "rifle");

    ss2::assets::UnitSpec sniper{};
    sniper.id = "sniper_det";
    sniper.selType = "sniper";
    sniper.health = 90;
    sniper.ammo1 = 25;
    sniper.sight = 14;
    sniper.moveSpeed = 0.45;
    sniper.shot1 = makeWeapon("m_sni", ss2::assets::HitType::Sniper, 95, 115, 280, 320, 0, 14, 1, 14, "sniper");

    ss2::assets::UnitSpec mortar{};
    mortar.id = "mortar_det";
    mortar.selType = "mortar";
    mortar.health = 100;
    mortar.ammo1 = 30;
    mortar.sight = 11;
    mortar.shot1 = makeWeapon("m_mor", ss2::assets::HitType::Explosive, 160, 190, 360, 380, 35, 24, 1, 24, "mortar", 25, 0, 1, true, 1);

    ss2::assets::UnitSpec flamer{};
    flamer.id = "flamer_det";
    flamer.selType = "flameinf";
    flamer.health = 110;
    flamer.ammo1 = 40;
    flamer.sight = 10;
    flamer.shot1 = makeWeapon("m_flm", ss2::assets::HitType::Fire, 75, 95, 120, 130, 15, 6, 1, 6, "flamer");

    ss2::assets::UnitSpec tank{};
    tank.id = "tank_det";
    tank.isVehicle = true;
    tank.selType = "tank";
    tank.health = 650;
    tank.ammo1 = 30;
    tank.ammo2 = 500;
    tank.sight = 11;
    tank.marchSightBonus = 4;
    tank.marchEnabled = true;
    tank.crewNumber = 4;
    tank.moveSpeed = 1.4;
    tank.turnDelay = 6;
    tank.scanDelay = 20;
    tank.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {20, 12, 8};
    tank.shot1 = makeWeapon("t_ap", ss2::assets::HitType::Pierce, 140, 140, 340, 340, 20, 22, 1, 22, "tankgun");
    tank.shot2 = makeWeapon("t_mg", ss2::assets::HitType::Machine, 14, 14, 210, 210, 15, 4, 4, 14, "mg");

    ss2::assets::UnitSpec car{};
    car.id = "car_det";
    car.isVehicle = true;
    car.selType = "car";
    car.health = 320;
    car.ammo1 = 400;
    car.sight = 12;
    car.crewNumber = 2;
    car.moveSpeed = 1.8;
    car.canMoveBackward = true;
    car.backMoveSpeed = 1.0;
    car.moraleResist = 2000;
    car.moraleRndMove = -17408;
    car.scanDelay = 20;
    car.shot1 = makeWeapon("c_he", ss2::assets::HitType::Explosive, 90, 90, 280, 280, 25, 18, 1, 18, "autocannon");

    ss2::assets::UnitSpec supply{};
    supply.id = "supply_det";
    supply.isVehicle = true;
    supply.selType = "sappers";
    supply.health = 220;
    supply.ammo1 = 5000;
    supply.ammo2 = 2000;
    supply.sight = 9;
    supply.crewNumber = 2;
    supply.ammoRegenDelay = 8;
    supply.ammoRegenDelayMax = 6;

    ss2::assets::UnitSpec amb{};
    amb.id = "zis-5.med_det";
    amb.isVehicle = true;
    amb.selType = "transport";
    amb.health = 180;
    amb.sight = 9;
    amb.crewNumber = 2;
    amb.healDelay = 10;
    amb.healDelayMax = 8;

    ss2::assets::PlaneSpec bomber{};
    bomber.id = "b-17c_det";
    bomber.health = 1200;
    bomber.sight = 20;
    bomber.flySpeed = 7.0;
    bomber.bombDamage = 2400;
    bomber.bombsNumber = 4;

    ss2::assets::PlaneSpec fighter{};
    fighter.id = "spitfire_det";
    fighter.health = 500;
    fighter.sight = 22;
    fighter.flySpeed = 10.0;
    fighter.airDamage = 45;
    fighter.groundDamage = 20;
    fighter.groundAmmo = 3;

    auto cat = ss2::assets::UnitCatalog::fromSpecs(
        {rifle, sniper, mortar, flamer, tank, car, supply, amb},
        {bomber, fighter});

    const auto [p0x, p0y] = tc64(20, 22);
    const auto [p1x, p1y] = tc64(20, 23);
    const auto [p2x, p2y] = tc64(21, 22);
    const auto [p3x, p3y] = tc64(21, 23);
    const auto [v0x, v0y] = tc64(22, 22);
    const auto [v1x, v1y] = tc64(19, 24);
    const auto [v2x, v2y] = tc64(19, 25);

    const auto [e0x, e0y] = tc64(31, 22);
    const auto [e1x, e1y] = tc64(31, 23);
    const auto [e2x, e2y] = tc64(32, 22);
    const auto [ev0x, ev0y] = tc64(30, 22);

    std::vector<ss2::assets::Infantry> inf{
        makeInf("rifle_det", p0x, p0y, 90, 100, 1, 0),
        makeInf("sniper_det", p1x, p1y, 100, 100, 1, 0),
        makeInf("mortar_det", p2x, p2y, 100, 100, 1, 0),
        makeInf("flamer_det", p3x, p3y, 100, 100, 1, 0),
        makeInf("rifle_det", e0x, e0y, 100, 100, 11, 1),
        makeInf("sniper_det", e1x, e1y, 100, 100, 11, 1),
        makeInf("rifle_det", e2x, e2y, 100, 100, 11, 1),
    };
    std::vector<ss2::assets::Placement> veh{
        makeVeh("tank_det", v0x, v0y, 0, 80, 100, 1, 0),
        makeVeh("supply_det", v1x, v1y, 0, 100, 100, 1, 0),
        makeVeh("zis-5.med_det", v2x, v2y, 0, 100, 0, 1, 0),
        makeVeh("car_det", ev0x, ev0y, 16, 100, 100, 11, 1),
    };

    ss2::TutorialUnits sim(
        ss2::InfantryPathGrid::fromSubCellMasks(64, 64, flags, obstacles),
        inf, veh, ss2::FogGrid(64, 64, flags), 2, true, {}, cat);

    // Building + minefield
    const auto [bx, by] = tc64(26, 24);
    ss2::assets::BuildingPlacement bld{};
    bld.x = bx;
    bld.y = by;
    bld.left = bx - 32;
    bld.top = by - 32;
    bld.width = 64;
    bld.height = 64;
    bld.capacity = 4;
    bld.maxHp = 400;
    sim.setBuildings({std::move(bld)});
    std::vector<std::uint8_t> mines(64u * 64u, 0u);
    mines[22u * 64u + 26u] = 0xC5u;
    sim.setMines(std::move(mines));

    // Air support for both teams
    std::array<std::array<ss2::assets::AirSupportSlot, 5>, 4> air{};
    air[0][0] = {"b-17c_det", 1, 1};
    air[1][4] = {"spitfire_det", 1, 1};
    std::array<int, 4> pGrp{};
    std::array<std::vector<ss2::assets::ParatrooperEntry>, 4> pRos{};
    sim.setAirSupport(air, pGrp, pRos);

    // Issue movement, scatter, attack, and airstrike orders across multiple systems
    const auto [mvX, mvY] = tc64(28, 22);
    (void)sim.moveVehicles(std::array<size_t, 1>{0u}, mvX, mvY);
    (void)sim.orderScatter(std::array<size_t, 2>{0u, 1u});
    (void)sim.orderAttack(std::array<size_t, 1>{0u}, 4u);
    (void)sim.callAirSupport(ss2::assets::AirMissionKind::Bomber, e0x, e0y, 0);
    (void)sim.callAirSupport(ss2::assets::AirMissionKind::Fighter, p0x, p0y, 1);

    const std::uint64_t h0 = sim.stateHash();
    std::uint64_t h125 = 0u;
    for (int t = 1; t <= 250; ++t) {
        (void)sim.step();
        if (t == 125) h125 = sim.stateHash();
    }
    const std::uint64_t h250 = sim.stateHash();
    return {h0, h125, h250};
}

}  // namespace

int main() try {
    testDetMathUnits();
    const auto [h0a, h125a, h250a] = runCombinedArmsOnce();
    const auto [h0b, h125b, h250b] = runCombinedArmsOnce();
    require(h0a == h0b && h125a == h125b && h250a == h250b, "Repeated combined-arms run diverged");
    require(h0a != h125a && h125a != h250a, "Combined-arms hashes did not advance across ticks");

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << "SIM_DETERMINISM_HASHES t0=0x" << std::setw(16) << h0a
        << " t125=0x" << std::setw(16) << h125a
        << " t250=0x" << std::setw(16) << h250a;
    std::cout << oss.str() << "\nsim_determinism_test PASSED\n";
    return 0;
} catch (const std::exception& ex) {
    std::cerr << "sim_determinism_test FAILED: " << ex.what() << '\n';
    return 1;
}
