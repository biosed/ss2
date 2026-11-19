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

Tutorial::Tutorial(assets::TutorialData data, unsigned initialZoneCount) : data_(std::move(data)) {
    if (data_.scripts.size() < active_.size()) throw std::runtime_error("Incomplete tutorial script set");
    active_.fill(true);
    active_[2] = false; // AI visibility trigger belongs to the world simulation.
    scan(initialZoneCount);
}

void Tutorial::step(unsigned inZone) {
    ++tick_;
    if ((tick_ & 63u) == 3) scan(inZone);
}

std::vector<MissionEvent> Tutorial::takeEvents() { return std::exchange(events_, {}); }

void Tutorial::scan(unsigned inZone) {
    for (unsigned i = 0; i < active_.size(); ++i)
        if (active_[i]) evaluate(i, inZone);
}

void Tutorial::evaluate(unsigned script, unsigned inZone) {
    std::vector<uint32_t> stack;
    const auto pop = [&]() {
        if (stack.empty()) throw std::runtime_error("Mission stack underflow");
        auto value = stack.back();
        stack.pop_back();
        return value;
    };
    constexpr uint32_t variableBase = 0x7fffff00;
    const auto valueOf = [&](uint32_t n) { return n >= variableBase ? variables_.at(n-variableBase) : n; };
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
        case 6: {
            const auto zone = pop(), player = pop(), threshold = pop(), mode = pop();
            if (zone != 2 || player != 0) throw std::runtime_error("Unsupported mission zone query");
            stack.push_back(compare(mode, inZone, threshold));
            break;
        }
        case 7: if (!pop()) return; break;
        case 11: active_[script] = false; break;
        case 12: {
            const auto seconds = pop(), id = pop();
            timers_.at(id) = tick_ + seconds * 25u;
            break;
        }
        case 14: {
            const auto zone = pop();
            data_.locations.at(zone);
            events_.push_back({MissionEvent::Camera, zone});
            break;
        }
        case 15: case 87: case 88: {
            std::optional<uint32_t> marker;
            if (op == 88) marker = pop();
            const auto phrase = pop();
            (void)data_.phrases.at(phrase);
            events_.push_back({op == 15 ? MissionEvent::Message : MissionEvent::Dialog, phrase});
            if (marker) {
                data_.markers.at(*marker);
                events_.push_back({MissionEvent::Marker, *marker});
            }
            break;
        }
        case 27: {
            const auto until = timers_.at(pop());
            stack.push_back(until && int32_t(tick_ - until) >= 0);
            break;
        }
        case 29: {
            const auto seconds = pop(), mode = pop();
            stack.push_back(compare(mode, tick_, seconds*25u));
            break;
        }
        case 47: {
            const auto value = valueOf(pop()), address = pop();
            variables_.at(address-variableBase) = value;
            break;
        }
        case 50: {
            const auto rhs = valueOf(pop()), mode = pop(), lhs = valueOf(pop());
            stack.push_back(compare(mode, lhs, rhs));
            break;
        }
        case 51: stack.push_back(1); active_[script] = false; break;
        default: throw std::runtime_error("Unsupported Tutorial I opening opcode " + std::to_string(op));
        }
    }
    throw std::runtime_error("Mission script has no terminator");
}
}
