#pragma once

#include "ss2/simulation.hpp"
#include "assets/map_image.hpp"
#include <atomic>
#include <memory>
#include <vector>
#include <utility>

struct SDL_Window;
union SDL_Event;

namespace ss2 {
struct ValidationReport {
    std::atomic<unsigned> errors{0};
};

struct MapRect { int x, y, width, height; };

class VulkanWindow {
public:
    explicit VulkanWindow(SDL_Window*, ValidationReport* = nullptr);
    ~VulkanWindow();
    void setMap(const assets::MapImage&);
    void focusMap(int x, int y);
    std::pair<double, double> mapPosition(float windowX, float windowY) const;
    void highlight(std::vector<MapRect>);
    void handleEvent(const SDL_Event&);
    std::pair<unsigned, unsigned> extent() const;
    bool present(const Snapshot&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
