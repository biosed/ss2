#pragma once
#include "assets/infantry.hpp"
#include "render/window.hpp"
#include <SDL3/SDL.h>
#include <optional>
#include <set>

namespace ss2 {
class TutorialSelection {
public:
    TutorialSelection(SDL_Window*, VulkanWindow&, std::vector<assets::Infantry>);
    void handle(const SDL_Event&);

private:
    SDL_Window* window_;
    VulkanWindow& renderer_;
    std::vector<assets::Infantry> units_;
    std::set<size_t> selected_;
    std::optional<SDL_FPoint> drag_;
    void refresh();
    MapRect box(SDL_FPoint a, SDL_FPoint b) const;
    void release(const SDL_MouseButtonEvent&);
};
}
