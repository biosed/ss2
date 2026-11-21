#include "ui/hidpi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int gFailures = 0;

void check(bool cond, const char* msg) {
    if (cond) return;
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++gFailures;
}

bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) <= eps;
}

// Mirrors VulkanWindow::windowScale() * input coordinates inside VulkanWindow::mapPosition().
ss2::hidpi::Point2D simulateMapPositionScale(double logicalX, double logicalY, int windowW, int windowH, int pixelW, int pixelH) {
    const double sx = static_cast<double>(pixelW) / static_cast<double>(windowW);
    const double sy = static_cast<double>(pixelH) / static_cast<double>(windowH);
    return {logicalX * sx, logicalY * sy};
}

int physicalMapViewHeight(int pixelH, int hudHeightPx) {
    return std::max(1, pixelH - std::max(0, hudHeightPx));
}

void testWindowToPhysical() {
    // 1.0x scale: 1280x720 -> 1280x720
    {
        const auto p = ss2::hidpi::windowToPhysical(640.0, 360.0, 1280, 720, 1280, 720);
        check(p.has_value() && near(p->x, 640.0) && near(p->y, 360.0), "windowToPhysical 1.0x center");
    }
    // 1.25x scale: 1280x720 -> 1600x900
    {
        const auto p1 = ss2::hidpi::windowToPhysical(640.0, 360.0, 1280, 720, 1600, 900);
        check(p1.has_value() && near(p1->x, 800.0) && near(p1->y, 450.0), "windowToPhysical 1.25x center");
        const auto p2 = ss2::hidpi::windowToPhysical(1024.0, 576.0, 1280, 720, 1600, 900);
        check(p2.has_value() && near(p2->x, 1280.0) && near(p2->y, 720.0), "windowToPhysical 1.25x arbitrary");
    }
    // 1.5x scale: 1280x720 -> 1920x1080
    {
        const auto p = ss2::hidpi::windowToPhysical(640.0, 360.0, 1280, 720, 1920, 1080);
        check(p.has_value() && near(p->x, 960.0) && near(p->y, 540.0), "windowToPhysical 1.5x center");
    }
    // 2.0x scale: 1280x720 -> 2560x1440
    {
        const auto p = ss2::hidpi::windowToPhysical(640.0, 360.0, 1280, 720, 2560, 1440);
        check(p.has_value() && near(p->x, 1280.0) && near(p->y, 720.0), "windowToPhysical 2.0x center");
    }
    // Regression guards for Menu::rowAt when logical or physical window sizes are non-positive.
    check(!ss2::hidpi::windowToPhysical(100.0, 100.0, 0, 720, 1600, 900).has_value(), "windowToPhysical rejects windowW==0");
    check(!ss2::hidpi::windowToPhysical(100.0, 100.0, 1280, 0, 1600, 900).has_value(), "windowToPhysical rejects windowH==0");
    check(!ss2::hidpi::windowToPhysical(100.0, 100.0, -1, 720, 1600, 900).has_value(), "windowToPhysical rejects windowW<0");
    check(!ss2::hidpi::windowToPhysical(100.0, 100.0, 1280, 720, 0, 900).has_value(), "windowToPhysical rejects pixelW==0");
    check(!ss2::hidpi::windowToPhysical(100.0, 100.0, 1280, 720, 1600, 0).has_value(), "windowToPhysical rejects pixelH==0");
    check(!ss2::hidpi::windowToPhysical(100.0, 100.0, 1280, 720, -1600, -900).has_value(), "windowToPhysical rejects negative pixel dims");
}

void testLogicalWindowCenter() {
    struct Case {
        int winW;
        int winH;
        int pixW;
        int pixH;
        const char* label;
    };
    const Case cases[] = {
        {1280, 720, 1280, 720, "1.0x"},
        {1280, 720, 1600, 900, "1.25x"},
        {1280, 720, 1920, 1080, "1.5x"},
        {1280, 720, 2560, 1440, "2.0x"},
    };
    for (const auto& c : cases) {
        const auto center = ss2::hidpi::logicalWindowCenter(c.winW, c.winH);
        const auto scaled = simulateMapPositionScale(center.x, center.y, c.winW, c.winH, c.pixW, c.pixH);
        check(near(scaled.x, c.pixW * 0.5) && near(scaled.y, c.pixH * 0.5), c.label);
    }

    // Explicitly verify the pre-fix 1.25x bug: passing pixel dimensions (1600x900) double-scaled to (1000, 562.5).
    const auto buggyScaled = simulateMapPositionScale(1600.0 * 0.5, 900.0 * 0.5, 1280, 720, 1600, 900);
    check(near(buggyScaled.x, 1000.0) && near(buggyScaled.y, 562.5) && !near(buggyScaled.x, 800.0) && !near(buggyScaled.y, 450.0),
          "1.25x double-scale regression baseline check");

    // Fallback check when window size is zero or negative.
    const auto fb = ss2::hidpi::logicalWindowCenter(0, -10);
    check(near(fb.x, 640.0) && near(fb.y, 360.0), "logicalWindowCenter fallback");
}

void testLogicalMapViewHeight() {
    // 1.0x scale: 720 logical, 720 pixel, 180 HUD -> 540 logical -> 540 physical
    {
        const double h = ss2::hidpi::logicalMapViewHeight(720, 720, 180);
        const auto scaled = simulateMapPositionScale(1280.0, h, 1280, 720, 1280, 720);
        check(near(h, 540.0) && near(scaled.y, physicalMapViewHeight(720, 180)), "logicalMapViewHeight 1.0x");
    }
    // 1.25x scale: 720 logical, 900 pixel, 180 HUD -> 576 logical -> 720 physical == (900 - 180)
    {
        const double h = ss2::hidpi::logicalMapViewHeight(720, 900, 180);
        const auto scaled = simulateMapPositionScale(1280.0, h, 1280, 720, 1600, 900);
        check(near(h, 576.0) && near(scaled.y, physicalMapViewHeight(900, 180)) && near(scaled.y, 720.0),
              "logicalMapViewHeight 1.25x");
        // Explicitly verify pre-fix subtraction bug: (720 - 180) = 540 logical -> 675 physical (45px short).
        const auto buggyScaled = simulateMapPositionScale(1280.0, 720.0 - 180.0, 1280, 720, 1600, 900);
        check(near(buggyScaled.y, 675.0) && !near(buggyScaled.y, 720.0), "1.25x hud-subtraction regression baseline check");
    }
    // 1.25x scale with non-divisible HUD height: 900 - 177 = 723 physical -> 578.4 logical -> 723.0 physical
    {
        const double h = ss2::hidpi::logicalMapViewHeight(720, 900, 177);
        const auto scaled = simulateMapPositionScale(1280.0, h, 1280, 720, 1600, 900);
        check(near(h, 578.4) && near(scaled.y, 723.0), "logicalMapViewHeight 1.25x fractional");
    }
    // 1.5x scale: 720 logical, 1080 pixel, 216 HUD -> 576 logical -> 864 physical
    {
        const double h = ss2::hidpi::logicalMapViewHeight(720, 1080, 216);
        const auto scaled = simulateMapPositionScale(1280.0, h, 1280, 720, 1920, 1080);
        check(near(h, 576.0) && near(scaled.y, 864.0), "logicalMapViewHeight 1.5x");
    }
    // 2.0x scale: 720 logical, 1440 pixel, 288 HUD -> 576 logical -> 1152 physical
    {
        const double h = ss2::hidpi::logicalMapViewHeight(720, 1440, 288);
        const auto scaled = simulateMapPositionScale(1280.0, h, 1280, 720, 2560, 1440);
        check(near(h, 576.0) && near(scaled.y, 1152.0), "logicalMapViewHeight 2.0x");
    }
    // Edge cases
    check(near(ss2::hidpi::logicalMapViewHeight(720, 900, 0), 720.0), "logicalMapViewHeight zero hud");
    check(near(ss2::hidpi::logicalMapViewHeight(720, 900, -10), 720.0), "logicalMapViewHeight negative hud");
    {
        const double h = ss2::hidpi::logicalMapViewHeight(720, 900, 1000);
        const auto scaled = simulateMapPositionScale(1280.0, h, 1280, 720, 1600, 900);
        check(near(scaled.y, 1.0), "logicalMapViewHeight clamped hud >= pixelH");
    }
    check(near(ss2::hidpi::logicalMapViewHeight(720, 0, 180), 540.0), "logicalMapViewHeight pixelH<=0 fallback");
    check(near(ss2::hidpi::logicalMapViewHeight(0, 900, 180), 1.0), "logicalMapViewHeight windowH<=0 guard");
}

}  // namespace

int main() {
    testWindowToPhysical();
    testLogicalWindowCenter();
    testLogicalMapViewHeight();
    if (gFailures != 0) {
        std::fprintf(stderr, "hidpi_test: %d failure(s)\n", gFailures);
        return EXIT_FAILURE;
    }
    std::printf("hidpi_test: all checks passed\n");
    return EXIT_SUCCESS;
}
