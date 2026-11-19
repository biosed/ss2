#pragma once
#include "assets/map_image.hpp"
#include <filesystem>
#include <array>
#include <string>
#include <utility>

namespace ss2::assets {
struct TutorialData {
    std::vector<std::uint32_t> flags;
    std::vector<std::uint16_t> obstacles;
    std::vector<std::string> phrases;
    std::vector<std::vector<std::uint32_t>> scripts;
    std::array<std::uint8_t, 128 * 128> zones{};
    std::array<std::pair<int, int>, 64> locations{};
    std::array<std::pair<int, int>, 100> markers{};
};
TutorialData readTutorialData(const std::filesystem::path& mission);
MapImage readMissionOverview(const std::filesystem::path& mission);
}
