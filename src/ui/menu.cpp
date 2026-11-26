#include "ui/menu.hpp"
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include "ui/hidpi.hpp"
#include "render/window.hpp"

#include <pango/pangocairo.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

std::uint32_t readU32(std::span<const std::uint8_t> buf, std::size_t off) {
    if (off > buf.size() || buf.size() - off < 4u) return 0u;
    return std::uint32_t(buf[off]) | (std::uint32_t(buf[off + 1u]) << 8u) |
           (std::uint32_t(buf[off + 2u]) << 16u) | (std::uint32_t(buf[off + 3u]) << 24u);
}

std::uint16_t readU16(std::span<const std::uint8_t> buf, std::size_t off) {
    if (off > buf.size() || buf.size() - off < 2u) return 0u;
    return static_cast<std::uint16_t>(buf[off] | (std::uint16_t(buf[off + 1u]) << 8u));
}

Surface mapImageToCairo(const assets::MapImage& img) {
    if (img.width == 0u || img.height == 0u ||
        img.rgba.size() < std::size_t(img.width) * img.height * 4u) {
        return Surface(nullptr, cairo_surface_destroy);
    }
    Surface surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(img.width), int(img.height)),
                 cairo_surface_destroy);
    if (cairo_surface_status(surf.get()) != CAIRO_STATUS_SUCCESS) {
        return Surface(nullptr, cairo_surface_destroy);
    }
    auto* raw = cairo_image_surface_get_data(surf.get());
    const int stride = cairo_image_surface_get_stride(surf.get());
    if (!SDL_ConvertPixels(int(img.width), int(img.height), SDL_PIXELFORMAT_RGBA32,
                           img.rgba.data(), int(img.width) * 4, SDL_PIXELFORMAT_ARGB8888,
                           raw, stride)) {
        return Surface(nullptr, cairo_surface_destroy);
    }
    cairo_surface_mark_dirty(surf.get());
    return surf;
}

void drawSubRectNearest(cairo_t* cr, cairo_surface_t* src,
                        double sx, double sy, double sw, double sh,
                        double dx, double dy, double dw, double dh) {
    if (!src || sw <= 0.0 || sh <= 0.0 || dw <= 0.0 || dh <= 0.0) return;
    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, dw / sw, dh / sh);
    cairo_rectangle(cr, 0.0, 0.0, sw, sh);
    cairo_clip(cr);
    cairo_set_source_surface(cr, src, -sx, -sy);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
}
} // namespace

struct Menu::Skin {
    Surface frame = Surface(nullptr, cairo_surface_destroy);
    Surface interior = Surface(nullptr, cairo_surface_destroy);
    int width{};
    int height{};
};

Menu::Menu(SDL_Window* parent) : parent_(parent) {
    if (!parent_) throw std::invalid_argument("Menu needs a parent window");
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> map(pango_cairo_font_map_new(), g_object_unref);
    fonts_.reset(pango_font_map_create_context(map.get()), g_object_unref);
}

Menu::~Menu() = default;

bool Menu::hasRetailSkin() const {
    return skin_ && skin_->frame != nullptr;
}

void Menu::loadRetailAssets(const std::filesystem::path& gameDir) {
    if (gameDir.empty()) return;
    try {
        const auto langArchives = assets::resolveLangArchives(gameDir, 0);
        const auto msgbOpt = assets::tryReadSueEntryChain(langArchives, "msgb.dat");
        if (!msgbOpt) return;
        const auto& msgb = *msgbOpt;
        if (msgb.size() < 20u) return;
        const std::span<const std::uint8_t> view(msgb);
        const std::uint32_t count = readU32(view, 0u);
        if (count < 3u || count > 256u || 4u + (std::size_t(count) + 1u) * 4u > view.size()) return;
        const std::size_t palOff = readU32(view, 4u);
        const std::size_t palEnd = readU32(view, 8u);
        const std::size_t frmOff = readU32(view, 12u);
        const std::size_t frmEnd = readU32(view, 16u);
        if (palEnd < palOff || palEnd - palOff < 512u || palEnd > view.size()) return;
        if (frmEnd < frmOff || frmEnd - frmOff < 9u || frmEnd > view.size()) return;
        const auto pal = view.subspan(palOff, palEnd - palOff);
        const auto spr = view.subspan(frmOff, frmEnd - frmOff);
        if (spr[8] != 0xa1u && spr[8] != 0xa2u) return;
        const int ox = static_cast<std::int16_t>(readU16(spr, 0u));
        const int oy = static_cast<std::int16_t>(readU16(spr, 2u));
        const unsigned w = readU16(spr, 4u);
        const unsigned h = readU16(spr, 6u);
        if (w < 16u || h < 16u || w > 1024u || h > 1024u) return;

        assets::MapImage img{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4u, 0u)};
        assets::drawIndexedFrame(img, -ox, -oy, spr, pal.first(512u));
        auto frameSurf = mapImageToCairo(img);
        if (!frameSurf) return;

        Surface interiorSurf(nullptr, cairo_surface_destroy);
        if (w > 16u && h > 16u) {
            const unsigned iw = w - 8u;
            const unsigned ih = h - 8u;
            assets::MapImage tile{iw, ih, std::vector<std::uint8_t>(std::size_t(iw) * ih * 4u, 0u)};
            for (unsigned r = 0u; r < ih; ++r) {
                std::memcpy(tile.rgba.data() + std::size_t(r) * iw * 4u,
                            img.rgba.data() + (std::size_t(r + 4u) * w + 4u) * 4u,
                            std::size_t(iw) * 4u);
            }
            interiorSurf = mapImageToCairo(tile);
        }

        auto next = std::make_unique<Skin>();
        next->frame = std::move(frameSurf);
        next->interior = std::move(interiorSurf);
        next->width = int(w);
        next->height = int(h);
        skin_ = std::move(next);
        if (active_) render();
    } catch (...) {
    }
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
    const int uiScale = std::max(1, static_cast<int>(std::lround(double(pixelH) / 1080.0)));
    const int titlePx = fontPx * 5 / 4, pad = fontPx;
    rowHeight_ = std::max(fontPx + 10, static_cast<int>(std::lround(fontPx * 1.72)));
    const int panelW = std::clamp(std::max(440, pixelW * 42 / 100), 1, pixelW);
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
    const bool hasScroll = int(items_.size()) > visible_;
    if (hasScroll) {
        scrollBarW_ = std::max(14, 16 * uiScale);
        scrollBarX_ = panelW - pad / 2 - scrollBarW_;
        scrollBarY_ = rowsTop_;
        scrollBarH_ = visible_ * rowHeight_;
        scrollArrowH_ = std::min(scrollBarW_, std::max(12, scrollBarH_ / 6));
    } else {
        scrollBarX_ = scrollBarY_ = scrollBarW_ = scrollBarH_ = scrollArrowH_ = 0;
        scrollThumbTop_ = scrollThumbH_ = 0;
    }
    const int contentRight = hasScroll ? (scrollBarX_ - std::max(6, pad / 3)) : (panelW - pad);
    const int rowBoxRight = hasScroll ? (scrollBarX_ - std::max(4, pad / 4)) : (panelW - pad / 2);
    const int contentW = std::max(60, contentRight - pad);

    Surface panel(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panelW, panelH), cairo_surface_destroy);
    Context graphics(cairo_create(panel.get()), cairo_destroy);
    auto* cr = graphics.get();
    const bool skinned = hasRetailSkin();
    const double lw = double(uiScale);
    if (skinned) {
        colour(cr, 18, 22, 18);
        cairo_paint(cr);
        const int border = static_cast<int>(std::clamp<long>(
            std::lround(4.0 * std::clamp(double(fontPx) / 16.0, 1.0, 2.0)),
            4L, std::max(4L, std::min<long>(panelW, panelH) / 4L)));
        const double b = double(border);
        if (skin_->interior) {
            cairo_save(cr);
            cairo_rectangle(cr, b, b, std::max(1.0, panelW - 2.0 * b), std::max(1.0, panelH - 2.0 * b));
            cairo_clip(cr);
            cairo_pattern_t* pat = cairo_pattern_create_for_surface(skin_->interior.get());
            cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
            cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
            if (uiScale > 1) {
                cairo_matrix_t mat;
                cairo_matrix_init_scale(&mat, 1.0 / lw, 1.0 / lw);
                cairo_pattern_set_matrix(pat, &mat);
            }
            cairo_set_source(cr, pat);
            cairo_paint_with_alpha(cr, 0.72);
            cairo_pattern_destroy(pat);
            cairo_set_source_rgba(cr, 12.0 / 255.0, 16.0 / 255.0, 12.0 / 255.0, 0.36);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        const double sw = double(skin_->width);
        const double sh = double(skin_->height);
        constexpr double sB = 4.0;
        const std::array<std::array<double, 8>, 8> strips{{
            {0.0,     0.0,     sB,        sB,        0.0,        0.0,        b,             b},
            {sw - sB, 0.0,     sB,        sB,        panelW - b, 0.0,        b,             b},
            {0.0,     sh - sB, sB,        sB,        0.0,        panelH - b, b,             b},
            {sw - sB, sh - sB, sB,        sB,        panelW - b, panelH - b, b,             b},
            {sB,      0.0,     sw - 2*sB, sB,        b,          0.0,        panelW - 2*b,  b},
            {sB,      sh - sB, sw - 2*sB, sB,        b,          panelH - b, panelW - 2*b,  b},
            {0.0,     sB,      sB,        sh - 2*sB, 0.0,        b,          b,             panelH - 2*b},
            {sw - sB, sB,      sB,        sh - 2*sB, panelW - b, b,          b,             panelH - 2*b},
        }};
        for (const auto& s : strips) {
            drawSubRectNearest(cr, skin_->frame.get(), s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
        }
        cairo_set_source_rgba(cr, 146.0 / 255.0, 132.0 / 255.0, 92.0 / 255.0, 0.55);
        cairo_set_line_width(cr, lw);
        cairo_rectangle(cr, b + lw * 0.5, b + lw * 0.5, std::max(1.0, panelW - 2.0 * b - lw),
                        std::max(1.0, panelH - 2.0 * b - lw));
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, 52.0 / 255.0, 48.0 / 255.0, 30.0 / 255.0, 0.40);
        cairo_rectangle(cr, b + 2.0 * lw, b + 2.0 * lw, std::max(1.0, panelW - 2.0 * b - 4.0 * lw),
                        std::max(1.0, rowsTop_ - b - pad / 4.0 - 2.0 * lw));
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.65);
        text(cr, fonts_.get(), title_, titlePx, true, pad + lw, pad + lw, panelW - 2 * pad, Align::left);
        colour(cr, 232, 214, 158);
        text(cr, fonts_.get(), title_, titlePx, true, pad, pad, panelW - 2 * pad, Align::left);
        colour(cr, 142, 128, 86);
        cairo_rectangle(cr, pad, rowsTop_ - pad / 4.0, panelW - 2 * pad, lw);
        cairo_fill(cr);
    } else {
        colour(cr, 27, 32, 30);
        cairo_paint(cr);
        colour(cr, 132, 127, 101);
        cairo_set_line_width(cr, lw);
        cairo_rectangle(cr, lw * 0.5, lw * 0.5, panelW - lw, panelH - lw);
        cairo_stroke(cr);
        colour(cr, 201, 190, 152);
        text(cr, fonts_.get(), title_, titlePx, true, pad, pad, panelW - 2 * pad, Align::left);
        colour(cr, 70, 76, 68);
        cairo_rectangle(cr, pad, rowsTop_ - pad / 4.0, panelW - 2 * pad, lw);
        cairo_fill(cr);
    }

    for (int i = first_; i < first_ + visible_ && i < int(items_.size()); ++i) {
        const auto& item = items_[i];
        const int y = rowsTop_ + (i - first_) * rowHeight_;
        const bool isSection = !item.selectable && item.value.empty() && !item.label.empty() &&
                               item.id.starts_with("sec:");
        if (i == highlight_) {
            const double rowInset = 2.0 * lw;
            const double rowW = std::max(1.0, double(rowBoxRight) - pad / 2.0);
            if (skinned) {
                colour(cr, i == armed_ ? 98 : 68, i == armed_ ? 104 : 78, i == armed_ ? 68 : 52);
                cairo_rectangle(cr, pad / 2.0, y + rowInset, rowW, std::max(1.0, rowHeight_ - 2.0 * rowInset));
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 182.0 / 255.0, 164.0 / 255.0, 108.0 / 255.0, 0.80);
                cairo_set_line_width(cr, lw);
                cairo_stroke(cr);
                colour(cr, 220, 194, 116);
                cairo_rectangle(cr, pad / 2.0 + 2.0 * lw, y + 4.0 * lw, 3.0 * lw, std::max(1.0, rowHeight_ - 8.0 * lw));
                cairo_fill(cr);
            } else {
                colour(cr, i == armed_ ? 96 : 74, i == armed_ ? 104 : 86, i == armed_ ? 78 : 60);
                cairo_rectangle(cr, pad / 2.0, y + rowInset, rowW, std::max(1.0, rowHeight_ - 2.0 * rowInset));
                cairo_fill(cr);
            }
        }
        const int half = contentW / 2;
        const bool hasValue = !item.value.empty();
        if (isSection) {
            if (skinned) colour(cr, 224, 202, 136);
            else colour(cr, 212, 198, 148);
        } else if (skinned) {
            if (i == highlight_) colour(cr, 246, 244, 232);
            else if (item.selectable) colour(cr, 232, 230, 218);
            else colour(cr, 168, 174, 160);
        } else {
            if (item.selectable) colour(cr, 236, 236, 228); else colour(cr, 170, 176, 168);
        }
        text(cr, fonts_.get(), item.label, isSection ? std::max(12, fontPx - 1) : fontPx,
             isSection || (item.selectable && i == highlight_),
             pad, y + (rowHeight_ - fontPx * 1.3) / 2,
             hasValue ? half : contentW, Align::left);
        if (isSection) {
            cairo_set_source_rgba(cr, 168.0 / 255.0, 152.0 / 255.0, 102.0 / 255.0, 0.55);
            cairo_rectangle(cr, pad, y + rowHeight_ - 2.0 * lw, contentW, lw);
            cairo_fill(cr);
        }
        if (hasValue) {
            if (skinned) colour(cr, i == highlight_ ? 236 : 206, i == highlight_ ? 214 : 192, i == highlight_ ? 148 : 146);
            else colour(cr, 201, 190, 152);
            text(cr, fonts_.get(), item.value, fontPx, false, pad + half, y + (rowHeight_ - fontPx * 1.3) / 2,
                 contentW - half, Align::right);
        }
    }
    // Visible scrollbar track, arrow caps, and proportional thumb when items overflow.
    if (hasScroll) {
        const double sx = double(scrollBarX_), sy = double(scrollBarY_);
        const double sw = double(scrollBarW_), sh = double(scrollBarH_), ah = double(scrollArrowH_);
        cairo_set_source_rgba(cr, 14.0 / 255.0, 18.0 / 255.0, 12.0 / 255.0, 0.85);
        cairo_rectangle(cr, sx, sy, sw, sh);
        cairo_fill_preserve(cr);
        colour(cr, 132, 122, 92);
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);

        auto drawArrowBox = [&](double ay, bool up) {
            colour(cr, 48, 54, 42);
            cairo_rectangle(cr, sx + lw, ay + lw, sw - 2.0 * lw, ah - 2.0 * lw);
            cairo_fill_preserve(cr);
            colour(cr, 132, 122, 92);
            cairo_stroke(cr);
            colour(cr, 224, 210, 164);
            const double cx = sx + sw * 0.5;
            const double cy = ay + ah * 0.5;
            const double r = std::max(3.0, std::min(sw, ah) * 0.25);
            cairo_move_to(cr, cx, up ? (cy - r) : (cy + r));
            cairo_line_to(cr, cx - r, up ? (cy + r * 0.7) : (cy - r * 0.7));
            cairo_line_to(cr, cx + r, up ? (cy + r * 0.7) : (cy - r * 0.7));
            cairo_close_path(cr);
            cairo_fill(cr);
        };
        drawArrowBox(sy, true);
        drawArrowBox(sy + sh - ah, false);

        const int trackY = scrollBarY_ + scrollArrowH_;
        const int trackH = std::max(1, scrollBarH_ - 2 * scrollArrowH_);
        const int maxFirst = std::max(1, int(items_.size()) - visible_);
        scrollThumbH_ = std::clamp(trackH * visible_ / int(items_.size()), std::max(14, int(16 * lw)), trackH);
        const int travel = std::max(0, trackH - scrollThumbH_);
        scrollThumbTop_ = trackY + (travel * first_) / maxFirst;
        colour(cr, 118, 124, 86);
        cairo_rectangle(cr, sx + 2.0 * lw, double(scrollThumbTop_), sw - 4.0 * lw, double(scrollThumbH_));
        cairo_fill_preserve(cr);
        colour(cr, 206, 186, 120);
        cairo_stroke(cr);
    }
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
    if (!SDL_GetWindowSize(parent_, &w, &h) || !SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) return -1;
    const auto phys = hidpi::windowToPhysical(double(windowX), double(windowY), w, h, pixelW, pixelH);
    if (!phys) return -1;
    const double x = phys->x - left_, y = phys->y - top_;
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
        case SDLK_LEFT:
            if (!event.key.repeat && highlight_ >= 0) result.step = -1;
            break;
        case SDLK_RIGHT:
            if (!event.key.repeat && highlight_ >= 0) result.step = 1;
            break;
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
        if (scrollBarW_ > 0) {
            int w = 0, h = 0, pixelW = 0, pixelH = 0;
            if (SDL_GetWindowSize(parent_, &w, &h) && SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) {
                if (const auto phys = hidpi::windowToPhysical(double(event.button.x), double(event.button.y), w, h, pixelW, pixelH)) {
                    const double px = phys->x - left_, py = phys->y - top_;
                    if (px >= scrollBarX_ && px < scrollBarX_ + scrollBarW_ &&
                        py >= scrollBarY_ && py < scrollBarY_ + scrollBarH_) {
                        if (py < scrollBarY_ + scrollArrowH_) scroll(-1);
                        else if (py >= scrollBarY_ + scrollBarH_ - scrollArrowH_) scroll(1);
                        else if (py < scrollThumbTop_) scroll(-std::max(1, visible_ - 1));
                        else if (py >= scrollThumbTop_ + scrollThumbH_) scroll(std::max(1, visible_ - 1));
                        return result;
                    }
                }
            }
        }
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
