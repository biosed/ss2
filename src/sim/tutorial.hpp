#pragma once
#include "assets/mission.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace ss2 {
struct MissionEvent {
    enum Kind { Dialog, Message, Camera, Marker, Victory } kind;
    unsigned index;
};

struct MissionQueries {
    std::function<unsigned(unsigned zone, unsigned player)> countInZone;
    std::function<unsigned(unsigned group)> countAliveByGroup;
};

// Tutorial I mission trigger VM operating on mission ticks.
class Tutorial {
public:
    explicit Tutorial(assets::TutorialData, unsigned initialZoneCount, unsigned initialEnemyGroup3 = 5);
    Tutorial(assets::TutorialData, MissionQueries queries);
    void step(unsigned playerUnitsInZone2, unsigned enemyGroup3Alive = 5);
    void step(const MissionQueries& queries);
    const assets::TutorialData& data() const { return data_; }
    uint32_t tick() const { return tick_; }
    std::vector<MissionEvent> takeEvents();
private:
    assets::TutorialData data_;
    uint32_t tick_{};
    std::array<bool, 30> active_{};
    std::array<uint32_t, 100> timers_{}, variables_{};
    std::vector<MissionEvent> events_;
    void initActive(bool fullMission);
    void scan(const MissionQueries& queries);
    void evaluate(unsigned script, const MissionQueries& queries);
};
unsigned tutorialTurnDelay(const std::filesystem::path& gameDir);
}
