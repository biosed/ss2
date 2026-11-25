#include "ui/gold_frontend_internal.hpp"

#include "render/window.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ss2::gold_ui {

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

std::string toUpperAscii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
    }
    return s;
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
            out.push_back(static_cast<char>(0xC0u | (c >> 6)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
            ++i;
        }
    }
    return out;
}

Layout makeTextLayout(PangoContext* ctx, const std::string& text, double sizePx,
                      bool bold, double maxWidthPx, PangoAlignment align) {
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

        if (!underlined && target != '\0' &&
            std::toupper(static_cast<unsigned char>(ch)) == static_cast<unsigned char>(target)) {
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

namespace {
const RamkaFrames* g_activeRamka = nullptr;

bool ramkaComplete(const RamkaFrames& frames) {
    for (const auto& f : frames) {
        if (!f.surface || f.w <= 0 || f.h <= 0) return false;
    }
    return true;
}
} // namespace

RamkaScope::RamkaScope(const RamkaFrames* frames) : prev_(g_activeRamka) {
    g_activeRamka = (frames && ramkaComplete(*frames)) ? frames : nullptr;
}

RamkaScope::~RamkaScope() {
    g_activeRamka = prev_;
}

void drawRamkaBorder(cairo_t* cr, const RamkaFrames& f, double x, double y, double w, double h) {
    const double cw = f[0].w, ch = f[0].h;
    const double rw = f[2].w, bh = f[5].h;
    const double midW = std::max(0.0, w - cw - rw);
    const double midH = std::max(0.0, h - ch - bh);
    drawSurfaceStretched(cr, f[0].surface.get(), x, y, cw, ch);
    drawSurfaceStretched(cr, f[2].surface.get(), x + w - rw, y, rw, f[2].h);
    drawSurfaceStretched(cr, f[5].surface.get(), x, y + h - bh, f[5].w, bh);
    drawSurfaceStretched(cr, f[7].surface.get(), x + w - f[7].w, y + h - f[7].h, f[7].w, f[7].h);
    if (midW > 0.0) {
        drawSurfaceStretched(cr, f[1].surface.get(), x + cw, y, midW, f[1].h);
        drawSurfaceStretched(cr, f[6].surface.get(), x + cw, y + h - f[6].h, midW, f[6].h);
    }
    if (midH > 0.0) {
        drawSurfaceStretched(cr, f[3].surface.get(), x, y + ch, f[3].w, midH);
        drawSurfaceStretched(cr, f[4].surface.get(), x + w - f[4].w, y + ch, f[4].w, midH);
    }
}

bool drawPckAt(cairo_t* cr, const PckSurface& p, double x, double y) {
    if (!p.surface) return false;
    drawSurfaceStretched(cr, p.surface.get(), x, y, p.w, p.h);
    return true;
}

void drawBronzeBox(cairo_t* cr, double x, double y, double w, double h, double fillAlpha) {
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source_rgba(cr, 12.0 / 255.0, 15.0 / 255.0, 8.0 / 255.0, fillAlpha);
    cairo_fill(cr);

    if (g_activeRamka != nullptr) {
        const auto& f = *g_activeRamka;
        if (w >= double(f[0].w + f[2].w) && h >= double(f[0].h + f[5].h)) {
            drawRamkaBorder(cr, f, x, y, w, h);
            cairo_restore(cr);
            return;
        }
    }

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

void drawHeading(cairo_t* cr, PangoContext* ctx, const std::string& text, const RectD& r, double px) {
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
                           double basePx, bool preserveCase) {
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

}  // namespace ss2::gold_ui

namespace ss2 {
namespace {

using namespace gold_ui;

constexpr std::array<ButtonSpec, 7> kMainButtons{{
    {"single", "Single Player", 'S', {65, 111, 246, 35}, true},
    {"multi", "Multiplayer", 'M', {78, 178, 220, 35}, true},
    {"options", "Options", 'O', {115, 245, 141, 35}, true},
    {"ratings", "Ratings", 'R', {115, 312, 140, 35}, true},
    {"replay", "Replay", 'P', {115, 378, 140, 35}, true},
    {"credits", "Credits", 'C', {115, 445, 140, 35}, true},
    {"quit", "Quit", '\0', {146, 512, 83, 35}, true},
}};

}  // namespace

GoldFrontEnd::GoldFrontEnd(SDL_Window* window, const std::filesystem::path& gameDir,
                           std::optional<int> fixedBgIndex)
    : window_(window), gameDir_(gameDir), assets_(std::make_unique<Assets>()) {
    availableMods_ = gameDir_.empty()
        ? std::vector<assets::ModInfo>{}
        : assets::listInstalledMods(gameDir_, defaultModsDir());
    loadAssets(fixedBgIndex);
    loadCreditsText();
    ensureFocusedValid();
    syncMenuMusic();
}

void GoldFrontEnd::reloadModAssets() {
    if (!assets_) return;
    stopMenuMusic();
    assets_->overviews.clear();
    assets_->mainBg = makeNullSurface();
    assets_->briefingBg = makeNullSurface();
    assets_->menuArchives.clear();
    assets_->menuSuePath.clear();
    assets_->backwavBytes = 0;
    assets_->briefingMusicBytes = 0;
    assets_->loadedMenuSue = false;
    availableMods_ = gameDir_.empty()
        ? std::vector<assets::ModInfo>{}
        : assets::listInstalledMods(gameDir_, defaultModsDir());
    loadAssets(std::nullopt);
    loadCreditsText();
    syncMenuMusic();
    dirty_ = true;
}

GoldFrontEnd::~GoldFrontEnd() {
    stopMenuMusic();
    if (assets_) {
        assets_->menuCursor.restore();
    }
}

bool GoldFrontEnd::hasMenuAssets() const {
    return assets_ && assets_->loadedMenuSue && assets_->mainBg != nullptr;
}

bool GoldFrontEnd::hasMenuCursor() const {
    return assets_ && assets_->menuCursor.loaded();
}

bool GoldFrontEnd::hasRetailWidgets() const {
    if (!assets_) return false;
    for (const auto& f : assets_->ramka) {
        if (!f.surface) return false;
    }
    return assets_->diamondPlate.surface &&
           assets_->speedBar.surface && assets_->speedThumb[0].surface && assets_->speedThumb[1].surface &&
           assets_->rombik[0].surface && assets_->rombik[1].surface &&
           assets_->colorSwatch[0].surface && assets_->colorSwatch[22].surface;
}

bool GoldFrontEnd::hasMenuMusic() const {
    return assets_ && assets_->backwavBytes >= 4u && assets_->briefingMusicBytes >= 4u;
}

std::string_view GoldFrontEnd::activeMusicTrack() const {
    return assets_ ? std::string_view(assets_->activeMusicEntry) : std::string_view{};
}

bool GoldFrontEnd::hasCreditsArt() const {
    return assets_ && assets_->creditsBg.surface != nullptr;
}

std::size_t GoldFrontEnd::creditsLineCount() const {
    return assets_ ? assets_->creditsLines.size() : 0;
}

void GoldFrontEnd::loadAssets(std::optional<int> fixedBgIndex) {
    if (gameDir_.empty()) return;
    const auto menuArchives = assets::resolveMenuArchives(gameDir_);
    if (menuArchives.empty()) return;
    assets_->menuArchives = menuArchives;
    assets_->menuSuePath = menuArchives.front();
    assets_->backwavEntry = assets::resolveBackWavEntry(gameDir_);

    int chosen = fixedBgIndex.value_or(-1);
    if (chosen < 0 || chosen > 68) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 68);
        chosen = dist(rng);
    }

    auto loadBg = [&](const std::string& entry) -> Surface {
        try {
            if (const auto bytes = assets::tryReadSueEntryChain(menuArchives, entry)) {
                return mapImageToSurface(assets::decodeJpeg(*bytes));
            }
        } catch (...) {
        }
        return makeNullSurface();
    };
    auto tryLoadMenuPckSlot = [&](const char* entry, std::size_t frameIdx, bool keyMagenta, PckSurface& dst) {
        try {
            if (const auto bytes = assets::tryReadSueEntryChain(menuArchives, entry)) {
                if (frameIdx < assets::pckFrameCount(*bytes)) {
                    dst = pckFrameToSurface(assets::decodeUiPckFrame(*bytes, frameIdx), keyMagenta);
                }
            }
        } catch (...) {
        }
    };

    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "mainbk%03d.jpg", chosen);
    assets_->mainBg = loadBg(nameBuf);
    if (!assets_->mainBg && chosen != 0) assets_->mainBg = loadBg("mainbk000.jpg");
    assets_->briefingBg = loadBg("brfbk000.jpg");

    tryLoadMenuPckSlot("nbramka_bk.pck", 0, true, assets_->nbramkaFrame);
    tryLoadMenuPckSlot("sp3_rb.pck", 0, true, assets_->diamondPlate);
    tryLoadMenuPckSlot("CREDITS.PCK", 0, false, assets_->creditsBg);
    tryLoadMenuPckSlot("rg_bt.pck", 1, false, assets_->ratingsDropdownBtn);
    try {
        if (const auto rk = assets::tryReadSueEntryChain(menuArchives, "ramka.pck")) {
            const auto n = assets::pckFrameCount(*rk);
            for (std::size_t i = 0; i < assets_->ramka.size() && i < n; ++i) {
                assets_->ramka[i] = pckFrameToSurface(assets::decodeUiPckFrame(*rk, i), true);
            }
        }
    } catch (...) {
    }
    tryLoadMenuPckSlot("speedbar.pck", 0, true, assets_->speedBar);
    tryLoadMenuPckSlot("speed_bt.pck", 0, true, assets_->speedThumb[0]);
    tryLoadMenuPckSlot("speed_bt.pck", 1, true, assets_->speedThumb[1]);
    tryLoadMenuPckSlot("rombik.pck", 0, true, assets_->rombik[0]);
    tryLoadMenuPckSlot("rombik.pck", 10, true, assets_->rombik[1]);
    try {
        if (const auto cb = assets::tryReadSueEntryChain(menuArchives, "colors_bt.pck")) {
            const auto n = assets::pckFrameCount(*cb);
            for (std::size_t i = 0; i < assets_->colorSwatch.size() && i < n; ++i) {
                assets_->colorSwatch[i] = pckFrameToSurface(assets::decodeUiPckFrame(*cb, i), true);
            }
        }
    } catch (...) {
    }
    try {
        if (const auto nbrif = assets::tryReadSueEntryChain(menuArchives, "nbrif_bt.pck")) {
            const auto n = assets::pckFrameCount(*nbrif);
            for (std::size_t i = 0; i < 12 && (8 + i) < n; ++i) {
                assets_->slotButtons[i] = pckFrameToSurface(assets::decodeUiPckFrame(*nbrif, 8 + i), true);
            }
        }
    } catch (...) {
    }
    try {
        if (const auto sbt = assets::tryReadSueEntryChain(menuArchives, "scrollbar_bt.pck")) {
            const auto n = assets::pckFrameCount(*sbt);
            for (std::size_t i = 0; i < 6 && i < n; ++i) {
                assets_->scrollButtons[i] = pckFrameToSurface(assets::decodeUiPckFrame(*sbt, i), true);
            }
        }
    } catch (...) {
    }
    try {
        if (const auto ssp = assets::tryReadSueEntryChain(menuArchives, "scrollbar_sp.pck")) {
            const auto n = assets::pckFrameCount(*ssp);
            for (std::size_t i = 0; i < 3 && i < n; ++i) {
                assets_->scrollTrack[i] = pckFrameToSurface(assets::decodeUiPckFrame(*ssp, i), false);
            }
        }
    } catch (...) {
    }

    assets_->backwavBytes =
        assets::sueChainEntrySize(menuArchives, assets_->backwavEntry).value_or(0u) & ~std::size_t(3u);
    assets_->briefingMusicBytes =
        assets::sueChainEntrySize(menuArchives, "briefing").value_or(0u) & ~std::size_t(3u);

    assets_->loadedMenuSue = assets_->mainBg != nullptr || assets_->briefingBg != nullptr;
    if (window_ != nullptr) {
        assets_->menuCursor.load(gameDir_);
        assets_->menuCursor.apply();
    }
}

void GoldFrontEnd::loadCreditsText() {
    assets_->creditsLines.clear();
    if (!gameDir_.empty()) {
        const auto archives = assets::resolveMenuArchives(gameDir_);
        for (const auto& arch : archives) {
            try {
                const auto raw = assets::tryReadSueEntry(arch, "credits.seq");
                if (!raw || raw->empty()) continue;
                const std::string text(reinterpret_cast<const char*>(raw->data()), raw->size());
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

std::size_t GoldFrontEnd::activeListCount() const {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        const auto& active = activeCampaignList();
        const std::size_t ci = activeCampaignIndex();
        return ci < active.size() ? active[ci].missions.size() : 0;
    }
    if (fe_.screen() == FrontEnd::Screen::Replays) return replays_.size();
    if (fe_.screen() == FrontEnd::Screen::LoadGame) return saves_.size();
    if (fe_.screen() == FrontEnd::Screen::Multiplayer) return mpMissions_.size();
    return missions_.size();
}

std::size_t GoldFrontEnd::activeListSelected() const {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) return fe_.selectedCampaignMission();
    if (fe_.screen() == FrontEnd::Screen::Replays) return fe_.selectedReplay();
    if (fe_.screen() == FrontEnd::Screen::LoadGame) return fe_.selectedSave();
    if (fe_.screen() == FrontEnd::Screen::Multiplayer) return fe_.selectedMultiplayerMission();
    return fe_.selected();
}

void GoldFrontEnd::selectActiveRow(std::size_t index) {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) fe_.selectCampaignMission(index);
    else if (fe_.screen() == FrontEnd::Screen::Replays) fe_.selectReplay(index);
    else if (fe_.screen() == FrontEnd::Screen::LoadGame) fe_.selectSave(index);
    else if (fe_.screen() == FrontEnd::Screen::Multiplayer) fe_.selectMultiplayerMission(index);
    else fe_.select(index);
}

const char* GoldFrontEnd::activeRowPrefix() const {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) return "cmission:";
    if (fe_.screen() == FrontEnd::Screen::Replays) return "replay:";
    if (fe_.screen() == FrontEnd::Screen::LoadGame) return "save:";
    if (fe_.screen() == FrontEnd::Screen::Multiplayer) return "mpmap:";
    return "mission:";
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
            focusedId_ = (profileEditMode_ == ProfileEditMode::None) ? "missions" : "profile_name_field";
            break;
        case FrontEnd::Screen::Campaigns:
            focusedId_ = !campaigns_.empty()
                ? ("campaign:" + std::to_string(fe_.selectedCampaign()))
                : "cancel";
            break;
        case FrontEnd::Screen::CustomCampaigns:
            focusedId_ = !customCampaigns_.empty()
                ? ("ccampaign:" + std::to_string(fe_.selectedCustomCampaign()))
                : "cancel";
            break;
        case FrontEnd::Screen::SingleMissions:
        case FrontEnd::Screen::CampaignMissions:
        case FrontEnd::Screen::Briefing:
            focusedId_ = "start";
            break;
        case FrontEnd::Screen::Options:
            focusedId_ = kOptionRowIds[0];
            break;
        case FrontEnd::Screen::Replays:
            focusedId_ = replays_.empty() ? "cancel" : "replay_start";
            break;
        case FrontEnd::Screen::LoadGame:
            focusedId_ = saves_.empty() ? "cancel" : "load_start";
            break;
        case FrontEnd::Screen::Multiplayer:
            focusedId_ = (!fe_.mpIsHost() || !mpMissions_.empty()) ? "mp_start" : "cancel";
            break;
        case FrontEnd::Screen::Ratings:
        case FrontEnd::Screen::Credits:
            focusedId_ = "cancel";
            break;
        case FrontEnd::Screen::Launch:
        case FrontEnd::Screen::LaunchReplay:
        case FrontEnd::Screen::LaunchSave:
        case FrontEnd::Screen::LaunchMultiplayer:
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
        if (previous == FrontEnd::Screen::Briefing) {
            clearBriefingMedia();
        }
        if (assets_ && previous == FrontEnd::Screen::Multiplayer &&
            fe_.screen() != FrontEnd::Screen::Multiplayer && assets_->lanScannerActive) {
            assets_->lanScanner.stop();
            assets_->lanScannerActive = false;
            assets_->lastLanCount = 0u;
        }
        hoveredId_.clear();
        draggingThumb_ = false;
        briefScroll_ = 0.0;
        ratingsScroll_ = 0;
        creditsScroll_ = 0.0;
        creditsContentHeight_ = 0.0;
        creditsPaused_ = false;
        noticeText_.clear();
        noticeRemaining_ = {};
        if (mpEditField_ != MpEditField::None) {
            mpEditField_ = MpEditField::None;
            if (window_) SDL_StopTextInput(window_);
        }
        if (profileEditMode_ != ProfileEditMode::None) {
            profileEditMode_ = ProfileEditMode::None;
            profileNameDraft_ = settings_.activeProfile;
            if (window_) SDL_StopTextInput(window_);
        }
        ensureFocusedValid();
        if (fe_.screen() == FrontEnd::Screen::SinglePlayer) {
            ensureProfilesInitialized(settings_);
            profileNameDraft_ = settings_.activeProfile;
            const std::size_t activeIdx = findProfileIndex(settings_, settings_.activeProfile);
            const int maxOff = std::max(0, int(settings_.profiles.size()) - kVisibleProfileRows);
            profileScroll_ = std::clamp(profileScroll_, 0, maxOff);
            if (activeIdx < settings_.profiles.size()) {
                if (int(activeIdx) < profileScroll_) profileScroll_ = int(activeIdx);
                else if (int(activeIdx) >= profileScroll_ + kVisibleProfileRows) {
                    profileScroll_ = std::min(maxOff, int(activeIdx) - kVisibleProfileRows + 1);
                }
            }
        } else if (fe_.screen() == FrontEnd::Screen::SingleMissions) {
            ensureSelectionVisible();
            if (const auto sel = fe_.selected(); sel < missions_.size()) {
                ensureOverviewLoaded(missions_[sel].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::Campaigns ||
                   fe_.screen() == FrontEnd::Screen::CustomCampaigns) {
            const bool custom = (fe_.screen() == FrontEnd::Screen::CustomCampaigns);
            const auto& list = custom ? customCampaigns_ : campaigns_;
            const std::size_t ci = previewCampaignIndex(custom);
            if (ci < list.size() && !list[ci].missions.empty()) {
                std::size_t mIdx = 0;
                for (std::size_t i = 0; i < list[ci].missions.size(); ++i) {
                    if (!list[ci].missions[i].completed) {
                        mIdx = i;
                        break;
                    }
                }
                ensureOverviewLoaded(list[ci].missions[mIdx].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
            scrollOffset_ = 0;
            ensureSelectionVisible();
            const auto& active = activeCampaignList();
            const std::size_t ci = activeCampaignIndex();
            const std::size_t mi = fe_.selectedCampaignMission();
            if (ci < active.size() && mi < active[ci].missions.size()) {
                ensureOverviewLoaded(active[ci].missions[mi].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::Replays ||
                   fe_.screen() == FrontEnd::Screen::LoadGame) {
            scrollOffset_ = 0;
            ensureSelectionVisible();
        } else if (fe_.screen() == FrontEnd::Screen::Multiplayer) {
            mpPortDraft_ = std::to_string(fe_.mpPort());
            scrollOffset_ = 0;
            ensureSelectionVisible();
            if (const auto sel = fe_.selectedMultiplayerMission(); sel < mpMissions_.size()) {
                ensureOverviewLoaded(mpMissions_[sel].path);
            }
            if (assets_ && !fe_.mpIsHost() && !assets_->lanScannerActive) {
                assets_->lanScannerActive = assets_->lanScanner.start(kDefaultLanDiscoveryPort);
                assets_->lastLanCount = assets_->lanScanner.games().size();
            }
        } else if (fe_.screen() == FrontEnd::Screen::Options) {
            beginOptionsEdit();
        } else if (fe_.screen() == FrontEnd::Screen::Briefing) {
            ensureOverviewLoaded(fe_.launchPath());
            loadBriefingMedia(fe_.launchPath());
        }
        if (fe_.screen() != FrontEnd::Screen::Options) {
            settings_.difficulty = fe_.difficulty();
        }
        dirty_ = true;
    } else if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        settings_.difficulty = fe_.difficulty();
        dirty_ = true;
    } else if (fe_.screen() == FrontEnd::Screen::Ratings) {
        ratingsScroll_ = 0;
        dirty_ = true;
    } else if (fe_.screen() == FrontEnd::Screen::Multiplayer) {
        dirty_ = true;
    }
    syncNoticeFromState();
    syncMenuMusic();
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

void GoldFrontEnd::ensureOverviewLoaded(const std::string& path) {
    if (path.empty() || assets_->overviews.contains(path)) return;
    try {
        auto img = assets::readMissionOverview(path);
        assets_->overviews.emplace(path, mapImageToSurface(img));
    } catch (...) {
        assets_->overviews.emplace(path, makeNullSurface());
    }
}

void GoldFrontEnd::cycleFocus(const std::vector<std::string>& ids, int step) {
    if (ids.empty()) return;
    auto it = std::ranges::find(ids, focusedId_);
    const int idx = it == ids.end() ? 0 : int(it - ids.begin());
    const int next = (idx + step + int(ids.size())) % int(ids.size());
    focusedId_ = ids[next];
    if (std::size_t ci = 0; parseIndexAfter(focusedId_, "campaign:", ci)) {
        fe_.selectCampaign(ci);
    } else if (std::size_t cci = 0; parseIndexAfter(focusedId_, "ccampaign:", cci)) {
        fe_.selectCustomCampaign(cci);
    }
    dirty_ = true;
}

std::string GoldFrontEnd::hitTestActiveListAndScrollbar(double sx, double sy) const {
    const std::size_t count = activeListCount();
    const RectD listRect{kListX + 4, kListY + 6, kListW - 8, kVisibleMissionRows * kRowH};
    if (listRect.contains(sx, sy) && count > 0) {
        const int row = int((sy - (kListY + 6)) / kRowH);
        const std::size_t idx = std::size_t(scrollOffset_ + row);
        if (idx < count) return std::string(activeRowPrefix()) + std::to_string(idx);
    }
    if (RectD{kScrollX, kScrollY, kScrollW, kScrollArrowH}.contains(sx, sy)) return "scroll_up";
    if (RectD{kScrollX, kScrollY + kScrollH - kScrollArrowH, kScrollW, kScrollArrowH}.contains(sx, sy)) {
        return "scroll_down";
    }
    if (RectD{kScrollX, kTrackY, kScrollW, kTrackH}.contains(sx, sy)) {
        if (int(count) > kVisibleMissionRows) {
            const double thumbY = thumbTopFor(scrollOffset_, count);
            if (sy >= thumbY && sy < thumbY + kThumbH) return "scroll_thumb";
            return sy < thumbY ? "scroll_page_up" : "scroll_page_down";
        }
    }
    return {};
}

void GoldFrontEnd::handleActiveListNavigationKey(SDL_Keycode key) {
    const std::size_t count = activeListCount();
    if (count == 0) return;
    auto loadSelectedOverview = [&] {
        if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
            const auto& active = activeCampaignList();
            const std::size_t ci = activeCampaignIndex();
            const std::size_t mi = fe_.selectedCampaignMission();
            if (ci < active.size() && mi < active[ci].missions.size()) {
                ensureOverviewLoaded(active[ci].missions[mi].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::SingleMissions) {
            if (const auto mi = fe_.selected(); mi < missions_.size()) {
                ensureOverviewLoaded(missions_[mi].path);
            }
        } else if (fe_.screen() == FrontEnd::Screen::Multiplayer) {
            if (const auto mi = fe_.selectedMultiplayerMission(); mi < mpMissions_.size()) {
                ensureOverviewLoaded(mpMissions_[mi].path);
            }
        }
    };
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

void GoldFrontEnd::drawGoldScrollbar(const RenderEnv& env, double sx, double sy, double sw, double sh,
                                     int visibleRows, bool showThumb, std::size_t itemCount, int offset,
                                     bool upHov, bool downHov, bool thumbActive) const {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;
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
}

std::string GoldFrontEnd::hitTestStage(double sx, double sy) const {
    switch (fe_.screen()) {
        case FrontEnd::Screen::MainMenu:
            return hitTestButtons(kMainButtons, sx, sy);
        case FrontEnd::Screen::SinglePlayer:
            return hitTestSinglePlayer(sx, sy);
        case FrontEnd::Screen::Campaigns:
        case FrontEnd::Screen::CustomCampaigns:
            return hitTestCampaigns(sx, sy);
        case FrontEnd::Screen::SingleMissions:
        case FrontEnd::Screen::CampaignMissions:
            return hitTestMissionBrowser(sx, sy);
        case FrontEnd::Screen::Replays:
        case FrontEnd::Screen::LoadGame:
        case FrontEnd::Screen::Multiplayer:
            return hitTestMultiplayerOrBrowser(sx, sy);
        case FrontEnd::Screen::Options:
            return hitTestOptions(sx, sy);
        case FrontEnd::Screen::Briefing:
            return hitTestBriefing(sx, sy);
        case FrontEnd::Screen::Ratings:
            return hitTestRatings(sx, sy);
        case FrontEnd::Screen::Credits:
            return hitTestCredits(sx, sy);
        case FrontEnd::Screen::Launch:
        case FrontEnd::Screen::LaunchReplay:
        case FrontEnd::Screen::LaunchSave:
        case FrontEnd::Screen::LaunchMultiplayer:
        case FrontEnd::Screen::Quit:
            return {};
    }
    return {};
}

void GoldFrontEnd::activateId(const std::string& id) {
    if (id.empty()) return;
    const auto prev = fe_.screen();

    if (id.rfind("opt_dec:", 0) == 0) {
        const std::string optId = id.substr(8);
        focusedId_ = optId;
        adjustOption(optId, -1);
        return;
    }
    if (id.rfind("opt_inc:", 0) == 0) {
        const std::string optId = id.substr(8);
        focusedId_ = optId;
        adjustOption(optId, 1);
        return;
    }
    if (prev == FrontEnd::Screen::Options) {
        if (id == "opt_enhanced_gfx") {
            focusedId_ = id;
            adjustOption(id, 1);
            return;
        }
        if (id == "ok") commitOptionsEdit();
        else if (id == "cancel" || id == "back") cancelOptionsEdit();
    }

    if (id == "scroll_up") { scrollBy(-1); return; }
    if (id == "scroll_down") { scrollBy(1); return; }
    if (id == "scroll_page_up") { scrollBy(-kVisibleMissionRows); return; }
    if (id == "scroll_page_down") { scrollBy(kVisibleMissionRows); return; }
    if (id == "ratings_scroll_up") { scrollRatingsBy(-1); return; }
    if (id == "ratings_scroll_down") { scrollRatingsBy(1); return; }
    if (id == "ratings_scroll_page_up") { scrollRatingsBy(-kVisibleRatingsRows); return; }
    if (id == "ratings_scroll_page_down") { scrollRatingsBy(kVisibleRatingsRows); return; }
    if (id == "credits_toggle_pause") {
        creditsPaused_ = !creditsPaused_;
        dirty_ = true;
        return;
    }
    if (id == "brief_restart") {
        if (prev == FrontEnd::Screen::Briefing && hasBriefingMedia()) {
            selectBriefingPart(0);
        }
        return;
    }
    if (id == "brief_pause") {
        if (prev == FrontEnd::Screen::Briefing && hasBriefingMedia()) {
            briefPaused_ = !briefPaused_;
            setBriefingVoicePaused(briefPaused_);
            dirty_ = true;
        }
        return;
    }
    if (std::size_t slotIdx = 0; parseIndexAfter(id, "slot_", slotIdx)) {
        if (prev == FrontEnd::Screen::Briefing && slotIdx >= 1u && slotIdx <= briefingPartCount()) {
            selectBriefingPart(static_cast<int>(slotIdx));
        }
        return;
    }

    if (prev == FrontEnd::Screen::SinglePlayer && activateSinglePlayer(id)) {
        return;
    }
    if (prev == FrontEnd::Screen::LoadGame && id == "load_delete") {
        if (const auto* s = fe_.selectedSaveEntry()) {
            pendingDeleteSave_ = s->path;
        } else {
            showNotice(tr("No saved games yet."));
        }
        return;
    }
    if (prev == FrontEnd::Screen::Multiplayer && activateMultiplayer(id)) {
        return;
    }

    fe_.activate(id);
    onScreenChanged(prev);
}

void GoldFrontEnd::handleKey(SDL_Keycode key) {
    const auto prev = fe_.screen();

    if (key == SDLK_ESCAPE) {
        if (prev == FrontEnd::Screen::SinglePlayer && profileEditMode_ != ProfileEditMode::None) {
            profileEditMode_ = ProfileEditMode::None;
            profileNameDraft_ = settings_.activeProfile;
            if (window_) SDL_StopTextInput(window_);
            focusedId_ = "missions";
            dirty_ = true;
            return;
        }
        if (prev == FrontEnd::Screen::Options) cancelOptionsEdit();
        if (prev == FrontEnd::Screen::Multiplayer) finishMultiplayerEdit();
        fe_.back();
        onScreenChanged(prev);
        return;
    }

    if (prev == FrontEnd::Screen::SinglePlayer && profileEditMode_ != ProfileEditMode::None &&
        handleSinglePlayerEditKey(key)) {
        return;
    }
    if (prev == FrontEnd::Screen::Multiplayer && mpEditField_ != MpEditField::None &&
        handleMultiplayerEditKey(key)) {
        return;
    }

    if (key >= SDLK_A && key <= SDLK_Z) {
        if (const std::string id = fe_.shortcutId(char('a' + (key - SDLK_A))); !id.empty()) {
            activateId(id);
            return;
        }
    }

    switch (fe_.screen()) {
        case FrontEnd::Screen::MainMenu:
            if (key == SDLK_UP || key == SDLK_LEFT) cycleFocus({"single", "multi", "options", "ratings", "replay", "credits", "quit"}, -1);
            else if (key == SDLK_DOWN || key == SDLK_RIGHT) cycleFocus({"single", "multi", "options", "ratings", "replay", "credits", "quit"}, 1);
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) activateId(focusedId_);
            break;
        case FrontEnd::Screen::SinglePlayer:
            handleSinglePlayerKey(key);
            break;
        case FrontEnd::Screen::Campaigns:
        case FrontEnd::Screen::CustomCampaigns:
            handleCampaignsKey(key);
            break;
        case FrontEnd::Screen::SingleMissions:
        case FrontEnd::Screen::CampaignMissions:
            handleMissionBrowserKey(key);
            break;
        case FrontEnd::Screen::Replays:
        case FrontEnd::Screen::LoadGame:
        case FrontEnd::Screen::Multiplayer:
            handleMultiplayerOrBrowserKey(key);
            break;
        case FrontEnd::Screen::Options:
            handleOptionsKey(key);
            break;
        case FrontEnd::Screen::Briefing:
            handleBriefingKey(key);
            break;
        case FrontEnd::Screen::Ratings:
            handleRatingsKey(key);
            break;
        case FrontEnd::Screen::Credits:
            handleCreditsKey(key);
            break;
        case FrontEnd::Screen::Launch:
        case FrontEnd::Screen::LaunchReplay:
        case FrontEnd::Screen::LaunchSave:
        case FrontEnd::Screen::LaunchMultiplayer:
        case FrontEnd::Screen::Quit:
            break;
    }
}

void GoldFrontEnd::handleMouseMove(double stageX, double stageY) {
    if (draggingThumb_) {
        if ((fe_.screen() == FrontEnd::Screen::SingleMissions ||
             fe_.screen() == FrontEnd::Screen::CampaignMissions ||
             fe_.screen() == FrontEnd::Screen::Replays ||
             fe_.screen() == FrontEnd::Screen::LoadGame ||
             fe_.screen() == FrontEnd::Screen::Multiplayer)) {
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
    if (hit.empty()) {
        if (fe_.screen() == FrontEnd::Screen::Multiplayer && mpEditField_ != MpEditField::None) {
            finishMultiplayerEdit();
            dirty_ = true;
        }
        if (fe_.screen() == FrontEnd::Screen::SinglePlayer && profileEditMode_ != ProfileEditMode::None) {
            profileEditMode_ = ProfileEditMode::None;
            profileNameDraft_ = settings_.activeProfile;
            if (window_) SDL_StopTextInput(window_);
            dirty_ = true;
        }
        return;
    }

    if (hit == "scroll_thumb" || hit == "ratings_scroll_thumb") {
        draggingThumb_ = true;
        dragStartY_ = stageY;
        dragStartScroll_ = (hit == "ratings_scroll_thumb") ? ratingsScroll_ : scrollOffset_;
        return;
    }

    if (fe_.screen() == FrontEnd::Screen::SingleMissions ||
        fe_.screen() == FrontEnd::Screen::CampaignMissions ||
        fe_.screen() == FrontEnd::Screen::Replays ||
        fe_.screen() == FrontEnd::Screen::LoadGame ||
        fe_.screen() == FrontEnd::Screen::Multiplayer) {
        std::size_t idx = 0;
        if (parseIndexAfter(hit, activeRowPrefix(), idx) && idx < activeListCount()) {
            const bool alreadySelected = (idx == activeListSelected());
            selectActiveRow(idx);
            if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
                const auto& active = activeCampaignList();
                const std::size_t ci = activeCampaignIndex();
                if (ci < active.size() && idx < active[ci].missions.size()) {
                    ensureOverviewLoaded(active[ci].missions[idx].path);
                }
            } else if (fe_.screen() == FrontEnd::Screen::SingleMissions && idx < missions_.size()) {
                ensureOverviewLoaded(missions_[idx].path);
            } else if (fe_.screen() == FrontEnd::Screen::Multiplayer && idx < mpMissions_.size()) {
                ensureOverviewLoaded(mpMissions_[idx].path);
            }
            if (mpEditField_ != MpEditField::None) {
                mpEditField_ = MpEditField::None;
                if (window_) SDL_StopTextInput(window_);
            }
            focusedId_ = (fe_.screen() == FrontEnd::Screen::Replays) ? "replay_start"
                       : (fe_.screen() == FrontEnd::Screen::LoadGame) ? "load_start"
                       : (fe_.screen() == FrontEnd::Screen::Multiplayer) ? "mp_start" : "start";
            dirty_ = true;
            if (fe_.screen() == FrontEnd::Screen::Multiplayer) {
                if (clicks >= 2 && fe_.mpIsHost()) activateId("mp_start");
            } else if (clicks >= 2 || (clicks == 0 && alreadySelected)) {
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
         fe_.screen() == FrontEnd::Screen::CampaignMissions ||
         fe_.screen() == FrontEnd::Screen::Replays ||
         fe_.screen() == FrontEnd::Screen::LoadGame ||
         fe_.screen() == FrontEnd::Screen::Multiplayer) &&
        dy != 0.0f) {
        scrollBy(dy > 0 ? -3 : 3);
    } else if (fe_.screen() == FrontEnd::Screen::Briefing && dy != 0.0f) {
        briefScroll_ = std::max(0.0, briefScroll_ - double(dy) * 28.0);
        dirty_ = true;
    } else if (fe_.screen() == FrontEnd::Screen::Ratings && dy != 0.0f) {
        scrollRatingsBy(dy > 0 ? -3 : 3);
    } else if (fe_.screen() == FrontEnd::Screen::SinglePlayer && dy != 0.0f) {
        const int maxOff = std::max(0, int(settings_.profiles.size()) - kVisibleProfileRows);
        const int next = std::clamp(profileScroll_ + (dy > 0 ? -3 : 3), 0, maxOff);
        if (next != profileScroll_) {
            profileScroll_ = next;
            dirty_ = true;
        }
    } else if (fe_.screen() == FrontEnd::Screen::Credits && dy != 0.0f) {
        const double minScroll = -kCreditsClipH + 40.0;
        const double maxScroll = creditsContentHeight_ > 0.0 ? creditsContentHeight_ : 1.0e6;
        creditsScroll_ = std::clamp(creditsScroll_ - double(dy) * 32.0, minScroll, maxScroll);
        dirty_ = true;
    }
}

void GoldFrontEnd::handleEvent(const SDL_Event& event) {
    if (window_ && assets_) assets_->menuCursor.apply();
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        window_ && event.window.windowID == SDL_GetWindowID(window_)) {
        dirty_ = true;
        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        handleKey(event.key.key);
        return;
    }

    if (event.type == SDL_EVENT_TEXT_INPUT &&
        fe_.screen() == FrontEnd::Screen::SinglePlayer &&
        profileEditMode_ != ProfileEditMode::None && event.text.text) {
        handleSinglePlayerTextInput(event.text.text);
        return;
    }

    if (event.type == SDL_EVENT_TEXT_INPUT &&
        fe_.screen() == FrontEnd::Screen::Multiplayer &&
        mpEditField_ != MpEditField::None && event.text.text) {
        handleMultiplayerTextInput(event.text.text);
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
    if (window_ && assets_) assets_->menuCursor.apply();
    syncMenuMusic();
    if (noticeRemaining_.count() > 0) {
        if (dt >= noticeRemaining_) {
            noticeRemaining_ = {};
            noticeText_.clear();
            dirty_ = true;
        } else {
            noticeRemaining_ -= dt;
        }
    }
    if (fe_.screen() == FrontEnd::Screen::Briefing) {
        advanceBriefingMedia(dt);
    }
    if (assets_ && fe_.screen() == FrontEnd::Screen::Multiplayer) {
        if (!fe_.mpIsHost()) {
            if (!assets_->lanScannerActive) {
                assets_->lanScannerActive = assets_->lanScanner.start(kDefaultLanDiscoveryPort);
            }
            if (assets_->lanScannerActive) {
                assets_->lanScanner.poll();
                if (assets_->lanScanner.games().size() != assets_->lastLanCount) {
                    assets_->lastLanCount = assets_->lanScanner.games().size();
                    dirty_ = true;
                }
            }
        } else if (assets_->lanScannerActive) {
            assets_->lanScanner.stop();
            assets_->lanScannerActive = false;
            assets_->lastLanCount = 0u;
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

void GoldFrontEnd::stopMenuMusic() {
    if (!assets_) return;
    assets_->activeMusicEntry.clear();
    assets_->musicReadOffset = 0u;
    if (assets_->musicAudioStream) {
        SDL_ClearAudioStream(assets_->musicAudioStream);
    }
}

void GoldFrontEnd::pumpMenuMusic() {
    if (!assets_ || window_ == nullptr || assets_->activeMusicEntry.empty() ||
        assets_->menuArchives.empty()) {
        return;
    }
    const std::size_t trackBytes = (assets_->activeMusicEntry == "briefing")
        ? assets_->briefingMusicBytes
        : assets_->backwavBytes;
    if (trackBytes < 4u) return;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    }
    constexpr SDL_AudioSpec kDstSpec{SDL_AUDIO_S16, 2, 48000};
    constexpr SDL_AudioSpec kSrcSpec{SDL_AUDIO_S16, 2, 22050};
    if (!assets_->musicAudioDev) {
        assets_->musicAudioDev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &kDstSpec);
        if (!assets_->musicAudioDev) return;
    }
    if (!assets_->musicAudioStream) {
        assets_->musicAudioStream = SDL_CreateAudioStream(&kSrcSpec, &kDstSpec);
        if (!assets_->musicAudioStream ||
            !SDL_BindAudioStream(assets_->musicAudioDev, assets_->musicAudioStream)) {
            if (assets_->musicAudioStream) {
                SDL_DestroyAudioStream(assets_->musicAudioStream);
                assets_->musicAudioStream = nullptr;
            }
            return;
        }
    }

    const UserSettings& cfg = (fe_.screen() == FrontEnd::Screen::Options) ? optionsDraft_ : settings_;
    const bool voiceSpeaking = (assets_->activeMusicEntry == "briefing" &&
                                assets_->briefAudioStream != nullptr &&
                                !briefPaused_ &&
                                SDL_GetAudioStreamQueued(assets_->briefAudioStream) > 0);
    const float gain = std::clamp(cfg.musicVolume, 0.0f, 1.0f) * (voiceSpeaking ? 0.45f : 1.0f);
    SDL_SetAudioStreamGain(assets_->musicAudioStream, gain);
    SDL_ResumeAudioDevice(assets_->musicAudioDev);

    constexpr int kLowWaterBytes = 96000;
    constexpr int kHighWaterBytes = 144000;
    constexpr std::size_t kSliceBytes = 65536u;
    for (int iter = 0; iter < 3; ++iter) {
        const int queued = SDL_GetAudioStreamQueued(assets_->musicAudioStream);
        if (queued >= kHighWaterBytes || (iter > 0 && queued >= kLowWaterBytes)) break;
        if (assets_->musicReadOffset >= trackBytes) assets_->musicReadOffset = 0u;
        const std::size_t want = (std::min(kSliceBytes, trackBytes - assets_->musicReadOffset)) & ~std::size_t(3u);
        if (want < 4u) {
            assets_->musicReadOffset = 0u;
            continue;
        }
        assets::Bytes pcm;
        try {
            pcm = assets::readSueChainRange(assets_->menuArchives, assets_->activeMusicEntry,
                                            assets_->musicReadOffset, want);
        } catch (...) {
            stopMenuMusic();
            return;
        }
        const std::size_t aligned = pcm.size() & ~std::size_t(3u);
        if (aligned < 4u) {
            assets_->musicReadOffset = 0u;
            break;
        }
        (void)SDL_PutAudioStreamData(assets_->musicAudioStream, pcm.data(), static_cast<int>(aligned));
        assets_->musicReadOffset = (assets_->musicReadOffset + aligned) % trackBytes;
    }
}

void GoldFrontEnd::syncMenuMusic() {
    if (!assets_) return;
    const auto screen = fe_.screen();
    const UserSettings& cfg = (screen == FrontEnd::Screen::Options) ? optionsDraft_ : settings_;
    const bool terminal = (screen == FrontEnd::Screen::Launch ||
                           screen == FrontEnd::Screen::LaunchReplay ||
                           screen == FrontEnd::Screen::LaunchSave ||
                           screen == FrontEnd::Screen::LaunchMultiplayer ||
                           screen == FrontEnd::Screen::Quit);
    std::string desired;
    if (!terminal && !cfg.musicMuted && cfg.musicVolume > 0.001f) {
        if (screen == FrontEnd::Screen::Briefing) {
            if (assets_->briefingMusicBytes >= 4u) desired = "briefing";
        } else if (assets_->backwavBytes >= 4u) {
            desired = assets_->backwavEntry;
        }
    }
    if (desired.empty()) {
        stopMenuMusic();
        return;
    }
    if (assets_->activeMusicEntry != desired) {
        assets_->activeMusicEntry = desired;
        assets_->musicReadOffset = 0u;
        if (assets_->musicAudioStream) {
            SDL_ClearAudioStream(assets_->musicAudioStream);
        }
    }
    pumpMenuMusic();
}

void GoldFrontEnd::renderMainMenu(const RenderEnv& env) {
    drawHeading(env.cr, env.pangoCtx, tr("Main Menu"), {135, 38, 126, 26}, 17.0);
    for (const auto& b : kMainButtons) {
        drawGoldStencilButton(env.cr, env.pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                              hoveredId_ == b.id, focusedId_ == b.id, b.enabled, 24.0);
    }
    auto ver = makeTextLayout(env.pangoCtx, "ver 2.2", 11.5, false);
    cairo_move_to(env.cr, 725, 576);
    cairo_set_source_rgba(env.cr, 232.0 / 255.0, 228.0 / 255.0, 200.0 / 255.0, 0.85);
    pango_cairo_show_layout(env.cr, ver.get());
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
    const RenderEnv env{cr, pangoCtx};
    const auto screen = fe_.screen();
    const RamkaScope ramkaScope(&assets_->ramka);

    if (screen == FrontEnd::Screen::Credits) {
        renderCredits(env);
    } else if (screen == FrontEnd::Screen::Briefing) {
        renderBriefing(env);
    } else {
        if (assets_->mainBg) drawSurfaceStretched(cr, assets_->mainBg.get(), 0, 0, kStageW, kStageH);
        else drawFallbackBackdrop(cr, false);

        switch (screen) {
            case FrontEnd::Screen::MainMenu:
                renderMainMenu(env);
                break;
            case FrontEnd::Screen::SinglePlayer:
                renderSinglePlayer(env);
                break;
            case FrontEnd::Screen::Campaigns:
            case FrontEnd::Screen::CustomCampaigns:
                renderCampaigns(env);
                break;
            case FrontEnd::Screen::CampaignMissions:
            case FrontEnd::Screen::SingleMissions:
                renderMissionBrowserScreen(env);
                break;
            case FrontEnd::Screen::Ratings:
                renderRatings(env);
                break;
            case FrontEnd::Screen::Options:
                renderOptions(env);
                break;
            case FrontEnd::Screen::Replays:
            case FrontEnd::Screen::LoadGame:
                renderReplaysOrLoadGame(env);
                break;
            case FrontEnd::Screen::Multiplayer:
                renderMultiplayer(env);
                break;
            case FrontEnd::Screen::Briefing:
            case FrontEnd::Screen::Credits:
            case FrontEnd::Screen::Launch:
            case FrontEnd::Screen::LaunchReplay:
            case FrontEnd::Screen::LaunchSave:
            case FrontEnd::Screen::LaunchMultiplayer:
            case FrontEnd::Screen::Quit:
                break;
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
    if (window_ && assets_) assets_->menuCursor.apply();
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

}  // namespace ss2
