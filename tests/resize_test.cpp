#include "render/window.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
struct WindowSizeOverridden {};

void checkSize(SDL_Window* window, ss2::VulkanWindow& renderer, int width, int height) {
    using namespace std::chrono_literals;
    if (!SDL_SetWindowSize(window, width, height) || !SDL_SyncWindow(window))
        throw std::runtime_error(SDL_GetError());
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    bool sizeOverridden = false;
    do {
        SDL_Event event;
        while (SDL_PollEvent(&event)) renderer.handleEvent(event);
        int w{}, h{}, pixelsX{}, pixelsY{};
        if (!SDL_GetWindowSize(window, &w, &h) || !SDL_GetWindowSizeInPixels(window, &pixelsX, &pixelsY))
            throw std::runtime_error(SDL_GetError());
        if (renderer.present({})) {
            bool drawableMatched = renderer.extent() == std::pair{unsigned(pixelsX), unsigned(pixelsY)};
            sizeOverridden = drawableMatched && (w != width || h != height);
            if (drawableMatched && !sizeOverridden) return;
        }
        SDL_Delay(5);
    } while (std::chrono::steady_clock::now() < deadline);
    if (sizeOverridden) throw WindowSizeOverridden{};
    throw std::runtime_error("Presentation did not reach the resized drawable extent");
}

void presentFrame(ss2::VulkanWindow& renderer) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    do {
        SDL_Event event;
        while (SDL_PollEvent(&event)) renderer.handleEvent(event);
        if (renderer.present({})) return;
        SDL_Delay(5);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Timed out waiting to present frame");
}
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    ss2::ValidationReport report;
    int result = 0;
    try {
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow("Swapchain resize test", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE),
            SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        checkSize(window.get(), renderer, 320, 240);
        renderer.setMap({96, 64, std::vector<std::uint8_t>(96 * 64 * 4, 128)});
        renderer.highlight({{20, 20, 8, 16}, {25, 23, 8, 16}});
        checkSize(window.get(), renderer, 640, 360);
        const auto [x, y] = renderer.mapPosition(320, 180);
        if (std::abs(x - 48) > 1 || std::abs(y - 32) > 1)
            throw std::runtime_error("Map picking missed the center after resize");
        renderer.setMap({1024, 1024, std::vector<std::uint8_t>(1024 * 1024 * 4, 64)});
        renderer.focusMap(-500, -500);
        checkSize(window.get(), renderer, 384, 256);
        const auto [edgeX, edgeY] = renderer.mapPosition(0, 0);
        if (std::abs(edgeX) > 1 || std::abs(edgeY) > 1)
            throw std::runtime_error("Map camera failed to clamp at the upper-left edge");
        // Wayland gives clients no portable minimized-state acknowledgement.
        if (std::string_view(SDL_GetCurrentVideoDriver()) == "x11" && SDL_MinimizeWindow(window.get())) {
            SDL_SyncWindow(window.get());
            if (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) {
                if (renderer.present({})) throw std::runtime_error("Counted a frame while minimized");
                if (!SDL_RestoreWindow(window.get())) throw std::runtime_error(SDL_GetError());
                checkSize(window.get(), renderer, 384, 256);
                std::cout << "Minimize and restore observed\n";
            } else std::cout << "Window manager did not acknowledge minimize\n";
        }
        constexpr unsigned panMapW = 1024, panMapH = 512;
        renderer.setMap({panMapW, panMapH, std::vector<std::uint8_t>(std::size_t(panMapW) * panMapH * 4, 96)});
        presentFrame(renderer);
        int winW{}, winH{};
        if (!SDL_GetWindowSize(window.get(), &winW, &winH)) throw std::runtime_error(SDL_GetError());
        const float centerWinX = float(winW) * 0.5f, centerWinY = float(winH) * 0.5f;
        const auto [beforeX, beforeY] = renderer.mapPosition(centerWinX, centerWinY);
        if (!std::isfinite(beforeX) || !std::isfinite(beforeY))
            throw std::runtime_error("Initial mapPosition is not finite");
        renderer.panMap(120.0, 60.0);
        presentFrame(renderer);
        const auto [pannedX, pannedY] = renderer.mapPosition(centerWinX, centerWinY);
        if (!std::isfinite(pannedX) || !std::isfinite(pannedY) ||
            pannedX <= beforeX || pannedY <= beforeY)
            throw std::runtime_error("panMap did not shift mapPosition positively in both X and Y");
        renderer.panMap(1e6, 1e6);
        presentFrame(renderer);
        const auto [clampedX, clampedY] = renderer.mapPosition(centerWinX, centerWinY);
        if (!std::isfinite(clampedX) || !std::isfinite(clampedY) ||
            clampedX < 0.0 || clampedX > double(panMapW) ||
            clampedY < 0.0 || clampedY > double(panMapH))
            throw std::runtime_error("panMap failed to clamp mapPosition within map bounds");
    } catch (const WindowSizeOverridden&) {
        std::cout << "Compositor overrode requested window size; matching actual drawable presented\n";
        result = 77;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    SDL_Quit();
    return report.errors.load() ? 1 : result;
}
