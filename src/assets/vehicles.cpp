#include "assets/vehicles.hpp"
#include "assets/indexed.hpp"
#include "assets/scenery.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;

std::uint16_t u16(View data, std::size_t pos) {
    if (pos > data.size() || data.size() - pos < 2)
        throw std::runtime_error("Truncated vehicle sprite header");
    return static_cast<std::uint16_t>(std::uint16_t(data[pos]) | (std::uint16_t(data[pos + 1]) << 8));
}

std::int16_t i16(View data, std::size_t pos) {
    return static_cast<std::int16_t>(u16(data, pos));
}

std::uint32_t u32(View data, std::size_t pos) {
    if (pos > data.size() || data.size() - pos < 4)
        throw std::runtime_error("Truncated vehicle asset record");
    return std::uint32_t(data[pos]) | (std::uint32_t(data[pos + 1]) << 8) |
           (std::uint32_t(data[pos + 2]) << 16) | (std::uint32_t(data[pos + 3]) << 24);
}

std::int32_t i32(View data, std::size_t pos) {
    return static_cast<std::int32_t>(u32(data, pos));
}

View pckFrame(View pck, std::size_t index) {
    const std::size_t first = u32(pck, 0);
    if (first < 8 || (first % 4) != 0 || first > pck.size())
        throw std::runtime_error("Invalid vehicle PCK frame table");
    const std::size_t count = first / 4 - 1;
    if (index >= count)
        throw std::runtime_error("Vehicle PCK frame index out of range");
    const std::size_t start = u32(pck, index * 4);
    const std::size_t end = u32(pck, (index + 1) * 4);
    if (start < first || end < start || end > pck.size())
        throw std::runtime_error("Invalid vehicle PCK frame range");
    return pck.subspan(start, end - start);
}

std::optional<Bytes> tryReadSue(std::span<const std::filesystem::path> archives, std::string_view entry) {
    return tryReadSueEntryChain(archives, entry);
}

std::optional<std::string> resolveUnitStem(std::span<const std::filesystem::path> descArchives, bool hasDesc,
                                           std::string_view unitId) {
    if (hasDesc) {
        if (const auto bytes = tryReadSue(descArchives, "UNITS\\" + std::string(unitId))) {
            std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            while (!text.empty()) {
                const auto eol = text.find_first_of("\r\n");
                std::string_view line = (eol == std::string_view::npos) ? text : text.substr(0, eol);
                text = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                    line.remove_prefix(1);
                if (line.size() > 4 && equalsIgnoreCaseAscii(line.substr(0, 4), "file") &&
                    (line[4] == ' ' || line[4] == '\t')) {
                    line.remove_prefix(5);
                    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                        line.remove_prefix(1);
                    const auto end = line.find_first_of(" \t\r\n");
                    const auto stem = (end == std::string_view::npos) ? line : line.substr(0, end);
                    if (stem.empty() || equalsIgnoreCaseAscii(stem, "nofile")) return std::nullopt;
                    return std::string(stem);
                }
            }
        }
    }
    return std::string(unitId);
}

std::uint8_t frameTag(View frame) {
    return frame.size() >= 9u ? frame[8] : 0u;
}

bool isRenderableFrame(View frame) {
    if (frame.size() < 9u) return false;
    if (frame[8] != 0xa1u && frame[8] != 0xa2u) return false;
    const std::uint32_t w = std::uint32_t(frame[4]) | (std::uint32_t(frame[5]) << 8);
    const std::uint32_t h = std::uint32_t(frame[6]) | (std::uint32_t(frame[7]) << 8);
    return w > 1u || h > 1u;
}

bool isRenderableShadowFrame(View frame) {
    if (frame.size() < 9u || frame[8] != 0xa3u) return false;
    const std::uint32_t w = std::uint32_t(frame[4]) | (std::uint32_t(frame[5]) << 8);
    const std::uint32_t h = std::uint32_t(frame[6]) | (std::uint32_t(frame[7]) << 8);
    return w >= 1u && h >= 1u && w <= 2048u && h <= 2048u && (w > 1u || h > 1u);
}

std::pair<int, int> readHotOffset(View hot, unsigned hull) {
    const int hotDx = i32(hot, std::size_t(hull) * 8u);
    const int hotDy = i32(hot, std::size_t(hull) * 8u + 4u);
    if (hotDx < -2048 || hotDx > 2048 || hotDy < -2048 || hotDy > 2048)
        throw std::runtime_error("Vehicle turret offset exceeds sprite bounds");
    return {hotDx, hotDy};
}

enum class Kind { Tank, Artillery, HeavyMg };

struct Spec {
    const char* pck;
    const char* col;
    const char* hot;
    Kind kind;
};

struct LoadedAsset {
    Bytes pck;
    Bytes col;
    Bytes hot;
    Kind kind;
};

struct Vehicle {
    const LoadedAsset* asset;
    int x, y;
    unsigned dir;
};

struct MissionVehicleAsset {
    Bytes pck;
    Bytes col;
    Bytes hot;
    std::size_t frameCount;
};

struct MissionVehicle {
    const MissionVehicleAsset* asset;
    int x, y;
    unsigned dir;
};

struct FrameLayer {
    View frame;
    int ox{0};
    int oy{0};
    std::size_t frameIdx{0};
    bool isTurret{false};
};

struct LayerList {
    std::array<FrameLayer, 4> items{};
    std::size_t count{0};

    void push(View frame, int ox, int oy, std::size_t frameIdx = 0, bool isTurret = false) {
        if (count < items.size() && isRenderableFrame(frame)) {
            items[count++] = {frame, ox, oy, frameIdx, isTurret};
        }
    }
    void pushIdx(View pck, std::size_t frameIdx, int ox, int oy, bool isTurret = false) {
        push(pckFrame(pck, frameIdx), ox, oy, frameIdx, isTurret);
    }
};

struct WreckLayers {
    LayerList layers;
    bool dedicated{false};
};

struct ShadowLayerList {
    std::array<FrameLayer, 2> items{};
    std::size_t count{0};

    void push(View frame, int ox, int oy, std::size_t frameIdx = 0, bool isTurret = false) {
        if (count < items.size() && isRenderableShadowFrame(frame)) {
            items[count++] = {frame, ox, oy, frameIdx, isTurret};
        }
    }
    void pushIdx(View pck, std::size_t frameIdx, int ox, int oy, bool isTurret = false) {
        push(pckFrame(pck, frameIdx), ox, oy, frameIdx, isTurret);
    }
};

std::optional<MissionVehicleAsset> tryLoadVehicleStemAsset(std::span<const std::filesystem::path> archives,
                                                           std::string_view stem) {
    auto pck = tryReadSue(archives, std::string(stem) + ".PCK");
    auto col = tryReadSue(archives, std::string(stem) + ".COL");
    if (!pck || !col || col->size() != 512u || pck->size() < 8u) return std::nullopt;
    const std::size_t first = u32(*pck, 0);
    if (first < 8u || (first % 4u) != 0u || first > pck->size()) return std::nullopt;
    const std::size_t frameCount = first / 4u - 1u;
    if (frameCount != 7u && frameCount < 32u) return std::nullopt;
    auto hot = tryReadSue(archives, std::string(stem) + ".hot");
    return MissionVehicleAsset{std::move(*pck), std::move(*col),
                               hot ? std::move(*hot) : Bytes{}, frameCount};
}

LayerList collectVehicleLayers(View pck, View hot, std::size_t frameCount,
                               int x, int y, unsigned hullDir, unsigned turretDir,
                               unsigned trackPhase = 0u) {
    const unsigned hull = hullDir & 31u;
    const unsigned turret = turretDir & 31u;
    const unsigned phase = trackPhase & 1u;
    LayerList layers;
    const auto pushWithPhase = [&](std::size_t base0, std::size_t base1) {
        const std::size_t idx = (phase ? base1 : base0) + hull;
        const auto fr = pckFrame(pck, idx);
        if (isRenderableFrame(fr)) {
            layers.push(fr, x, y, idx, false);
        } else if (phase) {
            layers.pushIdx(pck, base0 + hull, x, y, false);
        }
    };
    switch (frameCount) {
    case 7u:
        layers.pushIdx(pck, 0u, x, y);
        layers.pushIdx(pck, 1u, x, y, true);
        break;
    case 160u:
        pushWithPhase(0u, 32u);
        break;
    case 192u:
        layers.pushIdx(pck, hull, x, y);
        pushWithPhase(32u, 64u);
        break;
    case 256u:
        if (frameTag(pckFrame(pck, 160u + hull)) == 0xa2u) {
            pushWithPhase(32u, 96u);
            pushWithPhase(0u, 64u);
        } else {
            layers.pushIdx(pck, 32u + hull, x, y);
            layers.pushIdx(pck, hull, x, y);
        }
        break;
    case 288u: {
        const std::size_t tIdx = 192u + turret;
        const auto block6 = pckFrame(pck, tIdx);
        if (frameTag(block6) == 0xa2u) {
            layers.pushIdx(pck, hull, x, y);
            pushWithPhase(32u, 64u);
            if (isRenderableFrame(block6) && hot.size() >= 32u * 8u) {
                const auto [hotDx, hotDy] = readHotOffset(hot, hull);
                layers.push(block6, x + hotDx, y + hotDy, tIdx, true);
            }
        } else {
            pushWithPhase(32u, 96u);
            pushWithPhase(0u, 64u);
        }
        break;
    }
    case 352u:
        if (frameTag(pckFrame(pck, 64u + hull)) == 0xa2u) {
            layers.pushIdx(pck, hull, x, y);
            layers.pushIdx(pck, 32u + hull, x, y);
            layers.pushIdx(pck, 160u + turret, x, y, true);
        } else {
            layers.pushIdx(pck, hull, x, y);
        }
        break;
    case 448u:
        layers.pushIdx(pck, hull, x, y);
        layers.pushIdx(pck, 32u + hull, x, y);
        layers.pushIdx(pck, 128u + turret, x, y, true);
        break;
    default:
        if (frameCount < 96u) {
            const auto block0 = pckFrame(pck, hull);
            if (isRenderableFrame(block0)) {
                layers.push(block0, x, y, hull, false);
            } else if (32u + hull < frameCount) {
                layers.pushIdx(pck, 32u + hull, x, y);
            }
        } else {
            layers.pushIdx(pck, hull, x, y);
        }
        break;
    }
    return layers;
}

WreckLayers collectWreckLayers(View pck, View hot, std::size_t frameCount,
                               int x, int y, unsigned hullDir, unsigned turretDir) {
    const unsigned hull = hullDir & 31u;
    const unsigned turret = turretDir & 31u;
    WreckLayers out;
    switch (frameCount) {
    case 7u:
        out.layers.pushIdx(pck, 4u, x, y);
        out.layers.pushIdx(pck, 5u, x, y);
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers.pushIdx(pck, 0u, x, y);
            out.layers.pushIdx(pck, 1u, x, y);
        }
        break;
    case 66u:
        out.layers.pushIdx(pck, 32u, x, y);
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers.pushIdx(pck, hull, x, y);
        }
        break;
    case 128u:
        if (frameTag(pckFrame(pck, 32u + hull)) == 0xa3u) {
            out.layers.pushIdx(pck, 64u + hull, x, y);
        } else {
            out.layers.pushIdx(pck, 32u + hull, x, y);
        }
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers.pushIdx(pck, hull, x, y);
        }
        break;
    case 160u:
        out.layers.pushIdx(pck, 64u + hull, x, y);
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers.pushIdx(pck, hull, x, y);
        }
        break;
    case 192u:
        out.layers.pushIdx(pck, 96u + hull, x, y);
        out.layers.pushIdx(pck, 128u + hull, x, y);
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers.pushIdx(pck, hull, x, y);
            out.layers.pushIdx(pck, 32u + hull, x, y);
        }
        break;
    case 256u:
        if (frameTag(pckFrame(pck, 160u + hull)) == 0xa2u) {
            out.layers.pushIdx(pck, 160u + hull, x, y);
            out.layers.pushIdx(pck, 128u + hull, x, y);
        } else {
            out.layers.pushIdx(pck, 128u + hull, x, y);
        }
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers.pushIdx(pck, 32u + hull, x, y);
            out.layers.pushIdx(pck, hull, x, y);
        }
        break;
    case 288u:
        if (frameTag(pckFrame(pck, 192u + turret)) == 0xa2u) {
            out.layers.pushIdx(pck, 96u + hull, x, y);
            out.layers.pushIdx(pck, 128u + hull, x, y);
            if (hot.size() >= 32u * 8u) {
                const auto wt = pckFrame(pck, 224u + turret);
                if (isRenderableFrame(wt)) {
                    const auto [hotDx, hotDy] = readHotOffset(hot, hull);
                    out.layers.push(wt, x + hotDx, y + hotDy, 224u + turret, true);
                }
            }
            if (out.layers.count > 0u) {
                out.dedicated = true;
            } else {
                out.layers.pushIdx(pck, hull, x, y);
                out.layers.pushIdx(pck, 32u + hull, x, y);
            }
        } else {
            out.layers.pushIdx(pck, 160u + hull, x, y);
            out.layers.pushIdx(pck, 128u + hull, x, y);
            if (out.layers.count > 0u) {
                out.dedicated = true;
            } else {
                out.layers.pushIdx(pck, 32u + hull, x, y);
                out.layers.pushIdx(pck, hull, x, y);
            }
        }
        break;
    case 352u:
        out.layers.pushIdx(pck, 288u + hull, x, y);
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers = collectVehicleLayers(pck, hot, frameCount, x, y, hullDir, turretDir);
        }
        break;
    case 448u:
        out.layers.pushIdx(pck, 384u + hull, x, y);
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers = collectVehicleLayers(pck, hot, frameCount, x, y, hullDir, turretDir);
        }
        break;
    case 512u:
        out.layers.pushIdx(pck, 128u + hull, x, y);
        if (out.layers.count > 0u) {
            out.dedicated = true;
        } else {
            out.layers.pushIdx(pck, hull, x, y);
        }
        break;
    default:
        if (32u + hull < frameCount && isRenderableFrame(pckFrame(pck, 32u + hull))) {
            out.layers.pushIdx(pck, 32u + hull, x, y);
            out.dedicated = true;
        } else if (hull < frameCount) {
            out.layers.pushIdx(pck, hull, x, y);
        }
        break;
    }
    return out;
}

ShadowLayerList collectVehicleShadowLayers(View pck, View hot, std::size_t frameCount,
                                           int x, int y, unsigned hullDir, unsigned turretDir, bool alive) {
    const unsigned hull = hullDir & 31u;
    const unsigned turret = turretDir & 31u;
    ShadowLayerList shadows;
    switch (frameCount) {
    case 7u:
        shadows.pushIdx(pck, 6u, x, y);
        break;
    case 66u:
        shadows.pushIdx(pck, alive ? 64u : 65u, x, y);
        break;
    case 128u:
        if (frameTag(pckFrame(pck, 32u + hull)) == 0xa3u) {
            shadows.pushIdx(pck, (alive ? 32u : 96u) + hull, x, y);
        } else {
            shadows.pushIdx(pck, (alive ? 64u : 96u) + hull, x, y);
        }
        break;
    case 160u:
        shadows.pushIdx(pck, (alive ? 96u : 128u) + hull, x, y);
        break;
    case 192u:
        shadows.pushIdx(pck, 160u + hull, x, y);
        break;
    case 256u:
        if (frameTag(pckFrame(pck, 160u + hull)) == 0xa3u) {
            shadows.pushIdx(pck, (alive ? 160u : 224u) + hull, x, y);
        } else {
            shadows.pushIdx(pck, (alive ? 192u : 224u) + hull, x, y);
        }
        break;
    case 288u: {
        const auto block6 = pckFrame(pck, 192u + turret);
        if (frameTag(block6) == 0xa2u) {
            shadows.pushIdx(pck, 160u + hull, x, y);
            const bool showTurretShadow = alive
                ? isRenderableFrame(block6)
                : isRenderableFrame(pckFrame(pck, 224u + turret));
            if (showTurretShadow && hot.size() >= 32u * 8u) {
                const auto [hotDx, hotDy] = readHotOffset(hot, hull);
                shadows.pushIdx(pck, 256u + turret, x + hotDx, y + hotDy, true);
            }
        } else {
            shadows.pushIdx(pck, (alive ? 192u : 256u) + hull, x, y);
        }
        break;
    }
    case 352u:
        if (frameTag(pckFrame(pck, 64u + hull)) == 0xa2u) {
            if (alive) {
                shadows.pushIdx(pck, 96u + hull, x, y);
                shadows.pushIdx(pck, 224u + turret, x, y, true);
            } else {
                shadows.pushIdx(pck, 320u + hull, x, y);
            }
        } else {
            shadows.pushIdx(pck, (alive ? 64u : 320u) + hull, x, y);
        }
        break;
    case 448u:
        if (alive) {
            shadows.pushIdx(pck, 64u + hull, x, y);
            shadows.pushIdx(pck, 256u + turret, x, y, true);
        } else {
            shadows.pushIdx(pck, 416u + hull, x, y);
        }
        break;
    case 512u:
        shadows.pushIdx(pck, (alive ? 192u : 224u) + hull, x, y);
        break;
    default:
        break;
    }
    return shadows;
}

Bytes makeWreckPalette(View col) {
    Bytes out(col.begin(), col.end());
    for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
        const std::uint16_t w = static_cast<std::uint16_t>(out[i] | (unsigned(out[i + 1]) << 8));
        const unsigned r5 = std::min(31u, unsigned(((w >> 11) & 31u) * 48u / 100u));
        const unsigned g6 = std::min(63u, unsigned(((w >> 5) & 63u) * 44u / 100u));
        const unsigned b5 = std::min(31u, unsigned((w & 31u) * 40u / 100u));
        const std::uint16_t dark = static_cast<std::uint16_t>((r5 << 11) | (g6 << 5) | b5);
        out[i] = static_cast<std::uint8_t>(dark & 0xFFu);
        out[i + 1] = static_cast<std::uint8_t>((dark >> 8) & 0xFFu);
    }
    return out;
}

void updateVehicleBounds(VehicleUnit& unit, View pck, View hot, std::size_t frameCount, unsigned trackPhase) {
    const bool alive = unit.alive && unit.hp > 0;
    const auto layers = alive
        ? collectVehicleLayers(pck, hot, frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir, trackPhase)
        : collectWreckLayers(pck, hot, frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir).layers;
    const auto shadows = collectVehicleShadowLayers(
        pck, hot, frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir, alive);
    bool hasRect = false;
    int minLeft = 0;
    int minTop = 0;
    int maxRight = 0;
    int maxBottom = 0;
    const auto accumulate = [&](const auto& list) {
        for (std::size_t i = 0; i < list.count; ++i) {
            const auto& layer = list.items[i];
            const int w = static_cast<int>(u16(layer.frame, 4));
            const int h = static_cast<int>(u16(layer.frame, 6));
            if (w < 1 || h < 1) continue;
            const int fLeft = layer.ox + i16(layer.frame, 0);
            const int fTop = layer.oy + i16(layer.frame, 2);
            const int fRight = fLeft + w;
            const int fBottom = fTop + h;
            if (!hasRect) {
                minLeft = fLeft;
                minTop = fTop;
                maxRight = fRight;
                maxBottom = fBottom;
                hasRect = true;
            } else {
                minLeft = std::min(minLeft, fLeft);
                minTop = std::min(minTop, fTop);
                maxRight = std::max(maxRight, fRight);
                maxBottom = std::max(maxBottom, fBottom);
            }
        }
    };
    accumulate(layers);
    accumulate(shadows);
    if (!hasRect) {
        unit.left = unit.x;
        unit.top = unit.y;
        unit.width = 0;
        unit.height = 0;
    } else {
        unit.left = minLeft;
        unit.top = minTop;
        unit.width = maxRight - minLeft;
        unit.height = maxBottom - minTop;
    }
}

void drawShadowWithDepth(MapImage& target, int patchX, int patchY, int ox, int oy,
                         View frame, int sortY, const SceneryDepth* depth,
                         std::vector<std::uint8_t>& shadowMask) {
    const int patchOx = ox - patchX;
    const int patchOy = oy - patchY;
    const int w = static_cast<int>(u16(frame, 4));
    const int h = static_cast<int>(u16(frame, 6));
    if (w < 1 || h < 1 || w > 2048 || h > 2048) return;
    const std::int64_t left64 = std::int64_t(patchOx) + i16(frame, 0);
    const std::int64_t top64 = std::int64_t(patchOy) + i16(frame, 2);
    if (left64 >= std::int64_t(target.width) || top64 >= std::int64_t(target.height) ||
        left64 + w <= 0 || top64 + h <= 0) {
        return;
    }
    const int left = static_cast<int>(left64);
    const int top = static_cast<int>(top64);
    const int x0 = std::max(0, left);
    const int y0 = std::max(0, top);
    const int x1 = std::min(static_cast<int>(target.width), left + w);
    const int y1 = std::min(static_cast<int>(target.height), top + h);
    if (x0 >= x1 || y0 >= y1) return;
    if (!depth || !depth->intersects(patchX + x0, patchY + y0, x1 - x0, y1 - y0)) {
        drawShadowFrame(target, patchOx, patchOy, frame, &shadowMask);
        return;
    }
    const auto cov = readShadowCoverage(patchOx, patchOy, frame);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            if (!cov.covers(px, py)) continue;
            const std::size_t idx = std::size_t(py) * std::size_t(target.width) + std::size_t(px);
            if (shadowMask[idx]) continue;
            if (depth->occludes(patchX + px, patchY + py, sortY) ||
                depth->hasShadow(patchX + px, patchY + py)) {
                continue;
            }
            auto* pixel = target.rgba.data() + idx * 4u;
            if (pixel[3] == 0u) continue;
            shadowMask[idx] = 1u;
            pixel[0] = static_cast<std::uint8_t>(pixel[0] >> 1);
            pixel[1] = static_cast<std::uint8_t>((pixel[1] >> 1) + 16u);
            pixel[2] = static_cast<std::uint8_t>((pixel[2] >> 1) + 41u);
        }
    }
}

void drawFrameWithDepth(MapImage& target, int patchX, int patchY, int ox, int oy,
                        View frame, View col, int sortY, const SceneryDepth* depth) {
    const int patchOx = ox - patchX;
    const int patchOy = oy - patchY;
    const int w = static_cast<int>(u16(frame, 4));
    const int h = static_cast<int>(u16(frame, 6));
    if (w < 1 || h < 1 || w > 2048 || h > 2048) {
        drawIndexedFrame(target, patchOx, patchOy, frame, col);
        return;
    }
    const std::int64_t left64 = std::int64_t(patchOx) + i16(frame, 0);
    const std::int64_t top64 = std::int64_t(patchOy) + i16(frame, 2);
    if (left64 >= std::int64_t(target.width) || top64 >= std::int64_t(target.height) ||
        left64 + w <= 0 || top64 + h <= 0) {
        return;
    }
    const int left = static_cast<int>(left64);
    const int top = static_cast<int>(top64);
    const int x0 = std::max(0, left);
    const int y0 = std::max(0, top);
    const int x1 = std::min(static_cast<int>(target.width), left + w);
    const int y1 = std::min(static_cast<int>(target.height), top + h);
    if (x0 >= x1 || y0 >= y1) return;
    const int cw = x1 - x0;
    const int ch = y1 - y0;
    if (!depth || !depth->intersects(patchX + x0, patchY + y0, cw, ch)) {
        drawIndexedFrame(target, patchOx, patchOy, frame, col);
        return;
    }
    MapImage scratch{static_cast<std::uint32_t>(cw), static_cast<std::uint32_t>(ch),
                     std::vector<std::uint8_t>(std::size_t(cw) * std::size_t(ch) * 4u)};
    for (int row = 0; row < ch; ++row) {
        const auto* src = target.rgba.data() +
            (std::size_t(y0 + row) * std::size_t(target.width) + std::size_t(x0)) * 4u;
        std::copy_n(src, std::size_t(cw) * 4u,
                    scratch.rgba.data() + std::size_t(row) * std::size_t(cw) * 4u);
    }
    SpriteCoverage cov;
    drawIndexedFrame(scratch, patchOx - x0, patchOy - y0, frame, col, &cov);
    for (int row = 0; row < ch; ++row) {
        const int py = y0 + row;
        const int cy = py - top;
        auto* dstRow = target.rgba.data() +
            (std::size_t(py) * std::size_t(target.width) + std::size_t(x0)) * 4u;
        const auto* srcRow = scratch.rgba.data() + std::size_t(row) * std::size_t(cw) * 4u;
        for (int colIdx = 0; colIdx < cw; ++colIdx) {
            const int px = x0 + colIdx;
            const int cx = px - left;
            if (!cov.pixels[std::size_t(cy) * std::size_t(w) + std::size_t(cx)]) continue;
            if (depth->occludes(patchX + px, patchY + py, sortY)) continue;
            std::copy_n(srcRow + std::size_t(colIdx) * 4u, 4u,
                        dstRow + std::size_t(colIdx) * 4u);
        }
    }
}
}

void VehicleScene::initArchives() {
    gameArchives_ = resolveGameArchives(gameDir_);
    descArchives_ = resolveDescArchives(gameDir_, 0);
    hasArchive_ = !gameArchives_.empty();
    hasDesc_ = !descArchives_.empty();
    archivesInitialized_ = true;
}

std::optional<std::size_t> VehicleScene::ensureAssetLoaded(const std::string& id) {
    if (const auto it = assetById_.find(id); it != assetById_.end()) return it->second;
    if (skippedIds_.contains(id)) return std::nullopt;
    if (!archivesInitialized_) {
        if (gameDir_.empty()) {
            skippedIds_.insert(id);
            return std::nullopt;
        }
        initArchives();
    }
    if (!hasArchive_) throw std::runtime_error("Cannot open SUE archive");

    std::optional<std::size_t> assetIndex;
    if (const auto stem = resolveUnitStem(descArchives_, hasDesc_, id)) {
        auto stemIt = assetByStem_.find(*stem);
        if (stemIt == assetByStem_.end()) {
            std::optional<std::size_t> loadedIndex;
            if (auto loaded = tryLoadVehicleStemAsset(gameArchives_, *stem)) {
                loadedIndex = assets_.size();
                auto wreck = makeWreckPalette(loaded->col);
                assets_.push_back(Asset{std::move(loaded->pck), std::move(loaded->col),
                                        std::move(wreck), std::move(loaded->hot), loaded->frameCount});
            }
            stemIt = assetByStem_.emplace(*stem, loadedIndex).first;
        }
        assetIndex = stemIt->second;
    }
    if (!assetIndex) {
        skippedIds_.insert(id);
        return std::nullopt;
    }
    assetById_.emplace(id, *assetIndex);
    return assetIndex;
}

VehicleScene::VehicleScene(const std::filesystem::path& gameDir, std::span<const Placement> placements)
    : gameDir_(gameDir) {
    if (placements.empty()) return;
    initArchives();
    if (!hasArchive_) throw std::runtime_error("Cannot open SUE archive");

    for (const auto& unit : placements) {
        (void)spawn(unit);
    }
}

std::optional<std::size_t> VehicleScene::spawn(const Placement& p) {
    if (p.direction < 0 || p.direction >= 32)
        throw std::runtime_error("Vehicle direction out of range");

    const auto assetIndex = ensureAssetLoaded(p.id);
    if (!assetIndex) return std::nullopt;

    const auto& asset = assets_[*assetIndex];
    VehicleUnit vu{
        p.id,
        p.x,
        p.y,
        p.hp,
        p.ammo,
        p.group,
        p.owner,
        static_cast<unsigned>(p.direction),
        static_cast<unsigned>(p.direction),
        0, 0, 0, 0,
        p.hp > 0,
        true,
        (p.owner != 3) || !p.passengers.empty(),
        p.expa,
    };
    updateVehicleBounds(vu, asset.pck, asset.hot, asset.frameCount, 0u);
    const std::size_t index = units_.size();
    unitAsset_.reserve(index + 1u);
    unitTrackPhase_.reserve(index + 1u);
    unitMoveAccum_.reserve(index + 1u);
    units_.push_back(std::move(vu));
    unitAsset_.push_back(*assetIndex);
    unitTrackPhase_.push_back(0u);
    unitMoveAccum_.push_back(0);
    return index;
}

std::size_t VehicleScene::spawnVisualPlaceholder(const Placement& p, int boxW, int boxH) {
    const int w = std::max(8, boxW);
    const int h = std::max(8, boxH);
    VehicleUnit vu{
        p.id,
        p.x,
        p.y,
        p.hp,
        p.ammo,
        p.group,
        p.owner,
        static_cast<unsigned>(p.direction & 31),
        static_cast<unsigned>(p.direction & 31),
        p.x - w / 2,
        p.y - h / 2,
        w,
        h,
        p.hp > 0,
        true,
        (p.owner != 3) || !p.passengers.empty(),
        p.expa,
    };
    const std::size_t index = units_.size();
    unitAsset_.reserve(index + 1u);
    units_.push_back(std::move(vu));
    unitAsset_.push_back(static_cast<std::size_t>(-1));
    unitTrackPhase_.push_back(0u);
    unitMoveAccum_.push_back(0);
    return index;
}

void VehicleScene::drawAll(MapImage& target, const SceneryDepth* depth) const {
    drawRegion(target, 0, 0, depth);
}

void VehicleScene::drawRegion(MapImage& patch, int patchX, int patchY, const SceneryDepth* depth) const {
    if (!patch.width || !patch.height) return;
    if (std::uint64_t(patch.width) * patch.height > patch.rgba.size() / 4u)
        throw std::runtime_error("Invalid vehicle render target");

    const std::int64_t patchLeft = patchX;
    const std::int64_t patchTop = patchY;
    const std::int64_t patchRight = patchLeft + static_cast<std::int64_t>(patch.width);
    const std::int64_t patchBottom = patchTop + static_cast<std::int64_t>(patch.height);

    std::vector<std::size_t> order;
    order.reserve(units_.size());
    for (std::size_t i = 0; i < units_.size(); ++i) {
        if (unitAsset_[i] == static_cast<std::size_t>(-1)) continue;
        const auto& unit = units_[i];
        if (gpuRenderedIds_ && gpuRenderedIds_->contains(unit.id)) continue;
        if (!unit.visible) continue;
        if (unit.width <= 0 || unit.height <= 0) continue;
        const std::int64_t uLeft = unit.left;
        const std::int64_t uTop = unit.top;
        const std::int64_t uRight = uLeft + unit.width;
        const std::int64_t uBottom = uTop + unit.height;
        if (uRight <= patchLeft || uLeft >= patchRight ||
            uBottom <= patchTop || uTop >= patchBottom) {
            continue;
        }
        order.push_back(i);
    }
    if (order.empty()) return;

    std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        const bool aliveA = units_[a].alive && units_[a].hp > 0;
        const bool aliveB = units_[b].alive && units_[b].hp > 0;
        if (aliveA != aliveB) return !aliveA;
        return units_[a].y < units_[b].y;
    });

    std::vector<std::uint8_t> shadowMask(std::size_t(patch.width) * std::size_t(patch.height), 0u);
    for (const std::size_t idx : order) {
        const auto& unit = units_[idx];
        const auto& asset = assets_[unitAsset_[idx]];
        const bool alive = unit.alive && unit.hp > 0;
        const auto shadows = collectVehicleShadowLayers(
            asset.pck, asset.hot, asset.frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir, alive);
        for (std::size_t i = 0; i < shadows.count; ++i) {
            drawShadowWithDepth(patch, patchX, patchY, shadows.items[i].ox, shadows.items[i].oy,
                                shadows.items[i].frame, unit.y, depth, shadowMask);
        }
    }

    for (const std::size_t idx : order) {
        const auto& unit = units_[idx];
        const auto& asset = assets_[unitAsset_[idx]];
        const bool alive = unit.alive && unit.hp > 0;
        if (alive) {
            const unsigned trackPhase = idx < unitTrackPhase_.size() ? unitTrackPhase_[idx] : 0u;
            const auto layers = collectVehicleLayers(
                asset.pck, asset.hot, asset.frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir, trackPhase);
            for (std::size_t i = 0; i < layers.count; ++i) {
                drawFrameWithDepth(patch, patchX, patchY, layers.items[i].ox, layers.items[i].oy,
                                   layers.items[i].frame, asset.col, unit.y, depth);
            }
        } else {
            const auto wreck = collectWreckLayers(
                asset.pck, asset.hot, asset.frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir);
            const auto& pal = wreck.dedicated ? asset.col : asset.wreckCol;
            for (std::size_t i = 0; i < wreck.layers.count; ++i) {
                drawFrameWithDepth(patch, patchX, patchY, wreck.layers.items[i].ox, wreck.layers.items[i].oy,
                                   wreck.layers.items[i].frame, pal, unit.y, depth);
            }
        }
    }
}

DirtyRect VehicleScene::update(std::size_t index, int x, int y, unsigned hullDir, unsigned turretDir,
                               int hp, int visible, int ammo) {
    auto& unit = units_.at(index);
    const int oldLeft = unit.left;
    const int oldTop = unit.top;
    const int oldRight = unit.left + unit.width;
    const int oldBottom = unit.top + unit.height;
    const bool hadBox = unit.width > 0 && unit.height > 0;

    auto next = unit;
    next.x = x;
    next.y = y;
    next.hullDir = hullDir & 31u;
    next.turretDir = turretDir & 31u;
    if (hp >= 0) {
        next.hp = hp;
        next.alive = next.hp > 0;
    }
    if (visible >= 0) {
        next.visible = visible != 0;
    }
    if (ammo >= 0) {
        next.ammo = ammo;
    }
    unsigned trackPhase = index < unitTrackPhase_.size() ? unitTrackPhase_[index] : 0u;
    if (next.alive && next.hp > 0 && (x != unit.x || y != unit.y) &&
        index < unitTrackPhase_.size() && index < unitMoveAccum_.size()) {
        unitMoveAccum_[index] += std::abs(x - unit.x) + std::abs(y - unit.y);
        if (unitMoveAccum_[index] >= 4) {
            unitTrackPhase_[index] ^= 1u;
            unitMoveAccum_[index] %= 4;
            trackPhase = unitTrackPhase_[index];
        }
    }
    if (unitAsset_[index] == static_cast<std::size_t>(-1)) {
        next.left = x - next.width / 2;
        next.top = y - next.height / 2;
        unit = std::move(next);
        return {};
    }
    const auto& asset = assets_[unitAsset_[index]];
    updateVehicleBounds(next, asset.pck, asset.hot, asset.frameCount, trackPhase);

    const int newLeft = next.left;
    const int newTop = next.top;
    const int newRight = next.left + next.width;
    const int newBottom = next.top + next.height;
    const bool hasBox = next.width > 0 && next.height > 0;
    unit = std::move(next);

    if (!hadBox && !hasBox) return {};
    const int left = (hadBox && hasBox) ? std::min(oldLeft, newLeft) : (hadBox ? oldLeft : newLeft);
    const int top = (hadBox && hasBox) ? std::min(oldTop, newTop) : (hadBox ? oldTop : newTop);
    const int right = (hadBox && hasBox) ? std::max(oldRight, newRight) : (hadBox ? oldRight : newRight);
    const int bottom = (hadBox && hasBox) ? std::max(oldBottom, newBottom) : (hadBox ? oldBottom : newBottom);
    return DirtyRect{left - 4, top - 4, right + 4, bottom + 4};
}

void VehicleScene::setOwner(std::size_t index, int owner) {
    units_.at(index).owner = owner;
}

std::vector<Placement> drawTutorialVehicles(MapImage& map, const std::filesystem::path& gameDir, std::span<const Placement> source) {
    // Original UNITS definitions in desc_common.sue -> game_common.sue exact case-sensitive entry names.
    const std::map<std::string, Spec> specs{
        {"panther", {"PANTHER.PCK", "PANTHER.COL", "panther.hot", Kind::Tank}},
        {"zis-3",   {"GUN45RUS.PCK", "GUN45RUS.COL", nullptr, Kind::Artillery}},
        {"m3a1",    {"GUN100.PCK", "GUN100.COL", nullptr, Kind::Artillery}},
        {"ds39",    {"MACHGUN.PCK", "MACHGUN.COL", nullptr, Kind::HeavyMg}},
    };

    const auto archives = resolveGameArchives(gameDir);
    if (archives.empty()) throw std::runtime_error("Cannot open SUE archive");
    std::map<std::string, LoadedAsset> loaded;
    std::vector<Vehicle> placements;
    std::vector<Placement> units;

    for (const auto& unit : source) {
        const auto& id = unit.id;
        const auto specIt = specs.find(id);
        if (specIt == specs.end()) continue;
        if (unit.direction < 0 || unit.direction >= 32)
            throw std::runtime_error("Vehicle direction out of range");

        auto [it, inserted] = loaded.try_emplace(id);
        if (inserted) {
            const auto& spec = specIt->second;
            it->second.pck = readSueEntryChain(archives, spec.pck);
            it->second.col = readSueEntryChain(archives, spec.col);
            if (it->second.col.size() != 512)
                throw std::runtime_error("Invalid vehicle COL palette size");
            if (spec.hot) {
                it->second.hot = readSueEntryChain(archives, spec.hot);
                if (it->second.hot.size() < 32u * 8u)
                    throw std::runtime_error("Invalid vehicle HOT table size");
            }
            it->second.kind = spec.kind;
        }
        units.push_back(unit);
        placements.push_back({&it->second, unit.x, unit.y, unsigned(unit.direction)});
    }

    std::stable_sort(placements.begin(), placements.end(),
                     [](const Vehicle& a, const Vehicle& b) { return a.y < b.y; });

    if (!placements.empty()) {
        if (!map.width || !map.height || (std::uint64_t(map.width) * map.height > map.rgba.size() / 4u))
            throw std::runtime_error("Invalid vehicle render target");
        std::vector<std::uint8_t> shadowMask(std::size_t(map.width) * std::size_t(map.height), 0u);
        const auto tryShadow = [&](View pck, std::size_t index, int ox, int oy) {
            const auto sh = pckFrame(pck, index);
            if (isRenderableShadowFrame(sh)) drawShadowFrame(map, ox, oy, sh, &shadowMask);
        };
        for (const auto& p : placements) {
            const auto& a = *p.asset;
            switch (a.kind) {
            case Kind::Tank: {
                tryShadow(a.pck, 160u + p.dir, p.x, p.y);
                const auto [hotDx, hotDy] = readHotOffset(a.hot, p.dir);
                tryShadow(a.pck, 256u + p.dir, p.x + hotDx, p.y + hotDy);
                break;
            }
            case Kind::Artillery:
                tryShadow(a.pck, 96u + p.dir, p.x, p.y);
                tryShadow(a.pck, 224u + p.dir, p.x, p.y);
                break;
            case Kind::HeavyMg:
                tryShadow(a.pck, 64u, p.x, p.y);
                break;
            }
        }
    }

    for (const auto& p : placements) {
        const auto& a = *p.asset;
        switch (a.kind) {
        case Kind::Tank: {
            // Block 0: intact hull (0..31); Block 1: tracks/wheels (32..63); Block 6: turret (192..223) offset by hot0[dir].
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, p.dir), a.col);
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, 32u + p.dir), a.col);
            const auto [hotDx, hotDy] = readHotOffset(a.hot, p.dir);
            drawIndexedFrame(map, p.x + hotDx, p.y + hotDy, pckFrame(a.pck, 192u + p.dir), a.col);
            break;
        }
        case Kind::Artillery:
            // Block 0: wheels (0..31), Block 1: deployed carriage (32..63), Block 5: shield+barrel (160..191).
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, p.dir), a.col);
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, 32u + p.dir), a.col);
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, 160u + p.dir), a.col);
            break;
        case Kind::HeavyMg:
            // Frames 0..31: deployed heavy MG directions.
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, p.dir), a.col);
            break;
        }
    }
    return units;
}

std::size_t drawMissionVehicles(MapImage& map, const std::filesystem::path& gameDir,
                                const std::filesystem::path& missionPath,
                                const SceneryDepth* depth) {
    const auto source = readMissionPlacements(missionPath);
    if (source.empty()) return 0;

    const auto archives = resolveGameArchives(gameDir);
    if (archives.empty())
        throw std::runtime_error("Cannot open SUE archive");
    const auto descArchives = resolveDescArchives(gameDir, 0);
    const bool hasDesc = !descArchives.empty();

    std::map<std::string, std::optional<MissionVehicleAsset>> cache;
    std::vector<MissionVehicle> placements;
    for (const auto& unit : source) {
        if (unit.direction < 0 || unit.direction >= 32)
            throw std::runtime_error("Vehicle direction out of range");

        auto it = cache.find(unit.id);
        if (it == cache.end()) {
            std::optional<MissionVehicleAsset> loaded;
            if (const auto stem = resolveUnitStem(descArchives, hasDesc, unit.id)) {
                loaded = tryLoadVehicleStemAsset(archives, *stem);
            }
            it = cache.emplace(unit.id, std::move(loaded)).first;
        }
        if (it->second)
            placements.push_back({&*it->second, unit.x, unit.y, unsigned(unit.direction)});
    }
    if (placements.empty()) return 0;
    if (!map.width || !map.height || (std::uint64_t(map.width) * map.height > map.rgba.size() / 4u))
        throw std::runtime_error("Invalid vehicle render target");

    std::stable_sort(placements.begin(), placements.end(),
                     [](const MissionVehicle& a, const MissionVehicle& b) { return a.y < b.y; });

    std::vector<std::uint8_t> shadowMask(std::size_t(map.width) * std::size_t(map.height), 0u);
    for (const auto& p : placements) {
        const auto& a = *p.asset;
        const auto shadows = collectVehicleShadowLayers(
            a.pck, a.hot, a.frameCount, p.x, p.y, p.dir, p.dir, true);
        for (std::size_t i = 0; i < shadows.count; ++i) {
            drawShadowWithDepth(map, 0, 0, shadows.items[i].ox, shadows.items[i].oy,
                                shadows.items[i].frame, p.y, depth, shadowMask);
        }
    }

    std::size_t drawn = 0;
    for (const auto& p : placements) {
        const auto& a = *p.asset;
        const auto layers = collectVehicleLayers(a.pck, a.hot, a.frameCount, p.x, p.y, p.dir, p.dir);
        for (std::size_t i = 0; i < layers.count; ++i) {
            drawFrameWithDepth(map, 0, 0, layers.items[i].ox, layers.items[i].oy,
                               layers.items[i].frame, a.col, p.y, depth);
        }
        ++drawn;
    }
    return drawn;
}

std::optional<MapImage> loadAircraftSprite(const std::filesystem::path& gameDir,
                                           std::string_view unitId,
                                           unsigned dir32) {
    if (gameDir.empty() || unitId.empty()) return std::nullopt;
    const auto archives = resolveGameArchives(gameDir);
    if (archives.empty()) return std::nullopt;
    const auto descArchives = resolveDescArchives(gameDir, 0);
    const bool hasDesc = !descArchives.empty();

    const auto stem = resolveUnitStem(descArchives, hasDesc, unitId);
    if (!stem) return std::nullopt;
    const auto asset = tryLoadVehicleStemAsset(archives, *stem);
    if (!asset || asset->frameCount < 32u) return std::nullopt;

    const auto frame = pckFrame(asset->pck, dir32 & 31u);
    if (!isRenderableFrame(frame)) return std::nullopt;
    const int ox = i16(frame, 0);
    const int oy = i16(frame, 2);
    const unsigned w = u16(frame, 4);
    const unsigned h = u16(frame, 6);
    if (w == 0 || h == 0 || w > 512 || h > 512) return std::nullopt;

    MapImage img{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4u, 0)};
    drawIndexedFrame(img, -ox, -oy, frame, asset->col);
    for (std::size_t i = 0; i < img.rgba.size(); i += 4u) {
        const unsigned a = img.rgba[i + 3u];
        if (a > 0u && a < 255u) {
            img.rgba[i] = static_cast<std::uint8_t>(std::min(255u, (unsigned(img.rgba[i]) * 255u) / a));
            img.rgba[i + 1u] = static_cast<std::uint8_t>(std::min(255u, (unsigned(img.rgba[i + 1u]) * 255u) / a));
            img.rgba[i + 2u] = static_cast<std::uint8_t>(std::min(255u, (unsigned(img.rgba[i + 2u]) * 255u) / a));
        }
    }
    return img;
}

std::optional<MapImage> VehicleScene::renderUnitPortrait(const std::string& id, unsigned size) {
    if (size == 0u || id.empty()) return std::nullopt;
    const auto assetIndex = ensureAssetLoaded(id);
    if (!assetIndex) return std::nullopt;
    const auto& asset = assets_[*assetIndex];

    auto layers = collectVehicleLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, 12u, 12u);
    if (layers.count == 0u) {
        layers = collectVehicleLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, 0u, 0u);
    }
    if (layers.count == 0u) return std::nullopt;

    bool hasRect = false;
    int minLeft = 0, minTop = 0, maxRight = 0, maxBottom = 0;
    for (std::size_t i = 0; i < layers.count; ++i) {
        const auto& layer = layers.items[i];
        const int w = static_cast<int>(u16(layer.frame, 4));
        const int h = static_cast<int>(u16(layer.frame, 6));
        if (w < 1 || h < 1) continue;
        const int fLeft = layer.ox + i16(layer.frame, 0);
        const int fTop = layer.oy + i16(layer.frame, 2);
        const int fRight = fLeft + w;
        const int fBottom = fTop + h;
        if (!hasRect) {
            minLeft = fLeft;
            minTop = fTop;
            maxRight = fRight;
            maxBottom = fBottom;
            hasRect = true;
        } else {
            minLeft = std::min(minLeft, fLeft);
            minTop = std::min(minTop, fTop);
            maxRight = std::max(maxRight, fRight);
            maxBottom = std::max(maxBottom, fBottom);
        }
    }
    if (!hasRect) return std::nullopt;
    const int bw = maxRight - minLeft;
    const int bh = maxBottom - minTop;
    if (bw <= 0 || bh <= 0 || bw > 1024 || bh > 1024) return std::nullopt;

    MapImage raw{static_cast<unsigned>(bw), static_cast<unsigned>(bh),
                 std::vector<std::uint8_t>(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * 4u, 0u)};
    for (std::size_t i = 0; i < layers.count; ++i) {
        drawIndexedFrame(raw, layers.items[i].ox - minLeft, layers.items[i].oy - minTop,
                         layers.items[i].frame, asset.col);
    }

    MapImage out{size, size, std::vector<std::uint8_t>(static_cast<std::size_t>(size) * size * 4u, 255u)};
    const double half = static_cast<double>(size) * 0.5;
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            const double nx = (static_cast<double>(x) + 0.5 - half) / std::max(1.0, half);
            const double ny = (static_cast<double>(y) + 0.5 - half) / std::max(1.0, half);
            const double t = std::clamp(std::sqrt(nx * nx + ny * ny) * 0.72, 0.0, 1.0);
            const std::size_t di = (static_cast<std::size_t>(y) * size + x) * 4u;
            out.rgba[di + 0u] = static_cast<std::uint8_t>(std::lround(54.0 - 30.0 * t));
            out.rgba[di + 1u] = static_cast<std::uint8_t>(std::lround(62.0 - 33.0 * t));
            out.rgba[di + 2u] = static_cast<std::uint8_t>(std::lround(50.0 - 26.0 * t));
            out.rgba[di + 3u] = 255u;
        }
    }

    const unsigned inner = std::max(4u, size > 4u ? size - 4u : size);
    const double scale = std::min(static_cast<double>(inner) / static_cast<double>(bw),
                                  static_cast<double>(inner) / static_cast<double>(bh));
    const unsigned dstW = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(bw) * scale)));
    const unsigned dstH = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(bh) * scale)));
    const int offX = (static_cast<int>(size) - static_cast<int>(dstW)) / 2;
    const int offY = (static_cast<int>(size) - static_cast<int>(dstH)) / 2;
    for (unsigned dy = 0; dy < dstH; ++dy) {
        const int py = offY + static_cast<int>(dy);
        if (py < 0 || py >= static_cast<int>(size)) continue;
        const unsigned sy = std::min<unsigned>(static_cast<unsigned>(bw ? (dy * static_cast<unsigned>(bh)) / dstH : 0u),
                                               static_cast<unsigned>(bh - 1));
        for (unsigned dx = 0; dx < dstW; ++dx) {
            const int px = offX + static_cast<int>(dx);
            if (px < 0 || px >= static_cast<int>(size)) continue;
            const unsigned sx = std::min<unsigned>((dx * static_cast<unsigned>(bw)) / dstW,
                                                   static_cast<unsigned>(bw - 1));
            const std::size_t si = (static_cast<std::size_t>(sy) * static_cast<unsigned>(bw) + sx) * 4u;
            const unsigned sa = raw.rgba[si + 3u];
            if (sa == 0u) continue;
            const std::size_t di = (static_cast<std::size_t>(py) * size + static_cast<unsigned>(px)) * 4u;
            if (sa == 255u) {
                out.rgba[di + 0u] = raw.rgba[si + 0u];
                out.rgba[di + 1u] = raw.rgba[si + 1u];
                out.rgba[di + 2u] = raw.rgba[si + 2u];
            } else {
                const unsigned inv = 255u - sa;
                out.rgba[di + 0u] = static_cast<std::uint8_t>(std::min(255u, raw.rgba[si + 0u] + (out.rgba[di + 0u] * inv) / 255u));
                out.rgba[di + 1u] = static_cast<std::uint8_t>(std::min(255u, raw.rgba[si + 1u] + (out.rgba[di + 1u] * inv) / 255u));
                out.rgba[di + 2u] = static_cast<std::uint8_t>(std::min(255u, raw.rgba[si + 2u] + (out.rgba[di + 2u] * inv) / 255u));
            }
        out.rgba[di + 3u] = 255u;
        }
    }
    return out;
}

namespace {

MapImage upscale2xEpx(const MapImage& src) {
    if (!src.width || !src.height) return {};
    const unsigned sw = src.width;
    const unsigned sh = src.height;
    const unsigned dw = sw * 2u;
    const unsigned dh = sh * 2u;
    MapImage dst{dw, dh, std::vector<std::uint8_t>(std::size_t(dw) * std::size_t(dh) * 4u, 0u)};
    const auto px = [&](int x, int y) -> const std::uint8_t* {
        const unsigned cx = static_cast<unsigned>(std::clamp(x, 0, static_cast<int>(sw) - 1));
        const unsigned cy = static_cast<unsigned>(std::clamp(y, 0, static_cast<int>(sh) - 1));
        return src.rgba.data() + (std::size_t(cy) * sw + cx) * 4u;
    };
    const auto eq = [](const std::uint8_t* a, const std::uint8_t* b) -> bool {
        if (a[3] == 0u && b[3] == 0u) return true;
        if (a[3] == 0u || b[3] == 0u) return false;
        return std::abs(int(a[0]) - int(b[0])) <= 14 &&
               std::abs(int(a[1]) - int(b[1])) <= 14 &&
               std::abs(int(a[2]) - int(b[2])) <= 14 &&
               std::abs(int(a[3]) - int(b[3])) <= 32;
    };
    const auto put = [&](unsigned dx, unsigned dy, const std::uint8_t* c) {
        auto* out = dst.rgba.data() + (std::size_t(dy) * dw + dx) * 4u;
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        out[3] = c[3];
    };
    for (int y = 0; y < static_cast<int>(sh); ++y) {
        for (int x = 0; x < static_cast<int>(sw); ++x) {
            const auto* p = px(x, y);
            const auto* a = (y > 0) ? px(x, y - 1) : p;
            const auto* d = (y + 1 < static_cast<int>(sh)) ? px(x, y + 1) : p;
            const auto* c = (x > 0) ? px(x - 1, y) : p;
            const auto* b = (x + 1 < static_cast<int>(sw)) ? px(x + 1, y) : p;
            const std::uint8_t* q0 = p;
            const std::uint8_t* q1 = p;
            const std::uint8_t* q2 = p;
            const std::uint8_t* q3 = p;
            if (!eq(c, b) && !eq(a, d)) {
                if (eq(c, a)) q0 = c;
                if (eq(a, b)) q1 = b;
                if (eq(d, c)) q2 = c;
                if (eq(b, d)) q3 = b;
            }
            const unsigned ux = static_cast<unsigned>(x) * 2u;
            const unsigned uy = static_cast<unsigned>(y) * 2u;
            put(ux, uy, q0);
            put(ux + 1u, uy, q1);
            put(ux, uy + 1u, q2);
            put(ux + 1u, uy + 1u, q3);
        }
    }
    return dst;
}

} // namespace

std::optional<GpuVehicleAtlas> VehicleScene::buildGpuAtlas(const std::string& id, bool scale2xEpx) {
    if (id.empty()) return std::nullopt;
    const auto assetIndex = ensureAssetLoaded(id);
    if (!assetIndex) return std::nullopt;
    const auto& asset = assets_[*assetIndex];
    if (asset.pck.size() < 8u || asset.col.size() < 512u) return std::nullopt;

    GpuVehicleAtlas out;
    out.scale = scale2xEpx ? 2u : 1u;
    out.frameCount = asset.frameCount;
    if (asset.hot.size() >= 32u * 8u) {
        out.hasHot = true;
        for (unsigned d = 0; d < 32u; ++d) {
            const int hotDx = i32(asset.hot, std::size_t(d) * 8u);
            const int hotDy = i32(asset.hot, std::size_t(d) * 8u + 4u);
            if (hotDx >= -2048 && hotDx <= 2048 && hotDy >= -2048 && hotDy <= 2048) {
                out.hotOffsets[d] = {static_cast<std::int16_t>(hotDx), static_cast<std::int16_t>(hotDy)};
            }
        }
    }

    struct PendingKey {
        std::size_t idx{0};
        bool useWreckPal{false};
        bool isShadow{false};
        bool operator<(const PendingKey& o) const {
            return std::tie(idx, useWreckPal, isShadow) < std::tie(o.idx, o.useWreckPal, o.isShadow);
        }
    };
    std::set<PendingKey> needed;
    for (unsigned h = 0; h < 32u; ++h) {
        for (unsigned ph = 0; ph < 2u; ++ph) {
            const auto aliveL = collectVehicleLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, h, h, ph);
            for (std::size_t i = 0; i < aliveL.count; ++i) {
                needed.insert({aliveL.items[i].frameIdx, false, false});
            }
        }
        const auto turretL = collectVehicleLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, 0u, h, 0u);
        for (std::size_t i = 0; i < turretL.count; ++i) {
            needed.insert({turretL.items[i].frameIdx, false, false});
        }
        const auto wreckL = collectWreckLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, h, h);
        for (std::size_t i = 0; i < wreckL.layers.count; ++i) {
            needed.insert({wreckL.layers.items[i].frameIdx, !wreckL.dedicated, false});
        }
        const auto wreckTurret = collectWreckLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, 0u, h);
        for (std::size_t i = 0; i < wreckTurret.layers.count; ++i) {
            needed.insert({wreckTurret.layers.items[i].frameIdx, !wreckTurret.dedicated, false});
        }
        for (bool alv : {true, false}) {
            const auto sh = collectVehicleShadowLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, h, h, alv);
            for (std::size_t i = 0; i < sh.count; ++i) {
                needed.insert({sh.items[i].frameIdx, false, true});
            }
            const auto shT = collectVehicleShadowLayers(asset.pck, asset.hot, asset.frameCount, 0, 0, 0u, h, alv);
            for (std::size_t i = 0; i < shT.count; ++i) {
                needed.insert({shT.items[i].frameIdx, false, true});
            }
        }
    }
    if (needed.empty()) return std::nullopt;

    struct DecodedEntry {
        PendingKey key;
        MapImage img;
        std::int16_t ox{0}, oy{0};
    };
    std::vector<DecodedEntry> entries;
    entries.reserve(needed.size());
    std::uint64_t totalArea = 0;
    for (const auto& pk : needed) {
        if (pk.idx >= asset.frameCount) continue;
        const auto fr = pckFrame(asset.pck, pk.idx);
        const int ox = i16(fr, 0);
        const int oy = i16(fr, 2);
        const unsigned w = u16(fr, 4);
        const unsigned h = u16(fr, 6);
        if (w < 1u || h < 1u || w > 512u || h > 512u) continue;
        MapImage raw{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4u, 0u)};
        if (pk.isShadow) {
            if (!isRenderableShadowFrame(fr)) continue;
            const auto cov = readShadowCoverage(0, 0, fr);
            for (std::size_t p = 0; p < cov.pixels.size(); ++p) {
                if (cov.pixels[p]) {
                    raw.rgba[p * 4u + 0u] = 255u;
                    raw.rgba[p * 4u + 1u] = 255u;
                    raw.rgba[p * 4u + 2u] = 255u;
                    raw.rgba[p * 4u + 3u] = 255u;
                }
            }
        } else {
            if (!isRenderableFrame(fr)) continue;
            const auto& pal = pk.useWreckPal ? asset.wreckCol : asset.col;
            drawIndexedFrame(raw, -ox, -oy, fr, pal);
        }
        MapImage finalImg = (scale2xEpx && w <= 256u && h <= 256u) ? upscale2xEpx(raw) : std::move(raw);
        if (!finalImg.width || !finalImg.height) continue;
        totalArea += std::uint64_t(finalImg.width + 2u) * std::uint64_t(finalImg.height + 2u);
        entries.push_back({pk, std::move(finalImg), static_cast<std::int16_t>(ox), static_cast<std::int16_t>(oy)});
    }
    if (entries.empty()) return std::nullopt;

    unsigned atlasW = 512u;
    if (totalArea > 320000ull) atlasW = 1024u;
    if (totalArea > 1200000ull) atlasW = 2048u;
    if (totalArea > 4800000ull) atlasW = 4096u;

    constexpr unsigned kPad = 2u;
    unsigned curX = kPad, curY = kPad, rowH = 0u;
    struct Placed {
        unsigned x{}, y{};
    };
    std::vector<Placed> placed(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const unsigned ew = entries[i].img.width;
        const unsigned eh = entries[i].img.height;
        if (ew + kPad * 2u > atlasW) atlasW = std::min(4096u, ew + kPad * 2u);
        if (curX + ew + kPad > atlasW) {
            curX = kPad;
            curY += rowH + kPad;
            rowH = 0u;
        }
        placed[i] = {curX, curY};
        curX += ew + kPad;
        rowH = std::max(rowH, eh);
    }
    const unsigned atlasH = std::max(256u, curY + rowH + kPad);
    if (atlasW > 4096u || atlasH > 4096u) {
        if (scale2xEpx) return buildGpuAtlas(id, false);
        return std::nullopt;
    }

    out.atlas = MapImage{atlasW, atlasH, std::vector<std::uint8_t>(std::size_t(atlasW) * atlasH * 4u, 0u)};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const unsigned px = placed[i].x;
        const unsigned py = placed[i].y;
        const unsigned ew = e.img.width;
        const unsigned eh = e.img.height;
        for (unsigned row = 0; row < eh; ++row) {
            const auto* srcRow = e.img.rgba.data() + std::size_t(row) * ew * 4u;
            auto* dstRow = out.atlas.rgba.data() + (std::size_t(py + row) * atlasW + px) * 4u;
            std::copy_n(srcRow, std::size_t(ew) * 4u, dstRow);
        }
        GpuFrameRect rect{
            static_cast<std::uint16_t>(px),
            static_cast<std::uint16_t>(py),
            static_cast<std::uint16_t>(ew),
            static_cast<std::uint16_t>(eh),
            e.ox,
            e.oy,
            true
        };
        if (e.key.useWreckPal) {
            out.fallbackWreckFrames.emplace(e.key.idx, rect);
        } else {
            out.frames.emplace(e.key.idx, rect);
        }
    }
    return out;
}

void VehicleScene::appendGpuSprites(std::size_t unitIndex, const GpuVehicleAtlas& gpu, int mapScale,
                                    float recoilDx, float recoilDy,
                                    std::vector<GpuVehicleSprite>& out) const {
    if (unitIndex >= units_.size()) return;
    const auto& unit = units_[unitIndex];
    if (!unit.visible || unitAsset_[unitIndex] == static_cast<std::size_t>(-1)) return;
    const auto& asset = assets_[unitAsset_[unitIndex]];
    const bool alive = unit.alive && unit.hp > 0;
    const float s = static_cast<float>(std::max(1, mapScale));
    const float gs = static_cast<float>(std::max(1u, gpu.scale));

    const auto shadows = collectVehicleShadowLayers(
        asset.pck, asset.hot, asset.frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir, alive);
    for (std::size_t i = 0; i < shadows.count; ++i) {
        const auto& sl = shadows.items[i];
        const auto it = gpu.frames.find(sl.frameIdx);
        if (it == gpu.frames.end() || !it->second.valid) continue;
        const auto& fr = it->second;
        out.push_back({
            static_cast<float>(sl.ox + fr.ox) * s + (sl.isTurret ? recoilDx : 0.0f),
            static_cast<float>(sl.oy + fr.oy) * s + (sl.isTurret ? recoilDy : 0.0f),
            (static_cast<float>(fr.w) / gs) * s,
            (static_cast<float>(fr.h) / gs) * s,
            static_cast<float>(fr.u),
            static_cast<float>(fr.v),
            static_cast<float>(fr.u + fr.w),
            static_cast<float>(fr.v + fr.h),
            true,
            sl.isTurret,
            alive ? -3 : -9502
        });
    }

    if (alive) {
        const unsigned trackPhase = unitIndex < unitTrackPhase_.size() ? unitTrackPhase_[unitIndex] : 0u;
        const auto layers = collectVehicleLayers(
            asset.pck, asset.hot, asset.frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir, trackPhase);
        for (std::size_t i = 0; i < layers.count; ++i) {
            const auto& ll = layers.items[i];
            const auto it = gpu.frames.find(ll.frameIdx);
            if (it == gpu.frames.end() || !it->second.valid) continue;
            const auto& fr = it->second;
            const bool applyRecoil = ll.isTurret || (!gpu.hasHot && (i + 1u == layers.count));
            out.push_back({
                static_cast<float>(ll.ox + fr.ox) * s + (applyRecoil ? recoilDx : 0.0f),
                static_cast<float>(ll.oy + fr.oy) * s + (applyRecoil ? recoilDy : 0.0f),
                (static_cast<float>(fr.w) / gs) * s,
                (static_cast<float>(fr.h) / gs) * s,
                static_cast<float>(fr.u),
                static_cast<float>(fr.v),
                static_cast<float>(fr.u + fr.w),
                static_cast<float>(fr.v + fr.h),
                false,
                ll.isTurret,
                ll.isTurret ? 1 : 0
            });
        }
    } else {
        const auto wreck = collectWreckLayers(
            asset.pck, asset.hot, asset.frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir);
        const auto& fMap = wreck.dedicated ? gpu.frames : gpu.fallbackWreckFrames;
        for (std::size_t i = 0; i < wreck.layers.count; ++i) {
            const auto& wl = wreck.layers.items[i];
            auto it = fMap.find(wl.frameIdx);
            if (it == fMap.end()) it = gpu.frames.find(wl.frameIdx);
            if (it == fMap.end() || !it->second.valid) continue;
            const auto& fr = it->second;
            out.push_back({
                static_cast<float>(wl.ox + fr.ox) * s,
                static_cast<float>(wl.oy + fr.oy) * s,
                (static_cast<float>(fr.w) / gs) * s,
                (static_cast<float>(fr.h) / gs) * s,
                static_cast<float>(fr.u),
                static_cast<float>(fr.v),
                static_cast<float>(fr.u + fr.w),
                static_cast<float>(fr.v + fr.h),
                false,
                false,
                -9500
            });
        }
    }
}
}
