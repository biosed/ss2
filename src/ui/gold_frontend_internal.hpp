#pragma once

#include "ui/gold_frontend.hpp"

#include "assets/briefing_media.hpp"
#include "assets/mission.hpp"
#include "assets/sprite.hpp"
#include "ui/cursor_bank.hpp"
#include "ui/strings.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ss2::gold_ui {

using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;
using FontMap = std::unique_ptr<PangoFontMap, decltype(&g_object_unref)>;
using FontCtx = std::unique_ptr<PangoContext, decltype(&g_object_unref)>;
using FontDesc = std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)>;

inline constexpr int kStageW = 800;
inline constexpr int kStageH = 600;
inline constexpr int kVisibleMissionRows = 17;
inline constexpr double kListX = 43.0;
inline constexpr double kListY = 134.0;
inline constexpr double kListW = 330.0;
inline constexpr double kListH = 352.0;
inline constexpr double kRowH = 20.0;
inline constexpr double kScrollX = 373.0;
inline constexpr double kScrollY = 134.0;
inline constexpr double kScrollW = 18.0;
inline constexpr double kScrollArrowH = 18.0;
inline constexpr double kScrollH = 352.0;
inline constexpr double kTrackY = kScrollY + kScrollArrowH;
inline constexpr double kTrackH = kScrollH - 2.0 * kScrollArrowH;
inline constexpr double kThumbH = 21.0;

inline constexpr int kVisibleProfileRows = 17;

inline constexpr double kBriefBoxX = 30.0;
inline constexpr double kBriefBoxY = 18.0;
inline constexpr double kBriefBoxW = 728.0;
inline constexpr double kBriefBoxH = 140.0;

inline constexpr int kVisibleRatingsRows = 15;
inline constexpr double kRatingsRowH = 20.0;
inline constexpr double kRatingsTableX = 43.0;
inline constexpr double kRatingsTableY = 136.0;
inline constexpr double kRatingsTableW = 696.0;
inline constexpr double kRatingsTableH = 352.0;
inline constexpr double kRatingsScrollX = 739.0;
inline constexpr double kRatingsScrollY = 136.0;
inline constexpr double kRatingsScrollW = 18.0;
inline constexpr double kRatingsScrollH = 352.0;
inline constexpr double kRatingsTrackY = kRatingsScrollY + kScrollArrowH;
inline constexpr double kRatingsTrackH = kRatingsScrollH - 2.0 * kScrollArrowH;

inline constexpr double kCreditsClipX = 112.0;
inline constexpr double kCreditsClipY = 74.0;
inline constexpr double kCreditsClipW = 576.0;
inline constexpr double kCreditsClipH = 410.0;

inline constexpr std::array<const char*, 9> kOptionRowIds{{
    "opt_difficulty", "opt_language", "opt_sfx", "opt_music_vol",
    "opt_music_mute", "opt_speed", "opt_smooth", "opt_effects", "opt_skip_intro"
}};

struct RectD {
    double x, y, w, h;
    bool contains(double px, double py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct ButtonSpec {
    const char* id;
    const char* enLabel;
    char shortcut;
    RectD rect;
    bool enabled;
};

inline std::string hitTestButtons(std::span<const ButtonSpec> specs, double sx, double sy) {
    for (const auto& b : specs) {
        if (b.rect.contains(sx, sy)) return b.id;
    }
    return {};
}

inline double thumbTopFor(int scrollOffset, std::size_t rowCount,
                          int visibleRows = kVisibleMissionRows,
                          double trackY = kTrackY, double trackH = kTrackH) {
    if (int(rowCount) <= visibleRows) return trackY;
    const double maxScroll = double(rowCount - std::size_t(visibleRows));
    return trackY + (trackH - kThumbH) * (double(scrollOffset) / maxScroll);
}

inline const char* difficultyKey(int diff) {
    return diff == 1 ? "Easy" : diff == 3 ? "Hard" : "Normal";
}

inline bool parseIndexAfter(std::string_view text, std::string_view prefix, std::size_t& out) {
    if (!text.starts_with(prefix)) return false;
    const std::string_view digits = text.substr(prefix.size());
    if (digits.empty()) return false;
    const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), out);
    return res.ec == std::errc{} && res.ptr == digits.data() + digits.size();
}

inline Surface makeNullSurface() {
    return Surface(nullptr, cairo_surface_destroy);
}

struct PckSurface {
    int x{}, y{}, w{}, h{};
    Surface surface = makeNullSurface();
};

Surface mapImageToSurface(const assets::MapImage& img);
PckSurface pckFrameToSurface(const assets::SpriteFrame& frame, bool keyMagenta);
void tryLoadPckSlot(const std::filesystem::path& archive, const char* entry,
                    std::size_t frameIdx, bool keyMagenta, PckSurface& dst);
std::string toUpperAscii(std::string s);
std::string sanitizeUtf8(std::string_view input);
Layout makeTextLayout(PangoContext* ctx, const std::string& text, double sizePx,
                      bool bold, double maxWidthPx = -1.0,
                      PangoAlignment align = PANGO_ALIGN_LEFT);
std::string buildGoldMarkup(const std::string& label, char shortcut);
void drawSurfaceStretched(cairo_t* cr, cairo_surface_t* surf, double x, double y, double w, double h);
void drawBronzeBox(cairo_t* cr, double x, double y, double w, double h, double fillAlpha);
// menu_common.sue ramka.pck: 8 x 5x5 riveted border pieces [TL,T,TR,L,R,BL,B,BR].
// While a RamkaScope is alive, drawBronzeBox frames boxes with the retail 9-slice
// instead of the vector bronze stroke.
using RamkaFrames = std::array<PckSurface, 8>;
// Blit a decoded PCK frame at its native size; false when the frame is missing.
bool drawPckAt(cairo_t* cr, const PckSurface& p, double x, double y);
void drawRamkaBorder(cairo_t* cr, const RamkaFrames& frames, double x, double y, double w, double h);
class RamkaScope {
public:
    explicit RamkaScope(const RamkaFrames* frames);
    ~RamkaScope();
    RamkaScope(const RamkaScope&) = delete;
    RamkaScope& operator=(const RamkaScope&) = delete;
private:
    const RamkaFrames* prev_;
};
void drawFallbackBackdrop(cairo_t* cr, bool briefing);
void drawHeading(cairo_t* cr, PangoContext* ctx, const std::string& text, const RectD& r, double px = 17.0);
void drawGoldStencilButton(cairo_t* cr, PangoContext* ctx, const RectD& r,
                           const std::string& label, char shortcut,
                           bool hovered, bool focused, bool enabled,
                           double basePx = 24.0, bool preserveCase = false);
void drawDiamondOverview(cairo_t* cr, cairo_surface_t* surf,
                         double x, double y, double w, double h,
                         bool drawFrameStroke);

}  // namespace ss2::gold_ui

namespace ss2 {

struct GoldFrontEnd::Assets {
    gold_ui::FontMap fontMap{pango_cairo_font_map_new(), g_object_unref};
    gold_ui::FontCtx fontCtx{pango_font_map_create_context(fontMap.get()), g_object_unref};

    gold_ui::Surface mainBg = gold_ui::makeNullSurface();
    gold_ui::Surface briefingBg = gold_ui::makeNullSurface();
    gold_ui::PckSurface creditsBg;
    gold_ui::PckSurface ratingsDropdownBtn;
    gold_ui::PckSurface nbramkaFrame;
    gold_ui::RamkaFrames ramka;                       // ramka.pck[0..7] 5x5 9-slice
    gold_ui::PckSurface speedBar;                     // speedbar.pck[0] 124x11
    std::array<gold_ui::PckSurface, 2> speedThumb;    // speed_bt.pck[0..1] 21x13
    std::array<gold_ui::PckSurface, 2> rombik;        // rombik.pck[0]=unlit, [10]=lit 25x14
    std::array<gold_ui::PckSurface, 36> colorSwatch;  // colors_bt.pck 12 colours x 3 states
    std::array<gold_ui::PckSurface, 12> slotButtons;   // nbrif_bt[8..19]
    std::array<gold_ui::PckSurface, 6> scrollButtons;  // scrollbar_bt[0..5]
    std::array<gold_ui::PckSurface, 3> scrollTrack;    // scrollbar_sp[0..2]
    std::unordered_map<std::string, gold_ui::Surface> overviews;
    std::vector<std::string> creditsLines;
    assets::MissionBriefingMedia briefMedia;
    gold_ui::Surface briefOverlay = gold_ui::makeNullSurface();
    SDL_AudioDeviceID briefAudioDev{0};
    SDL_AudioStream* briefAudioStream{nullptr};
    MenuCursorAnimator menuCursor;
    bool loadedMenuSue{false};

    ~Assets() {
        if (briefAudioStream) {
            SDL_DestroyAudioStream(briefAudioStream);
            briefAudioStream = nullptr;
        }
        if (briefAudioDev) {
            SDL_CloseAudioDevice(briefAudioDev);
            briefAudioDev = 0;
        }
    }
};

struct GoldFrontEnd::RenderEnv {
    cairo_t* cr{};
    PangoContext* pangoCtx{};
};

}  // namespace ss2
