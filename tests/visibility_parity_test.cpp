#include "assets/infantry.hpp"
#include "assets/mission.hpp"
#include "assets/placement.hpp"
#include "assets/terrain.hpp"
#include "assets/unit_catalog.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/tutorial_coordinates.hpp"
#include "sim/tutorial_units.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool cond, const std::string& msg) {
    if (!cond) throw std::runtime_error(msg);
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
}

std::vector<std::uint8_t> zlibCompress(const std::vector<std::uint8_t>& src) {
    uLongf dstLen = compressBound(static_cast<uLong>(src.size()));
    std::vector<std::uint8_t> dst(dstLen);
    if (compress2(dst.data(), &dstLen, src.data(), static_cast<uLong>(src.size()), Z_BEST_SPEED) != Z_OK) {
        throw std::runtime_error("zlib compress2 failed");
    }
    dst.resize(dstLen);
    return dst;
}

void writeSyntheticGameCommonSue(const std::filesystem::path& path) {
    // Minimal FZFS v1 SUE with uncompressed (kind=2) SOLD.PL and dguner.rs2.
    std::vector<std::uint8_t> soldPl(512, 0);
    soldPl[2] = 0x00;
    soldPl[3] = 0xf8; // palette index 1 = pure red (RGB565 0xf800 -> RGBA 255,0,0,255)

    // Single 4x4 opaque 0xa1 sprite at offset (-2, -4) from unit foot.
    std::vector<std::uint8_t> frame;
    appendU16(frame, static_cast<std::uint16_t>(static_cast<std::int16_t>(-2)));
    appendU16(frame, static_cast<std::uint16_t>(static_cast<std::int16_t>(-4)));
    appendU16(frame, 4);
    appendU16(frame, 4);
    frame.push_back(0xa1u);
    for (int row = 0; row < 4; ++row) {
        appendU16(frame, 2);
        frame.push_back(0x84u); // compact repeat x4
        frame.push_back(0x01u); // palette index 1
    }

    std::vector<std::uint8_t> rs2;
    for (unsigned grp = 0; grp < 40; ++grp) {
        if (grp < 8u) {
            appendU32(rs2, 1u);
            appendU32(rs2, static_cast<std::uint32_t>(4u + frame.size()));
            appendU32(rs2, 4u);
            rs2.insert(rs2.end(), frame.begin(), frame.end());
        } else {
            appendU32(rs2, 0u);
            appendU32(rs2, 0u);
        }
    }

    struct Entry {
        std::string name;
        std::vector<std::uint8_t> data;
        std::uint32_t offset{};
    };
    std::vector<Entry> entries{{"SOLD.PL", std::move(soldPl)}, {"DGUNER.RS2", std::move(rs2)}};

    std::vector<std::uint8_t> dataSec;
    for (auto& e : entries) {
        e.offset = static_cast<std::uint32_t>(8u + dataSec.size());
        dataSec.insert(dataSec.end(), e.data.begin(), e.data.end());
    }

    std::vector<std::uint8_t> items;
    std::vector<std::uint8_t> names;
    for (const auto& e : entries) {
        const std::uint32_t nameOff = static_cast<std::uint32_t>(names.size());
        names.insert(names.end(), e.name.begin(), e.name.end());
        names.push_back(0);
        appendU32(items, nameOff);
        appendU32(items, 2u); // kind=2 uncompressed
        appendU32(items, e.offset);
        appendU32(items, static_cast<std::uint32_t>(e.data.size()));
        appendU32(items, 0u);
    }
    const std::vector<std::uint8_t> offsets(4, 0);

    const auto writeTable = [](std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& raw,
                               std::uint32_t count, std::uint32_t width) {
        const auto packed = zlibCompress(raw);
        appendU32(out, static_cast<std::uint32_t>(packed.size()));
        appendU32(out, count);
        appendU32(out, width);
        out.insert(out.end(), packed.begin(), packed.end());
    };

    std::vector<std::uint8_t> archive{'F', 'Z', 'F', 'S'};
    appendU32(archive, static_cast<std::uint32_t>(dataSec.size()));
    archive.insert(archive.end(), dataSec.begin(), dataSec.end());
    writeTable(archive, items, static_cast<std::uint32_t>(entries.size()), 20u);
    writeTable(archive, names, static_cast<std::uint32_t>(names.size()), 1u);
    writeTable(archive, offsets, 1u, 4u);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
    require(bool(out), "Failed to write synthetic SUE archive");
}

void testSightMatrixParity() {
    // Exact Gold 2.2 CSoldier::GetSightRadius (0x100212c0) table across base x moving x difficulty x weather.
    const std::array<int, 4> weatherPcts{100, 70, 70, 50};
    for (int base : {11, 12, 13}) {
        ss2::InfantryCombatDesc desc = ss2::InfantryCombatDesc::dguner();
        desc.sightBase = base;
        desc.sightDelta = 0;
        for (bool moving : {false, true}) {
            for (int diff : {1, 2}) {
                for (int visPct : weatherPcts) {
                    const int afterMove = (moving && base > 1) ? (base - 1) : base;
                    const int afterWeather = visPct >= 100 ? afterMove : std::max(1, (afterMove * visPct) / 100);
                    const int expected = std::clamp(diff == 1 ? (afterWeather * 13) / 10 : afterWeather, 1, 31);
                    const int got = ss2::effectiveSightRadius(desc, 0, false, 0, diff, moving, visPct);
                    require(got == expected,
                            "effectiveSightRadius mismatch at base=" + std::to_string(base) +
                                " moving=" + std::to_string(moving) + " diff=" + std::to_string(diff) +
                                " visPct=" + std::to_string(visPct));
                }
            }
        }
    }
    // Pin weather-before-Easy ordering: officer (13), Easy (1.3x), Snow (50%): floor(13*50/100)=6 -> floor(6*1.3)=7.
    const auto off = ss2::InfantryCombatDesc::forUnitId("dofficer");
    require(ss2::effectiveSightRadius(off, 0, false, 0, 1, false, 50) == 7,
            "Officer Easy Snow sight must be 7 (weather truncation before 1.3x Easy bonus)");
}

void testPerimeterFlags() {
    std::vector<std::uint32_t> flags(16u * 16u, 0x2000u); // start elevated everywhere
    ss2::FogGrid fog(16, 16, flags);
    fog.applyMapPerimeterFlags();
    // Tile (10, 2) is in outer 3-tile border -> 0x0800 blocks LOS across tile y=2.
    // Tile (10, 3) is ring 3 -> 0x2000 cleared, not elevated.
    require(!fog.isElevated(10, 3), "applyMapPerimeterFlags must clear 0x2000 on 4-tile border");
    require(fog.isElevated(10, 4), "applyMapPerimeterFlags must leave interior tiles elevated");

    fog.clearAll();
    fog.castRay(8, 5, 0, -5, 0x11u);
    require((fog.tile(8, 2) & 0x01u) != 0u, "Ray must mark blocking border tile (8, 2) visible");
    require((fog.tile(8, 1) & 0x01u) == 0u, "Ray must stop behind 0x0800 border tile (8, 2)");
}

void testMovingSightPenaltyLive() {
    auto grid = ss2::InfantryPathGrid::fromWalkable(80, 80, std::vector<std::uint8_t>(80 * 80, 1));
    ss2::FogGrid fog(40, 40, std::vector<std::uint32_t>(40 * 40, 0));
    const auto subPos = [](int su, int sv) {
        return std::pair<int, int>{40 * 32 + su - sv, (su + sv) / 2};
    };
    const auto tileCenter = [&](int tx, int ty) {
        return subPos(tx * 32 + 16, ty * 32 + 16);
    };
    // Start in slot (0,0) of tile (20, 32); moving East steps to slot (1,0) inside the SAME tile (20, 32).
    const auto [px, py] = subPos(20 * 32 + 8, 32 * 32 + 8);
    const auto [ex, ey] = tileCenter(20, 20); // dy = 12 tiles -> spotted stationary (12), unspotted moving (11)
    ss2::assets::Infantry p0{};
    p0.id = "dguner"; p0.x = px; p0.y = py; p0.hp = 100; p0.ammo = 40; p0.group = 0; p0.owner = 0;
    ss2::assets::Infantry e0{};
    e0.id = "aguner"; e0.x = ex; e0.y = ey; e0.hp = 100; e0.ammo = 64; e0.group = 3; e0.owner = 1;
    std::vector<ss2::assets::Infantry> units{p0, e0};

    ss2::TutorialUnits sim(std::move(grid), units, {}, std::move(fog), 2, false);
    require(sim.isValidAttackTarget(1, 0), "Stationary dguner must spot enemy at dy=12");

    // Order soldier East to tile (21, 32). Tick 1 plans route; Tick 2 enters sub-step (1,0) still in tile (20, 32).
    const auto [destX, destY] = tileCenter(21, 32);
    const std::array<std::size_t, 1> sel{0};
    require(sim.orderSelection(sel, destX, destY) == 1u, "East move order must be accepted");
    (void)sim.step();
    (void)sim.step();
    const auto snapMoving = sim.takeSnapshot();
    require(snapMoving.soldiers[0].cellX / 2 == 20 && snapMoving.soldiers[0].cellY / 2 == 32,
            "Soldier must still occupy tile (20, 32) on first East sub-step");
    require(!sim.isValidAttackTarget(1, 0),
            "Moving soldier (-1 sight penalty: 12 -> 11) in same tile must immediately lose dy=12 spot");

    require(sim.stopSelection(sel) == 1u, "stopSelection must accept moving soldier");
    for (int t = 0; t < 40 && !sim.isValidAttackTarget(1, 0); ++t) {
        (void)sim.step();
    }
    const auto snapStopped = sim.takeSnapshot();
    require(snapStopped.soldiers[0].cellX / 2 == 20 && snapStopped.soldiers[0].cellY / 2 == 32,
            "Stopped soldier should finish its sub-step inside tile (20, 32)");
    require(sim.isValidAttackTarget(1, 0), "Stopped soldier must regain full stationary dy=12 spot");
}

void testRendered1xFogAndVisibility(const char* runTag) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmpDir = std::filesystem::temp_directory_path() /
                        ("ss2_vis_parity_1x_" + std::string(runTag) + "_" + std::to_string(nonce));
    std::filesystem::remove_all(tmpDir);
    std::filesystem::create_directories(tmpDir);
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } cleanup{tmpDir};
    writeSyntheticGameCommonSue(tmpDir / "game_common.sue");

    constexpr unsigned kFogW = 8, kFogH = 8;
    constexpr int kOriginX = int(kFogH) * 32; // 256
    constexpr unsigned kMapW = (kFogW + kFogH) * 32u; // 512
    constexpr unsigned kMapH = (kFogW + kFogH) * 16u; // 256
    ss2::assets::MapImage bg{kMapW, kMapH, std::vector<std::uint8_t>(std::size_t(kMapW) * kMapH * 4u, 255)};
    for (std::size_t i = 0; i < bg.rgba.size(); i += 4u) {
        bg.rgba[i + 0] = 200;
        bg.rgba[i + 1] = 200;
        bg.rgba[i + 2] = 200;
    }

    // Place enemy dguner at isometric center of tile (tx=2, ty=3): mx = 256 + (2-3)*32 = 224 (negative d = -32!), my = (2+3)*16+16 = 96.
    const int enemyTx = 2, enemyTy = 3;
    const int enemyMx = kOriginX + (enemyTx - enemyTy) * 32; // 224 < originX
    const int enemyMy = (enemyTx + enemyTy) * 16 + 16;       // 96
    const std::vector<ss2::assets::Placement> placements{
        {"dguner", enemyMx, enemyMy, 0, 100, 40, 3, 1},
    };
    ss2::assets::InfantryScene scene(bg, tmpDir, placements, /*originalSize=*/true, {}, {}, false, {}, nullptr, kFogW, kFogH);

    // Initially hide unspotted enemy, apply all-shroud fog, verify floor-shift negative d tiles.
    (void)scene.update(0, enemyMx, enemyMy, 0, 0, false, 100, 0);
    std::vector<std::uint8_t> fog(kFogW * kFogH, 0u);
    fog[2u * kFogW + 3u] = 0x10u; // tile (3, 2): mx = 288, my = 96 -> explored only (scale 178)
    fog[3u * kFogW + 4u] = 0x01u; // tile (4, 3): mx = 288, my = 128 -> visible (scale 256)
    (void)scene.updateFog(fog);
    auto img = scene.compose();
    require(img.width == kMapW && img.height == kMapH, "1x scene resolution must match terrain pixels 1:1");

    const auto pxAt = [&](int mx, int my) -> std::array<std::uint8_t, 3> {
        const std::size_t idx = (std::size_t(my) * img.width + static_cast<std::size_t>(mx)) * 4u;
        return {img.rgba[idx], img.rgba[idx + 1], img.rgba[idx + 2]};
    };
    // Visible tile (4, 3): ground stays 200
    require((pxAt(288, 128) == std::array<std::uint8_t, 3>{200, 200, 200}), "Visible tile at 1x darkened unexpectedly");
    // Explored fog tile (3, 2): floor((200 * 178) >> 8) = 139
    require((pxAt(288, 96) == std::array<std::uint8_t, 3>{139, 139, 139}), "Explored fog brightness mismatch at 1x");
    // Negative-delta unexplored shroud at enemy tile (2, 3), including odd mx=255 boundary (halfX = -1): (200 * 112) >> 8 = 87
    require((pxAt(enemyMx, enemyMy) == std::array<std::uint8_t, 3>{87, 87, 87}), "Unexplored shroud mismatch on mx < originX");
    require((pxAt(224, 94) == std::array<std::uint8_t, 3>{87, 87, 87}), "Unspotted enemy rendered under unexplored shroud");

    // Now mark tile (2, 3) visible and reveal enemy (visible=1): sprite pixel at (224, 94) turns red!
    fog[static_cast<std::size_t>(enemyTy) * kFogW + static_cast<std::size_t>(enemyTx)] = 0x11u;
    (void)scene.updateFog(fog);
    (void)scene.update(0, enemyMx, enemyMy, 0, 0, false, 100, 1);
    img = scene.compose();
    require((pxAt(224, 94) == std::array<std::uint8_t, 3>{255, 0, 0}),
            "Spotted enemy sprite did not appear at 1x zoom on visible tile");
    // Ground outside sprite inside the visible tile is bright 200
    require((pxAt(224, 97) == std::array<std::uint8_t, 3>{200, 200, 200}),
            "Visible ground around revealed enemy was not unshaded");
}

struct CheckpointExpectation {
    int tick;
    int vis;
    int exp;
    std::uint32_t crc;
};

void testGoldControlledVisibility(const std::filesystem::path& gameDir) {
    const auto missionPath = gameDir / "maps/single/--- TUTORIAL I ---.SSM";
    const auto data = ss2::assets::readTutorialData(missionPath);
    const auto allPlacements = ss2::assets::readTutorialPlacements(gameDir);
    const auto catalog = ss2::assets::UnitCatalog::load(gameDir);

    std::vector<ss2::assets::Infantry> infantry;
    std::vector<ss2::assets::Placement> vehicles;
    std::vector<std::size_t> placementIndexByInfantry;
    std::vector<std::size_t> squad;
    std::vector<std::size_t> group2Infantry;

    for (std::size_t i = 0; i < allPlacements.size(); ++i) {
        const auto& p = allPlacements[i];
        if (p.id == "zis-3" || p.id == "m3a1" || p.id == "ds39" || p.id == "panther") {
            vehicles.push_back(p);
        } else {
            const std::size_t infIdx = infantry.size();
            infantry.push_back({p.id, p.x, p.y, p.hp, p.ammo, p.group, p.owner, 0, 0, 0, 0, unsigned(p.direction)});
            infantry.back().expa = p.expa;
            placementIndexByInfantry.push_back(i);
            if (p.owner == 0) squad.push_back(infIdx);
            if (p.owner == 1 && p.group == 2) group2Infantry.push_back(infIdx);
        }
    }
    require(squad.size() == 29u, "Tutorial I player squad must have 29 infantry");
    require(group2Infantry.size() == 4u, "Tutorial I Group 2 must have 4 aguner enemies");

    ss2::FogGrid fog(data.width, data.height, data.flags);
    fog.applyMapPerimeterFlags();
    require(fog.loadCirc64FromGame(gameDir), "CIRC64.DAT must load from retail archive");

    ss2::TutorialUnits sim(ss2::InfantryPathGrid::loadTutorial(gameDir), infantry, vehicles,
                           std::move(fog), /*difficulty=*/2, /*autoEngage=*/false, {}, catalog);
    sim.setPlayerTeams(data.playerTeams);

    // Construct real 1x-zoom InfantryScene for controlled visual verification around Group 2.
    std::vector<ss2::assets::Placement> infPlacements;
    infPlacements.reserve(infantry.size());
    for (std::size_t idx : placementIndexByInfantry) infPlacements.push_back(allPlacements[idx]);
    ss2::assets::InfantryScene scene(ss2::assets::loadTutorialTerrain(gameDir, 0), gameDir,
                                     infPlacements, /*originalSize=*/true);
    for (std::size_t i = 0; i < infantry.size(); ++i) {
        if (infantry[i].owner != 0) {
            (void)scene.update(i, infantry[i].x, infantry[i].y, infantry[i].direction, 0, false,
                               infantry[i].hp, 0, infantry[i].ammo);
        }
    }

    auto fogStats = [](const std::vector<std::uint8_t>& grid) {
        std::vector<std::uint8_t> plane(grid.size());
        int vis = 0, exp = 0;
        for (std::size_t i = 0; i < grid.size(); ++i) {
            const std::uint8_t v = grid[i] & 0x11u;
            plane[i] = v;
            if (v & 0x01u) ++vis;
            if (v & 0x10u) ++exp;
        }
        const std::uint32_t crc = static_cast<std::uint32_t>(
            crc32_z(0u, plane.data(), plane.size()));
        return std::tuple<int, int, std::uint32_t>{vis, exp, crc};
    };

    // Gold 2.2 Unicorn oracle expected values (verified tick-by-tick against n2Game_Dll.dll).
    static constexpr std::array<CheckpointExpectation, 11> kExpected{{
        {0,    570,  570, 0x7377abd5u},
        {500,  540,  771, 0xdb09fda1u},
        {1000, 565,  990, 0xeca59facu},
        {1200, 554, 1080, 0x56a700c9u},
        {1400, 547, 1167, 0xd6ae36cdu},
        {1600, 539, 1245, 0xa2ad28f5u},
        {1800, 601, 1340, 0xd60e2a47u},
        {2000, 582, 1413, 0x754b0056u},
        {2200, 576, 1485, 0x12078aa5u},
        {2600, 566, 1639, 0x72f185c6u},
        {3200, 581, 1639, 0xb8523a55u},
    }};
    static constexpr int kExpectedGroup2SpotTick = 1414;

    auto initialFog = sim.takeFogSnapshot();
    require(initialFog.has_value(), "Initial fog snapshot missing");
    std::vector<std::uint8_t> lastFog = *initialFog;
    (void)scene.updateFog(lastFog);

    const auto checkCheckpoint = [&](int tick) {
        const auto [vis, exp, crc] = fogStats(lastFog);
        std::cout << "NATIVE_VIS_CHECKPOINT tick=" << tick << " vis=" << vis << " exp=" << exp
                  << " crc=0x" << std::hex << crc << std::dec << '\n';
        for (const auto& ck : kExpected) {
            if (ck.tick == tick) {
                require(vis == ck.vis && exp == ck.exp && crc == ck.crc,
                        "Gold visibility parity mismatch at tick " + std::to_string(tick) +
                            " (vis=" + std::to_string(vis) + " exp=" + std::to_string(exp) + ")");
                return;
            }
        }
    };

    checkCheckpoint(0);
    for (std::size_t idx : group2Infantry) {
        require(!sim.isValidAttackTarget(idx, 0), "Group-2 enemy must start unspotted");
    }

    require(sim.orderSelection(squad, 2960, 1776) == squad.size(),
            "Tutorial I wreck march order did not accept all 29 soldiers");

    auto livePositions = infantry;
    std::uint32_t posCrc = 0;
    std::map<std::size_t, int> firstSpotTick;
    ss2::assets::MapImage beforeSpotCrop;
    ss2::assets::MapImage afterSpotCrop;

    // Crop centered around Group-2 placement #14 (infantry index group2Infantry[0]).
    const int g2x = infantry[group2Infantry[0]].x;
    const int g2y = infantry[group2Infantry[0]].y;
    const int cropL = std::max(0, g2x - 24);
    const int cropT = std::max(0, g2y - 36);
    const int cropR = cropL + 48;
    const int cropB = cropT + 48;

    for (int tick = 1; tick <= 3200; ++tick) {
        const auto poses = sim.step();
        if (auto f = sim.takeFogSnapshot()) {
            lastFog = std::move(*f);
            if (tick >= kExpectedGroup2SpotTick - 1) {
                (void)scene.updateFog(lastFog);
            }
        }
        for (const auto& p : poses) {
            livePositions[p.index].x = p.x;
            livePositions[p.index].y = p.y;
            if (tick >= kExpectedGroup2SpotTick - 1) {
                (void)scene.update(p.index, p.x, p.y, p.direction, p.progress, p.walking, p.hp,
                                   p.visible ? 1 : 0, p.ammo);
            }
        }

        std::array<std::uint8_t, 29 * 4> posBuf{};
        for (std::size_t k = 0; k < squad.size(); ++k) {
            const auto& u = livePositions[squad[k]];
            posBuf[k * 4 + 0] = static_cast<std::uint8_t>(u.x & 0xff);
            posBuf[k * 4 + 1] = static_cast<std::uint8_t>((u.x >> 8) & 0xff);
            posBuf[k * 4 + 2] = static_cast<std::uint8_t>(u.y & 0xff);
            posBuf[k * 4 + 3] = static_cast<std::uint8_t>((u.y >> 8) & 0xff);
        }
        posCrc = static_cast<std::uint32_t>(crc32_z(posCrc, posBuf.data(), posBuf.size()));

        for (std::size_t idx : group2Infantry) {
            if (!firstSpotTick.contains(idx) && sim.isValidAttackTarget(idx, 0)) {
                firstSpotTick[idx] = tick;
            }
        }

        if (tick == kExpectedGroup2SpotTick - 1) {
            (void)scene.updateFog(lastFog);
            beforeSpotCrop = scene.repaintRegion(cropL, cropT, cropR, cropB).pixels;
        } else if (tick == kExpectedGroup2SpotTick) {
            (void)scene.updateFog(lastFog);
            afterSpotCrop = scene.repaintRegion(cropL, cropT, cropR, cropB).pixels;
        }

        for (const auto& ck : kExpected) {
            if (ck.tick == tick) {
                checkCheckpoint(tick);
                break;
            }
        }
    }

    require(posCrc == 0x0489743fu, "Convoy trajectory CRC32 drifted while FogGrid enabled");
    for (std::size_t idx : group2Infantry) {
        const std::size_t plIdx = placementIndexByInfantry[idx];
        const int tSpot = firstSpotTick.contains(idx) ? firstSpotTick[idx] : -1;
        const auto [u, v] = ss2::tutorialFootWorld(infantry[idx].x, infantry[idx].y, 4096);
        std::cout << "NATIVE_GROUP2_SPOT placement=" << plIdx << " id=" << infantry[idx].id
                  << " tile=(" << (u >> 5) << ',' << (v >> 5) << ") tick=" << tSpot << '\n';
        require(tSpot == kExpectedGroup2SpotTick,
                "Group-2 enemy spotted on tick " + std::to_string(tSpot) + " instead of Gold tick 1414");
    }

    // 1x zoom rendered pixel comparison across tick 1413 (unexplored/unspotted) -> 1414 (visible/spotted):
    // Shroud lifts on newly spotted tile (36,78) and the 1x enemy sprite pixels appear on screen.
    require(beforeSpotCrop.width == 48u && afterSpotCrop.width == 48u &&
                beforeSpotCrop.rgba.size() == afterSpotCrop.rgba.size(),
            "Group-2 1x render crop dimensions mismatch");
    // Compare afterSpotCrop with a temporarily hidden Group-2 enemy at tick 1414 (same tick fog)
    // to isolate rendered sprite pixels from ground fog brightness transition.
    std::vector<std::size_t> revealedInCrop;
    for (std::size_t g2 : group2Infantry) {
        if (scene.units()[g2].visible) {
            revealedInCrop.push_back(g2);
        }
    }
    require(revealedInCrop.size() == 4u, "All 4 Group-2 enemies must be visible in 1x InfantryScene at tick 1414");
    for (std::size_t g2 : revealedInCrop) {
        (void)scene.update(g2, infantry[g2].x, infantry[g2].y, infantry[g2].direction, 0, false,
                           infantry[g2].hp, 0, infantry[g2].ammo);
    }
    const auto afterHiddenCrop = scene.repaintRegion(cropL, cropT, cropR, cropB).pixels;
    std::size_t spritePixels = 0;
    for (std::size_t i = 0; i < afterSpotCrop.rgba.size(); i += 4u) {
        if (afterSpotCrop.rgba[i] != afterHiddenCrop.rgba[i] ||
            afterSpotCrop.rgba[i + 1] != afterHiddenCrop.rgba[i + 1] ||
            afterSpotCrop.rgba[i + 2] != afterHiddenCrop.rgba[i + 2]) {
            ++spritePixels;
        }
    }
    std::uint64_t lumaBefore = 0, lumaHiddenAfter = 0;
    std::size_t brightenedPixels = 0;
    for (std::size_t i = 0; i < beforeSpotCrop.rgba.size(); i += 4u) {
        const int bSum = int(beforeSpotCrop.rgba[i]) + beforeSpotCrop.rgba[i + 1] + beforeSpotCrop.rgba[i + 2];
        const int hSum = int(afterHiddenCrop.rgba[i]) + afterHiddenCrop.rgba[i + 1] + afterHiddenCrop.rgba[i + 2];
        lumaBefore += static_cast<std::uint64_t>(bSum);
        lumaHiddenAfter += static_cast<std::uint64_t>(hSum);
        if (hSum > bSum) ++brightenedPixels;
    }
    require(lumaHiddenAfter > lumaBefore && brightenedPixels >= 500u && spritePixels >= 20u,
            "1x Group-2 crop did not lift shroud / render revealed sprite on tick 1414");

    std::cout << "gold_visibility_parity: PASS (11/11 Gold checkpoints, Group-2 spot tick 1414, 1x render verified)\n";
}

} // namespace

int main(int argc, char** argv) try {
    testSightMatrixParity();
    testPerimeterFlags();
    testMovingSightPenaltyLive();
    testRendered1xFogAndVisibility(argc > 1 ? "gold" : "ci");
    std::cout << "visibility_parity: PASS (48 Gold sight rows, perimeter LOS, moving -1 drop/restore, 1x fog/sprite render)\n";
    if (argc > 1) {
        testGoldControlledVisibility(argv[1]);
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "visibility_parity_test failed: " << e.what() << '\n';
    return 1;
}
