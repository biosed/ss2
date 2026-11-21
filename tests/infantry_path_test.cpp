#include "sim/infantry_path.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/tutorial_units.hpp"
#include "assets/mission.hpp"
#include "assets/placement.hpp"
#include "assets/infantry.hpp"
#include <algorithm>
#include <array>
#include <zlib.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

// Same ordering and delimiter as verify_infantry_route.py --all-zone.
static unsigned long objectiveRouteChecksum(const std::filesystem::path& game) {
    auto grid = ss2::InfantryPathGrid::loadTutorial(game);
    const auto mission = ss2::assets::readTutorialData(game / "maps/single/--- TUTORIAL I ---.SSM");
    std::vector<std::pair<int, int>> starts;
    for (const auto& unit : ss2::assets::readTutorialPlacements(game)) {
        const auto cell = grid.fromWorld((2*unit.y + unit.x - 4096)/2, (2*unit.y - unit.x + 4096)/2);
        grid.setOccupied(cell, true);
        if (unit.owner == 0) starts.emplace_back(cell.x, cell.y);
    }
    std::ranges::sort(starts);
    for (bool spread : {false,true}) {
        auto formationGrid = grid;
        std::vector<ss2::SubCell> sources;
        for (const auto& [x,y] : starts) formationGrid.setOccupied({x,y},false);
        for (size_t i = 0; i < starts.size(); ++i) {
            const auto [x,y] = starts[i];
            sources.push_back({x,y-(spread && i%2 == 0 ? 60 : 0)});
            formationGrid.setOccupied(sources.back(),true);
        }
        unsigned long formationCrc = 0;
        for (int i = 0; i < int(mission.zones.size()); ++i) {
            if ((mission.zones[i]&63u) != 2) continue;
            const auto assigned = formationGrid.formationDestinations(sources,{2*(i%128),2*(i/128)});
            assert(assigned.size() == sources.size());
            for (auto cell : assigned) {
                const std::array<std::uint8_t,2> tile{std::uint8_t(cell.x/2), std::uint8_t(cell.y/2)};
                formationCrc = crc32_z(formationCrc,tile.data(),tile.size());
            }
        }
        assert(formationCrc == (spread ? 0x72fc5094UL : 0xc843ae03UL));
        std::cout << "Formation spread=" << spread << " CRC32 " << std::hex << formationCrc << std::dec << '\n';
    }
    unsigned long checksum = 0;
    size_t cases = 0;
    for (const auto& [x, y] : starts) {
        const ss2::SubCell start{x, y};
        grid.setOccupied(start, false);
        for (int i = 0; i < int(mission.zones.size()); ++i) {
            if ((mission.zones[i] & 63u) != 2) continue;
            auto steps = grid.findDirections(start, {2*(i%128), 2*(i/128)});
            steps.push_back(255);
            checksum = crc32_z(checksum, steps.data(), steps.size());
            ++cases;
        }
        grid.setOccupied(start, true);
    }
    assert(cases == 7366);
    return checksum;
}

// Direct 0x1008bda0 samples on the unobstructed Tutorial I strip at y=60.
// Freeze the RNG input across searches so the backoff bytes can be compared.
static void checkBlockedRouteRetry(ss2::InfantryPathGrid grid) {
    using Action = ss2::InfantryPathGrid::PlanAction;
    using State = ss2::InfantryPathGrid::RouteState;
    const std::uint8_t random = ((0x76321fu * 0x41c64e6du + 0x3039u) >> 16) & 255;
    const ss2::SubCell start{61,120}, goal{66,120};
    grid.setOccupied(start, true);
    State convoy;
    auto route = grid.plan(start, goal, {}, grid.liveSlots(), convoy, random);
    assert(route.action == Action::replaceQueue && route.directions == std::vector<std::uint8_t>({0,0,0}));
    assert(convoy.baselineCost == 3);
    auto moving = grid.liveSlots();
    grid.setOccupied({62,120}, true);
    grid.setOccupied({62,121}, true);
    route = grid.plan(start, goal, 0, moving, convoy, random);
    assert(route.action == Action::keepQueue && convoy.wait == 2);
    route = grid.plan(start, goal, 0, grid.liveSlots(), convoy, random);
    assert(route.action == Action::replaceQueue && route.directions == std::vector<std::uint8_t>({7,1,0}));
    assert(convoy.wait == 0 && convoy.penalty == 0);

    grid.clearOccupancy();
    grid.setOccupied(start, true);
    const ss2::SubCell occupiedGoal{64,120};
    for (int slot = 0; slot < 4; ++slot) grid.setOccupied({64+(slot&1),120+(slot>>1)}, true);
    State stopped;
    route = grid.plan(start, occupiedGoal, {}, grid.liveSlots(), stopped, random);
    assert(route.action == Action::replaceQueue && route.directions == std::vector<std::uint8_t>({0}));
    grid.setOccupied(start, false);
    const ss2::SubCell approach{63,120};
    grid.setOccupied(approach, true);
    for (int attempt = 1; attempt <= 4; ++attempt) {
        route = grid.plan(approach, occupiedGoal, {}, grid.liveSlots(), stopped, random);
        assert(route.action == (attempt == 4 ? Action::giveUp : Action::wait));
        assert(stopped.penalty == 41*attempt && stopped.backoff == 27);
    }
}

// Size-driven 256/512 tile maps: the Bug2 search deque must grow past the old
// 128-slot ring and the iteration guard must scale with map area.
static void checkLargeMapPathing() {
    for (int side : {256, 512}) {
        const std::size_t n = std::size_t(side) * std::size_t(side);
        std::vector<std::uint32_t> flags(n, 0u);
        std::vector<std::uint16_t> obstacles(n, 0x000Fu);
        auto grid = ss2::InfantryPathGrid::fromSubCellMasks(side, side, flags, obstacles, 4);
        assert(grid.width() == side*2 && grid.height() == side*2);
        const ss2::SubCell start{2*(side-60), 2*(side-60)}, goal{2*(side-30), 2*(side-60)};
        auto path = grid.findPath(start, goal);
        assert(!path.empty() && path.back() == goal);
        for (const auto& c : path) assert(grid.walkable(c));
        const ss2::SubCell a{8, 8}, b{2*(side-8), 2*(side-8)};
        auto longPath = grid.findPath(a, b);
        assert(!longPath.empty());
        for (const auto& c : longPath) assert(grid.walkable(c));
        std::vector<ss2::SubCell> sources{{2*(side/2), 2*(side/2)}, {2*(side/2+1), 2*(side/2)}};
        auto assigned = grid.formationDestinations(sources, {2*(side/2+3), 2*(side/2)});
        assert(assigned.size() == sources.size());
        for (const auto& c : assigned) assert(grid.inBounds(c));
    }
    std::cout << "Large-map (256/512) infantry_path tests PASSED\n";
}

// FogGrid and TutorialUnits::countInZone must scale to non-128 mission grids.
static void checkLargeFogZones() {
    ss2::FogGrid fog(512, 512, std::vector<std::uint32_t>(512u*512u, 0u));
    fog.buildFallbackCirc64();
    fog.revealCircle(400, 400, 10, 0);
    assert(fog.raw().size() == 512u*512u);
    assert(fog.tile(400, 400) != 0);
    assert(fog.tile(0, 0) == 0 && fog.tile(511, 511) == 0);

    const int side = 256;
    auto grid = ss2::InfantryPathGrid::fromSubCellMasks(side, side,
        std::vector<std::uint32_t>(side*side, 0u), std::vector<std::uint16_t>(side*side, 0x000Fu), 4);
    const int tileX = 120, tileY = 100, originX = side*32;
    const auto [u, v] = ss2::InfantryPathGrid::toWorldCenter({tileX*2, tileY*2});
    ss2::assets::Infantry inf{};
    inf.id = "aguner"; inf.x = originX + u - v; inf.y = (u + v)/2;
    inf.owner = 0; inf.hp = 100; inf.ammo = 40; inf.group = 0;
    const std::array<ss2::assets::Infantry, 1> units{inf};
    ss2::TutorialUnits soldiers(std::move(grid), units, {}, std::nullopt);
    std::vector<std::uint8_t> zones(std::size_t(side)*side, 0u);
    zones[std::size_t(tileY)*side + tileX] = 5u;
    assert(soldiers.countInZone(zones, 5u, 0) == 1u);
    assert(soldiers.countInZone(zones, 6u, 0) == 0u);
    bool threw = false;
    try { soldiers.countInZone(std::vector<std::uint8_t>(128u*128u, 0u), 5u, 0); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    std::cout << "Large-grid fog + countInZone tests PASSED\n";
}

int main(int argc, char** argv) {
    // Gold's closest fallback lies around this wall, adjacent to an inaccessible target.
    // Scoring only squared distance and step count stops at the near side of the wall.
    {
        constexpr int side = 64;
        std::vector<std::uint32_t> flags(side*side);
        for (int y = 25; y <= 39; ++y) flags[y*side+34] = 15;
        flags[32*side+35] = 15;
        auto wall = ss2::InfantryPathGrid::fromSubCellMasks(side,side,std::move(flags),
                                                            std::vector<std::uint16_t>(side*side));
        auto reference = std::vector<std::uint8_t>(16,0);
        reference.insert(reference.end(),8,7);
        reference.push_back(1);
        reference.insert(reference.end(),6,2);
        assert(wall.findTileDirections({20,64},{70,64}) == reference);
    }
    // 1. Synthetic 16x16 tile grid (32x32 sub-cells), borderMargin=4 -> walkable tiles [4..11]x[4..11]
    std::vector<std::uint32_t> flags(16 * 16, 0u);
    std::vector<std::uint16_t> obstacles(16 * 16, 0x000Fu); // ground everywhere
    obstacles[6 * 16 + 8] = 0x00F0u; // Deep water at tile (8, 6)
    obstacles[6 * 16 + 6] = 0x0F00u; // Shallows at tile (6, 6) (walkable for infantry)
    flags[6 * 16 + 7] = 0x1u;        // Static sub-cell obstacle in tile (7, 6) slot 0 (slots 1,2,3 open)
    flags[6 * 16 + 9] = 0x4000u;     // Hard impassable tile (9, 6)

    auto grid = ss2::InfantryPathGrid::fromSubCellMasks(16, 16, flags, obstacles, 4);
    assert(grid.width() == 32 && grid.height() == 32);
    assert(!grid.walkable({7, 10})); // outside borderMargin
    assert(grid.walkable({8, 10}));  // inside borderMargin
    assert(!grid.walkable({16, 12})); // deep water
    assert(grid.walkable({12, 12}));  // shallows
    assert(!grid.walkable({14, 12})); // slot 0 obstacle
    assert(grid.walkable({15, 12}));  // slot 1 open
    assert(!grid.walkable({18, 12})); // 0x4000

    // 2. Exact 0x100f8718 transition expansion (tile (5,6) slot 0 -> tile (6,6)):
    auto dirs = grid.findDirections({10, 12}, {12, 12});
    assert(dirs.size() == 2 && dirs[0] == 0 && dirs[1] == 0);
    auto path = grid.findPath({10, 12}, {12, 12});
    assert(path.size() == 2 && path[0] == (ss2::SubCell{11, 12}) && path[1] == (ss2::SubCell{12, 12}));

    // 3. Same-tile goal returns empty (0x10021a50 stops when tile == goalTile)
    assert(grid.findDirections({10, 12}, {11, 12}).empty());

    // 4. Exact 0x1008ba40 partial-tile entry: (6,6) -> (7,6) enters open slot 2 (14,13) via cardinal fallback
    auto partial = grid.findPath({12, 12}, {14, 13});
    assert(!partial.empty());
    assert(partial.back().x / 2 == 7 && partial.back().y / 2 == 6);
    for (const auto& c : partial) assert(grid.walkable(c));

    // 5. Occupancy cardinal fallback (0x10021984):
    grid.setOccupied({11, 12}, true);
    auto occDirs = grid.findDirections({10, 12}, {12, 12});
    assert(occDirs.size() == 2 && occDirs[0] == 1 && occDirs[1] == 0);
    grid.clearOccupancy();

    // 6. Reachable obstacle detour around deep water (8,6) and 0x4000 (9,6) from (6,6) to (10,6)
    auto detour = grid.findPath({12, 12}, {20, 12});
    assert(!detour.empty());
    assert(detour.back().x / 2 == 10 && detour.back().y / 2 == 6);
    for (const auto& c : detour) assert(grid.walkable(c));

    // 7. Fully blocked goal (9,6) from (5,6) stops at closest reachable walkable tile (0x1008be20 score)
    auto edgePath = grid.findPath({10, 12}, {18, 12});
    assert(!edgePath.empty());
    const int etx = edgePath.back().x / 2, ety = edgePath.back().y / 2;
    assert((etx - 9) * (etx - 9) + (ety - 6) * (ety - 6) <= 2);
    for (const auto& c : edgePath) assert(grid.walkable(c));

    // 8. Boxed-in soldier (surrounded by 0x4000) returns empty
    auto boxedFlags = flags;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx != 0 || dy != 0) boxedFlags[(6 + dy) * 16 + (5 + dx)] = 0x4000u;
    auto boxed = ss2::InfantryPathGrid::fromSubCellMasks(16, 16, boxedFlags, obstacles, 4);
    assert(boxed.findDirections({10, 12}, {20, 12}).empty());

    checkLargeMapPathing();
    checkLargeFogZones();

    std::cout << "Synthetic infantry_path tests PASSED\n";

    // Naval mask passability & findTileDirectionsForMask (0x000Fu = water+shallows passable, dry ground blocked).
    {
        std::vector<std::uint32_t> nflags(16 * 16, 0u);
        std::vector<std::uint16_t> nobst(16 * 16, 0x000Fu); // dry ground everywhere
        for (int tx = 5; tx <= 8; ++tx) nobst[8 * 16 + tx] = 0x00F0u; // deep water channel
        nobst[8 * 16 + 9] = 0x0F00u;                                 // shallows landing bank
        auto navGrid = ss2::InfantryPathGrid::fromSubCellMasks(16, 16, nflags, nobst, 4);
        assert(navGrid.walkableMask({10, 16}, 0x000Fu));
        assert(navGrid.walkableMask({18, 16}, 0x000Fu));  // shallows passable to naval craft
        assert(!navGrid.walkableMask({20, 16}, 0x000Fu)); // dry ground blocked to naval craft
        const auto waterDirs = navGrid.findTileDirectionsForMask({10, 16}, {18, 16}, 0x000Fu, 64);
        assert(waterDirs.size() == 4u && std::all_of(waterDirs.begin(), waterDirs.end(), [](std::uint8_t d) { return d == 0u; }));
        assert(navGrid.findTileDirectionsForMask({10, 16}, {20, 16}, 0x000Fu, 64).empty());
    }

    if (argc > 1) {
        const auto checksum = objectiveRouteChecksum(argv[1]);
        std::cout << "Opening objective routes CRC32 " << std::hex << checksum << std::dec << '\n';
        assert(checksum == 0xc67b6a1bUL);
        auto tut = ss2::InfantryPathGrid::loadTutorial(argv[1]);
        checkBlockedRouteRetry(tut);
        assert(tut.width() == 256 && tut.height() == 256);
        const auto start = ss2::InfantryPathGrid::fromWorld(1112, 3656); // dofficer tile (34,114)
        assert(tut.walkable(start));
        const auto adj = tut.findDirections(start, {start.x, start.y - 2}); // 1 tile N
        assert(!adj.empty());
        const auto multi = tut.findDirections({68, 220}, {70, 220}); // (34,110)->(35,110)
        assert(!multi.empty());

        // Route from dofficer (34,114) directly to Objective 1 wreck (U=1208, V=2344 -> tile (37,73))
        // Under exact 0x1008bda0/0x1008ba40, tile (37,73) has open slots 0 & 2 -> enters exact (37,73)!
        const auto wreck = ss2::InfantryPathGrid::fromWorld(1208, 2344);
        const auto toWreck = tut.findPath(start, wreck);
        assert(!toWreck.empty());
        const int wtx = toWreck.back().x / 2, wty = toWreck.back().y / 2;
        assert(wtx == 37 && wty == 73);
        for (const auto& c : toWreck) assert(tut.walkable(c));

        // Detours directly around the wreck
        const auto aroundN = tut.findPath({37 * 2, 76 * 2}, {37 * 2, 70 * 2});
        assert(!aroundN.empty() && aroundN.back().x / 2 == 37 && aroundN.back().y / 2 == 70);
        for (const auto& c : aroundN) assert(tut.walkable(c));

        const auto aroundE = tut.findPath({35 * 2, 73 * 2}, {40 * 2, 73 * 2});
        assert(!aroundE.empty() && aroundE.back().x / 2 == 40 && aroundE.back().y / 2 == 73);
        for (const auto& c : aroundE) assert(tut.walkable(c));

        // Out-of-bounds border goal rejected
        assert(tut.findDirections(start, {2, 2}).empty());

        std::cout << "Tutorial I (34,110)->(35,110) substeps=" << multi.size()
                  << " | dofficer->wreck end=(" << wtx << "," << wty << ") substeps=" << toWreck.size() << "\n";
    }
    return 0;
}
