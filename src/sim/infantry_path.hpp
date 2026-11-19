#pragma once
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace ss2 {
struct SubCell {
    int x{}, y{}; // 256x256 sub-cell coordinates: (u / 16, v / 16) -> tile (x>>1, y>>1), slot ((x&1) | ((y&1)<<1))
    bool operator==(const SubCell&) const = default;
};

class InfantryPathGrid {
public:
    static InfantryPathGrid loadTutorial(const std::filesystem::path& gameDir);
    static InfantryPathGrid fromSubCellMasks(int tileWidth, int tileHeight,
                                             std::vector<std::uint32_t> flags,
                                             std::vector<std::uint16_t> obstacles,
                                             int borderMargin = 4);
    static InfantryPathGrid fromWalkable(int subWidth, int subHeight, std::vector<std::uint8_t> walkable);

    int width() const { return subWidth_; }
    int height() const { return subHeight_; }
    bool inBounds(SubCell cell) const;
    bool walkable(SubCell cell) const;

    // 4-bit per-slot occupancy matching n2Game_Dll.dll 0x10036370 / 0x10056710.
    void clearOccupancy();
    void setOccupied(SubCell cell, bool occupied);
    bool occupied(SubCell cell) const;
    bool canEnter(SubCell cell) const;
    bool clearRouteTile(int tileX, int tileY) const;

    // Bounded tile-level straight-line route (n2Game_Dll.dll 0x10088780) expanded through the exact
    // 32-entry sub-cell transition table (0x100f8718 / 0x100218d0). Unverified obstacle detours are
    // rejected (returns empty). Directions are 0..7 matching 0x100fbcb4:
    // 0:(+1,0), 1:(+1,+1), 2:(0,+1), 3:(-1,+1), 4:(-1,0), 5:(-1,-1), 6:(0,-1), 7:(+1,-1).
    std::vector<std::uint8_t> findDirections(SubCell start, SubCell goal, int maxSteps = 512) const;
    std::vector<SubCell> findPath(SubCell start, SubCell goal, int maxSteps = 512) const;

    static SubCell fromWorld(int u, int v);
    static std::pair<int, int> toWorldCenter(SubCell cell);
    static SubCell step(SubCell cell, std::uint8_t dir);

private:
    int tileWidth_{}, tileHeight_{};
    int subWidth_{}, subHeight_{};
    int borderMargin_{0};
    std::vector<std::uint32_t> flags_;     // P-FLAGS (128x128)
    std::vector<std::uint16_t> obstacles_; // P-OBSTACLES (128x128)
    std::vector<std::uint8_t> walkable_;   // Optional direct sub-cell mask when constructed via fromWalkable
    std::vector<std::uint32_t> occupancy_; // 4-bit refcount per slot (bits 4*s..4*s+3)
};
}
