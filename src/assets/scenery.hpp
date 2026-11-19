#pragma once
#include "assets/indexed.hpp"
#include <array>
#include <filesystem>
#include <memory>

namespace ss2::assets {
class SceneryDepth {
public:
    SceneryDepth() = default;
    SceneryDepth(int width, int height);
    void addBody(const SpriteCoverage&, int sortY);
    void addShadow(const SpriteCoverage&);
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
                                SceneryDepth* depth = nullptr);
}
