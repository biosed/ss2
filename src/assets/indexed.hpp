#pragma once
#include "assets/map_image.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

struct AniSequence {
    std::uint32_t magic{};
    int hotX{};
    int hotY{};
    unsigned width{};
    unsigned height{};
    std::vector<MapImage> frames;
};

SpriteCoverage readShadowCoverage(int originX, int originY, std::span<const std::uint8_t> sprite);
void drawShadowFrame(MapImage& map, int originX, int originY, std::span<const std::uint8_t> sprite,
                     std::vector<std::uint8_t>* shadowMask = nullptr);
void drawIndexedFrame(MapImage&, int originX, int originY, std::span<const std::uint8_t> sprite,
                      std::span<const std::uint8_t> palette, SpriteCoverage* coverage = nullptr);
void drawAlphaIndexedFrame(MapImage& map, int originX, int originY, std::span<const std::uint8_t> sprite,
                           std::span<const std::uint8_t> palette1024);

std::size_t aniFrameCount(std::span<const std::uint8_t> ani);
std::span<const std::uint8_t> aniFrameSlice(std::span<const std::uint8_t> ani, std::size_t index);
AniSequence decodeAniSequence(std::span<const std::uint8_t> ani, std::span<const std::uint8_t> palette);
}
