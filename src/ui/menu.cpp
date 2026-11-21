#include "ui/menu.hpp"
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

void colour(cairo_t* cr, int r, int g, int b) { cairo_set_source_rgb(cr, r / 255., g / 255., b / 255.); }

enum class Align { left, right };
void text(cairo_t* cr, PangoContext* fonts, const std::string& value, int size, bool bold, double x, double y, int width, Align align) {
    Layout layout(pango_layout_new(fonts), g_object_unref);
    std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
        pango_font_description_from_string("Inter, sans-serif"), pango_font_description_free);
    pango_font_description_set_weight(font.get(), bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(font.get(), size * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_text(layout.get(), value.c_str(), -1);
    pango_layout_set_width(layout.get(), std::max(1, width) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout.get(), true);   // a row never grows a second line
    pango_layout_set_alignment(layout.get(), align == Align::right ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout.get());
}
const std::string empty;
}

Menu::Menu(SDL_Window* parent) : parent_(parent) {
    if (!parent_) throw std::invalid_argument("Menu needs a parent window");
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> map(pango_cairo_font_map_new(), g_object_unref);
    fonts_.reset(pango_font_map_create_context(map.get()), g_object_unref);
}

const std::string& Menu::highlighted() const {
    return highlight_ >= 0 && highlight_ < int(items_.size()) ? items_[highlight_].id : empty;
}

void Menu::highlight(const std::string& id) {
    if (id.empty()) return;
    for (int i = 0; i < int(items_.size()); ++i) {
        if (items_[i].selectable && items_[i].id == id) {
            if (highlight_ != i) { highlight_ = i; follow_ = true; render(); }
            return;
        }
    }
}

void Menu::open(std::string title, std::vector<MenuItem> items) {
    title_ = std::move(title);
    items_ = std::move(items);
    active_ = true;
    armed_ = -1;
    first_ = 0;
    highlight_ = -1;
    for (int i = 0; i < int(items_.size()); ++i)
        if (items_[i].selectable) { highlight_ = i; break; }
    follow_ = true;
    render();
}

void Menu::setItems(std::vector<MenuItem> items) {
    const std::string keep = highlighted();
    items_ = std::move(items);
    highlight_ = -1;
    for (int i = 0; i < int(items_.size()); ++i) {
        if (!items_[i].selectable) continue;
        if (highlight_ < 0) highlight_ = i;
        if (!keep.empty() && items_[i].id == keep) { highlight_ = i; break; }
    }
    armed_ = -1;
    follow_ = true;
    if (active_) render();
}

void Menu::close() {
    if (!active_) return;
    active_ = false;
    armed_ = -1;
    ++revision_;
}

bool Menu::move(int step) {
    if (items_.empty()) return false;
    int at = highlight_;
    for (int tries = 0; tries < int(items_.size()); ++tries) {
        at = (at + step + int(items_.size())) % int(items_.size());
        if (items_[at].selectable) {
            if (at == highlight_) return false;
            highlight_ = at;
            follow_ = true;
            render();
            return true;
        }
    }
    return false;
}

void Menu::scroll(int rows) {
    const int target = std::clamp(first_ + rows, 0, std::max(0, int(items_.size()) - visible_));
    if (target == first_) return;
    first_ = target;
    render();
}

Menu::Rect Menu::rowRect(const std::string& id) const {
    for (int i = first_; i < first_ + visible_ && i < int(items_.size()); ++i)
        if (items_[i].id == id) return {0, rowsTop_ + (i - first_) * rowHeight_, int(width_), rowHeight_};
    return {};
}

void Menu::render() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) throw std::runtime_error(SDL_GetError());
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);
    const int fontPx = std::clamp(int(std::lround(pixelH / 40.0)), 14, 44);
    const int titlePx = fontPx * 5 / 4, pad = fontPx;
    rowHeight_ = fontPx * 2;
    const int panelW = std::clamp(std::max(380, pixelW / 2), 1, pixelW);
    const int chrome = pad + titlePx * 3 / 2 + pad / 2 + pad;        // title block above, margin below
    visible_ = std::clamp((pixelH * 4 / 5 - chrome) / rowHeight_, 1, std::max(1, int(items_.size())));
    // Bring the highlighted row on screen after it moved; free scrolling may leave it behind.
    if (std::exchange(follow_, false) && highlight_ >= 0) {
        if (highlight_ < first_) first_ = highlight_;
        if (highlight_ >= first_ + visible_) first_ = highlight_ - visible_ + 1;
    }
    first_ = std::clamp(first_, 0, std::max(0, int(items_.size()) - visible_));
    rowsTop_ = pad + titlePx * 3 / 2 + pad / 2;
    const int panelH = std::min(pixelH, rowsTop_ + visible_ * rowHeight_ + pad);

    Surface panel(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panelW, panelH), cairo_surface_destroy);
    Context graphics(cairo_create(panel.get()), cairo_destroy);
    auto* cr = graphics.get();
    colour(cr, 27, 32, 30);
    cairo_paint(cr);
    colour(cr, 132, 127, 101);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, .5, .5, panelW - 1, panelH - 1);
    cairo_stroke(cr);
    colour(cr, 201, 190, 152);
    text(cr, fonts_.get(), title_, titlePx, true, pad, pad, panelW - 2 * pad, Align::left);
    colour(cr, 70, 76, 68);
    cairo_rectangle(cr, pad, rowsTop_ - pad / 4.0, panelW - 2 * pad, 1);
    cairo_fill(cr);

    for (int i = first_; i < first_ + visible_ && i < int(items_.size()); ++i) {
        const auto& item = items_[i];
        const int y = rowsTop_ + (i - first_) * rowHeight_;
        if (i == highlight_) {
            colour(cr, i == armed_ ? 96 : 74, i == armed_ ? 104 : 86, i == armed_ ? 78 : 60);
            cairo_rectangle(cr, pad / 2.0, y + 2, panelW - pad, rowHeight_ - 4);
            cairo_fill(cr);
        }
        const int half = (panelW - 2 * pad) / 2;
        const bool hasValue = !item.value.empty();
        if (item.selectable) colour(cr, 236, 236, 228); else colour(cr, 170, 176, 168);
        text(cr, fonts_.get(), item.label, fontPx, item.selectable && i == highlight_, pad, y + (rowHeight_ - fontPx * 1.3) / 2,
             hasValue ? half : panelW - 2 * pad, Align::left);
        if (hasValue) {
            colour(cr, 201, 190, 152);
            text(cr, fonts_.get(), item.value, fontPx, false, pad + half, y + (rowHeight_ - fontPx * 1.3) / 2, half, Align::right);
        }
    }
    // Marks at the edge when rows are scrolled out of view.
    colour(cr, 132, 127, 101);
    if (first_ > 0) { cairo_rectangle(cr, panelW / 2.0 - 12, rowsTop_ - 3, 24, 2); cairo_fill(cr); }
    if (first_ + visible_ < int(items_.size())) { cairo_rectangle(cr, panelW / 2.0 - 12, panelH - pad / 2.0, 24, 2); cairo_fill(cr); }
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));

    width_ = unsigned(panelW);
    height_ = unsigned(panelH);
    left_ = (pixelW - panelW) / 2;
    top_ = (pixelH - panelH) / 2;
    rgba_.resize(std::size_t(panelW) * panelH * 4);
    cairo_surface_flush(panel.get());
    if (!SDL_ConvertPixels(panelW, panelH, SDL_PIXELFORMAT_ARGB8888, cairo_image_surface_get_data(panel.get()),
                           cairo_image_surface_get_stride(panel.get()), SDL_PIXELFORMAT_RGBA32, rgba_.data(), panelW * 4))
        throw std::runtime_error(SDL_GetError());
    for (std::size_t i = 3; i < rgba_.size(); i += 4) rgba_[i] = 255;
    ++revision_;
}

int Menu::rowAt(float windowX, float windowY) const {
    int w = 0, h = 0, pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSize(parent_, &w, &h) || !SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH) || w <= 0 || h <= 0) return -1;
    const double x = double(windowX) * pixelW / w - left_, y = double(windowY) * pixelH / h - top_;
    if (x < 0 || x >= width_ || y < rowsTop_ || rowHeight_ <= 0) return -1;
    const int row = first_ + int((y - rowsTop_) / rowHeight_);
    if (row < first_ || row >= first_ + visible_ || row >= int(items_.size())) return -1;
    return items_[row].selectable ? row : -1;
}

MenuResult Menu::handle(const SDL_Event& event) {
    MenuResult result;
    if (!active_) return result;
    const auto id = SDL_GetWindowID(parent_);
    switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event.window.windowID == id) render();
        return result;
    case SDL_EVENT_KEY_DOWN:
        result.consumed = true;
        switch (event.key.key) {
        case SDLK_UP: if (!move(-1)) scroll(-1); break;
        case SDLK_DOWN: if (!move(1)) scroll(1); break;
        case SDLK_PAGEUP: scroll(-std::max(1, visible_ - 1)); break;
        case SDLK_PAGEDOWN: scroll(std::max(1, visible_ - 1)); break;
        case SDLK_HOME: scroll(-int(items_.size())); break;
        case SDLK_END: scroll(int(items_.size())); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            if (!event.key.repeat && highlight_ >= 0) result.activated = items_[highlight_].id;
            break;
        case SDLK_ESCAPE:
            if (!event.key.repeat) { close(); result.closed = true; }
            break;
        default: break;
        }
        return result;
    case SDL_EVENT_MOUSE_WHEEL:
        result.consumed = true;
        if (event.wheel.y > 0) scroll(-3);
        else if (event.wheel.y < 0) scroll(3);
        return result;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        result.consumed = true;
        return result;
    case SDL_EVENT_MOUSE_MOTION: {
        result.consumed = true;
        const int row = rowAt(event.motion.x, event.motion.y);
        if (row >= 0 && row != highlight_) { highlight_ = row; render(); }
        return result;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        result.consumed = true;
        if (event.button.button != SDL_BUTTON_LEFT) return result;
        const int row = rowAt(event.button.x, event.button.y);
        if (row >= 0) { highlight_ = armed_ = row; render(); }
        return result;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        result.consumed = true;
        if (event.button.button != SDL_BUTTON_LEFT) return result;
        const int row = rowAt(event.button.x, event.button.y);
        const int was = std::exchange(armed_, -1);
        if (was >= 0) {
            if (row == was) result.activated = items_[row].id;
            render();
        }
        return result;
    }
    default:
        return result;
    }
}

void Menu::draw(VulkanWindow& renderer) {
    if (drawn_ == revision_) return;
    if (active_) renderer.setOverlay(width_, height_, rgba_, OverlaySlot::menu);
    else renderer.clearOverlay(OverlaySlot::menu);
    drawn_ = revision_;
}
}
