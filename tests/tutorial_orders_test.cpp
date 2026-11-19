#include "assets/mission.hpp"
#include "sim/tutorial.hpp"
#include "sim/tutorial_units.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) try {
    if (argc != 2) return 1;
    const auto game = std::filesystem::path(argv[1]);
    auto data = ss2::assets::readTutorialData(game / "maps/single/--- TUTORIAL I ---.SSM");
    auto placements = ss2::assets::readTutorialPlacements(game);
    std::vector<ss2::assets::Infantry> infantry;
    std::vector<ss2::assets::Placement> vehicles;
    for (const auto& p : placements) {
        if (p.id == "zis-3" || p.id == "m3a1" || p.id == "ds39" || p.id == "panther") vehicles.push_back(p);
        else infantry.push_back({p.id,p.x,p.y,p.hp,p.ammo,p.group,p.owner,0,0,0,0,unsigned(p.direction)});
    }
    ss2::TutorialUnits soldiers(ss2::InfantryPathGrid::loadTutorial(game), infantry, vehicles);
    ss2::Tutorial vm(std::move(data), 0);
    bool objective = false, nextBriefing = false;
    const auto tick = [&]() {
        auto poses = soldiers.step();
        vm.step(soldiers.countInZone(vm.data().zones, 2));
        for (const auto event : vm.takeEvents()) {
            if (event.kind == ss2::MissionEvent::Message && event.index == 4) objective = true;
            if (event.kind == ss2::MissionEvent::Dialog && event.index == 5) nextBriefing = true;
        }
        return poses;
    };
    for (int i = 0; i < 516; ++i) tick();
    for (int tx : {34,35}) {
        const int u = tx * 32 + 8, v = 3576;
        const auto it = std::ranges::find_if(infantry, [&](const auto& s) {
            return s.owner == 0 && s.x == 4096+u-v && s.y == (u+v)/2;
        });
        if (it == infantry.end()) throw std::runtime_error("Missing front rank soldier");
        const size_t index = it-infantry.begin();
        if (!soldiers.order(index, 4096 + 32*(tx-81), 16*(tx+81+1))) throw std::runtime_error("Move rejected");
        unsigned steps = 0, ticks = 0;
        while (++ticks < 3000) {
            const auto changes = tick();
            if (changes.size() != 1 || changes[0].index != index) throw std::runtime_error("Lost an ordered soldier");
            const auto& pose = changes[0];
            if (!pose.walking) {
                if (pose.x != 4096+u-(v-960) || pose.y != (u+v-960)/2) throw std::runtime_error("Arrival differs");
                break;
            }
            if (!pose.progress) ++steps;
            if (pose.direction != 6) throw std::runtime_error("Retail route differs");
        }
        if (ticks == 3000 || steps != 60) throw std::runtime_error("Expected sixty retail substeps");
        if (tx == 34 && objective) throw std::runtime_error("One soldier incorrectly completed the objective");
    }
    for (unsigned i = 0; i < 256; ++i) tick();
    if (!objective || !nextBriefing) throw std::runtime_error("Missing original completion callbacks");
    std::cout << "Two original soldier placements reached zone 2 via 120 matched substeps, then triggered phrases 4 and 5\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
