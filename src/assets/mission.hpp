#pragma once
#include "assets/map_image.hpp"
#include <filesystem>

namespace ss2::assets {
MapImage readMissionOverview(const std::filesystem::path& mission);
}
