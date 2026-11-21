#include "ui/hud.hpp"
#include "ui/strings.hpp"
#include "render/window.hpp"

#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ss2 {
namespace {
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;

void colour(cairo_t* cr, int r, int g, int b, double a = 1) {
    cairo_set_source_rgba(cr, r / 255., g / 255., b / 255., a);
}

// Draws one line of text; returns its pixel width. Text wider than maxWidth is ellipsized.
// With centre set, the line is centred within maxWidth.
int text(cairo_t* cr, PangoContext* fonts, const std::string& value, int size, bool bold, double x, double y, int maxWidth,
         bool centre = false) {
    Layout layout(pango_layout_new(fonts), g_object_unref);
    std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
        pango_font_description_from_string("Inter, sans-serif"), pango_font_description_free);
    pango_font_description_set_weight(font.get(), bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(font.get(), size * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_text(layout.get(), value.c_str(), -1);
    if (maxWidth > 0) {
        pango_layout_set_width(layout.get(), maxWidth * PANGO_SCALE);
        pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
        if (centre) pango_layout_set_alignment(layout.get(), PANGO_ALIGN_CENTER);
    }
    int w{}, h{};
    pango_layout_get_pixel_size(layout.get(), &w, &h);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout.get());
    return w;
}

// The largest size from `size` down to about 70% of it at which the text fits the width
// on one line; longer words in other languages should shrink before they are cut off.
int fittedSize(PangoContext* fonts, const std::string& value, int size, bool bold, int maxWidth) {
    for (int candidate = size; candidate > std::max(8, size * 7 / 10); --candidate) {
        Layout layout(pango_layout_new(fonts), g_object_unref);
        std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
            pango_font_description_from_string("Inter, sans-serif"), pango_font_description_free);
        pango_font_description_set_weight(font.get(), bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
        pango_font_description_set_absolute_size(font.get(), candidate * PANGO_SCALE);
        pango_layout_set_font_description(layout.get(), font.get());
        pango_layout_set_text(layout.get(), value.c_str(), -1);
        int w{}, h{};
        pango_layout_get_pixel_size(layout.get(), &w, &h);
        if (w <= maxWidth) return candidate;
    }
    return std::max(8, size * 7 / 10);
}

// Wraps RGBA8 pixels as a cairo image (premultiplied, native-endian ARGB).
Surface image(const assets::MapImage& source) {
    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(source.width), int(source.height)),
                    cairo_surface_destroy);
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error("Could not create a HUD image");
    auto* out = cairo_image_surface_get_data(surface.get());
    const int stride = cairo_image_surface_get_stride(surface.get());
    for (unsigned y = 0; y < source.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(out + std::size_t(y) * stride);
        const auto* in = &source.rgba[std::size_t(y) * source.width * 4];
        for (unsigned x = 0; x < source.width; ++x, in += 4) {
            const unsigned a = in[3];
            const auto pre = [a](unsigned c) { return (c * a + 127) / 255; };
            row[x] = (a << 24) | (pre(in[0]) << 16) | (pre(in[1]) << 8) | pre(in[2]);
        }
    }
    cairo_surface_mark_dirty(surface.get());
    return surface;
}

bool inside(Hud::Rect r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

bool hasRole(const HudState& state, std::initializer_list<const char*> needles) {
    for (const auto& c : state.selection) {
        std::string low = c.label;
        for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (const char* n : needles) {
            if (low.find(n) != std::string::npos) return true;
        }
    }
    return false;
}

bool hasOfficerSelection(const HudState& state) {
    return (state.hasSelection || !state.selection.empty()) && hasRole(state, {"officer"});
}
}

Hud::Hud(SDL_Window* parent) : parent_(parent) {
    if (!parent_) throw std::invalid_argument("HUD needs a parent window");
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> map(pango_cairo_font_map_new(), g_object_unref);
    fonts_.reset(pango_font_map_create_context(map.get()), g_object_unref);
    buttons_ = {{HudAction::stop, "Stop", "S", {}}, {HudAction::holdFire, "Hold fire", "H", {}},
                {HudAction::crouch, "Crouch", "C", {}}, {HudAction::mission, "Mission", "M", {}},
                {HudAction::pause, "Pause", "P", {}}, {HudAction::log, "Log", "L", {}}};
    layout();
    render();
}

void Hud::setMinimap(const assets::MapImage& overview) {
    if (!overview.width || !overview.height || std::uint64_t(overview.width) * overview.height * 4 != overview.rgba.size())
        throw std::invalid_argument("Unsupported minimap dimensions or pixel count");
    overview_ = overview;
    layout();
    render();
}

void Hud::setPortrait(const std::string& label, const assets::MapImage& portrait) {
    if (!portrait.width || !portrait.height || std::uint64_t(portrait.width) * portrait.height * 4 != portrait.rgba.size())
        throw std::invalid_argument("Unsupported portrait dimensions or pixel count");
    portraits_[label] = portrait;
    render();
}

void Hud::log(std::string message) {
    if (message.empty()) return;
    // One line per entry; a few hundred entries are plenty for a session's review.
    // Line breaks of either kind become spaces; a CR LF pair becomes one space.
    std::string flat;
    for (char c : message) {
        if (c == '\r' || c == '\n' || c == '\t') {
            if (!flat.empty() && flat.back() != ' ') flat += ' ';
        } else flat += c;
    }
    while (!flat.empty() && flat.back() == ' ') flat.pop_back();
    if (flat.empty()) return;
    message = std::move(flat);
    messages_.push_back(std::move(message));
    if (messages_.size() > 400) messages_.erase(messages_.begin(), messages_.begin() + 100);
    render();
}

std::string Hud::logText(std::size_t newest) const {
    std::string out;
    const std::size_t first = messages_.size() > newest ? messages_.size() - newest : 0;
    for (std::size_t i = first; i < messages_.size(); ++i) {
        if (!out.empty()) out += '\n';
        out += messages_[i];
    }
    return out;
}

// The bar must not rely on being handed the resize event: a caller that routes events
// elsewhere while a dialog is open would leave it laid out for the old window, drawn at
// the wrong size and swallowing clicks in the wrong place.
void Hud::refit() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) return;
    if (unsigned(std::max(pixelW, 1)) == width_ && std::max(pixelH, 1) == top_ + int(height_)) {
        // Same size: only a change of interface language needs a new picture.
        if (int(language()) != language_) render();
        return;
    }
    pressed_.reset();
    hovered_.reset();
    pressedAir_.reset();
    hoveredAir_.reset();
    pressedGroup_.reset();
    hoveredGroup_.reset();
    pressedClass_.reset();
    hoveredClass_.reset();
    draggingMinimap_ = false;
    layout();
    render();
}

void Hud::update(const HudState& state) {
    refit();
    for (const auto& dot : state.dots)
        if (!std::isfinite(dot.u) || !std::isfinite(dot.v)) throw std::invalid_argument("HUD dot has a non-finite position");
    for (const auto& plane : state.planes)
        if (!std::isfinite(plane.u) || !std::isfinite(plane.v)) throw std::invalid_argument("HUD plane has a non-finite position");
    for (float v : {state.viewU, state.viewV, state.viewW, state.viewH})
        if (!std::isfinite(v)) throw std::invalid_argument("HUD view rectangle has a non-finite value");
    if (state.hasObjectiveMarker && (!std::isfinite(state.objectiveU) || !std::isfinite(state.objectiveV)))
        throw std::invalid_argument("HUD objective marker has a non-finite position");
    if (state == state_) return;
    state_ = state;
    render();
}

Hud::Rect Hud::row3Rect(int col) const {
    if (buttons_.empty() || col < 0 || col > 2) return {};
    const int bw = buttons_[0].rect.width, bh = buttons_[0].rect.height;
    return {buttons_[0].rect.x + col * (bw + 2), buttons_[0].rect.y + 2 * (bh + 2), bw, bh};
}

Hud::Rect Hud::reconButtonRect() const {
    return row3Rect(0);
}

std::optional<HudAction> Hud::row3Action(int col) const {
    const bool hasUnits = state_.hasSelection || !state_.selection.empty();
    if (!hasUnits) return std::nullopt;
    if (col == 0) return hasOfficerSelection(state_) ? HudAction::recon : HudAction::attackMove;
    if (col == 1) {
        if (state_.hasCargo) return HudAction::unload;
        if (state_.hasSapper) return HudAction::mine;
        if (hasRole(state_, {"medic"})) return std::nullopt;
        return HudAction::patrol;
    }
    if (col == 2) {
        if (state_.hasBombard) return HudAction::attackGround;
        if (row3Action(1) != HudAction::patrol) return HudAction::patrol;
        if (state_.hasInfantry) return HudAction::scatter;
    }
    return std::nullopt;
}

Hud::Rect Hud::buttonRect(HudAction action) const {
    for (const auto& b : buttons_) if (b.action == action) return b.rect;
    for (int col = 0; col < 3; ++col) {
        if (row3Action(col) == action) return row3Rect(col);
    }
    return {};
}

Hud::Rect Hud::airButtonRect(int slot) const {
    if (slot < 0 || slot >= 5 || state_.airReady[std::size_t(slot)] <= 0 || readout_.width <= 40) return {};
    const int pad = 6;
    const int leftBlockW = std::clamp(readout_.x - 4, minimap_.x + minimap_.width + pad, int(width_));
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, int(height_));
    const int sx = leftBlockW + 2, sw = std::max(1, int(width_) - sx - 2);
    const int size = std::clamp(strip_ * 3 / 5, 9, 14);
    const int pillH = std::max(12, strip_ - 7);
    const int pillY = trayTop + 4;
    static constexpr const char* kLetters[5] = {"B", "R", "P", "S", "F"};
    int rightEdge = sx + sw - 4;
    for (int k = 4; k >= 0; --k) {
        const int ready = state_.airReady[std::size_t(k)];
        if (ready <= 0) continue;
        const std::string label = std::string(kLetters[k]) + ":" + std::to_string(ready);
        const int pillW = std::max(34, int(label.size()) * size * 2 / 3 + size);
        const int pillX = rightEdge - pillW;
        if (pillX < sx + sw / 2) break;
        if (k == slot) return {pillX, pillY, pillW, pillH};
        rightEdge = pillX - 4;
    }
    return {};
}

Hud::Rect Hud::groupChipRect(unsigned digit) const {
    if (readout_.width <= 40 || state_.groups.empty()) return {};
    const int pad = 6;
    const int leftBlockW = std::clamp(readout_.x - 4, minimap_.x + minimap_.width + pad, int(width_));
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, int(height_));
    const int sx = leftBlockW + 2, sw = std::max(1, int(width_) - sx - 2);
    const int size = std::clamp(strip_ * 3 / 5, 9, 14);
    const int half = sw / 2;
    int chipX = sx + half + 4;
    const int chipH = std::max(8, strip_ - 7);
    for (const auto& group : state_.groups) {
        const std::string label = std::to_string(group.digit) + ":" + std::to_string(group.count);
        const int chipW = int(label.size()) * size * 2 / 3 + size;
        if (chipX + chipW > sx + sw * 3 / 4) break;
        if (group.digit == digit) return {chipX, trayTop + 5, chipW, chipH};
        chipX += chipW + 4;
    }
    return {};
}

Hud::Rect Hud::classCardRect(std::size_t index) const {
    if (readout_.width <= 40 || index >= state_.selection.size()) return {};
    const int full = int(height_);
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, full);
    const int trayH = full - trayTop;
    const int small = std::clamp(trayH / 7, 9, 15);
    const int inX = readout_.x + 8;
    const int inW = std::max(1, readout_.width - 16);
    const int cardsY = readout_.y + small + 3;
    const int cardsH = readout_.y + readout_.height - cardsY - 1;
    if (cardsH < 14) return {};
    const int portrait = cardsH - 4, gap = 4;
    const int widest = std::max(portrait + small * 8, 112);
    const int narrowest = std::max(portrait + small * 5, 82);
    const int classes = int(state_.selection.size());
    const int cardW = std::clamp((inW - (classes - 1) * gap) / classes, narrowest, widest);
    const int fit = std::max(1, (inW + gap) / (cardW + gap));
    if (int(index) >= fit) return {};
    if (int(index) == fit - 1 && classes > fit) return {};
    return {inX + int(index) * (cardW + gap), cardsY, cardW, cardsH};
}

void Hud::layout() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) throw std::runtime_error(SDL_GetError());
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);
    width_ = unsigned(pixelW);
    // Classic Sudden Strike II bottom-left L-shaped console height (~22% of screen, bounded <= H/3).
    const int full = std::clamp(int(std::lround(pixelH * 0.22)), std::min(110, pixelH), std::min(260, std::max(110, pixelH / 3)));
    height_ = unsigned(full);
    strip_ = std::clamp(full / 7, 16, 22);
    top_ = pixelH - full;
    const int pad = 6;

    // 1. Far-left: Isometric diamond minimap in armored steel housing (2:1 aspect, <= 26% of width).
    const int mapH = std::max(24, full - 2 * pad);
    const double aspect = overview_.width && overview_.height ? double(overview_.width) / overview_.height : 2.0;
    int mapW = int(std::lround(mapH * aspect)), fittedH = mapH;
    const int widest = std::max(24, pixelW * 26 / 100);
    if (mapW > widest) {
        mapW = widest;
        fittedH = std::max(16, int(std::lround(mapW / aspect)));
    }
    minimap_ = {pad, pad + (mapH - fittedH) / 2, mapW, fittedH};

    // 2. Immediately right of minimap: classic 3x3 square metallic command grid.
    const int count = int(buttons_.size());
    const int rows = 3;
    const int perRow = 3;
    const int gap = 2;
    const int gridH = full - 2 * pad;
    const int buttonH = std::max(20, (gridH - (rows - 1) * gap) / rows);
    const int maxGridW = std::max(perRow * 22, pixelW * 18 / 100);
    const int maxCellW = std::max(20, (maxGridW - (perRow - 1) * gap) / perRow);
    const int buttonW = std::clamp(buttonH, 20, maxCellW);
    const int blockW = perRow * buttonW + (perRow - 1) * gap;
    const int startX = minimap_.x + minimap_.width + pad;
    for (int i = 0; i < count; ++i) {
        const int row = i / perRow, column = i % perRow;
        buttons_[i].rect = {startX + column * (buttonW + gap), pad + row * (buttonH + gap), buttonW, buttonH};
    }

    // 3. Right of the tall left cluster: slim horizontal unit/status tray flush with bottom edge.
    //    Everything above trayTop on the right is transparent (alpha=0) so live battlefield shows through.
    const int leftBlockW = startX + blockW + pad;
    const int trayH = std::clamp(int(std::lround(full * 0.54)), std::min(72, full), std::max(72, std::min(126, full - 24)));
    const int trayTop = full - trayH;
    const int meterH = 10;
    readout_ = {leftBlockW + 4, trayTop + strip_ + 1, std::max(0, pixelW - leftBlockW - 8),
                std::max(18, trayH - strip_ - meterH - 3)};
}

namespace {
void bevelFrame(cairo_t* cr, double x, double y, double w, double h, bool inset) {
    if (w <= 2 || h <= 2) return;
    // Crisp metallic highlight on top/left (raised) or bottom/right (inset)
    cairo_set_line_width(cr, 1.5);
    if (!inset) colour(cr, 186, 194, 182); else colour(cr, 16, 20, 18);
    cairo_move_to(cr, x + 0.75, y + h - 0.75);
    cairo_line_to(cr, x + 0.75, y + 0.75);
    cairo_line_to(cr, x + w - 0.75, y + 0.75);
    cairo_stroke(cr);
    if (!inset) colour(cr, 16, 20, 18); else colour(cr, 162, 170, 158);
    cairo_move_to(cr, x + w - 0.75, y + 0.75);
    cairo_line_to(cr, x + w - 0.75, y + h - 0.75);
    cairo_line_to(cr, x + 0.75, y + h - 0.75);
    cairo_stroke(cr);
}

void drawRivet(cairo_t* cr, double cx, double cy, double r) {
    colour(cr, 18, 22, 20);
    cairo_arc(cr, cx + 0.6, cy + 0.6, r, 0, 2 * G_PI);
    cairo_fill(cr);
    colour(cr, 128, 136, 124);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill(cr);
    colour(cr, 204, 210, 198);
    cairo_arc(cr, cx - r * 0.3, cy - r * 0.3, r * 0.4, 0, 2 * G_PI);
    cairo_fill(cr);
}

void drawCompassRose(cairo_t* cr, double cx, double cy, double r) {
    colour(cr, 16, 20, 16);
    cairo_arc(cr, cx, cy, r * 0.32, 0, 2 * G_PI);
    cairo_fill(cr);
    colour(cr, 214, 198, 130);
    cairo_set_line_width(cr, 1.3);
    cairo_move_to(cr, cx, cy - r); cairo_line_to(cr, cx, cy + r);
    cairo_move_to(cr, cx - r, cy); cairo_line_to(cr, cx + r, cy);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r);
    cairo_line_to(cr, cx - r * 0.22, cy - r * 0.35);
    cairo_line_to(cr, cx + r * 0.22, cy - r * 0.35);
    cairo_close_path(cr);
    cairo_fill(cr);
}
void drawDiamondPath(cairo_t* cr, double x, double y, double w, double h) {
    const double cx = x + w * 0.5, cy = y + h * 0.5;
    cairo_new_sub_path(cr);
    cairo_move_to(cr, cx, y);
    cairo_line_to(cr, x + w, cy);
    cairo_line_to(cr, cx, y + h);
    cairo_line_to(cr, x, cy);
    cairo_close_path(cr);
}

void drawPowerSymbol(cairo_t* cr, double cx, double cy, double r) {
    colour(cr, 188, 196, 184);
    cairo_set_line_width(cr, 1.4);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, -G_PI * 0.25, G_PI * 1.25);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r * 1.25);
    cairo_line_to(cr, cx, cy - r * 0.15);
    cairo_stroke(cr);
}

void drawCommandIcon(cairo_t* cr, HudAction action, double cx, double cy, double r, bool active) {
    if (r <= 1.0) return;
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const double lw = std::clamp(r * 0.22, 1.25, 2.6);
    const auto strokeShadow = [&](auto&& drawPath) {
        cairo_save(cr);
        cairo_translate(cr, 0.7, 0.7);
        colour(cr, 10, 13, 11, 0.8);
        cairo_set_line_width(cr, lw);
        drawPath();
        cairo_stroke(cr);
        cairo_restore(cr);
    };
    const auto mainTone = [&]() {
        if (active) colour(cr, 238, 244, 174);
        else colour(cr, 218, 224, 210);
    };

    switch (action) {
    case HudAction::stop: {
        // Octagonal halt emblem with crisp inner halt square
        const double rad = r * 0.88;
        auto octagon = [&](double ox, double oy) {
            cairo_new_sub_path(cr);
            for (int k = 0; k < 8; ++k) {
                const double a = G_PI / 8.0 + k * (G_PI / 4.0);
                const double px = ox + std::cos(a) * rad, py = oy + std::sin(a) * rad;
                if (k == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
            }
            cairo_close_path(cr);
        };
        colour(cr, 10, 13, 11, 0.75);
        octagon(cx + 0.7, cy + 0.7);
        cairo_fill(cr);
        if (active) colour(cr, 198, 72, 60); else colour(cr, 146, 50, 44);
        octagon(cx, cy);
        cairo_fill_preserve(cr);
        mainTone();
        cairo_set_line_width(cr, std::max(1.0, lw * 0.7));
        cairo_stroke(cr);
        const double sq = r * 0.36;
        colour(cr, 236, 238, 226);
        cairo_rectangle(cr, cx - sq, cy - sq, sq * 2.0, sq * 2.0);
        cairo_fill(cr);
        break;
    }
    case HudAction::holdFire: {
        // Crosshair reticle with bold diagonal slash
        const double ringR = r * 0.68, outerR = r * 0.94, innerR = r * 0.34;
        auto crosshair = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, ringR, 0, 2 * G_PI);
            cairo_move_to(cr, cx - outerR, cy); cairo_line_to(cr, cx - innerR, cy);
            cairo_move_to(cr, cx + innerR, cy); cairo_line_to(cr, cx + outerR, cy);
            cairo_move_to(cr, cx, cy - outerR); cairo_line_to(cr, cx, cy - innerR);
            cairo_move_to(cr, cx, cy + innerR); cairo_line_to(cr, cx, cy + outerR);
        };
        strokeShadow(crosshair);
        mainTone();
        cairo_set_line_width(cr, std::max(1.1, lw * 0.85));
        crosshair();
        cairo_stroke(cr);
        const double s = r * 0.78;
        colour(cr, 12, 15, 13, 0.85);
        cairo_set_line_width(cr, lw * 1.55);
        cairo_move_to(cr, cx - s, cy + s); cairo_line_to(cr, cx + s, cy - s);
        cairo_stroke(cr);
        if (active) colour(cr, 240, 96, 74); else colour(cr, 212, 86, 72);
        cairo_set_line_width(cr, lw * 1.05);
        cairo_move_to(cr, cx - s, cy + s); cairo_line_to(cr, cx + s, cy - s);
        cairo_stroke(cr);
        break;
    }
    case HudAction::crouch: {
        // Downward kneeling/shield chevron over ground baseline
        auto chevron = [&]() {
            cairo_move_to(cr, cx - r * 0.74, cy - r * 0.56);
            cairo_line_to(cr, cx, cy + r * 0.08);
            cairo_line_to(cr, cx + r * 0.74, cy - r * 0.56);
            cairo_move_to(cr, cx - r * 0.58, cy - r * 0.04);
            cairo_line_to(cr, cx, cy + r * 0.52);
            cairo_line_to(cr, cx + r * 0.58, cy - r * 0.04);
            cairo_move_to(cr, cx - r * 0.68, cy + r * 0.78);
            cairo_line_to(cr, cx + r * 0.68, cy + r * 0.78);
        };
        strokeShadow(chevron);
        mainTone();
        cairo_set_line_width(cr, lw);
        chevron();
        cairo_stroke(cr);
        break;
    }
    case HudAction::mission: {
        // Tactical target crosshair ring with central military star
        const double ringR = r * 0.82, tickIn = r * 0.64, tickOut = r * 0.98;
        auto reticle = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, ringR, 0, 2 * G_PI);
            cairo_move_to(cr, cx, cy - tickOut); cairo_line_to(cr, cx, cy - tickIn);
            cairo_move_to(cr, cx, cy + tickIn);  cairo_line_to(cr, cx, cy + tickOut);
            cairo_move_to(cr, cx - tickOut, cy); cairo_line_to(cr, cx - tickIn, cy);
            cairo_move_to(cr, cx + tickIn, cy);  cairo_line_to(cr, cx + tickOut, cy);
        };
        strokeShadow(reticle);
        colour(cr, 186, 194, 178);
        cairo_set_line_width(cr, std::max(1.0, lw * 0.72));
        reticle();
        cairo_stroke(cr);
        const double outer = r * 0.54, inner = r * 0.22;
        cairo_new_sub_path(cr);
        for (int k = 0; k < 10; ++k) {
            const double rad = (k % 2 == 0) ? outer : inner;
            const double a = -G_PI * 0.5 + k * (G_PI / 5.0);
            const double px = cx + std::cos(a) * rad, py = cy + std::sin(a) * rad;
            if (k == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
        }
        cairo_close_path(cr);
        if (active) colour(cr, 244, 224, 138); else colour(cr, 218, 194, 114);
        cairo_fill_preserve(cr);
        colour(cr, 18, 20, 16, 0.7);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);
        break;
    }
    case HudAction::pause: {
        // Dual vertical machined bars
        const double barW = std::max(2.0, r * 0.34), barH = r * 1.44, gap = std::max(2.0, r * 0.32);
        const double y0 = cy - barH * 0.5;
        const double lx = cx - gap * 0.5 - barW, rx = cx + gap * 0.5;
        colour(cr, 10, 13, 11, 0.75);
        cairo_rectangle(cr, lx + 0.8, y0 + 0.8, barW, barH);
        cairo_rectangle(cr, rx + 0.8, y0 + 0.8, barW, barH);
        cairo_fill(cr);
        mainTone();
        cairo_rectangle(cr, lx, y0, barW, barH);
        cairo_rectangle(cr, rx, y0, barW, barH);
        cairo_fill(cr);
        break;
    }
    case HudAction::log: {
        // Horizontal dispatch / field-radio lines with signal bullets
        const double x0 = cx - r * 0.82, xBar = cx - r * 0.38, xEnd = cx + r * 0.82;
        const double ys[3] = {cy - r * 0.48, cy, cy + r * 0.48};
        const double ends[3] = {xEnd, xEnd - r * 0.14, xEnd - r * 0.30};
        const double dot = std::max(1.4, lw * 0.85);
        auto lines = [&]() {
            for (int k = 0; k < 3; ++k) {
                cairo_move_to(cr, xBar, ys[k]);
                cairo_line_to(cr, ends[k], ys[k]);
            }
        };
        strokeShadow(lines);
        for (int k = 0; k < 3; ++k) {
            if (k == 0) colour(cr, 218, 198, 126); else mainTone();
            cairo_rectangle(cr, x0, ys[k] - dot * 0.5, dot, dot);
            cairo_fill(cr);
            cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
            cairo_move_to(cr, xBar, ys[k]);
            cairo_line_to(cr, ends[k], ys[k]);
            cairo_stroke(cr);
        }
        break;
    }
    case HudAction::recon: {
        auto bino = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx - r * 0.48, cy, r * 0.44, 0, 2 * G_PI);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx + r * 0.48, cy, r * 0.44, 0, 2 * G_PI);
            cairo_move_to(cr, cx - r * 0.14, cy - r * 0.22);
            cairo_line_to(cr, cx + r * 0.14, cy - r * 0.22);
        };
        strokeShadow(bino);
        mainTone();
        cairo_set_line_width(cr, std::max(1.2, lw * 0.9));
        bino();
        cairo_stroke(cr);
        break;
    }
    case HudAction::attackMove: {
        // Tactical thrust arrow toward forward combat pip
        auto thrust = [&]() {
            cairo_move_to(cr, cx - r * 0.82, cy + r * 0.48);
            cairo_line_to(cr, cx + r * 0.62, cy - r * 0.48);
            cairo_move_to(cr, cx + r * 0.12, cy - r * 0.56);
            cairo_line_to(cr, cx + r * 0.68, cy - r * 0.52);
            cairo_line_to(cr, cx + r * 0.46, cy - r * 0.02);
        };
        strokeShadow(thrust);
        colour(cr, active ? 242 : 226, active ? 132 : 112, active ? 92 : 84);
        cairo_set_line_width(cr, lw);
        thrust();
        cairo_stroke(cr);
        break;
    }
    case HudAction::patrol: {
        // Bidirectional alternating patrol sweep arrows
        auto loop = [&]() {
            cairo_move_to(cr, cx - r * 0.78, cy - r * 0.34);
            cairo_line_to(cr, cx + r * 0.68, cy - r * 0.34);
            cairo_move_to(cr, cx + r * 0.34, cy - r * 0.64);
            cairo_line_to(cr, cx + r * 0.74, cy - r * 0.34);
            cairo_line_to(cr, cx + r * 0.34, cy - r * 0.04);
            cairo_move_to(cr, cx + r * 0.78, cy + r * 0.34);
            cairo_line_to(cr, cx - r * 0.68, cy + r * 0.34);
            cairo_move_to(cr, cx - r * 0.34, cy + r * 0.04);
            cairo_line_to(cr, cx - r * 0.74, cy + r * 0.34);
            cairo_line_to(cr, cx - r * 0.34, cy + r * 0.64);
        };
        strokeShadow(loop);
        mainTone();
        cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
        loop();
        cairo_stroke(cr);
        break;
    }
    case HudAction::attackGround: {
        // Ballistic arc & starburst ground impact reticle
        auto shell = [&]() {
            cairo_move_to(cr, cx - r * 0.78, cy + r * 0.25);
            cairo_curve_to(cr, cx - r * 0.35, cy - r * 0.85, cx + r * 0.30, cy - r * 0.75, cx + r * 0.56, cy + r * 0.16);
            cairo_move_to(cr, cx + r * 0.22, cy + r * 0.42);
            cairo_line_to(cr, cx + r * 0.86, cy + r * 0.42);
            cairo_move_to(cr, cx + r * 0.54, cy + r * 0.10);
            cairo_line_to(cr, cx + r * 0.54, cy + r * 0.74);
        };
        strokeShadow(shell);
        colour(cr, active ? 244 : 230, active ? 186 : 164, active ? 96 : 86);
        cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
        shell();
        cairo_stroke(cr);
        break;
    }
    case HudAction::unload: {
        // Open transport hull + disembark arrow
        auto hatch = [&]() {
            cairo_move_to(cr, cx - r * 0.68, cy - r * 0.56);
            cairo_line_to(cr, cx - r * 0.68, cy + r * 0.56);
            cairo_line_to(cr, cx - r * 0.08, cy + r * 0.56);
            cairo_move_to(cr, cx - r * 0.68, cy - r * 0.56);
            cairo_line_to(cr, cx - r * 0.08, cy - r * 0.56);
            cairo_move_to(cr, cx - r * 0.28, cy);
            cairo_line_to(cr, cx + r * 0.76, cy);
            cairo_move_to(cr, cx + r * 0.40, cy - r * 0.34);
            cairo_line_to(cr, cx + r * 0.78, cy);
            cairo_line_to(cr, cx + r * 0.40, cy + r * 0.34);
        };
        strokeShadow(hatch);
        mainTone();
        cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
        hatch();
        cairo_stroke(cr);
        break;
    }
    case HudAction::mine: {
        // Anti-tank circular mine housing with central detonator pressure cap
        auto mine = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, r * 0.68, 0, 2 * G_PI);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, r * 0.26, 0, 2 * G_PI);
        };
        strokeShadow(mine);
        colour(cr, active ? 244 : 216, active ? 222 : 196, active ? 132 : 114);
        cairo_set_line_width(cr, std::max(1.15, lw * 0.9));
        mine();
        cairo_stroke(cr);
        break;
    }
    case HudAction::scatter: {
        // Four outward-pointing arrows representing emergency disperse
        auto arrows = [&]() {
            static constexpr double dirs[4][2] = {{1, -1}, {-1, -1}, {1, 1}, {-1, 1}};
            for (const auto& d : dirs) {
                const double sx0 = cx + d[0] * r * 0.16, sy0 = cy + d[1] * r * 0.16;
                const double ex = cx + d[0] * r * 0.72, ey = cy + d[1] * r * 0.72;
                cairo_move_to(cr, sx0, sy0);
                cairo_line_to(cr, ex, ey);
                cairo_move_to(cr, ex - d[0] * r * 0.34, ey);
                cairo_line_to(cr, ex, ey);
                cairo_line_to(cr, ex, ey - d[1] * r * 0.34);
            }
        };
        strokeShadow(arrows);
        mainTone();
        cairo_set_line_width(cr, std::max(1.05, lw * 0.85));
        arrows();
        cairo_stroke(cr);
        break;
    }
    default:
        break;
    }
    cairo_restore(cr);
}
} // namespace

void Hud::render() {
    language_ = int(language());
    const int w = int(width_), full = int(height_);
    Surface target(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, full), cairo_surface_destroy);
    Context graphics(cairo_create(target.get()), cairo_destroy);
    auto* cr = graphics.get();

    // Start fully transparent so the upper-right L-cutout reveals the live battlefield.
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const int pad = 6;
    const int leftBlockW = std::clamp(readout_.x - 4, minimap_.x + minimap_.width + pad, w);
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, full);
    const int trayH = full - trayTop;
    const int small = std::clamp(trayH / 7, 9, 15), body = std::clamp(trayH / 6, 10, 18);

    // L-shaped weathered gunmetal-steel console plate with machined horizontal grain & steel bevels.
    {
        auto fillSteelPlate = [&](int rx, int ry, int rw, int rh) {
            if (rw <= 0 || rh <= 0) return;
            cairo_save(cr);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_clip(cr);
            auto* gradient = cairo_pattern_create_linear(0, ry, 0, ry + rh);
            cairo_pattern_add_color_stop_rgb(gradient, 0, 56 / 255., 62 / 255., 54 / 255.);
            cairo_pattern_add_color_stop_rgb(gradient, 0.35, 36 / 255., 41 / 255., 36 / 255.);
            cairo_pattern_add_color_stop_rgb(gradient, 1, 20 / 255., 24 / 255., 21 / 255.);
            cairo_set_source(cr, gradient);
            cairo_paint(cr);
            cairo_pattern_destroy(gradient);
            cairo_set_line_width(cr, 1.0);
            for (int gy = ry + 2; gy < ry + rh; gy += 3) {
                if ((gy % 6) == 0) colour(cr, 66, 72, 64);
                else colour(cr, 26, 30, 27);
                cairo_move_to(cr, rx, gy + 0.5);
                cairo_line_to(cr, rx + rw, gy + 0.5);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
        };
        // Tall left armored cluster (minimap + 3x3 command grid)
        fillSteelPlate(0, 0, leftBlockW, full);
        // Low right horizontal unit/status tray
        fillSteelPlate(leftBlockW, trayTop, w - leftBlockW, trayH);

        // Crisp 2px bright steel outer rim along the entire L-shaped top/step perimeter
        colour(cr, 198, 206, 192);
        cairo_rectangle(cr, 0, 0, leftBlockW, 2);
        cairo_rectangle(cr, leftBlockW - 2, 0, 2, trayTop + 2);
        cairo_rectangle(cr, leftBlockW - 2, trayTop, w - leftBlockW + 2, 2);
        cairo_fill(cr);
        colour(cr, 16, 20, 18);
        cairo_rectangle(cr, 0, 2, leftBlockW - 2, 1);
        cairo_rectangle(cr, leftBlockW, trayTop + 2, w - leftBlockW, 1);
        cairo_fill(cr);
    }

    // 1. Isometric diamond minimap in dark riveted steel housing + brass compass rose + corner buttons.
    {
        const auto m = minimap_;
        colour(cr, 16, 20, 17);
        cairo_rectangle(cr, m.x - 2, m.y - 2, m.width + 4, m.height + 4);
        cairo_fill(cr);
        bevelFrame(cr, m.x - 3, m.y - 3, m.width + 6, m.height + 6, true);

        // Dark charcoal diamond well
        colour(cr, 10, 13, 12);
        drawDiamondPath(cr, m.x, m.y, m.width, m.height);
        cairo_fill(cr);

        if (overview_.width) {
            const auto picture = image(overview_);
            cairo_save(cr);
            drawDiamondPath(cr, m.x, m.y, m.width, m.height);
            cairo_clip(cr);
            cairo_translate(cr, m.x, m.y);
            cairo_scale(cr, double(m.width) / overview_.width, double(m.height) / overview_.height);
            cairo_set_source_surface(cr, picture.get(), 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        // Dots and camera view rectangle clipped inside the diamond
        cairo_save(cr);
        drawDiamondPath(cr, m.x, m.y, m.width, m.height);
        cairo_clip(cr);
        const double radius = std::max(1.5, full / 75.0);
        for (bool hostile : {false, true})
            for (const auto& dot : state_.dots) {
                if (dot.hostile != hostile) continue;
                if (hostile) colour(cr, 232, 72, 60);
                else if (dot.allied) colour(cr, 240, 215, 75);
                else if (dot.neutral) colour(cr, 130, 165, 225);
                else colour(cr, 110, 224, 120);
                cairo_arc(cr, m.x + dot.u * m.width, m.y + dot.v * m.height, radius, 0, 2 * G_PI);
                cairo_fill(cr);
            }
        for (const auto& plane : state_.planes) {
            const double px = m.x + double(plane.u) * m.width;
            const double py = m.y + double(plane.v) * m.height;
            const double span = std::clamp(full / 38.0, 3.0, 5.5);
            auto drawCross = [&]() {
                cairo_move_to(cr, px - span, py);
                cairo_line_to(cr, px + span, py);
                cairo_move_to(cr, px, py - span);
                cairo_line_to(cr, px, py + span);
            };
            colour(cr, 12, 16, 14, 0.9);
            cairo_set_line_width(cr, 2.8);
            drawCross();
            cairo_stroke(cr);
            if (plane.hostile) colour(cr, 255, 118, 66);
            else colour(cr, 135, 240, 255);
            cairo_set_line_width(cr, 1.5);
            drawCross();
            cairo_stroke(cr);
        }
        for (const auto& ping : state_.pings) {
            const float inten = std::clamp(ping.intensity, 0.0f, 1.0f);
            if (inten <= 0.0f) continue;
            const double px = m.x + std::clamp(ping.u, 0.0f, 1.0f) * m.width;
            const double py = m.y + std::clamp(ping.v, 0.0f, 1.0f) * m.height;
            const double ringR = 4.0 + 10.0 * (1.0 - double(inten));
            if (ping.hostile) colour(cr, 245, 75, 65, 0.9 * double(inten));
            else colour(cr, 90, 225, 255, 0.9 * double(inten));
            cairo_set_line_width(cr, 1.8);
            cairo_arc(cr, px, py, ringR, 0, 2 * G_PI);
            cairo_stroke(cr);
        }
        if (state_.hasObjectiveMarker) {
            const double ox = m.x + std::clamp(state_.objectiveU, 0.0f, 1.0f) * m.width;
            const double oy = m.y + std::clamp(state_.objectiveV, 0.0f, 1.0f) * m.height;
            const double ro = std::clamp(full / 24.0, 4.5, 7.5);
            const double ri = ro * 0.36;
            auto starPath = [&]() {
                cairo_new_sub_path(cr);
                for (int k = 0; k < 8; ++k) {
                    const double a = -G_PI * 0.5 + k * (G_PI * 0.25);
                    const double rad = (k % 2 == 0) ? ro : ri;
                    const double sx = ox + std::cos(a) * rad;
                    const double sy = oy + std::sin(a) * rad;
                    if (k == 0) cairo_move_to(cr, sx, sy); else cairo_line_to(cr, sx, sy);
                }
                cairo_close_path(cr);
            };
            starPath();
            colour(cr, 14, 18, 15, 0.92);
            cairo_set_line_width(cr, 2.2);
            cairo_stroke_preserve(cr);
            colour(cr, 246, 216, 84);
            cairo_fill(cr);
        }
        colour(cr, 245, 245, 240);
        cairo_set_line_width(cr, std::max(1.0, std::round(full / 120.0)));
        cairo_rectangle(cr, std::floor(m.x + state_.viewU * m.width) + .5, std::floor(m.y + state_.viewV * m.height) + .5,
                        std::max(2.0, std::round(double(state_.viewW) * m.width)),
                        std::max(2.0, std::round(double(state_.viewH) * m.height)));
        cairo_stroke(cr);
        cairo_restore(cr);

        // Sharp light-steel diamond border matching the GOG SS2 minimap frame
        colour(cr, 178, 186, 172);
        cairo_set_line_width(cr, 1.5);
        drawDiamondPath(cr, m.x, m.y, m.width, m.height);
        cairo_stroke(cr);

        // Brass compass rose on upper-right steel corner outside the diamond
        if (m.width >= 60 && m.height >= 40) {
            drawCompassRose(cr, m.x + m.width - 13.0, m.y + 13.0, std::clamp(m.height * 0.095, 5.5, 10.0));
            // Classic power symbol tucked in lower-left corner outside the diamond (opens Pause Menu)
            const bool menuHover = hovered_ && *hovered_ == HudAction::menu;
            if (menuHover) {
                colour(cr, 72, 82, 62);
                cairo_arc(cr, m.x + 11.0, m.y + m.height - 22.0, 8.0, 0, 2 * G_PI);
                cairo_fill(cr);
            }
            drawPowerSymbol(cr, m.x + 11.0, m.y + m.height - 22.0, 5.0);
            // 3 tiny square utility pips along the bottom-left lip (down, up, pause)
            const int pip = 10, py = m.y + m.height - pip - 1;
            for (int k = 0; k < 3; ++k) {
                const int px = m.x + 2 + k * (pip + 2);
                colour(cr, k == 2 && state_.paused ? 74 : 32, k == 2 && state_.paused ? 88 : 37, k == 2 && state_.paused ? 56 : 33);
                cairo_rectangle(cr, px, py, pip, pip);
                cairo_fill(cr);
                bevelFrame(cr, px, py, pip, pip, false);
                colour(cr, 198, 204, 192);
                cairo_set_line_width(cr, 1.1);
                if (k == 0) {
                    cairo_move_to(cr, px + 5.0, py + 2.5); cairo_line_to(cr, px + 5.0, py + 7.5);
                    cairo_move_to(cr, px + 3.0, py + 5.5); cairo_line_to(cr, px + 5.0, py + 7.5); cairo_line_to(cr, px + 7.0, py + 5.5);
                    cairo_stroke(cr);
                } else if (k == 1) {
                    cairo_move_to(cr, px + 5.0, py + 7.5); cairo_line_to(cr, px + 5.0, py + 2.5);
                    cairo_move_to(cr, px + 3.0, py + 4.5); cairo_line_to(cr, px + 5.0, py + 2.5); cairo_line_to(cr, px + 7.0, py + 4.5);
                    cairo_stroke(cr);
                } else {
                    cairo_rectangle(cr, px + 3.0, py + 2.5, 1.5, 5.0);
                    cairo_rectangle(cr, px + 5.5, py + 2.5, 1.5, 5.0);
                    cairo_fill(cr);
                }
            }
        }
    }

    // 2. Classic 3x3 square metallic command grid immediately right of minimap (context-sensitive).
    const bool hasUnits = state_.hasSelection || !state_.selection.empty();

    const auto isActionActive = [&](HudAction act) {
        return (act == HudAction::holdFire && state_.holdFire) ||
               (act == HudAction::crouch && state_.crouched) ||
               (act == HudAction::pause && state_.paused) ||
               (act == HudAction::recon && state_.binoculars) ||
               (act == HudAction::attackMove && state_.attackMoveArmed) ||
               (act == HudAction::patrol && state_.patrolArmed) ||
               (act == HudAction::attackGround && state_.attackGroundArmed) ||
               (act == HudAction::mine && state_.mineArmed);
    };
    const auto drawCommandCell = [&](const Rect& r, HudAction act, const char* label, const char* key) {
        const bool on = isActionActive(act);
        const bool down = pressed_ && *pressed_ == act;
        const bool hover = hovered_ && *hovered_ == act;
        auto* bg = cairo_pattern_create_linear(r.x, r.y, r.x, r.y + r.height);
        if (down) {
            cairo_pattern_add_color_stop_rgb(bg, 0, 86 / 255., 98 / 255., 66 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 56 / 255., 64 / 255., 46 / 255.);
        } else if (on) {
            cairo_pattern_add_color_stop_rgb(bg, 0, 74 / 255., 88 / 255., 56 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 46 / 255., 56 / 255., 38 / 255.);
        } else if (hover) {
            cairo_pattern_add_color_stop_rgb(bg, 0, 62 / 255., 69 / 255., 60 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 38 / 255., 43 / 255., 38 / 255.);
        } else {
            cairo_pattern_add_color_stop_rgb(bg, 0, 38 / 255., 43 / 255., 39 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 22 / 255., 26 / 255., 23 / 255.);
        }
        cairo_set_source(cr, bg);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_fill(cr);
        cairo_pattern_destroy(bg);
        bevelFrame(cr, r.x, r.y, r.width, r.height, down);
        if (on) {
            colour(cr, 206, 222, 130);
            cairo_set_line_width(cr, 1.5);
            cairo_rectangle(cr, r.x + 1.5, r.y + 1.5, r.width - 3, r.height - 3);
            cairo_stroke(cr);
        }
        const int labelSize = std::clamp(r.height / 5, 7, 13), keySize = std::clamp(r.height / 5, 7, 11);
        const std::string lbl = tr(label);
        const int capW = std::max(8, r.width - 4);
        const int capSize = fittedSize(fonts_.get(), lbl, labelSize, false, capW);
        const double captionY = r.y + r.height - labelSize * 1.35 - 2;
        const double iconTop = r.y + 4.0;
        const double iconBottom = captionY - 1.0;
        const double iconR = std::clamp(std::min(double(r.width) - 8.0, iconBottom - iconTop) * 0.31, 4.5, 16.0);
        drawCommandIcon(cr, act, r.x + r.width * 0.53, (iconTop + iconBottom) * 0.53, iconR, on || down);
        if (key && *key) {
            const int badgeW = std::max(9, keySize + 2), badgeH = std::max(9, keySize + 2);
            const double bx = r.x + 2.0, by = r.y + 2.0;
            colour(cr, 14, 18, 15, 0.72);
            cairo_rectangle(cr, bx, by, badgeW, badgeH);
            cairo_fill(cr);
            colour(cr, on ? 168 : 96, on ? 182 : 104, on ? 104 : 92, 0.65);
            cairo_set_line_width(cr, 0.8);
            cairo_rectangle(cr, bx + 0.4, by + 0.4, badgeW - 0.8, badgeH - 0.8);
            cairo_stroke(cr);
            colour(cr, on ? 236 : 196, on ? 242 : 202, on ? 186 : 188);
            text(cr, fonts_.get(), key, keySize, true, bx, by + (badgeH - keySize * 1.2) * 0.5, badgeW, true);
        }
        colour(cr, on ? 228 : 186, on ? 234 : 192, on ? 196 : 180);
        text(cr, fonts_.get(), lbl, capSize, false, r.x + 2, captionY, capW, true);
    };

    if (!buttons_.empty()) {
        const int gx0 = buttons_[0].rect.x;
        const int gy0 = buttons_[0].rect.y;
        const int bw = buttons_[0].rect.width;
        const int bh = buttons_[0].rect.height;
        const int gap = 2;
        const int gridW = 3 * bw + 2 * gap;
        const int gridH = 3 * bh + 2 * gap;

        // Recessed dark charcoal backing for the full 3x3 grid
        colour(cr, 14, 17, 15);
        cairo_rectangle(cr, gx0 - 2, gy0 - 2, gridW + 4, gridH + 4);
        cairo_fill(cr);
        bevelFrame(cr, gx0 - 3, gy0 - 3, gridW + 6, gridH + 6, true);

        static constexpr std::pair<HudAction, std::pair<const char*, const char*>> kRow3Meta[] = {
            {HudAction::recon, {"Recon", "B"}},
            {HudAction::attackMove, {"Attack", "A"}},
            {HudAction::patrol, {"Patrol", "Q"}},
            {HudAction::attackGround, {"Bombard", "G"}},
            {HudAction::unload, {"Unload", "U"}},
            {HudAction::mine, {"Mine", "E"}},
            {HudAction::scatter, {"Scatter", "D"}},
        };
        for (int col = 0; col < 3; ++col) {
            const Rect r = row3Rect(col);
            if (const auto act = row3Action(col)) {
                const char* lbl = "";
                const char* key = "";
                for (const auto& [k, mk] : kRow3Meta) {
                    if (k == *act) { lbl = mk.first; key = mk.second; break; }
                }
                drawCommandCell(r, *act, lbl, key);
                continue;
            }
            const bool showMedic = (col == 1 && hasUnits && hasRole(state_, {"medic"}));
            const bool showSpecial = (col == 2 && hasUnits && hasRole(state_, {"flam", "sniper", "sapper", "miner", "at "}));
            if (!showMedic && !showSpecial) {
                colour(cr, 22, 26, 23);
                cairo_rectangle(cr, r.x, r.y, r.width, r.height);
                cairo_fill(cr);
                bevelFrame(cr, r.x, r.y, r.width, r.height, true);
                continue;
            }
            const int rx = r.x, ry = r.y;
            auto* bg = cairo_pattern_create_linear(rx, ry, rx, ry + bh);
            cairo_pattern_add_color_stop_rgb(bg, 0, 46 / 255., 54 / 255., 44 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 26 / 255., 31 / 255., 26 / 255.);
            cairo_set_source(cr, bg);
            cairo_rectangle(cr, rx, ry, bw, bh);
            cairo_fill(cr);
            cairo_pattern_destroy(bg);
            bevelFrame(cr, rx, ry, bw, bh, false);

            const int labelSize = std::clamp(bh / 5, 7, 13);
            const double captionY = ry + bh - labelSize * 1.35 - 2;
            const double cx = rx + bw * 0.5, cy = (ry + 4.0 + captionY) * 0.5;
            const double ir = std::clamp(std::min(double(bw) - 8.0, captionY - ry - 5.0) * 0.30, 4.5, 14.0);
            if (showMedic) {
                colour(cr, 226, 72, 62);
                cairo_rectangle(cr, cx - ir * 0.25, cy - ir * 0.75, ir * 0.5, ir * 1.5);
                cairo_rectangle(cr, cx - ir * 0.75, cy - ir * 0.25, ir * 1.5, ir * 0.5);
                cairo_fill(cr);
                colour(cr, 186, 194, 178);
                text(cr, fonts_.get(), "Heal", fittedSize(fonts_.get(), "Heal", labelSize, false, std::max(8, bw - 4)),
                     false, rx + 2, captionY, std::max(8, bw - 4), true);
            } else {
                colour(cr, 236, 156, 62);
                cairo_move_to(cr, cx, cy - ir * 0.85);
                cairo_line_to(cr, cx + ir * 0.55, cy + ir * 0.25);
                cairo_line_to(cr, cx, cy + ir * 0.75);
                cairo_line_to(cr, cx - ir * 0.55, cy + ir * 0.25);
                cairo_close_path(cr);
                cairo_fill(cr);
                colour(cr, 186, 194, 178);
                text(cr, fonts_.get(), "Special", fittedSize(fonts_.get(), "Special", labelSize, false, std::max(8, bw - 4)),
                     false, rx + 2, captionY, std::max(8, bw - 4), true);
            }
        }
    }
    for (const auto& b : buttons_) {
        const bool unitOrder = (b.action == HudAction::stop || b.action == HudAction::holdFire || b.action == HudAction::crouch);
        const auto r = b.rect;
        if (unitOrder && !hasUnits) {
            // Match original Sudden Strike II: unit-order slots are blank recessed steel when nothing is selected.
            colour(cr, 22, 26, 23);
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
            cairo_fill(cr);
            bevelFrame(cr, r.x, r.y, r.width, r.height, true);
            continue;
        }
        drawCommandCell(r, b.action, b.label, b.key);
    }

    // 3. Slim bottom-right tray: top status strip, unit portrait bay, and classic EX/L/M/A/R stat lip.
    if (readout_.width > 40) {
        // 3a. Top status strip inside the right tray
        {
            const int sx = leftBlockW + 2, sw = std::max(1, w - sx - 2);
            const int size = std::clamp(strip_ * 3 / 5, 9, 14), y = trayTop + (strip_ - size) / 2 + 1;
            colour(cr, 16, 20, 17);
            cairo_rectangle(cr, sx, trayTop + 3, sw, strip_ - 3);
            cairo_fill(cr);
            colour(cr, 130, 138, 126);
            cairo_rectangle(cr, sx, trayTop + strip_, sw, 1);
            cairo_fill(cr);

            int x = sx + 6;
            const int half = sw / 2;
            if (!state_.objective.empty()) {
                colour(cr, 214, 198, 130);
                x += text(cr, fonts_.get(), tr("Objective"), size, true, x, y, 0) + size / 2;
                colour(cr, 234, 236, 228);
                text(cr, fonts_.get(), state_.objective, size, false, x, y, std::max(1, sx + half - x));
            }
            int chipX = sx + half + 4;
            for (const auto& group : state_.groups) {
                const auto gr = groupChipRect(group.digit);
                if (gr.width <= 0 || gr.height <= 0) break;
                const std::string label = std::to_string(group.digit) + ":" + std::to_string(group.count);
                const bool down = pressedGroup_ && *pressedGroup_ == group.digit;
                const bool hov = hoveredGroup_ && *hoveredGroup_ == group.digit;
                if (down) colour(cr, 86, 98, 66);
                else if (hov) colour(cr, 64, 74, 58);
                else colour(cr, 48, 56, 44);
                cairo_rectangle(cr, gr.x, gr.y, gr.width, gr.height);
                cairo_fill(cr);
                bevelFrame(cr, gr.x, gr.y, gr.width, gr.height, down);
                colour(cr, 236, 238, 230);
                text(cr, fonts_.get(), label, size, true, gr.x, y, gr.width, true);
                chipX = std::max(chipX, gr.x + gr.width + 4);
            }
            int airLeft = sx + sw - 4;
            static constexpr const char* kAirLetters[5] = {"B", "R", "P", "S", "F"};
            for (int k = 0; k < 5; ++k) {
                const auto ar = airButtonRect(k);
                if (ar.width <= 0 || ar.height <= 0) continue;
                airLeft = std::min(airLeft, ar.x);
                const bool armed = (state_.armedAir == k);
                const bool down = (pressedAir_ && *pressedAir_ == k);
                const bool hov = (hoveredAir_ && *hoveredAir_ == k);
                if (armed) colour(cr, 96, 110, 56);
                else if (down) colour(cr, 86, 98, 66);
                else if (hov) colour(cr, 64, 74, 58);
                else colour(cr, 42, 50, 44);
                cairo_rectangle(cr, ar.x, ar.y, ar.width, ar.height);
                cairo_fill(cr);
                bevelFrame(cr, ar.x, ar.y, ar.width, ar.height, down);
                if (armed) {
                    colour(cr, 235, 212, 118);
                    cairo_set_line_width(cr, 1.2);
                    cairo_rectangle(cr, ar.x + 1, ar.y + 1, ar.width - 2, ar.height - 2);
                    cairo_stroke(cr);
                }
                colour(cr, armed ? 245 : 230, armed ? 236 : 220, armed ? 180 : 172);
                const std::string lbl = std::string(kAirLetters[k]) + ":" + std::to_string(state_.airReady[std::size_t(k)]);
                text(cr, fonts_.get(), lbl, size, true, ar.x, y - 1, ar.width, true);
            }
            static const std::pair<HudAction, const char*> hints[] = {
                {HudAction::stop, "Stop: cancel movement and attacks (S)"},
                {HudAction::holdFire, "Hold fire: toggle firing at will (H)"},
                {HudAction::crouch, "Crouch: toggle crouching, slower but harder to hit (C)"},
                {HudAction::mission, "Mission: jump to the mission marker (M)"},
                {HudAction::pause, "Pause: stop or resume the mission clock (P)"},
                {HudAction::log, "Log: review recent messages (L)"},
                {HudAction::recon, "Recon: Officer binoculars for extended vision (B)"},
                {HudAction::attackMove, "Attack-Move: advance and engage hostiles (A)"},
                {HudAction::patrol, "Patrol: loop between position and target (Q)"},
                {HudAction::attackGround, "Bombard: fire at ground target (G)"},
                {HudAction::unload, "Unload: disembark passengers (U)"},
                {HudAction::mine, "Mine: lay mine or disarm detected mines (E)"},
                {HudAction::scatter, "Scatter: disperse infantry outward from blasts (D)"}};
            std::string right;
            bool hint = false;
            if (hovered_) for (const auto& [action, line] : hints) if (action == *hovered_) { right = tr(line); hint = true; }
            if (!hint && hoveredAir_ && *hoveredAir_ >= 0 && *hoveredAir_ < 5) {
                static constexpr const char* kAirHints[5] = {
                    "Bomber: click map to carpet-bomb (Alt+1)",
                    "Recon: click map for scout overflight (Alt+2)",
                    "Paratrooper: click map to drop airborne squad (Alt+3)",
                    "Supply: click map for supply drop (Alt+4)",
                    "Fighter: click map for strafing run (Alt+5)",
                };
                right = tr(kAirHints[*hoveredAir_]);
                hint = true;
            }
            if (!hint && hoveredGroup_) {
                right = trf("Group {0}: click to recall (click again to focus, Shift+click to add)",
                            {std::to_string(*hoveredGroup_)});
                hint = true;
            }
            if (!hint && hoveredClass_) {
                right = trf("{0}: click to select only this class, Shift+click to remove",
                            {tr(*hoveredClass_)});
                hint = true;
            }
            if (!hint && !messages_.empty()) right = messages_.back();
            if (!right.empty()) {
                const int rx = std::max(chipX + 4, sx + sw * 3 / 5);
                const int maxRight = (airLeft < sx + sw - 4) ? (airLeft - 6) : (sx + sw - 4);
                const int rw = maxRight - rx;
                if (rw > 8) {
                    if (hint) colour(cr, 206, 222, 136); else colour(cr, 178, 184, 174);
                    cairo_save(cr);
                    cairo_rectangle(cr, rx, trayTop + 3, rw, strip_ - 3);
                    cairo_clip(cr);
                    text(cr, fonts_.get(), right, size, false, rx, y, rw);
                    cairo_restore(cr);
                }
            }
        }

        // 3b. Recessed steel unit & squad bay
        const auto r = readout_;
        colour(cr, 18, 22, 19);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_fill(cr);
        bevelFrame(cr, r.x - 1, r.y - 1, r.width + 2, r.height + 2, true);
        if (r.width > 120 && r.height > 28) {
            drawRivet(cr, r.x + 4, r.y + 4, 1.8);
            drawRivet(cr, r.x + r.width - 4, r.y + 4, 1.8);
        }
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_clip(cr);
        const int inX = r.x + 8;
        const int inW = std::max(1, r.width - 16);
        unsigned total = 0;
        for (const auto& c : state_.selection) total += c.count;
        const std::string title = total == 1 ? tr("1 soldier selected")
                                : total ? trf("{0} soldiers selected", {std::to_string(total)}) : tr("No units selected");
        colour(cr, 214, 198, 130);
        const int titleW = text(cr, fonts_.get(), title, small, true, inX, r.y + 2, inW / 2);

        // Stance + losses inline on the right of the header line to maximize portrait height
        std::string status = tr(state_.crouched ? "Crouched" : "Standing");
        status += "  " + tr(state_.holdFire ? "Hold fire" : "Fire at will");
        if (state_.paused) status += "  " + tr("Paused");
        colour(cr, 174, 182, 170);
        const int statusX = inX + titleW + 12;
        const int statusW = text(cr, fonts_.get(), status, small, false, statusX, r.y + 2, std::max(1, inW - titleW - 12));
        if (state_.losses) {
            colour(cr, 232, 112, 96);
            text(cr, fonts_.get(), trf("Losses {0}", {std::to_string(state_.losses)}), small, true,
                 statusX + statusW + 10, r.y + 2, std::max(1, r.x + r.width - (statusX + statusW + 14)));
        }

        // Compact metallic portrait slots for selected classes
        const int cardsY = r.y + small + 3, cardsH = r.y + r.height - cardsY - 1;
        if (cardsH >= 14 && !state_.selection.empty()) {
            const int portrait = cardsH - 4, gap = 4;
            const int widest = std::max(portrait + small * 8, 112), narrowest = std::max(portrait + small * 5, 82);
            const int classes = int(state_.selection.size());
            const int cardW = std::clamp((inW - (classes - 1) * gap) / classes, narrowest, widest);
            const int fit = std::max(1, (inW + gap) / (cardW + gap));
            int x = inX;
            for (int i = 0; i < int(state_.selection.size()) && i < fit; ++i, x += cardW + gap) {
                const auto& c = state_.selection[i];
                const bool last = i == fit - 1 && int(state_.selection.size()) > fit;
                const bool cardDown = !last && pressedClass_ && *pressedClass_ == c.label;
                const bool cardHov = !last && hoveredClass_ && *hoveredClass_ == c.label;
                if (cardDown) colour(cr, 56, 68, 52);
                else if (cardHov) colour(cr, 42, 50, 42);
                else colour(cr, 28, 33, 29);
                cairo_rectangle(cr, x, cardsY, cardW, cardsH);
                cairo_fill(cr);
                bevelFrame(cr, x, cardsY, cardW, cardsH, cardDown);
                if (last) {
                    colour(cr, 178, 184, 174);
                    text(cr, fonts_.get(), "+" + std::to_string(state_.selection.size() - fit + 1) + " more", body, false,
                         x + 4, cardsY + (cardsH - body) / 2.0, cardW - 8);
                    break;
                }
                const auto found = portraits_.find(c.label);
                if (found != portraits_.end()) {
                    const auto picture = image(found->second);
                    cairo_save(cr);
                    cairo_rectangle(cr, x + 2, cardsY + 2, portrait, portrait);
                    cairo_clip(cr);
                    cairo_translate(cr, x + 2, cardsY + 2);
                    cairo_scale(cr, double(portrait) / found->second.width, double(portrait) / found->second.height);
                    cairo_set_source_surface(cr, picture.get(), 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else {
                    colour(cr, 48, 56, 44);
                    cairo_rectangle(cr, x + 2, cardsY + 2, portrait, portrait);
                    cairo_fill(cr);
                    colour(cr, 214, 198, 130);
                    const std::string initial = c.label.empty() ? std::string("?") : c.label.substr(0, 1);
                    const int letter = std::max(9, portrait / 2);
                    text(cr, fonts_.get(), initial, letter, true, x + 2, cardsY + 2 + (portrait - letter * 1.25) / 2.0, portrait, true);
                }
                bevelFrame(cr, x + 2, cardsY + 2, portrait, portrait, true);
                const int tx = x + portrait + 5, tw = std::max(8, cardW - portrait - 8);
                colour(cr, 236, 238, 230);
                {
                    const std::string name = tr(c.label) + " x" + std::to_string(c.count);
                    text(cr, fonts_.get(), name, fittedSize(fonts_.get(), name, small, true, tw), true, tx, cardsY + 2, tw);
                }
                const auto bar = [&](int row, float value, int red, int green, int blue) {
                    const double barH = std::max(2.5, cardsH / 10.0), by = cardsY + cardsH - (2 - row) * (barH + 2) - 2;
                    colour(cr, 14, 17, 15);
                    cairo_rectangle(cr, tx, by, tw, barH);
                    cairo_fill(cr);
                    colour(cr, red, green, blue);
                    cairo_rectangle(cr, tx, by, tw * std::clamp(value, 0.f, 1.f), barH);
                    cairo_fill(cr);
                };
                const float hp = std::clamp(c.health, 0.f, 1.f);
                bar(0, c.health, int(220 - 110 * hp), int(80 + 144 * hp), 90);
                bar(1, c.ammo, 214, 170, 82);
            }
        }
        cairo_restore(cr);

        // 3c. Classic Sudden Strike II bottom metallic EX / L / M / A / R stat tick bar
        {
            const int my = full - 10, mh = 9, mw = std::max(1, w - leftBlockW - 6);
            colour(cr, 16, 19, 17);
            cairo_rectangle(cr, leftBlockW + 3, my, mw, mh);
            cairo_fill(cr);
            bevelFrame(cr, leftBlockW + 2, my - 1, mw + 2, mh + 2, true);
            static const char* labels[5] = {"EX", "L", "M", "A", "R"};
            struct Rgb { int r, g, b; };
            const auto meterColour = [](int index, float v) -> Rgb {
                switch (index) {
                case 0: return {230, 186, 66};   // EX: gold / amber
                case 1: {                        // L: green -> amber -> red by value
                    if (v >= 0.5f) {
                        const float t = (v - 0.5f) * 2.f;
                        return {int(std::lround(228 - 144 * t)), int(std::lround(174 + 34 * t)), int(std::lround(58 + 38 * t))};
                    }
                    const float t = v * 2.f;
                    return {int(std::lround(220 + 8 * t)), int(std::lround(62 + 112 * t)), int(std::lround(54 + 4 * t))};
                }
                case 2: return {118, 174, 188};  // M: sky / olive-slate
                case 3: return {214, 152, 68};   // A: warm brass
                default: return {68, 196, 148};  // R: crisp green / teal
                }
            };
            const int segW = mw / 5;
            for (int i = 0; i < 5; ++i) {
                const int lx = leftBlockW + 5 + i * segW;
                const int tx = lx + 16, ty = my + 2, tw = std::max(4, segW - 22), th = mh - 4;
                if (state_.hasSelection) colour(cr, 214, 220, 202); else colour(cr, 186, 192, 176);
                text(cr, fonts_.get(), labels[i], 7, true, lx, my, 16);
                colour(cr, 36, 42, 36);
                cairo_rectangle(cr, tx, ty, tw, th);
                cairo_fill(cr);
                if (state_.hasSelection) {
                    const float raw = state_.meters[std::size_t(i)];
                    const float v = std::isfinite(raw) ? std::clamp(raw, 0.f, 1.f) : 0.f;
                    const double fillW = tw * double(v);
                    if (fillW > 0.0) {
                        const auto c = meterColour(i, v);
                        colour(cr, c.r, c.g, c.b);
                        cairo_rectangle(cr, tx, ty, fillW, th);
                        cairo_fill(cr);
                        colour(cr, std::min(255, c.r + 36), std::min(255, c.g + 36), std::min(255, c.b + 36), 0.7);
                        cairo_rectangle(cr, tx, ty, fillW, 1.5);
                        cairo_fill(cr);
                    }
                }
            }
        }
    }
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));

    rgba_.resize(std::size_t(w) * full * 4);
    cairo_surface_flush(target.get());
    if (!SDL_ConvertPixels(w, full, SDL_PIXELFORMAT_ARGB8888, cairo_image_surface_get_data(target.get()),
                           cairo_image_surface_get_stride(target.get()), SDL_PIXELFORMAT_RGBA32, rgba_.data(), w * 4))
        throw std::runtime_error(SDL_GetError());
    // Cairo ARGB32 is premultiplied; un-premultiply into straight RGBA8 while keeping transparent cutout alpha=0.
    for (std::size_t i = 0; i < rgba_.size(); i += 4) {
        const std::uint8_t a = rgba_[i + 3];
        if (a == 0) {
            rgba_[i] = rgba_[i + 1] = rgba_[i + 2] = 0;
        } else if (a < 255) {
            rgba_[i] = std::uint8_t(std::min<unsigned>(255, (unsigned(rgba_[i]) * 255 + a / 2) / a));
            rgba_[i + 1] = std::uint8_t(std::min<unsigned>(255, (unsigned(rgba_[i + 1]) * 255 + a / 2) / a));
            rgba_[i + 2] = std::uint8_t(std::min<unsigned>(255, (unsigned(rgba_[i + 2]) * 255 + a / 2) / a));
        }
    }
    ++revision_;
}

std::optional<SDL_FPoint> Hud::barPoint(float windowX, float windowY) const {
    int w = 0, h = 0, pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSize(parent_, &w, &h) || !SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH) || w <= 0 || h <= 0)
        return std::nullopt;
    return SDL_FPoint{windowX * pixelW / w, windowY * pixelH / h - top_};
}

std::optional<HudAction> Hud::buttonAt(float x, float y) const {
    const bool hasUnits = state_.hasSelection || !state_.selection.empty();
    for (const auto& b : buttons_) {
        if (!inside(b.rect, x, y)) continue;
        const bool unitOrder = (b.action == HudAction::stop || b.action == HudAction::holdFire || b.action == HudAction::crouch);
        if (unitOrder && !hasUnits) return std::nullopt;
        return b.action;
    }
    for (int col = 0; col < 3; ++col) {
        if (const auto act = row3Action(col)) {
            if (inside(row3Rect(col), x, y)) return *act;
        }
    }
    if (minimap_.width >= 60 && minimap_.height >= 40) {
        const Rect powerRect{minimap_.x + 2, minimap_.y + minimap_.height - 30, 18, 16};
        if (inside(powerRect, x, y)) return HudAction::menu;
        const Rect pausePip{minimap_.x + 26, minimap_.y + minimap_.height - 12, 12, 12};
        if (inside(pausePip, x, y)) return HudAction::pause;
    }
    return std::nullopt;
}

std::optional<int> Hud::airAt(float x, float y) const {
    for (int k = 0; k < 5; ++k) {
        const auto r = airButtonRect(k);
        if (r.width > 0 && r.height > 0 && inside(r, x, y)) return k;
    }
    return std::nullopt;
}

std::optional<unsigned> Hud::groupAt(float x, float y) const {
    for (const auto& g : state_.groups) {
        const auto r = groupChipRect(g.digit);
        if (r.width > 0 && r.height > 0 && inside(r, x, y)) return g.digit;
    }
    return std::nullopt;
}

std::optional<std::string> Hud::classAt(float x, float y) const {
    for (std::size_t i = 0; i < state_.selection.size(); ++i) {
        const auto r = classCardRect(i);
        if (r.width > 0 && r.height > 0 && inside(r, x, y)) return state_.selection[i].label;
    }
    return std::nullopt;
}

bool Hud::minimapPoint(float x, float y, float& u, float& v, bool clampOutside) const {
    if (!clampOutside) {
        if (!inside(minimap_, x, y)) return false;
        const float rx = std::max(1.0f, minimap_.width * 0.5f);
        const float ry = std::max(1.0f, minimap_.height * 0.5f);
        const float nx = std::abs(x - (minimap_.x + rx)) / rx;
        const float ny = std::abs(y - (minimap_.y + ry)) / ry;
        if (nx + ny > 1.01f) return false;
    }
    u = std::clamp((x - minimap_.x) / std::max(1, minimap_.width), 0.f, 1.f);
    v = std::clamp((y - minimap_.y) / std::max(1, minimap_.height), 0.f, 1.f);
    return true;
}

bool Hud::opaqueAtBarPoint(const std::optional<SDL_FPoint>& p) const {
    if (!p) return false;
    const int ix = int(std::floor(p->x)), iy = int(std::floor(p->y));
    if (ix < 0 || iy < 0 || ix >= int(width_) || iy >= int(height_)) return false;
    const std::size_t idx = (std::size_t(iy) * width_ + std::size_t(ix)) * 4 + 3;
    return idx < rgba_.size() && rgba_[idx] > 0;
}

bool Hud::opaqueAtWindow(float windowX, float windowY) const {
    return opaqueAtBarPoint(barPoint(windowX, windowY));
}

std::optional<HudTip> Hud::tooltipAtWindow(float windowX, float windowY) const {
    const auto p = barPoint(windowX, windowY);
    if (!opaqueAtBarPoint(p)) return std::nullopt;
    if (const auto act = buttonAt(p->x, p->y)) {
        switch (*act) {
        case HudAction::stop:
            return HudTip{tr("Stop (S)"), {tr("Halt movement and clear current orders")}};
        case HudAction::holdFire:
            return HudTip{tr("Hold Fire (H)"), {tr("Toggle weapons hold / fire at will")}};
        case HudAction::crouch:
            return HudTip{tr("Crouch / Prone (C)"), {tr("Take cover and reduce visibility")}};
        case HudAction::mission:
            return HudTip{tr("Mission Objective (M)"), {tr("Focus camera on mission marker")}};
        case HudAction::log:
            return HudTip{tr("Battle Log (L)"), {tr("Open mission event log")}};
        case HudAction::menu:
            return HudTip{tr("Menu (Esc)"), {tr("Open pause menu, save/load & controls")}};
        case HudAction::pause:
            return HudTip{tr("Pause (Ctrl+P)"), {tr("Pause or resume battle clock")}};
        case HudAction::recon:
            return HudTip{tr("Binoculars (B)"), {tr("Extend officer sight range while stationary")}};
        case HudAction::attackMove:
            return HudTip{tr("Attack-Move (A)"), {tr("Advance and engage hostiles along the way")}};
        case HudAction::patrol:
            return HudTip{tr("Patrol (Q)"), {tr("Patrol continuously between current position and target")}};
        case HudAction::attackGround:
            return HudTip{tr("Attack Ground (G)"), {tr("Bombard terrain or buildings with primary ordnance")}};
        case HudAction::unload:
            return HudTip{tr("Unload / Unhitch (U)"), {tr("Disembark passengers, unhitch gun, or evacuate building")}};
        case HudAction::mine:
            return HudTip{tr("Lay AT Mine (E)"), {tr("Sapper plants an anti-tank mine at current tile")}};
        case HudAction::scatter:
            return HudTip{tr("Scatter (D)"), {tr("Disperse infantry immediately away from explosions")}};
        default:
            return std::nullopt;
        }
    }
    if (const auto slot = airAt(p->x, p->y)) {
        static constexpr std::array<const char*, 5> kAirNames{{
            "Recon Plane", "Fighter Strafe", "Bomber Strike", "Paratroopers", "Supply Drop"
        }};
        const int k = *slot;
        if (k >= 0 && k < 5) {
            return HudTip{
                tr(kAirNames[static_cast<std::size_t>(k)]) + " (Alt+" + std::to_string(k + 1) + ")",
                {trf("Ready sorties: {0}", {std::to_string(state_.airReady[static_cast<std::size_t>(k)])}),
                 tr("Click or press F, then click map or minimap")}
            };
        }
    }
    if (const auto grp = groupAt(p->x, p->y)) {
        unsigned cnt = 0;
        for (const auto& g : state_.groups) {
            if (g.digit == *grp) { cnt = g.count; break; }
        }
        return HudTip{
            trf("Control Group {0}", {std::to_string(*grp)}),
            {trf("{0} units — click to select, click again to focus", {std::to_string(cnt)})}
        };
    }
    if (const auto cls = classAt(p->x, p->y)) {
        return HudTip{tr(*cls), {tr("Click to select class • Shift/R-click to exclude")}};
    }
    return std::nullopt;
}

HudResult Hud::handle(const SDL_Event& event) {
    refit();
    const auto id = SDL_GetWindowID(parent_);
    const auto ours = [id](SDL_WindowID other) { return !other || other == id; };
    const auto opaqueAt = [this](const std::optional<SDL_FPoint>& p) -> bool {
        return opaqueAtBarPoint(p);
    };
    HudResult result;
    switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event.window.windowID == id) {
            layout();
            render();
        }
        return result;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        draggingMinimap_ = draggingMinimap_ && event.type != SDL_EVENT_WINDOW_FOCUS_LOST;
        if (pressed_ || hovered_ || pressedAir_ || hoveredAir_ ||
            pressedGroup_ || hoveredGroup_ || pressedClass_ || hoveredClass_) {
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                pressed_.reset();
                pressedAir_.reset();
                pressedGroup_.reset();
                pressedClass_.reset();
            }
            hovered_.reset();
            hoveredAir_.reset();
            hoveredGroup_.reset();
            hoveredClass_.reset();
            render();
        }
        return result;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (!ours(event.button.windowID)) return result;
        const auto p = barPoint(event.button.x, event.button.y);
        if (!opaqueAt(p)) return result;
        result.consumed = true;
        if (event.button.button == SDL_BUTTON_RIGHT && minimapPoint(p->x, p->y, result.u, result.v, false)) {
            result.action = HudAction::orderMinimap;
            return result;
        }
        if (event.button.button == SDL_BUTTON_RIGHT) {
            if (const auto hitClass = classAt(p->x, p->y)) {
                result.action = HudAction::selectClass;
                result.classLabel = *hitClass;
                result.shiftHeld = true;
                return result;
            }
        }
        if (event.button.button != SDL_BUTTON_LEFT) return result;
        if (const auto hitAir = airAt(p->x, p->y)) {
            pressedAir_ = hitAir;
            pressed_.reset();
            pressedGroup_.reset();
            pressedClass_.reset();
            render();
        } else if (const auto hitGroup = groupAt(p->x, p->y)) {
            pressedGroup_ = hitGroup;
            pressed_.reset();
            pressedAir_.reset();
            pressedClass_.reset();
            render();
        } else if (const auto hitClass = classAt(p->x, p->y)) {
            pressedClass_ = hitClass;
            pressed_.reset();
            pressedAir_.reset();
            pressedGroup_.reset();
            render();
        } else if (const auto hit = buttonAt(p->x, p->y)) {
            pressed_ = hit;
            pressedAir_.reset();
            pressedGroup_.reset();
            pressedClass_.reset();
            render();
        } else if (minimapPoint(p->x, p->y, result.u, result.v, false)) {
            draggingMinimap_ = true;
            result.action = HudAction::moveCamera;
        }
        return result;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!ours(event.motion.windowID)) return result;
        const auto p = barPoint(event.motion.x, event.motion.y);
        if (!p) return result;
        // A drag that began on the minimap keeps steering the camera even outside it.
        if (draggingMinimap_ && (event.motion.state & SDL_BUTTON_LMASK)) {
            result.consumed = true;
            result.action = HudAction::moveCamera;
            minimapPoint(p->x, p->y, result.u, result.v, true);
            return result;
        }
        result.consumed = pressed_.has_value() || pressedAir_.has_value() ||
                          pressedGroup_.has_value() || pressedClass_.has_value() || opaqueAt(p);
        const auto over = buttonAt(p->x, p->y);
        const auto overAir = airAt(p->x, p->y);
        const auto overGroup = groupAt(p->x, p->y);
        const auto overClass = classAt(p->x, p->y);
        if (over != hovered_ || overAir != hoveredAir_ || overGroup != hoveredGroup_ || overClass != hoveredClass_) {
            hovered_ = over;
            hoveredAir_ = overAir;
            hoveredGroup_ = overGroup;
            hoveredClass_ = overClass;
            render();
        }
        return result;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!ours(event.button.windowID)) return result;
        const auto p = barPoint(event.button.x, event.button.y);
        const bool overBar = opaqueAt(p);
        if (event.button.button == SDL_BUTTON_LEFT &&
            (pressed_ || pressedAir_ || pressedGroup_ || pressedClass_ || draggingMinimap_)) {
            // The release ends a gesture that began on the bar, wherever it lands.
            result.consumed = true;
            if (pressedAir_) {
                if (p && airAt(p->x, p->y) == pressedAir_) {
                    result.action = HudAction::airSupport;
                    result.airSlot = *pressedAir_;
                }
                pressedAir_.reset();
                render();
            } else if (pressedGroup_) {
                if (p && groupAt(p->x, p->y) == pressedGroup_) {
                    result.action = HudAction::recallGroup;
                    result.groupDigit = int(*pressedGroup_);
                    result.shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                }
                pressedGroup_.reset();
                render();
            } else if (pressedClass_) {
                if (p && classAt(p->x, p->y) == pressedClass_) {
                    result.action = HudAction::selectClass;
                    result.classLabel = *pressedClass_;
                    result.shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                }
                pressedClass_.reset();
                render();
            } else if (pressed_) {
                if (p && buttonAt(p->x, p->y) == pressed_) result.action = *pressed_;
                pressed_.reset();
                render();
            }
            draggingMinimap_ = false;
            return result;
        }
        result.consumed = overBar;
        return result;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!ours(event.wheel.windowID)) return result;
        const auto p = barPoint(event.wheel.mouse_x, event.wheel.mouse_y);
        result.consumed = opaqueAt(p);
        return result;
    }
    default:
        return result;
    }
}

void Hud::draw(VulkanWindow& renderer) {
    refit();
    if (drawn_ == revision_) return;
    renderer.setOverlay(width_, height_, rgba_, OverlaySlot::hud);
    drawn_ = revision_;
}
}
