#pragma once
#include "assets/map_image.hpp"
#include <filesystem>
#include <string>

namespace ss2::assets {
struct Infantry {
    std::string id;
    int x, y, hp, ammo, group, owner;
    int left, top, width, height;
};

std::vector<Infantry> placeTutorialInfantry(MapImage&, const std::filesystem::path& gameDir);
}
