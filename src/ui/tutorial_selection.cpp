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
bool isSelectionBox(SDL_FPoint a, SDL_FPoint b) {
    // Gold's inclusive screen rectangle becomes a box once either side exceeds 8 pixels.
    return std::abs(std::floor(a.x)-std::floor(b.x)) >= 8 ||
           std::abs(std::floor(a.y)-std::floor(b.y)) >= 8;
}
assets::VehicleUnit makeVehicleUnit(const assets::Placement& p, int mapScale) {
    const unsigned dir = p.direction >= 0 ? static_cast<unsigned>(p.direction) : 0u;
    const int half = 24 * mapScale;
    return assets::VehicleUnit{
        p.id,
        p.x,
        p.y,
        p.hp,
        p.ammo,
        p.group,
        p.owner,
        dir,
        dir,
        p.x - half,
        p.y - half,
        48 * mapScale,
        48 * mapScale,
        p.hp > 0,
        true,
    };
}
MapRect vehicleHitBox(const assets::VehicleUnit& v, int mapScale) {
    if (v.width > 0 && v.height > 0) return {v.left, v.top, v.width, v.height};
    const int size = 32 * mapScale;
    const int half = size / 2;
    return {v.x - half, v.y - half, size, size};
}
}

TutorialSelection::TutorialSelection(SDL_Window* window, VulkanWindow& renderer, std::vector<assets::Infantry> units,
                                     std::vector<assets::Placement> vehicles, int mapScale,
                                     int originX, std::string missionTitle)
    : window_(window), renderer_(renderer), units_(std::move(units)),
      mapScale_(mapScale), originX_(originX), missionTitle_(std::move(missionTitle)) {
    if (mapScale_ <= 0) throw std::invalid_argument("Map scale must be positive");
    vehicles_.reserve(vehicles.size());
    for (const auto& p : vehicles) vehicles_.push_back(makeVehicleUnit(p, mapScale_));
    focusInitialUnit();
    refresh();
}

void TutorialSelection::focusInitialUnit() {
    // Focus the placed German officer (Tutorial I), or any player officer, or first player unit.
    auto first = std::ranges::find_if(units_, [](const auto& u) { return u.owner == 0 && u.hp > 0 && u.id == "dofficer"; });
    if (first == units_.end())
        first = std::ranges::find_if(units_, [](const auto& u) { return u.owner == 0 && u.hp > 0 && u.id.find("officer") != std::string::npos; });
    if (first == units_.end())
        first = std::ranges::find_if(units_, [](const auto& u) { return u.owner == 0 && u.hp > 0; });
    if (first != units_.end()) {
        selected_.insert(static_cast<size_t>(first - units_.begin()));
        renderer_.focusMap(first->x, first->y);
        return;
    }
    auto firstVeh = std::ranges::find_if(vehicles_, [](const auto& v) {
        return v.owner == 0 && v.alive && v.hp > 0 && v.visible;
    });
    if (firstVeh != vehicles_.end()) {
        selectedVehicles_.insert(static_cast<size_t>(firstVeh - vehicles_.begin()));
        renderer_.focusMap(firstVeh->x, firstVeh->y);
    }
}

void TutorialSelection::setVehicles(std::vector<assets::VehicleUnit> vehicles) {
    vehicles_ = std::move(vehicles);
    selectedVehicles_.clear();
    for (auto& group : vehicleGroups_) group.clear();
    nextVehicle_ = 0;
    if (selected_.empty()) {
        const bool hasPlayerInfantry = std::ranges::any_of(units_, [](const auto& u) {
            return u.owner == 0 && u.hp > 0;
        });
        if (!hasPlayerInfantry) {
            auto firstVeh = std::ranges::find_if(vehicles_, [](const auto& v) {
                return v.owner == 0 && v.alive && v.hp > 0 && v.visible;
            });
            if (firstVeh != vehicles_.end()) {
                selectedVehicles_.insert(static_cast<size_t>(firstVeh - vehicles_.begin()));
                renderer_.focusMap(firstVeh->x, firstVeh->y);
            }
        }
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
    const auto [pointerU,pointerV] = tutorialPointerWorld(mapX, mapY, originX_);
    int bestDistance = 26;
    std::optional<size_t> picked;
    for (size_t i = 0; i < units_.size(); ++i) {
        if (!alive(i) || !units_[i].visible) continue;
        const int footX = units_[i].x / mapScale_, footY = units_[i].y / mapScale_;
        const auto [u,v] = tutorialFootWorld(footX, footY, originX_);
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

std::optional<size_t> TutorialSelection::vehicleAt(int mapX, int mapY) const {
    std::optional<size_t> picked;
    long long bestDistSq = 0;
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!vehicleAlive(i) || !v.visible) continue;
        if (!contains(vehicleHitBox(v, mapScale_), mapX, mapY)) continue;
        const long long dx = static_cast<long long>(mapX) - v.x;
        const long long dy = static_cast<long long>(mapY) - v.y;
        const long long distSq = dx * dx + dy * dy;
        if (!picked || distSq < bestDistSq) {
            bestDistSq = distSq;
            picked = i;
        }
    }
    return picked;
}

void TutorialSelection::update(size_t index, const assets::Infantry& unit) {
    units_.at(index) = unit;
    if (!alive(index) || unit.owner != 0) {
        const bool wasSelected = selected_.erase(index) != 0;
        for (auto& group : groups_) group.erase(index);
        // A box being dragged owns the highlight; its release refreshes everything.
        if (wasSelected && !drag_) refresh();
        return;
    }
    if (!unit.visible) {
        if (selected_.erase(index) != 0 && !drag_) refresh();
        return;
    }
    if (selected_.contains(index) && !drag_) highlight();
}

void TutorialSelection::updateVehicle(size_t index, const assets::VehicleUnit& v) {
    vehicles_.at(index) = v;
    const bool dead = !vehicleAlive(index);
    if (dead || v.owner != 0) {
        if (dead) vehicles_[index].alive = false;
        const bool wasSelected = selectedVehicles_.erase(index) != 0;
        for (auto& group : vehicleGroups_) group.erase(index);
        if (wasSelected && !drag_) refresh();
        return;
    }
    if (!v.visible) {
        if (selectedVehicles_.erase(index) != 0 && !drag_) refresh();
        return;
    }
    if (selectedVehicles_.contains(index) && !drag_) highlight();
}

void TutorialSelection::addUnit(const assets::Infantry& unit) {
    units_.push_back(unit);
}

void TutorialSelection::addVehicle(const assets::VehicleUnit& v) {
    vehicles_.push_back(v);
}

size_t TutorialSelection::playerLosses() const {
    const auto infLosses = std::ranges::count_if(units_, [](const auto& u) {
        return u.owner == 0 && u.hp <= 0;
    });
    const auto vehLosses = std::ranges::count_if(vehicles_, [](const auto& v) {
        return v.owner == 0 && (!v.alive || v.hp <= 0);
    });
    return static_cast<size_t>(infLosses + vehLosses);
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

void TutorialSelection::recallControlGroup(unsigned digit, bool additive) {
    if (digit >= 10u) return;
    auto& group = groups_[digit];
    auto& vgroup = vehicleGroups_[digit];
    std::erase_if(group, [&](size_t i) { return i >= units_.size() || !alive(i) || units_[i].owner != 0; });
    std::erase_if(vgroup, [&](size_t i) { return i >= vehicles_.size() || !vehicleAlive(i) || vehicles_[i].owner != 0; });
    if (additive) {
        selected_.insert(group.begin(), group.end());
        selectedVehicles_.insert(vgroup.begin(), vgroup.end());
    } else {
        selected_ = group;
        selectedVehicles_ = vgroup;
    }
    refresh();
}

void TutorialSelection::setSelection(std::set<size_t> infantry, std::set<size_t> vehicles) {
    std::erase_if(infantry, [&](size_t i) {
        return i >= units_.size() || !alive(i) || !units_[i].visible || units_[i].owner != 0;
    });
    std::erase_if(vehicles, [&](size_t i) {
        return i >= vehicles_.size() || !vehicleAlive(i) || !vehicles_[i].visible || vehicles_[i].owner != 0;
    });
    selected_ = std::move(infantry);
    selectedVehicles_ = std::move(vehicles);
    refresh();
}

std::optional<std::pair<int, int>> TutorialSelection::selectionCentroid() const {
    long long sumX = 0, sumY = 0;
    long long count = 0;
    for (const auto i : selected_) {
        if (i < units_.size() && alive(i) && units_[i].owner == 0) {
            sumX += units_[i].x;
            sumY += units_[i].y;
            ++count;
        }
    }
    for (const auto i : selectedVehicles_) {
        if (i < vehicles_.size() && vehicleAlive(i) && vehicles_[i].owner == 0) {
            sumX += vehicles_[i].x;
            sumY += vehicles_[i].y;
            ++count;
        }
    }
    if (count == 0) return std::nullopt;
    return std::pair{static_cast<int>(sumX / count), static_cast<int>(sumY / count)};
}

size_t TutorialSelection::selectAllPlayerUnits(bool screenOnly) {
    MapRect visible{};
    if (screenOnly) {
        int w{}, h{};
        SDL_GetWindowSize(window_, &w, &h);
        visible = box({0, 0}, {float(w), float(h)});
    }
    std::set<size_t> infHits;
    for (size_t i = 0; i < units_.size(); ++i) {
        if (alive(i) && units_[i].visible && units_[i].owner == 0 &&
            (!screenOnly || contains(visible, units_[i].x, units_[i].y))) {
            infHits.insert(i);
        }
    }
    std::set<size_t> vehHits;
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        if (vehicleAlive(i) && vehicles_[i].visible && vehicles_[i].owner == 0 &&
            (!screenOnly || contains(visible, vehicles_[i].x, vehicles_[i].y))) {
            vehHits.insert(i);
        }
    }
    selected_ = std::move(infHits);
    selectedVehicles_ = std::move(vehHits);
    refresh();
    return selected_.size() + selectedVehicles_.size();
}

void TutorialSelection::highlight() {
    // In original Sudden Strike II, selected units are indicated by their floating
    // health bar rather than a green bounding box; reserve highlight() for drag boxes.
    renderer_.highlight({});
}

void TutorialSelection::refresh() {
    highlight();
    std::string title = "Sudden Strike II | " + missionTitle_;
    if (selected_.size() == 1 && selectedVehicles_.empty()) {
        const auto& u = units_[*selected_.begin()];
        title += " | " + u.id + "   HP " + std::to_string(u.hp) + "   Ammo " + std::to_string(u.ammo)
               + "   Group " + std::to_string(u.group) + "   Owner " + std::to_string(u.owner);
    } else if (selected_.empty() && selectedVehicles_.size() == 1) {
        const auto& v = vehicles_[*selectedVehicles_.begin()];
        title += " | " + v.id + "   HP " + std::to_string(v.hp) + "   Ammo " + std::to_string(v.ammo)
               + "   Group " + std::to_string(v.group) + "   Owner " + std::to_string(v.owner);
    } else if (!selected_.empty() && selectedVehicles_.empty()) {
        title += " | " + std::to_string(selected_.size()) + " infantry selected";
    } else if (selected_.empty() && !selectedVehicles_.empty()) {
        title += " | " + std::to_string(selectedVehicles_.size()) + " vehicles selected";
    } else if (!selected_.empty() && !selectedVehicles_.empty()) {
        title += " | " + std::to_string(selected_.size()) + " infantry, "
               + std::to_string(selectedVehicles_.size())
               + (selectedVehicles_.size() == 1 ? " vehicle selected" : " vehicles selected");
    }
    SDL_SetWindowTitle(window_, title.c_str());
}

void TutorialSelection::release(const SDL_MouseButtonEvent& button) {
    const SDL_FPoint end{button.x, button.y};
    const auto mods = SDL_GetModState();
    std::set<size_t> infHits;
    std::set<size_t> vehHits;
    if (isSelectionBox(*drag_, end)) {
        const auto area = box(*drag_, end);
        for (size_t i = 0; i < units_.size(); ++i)
            if (alive(i) && units_[i].visible && units_[i].owner == 0 && contains(area, units_[i].x, units_[i].y))
                infHits.insert(i);
        for (size_t i = 0; i < vehicles_.size(); ++i)
            if (vehicleAlive(i) && vehicles_[i].visible && vehicles_[i].owner == 0 &&
                contains(area, vehicles_[i].x, vehicles_[i].y))
                vehHits.insert(i);
    } else {
        const auto middle = [](float a, float b) {
            return std::floor((std::floor(a) + std::floor(b)) * 0.5f);
        };
        const auto [x, y] = renderer_.mapPosition(middle(drag_->x, end.x), middle(drag_->y, end.y));
        const int mapX = int(std::floor(x)), mapY = int(std::floor(y));
        const auto pickedInf = infantryAt(mapX, mapY);
        const auto pickedVeh = vehicleAt(mapX, mapY);
        if (pickedInf && alive(*pickedInf) && units_[*pickedInf].owner == 0) {
            const auto& u = units_[*pickedInf];
            infHits.insert(*pickedInf);
            if (button.clicks >= 2) {
                int w{}, h{};
                SDL_GetWindowSize(window_, &w, &h);
                const auto visible = box({0, 0}, {float(w), float(h)});
                for (size_t j = 0; j < units_.size(); ++j) {
                    const auto& v = units_[j];
                    if (alive(j) && v.visible && v.owner == u.owner && v.id == u.id &&
                        ((mods & SDL_KMOD_CTRL) || contains(visible, v.x, v.y)))
                        infHits.insert(j);
                }
            }
        } else if (pickedVeh && vehicleAlive(*pickedVeh) && vehicles_[*pickedVeh].owner == 0) {
            const auto& u = vehicles_[*pickedVeh];
            vehHits.insert(*pickedVeh);
            if (button.clicks >= 2) {
                int w{}, h{};
                SDL_GetWindowSize(window_, &w, &h);
                const auto visible = box({0, 0}, {float(w), float(h)});
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    const auto& v = vehicles_[j];
                    if (vehicleAlive(j) && v.visible && v.owner == u.owner && v.id == u.id &&
                        ((mods & SDL_KMOD_CTRL) || contains(visible, v.x, v.y)))
                        vehHits.insert(j);
                }
            }
        }
    }
    if (mods & SDL_KMOD_SHIFT) {
        for (auto i : infHits) {
            if (!selected_.erase(i)) selected_.insert(i);
        }
        for (auto i : vehHits) {
            if (!selectedVehicles_.erase(i)) selectedVehicles_.insert(i);
        }
    } else {
        selected_ = std::move(infHits);
        selectedVehicles_ = std::move(vehHits);
    }
    drag_.reset();
    refresh();
}

void TutorialSelection::handle(const SDL_Event& e) {
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key >= SDLK_0 && e.key.key <= SDLK_9) {
        const size_t slot = static_cast<size_t>(e.key.key - SDLK_0);
        auto& group = groups_[slot];
        auto& vgroup = vehicleGroups_[slot];
        if (e.key.mod & SDL_KMOD_CTRL) {
            group.clear();
            for (const auto i : selected_) if (alive(i) && units_[i].owner == 0) group.insert(i);
            vgroup.clear();
            for (const auto i : selectedVehicles_) if (vehicleAlive(i) && vehicles_[i].owner == 0) vgroup.insert(i);
            refresh();
        } else {
            const bool additive = (e.key.mod & SDL_KMOD_SHIFT) != 0;
            const auto prevInf = selected_;
            const auto prevVeh = selectedVehicles_;
            recallControlGroup(static_cast<unsigned>(slot), additive);
            if (!additive && !e.key.repeat && (!selected_.empty() || !selectedVehicles_.empty()) &&
                selected_ == prevInf && selectedVehicles_ == prevVeh) {
                if (const auto ctr = selectionCentroid()) {
                    renderer_.focusMap(ctr->first, ctr->second, true);
                }
            }
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        drag_ = SDL_FPoint{e.button.x, e.button.y};
    } else if (drag_ && e.type == SDL_EVENT_MOUSE_MOTION) {
        if (isSelectionBox(*drag_, {e.motion.x, e.motion.y}))
            renderer_.highlight({box(*drag_, {e.motion.x, e.motion.y})});
        else
            renderer_.highlight({});
    } else if (drag_ && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        release(e.button);
    } else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        cancelDrag();
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_M && marker_) {
        renderer_.focusMap(marker_->x + marker_->width / 2, marker_->y + marker_->height / 2, true);
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_V && !vehicles_.empty()) {
        const size_t begin = selectedVehicles_.empty()
            ? (nextVehicle_ % vehicles_.size())
            : ((*selectedVehicles_.rbegin() + 1) % vehicles_.size());
        for (size_t i = 0; i < vehicles_.size(); ++i) {
            const size_t next = (begin + i) % vehicles_.size();
            if (vehicles_[next].owner != 0 || !vehicleAlive(next) || !vehicles_[next].visible) continue;
            nextVehicle_ = (next + 1) % vehicles_.size();
            selected_.clear();
            selectedVehicles_ = {next};
            renderer_.focusMap(vehicles_[next].x, vehicles_[next].y, true);
            refresh();
            return;
        }
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_TAB) {
        if (selected_.size() + selectedVehicles_.size() > 1) {
            int x = 0, y = 0, count = 0;
            for (auto i : selected_) {
                if (units_[i].owner != 0 || !alive(i)) continue;
                x += units_[i].x;
                y += units_[i].y;
                ++count;
            }
            for (auto i : selectedVehicles_) {
                if (vehicles_[i].owner != 0 || !vehicleAlive(i)) continue;
                x += vehicles_[i].x;
                y += vehicles_[i].y;
                ++count;
            }
            if (count) {
                renderer_.focusMap(x / count, y / count, true);
                return;
            }
        }
        const bool hasPlayerInfantry = std::ranges::any_of(units_, [](const auto& u) {
            return u.owner == 0 && u.hp > 0;
        });
        if (hasPlayerInfantry) {
            const size_t begin = selected_.empty() ? 0 : (*selected_.rbegin() + 1) % units_.size();
            for (size_t i = 0; i < units_.size(); ++i) {
                const size_t next = (begin + i) % units_.size();
                if (units_[next].owner != 0 || !alive(next) || !units_[next].visible) continue;
                selected_ = {next};
                selectedVehicles_.clear();
                renderer_.focusMap(units_[next].x, units_[next].y, true);
                refresh();
                break;
            }
        } else if (!vehicles_.empty()) {
            const size_t begin = selectedVehicles_.empty() ? 0 : (*selectedVehicles_.rbegin() + 1) % vehicles_.size();
            for (size_t i = 0; i < vehicles_.size(); ++i) {
                const size_t next = (begin + i) % vehicles_.size();
                if (vehicles_[next].owner != 0 || !vehicleAlive(next) || !vehicles_[next].visible) continue;
                selected_.clear();
                selectedVehicles_ = {next};
                renderer_.focusMap(vehicles_[next].x, vehicles_[next].y, true);
                refresh();
                break;
            }
        }
    }
}
}
