#include "sim/infantry_step.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace ss2 {
std::pair<int, int> InfantryStep::position() const {
    static constexpr std::array<std::pair<int,int>, 8> directions{{
        {1,0}, {1,1}, {0,1}, {-1,1}, {-1,0}, {-1,-1}, {0,-1}, {1,-1}
    }};
    if (slot >= 4) throw std::invalid_argument("Invalid infantry destination slot");
    const auto [du, dv] = directions.at(direction);
    const int remaining = running ? 255 - progress : 0;
    const int subU = (8 + 16 * int(slot % 2)) * 256 - 16 * du * remaining;
    const int subV = (8 + 16 * int(slot / 2)) * 256 - 16 * dv * remaining;
    return {tileU * 32 + (subU >> 8), tileV * 32 + (subV >> 8)};
}
std::uint32_t infantryRandom(std::uint32_t& state, std::uint32_t mask) {
    state = state * 0x41c64e6du + 0x3039u;
    return (state >> 16) & mask;
}

void InfantryStep::advance(bool crouched, std::uint32_t& sharedRandomState, unsigned customBase) {
    if (!running) return;
    const auto bit = infantryRandom(sharedRandomState, 1);
    if (progress == 255) {
        running = false;
        progress = 0;
        return;
    }
    const unsigned base = customBase > 0u ? std::clamp(customBase, 2u, 16u) : (crouched ? 5u : 8u);
    progress = std::min(255u, unsigned(progress) + base + bit);
}
}
