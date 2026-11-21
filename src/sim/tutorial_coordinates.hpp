#pragma once
#include <utility>

namespace ss2 {
// Gold 0x1009d7b6 halves the horizontal component before adding or
// subtracting it. On odd columns this determines which tile gets the order.
constexpr std::pair<int,int> tutorialPointerWorld(int x, int y, int originX = 4096) {
    const int halfX = (x-originX) >> 1;
    return {y+halfX,y-halfX};
}

// The inverse of a rendered foot at x=originX+U-V, y=floor((U+V)/2).
constexpr std::pair<int,int> tutorialFootWorld(int x, int y, int originX = 4096) {
    const auto [u,v] = tutorialPointerWorld(x,y,originX);
    return {u+(x&1),v};
}
}
