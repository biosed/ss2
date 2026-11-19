// Tooltip: content, placement near the pointer, staying on screen, and its place in a frame.
#include "render/window.hpp"
#include "ui/tooltip.hpp"
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    ss2::ValidationReport report;
    int status = 0;
    try {
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow("Tooltip test", 800, 600, SDL_WINDOW_VULKAN), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        int w{}, h{}, pixelW{}, pixelH{};
        SDL_GetWindowSize(window.get(), &w, &h);
        SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
        const float sx = float(w) / pixelW, sy = float(h) / pixelH;   // pixels -> window coordinates

        ss2::Tooltip tip(window.get());
        check(!tip.visible(), "starts hidden");
        tip.hide();

        // Content changes the picture; the same content does not redraw.
        tip.show({"Rifleman", {"Health 80 / 100", "Standing"}, 0.8f}, 100 * sx, 100 * sy);
        check(tip.visible() && tip.width() > 20 && tip.height() > 20, "shown with a size");
        check(tip.width() <= unsigned(pixelW) / 2 && tip.height() <= unsigned(pixelH) / 2, "stays small");
        for (std::size_t i = 3; i < tip.rgba().size(); i += 4) check(tip.rgba()[i] == 255, "opaque");
        auto shot = tip.rgba();
        const auto wide = tip.width(), tall = tip.height();
        tip.show({"Rifleman", {"Health 80 / 100", "Standing"}, 0.8f}, 300 * sx, 200 * sy);
        check(tip.rgba() == shot, "same content keeps the picture");
        tip.show({"Rifleman", {"Health 20 / 100", "Standing"}, 0.2f}, 300 * sx, 200 * sy);
        check(tip.rgba() != shot && tip.width() == wide && tip.height() == tall, "a new value redraws at the same size");
        tip.show({"Rifleman", {"Health 20 / 100", "Standing"}, -1}, 300 * sx, 200 * sy);
        check(tip.height() < tall, "no gauge makes it shorter");
        tip.show({"Rifleman", {}, -1}, 300 * sx, 200 * sy);
        const auto titleOnly = tip.height();
        tip.show({"Rifleman", {"One", "", "Two"}, -1}, 300 * sx, 200 * sy);
        check(tip.height() > titleOnly, "lines add height");
        tip.show({"", {}, -1}, 300 * sx, 200 * sy);
        check(tip.width() > 0 && tip.height() > 0, "empty content is still a valid box");
        tip.show({std::string(2000, 'W'), {"a\rb\nc"}, 0.5f}, 300 * sx, 200 * sy);
        check(tip.width() <= unsigned(pixelW) / 2, "a very long title is cut, not grown");
        tip.show({"NaN", {}, 0.5f}, std::numeric_limits<float>::quiet_NaN(), 10);
        check(tip.left() >= 0, "a non-finite position is ignored");

        // Placement: below-right of the pointer, never covering it, never off screen.
        const ss2::TooltipContent sample{"Machine gunner", {"Health 55 / 100", "Ammunition low"}, 0.55f};
        const auto covers = [&](int px, int py) {
            return px >= tip.left() && px < tip.left() + int(tip.width()) && py >= tip.top() && py < tip.top() + int(tip.height());
        };
        const auto inside = [&] {
            return tip.left() >= 0 && tip.top() >= 0 && tip.left() + int(tip.width()) <= pixelW && tip.top() + int(tip.height()) <= pixelH;
        };
        tip.show(sample, 200 * sx, 150 * sy);
        check(tip.left() > 200 && tip.top() > 150 && tip.left() < 260 && tip.top() < 220, "sits just below and right of the pointer");
        for (auto [px, py] : {std::pair{pixelW - 3, 150}, {200, pixelH - 3}, {pixelW - 3, pixelH - 3}, {2, 2}, {pixelW / 2, pixelH / 2}}) {
            tip.show(sample, px * sx, py * sy);
            check(inside(), "stays inside the window");
            check(!covers(px, py), "does not cover the pointer");
        }
        tip.show(sample, (pixelW - 3) * sx, 150 * sy);
        check(tip.left() + int(tip.width()) < pixelW - 3, "flips to the left at the right edge");
        tip.show(sample, 200 * sx, (pixelH - 3) * sy);
        check(tip.top() + int(tip.height()) < pixelH - 3, "flips above at the bottom edge");
        tip.show(sample, -500 * sx, -500 * sy);
        check(inside(), "a pointer outside the window still gives a visible box");

        // In a frame: drawn where it says it is, moved without a redraw, removed on hide.
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        renderer.setMap({64, 48, std::vector<std::uint8_t>(64 * 48 * 4, 200)});
        const auto matches = [&](const ss2::assets::MapImage& frame) {
            for (int y : {0, int(tip.height()) / 2, int(tip.height()) - 1})
                for (int x : {0, int(tip.width()) / 2, int(tip.width()) - 1}) {
                    const auto* got = &frame.rgba[(std::size_t(tip.top() + y) * frame.width + tip.left() + x) * 4];
                    const auto* want = &tip.rgba()[(std::size_t(y) * tip.width() + x) * 4];
                    if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2]) return false;
                }
            return true;
        };
        tip.show(sample, 200 * sx, 150 * sy);
        tip.draw(renderer);
        auto frame = renderer.captureFrame();
        check(matches(frame), "tooltip pixels reach the screen at its position");
        const int oldLeft = tip.left(), oldTop = tip.top();
        tip.show(sample, 500 * sx, 400 * sy);
        tip.draw(renderer);
        frame = renderer.captureFrame();
        check(matches(frame), "tooltip follows the pointer");
        const auto* vacated = &frame.rgba[(std::size_t(oldTop + 2) * frame.width + oldLeft + 2) * 4];
        check(vacated[0] != tip.rgba()[(2 * tip.width() + 2) * 4] || vacated[1] != tip.rgba()[(2 * tip.width() + 2) * 4 + 1], "old position is vacated");
        tip.hide();
        tip.draw(renderer);
        tip.draw(renderer);
        frame = renderer.captureFrame();
        const auto* gone = &frame.rgba[(std::size_t(tip.top() + 2) * frame.width + tip.left() + 2) * 4];
        check(gone[0] != tip.rgba()[(2 * tip.width() + 2) * 4] || gone[1] != tip.rgba()[(2 * tip.width() + 2) * 4 + 1], "hidden tooltip is removed");
        tip.show(sample, 100 * sx, 100 * sy);
        tip.draw(renderer);
        frame = renderer.captureFrame();
        check(matches(frame), "shown again after hiding");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
