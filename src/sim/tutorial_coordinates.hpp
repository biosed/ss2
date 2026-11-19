#pragma once
#include <utility>

namespace ss2 {
// Gold 0x1009d7b6 halves the horizontal component before adding or
// subtracting it. On odd columns this determines which tile gets the order.
constexpr std::pair<int,int> tutorialPointerWorld(int x, int y) {
    const int halfX = (x-4096) >> 1;
    return {y+halfX,y-halfX};
}

// The inverse of a rendered foot at x=4096+U-V, y=floor((U+V)/2).
constexpr std::pair<int,int> tutorialFootWorld(int x, int y) {
    const auto [u,v] = tutorialPointerWorld(x,y);
    return {u+(x&1),v};
}
}
