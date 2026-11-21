#include "sim/tutorial.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace ss2 {
namespace {
bool compare(uint32_t mode, int32_t lhs, int32_t rhs) {
    switch (mode) {
    case 0: return lhs == rhs;
    case 1: return lhs > rhs;
    case 2: return lhs < rhs;
    default: throw std::runtime_error("Unsupported mission comparison");
    }
}
}

unsigned tutorialTurnDelay(const std::filesystem::path& gameDir) {
    std::ifstream file(gameDir / "sudtest.ini");
    bool game = false;
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with('[')) game = line == "[Game]";
        if (game && line.starts_with("TurnDelay=")) {
            unsigned value{};
            const auto [ptr, err] = std::from_chars(line.data() + 10, line.data() + line.size(), value);
            if (err != std::errc{} || ptr != line.data() + line.size() || !value)
                throw std::runtime_error("Invalid Gold Game/TurnDelay");
            return value;
        }
    }
    return 40; // Default passed by the retail single-player initialization.
}

namespace {
MissionQueries legacyQueries(unsigned inZone2, unsigned enemyGroup3Alive) {
    return {
        [inZone2](unsigned zone, unsigned player) -> unsigned {
            if (zone != 2 || player != 0) throw std::runtime_error("Unsupported mission zone query");
            return inZone2;
        },
        [enemyGroup3Alive](unsigned group) -> unsigned {
            if (group == 3) return enemyGroup3Alive;
            return 100; // non-legacy groups inactive in legacy 16-script mode
        },
        {},
        {},
        {},
        {},
        {}
    };
}
}

void Tutorial::initActive(bool fullMission) {
    if (data_.scripts.empty()) throw std::runtime_error("Incomplete tutorial script set");
    active_.assign(data_.scripts.size(), false);
    const std::size_t limit = fullMission ? data_.scripts.size()
                                          : std::min<std::size_t>(16, data_.scripts.size());
    for (std::size_t i = 0; i < limit; ++i) active_[i] = true;
    if (!fullMission || active_.size() == 30) {
        if (active_.size() > 2) active_[2] = false;  // AI visibility trigger belongs to the world simulation.
    }
    if (fullMission && active_.size() == 30) {
        active_[18] = false; // Reinforcement group 71 spawns via OP(40), not present at start.
        active_[25] = false; // Allied vehicle group 50 destruction requires anti-tank mechanics.
    }
}

Tutorial::Tutorial(assets::TutorialData data, unsigned initialZoneCount, unsigned initialEnemyGroup3)
    : data_(std::move(data)) {
    initActive(false);
    scan(legacyQueries(initialZoneCount, initialEnemyGroup3));
}

Tutorial::Tutorial(assets::TutorialData data, MissionQueries queries) : data_(std::move(data)) {
    initActive(true);
    scan(queries);
}

void Tutorial::step(unsigned inZone, unsigned enemyGroup3Alive) {
    step(legacyQueries(inZone, enemyGroup3Alive));
}

void Tutorial::step(const MissionQueries& queries) {
    ++tick_;
    if (countdown_ > 0) --countdown_;
    if ((tick_ & 63u) == 3) scan(queries);
}

std::vector<MissionEvent> Tutorial::takeEvents() { return std::exchange(events_, {}); }

void Tutorial::scan(const MissionQueries& queries) {
    for (unsigned i = 0; i < active_.size(); ++i)
        if (active_[i]) evaluate(i, queries);
}

void Tutorial::evaluate(unsigned script, const MissionQueries& queries) {
    std::vector<uint32_t> stack;
    const auto pop = [&]() {
        if (stack.empty()) throw std::runtime_error("Mission stack underflow");
        auto value = stack.back();
        stack.pop_back();
        return value;
    };
    constexpr uint32_t variableBase = 0x7fffff00;
    const auto valueOf = [&](uint32_t n) -> uint32_t {
        if (n < variableBase) return n;
        const auto idx = n - variableBase;
        return idx < variables_.size() ? variables_[idx] : 0u;
    };
    for (auto op : data_.scripts[script]) {
        if (op == 0x7fffffff) return;
        if (op & 0x80000000u) {
            if (stack.size() >= 1024) throw std::runtime_error("Mission stack exceeds limit");
            stack.push_back(op & 0x7fffffff);
            continue;
        }
        switch (op) {
        case 1: {
            const auto b = pop(), a = pop();
            stack.push_back(a && b);
            break;
        }
        case 2: {
            const auto b = pop(), a = pop();
            stack.push_back(a || b);
            break;
        }
        case 3: {
            // 4th operand is a unit-class bitmask (bit0 active-crew, bit1 unmanned, bit2 reserve, bit3 pending reinforcements).
            const auto threshold = pop(), mode = pop(), group = pop(), mask = pop();
            const unsigned alive = queries.countByGroupMask ? queries.countByGroupMask(group, mask)
                                 : (queries.countAliveByGroup ? queries.countAliveByGroup(group) : 0u);
            stack.push_back(compare(mode, int32_t(alive), int32_t(threshold)));
            break;
        }
        case 4: {
            const auto threshold = pop(), mode = pop(), player = pop(), mask = pop();
            const unsigned alive = queries.countByPlayerMask ? queries.countByPlayerMask(int(player), mask)
                                 : (queries.countAliveByPlayer ? queries.countAliveByPlayer(player) : 0u);
            stack.push_back(compare(mode, int32_t(alive), int32_t(threshold)));
            break;
        }
        case 5: {
            const auto zone = pop(), group = pop(), threshold = pop(), mode = pop();
            const unsigned count = queries.countGroupInZone ? queries.countGroupInZone(zone, group) : 0u;
            stack.push_back(compare(mode, int32_t(count), int32_t(threshold)));
            break;
        }
        case 6: {
            const auto zone = pop(), player = pop(), threshold = pop(), mode = pop();
            const unsigned count = queries.countInZone ? queries.countInZone(zone, player) : 0u;
            stack.push_back(compare(mode, int32_t(count), int32_t(threshold)));
            break;
        }
        case 7: if (!pop()) return; break;
        case 11: active_[script] = false; break;
        case 12: {
            const auto seconds = pop(), id = pop();
            if (id < timers_.size()) timers_[id] = tick_ + seconds * 25u;
            break;
        }
        case 13: {
            const auto id = pop();
            if (id < timers_.size()) timers_[id] = 0;
            break;
        }
        case 14: {
            const auto zone = pop();
            if (zone < data_.locations.size()) {
                events_.push_back({MissionEvent::Camera, zone});
            }
            break;
        }
        case 15: case 57: case 87: case 88: {
            std::optional<uint32_t> marker;
            if (op == 57 || op == 88) marker = pop();
            const auto phrase = pop();
            if (phrase < data_.phrases.size()) {
                events_.push_back({(op == 15 || op == 57) ? MissionEvent::Message : MissionEvent::Dialog, phrase});
            }
            if (marker && *marker < data_.markers.size()) {
                events_.push_back({MissionEvent::Marker, *marker});
            }
            break;
        }
        case 16: {
            const auto seconds = pop();
            countdown_ = int32_t(seconds * 25u);
            break;
        }
        case 17: {
            // Gold packs (nextMissionId << 8 | nextSubIdx); the campaign router advances the mission
            // (0x100 -> 001000, 257 -> finale win, 258 -> defeat/retry), not a player/team change.
            const auto packed = pop();
            events_.push_back({MissionEvent::PlayerTeam, packed, packed >> 8, packed & 0xffu, 0u, 0u});
            break;
        }
        case 18: {
            const auto result = pop();
            events_.push_back({result == 2u ? MissionEvent::Defeat : MissionEvent::Victory, result});
            break;
        }
        case 8: {
            for (int k = 0; k < 5; ++k) (void)pop(); // spawn / reserve deployment action
            break;
        }
        case 9: case 31: case 91: {
            for (int k = 0; k < 4; ++k) (void)pop(); // condition query
            stack.push_back(0);
            break;
        }
        case 19: {
            const auto behavior = pop(), group = pop();
            if (group < groupAi_.size()) {
                groupAi_[group].behavior = behavior;
                const auto z = groupAi_[group].zone1 ? groupAi_[group].zone1 : groupAi_[group].zone2;
                if (behavior != 0 && z > 0 && z < data_.locations.size()) {
                    events_.push_back({MissionEvent::GroupOrder, group, behavior, 0u, z, 0u});
                }
            }
            break;
        }
        case 20: {
            const auto loc = pop(), group = pop();
            if (group < groupAi_.size()) {
                groupAi_[group].zone1 = loc;
                if (groupAi_[group].behavior != 0 && loc > 0 && loc < data_.locations.size()) {
                    events_.push_back({MissionEvent::GroupOrder, group, groupAi_[group].behavior, 0u, loc, 0u});
                }
            }
            break;
        }
        case 21: {
            const auto loc = pop(), group = pop();
            if (group < groupAi_.size()) {
                groupAi_[group].zone2 = loc;
                if (groupAi_[group].behavior != 0 && groupAi_[group].zone1 == 0 && loc > 0 && loc < data_.locations.size()) {
                    events_.push_back({MissionEvent::GroupOrder, group, groupAi_[group].behavior, 0u, loc, 0u});
                }
            }
            break;
        }
        case 22: {
            const auto val = pop(), group = pop();
            if (group < groupAi_.size()) groupAi_[group].aiFlag0 = val;
            break;
        }
        case 23: {
            const auto val = pop(), group = pop();
            if (group < groupAi_.size()) groupAi_[group].aiFlag1 = val;
            break;
        }
        case 24: case 25: {
            const auto slot = pop(), count = pop(), owner = pop();
            if (owner < 4u && slot < 5u && count > 0u) {
                events_.push_back({op == 24u ? MissionEvent::AirSupportAddPlanes : MissionEvent::AirSupportAddSorties,
                                   slot, owner, 0u, count, 0u});
            }
            break;
        }
        case 26: {
            const auto val = pop();
            stack.push_back(val == 0u ? 1u : 0u);
            break;
        }
        case 41: {
            const auto marker = pop();
            const bool destroyed = queries.isMarkerObjectDestroyed
                ? queries.isMarkerObjectDestroyed(marker)
                : false;
            stack.push_back(destroyed ? 1u : 0u);
            break;
        }
        case 42: {
            const auto extra = pop(), marker = pop(), owner = pop(), slot = pop(), count = pop();
            if (owner < 4u && slot < 5u && count > 0u && marker < data_.markers.size()) {
                events_.push_back({MissionEvent::AirStrike, marker, owner, slot, count, extra});
            }
            break;
        }
        case 58: {
            const auto newOwner = pop(), group = pop();
            if (newOwner < 4u) {
                events_.push_back({MissionEvent::GroupTransferOwner, group, newOwner, 0u, 0u, 0u});
            }
            break;
        }
        case 59: case 60: case 77: case 78: case 92: {
            const auto target = pop(), group = pop();
            const bool isMarker = (op == 60u || op == 78u);
            const unsigned mode = (op == 92u) ? 2u : ((op == 59u || op == 60u) ? 1u : 0u);
            const bool valid = isMarker ? (target < data_.markers.size())
                                        : (target > 0u && target < data_.locations.size());
            if (valid) {
                events_.push_back({MissionEvent::GroupBombard, group, isMarker ? 1u : 0u, mode, target, 0u});
            }
            break;
        }
        case 82: {
            const auto zone = pop(), oldGroup = pop(), newGroup = pop();
            events_.push_back({MissionEvent::GroupReassignInZone, oldGroup, newGroup, 0u, zone, 0u});
            break;
        }
        case 93: {
            const auto threshold = pop(), mode = pop(), group = pop(), team = pop(), mask = pop();
            const unsigned count = queries.countByGroupTeamMask
                ? queries.countByGroupTeamMask(group, int(team), mask) : 0u;
            stack.push_back(compare(mode, int32_t(count), int32_t(threshold)));
            break;
        }
        case 63: case 71: case 84: case 85: case 86: {
            (void)pop(); (void)pop(); // 2-arg mission state / bridge action
            break;
        }
        case 72: case 75: {
            (void)pop(); (void)pop(); (void)pop(); // 3-arg objective action
            break;
        }
        case 33: {
            for (int k = 0; k < 6; ++k) (void)pop(); // 6-arg unit/area query
            stack.push_back(0);
            break;
        }
        case 39: {
            for (int k = 0; k < 8; ++k) (void)pop(); // 8-arg custom reinforcement template
            break;
        }
        case 46: case 83: {
            for (int k = 0; k < 4; ++k) (void)pop(); // 4-arg objective / spawn action
            break;
        }
        case 62: {
            (void)pop(); // 1-arg objective / diplomacy toggle
            break;
        }
        case 90: {
            (void)pop(); (void)pop(); (void)pop(); // 3-arg condition query
            stack.push_back(0);
            break;
        }
        case 40: {
            const auto delaySec = pop(), destZone = pop(), gateIdx = pop(), waveIdx = pop(), owner = pop();
            if (waveIdx < data_.reinforcements.size() && !data_.reinforcements[waveIdx].empty()) {
                events_.push_back({MissionEvent::Reinforcement, waveIdx, owner, gateIdx, destZone, delaySec});
            }
            break;
        }
        case 27: {
            const auto id = pop();
            const uint32_t until = id < timers_.size() ? timers_[id] : 0u;
            const bool expired = until != 0u && int32_t(tick_ - until) >= 0;
            if (expired && id < timerPeriods_.size() && timerPeriods_[id] != 0u) {
                timers_[id] = tick_ + timerPeriods_[id];
            }
            stack.push_back(expired);
            break;
        }
        case 28: {
            const auto seconds = pop(), mode = pop();
            stack.push_back(compare(mode, countdown_, int32_t(seconds * 25u)));
            break;
        }
        case 29: {
            const auto seconds = pop(), mode = pop();
            stack.push_back(compare(mode, int32_t(tick_), int32_t(seconds * 25u)));
            break;
        }
        case 47: {
            const auto value = valueOf(pop()), address = pop();
            if (address >= variableBase && address - variableBase < variables_.size()) {
                variables_[address - variableBase] = value;
            }
            break;
        }
        case 48: {
            const auto delta = valueOf(pop()), mode = pop(), address = pop();
            if (address >= variableBase && address - variableBase < variables_.size()) {
                if (mode == 0) variables_[address - variableBase] += delta;
                else if (mode == 1) variables_[address - variableBase] -= delta;
            }
            break;
        }
        case 49: {
            // Gold compares ticks-since-last-hit against seconds*25; a never-hit player yields false.
            const auto seconds = pop(), mode = pop(), player = pop();
            const auto last = queries.lastHitTick ? queries.lastHitTick(player) : std::nullopt;
            if (!last) { stack.push_back(0); break; }
            stack.push_back(compare(mode, int32_t(tick_ - *last), int32_t(seconds * 25u)));
            break;
        }
        case 50: {
            const auto rhs = valueOf(pop()), mode = pop(), lhs = valueOf(pop());
            stack.push_back(compare(mode, int32_t(lhs), int32_t(rhs)));
            break;
        }
        case 51: stack.push_back(1); active_[script] = false; break;
        case 76: {
            const auto periodSec = pop(), id = pop();
            if (id < timerPeriods_.size()) timerPeriods_[id] = periodSec * 25u;
            break;
        }
        case 79: {
            const auto group = pop(), hi = pop(), lo = pop();
            if (group < groupAi_.size() && groupAi_[group].waypoints.size() < 16) {
                groupAi_[group].waypoints.emplace_back(lo, hi);
            }
            break;
        }
        case 80: {
            const auto group = pop();
            if (group < groupAi_.size()) {
                auto& ai = groupAi_[group];
                if (!ai.waypoints.empty()) {
                    // Gold advances the cursor before reading, wrapping at the queue length.
                    ai.waypointIndex = (ai.waypointIndex + 1) % ai.waypoints.size();
                    ai.zone1 = ai.waypoints[ai.waypointIndex].first;
                    ai.zone2 = ai.waypoints[ai.waypointIndex].second;
                    const auto z = ai.zone1 ? ai.zone1 : ai.zone2;
                    // Patrol groups carry no OP(19) behavior; the waypoint advance itself is the order.
                    if (z > 0 && z < data_.locations.size()) {
                        events_.push_back({MissionEvent::GroupOrder, group, ai.behavior, 0u, z, 0u});
                    }
                }
            }
            break;
        }
        case 89: {
            const auto player = pop(), radius = pop(), marker = pop();
            if (marker < data_.markers.size()) {
                events_.push_back({MissionEvent::FogReveal, marker, player, 0u, std::min<unsigned>(radius, 32u), 0u});
            }
            break;
        }
        default:
            // Unknown CAOF opcode has unknown stack arity; retire this trigger and
            // stop so it neither misaligns the operand stack nor re-fires each scan.
            active_[script] = false;
            return;
        }
    }
    throw std::runtime_error("Mission script has no terminator");
}
}
