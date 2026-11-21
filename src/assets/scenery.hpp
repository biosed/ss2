#pragma once
#include "assets/indexed.hpp"
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace ss2::assets {
struct BuildingPlacement {
    int x{}, y{};
    int left{}, top{}, width{}, height{};
    int capacity{4};
    int maxHp{300};
    int ruinLeft{}, ruinTop{};
    MapImage ruinedPatch;
};

class SceneryDepth {
public:
    SceneryDepth() = default;
    SceneryDepth(int width, int height);
    void addBody(const SpriteCoverage&, int sortY);
    void addShadow(const SpriteCoverage&);
    void clearRect(int left, int top, int width, int height);
    bool occludes(int x, int y, int footY, bool revealed = false, int checkerPhase = 0) const;
    bool intersects(int left, int top, int width, int height) const;
private:
    static constexpr int side = 32;
    using Tile = std::array<std::uint16_t, side*side>;
    int width_{}, height_{}, columns_{};
    std::vector<std::unique_ptr<Tile>> tiles_;
    void stamp(const SpriteCoverage&, std::uint16_t bodyDepth);
    std::uint16_t depth(int x, int y) const;
};
std::size_t drawTutorialScenery(MapImage&, const std::filesystem::path& gameDir,
                                SceneryDepth* depth = nullptr,
                                unsigned scheme = 0,
                                std::vector<BuildingPlacement>* buildings = nullptr);
std::size_t drawMissionScenery(MapImage& map, const std::filesystem::path& gameDir,
                               const std::filesystem::path& missionPath,
                               SceneryDepth* depth = nullptr,
                               std::vector<BuildingPlacement>* buildings = nullptr);

struct CraterCatalog {
    std::vector<MapImage> smallCraters;
    std::vector<MapImage> largeCraters;
};

CraterCatalog loadCraterCatalog(const std::filesystem::path& gameDir, unsigned scheme = 0);
}
