#pragma once
#include "assets/map_image.hpp"
#include "assets/placement.hpp"
#include <array>
#include <filesystem>
#include <map>
#include <span>
#include <string>

namespace ss2::assets {
struct Infantry {
    std::string id;
    int x, y, hp, ammo, group, owner;
    int left, top, width, height;
    unsigned direction{}, progress{};
    bool walking{};
};

struct MapPatch {
    int x, y;
    MapImage pixels;
};

class InfantryScene {
public:
    InfantryScene(MapImage background, const std::filesystem::path& gameDir, std::span<const Placement>);
    const std::vector<Infantry>& units() const { return units_; }
    MapImage compose() const;
    MapPatch update(size_t index, int x, int y, unsigned direction, unsigned progress, bool walking);
private:
    using Frames = std::vector<std::vector<std::uint8_t>>;
    using PoseSet = std::array<Frames, 16>;
    std::map<std::string, PoseSet> poses_;
    std::vector<std::uint8_t> palette_;
    MapImage ground_;
    std::vector<Infantry> units_;
    std::span<const std::uint8_t> frame(const Infantry&) const;
    void bounds(Infantry&);
    void paint(MapImage&, int x, int y) const;
};
}
