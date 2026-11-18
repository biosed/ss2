#pragma once

#include "ss2/simulation.hpp"
#include <atomic>
#include <memory>
#include <utility>

struct SDL_Window;
union SDL_Event;

namespace ss2 {
struct ValidationReport {
    std::atomic<unsigned> errors{0};
};

class VulkanWindow {
public:
    explicit VulkanWindow(SDL_Window*, ValidationReport* = nullptr);
    ~VulkanWindow();
    void handleEvent(const SDL_Event&);
    std::pair<unsigned, unsigned> extent() const;
    bool present(const Snapshot&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
