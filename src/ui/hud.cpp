#include "ui/hud.hpp"
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

void rounded(cairo_t* cr, double x, double y, double w, double h, double radius) {
    const double r = std::min(radius, std::min(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
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
    if (unsigned(std::max(pixelW, 1)) == width_ && std::max(pixelH, 1) == top_ + int(height_)) return;
    pressed_.reset();
    hovered_.reset();
    draggingMinimap_ = false;
    layout();
    render();
}

void Hud::update(const HudState& state) {
    refit();
    for (const auto& dot : state.dots)
        if (!std::isfinite(dot.u) || !std::isfinite(dot.v)) throw std::invalid_argument("HUD dot has a non-finite position");
    for (float v : {state.viewU, state.viewV, state.viewW, state.viewH})
        if (!std::isfinite(v)) throw std::invalid_argument("HUD view rectangle has a non-finite value");
    if (state == state_) return;
    state_ = state;
    render();
}

Hud::Rect Hud::buttonRect(HudAction action) const {
    for (const auto& b : buttons_) if (b.action == action) return b.rect;
    return {};
}

void Hud::layout() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) throw std::runtime_error(SDL_GetError());
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);
    width_ = unsigned(pixelW);
    // About a sixth of the window, never so tall that it takes over a small window.
    const int base = std::clamp(int(std::lround(pixelH * 0.17)), std::min(72, pixelH), std::min(240, std::max(72, pixelH / 3)));
    // A strip for the objective, control groups and the latest message sits on top of the sections.
    strip_ = std::clamp(base / 6, 16, 34);
    height_ = unsigned(std::min(base + strip_, std::max(72, pixelH / 3 + strip_)));
    top_ = pixelH - int(height_);
    const int h = int(height_) - strip_, pad = std::max(4, h / 14), oy = strip_;

    // Minimap: as tall as the bar allows, as wide as the map's shape asks, at most 30% of the bar.
    const int mapH = h - 2 * pad;
    const double aspect = overview_.width && overview_.height ? double(overview_.width) / overview_.height : 2.0;
    int mapW = int(std::lround(mapH * aspect)), fittedH = mapH;
    const int widest = std::max(16, pixelW * 3 / 10);
    if (mapW > widest) {
        mapW = widest;
        fittedH = std::max(1, int(std::lround(mapW / aspect)));
    }
    minimap_ = {pad, oy + pad + (mapH - fittedH) / 2, mapW, fittedH};

    // Buttons: one row on the right when there is room, otherwise two rows.
    const int count = int(buttons_.size());
    const int gap = std::max(3, pad / 2);
    const int available = std::max(60, pixelW * 3 / 10);
    const int rows = available / count >= h ? 1 : 2;
    const int perRow = (count + rows - 1) / rows;
    const int buttonH = (h - 2 * pad - (rows - 1) * gap) / rows;
    const int buttonW = std::clamp((available - (perRow - 1) * gap) / perRow, 20, std::max(20, buttonH * 2));
    const int blockW = perRow * buttonW + (perRow - 1) * gap;
    const int startX = pixelW - pad - blockW;
    for (int i = 0; i < count; ++i) {
        const int row = i / perRow, column = i % perRow;
        buttons_[i].rect = {startX + column * (buttonW + gap), oy + pad + row * (buttonH + gap), buttonW, buttonH};
    }
    const int readoutX = minimap_.x + minimap_.width + pad * 2;
    readout_ = {readoutX, oy + pad, std::max(0, startX - pad * 2 - readoutX), h - 2 * pad};
}

void Hud::render() {
    const int w = int(width_), full = int(height_), h = full - strip_;
    Surface target(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, full), cairo_surface_destroy);
    Context graphics(cairo_create(target.get()), cairo_destroy);
    auto* cr = graphics.get();
    const int pad = std::max(4, h / 14);
    const int small = std::clamp(h / 9, 10, 22), body = std::clamp(h / 7, 11, 28);

    // Plate: dark steel with a lighter top edge.
    {
        auto* gradient = cairo_pattern_create_linear(0, 0, 0, full);
        cairo_pattern_add_color_stop_rgb(gradient, 0, 44 / 255., 49 / 255., 46 / 255.);
        cairo_pattern_add_color_stop_rgb(gradient, 1, 24 / 255., 27 / 255., 26 / 255.);
        cairo_set_source(cr, gradient);
        cairo_paint(cr);
        cairo_pattern_destroy(gradient);
        colour(cr, 122, 128, 96);
        cairo_rectangle(cr, 0, 0, w, 2);
        cairo_fill(cr);
        colour(cr, 12, 14, 13);
        cairo_rectangle(cr, 0, 2, w, 1);
        cairo_fill(cr);
    }

    // Strip: objective on the left, control groups in the middle, and on the right either
    // an explanation of the button under the cursor or the latest message.
    {
        const int size = std::clamp(strip_ * 3 / 5, 9, 20), y = (strip_ - size) / 2 - 1 + 2;
        colour(cr, 16, 19, 18);
        cairo_rectangle(cr, 0, 3, w, strip_ - 3);
        cairo_fill(cr);
        colour(cr, 60, 66, 58);
        cairo_rectangle(cr, 0, strip_ - 1, w, 1);
        cairo_fill(cr);
        const int third = w / 3;
        int x = pad;
        if (!state_.objective.empty()) {
            colour(cr, 201, 190, 152);
            x += text(cr, fonts_.get(), "Objective", size, true, x, y, 0) + size / 2;
            colour(cr, 236, 236, 228);
            text(cr, fonts_.get(), state_.objective, size, false, x, y, std::max(1, w * 4 / 9 - x));
        }
        // Control-group chips.
        int chipX = w * 4 / 9 + pad;
        for (const auto& group : state_.groups) {
            const std::string label = std::to_string(group.digit) + ":" + std::to_string(group.count);
            const int chipW = int(label.size()) * size * 2 / 3 + size;
            if (chipX + chipW > w - third) break;
            colour(cr, 58, 66, 52);
            rounded(cr, chipX, 5, chipW, strip_ - 8, 3);
            cairo_fill(cr);
            colour(cr, 236, 236, 228);
            text(cr, fonts_.get(), label, size, true, chipX, y, chipW, true);
            chipX += chipW + size / 3;
        }
        static const std::pair<HudAction, const char*> hints[] = {
            {HudAction::stop, "Stop: cancel movement and attacks (S)"},
            {HudAction::holdFire, "Hold fire: toggle firing at will (H)"},
            {HudAction::crouch, "Crouch: toggle crouching, slower but harder to hit (C)"},
            {HudAction::mission, "Mission: jump to the mission marker (M)"},
            {HudAction::pause, "Pause: stop or resume the mission clock (P)"},
            {HudAction::log, "Log: review recent messages (L)"}};
        std::string right;
        bool hint = false;
        if (hovered_) for (const auto& [action, line] : hints) if (action == *hovered_) { right = line; hint = true; }
        if (!hint && !messages_.empty()) right = messages_.back();
        if (!right.empty()) {
            if (hint) colour(cr, 190, 204, 130); else colour(cr, 170, 176, 168);
            cairo_save(cr);
            cairo_rectangle(cr, w - third, 3, third - pad, strip_ - 4);
            cairo_clip(cr);
            text(cr, fonts_.get(), right, size, false, w - third, y, third - pad);
            cairo_restore(cr);
        }
    }

    // Minimap.
    {
        const auto m = minimap_;
        colour(cr, 14, 17, 16);
        cairo_rectangle(cr, m.x, m.y, m.width, m.height);
        cairo_fill(cr);
        if (overview_.width) {
            const auto picture = image(overview_);
            cairo_save(cr);
            cairo_rectangle(cr, m.x, m.y, m.width, m.height);
            cairo_clip(cr);
            cairo_translate(cr, m.x, m.y);
            cairo_scale(cr, double(m.width) / overview_.width, double(m.height) / overview_.height);
            cairo_set_source_surface(cr, picture.get(), 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        cairo_save(cr);
        cairo_rectangle(cr, m.x, m.y, m.width, m.height);
        cairo_clip(cr);
        const double radius = std::max(1.5, h / 70.0);
        // Hostile dots last, so they stay visible in a crowd.
        for (bool hostile : {false, true})
            for (const auto& dot : state_.dots) {
                if (dot.hostile != hostile) continue;
                if (hostile) colour(cr, 232, 72, 60); else colour(cr, 110, 224, 120);
                cairo_arc(cr, m.x + dot.u * m.width, m.y + dot.v * m.height, radius, 0, 2 * G_PI);
                cairo_fill(cr);
            }
        colour(cr, 245, 245, 240);
        cairo_set_line_width(cr, std::max(1.0, std::round(h / 110.0)));
        // Snapped to whole pixels so the outline stays crisp.
        cairo_rectangle(cr, std::floor(m.x + state_.viewU * m.width) + .5, std::floor(m.y + state_.viewV * m.height) + .5,
                        std::max(2.0, std::round(double(state_.viewW) * m.width)),
                        std::max(2.0, std::round(double(state_.viewH) * m.height)));
        cairo_stroke(cr);
        cairo_restore(cr);
        colour(cr, 96, 102, 80);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, m.x - .5, m.y - .5, m.width + 1, m.height + 1);
        cairo_stroke(cr);
    }

    // Selection readout.
    if (readout_.width > 40) {
        const auto r = readout_;
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_clip(cr);
        unsigned total = 0;
        for (const auto& c : state_.selection) total += c.count;
        colour(cr, 201, 190, 152);
        const std::string title = total ? std::to_string(total) + (total == 1 ? " soldier selected" : " soldiers selected")
                                        : std::string("No units selected");
        text(cr, fonts_.get(), title, small, true, r.x, r.y, r.width);

        // Status line along the bottom of the readout.
        const int statusY = r.y + r.height - small - 4;
        std::string status = state_.crouched ? "Crouched" : "Standing";
        status += state_.holdFire ? "   Hold fire" : "   Fire at will";
        if (state_.paused) status += "   Paused";
        colour(cr, 170, 176, 168);
        const int statusW = text(cr, fonts_.get(), status, small, false, r.x, statusY, r.width);
        if (state_.losses) {
            colour(cr, 232, 120, 104);
            text(cr, fonts_.get(), "Losses " + std::to_string(state_.losses), small, true, r.x + statusW + small, statusY,
                 std::max(1, r.width - statusW - small));
        }

        // One card per class, as many as fit.
        const int cardsY = r.y + small + pad, cardsH = statusY - pad - cardsY;
        if (cardsH >= 20 && !state_.selection.empty()) {
            // Cards share the width: full size when there is room, narrower to fit every
            // class, and only when even the narrowest will not fit does the last slot say "+N more".
            const int portrait = cardsH, gap = pad;
            const int widest = std::max(portrait + small * 9, 120), narrowest = std::max(portrait + small * 5, 90);
            const int classes = int(state_.selection.size());
            const int cardW = std::clamp((r.width - (classes - 1) * gap) / classes, narrowest, widest);
            const int fit = std::max(1, (r.width + gap) / (cardW + gap));
            int x = r.x;
            for (int i = 0; i < int(state_.selection.size()) && i < fit; ++i, x += cardW + gap) {
                const auto& c = state_.selection[i];
                const bool last = i == fit - 1 && int(state_.selection.size()) > fit;
                colour(cr, 18, 21, 20);
                rounded(cr, x, cardsY, cardW, cardsH, 4);
                cairo_fill(cr);
                if (last) {
                    colour(cr, 170, 176, 168);
                    text(cr, fonts_.get(), "+" + std::to_string(state_.selection.size() - fit + 1) + " more", body, false,
                         x + pad, cardsY + (cardsH - body) / 2.0, cardW - 2 * pad);
                    break;
                }
                const auto found = portraits_.find(c.label);
                if (found != portraits_.end()) {
                    const auto picture = image(found->second);
                    cairo_save(cr);
                    rounded(cr, x, cardsY, portrait, cardsH, 4);
                    cairo_clip(cr);
                    cairo_translate(cr, x, cardsY);
                    cairo_scale(cr, double(portrait) / found->second.width, double(cardsH) / found->second.height);
                    cairo_set_source_surface(cr, picture.get(), 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else {
                    colour(cr, 58, 66, 52);
                    rounded(cr, x, cardsY, portrait, cardsH, 4);
                    cairo_fill(cr);
                    colour(cr, 201, 190, 152);
                    const std::string initial = c.label.empty() ? std::string("?") : c.label.substr(0, 1);
                    const int letter = std::max(10, cardsH / 2);
                    text(cr, fonts_.get(), initial, letter, true, x, cardsY + (cardsH - letter * 1.25) / 2.0, portrait, true);
                }
                const int tx = x + portrait + pad, tw = cardW - portrait - 2 * pad;
                colour(cr, 236, 236, 228);
                text(cr, fonts_.get(), c.label, small, true, tx, cardsY + 2, tw);
                colour(cr, 190, 194, 184);
                text(cr, fonts_.get(), "x" + std::to_string(c.count), small, false, tx, cardsY + 2 + small * 1.3, tw);
                const auto bar = [&](int row, float value, int red, int green, int blue) {
                    const double barH = std::max(3.0, cardsH / 9.0), y = cardsY + cardsH - (2 - row) * (barH + 3) - 2;
                    colour(cr, 44, 48, 46);
                    cairo_rectangle(cr, tx, y, tw, barH);
                    cairo_fill(cr);
                    colour(cr, red, green, blue);
                    cairo_rectangle(cr, tx, y, tw * std::clamp(value, 0.f, 1.f), barH);
                    cairo_fill(cr);
                };
                const float hp = std::clamp(c.health, 0.f, 1.f);
                // Health runs from red through amber to green; ammunition is a fixed amber.
                bar(0, c.health, int(220 - 110 * hp), int(80 + 144 * hp), 90);
                bar(1, c.ammo, 214, 170, 82);
            }
        }
        cairo_restore(cr);
    }

    // Buttons. A toggle that is on, or a button being pressed, is drawn lit.
    for (const auto& b : buttons_) {
        const bool on = (b.action == HudAction::holdFire && state_.holdFire) || (b.action == HudAction::crouch && state_.crouched) ||
                        (b.action == HudAction::pause && state_.paused);
        const bool down = pressed_ && *pressed_ == b.action;
        const auto r = b.rect;
        const bool hover = hovered_ && *hovered_ == b.action;
        if (down) colour(cr, 96, 104, 78); else if (on) colour(cr, 74, 86, 60);
        else if (hover) colour(cr, 66, 72, 66); else colour(cr, 52, 57, 54);
        rounded(cr, r.x, r.y, r.width, r.height, 5);
        cairo_fill(cr);
        if (on) colour(cr, 190, 204, 130); else colour(cr, 96, 102, 92);
        cairo_set_line_width(cr, on ? 2 : 1);
        rounded(cr, r.x + .5, r.y + .5, r.width - 1, r.height - 1, 5);
        cairo_stroke(cr);
        const int keySize = std::clamp(r.height / 3, 9, 26), labelSize = std::clamp(r.height / 5, 8, 18);
        colour(cr, 236, 236, 228);
        text(cr, fonts_.get(), b.key, keySize, true, r.x, r.y + r.height * 0.12, r.width, true);
        colour(cr, 190, 194, 184);
        text(cr, fonts_.get(), b.label, labelSize, false, r.x + 3, r.y + r.height - labelSize * 1.3 - std::max(3, r.height / 10),
             r.width - 6, true);
    }
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));

    rgba_.resize(std::size_t(w) * full * 4);
    cairo_surface_flush(target.get());
    if (!SDL_ConvertPixels(w, full, SDL_PIXELFORMAT_ARGB8888, cairo_image_surface_get_data(target.get()),
                           cairo_image_surface_get_stride(target.get()), SDL_PIXELFORMAT_RGBA32, rgba_.data(), w * 4))
        throw std::runtime_error(SDL_GetError());
    // The bar is copied to the screen as-is, so it must be fully opaque.
    for (std::size_t i = 3; i < rgba_.size(); i += 4) rgba_[i] = 255;
    ++revision_;
}

std::optional<SDL_FPoint> Hud::barPoint(float windowX, float windowY) const {
    int w = 0, h = 0, pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSize(parent_, &w, &h) || !SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH) || w <= 0 || h <= 0)
        return std::nullopt;
    return SDL_FPoint{windowX * pixelW / w, windowY * pixelH / h - top_};
}

std::optional<HudAction> Hud::buttonAt(float x, float y) const {
    for (const auto& b : buttons_) if (inside(b.rect, x, y)) return b.action;
    return std::nullopt;
}

bool Hud::minimapPoint(float x, float y, float& u, float& v, bool clampOutside) const {
    if (!clampOutside && !inside(minimap_, x, y)) return false;
    u = std::clamp((x - minimap_.x) / std::max(1, minimap_.width), 0.f, 1.f);
    v = std::clamp((y - minimap_.y) / std::max(1, minimap_.height), 0.f, 1.f);
    return true;
}

HudResult Hud::handle(const SDL_Event& event) {
    refit();
    const auto id = SDL_GetWindowID(parent_);
    const auto ours = [id](SDL_WindowID other) { return !other || other == id; };
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
        if (pressed_ || hovered_) {
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) pressed_.reset();
            hovered_.reset();
            render();
        }
        return result;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (!ours(event.button.windowID)) return result;
        const auto p = barPoint(event.button.x, event.button.y);
        if (!p || p->y < 0 || p->y >= float(height_)) return result;
        result.consumed = true;
        if (event.button.button != SDL_BUTTON_LEFT) return result;
        if (const auto hit = buttonAt(p->x, p->y)) {
            pressed_ = hit;
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
        result.consumed = pressed_.has_value() || (p->y >= 0 && p->y < float(height_));
        const auto over = buttonAt(p->x, p->y);
        if (over != hovered_) {
            hovered_ = over;
            render();
        }
        return result;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!ours(event.button.windowID)) return result;
        const auto p = barPoint(event.button.x, event.button.y);
        const bool overBar = p && p->y >= 0 && p->y < float(height_);
        if (event.button.button == SDL_BUTTON_LEFT && (pressed_ || draggingMinimap_)) {
            // The release ends a gesture that began on the bar, wherever it lands.
            result.consumed = true;
            if (pressed_) {
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
        result.consumed = p && p->y >= 0 && p->y < float(height_);
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
