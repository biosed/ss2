#pragma once
#include <cstdint>
#include <utility>

namespace ss2 {
struct InfantryStep {
    int tileU, tileV;
    unsigned slot, direction;
    std::uint8_t progress{};
    bool running{true};
    // Destination tile/slot and step direction are supplied by the route consumer.
    std::pair<int, int> position() const;
    void advance(bool crouched, std::uint32_t& sharedRandomState);
};
}
