// Checks the sprite pass by reading rendered frames back and comparing pixel colours.
#include "render/window.hpp"
#include "ui/combat_vfx.hpp"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using ss2::Sprite;
using ss2::assets::MapImage;

struct Rgb { int r, g, b; };

MapImage solid(unsigned width, unsigned height, Rgb c, int alpha = 255) {
    MapImage image{width, height, std::vector<std::uint8_t>(std::size_t(width) * height * 4)};
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = std::uint8_t(c.r);
        image.rgba[i + 1] = std::uint8_t(c.g);
        image.rgba[i + 2] = std::uint8_t(c.b);
        image.rgba[i + 3] = std::uint8_t(alpha);
    }
    return image;
}

// An 8x8 atlas of four 4x4 cells: opaque red, opaque green, half-transparent blue, fully transparent white.
MapImage quadrants() {
    auto image = solid(8, 8, {255, 255, 255}, 0);
    const auto fill = [&](unsigned x0, unsigned y0, Rgb c, int alpha) {
        for (unsigned y = y0; y < y0 + 4; ++y)
            for (unsigned x = x0; x < x0 + 4; ++x) {
                auto* p = &image.rgba[(std::size_t(y) * 8 + x) * 4];
                p[0] = std::uint8_t(c.r); p[1] = std::uint8_t(c.g); p[2] = std::uint8_t(c.b); p[3] = std::uint8_t(alpha);
            }
    };
    fill(0, 0, {255, 0, 0}, 255);
    fill(4, 0, {0, 255, 0}, 255);
    fill(0, 4, {0, 0, 255}, 128);
    return image;
}
// Source rectangles one texel inside each cell, so linear filtering never reaches a neighbour.
constexpr float redCell[4] = {1, 1, 3, 3}, greenCell[4] = {5, 1, 7, 3}, blueCell[4] = {1, 5, 3, 7}, clearCell[4] = {5, 5, 7, 7};

Sprite sprite(float x, float y, float w, float h, const float cell[4], unsigned atlas = 0, float brightness = 1, float alpha = 1) {
    return {x, y, w, h, cell[0], cell[1], cell[2], cell[3], atlas, brightness, alpha};
}

double decode(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double encode(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }

// What blending `source` over `under` at `alpha` should display. An sRGB surface blends in linear light.
Rgb blended(Rgb source, Rgb under, double alpha, bool srgb) {
    const auto mix = [&](int s, int u) {
        const double a = s / 255.0, b = u / 255.0;
        const double out = srgb ? encode(decode(a) * alpha + decode(b) * (1 - alpha)) : a * alpha + b * (1 - alpha);
        return int(std::lround(out * 255));
    };
    return {mix(source.r, under.r), mix(source.g, under.g), mix(source.b, under.b)};
}

struct Frame {
    MapImage image;
    Rgb at(int x, int y) const {
        const auto* p = &image.rgba.at((std::size_t(y) * image.width + std::size_t(x)) * 4);
        return {p[0], p[1], p[2]};
    }
};

void expect(const Frame& frame, int x, int y, Rgb want, int tolerance, const char* what) {
    const auto got = frame.at(x, y);
    if (std::abs(got.r - want.r) > tolerance || std::abs(got.g - want.g) > tolerance || std::abs(got.b - want.b) > tolerance)
        throw std::runtime_error(std::string(what) + ": at " + std::to_string(x) + "," + std::to_string(y) + " got " +
            std::to_string(got.r) + "," + std::to_string(got.g) + "," + std::to_string(got.b) + " want " +
            std::to_string(want.r) + "," + std::to_string(want.g) + "," + std::to_string(want.b));
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
            SDL_CreateWindow("Sprite pass test", 320, 240, SDL_WINDOW_VULKAN), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);

        // A 64x48 map at scale 1 sits centred in the 320x240 window: map (0,0) is screen (128,96).
        const Rgb ground{100, 100, 100}, red{255, 0, 0}, green{0, 255, 0}, blue{0, 0, 255};
        renderer.setMap(solid(64, 48, ground));
        const auto capture = [&] { return Frame{renderer.captureFrame()}; };
        auto frame = capture();
        if (frame.image.width != 320 || frame.image.height != 240) throw std::runtime_error("Captured frame has the wrong size");
        const bool srgb = renderer.surfaceIsSrgb();
        const Rgb outside = frame.at(4, 4);
        expect(frame, 128 + 32, 96 + 24, ground, 0, "map without sprites");
        if (outside.r == ground.r && outside.g == ground.g) throw std::runtime_error("Letterbox was not cleared");

        rejects([&] { renderer.setSprites({sprite(0, 0, 4, 4, redCell)}); }, "sprite with no atlas");
        rejects([&] { renderer.setSpriteAtlas(ss2::VulkanWindow::spriteAtlasSlots, quadrants()); }, "atlas slot out of range");
        rejects([&] { renderer.setSpriteAtlas(0, {4, 4, std::vector<std::uint8_t>(12)}); }, "short atlas");
        rejects([&] { renderer.setSpriteAtlas(0, {0, 4, {}}); }, "empty atlas");

        renderer.setSpriteAtlas(0, quadrants());
        rejects([&] { renderer.setSprites({sprite(0, 0, 4, 4, redCell, 3)}); }, "sprite naming an unset slot");
        rejects([&] { renderer.setSprites({sprite(std::numeric_limits<float>::quiet_NaN(), 0, 4, 4, redCell)}); }, "sprite with NaN");

        // Opaque, half-transparent and fully transparent sprites, each 8x8 map pixels.
        renderer.setSprites({sprite(4, 4, 8, 8, redCell), sprite(20, 4, 8, 8, greenCell),
                             sprite(36, 4, 8, 8, blueCell), sprite(52, 4, 8, 8, clearCell)});
        frame = capture();
        expect(frame, 128 + 8, 96 + 8, red, 1, "opaque red sprite");
        expect(frame, 128 + 24, 96 + 8, green, 1, "opaque green sprite");
        expect(frame, 128 + 40, 96 + 8, blended(blue, ground, 128 / 255.0, srgb), 3, "half-transparent sprite");
        expect(frame, 128 + 56, 96 + 8, ground, 1, "fully transparent sprite");
        expect(frame, 128 + 14, 96 + 8, ground, 1, "gap between sprites");
        // Edges are exact: the red sprite covers map 4..12, i.e. screen 132..139 and 100..107.
        expect(frame, 132, 104, red, 1, "left edge inside");
        expect(frame, 131, 104, ground, 1, "left edge outside");
        expect(frame, 139, 104, red, 1, "right edge inside");
        expect(frame, 140, 104, ground, 1, "right edge outside");
        expect(frame, 136, 100, red, 1, "top edge inside");
        expect(frame, 136, 99, ground, 1, "top edge outside");
        expect(frame, 136, 107, red, 1, "bottom edge inside");
        expect(frame, 136, 108, ground, 1, "bottom edge outside");
        expect(frame, 128 + 8, 96 + 30, ground, 0, "ground away from sprites");

        // List order is paint order.
        renderer.setSprites({sprite(4, 4, 16, 16, redCell), sprite(12, 12, 16, 16, greenCell)});
        frame = capture();
        expect(frame, 128 + 8, 96 + 8, red, 1, "first sprite where alone");
        expect(frame, 128 + 16, 96 + 16, green, 1, "later sprite on top in the overlap");
        expect(frame, 128 + 24, 96 + 24, green, 1, "second sprite where alone");

        // Brightness scales the displayed value; alpha scales opacity.
        renderer.setSprites({sprite(4, 4, 8, 8, redCell, 0, 0.5f, 1), sprite(20, 4, 8, 8, greenCell, 0, 1, 0.25f),
                             sprite(36, 4, 8, 8, redCell, 0, 1, 0), sprite(4, 30, 8, 8, redCell, 0, 0, 1)});
        frame = capture();
        expect(frame, 128 + 8, 96 + 8, {128, 0, 0}, 3, "half brightness");
        expect(frame, 128 + 24, 96 + 8, blended(green, ground, 0.25, srgb), 3, "quarter alpha");
        expect(frame, 128 + 40, 96 + 8, ground, 1, "zero alpha draws nothing");
        expect(frame, 128 + 8, 96 + 34, {0, 0, 0}, 1, "zero brightness is black");

        // Sprites are clipped to the map: one hanging off the left edge must not touch the letterbox.
        renderer.setSprites({sprite(-6, 20, 12, 8, redCell), sprite(58, 40, 20, 20, greenCell)});
        frame = capture();
        expect(frame, 128 + 2, 96 + 24, red, 1, "part of the sprite inside the map");
        expect(frame, 128 - 3, 96 + 24, outside, 0, "letterbox left of the map");
        expect(frame, 128 + 62, 96 + 46, green, 1, "corner sprite inside the map");
        expect(frame, 128 + 66, 96 + 44, outside, 0, "letterbox right of the map");
        expect(frame, 128 + 60, 96 + 50, outside, 0, "letterbox below the map");

        // A second atlas, and replacing an atlas with one of another size.
        renderer.setSpriteAtlas(5, solid(2, 2, {10, 200, 250}));
        const float whole2[4] = {0, 0, 2, 2};
        renderer.setSprites({sprite(4, 4, 8, 8, redCell), sprite(20, 4, 8, 8, whole2, 5), sprite(36, 4, 8, 8, greenCell)});
        frame = capture();
        expect(frame, 128 + 8, 96 + 8, red, 1, "atlas 0 before atlas 5");
        expect(frame, 128 + 24, 96 + 8, {10, 200, 250}, 1, "atlas 5");
        expect(frame, 128 + 40, 96 + 8, green, 1, "atlas 0 after atlas 5");
        renderer.setSpriteAtlas(5, solid(16, 4, {250, 240, 20}));
        frame = capture();
        expect(frame, 128 + 24, 96 + 8, {250, 240, 20}, 1, "replaced atlas");

        // Outlines and markers are drawn above sprites; panels above everything.
        renderer.setSprites({sprite(4, 4, 24, 24, redCell)});
        renderer.highlight({{10, 10, 6, 6}});
        renderer.setMarkers({{{20, 14, 4, 4}, ss2::MarkerKind::attack}});
        frame = capture();
        expect(frame, 128 + 13, 96 + 13, red, 1, "sprite inside the outline");
        expect(frame, 128 + 8, 96 + 13, {100, 255, 100}, 0, "selection outline over a sprite");
        expect(frame, 128 + 18, 96 + 16, {232, 72, 60}, 0, "attack marker over a sprite");
        renderer.setOverlay(40, 20, solid(40, 20, {7, 8, 9}).rgba);
        frame = capture();
        expect(frame, 160, 120, {7, 8, 9}, 0, "panel over sprites");
        renderer.clearOverlay();
        renderer.highlight({});
        renderer.setMarkers({});

        // Sprites follow the camera: after zooming in the same map point is still red.
        renderer.setSprites({sprite(24, 16, 16, 16, redCell)});
        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.y = 3;
        renderer.handleEvent(wheel);
        frame = capture();
        const auto [mapX, mapY] = renderer.mapPosition(160, 120);
        if (std::abs(mapX - 32) > 1.5 || std::abs(mapY - 24) > 1.5) throw std::runtime_error("Zoom did not keep the map centre");
        expect(frame, 160, 120, red, 1, "sprite under the zoomed camera");
        expect(frame, 160 + 30, 120, ground, 1, "ground beside the zoomed sprite");
        wheel.wheel.y = -3;
        renderer.handleEvent(wheel);

        // Many sprites, then none; a new map removes sprites but keeps atlases.
        std::vector<Sprite> many;
        for (int i = 0; i < 2000; ++i) many.push_back(sprite(float(i % 60), float((i / 60) % 44), 3, 3, i % 2 ? redCell : greenCell));
        renderer.setSprites(many);
        frame = capture();
        renderer.setSprites({});
        frame = capture();
        expect(frame, 128 + 8, 96 + 8, ground, 0, "no sprites left");
        renderer.setSprites({sprite(4, 4, 8, 8, redCell)});
        renderer.setMap(solid(64, 48, {60, 70, 80}));
        frame = capture();
        expect(frame, 128 + 8, 96 + 8, {60, 70, 80}, 0, "new map starts without sprites");
        renderer.setSprites({sprite(4, 4, 8, 8, greenCell)});
        frame = capture();
        expect(frame, 128 + 8, 96 + 8, green, 1, "atlas survives a map replacement");

        // No halo: transparent texels with bright colour must not tint a filtered edge.
        // Half the atlas is opaque red, half is white with zero alpha; the sprite is
        // magnified so the seam is spread over many screen pixels.
        {
            const Rgb dark{20, 20, 20};
            renderer.setMap(solid(64, 48, dark));
            auto seam = solid(8, 8, {255, 255, 255}, 0);
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 4; ++x) {
                    auto* p = &seam.rgba[(std::size_t(y) * 8 + x) * 4];
                    p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255;
                }
            renderer.setSpriteAtlas(6, seam);
            const float across[4] = {2, 2, 6, 6};
            renderer.setSprites({sprite(8, 8, 48, 32, across, 6)});
            frame = capture();
            expect(frame, 128 + 12, 96 + 24, {255, 0, 0}, 1, "opaque side of the seam");
            expect(frame, 128 + 52, 96 + 24, dark, 1, "transparent side of the seam");
            int partial = 0;
            for (int x = 128 + 8; x < 128 + 56; ++x) {
                const auto c = frame.at(x, 96 + 24);
                if (c.g > dark.g + 1 || c.b > dark.b + 1)
                    throw std::runtime_error("Halo: transparent texel colour leaked into the edge at x=" + std::to_string(x) +
                        " (" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + ")");
                if (c.r > dark.r + 8 && c.r < 247) ++partial;
            }
            if (partial < 2) throw std::runtime_error("Seam was not filtered, so the halo check proves nothing");
            renderer.setMap(solid(64, 48, {60, 70, 80}));
        }

        // Terrain filter: a two-colour map zoomed in shows only its two colours with nearest
        // scaling, and blended colours along the seam with linear scaling.
        {
            auto stripes = solid(64, 48, {0, 0, 0});
            for (unsigned y = 0; y < 48; ++y)
                for (unsigned x = 32; x < 64; ++x) {
                    auto* p = &stripes.rgba[(std::size_t(y) * 64 + x) * 4];
                    p[0] = p[1] = p[2] = 255;
                }
            renderer.setMap(stripes);
            wheel.wheel.y = 6;
            renderer.handleEvent(wheel);
            const auto inBetween = [&] {
                const auto shot = capture();
                int count = 0;
                // The seam is at the map centre, which is the screen centre; stay inside the map.
                for (int x = 130; x < 190; ++x) {
                    const auto c = shot.at(x, 120);
                    if (c.r > 8 && c.r < 247) ++count;
                }
                return count;
            };
            if (inBetween() != 0) throw std::runtime_error("Nearest filtering produced blended terrain pixels");
            renderer.setMapFilterLinear(true);
            if (inBetween() < 2) throw std::runtime_error("Linear filtering produced no blended terrain pixels");
            renderer.setMapFilterLinear(false);
            if (inBetween() != 0) throw std::runtime_error("Switching back to nearest left blended terrain pixels");
            wheel.wheel.y = -6;
            renderer.handleEvent(wheel);
        }

        // Gauges over the map: a dark track with a filled part whose colour follows the value.
        {
            renderer.setMap(solid(64, 48, ground));
            renderer.setSprites({});
            // Three gauges 20 map pixels wide, centred at x = 14, 34 and 52, tops at y = 20.
            renderer.setBars({{14, 20, 20, 1.0f}, {34, 20, 20, 0.5f}, {52, 20, 20, 0.1f}});
            frame = capture();
            const int y = 96 + 20 - 2;   // inside the 3-pixel gauge that ends at the top edge
            expect(frame, 128 + 6, y, {100, 255, 100}, 0, "full gauge is green at its left end");
            expect(frame, 128 + 22, y, {100, 255, 100}, 0, "full gauge is green at its right end");
            expect(frame, 128 + 26, y, {240, 210, 90}, 0, "half gauge is amber in its filled half");
            expect(frame, 128 + 41, y, {24, 26, 24}, 0, "half gauge shows the dark track in its empty half");
            expect(frame, 128 + 43, y, {232, 72, 60}, 0, "low gauge is red in its filled part");
            expect(frame, 128 + 58, y, {24, 26, 24}, 0, "low gauge is mostly track");
            expect(frame, 128 + 30, 96 + 30, ground, 0, "ground below the gauges");
            // Clipped at the map edge, cleared by an empty list, rejected when not finite.
            renderer.setBars({{2, 10, 30, 1.0f}});
            frame = capture();
            expect(frame, 128 + 4, 96 + 8, {100, 255, 100}, 0, "gauge part inside the map");
            expect(frame, 128 - 5, 96 + 8, outside, 0, "gauge does not spill into the letterbox");
            renderer.setBars({});
            frame = capture();
            expect(frame, 128 + 4, 96 + 8, ground, 0, "gauges removed");
            rejects([&] { renderer.setBars({{std::numeric_limits<float>::infinity(), 0, 4, 1}}); }, "gauge with infinity");
        }

        // With a command bar the map uses the window above it: a 64x48 map at scale 1 is
        // centred in the top 200 rows when the bar is 40 rows tall.
        {
            renderer.setOverlay(320, 40, solid(320, 40, {9, 9, 9}).rgba, ss2::OverlaySlot::hud);
            frame = capture();
            expect(frame, 160, 76, ground, 0, "map top edge moved up for the bar");
            expect(frame, 160, 75, outside, 0, "letterbox above the map");
            expect(frame, 160, 123, ground, 0, "map bottom edge");
            expect(frame, 160, 124, outside, 0, "letterbox between map and bar");
            expect(frame, 160, 210, {9, 9, 9}, 0, "bar at the bottom");
            const auto [u, v] = renderer.mapPosition(160, 100);
            if (std::abs(u - 32) > 1 || std::abs(v - 24) > 1) throw std::runtime_error("Clicks do not follow the moved map");
            renderer.clearOverlay(ss2::OverlaySlot::hud);
            frame = capture();
            expect(frame, 160, 96, ground, 0, "map returns to the full window");
        }

        // Beams and flashes: soft-edged streaks of light in map space, above every sprite.
        {
            using ss2::MapBeam;
            const Rgb white{255, 255, 255};
            const auto distance = [&](int x, int y, Rgb c) {
                const Rgb p = frame.at(128 + x, 96 + y);
                return std::abs(p.r - c.r) + std::abs(p.g - c.g) + std::abs(p.b - c.b);
            };
            renderer.setMap(solid(64, 48, ground));
            // A red beam 8 wide along y = 24 and a green diagonal 6 wide, in one list.
            renderer.setBeams({{10, 24, 50, 24, 8, 255, 0, 0}, {6, 30, 20, 44, 6, 0, 255, 0}});
            frame = capture();
            expect(frame, 128 + 30, 96 + 23, red, 2, "beam core is the plain colour");
            expect(frame, 128 + 30, 96 + 24, red, 2, "beam core, other side of the axis");
            expect(frame, 128 + 12, 96 + 24, red, 2, "beam core near its start");
            expect(frame, 128 + 30, 96 + 17, ground, 0, "ground above the beam");
            expect(frame, 128 + 30, 96 + 30, ground, 0, "ground below the beam");
            expect(frame, 128 + 8, 96 + 24, ground, 0, "ground before the start");
            expect(frame, 128 + 52, 96 + 24, ground, 0, "ground past the end");
            if (distance(30, 20, red) < 40 || distance(30, 20, ground) < 2) throw std::runtime_error("Beam edge is not a soft blend");
            if (distance(30, 27, red) < 40 || distance(30, 27, ground) < 2) throw std::runtime_error("Beam edge is not symmetric");
            expect(frame, 128 + 13, 96 + 37, green, 2, "diagonal beam core, second colour");
            expect(frame, 128 + 20, 96 + 32, ground, 0, "ground beside the diagonal beam");
            expect(frame, 128 + 6, 96 + 42, ground, 0, "ground on its other side");

            // Opacity runs from one end to the other.
            renderer.setBeams({{10, 24, 50, 24, 8, 255, 0, 0, 0.f, 1.f}});
            frame = capture();
            const int tail = distance(13, 24, ground), middle = distance(30, 24, ground), head = distance(47, 24, ground);
            if (!(tail < middle && middle < head) || distance(47, 24, red) > 60 || tail > 60)
                throw std::runtime_error("Beam does not fade from tail to head");

            // A point is a round flash: full colour in the middle, nothing in the corners.
            renderer.setBeams({{32, 24, 32, 24, 12, 255, 255, 255}});
            frame = capture();
            expect(frame, 128 + 31, 96 + 23, white, 2, "flash centre");
            expect(frame, 128 + 32, 96 + 24, white, 2, "flash centre, other side");
            expect(frame, 128 + 26, 96 + 18, ground, 1, "flash has clear corners");
            expect(frame, 128 + 37, 96 + 29, ground, 1, "flash opposite corner");
            expect(frame, 128 + 40, 96 + 24, ground, 0, "ground beside the flash");
            if (distance(27, 24, white) < 40 || distance(27, 24, ground) < 2) throw std::runtime_error("Flash rim is not a soft blend");

            // Above sprites, clipped to the map, and half opacity blends with what is under it.
            renderer.setSprites({sprite(20, 16, 24, 16, greenCell)});
            renderer.setBeams({{-20, 24, 34, 24, 8, 255, 0, 0}, {50, 10, 50, 10, 10, 255, 255, 255, 0.5f, 0.5f}});
            frame = capture();
            expect(frame, 128 + 28, 96 + 30, green, 0, "sprite where no beam crosses");
            expect(frame, 128 + 28, 96 + 24, red, 2, "beam is drawn over the sprite");
            expect(frame, 128 + 3, 96 + 24, red, 2, "beam part inside the map");
            expect(frame, 128 - 4, 96 + 24, outside, 0, "beam does not spill into the letterbox");
            expect(frame, 128 + 50, 96 + 10, blended(white, ground, 0.5, renderer.surfaceIsSrgb()), 3, "half-opaque flash blends with the ground");
            renderer.setSprites({});

            // Many colours over time are fine; too many at once, or bad numbers, are refused.
            for (int round = 0; round < 3; ++round) {
                std::vector<MapBeam> list;
                for (unsigned i = 0; i < ss2::VulkanWindow::maxBeamColours; ++i)
                    list.push_back({4.f + i * 3, 4, 4.f + i * 3, 8, 2, std::uint8_t(round * 80 + i), std::uint8_t(i * 15), 200});
                renderer.setBeams(list);
                frame = capture();
            }
            renderer.setBeams({{32, 4, 32, 44, 6, 0, 255, 0}});
            frame = capture();
            expect(frame, 128 + 32, 96 + 24, green, 2, "a vertical beam after the palette was replaced");
            {
                std::vector<MapBeam> tooMany;
                for (unsigned i = 0; i <= ss2::VulkanWindow::maxBeamColours; ++i) tooMany.push_back({1, 1, 5, 5, 2, std::uint8_t(i), 0, 0});
                rejects([&] { renderer.setBeams(tooMany); }, "more colours than one list may hold");
            }
            rejects([&] { renderer.setBeams({{0, 0, std::numeric_limits<float>::quiet_NaN(), 0, 2}}); }, "beam with NaN");
            frame = capture();
            expect(frame, 128 + 32, 96 + 24, green, 2, "a refused list leaves the old one in place");
            // Zero width and zero opacity draw nothing; an empty list and a new map clear.
            renderer.setBeams({{10, 24, 50, 24, 0, 255, 0, 0}, {10, 30, 50, 30, 8, 255, 0, 0, 0.f, 0.f}});
            frame = capture();
            expect(frame, 128 + 30, 96 + 24, ground, 0, "zero-width beam draws nothing");
            expect(frame, 128 + 30, 96 + 30, ground, 0, "fully transparent beam draws nothing");
            renderer.setBeams({{10, 24, 50, 24, 8, 255, 0, 0}});
            renderer.setMap(solid(64, 48, ground));
            frame = capture();
            expect(frame, 128 + 30, 96 + 24, ground, 0, "a new map removes beams");
            renderer.setBeams({{10, 24, 50, 24, 8, 255, 0, 0}});
            renderer.setBeams({});
            frame = capture();
            expect(frame, 128 + 30, 96 + 24, ground, 0, "an empty list removes beams");
        }

        // Nearly transparent texels under fog dimming stay nearly invisible: recovering the
        // colour from a premultiplied texel must not blow a rounding step up into a speckle.
        for (int alpha : {1, 2, 3, 5, 8, 16}) {
            renderer.setSpriteAtlas(7, solid(4, 4, {255, 255, 255}, alpha));
            const float whole[4] = {0, 0, 4, 4};
            for (float brightness : {0.9f, 0.5f, 0.2f}) {
                renderer.setSprites({sprite(20, 16, 24, 16, whole, 7, brightness)});
                frame = capture();
                const Rgb want = blended({int(std::lround(255 * brightness)), int(std::lround(255 * brightness)), int(std::lround(255 * brightness))},
                                         ground, alpha / 255.0, renderer.surfaceIsSrgb());
                expect(frame, 128 + 30, 96 + 24, want, 3, "faint texel under dimming");
            }
            renderer.setSprites({});
        }

        // A tooltip overlay goes where it is put, stays inside the window, and sits above the bar.
        {
            const Rgb tip{200, 180, 40};
            renderer.setOverlay(20, 10, solid(20, 10, tip).rgba, ss2::OverlaySlot::tooltip);
            renderer.moveOverlay(ss2::OverlaySlot::tooltip, 30, 40);
            frame = capture();
            expect(frame, 30, 40, tip, 0, "tooltip top-left corner");
            expect(frame, 49, 49, tip, 0, "tooltip bottom-right corner");
            expect(frame, 29, 40, outside, 0, "left of the tooltip");
            expect(frame, 50, 49, outside, 0, "right of the tooltip");
            renderer.moveOverlay(ss2::OverlaySlot::tooltip, 5000, -70);
            frame = capture();
            expect(frame, 319, 0, tip, 0, "tooltip is kept inside the window");
            expect(frame, 300, 9, tip, 0, "tooltip far corner after clamping");
            renderer.setOverlay(320, 40, solid(320, 40, {9, 9, 9}).rgba, ss2::OverlaySlot::hud);
            renderer.moveOverlay(ss2::OverlaySlot::tooltip, 100, 225);
            frame = capture();
            expect(frame, 105, 230, tip, 0, "tooltip is drawn over the bar");
            renderer.setOverlay(60, 60, solid(60, 60, {1, 2, 3}).rgba);
            renderer.moveOverlay(ss2::OverlaySlot::tooltip, 150, 110);
            frame = capture();
            expect(frame, 155, 115, {1, 2, 3}, 0, "a panel covers the tooltip");
            renderer.clearOverlay();
            renderer.clearOverlay(ss2::OverlaySlot::hud);
            renderer.clearOverlay(ss2::OverlaySlot::tooltip);
            frame = capture();
            expect(frame, 155, 115, ground, 0, "tooltip removed");
        }

        // CombatVfxSystem: asset-free fallback, synthetic 0xCA000002 .ANI packing,
        // directional muzzle flashes, parachute/crate emission, decal expiry, fog culling,
        // and optional retail Gold 2.2 game_common.sue verification.
        {
            ss2::CombatVfxSystem vfx;
            if (!vfx.hasAtlas() || vfx.hasRetailAssets()) {
                throw std::runtime_error("Default CombatVfxSystem should start with procedural fallback atlas");
            }
            if (vfx.bank().clipFrameCount(ss2::CombatVfxClip::ExplosionLarge) == 0u ||
                vfx.bank().clipFrameCount(ss2::CombatVfxClip::ParachuteCanopy) < 9u ||
                vfx.bank().clipFrameCount(ss2::CombatVfxClip::AmmoCrate) == 0u) {
                throw std::runtime_error("Procedural CombatVfxBank missing core fallback frames");
            }

            // Build a synthetic 2-frame 0xCA000002 + 0xA9 .ANI with 1024-byte alpha palette:
            // Frame 0: 6x6 bright orange-red square at (-3,-3); Frame 1: 6x6 bright cyan square at (-3,-3).
            std::vector<std::uint8_t> synCol(1024u, 0u);
            synCol[2] = 0xFEu; // index 0 transparent (bgWeight5 == 31)
            // Index 1: opaque bright red (RGB565 = 0xF800, bgWeight = 0)
            synCol[4] = 0x00u; synCol[5] = 0xF8u; synCol[6] = 0x00u; synCol[7] = 0x00u;
            // Index 2: opaque bright green (RGB565 = 0x07E0, bgWeight = 0)
            synCol[8] = 0xE0u; synCol[9] = 0x07u; synCol[10] = 0x00u; synCol[11] = 0x00u;

            const auto makeA9Frame = [](std::uint8_t palIdx) {
                std::vector<std::uint8_t> fr;
                const auto push16 = [&](int v) {
                    const auto u = static_cast<std::uint16_t>(v);
                    fr.push_back(static_cast<std::uint8_t>(u & 0xFFu));
                    fr.push_back(static_cast<std::uint8_t>((u >> 8u) & 0xFFu));
                };
                push16(-3); push16(-3); push16(6); push16(6);
                fr.push_back(0xA9u);
                for (int row = 0; row < 6; ++row) {
                    push16(2); // 2-byte repeat run: (128 | 6), palIdx
                    fr.push_back(static_cast<std::uint8_t>(0x80u | 6u));
                    fr.push_back(palIdx);
                }
                return fr;
            };
            const auto fr0 = makeA9Frame(1u);
            const auto fr1 = makeA9Frame(2u);
            std::vector<std::uint8_t> synAni;
            const auto push32 = [&](std::uint32_t v) {
                synAni.push_back(static_cast<std::uint8_t>(v & 0xFFu));
                synAni.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
                synAni.push_back(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
                synAni.push_back(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
            };
            push32(0xCA000002u);
            push32(2u);
            const std::uint32_t off0 = 8u + 3u * 4u;
            const std::uint32_t off1 = off0 + static_cast<std::uint32_t>(fr0.size());
            const std::uint32_t off2 = off1 + static_cast<std::uint32_t>(fr1.size());
            push32(off0); push32(off1); push32(off2);
            synAni.insert(synAni.end(), fr0.begin(), fr0.end());
            synAni.insert(synAni.end(), fr1.begin(), fr1.end());

            if (!vfx.packSyntheticAni(ss2::CombatVfxClip::Custom, synAni, synCol, 1u) ||
                vfx.bank().clipFrameCount(ss2::CombatVfxClip::Custom) != 2u) {
                throw std::runtime_error("Failed to pack synthetic 0xCA000002 .ANI into CombatVfxSystem");
            }
            renderer.setMap(solid(64, 48, ground));
            renderer.setSpriteAtlas(14u, vfx.atlas());
            vfx.bindAtlasSlot(14u);
            vfx.spawnClip(ss2::CombatVfxClip::Custom, 32.0f, 24.0f, 0u, 1.0f, 2.0f, 1.0f);

            // Unexplored fog (< 0.55) culls hidden VFX; visible fog (1.0) emits frame 0 (red), then frame 1 (green).
            std::vector<ss2::CombatVfxSystem::RankedSprite> ranked;
            vfx.appendSprites(1, [](int, int) { return 0.44f; }, ranked);
            if (!ranked.empty()) throw std::runtime_error("CombatVfxSystem leaked sprite through unexplored fog");

            vfx.appendSprites(1, [](int, int) { return 1.0f; }, ranked);
            if (ranked.size() != 1u) throw std::runtime_error("Expected 1 visible synthetic VFX sprite");
            renderer.setSprites({ranked[0].sprite});
            frame = capture();
            expect(frame, 128 + 32, 96 + 24, red, 3, "synthetic ANI frame 0 red");

            (void)vfx.step(0.60f);
            ranked.clear();
            vfx.appendSprites(1, [](int, int) { return 1.0f; }, ranked);
            if (ranked.size() != 1u) throw std::runtime_error("Expected synthetic VFX frame 1 still active");
            renderer.setSprites({ranked[0].sprite});
            frame = capture();
            expect(frame, 128 + 32, 96 + 24, green, 8, "synthetic ANI frame 1 green");

            (void)vfx.step(0.50f);
            if (vfx.activeAnimCount() != 0u) throw std::runtime_error("Expired CombatVfx animation was not removed");

            // Parachute canopy + crate immediate-mode helpers and vehicle decal stepping/expiry.
            std::vector<Sprite> imm;
            vfx.appendParachuteSprites(24.0f, 36.0f, 12.0f, 0.5f, 1, 1.0f, imm);
            vfx.appendCrateSprite(24.0f, 24.0f, 0u, false, 1, 1.0f, 1.0f, imm);
            if (imm.size() < 3u) throw std::runtime_error("Expected shadow + canopy + crate immediate sprites");
            renderer.setSprites(imm);
            frame = capture();

            vfx.recordVehicleMotion(0u, 10, 20, true, true, false, false, true, false, false);
            vfx.recordVehicleMotion(0u, 30, 20, true, true, false, false, true, false, false);
            if (vfx.activeDecalCount() != 0u) {
                throw std::runtime_error("Hidden enemy vehicle motion leaked track decal");
            }
            vfx.recordVehicleMotion(1u, 10, 20, true, true, true, false, true, false, false);
            vfx.recordVehicleMotion(1u, 30, 20, true, true, true, false, true, false, false);
            if (vfx.activeDecalCount() != 1u) {
                throw std::runtime_error("Visible moving tank did not emit tread decal");
            }
            (void)vfx.step(6.0f);
            if (vfx.activeDecalCount() != 0u) {
                throw std::runtime_error("Vehicle tread decal did not expire");
            }

            // Asset-free coverage of combat_vfx.hpp RFC-1950/1951 inflateZlibExact (btype 0/1/2 +
            // malformed streams) and SueArchiveReader (kind=2 + multi-chunk kind=8) without SS2_PRIVATE_GAME.
            {
                using namespace ss2::combat_vfx_detail;
                const auto makeStoredZlib = [](const std::vector<std::uint8_t>& raw) {
                    if (raw.size() > 65535u) throw std::runtime_error("stored block > 64K");
                    std::vector<std::uint8_t> z{0x78u, 0x01u, 0x01u};
                    const auto len = static_cast<std::uint16_t>(raw.size());
                    const auto nlen = static_cast<std::uint16_t>(len ^ 0xFFFFu);
                    z.push_back(static_cast<std::uint8_t>(len & 0xFFu));
                    z.push_back(static_cast<std::uint8_t>((len >> 8u) & 0xFFu));
                    z.push_back(static_cast<std::uint8_t>(nlen & 0xFFu));
                    z.push_back(static_cast<std::uint8_t>((nlen >> 8u) & 0xFFu));
                    z.insert(z.end(), raw.begin(), raw.end());
                    std::uint32_t s1 = 1u, s2 = 0u;
                    for (std::uint8_t b : raw) {
                        s1 = (s1 + b) % 65521u;
                        s2 = (s2 + s1) % 65521u;
                    }
                    const std::uint32_t adler = (s2 << 16u) | s1;
                    z.push_back(static_cast<std::uint8_t>((adler >> 24u) & 0xFFu));
                    z.push_back(static_cast<std::uint8_t>((adler >> 16u) & 0xFFu));
                    z.push_back(static_cast<std::uint8_t>((adler >> 8u) & 0xFFu));
                    z.push_back(static_cast<std::uint8_t>(adler & 0xFFu));
                    return z;
                };

                // btype=0 stored block
                const std::vector<std::uint8_t> raw0{'S', 'S', '2', '_', 'Z', 'L', 'I', 'B', 0, 1, 2, 3};
                const auto z0 = makeStoredZlib(raw0);
                std::vector<std::uint8_t> out;
                if (!inflateZlibExact(z0, raw0.size(), out) || out != raw0) {
                    throw std::runtime_error("inflateZlibExact failed on btype=0 stored block");
                }

                // btype=1 fixed Huffman + LZ77 back-reference ("ABCD" * 8)
                static constexpr std::array<std::uint8_t, 14> kZ1{
                    0x78, 0x01, 0x73, 0x74, 0x72, 0x76, 0x71, 0xc4, 0x83, 0x01, 0x89, 0x20, 0x08, 0x51
                };
                if (!inflateZlibExact(kZ1, 32u, out) || out.size() != 32u ||
                    std::memcmp(out.data(), "ABCDABCDABCDABCD", 16u) != 0) {
                    throw std::runtime_error("inflateZlibExact failed on btype=1 fixed Huffman stream");
                }

                // btype=2 dynamic Huffman + LZ77 back-reference (656 bytes)
                static constexpr std::array<std::uint8_t, 208> kZ2{
                    0x78, 0x9c, 0xcd, 0x91, 0x81, 0x8d, 0x40, 0x31, 0x08, 0x42, 0x57, 0x60, 0x84, 0x37, 0x02, 0xfb,
                    0x6f, 0x85, 0x2e, 0x70, 0xc1, 0xfe, 0xbb, 0x19, 0xae, 0x69, 0x6c, 0xa3, 0x15, 0x28, 0x22, 0xc9,
                    0x8e, 0xd0, 0x6d, 0x44, 0x14, 0x4b, 0x23, 0x29, 0xc4, 0xac, 0x08, 0xc1, 0x7a, 0x8b, 0x86, 0x01,
                    0x59, 0x5a, 0x70, 0x5e, 0x61, 0x22, 0x6b, 0x0e, 0x84, 0xb4, 0xd2, 0xa7, 0xa6, 0xb7, 0x8d, 0x6c,
                    0x5a, 0x60, 0xca, 0xd3, 0x80, 0x7c, 0x55, 0x4d, 0x22, 0x65, 0x84, 0xec, 0xa6, 0x83, 0x78, 0xc8,
                    0x7c, 0x64, 0xd7, 0x50, 0xb6, 0x29, 0x5b, 0x25, 0x62, 0xaf, 0xd0, 0xdd, 0x9b, 0x0e, 0xda, 0xec,
                    0x61, 0x97, 0x50, 0x86, 0xd9, 0x0f, 0xa5, 0xa0, 0xd1, 0xea, 0x50, 0x4b, 0x0c, 0x4c, 0xd1, 0x6e,
                    0xb3, 0xfb, 0xa7, 0xc1, 0xac, 0x15, 0x79, 0x8e, 0xb3, 0x8d, 0x7a, 0x7a, 0xe5, 0xaa, 0xa5, 0x32,
                    0x4b, 0xd5, 0xa7, 0x6d, 0x90, 0x43, 0xb5, 0x6d, 0xee, 0xeb, 0xb9, 0x23, 0xe9, 0x0f, 0xce, 0x92,
                    0x54, 0xe3, 0x32, 0xda, 0x8f, 0xbe, 0xde, 0x9e, 0x49, 0xf6, 0xe3, 0xaf, 0x75, 0xc5, 0x65, 0x3c,
                    0x3c, 0xf7, 0xcb, 0xc3, 0xbc, 0x7c, 0x7c, 0x38, 0x7c, 0x76, 0xc9, 0x67, 0xd1, 0x99, 0xdc, 0xc1,
                    0x2c, 0x13, 0xcc, 0xcd, 0xe8, 0xbc, 0xe6, 0x77, 0x3e, 0xe7, 0x64, 0x71, 0xbe, 0xe9, 0x38, 0xf3,
                    0xca, 0xef, 0x2f, 0x8f, 0xea, 0x1d, 0x1f, 0xfe, 0xfc, 0x9f, 0xf8, 0x03, 0x3e, 0x23, 0xc5, 0x1d
                };
                if (!inflateZlibExact(kZ2, 656u, out) || out.size() != 656u) {
                    throw std::runtime_error("inflateZlibExact failed on btype=2 dynamic Huffman stream");
                }
                static constexpr std::array<std::uint8_t, 6> kTailPat{0x10, 0x20, 0xF1, 0x10, 0x20, 0xF2};
                for (std::size_t off = 512u; off < 656u; ++off) {
                    if (out[off] != kTailPat[(off - 512u) % 6u]) {
                        throw std::runtime_error("inflateZlibExact btype=2 decoded pattern mismatch");
                    }
                }

                // Malformed streams: bad header FCHECK, wrong expectedSize, truncated stream.
                auto badHdr = z0;
                badHdr[1] ^= 0x01u;
                if (inflateZlibExact(badHdr, raw0.size(), out) ||
                    inflateZlibExact(z0, raw0.size() + 1u, out) ||
                    inflateZlibExact(std::span<const std::uint8_t>(z0.data(), z0.size() - 5u), raw0.size(), out)) {
                    throw std::runtime_error("inflateZlibExact accepted malformed zlib input");
                }

                // Synthetic game_common.sue with kind=8 multi-chunk entries for 8 VFX .ani/.col stems.
                struct SynEntry {
                    std::string name;
                    std::vector<std::uint8_t> data;
                };
                std::vector<SynEntry> synEntries;
                for (const char* stem : {"a_v50", "a_v36", "a_v30", "a_v24", "a_v16", "BOOMBOX1", "BOOMBOX2", "BOOMBOX3"}) {
                    synEntries.push_back({std::string(stem) + ".ani", synAni});
                    synEntries.push_back({std::string(stem) + ".col", synCol});
                }

                const auto tmpDir = std::filesystem::temp_directory_path() / "ss2_sprite_pass_syn_vfx";
                std::filesystem::remove_all(tmpDir);
                std::filesystem::create_directories(tmpDir);
                const auto push32V = [](std::vector<std::uint8_t>& dst, std::uint32_t v) {
                    dst.push_back(static_cast<std::uint8_t>(v & 0xFFu));
                    dst.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
                    dst.push_back(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
                    dst.push_back(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
                };
                std::vector<std::uint8_t> dataSec, items, names;
                std::vector<std::uint32_t> offsets;
                constexpr std::uint32_t kSynChunk = 256u;
                for (const auto& e : synEntries) {
                    const auto nameOff = static_cast<std::uint32_t>(names.size());
                    names.insert(names.end(), e.name.begin(), e.name.end());
                    names.push_back(0u);
                    const auto firstIdx = static_cast<std::uint32_t>(offsets.size());
                    offsets.push_back(static_cast<std::uint32_t>(8u + dataSec.size()));
                    for (std::size_t off = 0u; off < e.data.size(); off += kSynChunk) {
                        const std::size_t n = std::min<std::size_t>(kSynChunk, e.data.size() - off);
                        const auto comp = makeStoredZlib(
                            std::vector<std::uint8_t>(e.data.begin() + static_cast<std::ptrdiff_t>(off),
                                                       e.data.begin() + static_cast<std::ptrdiff_t>(off + n)));
                        dataSec.insert(dataSec.end(), comp.begin(), comp.end());
                        offsets.push_back(static_cast<std::uint32_t>(8u + dataSec.size()));
                    }
                    push32V(items, nameOff);
                    push32V(items, 8u);
                    push32V(items, firstIdx);
                    push32V(items, static_cast<std::uint32_t>(e.data.size()));
                    push32V(items, kSynChunk);
                }
                std::vector<std::uint8_t> offBytes;
                for (std::uint32_t o : offsets) push32V(offBytes, o);
                const auto appendTbl = [&](std::vector<std::uint8_t>& arc, const std::vector<std::uint8_t>& tbl,
                                           std::uint32_t cnt, std::uint32_t width) {
                    const auto comp = makeStoredZlib(tbl);
                    push32V(arc, static_cast<std::uint32_t>(comp.size()));
                    push32V(arc, cnt);
                    push32V(arc, width);
                    arc.insert(arc.end(), comp.begin(), comp.end());
                };
                std::vector<std::uint8_t> archive{'F', 'Z', 'F', 'S'};
                push32V(archive, static_cast<std::uint32_t>(dataSec.size()));
                archive.insert(archive.end(), dataSec.begin(), dataSec.end());
                appendTbl(archive, items, static_cast<std::uint32_t>(synEntries.size()), 20u);
                appendTbl(archive, names, static_cast<std::uint32_t>(names.size()), 1u);
                appendTbl(archive, offBytes, static_cast<std::uint32_t>(offsets.size()), 4u);

                std::ofstream outSue(tmpDir / "game_common.sue", std::ios::binary | std::ios::trunc);
                outSue.write(reinterpret_cast<const char*>(archive.data()),
                             static_cast<std::streamsize>(archive.size()));
                outSue.close();

                ss2::CombatVfxSystem synVfx;
                if (!synVfx.load(tmpDir) || !synVfx.hasRetailAssets() ||
                    synVfx.bank().clipFrameCount(ss2::CombatVfxClip::ExplosionHuge) != 1u ||
                    synVfx.bank().clipFrameCount(ss2::CombatVfxClip::CrateBlast3) != 1u) {
                    std::filesystem::remove_all(tmpDir);
                    throw std::runtime_error("Synthetic game_common.sue CombatVfxSystem load failed");
                }

                // Extend with sudtest.ini [Data] Game=myvfx\;game_common.sue unpacked folder + RFC-1952 gzip loose .ani.
                {
                    const auto myVfxDir = tmpDir / "myvfx";
                    std::filesystem::create_directories(myVfxDir);
                    std::ofstream outCol(myVfxDir / "a_sledkater.col", std::ios::binary | std::ios::trunc);
                    outCol.write(reinterpret_cast<const char*>(synCol.data()),
                                 static_cast<std::streamsize>(synCol.size()));
                    outCol.close();

                    const auto makeStoredGzip = [](const std::vector<std::uint8_t>& raw) {
                        std::vector<std::uint8_t> gz{0x1Fu, 0x8Bu, 0x08u, 0x00u, 0u, 0u, 0u, 0u, 0x00u, 0x03u};
                        gz.push_back(0x01u);
                        const auto len = static_cast<std::uint16_t>(raw.size());
                        const auto nlen = static_cast<std::uint16_t>(len ^ 0xFFFFu);
                        gz.push_back(static_cast<std::uint8_t>(len & 0xFFu));
                        gz.push_back(static_cast<std::uint8_t>((len >> 8u) & 0xFFu));
                        gz.push_back(static_cast<std::uint8_t>(nlen & 0xFFu));
                        gz.push_back(static_cast<std::uint8_t>((nlen >> 8u) & 0xFFu));
                        gz.insert(gz.end(), raw.begin(), raw.end());
                        std::uint32_t crc = 0xFFFFFFFFu;
                        for (std::uint8_t b : raw) {
                            crc ^= b;
                            for (int bit = 0; bit < 8; ++bit) {
                                crc = (crc & 1u) ? ((crc >> 1u) ^ 0xEDB88320u) : (crc >> 1u);
                            }
                        }
                        crc ^= 0xFFFFFFFFu;
                        const auto push32Le = [&](std::uint32_t v) {
                            gz.push_back(static_cast<std::uint8_t>(v & 0xFFu));
                            gz.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
                            gz.push_back(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
                            gz.push_back(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
                        };
                        push32Le(crc);
                        push32Le(static_cast<std::uint32_t>(raw.size()));
                        return gz;
                    };

                    const auto gzAni = makeStoredGzip(synAni);
                    std::ofstream outAni(myVfxDir / "a_sledkater.ani", std::ios::binary | std::ios::trunc);
                    outAni.write(reinterpret_cast<const char*>(gzAni.data()),
                                 static_cast<std::streamsize>(gzAni.size()));
                    outAni.close();

                    std::ofstream outIni(tmpDir / "sudtest.ini", std::ios::binary | std::ios::trunc);
                    outIni << "[Data]\nGame=myvfx\\;game_common.sue\n";
                    outIni.close();

                    ss2::CombatVfxSystem chainVfx;
                    if (!chainVfx.load(tmpDir) || !chainVfx.hasRetailAssets() ||
                        chainVfx.bank().clipFrameCount(ss2::CombatVfxClip::ExplosionHuge) != 1u ||
                        chainVfx.bank().clipFrameCount(ss2::CombatVfxClip::WakeBoat) != 1u) {
                        std::filesystem::remove_all(tmpDir);
                        throw std::runtime_error("sudtest.ini Game=myvfx\\;game_common.sue unpacked+gzip CombatVfxSystem load failed");
                    }
                }
                std::filesystem::remove_all(tmpDir);
            }

            // Optional retail Gold 2.2 game_common.sue verification.
            std::filesystem::path gameDir;
            if (const char* env = std::getenv("SS2_PRIVATE_GAME"); env && *env) {
                gameDir = env;
            } else if (const char* home = std::getenv("HOME"); home && *home) {
                const auto cand = std::filesystem::path(home) / "ss2-private" / "game" / "app";
                std::error_code ec;
                if (std::filesystem::is_directory(cand, ec)) gameDir = cand;
            }
            if (!gameDir.empty()) {
                std::error_code ec;
                const auto sueCheck = ss2::combat_vfx_detail::resolveCaseInsensitive(gameDir, "game_common.sue");
                if (std::filesystem::is_regular_file(sueCheck, ec)) {
                    ss2::CombatVfxSystem retailVfx;
                    if (!retailVfx.load(gameDir) || !retailVfx.hasRetailAssets()) {
                        throw std::runtime_error("Failed to decode retail Gold 2.2 VFX from " + gameDir.string());
                    }
                    const auto& ratlas = retailVfx.atlas();
                    if (ratlas.width == 0u || ratlas.height == 0u ||
                        ratlas.width > ss2::CombatVfxBank::kMaxAtlasDim ||
                        ratlas.height > ss2::CombatVfxBank::kMaxAtlasDim) {
                        throw std::runtime_error("Retail VFX atlas out of bounds");
                    }
                    const auto& b = retailVfx.bank();
                    if (b.clipFrameCount(ss2::CombatVfxClip::ExplosionHuge) < 10u ||
                        b.clipFrameCount(ss2::CombatVfxClip::CrateBlast1) < 10u ||
                        b.clipFrameCount(ss2::CombatVfxClip::BoxDust) < 10u ||
                        b.clipDirectionCount(ss2::CombatVfxClip::MuzzleCannon) != 32u ||
                        b.clipDirectionCount(ss2::CombatVfxClip::MuzzleHowitzer) != 32u ||
                        b.clipDirectionCount(ss2::CombatVfxClip::MuzzleKatyusha) != 16u ||
                        b.clipFrameCount(ss2::CombatVfxClip::MuzzleMortar) < 10u ||
                        b.clipFrameCount(ss2::CombatVfxClip::FlamePlume) < 15u ||
                        b.clipFrameCount(ss2::CombatVfxClip::WaterSplash3) < 10u ||
                        b.clipFrameCount(ss2::CombatVfxClip::TrackTank) < 10u ||
                        b.clipFrameCount(ss2::CombatVfxClip::WakeBoat) < 10u ||
                        b.clipFrameCount(ss2::CombatVfxClip::ParachuteCanopy) != 21u ||
                        b.clipDirectionCount(ss2::CombatVfxClip::AmmoCrate) != 8u) {
                        throw std::runtime_error("Retail CombatVfxBank missing expected .ANI / .PCK / .RS2 sequences");
                    }
                    renderer.setSpriteAtlas(14u, ratlas);
                    retailVfx.bindAtlasSlot(14u);
                    retailVfx.spawnExplosion(32.0f, 28.0f, 3, false);
                    retailVfx.spawnMuzzleFlash(ss2::CombatVfxClip::MuzzleHowitzer, 20.0f, 24.0f, 0u, 4.0f);
                    retailVfx.spawnWaterSplash(44.0f, 28.0f, 3);
                    retailVfx.spawnParachuteDrop(32.0f, 36.0f, true, false, 16.0f, 0.8f);
                    (void)retailVfx.step(0.18f);

                    std::vector<ss2::CombatVfxSystem::RankedSprite> rItems;
                    retailVfx.appendSprites(1, [](int, int) { return 1.0f; }, rItems);
                    if (rItems.size() < 5u) {
                        throw std::runtime_error("Retail CombatVfxSystem emitted too few sprites");
                    }
                    std::vector<Sprite> rSprites;
                    rSprites.reserve(rItems.size());
                    for (const auto& it : rItems) rSprites.push_back(it.sprite);
                    renderer.setMap(solid(64, 48, ground));
                    renderer.setSprites(rSprites);
                    frame = capture();
                    int changedPixels = 0;
                    for (int py = 96; py < 96 + 48; ++py) {
                        for (int px = 128; px < 128 + 64; ++px) {
                            const auto c = frame.at(px, py);
                            if (c.r != ground.r || c.g != ground.g || c.b != ground.b) ++changedPixels;
                        }
                    }
                    if (changedPixels < 24) {
                        throw std::runtime_error("Retail CombatVfxSystem rendered no visible pixels on VulkanWindow");
                    }
                }
            }
            renderer.setSprites({});
            renderer.setMap(solid(64, 48, ground));
        }

        // Enhanced Graphics pipeline: MapGrade (Pixel-AA / 5-tap Catmull-Rom + CAS + vignette),
        // 3-level mipmapped >=256px sprite atlases, overbright lighting, and additive glow beams.
        {
            renderer.setMap(solid(64, 48, ground));
            renderer.setMapFilterLinear(false);
            renderer.setMapGrade({true, 0.40f, 0.28f, 1.05f, 1.00f, 0.94f, 1.06f});
            frame = capture();
            const Rgb centerG = frame.at(128 + 32, 96 + 24);
            const Rgb cornerG = frame.at(128 + 1, 96 + 1);
            if (cornerG.r >= centerG.r)
                throw std::runtime_error("MapGrade vignette did not darken viewport corner relative to center");

            renderer.setMapFilterLinear(true);
            frame = capture();
            if (frame.at(128 + 1, 96 + 1).r >= frame.at(128 + 32, 96 + 24).r)
                throw std::runtime_error("MapGrade Catmull-Rom mode vignette failed");

            // Animated water shimmer modulates deep blue river texels over time while leaving stone calm.
            const Rgb water{36, 78, 128};
            renderer.setMapFilterLinear(false);
            renderer.setMap(solid(64, 48, water));
            renderer.setMapGrade({true, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f});
            const auto waterT0 = capture();
            renderer.setMapGrade({true, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.4f, 1.0f});
            const auto waterT1 = capture();
            bool waterAnimated = false;
            for (int px = 128 + 8; px < 128 + 48 && !waterAnimated; ++px) {
                const auto c0 = waterT0.at(px, 96 + 24);
                const auto c1 = waterT1.at(px, 96 + 24);
                if (std::abs(int(c0.b) - int(c1.b)) >= 4 || std::abs(int(c0.g) - int(c1.g)) >= 4) {
                    waterAnimated = true;
                }
            }
            if (!waterAnimated)
                throw std::runtime_error("MapGrade waterShimmer did not animate water texels across timeSec");

            renderer.setMap(solid(64, 48, ground));
            renderer.setMapGrade({});
            frame = capture();
            expect(frame, 128 + 32, 96 + 24, ground, 0, "disabling MapGrade restores exact map color");

            rejects([&] {
                ss2::MapGrade bad{};
                bad.sharpness = std::numeric_limits<float>::quiet_NaN();
                renderer.setMapGrade(bad);
            }, "MapGrade with NaN");

            // >=256x256 atlas triggers 3-level mipmaps without validation errors, and brightness > 1.0 boosts lit texels.
            renderer.setSpriteAtlas(8, solid(256, 256, {120, 100, 80}));
            const float whole256[4] = {0.f, 0.f, 256.f, 256.f};
            renderer.setSprites({sprite(16, 12, 16, 16, whole256, 8, 1.4f, 1.0f)});
            frame = capture();
            const Rgb lit = frame.at(128 + 24, 96 + 20);
            if (lit.r <= 122 || lit.g <= 102 || lit.b <= 82)
                throw std::runtime_error("Overbright dynamic sprite lighting did not brighten lit texels");
            renderer.setSprites({});

            // Additive MapBeam boosts underlying ground luminosity cleanly instead of darkening toward RGB.
            renderer.setMap(solid(64, 48, ground));
            renderer.setBeams({{32, 24, 32, 24, 16, 90, 70, 30, 0.85f, 0.85f, true}});
            frame = capture();
            const Rgb glowPx = frame.at(128 + 32, 96 + 24);
            if (glowPx.r <= ground.r + 5 || glowPx.g <= ground.g + 4)
                throw std::runtime_error("Additive glow beam failed to brighten ground pixels");
            renderer.setBeams({});
        }

        // Ordinary presentation still works with sprites set.
        for (int i = 0; i < 3; ++i) {
            bool shown = false;
            for (int attempt = 0; attempt < 200 && !shown; ++attempt) { SDL_PumpEvents(); shown = renderer.present({}); if (!shown) SDL_Delay(5); }
            if (!shown) throw std::runtime_error("Timed out waiting for presentation");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
