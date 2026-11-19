#pragma once
#include "assets/map_image.hpp"
#include <array>
#include <filesystem>
#include <vector>

namespace ss2::assets {
struct InfantryPose {
    int left, top, width, height; // Logical map pixels relative to the unit's foot anchor.
    MapImage pixels;             // Premultiplied RGBA at the scene resolution.
};
using InfantryPoses = std::array<std::vector<InfantryPose>, 16>;

// Eight directions per row. One standing row followed by four walking phases.
InfantryPoses loadInfantryAtlas(const std::filesystem::path& path);
// Load a straight-alpha RGBA8 PNG (e.g., 1024x640 4x GPU infantry sprite atlas).
MapImage loadPngRgba(const std::filesystem::path& path);
}
