#pragma once
#include "assets/infantry.hpp"
#include "render/window.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <optional>
#include <set>

namespace ss2 {
class TutorialSelection {
public:
    TutorialSelection(SDL_Window*, VulkanWindow&, std::vector<assets::Infantry>, std::vector<assets::Placement> vehicles = {});
    const std::set<size_t>& selected() const { return selected_; }
    void update(size_t index, const assets::Infantry&);
    void mark(int x, int y);
    void handle(const SDL_Event&);

private:
    SDL_Window* window_;
    VulkanWindow& renderer_;
    std::vector<assets::Infantry> units_;
    std::vector<assets::Placement> vehicles_;
    size_t nextVehicle_{};
    std::set<size_t> selected_;
    std::array<std::set<size_t>, 10> groups_;
    std::optional<MapRect> marker_;
    std::optional<SDL_FPoint> drag_;
    void refresh();
    void highlight();
    MapRect box(SDL_FPoint a, SDL_FPoint b) const;
    void release(const SDL_MouseButtonEvent&);
};
}
