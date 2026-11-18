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
