#include "sim/infantry_path.hpp"
#include <cassert>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    // 1. Synthetic 16x16 tile grid (32x32 sub-cells), borderMargin=4 -> walkable tiles [4..11]x[4..11]
    std::vector<std::uint32_t> flags(16 * 16, 0u);
    std::vector<std::uint16_t> obstacles(16 * 16, 0x000Fu); // ground everywhere
    obstacles[6 * 16 + 8] = 0x00F0u; // Deep water at tile (8, 6)
    obstacles[6 * 16 + 6] = 0x0F00u; // Shallows at tile (6, 6) (walkable for infantry)
    flags[6 * 16 + 7] = 0x1u;        // Static sub-cell obstacle in tile (7, 6) slot 0
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
    // Slot 0 + dir 0 -> {cross=0, primary=0} -> slot 1 (11,12), then slot 1 + dir 0 -> {cross=1, primary=0} -> tile (6,6) slot 0 (12,12)
    auto dirs = grid.findDirections({10, 12}, {12, 12});
    assert(dirs.size() == 2 && dirs[0] == 0 && dirs[1] == 0);
    auto path = grid.findPath({10, 12}, {12, 12});
    assert(path.size() == 2 && path[0] == (ss2::SubCell{11, 12}) && path[1] == (ss2::SubCell{12, 12}));

    // 3. Same-tile goal returns empty (0x10021a50 stops when tile == goalTile)
    assert(grid.findDirections({10, 12}, {11, 12}).empty());

    // 4. Exact 0x10088410 predicate rejects tile (7,6) because slot 0 bit is set (flags & 0xF != 0)
    assert(grid.findDirections({12, 12}, {14, 13}).empty());

    // 5. Occupancy cardinal fallback (0x10021984):
    // Start (10,12) [tile (5,6) slot 0] -> tile (6,6) with primary slot 1 (11,12) occupied:
    // Step 1: slot 0 + dir 0 -> primary (11,12) occupied -> altDir=1 (SE) -> slot 3 (11,13) [cross=0]
    // Step 2: slot 3 + dir 0 -> primary=0 -> tile (6,6) slot 2 (12,13) [cross=1, arrives in goal tile (6,6)!]
    grid.setOccupied({11, 12}, true);
    auto occDirs = grid.findDirections({10, 12}, {12, 12});
    assert(occDirs.size() == 2 && occDirs[0] == 1 && occDirs[1] == 0);
    grid.clearOccupancy();

    // 6. Diagonal corner check (0x100884d5 / 0x100fbcfc):
    // Move NE (dir 7) from tile (6,7) to tile (7,6) is blocked; move SE (dir 1) from (6,5) to (7,6) blocked;
    // Move SW (dir 3) from (8,5) to (7,6) blocked; test diagonal corner-only block:
    // Tile (7,6) has slot 0 blocked (bit 0 = 0x1). Moving NW (dir 5, k=2) from (7,7) -> (6,6) checks:
    // cornerA (dstTx=6, srcTy=7) mask 0x2 (clear), cornerB (srcTx=7, dstTy=6) mask 0x4 (slot 2 of (7,6), clear) -> ALLOWED!
    // Moving SE (dir 1, k=0) from (6,5) -> (7,6) blocked by dst. Moving SW (dir 3, k=1) from (7,5) -> (6,6) checks:
    // cornerA (6,5) mask 0x8, cornerB (7,6) mask 0x1 (BLOCKED!) -> REJECTED!
    assert(!grid.findDirections({14, 14}, {12, 12}).empty()); // (7,7) -> (6,6) NW allowed
    assert(grid.findDirections({14, 10}, {12, 12}).empty());  // (7,5) -> (6,6) SW blocked by cornerB (7,6) slot 0!

    // 7. Unverified obstacle detour rejected (crosses deep water at (8,6))
    assert(grid.findDirections({12, 12}, {20, 12}).empty());

    std::cout << "Synthetic infantry_path tests PASSED\n";

    if (argc > 1) {
        auto tut = ss2::InfantryPathGrid::loadTutorial(argv[1]);
        assert(tut.width() == 256 && tut.height() == 256);
        const auto start = ss2::InfantryPathGrid::fromWorld(1112, 3656); // dofficer tile (34,114)
        assert(tut.walkable(start));
        const auto adj = tut.findDirections(start, {start.x, start.y - 2}); // 1 tile N
        assert(!adj.empty());
        const auto multi = tut.findDirections({68, 220}, {70, 220}); // (34,110)->(35,110) matching Codex Unicorn probe
        assert(!multi.empty());
        std::cout << "Tutorial I (34,110)->(35,110) substeps=" << multi.size()
                  << " dirs=[" << int(multi[0]) << (multi.size() > 1 ? "," + std::to_string(multi[1]) : "") << "]\n";
    }
    return 0;
}
