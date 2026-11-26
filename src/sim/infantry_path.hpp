#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ss2 {
class StateHasher;

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
    bool walkableMask(SubCell cell, std::uint16_t forbiddenMask) const;
    bool isShallows(SubCell cell) const;
    void setSubCellBlocked(SubCell cell, bool blocked);

    // 4-bit per-slot occupancy matching n2Game_Dll.dll 0x10036370 / 0x10056710.
    void clearOccupancy();
    void setOccupied(SubCell cell, bool occupied);
    bool occupied(SubCell cell) const;
    bool canEnter(SubCell cell) const;
    bool hasTerrainRoute(SubCell start, SubCell goal) const;
    std::vector<SubCell> formationDestinations(std::span<const SubCell> sources, SubCell target) const;

    // Gold infantry queue and sub-cell movement routines, 0x1008bda0 and 0x100218d0.
    // Directions 0..7 run clockwise from (+1,0) through (+1,-1).
    using SlotMask = std::vector<std::uint8_t>;
    SlotMask liveSlots() const;
    struct RouteState {
        int baselineCost{-1};
        double baselineProgress{};
        std::uint16_t wait{};
        std::uint8_t penalty{}, backoff{};
    };
    enum class PlanAction { keepQueue, replaceQueue, wait, giveUp };
    struct Plan { PlanAction action; std::vector<std::uint8_t> directions; };
    Plan plan(SubCell start, SubCell goal, std::optional<std::uint8_t> queuedDirection,
              std::span<const std::uint8_t> prioritySlots, RouteState&, std::uint8_t random) const;
    struct AcceptedStep { SubCell cell; std::uint8_t direction; bool crossesTile; };
    std::vector<std::uint8_t> findTileDirections(SubCell start, SubCell goal, int maxSteps = 512) const;
    std::vector<std::uint8_t> findTileDirectionsForMask(SubCell start, SubCell goal,
                                                        std::uint16_t forbiddenMask, int maxSteps = 256) const;
    std::vector<SubCell> findSubCellPathForMask(SubCell start, SubCell goal,
                                                std::uint16_t forbiddenMask, int maxSteps = 512) const;
    std::optional<AcceptedStep> acceptStep(SubCell from, std::uint8_t tileDirection) const;
    std::vector<std::uint8_t> findDirections(SubCell start, SubCell goal, int maxSteps = 512) const;
    std::vector<SubCell> findPath(SubCell start, SubCell goal, int maxSteps = 512) const;

    static SubCell fromWorld(int u, int v);
    static std::pair<int, int> toWorldCenter(SubCell cell);
    static SubCell step(SubCell cell, std::uint8_t dir);

    // Feeds terrain and occupancy into a state hash (tutorial_units_hash.cpp).
    void hashState(StateHasher&) const;

private:
    bool formationTile(int tileX, int tileY, SubCell source) const;
    struct Search {
        std::vector<std::uint8_t> directions;
        SubCell endTile;
        double cost{};
    };
    Search searchTiles(SubCell start, SubCell goal, int maxSteps, bool useOccupancy,
                       std::span<const std::uint8_t> prioritySlots) const;
    bool canCrossTile(SubCell start, int ax, int ay, int bx, int by, std::uint8_t direction,
                      bool useOccupancy, std::span<const std::uint8_t> slots) const;
    std::uint8_t occupiedSlots(int tileX, int tileY) const;
    int tileWidth_{}, tileHeight_{};
    int subWidth_{}, subHeight_{};
    int borderMargin_{0};
    std::vector<std::uint32_t> flags_;     // P-FLAGS (tileWidth x tileHeight)
    std::vector<std::uint16_t> obstacles_; // P-OBSTACLES (tileWidth x tileHeight)
    std::vector<std::uint8_t> walkable_;   // Optional direct sub-cell mask when constructed via fromWalkable
    std::vector<std::uint32_t> occupancy_; // 4-bit refcount per slot (bits 4*s..4*s+3)
};
}
