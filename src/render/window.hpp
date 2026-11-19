#pragma once

#include "ss2/simulation.hpp"
#include "assets/map_image.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
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
    // Replaces a rectangle of the current map with the same RGBA8 layout as setMap.
    // Patches are queued, applied in call order before the next presented frame, and
    // clipped to the map. Throws if there is no map or the patch is malformed.
    void updateMapRegion(int x, int y, const assets::MapImage& patch);
    // Reads the map image back, including queued patches. Slow; for tests and screenshots.
    assets::MapImage captureMap();
    void focusMap(int x, int y);
    std::pair<double, double> mapPosition(float windowX, float windowY) const;
    void highlight(std::vector<MapRect>);
    // An opaque RGBA8 panel drawn centred over everything else, clipped to the window.
    void setOverlay(unsigned width, unsigned height, std::span<const std::uint8_t> rgba);
    void clearOverlay();
    void handleEvent(const SDL_Event&);
    std::pair<unsigned, unsigned> extent() const;
    bool present(const Snapshot&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
