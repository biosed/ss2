#pragma once
#include "assets/mission.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace ss2 {
struct MissionEvent {
    enum Kind { Dialog, Message, Camera, Marker } kind;
    unsigned index;
};

// The verified opening triggers of Tutorial I operate on mission ticks.
class Tutorial {
public:
    explicit Tutorial(assets::TutorialData, unsigned initialZoneCount);
    void step(unsigned playerUnitsInZone2);
    const assets::TutorialData& data() const { return data_; }
    uint32_t tick() const { return tick_; }
    std::vector<MissionEvent> takeEvents();
private:
    assets::TutorialData data_;
    uint32_t tick_{};
    std::array<bool, 12> active_{};
    std::array<uint32_t, 100> timers_{}, variables_{};
    std::vector<MissionEvent> events_;
    void scan(unsigned inZone);
    void evaluate(unsigned script, unsigned inZone);
};
unsigned tutorialTurnDelay(const std::filesystem::path& gameDir);
}
