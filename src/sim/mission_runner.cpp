#include "sim/mission_runner.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace ss2 {
namespace {
MissionQueries makeQueries(const TutorialUnits& units, const std::array<std::uint8_t, 128 * 128>& zones) {
    return {
        [&units, &zones](unsigned zone, unsigned player) {
            return units.countInZone(zones, zone, int(player));
        },
        [&units](unsigned group) {
            return units.countAliveByGroup(group);
        }
    };
}
}

MissionRunner::MissionRunner(assets::TutorialData data, unsigned delayMs, TutorialUnits units)
    : units_(std::move(units)), tutorial_(data, makeQueries(units_, data.zones)) {
    fog_ = units_.takeFogSnapshot();
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
                if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
                tutorial_.step(makeQueries(units_, tutorial_.data().zones));
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
std::optional<std::vector<std::uint8_t>> MissionRunner::pollFog() {
    std::lock_guard lock(mutex_);
    return std::exchange(fog_, std::nullopt);
}
bool MissionRunner::canMove(std::span<const size_t> indices, int x, int y) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canMoveSelection(indices, x, y);
}
std::optional<size_t> MissionRunner::move(std::span<const size_t> indices, int x, int y) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderSelection(indices, x, y);
}
bool MissionRunner::isValidAttackTarget(size_t targetIndex) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.isValidAttackTarget(targetIndex, 0);
}
std::optional<size_t> MissionRunner::attack(std::span<const size_t> indices, size_t targetIndex) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderAttack(indices, targetIndex);
}
size_t MissionRunner::stop(std::span<const size_t> indices) {
    std::lock_guard lock(mutex_);
    if (modal_) return 0;
    return units_.stopSelection(indices);
}
std::optional<bool> MissionRunner::toggleHoldFire(std::span<const size_t> indices) {
    std::lock_guard lock(mutex_);
    if (modal_ || indices.empty()) return std::nullopt;
    return units_.toggleHoldFire(indices);
}
std::optional<bool> MissionRunner::toggleCrouch(std::span<const size_t> indices) {
    std::lock_guard lock(mutex_);
    if (modal_ || indices.empty()) return std::nullopt;
    const bool crouched = units_.toggleCrouch(indices);
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return crouched;
}
void MissionRunner::acknowledge() {
    std::lock_guard lock(mutex_);
    modal_ = false;
}
bool MissionRunner::togglePause() {
    std::lock_guard lock(mutex_);
    paused_ = !paused_;
    return paused_;
}
}
