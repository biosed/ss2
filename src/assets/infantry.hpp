#pragma once
#include "assets/infantry_atlas.hpp"
#include "assets/placement.hpp"
#include "assets/scenery.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ss2::assets {
struct Infantry {
    std::string id;
    int x, y, hp, ammo, group, owner;
    int left, top, width, height;
    unsigned direction{}, progress{};
    bool walking{};
    bool visible{true};
    int expa{0};
    bool operator==(const Infantry&) const = default;
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
                  SceneryDepth depth = {}, const std::map<std::string, unsigned>* gpuSlots = nullptr,
                  unsigned fogW = 128, unsigned fogH = 128);
    const std::vector<Infantry>& units() const { return units_; }
    std::size_t paintedCount() const { return painted_; }
    MapImage compose() const;
    using Cursor = std::optional<std::pair<int,int>>;
    using VehicleLayer = std::function<void(MapImage& patch, int patchX, int patchY, const SceneryDepth* depth)>;
    void setVehicleLayer(VehicleLayer layer);
    MapPatch repaintRegion(int left, int top, int right, int bottom) const;
    MapPatch stampGroundPatch(int x, int y, const MapImage& patch, bool clearDepth = true);
    MapImage copyGroundRect(int x, int y, int width, int height) const;
    MapPatch stampAlphaOverlay(int centerX, int centerY, const MapImage& sprite);
    std::vector<MapPatch> setInspection(const std::set<std::size_t>& selected, Cursor cursor, int checkerPhase);
    std::optional<std::pair<size_t, MapPatch>> spawn(const Placement& p);
    MapPatch update(size_t index, int x, int y, unsigned direction, unsigned progress, bool walking,
                    int hp = -1, int visible = -1, int ammo = -1);
    void setFogViewerOwner(int owner);
    MapPatch updateFog(std::span<const std::uint8_t> fog);
    std::optional<MapImage> renderUnitPortrait(const std::string& id, unsigned size = 48) const;
private:
    std::filesystem::path gameDir_;
    bool originalSize_{false};
    std::filesystem::path atlases_;
    std::filesystem::path archive_;
    std::filesystem::path descArchive_;
    bool hasArchive_{false};
    bool hasDesc_{false};
    std::vector<std::uint8_t> palette_;
    std::set<std::string> paintedTypes_;
    std::set<std::string> skippedTypes_;
    std::map<std::string, InfantryPoses> poses_;
    std::size_t painted_{};
    int resolution_;
    MapImage ground_;
    SceneryDepth depth_;
    unsigned fogW_{128}, fogH_{128};
    int fogViewerOwner_{0};
    std::vector<std::uint8_t> fog_;
    const std::map<std::string, unsigned>* gpuSlots_{nullptr};
    bool gpuDrawn(const std::string& id) const { return gpuSlots_ && gpuSlots_->contains(id); }
    VehicleLayer vehicleLayer_;
    std::set<std::size_t> selected_;
    Cursor cursor_;
    int checkerPhase_{};
    std::vector<Infantry> units_;
    bool ensureClassLoaded(const std::string& id, std::array<int, 4>* firstBounds = nullptr);
    const InfantryPose& frame(const Infantry&) const;
    void bounds(Infantry&);
    MapPatch repaint(int left, int top, int right, int bottom) const;
    void paint(MapImage&, int x, int y) const;
};

std::vector<Infantry> loadMissionInfantry(const std::filesystem::path& gameDir,
                                          const std::filesystem::path& missionPath);
}
