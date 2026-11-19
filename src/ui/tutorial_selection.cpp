#include "ui/tutorial_selection.hpp"
#include "sim/tutorial_coordinates.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ss2 {
namespace {
bool contains(MapRect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}
MapRect bounds(const assets::Infantry& u) { return {u.left, u.top, u.width, u.height}; }
bool isSelectionBox(SDL_FPoint a, SDL_FPoint b) {
    // Gold's inclusive screen rectangle becomes a box once either side exceeds 8 pixels.
    return std::abs(std::floor(a.x)-std::floor(b.x)) >= 8 ||
           std::abs(std::floor(a.y)-std::floor(b.y)) >= 8;
}
}

TutorialSelection::TutorialSelection(SDL_Window* window, VulkanWindow& renderer, std::vector<assets::Infantry> units,
                                     std::vector<assets::Placement> vehicles, int mapScale)
    : window_(window), renderer_(renderer), units_(std::move(units)), vehicles_(std::move(vehicles)), mapScale_(mapScale) {
    if (mapScale_ <= 0) throw std::invalid_argument("Map scale must be positive");
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

std::optional<size_t> TutorialSelection::infantryAt(int x, int y) const {
    // Gold 0x1009b880 compares map-space distances to the feet. For ordinary
    // ground it requires distance - (infantry radius 5 / 2) < 24.
    const int mapX = int(std::floor(double(x) / mapScale_));
    const int mapY = int(std::floor(double(y) / mapScale_));
    const auto [pointerU,pointerV] = tutorialPointerWorld(mapX,mapY);
    int bestDistance = 26;
    std::optional<size_t> picked;
    for (size_t i = 0; i < units_.size(); ++i) {
        if (!alive(i)) continue;
        const int footX = units_[i].x / mapScale_, footY = units_[i].y / mapScale_;
        const auto [u,v] = tutorialFootWorld(footX,footY);
        const int du = std::abs(u-pointerU), dv = std::abs(v-pointerV);
        const int distance = std::max(du,dv) + std::min(du,dv)/2;
        // The original strict comparison keeps the earlier unit in the live list on a tie.
        if (distance < bestDistance) {
            bestDistance = distance;
            picked = i;
        }
    }
    return picked;
}

void TutorialSelection::update(size_t index, const assets::Infantry& unit) {
    units_.at(index) = unit;
    if (!alive(index)) {
        const bool wasSelected = selected_.erase(index) != 0;
        for (auto& group : groups_) group.erase(index);
        // A box being dragged owns the highlight; its release refreshes everything.
        if (wasSelected && !drag_) refresh();
        return;
    }
    if (selected_.contains(index) && !drag_) highlight();
}

size_t TutorialSelection::playerLosses() const {
    return static_cast<size_t>(std::ranges::count_if(units_, [](const auto& u) { return u.owner == 0 && u.hp <= 0; }));
}

bool TutorialSelection::cancelDrag() {
    if (!drag_) return false;
    drag_.reset();
    highlight();
    return true;
}

void TutorialSelection::mark(int x, int y) {
    marker_ = MapRect{x - 12, y - 6, 25, 13};
    refresh();
}

void TutorialSelection::highlight() {
    std::vector<MapRect> rects;
    if (marker_) rects.push_back(*marker_);
    for (auto i : selected_) rects.push_back(bounds(units_[i]));
    renderer_.highlight(std::move(rects));
}

void TutorialSelection::refresh() {
    highlight();
    std::string title = "Sudden Strike II | Tutorial I";
    if (selected_.size() == 1) {
        const auto& u = units_[*selected_.begin()];
        title += " | " + u.id + "   HP " + std::to_string(u.hp) + "   Ammo " + std::to_string(u.ammo)
               + "   Group " + std::to_string(u.group) + "   Owner " + std::to_string(u.owner);
    } else if (!selected_.empty()) title += " | " + std::to_string(selected_.size()) + " infantry selected";
    SDL_SetWindowTitle(window_, title.c_str());
}

void TutorialSelection::release(const SDL_MouseButtonEvent& button) {
    const SDL_FPoint end{button.x, button.y};
    const auto mods = SDL_GetModState();
    std::set<size_t> hits;
    if (isSelectionBox(*drag_,end)) {
        const auto area = box(*drag_, end);
        for (size_t i = 0; i < units_.size(); ++i)
            if (alive(i) && units_[i].owner == 0 && contains(area, units_[i].x, units_[i].y)) hits.insert(i);
    } else {
        const auto middle = [](float a, float b) {
            return std::floor((std::floor(a)+std::floor(b))*0.5f);
        };
        const auto [x, y] = renderer_.mapPosition(middle(drag_->x,end.x),middle(drag_->y,end.y));
        const auto picked = infantryAt(int(std::floor(x)), int(std::floor(y)));
        if (picked) {
            const auto& u = units_[*picked];
            hits.insert(*picked);
            if (button.clicks == 2) {
                int w{}, h{};
                SDL_GetWindowSize(window_, &w, &h);
                const auto visible = box({0, 0}, {float(w), float(h)});
                for (size_t j = 0; j < units_.size(); ++j) {
                    const auto& v = units_[j];
                    if (alive(j) && v.owner == u.owner && v.id == u.id &&
                        ((mods & SDL_KMOD_CTRL) || contains(visible, v.x, v.y))) hits.insert(j);
                }
            }
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
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key >= SDLK_0 && e.key.key <= SDLK_9) {
        auto& group = groups_[e.key.key - SDLK_0];
        if (e.key.mod & SDL_KMOD_CTRL) {
            group.clear();
            for (const auto i : selected_) if (alive(i) && units_[i].owner == 0) group.insert(i);
        } else if (e.key.mod & SDL_KMOD_SHIFT) selected_.insert(group.begin(), group.end());
        else selected_ = group;
        refresh();
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        drag_ = SDL_FPoint{e.button.x, e.button.y};
    } else if (drag_ && e.type == SDL_EVENT_MOUSE_MOTION) {
        if (isSelectionBox(*drag_,{e.motion.x,e.motion.y}))
            renderer_.highlight({box(*drag_, {e.motion.x, e.motion.y})});
    } else if (drag_ && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        release(e.button);
    } else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        cancelDrag();
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_M && marker_) {
        renderer_.focusMap(marker_->x+marker_->width/2, marker_->y+marker_->height/2,true);
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
        if (selected_.size() > 1) {
            int x = 0, y = 0, count = 0;
            for (auto i : selected_) {
                if (units_[i].owner != 0) continue;
                x += units_[i].x;
                y += units_[i].y;
                ++count;
            }
            if (count) {
                renderer_.focusMap(x/count,y/count,true);
                return;
            }
        }
        const size_t begin = selected_.empty() ? 0 : (*selected_.rbegin()+1)%units_.size();
        for (size_t i = 0; i < units_.size(); ++i) {
            const size_t next = (begin+i)%units_.size();
            if (units_[next].owner != 0 || !alive(next)) continue;
            selected_ = {next};
            renderer_.focusMap(units_[next].x, units_[next].y,true);
            refresh();
            break;
        }
    }
}
}
