#include "ui/gold_frontend.hpp"

#include "assets/mission.hpp"
#include "assets/sprite.hpp"
#include "render/window.hpp"
#include "ui/strings.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ss2 {
namespace {

using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;
using FontMap = std::unique_ptr<PangoFontMap, decltype(&g_object_unref)>;
using FontCtx = std::unique_ptr<PangoContext, decltype(&g_object_unref)>;
using FontDesc = std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)>;

constexpr int kStageW = 800;
constexpr int kStageH = 600;
constexpr int kVisibleMissionRows = 17;
constexpr double kListX = 43.0;
constexpr double kListY = 134.0;
constexpr double kListW = 330.0;
constexpr double kListH = 352.0;
constexpr double kRowH = 20.0;
constexpr double kScrollX = 373.0;
constexpr double kScrollY = 134.0;
constexpr double kScrollW = 18.0;
constexpr double kScrollArrowH = 18.0;
constexpr double kScrollH = 352.0;
constexpr double kTrackY = kScrollY + kScrollArrowH;
constexpr double kTrackH = kScrollH - 2.0 * kScrollArrowH;
constexpr double kThumbH = 21.0;

double thumbTopFor(int scrollOffset, std::size_t rowCount,
                   int visibleRows = kVisibleMissionRows,
                   double trackY = kTrackY, double trackH = kTrackH) {
    if (int(rowCount) <= visibleRows) return trackY;
    const double maxScroll = double(rowCount - std::size_t(visibleRows));
    return trackY + (trackH - kThumbH) * (double(scrollOffset) / maxScroll);
}

constexpr double kBriefBoxX = 30.0;
constexpr double kBriefBoxY = 18.0;
constexpr double kBriefBoxW = 728.0;
constexpr double kBriefBoxH = 140.0;

constexpr int kVisibleRatingsRows = 15;
constexpr double kRatingsRowH = 20.0;
constexpr double kRatingsTableX = 43.0;
constexpr double kRatingsTableY = 136.0;
constexpr double kRatingsTableW = 696.0;
constexpr double kRatingsTableH = 352.0;
constexpr double kRatingsScrollX = 739.0;
constexpr double kRatingsScrollY = 136.0;
constexpr double kRatingsScrollW = 18.0;
constexpr double kRatingsScrollH = 352.0;
constexpr double kRatingsTrackY = kRatingsScrollY + kScrollArrowH;
constexpr double kRatingsTrackH = kRatingsScrollH - 2.0 * kScrollArrowH;

constexpr double kCreditsClipX = 112.0;
constexpr double kCreditsClipY = 74.0;
constexpr double kCreditsClipW = 576.0;
constexpr double kCreditsClipH = 410.0;

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

constexpr std::array<ButtonSpec, 7> kMainButtons{{
    {"single", "Single Player", 'S', {65, 111, 246, 35}, true},
    {"multi", "Multiplayer", 'M', {78, 178, 220, 35}, true},
    {"options", "Options", 'O', {115, 245, 141, 35}, true},
    {"ratings", "Ratings", 'R', {115, 312, 140, 35}, true},
    {"replay", "Replay", 'P', {115, 378, 140, 35}, true},
    {"credits", "Credits", 'C', {115, 445, 140, 35}, true},
    {"quit", "Quit", '\0', {146, 512, 83, 35}, true},
}};

constexpr std::array<ButtonSpec, 7> kSinglePlayerButtons{{
    {"profile_new", "New", '\0', {305, 97, 56, 27}, true},
    {"profile_rename", "Rename", '\0', {69, 126, 98, 30}, true},
    {"campaigns", "Campaigns", 'C', {78, 179, 218, 35}, true},
    {"missions", "Single Missions", 'S', {40, 246, 293, 35}, true},
    {"load", "Load", 'L', {134, 312, 108, 35}, true},
    {"profile_delete", "Delete", '\0', {480, 462, 82, 27}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr std::array<ButtonSpec, 2> kCampaignFooterButtons{{
    {"custom", "Custom", 'C', {118, 512, 140, 35}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr std::array<ButtonSpec, 2> kSingleMissionButtons{{
    {"start", "Start", 'S', {135, 512, 106, 35}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr RectD kDifficultyButtonRect{268, 515, 244, 30};
constexpr RectD kRatingsCampaignsTabRect{43, 94, 168, 28};
constexpr RectD kRatingsMissionsTabRect{220, 94, 210, 28};
constexpr RectD kRatingsCancelRect{542, 512, 118, 35};
constexpr RectD kCreditsFrameRect{96, 62, 608, 434};
constexpr RectD kCreditsCancelRect{542, 512, 118, 35};

constexpr std::array<ButtonSpec, 4> kBriefingButtons{{
    {"brief_restart", "Restart", 'R', {577, 211, 186, 33}, false},
    {"brief_pause", "Pause", 'P', {627, 253, 136, 33}, false},
    {"start", "Start", 'S', {627, 484, 136, 33}, true},
    {"cancel", "Cancel", '\0', {627, 526, 136, 33}, true},
}};

RectD campaignRectFor(std::size_t idx, std::size_t count) {
    static constexpr std::array<double, 5> kGoldYs{{111.0, 178.0, 245.0, 312.0, 378.0}};
    if (count <= kGoldYs.size() && idx < kGoldYs.size()) {
        return RectD{78.0, kGoldYs[idx], 220.0, 35.0};
    }
    const double step = 338.0 / double(std::max<std::size_t>(1, count));
    const double h = std::min(34.0, std::max(22.0, step - 4.0));
    return RectD{78.0, 102.0 + double(idx) * step, 220.0, h};
}

struct CampaignButtonDisplay {
    std::string label;
    char shortcut;
};

CampaignButtonDisplay campaignButtonDisplay(const std::string& name) {
    std::string low = name;
    for (char& ch : low) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (low.find("german") != std::string::npos || low == "de") return {tr("Germany"), 'G'};
    if (low.find("japan") != std::string::npos) return {tr("Japan"), 'J'};
    if (low.find("brit") != std::string::npos || low == "gb" || low == "uk") return {tr("GB"), 'B'};
    if (low == "us" || low.starts_with("us ") || low.find("usa") != std::string::npos ||
        low.find("american") != std::string::npos || low.find("u.s.") != std::string::npos) {
        return {tr("USA"), 'U'};
    }
    if (low.find("russ") != std::string::npos || low.find("ussr") != std::string::npos ||
        low.find("soviet") != std::string::npos) {
        return {tr("USSR"), 'R'};
    }
    return {name, '\0'};
}

const char* difficultyKey(int diff) {
    return diff == 1 ? "Easy" : diff == 3 ? "Hard" : "Normal";
}

bool parseIndexAfter(std::string_view text, std::string_view prefix, std::size_t& out) {
    if (!text.starts_with(prefix)) return false;
    const std::string_view digits = text.substr(prefix.size());
    if (digits.empty()) return false;
    const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), out);
    return res.ec == std::errc{} && res.ptr == digits.data() + digits.size();
}

Surface makeNullSurface() {
    return Surface(nullptr, cairo_surface_destroy);
}

Surface mapImageToSurface(const assets::MapImage& img) {
    if (!img.width || !img.height || img.rgba.size() != std::size_t(img.width) * img.height * 4) {
        return makeNullSurface();
    }
    Surface surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(img.width), int(img.height)),
                 cairo_surface_destroy);
    if (cairo_surface_status(surf.get()) != CAIRO_STATUS_SUCCESS) return makeNullSurface();
    unsigned char* dst = cairo_image_surface_get_data(surf.get());
    const int stride = cairo_image_surface_get_stride(surf.get());
    if (!SDL_ConvertPixels(int(img.width), int(img.height), SDL_PIXELFORMAT_RGBA32,
                           img.rgba.data(), int(img.width) * 4, SDL_PIXELFORMAT_ARGB8888,
                           dst, stride)) {
        return makeNullSurface();
    }
    cairo_surface_mark_dirty(surf.get());
    return surf;
}

struct PckSurface {
    int x{}, y{}, w{}, h{};
    Surface surface = makeNullSurface();
};

PckSurface pckFrameToSurface(const assets::SpriteFrame& frame, bool keyMagenta) {
    PckSurface out{frame.x, frame.y, frame.width, frame.height, makeNullSurface()};
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.pixels.size() != std::size_t(frame.width) * frame.height * 2) {
        return out;
    }
    Surface surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, frame.width, frame.height),
                 cairo_surface_destroy);
    if (cairo_surface_status(surf.get()) != CAIRO_STATUS_SUCCESS) return out;
    unsigned char* raw = cairo_image_surface_get_data(surf.get());
    const int stride = cairo_image_surface_get_stride(surf.get());
    for (int y = 0; y < frame.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(raw + std::size_t(y) * stride);
        for (int x = 0; x < frame.width; ++x) {
            const std::size_t idx = (std::size_t(y) * frame.width + x) * 2;
            const std::uint16_t val =
                static_cast<std::uint16_t>(frame.pixels[idx]) |
                (static_cast<std::uint16_t>(frame.pixels[idx + 1]) << 8);
            if (keyMagenta && val == 0xF81Fu) {
                row[x] = 0u;
            } else {
                const std::uint32_t r = ((val >> 11) & 31u) * 255u / 31u;
                const std::uint32_t g = ((val >> 5) & 63u) * 255u / 63u;
                const std::uint32_t b = (val & 31u) * 255u / 31u;
                row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
    cairo_surface_mark_dirty(surf.get());
    out.surface = std::move(surf);
    return out;
}

std::string toUpperAscii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
    }
    return s;
}

Layout makeTextLayout(PangoContext* ctx, const std::string& text, double sizePx,
                      bool bold, double maxWidthPx = -1.0,
                      PangoAlignment align = PANGO_ALIGN_LEFT) {
    Layout layout(pango_layout_new(ctx), g_object_unref);
    FontDesc font(pango_font_description_from_string(
                      bold ? "DejaVu Sans Bold, Liberation Sans Bold, sans-serif Bold"
                           : "DejaVu Sans, Liberation Sans, sans-serif"),
                  pango_font_description_free);
    pango_font_description_set_absolute_size(font.get(), sizePx * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_text(layout.get(), text.c_str(), -1);
    pango_layout_set_alignment(layout.get(), align);
    if (maxWidthPx > 0.0) {
        pango_layout_set_width(layout.get(), int(std::lround(maxWidthPx * PANGO_SCALE)));
        pango_layout_set_wrap(layout.get(), PANGO_WRAP_WORD_CHAR);
    }
    return layout;
}

std::string buildGoldMarkup(const std::string& label, char shortcut) {
    std::string escaped;
    bool underlined = false;
    const char target = (shortcut >= 'a' && shortcut <= 'z') ? char(shortcut - 'a' + 'A') : shortcut;
    for (char ch : label) {
        std::string piece;
        if (ch == '&') piece = "&amp;";
        else if (ch == '<') piece = "&lt;";
        else if (ch == '>') piece = "&gt;";
        else piece.push_back(ch);

        if (!underlined && target != '\0' && std::toupper(static_cast<unsigned char>(ch)) == static_cast<unsigned char>(target)) {
            escaped += "<u>" + piece + "</u>";
            underlined = true;
        } else {
            escaped += piece;
        }
    }
    return escaped;
}

void drawSurfaceStretched(cairo_t* cr, cairo_surface_t* surf, double x, double y, double w, double h) {
    if (!surf) return;
    const int sw = cairo_image_surface_get_width(surf);
    const int sh = cairo_image_surface_get_height(surf);
    if (sw <= 0 || sh <= 0) return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, w / sw, h / sh);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

void drawBronzeBox(cairo_t* cr, double x, double y, double w, double h, double fillAlpha) {
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source_rgba(cr, 12.0 / 255.0, 15.0 / 255.0, 8.0 / 255.0, fillAlpha);
    cairo_fill(cr);

    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgb(cr, 182.0 / 255.0, 174.0 / 255.0, 146.0 / 255.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 48.0 / 255.0, 44.0 / 255.0, 34.0 / 255.0);
    cairo_rectangle(cr, x + 2.5, y + 2.5, w - 5.0, h - 5.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void drawFallbackBackdrop(cairo_t* cr, bool briefing) {
    cairo_pattern_t* pat = cairo_pattern_create_linear(0, 0, kStageW, kStageH);
    if (briefing) {
        cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.11, 0.13, 0.09);
        cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.05, 0.07, 0.04);
    } else {
        cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.13, 0.14, 0.09);
        cairo_pattern_add_color_stop_rgb(pat, 0.5, 0.09, 0.10, 0.07);
        cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.05, 0.06, 0.04);
    }
    cairo_set_source(cr, pat);
    cairo_rectangle(cr, 0, 0, kStageW, kStageH);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

void drawHeading(cairo_t* cr, PangoContext* ctx, const std::string& text, const RectD& r, double px = 17.0) {
    auto layout = makeTextLayout(ctx, text, px, false);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout.get(), &tw, &th);
    const double dx = r.x + (r.w - tw) * 0.5;
    const double dy = r.y + (r.h - th) * 0.5;

    cairo_save(cr);
    cairo_move_to(cr, dx + 1.5, dy + 1.5);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.90);
    pango_cairo_show_layout(cr, layout.get());

    cairo_move_to(cr, dx, dy);
    cairo_set_source_rgb(cr, 232.0 / 255.0, 228.0 / 255.0, 206.0 / 255.0);
    pango_cairo_show_layout(cr, layout.get());
    cairo_restore(cr);
}

void drawGoldStencilButton(cairo_t* cr, PangoContext* ctx, const RectD& r,
                           const std::string& label, char shortcut,
                           bool hovered, bool focused, bool enabled,
                           double basePx = 24.0, bool preserveCase = false) {
    const std::string shown = preserveCase ? label : toUpperAscii(label);
    const std::string markup = buildGoldMarkup(shown, shortcut);
    double px = basePx;
    Layout layout = makeTextLayout(ctx, shown, px, true);
    pango_layout_set_markup(layout.get(), markup.c_str(), -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout.get(), &tw, &th);
    while (tw > r.w + 10.0 && px > 12.0) {
        px -= 1.0;
        layout = makeTextLayout(ctx, shown, px, true);
        pango_layout_set_markup(layout.get(), markup.c_str(), -1);
        pango_layout_get_pixel_size(layout.get(), &tw, &th);
    }

    const double dx = r.x + (r.w - tw) * 0.5;
    const double dy = r.y + (r.h - th) * 0.5;

    cairo_save(cr);
    if (enabled && (hovered || focused)) {
        cairo_pattern_t* glow = cairo_pattern_create_radial(
            r.x + r.w * 0.5, r.y + r.h * 0.5, 4.0,
            r.x + r.w * 0.5, r.y + r.h * 0.5, std::max(r.w * 0.62, 24.0));
        cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.85, 0.68, 0.25, hovered ? 0.26 : 0.16);
        cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.85, 0.68, 0.25, 0.0);
        cairo_set_source(cr, glow);
        cairo_rectangle(cr, r.x - 8, r.y - 4, r.w + 16, r.h + 8);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);
    }

    for (const auto [ox, oy] : std::array<std::pair<double, double>, 5>{
             {{2.2, 2.2}, {-1.0, 0.0}, {1.0, 0.0}, {0.0, -1.0}, {0.0, 1.0}}}) {
        cairo_move_to(cr, dx + ox, dy + oy);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, enabled ? 0.92 : 0.60);
        pango_cairo_show_layout(cr, layout.get());
    }

    cairo_pattern_t* grad = cairo_pattern_create_linear(0, dy, 0, dy + th);
    if (!enabled) {
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.48, 0.48, 0.42);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.28, 0.28, 0.24);
    } else if (hovered || focused) {
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 1.0, 0.95, 0.68);
        cairo_pattern_add_color_stop_rgb(grad, 0.55, 0.95, 0.80, 0.42);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.72, 0.55, 0.24);
    } else {
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.90, 0.89, 0.80);
        cairo_pattern_add_color_stop_rgb(grad, 0.55, 0.76, 0.75, 0.65);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.50, 0.50, 0.42);
    }
    cairo_move_to(cr, dx, dy);
    cairo_set_source(cr, grad);
    pango_cairo_show_layout(cr, layout.get());
    cairo_pattern_destroy(grad);
    cairo_restore(cr);
}

void drawDiamondOverview(cairo_t* cr, cairo_surface_t* surf,
                         double x, double y, double w, double h,
                         bool drawFrameStroke) {
    const double cx = x + w * 0.5;
    const double cy = y + h * 0.5;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, cx, y);
    cairo_line_to(cr, x + w, cy);
    cairo_line_to(cr, cx, y + h);
    cairo_line_to(cr, x, cy);
    cairo_close_path(cr);

    if (drawFrameStroke) {
        cairo_save(cr);
        cairo_clip_preserve(cr);
    } else {
        cairo_clip(cr);
    }

    cairo_set_source_rgb(cr, 8.0 / 255.0, 10.0 / 255.0, 6.0 / 255.0);
    cairo_paint(cr);

    if (surf) {
        const double padW = w * 0.02;
        const double padH = h * 0.02;
        drawSurfaceStretched(cr, surf, x - padW, y - padH, w + 2.0 * padW, h + 2.0 * padH);
    }

    if (drawFrameStroke) {
        cairo_restore(cr);
        cairo_set_line_width(cr, 1.8);
        cairo_set_source_rgb(cr, 170.0 / 255.0, 162.0 / 255.0, 134.0 / 255.0);
        cairo_stroke(cr);
    } else {
        cairo_restore(cr);
    }
}

void tryLoadPckSlot(const std::filesystem::path& archive, const char* entry,
                    std::size_t frameIdx, bool keyMagenta, PckSurface& dst) {
    try {
        const auto bytes = assets::readSueEntry(archive, entry);
        if (frameIdx < assets::pckFrameCount(bytes)) {
            dst = pckFrameToSurface(assets::decodeUiPckFrame(bytes, frameIdx), keyMagenta);
        }
    } catch (...) {
    }
}

std::string sanitizeUtf8(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        int extra = -1;
        if ((c >> 5) == 0x06 && c >= 0xC2) extra = 1;
        else if ((c >> 4) == 0x0E) extra = 2;
        else if ((c >> 3) == 0x1E && c <= 0xF4) extra = 3;

        bool valid = (extra > 0 && i + std::size_t(extra) < input.size());
        if (valid) {
            for (int k = 1; k <= extra; ++k) {
                if ((static_cast<unsigned char>(input[i + std::size_t(k)]) & 0xC0u) != 0x80u) {
                    valid = false;
                    break;
                }
            }
        }
        if (valid) {
            out.append(input.substr(i, std::size_t(extra) + 1));
            i += std::size_t(extra) + 1;
        } else {
            // Convert legacy single-byte high character (Latin-1/CP1252) to valid UTF-8.
            out.push_back(static_cast<char>(0xC0u | (c >> 6)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
            ++i;
        }
    }
    return out;
}

bool isCreditsRoleHeading(std::string_view line) {
    bool hasAlpha = false;
    for (unsigned char c : line) {
        if (c >= 'a' && c <= 'z') return false;
        if (c >= 'A' && c <= 'Z') hasAlpha = true;
    }
    return hasAlpha && line.size() >= 3;
}

struct RatingsRow {
    bool isHeader{false};
    std::string name;
    bool completed{false};
    int score{0};
    std::string status;
};

std::vector<RatingsRow> buildRatingsRows(FrontEnd::RatingsMode mode,
                                         const std::vector<GoldCampaignEntry>& campaigns,
                                         const std::vector<GoldMissionEntry>& missions) {
    std::vector<RatingsRow> rows;
    if (mode == FrontEnd::RatingsMode::Campaigns) {
        for (const auto& c : campaigns) {
            int done = 0;
            int campScore = 0;
            for (const auto& m : c.missions) {
                if (m.completed) ++done;
                campScore += std::max(0, m.highScore);
            }
            rows.push_back(RatingsRow{
                true,
                c.name,
                !c.missions.empty() && done == int(c.missions.size()),
                campScore,
                std::to_string(done) + "/" + std::to_string(c.missions.size()) + " " + tr("Completed"),
            });
            for (const auto& m : c.missions) {
                rows.push_back(RatingsRow{
                    false,
                    "  " + m.name,
                    m.completed,
                    std::max(0, m.highScore),
                    m.completed ? tr("Completed") : std::string{},
                });
            }
        }
        return rows;
    }

    std::vector<std::size_t> order(missions.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (missions[a].highScore != missions[b].highScore) {
            return missions[a].highScore > missions[b].highScore;
        }
        if (missions[a].completed != missions[b].completed) {
            return missions[a].completed && !missions[b].completed;
        }
        return a < b;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const auto& m = missions[order[rank]];
        rows.push_back(RatingsRow{
            false,
            std::to_string(rank + 1) + ". " + m.name,
            m.completed,
            std::max(0, m.highScore),
            m.completed ? tr("Completed") : std::string{},
        });
    }
    return rows;
}

}  // namespace

struct GoldFrontEnd::Assets {
    FontMap fontMap{pango_cairo_font_map_new(), g_object_unref};
    FontCtx fontCtx{pango_font_map_create_context(fontMap.get()), g_object_unref};

    Surface mainBg = makeNullSurface();
    Surface briefingBg = makeNullSurface();
    PckSurface creditsBg;
    PckSurface ratingsDropdownBtn;
    PckSurface nbramkaFrame;
    std::array<PckSurface, 12> slotButtons;   // nbrif_bt[8..19]
    std::array<PckSurface, 6> scrollButtons;  // scrollbar_bt[0..5]
    std::array<PckSurface, 3> scrollTrack;    // scrollbar_sp[0..2]
    std::unordered_map<std::string, Surface> overviews;
    std::vector<std::string> creditsLines;
    bool loadedMenuSue{false};
};

GoldFrontEnd::GoldFrontEnd(SDL_Window* window, const std::filesystem::path& gameDir,
                           std::optional<int> fixedBgIndex)
    : window_(window), gameDir_(gameDir), assets_(std::make_unique<Assets>()) {
    loadAssets(fixedBgIndex);
    loadCreditsText();
    ensureFocusedValid();
}

GoldFrontEnd::~GoldFrontEnd() = default;

bool GoldFrontEnd::hasMenuAssets() const {
    return assets_ && assets_->loadedMenuSue && assets_->mainBg != nullptr;
}

bool GoldFrontEnd::hasCreditsArt() const {
    return assets_ && assets_->creditsBg.surface != nullptr;
}

std::size_t GoldFrontEnd::creditsLineCount() const {
    return assets_ ? assets_->creditsLines.size() : 0;
}

void GoldFrontEnd::loadAssets(std::optional<int> fixedBgIndex) {
    if (gameDir_.empty()) return;
    const auto menuSue = assets::resolveCaseInsensitivePath(gameDir_, "menu_common.sue");
    std::error_code ec;
    if (!std::filesystem::exists(menuSue, ec)) return;

    int chosen = fixedBgIndex.value_or(-1);
    if (chosen < 0 || chosen > 68) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 68);
        chosen = dist(rng);
    }

    auto loadBg = [&](const std::string& entry) -> Surface {
        try {
            const auto bytes = assets::readSueEntry(menuSue, entry);
            return mapImageToSurface(assets::decodeJpeg(bytes));
        } catch (...) {
            return makeNullSurface();
        }
    };

    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "mainbk%03d.jpg", chosen);
    assets_->mainBg = loadBg(nameBuf);
    if (!assets_->mainBg && chosen != 0) assets_->mainBg = loadBg("mainbk000.jpg");
    assets_->briefingBg = loadBg("brfbk000.jpg");

    tryLoadPckSlot(menuSue, "nbramka_bk.pck", 0, true, assets_->nbramkaFrame);
    tryLoadPckSlot(menuSue, "CREDITS.PCK", 0, false, assets_->creditsBg);
    tryLoadPckSlot(menuSue, "rg_bt.pck", 1, false, assets_->ratingsDropdownBtn);
    try {
        const auto nbrif = assets::readSueEntry(menuSue, "nbrif_bt.pck");
        const auto n = assets::pckFrameCount(nbrif);
        for (std::size_t i = 0; i < 12 && (8 + i) < n; ++i) {
            assets_->slotButtons[i] = pckFrameToSurface(assets::decodeUiPckFrame(nbrif, 8 + i), true);
        }
    } catch (...) {
    }
    try {
        const auto sbt = assets::readSueEntry(menuSue, "scrollbar_bt.pck");
        const auto n = assets::pckFrameCount(sbt);
        for (std::size_t i = 0; i < 6 && i < n; ++i) {
            assets_->scrollButtons[i] = pckFrameToSurface(assets::decodeUiPckFrame(sbt, i), true);
        }
    } catch (...) {
    }
    try {
        const auto ssp = assets::readSueEntry(menuSue, "scrollbar_sp.pck");
        const auto n = assets::pckFrameCount(ssp);
        for (std::size_t i = 0; i < 3 && i < n; ++i) {
            assets_->scrollTrack[i] = pckFrameToSurface(assets::decodeUiPckFrame(ssp, i), false);
        }
    } catch (...) {
    }

    assets_->loadedMenuSue = assets_->mainBg != nullptr || assets_->briefingBg != nullptr;
}

void GoldFrontEnd::loadCreditsText() {
    assets_->creditsLines.clear();
    if (!gameDir_.empty()) {
        std::vector<std::filesystem::path> archives;
        for (const char* sueName : {"lang.sue", "menu_common.sue"}) {
            const auto p = assets::resolveCaseInsensitivePath(gameDir_, sueName);
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) archives.push_back(p);
        }
        for (const auto& arch : archives) {
            try {
                const auto raw = assets::readSueEntry(arch, "credits.seq");
                if (raw.empty()) continue;
                const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
                std::size_t start = 0;
                while (start <= text.size()) {
                    const std::size_t nl = text.find('\n', start);
                    std::string line = (nl == std::string::npos)
                        ? text.substr(start) : text.substr(start, nl - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    assets_->creditsLines.push_back(sanitizeUtf8(line));
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
                break;
            } catch (...) {
            }
        }
    }

    if (assets_->creditsLines.empty()) {
        assets_->creditsLines = {
            "- FIREGLOW -",
            "",
            "EXECUTIVE PRODUCER",
            "Viktor Vinokurov",
            "",
            "PROJECT LEADER",
            "Vladimir V. Medeiko",
            "",
            "SENIOR PROGRAMMER",
            "Denis Konovalov",
            "",
        };
    } else {
        assets_->creditsLines.insert(assets_->creditsLines.end(), {"", "", "", ""});
    }

    assets_->creditsLines.push_back("- NATIVE LINUX / VULKAN PORT -");
    assets_->creditsLines.push_back("");
    assets_->creditsLines.push_back("Sudden Strike II Gold (C++20 / SDL3 / Vulkan)");
}

void GoldFrontEnd::setMissions(std::vector<GoldMissionEntry> missions) {
    missions_ = std::move(missions);
    std::vector<FrontEnd::Entry> base;
    base.reserve(missions_.size());
    for (const auto& m : missions_) {
        base.push_back(FrontEnd::Entry{m.name, m.path, m.description, m.tag});
    }
    fe_.setMissions(std::move(base));
    assets_->overviews.clear();
    if (fe_.screen() == FrontEnd::Screen::SingleMissions) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
    } else if (fe_.screen() == FrontEnd::Screen::Ratings) {
        const int maxOff = std::max(0, int(ratingsRowCount()) - kVisibleRatingsRows);
        ratingsScroll_ = std::clamp(ratingsScroll_, 0, maxOff);
    }
    dirty_ = true;
}

void GoldFrontEnd::setCampaigns(std::vector<GoldCampaignEntry> campaigns) {
    campaigns_ = std::move(campaigns);
    std::vector<FrontEnd::CampaignEntry> base;
    base.reserve(campaigns_.size());
    for (const auto& c : campaigns_) {
        FrontEnd::CampaignEntry ce;
        ce.name = c.name;
        ce.missions.reserve(c.missions.size());
        for (const auto& m : c.missions) {
            ce.missions.push_back(FrontEnd::CampaignMissionEntry{
                m.code, m.name, m.path, m.easyPath, m.description, m.tag
            });
        }
        base.push_back(std::move(ce));
    }
    fe_.setCampaigns(std::move(base));
    assets_->overviews.clear();
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
    } else if (fe_.screen() == FrontEnd::Screen::Ratings) {
        const int maxOff = std::max(0, int(ratingsRowCount()) - kVisibleRatingsRows);
        ratingsScroll_ = std::clamp(ratingsScroll_, 0, maxOff);
    }
    if (fe_.screen() == FrontEnd::Screen::Campaigns) ensureFocusedValid();
    dirty_ = true;
}

void GoldFrontEnd::setDifficulty(int difficulty) {
    fe_.setDifficulty(difficulty);
    dirty_ = true;
}

std::size_t GoldFrontEnd::activeListCount() const {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        const std::size_t ci = fe_.selectedCampaign();
        return ci < campaigns_.size() ? campaigns_[ci].missions.size() : 0;
    }
    return missions_.size();
}

std::size_t GoldFrontEnd::activeListSelected() const {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        return fe_.selectedCampaignMission();
    }
    return fe_.selected();
}

void GoldFrontEnd::selectActiveRow(std::size_t index) {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        fe_.selectCampaignMission(index);
    } else {
        fe_.select(index);
    }
}

const char* GoldFrontEnd::activeRowPrefix() const {
    return fe_.screen() == FrontEnd::Screen::CampaignMissions ? "cmission:" : "mission:";
}

std::size_t GoldFrontEnd::previewCampaignIndex() const {
    if (campaigns_.empty()) return 0;
    std::size_t idx = 0;
    if (parseIndexAfter(hoveredId_, "campaign:", idx) && idx < campaigns_.size()) return idx;
    if (parseIndexAfter(focusedId_, "campaign:", idx) && idx < campaigns_.size()) return idx;
    return fe_.selectedCampaign() < campaigns_.size() ? fe_.selectedCampaign() : 0;
}

std::size_t GoldFrontEnd::ratingsRowCount() const {
    if (fe_.ratingsMode() == FrontEnd::RatingsMode::Campaigns) {
        std::size_t total = 0;
        for (const auto& c : campaigns_) {
            total += 1 + c.missions.size();
        }
        return total;
    }
    return missions_.size();
}

GoldStageRect GoldFrontEnd::computeStage(int pixelW, int pixelH) {
    const int pw = std::max(1, pixelW);
    const int ph = std::max(1, pixelH);
    const double scale = std::min(double(pw) / kStageW, double(ph) / kStageH);
    const double w = kStageW * scale;
    const double h = kStageH * scale;
    return GoldStageRect{(pw - w) * 0.5, (ph - h) * 0.5, w, h, scale};
}

GoldPoint GoldFrontEnd::windowToStage(double windowX, double windowY,
                                      int winW, int winH, int pixelW, int pixelH) {
    if (winW <= 0 || winH <= 0 || pixelW <= 0 || pixelH <= 0) return {};
    const double px = windowX * (double(pixelW) / winW);
    const double py = windowY * (double(pixelH) / winH);
    const GoldStageRect st = computeStage(pixelW, pixelH);
    if (st.scale <= 0.0) return {};
    const double sx = (px - st.x) / st.scale;
    const double sy = (py - st.y) / st.scale;
    const bool inside = sx >= 0.0 && sx < kStageW && sy >= 0.0 && sy < kStageH;
    return GoldPoint{sx, sy, inside};
}

void GoldFrontEnd::ensureFocusedValid() {
    switch (fe_.screen()) {
        case FrontEnd::Screen::MainMenu:
            if (focusedId_.empty()) focusedId_ = "single";
            break;
        case FrontEnd::Screen::SinglePlayer:
            focusedId_ = "missions";
            break;
        case FrontEnd::Screen::Campaigns:
            focusedId_ = !campaigns_.empty()
                ? ("campaign:" + std::to_string(fe_.selectedCampaign()))
                : "cancel";
            break;
        case FrontEnd::Screen::SingleMissions:
        case FrontEnd::Screen::CampaignMissions:
        case FrontEnd::Screen::Briefing:
            focusedId_ = "start";
            break;
        case FrontEnd::Screen::Ratings:
        case FrontEnd::Screen::Credits:
            focusedId_ = "cancel";
            break;
        case FrontEnd::Screen::Launch:
        case FrontEnd::Screen::Quit:
            focusedId_.clear();
            break;
    }
}

void GoldFrontEnd::syncNoticeFromState() {
    if (const std::string n = fe_.takeNotice(); !n.empty()) {
        showNotice(n);
    }
}

void GoldFrontEnd::showNotice(std::string text) {
    noticeText_ = std::move(text);
    noticeRemaining_ = std::chrono::seconds(30);
    dirty_ = true;
}

void GoldFrontEnd::onScreenChanged(FrontEnd::Screen previous) {
    if (fe_.screen() != previous) {
        hoveredId_.clear();
        draggingThumb_ = false;
        briefScroll_ = 0.0;
        ratingsScroll_ = 0;
        creditsScroll_ = 0.0;
        creditsContentHeight_ = 0.0;
        creditsPaused_ = false;
        noticeText_.clear();
        noticeRemaining_ = {};
        ensureFocusedValid();
        if (fe_.screen() == FrontEnd::Screen::SingleMissions) {
            ensureSelectionVisible();
            if (const auto sel = fe_.selected(); sel < missions_.size()) {
                ensureOverviewLoaded(missions_[sel].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::Campaigns) {
            const std::size_t ci = previewCampaignIndex();
            if (ci < campaigns_.size() && !campaigns_[ci].missions.empty()) {
                std::size_t mIdx = 0;
                for (std::size_t i = 0; i < campaigns_[ci].missions.size(); ++i) {
                    if (!campaigns_[ci].missions[i].completed) {
                        mIdx = i;
                        break;
                    }
                }
                ensureOverviewLoaded(campaigns_[ci].missions[mIdx].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
            scrollOffset_ = 0;
            ensureSelectionVisible();
            const std::size_t ci = fe_.selectedCampaign();
            const std::size_t mi = fe_.selectedCampaignMission();
            if (ci < campaigns_.size() && mi < campaigns_[ci].missions.size()) {
                ensureOverviewLoaded(campaigns_[ci].missions[mi].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::Briefing) {
            ensureOverviewLoaded(fe_.launchPath());
        }
        dirty_ = true;
    } else if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        dirty_ = true;
    } else if (fe_.screen() == FrontEnd::Screen::Ratings) {
        ratingsScroll_ = 0;
        dirty_ = true;
    }
    syncNoticeFromState();
}

void GoldFrontEnd::ensureSelectionVisible() {
    const std::size_t count = activeListCount();
    if (count == 0) {
        scrollOffset_ = 0;
        return;
    }
    const int sel = int(activeListSelected());
    const int maxOff = std::max(0, int(count) - kVisibleMissionRows);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxOff);
    if (sel < scrollOffset_) scrollOffset_ = sel;
    else if (sel >= scrollOffset_ + kVisibleMissionRows) {
        scrollOffset_ = std::min(maxOff, sel - kVisibleMissionRows + 1);
    }
}

void GoldFrontEnd::scrollBy(int deltaRows) {
    const std::size_t count = activeListCount();
    const int maxOff = std::max(0, int(count) - kVisibleMissionRows);
    const int next = std::clamp(scrollOffset_ + deltaRows, 0, maxOff);
    if (next != scrollOffset_) {
        scrollOffset_ = next;
        dirty_ = true;
    }
}

void GoldFrontEnd::scrollRatingsBy(int deltaRows) {
    const std::size_t count = ratingsRowCount();
    const int maxOff = std::max(0, int(count) - kVisibleRatingsRows);
    const int next = std::clamp(ratingsScroll_ + deltaRows, 0, maxOff);
    if (next != ratingsScroll_) {
        ratingsScroll_ = next;
        dirty_ = true;
    }
}

void GoldFrontEnd::ensureOverviewLoaded(const std::string& path) {
    if (path.empty() || assets_->overviews.contains(path)) return;
    try {
        auto img = assets::readMissionOverview(path);
        assets_->overviews.emplace(path, mapImageToSurface(img));
    } catch (...) {
        assets_->overviews.emplace(path, makeNullSurface());
    }
}

std::string GoldFrontEnd::hitTestStage(double sx, double sy) const {
    auto checkButtons = [&](const auto& specs) -> std::string {
        for (const auto& b : specs) {
            if (b.rect.contains(sx, sy)) return b.id;
        }
        return {};
    };

    switch (fe_.screen()) {
        case FrontEnd::Screen::MainMenu:
            return checkButtons(kMainButtons);
        case FrontEnd::Screen::SinglePlayer:
            return checkButtons(kSinglePlayerButtons);
        case FrontEnd::Screen::Campaigns: {
            for (std::size_t i = 0; i < campaigns_.size(); ++i) {
                if (campaignRectFor(i, campaigns_.size()).contains(sx, sy)) {
                    return "campaign:" + std::to_string(i);
                }
            }
            return checkButtons(kCampaignFooterButtons);
        }
        case FrontEnd::Screen::SingleMissions:
        case FrontEnd::Screen::CampaignMissions: {
            if (fe_.screen() == FrontEnd::Screen::CampaignMissions &&
                kDifficultyButtonRect.contains(sx, sy)) {
                return "difficulty";
            }
            if (const auto id = checkButtons(kSingleMissionButtons); !id.empty()) return id;
            const std::size_t count = activeListCount();
            const RectD listRect{kListX + 4, kListY + 6, kListW - 8, kVisibleMissionRows * kRowH};
            if (listRect.contains(sx, sy) && count > 0) {
                const int row = int((sy - (kListY + 6)) / kRowH);
                const std::size_t idx = std::size_t(scrollOffset_ + row);
                if (idx < count) return std::string(activeRowPrefix()) + std::to_string(idx);
            }
            if (RectD{kScrollX, kScrollY, kScrollW, kScrollArrowH}.contains(sx, sy)) return "scroll_up";
            if (RectD{kScrollX, kScrollY + kScrollH - kScrollArrowH, kScrollW, kScrollArrowH}.contains(sx, sy))
                return "scroll_down";
            if (RectD{kScrollX, kTrackY, kScrollW, kTrackH}.contains(sx, sy)) {
                if (int(count) > kVisibleMissionRows) {
                    const double thumbY = thumbTopFor(scrollOffset_, count);
                    if (sy >= thumbY && sy < thumbY + kThumbH) return "scroll_thumb";
                    return sy < thumbY ? "scroll_page_up" : "scroll_page_down";
                }
            }
            return {};
        }
        case FrontEnd::Screen::Briefing: {
            if (const auto id = checkButtons(kBriefingButtons); !id.empty()) return id;
            for (std::size_t s = 0; s < 4; ++s) {
                const auto& slot = assets_->slotButtons[s * 3];
                if (slot.surface && RectD{double(slot.x), double(slot.y), double(slot.w), double(slot.h)}.contains(sx, sy)) {
                    return "slot_" + std::to_string(s + 1);
                }
            }
            return {};
        }
        case FrontEnd::Screen::Ratings: {
            if (kRatingsCampaignsTabRect.contains(sx, sy)) return "ratings_campaigns";
            if (kRatingsMissionsTabRect.contains(sx, sy)) return "ratings_missions";
            if (kRatingsCancelRect.contains(sx, sy)) return "cancel";
            const std::size_t count = ratingsRowCount();
            if (RectD{kRatingsScrollX, kRatingsScrollY, kRatingsScrollW, kScrollArrowH}.contains(sx, sy)) {
                return "ratings_scroll_up";
            }
            if (RectD{kRatingsScrollX, kRatingsScrollY + kRatingsScrollH - kScrollArrowH, kRatingsScrollW, kScrollArrowH}.contains(sx, sy)) {
                return "ratings_scroll_down";
            }
            if (RectD{kRatingsScrollX, kRatingsTrackY, kRatingsScrollW, kRatingsTrackH}.contains(sx, sy) &&
                int(count) > kVisibleRatingsRows) {
                const double thumbY = thumbTopFor(ratingsScroll_, count, kVisibleRatingsRows, kRatingsTrackY, kRatingsTrackH);
                if (sy >= thumbY && sy < thumbY + kThumbH) return "ratings_scroll_thumb";
                return sy < thumbY ? "ratings_scroll_page_up" : "ratings_scroll_page_down";
            }
            return {};
        }
        case FrontEnd::Screen::Credits: {
            if (kCreditsCancelRect.contains(sx, sy)) return "cancel";
            if (kCreditsFrameRect.contains(sx, sy)) return "credits_toggle_pause";
            return {};
        }
        case FrontEnd::Screen::Launch:
        case FrontEnd::Screen::Quit:
            return {};
    }
    return {};
}

void GoldFrontEnd::activateId(const std::string& id) {
    if (id.empty()) return;
    const auto prev = fe_.screen();

    if (id == "scroll_up") {
        scrollBy(-1);
        return;
    }
    if (id == "scroll_down") {
        scrollBy(1);
        return;
    }
    if (id == "scroll_page_up") {
        scrollBy(-kVisibleMissionRows);
        return;
    }
    if (id == "scroll_page_down") {
        scrollBy(kVisibleMissionRows);
        return;
    }
    if (id == "ratings_scroll_up") {
        scrollRatingsBy(-1);
        return;
    }
    if (id == "ratings_scroll_down") {
        scrollRatingsBy(1);
        return;
    }
    if (id == "ratings_scroll_page_up") {
        scrollRatingsBy(-kVisibleRatingsRows);
        return;
    }
    if (id == "ratings_scroll_page_down") {
        scrollRatingsBy(kVisibleRatingsRows);
        return;
    }
    if (id == "credits_toggle_pause") {
        creditsPaused_ = !creditsPaused_;
        dirty_ = true;
        return;
    }
    if (id == "profile_new" || id == "profile_rename" || id == "profile_delete" ||
        id == "brief_restart" || id == "brief_pause" || id.rfind("slot_", 0) == 0) {
        if (id == "profile_new" || id == "profile_rename" || id == "profile_delete") {
            showNotice(tr("Not implemented yet."));
        }
        return;
    }

    fe_.activate(id);
    onScreenChanged(prev);
}

void GoldFrontEnd::handleKey(SDL_Keycode key) {
    const auto prev = fe_.screen();

    if (key == SDLK_ESCAPE) {
        fe_.back();
        onScreenChanged(prev);
        return;
    }

    if (key >= SDLK_A && key <= SDLK_Z) {
        if (const std::string id = fe_.shortcutId(char('a' + (key - SDLK_A))); !id.empty()) {
            fe_.activate(id);
            onScreenChanged(prev);
            return;
        }
    }

    auto cycleFocus = [&](const std::vector<std::string>& ids, int step) {
        if (ids.empty()) return;
        auto it = std::ranges::find(ids, focusedId_);
        const int idx = it == ids.end() ? 0 : int(it - ids.begin());
        const int next = (idx + step + int(ids.size())) % int(ids.size());
        focusedId_ = ids[next];
        if (std::size_t ci = 0; parseIndexAfter(focusedId_, "campaign:", ci)) {
            fe_.selectCampaign(ci);
        }
        dirty_ = true;
    };

    switch (fe_.screen()) {
        case FrontEnd::Screen::MainMenu:
            if (key == SDLK_UP || key == SDLK_LEFT) cycleFocus({"single", "multi", "options", "ratings", "replay", "credits", "quit"}, -1);
            else if (key == SDLK_DOWN || key == SDLK_RIGHT) cycleFocus({"single", "multi", "options", "ratings", "replay", "credits", "quit"}, 1);
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) activateId(focusedId_);
            break;
        case FrontEnd::Screen::SinglePlayer:
            if (key == SDLK_UP || key == SDLK_LEFT) cycleFocus({"campaigns", "missions", "load", "cancel"}, -1);
            else if (key == SDLK_DOWN || key == SDLK_RIGHT) cycleFocus({"campaigns", "missions", "load", "cancel"}, 1);
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) activateId(focusedId_);
            break;
        case FrontEnd::Screen::Campaigns: {
            std::vector<std::string> order;
            order.reserve(campaigns_.size() + 2);
            for (std::size_t i = 0; i < campaigns_.size(); ++i) {
                order.push_back("campaign:" + std::to_string(i));
            }
            order.push_back("custom");
            order.push_back("cancel");
            if (key == SDLK_UP) cycleFocus(order, -1);
            else if (key == SDLK_DOWN) cycleFocus(order, 1);
            else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
                if (focusedId_ == "cancel" && !campaigns_.empty()) {
                    focusedId_ = "campaign:" + std::to_string(fe_.selectedCampaign());
                } else {
                    focusedId_ = "cancel";
                }
                dirty_ = true;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
                activateId(focusedId_);
            }
            break;
        }
        case FrontEnd::Screen::SingleMissions:
        case FrontEnd::Screen::CampaignMissions: {
            const std::size_t count = activeListCount();
            auto loadSelectedOverview = [&] {
                if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
                    const std::size_t ci = fe_.selectedCampaign();
                    const std::size_t mi = fe_.selectedCampaignMission();
                    if (ci < campaigns_.size() && mi < campaigns_[ci].missions.size()) {
                        ensureOverviewLoaded(campaigns_[ci].missions[mi].path);
                    }
                } else if (const auto mi = fe_.selected(); mi < missions_.size()) {
                    ensureOverviewLoaded(missions_[mi].path);
                }
            };
            if (count > 0) {
                if (key == SDLK_UP) {
                    const std::size_t sel = activeListSelected();
                    selectActiveRow(sel > 0 ? sel - 1 : 0);
                    ensureSelectionVisible();
                    loadSelectedOverview();
                    dirty_ = true;
                } else if (key == SDLK_DOWN) {
                    const std::size_t sel = activeListSelected();
                    selectActiveRow(std::min(count - 1, sel + 1));
                    ensureSelectionVisible();
                    loadSelectedOverview();
                    dirty_ = true;
                } else if (key == SDLK_PAGEUP) {
                    const std::size_t sel = activeListSelected();
                    selectActiveRow(sel > std::size_t(kVisibleMissionRows) ? sel - kVisibleMissionRows : 0);
                    ensureSelectionVisible();
                    loadSelectedOverview();
                    dirty_ = true;
                } else if (key == SDLK_PAGEDOWN) {
                    const std::size_t sel = activeListSelected();
                    selectActiveRow(std::min(count - 1, sel + std::size_t(kVisibleMissionRows)));
                    ensureSelectionVisible();
                    loadSelectedOverview();
                    dirty_ = true;
                } else if (key == SDLK_HOME) {
                    selectActiveRow(0);
                    ensureSelectionVisible();
                    loadSelectedOverview();
                    dirty_ = true;
                } else if (key == SDLK_END) {
                    selectActiveRow(count - 1);
                    ensureSelectionVisible();
                    loadSelectedOverview();
                    dirty_ = true;
                }
            }
            if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
                const int step = key == SDLK_LEFT ? -1 : 1;
                if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
                    cycleFocus({"start", "difficulty", "cancel"}, step);
                } else {
                    cycleFocus({"start", "cancel"}, step);
                }
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                if (focusedId_ == "cancel" || (fe_.screen() == FrontEnd::Screen::CampaignMissions && focusedId_ == "difficulty")) {
                    activateId(focusedId_);
                } else {
                    activateId("start");
                }
            }
            break;
        }
        case FrontEnd::Screen::Briefing:
            if (key == SDLK_UP) {
                briefScroll_ = std::max(0.0, briefScroll_ - 28.0);
                dirty_ = true;
            } else if (key == SDLK_DOWN) {
                briefScroll_ += 28.0;
                dirty_ = true;
            } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
                cycleFocus({"start", "cancel"}, 1);
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
                activateId(focusedId_.empty() ? "start" : focusedId_);
            }
            break;
        case FrontEnd::Screen::Ratings: {
            const std::size_t count = ratingsRowCount();
            const int maxOff = std::max(0, int(count) - kVisibleRatingsRows);
            if (key == SDLK_UP) scrollRatingsBy(-1);
            else if (key == SDLK_DOWN) scrollRatingsBy(1);
            else if (key == SDLK_PAGEUP) scrollRatingsBy(-kVisibleRatingsRows);
            else if (key == SDLK_PAGEDOWN) scrollRatingsBy(kVisibleRatingsRows);
            else if (key == SDLK_HOME) {
                if (ratingsScroll_ != 0) { ratingsScroll_ = 0; dirty_ = true; }
            } else if (key == SDLK_END) {
                if (ratingsScroll_ != maxOff) { ratingsScroll_ = maxOff; dirty_ = true; }
            } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
                cycleFocus({"ratings_campaigns", "ratings_missions", "cancel"}, key == SDLK_LEFT ? -1 : 1);
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
                activateId(focusedId_.empty() ? "cancel" : focusedId_);
            }
            break;
        }
        case FrontEnd::Screen::Credits: {
            const double minScroll = -kCreditsClipH + 40.0;
            const double maxScroll = creditsContentHeight_ > 0.0 ? creditsContentHeight_ : 1.0e6;
            if (key == SDLK_SPACE) {
                creditsPaused_ = !creditsPaused_;
                dirty_ = true;
            } else if (key == SDLK_UP) {
                creditsScroll_ = std::max(minScroll, creditsScroll_ - 28.0);
                dirty_ = true;
            } else if (key == SDLK_DOWN) {
                creditsScroll_ = std::min(maxScroll, creditsScroll_ + 28.0);
                dirty_ = true;
            } else if (key == SDLK_PAGEUP) {
                creditsScroll_ = std::max(minScroll, creditsScroll_ - 160.0);
                dirty_ = true;
            } else if (key == SDLK_PAGEDOWN) {
                creditsScroll_ = std::min(maxScroll, creditsScroll_ + 160.0);
                dirty_ = true;
            } else if (key == SDLK_HOME) {
                creditsScroll_ = 0.0;
                dirty_ = true;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                activateId("cancel");
            }
            break;
        }
        case FrontEnd::Screen::Launch:
        case FrontEnd::Screen::Quit:
            break;
    }
}

void GoldFrontEnd::handleMouseMove(double stageX, double stageY) {
    if (draggingThumb_) {
        if ((fe_.screen() == FrontEnd::Screen::SingleMissions ||
             fe_.screen() == FrontEnd::Screen::CampaignMissions)) {
            const std::size_t count = activeListCount();
            if (int(count) > kVisibleMissionRows) {
                const double travel = std::max(1.0, kTrackH - kThumbH);
                const int maxOff = int(count) - kVisibleMissionRows;
                const int target = int(std::lround(dragStartScroll_ + ((stageY - dragStartY_) / travel) * maxOff));
                const int clamped = std::clamp(target, 0, maxOff);
                if (clamped != scrollOffset_) {
                    scrollOffset_ = clamped;
                    dirty_ = true;
                }
                return;
            }
        } else if (fe_.screen() == FrontEnd::Screen::Ratings) {
            const std::size_t count = ratingsRowCount();
            if (int(count) > kVisibleRatingsRows) {
                const double travel = std::max(1.0, kRatingsTrackH - kThumbH);
                const int maxOff = int(count) - kVisibleRatingsRows;
                const int target = int(std::lround(dragStartScroll_ + ((stageY - dragStartY_) / travel) * maxOff));
                const int clamped = std::clamp(target, 0, maxOff);
                if (clamped != ratingsScroll_) {
                    ratingsScroll_ = clamped;
                    dirty_ = true;
                }
                return;
            }
        }
    }

    const std::string hit = hitTestStage(stageX, stageY);
    if (hit != hoveredId_) {
        hoveredId_ = hit;
        dirty_ = true;
    }
}

void GoldFrontEnd::handleMouseDown(double stageX, double stageY, std::uint8_t clicks) {
    const std::string hit = hitTestStage(stageX, stageY);
    if (hit.empty()) return;

    if (hit == "scroll_thumb" || hit == "ratings_scroll_thumb") {
        draggingThumb_ = true;
        dragStartY_ = stageY;
        dragStartScroll_ = (hit == "ratings_scroll_thumb") ? ratingsScroll_ : scrollOffset_;
        return;
    }

    if (fe_.screen() == FrontEnd::Screen::SingleMissions ||
        fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        std::size_t idx = 0;
        if (parseIndexAfter(hit, activeRowPrefix(), idx) && idx < activeListCount()) {
            const bool alreadySelected = (idx == activeListSelected());
            selectActiveRow(idx);
            if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
                const std::size_t ci = fe_.selectedCampaign();
                if (ci < campaigns_.size() && idx < campaigns_[ci].missions.size()) {
                    ensureOverviewLoaded(campaigns_[ci].missions[idx].path);
                }
            } else if (idx < missions_.size()) {
                ensureOverviewLoaded(missions_[idx].path);
            }
            focusedId_ = "start";
            dirty_ = true;
            if (clicks >= 2 || (clicks == 0 && alreadySelected)) {
                activateId(hit);
            }
            return;
        }
    }

    focusedId_ = hit;
    activateId(hit);
}

void GoldFrontEnd::handleMouseUp() {
    draggingThumb_ = false;
}

void GoldFrontEnd::handleMouseWheel(float dy) {
    if ((fe_.screen() == FrontEnd::Screen::SingleMissions ||
         fe_.screen() == FrontEnd::Screen::CampaignMissions) &&
        dy != 0.0f) {
        scrollBy(dy > 0 ? -3 : 3);
    } else if (fe_.screen() == FrontEnd::Screen::Briefing && dy != 0.0f) {
        briefScroll_ = std::max(0.0, briefScroll_ - double(dy) * 28.0);
        dirty_ = true;
    } else if (fe_.screen() == FrontEnd::Screen::Ratings && dy != 0.0f) {
        scrollRatingsBy(dy > 0 ? -3 : 3);
    } else if (fe_.screen() == FrontEnd::Screen::Credits && dy != 0.0f) {
        const double minScroll = -kCreditsClipH + 40.0;
        const double maxScroll = creditsContentHeight_ > 0.0 ? creditsContentHeight_ : 1.0e6;
        creditsScroll_ = std::clamp(creditsScroll_ - double(dy) * 32.0, minScroll, maxScroll);
        dirty_ = true;
    }
}

void GoldFrontEnd::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        window_ && event.window.windowID == SDL_GetWindowID(window_)) {
        dirty_ = true;
        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        handleKey(event.key.key);
        return;
    }

    auto toStage = [&](float wx, float wy) -> GoldPoint {
        if (!window_) return GoldPoint{wx, wy, true};
        int winW = 0, winH = 0;
        if (!SDL_GetWindowSize(window_, &winW, &winH)) return {};
        int pw = 0, ph = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &pw, &ph) || pw <= 0 || ph <= 0) {
            pw = lastPixelW_;
            ph = lastPixelH_;
        }
        return windowToStage(wx, wy, winW, winH, pw, ph);
    };

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const auto pt = toStage(event.motion.x, event.motion.y);
        if (pt.inside || draggingThumb_) handleMouseMove(pt.x, pt.y);
        else if (!hoveredId_.empty()) {
            hoveredId_.clear();
            dirty_ = true;
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        const auto pt = toStage(event.button.x, event.button.y);
        if (pt.inside) handleMouseDown(pt.x, pt.y, event.button.clicks);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
        handleMouseUp();
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        handleMouseWheel(event.wheel.y);
    }
}

void GoldFrontEnd::advance(std::chrono::nanoseconds dt) {
    if (noticeRemaining_.count() > 0) {
        if (dt >= noticeRemaining_) {
            noticeRemaining_ = {};
            noticeText_.clear();
            dirty_ = true;
        } else {
            noticeRemaining_ -= dt;
        }
    }
    if (fe_.screen() == FrontEnd::Screen::Credits && !creditsPaused_) {
        const double dtSec = std::clamp(std::chrono::duration<double>(dt).count(), 0.0, 0.25);
        if (dtSec > 0.0) {
            creditsScroll_ += dtSec * 34.0;
            if (creditsContentHeight_ > 0.0 && creditsScroll_ > creditsContentHeight_ + 48.0) {
                creditsScroll_ = -kCreditsClipH + 40.0;
            }
            dirty_ = true;
        }
    }
}

void GoldFrontEnd::renderToBuffer(int pixelW, int pixelH, std::vector<std::uint8_t>& outRgba) {
    const int pw = std::max(1, pixelW);
    const int ph = std::max(1, pixelH);

    Surface canvas(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph), cairo_surface_destroy);
    Context crGuard(cairo_create(canvas.get()), cairo_destroy);
    cairo_t* cr = crGuard.get();

    cairo_set_source_rgb(cr, 10.0 / 255.0, 12.0 / 255.0, 10.0 / 255.0);
    cairo_paint(cr);

    const GoldStageRect stage = computeStage(pw, ph);
    cairo_save(cr);
    cairo_translate(cr, stage.x, stage.y);
    cairo_scale(cr, stage.scale, stage.scale);
    cairo_rectangle(cr, 0, 0, kStageW, kStageH);
    cairo_clip(cr);

    auto* pangoCtx = assets_->fontCtx.get();
    const auto screen = fe_.screen();

    if (screen == FrontEnd::Screen::Credits) {
        if (assets_->creditsBg.surface) {
            drawSurfaceStretched(cr, assets_->creditsBg.surface.get(), 0, 0, kStageW, kStageH);
        } else if (assets_->mainBg) {
            drawSurfaceStretched(cr, assets_->mainBg.get(), 0, 0, kStageW, kStageH);
        } else {
            drawFallbackBackdrop(cr, false);
        }

        drawHeading(cr, pangoCtx, tr("Credits"), {200, 22, 400, 32}, 18.0);
        drawBronzeBox(cr, kCreditsFrameRect.x, kCreditsFrameRect.y,
                      kCreditsFrameRect.w, kCreditsFrameRect.h, 0.72);

        cairo_save(cr);
        cairo_rectangle(cr, kCreditsClipX, kCreditsClipY, kCreditsClipW, kCreditsClipH);
        cairo_clip(cr);

        double lineY = 0.0;
        for (const std::string& line : assets_->creditsLines) {
            if (line.empty()) {
                lineY += 14.0;
                continue;
            }
            const bool isBanner = line.size() >= 3 && line.front() == '-' && line.back() == '-';
            const bool isRole = !isBanner && isCreditsRoleHeading(line);
            const double fontPx = isBanner ? 19.0 : (isRole ? 14.5 : 15.5);
            const bool bold = isBanner || isRole;

            auto layout = makeTextLayout(pangoCtx, line, fontPx, bold, kCreditsClipW - 16.0, PANGO_ALIGN_CENTER);
            int tw = 0, th = 0;
            pango_layout_get_pixel_size(layout.get(), &tw, &th);
            const double drawY = kCreditsClipY + 32.0 + lineY - creditsScroll_;
            if (drawY + th >= kCreditsClipY - 8.0 && drawY <= kCreditsClipY + kCreditsClipH + 8.0) {
                cairo_move_to(cr, kCreditsClipX + 8.0 + 1.5, drawY + 1.5);
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.92);
                pango_cairo_show_layout(cr, layout.get());

                cairo_move_to(cr, kCreditsClipX + 8.0, drawY);
                if (isBanner) {
                    cairo_set_source_rgb(cr, 248.0 / 255.0, 218.0 / 255.0, 128.0 / 255.0);
                } else if (isRole) {
                    cairo_set_source_rgb(cr, 220.0 / 255.0, 208.0 / 255.0, 160.0 / 255.0);
                } else {
                    cairo_set_source_rgb(cr, 244.0 / 255.0, 246.0 / 255.0, 238.0 / 255.0);
                }
                pango_cairo_show_layout(cr, layout.get());
            }
            lineY += double(th) + (isBanner ? 8.0 : 5.0);
        }
        creditsContentHeight_ = lineY;
        cairo_restore(cr);

        const std::string hintText = creditsPaused_
            ? tr("Paused — Space to resume")
            : tr("Wheel / Up / Down to scroll • Space to pause");
        auto hintLayout = makeTextLayout(pangoCtx, hintText, 12.5, false);
        cairo_move_to(cr, kCreditsFrameRect.x, 520.0);
        cairo_set_source_rgba(cr, 220.0 / 255.0, 216.0 / 255.0, 190.0 / 255.0, 0.88);
        pango_cairo_show_layout(cr, hintLayout.get());

        drawGoldStencilButton(cr, pangoCtx, kCreditsCancelRect, tr("Cancel"), '\0',
                              hoveredId_ == "cancel", focusedId_ == "cancel", true, 24.0);
    } else if (screen == FrontEnd::Screen::Briefing) {
        if (assets_->briefingBg) drawSurfaceStretched(cr, assets_->briefingBg.get(), 0, 0, kStageW, kStageH);
        else drawFallbackBackdrop(cr, true);

        drawBronzeBox(cr, kBriefBoxX, kBriefBoxY, kBriefBoxW, kBriefBoxH, 0.82);
        {
            const double padX = 14.0, padY = 10.0;
            const double innerW = kBriefBoxW - 2.0 * padX;
            const double innerH = kBriefBoxH - 2.0 * padY;
            auto layout = makeTextLayout(pangoCtx, fe_.briefingText(), 13.5, false, innerW);
            int tw = 0, th = 0;
            pango_layout_get_pixel_size(layout.get(), &tw, &th);
            const double maxScroll = std::max(0.0, th - innerH);
            briefScroll_ = std::clamp(briefScroll_, 0.0, maxScroll);

            cairo_save(cr);
            cairo_rectangle(cr, kBriefBoxX + padX, kBriefBoxY + padY, innerW, innerH);
            cairo_clip(cr);
            cairo_move_to(cr, kBriefBoxX + padX, kBriefBoxY + padY - briefScroll_);
            cairo_set_source_rgb(cr, 245.0 / 255.0, 248.0 / 255.0, 240.0 / 255.0);
            pango_cairo_show_layout(cr, layout.get());
            cairo_restore(cr);
        }

        const std::string overviewPath = fe_.launchPath();
        ensureOverviewLoaded(overviewPath);
        cairo_surface_t* overview = nullptr;
        if (const auto it = assets_->overviews.find(overviewPath); it != assets_->overviews.end()) {
            overview = it->second.get();
        }
        drawDiamondOverview(cr, overview, 15.0, 187.0, 764.0, 380.0, assets_->nbramkaFrame.surface == nullptr);

        if (assets_->nbramkaFrame.surface) {
            cairo_save(cr);
            cairo_set_source_surface(cr, assets_->nbramkaFrame.surface.get(),
                                     assets_->nbramkaFrame.x, assets_->nbramkaFrame.y);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        for (std::size_t s = 0; s < 4; ++s) {
            const std::string slotId = "slot_" + std::to_string(s + 1);
            const std::size_t frameOff = s * 3 + (hoveredId_ == slotId ? 1 : 0);
            const auto& slot = assets_->slotButtons[frameOff];
            if (slot.surface) {
                cairo_set_source_surface(cr, slot.surface.get(), slot.x, slot.y);
                cairo_paint(cr);
            }
        }

        drawHeading(cr, pangoCtx, tr("Mission Briefing"), {565, 161, 192, 26}, 16.0);
        for (const auto& b : kBriefingButtons) {
            drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                  hoveredId_ == b.id, focusedId_ == b.id, b.enabled, 22.5);
        }
    } else {
        if (assets_->mainBg) drawSurfaceStretched(cr, assets_->mainBg.get(), 0, 0, kStageW, kStageH);
        else drawFallbackBackdrop(cr, false);

        auto drawGoldScrollbar = [&](double sx, double sy, double sw, double sh,
                                     int visibleRows, bool showThumb, std::size_t itemCount, int offset,
                                     bool upHov, bool downHov, bool thumbActive) {
            drawBronzeBox(cr, sx, sy, sw, sh, 0.90);
            const double trackY = sy + kScrollArrowH;
            const double trackH = sh - 2.0 * kScrollArrowH;
            if (assets_->scrollTrack[1].surface) {
                const double midStart = trackY + (assets_->scrollTrack[0].surface ? 10.0 : 0.0);
                const double midEnd = trackY + trackH - (assets_->scrollTrack[2].surface ? 10.0 : 0.0);
                if (assets_->scrollTrack[0].surface) {
                    drawSurfaceStretched(cr, assets_->scrollTrack[0].surface.get(), sx + 3.0, trackY, 11.0, 10.0);
                }
                for (double ty = midStart; ty < midEnd; ty += 10.0) {
                    drawSurfaceStretched(cr, assets_->scrollTrack[1].surface.get(),
                                         sx + 3.0, ty, 11.0, std::min(10.0, midEnd - ty));
                }
                if (assets_->scrollTrack[2].surface) {
                    drawSurfaceStretched(cr, assets_->scrollTrack[2].surface.get(),
                                         sx + 3.0, trackY + trackH - 10.0, 11.0, 10.0);
                }
            }
            auto drawSB = [&](std::size_t idx, double bx, double by, double bw, double bh, const char* fallbackSym) {
                if (assets_->scrollButtons[idx].surface) {
                    drawSurfaceStretched(cr, assets_->scrollButtons[idx].surface.get(), bx, by, bw, bh);
                } else {
                    drawBronzeBox(cr, bx, by, bw, bh, 0.95);
                    if (fallbackSym) {
                        auto sym = makeTextLayout(pangoCtx, fallbackSym, 10.0, true);
                        cairo_move_to(cr, bx + 4, by + 2);
                        cairo_set_source_rgb(cr, 210.0 / 255.0, 205.0 / 255.0, 180.0 / 255.0);
                        pango_cairo_show_layout(cr, sym.get());
                    }
                }
            };
            drawSB(upHov ? 1 : 0, sx + 3.0, sy + 2.0, 12.0, 14.0, "^");
            drawSB(downHov ? 3 : 2, sx + 3.0, sy + sh - kScrollArrowH + 2.0, 12.0, 14.0, "v");
            if (showThumb && int(itemCount) > visibleRows) {
                const double maxScroll = double(itemCount - std::size_t(visibleRows));
                const double thumbY = trackY + (trackH - kThumbH) * (double(offset) / maxScroll);
                drawSB(thumbActive ? 5 : 4, sx + 2.5, thumbY, 13.0, kThumbH, nullptr);
            }
        };

        auto renderMissionBrowser = [&](const std::string& heading, auto rowAt, std::size_t rowCount,
                                        std::size_t selectedRow, const char* rowPrefix, bool showDifficulty) {
            drawHeading(cr, pangoCtx, heading, {200, 38, 400, 28}, 17.0);
            drawHeading(cr, pangoCtx, tr("Missions:"), {130, 110, 150, 20}, 15.0);
            drawHeading(cr, pangoCtx, tr("Description:"), {520, 110, 144, 20}, 15.0);

            drawBronzeBox(cr, kListX, kListY, kListW, kListH, 0.88);
            cairo_save(cr);
            cairo_rectangle(cr, kListX + 4, kListY + 4, kListW - 8, kListH - 8);
            cairo_clip(cr);

            if (rowCount == 0) {
                auto emptyLbl = makeTextLayout(pangoCtx, tr("No missions installed."), 14.0, false);
                cairo_move_to(cr, kListX + 16, kListY + 18);
                cairo_set_source_rgb(cr, 180.0 / 255.0, 180.0 / 255.0, 165.0 / 255.0);
                pango_cairo_show_layout(cr, emptyLbl.get());
            } else {
                const int endIdx = std::min(int(rowCount), scrollOffset_ + kVisibleMissionRows);
                for (int idx = scrollOffset_; idx < endIdx; ++idx) {
                    const int row = idx - scrollOffset_;
                    const double ry = kListY + 6.0 + row * kRowH;
                    const bool isSelected = (std::size_t(idx) == selectedRow);
                    const bool isHovered = (hoveredId_ == std::string(rowPrefix) + std::to_string(idx));
                    if (isSelected) {
                        cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.38);
                        cairo_rectangle(cr, kListX + 5, ry, kListW - 10, kRowH - 1.0);
                        cairo_fill(cr);
                    } else if (isHovered) {
                        cairo_set_source_rgba(cr, 72.0 / 255.0, 96.0 / 255.0, 56.0 / 255.0, 0.22);
                        cairo_rectangle(cr, kListX + 5, ry, kListW - 10, kRowH - 1.0);
                        cairo_fill(cr);
                    }

                    const auto& m = rowAt(std::size_t(idx));
                    std::string label = m.name;
                    if (m.completed) label += "  [\xE2\x9C\x93]";
                    auto rowLayout = makeTextLayout(pangoCtx, label, 13.0, isSelected);
                    pango_layout_set_width(rowLayout.get(), int((kListW - 22.0) * PANGO_SCALE));
                    pango_layout_set_ellipsize(rowLayout.get(), PANGO_ELLIPSIZE_END);

                    cairo_move_to(cr, kListX + 12, ry + 2.0);
                    if (isSelected) cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
                    else if (isHovered) cairo_set_source_rgb(cr, 128.0 / 255.0, 224.0 / 255.0, 138.0 / 255.0);
                    else cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
                    pango_cairo_show_layout(cr, rowLayout.get());
                }
            }
            cairo_restore(cr);

            drawGoldScrollbar(kScrollX, kScrollY, kScrollW, kScrollH, kVisibleMissionRows,
                              true, rowCount, scrollOffset_,
                              hoveredId_ == "scroll_up", hoveredId_ == "scroll_down",
                              hoveredId_ == "scroll_thumb" || draggingThumb_);

            if (selectedRow < rowCount) {
                const auto& m = rowAt(selectedRow);
                cairo_save(cr);
                cairo_set_source_rgba(cr, 10.0 / 255.0, 14.0 / 255.0, 7.0 / 255.0, 0.42);
                cairo_rectangle(cr, 438, 136, 330, 186);
                cairo_fill(cr);
                cairo_restore(cr);
                {
                    auto desc = makeTextLayout(pangoCtx, m.description, 13.5, false, 316.0);
                    pango_layout_set_height(desc.get(), int(172.0 * PANGO_SCALE));
                    pango_layout_set_ellipsize(desc.get(), PANGO_ELLIPSIZE_END);
                    cairo_move_to(cr, 445, 142);
                    cairo_set_source_rgb(cr, 238.0 / 255.0, 240.0 / 255.0, 228.0 / 255.0);
                    pango_cairo_show_layout(cr, desc.get());
                }

                ensureOverviewLoaded(m.path);
                cairo_surface_t* overview = nullptr;
                if (const auto it = assets_->overviews.find(m.path); it != assets_->overviews.end()) {
                    overview = it->second.get();
                }
                drawDiamondOverview(cr, overview, 492.0, 338.0, 224.0, 117.0, true);

                std::string sizeStr = tr("Size:") + "  " + std::to_string(m.width) + "x" + std::to_string(m.height);
                if (m.scheme == 2) sizeStr += "  (Beach)";
                else if (m.width > 0) sizeStr += "  (Summer)";
                drawHeading(cr, pangoCtx, sizeStr, {464, 466, 280, 20}, 14.5);
                if (m.highScore > 0) {
                    drawHeading(cr, pangoCtx, "Score: " + std::to_string(m.highScore), {464, 486, 280, 18}, 12.5);
                }
            }

            if (showDifficulty) {
                const bool hov = (hoveredId_ == "difficulty");
                const bool foc = (focusedId_ == "difficulty");
                drawBronzeBox(cr, kDifficultyButtonRect.x, kDifficultyButtonRect.y,
                              kDifficultyButtonRect.w, kDifficultyButtonRect.h,
                              (hov || foc) ? 0.92 : 0.84);
                const std::string diffLabel = tr("Difficulty") + ": " + tr(difficultyKey(fe_.difficulty()));
                drawGoldStencilButton(cr, pangoCtx, kDifficultyButtonRect, diffLabel, 'D',
                                      hov, foc, true, 15.5, true);
            }

            for (const auto& b : kSingleMissionButtons) {
                const bool enabled = std::string_view(b.id) == "start" ? (rowCount > 0) : b.enabled;
                drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                      hoveredId_ == b.id, focusedId_ == b.id, enabled, 24.0);
            }
        };

        if (screen == FrontEnd::Screen::MainMenu) {
            drawHeading(cr, pangoCtx, tr("Main Menu"), {135, 38, 126, 26}, 17.0);
            for (const auto& b : kMainButtons) {
                drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                      hoveredId_ == b.id, focusedId_ == b.id, b.enabled, 24.0);
            }
            auto ver = makeTextLayout(pangoCtx, "ver 2.2", 11.5, false);
            cairo_move_to(cr, 725, 576);
            cairo_set_source_rgba(cr, 232.0 / 255.0, 228.0 / 255.0, 200.0 / 255.0, 0.85);
            pango_cairo_show_layout(cr, ver.get());
        } else if (screen == FrontEnd::Screen::SinglePlayer) {
            drawHeading(cr, pangoCtx, tr("Single Player"), {310, 28, 180, 34}, 17.0);

            auto nameLbl = makeTextLayout(pangoCtx, tr("Player name:"), 14.0, false);
            cairo_move_to(cr, 122, 72);
            cairo_set_source_rgb(cr, 236.0 / 255.0, 232.0 / 255.0, 212.0 / 255.0);
            pango_cairo_show_layout(cr, nameLbl.get());
            drawBronzeBox(cr, 66, 94, 238, 28, 0.86);
            auto prof = makeTextLayout(pangoCtx, "YourID", 13.5, true);
            cairo_move_to(cr, 74, 100);
            cairo_set_source_rgb(cr, 240.0 / 255.0, 240.0 / 255.0, 230.0 / 255.0);
            pango_cairo_show_layout(cr, prof.get());

            auto listLbl = makeTextLayout(pangoCtx, tr("Player list:"), 14.0, false);
            cairo_move_to(cr, 540, 72);
            cairo_set_source_rgb(cr, 236.0 / 255.0, 232.0 / 255.0, 212.0 / 255.0);
            pango_cairo_show_layout(cr, listLbl.get());
            drawBronzeBox(cr, 462, 95, 258, 362, 0.86);
            drawGoldScrollbar(720, 95, 18, 362, kVisibleMissionRows, false, 1, 0, false, false, false);
            auto item = makeTextLayout(pangoCtx, "YourID", 13.5, true);
            cairo_move_to(cr, 472, 104);
            cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
            pango_cairo_show_layout(cr, item.get());

            for (const auto& b : kSinglePlayerButtons) {
                const bool small = (std::string_view(b.id).rfind("profile_", 0) == 0);
                drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                      hoveredId_ == b.id, focusedId_ == b.id, b.enabled,
                                      small ? 17.5 : 24.0, small);
            }
        } else if (screen == FrontEnd::Screen::Campaigns) {
            drawHeading(cr, pangoCtx, tr("Campaigns"), {310, 28, 180, 34}, 17.0);

            if (campaigns_.empty()) {
                drawBronzeBox(cr, 43, 134, 330, 120, 0.88);
                auto emptyLbl = makeTextLayout(pangoCtx, tr("No campaigns installed."), 14.0, false);
                cairo_move_to(cr, 60, 182);
                cairo_set_source_rgb(cr, 180.0 / 255.0, 180.0 / 255.0, 165.0 / 255.0);
                pango_cairo_show_layout(cr, emptyLbl.get());
            } else {
                const std::size_t pIdx = previewCampaignIndex();
                for (std::size_t i = 0; i < campaigns_.size(); ++i) {
                    const std::string cid = "campaign:" + std::to_string(i);
                    const auto disp = campaignButtonDisplay(campaigns_[i].name);
                    const RectD r = campaignRectFor(i, campaigns_.size());
                    drawGoldStencilButton(cr, pangoCtx, r, disp.label, disp.shortcut,
                                          hoveredId_ == cid, focusedId_ == cid || i == pIdx, true, 24.0);
                }

                // Right-hand Campaign Dossier panel synced with hovered/focused/selected nation
                const auto& camp = campaigns_[pIdx];
                drawHeading(cr, pangoCtx, camp.name, {420, 74, 348, 26}, 15.5);
                drawBronzeBox(cr, 420, 104, 348, 384, 0.86);

                std::size_t done = 0;
                std::size_t firstUncompleted = 0;
                bool foundUncompleted = false;
                for (std::size_t mi = 0; mi < camp.missions.size(); ++mi) {
                    if (camp.missions[mi].completed) ++done;
                    else if (!foundUncompleted) {
                        firstUncompleted = mi;
                        foundUncompleted = true;
                    }
                }

                const std::string summaryStr =
                    tr("Missions:") + " " + std::to_string(camp.missions.size()) + "   |   " +
                    std::to_string(done) + "/" + std::to_string(camp.missions.size()) + " " + tr("Completed");
                auto sumLayout = makeTextLayout(pangoCtx, summaryStr, 12.5, true);
                cairo_move_to(cr, 434, 114);
                cairo_set_source_rgb(cr, 232.0 / 255.0, 224.0 / 255.0, 188.0 / 255.0);
                pango_cairo_show_layout(cr, sumLayout.get());

                const std::size_t maxPreviewRows = std::min<std::size_t>(camp.missions.size(), 8);
                for (std::size_t mi = 0; mi < maxPreviewRows; ++mi) {
                    const double ry = 138.0 + double(mi) * 19.0;
                    const bool highlightNext = (mi == firstUncompleted);
                    if (highlightNext) {
                        cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.34);
                        cairo_rectangle(cr, 430, ry, 328, 18.0);
                        cairo_fill(cr);
                    }
                    std::string mLabel = camp.missions[mi].name;
                    if (camp.missions[mi].completed) mLabel += "  [\xE2\x9C\x93]";
                    auto ml = makeTextLayout(pangoCtx, mLabel, 12.5, highlightNext);
                    pango_layout_set_width(ml.get(), int(316.0 * PANGO_SCALE));
                    pango_layout_set_ellipsize(ml.get(), PANGO_ELLIPSIZE_END);
                    cairo_move_to(cr, 434, ry + 1.0);
                    if (highlightNext) cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
                    else cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
                    pango_cairo_show_layout(cr, ml.get());
                }

                if (!camp.missions.empty()) {
                    const auto& pm = camp.missions[firstUncompleted];
                    ensureOverviewLoaded(pm.path);
                    cairo_surface_t* overview = nullptr;
                    if (const auto it = assets_->overviews.find(pm.path); it != assets_->overviews.end()) {
                        overview = it->second.get();
                    }
                    drawDiamondOverview(cr, overview, 482.0, 304.0, 224.0, 117.0, true);

                    std::string sizeStr = tr("Size:") + "  " + std::to_string(pm.width) + "x" + std::to_string(pm.height);
                    if (pm.scheme == 2) sizeStr += "  (Beach)";
                    else if (pm.width > 0) sizeStr += "  (Summer)";
                    drawHeading(cr, pangoCtx, sizeStr, {434, 430, 320, 20}, 13.5);
                }

                const std::string diffStr = tr("Difficulty") + ": " + tr(difficultyKey(fe_.difficulty()));
                drawHeading(cr, pangoCtx, diffStr, {434, 456, 320, 18}, 12.5);
            }

            for (const auto& b : kCampaignFooterButtons) {
                drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                      hoveredId_ == b.id, focusedId_ == b.id, b.enabled, 24.0);
            }
        } else if (screen == FrontEnd::Screen::CampaignMissions) {
            const std::size_t ci = fe_.selectedCampaign();
            static const std::vector<GoldCampaignMissionEntry> kEmptyCampaignMissions;
            const auto& campName = ci < campaigns_.size() ? campaigns_[ci].name : tr("Campaigns");
            const auto& cMissions = ci < campaigns_.size() ? campaigns_[ci].missions : kEmptyCampaignMissions;
            renderMissionBrowser(campName, [&](std::size_t i) -> const auto& { return cMissions[i]; },
                                 cMissions.size(), fe_.selectedCampaignMission(), "cmission:", true);
        } else if (screen == FrontEnd::Screen::SingleMissions) {
            renderMissionBrowser(tr("Single Player"), [&](std::size_t i) -> const auto& { return missions_[i]; },
                                 missions_.size(), fe_.selected(), "mission:", false);
        } else if (screen == FrontEnd::Screen::Ratings) {
            drawHeading(cr, pangoCtx, tr("Ratings"), {200, 28, 400, 32}, 18.0);

            auto modeLbl = makeTextLayout(pangoCtx, tr("Rating:"), 14.0, false);
            cairo_move_to(cr, kRatingsTableX, 71.0);
            cairo_set_source_rgb(cr, 236.0 / 255.0, 232.0 / 255.0, 212.0 / 255.0);
            pango_cairo_show_layout(cr, modeLbl.get());

            const auto mode = fe_.ratingsMode();
            const bool campActive = (mode == FrontEnd::RatingsMode::Campaigns);
            const bool missActive = (mode == FrontEnd::RatingsMode::SingleMissions);

            auto drawModeTab = [&](const RectD& r, const char* id, const std::string& label,
                                   char shortcut, bool active) {
                const bool hov = (hoveredId_ == id);
                const bool foc = (focusedId_ == id);
                drawBronzeBox(cr, r.x, r.y, r.w, r.h, active ? 0.94 : (hov ? 0.88 : 0.78));
                if (active) {
                    cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.34);
                    cairo_rectangle(cr, r.x + 3.0, r.y + 3.0, r.w - 6.0, r.h - 6.0);
                    cairo_fill(cr);
                }
                drawGoldStencilButton(cr, pangoCtx, r, label, shortcut, hov, foc || active, true, 15.0, true);
            };
            drawModeTab(kRatingsCampaignsTabRect, "ratings_campaigns", tr("Campaigns"), 'C', campActive);
            drawModeTab(kRatingsMissionsTabRect, "ratings_missions", tr("Single Missions"), 'S', missActive);

            int totalScore = 0;
            std::size_t completedCount = 0;
            std::size_t totalMissions = 0;
            if (campActive) {
                for (const auto& c : campaigns_) {
                    for (const auto& m : c.missions) {
                        ++totalMissions;
                        if (m.completed) ++completedCount;
                        totalScore += std::max(0, m.highScore);
                    }
                }
            } else {
                totalMissions = missions_.size();
                for (const auto& m : missions_) {
                    if (m.completed) ++completedCount;
                    totalScore += std::max(0, m.highScore);
                }
            }

            drawBronzeBox(cr, 438, 94, 319, 28, 0.86);
            const std::string totalStr = tr("Total Score") + ": " + std::to_string(totalScore) +
                "   (" + std::to_string(completedCount) + "/" + std::to_string(totalMissions) + ")";
            drawHeading(cr, pangoCtx, totalStr, {442, 94, 311, 28}, 13.5);

            drawBronzeBox(cr, kRatingsTableX, kRatingsTableY, kRatingsTableW, kRatingsTableH, 0.88);
            {
                auto colMission = makeTextLayout(pangoCtx, tr("Missions:"), 13.5, true);
                cairo_move_to(cr, kRatingsTableX + 14.0, kRatingsTableY + 8.0);
                cairo_set_source_rgb(cr, 232.0 / 255.0, 224.0 / 255.0, 188.0 / 255.0);
                pango_cairo_show_layout(cr, colMission.get());

                auto colStatus = makeTextLayout(pangoCtx, tr("Completed"), 13.5, true, 130.0, PANGO_ALIGN_CENTER);
                cairo_move_to(cr, 480.0, kRatingsTableY + 8.0);
                pango_cairo_show_layout(cr, colStatus.get());

                auto colScore = makeTextLayout(pangoCtx, tr("Score"), 13.5, true, 110.0, PANGO_ALIGN_RIGHT);
                cairo_move_to(cr, 614.0, kRatingsTableY + 8.0);
                pango_cairo_show_layout(cr, colScore.get());

                cairo_set_source_rgba(cr, 182.0 / 255.0, 174.0 / 255.0, 146.0 / 255.0, 0.55);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, kRatingsTableX + 8.0, kRatingsTableY + 30.0);
                cairo_line_to(cr, kRatingsTableX + kRatingsTableW - 8.0, kRatingsTableY + 30.0);
                cairo_stroke(cr);
            }

            const auto rows = buildRatingsRows(mode, campaigns_, missions_);
            const int maxOff = std::max(0, int(rows.size()) - kVisibleRatingsRows);
            ratingsScroll_ = std::clamp(ratingsScroll_, 0, maxOff);

            cairo_save(cr);
            cairo_rectangle(cr, kRatingsTableX + 6.0, kRatingsTableY + 32.0, kRatingsTableW - 12.0, kRatingsTableH - 38.0);
            cairo_clip(cr);

            if (rows.empty()) {
                const std::string emptyMsg = campActive ? tr("No campaigns installed.") : tr("No missions installed.");
                auto emptyLbl = makeTextLayout(pangoCtx, emptyMsg, 14.0, false);
                cairo_move_to(cr, kRatingsTableX + 18.0, kRatingsTableY + 48.0);
                cairo_set_source_rgb(cr, 180.0 / 255.0, 180.0 / 255.0, 165.0 / 255.0);
                pango_cairo_show_layout(cr, emptyLbl.get());
            } else {
                const int endIdx = std::min(int(rows.size()), ratingsScroll_ + kVisibleRatingsRows);
                for (int idx = ratingsScroll_; idx < endIdx; ++idx) {
                    const int visRow = idx - ratingsScroll_;
                    const double ry = kRatingsTableY + 36.0 + visRow * kRatingsRowH;
                    const auto& row = rows[std::size_t(idx)];
                    if (row.isHeader) {
                        cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.34);
                        cairo_rectangle(cr, kRatingsTableX + 8.0, ry, kRatingsTableW - 16.0, kRatingsRowH - 1.0);
                        cairo_fill(cr);
                    } else if ((visRow % 2) == 1) {
                        cairo_set_source_rgba(cr, 40.0 / 255.0, 48.0 / 255.0, 32.0 / 255.0, 0.24);
                        cairo_rectangle(cr, kRatingsTableX + 8.0, ry, kRatingsTableW - 16.0, kRatingsRowH - 1.0);
                        cairo_fill(cr);
                    }

                    auto nameLayout = makeTextLayout(pangoCtx, row.name, 13.0, row.isHeader);
                    pango_layout_set_width(nameLayout.get(), int(412.0 * PANGO_SCALE));
                    pango_layout_set_ellipsize(nameLayout.get(), PANGO_ELLIPSIZE_END);
                    cairo_move_to(cr, kRatingsTableX + 14.0, ry + 2.0);
                    if (row.isHeader) {
                        cairo_set_source_rgb(cr, 245.0 / 255.0, 224.0 / 255.0, 145.0 / 255.0);
                    } else if (row.completed) {
                        cairo_set_source_rgb(cr, 242.0 / 255.0, 244.0 / 255.0, 236.0 / 255.0);
                    } else {
                        cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
                    }
                    pango_cairo_show_layout(cr, nameLayout.get());

                    const std::string statusShown = row.isHeader
                        ? row.status
                        : (row.completed ? (tr("Completed") + " [\xE2\x9C\x93]") : std::string("-"));
                    auto statLayout = makeTextLayout(pangoCtx, statusShown, 12.5, row.isHeader, 130.0, PANGO_ALIGN_CENTER);
                    cairo_move_to(cr, 480.0, ry + 2.0);
                    if (row.completed || row.isHeader) {
                        cairo_set_source_rgb(cr, 212.0 / 255.0, 224.0 / 255.0, 178.0 / 255.0);
                    } else {
                        cairo_set_source_rgb(cr, 132.0 / 255.0, 136.0 / 255.0, 122.0 / 255.0);
                    }
                    pango_cairo_show_layout(cr, statLayout.get());

                    const std::string scoreShown = (row.score > 0 || row.isHeader)
                        ? std::to_string(row.score)
                        : std::string("-");
                    auto scoreLayout = makeTextLayout(pangoCtx, scoreShown, 13.0, row.isHeader || row.score > 0, 110.0, PANGO_ALIGN_RIGHT);
                    cairo_move_to(cr, 614.0, ry + 2.0);
                    if (row.score > 0) {
                        cairo_set_source_rgb(cr, 246.0 / 255.0, 216.0 / 255.0, 124.0 / 255.0);
                    } else {
                        cairo_set_source_rgb(cr, 140.0 / 255.0, 142.0 / 255.0, 130.0 / 255.0);
                    }
                    pango_cairo_show_layout(cr, scoreLayout.get());
                }
            }
            cairo_restore(cr);

            drawGoldScrollbar(kRatingsScrollX, kRatingsScrollY, kRatingsScrollW, kRatingsScrollH,
                              kVisibleRatingsRows, true, rows.size(), ratingsScroll_,
                              hoveredId_ == "ratings_scroll_up", hoveredId_ == "ratings_scroll_down",
                              hoveredId_ == "ratings_scroll_thumb" || draggingThumb_);

            if (totalScore == 0 && !rows.empty()) {
                auto noScore = makeTextLayout(pangoCtx, tr("No scores recorded yet."), 13.0, false);
                cairo_move_to(cr, kRatingsTableX, 520.0);
                cairo_set_source_rgba(cr, 220.0 / 255.0, 216.0 / 255.0, 190.0 / 255.0, 0.85);
                pango_cairo_show_layout(cr, noScore.get());
            }

            drawGoldStencilButton(cr, pangoCtx, kRatingsCancelRect, tr("Cancel"), '\0',
                                  hoveredId_ == "cancel", focusedId_ == "cancel", true, 24.0);
        }
    }

    if (!noticeText_.empty()) {
        drawBronzeBox(cr, 175, 558, 450, 32, 0.92);
        auto nLayout = makeTextLayout(pangoCtx, noticeText_, 13.5, true);
        int nw = 0, nh = 0;
        pango_layout_get_pixel_size(nLayout.get(), &nw, &nh);
        cairo_move_to(cr, 175 + (450 - nw) * 0.5, 558 + (32 - nh) * 0.5);
        cairo_set_source_rgb(cr, 250.0 / 255.0, 222.0 / 255.0, 148.0 / 255.0);
        pango_cairo_show_layout(cr, nLayout.get());
    }

    cairo_restore(cr);

    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));
    }
    cairo_surface_flush(canvas.get());
    outRgba.resize(std::size_t(pw) * ph * 4);
    if (!SDL_ConvertPixels(pw, ph, SDL_PIXELFORMAT_ARGB8888,
                           cairo_image_surface_get_data(canvas.get()),
                           cairo_image_surface_get_stride(canvas.get()),
                           SDL_PIXELFORMAT_RGBA32, outRgba.data(), pw * 4)) {
        throw std::runtime_error(SDL_GetError());
    }
}

void GoldFrontEnd::draw(VulkanWindow& renderer) {
    int pw = 0, ph = 0;
    if (window_ && SDL_GetWindowSizeInPixels(window_, &pw, &ph) && pw > 0 && ph > 0) {
        // Synchronized with physical pixel size even on frame N of a resize/DPI transition.
    } else {
        const auto [extW, extH] = renderer.extent();
        pw = int(extW);
        ph = int(extH);
    }
    pw = std::max(1, pw);
    ph = std::max(1, ph);

    if (!dirty_ && pw == lastPixelW_ && ph == lastPixelH_) return;
    lastPixelW_ = pw;
    lastPixelH_ = ph;
    dirty_ = false;
    renderToBuffer(pw, ph, rgba_);
    renderer.setOverlay(unsigned(pw), unsigned(ph), rgba_, OverlaySlot::panel);
}

}
