#pragma once
#include "assets/map_image.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ss2::assets {
struct HdMap {
    std::uint32_t scale{2};
    MapImage image;
};

// Decode a clean-room SS2H zlib-compressed HD map image from memory or a --hd-pack directory.
HdMap decodeHdMap(std::span<const std::uint8_t> bytes);
HdMap loadHdPackMap(const std::filesystem::path& hdPackDir);

// Encode helper (used by synthetic tests).
std::vector<std::uint8_t> encodeHdMap(const MapImage& image, std::uint32_t scale = 2);

// Edge-preserving 2x pixel-art upscaler (EPX / Scale2x) for dynamic moving sprite patches.
MapImage upscalePatch2x(const MapImage& src);
}
