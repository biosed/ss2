#pragma once
#include "assets/infantry.hpp"
#include "sim/infantry_path.hpp"
#include "sim/infantry_step.hpp"
#include <optional>

namespace ss2 {
struct SoldierPose {
    size_t index;
    int x, y;
    unsigned direction, progress;
    bool walking;
};

class TutorialUnits {
public:
    TutorialUnits(InfantryPathGrid, std::span<const assets::Infantry>, std::span<const assets::Placement> vehicles);
    bool canMove(size_t index, int mapX, int mapY) const;
    bool order(size_t index, int mapX, int mapY);
    std::vector<SoldierPose> step();
    unsigned countInZone(std::span<const std::uint8_t> zones, unsigned zone) const;
private:
    struct Soldier {
        SubCell cell;
        int owner;
        std::vector<std::uint8_t> route;
        size_t cursor{};
        std::optional<InfantryStep> motion;
    };
    InfantryPathGrid grid_;
    std::vector<Soldier> soldiers_;
    std::vector<SubCell> playerVehicles_;
    std::uint32_t randomState_{0x76321f};
    std::vector<std::uint8_t> directRoute(size_t index, int x, int y) const;
    bool startNext(Soldier&);
};
}
