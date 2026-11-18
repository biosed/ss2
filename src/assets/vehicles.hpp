#pragma once
#include "assets/map_image.hpp"
#include "assets/placement.hpp"
#include <span>
#include <cstddef>
#include <filesystem>

namespace ss2::assets {
std::vector<Placement> drawTutorialVehicles(MapImage&, const std::filesystem::path& gameDir, std::span<const Placement>);
}
