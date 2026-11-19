// Checks the sprite pass by reading rendered frames back and comparing pixel colours.
#include "render/window.hpp"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
