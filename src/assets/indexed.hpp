#pragma once
#include "assets/map_image.hpp"
#include <span>

namespace ss2::assets {
struct SpriteCoverage {
    int left{}, top{}, width{}, height{};
    std::vector<std::uint8_t> pixels;
    bool covers(int x, int y) const {
        x -= left;
        y -= top;
        return x >= 0 && x < width && y >= 0 && y < height && pixels[std::size_t(y)*width+x];
    }
};
SpriteCoverage readShadowCoverage(int originX, int originY, std::span<const std::uint8_t> sprite);
void drawIndexedFrame(MapImage&, int originX, int originY, std::span<const std::uint8_t> sprite,
                      std::span<const std::uint8_t> palette, SpriteCoverage* coverage = nullptr);
}
