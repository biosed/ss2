#pragma once

#include <algorithm>
#include <optional>

namespace ss2::hidpi {

// Two-dimensional coordinate pair in logical window or physical pixel space.
struct Point2D {
    double x = 0.0;
    double y = 0.0;
};

// Converts logical window coordinates to physical framebuffer pixels, returning std::nullopt on non-positive sizes.
inline std::optional<Point2D> windowToPhysical(
    double windowX, double windowY, int windowW, int windowH, int pixelW, int pixelH) noexcept {
    if (windowW <= 0 || windowH <= 0 || pixelW <= 0 || pixelH <= 0) return std::nullopt;
    return Point2D{
        windowX * static_cast<double>(pixelW) / static_cast<double>(windowW),
        windowY * static_cast<double>(pixelH) / static_cast<double>(windowH),
    };
}

// Returns the logical center point expected by VulkanWindow::mapPosition, falling back when window dimensions are invalid.
inline Point2D logicalWindowCenter(int windowW, int windowH, int fallbackW = 1280, int fallbackH = 720) noexcept {
    const int w = windowW > 0 ? windowW : std::max(1, fallbackW);
    const int h = windowH > 0 ? windowH : std::max(1, fallbackH);
    return Point2D{static_cast<double>(w) * 0.5, static_cast<double>(h) * 0.5};
}

// Converts the map viewport height (physical pixels above an opaque HUD bar) back into logical window units for mapPosition.
inline double logicalMapViewHeight(int windowH, int pixelH, int hudHeightPx) noexcept {
    if (windowH <= 0) return 1.0;
    const int clampedHud = std::max(0, hudHeightPx);
    if (pixelH <= 0) return static_cast<double>(std::max(1, windowH - clampedHud));
    const int physicalViewH = std::max(1, pixelH - clampedHud);
    return static_cast<double>(physicalViewH) * static_cast<double>(windowH) / static_cast<double>(pixelH);
}

}  // namespace ss2::hidpi
