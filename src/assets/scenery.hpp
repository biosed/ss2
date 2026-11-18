#pragma once
#include "assets/map_image.hpp"
#include <filesystem>

namespace ss2::assets {
std::size_t drawTutorialScenery(MapImage&, const std::filesystem::path& gameDir);
}
