#include "assets/mission.hpp"
#include "assets/placement.hpp"
#include "assets/unit_catalog.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/tutorial.hpp"
#include "sim/tutorial_units.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace ss2;

constexpr int kCamX = -900;
constexpr int kCamY = 1150;
constexpr int kDgCtrlU = 1328, kDgCtrlV = 2128; // tile (41, 66), dy=13
constexpr int kDgSpotU = 1328, kDgSpotV = 2096; // tile (41, 65), dy=12
constexpr int kAgU = 1336, kAgV = 1720;         // tile (41, 53)
constexpr int kAgScreenX = (kAgU - kAgV) - kCamX;       // 516
constexpr int kAgScreenY = ((kAgU + kAgV) / 2) - kCamY; // 378

void testSightAndLosControls() {
    const auto dg = InfantryCombatDesc::dguner();
    const auto smg = InfantryCombatDesc::forUnitId("dsmguner");
    const auto off = InfantryCombatDesc::forUnitId("dofficer");

    // Normal difficulty (d=2)
    assert(effectiveSightRadius(smg, 0, false, 0, 2) == 11);
    assert(effectiveSightRadius(dg, 0, false, 0, 2) == 12);
    assert(effectiveSightRadius(off, 0, false, 0, 2) == 13);
    // Easy difficulty (d=1, 1.3x player bonus)
    assert(effectiveSightRadius(smg, 0, false, 0, 1) == 14);
    assert(effectiveSightRadius(dg, 0, false, 0, 1) == 15);
    assert(effectiveSightRadius(off, 0, false, 0, 1) == 16);
    // Gold CSoldier::GetSightRadius (0x100212c0): moving -1 before weather before Easy 1.3x.
    assert(effectiveSightRadius(dg, 0, false, 0, 2, true, 100) == 11);
    assert(effectiveSightRadius(off, 0, false, 0, 1, false, 50) == 7);
    assert(effectiveSightRadius(off, 0, false, 0, 1, true, 50) == 7);
    // Gold CSoldier::GetSightRadius (0x100213a7: cmp eax, 0x1f) caps sight at 31 tiles.
    auto maxSightDesc = dg;
    maxSightDesc.sightBase = 30;
    maxSightDesc.sightDelta = 0;
    assert(effectiveSightRadius(maxSightDesc, 0, false, 0, 1) == 31);
    // Crouching sight drop
    assert(effectiveSightRadius(dg, 0, true, 0, 2) == 9);
    // Fallback crouch sight follows retail crouchsight base..max: dguner/aguner
    // 9..9, dsmguner 8..9, dofficer 9..10, reached at full experience (0x8000).
    const auto ag = InfantryCombatDesc::aguner();
    assert(effectiveSightRadius(ag, 0, true, 0, 2) == 9);
    assert(effectiveSightRadius(smg, 0, true, 0, 2) == 8);
    assert(effectiveSightRadius(off, 0, true, 0, 2) == 9);
    assert(effectiveSightRadius(dg, 0x8000, true, 0, 2) == 9);
    assert(effectiveSightRadius(ag, 0x8000, true, 0, 2) == 9);
    assert(effectiveSightRadius(smg, 0x8000, true, 0, 2) == 9);
    assert(effectiveSightRadius(off, 0x8000, true, 0, 2) == 10);

    // LOS occlusion control: P-FLAGS 0x0800 at tile (41, 58) blocks (41, 62) -> (41, 53) (dy=9t)
    std::vector<std::uint32_t> flags(128 * 128, 0);
    flags[58 * 128 + 41] = 0x0800u;
    FogGrid fog(128, 128, flags);
    CombatWorld world(std::move(fog), 2, 0x12345678u);
    world.addUnit(CombatUnit{1, 0, 41 * 32 + 16, 62 * 32 + 16, 100, 40, 0, false, dg});
    world.addUnit(CombatUnit{2, 1, kAgU, kAgV, 100, 64, 80, false, InfantryCombatDesc::aguner()});
    world.refreshSpotting();
    assert(world.findUnit(2)->spottedMask == 0);
    assert(!world.isValidEnemyTarget(2, 0));
    assert(world.hitTestEnemy(kAgScreenX, kAgScreenY, kCamX, kCamY, 0) == 0);
}

std::vector<CombatTickSnapshot> runMatchedFieldsEncounter(FogGrid fog) {
    CombatWorld world(std::move(fog), 2, 0x12345678u);
    world.addUnit(CombatUnit{1, 0, kDgCtrlU, kDgCtrlV, 100, 40, 0, false, InfantryCombatDesc::dguner()});
    world.addUnit(CombatUnit{2, 1, kAgU, kAgV, 100, 64, 80, false, InfantryCombatDesc::aguner()});

    // Phase 1: Unspotted control at (41, 66) (dy=13t > sight=12)
    world.refreshSpotting();
    assert(world.findUnit(2)->spottedMask == 0x00);
    assert(!world.isValidEnemyTarget(2, 0));
    assert(world.hitTestEnemy(kAgScreenX, kAgScreenY, kCamX, kCamY, 0) == 0);

    // Phase 2: Advance 1 tile north to (41, 65) (dy=12t == sight=12)
    world.setUnitPosition(1, kDgSpotU, kDgSpotV);
    world.refreshSpotting();
    assert(world.findUnit(2)->spottedMask == 0x01);
    assert(world.isValidEnemyTarget(2, 0));
    assert(world.hitTestEnemy(kAgScreenX, kAgScreenY, kCamX, kCamY, 0) == 2);

    // Phase 3: Right-click attack order & step to kill
    assert(world.issueAttackOrder(1, 2));
    std::vector<CombatTickSnapshot> trace;
    trace.push_back(world.snapshot(-1, 1, 2));

    for (int t = 0; t < 200; ++t) {
        world.step(1);
        const auto snap = world.snapshot(t, 1, 2);
        trace.push_back(snap);
        if (snap.targetHp <= 0) break;
    }

    // Verify exact Gold 2.2 Unicorn oracle matched_fields_encounter.json parity
    assert(trace.size() == 120); // t = -1 .. 118
    assert(trace.front().tick == -1);
    assert(trace.front().orderId == 1);
    assert(trace.front().rng == 0x12345678u);

    // Tick 0: order accepted, turn North (dir=6), Shot 1 launched
    const auto& t0 = trace[1];
    assert(t0.tick == 0);
    assert(t0.orderId == 3 && t0.orderQueue == 0xffffu);
    assert(t0.direction == 6 && t0.actionState == 1 && t0.animTicks == 0);
    assert(t0.reload == 38 && t0.burst == 1 && t0.ammo == 39 && t0.expa == 7);
    assert(t0.targetHp == 100 && t0.rng == 0x6f47bdb6u);

    // Tick 4: Shot 1 impact -> roll=19 <= 75 HIT -> HP 100->51
    const auto& t4 = trace[5];
    assert(t4.tick == 4 && t4.targetHp == 51 && t4.rng == 0x99946224u);

    // Tick 18: Fire anim finishes (act 1->0, anim=0, rld=20)
    const auto& t18 = trace[19];
    assert(t18.tick == 18 && t18.actionState == 0 && t18.animTicks == 0 && t18.reload == 20);

    // Tick 38: Shot 2 launched (ammo=38, expa=13)
    const auto& t38 = trace[39];
    assert(t38.tick == 38 && t38.actionState == 1 && t38.burst == 2 && t38.ammo == 38 && t38.expa == 13);

    // Tick 42: Shot 2 impact -> roll=83 > 75 MISS -> HP stays 51
    const auto& t42 = trace[43];
    assert(t42.tick == 42 && t42.targetHp == 51 && t42.rng == 0x6ac77c90u);

    // Tick 76: Shot 3 launched (ammo=37, expa=19)
    const auto& t76 = trace[77];
    assert(t76.tick == 76 && t76.burst == 3 && t76.ammo == 37 && t76.expa == 19);

    // Tick 80: Shot 3 impact -> roll=37 <= 75 HIT -> HP 51->2 + hurt coin flip
    const auto& t80 = trace[81];
    assert(t80.tick == 80 && t80.targetHp == 2 && t80.rng == 0x20cbc545u);

    // Tick 114: Shot 4 launched (ammo=36, expa=25)
    const auto& t114 = trace[115];
    assert(t114.tick == 114 && t114.burst == 4 && t114.ammo == 36 && t114.expa == 25);

    // Tick 118: Shot 4 impact -> roll=75 <= 75 HIT -> HP 2->0, final rng=0xe06870c1
    const auto& t118 = trace.back();
    assert(t118.tick == 118);
    assert(t118.ammo == 36 && t118.expa == 25);
    assert(t118.targetSpotted == 0x01 && t118.targetHp == 0);
    assert(t118.rng == 0xe06870c1u);

    // Verify all 21 discrete events & exact impact coordinates
    const auto& ev = world.events();
    assert(ev.size() == 21);
    assert(ev[4].kind == "PROJ_IMPACT" && ev[4].tick == 4 && ev[4].a == 1338 && ev[4].b == 1723);
    assert(ev[9].kind == "PROJ_IMPACT" && ev[9].tick == 42 && ev[9].a == 1339 && ev[9].b == 1719);
    assert(ev[13].kind == "PROJ_IMPACT" && ev[13].tick == 80 && ev[13].a == 1336 && ev[13].b == 1720);
    assert(ev[18].kind == "PROJ_IMPACT" && ev[18].tick == 118 && ev[18].a == 1339 && ev[18].b == 1719);

    return trace;
}

void testTutorialUnitsCombatLoop() {
    // Synthetic open map: player dguner at (41,65) U=1328,V=2096 -> map(3328, 1712)
    // Enemy Group-3 aguner at (41,53) U=1336,V=1720 -> map(3712, 1528)
    auto grid = InfantryPathGrid::fromWalkable(256, 256, std::vector<std::uint8_t>(256 * 256, 1));
    FogGrid fog(128, 128, std::vector<std::uint32_t>(128 * 128, 0));

    std::vector<assets::Infantry> units;
    assets::Infantry dg{};
    dg.id = "dguner";
    dg.x = 4096 + kDgSpotU - kDgSpotV;
    dg.y = (kDgSpotU + kDgSpotV) / 2;
    dg.hp = 100;
    dg.ammo = 40;
    dg.group = 0;
    dg.owner = 0;
    units.push_back(dg);

    assets::Infantry ag{};
    ag.id = "aguner";
    ag.x = 4096 + kAgU - kAgV;
    ag.y = (kAgU + kAgV) / 2;
    ag.hp = 100;
    ag.ammo = 64;
    ag.group = 3;
    ag.owner = 1;
    units.push_back(ag);

    TutorialUnits sim(std::move(grid), units, {}, std::move(fog), 2);
    assert(sim.countAliveInGroup(1, 3) == 1);
    assert(sim.isValidAttackTarget(1, 0));
    const std::array<size_t, 1> sel{0};
    assert(sim.orderAttack(sel, 1) == 1);

    int killTick = -1;
    for (int t = 0; t < 200; ++t) {
        const auto poses = sim.step();
        if (sim.countAliveInGroup(1, 3) == 0) {
            killTick = t;
            bool sawDeadPose = false;
            for (const auto& p : poses) {
                if (p.index == 1 && !p.alive && p.hp == 0) sawDeadPose = true;
            }
            assert(sawDeadPose);
            break;
        }
    }
    assert(killTick == 118);
    assert(!sim.isValidAttackTarget(1, 0));

    // Verify two-sided auto-engage & return fire (autoEngage = true)
    auto grid2 = InfantryPathGrid::fromWalkable(256, 256, std::vector<std::uint8_t>(256 * 256, 1));
    FogGrid fog2(128, 128, std::vector<std::uint32_t>(128 * 128, 0));
    TutorialUnits duel(std::move(grid2), units, {}, std::move(fog2), 2, true);
    bool playerDamaged = false;
    bool enemyKilled = false;
    for (int t = 0; t < 250; ++t) {
        for (const auto& p : duel.step()) {
            if (p.index == 0 && p.hp < 100) playerDamaged = true;
            if (p.index == 1 && !p.alive) enemyKilled = true;
        }
        if (playerDamaged && enemyKilled) break;
    }
    assert(playerDamaged && enemyKilled);

    // Verify marching squad pauses to engage spotted enemy, survives, and resumes march (repro for #247)
    auto grid3 = InfantryPathGrid::fromWalkable(256, 256, std::vector<std::uint8_t>(256 * 256, 1));
    FogGrid fog3(128, 128, std::vector<std::uint32_t>(128 * 256 / 2, 0));
    std::vector<assets::Infantry> squad;
    for (int k = 0; k < 6; ++k) {
        assets::Infantry s = dg;
        const int u = kDgCtrlU + (k % 3) * 32;
        const int v = kDgCtrlV + 96 + (k / 3) * 32; // start 3 tiles further south, outside sight
        s.x = 4096 + u - v;
        s.y = (u + v) / 2;
        squad.push_back(s);
    }
    squad.push_back(ag); // index 6 = stationary enemy at (41, 53)
    TutorialUnits march(std::move(grid3), squad, {}, std::move(fog3), 2, true);
    const std::array<size_t, 6> squadIds{0, 1, 2, 3, 4, 5};
    // Unspotted enemy outside sight must NOT be a valid attack target (closes #281 gap a).
    assert(!march.isValidAttackTarget(6, 0));
    assert(march.orderAttack(squadIds, 6) == 0);
    const int destX = 4096 + kAgU - (kAgV - 64);
    const int destY = (kAgU + (kAgV - 64)) / 2;
    assert(march.orderSelection(squadIds, destX, destY) == 6);
    for (int t = 0; t < 600; ++t) {
        march.step();
        if (march.countAliveInGroup(1, 3) == 0) break;
    }
    assert(march.countAliveInGroup(1, 3) == 0);
    assert(march.countAliveInGroup(0, 0) >= 5);

    // Phase 11 tactical controls: Stop, Hold-Fire, and Crouch sight reduction (12t -> 9t)
    auto grid4 = InfantryPathGrid::fromWalkable(256, 256, std::vector<std::uint8_t>(256 * 256, 1));
    FogGrid fog4(128, 128, std::vector<std::uint32_t>(128 * 128, 0));
    TutorialUnits tac(std::move(grid4), units, {}, std::move(fog4), 2, true);
    assert(tac.isValidAttackTarget(1, 0));                  // standing dy=12t <= sight 12
    assert(tac.toggleCrouch(sel) == true);                  // crouch drops sight to 9t
    assert(!tac.isValidAttackTarget(1, 0));                 // enemy at dy=12t now unspotted
    assert(tac.toggleCrouch(sel) == false);                 // stand back up -> sight 12t
    assert(tac.isValidAttackTarget(1, 0));
    assert(tac.toggleHoldFire(sel) == true);                // hold fire suppresses auto-engage
    assert(tac.stopSelection(sel) == 1);
    for (int t = 0; t < 50; ++t) tac.step();
    assert(tac.countAliveInGroup(1, 3) == 1);               // enemy untouched while holding fire
}
// The hardcoded fallback descriptors must agree with the retail catalog on sight.
void testFallbackMatchesRetailCatalog(const std::filesystem::path& gameDir) {
    const auto catalog = ss2::assets::UnitCatalog::load(gameDir);
    assert(catalog);
    for (const char* id : {"dguner", "aguner", "dsmguner", "rsmguner", "dofficer", "aofficer"}) {
        const auto* spec = catalog->find(id);
        assert(spec && spec->sight > 0 && spec->crouchSight > 0);
        const auto d = InfantryCombatDesc::forUnitId(id);
        assert(d.sightBase == spec->sight);
        assert(d.crouchSightBase == spec->crouchSight);
        assert(d.crouchSightDelta == spec->crouchSightMax - spec->crouchSight);
    }
}

// Same-map comparison with the retail oracle (verify_group3_spotting.py: real
// n2Game_Dll.dll, real Tutorial I P-FLAGS and CIRC64.DAT, Normal difficulty).
// Group-3 aguner at tile (41,53); FIRST_SPOTTED rows from the retail run.
bool spotsAguner(const ss2::assets::TutorialData& tutorial, const std::filesystem::path& gameDir,
                 const InfantryCombatDesc& observer, int tileX, int tileY) {
    FogGrid fog(128, 128, tutorial.flags);
    assert(fog.loadCirc64FromGame(gameDir));
    CombatWorld world(std::move(fog), 2, 0x12345678u);
    world.addUnit(CombatUnit{1, 0, tileX * 32 + 16, tileY * 32 + 16, 100, 40, 0, false, observer});
    world.addUnit(CombatUnit{2, 1, kAgU, kAgV, 100, 64, 80, false, InfantryCombatDesc::aguner()});
    world.refreshSpotting();
    const bool spotted = (world.findUnit(2)->spottedMask & 1u) != 0;
    assert(spotted == world.isValidEnemyTarget(2, 0));
    return spotted;
}

void testRetailSpottingRows(const ss2::assets::TutorialData& tutorial, const std::filesystem::path& gameDir) {
    // South approach along x=41: first spotted at dy = own sight, unspotted one tile further.
    const struct { const char* id; int dy; } rows[] = {{"dsmguner", 11}, {"dguner", 12}, {"dofficer", 13}};
    for (const auto& row : rows) {
        const auto desc = InfantryCombatDesc::forUnitId(row.id);
        assert(spotsAguner(tutorial, gameDir, desc, 41, 53 + row.dy));
        assert(!spotsAguner(tutorial, gameDir, desc, 41, 53 + row.dy + 1));
    }
    // Diagonal SE approach for dguner: first spotted at dx=dy=8 (11.3t), not at 9.
    // Off-axis, so this row depends on the real CIRC64 ring, not the fallback.
    const auto dg = InfantryCombatDesc::dguner();
    assert(spotsAguner(tutorial, gameDir, dg, 41 + 8, 53 + 8));
    assert(!spotsAguner(tutorial, gameDir, dg, 41 + 9, 53 + 9));
}
} // namespace

int main(int argc, char** argv) {
    testSightAndLosControls();
    testTutorialUnitsCombatLoop();

    if (argc > 1) {
        const std::filesystem::path gameDir(argv[1]);
        const auto tutorial = ss2::assets::readTutorialData(gameDir / "maps/single/--- TUTORIAL I ---.SSM");
        testFallbackMatchesRetailCatalog(gameDir);
        testRetailSpottingRows(tutorial, gameDir);
        FogGrid fog1(128, 128, tutorial.flags);
        assert(fog1.loadCirc64FromGame(gameDir));
        const auto run1 = runMatchedFieldsEncounter(std::move(fog1));

        FogGrid fog2(128, 128, tutorial.flags);
        assert(fog2.loadCirc64FromGame(gameDir));
        const auto run2 = runMatchedFieldsEncounter(std::move(fog2));
        assert(run1.size() == run2.size());
        for (std::size_t i = 0; i < run1.size(); ++i) {
            assert(run1[i].rng == run2[i].rng && run1[i].targetHp == run2[i].targetHp &&
                   run1[i].animTicks == run2[i].animTicks && run1[i].reload == run2[i].reload);
        }

        // Verify trigger[12..13] Phrase 5 Group-3 elimination progression
        Tutorial vm(tutorial, 0, 5);
        (void)vm.takeEvents();
        bool sawPhrase6 = false;
        bool sawPhrase7 = false;
        bool sawCamera4 = false;
        for (int t = 0; t < 300; ++t) {
            vm.step(0, t < 10 ? 5u : 0u);
            for (const auto& ev : vm.takeEvents()) {
                if (ev.kind == MissionEvent::Message && ev.index == 6) sawPhrase6 = true;
                if (ev.kind == MissionEvent::Dialog && ev.index == 7) sawPhrase7 = true;
                if (ev.kind == MissionEvent::Camera && ev.index == 4) sawCamera4 = true;
            }
        }
        assert(sawPhrase6 && sawPhrase7 && sawCamera4);

        // Verify full 30-trigger Tutorial I VM reaches OP(18) Mission Victory via trigger[28..29]
        bool enteredZone9 = false;
        MissionQueries fullQ{
            [&](unsigned zone, unsigned player) -> unsigned {
                return (zone == 9 && player == 0 && enteredZone9) ? 1u : 0u;
            },
            [](unsigned group) -> unsigned {
                return group == 30 ? 29u : 5u;
            }
        };
        Tutorial fullVm(tutorial, fullQ);
        (void)fullVm.takeEvents();
        bool sawPhrase18 = false;
        bool sawVictory = false;
        for (int t = 0; t < 400; ++t) {
            if (t == 10) enteredZone9 = true;
            fullVm.step(fullQ);
            for (const auto& ev : fullVm.takeEvents()) {
                if (ev.kind == MissionEvent::Dialog && ev.index == 18) sawPhrase18 = true;
                if (ev.kind == MissionEvent::Victory) sawVictory = true;
            }
        }
        assert(sawPhrase18 && sawVictory);

        std::cout << "gold_infantry_combat: PASS (real Tutorial I P-FLAGS + circ64.dat, catalog sight parity, retail spotting rows 4/4, 120 ticks, 21 events, final_rng=0xe06870c1, trigger[12..29] Victory OK)\n";
        return 0;
    }

    FogGrid open1(128, 128, std::vector<std::uint32_t>(128 * 128, 0));
    const auto run1 = runMatchedFieldsEncounter(std::move(open1));
    FogGrid open2(128, 128, std::vector<std::uint32_t>(128 * 128, 0));
    const auto run2 = runMatchedFieldsEncounter(std::move(open2));
    assert(run1.size() == run2.size() && run1.back().rng == run2.back().rng);
    std::cout << "infantry_combat: PASS (standalone 120 ticks, 21 events, final_rng=0xe06870c1)\n";
    return 0;
}
