#include "sim/infantry_step.hpp"
#include <array>
#include <iostream>

int main() {
    std::uint64_t digest = 14695981039346656037ull;
    for (auto seed : {0x76321fu, 0u, 0xffffffffu}) {
        for (unsigned crouched = 0; crouched < 2; ++crouched) {
            for (unsigned slot = 0; slot < 4; ++slot) {
                for (unsigned dir = 0; dir < 8; ++dir) {
                    ss2::InfantryStep step{34,110,slot,dir};
                    auto rng = seed;
                    for (;;) {
                        const auto [u,v] = step.position();
                        for (auto value : {step.running ? 2u : 0u, unsigned(step.progress), unsigned(u), unsigned(v), rng})
                            digest = (digest ^ value) * 1099511628211ull;
                        if (!step.running) break;
                        step.advance(crouched, rng);
                    }
                }
            }
        }
    }
    std::cout << "Native accepted-step trace digest " << std::hex << digest << '\n';
    return digest == 0x1e0747cec323df2dull ? 0 : 1;
}
