#include "sim/infantry_path.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include "assets/mission.hpp"
#include <stdexcept>

namespace ss2 {
namespace {
constexpr int kMaxTileSide = 128;
constexpr std::uint16_t kInfantryForbiddenMask = 0x00F0u; // walkonground=1, walkonwater=0, walkonshallows=1 (0x10061d25)

// n2Game_Dll.dll 0x100fbcb4: 8-direction table.
constexpr std::array<int, 8> kDx{1, 1, 0, -1, -1, -1, 0, 1};
constexpr std::array<int, 8> kDy{0, 1, 1, 1, 0, -1, -1, -1};

// n2Game_Dll.dll 0x100f8710: sub-cell slot XOR flip mask per direction.
constexpr std::array<std::uint8_t, 8> kXorMask{1, 3, 2, 3, 1, 3, 2, 3};

// n2Game_Dll.dll 0x100fbcfc / 0x100fbdfc: diagonal corner sub-cell masks for dir in {1,3,5,7} (idx = dir >> 1).
constexpr std::array<std::uint8_t, 4> kDiagCornerA{0x4u, 0x8u, 0x2u, 0x1u}; // (dstTx, srcTy)
constexpr std::array<std::uint8_t, 4> kDiagCornerB{0x2u, 0x1u, 0x4u, 0x8u}; // (srcTx, dstTy)

struct SubTransition {
    std::uint8_t crossCell;
    std::uint8_t primaryDir;
    std::uint8_t altDir; // 8 = no fallback
};

// n2Game_Dll.dll 0x100f8718: 4 slots x 8 directions transition table used by 0x100218d0.
constexpr std::array<std::array<SubTransition, 8>, 4> kSubTable{{
    {{{0, 0, 1}, {0, 1, 8}, {0, 2, 1}, {0, 2, 8}, {1, 4, 3}, {1, 5, 8}, {1, 6, 7}, {0, 0, 8}}},
    {{{1, 0, 1}, {0, 2, 8}, {0, 2, 3}, {0, 3, 8}, {0, 4, 3}, {0, 4, 8}, {1, 6, 5}, {1, 7, 8}}},
    {{{0, 0, 7}, {0, 0, 8}, {1, 2, 1}, {1, 3, 8}, {1, 4, 5}, {0, 6, 8}, {0, 6, 7}, {0, 7, 8}}},
    {{{1, 0, 7}, {1, 1, 8}, {1, 2, 3}, {0, 4, 8}, {0, 4, 5}, {0, 5, 8}, {0, 6, 5}, {0, 6, 8}}},
}};

std::uint8_t directionFromDelta(int dx, int dy) {
    const int sx = (dx > 0) - (dx < 0);
    const int sy = (dy > 0) - (dy < 0);
    for (std::uint8_t d = 0; d < 8; ++d)
        if (kDx[d] == sx && kDy[d] == sy) return d;
    return 8;
}
}

InfantryPathGrid InfantryPathGrid::loadTutorial(const std::filesystem::path& gameDir) {
    auto data = assets::readTutorialData(gameDir / "maps/single/--- TUTORIAL I ---.SSM");
    return fromSubCellMasks(128, 128, std::move(data.flags), std::move(data.obstacles));
}

InfantryPathGrid InfantryPathGrid::fromSubCellMasks(int tileWidth, int tileHeight,
                                                    std::vector<std::uint32_t> flags,
                                                    std::vector<std::uint16_t> obstacles,
                                                    int borderMargin) {
    if (tileWidth <= 0 || tileHeight <= 0 || tileWidth > kMaxTileSide || tileHeight > kMaxTileSide)
        throw std::runtime_error("Invalid path grid dimensions");
    const std::size_t count = std::size_t(tileWidth) * std::size_t(tileHeight);
    if (flags.size() != count || obstacles.size() != count)
        throw std::runtime_error("Path grid mask size mismatch");
    if (borderMargin < 0 || borderMargin * 2 >= tileWidth || borderMargin * 2 >= tileHeight)
        throw std::runtime_error("Invalid path grid border margin");

    InfantryPathGrid grid;
    grid.tileWidth_ = tileWidth;
    grid.tileHeight_ = tileHeight;
    grid.subWidth_ = tileWidth * 2;
    grid.subHeight_ = tileHeight * 2;
    grid.borderMargin_ = borderMargin;
    grid.flags_ = std::move(flags);
    grid.obstacles_ = std::move(obstacles);
    grid.occupancy_.assign(count, 0u);
    return grid;
}

InfantryPathGrid InfantryPathGrid::fromWalkable(int subWidth, int subHeight, std::vector<std::uint8_t> walkable) {
    if (subWidth <= 0 || subHeight <= 0 || (subWidth % 2) != 0 || (subHeight % 2) != 0 ||
        subWidth > kMaxTileSide * 2 || subHeight > kMaxTileSide * 2)
        throw std::runtime_error("Invalid walkable sub-cell dimensions");
    if (walkable.size() != std::size_t(subWidth) * std::size_t(subHeight))
        throw std::runtime_error("Walkable mask size mismatch");

    InfantryPathGrid grid;
    grid.tileWidth_ = subWidth / 2;
    grid.tileHeight_ = subHeight / 2;
    grid.subWidth_ = subWidth;
    grid.subHeight_ = subHeight;
    grid.borderMargin_ = 0;
    grid.walkable_ = std::move(walkable);
    grid.occupancy_.assign(std::size_t(grid.tileWidth_) * std::size_t(grid.tileHeight_), 0u);
    return grid;
}

bool InfantryPathGrid::inBounds(SubCell cell) const {
    if (cell.x < 0 || cell.y < 0 || cell.x >= subWidth_ || cell.y >= subHeight_) return false;
    const int tx = cell.x >> 1;
    const int ty = cell.y >> 1;
    return tx >= borderMargin_ && ty >= borderMargin_ &&
           tx < tileWidth_ - borderMargin_ && ty < tileHeight_ - borderMargin_;
}

bool InfantryPathGrid::walkable(SubCell cell) const {
    if (!inBounds(cell)) return false;
    if (!walkable_.empty())
        return walkable_[std::size_t(cell.y) * std::size_t(subWidth_) + std::size_t(cell.x)] != 0;

    const std::size_t idx = std::size_t(cell.y >> 1) * std::size_t(tileWidth_) + std::size_t(cell.x >> 1);
    const unsigned slot = unsigned((cell.x & 1) | ((cell.y & 1) << 1));
    const unsigned slotBit = 1u << slot;

    // Exact infantry static walkability from n2Game_Dll.dll 0x10056710:
    const std::uint32_t pf = flags_[idx];
    if ((pf & 0x4000u) != 0 || ((pf & slotBit) & 0xFu) != 0) return false;
    const std::uint16_t obst = obstacles_[idx] & kInfantryForbiddenMask;
    const unsigned blocked = unsigned(obst | (obst >> 4) | (obst >> 8) | (obst >> 12)) & 0xFu;
    return (blocked & slotBit) == 0;
}

void InfantryPathGrid::clearOccupancy() {
    std::fill(occupancy_.begin(), occupancy_.end(), 0u);
}

void InfantryPathGrid::setOccupied(SubCell cell, bool occ) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= subWidth_ || cell.y >= subHeight_) return;
    const std::size_t idx = std::size_t(cell.y >> 1) * std::size_t(tileWidth_) + std::size_t(cell.x >> 1);
    const unsigned shift = unsigned(((cell.x & 1) | ((cell.y & 1) << 1)) * 4);
    const std::uint32_t mask = 0xFu << shift;
    occupancy_[idx] = (occupancy_[idx] & ~mask) | (occ ? (1u << shift) : 0u);
}

bool InfantryPathGrid::occupied(SubCell cell) const {
    if (cell.x < 0 || cell.y < 0 || cell.x >= subWidth_ || cell.y >= subHeight_) return true;
    const std::size_t idx = std::size_t(cell.y >> 1) * std::size_t(tileWidth_) + std::size_t(cell.x >> 1);
    const unsigned shift = unsigned(((cell.x & 1) | ((cell.y & 1) << 1)) * 4);
    return ((occupancy_[idx] >> shift) & 0xFu) != 0;
}

bool InfantryPathGrid::canEnter(SubCell cell) const {
    return walkable(cell) && !occupied(cell);
}

bool InfantryPathGrid::clearRouteTile(int tileX, int tileY) const {
    if (!inBounds({tileX*2, tileY*2})) return false;
    // The retail checker can apply a player-specific mask to bits 20..23.
    // Restrict the verified direct branch to tiles clear for every player.
    if (!flags_.empty() && (flags_[std::size_t(tileY)*tileWidth_+tileX] & 0x00f00000)) return false;
    for (int slot = 0; slot < 4; ++slot)
        if (!canEnter({2*tileX+(slot&1), 2*tileY+(slot>>1)})) return false;
    return true;
}

std::vector<std::uint8_t> InfantryPathGrid::findDirections(SubCell start, SubCell goal, int maxSteps) const {
    std::vector<std::uint8_t> substeps;
    if (maxSteps <= 0 || !inBounds(start) || !inBounds(goal)) return substeps;

    // 1. Tile-level greedy straight-line route matching n2Game_Dll.dll 0x10088780 + 0x10088410 predicate.
    // Stops when tile == goalTile (matching 0x10021a50). Unverified obstacle detours are rejected.
    int curTx = start.x >> 1;
    int curTy = start.y >> 1;
    const int goalTx = goal.x >> 1;
    const int goalTy = goal.y >> 1;
    if (curTx == goalTx && curTy == goalTy) return substeps;

    const auto slotClear = [&](int tx, int ty, std::uint8_t slotMask) -> bool {
        for (int s = 0; s < 4; ++s) {
            if ((slotMask & (1u << s)) != 0 && !walkable({tx * 2 + (s & 1), ty * 2 + (s >> 1)}))
                return false;
        }
        return true;
    };

    // Exact 0x10088410 pass-1 static tile transition predicate:
    // All 4 sub-cells of (dstTx, dstTy) must be statically clear; diagonal steps also require the two
    // touching corner sub-cells in (dstTx, srcTy) and (srcTx, dstTy) (0x100fbcfc / 0x100fbdfc).
    const auto tileStepAllowed = [&](int srcTx, int srcTy, int dstTx, int dstTy, std::uint8_t dir) -> bool {
        if (!slotClear(dstTx, dstTy, 0xFu)) return false;
        if ((dir & 1u) != 0) {
            const std::size_t k = dir >> 1;
            if (!slotClear(dstTx, srcTy, kDiagCornerA[k])) return false;
            if (!slotClear(srcTx, dstTy, kDiagCornerB[k])) return false;
        }
        return true;
    };

    std::vector<std::uint8_t> tileDirs;
    tileDirs.reserve(64);
    while (curTx != goalTx || curTy != goalTy) {
        if (static_cast<int>(tileDirs.size()) >= maxSteps) return {};
        const std::uint8_t dir = directionFromDelta(goalTx - curTx, goalTy - curTy);
        if (dir >= 8) return {};
        const int nextTx = curTx + kDx[dir];
        const int nextTy = curTy + kDy[dir];
        if (!tileStepAllowed(curTx, curTy, nextTx, nextTy, dir)) return {};
        curTx = nextTx;
        curTy = nextTy;
        tileDirs.push_back(dir);
    }

    // 2. Expand queued tile directions through the exact 32-entry sub-cell transition table (0x100f8718 / 0x100218d0).
    // Consumes a queued tile direction ONLY when crossCell == 1.
    int tx = start.x >> 1;
    int ty = start.y >> 1;
    unsigned slot = unsigned((start.x & 1) | ((start.y & 1) << 1));
    std::size_t head = 0;

    while (head < tileDirs.size()) {
        if (static_cast<int>(substeps.size()) >= maxSteps) return {};
        const std::uint8_t dir = tileDirs[head];
        const SubTransition e = kSubTable[slot][dir];
        const int targetTx = e.crossCell ? (tx + kDx[dir]) : tx;
        const int targetTy = e.crossCell ? (ty + kDy[dir]) : ty;

        const unsigned primarySlot = slot ^ kXorMask[e.primaryDir];
        const SubCell primaryCell{targetTx * 2 + int(primarySlot & 1u), targetTy * 2 + int(primarySlot >> 1)};
        if (canEnter(primaryCell)) {
            substeps.push_back(e.primaryDir);
            tx = targetTx;
            ty = targetTy;
            slot = primarySlot;
            if (e.crossCell) ++head;
            continue;
        }

        // Exact 0x10021984..0x100219d3 fallback: cardinal tile dirs only (even dir, altDir < 8).
        if ((dir & 1u) == 0 && e.altDir < 8) {
            const unsigned altSlot = slot ^ kXorMask[e.altDir];
            const SubCell altCell{targetTx * 2 + int(altSlot & 1u), targetTy * 2 + int(altSlot >> 1)};
            if (canEnter(altCell)) {
                substeps.push_back(e.altDir);
                tx = targetTx;
                ty = targetTy;
                slot = altSlot;
                if (e.crossCell) ++head;
                continue;
            }
        }
        return {};
    }

    return substeps;
}

std::vector<SubCell> InfantryPathGrid::findPath(SubCell start, SubCell goal, int maxSteps) const {
    const auto dirs = findDirections(start, goal, maxSteps);
    std::vector<SubCell> path;
    path.reserve(dirs.size());
    SubCell cur = start;
    for (const auto d : dirs) {
        cur = step(cur, d);
        path.push_back(cur);
    }
    return path;
}

SubCell InfantryPathGrid::fromWorld(int u, int v) {
    return {u >> 4, v >> 4};
}

std::pair<int, int> InfantryPathGrid::toWorldCenter(SubCell cell) {
    return {cell.x * 16 + 8, cell.y * 16 + 8};
}

SubCell InfantryPathGrid::step(SubCell cell, std::uint8_t dir) {
    if (dir >= 8) return cell;
    return {cell.x + kDx[dir], cell.y + kDy[dir]};
}
}
