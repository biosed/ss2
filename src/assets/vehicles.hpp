#pragma once
#include "assets/map_image.hpp"
#include "assets/placement.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace ss2::assets {
class SceneryDepth;

struct DirtyRect {
    int left{0}, top{0}, right{0}, bottom{0};
    bool valid() const { return right > left && bottom > top; }
};

struct VehicleUnit {
    std::string id;
    int x{0}, y{0}, hp{100}, ammo{100}, group{0}, owner{0};
    unsigned hullDir{0}, turretDir{0};
    int left{0}, top{0}, width{0}, height{0};
    bool alive{true}, visible{true}, hasCrew{true};
    int expa{0};
    bool operator==(const VehicleUnit&) const = default;
};

class VehicleScene {
public:
    using DirtyRect = ss2::assets::DirtyRect;

    VehicleScene() = default;
    VehicleScene(const std::filesystem::path& gameDir, std::span<const Placement> placements);

    const std::vector<VehicleUnit>& vehicles() const { return units_; }
    std::optional<std::size_t> spawn(const Placement& p);
    void drawAll(MapImage& target, const SceneryDepth* depth = nullptr) const;
    void drawRegion(MapImage& patch, int patchX, int patchY, const SceneryDepth* depth = nullptr) const;
    DirtyRect update(std::size_t index, int x, int y, unsigned hullDir, unsigned turretDir,
                     int hp = -1, int visible = -1, int ammo = -1);
    void setOwner(std::size_t index, int owner);
    std::optional<MapImage> renderUnitPortrait(const std::string& id, unsigned size = 48);

private:
    struct Asset {
        std::vector<std::uint8_t> pck;
        std::vector<std::uint8_t> col;
        std::vector<std::uint8_t> wreckCol;
        std::vector<std::uint8_t> hot;
        std::size_t frameCount{0};
    };

    void initArchives();
    std::optional<std::size_t> ensureAssetLoaded(const std::string& id);

    std::filesystem::path gameDir_;
    std::filesystem::path archive_;
    std::filesystem::path descArchive_;
    bool archivesInitialized_{false};
    bool hasArchive_{false};
    bool hasDesc_{false};
    std::map<std::string, std::size_t> assetById_;
    std::map<std::string, std::optional<std::size_t>> assetByStem_;
    std::set<std::string> skippedIds_;
    std::vector<Asset> assets_;
    std::vector<VehicleUnit> units_;
    std::vector<std::size_t> unitAsset_;
    std::vector<unsigned> unitTrackPhase_;
    std::vector<int> unitMoveAccum_;
};

std::vector<Placement> drawTutorialVehicles(MapImage&, const std::filesystem::path& gameDir, std::span<const Placement>);
std::size_t drawMissionVehicles(MapImage& map, const std::filesystem::path& gameDir,
                                const std::filesystem::path& missionPath,
                                const SceneryDepth* depth = nullptr);
std::optional<MapImage> loadAircraftSprite(const std::filesystem::path& gameDir,
                                           std::string_view unitId,
                                           unsigned dir32);
}
