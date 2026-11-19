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
    TutorialSelection(SDL_Window*, VulkanWindow&, std::vector<assets::Infantry>,
                      std::vector<assets::Placement> vehicles = {}, int mapScale = 1);
    const std::set<size_t>& selected() const { return selected_; }
    std::optional<size_t> infantryAt(int mapX, int mapY) const;
    // A unit reported with no health left is removed from the selection and
    // from every saved group, and can no longer be picked, boxed or focused.
    void update(size_t index, const assets::Infantry&);
    // Player-owned units that have been reported dead so far.
    size_t playerLosses() const;
    void mark(int x, int y);
    bool cancelDrag();
    void handle(const SDL_Event&);

private:
    SDL_Window* window_;
    VulkanWindow& renderer_;
    std::vector<assets::Infantry> units_;
    std::vector<assets::Placement> vehicles_;
    int mapScale_;
    size_t nextVehicle_{};
    std::set<size_t> selected_;
    std::array<std::set<size_t>, 10> groups_;
    std::optional<MapRect> marker_;
    std::optional<SDL_FPoint> drag_;
    bool alive(size_t index) const { return units_[index].hp > 0; }
    void refresh();
    void highlight();
    MapRect box(SDL_FPoint a, SDL_FPoint b) const;
    void release(const SDL_MouseButtonEvent&);
};
}
