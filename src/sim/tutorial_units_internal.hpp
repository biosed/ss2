#pragma once

// Internal helpers shared by the tutorial_units*.cpp translation units.
// Not part of the public sim API; include only from those files.

#include "sim/tutorial_units.hpp"
#include "sim/tutorial_coordinates.hpp"
#include "sim/det_math.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ss2 {
namespace tutorial_units_internal {
inline constexpr double kTwoPi = 6.283185307179586476925286766559;

inline SubCell pointerCell(int x, int y, int originX) {
    const auto [u,v] = tutorialPointerWorld(x,y,originX);
    return InfantryPathGrid::fromWorld(u,v);
}
inline SubCell placementCell(int x, int y, int originX) {
    const auto [u,v] = tutorialFootWorld(x,y,originX);
    return InfantryPathGrid::fromWorld(u,v);
}
inline bool sameTile(SubCell a, SubCell b) {
    return a.x/2 == b.x/2 && a.y/2 == b.y/2;
}
inline unsigned vehicleDirFromMapDelta(double dx, double dy) {
    const double du = dy + dx * 0.5;
    const double dv = dy - dx * 0.5;
    return det::detDir32(du, dv);
}
inline unsigned turnTowardDir32(unsigned current, unsigned target, unsigned maxStep = 2) {
    const int cur = static_cast<int>(current & 31u);
    const int tgt = static_cast<int>(target & 31u);
    int diff = (tgt - cur + 32) % 32;
    if (diff == 0) return static_cast<unsigned>(cur);
    if (diff > 16) diff -= 32;
    const int step = std::clamp(diff, -static_cast<int>(maxStep), static_cast<int>(maxStep));
    return static_cast<unsigned>((cur + step + 32) % 32);
}
inline int directionalArmorValue(
    int targetX, int targetY, unsigned targetHullDir,
    const std::array<assets::DirectionalArmor, static_cast<std::size_t>(assets::HitType::Count)>& armorTable,
    bool hasCatalogArmor, int attackerX, int attackerY, assets::HitType hitType) {
    if (!hasCatalogArmor || hitType == assets::HitType::None || hitType >= assets::HitType::Count) {
        return 0;
    }
    const auto& da = armorTable[static_cast<std::size_t>(hitType)];
    const unsigned incomingDir = vehicleDirFromMapDelta(static_cast<double>(attackerX - targetX),
                                                        static_cast<double>(attackerY - targetY));
    int diff = std::abs(int(incomingDir) - int(targetHullDir & 31u));
    if (diff > 16) diff = 32 - diff;
    return (diff <= 5) ? da.front : (diff <= 11 ? da.side : da.rear);
}
inline int applyVehicleDamage(int targetX, int targetY, unsigned targetHullDir,
                       const std::array<assets::DirectionalArmor, static_cast<std::size_t>(assets::HitType::Count)>& armorTable,
                       bool hasCatalogArmor, int attackerX, int attackerY,
                       int rawDamage, assets::HitType hitType) {
    if (!hasCatalogArmor || hitType == assets::HitType::None || hitType >= assets::HitType::Count) {
        return rawDamage;
    }
    const int armorVal = directionalArmorValue(
        targetX, targetY, targetHullDir, armorTable, hasCatalogArmor, attackerX, attackerY, hitType);
    if (armorVal >= 256) return 0;
    const int reduced = rawDamage - armorVal * 4;
    if (reduced > 0) return reduced;
    if (rawDamage > armorVal * 2) return std::max(1, rawDamage / 12);
    return 0;
}
struct ArmorHitOutcome {
    std::uint8_t outcome{0}; // 0=None, 1=Penetrated, 2=Glancing, 3=Ricochet
    std::uint8_t impactDir32{0};
};
inline ArmorHitOutcome computeArmorHitOutcome(
    int targetX, int targetY, unsigned targetHullDir,
    const std::array<assets::DirectionalArmor, static_cast<std::size_t>(assets::HitType::Count)>& armorTable,
    bool hasCatalogArmor, int attackerX, int attackerY,
    int rawDamage, assets::HitType hitType, int dealt) {
    if (rawDamage <= 0 && dealt <= 0) return {};
    const std::uint8_t impactDir32 = static_cast<std::uint8_t>(
        det::detDir32(static_cast<double>(attackerX - targetX), static_cast<double>(attackerY - targetY)) & 31u);
    if (dealt <= 0) {
        return {std::uint8_t{3}, impactDir32};
    }
    const int armorVal = directionalArmorValue(
        targetX, targetY, targetHullDir, armorTable, hasCatalogArmor, attackerX, attackerY, hitType);
    const std::uint8_t outcome = (armorVal > 0 && rawDamage <= armorVal * 4) ? std::uint8_t{2} : std::uint8_t{1};
    return {outcome, impactDir32};
}
inline void pushOrUpdateSoldierPose(std::vector<SoldierPose>& changed, const SoldierPose& pose) {
    const auto it = std::ranges::find(changed, pose.index, &SoldierPose::index);
    if (it == changed.end()) {
        changed.push_back(pose);
        return;
    }
    const bool wasFired = it->fired;
    const bool wasGrenade = it->threwGrenade;
    const bool wasFlamer = it->firedFlamer;
    const bool wasExplosive = it->firedExplosive;
    const int oldAmmo = it->ammo;
    const std::int8_t oldCrouch = it->crouchedState;
    const int oldTx = it->targetX;
    const int oldTy = it->targetY;
    *it = pose;
    if (wasFired && !pose.fired) {
        it->fired = true;
        it->targetX = oldTx;
        it->targetY = oldTy;
    }
    it->threwGrenade = it->threwGrenade || wasGrenade;
    it->firedFlamer = it->firedFlamer || wasFlamer;
    it->firedExplosive = it->firedExplosive || wasExplosive;
    if (pose.ammo < 0 && oldAmmo >= 0) it->ammo = oldAmmo;
    if (pose.crouchedState < 0 && oldCrouch >= 0) it->crouchedState = oldCrouch;
}
inline int scaleByExpa(int base, int delta, int expa) {
    return base + ((expa * delta) >> 15);
}
inline SubCell findSpawnCell(const InfantryPathGrid& grid, SubCell desired) {
    if (grid.canEnter(desired)) return desired;
    const int w = grid.width(), h = grid.height();
    if (w <= 0 || h <= 0) return desired;
    const int cx = std::clamp(desired.x, 0, w - 1);
    const int cy = std::clamp(desired.y, 0, h - 1);
    const std::int64_t baseOx = std::int64_t(cx) - desired.x;
    const std::int64_t baseOy = std::int64_t(cy) - desired.y;
    const std::int64_t baseD2 = baseOx * baseOx + baseOy * baseOy;

    std::optional<SubCell> best;
    std::int64_t bestD2 = std::numeric_limits<std::int64_t>::max();
    const auto consider = [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h) return;
        const SubCell cand{x, y};
        if (!grid.canEnter(cand)) return;
        const std::int64_t ox = std::int64_t(x) - desired.x;
        const std::int64_t oy = std::int64_t(y) - desired.y;
        const std::int64_t d2 = ox * ox + oy * oy;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = cand;
        }
    };

    const int maxR = std::max({cx, w - 1 - cx, cy, h - 1 - cy});
    for (int r = 0; r <= maxR; ++r) {
        if (best && std::int64_t(r) * r + baseD2 >= bestD2) break;
        if (r == 0) {
            consider(cx, cy);
            continue;
        }
        for (int dx = -r; dx <= r; ++dx) {
            consider(cx + dx, cy - r);
            consider(cx + dx, cy + r);
        }
        for (int dy = -r + 1; dy <= r - 1; ++dy) {
            consider(cx - r, cy + dy);
            consider(cx + r, cy + dy);
        }
    }
    return best.value_or(desired);
}
inline std::pair<int, int> approachPointNear(double fromX, double fromY, double toX, double toY, double offsetPx,
                                      int maxW, int maxH) {
    const double dx = fromX - toX;
    const double dy = fromY - toY;
    const double dist = det::detHypot(dx, dy);
    double ax = toX;
    double ay = toY;
    if (dist > 1e-3 && dist > offsetPx) {
        ax = toX + (dx / dist) * offsetPx;
        ay = toY + (dy / dist) * offsetPx;
    } else if (dist <= 1e-3) {
        ax = toX + offsetPx;
    } else {
        ax = fromX;
        ay = fromY;
    }
    const int cx = std::clamp(det::detRoundToInt(ax), 0, std::max(0, maxW - 1));
    const int cy = std::clamp(det::detRoundToInt(ay), 0, std::max(0, maxH - 1));
    return {cx, cy};
}
}  // namespace tutorial_units_internal
using namespace tutorial_units_internal;
}  // namespace ss2
