#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace ss2 {

// Only an independently owned, read-only value crosses into presentation.
// Future map and entity fields need evidence from the original program.
struct Snapshot {
    std::uint64_t tick = 0;
};

class Simulation {
public:
    virtual ~Simulation() = default;
    virtual void step() = 0;
    virtual Snapshot snapshot() const = 0;
};

// The caller supplies the period. Production has no period until measured.
class TickClock {
public:
    using Duration = std::chrono::nanoseconds;
    explicit TickClock(Duration period) : period_(period) {
        if (period <= Duration::zero()) throw std::invalid_argument("Expected a positive tick period");
    }

    void advance(Duration elapsed, Simulation& simulation) {
        if (elapsed < Duration::zero()) throw std::invalid_argument("Negative elapsed time");
        // Keep the remainder even when presentation takes several ticks.
        auto count = elapsed / period_;
        const auto fraction = elapsed % period_;
        if (fraction >= period_ - remainder_) {
            ++count;
            remainder_ = fraction - (period_ - remainder_);
        } else {
            remainder_ += fraction;
        }
        while (count-- > 0) simulation.step();
    }

private:
    Duration period_;
    Duration remainder_{};
};

}
