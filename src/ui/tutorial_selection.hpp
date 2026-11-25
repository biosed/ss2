#pragma once
#include "assets/infantry.hpp"
#include "assets/vehicles.hpp"
#include "render/window.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ss2 {
class TutorialSelection {
public:
    TutorialSelection(SDL_Window*, VulkanWindow&, std::vector<assets::Infantry>,
                      std::vector<assets::Placement> vehicles = {}, int mapScale = 1,
                      int originX = 4096, std::string missionTitle = "Tutorial I");
    template <typename Vehicles>
        requires std::same_as<std::remove_cvref_t<Vehicles>, std::vector<assets::VehicleUnit>>
    TutorialSelection(SDL_Window* window, VulkanWindow& renderer, std::vector<assets::Infantry> units,
                      Vehicles&& vehicles, int mapScale = 1,
                      int originX = 4096, std::string missionTitle = "Tutorial I")
        : TutorialSelection(window, renderer, std::move(units),
                            std::vector<assets::Placement>{}, mapScale, originX, std::move(missionTitle)) {
        setVehicles(std::forward<Vehicles>(vehicles));
    }

    void setVehicles(std::vector<assets::VehicleUnit> vehicles);
    void setLocalPlayer(int owner);
    int localPlayer() const noexcept { return localPlayer_; }
    const std::set<size_t>& selected() const { return selected_; }
    const std::set<size_t>& selectedVehicles() const { return selectedVehicles_; }
    std::optional<size_t> infantryAt(int mapX, int mapY) const;
    std::optional<size_t> vehicleAt(int mapX, int mapY) const;
    // A unit reported with no health left is removed from the selection and
    // from every saved group, and can no longer be picked, boxed or focused.
    void update(size_t index, const assets::Infantry&);
    void updateVehicle(size_t index, const assets::VehicleUnit& v);
    void addUnit(const assets::Infantry&);
    void addVehicle(const assets::VehicleUnit& v);
    // Player-owned units that have been reported dead so far.
    size_t playerLosses() const;
    void mark(int x, int y);
    bool cancelDrag();
    void recallControlGroup(unsigned digit, bool additive = false);
    std::optional<unsigned> controlGroupOfInfantry(std::size_t index) const noexcept {
        for (unsigned d = 1u; d <= 10u; ++d) {
            const unsigned digit = d % 10u;
            if (groups_[digit].contains(index)) return digit;
        }
        return std::nullopt;
    }
    std::optional<unsigned> controlGroupOfVehicle(std::size_t index) const noexcept {
        for (unsigned d = 1u; d <= 10u; ++d) {
            const unsigned digit = d % 10u;
            if (vehicleGroups_[digit].contains(index)) return digit;
        }
        return std::nullopt;
    }
    void setSelection(std::set<size_t> infantry, std::set<size_t> vehicles);
    std::optional<std::pair<int, int>> selectionCentroid() const;
    size_t selectAllPlayerUnits(bool screenOnly);
    void handle(const SDL_Event&);

private:
    SDL_Window* window_;
    VulkanWindow& renderer_;
    std::vector<assets::Infantry> units_;
    std::vector<assets::VehicleUnit> vehicles_;
    int mapScale_;
    int originX_{4096};
    int localPlayer_{0};
    std::string missionTitle_{"Tutorial I"};
    size_t nextVehicle_{};
    std::set<size_t> selected_;
    std::set<size_t> selectedVehicles_;
    std::array<std::set<size_t>, 10> groups_;
    std::array<std::set<size_t>, 10> vehicleGroups_;
    std::optional<MapRect> marker_;
    std::optional<SDL_FPoint> drag_;
    bool alive(size_t index) const { return units_[index].hp > 0; }
    bool vehicleAlive(size_t index) const { return vehicles_[index].alive && vehicles_[index].hp > 0; }
    void focusInitialUnit();
    void refresh();
    void highlight();
    MapRect box(SDL_FPoint a, SDL_FPoint b) const;
    void release(const SDL_MouseButtonEvent&);
};
}
