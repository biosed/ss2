#include "ss2/simulation.hpp"
#include <array>

using namespace std::chrono_literals;
struct Counter final : ss2::Simulation {
    void step() override { ++tick; }
    ss2::Snapshot snapshot() const override { return {tick}; }
    std::uint64_t tick = 0;
};

int main() {
    Counter regular, stalled;
    ss2::TickClock a(11ms), b(11ms); // Arbitrary fixture timing.
    for (int i = 0; i < 120; ++i) a.advance(8ms, regular);
    for (auto delta : std::array{0ms, 133ms, 800ms, 27ms}) b.advance(delta, stalled);
    const auto saved = stalled.snapshot();
    if (regular.tick != 87 || saved.tick != regular.tick) return 1;
    b.advance(8ms, stalled);
    if (saved.tick != 87 || stalled.tick != 88) return 2;
    ss2::TickClock small(17ns), large(17ns);
    Counter many, one;
    for (int i = 0; i < 100000; ++i) small.advance(1ns, many);
    large.advance(100000ns, one);
    if (many.tick != 5882 || one.tick != many.tick) return 5;
    small.advance(11ns, many);
    large.advance(11ns, one);
    if (many.tick != 5883 || one.tick != many.tick) return 6;
    large.advance(17ns, one);
    if (one.tick != 5884) return 7;
    try { b.advance(-1ms, stalled); return 3; } catch (const std::invalid_argument&) {}
    try { ss2::TickClock invalid(0ms); return 4; } catch (const std::invalid_argument&) {}
}
