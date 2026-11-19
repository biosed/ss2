#pragma once
#include "assets/infantry_atlas.hpp"
#include "assets/placement.hpp"
#include "assets/scenery.hpp"
#include <array>
#include <filesystem>
#include <map>
#include <span>
#include <optional>
#include <set>
#include <string>

namespace ss2::assets {
struct Infantry {
    std::string id;
    int x, y, hp, ammo, group, owner;
    int left, top, width, height;
    unsigned direction{}, progress{};
    bool walking{};
    bool visible{true};
};

struct MapPatch {
    int x, y;
    MapImage pixels;
};

class InfantryScene {
public:
    InfantryScene(MapImage background, const std::filesystem::path& gameDir, std::span<const Placement>,
                  bool originalSize = false, MapImage hd = {},
                  const std::filesystem::path& atlases = {}, bool cleanGround = false,
                  SceneryDepth depth = {}, bool gpuSprites = false);
    const std::vector<Infantry>& units() const { return units_; }
    std::size_t paintedCount() const { return painted_; }
    MapImage compose() const;
    using Cursor = std::optional<std::pair<int,int>>;
    std::vector<MapPatch> setInspection(const std::set<std::size_t>& selected, Cursor cursor, int checkerPhase);
    MapPatch update(size_t index, int x, int y, unsigned direction, unsigned progress, bool walking, int hp = -1, int visible = -1);
    MapPatch updateFog(std::span<const std::uint8_t> fog128x128);
private:
    std::map<std::string, InfantryPoses> poses_;
    std::size_t painted_{};
    int resolution_;
    MapImage ground_;
    SceneryDepth depth_;
    std::vector<std::uint8_t> fog_;
    bool gpuSprites_{false};
    std::set<std::size_t> selected_;
    Cursor cursor_;
    int checkerPhase_{};
    std::vector<Infantry> units_;
    const InfantryPose& frame(const Infantry&) const;
    void bounds(Infantry&);
    MapPatch repaint(int left, int top, int right, int bottom) const;
    void paint(MapImage&, int x, int y) const;
};
}
