#pragma once

#include <cstdint>
#include <vector>

namespace ss2::assets {
struct MapImage {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
}
