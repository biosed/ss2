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

std::optional<Bytes> tryReadSue(const std::filesystem::path& archive, std::string_view entry) {
    try {
        return readSueEntry(archive, entry);
    } catch (const std::runtime_error& err) {
        if (std::string_view(err.what()) == "SUE entry not found") return std::nullopt;
        throw;
    }
}

std::optional<std::string> resolveUnitStem(const std::filesystem::path& descArchive, bool hasDesc,
                                           std::string_view unitId) {
    if (hasDesc) {
        if (const auto bytes = tryReadSue(descArchive, "UNITS\\" + std::string(unitId))) {
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
};

struct LayerList {
    std::array<FrameLayer, 3> items{};
    std::size_t count{0};

    void push(View frame, int ox, int oy) {
        if (isRenderableFrame(frame)) {
            items[count++] = {frame, ox, oy};
        }
    }
};

std::optional<MissionVehicleAsset> tryLoadVehicleStemAsset(const std::filesystem::path& archive,
                                                           std::string_view stem) {
    auto pck = tryReadSue(archive, std::string(stem) + ".PCK");
    auto col = tryReadSue(archive, std::string(stem) + ".COL");
    if (!pck || !col || col->size() != 512u || pck->size() < 8u) return std::nullopt;
    const std::size_t first = u32(*pck, 0);
    if (first < 8u || (first % 4u) != 0u || first > pck->size()) return std::nullopt;
    const std::size_t frameCount = first / 4u - 1u;
    if (frameCount < 32u) return std::nullopt;
    auto hot = tryReadSue(archive, std::string(stem) + ".hot");
    return MissionVehicleAsset{std::move(*pck), std::move(*col),
                               hot ? std::move(*hot) : Bytes{}, frameCount};
}

LayerList collectVehicleLayers(View pck, View hot, std::size_t frameCount,
                               int x, int y, unsigned hullDir, unsigned turretDir) {
    const unsigned hull = hullDir & 31u;
    const unsigned turret = turretDir & 31u;
    LayerList layers;
    switch (frameCount) {
    case 288u: {
        const auto block6 = pckFrame(pck, 192u + turret);
        if (frameTag(block6) == 0xa2u) {
            layers.push(pckFrame(pck, hull), x, y);
            if (isRenderableFrame(block6) && hot.size() >= 32u * 8u) {
                const int hotDx = i32(hot, std::size_t(hull) * 8u);
                const int hotDy = i32(hot, std::size_t(hull) * 8u + 4u);
                if (hotDx < -2048 || hotDx > 2048 || hotDy < -2048 || hotDy > 2048)
                    throw std::runtime_error("Vehicle turret offset exceeds sprite bounds");
                layers.push(block6, x + hotDx, y + hotDy);
            }
        } else {
            layers.push(pckFrame(pck, 32u + hull), x, y);
            layers.push(pckFrame(pck, hull), x, y);
        }
        break;
    }
    case 352u:
        if (frameTag(pckFrame(pck, 64u + hull)) == 0xa2u) {
            layers.push(pckFrame(pck, hull), x, y);
            layers.push(pckFrame(pck, 32u + hull), x, y);
            layers.push(pckFrame(pck, 160u + turret), x, y);
        } else {
            layers.push(pckFrame(pck, hull), x, y);
        }
        break;
    case 448u:
        layers.push(pckFrame(pck, hull), x, y);
        layers.push(pckFrame(pck, 32u + hull), x, y);
        layers.push(pckFrame(pck, 128u + turret), x, y);
        break;
    case 256u:
        layers.push(pckFrame(pck, 32u + hull), x, y);
        layers.push(pckFrame(pck, hull), x, y);
        break;
    default:
        if (frameCount < 96u) {
            const auto block0 = pckFrame(pck, hull);
            if (isRenderableFrame(block0)) {
                layers.push(block0, x, y);
            } else if (32u + hull < frameCount) {
                layers.push(pckFrame(pck, 32u + hull), x, y);
            }
        } else {
            layers.push(pckFrame(pck, hull), x, y);
        }
        break;
    }
    return layers;
}

LayerList collectWreckLayers(View pck, std::size_t frameCount, int x, int y, unsigned hullDir) {
    const unsigned hull = hullDir & 31u;
    LayerList layers;
    const auto tryBlock = [&](std::size_t base) -> bool {
        if (base + hull < frameCount) {
            const auto fr = pckFrame(pck, base + hull);
            if (isRenderableFrame(fr)) {
                layers.push(fr, x, y);
                return true;
            }
        }
        return false;
    };
    if (frameCount == 288u) {
        if (!tryBlock(96u) && !tryBlock(32u)) (void)tryBlock(0u);
    } else if (frameCount == 160u || frameCount == 352u) {
        if (!tryBlock(64u) && !tryBlock(32u)) (void)tryBlock(0u);
    } else {
        if (!tryBlock(32u)) (void)tryBlock(0u);
    }
    return layers;
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

void updateVehicleBounds(VehicleUnit& unit, View pck, View hot, std::size_t frameCount) {
    const auto layers = (unit.alive && unit.hp > 0)
        ? collectVehicleLayers(pck, hot, frameCount, unit.x, unit.y, unit.hullDir, unit.turretDir)
        : collectWreckLayers(pck, frameCount, unit.x, unit.y, unit.hullDir);
    bool hasRect = false;
    int minLeft = 0;
    int minTop = 0;
    int maxRight = 0;
    int maxBottom = 0;
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
    archive_ = resolveCaseInsensitivePath(gameDir_, "game_common.sue");
    descArchive_ = resolveCaseInsensitivePath(gameDir_, "desc_common.sue");
    std::error_code ec;
    hasArchive_ = std::filesystem::is_regular_file(archive_, ec);
    hasDesc_ = std::filesystem::is_regular_file(descArchive_, ec);
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
    if (const auto stem = resolveUnitStem(descArchive_, hasDesc_, id)) {
        auto stemIt = assetByStem_.find(*stem);
        if (stemIt == assetByStem_.end()) {
            std::optional<std::size_t> loadedIndex;
            if (auto loaded = tryLoadVehicleStemAsset(archive_, *stem)) {
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
    updateVehicleBounds(vu, asset.pck, asset.hot, asset.frameCount);
    const std::size_t index = units_.size();
    unitAsset_.reserve(index + 1u);
    units_.push_back(std::move(vu));
    unitAsset_.push_back(*assetIndex);
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
        const auto& unit = units_[i];
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

    for (const std::size_t idx : order) {
        const auto& unit = units_[idx];
        const auto& asset = assets_[unitAsset_[idx]];
        const bool alive = unit.alive && unit.hp > 0;
        const auto layers = alive
            ? collectVehicleLayers(asset.pck, asset.hot, asset.frameCount,
                                   unit.x, unit.y, unit.hullDir, unit.turretDir)
            : collectWreckLayers(asset.pck, asset.frameCount, unit.x, unit.y, unit.hullDir);
        const auto& pal = alive ? asset.col : asset.wreckCol;
        for (std::size_t i = 0; i < layers.count; ++i) {
            drawFrameWithDepth(patch, patchX, patchY, layers.items[i].ox, layers.items[i].oy,
                               layers.items[i].frame, pal, unit.y, depth);
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
    const auto& asset = assets_[unitAsset_[index]];
    updateVehicleBounds(next, asset.pck, asset.hot, asset.frameCount);

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

    const auto archive = resolveCaseInsensitivePath(gameDir, "game_common.sue");
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
            it->second.pck = readSueEntry(archive, spec.pck);
            it->second.col = readSueEntry(archive, spec.col);
            if (it->second.col.size() != 512)
                throw std::runtime_error("Invalid vehicle COL palette size");
            if (spec.hot) {
                it->second.hot = readSueEntry(archive, spec.hot);
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

    for (const auto& p : placements) {
        const auto& a = *p.asset;
        switch (a.kind) {
        case Kind::Tank: {
            // Block 0: intact hull (0..31); Block 6: intact turret (192..223) offset by hot0[dir].
            drawIndexedFrame(map, p.x, p.y, pckFrame(a.pck, p.dir), a.col);
            const int hotDx = i32(a.hot, std::size_t(p.dir) * 8u);
            const int hotDy = i32(a.hot, std::size_t(p.dir) * 8u + 4u);
            if (hotDx < -2048 || hotDx > 2048 || hotDy < -2048 || hotDy > 2048)
                throw std::runtime_error("Vehicle turret offset exceeds sprite bounds");
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
    (void)depth;
    const auto source = readMissionPlacements(missionPath);
    if (source.empty()) return 0;

    const auto archive = resolveCaseInsensitivePath(gameDir, "game_common.sue");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(archive, ec))
        throw std::runtime_error("Cannot open SUE archive");
    const auto descArchive = resolveCaseInsensitivePath(gameDir, "desc_common.sue");
    const bool hasDesc = std::filesystem::is_regular_file(descArchive, ec);

    std::map<std::string, std::optional<MissionVehicleAsset>> cache;
    std::vector<MissionVehicle> placements;
    for (const auto& unit : source) {
        if (unit.direction < 0 || unit.direction >= 32)
            throw std::runtime_error("Vehicle direction out of range");

        auto it = cache.find(unit.id);
        if (it == cache.end()) {
            std::optional<MissionVehicleAsset> loaded;
            if (const auto stem = resolveUnitStem(descArchive, hasDesc, unit.id)) {
                loaded = tryLoadVehicleStemAsset(archive, *stem);
            }
            it = cache.emplace(unit.id, std::move(loaded)).first;
        }
        if (it->second)
            placements.push_back({&*it->second, unit.x, unit.y, unsigned(unit.direction)});
    }

    std::stable_sort(placements.begin(), placements.end(),
                     [](const MissionVehicle& a, const MissionVehicle& b) { return a.y < b.y; });

    std::size_t drawn = 0;
    for (const auto& p : placements) {
        const auto& a = *p.asset;
        const auto layers = collectVehicleLayers(a.pck, a.hot, a.frameCount, p.x, p.y, p.dir, p.dir);
        for (std::size_t i = 0; i < layers.count; ++i) {
            drawIndexedFrame(map, layers.items[i].ox, layers.items[i].oy, layers.items[i].frame, a.col);
        }
        ++drawn;
    }
    return drawn;
}

std::optional<MapImage> loadAircraftSprite(const std::filesystem::path& gameDir,
                                           std::string_view unitId,
                                           unsigned dir32) {
    if (gameDir.empty() || unitId.empty()) return std::nullopt;
    std::error_code ec;
    const auto archive = resolveCaseInsensitivePath(gameDir, "game_common.sue");
    if (!std::filesystem::is_regular_file(archive, ec)) return std::nullopt;
    const auto descArchive = resolveCaseInsensitivePath(gameDir, "desc_common.sue");
    const bool hasDesc = std::filesystem::is_regular_file(descArchive, ec);

    const auto stem = resolveUnitStem(descArchive, hasDesc, unitId);
    if (!stem) return std::nullopt;
    const auto asset = tryLoadVehicleStemAsset(archive, *stem);
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
}
