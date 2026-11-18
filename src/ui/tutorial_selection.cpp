#include "ui/tutorial_selection.hpp"
#include <algorithm>
#include <cmath>

namespace ss2 {
namespace {
bool contains(MapRect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}
MapRect bounds(const assets::Infantry& u) { return {u.left, u.top, u.width, u.height}; }
}

TutorialSelection::TutorialSelection(SDL_Window* window, VulkanWindow& renderer, std::vector<assets::Infantry> units,
                                     std::vector<assets::Placement> vehicles)
    : window_(window), renderer_(renderer), units_(std::move(units)), vehicles_(std::move(vehicles)) {
    // This editor mission names owner 0 "Player". Focus the placed officer in that player's squad.
    const auto first = std::ranges::find_if(units_, [](const auto& u) { return u.owner == 0 && u.id == "dofficer"; });
    if (first != units_.end()) {
        selected_.insert(first - units_.begin());
        renderer_.focusMap(first->x, first->y);
    }
    refresh();
}

MapRect TutorialSelection::box(SDL_FPoint a, SDL_FPoint b) const {
    const auto [x0, y0] = renderer_.mapPosition(a.x, a.y);
    const auto [x1, y1] = renderer_.mapPosition(b.x, b.y);
    return {int(std::floor(std::min(x0, x1))), int(std::floor(std::min(y0, y1))),
            int(std::ceil(std::abs(x1 - x0))) + 1, int(std::ceil(std::abs(y1 - y0))) + 1};
}

void TutorialSelection::refresh() {
    std::vector<MapRect> rects;
    std::string title = "Sudden Strike II | Tutorial I | Native map";
    for (auto i : selected_) rects.push_back(bounds(units_[i]));
    if (selected_.size() == 1) {
        const auto& u = units_[*selected_.begin()];
        title += " | " + u.id + "   HP " + std::to_string(u.hp) + "   Ammo " + std::to_string(u.ammo)
               + "   Group " + std::to_string(u.group) + "   Owner " + std::to_string(u.owner);
    } else if (!selected_.empty()) title += " | " + std::to_string(selected_.size()) + " infantry selected";
    renderer_.highlight(std::move(rects));
    SDL_SetWindowTitle(window_, title.c_str());
}

void TutorialSelection::release(const SDL_MouseButtonEvent& button) {
    const SDL_FPoint end{button.x, button.y};
    const auto mods = SDL_GetModState();
    std::set<size_t> hits;
    if (std::hypot(end.x - drag_->x, end.y - drag_->y) > 3) {
        const auto area = box(*drag_, end);
        for (size_t i = 0; i < units_.size(); ++i)
            if (units_[i].owner == 0 && contains(area, units_[i].x, units_[i].y)) hits.insert(i);
    } else {
        const auto [x, y] = renderer_.mapPosition(end.x, end.y);
        for (size_t i = units_.size(); i > 0; --i) {
            const auto& u = units_[i - 1];
            if (!contains(bounds(u), int(std::floor(x)), int(std::floor(y)))) continue;
            hits.insert(i - 1);
            if (button.clicks == 2) {
                int w{}, h{};
                SDL_GetWindowSize(window_, &w, &h);
                const auto visible = box({0, 0}, {float(w), float(h)});
                for (size_t j = 0; j < units_.size(); ++j) {
                    const auto& v = units_[j];
                    if (v.owner == u.owner && v.id == u.id &&
                        ((mods & SDL_KMOD_CTRL) || contains(visible, v.x, v.y))) hits.insert(j);
                }
            }
            break;
        }
    }
    if (mods & SDL_KMOD_SHIFT) {
        for (auto i : hits) {
            if (!selected_.erase(i)) selected_.insert(i);
        }
    } else selected_ = std::move(hits);
    drag_.reset();
    refresh();
}

void TutorialSelection::handle(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        drag_ = SDL_FPoint{e.button.x, e.button.y};
    } else if (drag_ && e.type == SDL_EVENT_MOUSE_MOTION) {
        if (std::hypot(e.motion.x - drag_->x, e.motion.y - drag_->y) > 3)
            renderer_.highlight({box(*drag_, {e.motion.x, e.motion.y})});
    } else if (drag_ && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        release(e.button);
    } else if (drag_ && e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        drag_.reset();
        refresh();
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_V && !vehicles_.empty()) {
        const auto& unit = vehicles_[nextVehicle_];
        nextVehicle_ = (nextVehicle_ + 1) % vehicles_.size();
        selected_.clear();
        renderer_.highlight({});
        renderer_.focusMap(unit.x, unit.y);
        const std::string title = "Sudden Strike II | Tutorial I | Native map | " + unit.id +
            "   HP " + std::to_string(unit.hp) + "   Ammo " + std::to_string(unit.ammo);
        SDL_SetWindowTitle(window_, title.c_str());
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_TAB && !units_.empty()) {
        const size_t next = selected_.empty() ? 0 : (*selected_.rbegin() + 1) % units_.size();
        selected_ = {next};
        renderer_.focusMap(units_[next].x, units_[next].y);
        refresh();
    }
}
}
