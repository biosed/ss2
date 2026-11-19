#pragma once

#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ss2 {
class VulkanWindow;

// Greedy word wrap on spaces and newlines. A column is one UTF-8 code point,
// and words longer than a line are split between code points.
std::vector<std::string> wrapText(std::string_view text, std::size_t columns);

// A modeless text panel centred over the parent window. It never runs its own
// event loop and has no timers: it stays open until acknowledged. While
// active(), the caller should pass events here and keep them from the game.
class Briefing {
public:
    explicit Briefing(SDL_Window* parent);

    void open(std::string text);
    bool active() const { return active_; }

    // Returns true when this event closed the panel. Enter, Space and Escape
    // acknowledge, as does a left click released inside the panel.
    bool handle(const SDL_Event&);

    // Same effect as pressing Enter, for tests and scripted callers: advances
    // to the next page, or closes on the last one. Returns true when closed.
    bool acknowledge();

    // Hands the current panel to the renderer, or removes it once closed.
    // Call once per frame; it uploads only after a change.
    void draw(VulkanWindow&);

    // Panel pixels (RGBA8, opaque) and placement in window pixels.
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    const std::vector<std::uint8_t>& rgba() const { return rgba_; }
    std::size_t page() const { return page_; }
    std::size_t pageCount() const { return pages_; }

private:
    void layout();
    bool inside(float windowX, float windowY) const;

    SDL_Window* parent_;
    std::string text_;
    bool active_ = false, armed_ = false;
    std::size_t page_ = 0, pages_ = 0;
    unsigned width_ = 0, height_ = 0;
    int left_ = 0, top_ = 0;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0, drawn_ = 0;
};
}
