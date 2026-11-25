#include "ui/briefing.hpp"
#include "render/window.hpp"

#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ss2 {
namespace {
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;

Layout makeLayout(PangoContext* context, const std::string& text, int size, int maxWidth) {
    Layout layout(pango_layout_new(context), g_object_unref);
    std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
        pango_font_description_from_string("Inter, sans-serif"), pango_font_description_free);
    pango_font_description_set_absolute_size(font.get(), size * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_text(layout.get(), text.c_str(), -1);
    pango_layout_set_width(layout.get(), maxWidth * PANGO_SCALE);
    pango_layout_set_wrap(layout.get(), PANGO_WRAP_WORD_CHAR);
    pango_layout_set_spacing(layout.get(), size * PANGO_SCALE / 5);
    return layout;
}

void color(cairo_t* cr, int r, int g, int b) {
    cairo_set_source_rgb(cr, r / 255., g / 255., b / 255.);
}
}

Briefing::Briefing(SDL_Window* parent) : parent_(parent) {
    if (!parent_) throw std::invalid_argument("Briefing needs a parent window");
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> map(pango_cairo_font_map_new(), g_object_unref);
    fonts_.reset(pango_font_map_create_context(map.get()), g_object_unref);
}

void Briefing::open(std::string text) {
    text_ = std::move(text);
    page_ = 0;
    armed_ = false;
    active_ = true;
    layout();
}

void Briefing::layout() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) throw std::runtime_error(SDL_GetError());
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);

    const int fontPx = std::clamp(int(std::lround(pixelH / 38.0)), 19, 48);
    const int footerPx = std::max(14, 3 * fontPx / 4);
    const int maxPanelW = std::clamp(std::max(360, pixelW / 2), 1, pixelW);
    const int pad = std::min(fontPx, (maxPanelW - 1) / 4);
    const int contentW = std::max(1, maxPanelW - 2 * pad);

    // Measure and draw with the same font metrics at the final drawable resolution.
    auto body = makeLayout(fonts_.get(), text_, fontPx, contentW);
    int bodyW{};
    pango_layout_get_pixel_size(body.get(), &bodyW, nullptr);

    // Keep whole shaped lines together, including explicit breaks and fallback glyphs.
    std::vector<int> lineEnds;
    std::unique_ptr<PangoLayoutIter, decltype(&pango_layout_iter_free)> iter(
        pango_layout_get_iter(body.get()), pango_layout_iter_free);
    do {
        int end{};
        pango_layout_iter_get_line_yrange(iter.get(), nullptr, &end);
        lineEnds.push_back(PANGO_PIXELS_CEIL(end));
    } while (pango_layout_iter_next_line(iter.get()));

    const int footerGap = fontPx;
    const int footerHeight = 2 * footerPx;
    const int availH = std::max(160, pixelH - pixelH / 5);
    const int room = std::max(1, availH * 4 / 5 - 2 * pad - footerGap - footerHeight);
    std::vector<int> pageStarts{0};
    for (size_t i = 1; i < lineEnds.size(); ++i)
        if (lineEnds[i] - pageStarts.back() > room) pageStarts.push_back(lineEnds[i - 1]);
    pages_ = pageStarts.size();
    page_ = std::min(page_, pages_ - 1);
    const int start = pageStarts[page_];
    const int bodyH = (page_ + 1 < pages_ ? pageStarts[page_ + 1] : lineEnds.back()) - start;
    std::string action = "Enter or click to continue";
    if (pages_ > 1) action += "   " + std::to_string(page_ + 1) + " / " + std::to_string(pages_);
    auto footer = makeLayout(fonts_.get(), action, footerPx, contentW);
    int footerW{}, footerH{};
    pango_layout_get_pixel_size(footer.get(), &footerW, &footerH);
    const int panelW = std::min(maxPanelW, std::max(bodyW, footerW) + 2 * pad);
    const int panelH = std::min(availH, pad * 2 + bodyH + footerGap + footerH);
    Surface panel(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, panelW), panelH), cairo_surface_destroy);
    Context graphics(cairo_create(panel.get()), cairo_destroy);
    auto* cr = graphics.get();
    color(cr, 27, 32, 30);
    cairo_paint(cr);
    color(cr, 132, 127, 101);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, .5, .5, panelW - 1, panelH - 1);
    cairo_stroke(cr);
    cairo_save(cr);
    cairo_rectangle(cr, pad, pad, contentW, bodyH);
    cairo_clip(cr);
    cairo_move_to(cr, pad, pad - start);
    color(cr, 236, 236, 228);
    pango_cairo_show_layout(cr, body.get());
    cairo_restore(cr);
    cairo_move_to(cr, pad, pad + bodyH + footerGap);
    color(cr, 201, 190, 152);
    pango_cairo_show_layout(cr, footer.get());
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));

    width_ = unsigned(panelW);
    height_ = unsigned(panelH);
    left_ = (pixelW - panelW) / 2;
    top_ = std::max(0, (availH - panelH) / 2);
    rgba_.resize(std::size_t(panelW) * panelH * 4);
    cairo_surface_flush(panel.get());
    if (!SDL_ConvertPixels(panelW, panelH, SDL_PIXELFORMAT_ARGB8888, cairo_image_surface_get_data(panel.get()),
                           cairo_image_surface_get_stride(panel.get()), SDL_PIXELFORMAT_RGBA32, rgba_.data(), panelW * 4))
        throw std::runtime_error(SDL_GetError());
    ++revision_;
}

bool Briefing::inside(float windowX, float windowY) const {
    int w = 0, h = 0, pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSize(parent_, &w, &h) || !SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH) || w <= 0 || h <= 0)
        return false;
    const double x = double(windowX) * pixelW / w, y = double(windowY) * pixelH / h;
    return x >= left_ && x < left_ + double(width_) && y >= top_ && y < top_ + double(height_);
}

bool Briefing::acknowledge() {
    if (!active_) return false;
    armed_ = false;
    if (page_ + 1 < pages_) {
        ++page_;
        layout();
        return false;
    }
    active_ = false;
    ++revision_;
    return true;
}

bool Briefing::handle(const SDL_Event& event) {
    if (!active_) return false;
    const auto id = SDL_GetWindowID(parent_);
    const auto ours = [id](SDL_WindowID other) { return !other || other == id; };
    switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event.window.windowID == id) layout();
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (!ours(event.key.windowID) || event.key.repeat) return false;
        switch (event.key.key) {
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return acknowledge();
        // Escape leaves the whole panel, whichever page is showing.
        case SDLK_ESCAPE: page_ = pages_ - 1; return acknowledge();
        default: return false;
        }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ours(event.button.windowID) && event.button.button == SDL_BUTTON_LEFT)
            armed_ = inside(event.button.x, event.button.y);
        return false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!ours(event.button.windowID) || event.button.button != SDL_BUTTON_LEFT) return false;
        if (std::exchange(armed_, false) && inside(event.button.x, event.button.y)) return acknowledge();
        return false;
    default:
        return false;
    }
}

void Briefing::draw(VulkanWindow& renderer) {
    if (drawn_ == revision_) return;
    if (active_) renderer.setOverlay(width_, height_, rgba_);
    else renderer.clearOverlay();
    drawn_ = revision_;
}

Notice::Notice(SDL_Window* parent) : parent_(parent) {
    if (!parent_) throw std::invalid_argument("Notice needs a parent window");
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> map(pango_cairo_font_map_new(), g_object_unref);
    fonts_.reset(pango_font_map_create_context(map.get()), g_object_unref);
}

void Notice::show(std::string text, NoticeKind kind, Duration lifetime) {
    if (lifetime < Duration::zero()) throw std::invalid_argument("Negative notice lifetime");
    if (text.empty()) return clear();
    text_ = std::move(text);
    kind_ = kind;
    lifetime_ = lifetime;
    age_ = Duration::zero();
    visible_ = true;
    layout();
}

void Notice::clear() {
    if (!visible_) return;
    visible_ = false;
    ++revision_;
}

bool Notice::advance(Duration elapsed) {
    if (elapsed < Duration::zero()) throw std::invalid_argument("Negative elapsed time");
    if (!visible_ || lifetime_ == Duration::zero()) return false;
    // Saturate: a long stall must not wrap the age around.
    age_ = elapsed > Duration::max() - age_ ? Duration::max() : age_ + elapsed;
    if (age_ < lifetime_) return false;
    clear();
    return true;
}

void Notice::handle(const SDL_Event& event) {
    if (visible_ && event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        event.window.windowID == SDL_GetWindowID(parent_)) layout();
}

void Notice::layout() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) throw std::runtime_error(SDL_GetError());
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);

    const int fontPx = std::clamp(int(std::lround(pixelH / 44.0)), 17, 42);
    const int pad = std::max(4, fontPx * 2 / 3);
    const int bar = std::max(4, fontPx / 4);
    const int icon = fontPx;
    const int maxPanelW = std::clamp(pixelW * 2 / 3, std::min(pixelW, 240), pixelW);
    const int textRoom = std::max(1, maxPanelW - bar - icon - 3 * pad);

    // At most three lines; anything longer ends in an ellipsis.
    auto body = makeLayout(fonts_.get(), text_, fontPx, textRoom);
    pango_layout_set_ellipsize(body.get(), PANGO_ELLIPSIZE_END);
    pango_layout_set_height(body.get(), -3);
    int textW{}, textH{};
    pango_layout_get_pixel_size(body.get(), &textW, &textH);
    textW = std::min(textW, textRoom);

    const int panelW = std::min(pixelW, bar + icon + 3 * pad + textW);
    const int panelH = std::min(pixelH, std::max(icon, textH) + 2 * pad);
    Surface panel(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panelW, panelH), cairo_surface_destroy);
    Context graphics(cairo_create(panel.get()), cairo_destroy);
    auto* cr = graphics.get();

    // The kind is carried by the bar colour and by the mark's shape, not colour alone.
    struct Accent { int r, g, b; };
    const Accent accent = kind_ == NoticeKind::accepted ? Accent{104, 178, 120}
                        : kind_ == NoticeKind::unsupported ? Accent{214, 170, 82} : Accent{212, 96, 88};
    color(cr, 27, 32, 30);
    cairo_paint(cr);
    color(cr, accent.r, accent.g, accent.b);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, .5, .5, panelW - 1, panelH - 1);
    cairo_stroke(cr);
    cairo_rectangle(cr, 0, 0, bar, panelH);
    cairo_fill(cr);

    const double x = bar + pad, y = (panelH - icon) / 2.0, s = icon;
    cairo_set_line_width(cr, std::max(2.0, s / 7));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (kind_) {
    case NoticeKind::accepted: // Tick.
        cairo_move_to(cr, x + s * .12, y + s * .55);
        cairo_line_to(cr, x + s * .40, y + s * .82);
        cairo_line_to(cr, x + s * .88, y + s * .20);
        break;
    case NoticeKind::unsupported: // Circle with a bar.
        cairo_arc(cr, x + s / 2, y + s / 2, s * .40, 0, 2 * 3.14159265358979323846);
        cairo_move_to(cr, x + s * .28, y + s / 2);
        cairo_line_to(cr, x + s * .72, y + s / 2);
        break;
    case NoticeKind::blocked: // Cross.
        cairo_move_to(cr, x + s * .16, y + s * .16);
        cairo_line_to(cr, x + s * .84, y + s * .84);
        cairo_move_to(cr, x + s * .84, y + s * .16);
        cairo_line_to(cr, x + s * .16, y + s * .84);
        break;
    }
    cairo_stroke(cr);

    cairo_move_to(cr, bar + icon + 2 * pad, (panelH - textH) / 2.0);
    color(cr, 236, 236, 228);
    pango_cairo_show_layout(cr, body.get());
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));

    width_ = unsigned(panelW);
    height_ = unsigned(panelH);
    rgba_.resize(std::size_t(panelW) * panelH * 4);
    cairo_surface_flush(panel.get());
    if (!SDL_ConvertPixels(panelW, panelH, SDL_PIXELFORMAT_ARGB8888, cairo_image_surface_get_data(panel.get()),
                           cairo_image_surface_get_stride(panel.get()), SDL_PIXELFORMAT_RGBA32, rgba_.data(), panelW * 4))
        throw std::runtime_error(SDL_GetError());
    ++revision_;
}

void Notice::draw(VulkanWindow& renderer) {
    if (drawn_ == revision_) return;
    if (visible_) renderer.setOverlay(width_, height_, rgba_, OverlaySlot::notice);
    else renderer.clearOverlay(OverlaySlot::notice);
    drawn_ = revision_;
}
}
