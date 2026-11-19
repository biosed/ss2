#include "ui/briefing.hpp"
#include "render/window.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ss2 {
namespace {
constexpr int glyph = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
constexpr std::size_t preferredColumns = 60;

bool startsCodePoint(char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }

std::size_t codePoints(std::string_view s) {
    return static_cast<std::size_t>(std::ranges::count_if(s, startsCodePoint));
}

// Byte length of the first `count` code points.
std::size_t prefixBytes(std::string_view s, std::size_t count) {
    std::size_t i = 0;
    while (i < s.size() && count) {
        ++i;
        while (i < s.size() && !startsCodePoint(s[i])) ++i;
        --count;
    }
    return i;
}

struct Surface {
    SDL_Surface* surface;
    SDL_Renderer* renderer;
    Surface(int w, int h) : surface(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32)), renderer(nullptr) {
        if (surface) renderer = SDL_CreateSoftwareRenderer(surface);
        if (!renderer) {
            const std::string error = SDL_GetError();
            SDL_DestroySurface(surface);
            throw std::runtime_error(error);
        }
    }
    ~Surface() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
};
}

std::vector<std::string> wrapText(std::string_view text, std::size_t columns) {
    columns = std::max<std::size_t>(columns, 1);
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find('\n', start), text.size());
        std::string line;
        std::size_t used = 0;
        std::size_t i = start;
        while (i < end) {
            if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r') { ++i; continue; }
            auto stop = i;
            while (stop < end && text[stop] != ' ' && text[stop] != '\t' && text[stop] != '\r') ++stop;
            auto word = text.substr(i, stop - i);
            i = stop;
            while (!word.empty()) {
                const auto length = codePoints(word);
                const auto needed = length + (used ? 1 : 0);
                if (used + needed <= columns) {
                    if (used) line += ' ';
                    line += word;
                    used += needed;
                    break;
                }
                if (used) {
                    lines.push_back(std::move(line));
                    line.clear();
                    used = 0;
                    continue;
                }
                // The word alone overflows an empty line.
                const auto bytes = prefixBytes(word, columns);
                lines.emplace_back(word.substr(0, bytes));
                word.remove_prefix(bytes);
            }
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

Briefing::Briefing(SDL_Window* parent) : parent_(parent) {
    if (!parent_) throw std::invalid_argument("Briefing needs a parent window");
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

    // Whole-number text scale: 8 px glyphs at 720p, 16 px at 1080p, 32 px at 2160p.
    const int scale = std::clamp(pixelH / 480, 1, 8);
    const int cell = glyph * scale;
    const int lineHeight = cell * 3 / 2;
    const int pad = cell * 3 / 2;
    const int border = std::max(2, scale);

    const int widest = std::max(pixelW - 2 * cell, 8 * cell + 2 * pad);
    const int panelW = std::min(int(preferredColumns) * cell + 2 * pad, widest);
    const auto columns = static_cast<std::size_t>((panelW - 2 * pad) / cell);
    const auto lines = wrapText(text_, columns);

    const int roomForText = pixelH - 2 * cell - 2 * pad - 2 * lineHeight;
    const auto perPage = static_cast<std::size_t>(std::max(1, roomForText / lineHeight));
    pages_ = (lines.size() + perPage - 1) / perPage;
    page_ = std::min(page_, pages_ - 1);
    const auto shown = std::min(lines.size(), perPage);
    const int panelH = 2 * pad + int(shown) * lineHeight + 2 * lineHeight;

    Surface target(panelW, panelH);
    auto* r = target.renderer;
    SDL_SetRenderDrawColor(r, 214, 196, 140, 255);
    SDL_RenderClear(r);
    const SDL_FRect body{float(border), float(border), float(panelW - 2 * border), float(panelH - 2 * border)};
    SDL_SetRenderDrawColor(r, 18, 22, 28, 255);
    SDL_RenderFillRect(r, &body);
    const float ruleY = float(pad + int(shown) * lineHeight + lineHeight / 2);
    const SDL_FRect rule{float(pad), ruleY, float(panelW - 2 * pad), float(std::max(1, scale / 2))};
    SDL_SetRenderDrawColor(r, 80, 88, 96, 255);
    SDL_RenderFillRect(r, &rule);

    // Text positions are given in glyph-scale units.
    SDL_SetRenderScale(r, float(scale), float(scale));
    SDL_SetRenderDrawColor(r, 236, 236, 228, 255);
    const auto first = page_ * perPage;
    for (std::size_t i = 0; i < shown && first + i < lines.size(); ++i)
        SDL_RenderDebugText(r, float(pad) / scale, float(pad + int(i) * lineHeight) / scale, lines[first + i].c_str());

    std::string footer = page_ + 1 < pages_
        ? "Enter or click: continue (" + std::to_string(page_ + 1) + "/" + std::to_string(pages_) + ")"
        : "Enter or click: close";
    if (footer.size() > columns) footer = page_ + 1 < pages_ ? "[more]" : "[Enter]";
    SDL_SetRenderDrawColor(r, 160, 168, 176, 255);
    SDL_RenderDebugText(r, float(pad) / scale, float(pad + int(shown) * lineHeight + lineHeight) / scale, footer.c_str());
    SDL_SetRenderScale(r, 1, 1);
    if (!SDL_FlushRenderer(r)) throw std::runtime_error(SDL_GetError());

    width_ = unsigned(panelW);
    height_ = unsigned(panelH);
    left_ = (pixelW - panelW) / 2;
    top_ = (pixelH - panelH) / 2;
    rgba_.resize(std::size_t(panelW) * panelH * 4);
    const auto* source = static_cast<const std::uint8_t*>(target.surface->pixels);
    for (int y = 0; y < panelH; ++y)
        std::memcpy(&rgba_[std::size_t(y) * panelW * 4], source + std::size_t(y) * target.surface->pitch, std::size_t(panelW) * 4);
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
}
