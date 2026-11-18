#pragma once
#include "assets/map_image.hpp"
#include <span>

namespace ss2::assets {
void drawIndexedFrame(MapImage&, int originX, int originY, std::span<const std::uint8_t> sprite,
                      std::span<const std::uint8_t> palette);
}
