#include "assets/mission.hpp"
#include "sim/tutorial.hpp"
#include "sim/tutorial_units.hpp"
#include <algorithm>
#include <array>
#include <zlib.h>
#include <iostream>
#include <stdexcept>

static uLong appendPosition(uLong checksum, int x, int y) {
    const std::array<std::uint8_t,4> xy{
        std::uint8_t(x),std::uint8_t(x>>8),std::uint8_t(y),std::uint8_t(y>>8)};
    return crc32_z(checksum,xy.data(),xy.size());
}

int main(int argc, char** argv) try {
    const bool wreck = argc == 3 && std::string_view(argv[2]) == "--wreck";
    const bool fields = argc == 3 && std::string_view(argv[2]) == "--fields";
    const bool longMarch = argc == 3 && std::string_view(argv[2]) == "--long-marches";
    if (argc != 2 && !wreck && !fields && !longMarch) return 1;
    const auto game = std::filesystem::path(argv[1]);
    auto data = ss2::assets::readTutorialData(game / "maps/single/--- TUTORIAL I ---.SSM");
    auto placements = ss2::assets::readTutorialPlacements(game);
    std::vector<ss2::assets::Infantry> infantry;
    std::vector<ss2::assets::Placement> vehicles;
    for (const auto& p : placements) {
        if (p.id == "zis-3" || p.id == "m3a1" || p.id == "ds39" || p.id == "panther") vehicles.push_back(p);
        else infantry.push_back({p.id,p.x,p.y,p.hp,p.ammo,p.group,p.owner,0,0,0,0,unsigned(p.direction)});
    }
    // Gold's loader prepends the forward I-UNITS records, yielding forward mapunits order.
    if (longMarch) {
        const auto front = std::ranges::find_if(infantry,[](const auto& u) {
            return u.owner == 0 && u.x == 1616 && u.y == 2336;
        });
        if (front == infantry.end()) throw std::runtime_error("Missing long-route infantryman");
        const std::size_t id = front-infantry.begin();
        struct March { int tileX, tileY; uLong checksum; };
        for (const March goal : {March{34,35,0xd60f5668}, March{75,35,0xe3ff723a}, March{42,42,0x2ca70725}}) {
            ss2::TutorialUnits units(ss2::InfantryPathGrid::loadTutorial(game),infantry,vehicles);
            if (!units.order(id,4096+32*(goal.tileX-goal.tileY),16*(goal.tileX+goal.tileY+1)))
                throw std::runtime_error("Long march rejected");
            int x = front->x, y = front->y;
            uLong checksum = 0;
            for (int tick = 0; tick < 6300; ++tick) {
                for (const auto& p : units.step()) {
                    if (p.index == id) { x = p.x; y = p.y; }
                }
                checksum = appendPosition(checksum,x,y);
            }
            if (checksum != goal.checksum) throw std::runtime_error("Long march differs from Gold");
        }
        return 0;
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
    if (wreck || fields) {
        std::vector<size_t> squad;
        for (size_t i = 0; i < infantry.size(); ++i)
            if (infantry[i].owner == 0) squad.push_back(i);
        // The odd column in the fields run is on a tile boundary. Both clicks
        // resolve to tile (37,73) with Gold's screen-to-world conversion.
        const int wreckX = fields ? 2961 : 2960, wreckY = fields ? 1768 : 1776;
        if (squad.size() != 29 || soldiers.orderSelection(squad, wreckX, wreckY) != squad.size())
            throw std::runtime_error("Wreck ground order did not dispatch to the selected squad");
        // The isolated retail movement callbacks used this same unit order and seed.
        // Sample every soldier's rendered position after each unpaused tick.
        auto positions = infantry;
        unsigned long checksum = 0;
        int firstTwo = -1, fullSquad = -1;
        const int ticks = fields ? 5500 : 3200;
        for (int t = 0; t < ticks; ++t) {
            if (fields && t == 1872 && soldiers.orderSelection(squad,3601,1600) != squad.size())
                throw std::runtime_error("Fields order did not redirect the walking squad");
            for (const auto& pose : tick()) {
                positions[pose.index].x = pose.x;
                positions[pose.index].y = pose.y;
            }
            for (auto id : squad) {
                const auto& p = positions[id];
                checksum = appendPosition(checksum,p.x,p.y);
            }
            const unsigned inside = soldiers.countInZone(vm.data().zones, 2);
            if (inside >= 2 && firstTwo < 0) firstTwo = t;
            if (inside == squad.size() && fullSquad < 0) fullSquad = t;
        }
        if (!objective || !nextBriefing || firstTwo != 1758)
            throw std::runtime_error("Wreck mission callbacks differ from Gold");
        if (fields && checksum != 0x3fbc7902UL)
            throw std::runtime_error("In-flight second order differs from the original convoy positions");
        if (wreck && (fullSquad != 2087 || checksum != 0x0489743fUL))
            throw std::runtime_error("Wreck order differs from the original convoy positions or mission callbacks");
        return 0;
    }
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
            if (ticks == 12) {
                // An identical mid-step click must preserve the accepted direction and progress.
                if (!soldiers.order(index, 4096 + 32*(tx-81), 16*(tx+81+1)))
                    throw std::runtime_error("Repeated move rejected while walking");
            }
            const auto changes = tick();
            if (ticks == 1 && changes.empty()) continue; // Initial pathfinding sweep follows unit updates.
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
