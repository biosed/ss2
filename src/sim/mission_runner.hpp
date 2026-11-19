#pragma once
#include "sim/tutorial.hpp"
#include "sim/tutorial_units.hpp"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

namespace ss2 {
// The mission worker owns script execution. Only presentation events and input
// cross between this clock and SDL's main thread.
class MissionRunner {
public:
    MissionRunner(assets::TutorialData, unsigned delayMs, TutorialUnits);
    bool canMove(std::span<const size_t> indices, int x, int y);
    std::optional<size_t> move(std::span<const size_t> indices, int mapX, int mapY);
    bool isValidAttackTarget(size_t targetIndex);
    std::optional<size_t> attack(std::span<const size_t> indices, size_t targetIndex);
    size_t stop(std::span<const size_t> indices);
    std::optional<bool> toggleHoldFire(std::span<const size_t> indices);
    std::optional<bool> toggleCrouch(std::span<const size_t> indices);
    std::vector<SoldierPose> pollPoses();
    std::optional<std::vector<std::uint8_t>> pollFog();
    ~MissionRunner();
    std::vector<MissionEvent> poll();
    void acknowledge();
    bool togglePause();
    const assets::TutorialData& data() const { return tutorial_.data(); }
private:
    void collect();
    void run(std::stop_token, unsigned delayMs);
    TutorialUnits units_;
    Tutorial tutorial_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::vector<SoldierPose> poses_;
    std::optional<std::vector<std::uint8_t>> fog_;
    bool modal_{}, paused_{};
    std::vector<MissionEvent> pending_;
    std::exception_ptr error_;
    std::jthread worker_;
};
}
