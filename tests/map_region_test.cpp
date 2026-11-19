// Compares the GPU map image with a CPU reference after queued region updates.
#include "render/window.hpp"
#include <SDL3/SDL.h>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using ss2::assets::MapImage;

MapImage pattern(unsigned width, unsigned height, unsigned seed) {
    MapImage image{width, height, std::vector<std::uint8_t>(std::size_t(width) * height * 4)};
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            auto* p = &image.rgba[(std::size_t(y) * width + x) * 4];
            p[0] = std::uint8_t(x + seed);
            p[1] = std::uint8_t(y * 3 + seed);
            p[2] = std::uint8_t(seed * 37 + x * y);
            p[3] = 255;
        }
    return image;
}

// The same clipping rule, written independently of the renderer.
void apply(MapImage& map, int x, int y, const MapImage& patch) {
    for (unsigned row = 0; row < patch.height; ++row)
        for (unsigned column = 0; column < patch.width; ++column) {
            const long long mx = static_cast<long long>(x) + column, my = static_cast<long long>(y) + row;
            if (mx < 0 || my < 0 || mx >= map.width || my >= map.height) continue;
            for (int c = 0; c < 4; ++c)
                map.rgba[(std::size_t(my) * map.width + std::size_t(mx)) * 4 + c] =
                    patch.rgba[(std::size_t(row) * patch.width + column) * 4 + c];
        }
}

void present(ss2::VulkanWindow& renderer) {
    for (int i = 0; i < 200; ++i) {
        SDL_PumpEvents();
        if (renderer.present({})) return;
        SDL_Delay(5);
    }
    throw std::runtime_error("Timed out waiting for presentation");
}

void same(ss2::VulkanWindow& renderer, const MapImage& expected, const char* what) {
    const auto actual = renderer.captureMap();
    if (actual.width != expected.width || actual.height != expected.height)
        throw std::runtime_error(std::string(what) + ": captured size differs");
    for (std::size_t i = 0; i < expected.rgba.size(); ++i)
        if (actual.rgba[i] != expected.rgba[i]) {
            const auto pixel = i / 4;
            throw std::runtime_error(std::string(what) + ": first difference at " +
                std::to_string(pixel % expected.width) + "," + std::to_string(pixel / expected.width));
        }
}

template<class F> void rejects(F&& call, const char* what) {
    try { call(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string(what) + " was accepted");
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
            SDL_CreateWindow("Map region test", 320, 240, SDL_WINDOW_VULKAN), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        rejects([&] { renderer.updateMapRegion(0, 0, pattern(2, 2, 1)); }, "update without a map");
        rejects([&] { (void)renderer.captureMap(); }, "capture without a map");

        auto reference = pattern(64, 48, 0);
        renderer.setMap(reference);
        same(renderer, reference, "initial upload");
        rejects([&] { renderer.updateMapRegion(0, 0, {0, 4, {}}); }, "empty patch");
        rejects([&] { renderer.updateMapRegion(0, 0, {2, 2, std::vector<std::uint8_t>(12)}); }, "short patch");

        const auto update = [&](int x, int y, const MapImage& patch) {
            renderer.updateMapRegion(x, y, patch);
            apply(reference, x, y, patch);
        };

        // One frame: interior, each clipped edge and corner, overlap in call order, single texel.
        update(10, 7, pattern(9, 5, 11));
        update(-3, -2, pattern(8, 6, 23));
        update(60, 44, pattern(9, 9, 31));
        update(-4, 20, pattern(6, 3, 41));
        update(30, -1, pattern(5, 4, 43));
        update(12, 8, pattern(9, 5, 53));  // Overlaps the first patch and must win.
        update(12, 8, pattern(3, 2, 59));  // Same origin again, smaller.
        update(63, 0, pattern(1, 1, 61));
        present(renderer);
        same(renderer, reference, "patches applied with a frame");

        // Entirely outside the map: accepted and ignored.
        for (auto [x, y] : {std::pair{64, 0}, {0, 48}, {-5, 0}, {0, -5}, {1 << 30, 1 << 30}, {-(1 << 30), 3}})
            renderer.updateMapRegion(x, y, pattern(5, 5, 67));
        renderer.updateMapRegion(2147483647, 2147483647, pattern(4, 4, 71));
        present(renderer);
        same(renderer, reference, "patches outside the map");

        // Capture applies the queue without a presented frame.
        update(1, 1, pattern(20, 20, 73));
        same(renderer, reference, "patches applied by capture");

        // Larger than the map, and exactly the map.
        update(-10, -10, pattern(100, 100, 79));
        present(renderer);
        same(renderer, reference, "oversized patch");
        update(0, 0, pattern(64, 48, 83));
        present(renderer);
        same(renderer, reference, "full-map patch");

        // A moving sprite: erase and redraw every frame for several frames.
        const auto background = pattern(7, 7, 89), sprite = pattern(7, 7, 97);
        for (int step = 0; step < 12; ++step) {
            if (step) update(step * 5 - 5, 20, background);
            update(step * 5, 20, sprite);
            present(renderer);
        }
        same(renderer, reference, "moving sprite");

        // Many updates with no frame in between exceed the queue budget and are applied early.
        for (int i = 0; i < 400; ++i) update(i % 40 - 4, i % 30 - 2, pattern(32, 32, unsigned(i)));
        present(renderer);
        same(renderer, reference, "queue overflow");

        // Replacing the map discards patches queued for the old one.
        renderer.updateMapRegion(0, 0, pattern(8, 8, 101));
        reference = pattern(40, 72, 103);
        renderer.setMap(reference);
        same(renderer, reference, "replacement map");
        update(35, 70, pattern(10, 10, 107));
        present(renderer);
        same(renderer, reference, "patch on the replacement map");
        present(renderer);
        same(renderer, reference, "idle frame leaves the map alone");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
