#pragma once
#include "assets/map_image.hpp"
#include <cstdint>
#include <filesystem>
#include <span>

namespace ss2::assets {
MapImage decodeTerrain(std::span<const std::uint8_t> info, std::span<const std::uint8_t> rhombs,
                       std::span<const std::uint8_t> rhombsRes, std::span<const std::uint8_t> rhombsPl);
MapImage loadTutorialTerrain(const std::filesystem::path& gameDir, unsigned scheme = 0);
MapImage loadMissionTerrain(const std::filesystem::path& gameDir, const std::filesystem::path& missionPath);
}
