#include "sim/mission_runner.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace ss2 {
namespace {
std::array<int, 64> extractReinforcementWaveOwners(const std::vector<std::vector<std::uint32_t>>& scripts) {
    std::array<int, 64> owners;
    owners.fill(-1);
    for (const auto& sc : scripts) {
        for (std::size_t k = 5; k < sc.size(); ++k) {
            if (sc[k] != 40u) continue;
            if ((sc[k - 5] & 0x80000000u) == 0u || (sc[k - 4] & 0x80000000u) == 0u) continue;
            const std::uint32_t own = sc[k - 5] & 0x7fffffffu;
            const std::uint32_t wi = sc[k - 4] & 0x7fffffffu;
            if (wi < 64u && own < 4u && owners[wi] < 0) {
                owners[wi] = static_cast<int>(own);
            }
        }
    }
    return owners;
}
}

MissionRunner::MissionRunner(assets::TutorialData data, unsigned delayMs, TutorialUnits units)
    : units_(std::move(units)),
      spawnedReinforcementWaves_{},
      reinforcementWaveOwner_(extractReinforcementWaveOwners(data.scripts)),
      tutorial_(data, makeQueriesLocked(data.zones, data.reinforcements, data.markers)) {
    units_.setWeather(data.weather);
    fog_ = units_.takeFogSnapshot();
    collect();
    collectVehiclePoses();
    worker_ = std::jthread([this, delayMs](std::stop_token stop) { run(stop, delayMs); });
}
MissionRunner::~MissionRunner() {
    worker_.request_stop();
    changed_.notify_all();
}

void MissionRunner::collect() {
    for (const auto e : tutorial_.takeEvents()) {
        if (e.kind == MissionEvent::Dialog) modal_ = true;
        if (e.kind == MissionEvent::Reinforcement) {
            if (e.index < 64u && e.owner < 4u) {
                reinforcementWaveOwner_[e.index] = static_cast<int>(e.owner);
            }
            if (e.delaySec > 0) {
                scheduledReinforcements_.push_back({e, tutorial_.tick() + e.delaySec * 25u});
                continue;
            }
            if (e.index < 64u) spawnedReinforcementWaves_[e.index] = true;
        }
        if (e.kind == MissionEvent::GroupOrder) {
            const auto& locs = tutorial_.data().locations;
            if (e.destZone < locs.size()) {
                const auto [gx, gy] = locs[e.destZone];
                (void)units_.orderGroupMove(e.index, gx, gy, true);
            }
            continue;
        }
        if (e.kind == MissionEvent::FogReveal) {
            const auto& marks = tutorial_.data().markers;
            if (e.index < marks.size()) {
                const auto [mx, my] = marks[e.index];
                units_.revealFogAt(mx, my, int(e.destZone), int(e.owner));
                collectVehiclePoses();
                if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
            }
            continue;
        }
        if (e.kind == MissionEvent::PlayerTeam) {
            // Gold OP(17) stores nextCampaignMissionId = packed >> 8, subIndex = packed & 0xff.
            nextMissionSignal_ = {e.owner, e.gate};
            continue;
        }
        if (e.kind == MissionEvent::AirSupportAddPlanes) {
            units_.addAirSupportPlanes(int(e.owner), static_cast<assets::AirMissionKind>(e.index), int(e.destZone));
            continue;
        }
        if (e.kind == MissionEvent::AirSupportAddSorties) {
            units_.addAirSupportSorties(int(e.owner), static_cast<assets::AirMissionKind>(e.index), int(e.destZone));
            continue;
        }
        if (e.kind == MissionEvent::AirStrike) {
            const auto& marks = tutorial_.data().markers;
            if (e.index < marks.size()) {
                const auto [mx, my] = marks[e.index];
                const unsigned cnt = std::min<unsigned>(e.destZone, 16u);
                bool anyLaunched = false;
                for (unsigned k = 0; k < cnt; ++k) {
                    const int offset = (static_cast<int>(k) - static_cast<int>(cnt / 2u)) * 32;
                    if (units_.callAirSupport(static_cast<assets::AirMissionKind>(e.gate), mx + offset, my, int(e.owner))) {
                        anyLaunched = true;
                    }
                }
                if (anyLaunched) collectAirSorties();
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupTransferOwner) {
            std::vector<SoldierPose> changed;
            if (units_.transferGroupOwner(e.index, int(e.owner), &changed) > 0u) {
                mergeSoldierPoses(changed);
                collectVehiclePoses();
                if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupReassignInZone) {
            std::vector<SoldierPose> changed;
            if (units_.reassignGroupInZone(tutorial_.data().zones, e.destZone, e.index, e.owner, &changed) > 0u) {
                mergeSoldierPoses(changed);
                collectVehiclePoses();
            }
            continue;
        }
        if (e.kind == MissionEvent::GroupBombard) {
            if (e.owner == 1u) {
                const auto& marks = tutorial_.data().markers;
                if (e.destZone < marks.size()) {
                    const auto [mx, my] = marks[e.destZone];
                    (void)units_.orderGroupAttackGround(e.index, mx, my, true);
                }
            } else {
                const auto& locs = tutorial_.data().locations;
                if (e.destZone < locs.size()) {
                    const auto [gx, gy] = locs[e.destZone];
                    (void)units_.orderGroupAttackGround(e.index, gx, gy, true);
                }
            }
            continue;
        }
        pending_.push_back(e);
    }
}

void MissionRunner::flushScheduledReinforcements() {
    const std::uint32_t tick = tutorial_.tick();
    for (auto it = scheduledReinforcements_.begin(); it != scheduledReinforcements_.end();) {
        if (it->dueTick <= tick) {
            auto ready = it->event;
            ready.delaySec = 0;
            if (ready.index < 64u) spawnedReinforcementWaves_[ready.index] = true;
            pending_.push_back(ready);
            it = scheduledReinforcements_.erase(it);
        } else {
            ++it;
        }
    }
}

void MissionRunner::mergeSoldierPoses(std::span<const SoldierPose> incoming) {
    for (const auto& pose : incoming) {
        const auto old = std::ranges::find(poses_, pose.index, &SoldierPose::index);
        if (old == poses_.end()) {
            poses_.push_back(pose);
            continue;
        }
        const bool wasFired = old->fired;
        const bool wasGrenade = old->threwGrenade;
        const bool wasFlamer = old->firedFlamer;
        const bool wasExplosive = old->firedExplosive;
        const int oldAmmo = old->ammo;
        const std::int8_t oldCrouch = old->crouchedState;
        const int oldTx = old->targetX;
        const int oldTy = old->targetY;
        *old = pose;
        if (wasFired && !pose.fired) {
            old->fired = true;
            old->targetX = oldTx;
            old->targetY = oldTy;
        }
        old->threwGrenade = old->threwGrenade || wasGrenade;
        old->firedFlamer = old->firedFlamer || wasFlamer;
        old->firedExplosive = old->firedExplosive || wasExplosive;
        if (pose.ammo < 0 && oldAmmo >= 0) old->ammo = oldAmmo;
        if (pose.crouchedState < 0 && oldCrouch >= 0) old->crouchedState = oldCrouch;
    }
}

void MissionRunner::collectVehiclePoses() {
    for (const auto& pose : units_.takeVehiclePoses()) {
        const auto old = std::ranges::find(vehiclePoses_, pose.index, &VehiclePose::index);
        if (old == vehiclePoses_.end()) {
            vehiclePoses_.push_back(pose);
            continue;
        }
        const bool wasFired = old->fired;
        const int oldTx = old->targetX;
        const int oldTy = old->targetY;
        const bool oldSec = old->firedSecondary;
        const bool oldHeavy = old->heavyImpact;
        const bool oldHitVeh = old->hitVehicle;
        const bool oldRocket = old->firedRocket;
        const bool oldFlamer = old->firedFlamer;
        const std::uint8_t oldCrater = old->craterScale;
        const bool wasRepaired = old->repaired;
        const bool wasResupplied = old->resupplied;
        const bool wasCaptured = old->captured;
        const bool wasDecrewed = old->decrewed;
        std::string oldSnd = std::move(old->fireSound);
        *old = pose;
        if (wasFired && !pose.fired) {
            old->fired = true;
            old->targetX = oldTx;
            old->targetY = oldTy;
            old->firedSecondary = oldSec;
            old->heavyImpact = oldHeavy;
            old->hitVehicle = oldHitVeh;
            old->firedRocket = oldRocket;
            old->firedFlamer = oldFlamer;
            old->craterScale = oldCrater;
            old->fireSound = std::move(oldSnd);
        }
        old->repaired = old->repaired || wasRepaired;
        old->resupplied = old->resupplied || wasResupplied;
        old->captured = old->captured || wasCaptured;
        old->decrewed = old->decrewed || wasDecrewed;
    }
}

void MissionRunner::collectBuildingUpdates() {
    for (const auto& b : units_.takeBuildingUpdates()) {
        const auto old = std::ranges::find(buildingUpdates_, b.index, &BuildingSnapshot::index);
        if (old == buildingUpdates_.end()) {
            buildingUpdates_.push_back(b);
            continue;
        }
        const bool wasDamaged = old->damagedThisTick;
        const bool wasCollapsed = old->collapsedThisTick;
        *old = b;
        old->damagedThisTick = old->damagedThisTick || wasDamaged;
        old->collapsedThisTick = old->collapsedThisTick || wasCollapsed;
    }
}

void MissionRunner::collectAirSorties() {
    for (const auto& pose : units_.takeAirSorties()) {
        const auto old = std::ranges::find(airSorties_, pose.id, &AirSortiePose::id);
        if (old == airSorties_.end()) {
            airSorties_.push_back(pose);
            continue;
        }
        const bool hadAa = old->aaFired;
        const int aaX = old->aaOriginX;
        const int aaY = old->aaOriginY;
        std::string aaSnd = std::move(old->aaSoundGroup);
        const bool wasAlive = old->alive;
        const bool wasActive = old->active;
        *old = pose;
        if (hadAa && !pose.aaFired) {
            old->aaFired = true;
            old->aaOriginX = aaX;
            old->aaOriginY = aaY;
            old->aaSoundGroup = std::move(aaSnd);
        }
        old->alive = wasAlive && pose.alive;
        old->active = wasActive && pose.active;
    }
}

void MissionRunner::run(std::stop_token stop, unsigned delayMs) {
    using namespace std::chrono;
    using Clock = steady_clock;
    const auto start = Clock::now();
    const auto computePeriod = [&] {
        const unsigned pct = static_cast<unsigned>(std::clamp(speedPercent_.load(), 50, 300));
        return milliseconds(std::max(8u, (delayMs * 100u) / pct));
    };
    auto deadline = computePeriod();
    unsigned phase = 0;
    std::unique_lock lock(mutex_);
    try {
        while (!stop.stop_requested()) {
            changed_.wait_until(lock, stop, start + deadline, [] { return false; });
            if (stop.stop_requested()) break;
            const auto period = computePeriod();
            const auto now = duration_cast<milliseconds>(Clock::now() - start);
            // Gold SP accepts one update on a due dispatch. On the fourth phase
            // it resynchronizes to now; the intervening phases average the two times.
            deadline = (phase == 3 ? now : (deadline + now) / 2) + period;
            phase = (phase + 1) % 4;
            if (!modal_ && !paused_) {
                mergeSoldierPoses(units_.step());
                collectVehiclePoses();
                collectBuildingUpdates();
                collectAirSorties();
                auto newAir = units_.takeAirStrikeEvents();
                if (!newAir.empty()) {
                    airEvents_.insert(airEvents_.end(), newAir.begin(), newAir.end());
                }
                simTick_.store(units_.tick_, std::memory_order_relaxed);
                auto newMines = units_.takeMineEvents();
                if (!newMines.empty()) {
                    mineEvents_.insert(mineEvents_.end(), newMines.begin(), newMines.end());
                }
                if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
                tutorial_.step(makeQueriesLocked(tutorial_.data().zones, tutorial_.data().reinforcements,
                                                tutorial_.data().markers));
                collect();
                flushScheduledReinforcements();
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
std::vector<VehiclePose> MissionRunner::pollVehiclePoses() {
    std::lock_guard lock(mutex_);
    return std::exchange(vehiclePoses_, {});
}
std::optional<std::vector<std::uint8_t>> MissionRunner::pollFog() {
    std::lock_guard lock(mutex_);
    return std::exchange(fog_, std::nullopt);
}
bool MissionRunner::canMove(std::span<const size_t> indices, int x, int y) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canMoveSelection(indices, x, y);
}
std::optional<size_t> MissionRunner::move(std::span<const size_t> indices, int x, int y, bool append) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderSelection(indices, x, y, append);
}
std::optional<size_t> MissionRunner::attackMove(std::span<const size_t> indices, int x, int y, bool append) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderAttackMove(indices, x, y, append);
}
std::optional<size_t> MissionRunner::patrol(std::span<const size_t> indices, int mapX, int mapY) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderPatrol(indices, mapX, mapY);
}
size_t MissionRunner::scatter(std::span<const size_t> indices) {
    std::lock_guard lock(mutex_);
    if (modal_) return 0;
    return units_.orderScatter(indices);
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
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return crouched;
}
std::optional<bool> MissionRunner::toggleBinoculars(std::span<const size_t> indices) {
    std::lock_guard lock(mutex_);
    if (modal_ || indices.empty()) return std::nullopt;
    std::vector<SoldierPose> changed;
    const bool active = units_.toggleBinocularsWithPoses(indices, &changed);
    mergeSoldierPoses(changed);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return active;
}
size_t MissionRunner::spawnInfantry(const assets::Infantry& unit, int expa) {
    std::lock_guard lock(mutex_);
    const size_t index = units_.spawnInfantry(unit, expa);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return index;
}
std::optional<size_t> MissionRunner::moveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.moveVehicles(indices, mapX, mapY, append);
}
std::optional<size_t> MissionRunner::reverseMoveVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.reverseMoveVehicles(indices, mapX, mapY);
}
std::optional<size_t> MissionRunner::faceVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    const size_t res = units_.faceVehicles(indices, mapX, mapY);
    collectVehiclePoses();
    return res;
}
std::optional<size_t> MissionRunner::attackMoveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.attackMoveVehicles(indices, mapX, mapY, append);
}
std::optional<size_t> MissionRunner::patrolVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.patrolVehicles(indices, mapX, mapY);
}
std::optional<size_t> MissionRunner::attackWithVehicles(std::span<const size_t> indices, bool targetIsVehicle, size_t targetIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.attackWithVehicles(indices, targetIsVehicle, targetIdx);
}
std::optional<size_t> MissionRunner::attackVehicle(std::span<const size_t> indices, size_t targetVehicleIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.attackVehicle(indices, targetVehicleIdx);
}
size_t MissionRunner::stopVehicles(std::span<const size_t> indices) {
    std::lock_guard lock(mutex_);
    if (modal_) return 0;
    const size_t stopped = units_.stopVehicles(indices);
    collectVehiclePoses();
    return stopped;
}
bool MissionRunner::canSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canSupportVehicle(supporterVehicleIds, targetVehicleIdx);
}
std::optional<size_t> MissionRunner::orderSupportVehicle(std::span<const size_t> supporterVehicleIds,
                                                           size_t targetVehicleIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    const size_t res = units_.orderSupportVehicle(supporterVehicleIds, targetVehicleIdx);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canSupportBuilding(supporterVehicleIds, buildingIdx);
}
std::optional<size_t> MissionRunner::orderSupportBuilding(std::span<const size_t> supporterVehicleIds,
                                                            size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    const size_t res = units_.orderSupportBuilding(supporterVehicleIds, buildingIdx);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canHealSoldier(std::span<const size_t> medicSoldierIds,
                                   std::span<const size_t> ambulanceVehicleIds,
                                   size_t targetSoldierIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canHealSoldier(medicSoldierIds, ambulanceVehicleIds, targetSoldierIdx);
}
std::optional<size_t> MissionRunner::orderHealSoldier(std::span<const size_t> medicSoldierIds,
                                                        std::span<const size_t> ambulanceVehicleIds,
                                                        size_t targetSoldierIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    const size_t res = units_.orderHealSoldier(medicSoldierIds, ambulanceVehicleIds, targetSoldierIdx);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetVehicleIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canEscortVehicle(soldierIds, vehicleIds, targetVehicleIdx);
}
std::optional<size_t> MissionRunner::orderEscortVehicle(std::span<const size_t> soldierIds,
                                                          std::span<const size_t> vehicleIds,
                                                          size_t targetVehicleIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    const size_t res = units_.orderEscortVehicle(soldierIds, vehicleIds, targetVehicleIdx);
    collectVehiclePoses();
    return res;
}
bool MissionRunner::canEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetSoldierIdx) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canEscortSoldier(soldierIds, vehicleIds, targetSoldierIdx);
}
std::optional<size_t> MissionRunner::orderEscortSoldier(std::span<const size_t> soldierIds,
                                                          std::span<const size_t> vehicleIds,
                                                          size_t targetSoldierIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    const size_t res = units_.orderEscortSoldier(soldierIds, vehicleIds, targetSoldierIdx);
    collectVehiclePoses();
    return res;
}
std::optional<bool> MissionRunner::toggleVehicleHoldFire(std::span<const size_t> indices) {
    std::lock_guard lock(mutex_);
    if (modal_ || indices.empty()) return std::nullopt;
    return units_.toggleVehicleHoldFire(indices);
}
std::optional<bool> MissionRunner::toggleMarchMode(std::span<const size_t> vehicleIds) {
    std::lock_guard lock(mutex_);
    if (modal_ || vehicleIds.empty()) return std::nullopt;
    const auto toggled = units_.toggleMarchMode(vehicleIds);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return toggled;
}
bool MissionRunner::isValidVehicleAttackTarget(size_t targetIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.isValidVehicleAttackTarget(targetIdx);
}
bool MissionRunner::canBoardVehicle(size_t vehicleIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canBoardVehicle(vehicleIdx, 0);
}
std::optional<size_t> MissionRunner::boardVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderBoardVehicle(soldierIds, vehicleIdx);
}
size_t MissionRunner::unloadVehicles(std::span<const size_t> vehicleIds) {
    std::lock_guard lock(mutex_);
    if (modal_) return 0;
    std::vector<SoldierPose> outPoses;
    const size_t count = units_.unloadVehicles(vehicleIds, &outPoses);
    mergeSoldierPoses(outPoses);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return count;
}
bool MissionRunner::canCaptureVehicle(size_t vehicleIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canCaptureVehicle(vehicleIdx, 0);
}
std::optional<size_t> MissionRunner::captureVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderCaptureVehicle(soldierIds, vehicleIdx);
}
std::optional<size_t> MissionRunner::attackGroundVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderAttackGroundVehicles(indices, mapX, mapY);
}
std::optional<size_t> MissionRunner::attackGroundSoldiers(std::span<const size_t> indices, int mapX, int mapY) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderAttackGroundSoldiers(indices, mapX, mapY);
}
size_t MissionRunner::layMine(std::span<const size_t> soldierIds) {
    std::lock_guard lock(mutex_);
    if (modal_) return 0;
    std::vector<SoldierPose> changed;
    const size_t n = units_.orderLayMine(soldierIds, &changed);
    mergeSoldierPoses(changed);
    auto evs = units_.takeMineEvents();
    if (!evs.empty()) mineEvents_.insert(mineEvents_.end(), evs.begin(), evs.end());
    return n;
}
bool MissionRunner::canDisarmMineAt(std::span<const size_t> soldierIds, int mapX, int mapY) const {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canDisarmMineAt(soldierIds, mapX, mapY);
}
std::optional<size_t> MissionRunner::orderDisarmMine(std::span<const size_t> soldierIds, int mapX, int mapY) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderDisarmMine(soldierIds, mapX, mapY);
}
size_t MissionRunner::orderGroupMove(unsigned group, int mapX, int mapY, bool onlyNonPlayer) {
    std::lock_guard lock(mutex_);
    return units_.orderGroupMove(group, mapX, mapY, onlyNonPlayer);
}
std::vector<MineEvent> MissionRunner::pollMineEvents() {
    std::lock_guard lock(mutex_);
    return std::exchange(mineEvents_, {});
}
std::vector<VisibleMine> MissionRunner::visibleMines() const {
    std::lock_guard lock(mutex_);
    return units_.visibleMines(0);
}
std::vector<IdleUnit> MissionRunner::idlePlayerUnits() const {
    std::lock_guard lock(mutex_);
    return units_.idlePlayerUnits();
}
bool MissionRunner::canTowVehicle(size_t tractorIdx, size_t gunIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canTowVehicle(tractorIdx, gunIdx, 0);
}
std::optional<size_t> MissionRunner::towVehicle(size_t tractorIdx, size_t gunIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    const size_t res = units_.orderTowVehicle(tractorIdx, gunIdx);
    collectVehiclePoses();
    return res;
}
size_t MissionRunner::unhitchVehicles(std::span<const size_t> vehicleIndices) {
    std::lock_guard lock(mutex_);
    if (modal_) return 0;
    const size_t n = units_.unhitchVehicles(vehicleIndices);
    collectVehiclePoses();
    return n;
}
bool MissionRunner::canGarrisonBuilding(size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    return !modal_ && units_.canGarrisonBuilding(buildingIdx, 0);
}
std::optional<size_t> MissionRunner::garrisonBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderGarrisonBuilding(soldierIds, buildingIdx);
}
size_t MissionRunner::evacuateBuilding(size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return 0;
    std::vector<SoldierPose> outPoses;
    const size_t count = units_.evacuateBuilding(buildingIdx, &outPoses);
    mergeSoldierPoses(outPoses);
    collectBuildingUpdates();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return count;
}
std::optional<bool> MissionRunner::toggleBuildingHoldFire(size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.toggleBuildingHoldFire(buildingIdx);
}
std::optional<bool> MissionRunner::toggleBuildingBinoculars(size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    std::vector<SoldierPose> outPoses;
    const auto active = units_.toggleBuildingBinoculars(buildingIdx, &outPoses);
    mergeSoldierPoses(outPoses);
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return active;
}
std::optional<size_t> MissionRunner::orderBuildingAttack(size_t buildingIdx, bool targetIsVehicle, size_t targetIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderBuildingAttack(buildingIdx, targetIsVehicle, targetIdx);
}
std::optional<size_t> MissionRunner::orderBuildingAttackBuilding(size_t buildingIdx, size_t targetBuildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderBuildingAttackBuilding(buildingIdx, targetBuildingIdx);
}
std::optional<size_t> MissionRunner::attackBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.orderAttackBuilding(soldierIds, buildingIdx);
}
std::optional<size_t> MissionRunner::attackBuildingWithVehicles(std::span<const size_t> vehicleIds, size_t buildingIdx) {
    std::lock_guard lock(mutex_);
    if (modal_) return {};
    return units_.attackBuildingWithVehicles(vehicleIds, buildingIdx);
}
std::vector<BuildingSnapshot> MissionRunner::pollBuildingUpdates() {
    std::lock_guard lock(mutex_);
    return std::exchange(buildingUpdates_, {});
}
std::array<assets::AirSupportSlot, 5> MissionRunner::airSupportFor(int owner) const {
    std::lock_guard lock(mutex_);
    return units_.airSupportFor(owner);
}
std::array<assets::AirSupportSlot, 5> MissionRunner::playerAirSupport() {
    std::lock_guard lock(mutex_);
    return units_.playerAirSupport();
}
bool MissionRunner::callAirSupport(assets::AirMissionKind kind, int targetX, int targetY) {
    std::lock_guard lock(mutex_);
    if (modal_) return false;
    const bool ok = units_.callAirSupport(kind, targetX, targetY, 0);
    if (ok) collectAirSorties();
    return ok;
}
std::vector<AirSortiePose> MissionRunner::pollAirSorties() {
    std::lock_guard lock(mutex_);
    return std::exchange(airSorties_, {});
}
std::vector<AirStrikeEvent> MissionRunner::pollAirStrikeEvents() {
    std::lock_guard lock(mutex_);
    return std::exchange(airEvents_, {});
}
TutorialUnits::Snapshot MissionRunner::takeSnapshot() {
    std::lock_guard lock(mutex_);
    return units_.takeSnapshot();
}
bool MissionRunner::loadSnapshot(const TutorialUnits::Snapshot& snap) {
    std::lock_guard lock(mutex_);
    std::vector<SoldierPose> restoredPoses;
    if (!units_.loadSnapshot(snap, &restoredPoses)) return false;
    poses_ = std::move(restoredPoses);
    vehiclePoses_.clear();
    mineEvents_.clear();
    buildingUpdates_.clear();
    airSorties_.clear();
    airEvents_.clear();
    scheduledReinforcements_.clear();
    pending_.clear();
    modal_ = false;
    collectVehiclePoses();
    collectBuildingUpdates();
    simTick_.store(units_.tick_, std::memory_order_relaxed);
    if (auto f = units_.takeFogSnapshot()) fog_ = std::move(f);
    return true;
}
void MissionRunner::setSpeedPercent(int percent) {
    speedPercent_.store(std::clamp(percent, 50, 300));
    changed_.notify_all();
}
size_t MissionRunner::spawnVehicle(const assets::VehicleUnit& unit) {
    std::lock_guard lock(mutex_);
    const size_t index = units_.spawnVehicle(unit);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return index;
}
bool MissionRunner::embarkImmediate(size_t soldierIdx, size_t vehicleIdx) {
    std::lock_guard lock(mutex_);
    const bool ok = units_.embarkImmediate(soldierIdx, vehicleIdx);
    if (ok) {
        collectVehiclePoses();
        if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    }
    return ok;
}
std::vector<TacticalOrderLine> MissionRunner::selectionOrderLines(std::span<const size_t> soldierIds,
                                                                    std::span<const size_t> vehicleIds) {
    std::lock_guard lock(mutex_);
    return units_.selectionOrderLines(soldierIds, vehicleIds);
}
std::vector<VehicleRangePreview> MissionRunner::selectionRangePreviews(std::span<const size_t> vehicleIds,
                                                                         std::span<const size_t> soldierIds) const {
    std::lock_guard lock(mutex_);
    return units_.selectionRangePreviews(vehicleIds, soldierIds);
}
std::vector<assets::Placement> MissionRunner::abandonVehicles(std::span<const size_t> vehicleIds) {
    std::lock_guard lock(mutex_);
    if (modal_ || vehicleIds.empty()) return {};
    std::vector<SoldierPose> changed;
    auto spawns = units_.abandonVehicles(vehicleIds, &changed);
    mergeSoldierPoses(changed);
    collectVehiclePoses();
    if (auto snap = units_.takeFogSnapshot()) fog_ = std::move(snap);
    return spawns;
}
void MissionRunner::addReserveUnit(const TutorialUnits::ReserveUnit& entry) {
    std::lock_guard lock(mutex_);
    units_.addReserveUnit(entry);
}
std::vector<TutorialUnits::ReserveUnit> MissionRunner::takeReserveUnits(unsigned group, int pool, std::size_t maxCount) {
    std::lock_guard lock(mutex_);
    return units_.takeReserveUnits(group, pool, maxCount);
}
bool MissionRunner::isElevatedMapPoint(int mapX, int mapY) const {
    std::lock_guard lock(mutex_);
    return units_.isElevatedMapPoint(mapX, mapY);
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
std::optional<std::pair<std::uint32_t, std::uint32_t>> MissionRunner::nextMissionSignal() const {
    std::lock_guard lock(mutex_);
    return nextMissionSignal_;
}
unsigned MissionRunner::countPendingReinforcementsByGroup(
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group) const {
    unsigned total = 0;
    for (std::size_t wi = 0; wi < waves.size(); ++wi) {
        if (spawnedReinforcementWaves_[wi]) continue;
        for (const auto& u : waves[wi]) {
            if (u.group >= 0 && static_cast<unsigned>(u.group) == group) {
                ++total;
            }
            for (const auto& pax : u.passengers) {
                if (pax.group >= 0 && static_cast<unsigned>(pax.group) == group) {
                    ++total;
                }
            }
        }
    }
    return total;
}
unsigned MissionRunner::countPendingReinforcementsByGroupTeam(
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, unsigned group, int team) const {
    unsigned total = 0;
    for (std::size_t wi = 0; wi < waves.size(); ++wi) {
        if (spawnedReinforcementWaves_[wi]) continue;
        const int owner = reinforcementWaveOwner_[wi];
        if (owner < 0 || units_.playerTeam(owner) != team) continue;
        for (const auto& u : waves[wi]) {
            if (u.group >= 0 && static_cast<unsigned>(u.group) == group) {
                ++total;
            }
            for (const auto& pax : u.passengers) {
                if (pax.group >= 0 && static_cast<unsigned>(pax.group) == group) {
                    ++total;
                }
            }
        }
    }
    return total;
}
unsigned MissionRunner::countPendingReinforcementsByPlayer(
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves, int owner) const {
    if (owner < 0 || owner >= 4) return 0u;
    unsigned total = 0;
    for (std::size_t wi = 0; wi < waves.size(); ++wi) {
        if (spawnedReinforcementWaves_[wi]) continue;
        if (reinforcementWaveOwner_[wi] == owner) {
            for (const auto& u : waves[wi]) {
                total += 1u + static_cast<unsigned>(u.passengers.size());
            }
        }
    }
    return total;
}
MissionQueries MissionRunner::makeQueriesLocked(
    std::span<const std::uint8_t> zones,
    const std::array<std::vector<assets::ReinforcementUnit>, 64>& waves,
    std::span<const std::pair<int, int>> markers) const {
    return {
        [this, zones](unsigned zone, unsigned player) {
            return units_.countInZone(zones, zone, int(player));
        },
        [this](unsigned group) {
            return units_.countAliveByGroup(group);
        },
        [this](unsigned player) {
            return units_.countAliveByPlayer(int(player));
        },
        [this, zones](unsigned zone, unsigned group) {
            return units_.countGroupInZone(zones, zone, group);
        },
        [this](unsigned id) -> std::optional<unsigned> {
            if (auto g = units_.lastGroupHitTick(id)) return g;
            return units_.lastHitTick(id);
        },
        [this, &waves](unsigned group, unsigned mask) {
            unsigned total = units_.countByGroupMask(group, mask);
            if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroup(waves, group);
            return total;
        },
        [this, &waves](int owner, unsigned mask) {
            unsigned total = units_.countByPlayerMask(owner, mask);
            if ((mask & 8u) != 0u) total += countPendingReinforcementsByPlayer(waves, owner);
            return total;
        },
        [this, markers](unsigned marker) -> bool {
            if (marker >= markers.size()) return true;
            const auto [mx, my] = markers[marker];
            return units_.isObjectDestroyedAt(mx, my);
        },
        [this, &waves](unsigned group, int team, unsigned mask) {
            unsigned total = units_.countByGroupTeamMask(group, team, mask);
            if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroupTeam(waves, group, team);
            return total;
        }
    };
}
unsigned MissionRunner::countByGroupMask(unsigned group, unsigned mask) const {
    std::lock_guard lock(mutex_);
    unsigned total = units_.countByGroupMask(group, mask);
    if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroup(tutorial_.data().reinforcements, group);
    return total;
}
unsigned MissionRunner::countByGroupTeamMask(unsigned group, int team, unsigned mask) const {
    std::lock_guard lock(mutex_);
    unsigned total = units_.countByGroupTeamMask(group, team, mask);
    if ((mask & 8u) != 0u) total += countPendingReinforcementsByGroupTeam(tutorial_.data().reinforcements, group, team);
    return total;
}
unsigned MissionRunner::countByPlayerMask(int owner, unsigned mask) const {
    std::lock_guard lock(mutex_);
    unsigned total = units_.countByPlayerMask(owner, mask);
    if ((mask & 8u) != 0u) total += countPendingReinforcementsByPlayer(tutorial_.data().reinforcements, owner);
    return total;
}
}
