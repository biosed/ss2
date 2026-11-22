#pragma once
#include "assets/indexed.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
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

struct BridgeSubCell {
    int subX{}, subY{};
    std::uint8_t mask{};
};

struct BridgePlacement {
    int x{}, y{};
    int u{}, v{};
    int left{}, top{}, width{}, height{};
    int maxHp{3500};
    int patchLeft{}, patchTop{};
    MapImage basePatch;
    std::array<std::uint8_t, 512> palette{};
    std::array<std::vector<std::uint8_t>, 5> stageFrames{};
    std::vector<BridgeSubCell> cells;
};

class SceneryDepth {
public:
    SceneryDepth() = default;
    SceneryDepth(int width, int height);
    void addBody(const SpriteCoverage&, int sortY);
    void addShadow(const SpriteCoverage&);
    void clearRect(int left, int top, int width, int height);
    bool occludes(int x, int y, int footY, bool revealed = false, int checkerPhase = 0) const;
    bool hasShadow(int x, int y) const;
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
                                std::vector<BuildingPlacement>* buildings = nullptr,
                                std::vector<BridgePlacement>* bridges = nullptr);
std::size_t drawMissionScenery(MapImage& map, const std::filesystem::path& gameDir,
                               const std::filesystem::path& missionPath,
                               SceneryDepth* depth = nullptr,
                               std::vector<BuildingPlacement>* buildings = nullptr,
                               std::vector<BridgePlacement>* bridges = nullptr);

struct CraterCatalog {
    std::vector<MapImage> smallCraters;
    std::vector<MapImage> largeCraters;
};

CraterCatalog loadCraterCatalog(const std::filesystem::path& gameDir, unsigned scheme = 0);
}
