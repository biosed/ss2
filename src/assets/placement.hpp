#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace ss2::assets {
struct PassengerPlacement {
    std::string id;
    int group{0}, hp{100}, ammo{100}, expa{0};
    bool isCrew{false};
    bool operator==(const PassengerPlacement&) const = default;
};
struct Placement {
    std::string id;
    int x, y, direction, hp, ammo, group, owner;
    std::vector<PassengerPlacement> passengers{};
    int expa{0};
};
std::vector<Placement> readTutorialPlacements(const std::filesystem::path& gameDir);
std::vector<Placement> readMissionPlacements(const std::filesystem::path& missionPath);
}
