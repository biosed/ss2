#include "ui/tooltip.hpp"
#include "render/window.hpp"

#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ss2 {
namespace {
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;

void colour(cairo_t* cr, int r, int g, int b) { cairo_set_source_rgb(cr, r / 255., g / 255., b / 255.); }

Layout line(PangoContext* fonts, const std::string& value, int size, bool bold, int maxWidth) {
    Layout layout(pango_layout_new(fonts), g_object_unref);
    std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
        pango_font_description_from_string("Inter, sans-serif"), pango_font_description_free);
    pango_font_description_set_weight(font.get(), bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(font.get(), size * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_text(layout.get(), value.c_str(), -1);
    pango_layout_set_width(layout.get(), maxWidth * PANGO_SCALE);
    pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout.get(), true);
    return layout;
}
}

Tooltip::Tooltip(SDL_Window* parent) : parent_(parent) {
    if (!parent_) throw std::invalid_argument("Tooltip needs a parent window");
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> map(pango_cairo_font_map_new(), g_object_unref);
    fonts_.reset(pango_font_map_create_context(map.get()), g_object_unref);
}

void Tooltip::show(const TooltipContent& content, float pointerX, float pointerY) {
    if (!std::isfinite(pointerX) || !std::isfinite(pointerY)) return;
    const bool changed = !visible_ || !(content == content_);
    content_ = content;
    visible_ = true;
    pointerX_ = pointerX;
    pointerY_ = pointerY;
    if (changed) render();
    place();
}

void Tooltip::hide() {
    if (!visible_) return;
    visible_ = false;
    ++revision_;
}

void Tooltip::handle(const SDL_Event& event) {
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == SDL_GetWindowID(parent_) && visible_) {
        render();
        place();
    }
}

void Tooltip::render() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) throw std::runtime_error(SDL_GetError());
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);
    const int body = std::clamp(int(std::lround(pixelH / 52.0)), 11, 34), title = body * 9 / 8;
    const int uiScale = std::max(1, static_cast<int>(std::lround(double(pixelH) / 1080.0)));
    const int pad = std::max(5, body * 2 / 3), gap = std::max(2, body / 4);
    const int maxText = std::max(40, std::min(pixelW / 3, body * 26));

    std::vector<Layout> rows;
    std::vector<int> heights;
    int textW = 0, textH = 0;
    const auto add = [&](const std::string& value, int size, bool bold) {
        rows.push_back(line(fonts_.get(), value, size, bold, maxText));
        int w = 0, h = 0;
        pango_layout_get_pixel_size(rows.back().get(), &w, &h);
        textW = std::max(textW, std::min(w, maxText));
        heights.push_back(h);
        textH += h + (rows.size() > 1 ? gap : 0);
    };
    if (!content_.title.empty()) add(content_.title, title, true);
    const std::size_t titleRows = rows.size();
    for (const auto& value : content_.lines)
        if (!value.empty()) add(value, body, false);
    const bool gauge = content_.gauge >= 0;
    const int gaugeH = gauge ? std::max(4, body / 3) : 0;
    if (gauge) textW = std::max(textW, body * 6);
    const int w = std::min(pixelW, std::max(textW, 8) + 2 * pad);
    const int h = std::min(pixelH, textH + (gauge ? gaugeH + gap : 0) + 2 * pad);

    Surface panel(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h), cairo_surface_destroy);
    Context graphics(cairo_create(panel.get()), cairo_destroy);
    auto* cr = graphics.get();
    colour(cr, 22, 26, 24);
    cairo_paint(cr);
    colour(cr, 132, 127, 101);
    const double lw = double(uiScale);
    cairo_set_line_width(cr, lw);
    cairo_rectangle(cr, lw * 0.5, lw * 0.5, w - lw, h - lw);
    cairo_stroke(cr);
    int y = pad;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i < titleRows) colour(cr, 236, 230, 200); else colour(cr, 190, 196, 188);
        cairo_move_to(cr, pad, y);
        pango_cairo_show_layout(cr, rows[i].get());
        y += heights[i] + gap;
        if (gauge && i + 1 == titleRows) {
            const double value = std::clamp(double(content_.gauge), 0.0, 1.0);
            colour(cr, 44, 48, 44);
            cairo_rectangle(cr, pad, y, w - 2 * pad, gaugeH);
            cairo_fill(cr);
            if (value > 0.6) colour(cr, 100, 255, 100); else if (value > 0.3) colour(cr, 240, 210, 90); else colour(cr, 232, 72, 60);
            cairo_rectangle(cr, pad, y, std::lround((w - 2 * pad) * value), gaugeH);
            cairo_fill(cr);
            y += gaugeH + gap;
        }
    }
    if (gauge && !titleRows) {   // a gauge with no title goes last
        const double value = std::clamp(double(content_.gauge), 0.0, 1.0);
        colour(cr, 44, 48, 44);
        cairo_rectangle(cr, pad, y, w - 2 * pad, gaugeH);
        cairo_fill(cr);
        if (value > 0.6) colour(cr, 100, 255, 100); else if (value > 0.3) colour(cr, 240, 210, 90); else colour(cr, 232, 72, 60);
        cairo_rectangle(cr, pad, y, std::lround((w - 2 * pad) * value), gaugeH);
        cairo_fill(cr);
    }
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));

    width_ = unsigned(w);
    height_ = unsigned(h);
    rgba_.resize(std::size_t(w) * h * 4);
    cairo_surface_flush(panel.get());
    if (!SDL_ConvertPixels(w, h, SDL_PIXELFORMAT_ARGB8888, cairo_image_surface_get_data(panel.get()),
                           cairo_image_surface_get_stride(panel.get()), SDL_PIXELFORMAT_RGBA32, rgba_.data(), w * 4))
        throw std::runtime_error(SDL_GetError());
    for (std::size_t i = 3; i < rgba_.size(); i += 4) rgba_[i] = 255;
    ++revision_;
}

// Below and to the right of the pointer; flipped to the other side when that would
// leave the window, so the box never hides what it describes.
void Tooltip::place() {
    int w = 0, h = 0, pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSize(parent_, &w, &h) || !SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH) || w <= 0 || h <= 0) return;
    const int px = int(std::lround(double(pointerX_) * pixelW / w)), py = int(std::lround(double(pointerY_) * pixelH / h));
    const int offset = std::max(12, pixelH / 60);
    int left = px + offset, top = py + offset + offset / 2;
    if (left + int(width_) > pixelW) left = px - offset - int(width_);
    if (top + int(height_) > pixelH) top = py - offset - int(height_);
    left_ = std::clamp(left, 0, std::max(0, pixelW - int(width_)));
    top_ = std::clamp(top, 0, std::max(0, pixelH - int(height_)));
}

void Tooltip::draw(VulkanWindow& renderer) {
    if (drawn_ != revision_) {
        if (visible_) renderer.setOverlay(width_, height_, rgba_, OverlaySlot::tooltip);
        else renderer.clearOverlay(OverlaySlot::tooltip);
        drawn_ = revision_;
        placedLeft_ = placedTop_ = -1;
    }
    if (visible_ && (placedLeft_ != left_ || placedTop_ != top_)) {
        renderer.moveOverlay(OverlaySlot::tooltip, left_, top_);
        placedLeft_ = left_;
        placedTop_ = top_;
    }
}
}
