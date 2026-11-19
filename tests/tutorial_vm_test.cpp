#include "sim/tutorial.hpp"
#include <array>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) try {
    using Event = ss2::MissionEvent;
    if (argc == 2) {
        const auto file = std::filesystem::path(argv[1]) / "maps/single/--- TUTORIAL I ---.SSM";
        ss2::Tutorial vm(ss2::assets::readTutorialData(file), 0);
        std::vector<std::pair<unsigned, unsigned>> observed;
        for (unsigned i = 0; i <= 835; ++i) {
            if (i) vm.step(i < 579 ? 0 : i < 643 ? 1 : 2);
            for (auto event : vm.takeEvents())
                if (event.kind == Event::Dialog || event.kind == Event::Message) observed.emplace_back(i, event.index);
        }
        using P = std::pair<unsigned, unsigned>;
        const std::vector<P> expected{{0,19},{67,0},{131,28},{195,1},{259,2},{387,20},{451,3},{515,29},{643,4},{835,5}};
        if (observed != expected) throw std::runtime_error("Native mission trace differs from the Gold interpreter");
        std::cout << observed.size() << " mission callbacks match the retail reference\n";
    } else {
        // Hand-authored input exercises strict comparison and one-shot retirement.
        ss2::assets::TutorialData data;
        data.phrases.resize(1);
        data.scripts.resize(12, {0x7fffffff});
        data.scripts[0] = {0x80000001,0x80000001,0x80000000,0x80000002,6,7,11,0x80000000,15,0x7fffffff};
        ss2::Tutorial vm(std::move(data), 1);
        for (unsigned i = 0; i <= 131; ++i) {
            if (i) vm.step(i >= 4 ? 2 : 1);
            const auto events = vm.takeEvents();
            if (i == 67) {
                if (events.size() != 1 || events[0].kind != Event::Message) return 1;
            } else if (!events.empty()) return 1;
        }
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
