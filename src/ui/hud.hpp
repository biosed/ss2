#pragma once

#include "assets/map_image.hpp"
#include <SDL3/SDL.h>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct _PangoContext;

namespace ss2 {
class VulkanWindow;

// One line of the selection readout. health and ammo are 0..1, averaged by the caller.
struct HudClass {
    std::string label;
    unsigned count = 0;
    float health = 1, ammo = 1;
    bool operator==(const HudClass&) const = default;
};

// A unit on the minimap, as a 0..1 fraction of the map. The caller applies visibility rules.
struct HudDot {
    float u = 0, v = 0;
    bool hostile = false;
    bool operator==(const HudDot&) const = default;
};

// A saved control group: the digit that recalls it and how many units it holds.
struct HudGroup {
    unsigned digit = 0, count = 0;
    bool operator==(const HudGroup&) const = default;
};

struct HudState {
    std::string objective;                // one line; shown in the strip along the top of the bar
    std::vector<HudGroup> groups;         // only groups that exist
    std::vector<HudClass> selection;
    unsigned losses = 0;
    bool crouched = false, holdFire = false, paused = false;
    std::vector<HudDot> dots;
    float viewU = 0, viewV = 0, viewW = 1, viewH = 1;   // camera rectangle as map fractions
    bool operator==(const HudState&) const = default;
};

enum class HudAction { none, stop, holdFire, crouch, mission, pause, log, moveCamera };

struct HudResult {
    bool consumed = false;              // the event belongs to the bar; do not pass it on
    HudAction action = HudAction::none;
    float u = 0, v = 0;                 // map fraction, for moveCamera
};

// A command bar along the bottom of the parent window: minimap, selection readout and
// command buttons. It shows state and reports what was clicked; it never changes game
// state itself. The caller performs the action and sends the new state through update().
class Hud {
public:
    explicit Hud(SDL_Window* parent);

    void setMinimap(const assets::MapImage& overview);   // any size; scaled to fit
    void setPortrait(const std::string& label, const assets::MapImage& image);
    void update(const HudState&);                        // redraws only when something changed

    // Message log. The newest message is shown in the strip; logText() returns the most
    // recent ones, newest last, one per line, for showing in a panel when Log is clicked.
    void log(std::string message);
    std::string logText(std::size_t newest = 40) const;
    std::size_t logSize() const { return messages_.size(); }

    // Call before any other input handling. Mouse input inside the bar is consumed.
    // A button reports its action when the left button is released on it; the minimap
    // reports moveCamera on press and while dragging.
    HudResult handle(const SDL_Event&);

    void draw(VulkanWindow&);                            // once per frame; uploads only after a change

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }          // window pixels
    const std::vector<std::uint8_t>& rgba() const { return rgba_; }

    // Geometry in bar pixels, for tests and for callers that want to align with it.
    struct Rect { int x = 0, y = 0, width = 0, height = 0; };
    Rect minimapRect() const { return minimap_; }
    Rect buttonRect(HudAction) const;

private:
    void layout();
    void render();
    void refit();   // lays the bar out again when the window's pixel size no longer matches
    std::optional<SDL_FPoint> barPoint(float windowX, float windowY) const;
    std::optional<HudAction> buttonAt(float x, float y) const;
    bool minimapPoint(float x, float y, float& u, float& v, bool clampOutside) const;

    SDL_Window* parent_;
    std::shared_ptr<_PangoContext> fonts_;
    HudState state_;
    assets::MapImage overview_;
    std::map<std::string, assets::MapImage> portraits_;
    unsigned width_ = 0, height_ = 0;
    int top_ = 0;                        // window-pixel row where the bar starts
    Rect minimap_, readout_;
    struct Button { HudAction action; const char* label; const char* key; Rect rect; };
    std::vector<Button> buttons_;
    std::optional<HudAction> pressed_, hovered_;
    std::vector<std::string> messages_;
    int strip_ = 0;                      // height of the strip along the top of the bar
    bool draggingMinimap_ = false;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0, drawn_ = 0;
};
}
