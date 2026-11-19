#pragma once
#include "sim/tutorial.hpp"
#include "sim/tutorial_units.hpp"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace ss2 {
// The mission worker owns script execution. Only presentation events and input
// cross between this clock and SDL's main thread.
class MissionRunner {
public:
    MissionRunner(assets::TutorialData, unsigned delayMs, TutorialUnits);
    bool canMove(size_t index, int x, int y);
    bool move(size_t index, int mapX, int mapY);
    std::vector<SoldierPose> pollPoses();
    ~MissionRunner();
    std::vector<MissionEvent> poll();
    void acknowledge();
    void togglePause();
    const assets::TutorialData& data() const { return tutorial_.data(); }
private:
    void collect();
    void run(std::stop_token, unsigned delayMs);
    TutorialUnits units_;
    Tutorial tutorial_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::vector<SoldierPose> poses_;
    bool modal_{}, paused_{};
    std::vector<MissionEvent> pending_;
    std::exception_ptr error_;
    std::jthread worker_;
};
}
