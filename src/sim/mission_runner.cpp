#include "sim/mission_runner.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace ss2 {
MissionRunner::MissionRunner(assets::TutorialData data, unsigned delayMs, TutorialUnits units)
    : units_(std::move(units)), tutorial_(data, units_.countInZone(data.zones, 2)) {
    collect();
    worker_ = std::jthread([this, delayMs](std::stop_token stop) { run(stop, delayMs); });
}
MissionRunner::~MissionRunner() {
    worker_.request_stop();
    changed_.notify_all();
}

void MissionRunner::collect() {
    for (const auto e : tutorial_.takeEvents()) {
        if (e.kind == MissionEvent::Dialog) modal_ = true;
        pending_.push_back(e);
    }
}

void MissionRunner::run(std::stop_token stop, unsigned delayMs) {
    using namespace std::chrono;
    using Clock = steady_clock;
    const auto start = Clock::now();
    const auto period = milliseconds(delayMs);
    auto deadline = period;
    unsigned phase = 0;
    std::unique_lock lock(mutex_);
    try {
        while (!stop.stop_requested()) {
            changed_.wait_until(lock, stop, start + deadline, [] { return false; });
            if (stop.stop_requested()) break;
            const auto now = duration_cast<milliseconds>(Clock::now() - start);
            // Gold SP accepts one update on a due dispatch. On the fourth phase
            // it resynchronizes to now; the intervening phases average the two times.
            deadline = (phase == 3 ? now : (deadline + now) / 2) + period;
            phase = (phase + 1) % 4;
            if (!modal_ && !paused_) {
                for (auto pose : units_.step()) {
                    const auto old = std::ranges::find(poses_, pose.index, &SoldierPose::index);
                    if (old == poses_.end()) poses_.push_back(pose);
                    else *old = pose;
                }
                tutorial_.step(units_.countInZone(tutorial_.data().zones, 2));
                collect();
            }
        }
    } catch (...) { error_ = std::current_exception(); }
}

std::vector<MissionEvent> MissionRunner::poll() {
    std::lock_guard lock(mutex_);
    if (error_) std::rethrow_exception(error_);
    return std::exchange(pending_, {});
}
std::vector<SoldierPose> MissionRunner::pollPoses() {
    std::lock_guard lock(mutex_);
    return std::exchange(poses_, {});
}
bool MissionRunner::canMove(size_t index, int x, int y) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canMove(index, x, y);
}
bool MissionRunner::move(size_t index, int x, int y) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.order(index, x, y);
}
void MissionRunner::acknowledge() {
    std::lock_guard lock(mutex_);
    modal_ = false;
}
void MissionRunner::togglePause() {
    std::lock_guard lock(mutex_);
    paused_ = !paused_;
}
}
