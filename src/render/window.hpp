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

// Outlined map rectangles for order feedback, drawn above the selection highlight.
// Kinds differ in line weight as well as colour: attack is drawn two pixels thick.
enum class MarkerKind { selection, attack, move };
struct MapMarker { MapRect rect; MarkerKind kind; };

// panel and menu: centred. notice: bottom centre, above the bar when there is one. hud: a bar
// flush with the bottom edge. tooltip: wherever moveOverlay() put it, kept inside the
// window. Drawn back to front: hud, tooltip, notice, panel, menu.
enum class OverlaySlot { panel, notice, hud, menu, tooltip };

// A small horizontal gauge drawn over the map, e.g. a unit's health. x is the centre and
// y the top edge, in map pixels; width is in map pixels and scales with zoom, the height
// is a few screen pixels. value is 0..1 and also picks the colour: green, amber, then red.
struct MapBar { float x{}, y{}, width{}, value{}; };

// A soft-edged streak of light between two map points: a tracer, a flame jet, a beam.
// width is in map pixels, measured across the glow; the bright core is about half of it.
// alpha0 and alpha1 are the opacity at the start and the end, so a tracer can fade
// towards its tail. When both ends are the same point it is drawn as a round flash of
// that diameter, e.g. a muzzle flash. Colours are plain sRGB. The caller owns timing:
// send a new list each frame an effect changes, and drop an effect to end it.
struct MapBeam {
    float x0{}, y0{}, x1{}, y1{}, width{2};
    std::uint8_t r{255}, g{255}, b{255};
    float alpha0{1}, alpha1{1};
};

// A textured rectangle drawn above the terrain and below outlines, markers and panels.
struct Sprite {
    float x{}, y{}, width{}, height{};   // destination rectangle in map pixels
    float u0{}, v0{}, u1{}, v1{};        // source rectangle in atlas pixels
    unsigned atlas = 0;
    // brightness scales the displayed colour (as the terrain fog does); alpha scales opacity.
    float brightness = 1, alpha = 1;
};

class VulkanWindow {
public:
    explicit VulkanWindow(SDL_Window*, ValidationReport* = nullptr);
    ~VulkanWindow();
    void setMap(const assets::MapImage&, unsigned maxTileSize = 0);
    // Replaces a rectangle of the current map with the same RGBA8 layout as setMap.
    // Patches are queued, applied in call order before the next presented frame, and
    // clipped to the map. Throws if there is no map or the patch is malformed.
    void updateMapRegion(int x, int y, const assets::MapImage& patch);
    // Reads the map image back, including queued patches. Slow; for tests and screenshots.
    assets::MapImage captureMap();
    void focusMap(int x, int y, bool zoomFromOverview = false);
    std::pair<double, double> mapPosition(float windowX, float windowY) const;
    void highlight(std::vector<MapRect>);
    // Replaces all markers; an empty list removes them. They stay until replaced
    // or until the map is replaced, so the caller decides how long feedback lasts.
    // Replaces all gauges; an empty list removes them. Drawn above sprites, clipped to the
    // map area. Replacing the map removes them.
    void setBars(std::vector<MapBar>);
    // Move the camera by screen-space window points (scaled by current zoom and clamped to map bounds).
    void panMap(double screenDx, double screenDy);
    // Replaces all light streaks and flashes; an empty list removes them. Drawn above
    // every sprite and below gauges and outlines, clipped to the map area. One list may
    // use at most maxBeamColours different colours. Replacing the map removes them.
    static constexpr unsigned maxBeamColours = 16;
    void setBeams(std::vector<MapBeam>);
    // Terrain scaling filter. Nearest (the default) keeps pixel art crisp; linear smooths
    // a high-resolution map when zoomed in. Takes effect on the next frame.
    void setMapFilterLinear(bool linear);
    void setPreferSoftwareRenderer(bool prefer);
    static void setPreferSoftwareRendererDefault(bool prefer);
    void setMarkers(std::vector<MapMarker>);
    // Sprite atlases: RGBA8 with straight (non-premultiplied) alpha, sampled with
    // linear filtering. Replacing a slot is allowed at any time. Slots [16,24) are
    // reserved for plane sprites (7 live + 1 overflow); infantry/world stay below 16.
    static constexpr unsigned spriteAtlasSlots = 24;
    void setSpriteAtlas(unsigned slot, const assets::MapImage& atlas);
    // Replaces all sprites. They are alpha blended in list order, clipped to the map's
    // on-screen rectangle, and follow the camera. Replacing the map removes them.
    // Throws if a sprite names an atlas slot that has not been set.
    void setSprites(std::vector<Sprite>);
    // Renders one frame, presents it and returns its pixels as RGBA8, top row first.
    // For tests and screenshots. Throws if the surface cannot be read back.
    assets::MapImage captureFrame();
    bool surfaceIsSrgb() const;
    // An opaque RGBA8 panel drawn centred over everything else, clipped to the window.
    void setOverlay(unsigned width, unsigned height, std::span<const std::uint8_t> rgba,
                    OverlaySlot = OverlaySlot::panel);
    void clearOverlay(OverlaySlot = OverlaySlot::panel);
    // Top-left corner, in window pixels, for a slot that has no fixed place (tooltip).
    void moveOverlay(OverlaySlot, int left, int top);
    void handleEvent(const SDL_Event&);
    std::pair<unsigned, unsigned> extent() const;
    bool present(const Snapshot&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
