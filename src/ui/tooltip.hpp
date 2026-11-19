#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct _PangoContext;

namespace ss2 {
class VulkanWindow;

// What a tooltip says. gauge, when 0..1, draws a bar under the title (green, amber or
// red by value, like the gauges on the map); a negative value means no bar.
struct TooltipContent {
    std::string title;
    std::vector<std::string> lines;
    float gauge = -1;
    bool operator==(const TooltipContent&) const = default;
};

// A small box that follows the pointer, e.g. to describe the unit under it. It never
// takes input. The caller decides what is under the pointer and calls show() or hide();
// calling show() every frame with the same content costs nothing but a move.
class Tooltip {
public:
    explicit Tooltip(SDL_Window* parent);

    // Pointer position in window coordinates, as SDL mouse events report it.
    void show(const TooltipContent&, float pointerX, float pointerY);
    void hide();
    bool visible() const { return visible_; }

    // Call when the window's pixel size changes so the text is laid out again.
    void handle(const SDL_Event&);

    void draw(VulkanWindow&);      // once per frame, whether visible or not

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    int left() const { return left_; }   // window pixels, after keeping it on screen
    int top() const { return top_; }
    const std::vector<std::uint8_t>& rgba() const { return rgba_; }

private:
    void render();
    void place();

    SDL_Window* parent_;
    std::shared_ptr<_PangoContext> fonts_;
    TooltipContent content_;
    bool visible_ = false;
    float pointerX_ = 0, pointerY_ = 0;
    unsigned width_ = 0, height_ = 0;
    int left_ = 0, top_ = 0;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0, drawn_ = 0;
    int placedLeft_ = -1, placedTop_ = -1;
};
}
