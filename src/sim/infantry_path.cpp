#include "sim/infantry_path.hpp"
#include "assets/mission.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ss2 {
namespace {
constexpr int kMaxTileSide = 512;
constexpr std::uint16_t kInfantryForbiddenMask = 0x00F0u; // walkonground=1, walkonwater=0, walkonshallows=1 (0x10061d25)

// n2Game_Dll.dll 0x100fbcb4: 8-direction table.
constexpr std::array<int, 8> kDx{1, 1, 0, -1, -1, -1, 0, 1};
constexpr std::array<int, 8> kDy{0, 1, 1, 1, 0, -1, -1, -1};

// n2Game_Dll.dll 0x100f8710: sub-cell slot XOR flip mask per direction.
constexpr std::array<std::uint8_t, 8> kXorMask{1, 3, 2, 3, 1, 3, 2, 3};

// n2Game_Dll.dll 0x100fbc88: 4-bit non-edge mask per direction (used by infantry checker 0x1008ba40 / 0x1008bc10).
constexpr std::array<std::uint8_t, 8> kNonEdgeMask{0x5u, 0x7u, 0x3u, 0xBu, 0xAu, 0xEu, 0xCu, 0xDu};

// n2Game_Dll.dll 0x100fbc68: 4-bit exit-edge mask per direction (~kNonEdgeMask[d] & 0xF, used by 0x1008f6d0).
constexpr std::array<std::uint8_t, 8> kExitEdgeMask{0xAu, 0x8u, 0xCu, 0x4u, 0x5u, 0x1u, 0x3u, 0x2u};

// n2Game_Dll.dll 0x1008be20 back-trace ray order.
struct RayDir {
    int dx, dy;
    std::uint8_t dir;
};
constexpr std::array<RayDir, 8> kBacktraceRays{{
    {-1, -1, 5}, {-1, 0, 4}, {-1, 1, 3},
    {1, -1, 7},  {1, 0, 0},  {1, 1, 1},
    {0, -1, 6},  {0, 1, 2},
}};

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
} // namespace

InfantryPathGrid InfantryPathGrid::loadTutorial(const std::filesystem::path& gameDir) {
    const auto mission = assets::resolveCaseInsensitivePath(gameDir, "maps/single/--- TUTORIAL I ---.SSM");
    auto data = assets::readTutorialData(mission);
    return fromSubCellMasks(int(data.width), int(data.height), std::move(data.flags), std::move(data.obstacles));
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

bool InfantryPathGrid::walkableMask(SubCell cell, std::uint16_t forbiddenMask) const {
    if (!inBounds(cell)) return false;
    if (!walkable_.empty())
        return walkable_[std::size_t(cell.y) * std::size_t(subWidth_) + std::size_t(cell.x)] != 0;

    const std::size_t idx = std::size_t(cell.y >> 1) * std::size_t(tileWidth_) + std::size_t(cell.x >> 1);
    const unsigned slot = unsigned((cell.x & 1) | ((cell.y & 1) << 1));
    const unsigned slotBit = 1u << slot;

    const std::uint32_t pf = flags_[idx];
    if ((pf & 0x4000u) != 0 || ((pf & slotBit) & 0xFu) != 0) return false;
    const std::uint16_t obst = obstacles_[idx] & forbiddenMask;
    const unsigned blocked = unsigned(obst | (obst >> 4) | (obst >> 8) | (obst >> 12)) & 0xFu;
    return (blocked & slotBit) == 0;
}

bool InfantryPathGrid::isShallows(SubCell cell) const {
    if (!inBounds(cell) || obstacles_.empty()) return false;
    const std::size_t idx = std::size_t(cell.y >> 1) * std::size_t(tileWidth_) + std::size_t(cell.x >> 1);
    return idx < obstacles_.size() && (obstacles_[idx] & 0x0F00u) != 0;
}

void InfantryPathGrid::setSubCellBlocked(SubCell cell, bool blocked) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= subWidth_ || cell.y >= subHeight_) return;
    if (!walkable_.empty()) {
        walkable_[std::size_t(cell.y) * std::size_t(subWidth_) + std::size_t(cell.x)] = blocked ? 0u : 1u;
        return;
    }
    const std::size_t idx = std::size_t(cell.y >> 1) * std::size_t(tileWidth_) + std::size_t(cell.x >> 1);
    const std::uint32_t slotBit = 1u << unsigned((cell.x & 1) | ((cell.y & 1) << 1));
    if (idx >= flags_.size()) return;
    if (blocked) flags_[idx] |= slotBit;
    else flags_[idx] &= ~slotBit;
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

bool InfantryPathGrid::formationTile(int x, int y, SubCell source) const {
    if (!inBounds({x*2,y*2})) return false;
    if (flags_.empty()) {
        for (int slot = 0; slot < 4; ++slot)
            if (walkable({x*2+(slot&1), y*2+(slot>>1)})) return true;
        return false;
    }
    // 0x10056370, infantry flag bit 21. A candidate with any unoccupied
    // static slot passes immediately. For a full tile, the water mask decides.
    const auto tile = std::size_t(y)*tileWidth_+x;
    if (flags_[tile] & 0x4000u) return false;
    auto live = occupiedSlots(x,y);
    if (source.x/2 == x && source.y/2 == y)
        live &= ~(1u << ((source.x&1) + 2*(source.y&1)));
    return ((live | flags_[tile]) & 15u) != 15u || !(obstacles_[tile] & kInfantryForbiddenMask);
}

std::vector<SubCell> InfantryPathGrid::formationDestinations(
        std::span<const SubCell> sources, SubCell target) const {
    if (sources.empty() || !inBounds(target)) return {};
    // Standard ground order 0x100866c0, group centroid and spread at 0x100845b0.
    int minX=tileWidth_, maxX=0, minY=tileHeight_, maxY=0, sumX=0, sumY=0;
    for (const auto cell : sources) {
        if (!inBounds(cell)) return {};
        const int x = cell.x/2, y = cell.y/2;
        minX = std::min(minX,x); maxX = std::max(maxX,x);
        minY = std::min(minY,y); maxY = std::max(maxY,y);
        sumX += x; sumY += y;
    }
    const int dx=maxX-minX, dy=maxY-minY;
    const bool compact = std::max(dx,dy) + std::min(dx,dy)/2 < 21;
    const int goalX = target.x/2, goalY = target.y/2;
    const int centerX = sumX/int(sources.size()), centerY = sumY/int(sources.size());
    std::array<unsigned char,32*32> reserved{};
    if (!compact) {
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                if ((x|y)&1) reserved[y*32+x] = 4;
    }
    const auto fallback = [&](SubCell source) {
        // 0x10084640 sweeps the four sides in this order. An ineligible tile
        // stops the scan along that side. Each fallback tile takes four infantry.
        const auto scan = [&](int x, int y, int stepX, int stepY, int count) -> std::optional<SubCell> {
            for (int i = 0; i < count; ++i, x += stepX, y += stepY) {
                const SubCell tile{goalX+x-15, goalY+y-15};
                if (!formationTile(tile.x,tile.y,source)) break;
                auto& slots = reserved[y*32+x];
                if (slots < 4) {
                    ++slots;
                    return tile;
                }
            }
            return {};
        };
        for (int radius = 0; radius < 15; ++radius) {
            const int lo = 15-radius, hi = 15+radius, span = 2*radius+2;
            if (auto t = scan(lo,lo, 1,0,span)) return *t;
            if (auto t = scan(lo,lo, 0,1,span-1)) return *t;
            if (auto t = scan(hi,hi,-1,0,span)) return *t;
            if (auto t = scan(hi,hi, 0,-1,span)) return *t;
        }
        return SubCell{goalX,goalY};
    };

    std::vector<SubCell> result;
    result.reserve(sources.size());
    for (auto source : sources) {
        SubCell tile{goalX + source.x/2-centerX, goalY + source.y/2-centerY};
        if (!compact || !formationTile(tile.x,tile.y,source)) tile = fallback(source);
        result.push_back({2*tile.x,2*tile.y});
    }
    return result;
}

std::uint8_t InfantryPathGrid::occupiedSlots(int tx, int ty) const {
    const std::uint32_t occ = occupancy_[std::size_t(ty) * std::size_t(tileWidth_) + std::size_t(tx)];
    std::uint8_t m = 0;
    if (occ & 0x000Fu) m |= 1u;
    if (occ & 0x00F0u) m |= 2u;
    if (occ & 0x0F00u) m |= 4u;
    if (occ & 0xF000u) m |= 8u;
    return m;
}

InfantryPathGrid::SlotMask InfantryPathGrid::liveSlots() const {
    SlotMask result(std::size_t(tileWidth_) * tileHeight_);
    for (int y = 0; y < tileHeight_; ++y)
        for (int x = 0; x < tileWidth_; ++x)
            result[std::size_t(y)*tileWidth_+x] = occupiedSlots(x, y);
    return result;
}

bool InfantryPathGrid::canCrossTile(SubCell start, int ax, int ay, int bx, int by,
        std::uint8_t d, bool useOccupancy, std::span<const std::uint8_t> slots) const {
    const int startTx = start.x/2, startTy = start.y/2;
    const unsigned selfSlotBit = 1u << ((start.x&1) + 2*(start.y&1));
    const auto tileBlocked4 = [&](int tx, int ty, bool useOccupancy) -> std::uint8_t {
        std::uint8_t blk = 0;
        for (int s = 0; s < 4; ++s) {
            if (!walkable({tx * 2 + (s & 1), ty * 2 + (s >> 1)})) blk |= static_cast<std::uint8_t>(1u << s);
        }
        if (useOccupancy) blk |= slots.empty() ? occupiedSlots(tx, ty) : slots[std::size_t(ty)*tileWidth_ + tx];
        return blk;
    };

    if (bx == startTx && by == startTy) {
        std::swap(ax, bx);
        std::swap(ay, by);
        d = static_cast<std::uint8_t>((d - 4) & 7);
    }
    if (!inBounds({bx * 2, by * 2})) return false;
    if (!flags_.empty() && (flags_[std::size_t(by) * std::size_t(tileWidth_) + std::size_t(bx)] & 0x4000u) != 0)
        return false;

    const std::uint8_t exitMask = kNonEdgeMask[d];
    const std::uint8_t enterMask = kNonEdgeMask[(d - 4) & 7];

    std::uint8_t srcBlk = tileBlocked4(ax, ay, useOccupancy);
    if (ax == startTx && ay == startTy) srcBlk &= static_cast<std::uint8_t>(~selfSlotBit);
    if ((srcBlk | exitMask) == 0xFu) return false;

    std::uint8_t dstBlk = tileBlocked4(bx, by, useOccupancy);
    if (dstBlk == 0u) return true;
    if (bx == startTx && by == startTy) dstBlk &= static_cast<std::uint8_t>(~selfSlotBit);
    return (dstBlk | enterMask) != 0xFu;
}

InfantryPathGrid::Search InfantryPathGrid::searchTiles(SubCell start, SubCell goal, int maxSteps,
        bool usePriority, std::span<const std::uint8_t> slots) const {
    Search result{{}, {start.x/2, start.y/2}, 0.0};
    if (maxSteps <= 0 || !inBounds(start) || !inBounds(goal)) return result;

    const int startTx = start.x >> 1;
    const int startTy = start.y >> 1;
    const int goalTx = goal.x >> 1;
    const int goalTy = goal.y >> 1;
    if (startTx == goalTx && startTy == goalTy) return result;

    const unsigned startSlot = unsigned((start.x & 1) | ((start.y & 1) << 1));
    const std::uint8_t selfSlotBit = static_cast<std::uint8_t>(1u << startSlot);

    const auto infantryTileAllowed = [&](int ax, int ay, int bx, int by, std::uint8_t d, bool dynamic) {
        return canCrossTile(start, ax, ay, bx, by, d, dynamic, slots);
    };

    // Exact n2Game_Dll.dll 0x1008be20 (Bug2 deque search + 8-ray distGrid back-trace reconstruction).
    const auto runGoldInfantrySearch = [&](bool useOccupancy, int& outEndTx, int& outEndTy) -> std::vector<std::uint8_t> {
        const int W = tileWidth_;
        const int H = tileHeight_;
        const std::size_t N = std::size_t(W) * std::size_t(H);
        const auto idxOf = [&](int tx, int ty) -> std::size_t { return std::size_t(ty) * std::size_t(W) + std::size_t(tx); };

        std::vector<std::uint16_t> rayIdx(N, 0u);
        std::vector<std::uint16_t> distGrid(N, 0u);
        std::vector<std::uint8_t> mode0Visited(N, 0u);
        std::vector<std::uint16_t> wallVisited(N, 0u);

        // Pre-pass: mark straight greedy ray indices 1, 2, 3, ...
        {
            int rx = startTx, ry = startTy;
            std::uint16_t mark = 1u;
            rayIdx[idxOf(rx, ry)] = mark++;
            while (rx != goalTx || ry != goalTy) {
                const std::uint8_t d = directionFromDelta(goalTx - rx, goalTy - ry);
                rx += kDx[d];
                ry += kDy[d];
                if (rx < 0 || ry < 0 || rx >= W || ry >= H) break;
                rayIdx[idxOf(rx, ry)] = mark++;
            }
        }

        struct Agent {
            int tx, ty;
            std::uint8_t dir;
            int dist;
            int mode; // 0 = ray march, 1 = CCW wall follow, 2 = CW wall follow
            std::uint16_t rayMark;
        };
        std::deque<Agent> q;
        const auto pushBack = [&](const Agent& a) { q.push_back(a); };
        const auto pushFront = [&](const Agent& a) { q.push_front(a); };

        distGrid[idxOf(startTx, startTy)] = 2u;
        pushBack({startTx, startTy, directionFromDelta(goalTx - startTx, goalTy - startTy), 2, 0, 1u});

        int bestScore = std::numeric_limits<int>::max();
        int bestTx = startTx, bestTy = startTy;

        const auto canStep = [&](int tx, int ty, std::uint8_t d) -> bool {
            const int nx = tx + kDx[d];
            const int ny = ty + kDy[d];
            if (!infantryTileAllowed(tx, ty, nx, ny, d, useOccupancy)) return false;
            if ((d & 1u) != 0) {
                if (rayIdx[idxOf(nx, ty)] != 0u && rayIdx[idxOf(tx, ny)] != 0u) return false;
            }
            return true;
        };
        const auto canStartWallTurn = [&](int tx, int ty, std::uint8_t candidate) {
            const int nx = tx+kDx[candidate], ny = ty+kDy[candidate];
            // 0x1006b2ec marks the map perimeter with bit 30. Before rotating,
            // both wall followers test that bit at X*512+Y (0x1008c6ae).
            if (!inBounds({2*nx,2*ny})) return false;
            return flags_.empty() || nx >= H || ny >= W ||
                   !(flags_[std::size_t(nx)*W+ny] & 0x40000000u);
        };

        int iterGuard = 0;
        const int kMaxIters = std::max(16384, int(N));
        while (!q.empty() && ++iterGuard <= kMaxIters) {
            const Agent cur = q.front();
            q.pop_front();
            if (cur.dist >= 0xfff) continue;

            if (cur.tx == goalTx && cur.ty == goalTy) {
                bestTx = cur.tx;
                bestTy = cur.ty;
                break;
            }

            const int dx = goalTx-cur.tx, dy = goalTy-cur.ty;
            int score = dx*dx + dy*dy + (cur.dist >> 2);
            if (std::abs(dx) >= 2 || std::abs(dy) >= 2) {
                score += 16;
            } else {
                // 0x1008c1de preserves the upper obstacle nibble in this particular
                // comparison. A usable adjacent tile gets preference as a fallback.
                const auto i = idxOf(cur.tx,cur.ty);
                const unsigned obstacle = obstacles_.empty() ? 0u : obstacles_[i] & kInfantryForbiddenMask;
                unsigned blocked = obstacle | (obstacle >> 4) | (flags_.empty() ? 0u : flags_[i] & 15u);
                if (useOccupancy) blocked |= slots.empty() ? occupiedSlots(cur.tx,cur.ty) : slots[i];
                if (cur.tx == startTx && cur.ty == startTy) blocked &= ~unsigned(selfSlotBit);
                if ((blocked | kNonEdgeMask[directionFromDelta(dx,dy)]) == 255u) score += 16;
            }
            if (score < bestScore) {
                bestScore = score;
                bestTx = cur.tx;
                bestTy = cur.ty;
            }

            const std::size_t cIdx = idxOf(cur.tx, cur.ty);
            if (cur.mode == 0) {
                if (mode0Visited[cIdx] == 0u) {
                    mode0Visited[cIdx] = 1u;
                    const std::uint8_t d = directionFromDelta(goalTx - cur.tx, goalTy - cur.ty);
                    if (d < 8 && canStep(cur.tx, cur.ty, d)) {
                        const int nx = cur.tx + kDx[d];
                        const int ny = cur.ty + kDy[d];
                        if (distGrid[idxOf(nx, ny)] == 0u) distGrid[idxOf(nx, ny)] = static_cast<std::uint16_t>(cur.dist + 1);
                        pushBack({nx, ny, d, cur.dist + 1, 0, cur.rayMark});
                    } else if (d < 8) {
                        const std::uint16_t rm = rayIdx[cIdx];
                        pushFront({cur.tx, cur.ty, static_cast<std::uint8_t>((d + 3) & 7), cur.dist, 1, rm});
                        pushFront({cur.tx, cur.ty, static_cast<std::uint8_t>((d - 3) & 7), cur.dist, 2, rm});
                    }
                }
            } else if (cur.mode == 1) {
                std::uint8_t cand = static_cast<std::uint8_t>((cur.dir - 3) & 7);
                if (!canStartWallTurn(cur.tx,cur.ty,cand)) continue;
                const std::uint16_t bit = static_cast<std::uint16_t>(1u << cand);
                if ((wallVisited[cIdx] & bit) == 0u) {
                    wallVisited[cIdx] |= bit;
                    int tries = 0;
                    for (; tries < 10; ++tries) {
                        if (canStep(cur.tx, cur.ty, cand)) break;
                        cand = static_cast<std::uint8_t>((cand + 1) & 7);
                    }
                    if (tries < 8) {
                        const int nx = cur.tx + kDx[cand];
                        const int ny = cur.ty + kDy[cand];
                        const std::size_t nIdx = idxOf(nx, ny);
                        if (distGrid[nIdx] == 0u) distGrid[nIdx] = static_cast<std::uint16_t>(cur.dist + 1);
                        if (rayIdx[nIdx] > cur.rayMark) {
                            pushBack({nx, ny, cand, cur.dist + 1, 0, cur.rayMark});
                        } else {
                            pushBack({nx, ny, cand, cur.dist + 1, 1, cur.rayMark});
                        }
                    }
                }
            } else {
                std::uint8_t cand = static_cast<std::uint8_t>((cur.dir + 3) & 7);
                if (!canStartWallTurn(cur.tx,cur.ty,cand)) continue;
                const std::uint16_t bit = static_cast<std::uint16_t>(1u << (cand + 8));
                if ((wallVisited[cIdx] & bit) == 0u) {
                    wallVisited[cIdx] |= bit;
                    int tries = 0;
                    for (; tries < 10; ++tries) {
                        if (canStep(cur.tx, cur.ty, cand)) break;
                        cand = static_cast<std::uint8_t>((cand - 1) & 7);
                    }
                    if (tries < 8) {
                        const int nx = cur.tx + kDx[cand];
                        const int ny = cur.ty + kDy[cand];
                        const std::size_t nIdx = idxOf(nx, ny);
                        if (distGrid[nIdx] == 0u) distGrid[nIdx] = static_cast<std::uint16_t>(cur.dist + 1);
                        if (rayIdx[nIdx] > cur.rayMark) {
                            pushBack({nx, ny, cand, cur.dist + 1, 0, cur.rayMark});
                        } else {
                            pushBack({nx, ny, cand, cur.dist + 1, 2, cur.rayMark});
                        }
                    }
                }
            }
        }

        outEndTx = bestTx;
        outEndTy = bestTy;
        if (bestTx == startTx && bestTy == startTy) return {};

        // Exact 8-ray back-trace reconstruction from (bestTx, bestTy) -> (startTx, startTy) (0x1008be20).
        std::vector<std::uint8_t> rev;
        rev.reserve(64);
        int cx = bestTx, cy = bestTy;
        int btGuard = 0;
        while ((cx != startTx || cy != startTy) && ++btGuard <= maxSteps) {
            int bestDist = 0x7fff;
            int bestDx = 0, bestDy = 0;
            for (const auto& ray : kBacktraceRays) {
                int rx = cx, ry = cy;
                while (true) {
                    const int nx = rx + ray.dx;
                    const int ny = ry + ray.dy;
                    if (!infantryTileAllowed(rx, ry, nx, ny, ray.dir, useOccupancy)) break;
                    const std::uint16_t dval = distGrid[idxOf(nx, ny)];
                    if (dval > 0u) {
                        if (int(dval) < bestDist) {
                            bestDist = int(dval);
                            bestDx = ray.dx;
                            bestDy = ray.dy;
                        }
                        break;
                    }
                    rx = nx;
                    ry = ny;
                }
            }
            if (bestDx == 0 && bestDy == 0) return {};
            const std::uint8_t fwd = directionFromDelta(-bestDx, -bestDy);
            if (fwd >= 8) return {};
            rev.push_back(fwd);
            cx += bestDx;
            cy += bestDy;
        }
        if (cx != startTx || cy != startTy) return {};
        for (auto d : rev) result.cost += 1.0 + ((d & 1) ? 0.4142135623 : 0.0);
        std::reverse(rev.begin(), rev.end());
        if (rev.size() > 61u) rev.resize(61u); // matches 0x3c cap in 0x1008be20
        return rev;
    };

    // Exact 0x1008f6d0 Pass 2 (useOccupancy=true) + adjacent final-step append (lines 117-158).
    int endTx = startTx, endTy = startTy;
    auto tileDirs = runGoldInfantrySearch(usePriority, endTx, endTy);
    result.endTile = {endTx, endTy};
    if (tileDirs.empty()) return result;

    if (usePriority && std::abs(goalTx - endTx) < 2 && std::abs(goalTy - endTy) < 2 &&
        (endTx != goalTx || endTy != goalTy)) {
        const std::uint8_t d = directionFromDelta(goalTx - endTx, goalTy - endTy);
        if (d < 8 && infantryTileAllowed(endTx, endTy, goalTx, goalTy, d, false)) {
            const std::uint8_t occ = occupiedSlots(endTx, endTy);
            const std::uint8_t edge = kExitEdgeMask[d];
            if ((occ & edge) != edge && (selfSlotBit & edge) == 0u && tileDirs.size() < 63u) {
                tileDirs.push_back(d);
                result.cost += 1.0;
                result.endTile = {goalTx, goalTy};
            }
        }
    }

    result.directions = std::move(tileDirs);
    return result;
}

bool InfantryPathGrid::hasTerrainRoute(SubCell start, SubCell goal) const {
    if (!inBounds(start) || !inBounds(goal)) return false;
    if (start.x/2 == goal.x/2 && start.y/2 == goal.y/2) return true;
    return !searchTiles(start, goal, 512, false, {}).directions.empty();
}

std::vector<std::uint8_t> InfantryPathGrid::findTileDirections(SubCell start, SubCell goal, int maxSteps) const {
    return searchTiles(start, goal, maxSteps, true, {}).directions;
}

InfantryPathGrid::Plan InfantryPathGrid::plan(SubCell start, SubCell goal,
        std::optional<std::uint8_t> queued, std::span<const std::uint8_t> prioritySlots,
        RouteState& state, std::uint8_t random) const {
    if (!inBounds(start) || !inBounds(goal) ||
        (!prioritySlots.empty() && prioritySlots.size() != occupancy_.size()))
        throw std::invalid_argument("Invalid infantry planning grid");
    const int x = start.x/2, y = start.y/2, gx = goal.x/2, gy = goal.y/2;
    // The original square root (0x1007f0a0) makes five Newton iterations,
    // including for zero distance. Its final value enters the route-quality test.
    const auto remaining = [&](int tx, int ty) {
        const int squared = (tx-gx)*(tx-gx) + (ty-gy)*(ty-gy);
        double estimate = 1.0;
        while (estimate*estimate < squared) estimate *= 2.0;
        for (int i = 0; i < 5; ++i) estimate = (estimate*estimate+squared)/(2.0*estimate);
        return estimate;
    };
    const auto progress = [&](const Search& s) {
        return int(s.cost) ? (remaining(x,y) - remaining(s.endTile.x,s.endTile.y) + 6.0)/(s.cost+6.0) : 0.0;
    };
    if (state.baselineCost < 0) {
        const auto baseline = searchTiles(start, goal, 512, false, {});
        if (baseline.directions.empty()) return {PlanAction::giveUp, {}};
        state.baselineCost = int(baseline.cost);
        state.baselineProgress = progress(baseline);
    }
    if (queued && *queued < 8) {
        const auto d = *queued;
        const int nx = x+kDx[d], ny = y+kDy[d];
        if (!canCrossTile(start,x,y,nx,ny,d,true,{}) &&
             canCrossTile(start,x,y,nx,ny,d,true,prioritySlots)) {
            state.wait += random & 3;
            return {PlanAction::keepQueue, {}};
        }
    }
    state.wait = 0;
    auto route = searchTiles(start, goal, 512, true, prioritySlots);
    const double ratio = (state.baselineCost+32.0)/(route.cost+32.0);
    if (!route.directions.empty() && ratio >= 0.5 && progress(route) >= 0.5*state.baselineProgress) {
        state.penalty = state.backoff = 0;
        return {PlanAction::replaceQueue, std::move(route.directions)};
    }
    // 0x1008f6d0 gives a nearby destination extra penalty if its direct line is blocked.
    if ((x-gx)*(x-gx) + (y-gy)*(y-gy) < 256 && route.cost < 16.0) {
        int tx = x, ty = y;
        while (tx != gx || ty != gy) {
            const auto d = directionFromDelta(gx-tx,gy-ty);
            const int nx = tx+kDx[d], ny = ty+kDy[d];
            if (!canCrossTile(start,tx,ty,nx,ny,d,true,{})) {
                state.penalty += 40;
                break;
            }
            tx = nx;
            ty = ny;
        }
    }
    state.backoff = state.penalty < 20 ? random & 3 : 25 + (random & 7);
    state.penalty += ratio < 0.5 ? 11 : 1;
    return {state.penalty > 150 ? PlanAction::giveUp : PlanAction::wait, {}};
}

std::optional<InfantryPathGrid::AcceptedStep> InfantryPathGrid::acceptStep(
        SubCell from, std::uint8_t tileDirection) const {
    if (!inBounds(from) || tileDirection >= 8) return {};
    const unsigned slot = unsigned((from.x & 1) | ((from.y & 1) << 1));
    const auto transition = kSubTable[slot][tileDirection];
    const int tileX = from.x/2 + (transition.crossCell ? kDx[tileDirection] : 0);
    const int tileY = from.y/2 + (transition.crossCell ? kDy[tileDirection] : 0);
    const auto tryDirection = [&](std::uint8_t direction) -> std::optional<AcceptedStep> {
        const auto nextSlot = slot ^ kXorMask[direction];
        const SubCell next{2*tileX + int(nextSlot&1), 2*tileY + int(nextSlot>>1)};
        if (!canEnter(next)) return {};
        return AcceptedStep{next, direction, transition.crossCell != 0};
    };
    if (auto next = tryDirection(transition.primaryDir)) return next;
    if (!(tileDirection & 1) && transition.altDir < 8) return tryDirection(transition.altDir);
    return {};
}

std::vector<std::uint8_t> InfantryPathGrid::findDirections(SubCell start, SubCell goal, int maxSteps) const {
    const auto queue = findTileDirections(start, goal, maxSteps);
    std::vector<std::uint8_t> steps;
    for (size_t i = 0; i < queue.size();) {
        if (int(steps.size()) >= maxSteps) return {};
        const auto next = acceptStep(start, queue[i]);
        if (!next) break;
        steps.push_back(next->direction);
        start = next->cell;
        if (next->crossesTile) ++i;
    }
    return steps;
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

std::vector<std::uint8_t> InfantryPathGrid::findTileDirectionsForMask(
        SubCell start, SubCell goal, std::uint16_t forbiddenMask, int maxSteps) const {
    if (forbiddenMask == kInfantryForbiddenMask) {
        return findTileDirections(start, goal, maxSteps);
    }
    if (!inBounds(start) || !inBounds(goal) || maxSteps <= 0) return {};
    const int startTx = start.x >> 1;
    const int startTy = start.y >> 1;
    const int goalTx = goal.x >> 1;
    const int goalTy = goal.y >> 1;
    if (startTx == goalTx && startTy == goalTy) return {};

    auto tilePassable = [&](int tx, int ty) -> bool {
        if (tx < 0 || ty < 0 || tx >= tileWidth_ || ty >= tileHeight_) return false;
        for (int s = 0; s < 4; ++s) {
            if (walkableMask({tx * 2 + (s & 1), ty * 2 + (s >> 1)}, forbiddenMask)) return true;
        }
        return false;
    };
    if (!tilePassable(goalTx, goalTy)) return {};

    const std::size_t n = std::size_t(tileWidth_) * std::size_t(tileHeight_);
    std::vector<std::uint8_t> parentDir(n, 255u);
    std::deque<int> q;
    const int startIdx = startTy * tileWidth_ + startTx;
    const int goalIdx = goalTy * tileWidth_ + goalTx;
    parentDir[static_cast<std::size_t>(startIdx)] = 254u;
    q.push_back(startIdx);

    bool found = false;
    while (!q.empty()) {
        const int cur = q.front();
        q.pop_front();
        if (cur == goalIdx) {
            found = true;
            break;
        }
        const int cx = cur % tileWidth_;
        const int cy = cur / tileWidth_;
        for (std::uint8_t d = 0; d < 8; ++d) {
            const int nx = cx + kDx[d];
            const int ny = cy + kDy[d];
            if (!tilePassable(nx, ny)) continue;
            if (kDx[d] != 0 && kDy[d] != 0) {
                if (!tilePassable(cx + kDx[d], cy) || !tilePassable(cx, cy + kDy[d])) continue;
            }
            const int nIdx = ny * tileWidth_ + nx;
            if (parentDir[static_cast<std::size_t>(nIdx)] != 255u) continue;
            parentDir[static_cast<std::size_t>(nIdx)] = d;
            q.push_back(nIdx);
        }
    }
    if (!found) return {};

    std::vector<std::uint8_t> dirs;
    int cur = goalIdx;
    while (cur != startIdx) {
        const std::uint8_t d = parentDir[static_cast<std::size_t>(cur)];
        if (d >= 8u) break;
        dirs.push_back(d);
        const int cx = cur % tileWidth_;
        const int cy = cur / tileWidth_;
        cur = (cy - kDy[d]) * tileWidth_ + (cx - kDx[d]);
    }
    std::reverse(dirs.begin(), dirs.end());
    if (int(dirs.size()) > maxSteps) dirs.resize(static_cast<std::size_t>(maxSteps));
    return dirs;
}
} // namespace ss2
