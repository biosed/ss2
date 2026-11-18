#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace ss2::assets {
struct Placement {
    std::string id;
    int x, y, direction, hp, ammo, group, owner;
};
std::vector<Placement> readTutorialPlacements(const std::filesystem::path& gameDir);
}
