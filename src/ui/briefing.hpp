#pragma once

#include <SDL3/SDL.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

struct _PangoContext;

namespace ss2 {
class VulkanWindow;

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
    bool previousPage();

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
    std::shared_ptr<_PangoContext> fonts_;
    std::string text_;
    bool active_ = false, armed_ = false;
    std::size_t page_ = 0, pages_ = 0;
    unsigned width_ = 0, height_ = 0;
    int left_ = 0, top_ = 0;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0, drawn_ = 0;
};

enum class NoticeKind { accepted, unsupported, blocked };

// A short status strip at the bottom centre of the parent window. It is not
// modal: it never consumes input and needs no acknowledgement. It keeps no
// clock of its own; the caller reports elapsed time through advance().
class Notice {
public:
    using Duration = std::chrono::nanoseconds;
    explicit Notice(SDL_Window* parent);

    // Replaces the current notice and restarts its age. A zero lifetime keeps
    // it until clear() or the next show(). Empty text clears.
    void show(std::string text, NoticeKind kind, Duration lifetime = std::chrono::seconds(4));
    void clear();
    bool visible() const { return visible_; }
    NoticeKind kind() const { return kind_; }
    const std::string& text() const { return text_; }

    // Adds caller-measured time to the notice's age and hides it once the
    // lifetime has passed. Returns true when this call hid it.
    bool advance(Duration elapsed);

    // Only follows window size changes. Pass every event; none is consumed.
    void handle(const SDL_Event&);

    // Hands the strip to the renderer, or removes it. Call once per frame.
    void draw(VulkanWindow&);

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    const std::vector<std::uint8_t>& rgba() const { return rgba_; }

private:
    void layout();

    SDL_Window* parent_;
    std::shared_ptr<_PangoContext> fonts_;
    std::string text_;
    NoticeKind kind_ = NoticeKind::accepted;
    Duration lifetime_{}, age_{};
    bool visible_ = false;
    unsigned width_ = 0, height_ = 0;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0, drawn_ = 0;
};
}
