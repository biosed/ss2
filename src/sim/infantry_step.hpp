#pragma once
#include <cstdint>
#include <utility>

namespace ss2 {
std::uint32_t infantryRandom(std::uint32_t& state, std::uint32_t mask);

struct InfantryStep {
    int tileU, tileV;
    unsigned slot, direction;
    std::uint8_t progress{};
    bool running{true};
    // Destination tile/slot and step direction are supplied by the route consumer.
    std::pair<int, int> position() const;
    void advance(bool crouched, std::uint32_t& sharedRandomState, unsigned customBase = 0u);
};
}
