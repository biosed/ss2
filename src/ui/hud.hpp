#pragma once

#include "assets/map_image.hpp"
#include "assets/mission.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct _PangoContext;

namespace ss2 {
class VulkanWindow;
struct Sprite;
struct MapBeam;

// One line of the selection readout. health and ammo are 0..1, averaged by the caller.
struct HudClass {
    std::string label;
    unsigned count = 0;
    float health = 1, ammo = 1;
    bool operator==(const HudClass&) const = default;
};

// Unit classification for distinct minimap blip silhouettes.
enum class HudDotKind : std::uint8_t { Infantry = 0, Armor = 1, Structure = 2, Zeppelin = 3 };

// A unit on the minimap, as a 0..1 fraction of the map. The caller applies visibility rules.
struct HudDot {
    float u = 0, v = 0;
    bool hostile = false;
    bool allied = false;
    bool neutral = false;
    HudDotKind kind = HudDotKind::Infantry;
    bool operator==(const HudDot&) const = default;
};

// A fading radar pulse on the minimap (red for contact/loss, cyan for reinforcements/rally).
struct HudPing {
    float u = 0, v = 0;
    bool hostile = false;
    float intensity = 1.0f;
    bool operator==(const HudPing&) const = default;
};

// A saved control group: the digit that recalls it and how many units it holds.
struct HudGroup {
    unsigned digit = 0, count = 0;
    bool operator==(const HudGroup&) const = default;
};

struct HudPlane {
    float u = 0, v = 0;
    bool hostile = false;
    bool operator==(const HudPlane&) const = default;
};

struct HudState {
    std::string objective;                // one line; shown in the strip along the top of the bar
    std::vector<HudGroup> groups;         // only groups that exist
    std::vector<HudClass> selection;
    std::array<float, 5> meters{};        // EX, L, M, A, R in 0..1
    bool hasSelection = false;
    unsigned losses = 0;
    bool crouched = false, holdFire = false, paused = false, binoculars = false;
    bool attackMoveArmed = false, patrolArmed = false, attackGroundArmed = false, mineArmed = false;
    bool hasCargo = false, hasBombard = false, hasSapper = false, hasInfantry = false;
    std::vector<HudDot> dots;
    float viewU = 0, viewV = 0, viewW = 1, viewH = 1;   // camera rectangle as map fractions
    std::vector<HudPing> pings;
    bool hasObjectiveMarker = false;
    float objectiveU = 0.0f, objectiveV = 0.0f;
    std::array<int, 5> airReady{};
    int armedAir = -1;
    std::vector<HudPlane> planes;
    std::string activeSubgroup;
    bool operator==(const HudState&) const = default;
};

enum class HudAction {
    none, stop, holdFire, crouch, mission, pause, log, moveCamera, menu, recon,
    orderMinimap, airSupport, attackMove, patrol, attackGround, unload, mine,
    scatter, recallGroup, selectClass, speedDown, speedUp
};

struct HudResult {
    bool consumed = false;              // the event belongs to the bar; do not pass it on
    HudAction action = HudAction::none;
    float u = 0, v = 0;                 // map fraction, for moveCamera
    int airSlot = -1;                   // 0..4, for airSupport
    int groupDigit = -1;                // 0..9, for recallGroup
    std::string classLabel;             // HudClass::label, for selectClass
    bool shiftHeld = false;             // Shift modifier on click release
    bool hoverEntered = false;
};

struct HudTip {
    std::string title;
    std::vector<std::string> lines;
    bool operator==(const HudTip&) const = default;
};

// A command bar along the bottom of the parent window: minimap, selection readout and
// command buttons. It shows state and reports what was clicked; it never changes game
// state itself. The caller performs the action and sends the new state through update().
class Hud {
public:
    explicit Hud(SDL_Window* parent);
    ~Hud();

    void setMinimap(const assets::MapImage& overview);   // any size; scaled to fit
    void setPortrait(const std::string& label, const assets::MapImage& image);
    void update(const HudState&);                        // redraws only when something changed

    // Optional retail Gold 2.2 tactical skin (lng.sue\panel.dat, pdrop.dat, game.lng) and
    // damaged-aircraft VFX animations (game_common.sue\AIRFIRE.ANI, AIRSMOKE.ANI).
    // Safe to omit or call with a missing directory; falls back cleanly to procedural art.
    bool loadRetailAssets(const std::filesystem::path& gameDir);
    bool hasRetailAssets() const noexcept;
    std::size_t panelRecordCount() const noexcept;
    std::size_t pdropRecordCount() const noexcept;
    std::size_t retailTextCount() const noexcept;
    std::string retailText(std::string_view key, std::size_t index = 0) const;

    // Live atmospheric weather overlay across the map viewport (Clear has zero overhead).
    void setWeather(assets::WeatherKind kind, std::uint32_t phaseTicks = 0);
    assets::WeatherKind weather() const noexcept { return weather_; }
    std::uint32_t weatherPhase() const noexcept { return weatherPhase_; }

    // Damaged-aircraft smoke & fire trail VFX (AIRFIRE.ANI / AIRSMOKE.ANI + beam fallback).
    bool hasAirVfxAtlas() const noexcept;
    const assets::MapImage& airVfxAtlas() const noexcept;
    std::size_t airFireFrameCount() const noexcept;
    std::size_t airSmokeFrameCount() const noexcept;
    void bindAirVfxAtlasSlot(unsigned slot) noexcept;
    std::optional<unsigned> airVfxAtlasSlot() const noexcept;
    void recordAirSortieTrail(std::uint32_t sortieId, int mapX, int mapY, unsigned dir32,
                              int hp, int maxHp, bool alive, bool active, bool tookHit);
    void recordAirCrashTrail(int mapX, int mapY);
    bool stepAirTrails(float dtSeconds);
    bool hasActiveAirTrails() const noexcept;
    void appendAirTrailSprites(int mapScale, std::vector<Sprite>& outSprites) const;
    void appendAirTrailBeams(int mapScale, std::size_t maxCount, std::vector<MapBeam>& outBeams) const;

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
    Rect airButtonRect(int slot) const;
    Rect groupChipRect(unsigned digit) const;
    Rect classCardRect(std::size_t index) const;
    bool opaqueAtWindow(float windowX, float windowY) const;
    std::optional<HudTip> tooltipAtWindow(float windowX, float windowY) const;
    void cancelMinimapDrag() { draggingMinimap_ = false; }

private:
    struct RetailAssets;
    void layout();
    void render();
    void refit();   // lays the bar out again when the window's pixel size no longer matches
    Rect reconButtonRect() const;
    Rect row3Rect(int col) const;
    std::optional<HudAction> row3Action(int col) const;
    bool opaqueAtBarPoint(const std::optional<SDL_FPoint>& p) const;
    std::optional<SDL_FPoint> barPoint(float windowX, float windowY) const;
    std::optional<HudAction> buttonAt(float x, float y) const;
    std::optional<int> airAt(float x, float y) const;
    std::optional<unsigned> groupAt(float x, float y) const;
    std::optional<std::string> classAt(float x, float y) const;
    bool minimapPoint(float x, float y, float& u, float& v, bool clampOutside) const;

    SDL_Window* parent_;
    std::shared_ptr<_PangoContext> fonts_;
    HudState state_;
    assets::MapImage overview_;
    std::map<std::string, assets::MapImage> portraits_;
    unsigned width_ = 0, height_ = 0;
    int top_ = 0;                        // window-pixel row where the bar starts
    double uiScale_ = 1.0;
    Rect minimap_, readout_;
    Rect speedDownPip_{}, speedUpPip_{}, pausePip_{};
    struct Button { HudAction action; const char* label; const char* key; Rect rect; };
    std::vector<Button> buttons_;
    std::optional<HudAction> pressed_, hovered_;
    std::optional<int> pressedAir_, hoveredAir_;
    std::optional<unsigned> pressedGroup_, hoveredGroup_;
    std::optional<std::string> pressedClass_, hoveredClass_;
    std::vector<std::string> messages_;
    int language_ = -1;                  // the interface language the picture was drawn in
    int strip_ = 0;                      // height of the strip along the top of the bar
    bool draggingMinimap_ = false;
    std::vector<std::uint8_t> rgba_;
    assets::WeatherKind weather_ = assets::WeatherKind::Clear;
    std::uint32_t weatherPhase_ = 0;
    std::unique_ptr<RetailAssets> retail_;
    std::uint64_t revision_ = 0, drawn_ = 0;
    std::uint64_t weatherRevision_ = 0, weatherDrawn_ = 0;
};
}
