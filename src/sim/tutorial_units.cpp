#include "sim/tutorial_units.hpp"
#include "sim/tutorial_coordinates.hpp"
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
namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kVehicleArrivePx = 8.0;
constexpr double kInfantryVsVehicleRangePx = 240.0;
constexpr std::array<int, 8> kTileDx{1, 1, 0, -1, -1, -1, 0, 1};
constexpr std::array<int, 8> kTileDy{0, 1, 1, 1, 0, -1, -1, -1};

SubCell pointerCell(int x, int y, int originX) {
    const auto [u,v] = tutorialPointerWorld(x,y,originX);
    return InfantryPathGrid::fromWorld(u,v);
}
SubCell placementCell(int x, int y, int originX) {
    const auto [u,v] = tutorialFootWorld(x,y,originX);
    return InfantryPathGrid::fromWorld(u,v);
}
bool sameTile(SubCell a, SubCell b) {
    return a.x/2 == b.x/2 && a.y/2 == b.y/2;
}
std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}
bool isAntiVehicleSoldierId(std::string_view lowId) {
    static constexpr std::array<std::string_view, 8> kTags{{
        "bazooka", "panzerfaust", "piat", "flamer", "ats", "fatg", "antitank", "anti_tank"
    }};
    return std::ranges::any_of(kTags, [&](std::string_view tag) {
        return lowId.find(tag) != std::string_view::npos;
    });
}
bool isUnarmedVehicleId(std::string_view id) {
    const std::string low = toLowerAscii(id);
    static constexpr std::array<std::string_view, 21> kUnarmedTags{{
        "jeep", "kubel", "truck", "opel", "blitz", "studeb", "zis5", "zis-5",
        "maultier", "mack", "med", "supply", "ambul", "tractor",
        "horch", "bedford", "gmc", "cckw", "morris", "sdkfz.11", "type.94"
    }};
    return std::ranges::any_of(kUnarmedTags, [&](std::string_view tag) {
        return low.find(tag) != std::string_view::npos;
    });
}
std::uint8_t directionFromDelta(int du, int dv) {
    if (du == 0 && dv == 0) return 0;
    const double angle = std::atan2(double(dv), double(du));
    const int oct = int(std::llround(angle / (kTwoPi / 8.0)));
    return static_cast<std::uint8_t>((oct % 8 + 8) % 8);
}
unsigned vehicleDirFromMapDelta(double dx, double dy) {
    const double du = dy + dx * 0.5;
    const double dv = dy - dx * 0.5;
    if (du == 0.0 && dv == 0.0) return 0u;
    const double angle = std::atan2(dv, du);
    const long long step32 = std::llround(angle / (kTwoPi / 32.0));
    return static_cast<unsigned>(((step32 % 32) + 32) % 32);
}
unsigned turnTowardDir32(unsigned current, unsigned target, unsigned maxStep = 2) {
    const int cur = static_cast<int>(current & 31u);
    const int tgt = static_cast<int>(target & 31u);
    int diff = (tgt - cur + 32) % 32;
    if (diff == 0) return static_cast<unsigned>(cur);
    if (diff > 16) diff -= 32;
    const int step = std::clamp(diff, -static_cast<int>(maxStep), static_cast<int>(maxStep));
    return static_cast<unsigned>((cur + step + 32) % 32);
}
unsigned dirDelta32(unsigned a, unsigned b) {
    return std::min((a - b) & 31u, (b - a) & 31u);
}
int applyVehicleDamage(int targetX, int targetY, unsigned targetHullDir,
                       const std::array<assets::DirectionalArmor, static_cast<std::size_t>(assets::HitType::Count)>& armorTable,
                       bool hasCatalogArmor, int attackerX, int attackerY,
                       int rawDamage, assets::HitType hitType) {
    if (!hasCatalogArmor || hitType == assets::HitType::None || hitType >= assets::HitType::Count) {
        return rawDamage;
    }
    const auto& da = armorTable[static_cast<std::size_t>(hitType)];
    const unsigned incomingDir = vehicleDirFromMapDelta(static_cast<double>(attackerX - targetX),
                                                        static_cast<double>(attackerY - targetY));
    int diff = std::abs(int(incomingDir) - int(targetHullDir & 31u));
    if (diff > 16) diff = 32 - diff;
    const int armorVal = (diff <= 5) ? da.front : (diff <= 11 ? da.side : da.rear);
    if (armorVal >= 256) return 0;
    const int reduced = rawDamage - armorVal * 4;
    if (reduced > 0) return reduced;
    if (rawDamage > armorVal * 2) return std::max(1, rawDamage / 12);
    return 0;
}
void pushOrUpdateSoldierPose(std::vector<SoldierPose>& changed, const SoldierPose& pose) {
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
int scaleByExpa(int base, int delta, int expa) {
    return base + ((expa * delta) >> 15);
}
int sniperHitPercent(int armor) {
    switch (armor) {
    case 0: return 100;
    case 1: return 75;
    case 2: return 50;
    case 3: return 25;
    default: return 10;
    }
}
SubCell findSpawnCell(const InfantryPathGrid& grid, SubCell desired) {
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

bool needsSupplyService(int hp, int maxHp, int ammo, int maxAmmo, int ammo2, int maxAmmo2) {
    return hp < maxHp || (maxAmmo > 0 && ammo < maxAmmo) || (maxAmmo2 > 0 && ammo2 < maxAmmo2);
}
std::pair<int, int> approachPointNear(double fromX, double fromY, double toX, double toY, double offsetPx,
                                      int maxW, int maxH) {
    const double dx = fromX - toX;
    const double dy = fromY - toY;
    const double dist = std::hypot(dx, dy);
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
    const int cx = std::clamp(static_cast<int>(std::lround(ax)), 0, std::max(0, maxW - 1));
    const int cy = std::clamp(static_cast<int>(std::lround(ay)), 0, std::max(0, maxH - 1));
    return {cx, cy};
}
constexpr std::array<std::uint8_t, 8> kSnapshotMagicV1{'S', 'S', '2', 'S', 'N', 'A', 'P', '1'};
constexpr std::array<std::uint8_t, 8> kSnapshotMagicV2{'S', 'S', '2', 'S', 'N', 'A', 'P', '2'};
constexpr std::array<std::uint8_t, 8> kSnapshotMagicV3{'S', 'S', '2', 'S', 'N', 'A', 'P', '3'};

void appendU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void appendI32(std::vector<std::uint8_t>& out, std::int32_t v) {
    appendU32(out, static_cast<std::uint32_t>(v));
}

struct SnapshotReader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos{0};

    bool readU8(std::uint8_t& out) {
        if (pos >= bytes.size()) return false;
        out = bytes[pos++];
        return true;
    }
    bool readBool(bool& out) {
        std::uint8_t v = 0;
        if (!readU8(v)) return false;
        out = v != 0;
        return true;
    }
    bool readU32(std::uint32_t& out) {
        if (pos + 4 > bytes.size()) return false;
        out = static_cast<std::uint32_t>(bytes[pos]) |
              (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
        pos += 4;
        return true;
    }
    bool readI32(std::int32_t& out) {
        std::uint32_t raw = 0;
        if (!readU32(raw)) return false;
        out = static_cast<std::int32_t>(raw);
        return true;
    }
    bool readBytes(std::vector<std::uint8_t>& out, std::uint32_t len) {
        if (len > bytes.size() - pos) return false;
        out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
        return true;
    }
};
}
TutorialUnits::TutorialUnits(InfantryPathGrid grid, std::span<const assets::Infantry> units,
                             std::span<const assets::Placement> vehicles,
                             std::optional<FogGrid> fog, int difficulty, bool autoEngage,
                             std::vector<assets::VehicleUnit> vehicleUnits,
                             std::shared_ptr<const assets::UnitCatalog> catalog)
    : grid_(std::move(grid)), fog_(std::move(fog)), difficulty_(difficulty),
      autoEngage_(autoEngage), catalog_(std::move(catalog)) {
    const int tileW = grid_.width() / 2, tileH = grid_.height() / 2;
    originX_ = tileH * 32;
    mapPixelW_ = (tileW + tileH) * 32;
    mapPixelH_ = (tileW + tileH) * 16;
    for (const auto& unit : units) {
        (void)appendSoldier(unit, placementCell(unit.x, unit.y, originX_), missionExpaToSim(unit.expa));
    }
    if (!vehicleUnits.empty()) {
        for (const auto& vu : vehicleUnits) {
            (void)appendVehicle(vu);
        }
    } else {
        for (const auto& p : vehicles) {
            assets::VehicleUnit vu{};
            vu.id = p.id;
            vu.x = p.x;
            vu.y = p.y;
            vu.hp = p.hp;
            vu.ammo = p.ammo;
            vu.group = p.group;
            vu.owner = p.owner;
            vu.hullDir = static_cast<unsigned>(p.direction) & 31u;
            vu.turretDir = vu.hullDir;
            vu.alive = p.hp > 0;
            vu.visible = true;
            vu.hasCrew = (p.owner != 3) || !p.passengers.empty();
            vu.expa = p.expa;
            (void)appendVehicle(vu);
        }
    }
    if (fog_) refreshSpotting(nullptr, false);
}

bool TutorialUnits::mapBounds(int x, int y) const {
    return x >= 0 && x < mapPixelW_ && y >= 0 && y < mapPixelH_;
}

bool TutorialUnits::isElevatedMapPoint(int mapX, int mapY) const {
    if (!fog_) return false;
    const SubCell cell = placementCell(mapX, mapY, originX_);
    return fog_->isElevated(cell.x / 2, cell.y / 2);
}

int TutorialUnits::scaleVehicleDamageForDifficulty(int targetOwner, int rawDamage) const {
    if (rawDamage <= 0 || targetOwner != 0) return rawDamage;
    if (difficulty_ == 1) return std::max(1, (rawDamage * 80) / 100);
    if (difficulty_ == 3) return std::max(1, (rawDamage * 115) / 100);
    return rawDamage;
}

bool TutorialUnits::isVehicleUnbuttoned(const Vehicle& v) const {
    return v.hasCrew && v.marchEnabled && v.hatchOpen &&
           (v.lastCombatTick == 0u || tick_ >= v.lastCombatTick + 125u);
}

void TutorialUnits::markVehicleCombat(Vehicle& veh) {
    const unsigned ct = tick_ > 0u ? tick_ : 1u;
    const bool wasButtoned = veh.lastCombatTick != 0u && tick_ < veh.lastCombatTick + 125u;
    veh.lastCombatTick = ct;
    if ((veh.marchSightBonus > 0 || (veh.marchEnabled && veh.hatchOpen)) && !wasButtoned) {
        fogDirty_ = true;
    }
}

bool TutorialUnits::canMove(size_t index, int x, int y) const {
    if (!mapBounds(x,y) || index >= soldiers_.size()) return false;
    const auto& soldier = soldiers_[index];
    if (soldier.owner != 0 || soldier.hp <= 0 || soldier.embarked || soldier.garrisonBuilding) return false;
    const auto dest = pointerCell(x,y,originX_);
    if (!grid_.inBounds(dest)) return false;
    return (soldier.goal && sameTile(dest, *soldier.goal)) || grid_.hasTerrainRoute(soldier.cell,dest);
}

bool TutorialUnits::order(size_t index, int x, int y) {
    if (!canMove(index,x,y)) return false;
    auto& soldier = soldiers_[index];
    soldier.boardVehicleTarget.reset();
    soldier.captureVehicleTarget.reset();
    soldier.garrisonTarget.reset();
    soldier.attackTarget.reset();
    soldier.attackVehicleTarget.reset();
    soldier.attackBuildingTarget.reset();
    soldier.attackGround.reset();
    soldier.healTarget.reset();
    soldier.escortVehicleTarget.reset();
    soldier.escortSoldierTarget.reset();
    soldier.disarmMineTarget.reset();
    soldier.queuedGoals.clear();
    soldier.attackMove = false;
    soldier.patrolOrigin.reset();
    soldier.patrolDest.reset();
    const auto dest = pointerCell(x,y,originX_);
    // 0x100369d0 compares tile goals and preserves the current RUN substep.
    if (soldier.goal && sameTile(dest, *soldier.goal)) return true;
    soldier.goal = dest;
    soldier.route.clear();
    soldier.cursor = 0;
    soldier.planning = {};
    // Gold clears the pending request at 0x100369d0. The unit asks for
    // another route at 0x10021a50 once its current RUN substep finishes.
    soldier.needsRoute = false;
    return true;
}

bool TutorialUnits::orderSoldierInternal(size_t index, int x, int y) {
    if (!mapBounds(x, y) || index >= soldiers_.size()) return false;
    auto& soldier = soldiers_[index];
    if (soldier.hp <= 0 || soldier.embarked || soldier.garrisonBuilding) return false;
    const auto dest = findSpawnCell(grid_, pointerCell(x, y, originX_));
    if (!grid_.inBounds(dest) || !grid_.hasTerrainRoute(soldier.cell, dest)) return false;
    soldier.boardVehicleTarget.reset();
    soldier.captureVehicleTarget.reset();
    soldier.garrisonTarget.reset();
    soldier.attackTarget.reset();
    soldier.attackVehicleTarget.reset();
    soldier.attackBuildingTarget.reset();
    soldier.attackGround.reset();
    soldier.healTarget.reset();
    soldier.escortVehicleTarget.reset();
    soldier.escortSoldierTarget.reset();
    soldier.disarmMineTarget.reset();
    if (soldier.goal && sameTile(dest, *soldier.goal)) return true;
    soldier.goal = dest;
    soldier.route.clear();
    soldier.cursor = 0;
    soldier.planning = {};
    soldier.needsRoute = false;
    return true;
}

void TutorialUnits::moveVehicleInternal(Vehicle& v, int mapX, int mapY) {
    if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy) return;
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    v.moveTarget = std::pair{clampedX, clampedY};
    v.waypoints.clear();
    v.attackTarget.reset();
    v.attackGround.reset();
    v.attackBuildingTarget.reset();
    v.chasingAttackTarget = false;
    v.reverseMove = false;
    v.facingTargetDir.reset();
    v.supportTargetVehicle.reset();
    v.supportTargetBuilding.reset();
    v.supportTargetSoldier.reset();
    v.escortVehicleTarget.reset();
    v.escortSoldierTarget.reset();
    v.patrolOrigin.reset();
    v.patrolDest.reset();
}

std::vector<TutorialUnits::Destination> TutorialUnits::formationTargets(
        std::span<const size_t> ids, int mapX, int mapY) const {
    if (ids.empty() || ids.size() > soldiers_.size() || !mapBounds(mapX, mapY))
        return {};
    std::vector<SubCell> sources;
    sources.reserve(ids.size());
    for (size_t id : ids) {
        if (id >= soldiers_.size() || soldiers_[id].owner != 0 || soldiers_[id].hp <= 0 ||
            soldiers_[id].embarked || soldiers_[id].garrisonBuilding) return {};
        sources.push_back(soldiers_[id].cell);
    }
    const auto tiles = grid_.formationDestinations(sources, pointerCell(mapX,mapY,originX_));
    std::vector<Destination> result;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const int x = tiles[i].x/2, y = tiles[i].y/2;
        result.push_back({ids[i],originX_+(x-y)*32,(x+y)*16+16});
    }
    return result;
}

bool TutorialUnits::canMoveSelection(std::span<const size_t> ids, int x, int y) const {
    const auto targets = formationTargets(ids,x,y);
    return std::ranges::any_of(targets, [&](const auto& t) { return canMove(t.soldier,t.mapX,t.mapY); });
}

size_t TutorialUnits::orderSelection(std::span<const size_t> ids, int x, int y, bool append) {
    size_t accepted = 0;
    for (const auto& t : formationTargets(ids, x, y)) {
        auto& s = soldiers_[t.soldier];
        const bool busy = s.motion || s.goal || !s.route.empty();
        if (append && busy && s.owner == 0 && s.hp > 0 && !s.embarked && mapBounds(t.mapX, t.mapY)) {
            const auto dest = pointerCell(t.mapX, t.mapY, originX_);
            if (grid_.inBounds(dest) && grid_.hasTerrainRoute(s.cell, dest)) {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.escortVehicleTarget.reset();
                s.escortSoldierTarget.reset();
                s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
                s.attackGround.reset();
                s.queuedGoals.emplace_back(t.mapX, t.mapY);
                ++accepted;
                continue;
            }
        }
        if (order(t.soldier, t.mapX, t.mapY)) ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderAttackMove(std::span<const size_t> ids, int mapX, int mapY, bool append) {
    size_t accepted = 0;
    for (const auto& t : formationTargets(ids, mapX, mapY)) {
        auto& s = soldiers_[t.soldier];
        const bool busy = s.motion || s.goal || !s.route.empty();
        if (append && busy && s.owner == 0 && s.hp > 0 && !s.embarked && mapBounds(t.mapX, t.mapY)) {
            const auto dest = pointerCell(t.mapX, t.mapY, originX_);
            if (grid_.inBounds(dest) && grid_.hasTerrainRoute(s.cell, dest)) {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.escortVehicleTarget.reset();
                s.escortSoldierTarget.reset();
                s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
                s.attackGround.reset();
                s.queuedGoals.emplace_back(t.mapX, t.mapY);
                s.attackMove = true;
                ++accepted;
                continue;
            }
        }
        if (order(t.soldier, t.mapX, t.mapY)) {
            s.attackMove = true;
            ++accepted;
        }
    }
    return accepted;
}

size_t TutorialUnits::orderPatrol(std::span<const size_t> ids, int mapX, int mapY) {
    size_t accepted = 0;
    for (const auto& t : formationTargets(ids, mapX, mapY)) {
        auto& s = soldiers_[t.soldier];
        const auto [su, sv] = soldierWorld(s);
        const std::pair origin{originX_ + su - sv, (su + sv) / 2};
        const std::pair dest{t.mapX, t.mapY};
        if (orderSoldierInternal(t.soldier, t.mapX, t.mapY)) {
            s.queuedGoals.clear();
            if (std::hypot(static_cast<double>(dest.first - origin.first),
                           static_cast<double>(dest.second - origin.second)) >= 16.0) {
                s.patrolOrigin = origin;
                s.patrolDest = dest;
            } else {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
            }
            s.patrolToDest = true;
            s.attackMove = true;
            ++accepted;
        }
    }
    return accepted;
}

size_t TutorialUnits::orderScatter(std::span<const size_t> ids) {
    struct Entry {
        size_t id;
        double sx;
        double sy;
    };
    std::vector<Entry> eligible;
    eligible.reserve(ids.size());
    std::vector<size_t> seen;
    seen.reserve(ids.size());
    double sumX = 0.0, sumY = 0.0;
    for (const size_t id : ids) {
        if (id >= soldiers_.size() || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        const auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        const auto [su, sw] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sw);
        const double sy = static_cast<double>(su + sw) * 0.5;
        eligible.push_back({id, sx, sy});
        sumX += sx;
        sumY += sy;
    }
    if (eligible.empty()) return 0;
    const double cx = sumX / static_cast<double>(eligible.size());
    const double cy = sumY / static_cast<double>(eligible.size());
    std::vector<SubCell> claimedDest;
    claimedDest.reserve(eligible.size());
    static constexpr double kRadii[] = {88.0, 68.0, 52.0, 36.0};
    static constexpr double kAngles[] = {0.0, 0.45, -0.45, 0.90, -0.90, 1.50, -1.50};
    size_t accepted = 0;
    for (size_t k = 0; k < eligible.size(); ++k) {
        const auto& e = eligible[k];
        auto& s = soldiers_[e.id];
        double vx = e.sx - cx;
        double vy = e.sy - cy;
        double len = std::hypot(vx, vy);
        if (len < 6.0) {
            const double theta = 2.399963229728653 * static_cast<double>(k + 1u);
            vx = std::cos(theta);
            vy = std::sin(theta);
            len = 1.0;
        }
        const double ux = vx / len;
        const double uy = vy / len;
        bool placed = false;
        for (const double rad : kRadii) {
            for (const double ang : kAngles) {
                const double c = std::cos(ang), sn = std::sin(ang);
                const double rx = ux * c - uy * sn;
                const double ry = ux * sn + uy * c;
                const int tx = static_cast<int>(std::lround(e.sx + rx * rad));
                const int ty = static_cast<int>(std::lround(e.sy + ry * rad));
                if (!mapBounds(tx, ty)) continue;
                const SubCell dest = findSpawnCell(grid_, pointerCell(tx, ty, originX_));
                if (!grid_.inBounds(dest) || sameTile(dest, s.cell)) continue;
                if (std::ranges::any_of(claimedDest, [&](const SubCell& d) { return sameTile(d, dest); })) continue;
                if (!grid_.hasTerrainRoute(s.cell, dest)) continue;
                if (orderSoldierInternal(e.id, tx, ty)) {
                    s.queuedGoals.clear();
                    s.patrolOrigin.reset();
                    s.patrolDest.reset();
                    s.attackMove = false;
                    claimedDest.push_back(dest);
                    ++accepted;
                    placed = true;
                    break;
                }
            }
            if (placed) break;
        }
    }
    return accepted;
}

bool TutorialUnits::isValidAttackTarget(size_t targetIndex, int viewerOwner) const {
    if (!fog_ || targetIndex >= soldiers_.size()) return false;
    const auto& target = soldiers_[targetIndex];
    if (target.hp <= 0 || target.embarked || target.garrisonBuilding || !isHostile(viewerOwner, target.owner)) return false;
    return (target.spottedMask & (1u << viewerOwner)) != 0;
}

size_t TutorialUnits::orderAttack(std::span<const size_t> ids, size_t targetIndex) {
    if (!isValidAttackTarget(targetIndex, 0)) return 0;
    size_t accepted = 0;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.ammo <= 0) continue;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget = targetIndex;
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::stopSelection(std::span<const size_t> ids) {
    size_t stopped = 0;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked) continue;
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++stopped;
    }
    return stopped;
}

bool TutorialUnits::toggleHoldFire(std::span<const size_t> ids) {
    bool anyFree = false;
    for (size_t id : ids) {
        if (id < soldiers_.size() && soldiers_[id].owner == 0 && soldiers_[id].hp > 0 && !soldiers_[id].holdFire)
            anyFree = true;
    }
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0) continue;
        s.holdFire = anyFree;
        if (anyFree) s.autoTarget.reset();
    }
    return anyFree;
}

bool TutorialUnits::toggleCrouch(std::span<const size_t> ids) {
    bool anyStanding = false;
    for (size_t id : ids) {
        if (id < soldiers_.size() && soldiers_[id].owner == 0 && soldiers_[id].hp > 0 && !soldiers_[id].crouched)
            anyStanding = true;
    }
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0) continue;
        s.crouched = anyStanding;
    }
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting();
    }
    return anyStanding;
}

bool TutorialUnits::isSoldierCamouflaged(const Soldier& s) const {
    return s.isSniper && s.hp > 0 && !s.embarked && !s.garrisonBuilding &&
           s.crouched && !s.motion && s.route.empty() &&
           (s.lastFireTick == 0 || (tick_ >= s.lastFireTick && tick_ - s.lastFireTick >= 35u));
}

bool TutorialUnits::isSoldierCamouflaged(size_t index) const {
    return index < soldiers_.size() && isSoldierCamouflaged(soldiers_[index]);
}

int TutorialUnits::officerAuraDecayAt(int owner, double sx, double sy) const {
    if (owner < 0 || owner >= 4) return 1;
    int best = 1;
    for (size_t j = 0; j < soldiers_.size(); ++j) {
        const auto& other = soldiers_[j];
        if (other.hp <= 0 || other.embarked || other.owner != owner || !other.isOfficer) {
            continue;
        }
        double ox = 0.0;
        double oy = 0.0;
        if (other.garrisonBuilding) {
            const size_t ob = *other.garrisonBuilding;
            if (ob >= buildings_.size() || buildings_[ob].destroyed) continue;
            ox = static_cast<double>(buildings_[ob].x);
            oy = static_cast<double>(buildings_[ob].y);
        } else {
            const auto [ou, ov] = soldierWorld(other);
            ox = static_cast<double>(originX_ + ou - ov);
            oy = static_cast<double>((ou + ov) / 2);
        }
        if (std::hypot(ox - sx, oy - sy) <= static_cast<double>(other.officerAuraRadius)) {
            best = std::max(best, other.moraleRallyRate);
        }
    }
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.owner != owner || !v.isPropaganda) {
            continue;
        }
        if (std::hypot(v.fx - sx, v.fy - sy) <= 200.0) {
            best = std::max(best, 3);
        }
    }
    return best;
}

int TutorialUnits::officerAuraDecay(size_t soldierIdx) const {
    if (soldierIdx >= soldiers_.size()) return 1;
    const auto& s = soldiers_[soldierIdx];
    if (s.hp <= 0 || s.embarked) return 1;
    if (s.garrisonBuilding) {
        const size_t bIdx = *s.garrisonBuilding;
        if (bIdx >= buildings_.size() || buildings_[bIdx].destroyed) return 1;
        return officerAuraDecayAt(s.owner, static_cast<double>(buildings_[bIdx].x),
                                  static_cast<double>(buildings_[bIdx].y));
    }
    const auto [su, sv] = soldierWorld(s);
    const double sx = static_cast<double>(originX_ + su - sv);
    const double sy = static_cast<double>((su + sv) / 2);
    return officerAuraDecayAt(s.owner, sx, sy);
}

bool TutorialUnits::hasNearbyFriendlyOfficer(size_t soldierIdx) const {
    return officerAuraDecay(soldierIdx) > 1;
}

void TutorialUnits::applySuppression(size_t soldierIdx, int addTicks, bool allowGarrisoned) {
    if (soldierIdx >= soldiers_.size() || addTicks <= 0) return;
    auto& tgt = soldiers_[soldierIdx];
    if (tgt.hp <= 0 || tgt.embarked || (!allowGarrisoned && tgt.garrisonBuilding)) return;
    int effective = addTicks;
    if (tgt.moraleResist >= 5000) {
        effective = std::max(1, addTicks / 4);
    } else if (tgt.moraleResist >= 1000) {
        effective = std::max(1, addTicks / 2);
    }
    tgt.suppressionTicks = std::min(35, tgt.suppressionTicks + effective);
}

void TutorialUnits::applyVehicleSuppression(size_t vehicleIdx, int addTicks, int threatX, int threatY) {
    if (vehicleIdx >= vehicles_.size() || addTicks <= 0) return;
    auto& tv = vehicles_[vehicleIdx];
    if (!tv.alive || tv.hp <= 0 || !tv.hasCrew || tv.owner == 3) return;
    int effective = addTicks;
    if (tv.moraleResist >= 10000) {
        effective = std::max(1, addTicks / 4);
    } else if (tv.moraleResist >= 4000) {
        effective = std::max(1, addTicks / 2);
    }
    const int cap = (tv.moraleRndMove < 0) ? 60 : 35;
    tv.suppressionTicks = std::min(cap, tv.suppressionTicks + effective);
    if (tv.moraleRndMove < 0 && tv.moraleResist < 10000 && tv.speedPx > 0.0 &&
        !tv.towedBy && !tv.isMoveImpaired() &&
        (tv.lastPanicTick == 0u || tick_ >= tv.lastPanicTick + 60u)) {
        const int panicThresh = std::clamp((-tv.moraleRndMove) / 400, 38, 56);
        if (tv.suppressionTicks >= panicThresh) {
            double dx = (threatX >= 0 && threatY >= 0) ? (tv.fx - static_cast<double>(threatX)) : 0.0;
            double dy = (threatX >= 0 && threatY >= 0) ? (tv.fy - static_cast<double>(threatY)) : 0.0;
            double len = std::hypot(dx, dy);
            if (len < 1e-3) {
                const double ang = (static_cast<double>((tv.hullDir + 16u) & 31u) / 32.0) * kTwoPi;
                const double du = std::cos(ang);
                const double dv = std::sin(ang);
                dx = du - dv;
                dy = 0.5 * (du + dv);
                len = std::max(1e-3, std::hypot(dx, dy));
            }
            const int fleeX = std::clamp(static_cast<int>(std::lround(tv.fx + (dx / len) * 72.0)),
                                         32, std::max(32, mapPixelW_ - 32));
            const int fleeY = std::clamp(static_cast<int>(std::lround(tv.fy + (dy / len) * 72.0)),
                                         32, std::max(32, mapPixelH_ - 32));
            tv.pendingTowGun.reset();
            tv.queuedOrders.clear();
            tv.attackMove = false;
            tv.attackMoveEngaging = false;
            moveVehicleInternal(tv, fleeX, fleeY);
            if (tv.canMoveBackward) tv.reverseMove = true;
            tv.lastPanicTick = tick_ > 0u ? tick_ : 1u;
        }
    }
}

void TutorialUnits::applySoldierShotReload(Soldier& s) {
    ++s.attackBurst;
    int reloadVal = scaleByExpa(s.desc.reloadBase, s.desc.reloadDelta, s.expa);
    if (s.attackBurst >= s.desc.burstCount) {
        s.attackBurst = 0;
        reloadVal = scaleByExpa(s.desc.burstReloadBase, s.desc.burstReloadDelta, s.expa);
    }
    if (s.isSuppressed() && !s.fearless) {
        reloadVal = (reloadVal * 135 + 50) / 100;
    }
    s.attackReload = static_cast<std::int16_t>(std::max(1, reloadVal));
}

bool TutorialUnits::toggleBinoculars(std::span<const size_t> ids) {
    return toggleBinocularsWithPoses(ids, nullptr);
}

bool TutorialUnits::toggleBinocularsWithPoses(std::span<const size_t> ids, std::vector<SoldierPose>* changed) {
    bool anyOfficer = false;
    bool anyOff = false;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        const auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || !s.hasBinoculars) continue;
        anyOfficer = true;
        if (!s.binoculars) anyOff = true;
    }
    if (!anyOfficer) return false;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || !s.hasBinoculars) continue;
        s.binoculars = anyOff;
    }
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(changed);
    }
    if (changed) {
        for (auto& pose : *changed) {
            if (pose.index < soldiers_.size()) {
                pose.expa = soldiers_[pose.index].expa;
                pose.kills = soldiers_[pose.index].kills;
                pose.atGrenades = soldiers_[pose.index].atGrenades;
                pose.mineAmmo = soldiers_[pose.index].mineAmmo;
                pose.ammo = soldiers_[pose.index].ammo;
                pose.suppressed = soldiers_[pose.index].isSuppressed();
            }
        }
    }
    return anyOff;
}

size_t TutorialUnits::appendSoldier(const assets::Infantry& unit, SubCell cell, int expa) {
    const auto desc = InfantryCombatDesc::forUnitId(unit.id);
    Soldier s{};
    s.cell = cell;
    s.owner = unit.owner;
    s.expa = std::clamp(expa, 0, 0x8000);
    s.group = unit.group;
    s.direction = static_cast<std::uint8_t>(unit.direction & 7u);
    const std::string lowId = toLowerAscii(unit.id);
    const auto* spec = catalog_ ? catalog_->find(unit.id) : nullptr;
    s.desc = desc;
    if (spec && !spec->isVehicle) {
        s.attackPref = spec->attackPref;
        s.hasAttackPref = spec->hasAttackPref;
        if (spec->health > 0) s.desc.maxHp = spec->health;
        if (spec->ammo1 > 0) s.desc.maxAmmo = spec->ammo1;
        if (spec->sight > 0) {
            s.desc.sightBase = std::clamp(spec->sight, 6, 20);
            s.desc.crouchSightBase = std::max(4, s.desc.sightBase - 3);
        }
        if (spec->crouchSight > 0) {
            s.desc.crouchSightBase = std::clamp(spec->crouchSight, 4, 20);
            s.desc.crouchSightDelta = std::max(0, spec->crouchSightMax - spec->crouchSight);
        }
        if (spec->shot1.damage > 0 && spec->shot1.range > 0) {
            s.desc.damageBase = spec->shot1.damage;
            s.desc.damageDelta = std::max(0, spec->shot1.damageMax - spec->shot1.damage);
            s.desc.rangeBase = spec->shot1.range;
            s.desc.rangeDelta = std::max(0, spec->shot1.rangeMax - spec->shot1.range);
            if (spec->shot1.deadzone > 0) s.desc.deadzoneBase = spec->shot1.deadzone;
            if (spec->shot1.reloadTicks > 0) {
                s.desc.reloadBase = spec->shot1.reloadTicks;
                s.desc.reloadDelta = -std::max(0, s.desc.reloadBase / 6);
            }
            if (spec->shot1.burstShots > 0) s.desc.burstCount = spec->shot1.burstShots;
            if (spec->shot1.burstReloadTicks > 0) {
                s.desc.burstReloadBase = spec->shot1.burstReloadTicks;
                s.desc.burstReloadDelta = -std::max(0, s.desc.burstReloadBase / 6);
            }
        }
    }
    s.hp = unit.hp > 0 ? unit.hp : s.desc.maxHp;
    s.maxHp = std::max(1, std::max(s.desc.maxHp, s.hp));
    if (spec && !spec->isVehicle && spec->ammo1 > 0) {
        s.ammo = unit.ammo > 0 ? std::max(1, (spec->ammo1 * std::clamp(unit.ammo, 1, 100)) / 100) : spec->ammo1;
    } else {
        s.ammo = unit.ammo > 0 ? unit.ammo : s.desc.maxAmmo;
    }
    s.isMedic = lowId.find("medic") != std::string::npos || (spec && spec->isMedicInfantry());
    s.isOfficer = lowId.find("officer") != std::string::npos || lowId.find("general") != std::string::npos ||
                  (spec && spec->isOfficerInfantry());
    s.moraleResist = (spec && !spec->isVehicle) ? std::max(0, spec->moraleResist) : 0;
    s.fearless = (spec && !spec->isVehicle) ? spec->isFearless() : false;
    s.resupplyCost = (spec && !spec->isVehicle) ? std::max(0, spec->shot1.resupplyCost) : 0;
    s.canBeCrew = (spec && !spec->isVehicle) ? std::max(0, spec->canBeCrew) : 0;
    s.unitRadius = (spec && !spec->isVehicle && spec->unitRadius > 0) ? spec->unitRadius : 5;
    s.canFireFromInside = (spec && !spec->isVehicle) ? spec->canFireFromInside : true;
    s.crouchToFire = (spec && !spec->isVehicle) ? spec->crouchToFire : false;
    s.expaOnHit = (spec && !spec->isVehicle) ? std::max(0, spec->expaOnHit) : 0;
    if (spec && !spec->isVehicle) {
        if (spec->moveSpeed > 0.01) {
            s.standStepBase = static_cast<unsigned>(std::clamp<long>(std::lround(spec->moveSpeed * 16.0), 4l, 14l));
            const double msMax = spec->moveSpeedMax >= spec->moveSpeed ? spec->moveSpeedMax : spec->moveSpeed;
            s.standStepMax = static_cast<unsigned>(
                std::clamp<long>(std::lround(msMax * 16.0), static_cast<long>(s.standStepBase), 14l));
        }
        if (spec->crouchMoveSpeed > 0.01) {
            s.crouchStepBase = static_cast<unsigned>(
                std::clamp<long>(std::lround(spec->crouchMoveSpeed * 16.0), 3l, 12l));
            const double cmsMax = spec->crouchMoveSpeedMax >= spec->crouchMoveSpeed
                ? spec->crouchMoveSpeedMax : spec->crouchMoveSpeed;
            s.crouchStepMax = static_cast<unsigned>(
                std::clamp<long>(std::lround(cmsMax * 16.0), static_cast<long>(s.crouchStepBase), 12l));
        }
    }
    if (s.isOfficer) {
        const bool isGeneral = lowId.find("general") != std::string::npos ||
                               (spec && spec->selType == "general");
        s.officerAuraRadius = (spec && spec->officerRadius > 0) ? spec->officerRadius
                                                                : (isGeneral ? 350 : 160);
        const int retailRate = (spec && spec->moraleAutoIncrease > 0) ? spec->moraleAutoIncrease
                                                                      : (isGeneral ? 10 : 5);
        s.moraleRallyRate = std::clamp((retailRate + 1) / 2, 2, 6);
    }
    s.isAntiVehicle = isAntiVehicleSoldierId(lowId) || (spec && spec->isAntiTankInfantry());
    s.isFlamer = lowId.find("flam") != std::string::npos || (spec && spec->hasFlamethrower());
    s.isSniper = lowId.find("sniper") != std::string::npos || (spec && spec->isSniper());
    s.isSapper = lowId.find("sapper") != std::string::npos || lowId.find("miner") != std::string::npos ||
                 (spec && (spec->selType == "sapper" || spec->selType == "sappers"));
    s.isMortar = lowId.find("mortar") != std::string::npos || (spec && spec->isMortarInfantry());
    s.isHeavyMg = lowId.find("hmg") != std::string::npos || (spec && spec->isHeavyMgInfantry());
    s.hasBinoculars = s.isOfficer || s.isSniper || (spec && spec->binocularSight > 0);
    if (spec && !spec->isVehicle && spec->binocularSight > 0) {
        s.binoSightBonus = std::clamp(spec->binocularSight - s.desc.sightBase, 4, 14);
    } else {
        s.binoSightBonus = s.isSniper ? 4 : 6;
    }
    s.isKamikaze = lowId.find("kamikaze") != std::string::npos || (spec && spec->isKamikaze());
    s.canGrenadeGround = (spec && !spec->isVehicle && spec->canAttackPoint == 2) ||
                         lowId.find("smguner") != std::string::npos;
    if (s.isSapper) {
        s.canLayMines = true;
        s.maxMineAmmo = 5;
        s.mineAmmo = 5;
    } else if (spec && !spec->isVehicle && !s.isMedic && spec->layMineTime > 0 && spec->mineAmmo > 0) {
        s.canLayMines = true;
        s.maxMineAmmo = std::clamp(spec->mineAmmo, 1, 5);
        s.mineAmmo = s.maxMineAmmo;
    }
    s.getMineTime = (spec && !spec->isVehicle) ? std::max(0, spec->getMineTime) : 0;
    s.getMineTimeMax = (spec && !spec->isVehicle && spec->getMineTimeMax > 0)
        ? std::min(spec->getMineTimeMax, s.getMineTime)
        : s.getMineTime;
    if (!s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar && !s.isKamikaze) s.atGrenades = 2;
    soldiers_.push_back(std::move(s));
    grid_.setOccupied(cell, true);
    return soldiers_.size() - 1;
}

size_t TutorialUnits::appendVehicle(const assets::VehicleUnit& unit) {
    const SubCell cell = placementCell(unit.x, unit.y, originX_);
    Vehicle v{};
    v.id = unit.id;
    v.cell = cell;
    v.x = unit.x;
    v.y = unit.y;
    v.fx = static_cast<double>(unit.x);
    v.fy = static_cast<double>(unit.y);
    v.hullDir = unit.hullDir & 31u;
    v.turretDir = unit.turretDir & 31u;
    v.alive = unit.alive && unit.hp > 0;
    v.owner = unit.owner;
    v.group = unit.group;
    v.hasCrew = unit.hasCrew;
    v.expa = missionExpaToSim(unit.expa);
    v.visible = fog_ ? (unit.owner == 0) : unit.visible;
    const std::string lowId = toLowerAscii(unit.id);
    if (const auto* spec = catalog_ ? catalog_->find(unit.id) : nullptr) {
        v.maxHp = std::max(1, spec->health);
        v.hp = v.alive ? std::max(1, (v.maxHp * std::clamp(unit.hp, 1, 100)) / 100) : 0;
        v.speedPx = spec->moveSpeed > 0.01 ? std::clamp(spec->moveSpeed * 2.5, 0.8, 5.5) : 0.0;
        const double vMsMax = spec->moveSpeedMax >= spec->moveSpeed ? spec->moveSpeedMax : spec->moveSpeed;
        v.speedPxMax = v.speedPx > 0.0 ? std::clamp(vMsMax * 2.5, v.speedPx, 6.8) : 0.0;
        v.turnStep32 = (v.speedPx <= 0.0 || spec->turnDelay >= 12) ? 1u : (spec->turnDelay <= 6 ? 3u : 2u);
        v.sightTiles = std::clamp(spec->sight, 6, 22);
        v.bonusSight = spec->bonusSight;
        v.marchSightBonus = std::clamp(spec->marchSightBonus, 0, 8);
        v.marchEnabled = spec->marchEnabled || (spec->marchSightBonus > 0);
        v.hatchOpen = v.marchEnabled;
        v.isKamikaze = spec->isKamikaze() || lowId.find("sdkfz.303") != std::string::npos ||
                       lowId.find("goliath") != std::string::npos;
        if (spec->shot1.damage > 0 && spec->shot1.range > 0) {
            v.hasPrimary = true;
            v.damage = spec->shot1.damage;
            v.range = v.isKamikaze
                ? std::clamp(static_cast<double>(spec->shot1.range), 48.0, 96.0)
                : std::clamp(spec->shot1.range * 0.65, 140.0, 560.0);
            v.deadzone = std::max(0.0, spec->shot1.deadzone * 0.5);
            v.burstShots1 = std::clamp(spec->shot1.burstShots, 1, 24);
            v.burstReload1 = std::clamp(spec->shot1.burstReloadTicks > 0 ? spec->shot1.burstReloadTicks
                                        : (spec->shot1.reloadTicks > 0 ? spec->shot1.reloadTicks : 60),
                                        10, 360);
            v.cooldown = (v.burstShots1 > 1)
                ? std::clamp(spec->shot1.reloadTicks > 0 ? spec->shot1.reloadTicks : 6, 3, 60)
                : std::clamp(spec->shot1.reloadTicks > 0 ? spec->shot1.reloadTicks : 60, 10, 260);
            v.hitType = spec->shot1.hitType != assets::HitType::None ? spec->shot1.hitType : assets::HitType::Pierce;
            v.sound1 = spec->shot1.soundGroup;
            v.weapon1Flamer = spec->shot1.isFlamethrower();
            if (v.weapon1Flamer && spec->shot1.hitType == assets::HitType::None) {
                v.hitType = assets::HitType::Fire;
            }
            if (v.isKamikaze) {
                v.hitType = assets::HitType::Explosive;
            }
        }
        if (spec->shot2.damage > 0 && spec->shot2.range > 0) {
            v.hasSecondary = true;
            v.damage2 = spec->shot2.damage;
            v.range2 = std::clamp(spec->shot2.range * 0.65, 140.0, 520.0);
            v.cooldown2 = std::clamp(spec->shot2.reloadTicks > 0 ? spec->shot2.reloadTicks : 4, 2, 40);
            v.burstShots2 = std::clamp(spec->shot2.burstShots, 1, 12);
            v.burstReload2 = std::clamp(spec->shot2.burstReloadTicks > 0 ? spec->shot2.burstReloadTicks : 45, 10, 120);
            v.hitType2 = spec->shot2.hitType != assets::HitType::None ? spec->shot2.hitType : assets::HitType::Machine;
            v.sound2 = spec->shot2.soundGroup;
            v.weapon2Flamer = spec->shot2.isFlamethrower();
            if (v.weapon2Flamer && spec->shot2.hitType == assets::HitType::None) {
                v.hitType2 = assets::HitType::Fire;
            }
        }
        v.armed = v.hasPrimary || v.hasSecondary;
        v.ammo = spec->ammo1 > 0
            ? std::max(0, (spec->ammo1 * std::clamp(unit.ammo, 0, 100)) / 100)
            : (v.hasPrimary ? std::max(1, unit.ammo) : 0);
        v.ammo2 = spec->ammo2 > 0
            ? std::max(0, (spec->ammo2 * std::clamp(unit.ammo, 0, 100)) / 100)
            : (v.hasSecondary ? std::max(1, unit.ammo) : 0);
        v.armor = spec->armor;
        v.hasCatalogArmor = true;
        v.crewProtection = spec->crewProtection;
        v.hasCrewProtection = spec->hasCrewProtection;
        v.crewUnit = spec->crewUnit;
        v.canSelfRepair = spec->canSelfRepair;
        const int crewCount = std::clamp(spec->crewNumber > 0 ? spec->crewNumber : 2, 1, 8);
        v.crewNumber = crewCount;
        v.attackCrew = std::clamp(spec->attackCrew > 0 ? spec->attackCrew : 1, 1, crewCount);
        const int baseMoveCrew = std::clamp(spec->moveCrew > 0 ? spec->moveCrew : 1, 1, crewCount);
        v.moveCrew = std::max(baseMoveCrew, std::max(0, spec->soldToMove));
        v.crewVulnerability = spec->crewVulnerability > 0.0 ? spec->crewVulnerability : 0.0;
        v.expaOnHit = std::max(0, spec->expaOnHit);
        v.moraleResist = std::max(0, spec->moraleResist);
        v.crewCanBeHealed = spec->crewCanBeHealed;
        v.indirectArc1 = spec->shot1.indirectArc;
        v.craterScale1 = std::max(0, spec->shot1.craterScale);
        v.cookOffDamage = spec->cookOffDamage();
        v.cookOffRadius = spec->cookOffRadius();
        v.forbiddenTerrainMask = spec->forbiddenTerrainMask();
        v.isNaval = spec->isNavalCraft();
        v.targetType = spec->targetType;
        v.attackPref = spec->attackPref;
        v.hasAttackPref = spec->hasAttackPref;
        v.isWheeled = spec->isWheeled();
        v.isTracked = spec->isTracked();
        v.isTrain = spec->isTrain();
        v.maxCrewHp = 70 + crewCount * 35;
        v.crewHp = v.hasCrew ? v.maxCrewHp : 0;
        v.passengerCapacity = std::clamp(spec->stock, 0, 44);
        v.maxAmmo = std::max(v.ammo, spec->ammo1);
        v.maxAmmo2 = std::max(v.ammo2, spec->ammo2);
        v.moveDamageHp = spec->moveDamageHp;
        v.isSupply = spec->isSupplyTruck();
        v.isCrate = spec->isSupplyCrate();
        v.hasFiniteSupply = v.isSupply && (v.maxAmmo > 0 || v.maxAmmo2 > 0);
        v.isAmbulance = spec->isAmbulance();
        v.isArtillery = spec->isArtillery();
        v.isRocketArtillery = spec->isRocketArtillery();
        v.isCasemate = spec->isCasemate();
        v.canBeCrushed = spec->canBeCrushed;
        v.passengersCanFire = spec->passengersCanFire;
        v.accuracy1 = spec->shot1.accuracyBase;
        v.accuracy1Min = spec->shot1.accuracyMin;
        v.shotExpa1 = spec->shot1.expaPerShot;
        v.shotExpa2 = spec->shot2.expaPerShot;
        v.bonusShotRange = std::max(0, spec->bonusShotRange);
        v.bonusShotDeadZone = std::max(0, spec->bonusShotDeadZone);
        v.scanDelayTicks = spec->scanDelay > 0 ? spec->scanDelay : 0;
        v.crushExpaBonus = std::max(0, spec->expaCrush);
        v.expaReload = std::max(0, spec->expaReload);
        v.expaReloadMax = std::max(0, spec->expaReloadMax);
        v.ammoRegenDelay = std::max(0, spec->ammoRegenDelay);
        v.ammoRegenDelayMax = std::max(0, spec->ammoRegenDelayMax);
        v.healDelay = std::max(0, spec->healDelay);
        v.healDelayMax = std::max(0, spec->healDelayMax);
        v.moraleRndMove = spec->moraleRndMove;
        v.moraleRage = std::max(0, spec->moraleRage);
        v.isPropaganda = spec->isPropaganda();
        v.soldiersOnArmor = spec->soldiersOnArmor;
        v.resupplyCost1 = std::max(0, spec->shot1.resupplyCost);
        v.resupplyCost2 = std::max(0, spec->shot2.resupplyCost);
        v.repairCost = std::max(0, spec->repairCost);
        v.gunShotWait = std::max(0, spec->gunShotWait);
        v.unitRadius = std::max(0, spec->unitRadius);
        v.ammoPerShot1 = (spec->shot1.ammoPerShot >= 1000) ? 1 : std::clamp(spec->shot1.ammoPerShot, 0, 20);
        v.ammoPerShot2 = (spec->shot2.ammoPerShot >= 1000) ? 1 : std::clamp(spec->shot2.ammoPerShot, 0, 20);
        v.hasGunTurnDelay = spec->hasGunTurnDelay;
        if (spec->hasGunTurnDelay) {
            if (spec->gunTurnDelay <= 2) v.turretStep32 = 6u;
            else if (spec->gunTurnDelay <= 6) v.turretStep32 = 3u;
            else if (spec->gunTurnDelay <= 10) v.turretStep32 = 2u;
            else v.turretStep32 = 1u;
        } else {
            v.turretStep32 = 4u;
        }
        const int frontPierce = spec->armor[static_cast<std::size_t>(assets::HitType::Pierce)].front;
        v.canCrushInfantry = !v.isKamikaze && !spec->isTowable() &&
            (spec->hasCrushRange || spec->roleClass == "tank" || spec->selType == "tank" ||
             spec->selType == "spg" || frontPierce >= 25 || spec->health >= 250);
        v.towWeightClass = spec->towWeightClass;
        v.towSpeedByClass = spec->towSpeedByClass;
        v.canMoveBackward = spec->canMoveBackward && v.speedPx > 0.0;
        v.backSpeedPx = v.canMoveBackward
            ? (spec->backMoveSpeed > 0.01 ? std::clamp(spec->backMoveSpeed * 2.5, 0.4, 3.5)
                                          : std::max(0.4, v.speedPx * 0.5))
            : 0.0;
    } else {
        v.hp = v.alive ? unit.hp : std::max(0, unit.hp);
        v.maxHp = std::max(1, v.hp);
        v.crewHp = v.hasCrew ? v.maxCrewHp : 0;
        v.ammo = std::max(0, unit.ammo);
        v.maxAmmo = v.ammo;
        v.isKamikaze = lowId.find("kamikaze") != std::string::npos ||
                       lowId.find("sdkfz.303") != std::string::npos ||
                       lowId.find("goliath") != std::string::npos;
        v.armed = v.isKamikaze || !isUnarmedVehicleId(unit.id);
        v.hasPrimary = v.armed;
        if (v.isKamikaze) {
            v.range = 64.0;
            v.damage = 1875;
            v.cooldown = 1;
            v.hitType = assets::HitType::Explosive;
        }
        if (!v.armed) {
            v.range = 0.0;
            v.cooldown = 0;
            v.damage = 0;
        }
        v.isRocketArtillery = lowId.find("katy") != std::string::npos ||
                              lowId.find("nebel") != std::string::npos ||
                              lowId.find("bm-31") != std::string::npos ||
                              lowId.find("bm-13") != std::string::npos ||
                              lowId.find("calliope") != std::string::npos;
        v.isCasemate = v.isRocketArtillery ||
                       lowId.find("stug") != std::string::npos ||
                       lowId.find("jagd") != std::string::npos ||
                       lowId.find("hetzer") != std::string::npos ||
                       lowId.find("ferdinand") != std::string::npos ||
                       lowId.find("hummel") != std::string::npos ||
                       lowId.find("su-85") != std::string::npos ||
                       lowId.find("su-100") != std::string::npos ||
                       lowId.find("isu-") != std::string::npos;
        if (lowId.find("truck") != std::string::npos || lowId.find("opel") != std::string::npos ||
            lowId.find("maultier") != std::string::npos || lowId.find("halftrack") != std::string::npos ||
            lowId.find("m16") != std::string::npos || lowId.find("sdkfz") != std::string::npos) {
            v.passengerCapacity = 8;
        } else if (lowId.find("jeep") != std::string::npos || lowId.find("kubel") != std::string::npos ||
                   lowId.find("wyllis") != std::string::npos) {
            v.passengerCapacity = 3;
        }
        v.isSupply = lowId.find("sdkfz.11") != std::string::npos || lowId.find("morris.c8") != std::string::npos ||
                     lowId.find("ammo.box") != std::string::npos || lowId.find("supply") != std::string::npos ||
                     lowId == "m4" || lowId == "m5" || lowId == "ss";
        v.isAmbulance = lowId.size() >= 4 && lowId.ends_with(".med");
        v.canCrushInfantry = !v.isKamikaze && (v.maxHp >= 250) && (v.speedPx > 0.0);
        v.speedPxMax = v.speedPx;
        v.canMoveBackward = v.speedPx > 0.0;
        v.backSpeedPx = v.canMoveBackward ? std::max(0.4, v.speedPx * 0.5) : 0.0;
    }
    {
        const auto* spec = catalog_ ? catalog_->find(unit.id) : nullptr;
        v.isAntiAir = (spec && spec->isAntiAir()) ||
                      lowId.find("flak") != std::string::npos ||
                      lowId.find("wirbel") != std::string::npos ||
                      lowId.find("ostwind") != std::string::npos ||
                      lowId.find("bofors") != std::string::npos ||
                      lowId.find("m16") != std::string::npos ||
                      lowId.find("zsu") != std::string::npos ||
                      lowId.find("quad") != std::string::npos ||
                      ((lowId.find("20mm") != std::string::npos ||
                        lowId.find("37mm") != std::string::npos ||
                        lowId.find("40mm") != std::string::npos) &&
                       (lowId.find("aa") != std::string::npos || lowId.find("flak") != std::string::npos));
        v.isHeavyAntiAir = (spec && spec->isHeavyAntiAir()) ||
                           lowId.find("88") != std::string::npos ||
                           lowId.find("52-k") != std::string::npos ||
                           lowId.find("type.88") != std::string::npos;
    }
    if (v.alive) grid_.setOccupied(cell, true);
    vehicles_.push_back(std::move(v));
    const size_t idx = vehicles_.size() - 1;
    if (catalog_ && (vehicles_[idx].hp != unit.hp || vehicles_[idx].maxHp != std::max(1, unit.hp) ||
                     vehicles_[idx].ammo != unit.ammo)) {
        emitVehiclePose(idx);
    }
    return idx;
}

size_t TutorialUnits::spawnInfantry(const assets::Infantry& unit, int expa) {
    const SubCell cell = findSpawnCell(grid_, placementCell(unit.x, unit.y, originX_));
    const int initExpa = expa > 0 ? std::clamp(expa, 0, 0x8000) : missionExpaToSim(unit.expa);
    const size_t index = appendSoldier(unit, cell, initExpa);
    if (fog_) refreshSpotting();
    return index;
}

std::vector<std::pair<size_t, std::pair<int, int>>> TutorialUnits::vehicleFormationTargets(
        std::span<const size_t> eligibleIds, int mapX, int mapY) const {
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    if (eligibleIds.empty()) return {};
    if (eligibleIds.size() == 1u) {
        return {{eligibleIds[0], {clampedX, clampedY}}};
    }

    double sumX = 0.0, sumY = 0.0;
    for (const size_t id : eligibleIds) {
        sumX += vehicles_[id].fx;
        sumY += vehicles_[id].fy;
    }
    const double invN = 1.0 / static_cast<double>(eligibleIds.size());
    const double cx = sumX * invN;
    const double cy = sumY * invN;
    const double dx = static_cast<double>(clampedX) - cx;
    const double dy = static_cast<double>(clampedY) - cy;
    const double len = std::hypot(dx, dy);
    const double fx = (len >= 8.0) ? (dx / len) : 1.0;
    const double fy = (len >= 8.0) ? (dy / len) : 0.0;
    const double rx = -fy;
    const double ry = fx;

    const auto isWalkableMapPt = [&](int px, int py) {
        if (!mapBounds(px, py)) return false;
        const SubCell c = placementCell(px, py, originX_);
        return grid_.inBounds(c) && grid_.walkable(c);
    };

    const size_t n = eligibleIds.size();
    const size_t cols = std::min<size_t>(4u, n);
    std::vector<std::pair<int, int>> slots;
    slots.reserve(n);
    static constexpr std::array<std::pair<int, int>, 8> kProbeOffsets{{
        {32, 0}, {-32, 0}, {0, 24}, {0, -24}, {32, 24}, {-32, 24}, {32, -24}, {-32, -24}
    }};
    for (size_t k = 0; k < n; ++k) {
        const size_t row = k / cols;
        const size_t col = k % cols;
        const size_t rowCount = std::min(cols, n - row * cols);
        const double lat = (static_cast<double>(col) - 0.5 * static_cast<double>(rowCount - 1)) * 56.0;
        const double depth = -static_cast<double>(row) * 48.0;
        const int candX = std::clamp(static_cast<int>(std::lround(clampedX + rx * lat + fx * depth)),
                                     0, std::max(0, mapPixelW_ - 1));
        const int candY = std::clamp(static_cast<int>(std::lround(clampedY + ry * lat + fy * depth)),
                                     0, std::max(0, mapPixelH_ - 1));
        std::pair<int, int> chosen{clampedX, clampedY};
        if (isWalkableMapPt(candX, candY)) {
            chosen = {candX, candY};
        } else {
            for (const auto& [ox, oy] : kProbeOffsets) {
                const int px = std::clamp(candX + ox, 0, std::max(0, mapPixelW_ - 1));
                const int py = std::clamp(candY + oy, 0, std::max(0, mapPixelH_ - 1));
                if (isWalkableMapPt(px, py)) {
                    chosen = {px, py};
                    break;
                }
            }
        }
        slots.push_back(chosen);
    }

    std::vector<size_t> ordered(eligibleIds.begin(), eligibleIds.end());
    std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        const double pa = (vehicles_[a].fx - cx) * fx + (vehicles_[a].fy - cy) * fy;
        const double pb = (vehicles_[b].fx - cx) * fx + (vehicles_[b].fy - cy) * fy;
        if (std::abs(pa - pb) > 1e-6) return pa > pb;
        return a < b;
    });
    for (size_t rowStart = 0; rowStart < ordered.size(); rowStart += cols) {
        const size_t rowEnd = std::min(ordered.size(), rowStart + cols);
        std::sort(ordered.begin() + static_cast<std::ptrdiff_t>(rowStart),
                  ordered.begin() + static_cast<std::ptrdiff_t>(rowEnd),
                  [&](size_t a, size_t b) {
            const double la = (vehicles_[a].fx - cx) * rx + (vehicles_[a].fy - cy) * ry;
            const double lb = (vehicles_[b].fx - cx) * rx + (vehicles_[b].fy - cy) * ry;
            if (std::abs(la - lb) > 1e-6) return la < lb;
            return a < b;
        });
    }

    std::vector<std::pair<size_t, std::pair<int, int>>> out;
    out.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        out.emplace_back(ordered[k], slots[k]);
    }
    return out;
}

size_t TutorialUnits::moveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        v.pendingTowGun.reset();
        if (append && v.moveTarget) {
            v.queuedOrders.emplace_back(pt.first, pt.second);
        } else {
            v.queuedOrders.clear();
            v.attackMove = false;
            v.attackMoveEngaging = false;
            moveVehicleInternal(v, pt.first, pt.second);
        }
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::reverseMoveVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.canMoveBackward ||
            v.backSpeedPx <= 0.0 || v.towedBy)
            continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        moveVehicleInternal(v, pt.first, pt.second);
        v.reverseMove = true;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::faceVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || v.towedBy) continue;
        const double dx = static_cast<double>(clampedX) - v.fx;
        const double dy = static_cast<double>(clampedY) - v.fy;
        if (std::hypot(dx, dy) < 4.0) continue;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.pendingTowGun.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir = vehicleDirFromMapDelta(dx, dy);
        if (v.wasMoving) {
            v.wasMoving = false;
            emitVehiclePose(id);
        }
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::attackMoveVehicles(std::span<const size_t> indices, int mapX, int mapY, bool append) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        v.pendingTowGun.reset();
        if (append && v.moveTarget) {
            v.queuedOrders.emplace_back(pt.first, pt.second);
            v.attackMove = true;
        } else {
            v.queuedOrders.clear();
            moveVehicleInternal(v, pt.first, pt.second);
            v.attackMove = true;
            v.attackMoveEngaging = false;
        }
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::patrolVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    std::vector<size_t> eligible;
    eligible.reserve(indices.size());
    for (size_t id : indices) {
        if (id >= vehicles_.size() || std::ranges::find(eligible, id) != eligible.end()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        eligible.push_back(id);
    }
    size_t accepted = 0;
    for (const auto& [id, pt] : vehicleFormationTargets(eligible, mapX, mapY)) {
        auto& v = vehicles_[id];
        const std::pair origin{v.x, v.y};
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        moveVehicleInternal(v, pt.first, pt.second);
        if (std::hypot(static_cast<double>(pt.first - origin.first),
                       static_cast<double>(pt.second - origin.second)) >= 16.0) {
            v.patrolOrigin = origin;
            v.patrolDest = pt;
        } else {
            v.patrolOrigin.reset();
            v.patrolDest.reset();
        }
        v.patrolToDest = true;
        v.attackMove = true;
        v.attackMoveEngaging = false;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::stopVehicles(std::span<const size_t> indices) {
    size_t stopped = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        const bool wasMoving = v.moveTarget.has_value() || v.wasMoving;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.pendingTowGun.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.wasMoving = false;
        if (wasMoving) emitVehiclePose(id);
        ++stopped;
    }
    return stopped;
}

bool TutorialUnits::canSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) const {
    if (targetVehicleIdx >= vehicles_.size()) return false;
    const auto& target = vehicles_[targetVehicleIdx];
    if (!target.alive || target.hp <= 0) return false;
    const bool targetIsAccessibleCrate = target.isCrate && (target.owner == 0 || target.owner == 3);
    if (!targetIsAccessibleCrate && target.owner != 0) return false;

    const bool targetNeedsRepair = !target.isCrate && target.hp < target.maxHp;
    const bool targetNeedsAmmo1 = !target.isSupply && target.maxAmmo > 0 && target.ammo < target.maxAmmo;
    const bool targetNeedsAmmo2 = !target.isSupply && target.maxAmmo2 > 0 && target.ammo2 < target.maxAmmo2;
    if (!targetIsAccessibleCrate && !targetNeedsRepair && !targetNeedsAmmo1 && !targetNeedsAmmo2) {
        return false;
    }
    return std::ranges::any_of(supporterVehicleIds, [&](size_t id) {
        if (id >= vehicles_.size() || id == targetVehicleIdx) return false;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.isSupply || v.isCrate || v.speedPx <= 0.0 || v.towedBy) {
            return false;
        }
        if (targetIsAccessibleCrate) {
            return (v.ammo < v.maxAmmo && target.ammo > 0) || (v.ammo2 < v.maxAmmo2 && target.ammo2 > 0);
        }
        const bool canDoRepair = targetNeedsRepair && (!v.hasFiniteSupply || v.ammo2 > 0);
        const bool canDoAmmo = (targetNeedsAmmo1 || targetNeedsAmmo2) && (!v.hasFiniteSupply || v.ammo > 0);
        return canDoRepair || canDoAmmo;
    });
}

size_t TutorialUnits::orderSupportVehicle(std::span<const size_t> supporterVehicleIds, size_t targetVehicleIdx) {
    if (!canSupportVehicle(supporterVehicleIds, targetVehicleIdx)) return 0;
    const auto& target = vehicles_[targetVehicleIdx];
    size_t accepted = 0;
    std::vector<size_t> seen;
    for (size_t id : supporterVehicleIds) {
        if (id >= vehicles_.size() || id == targetVehicleIdx || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.isSupply || v.isCrate || v.speedPx <= 0.0 || v.towedBy) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        const double dist = std::hypot(target.fx - v.fx, target.fy - v.fy);
        if (dist <= 120.0) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, target.fx, target.fy, 64.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
            v.supportTargetVehicle = targetVehicleIdx;
        }
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::canSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) const {
    if (buildingIdx >= buildings_.size()) return false;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || (b.owner >= 0 && b.owner != 0)) return false;
    bool needsSupply = b.hp < b.maxHp;
    bool needsMedic = false;
    if (b.owner == 0) {
        for (size_t sIdx : b.occupants) {
            if (sIdx >= soldiers_.size()) continue;
            const auto& s = soldiers_[sIdx];
            if (s.owner != 0 || s.hp <= 0) continue;
            if (s.hp < s.maxHp) needsMedic = true;
            const bool canCarryGrenades = !s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar;
            const int maxInfAmmo = std::max(1, s.desc.maxAmmo);
            const int maxMines = s.maxMineAmmo > 0 ? s.maxMineAmmo : 5;
            if (s.ammo < maxInfAmmo || (canCarryGrenades && s.atGrenades < 2) ||
                (s.canLayMines && s.mineAmmo < maxMines)) {
                needsSupply = true;
            }
        }
    }
    if (!needsSupply && !needsMedic) return false;
    return std::ranges::any_of(supporterVehicleIds, [&](size_t id) {
        if (id >= vehicles_.size()) return false;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) return false;
        return (needsSupply && v.isSupply) || (needsMedic && v.isAmbulance);
    });
}

size_t TutorialUnits::orderSupportBuilding(std::span<const size_t> supporterVehicleIds, size_t buildingIdx) {
    if (!canSupportBuilding(supporterVehicleIds, buildingIdx)) return 0;
    const auto& b = buildings_[buildingIdx];
    bool needsSupply = b.hp < b.maxHp;
    bool needsMedic = false;
    if (b.owner == 0) {
        for (size_t sIdx : b.occupants) {
            if (sIdx >= soldiers_.size()) continue;
            const auto& s = soldiers_[sIdx];
            if (s.owner != 0 || s.hp <= 0) continue;
            if (s.hp < s.maxHp) needsMedic = true;
            const bool canCarryGrenades = !s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar;
            const int maxInfAmmo = std::max(1, s.desc.maxAmmo);
            const int maxMines = s.maxMineAmmo > 0 ? s.maxMineAmmo : 5;
            if (s.ammo < maxInfAmmo || (canCarryGrenades && s.atGrenades < 2) ||
                (s.canLayMines && s.mineAmmo < maxMines)) {
                needsSupply = true;
            }
        }
    }
    const double bRadius = static_cast<double>(std::max(b.width, b.height)) * 0.5;
    const double bReach = 125.0 + bRadius;
    size_t accepted = 0;
    std::vector<size_t> seen;
    for (size_t id : supporterVehicleIds) {
        if (id >= vehicles_.size() || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        if (!((needsSupply && v.isSupply) || (needsMedic && v.isAmbulance))) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        const double dist = std::hypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy);
        if (dist <= bReach) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, static_cast<double>(b.x), static_cast<double>(b.y),
                                                    bRadius + 32.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
            v.supportTargetBuilding = buildingIdx;
        }
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::canHealSoldier(std::span<const size_t> medicSoldierIds, std::span<const size_t> ambulanceVehicleIds,
                                   size_t targetSoldierIdx) const {
    if (targetSoldierIdx >= soldiers_.size()) return false;
    const auto& t = soldiers_[targetSoldierIdx];
    if (t.owner != 0 || t.hp <= 0 || t.embarked || t.garrisonBuilding || t.hp >= t.maxHp) return false;
    const bool hasMedic = std::ranges::any_of(medicSoldierIds, [&](size_t id) {
        if (id >= soldiers_.size() || id == targetSoldierIdx) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.isMedic;
    });
    if (hasMedic) return true;
    return std::ranges::any_of(ambulanceVehicleIds, [&](size_t id) {
        if (id >= vehicles_.size()) return false;
        const auto& v = vehicles_[id];
        return v.alive && v.hp > 0 && v.owner == 0 && v.isAmbulance && v.speedPx > 0.0 && !v.towedBy;
    });
}

size_t TutorialUnits::orderHealSoldier(std::span<const size_t> medicSoldierIds, std::span<const size_t> ambulanceVehicleIds,
                                       size_t targetSoldierIdx) {
    if (!canHealSoldier(medicSoldierIds, ambulanceVehicleIds, targetSoldierIdx)) return 0;
    const auto& t = soldiers_[targetSoldierIdx];
    const auto [tu, tv] = soldierWorld(t);
    const int tx = originX_ + tu - tv;
    const int ty = (tu + tv) / 2;
    size_t accepted = 0;

    std::vector<size_t> seenMedics;
    for (size_t id : medicSoldierIds) {
        if (id >= soldiers_.size() || id == targetSoldierIdx || std::ranges::find(seenMedics, id) != seenMedics.end()) {
            continue;
        }
        seenMedics.push_back(id);
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isMedic) continue;
        const auto [su, sv] = soldierWorld(s);
        const double worldDist = std::hypot(static_cast<double>(su - tu), static_cast<double>(sv - tv));
        if (worldDist <= 84.0) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
            s.attackMove = false;
            s.boardVehicleTarget.reset();
            s.captureVehicleTarget.reset();
            s.garrisonTarget.reset();
            s.attackTarget.reset();
            s.attackVehicleTarget.reset();
            s.attackBuildingTarget.reset();
            s.attackGround.reset();
            s.autoTarget.reset();
            s.healTarget.reset();
            s.escortVehicleTarget.reset();
            s.escortSoldierTarget.reset();
            s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            ++accepted;
        } else if (orderSoldierInternal(id, tx, ty)) {
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
            s.attackMove = false;
            s.autoTarget.reset();
            s.healTarget = targetSoldierIdx;
            ++accepted;
        }
    }

    std::vector<size_t> seenAmbs;
    for (size_t id : ambulanceVehicleIds) {
        if (id >= vehicles_.size() || std::ranges::find(seenAmbs, id) != seenAmbs.end()) continue;
        seenAmbs.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.isAmbulance || v.speedPx <= 0.0 || v.towedBy) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        const double dist = std::hypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
        if (dist <= 120.0) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, static_cast<double>(tx), static_cast<double>(ty),
                                                    56.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
            v.supportTargetSoldier = targetSoldierIdx;
        }
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::canEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetVehicleIdx) const {
    if (targetVehicleIdx >= vehicles_.size()) return false;
    const auto& t = vehicles_[targetVehicleIdx];
    if (!t.alive || t.hp <= 0 || t.owner != 0) return false;
    const bool hasSol = std::ranges::any_of(soldierIds, [&](size_t id) {
        if (id >= soldiers_.size()) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding;
    });
    if (hasSol) return true;
    return std::ranges::any_of(vehicleIds, [&](size_t id) {
        if (id >= vehicles_.size() || id == targetVehicleIdx) return false;
        const auto& v = vehicles_[id];
        return v.alive && v.hp > 0 && v.owner == 0 && v.speedPx > 0.0 && !v.towedBy &&
               v.towedVehicle != targetVehicleIdx;
    });
}

bool TutorialUnits::canEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     size_t targetSoldierIdx) const {
    if (targetSoldierIdx >= soldiers_.size()) return false;
    const auto& t = soldiers_[targetSoldierIdx];
    if (t.owner != 0 || t.hp <= 0 || t.embarked || t.garrisonBuilding) return false;
    const bool hasSol = std::ranges::any_of(soldierIds, [&](size_t id) {
        if (id >= soldiers_.size() || id == targetSoldierIdx) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding;
    });
    if (hasSol) return true;
    return std::ranges::any_of(vehicleIds, [&](size_t id) {
        if (id >= vehicles_.size()) return false;
        const auto& v = vehicles_[id];
        return v.alive && v.hp > 0 && v.owner == 0 && v.speedPx > 0.0 && !v.towedBy;
    });
}

size_t TutorialUnits::dispatchEscort(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                     double targetX, double targetY, std::optional<size_t> targetVehicleIdx,
                                     std::optional<size_t> targetSoldierIdx) {
    size_t accepted = 0;
    std::vector<size_t> seenSoldiers;
    seenSoldiers.reserve(soldierIds.size());
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size() || (targetSoldierIdx && id == *targetSoldierIdx) ||
            std::ranges::find(seenSoldiers, id) != seenSoldiers.end()) {
            continue;
        }
        seenSoldiers.push_back(id);
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        const auto [su, sv] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sv);
        const double sy = static_cast<double>(su + sv) * 0.5;
        if (std::hypot(targetX - sx, targetY - sy) <= 90.0) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        } else {
            const auto approach = approachPointNear(sx, sy, targetX, targetY, 52.0, mapPixelW_, mapPixelH_);
            (void)orderSoldierInternal(id, approach.first, approach.second);
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
        }
        s.escortVehicleTarget = targetVehicleIdx;
        s.escortSoldierTarget = targetSoldierIdx;
        s.attackMove = true;
        ++accepted;
    }

    std::vector<size_t> seenVehicles;
    seenVehicles.reserve(vehicleIds.size());
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size() || (targetVehicleIdx && id == *targetVehicleIdx) ||
            std::ranges::find(seenVehicles, id) != seenVehicles.end()) {
            continue;
        }
        seenVehicles.push_back(id);
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.speedPx <= 0.0 || v.towedBy) continue;
        if (targetVehicleIdx && v.towedVehicle == targetVehicleIdx) continue;
        v.pendingTowGun.reset();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.facingTargetDir.reset();
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.reverseMove = false;
        v.attackMoveEngaging = false;
        if (std::hypot(targetX - v.fx, targetY - v.fy) <= 110.0) {
            v.moveTarget.reset();
            v.waypoints.clear();
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(id);
            }
        } else {
            const auto approach = approachPointNear(v.fx, v.fy, targetX, targetY, 76.0, mapPixelW_, mapPixelH_);
            moveVehicleInternal(v, approach.first, approach.second);
        }
        v.escortVehicleTarget = targetVehicleIdx;
        v.escortSoldierTarget = targetSoldierIdx;
        v.attackMove = true;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderEscortVehicle(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                         size_t targetVehicleIdx) {
    if (!canEscortVehicle(soldierIds, vehicleIds, targetVehicleIdx)) return 0;
    const auto& target = vehicles_[targetVehicleIdx];
    return dispatchEscort(soldierIds, vehicleIds, target.fx, target.fy, targetVehicleIdx, std::nullopt);
}

size_t TutorialUnits::orderEscortSoldier(std::span<const size_t> soldierIds, std::span<const size_t> vehicleIds,
                                         size_t targetSoldierIdx) {
    if (!canEscortSoldier(soldierIds, vehicleIds, targetSoldierIdx)) return 0;
    const auto [tu, tv] = soldierWorld(soldiers_[targetSoldierIdx]);
    const double tx = static_cast<double>(originX_ + tu - tv);
    const double ty = static_cast<double>(tu + tv) * 0.5;
    return dispatchEscort(soldierIds, vehicleIds, tx, ty, std::nullopt, targetSoldierIdx);
}

std::optional<bool> TutorialUnits::toggleVehicleHoldFire(std::span<const size_t> indices) {
    bool anyValid = false;
    bool anyFree = false;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        anyValid = true;
        if (!v.holdFire) anyFree = true;
    }
    if (!anyValid) return std::nullopt;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        v.holdFire = anyFree;
    }
    return anyFree;
}

std::optional<bool> TutorialUnits::toggleMarchMode(std::span<const size_t> vehicleIds) {
    bool anyValid = false;
    bool anyClosed = false;
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || !v.marchEnabled) continue;
        anyValid = true;
        if (!v.hatchOpen) anyClosed = true;
    }
    if (!anyValid) return std::nullopt;
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || !v.marchEnabled) continue;
        v.hatchOpen = anyClosed;
        emitVehiclePose(id);
    }
    fogDirty_ = true;
    return anyClosed;
}

size_t TutorialUnits::attackWithVehicles(std::span<const size_t> indices, bool targetIsVehicle, size_t targetIdx) {
    const bool valid = targetIsVehicle ? isValidVehicleAttackTarget(targetIdx)
                                       : isValidAttackTarget(targetIdx, 0);
    if (!valid) return 0;
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.towedBy || !v.armed ||
            (v.ammo <= 0 && (!v.hasSecondary || v.ammo2 <= 0)))
            continue;
        v.pendingTowGun.reset();
        v.attackTarget = std::pair{targetIsVehicle, targetIdx};
        v.chasingAttackTarget = false;
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::attackVehicle(std::span<const size_t> indices, size_t targetVehicleIdx) {
    if (!isValidVehicleAttackTarget(targetVehicleIdx)) return 0;
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked ||
            (s.ammo <= 0 && (s.isAntiVehicle || s.isSniper || s.isFlamer || s.atGrenades <= 0)))
            continue;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackVehicleTarget = targetVehicleIdx;
        s.attackTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++accepted;
    }
    return accepted;
}

bool TutorialUnits::isValidVehicleAttackTarget(size_t targetIdx) const {
    if (targetIdx >= vehicles_.size()) return false;
    const auto& v = vehicles_[targetIdx];
    return v.alive && v.hp > 0 && isHostile(0, v.owner) && v.visible;
}

size_t TutorialUnits::spawnVehicle(const assets::VehicleUnit& unit) {
    const size_t index = appendVehicle(unit);
    if (fog_) refreshSpotting(nullptr, true);
    emitVehiclePose(index);
    return index;
}

std::vector<VehiclePose> TutorialUnits::takeVehiclePoses() {
    return std::exchange(vehiclePoses_, {});
}

size_t TutorialUnits::orderGroupMove(unsigned group, int mapX, int mapY, bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;

    std::vector<size_t> soldierIds;
    std::vector<SubCell> soldierSources;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
        if (onlyNonPlayer && s.owner == 0) continue;
        soldierIds.push_back(i);
        soldierSources.push_back(s.cell);
    }
    if (!soldierIds.empty()) {
        const auto destCell = findSpawnCell(grid_, pointerCell(clampedX, clampedY, originX_));
        const auto tiles = grid_.formationDestinations(soldierSources, destCell);
        for (size_t i = 0; i < soldierIds.size(); ++i) {
            const int tx = tiles[i].x / 2, ty = tiles[i].y / 2;
            const int px = originX_ + (tx - ty) * 32;
            const int py = (tx + ty) * 16 + 16;
            if (orderSoldierInternal(soldierIds[i], px, py)) {
                soldiers_[soldierIds[i]].attackMove = true;
                ++accepted;
            }
        }
    }

    std::vector<size_t> vehIds;
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        vehIds.push_back(i);
    }
    for (const auto& [vid, pt] : vehicleFormationTargets(vehIds, clampedX, clampedY)) {
        auto& v = vehicles_[vid];
        moveVehicleInternal(v, pt.first, pt.second);
        v.attackMove = true;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderGroupAttackGround(unsigned group, int mapX, int mapY, bool onlyNonPlayer) {
    const int g = int(group);
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy || v.group != g) continue;
        if (onlyNonPlayer && v.owner == 0) continue;
        if (!v.armed || !v.hasPrimary || (v.ammoPerShot1 != 0 && v.ammo <= 0)) continue;
        v.pendingTowGun.reset();
        v.attackGround = std::pair{clampedX, clampedY};
        v.attackTarget.reset();
        v.attackBuildingTarget.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.moveTarget.reset();
        v.waypoints.clear();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        ++accepted;
    }
    if (accepted == 0) {
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.group != g) continue;
            if (onlyNonPlayer && s.owner == 0) continue;
            const bool mortarOrFlamer = (s.isMortar || s.isFlamer) && s.ammo > 0;
            const bool smgGrenade = s.canGrenadeGround && s.atGrenades > 0;
            if (!mortarOrFlamer && !smgGrenade) continue;
            s.attackTarget.reset();
            s.attackVehicleTarget.reset();
            s.attackBuildingTarget.reset();
            s.autoTarget.reset();
            s.boardVehicleTarget.reset();
            s.captureVehicleTarget.reset();
            s.garrisonTarget.reset();
            s.healTarget.reset();
            s.escortVehicleTarget.reset();
            s.escortSoldierTarget.reset();
            s.disarmMineTarget.reset();
            s.disarmWorkTicks = 0;
            s.patrolOrigin.reset();
            s.patrolDest.reset();
            s.queuedGoals.clear();
            s.motion.reset();
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.attackMove = false;
            s.attackGround = std::pair{clampedX, clampedY};
            ++accepted;
        }
    }
    return accepted;
}

size_t TutorialUnits::transferGroupOwner(unsigned group, int newOwner, std::vector<SoldierPose>* outSoldiers) {
    if (newOwner < 0 || newOwner >= 4) return 0;
    const int g = int(group);
    size_t transferred = 0;
    std::vector<size_t> movedSoldiers;
    std::vector<size_t> movedVehicles;

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.group != g || s.owner == newOwner) continue;
        s.owner = newOwner;
        s.motion.reset();
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        movedSoldiers.push_back(i);
        ++transferred;
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.group != g || v.owner == newOwner) continue;
        v.owner = newOwner;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.pendingTowGun.reset();
        movedVehicles.push_back(i);
        ++transferred;
    }
    if (transferred > 0) {
        fogDirty_ = true;
        if (fog_) refreshSpotting(nullptr, false);
        for (size_t vid : movedVehicles) emitVehiclePose(vid);
        if (outSoldiers) {
            for (size_t sid : movedSoldiers) {
                const auto& s = soldiers_[sid];
                const auto [su, sv] = s.garrisonBuilding
                    ? tutorialFootWorld(buildings_[*s.garrisonBuilding].x, buildings_[*s.garrisonBuilding].y, originX_)
                    : soldierWorld(s);
                const bool vis = !fog_ || isAlliedWithPlayer(s.owner) || (s.spottedMask & 1u) != 0;
                pushOrUpdateSoldierPose(*outSoldiers, {
                    sid, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false,
                    s.hp, true, s.owner, s.group, false, vis, 0, 0, s.garrisonBuilding.has_value(),
                    s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo
                });
            }
        }
    }
    return transferred;
}

size_t TutorialUnits::reassignGroupInZone(std::span<const std::uint8_t> zones, unsigned zone,
                                            unsigned oldGroup, unsigned newGroup,
                                            std::vector<SoldierPose>* outSoldiers) {
    const std::size_t tileW = std::size_t(grid_.width() / 2), tileH = std::size_t(grid_.height() / 2);
    if (zones.size() != tileW * tileH) return 0;
    const auto inside = [&](SubCell cell) {
        if (cell.x < 0 || cell.y < 0) return false;
        const std::size_t tx = std::size_t(cell.x / 2);
        const std::size_t ty = std::size_t(cell.y / 2);
        if (tx >= tileW || ty >= tileH) return false;
        return (zones[ty * tileW + tx] & 63u) == zone;
    };
    const int og = int(oldGroup);
    const int ng = int(newGroup);
    size_t reassigned = 0;

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.group != og) continue;
        const SubCell locCell = s.garrisonBuilding ? buildings_[*s.garrisonBuilding].entryCell : s.cell;
        if (!inside(locCell)) continue;
        s.group = ng;
        s.motion.reset();
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++reassigned;
        if (outSoldiers) {
            const auto [su, sv] = s.garrisonBuilding
                ? tutorialFootWorld(buildings_[*s.garrisonBuilding].x, buildings_[*s.garrisonBuilding].y, originX_)
                : soldierWorld(s);
            const bool vis = !fog_ || isAlliedWithPlayer(s.owner) || (s.spottedMask & 1u) != 0;
            pushOrUpdateSoldierPose(*outSoldiers, {
                i, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false,
                s.hp, true, s.owner, s.group, false, vis, 0, 0, s.garrisonBuilding.has_value(),
                s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo
            });
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || v.group != og || !inside(v.cell)) continue;
        v.group = ng;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.pendingTowGun.reset();
        emitVehiclePose(i);
        ++reassigned;
    }
    return reassigned;
}

bool TutorialUnits::isObjectDestroyedAt(int mapX, int mapY) const {
    if (mapX == 0 && mapY == 0) return true;
    const Building* best = nullptr;
    double bestDist2 = 1e18;
    for (const auto& b : buildings_) {
        const bool inBox = (mapX >= b.left - 32 && mapX <= b.left + b.width + 32 &&
                            mapY >= b.top - 32 && mapY <= b.top + b.height + 32);
        const double dx = static_cast<double>(mapX - b.x);
        const double dy = static_cast<double>(mapY - b.y);
        const double dist2 = dx * dx + dy * dy;
        if (!inBox && dist2 > 96.0 * 96.0) continue;
        if (!best || dist2 < bestDist2) {
            best = &b;
            bestDist2 = dist2;
        }
    }
    if (!best) return true;
    return best->destroyed;
}

void TutorialUnits::revealFogAt(int mapX, int mapY, int radiusTiles, int player) {
    if (!fog_ || player < 0 || player >= 4) return;
    const SubCell cell = placementCell(mapX, mapY, originX_);
    fog_->revealCircle(cell.x / 2, cell.y / 2, std::clamp(radiusTiles, 1, 32), player);
    fogDirty_ = true;
    refreshSpotting(nullptr, true);
}

bool TutorialUnits::canBoardVehicle(size_t vehicleIdx, int viewerOwner) const {
    if (vehicleIdx >= vehicles_.size()) return false;
    const auto& v = vehicles_[vehicleIdx];
    return v.alive && v.hp > 0 && v.owner == viewerOwner &&
           int(v.passengers.size()) < v.passengerCapacity;
}

bool TutorialUnits::embarkImmediate(size_t soldierIdx, size_t vehicleIdx) {
    if (soldierIdx >= soldiers_.size() || vehicleIdx >= vehicles_.size()) return false;
    auto& s = soldiers_[soldierIdx];
    auto& v = vehicles_[vehicleIdx];
    if (s.hp <= 0 || s.embarked || !v.alive || v.hp <= 0 || s.owner != v.owner) return false;
    if (v.passengers.size() >= 44u) return false;

    const SubCell oldCell = s.cell;
    s.embarked = true;
    s.motion.reset();
    s.goal.reset();
    s.route.clear();
    s.cursor = 0;
    s.needsRoute = false;
    s.attackTarget.reset();
    s.attackVehicleTarget.reset();
    s.attackBuildingTarget.reset();
    s.attackGround.reset();
    s.autoTarget.reset();
    s.boardVehicleTarget.reset();
    s.healTarget.reset();
    s.escortVehicleTarget.reset();
    s.escortSoldierTarget.reset();
    s.disarmMineTarget.reset();
    s.binoculars = false;
    s.spottedMask = 0;
    s.cell = v.cell;
    if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) {
        grid_.setOccupied(oldCell, false);
    }
    v.passengers.push_back(soldierIdx);
    fogDirty_ = true;
    emitVehiclePose(vehicleIdx);
    return true;
}

size_t TutorialUnits::orderBoardVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    if (!canBoardVehicle(vehicleIdx, 0)) return 0;
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.boardVehicleTarget = vehicleIdx;
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::unloadVehicles(std::span<const size_t> vehicleIds, std::vector<SoldierPose>* outPoses) {
    size_t unloaded = 0;
    for (size_t vid : vehicleIds) {
        if (vid >= vehicles_.size()) continue;
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.passengers.empty()) continue;
        for (size_t sIdx : v.passengers) {
            if (sIdx >= soldiers_.size()) continue;
            auto& s = soldiers_[sIdx];
            if (s.hp <= 0) continue;
            const SubCell spot = findSpawnCell(grid_, v.cell);
            s.embarked = false;
            s.cell = spot;
            s.motion.reset();
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.boardVehicleTarget.reset();
            grid_.setOccupied(spot, true);
            if (outPoses) {
                const auto [u, w] = soldierWorld(s);
                pushOrUpdateSoldierPose(*outPoses, {
                    sIdx, originX_ + u - w, (u + w) / 2, s.direction, 0, false,
                    s.hp, true, s.owner, s.group, false, true
                });
            }
            ++unloaded;
        }
        v.passengers.clear();
        emitVehiclePose(vid);
        fogDirty_ = true;
    }
    if (unloaded > 0 && fog_) refreshSpotting(outPoses, true);
    if (outPoses) {
        for (auto& pose : *outPoses) {
            if (pose.index < soldiers_.size()) {
                pose.expa = soldiers_[pose.index].expa;
                pose.kills = soldiers_[pose.index].kills;
                pose.atGrenades = soldiers_[pose.index].atGrenades;
                pose.mineAmmo = soldiers_[pose.index].mineAmmo;
                pose.ammo = soldiers_[pose.index].ammo;
                pose.suppressed = soldiers_[pose.index].isSuppressed();
            }
        }
    }
    return unloaded;
}

void TutorialUnits::setPlayerTeams(std::array<int, 4> teams) {
    playerTeams_ = teams;
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(nullptr, true);
    }
}

bool TutorialUnits::isHostile(int ownerA, int ownerB) const {
    if (ownerA == ownerB) return false;
    if (ownerA == 3 || ownerB == 3) return false;
    if (ownerA >= 0 && ownerA < 4 && ownerB >= 0 && ownerB < 4) {
        const int teamA = playerTeams_[static_cast<std::size_t>(ownerA)];
        const int teamB = playerTeams_[static_cast<std::size_t>(ownerB)];
        if (teamA == 3 || teamB == 3 || teamA == teamB) return false;
    }
    return true;
}

bool TutorialUnits::isAlliedWithPlayer(int owner) const {
    if (owner == 0) return true;
    if (owner < 0 || owner >= 4 || owner == 3) return false;
    const int team = playerTeams_[static_cast<std::size_t>(owner)];
    return team != 3 && team == playerTeams_[0];
}

void TutorialUnits::setMines(std::vector<std::uint8_t> mines) {
    const std::size_t expected = static_cast<std::size_t>(grid_.width() / 2) * static_cast<std::size_t>(grid_.height() / 2);
    if (mines.size() == expected) {
        mines_ = std::move(mines);
    }
}

std::vector<MineEvent> TutorialUnits::takeMineEvents() {
    return std::exchange(mineEvents_, {});
}

size_t TutorialUnits::orderLayMine(std::span<const size_t> soldierIds, std::vector<SoldierPose>* outPoses) {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tileW <= 0 || tileH <= 0) return 0;
    size_t laid = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.canLayMines || s.mineAmmo <= 0) continue;
        const int tx = s.cell.x / 2;
        const int ty = s.cell.y / 2;
        if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) continue;
        if (mines_.empty()) mines_.assign(static_cast<std::size_t>(tileW) * tileH, 0u);
        const std::size_t idx = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx);
        if ((mines_[idx] & 0x01u) != 0) continue;
        mines_[idx] = 0x85u; // present | anti-tank | player-0 | visible
        --s.mineAmmo;
        const int wu = tx * 32 + 16, wv = ty * 32 + 16;
        mineEvents_.push_back({MineEvent::Laid, originX_ + wu - wv, (wu + wv) / 2, true});
        if (outPoses) {
            const auto [su, sv] = soldierWorld(s);
            const bool vis = !fog_ || s.owner == 0 || (s.spottedMask & 1u) != 0;
            SoldierPose p{id, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false,
                          s.hp, true, s.owner, s.group, false, vis, 0, 0, false,
                          s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s), s.mineAmmo};
            pushOrUpdateSoldierPose(*outPoses, p);
        }
        ++laid;
    }
    return laid;
}

std::optional<std::pair<int, int>> TutorialUnits::findDetectedHostileMineNear(int mapX, int mapY,
                                                                              double maxDistPx) const {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tileW <= 0 || tileH <= 0 || mines_.size() != static_cast<std::size_t>(tileW) * static_cast<std::size_t>(tileH) ||
        !mapBounds(mapX, mapY)) {
        return std::nullopt;
    }
    const auto mineOwner = [](std::uint8_t b) -> int {
        return (b & 0x40u) ? 1 : ((b & 0x20u) ? 2 : 0);
    };
    const SubCell center = pointerCell(mapX, mapY, originX_);
    const int cx = center.x / 2;
    const int cy = center.y / 2;
    std::optional<std::pair<int, int>> best;
    double bestDist = maxDistPx + 1e-6;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            const int tx = cx + dx;
            const int ty = cy + dy;
            if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) continue;
            const std::uint8_t m = mines_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) +
                                          static_cast<std::size_t>(tx)];
            if ((m & 0x81u) != 0x81u || !isHostile(mineOwner(m), 0)) continue;
            const int wu = tx * 32 + 16;
            const int wv = ty * 32 + 16;
            const int mx = originX_ + wu - wv;
            const int my = (wu + wv) / 2;
            const double dist = std::hypot(static_cast<double>(mx - mapX), static_cast<double>(my - mapY));
            if (dist <= bestDist) {
                bestDist = dist;
                best = std::pair{tx, ty};
            }
        }
    }
    return best;
}

bool TutorialUnits::canDisarmMineAt(std::span<const size_t> soldierIds, int mapX, int mapY) const {
    if (!findDetectedHostileMineNear(mapX, mapY, 28.0)) return false;
    return std::ranges::any_of(soldierIds, [&](size_t id) {
        if (id >= soldiers_.size()) return false;
        const auto& s = soldiers_[id];
        return s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.isSapper;
    });
}

size_t TutorialUnits::orderDisarmMine(std::span<const size_t> soldierIds, int mapX, int mapY) {
    const auto mineTile = findDetectedHostileMineNear(mapX, mapY, 28.0);
    if (!mineTile) return 0;
    const int wu = mineTile->first * 32 + 16;
    const int wv = mineTile->second * 32 + 16;
    const int mx = originX_ + wu - wv;
    const int my = (wu + wv) / 2;
    size_t accepted = 0;
    std::vector<size_t> seen;
    seen.reserve(soldierIds.size());
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size() || std::ranges::find(seen, id) != seen.end()) continue;
        seen.push_back(id);
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isSapper) continue;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        const int tileDist = std::max(std::abs(s.cell.x / 2 - mineTile->first),
                                      std::abs(s.cell.y / 2 - mineTile->second));
        if (tileDist <= 2) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        } else {
            const auto [su, sv] = soldierWorld(s);
            const double sx = static_cast<double>(originX_ + su - sv);
            const double sy = static_cast<double>(su + sv) * 0.5;
            const auto ap = approachPointNear(sx, sy, static_cast<double>(mx), static_cast<double>(my),
                                              40.0, mapPixelW_, mapPixelH_);
            if (!orderSoldierInternal(id, ap.first, ap.second)) {
                (void)orderSoldierInternal(id, mx, my);
            }
            s.queuedGoals.clear();
            s.patrolOrigin.reset();
            s.patrolDest.reset();
        }
        s.disarmMineTarget = *mineTile;
        s.attackMove = false;
        ++accepted;
    }
    return accepted;
}

std::vector<VisibleMine> TutorialUnits::visibleMines(int viewerOwner) const {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (tileW <= 0 || tileH <= 0 || mines_.size() != static_cast<std::size_t>(tileW) * static_cast<std::size_t>(tileH)) {
        return {};
    }
    const auto mineOwner = [](std::uint8_t b) -> int {
        return (b & 0x40u) ? 1 : ((b & 0x20u) ? 2 : 0);
    };
    std::vector<VisibleMine> out;
    for (int ty = 0; ty < tileH; ++ty) {
        for (int tx = 0; tx < tileW; ++tx) {
            const std::uint8_t m = mines_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) +
                                          static_cast<std::size_t>(tx)];
            if ((m & 0x01u) == 0) continue;
            const int owner = mineOwner(m);
            const bool friendly = (owner == viewerOwner) || (viewerOwner == 0 && isAlliedWithPlayer(owner));
            const bool detected = (m & 0x80u) != 0;
            if (!friendly && !detected) continue;
            const int wu = tx * 32 + 16, wv = ty * 32 + 16;
            out.push_back({originX_ + wu - wv, (wu + wv) / 2, (m & 0x04u) != 0, isHostile(owner, viewerOwner)});
        }
    }
    return out;
}

std::vector<IdleUnit> TutorialUnits::idlePlayerUnits() const {
    std::vector<IdleUnit> out;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.owner == 0 && s.hp > 0 && !s.embarked && !s.garrisonBuilding && !s.motion && !s.goal &&
            s.route.empty() && !s.attackTarget && !s.attackVehicleTarget && !s.attackBuildingTarget &&
            !s.attackGround && !s.autoTarget && !s.boardVehicleTarget && !s.captureVehicleTarget &&
            !s.garrisonTarget && !s.healTarget && !s.escortVehicleTarget && !s.escortSoldierTarget &&
            !s.disarmMineTarget && !s.patrolOrigin) {
            out.push_back({false, i});
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (v.alive && v.hp > 0 && v.owner == 0 && v.hasCrew && !v.towedBy && !v.moveTarget &&
            v.waypoints.empty() && !v.attackTarget && !v.attackGround && !v.attackBuildingTarget &&
            !v.pendingTowGun && !v.facingTargetDir && !v.supportTargetVehicle &&
            !v.supportTargetBuilding && !v.supportTargetSoldier &&
            !v.escortVehicleTarget && !v.escortSoldierTarget && !v.patrolOrigin) {
            out.push_back({true, i});
        }
    }
    return out;
}

bool TutorialUnits::canCaptureVehicle(size_t vehicleIdx, int viewerOwner) const {
    if (vehicleIdx >= vehicles_.size()) return false;
    const auto& v = vehicles_[vehicleIdx];
    return viewerOwner == 0 && v.alive && v.hp > 0 && v.visible && v.owner == 3;
}

size_t TutorialUnits::orderCaptureVehicle(std::span<const size_t> soldierIds, size_t vehicleIdx) {
    if (!canCaptureVehicle(vehicleIdx, 0)) return 0;
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.captureVehicleTarget = vehicleIdx;
        s.boardVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        ++accepted;
    }
    return accepted;
}

std::vector<TacticalOrderLine> TutorialUnits::selectionOrderLines(std::span<const size_t> soldierIds,
                                                                    std::span<const size_t> vehicleIds) const {
    std::vector<TacticalOrderLine> lines;
    const auto pushSeg = [&](int x0, int y0, int x1, int y1, TacticalOrderLine::Kind kind) {
        if (lines.size() >= 48u) return;
        const int dx = x1 - x0;
        const int dy = y1 - y0;
        if (dx * dx + dy * dy < 64) return;
        for (const auto& ex : lines) {
            if (ex.kind == kind &&
                std::abs(ex.x0 - x0) + std::abs(ex.y0 - y0) + std::abs(ex.x1 - x1) + std::abs(ex.y1 - y1) < 16) {
                return;
            }
        }
        lines.push_back({x0, y0, x1, y1, kind});
    };
    const auto cellToMap = [&](SubCell c) -> std::pair<int, int> {
        const auto [u, v] = InfantryPathGrid::toWorldCenter(c);
        return {originX_ + u - v, (u + v) / 2};
    };

    for (size_t id : soldierIds) {
        if (lines.size() >= 48u) break;
        if (id >= soldiers_.size()) continue;
        const auto& s = soldiers_[id];
        if (s.hp <= 0 || s.embarked || s.owner != 0) continue;
        const auto [su, sv] = soldierWorld(s);
        const int sx = s.garrisonBuilding ? buildings_[*s.garrisonBuilding].x : (originX_ + su - sv);
        const int sy = s.garrisonBuilding ? buildings_[*s.garrisonBuilding].y : ((su + sv) / 2);

        if (s.patrolOrigin && s.patrolDest) {
            const auto active = s.patrolToDest ? *s.patrolDest : *s.patrolOrigin;
            pushSeg(sx, sy, active.first, active.second, TacticalOrderLine::Kind::Patrol);
            pushSeg(s.patrolOrigin->first, s.patrolOrigin->second, s.patrolDest->first, s.patrolDest->second,
                    TacticalOrderLine::Kind::Patrol);
        } else if (s.attackTarget && *s.attackTarget < soldiers_.size()) {
            const auto& t = soldiers_[*s.attackTarget];
            if (t.hp > 0 && !t.embarked && !t.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(t);
                pushSeg(sx, sy, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Attack);
            }
        } else if (s.attackVehicleTarget && *s.attackVehicleTarget < vehicles_.size()) {
            const auto& tv = vehicles_[*s.attackVehicleTarget];
            if (tv.alive && tv.hp > 0) pushSeg(sx, sy, tv.x, tv.y, TacticalOrderLine::Kind::Attack);
        } else if (s.attackBuildingTarget && *s.attackBuildingTarget < buildings_.size()) {
            const auto& tb = buildings_[*s.attackBuildingTarget];
            if (!tb.destroyed) pushSeg(sx, sy, tb.x, tb.y, TacticalOrderLine::Kind::Attack);
        } else if (s.healTarget && *s.healTarget < soldiers_.size()) {
            const auto& th = soldiers_[*s.healTarget];
            if (th.hp > 0 && !th.embarked && !th.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(th);
                pushSeg(sx, sy, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (s.escortVehicleTarget && *s.escortVehicleTarget < vehicles_.size()) {
            const auto& tv = vehicles_[*s.escortVehicleTarget];
            if (tv.alive && tv.hp > 0) pushSeg(sx, sy, tv.x, tv.y, TacticalOrderLine::Kind::Support);
        } else if (s.escortSoldierTarget && *s.escortSoldierTarget < soldiers_.size()) {
            const auto& ts = soldiers_[*s.escortSoldierTarget];
            if (ts.hp > 0 && !ts.embarked && !ts.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(ts);
                pushSeg(sx, sy, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (s.disarmMineTarget) {
            const int wu = s.disarmMineTarget->first * 32 + 16;
            const int wv = s.disarmMineTarget->second * 32 + 16;
            pushSeg(sx, sy, originX_ + wu - wv, (wu + wv) / 2, TacticalOrderLine::Kind::Support);
        } else if (s.attackGround) {
            pushSeg(sx, sy, s.attackGround->first, s.attackGround->second, TacticalOrderLine::Kind::AttackGround);
        } else if (s.goal) {
            const auto kind = s.attackMove ? TacticalOrderLine::Kind::AttackMove : TacticalOrderLine::Kind::Move;
            auto prev = cellToMap(*s.goal);
            pushSeg(sx, sy, prev.first, prev.second, kind);
            for (const auto& q : s.queuedGoals) {
                pushSeg(prev.first, prev.second, q.first, q.second, kind);
                prev = q;
            }
        }
    }

    for (size_t id : vehicleIds) {
        if (lines.size() >= 48u) break;
        if (id >= vehicles_.size()) continue;
        const auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;

        if (v.patrolOrigin && v.patrolDest) {
            const auto active = v.patrolToDest ? *v.patrolDest : *v.patrolOrigin;
            pushSeg(v.x, v.y, active.first, active.second, TacticalOrderLine::Kind::Patrol);
            pushSeg(v.patrolOrigin->first, v.patrolOrigin->second, v.patrolDest->first, v.patrolDest->second,
                    TacticalOrderLine::Kind::Patrol);
        } else if (v.attackGround) {
            pushSeg(v.x, v.y, v.attackGround->first, v.attackGround->second, TacticalOrderLine::Kind::AttackGround);
        } else if (v.attackTarget) {
            const auto [isVeh, tid] = *v.attackTarget;
            if (isVeh && tid < vehicles_.size() && vehicles_[tid].alive && vehicles_[tid].hp > 0) {
                pushSeg(v.x, v.y, vehicles_[tid].x, vehicles_[tid].y, TacticalOrderLine::Kind::Attack);
            } else if (!isVeh && tid < soldiers_.size() && soldiers_[tid].hp > 0 &&
                       !soldiers_[tid].embarked && !soldiers_[tid].garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(soldiers_[tid]);
                pushSeg(v.x, v.y, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Attack);
            }
        } else if (v.attackBuildingTarget && *v.attackBuildingTarget < buildings_.size()) {
            const auto& tb = buildings_[*v.attackBuildingTarget];
            if (!tb.destroyed) pushSeg(v.x, v.y, tb.x, tb.y, TacticalOrderLine::Kind::Attack);
        } else if (v.supportTargetVehicle && *v.supportTargetVehicle < vehicles_.size()) {
            const auto& tv = vehicles_[*v.supportTargetVehicle];
            if (tv.alive && tv.hp > 0) pushSeg(v.x, v.y, tv.x, tv.y, TacticalOrderLine::Kind::Support);
        } else if (v.supportTargetBuilding && *v.supportTargetBuilding < buildings_.size()) {
            const auto& tb = buildings_[*v.supportTargetBuilding];
            if (!tb.destroyed) pushSeg(v.x, v.y, tb.x, tb.y, TacticalOrderLine::Kind::Support);
        } else if (v.supportTargetSoldier && *v.supportTargetSoldier < soldiers_.size()) {
            const auto& ts = soldiers_[*v.supportTargetSoldier];
            if (ts.hp > 0 && !ts.embarked && !ts.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(ts);
                pushSeg(v.x, v.y, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (v.escortVehicleTarget && *v.escortVehicleTarget < vehicles_.size()) {
            const auto& tv = vehicles_[*v.escortVehicleTarget];
            if (tv.alive && tv.hp > 0) pushSeg(v.x, v.y, tv.x, tv.y, TacticalOrderLine::Kind::Support);
        } else if (v.escortSoldierTarget && *v.escortSoldierTarget < soldiers_.size()) {
            const auto& ts = soldiers_[*v.escortSoldierTarget];
            if (ts.hp > 0 && !ts.embarked && !ts.garrisonBuilding) {
                const auto [tu, tv] = soldierWorld(ts);
                pushSeg(v.x, v.y, originX_ + tu - tv, (tu + tv) / 2, TacticalOrderLine::Kind::Support);
            }
        } else if (v.moveTarget) {
            const auto kind = v.attackMove ? TacticalOrderLine::Kind::AttackMove : TacticalOrderLine::Kind::Move;
            auto prev = *v.moveTarget;
            pushSeg(v.x, v.y, prev.first, prev.second, kind);
            for (const auto& q : v.queuedOrders) {
                pushSeg(prev.first, prev.second, q.first, q.second, kind);
                prev = q;
            }
        }
    }
    return lines;
}

std::vector<VehicleRangePreview> TutorialUnits::selectionRangePreviews(std::span<const size_t> vehicleIds,
                                                                         std::span<const size_t> soldierIds) const {
    std::vector<VehicleRangePreview> out;
    std::vector<size_t> seen;
    for (const size_t vid : vehicleIds) {
        if (vid >= vehicles_.size()) continue;
        if (std::ranges::find(seen, vid) != seen.end()) continue;
        seen.push_back(vid);
        const auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew || !v.hasPrimary || v.range <= 0.0) continue;
        out.push_back({v.x, v.y, static_cast<float>(v.range), static_cast<float>(v.deadzone)});
        if (out.size() >= 4u) break;
    }
    if (out.size() < 4u && !soldierIds.empty()) {
        std::vector<size_t> seenSoldiers;
        for (const size_t sid : soldierIds) {
            if (sid >= soldiers_.size()) continue;
            if (std::ranges::find(seenSoldiers, sid) != seenSoldiers.end()) continue;
            seenSoldiers.push_back(sid);
            const auto& s = soldiers_[sid];
            if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding ||
                (!s.isMortar && !s.isFlamer)) {
                continue;
            }
            const int maxRange = scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa);
            const int minRange = scaleByExpa(s.desc.deadzoneBase, s.desc.deadzoneDelta, s.expa);
            if (maxRange <= 0) continue;
            const auto [su, sv] = soldierWorld(s);
            out.push_back({
                originX_ + su - sv, (su + sv) / 2,
                static_cast<float>(maxRange), static_cast<float>(minRange)
            });
            if (out.size() >= 4u) break;
        }
    }
    return out;
}

std::vector<assets::Placement> TutorialUnits::abandonVehicles(std::span<const size_t> vehicleIds,
                                                              std::vector<SoldierPose>* outPoses) {
    std::vector<assets::Placement> crewPlacements;
    for (size_t vid : vehicleIds) {
        if (vid >= vehicles_.size()) continue;
        auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.hasCrew) continue;
        if (!v.passengers.empty()) {
            (void)unloadVehicles(std::array<size_t, 1>{vid}, outPoses);
        }
        if (v.towedVehicle) detachTowedGun(vid);
        else if (v.towedBy) detachTowedGun(*v.towedBy);

        std::string crewId = "dguner";
        if (!v.crewUnit.empty()) {
            crewId = v.crewUnit;
        } else if (const auto* spec = catalog_ ? catalog_->find(v.id) : nullptr) {
            if (spec->nation == "russian") crewId = "rsmguner";
            else if (spec->nation == "american" || spec->nation == "british") crewId = "aguner";
            else if (spec->nation == "japanese") crewId = "jguner";
        }
        v.hasCrew = false;
        v.crewHp = 0;
        v.owner = 3;
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.patrolToDest = true;
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget.reset();
        v.pendingTowGun.reset();
        v.chasingAttackTarget = false;
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.wasMoving = false;
        fogDirty_ = true;
        emitVehiclePose(vid);

        const int cx = std::clamp(v.x + 18, 16, std::max(16, mapPixelW_ - 16));
        const int cy = std::clamp(v.y + 10, 16, std::max(16, mapPixelH_ - 16));
        crewPlacements.push_back({crewId, cx, cy, 0, 100, 40, v.group, 0});
    }
    if (!crewPlacements.empty() && fog_) {
        refreshSpotting(outPoses, true);
    }
    return crewPlacements;
}

void TutorialUnits::addReserveUnit(const ReserveUnit& entry) {
    reserveUnits_.push_back(entry);
}

std::vector<TutorialUnits::ReserveUnit> TutorialUnits::takeReserveUnits(unsigned group, int pool, std::size_t maxCount) {
    std::vector<ReserveUnit> deployed;
    std::erase_if(reserveUnits_, [&](const ReserveUnit& r) {
        if (deployed.size() >= maxCount) return false;
        if (r.group < 0 || static_cast<unsigned>(r.group) != group) return false;
        if (pool >= 0 && r.pool != pool) return false;
        deployed.push_back(r);
        return true;
    });
    return deployed;
}

unsigned TutorialUnits::countReserveByGroup(unsigned group) const {
    const int g = int(group);
    return unsigned(std::ranges::count_if(reserveUnits_, [&](const ReserveUnit& r) { return r.group == g; }));
}

unsigned TutorialUnits::countReserveByPlayer(int owner) const {
    return unsigned(std::ranges::count_if(reserveUnits_, [&](const ReserveUnit& r) { return r.owner == owner; }));
}

size_t TutorialUnits::orderAttackGroundVehicles(std::span<const size_t> indices, int mapX, int mapY) {
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.towedBy || !v.hasPrimary || v.ammo <= 0) continue;
        v.pendingTowGun.reset();
        v.attackGround = std::pair{clampedX, clampedY};
        v.attackTarget.reset();
        v.attackBuildingTarget.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.moveTarget.reset();
        v.waypoints.clear();
        v.chasingAttackTarget = false;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::orderAttackGroundSoldiers(std::span<const size_t> indices, int mapX, int mapY) {
    const int clampedX = std::clamp(mapX, 0, std::max(0, mapPixelW_ - 1));
    const int clampedY = std::clamp(mapY, 0, std::max(0, mapPixelH_ - 1));
    size_t accepted = 0;
    for (size_t id : indices) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        const bool mortarOrFlamer = (s.isMortar || s.isFlamer) && s.ammo > 0;
        const bool smgGrenade = s.canGrenadeGround && s.atGrenades > 0;
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding ||
            (!mortarOrFlamer && !smgGrenade)) {
            continue;
        }
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.autoTarget.reset();
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.queuedGoals.clear();
        s.attackMove = false;
        s.attackGround = std::pair{clampedX, clampedY};
        ++accepted;
    }
    return accepted;
}

std::optional<unsigned> TutorialUnits::lastHitTick(unsigned player) const {
    if (player >= lastHitTick_.size()) return std::nullopt;
    return lastHitTick_[player];
}

void TutorialUnits::setPlayerTeam(unsigned player, int team) {
    if (player >= playerTeams_.size()) return;
    playerTeams_[player] = team;
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(nullptr, true);
    }
}

int TutorialUnits::playerTeam(int owner) const noexcept {
    if (owner < 0 || owner >= 4) return -1;
    return playerTeams_[static_cast<std::size_t>(owner)];
}

int TutorialUnits::weatherVisibilityPct() const noexcept {
    if (weather_ == WeatherKind::Clear) return 100;
    const assets::WeatherSpec def{};
    const auto& ws = catalog_ ? catalog_->weather() : def;
    switch (weather_) {
    case WeatherKind::Fog: return std::clamp(ws.fogVisibilityPct, 0, 100);
    case WeatherKind::Rain: return std::clamp(ws.rainVisibilityPct, 0, 100);
    case WeatherKind::Snow: return std::clamp(ws.snowVisibilityPct, 0, 100);
    case WeatherKind::Clear: break;
    }
    return 100;
}

void TutorialUnits::setWeather(WeatherKind w) {
    if (weather_ == w) return;
    weather_ = w;
    if (fog_) {
        fogDirty_ = true;
        refreshSpotting(nullptr, true);
    }
}

std::optional<unsigned> TutorialUnits::lastGroupHitTick(unsigned group) const {
    if (group >= lastGroupHitTick_.size()) return std::nullopt;
    return lastGroupHitTick_[group];
}

void TutorialUnits::recordHit(int owner, int group) {
    if (owner >= 0 && owner < static_cast<int>(lastHitTick_.size())) {
        lastHitTick_[static_cast<std::size_t>(owner)] = tick_;
    }
    if (group >= 0 && group < static_cast<int>(lastGroupHitTick_.size())) {
        lastGroupHitTick_[static_cast<std::size_t>(group)] = tick_;
    }
}

void TutorialUnits::detachTowedGun(size_t tractorIdx) {
    if (tractorIdx >= vehicles_.size()) return;
    auto& tr = vehicles_[tractorIdx];
    tr.pendingTowGun.reset();
    if (!tr.towedVehicle) return;
    const size_t gunIdx = *tr.towedVehicle;
    tr.towedVehicle.reset();
    if (gunIdx < vehicles_.size()) {
        vehicles_[gunIdx].towedBy.reset();
        emitVehiclePose(gunIdx);
    }
    emitVehiclePose(tractorIdx);
}

void TutorialUnits::destroyVehicle(size_t vehicleIdx, std::vector<SoldierPose>& changed) {
    if (vehicleIdx >= vehicles_.size()) return;
    auto& tv = vehicles_[vehicleIdx];
    if (tv.towedBy) {
        const size_t tractorIdx = *tv.towedBy;
        tv.towedBy.reset();
        if (tractorIdx < vehicles_.size()) {
            vehicles_[tractorIdx].towedVehicle.reset();
            emitVehiclePose(tractorIdx);
        }
    }
    if (tv.towedVehicle) detachTowedGun(vehicleIdx);
    tv.alive = false;
    tv.hp = 0;
    tv.moveTarget.reset();
    tv.waypoints.clear();
    tv.queuedOrders.clear();
    tv.pendingTowGun.reset();
    tv.attackMove = false;
    tv.attackMoveEngaging = false;
    tv.attackGround.reset();
    tv.attackTarget.reset();
    tv.attackBuildingTarget.reset();
    tv.chasingAttackTarget = false;
    tv.reverseMove = false;
    tv.facingTargetDir.reset();
    tv.supportTargetVehicle.reset();
    tv.supportTargetBuilding.reset();
    tv.supportTargetSoldier.reset();
    tv.escortVehicleTarget.reset();
    tv.escortSoldierTarget.reset();
    tv.muzzleRevealTicks = 0u;
    tv.lastCombatTick = 0u;
    tv.wasMoving = false;
    tv.spottedMask = 0;
    tv.suppressionTicks = 0;
    tv.wasSuppressed = false;
    const int cookDmg = tv.cookOffDamage;
    const double cookR = tv.cookOffRadius;
    const int cookX = tv.x;
    const int cookY = tv.y;
    const SubCell cookCell = tv.cell;
    if (!soldierAtCell(tv.cell) && !vehicleAtCell(tv.cell)) {
        grid_.setOccupied(tv.cell, false);
    }
    if (cookDmg > 0 && cookR > 0.0 && cookOffDepth_ < 4) {
        ++cookOffDepth_;
        for (size_t k = 0; k < soldiers_.size(); ++k) {
            auto& ss = soldiers_[k];
            if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding) continue;
            const auto [su, sv] = soldierWorld(ss);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            const double sd = std::hypot(static_cast<double>(sx - cookX), static_cast<double>(sy - cookY));
            if (sd > cookR) continue;
            const double falloff = 1.0 - 0.55 * (sd / cookR);
            const int sdmg = std::max(1, static_cast<int>(std::lround(static_cast<double>(cookDmg) * 0.55 * falloff)));
            ss.hp = std::max(0, ss.hp - sdmg);
            recordHit(ss.owner, ss.group);
            if (ss.hp > 0) {
                applySuppression(k, 26);
            } else {
                if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
                ss.motion.reset();
                ss.goal.reset();
                ss.route.clear();
                ss.cursor = 0;
                ss.queuedGoals.clear();
                ss.attackMove = false;
                ss.attackTarget.reset();
                ss.attackVehicleTarget.reset();
                ss.attackBuildingTarget.reset();
                ss.autoTarget.reset();
                ss.binoculars = false;
                ss.spottedMask = 0;
                fogDirty_ = true;
            }
            const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
            SoldierPose sp{k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis};
            sp.suppressed = (ss.hp > 0 && ss.isSuppressed());
            pushOrUpdateSoldierPose(changed, sp);
        }
        for (size_t k = 0; k < vehicles_.size(); ++k) {
            if (k == vehicleIdx) continue;
            auto& tvOther = vehicles_[k];
            if (!tvOther.alive || tvOther.hp <= 0) continue;
            const double vd = std::hypot(tvOther.fx - static_cast<double>(cookX), tvOther.fy - static_cast<double>(cookY));
            if (vd > cookR) continue;
            const double falloff = 1.0 - 0.55 * (vd / cookR);
            const int rawSplash = std::max(1, static_cast<int>(std::lround(static_cast<double>(cookDmg) * falloff)));
            const int dmg = scaleVehicleDamageForDifficulty(
                tvOther.owner, applyVehicleDamage(tvOther.x, tvOther.y, tvOther.hullDir, tvOther.armor,
                                                    tvOther.hasCatalogArmor, cookX, cookY, rawSplash,
                                                    assets::HitType::Explosive));
            const int prevOwner = tvOther.owner;
            const int prevCrewHp = tvOther.crewHp;
            const bool wasAlive = tvOther.alive && tvOther.hp > 0;
            if (dmg > 0) {
                tvOther.hp = std::max(0, tvOther.hp - dmg);
            }
            if (wasAlive && tvOther.hp == 0) {
                destroyVehicle(k, changed);
            } else if (tvOther.hp > 0) {
                applyVehicleCrewDamage(k, cookX, cookY, rawSplash, assets::HitType::Explosive, changed, -1, -1);
                applyVehicleSuppression(k, 22, cookX, cookY);
            }
            if (dmg > 0 || tvOther.crewHp != prevCrewHp) {
                recordHit(prevOwner, tvOther.group);
                markVehicleCombat(tvOther);
                if (tvOther.hasCrew) {
                    reactToAttack(prevOwner, tvOther.group, cookX, cookY, k);
                }
                emitVehiclePose(k);
            } else if (tvOther.alive && tvOther.hp > 0 && tvOther.isSuppressed()) {
                emitVehiclePose(k);
            }
        }
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            const auto& tb = buildings_[bi];
            if (tb.destroyed) continue;
            const double bd = std::max(0.0, std::hypot(double(tb.x - cookX), double(tb.y - cookY)) -
                                            std::min(tb.width, tb.height) * 0.3);
            if (bd <= cookR + 12.0) {
                damageBuilding(bi, std::max(30, cookDmg / 2), assets::HitType::Explosive, -1, changed);
            }
        }
        if (!mines_.empty()) {
            triggerMineAtTile(cookCell.x / 2, cookCell.y / 2, changed, true);
        }
        --cookOffDepth_;
    }
    bailoutPassengers(vehicleIdx, changed);
    fogDirty_ = true;
}

bool TutorialUnits::canTowVehicle(size_t tractorIdx, size_t gunIdx, int viewerOwner) const {
    if (tractorIdx >= vehicles_.size() || gunIdx >= vehicles_.size() || tractorIdx == gunIdx) return false;
    const auto& tr = vehicles_[tractorIdx];
    const auto& gun = vehicles_[gunIdx];
    if (!tr.alive || tr.hp <= 0 || !gun.alive || gun.hp <= 0) return false;
    if (tr.owner != viewerOwner || isHostile(tr.owner, gun.owner)) return false;
    if (tr.towedVehicle || tr.towedBy || gun.towedVehicle || gun.towedBy) return false;
    if (assets::UnitSpec::towClassIndex(gun.towWeightClass) < 0) return false;
    return tr.towSpeedByClass[static_cast<std::size_t>(assets::UnitSpec::towClassIndex(gun.towWeightClass))] > 0.0;
}

size_t TutorialUnits::orderTowVehicle(size_t tractorIdx, size_t gunIdx) {
    if (tractorIdx >= vehicles_.size() || !canTowVehicle(tractorIdx, gunIdx, vehicles_[tractorIdx].owner)) return 0;
    auto& tr = vehicles_[tractorIdx];
    auto& gun = vehicles_[gunIdx];
    tr.attackTarget.reset();
    tr.attackGround.reset();
    tr.attackBuildingTarget.reset();
    tr.attackMove = false;
    tr.attackMoveEngaging = false;
    tr.queuedOrders.clear();
    tr.patrolOrigin.reset();
    tr.patrolDest.reset();
    tr.facingTargetDir.reset();
    tr.supportTargetVehicle.reset();
    tr.supportTargetBuilding.reset();
    tr.supportTargetSoldier.reset();
    tr.escortVehicleTarget.reset();
    tr.escortSoldierTarget.reset();
    const double dist = std::hypot(gun.fx - tr.fx, gun.fy - tr.fy);
    if (dist <= 68.0) {
        tr.pendingTowGun.reset();
        tr.moveTarget.reset();
        tr.waypoints.clear();
        tr.wasMoving = false;
        gun.moveTarget.reset();
        gun.waypoints.clear();
        gun.queuedOrders.clear();
        gun.attackTarget.reset();
        gun.attackGround.reset();
        gun.attackBuildingTarget.reset();
        gun.attackMove = false;
        gun.wasMoving = false;
        tr.towedVehicle = gunIdx;
        gun.towedBy = tractorIdx;
        emitVehiclePose(tractorIdx);
        emitVehiclePose(gunIdx);
        return 1;
    }
    tr.pendingTowGun = gunIdx;
    moveVehicleInternal(tr, gun.x, gun.y);
    return 1;
}

size_t TutorialUnits::unhitchVehicles(std::span<const size_t> vehicleIndices) {
    size_t unhitched = 0;
    for (size_t vid : vehicleIndices) {
        if (vid >= vehicles_.size()) continue;
        const auto& v = vehicles_[vid];
        if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
        if (v.towedVehicle) {
            detachTowedGun(vid);
            ++unhitched;
        } else if (v.towedBy) {
            detachTowedGun(*v.towedBy);
            ++unhitched;
        }
    }
    return unhitched;
}

void TutorialUnits::stepTowing() {
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& tr = vehicles_[i];
        if (!tr.alive || tr.hp <= 0) {
            if (tr.towedVehicle) detachTowedGun(i);
            tr.pendingTowGun.reset();
            continue;
        }
        if (tr.towedVehicle) {
            const size_t gIdx = *tr.towedVehicle;
            if (gIdx >= vehicles_.size() || !vehicles_[gIdx].alive || vehicles_[gIdx].hp <= 0) {
                detachTowedGun(i);
            }
        }
        if (!tr.pendingTowGun) continue;
        const size_t gIdx = *tr.pendingTowGun;
        if (!canTowVehicle(i, gIdx, tr.owner)) {
            tr.pendingTowGun.reset();
            continue;
        }
        auto& gun = vehicles_[gIdx];
        if (std::hypot(gun.fx - tr.fx, gun.fy - tr.fy) <= 68.0) {
            tr.pendingTowGun.reset();
            tr.moveTarget.reset();
            tr.waypoints.clear();
            tr.wasMoving = false;
            gun.moveTarget.reset();
            gun.waypoints.clear();
            gun.queuedOrders.clear();
            gun.attackTarget.reset();
            gun.attackGround.reset();
            gun.attackBuildingTarget.reset();
            gun.attackMove = false;
            gun.wasMoving = false;
            tr.towedVehicle = gIdx;
            gun.towedBy = i;
            emitVehiclePose(i);
            emitVehiclePose(gIdx);
        } else if (!tr.moveTarget) {
            moveVehicleInternal(tr, gun.x, gun.y);
            if (!tr.moveTarget) tr.pendingTowGun.reset();
        }
    }
}

SubCell TutorialUnits::findBuildingEntryCell(int mapX, int mapY) const {
    const int w = grid_.width(), h = grid_.height();
    const SubCell raw = pointerCell(mapX, mapY, originX_);
    if (w <= 0 || h <= 0) return raw;
    const int cx = std::clamp(raw.x, 0, w - 1);
    const int cy = std::clamp(raw.y, 0, h - 1);
    std::optional<SubCell> walkableFallback;
    for (int r = 0; r <= 24; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                const SubCell cand{cx + dx, cy + dy};
                if (!grid_.inBounds(cand)) continue;
                if (grid_.canEnter(cand)) return cand;
                if (!walkableFallback && grid_.walkable(cand)) walkableFallback = cand;
            }
        }
    }
    return walkableFallback.value_or(SubCell{cx, cy});
}

void TutorialUnits::emitBuildingUpdate(size_t bIdx, bool damaged, bool collapsed) {
    if (bIdx >= buildings_.size()) return;
    const auto& b = buildings_[bIdx];
    BuildingSnapshot snap{
        bIdx, b.x, b.y, b.left, b.top, b.width, b.height,
        b.hp, b.maxHp, b.capacity, static_cast<int>(b.occupants.size()),
        b.owner, b.destroyed, damaged, collapsed, b.occupants
    };
    const auto it = std::ranges::find(buildingUpdates_, bIdx, &BuildingSnapshot::index);
    if (it == buildingUpdates_.end()) {
        buildingUpdates_.push_back(snap);
        return;
    }
    const bool prevDamaged = it->damagedThisTick;
    const bool prevCollapsed = it->collapsedThisTick;
    *it = snap;
    it->damagedThisTick = it->damagedThisTick || prevDamaged;
    it->collapsedThisTick = it->collapsedThisTick || prevCollapsed;
}

void TutorialUnits::setBuildings(std::vector<assets::BuildingPlacement> buildings) {
    buildings_.clear();
    buildingUpdates_.clear();
    buildings_.reserve(buildings.size());
    for (const auto& bp : buildings) {
        Building b{};
        b.x = bp.x;
        b.y = bp.y;
        b.left = bp.left;
        b.top = bp.top;
        b.width = bp.width;
        b.height = bp.height;
        b.maxHp = std::max(100, bp.maxHp);
        b.hp = b.maxHp;
        b.capacity = std::clamp(bp.capacity, 2, 16);
        b.entryCell = findBuildingEntryCell(bp.x, bp.y);
        buildings_.push_back(std::move(b));
    }
}

bool TutorialUnits::canGarrisonBuilding(size_t buildingIdx, int viewerOwner) const {
    if (buildingIdx >= buildings_.size()) return false;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || int(b.occupants.size()) >= b.capacity) return false;
    return b.owner < 0 || !isHostile(b.owner, viewerOwner);
}

size_t TutorialUnits::orderGarrisonBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    if (!canGarrisonBuilding(buildingIdx, 0)) return 0;
    const auto& b = buildings_[buildingIdx];
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        s.garrisonTarget = buildingIdx;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackBuildingTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.goal = b.entryCell;
        s.route.clear();
        s.cursor = 0;
        s.planning = {};
        s.needsRoute = false;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::evacuateBuilding(size_t buildingIdx, std::vector<SoldierPose>* outPoses) {
    if (buildingIdx >= buildings_.size()) return 0;
    auto& b = buildings_[buildingIdx];
    size_t evacuated = 0;
    for (size_t sid : b.occupants) {
        if (sid >= soldiers_.size()) continue;
        auto& s = soldiers_[sid];
        if (s.hp <= 0) continue;
        const SubCell spot = findSpawnCell(grid_, b.entryCell);
        s.garrisonBuilding.reset();
        s.garrisonTarget.reset();
        s.cell = spot;
        s.motion.reset();
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        grid_.setOccupied(spot, true);
        if (outPoses) {
            const auto [u, w] = soldierWorld(s);
            const bool vis = !fog_ || isAlliedWithPlayer(s.owner) || ((fog_->tile(spot.x / 2, spot.y / 2) & 1u) != 0);
            pushOrUpdateSoldierPose(*outPoses, {
                sid, originX_ + u - w, (u + w) / 2, s.direction, 0, false,
                s.hp, true, s.owner, s.group, false, vis, 0, 0, false
            });
        }
        ++evacuated;
    }
    b.occupants.clear();
    b.owner = -1;
    fogDirty_ = true;
    emitBuildingUpdate(buildingIdx);
    if (evacuated > 0 && fog_) refreshSpotting(outPoses, true);
    if (outPoses) {
        for (auto& pose : *outPoses) {
            if (pose.index < soldiers_.size()) {
                pose.expa = soldiers_[pose.index].expa;
                pose.kills = soldiers_[pose.index].kills;
                pose.atGrenades = soldiers_[pose.index].atGrenades;
                pose.mineAmmo = soldiers_[pose.index].mineAmmo;
                pose.ammo = soldiers_[pose.index].ammo;
                pose.suppressed = soldiers_[pose.index].isSuppressed();
            }
        }
    }
    return evacuated;
}

std::optional<bool> TutorialUnits::toggleBuildingHoldFire(size_t buildingIdx) {
    if (buildingIdx >= buildings_.size()) return std::nullopt;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != 0 || b.occupants.empty()) return std::nullopt;
    return toggleHoldFire(b.occupants);
}

std::optional<bool> TutorialUnits::toggleBuildingBinoculars(size_t buildingIdx, std::vector<SoldierPose>* outPoses) {
    if (buildingIdx >= buildings_.size()) return std::nullopt;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != 0 || b.occupants.empty()) return std::nullopt;
    const bool hasSpotter = std::ranges::any_of(b.occupants, [&](size_t sid) {
        return sid < soldiers_.size() && soldiers_[sid].owner == 0 &&
               soldiers_[sid].hp > 0 && soldiers_[sid].hasBinoculars;
    });
    if (!hasSpotter) return std::nullopt;
    return toggleBinocularsWithPoses(b.occupants, outPoses);
}

size_t TutorialUnits::orderBuildingAttack(size_t buildingIdx, bool targetIsVehicle, size_t targetIdx) {
    if (buildingIdx >= buildings_.size()) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != 0 || b.occupants.empty()) return 0;
    std::vector<size_t> eligible;
    eligible.reserve(b.occupants.size());
    for (size_t occId : b.occupants) {
        if (occId < soldiers_.size() && soldiers_[occId].canFireFromInside) {
            eligible.push_back(occId);
        }
    }
    if (eligible.empty()) return 0;
    return targetIsVehicle ? attackVehicle(eligible, targetIdx) : orderAttack(eligible, targetIdx);
}

size_t TutorialUnits::orderBuildingAttackBuilding(size_t buildingIdx, size_t targetBuildingIdx) {
    if (buildingIdx >= buildings_.size() || buildingIdx == targetBuildingIdx) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner != 0 || b.occupants.empty()) return 0;
    std::vector<size_t> eligible;
    eligible.reserve(b.occupants.size());
    for (size_t occId : b.occupants) {
        if (occId < soldiers_.size() && soldiers_[occId].canFireFromInside) {
            eligible.push_back(occId);
        }
    }
    if (eligible.empty()) return 0;
    return orderAttackBuilding(eligible, targetBuildingIdx);
}

size_t TutorialUnits::orderAttackBuilding(std::span<const size_t> soldierIds, size_t buildingIdx) {
    if (buildingIdx >= buildings_.size()) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner < 0 || !isHostile(0, b.owner)) return 0;
    size_t accepted = 0;
    for (size_t id : soldierIds) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.ammo <= 0) continue;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.attackMove = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.attackBuildingTarget = buildingIdx;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::attackBuildingWithVehicles(std::span<const size_t> vehicleIds, size_t buildingIdx) {
    if (buildingIdx >= buildings_.size()) return 0;
    const auto& b = buildings_[buildingIdx];
    if (b.destroyed || b.owner < 0 || !isHostile(0, b.owner)) return 0;
    size_t accepted = 0;
    for (size_t id : vehicleIds) {
        if (id >= vehicles_.size()) continue;
        auto& v = vehicles_[id];
        if (!v.alive || v.hp <= 0 || v.owner != 0 || v.towedBy || !v.armed ||
            (v.ammo <= 0 && (!v.hasSecondary || v.ammo2 <= 0)))
            continue;
        v.pendingTowGun.reset();
        v.attackTarget.reset();
        v.attackGround.reset();
        v.attackBuildingTarget = buildingIdx;
        v.chasingAttackTarget = false;
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.queuedOrders.clear();
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        ++accepted;
    }
    return accepted;
}

std::vector<BuildingSnapshot> TutorialUnits::takeBuildingUpdates() {
    return std::exchange(buildingUpdates_, {});
}

void TutorialUnits::stepGarrisons(std::vector<SoldierPose>& changed) {
    if (buildings_.empty()) return;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.garrisonTarget) continue;
        const size_t bIdx = *s.garrisonTarget;
        if (!canGarrisonBuilding(bIdx, s.owner)) {
            s.garrisonTarget.reset();
            continue;
        }
        auto& b = buildings_[bIdx];
        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const auto [eu, ev] = InfantryPathGrid::toWorldCenter(b.entryCell);
        const int ex = originX_ + eu - ev;
        const int ey = (eu + ev) / 2;
        const bool inRect = sx >= b.left - 32 && sx <= b.left + b.width + 32 &&
                            sy >= b.top - 32 && sy <= b.top + b.height + 32;
        const double distEntry = std::hypot(double(ex - sx), double(ey - sy));
        const double distCenter = std::hypot(double(b.x - sx), double(b.y - sy));
        const double reach = std::max(60.0, std::min(double(b.width), double(b.height)) * 0.55);
        if (inRect || distEntry <= 60.0 || distCenter <= reach) {
            const SubCell oldCell = s.cell;
            s.garrisonBuilding = bIdx;
            s.garrisonTarget.reset();
            s.motion.reset();
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            s.crouched = false;
            s.binoculars = false;
            s.spottedMask = 0;
            s.cell = b.entryCell;
            if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) {
                grid_.setOccupied(oldCell, false);
            }
            b.occupants.push_back(i);
            if (b.owner < 0) b.owner = s.owner;
            fogDirty_ = true;
            pushOrUpdateSoldierPose(changed, {
                i, b.x, b.y, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false, 0, 0, true
            });
            emitBuildingUpdate(bIdx);
            continue;
        }
        if (!s.goal || !sameTile(*s.goal, b.entryCell)) {
            s.goal = b.entryCell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
    }
}

void TutorialUnits::damageBuilding(size_t bIdx, int rawDamage, assets::HitType hitType, int attackerOwner,
                                   std::vector<SoldierPose>& changed) {
    (void)attackerOwner;
    if (bIdx >= buildings_.size()) return;
    auto& b = buildings_[bIdx];
    if (b.destroyed) return;
    if (b.owner >= 0) recordHit(b.owner);

    const auto bldDef = catalog_ ? catalog_->buildingDefense() : assets::BuildingDefense::retailDefault();
    const int bldArmor = bldDef.armorFor(hitType);
    const int bldProt = bldDef.protectionFor(hitType);
    int structDmg = 0;
    int splashHits = 0;
    int splashDmg = 0;
    if (hitType == assets::HitType::Fire) {
        // LAND\DEFDOM: ARMOR FIRE 0, PROTECTION FIRE 0 — incendiary weapons bypass stone cover.
        structDmg = std::max(6, rawDamage);
        splashHits = 2;
        splashDmg = std::max(10, rawDamage);
    } else if (hitType == assets::HitType::Explosive || hitType == assets::HitType::Pierce ||
               hitType == assets::HitType::Transpierce || hitType == assets::HitType::Mine) {
        if (rawDamage > bldArmor) {
            structDmg = std::max(12, rawDamage * 3 / 2);
            splashHits = 2;
            splashDmg = std::max(4, rawDamage / 3);
        } else {
            structDmg = std::max(8, rawDamage);
            splashHits = 2;
            splashDmg = std::max(3, rawDamage / 4);
        }
    } else {
        // Small arms (Machine/Sniper): LAND\DEFDOM ARMOR MACHINE 50 prevents rifles from demolishing masonry.
        if (rawDamage > bldArmor) {
            structDmg = std::max(2, (rawDamage - bldArmor) / 2);
        }
        if (nextCombatRandom(3u) == 0u) {
            if (rawDamage > bldProt) {
                splashHits = 1;
                splashDmg = std::max(2, (rawDamage - bldProt) / 2 + 2);
            } else if (rawDamage >= 20) {
                splashHits = 1;
                splashDmg = 1;
            }
        }
    }

    if (splashHits > 0 && splashDmg > 0) {
        int left = splashHits;
        for (size_t sid : b.occupants) {
            if (left <= 0) break;
            if (sid >= soldiers_.size()) continue;
            auto& occ = soldiers_[sid];
            if (occ.hp <= 0) continue;
            occ.hp = std::max(0, occ.hp - splashDmg);
            recordHit(occ.owner, occ.group);
            --left;
            if (occ.hp == 0) {
                occ.garrisonBuilding.reset();
                occ.spottedMask = 0;
                fogDirty_ = true;
                pushOrUpdateSoldierPose(changed, {
                    sid, b.x, b.y, occ.direction, 0, false, 0, false, occ.owner, occ.group, false, false, 0, 0, true
                });
            }
        }
        std::erase_if(b.occupants, [&](size_t sid) { return sid >= soldiers_.size() || soldiers_[sid].hp <= 0; });
        if (b.occupants.empty()) b.owner = -1;
    }
    if (!b.occupants.empty() &&
        (hitType == assets::HitType::Fire || hitType == assets::HitType::Explosive ||
         hitType == assets::HitType::Pierce || hitType == assets::HitType::Transpierce ||
         hitType == assets::HitType::Mine)) {
        const int bldSup = (hitType == assets::HitType::Fire) ? 24 : 18;
        for (size_t sid : b.occupants) {
            applySuppression(sid, bldSup, true);
        }
    }

    b.hp = std::max(0, b.hp - structDmg);
    if (b.hp == 0) {
        b.destroyed = true;
        for (size_t sid : b.occupants) {
            if (sid >= soldiers_.size()) continue;
            auto& occ = soldiers_[sid];
            if (occ.hp <= 0) continue;
            occ.hp = std::max(0, occ.hp - 35);
            if (occ.hp == 0) {
                occ.garrisonBuilding.reset();
                occ.spottedMask = 0;
                pushOrUpdateSoldierPose(changed, {
                    sid, b.x, b.y, occ.direction, 0, false, 0, false, occ.owner, occ.group, false, false, 0, 0, true
                });
            }
        }
        std::erase_if(b.occupants, [&](size_t sid) { return sid >= soldiers_.size() || soldiers_[sid].hp <= 0; });
        for (size_t sid : b.occupants) {
            applySuppression(sid, 30, true);
        }
        (void)evacuateBuilding(bIdx, &changed);
        emitBuildingUpdate(bIdx, true, true);
    } else {
        emitBuildingUpdate(bIdx, structDmg > 0, false);
    }
}

unsigned TutorialUnits::countAliveInGroup(int owner, int group) const {
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.owner == owner && s.group == group;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.owner == owner && v.group == group;
    }));
}

unsigned TutorialUnits::countAliveByGroup(unsigned group) const {
    const int g = int(group);
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.group == g;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.group == g && (v.hasCrew || !v.passengers.empty());
    }));
}

unsigned TutorialUnits::countAliveByPlayer(int owner) const {
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.owner == owner;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.owner == owner && (v.hasCrew || !v.passengers.empty());
    }));
}

unsigned TutorialUnits::countByGroupMask(unsigned group, unsigned mask) const {
    const int g = int(group);
    unsigned total = 0;
    if ((mask & 1u) != 0u) {
        total += countAliveByGroup(group);
    }
    if ((mask & 2u) != 0u) {
        total += unsigned(std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.group == g && !v.hasCrew && v.passengers.empty();
        }));
    }
    if ((mask & 4u) != 0u) {
        total += countReserveByGroup(group);
    }
    return total;
}

unsigned TutorialUnits::countByGroupTeamMask(unsigned group, int team, unsigned mask) const {
    const int g = int(group);
    const auto matchesTeam = [&](int owner) {
        return owner >= 0 && owner < 4 && playerTeams_[static_cast<std::size_t>(owner)] == team;
    };
    unsigned total = 0;
    if ((mask & 1u) != 0u) {
        total += unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
            return s.hp > 0 && s.group == g && matchesTeam(s.owner);
        }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.group == g && (v.hasCrew || !v.passengers.empty()) &&
                   matchesTeam(v.owner);
        }));
    }
    if ((mask & 2u) != 0u) {
        total += unsigned(std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.group == g && !v.hasCrew && v.passengers.empty() &&
                   matchesTeam(v.owner);
        }));
    }
    if ((mask & 4u) != 0u) {
        total += unsigned(std::ranges::count_if(reserveUnits_, [&](const ReserveUnit& r) {
            return r.group == g && matchesTeam(r.owner);
        }));
    }
    return total;
}

unsigned TutorialUnits::countByPlayerMask(int owner, unsigned mask) const {
    unsigned total = 0;
    if ((mask & 1u) != 0u) {
        total += countAliveByPlayer(owner);
    }
    if ((mask & 2u) != 0u) {
        total += unsigned(std::ranges::count_if(vehicles_, [&](const auto& v) {
            return v.alive && v.hp > 0 && v.owner == owner && !v.hasCrew && v.passengers.empty();
        }));
    }
    if ((mask & 4u) != 0u) {
        total += countReserveByPlayer(owner);
    }
    return total;
}

std::pair<int, int> TutorialUnits::soldierWorld(const Soldier& s) const {
    if (s.motion) return s.motion->position();
    return InfantryPathGrid::toWorldCenter(s.cell);
}

bool TutorialUnits::soldierAtCell(SubCell cell) const {
    return std::ranges::any_of(soldiers_, [&](const auto& s) {
        return s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.cell == cell;
    });
}

bool TutorialUnits::vehicleAtCell(SubCell cell) const {
    return std::ranges::any_of(vehicles_, [&](const auto& v) {
        return v.alive && v.hp > 0 && v.cell == cell;
    });
}

void TutorialUnits::emitVehiclePose(size_t index, bool fired, int targetX, int targetY,
                                    bool firedSecondary, std::string_view fireSound,
                                    bool heavyImpact, bool hitVehicle,
                                    bool repaired, bool resupplied, bool captured,
                                    bool firedRocket, bool firedFlamer, bool decrewed) {
    if (index >= vehicles_.size()) return;
    const auto& v = vehicles_[index];
    VehiclePose pose{
        index,
        v.x,
        v.y,
        v.hullDir,
        v.turretDir,
        v.hp,
        v.ammo,
        v.alive,
        v.visible,
        v.moveTarget.has_value(),
        fired,
        targetX,
        targetY,
        v.owner,
        v.group,
        v.maxHp,
        firedSecondary,
        fired ? std::string(fireSound) : std::string{},
        static_cast<int>(v.passengers.size()),
        v.passengerCapacity,
        heavyImpact,
        hitVehicle,
        repaired,
        resupplied,
        captured,
        v.towedVehicle ? int(*v.towedVehicle) : -1,
        v.towedBy ? int(*v.towedBy) : -1,
        v.expa,
        v.kills,
        firedRocket,
        firedFlamer,
        v.ammo2,
        decrewed,
        v.isCrewImpaired(),
        v.isMoveImpaired(),
        v.isSuppressed(),
        static_cast<std::uint8_t>((fired && !firedSecondary && heavyImpact) ? std::clamp(v.craterScale1, 0, 15) : 0),
        isVehicleUnbuttoned(v),
    };
    const auto it = std::ranges::find(vehiclePoses_, index, &VehiclePose::index);
    if (it == vehiclePoses_.end()) {
        vehiclePoses_.push_back(pose);
        return;
    }
    const bool wasFired = it->fired;
    const int oldTx = it->targetX;
    const int oldTy = it->targetY;
    const bool oldSec = it->firedSecondary;
    const bool oldHeavy = it->heavyImpact;
    const bool oldHitVeh = it->hitVehicle;
    const bool oldRocket = it->firedRocket;
    const bool oldFlamer = it->firedFlamer;
    const std::uint8_t oldCrater = it->craterScale;
    const bool wasRepaired = it->repaired;
    const bool wasResupplied = it->resupplied;
    const bool wasCaptured = it->captured;
    const bool wasDecrewed = it->decrewed;
    std::string oldSnd = std::move(it->fireSound);
    *it = pose;
    if (wasFired && !fired) {
        it->fired = true;
        it->targetX = oldTx;
        it->targetY = oldTy;
        it->firedSecondary = oldSec;
        it->heavyImpact = oldHeavy;
        it->hitVehicle = oldHitVeh;
        it->firedRocket = oldRocket;
        it->firedFlamer = oldFlamer;
        it->craterScale = oldCrater;
        it->fireSound = std::move(oldSnd);
    }
    it->repaired = it->repaired || wasRepaired;
    it->resupplied = it->resupplied || wasResupplied;
    it->captured = it->captured || wasCaptured;
    it->decrewed = it->decrewed || wasDecrewed;
}

void TutorialUnits::refreshSpotting(std::vector<SoldierPose>* changed, bool emitVehiclePoses) {
    if (!fog_) return;
    fog_->clearCurrentVisibility();
    const auto revealWithAllies = [&](int tileX, int tileY, int r, int owner) {
        if (owner < 0 || owner >= 4) return;
        for (int p = 0; p < 4; ++p) {
            if (p == owner || (!isHostile(owner, p) && playerTeams_[p] != 3)) {
                fog_->revealCircle(tileX, tileY, r, p);
            }
        }
    };
    const int visPct = weatherVisibilityPct();
    const auto applyWeatherSight = [visPct](int r) {
        return visPct >= 100 ? r : std::max(1, (r * visPct) / 100);
    };
    for (const auto& s : soldiers_) {
        if (s.hp <= 0 || s.embarked) continue;
        int r = effectiveSightRadius(s.desc, s.expa, s.crouched, s.owner, difficulty_);
        if (s.garrisonBuilding) {
            r = std::min(63, r + 3 + (s.hasBinoculars && s.binoculars ? s.binoSightBonus : 0));
            const auto& b = buildings_[*s.garrisonBuilding];
            revealWithAllies(b.entryCell.x / 2, b.entryCell.y / 2, applyWeatherSight(r), s.owner);
            continue;
        }
        if (s.hasBinoculars && s.binoculars && !s.motion && s.route.empty()) {
            r = std::min(63, r + s.binoSightBonus);
        }
        if (fog_->isElevated(s.cell.x / 2, s.cell.y / 2)) {
            r = std::min(63, r + 2);
        }
        revealWithAllies(s.cell.x / 2, s.cell.y / 2, applyWeatherSight(r), s.owner);
    }
    for (const auto& v : vehicles_) {
        if (!v.alive || v.hp <= 0 || !v.hasCrew) continue;
        const bool buttonedUp = !v.hatchOpen || (v.lastCombatTick != 0u && tick_ < v.lastCombatTick + 125u);
        const int marchBonus = buttonedUp ? 0 : v.marchSightBonus;
        const int elevBonus = fog_->isElevated(v.cell.x / 2, v.cell.y / 2) ? 2 : 0;
        const int vr = std::min(63, v.sightTiles + elevBonus + (v.expa >= 16384 ? 2 : (v.expa >= 8192 ? 1 : 0)));
        int effVr = applyWeatherSight(vr);
        if (v.bonusSight != 0) {
            effVr = std::clamp(effVr + (v.bonusSight * visPct) / 100, 1, 63);
        }
        if (marchBonus > 0) {
            // Gold 0x1001d3e0 adds (marchsightbonus * visPct) / 100 after base weather sight.
            effVr = std::clamp(effVr + (marchBonus * visPct) / 100, effVr, 63);
        }
        revealWithAllies(v.cell.x / 2, v.cell.y / 2, effVr, v.owner);
    }
    for (const auto& st : sorties_) {
        if (st.kind != assets::AirMissionKind::Recon) continue;
        const SubCell cell = placementCell(static_cast<int>(std::lround(st.fx)),
                                           static_cast<int>(std::lround(st.fy)), originX_);
        revealWithAllies(cell.x / 2, cell.y / 2, applyWeatherSight(std::clamp(st.sightTiles, 12, 28)), st.owner);
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding) {
            s.spottedMask = 0;
            continue;
        }
        const bool allyVis = isAlliedWithPlayer(s.owner);
        const bool wasVisible = allyVis || (s.spottedMask & 1u) != 0;
        const std::uint8_t tileFog = fog_->tile(s.cell.x / 2, s.cell.y / 2) & 0x0fu;
        std::uint8_t mask = static_cast<std::uint8_t>(tileFog & ~(1u << s.owner));
        const bool camo = isSoldierCamouflaged(s);
        if (camo && mask != 0u) {
            const auto [su, sv] = soldierWorld(s);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            for (int p = 0; p < 4; ++p) {
                const std::uint8_t pBit = static_cast<std::uint8_t>(1u << p);
                if ((mask & pBit) == 0u || !isHostile(s.owner, p)) continue;
                bool detected = false;
                for (size_t j = 0; j < soldiers_.size() && !detected; ++j) {
                    const auto& d = soldiers_[j];
                    if (d.hp <= 0 || d.embarked || isHostile(p, d.owner)) continue;
                    const auto [du, dv] = d.garrisonBuilding
                        ? tutorialFootWorld(buildings_[*d.garrisonBuilding].x, buildings_[*d.garrisonBuilding].y, originX_)
                        : soldierWorld(d);
                    const int dx = originX_ + du - dv;
                    const int dy = (du + dv) / 2;
                    const double dist = std::hypot(static_cast<double>(dx - sx), static_cast<double>(dy - sy));
                    if (dist <= 140.0 ||
                        (d.hasBinoculars && d.binoculars && !d.motion && d.route.empty() && dist <= 640.0)) {
                        detected = true;
                    }
                }
                for (size_t j = 0; j < vehicles_.size() && !detected; ++j) {
                    const auto& dv = vehicles_[j];
                    if (!dv.alive || dv.hp <= 0 || isHostile(p, dv.owner)) continue;
                    if (std::hypot(static_cast<double>(dv.x - sx), static_cast<double>(dv.y - sy)) <= 140.0) {
                        detected = true;
                    }
                }
                if (!detected) mask = static_cast<std::uint8_t>(mask & ~pBit);
            }
        }
        s.spottedMask = mask;
        const bool nowVisible = allyVis || (s.spottedMask & 1u) != 0;
        if (changed && nowVisible != wasVisible) {
            const auto [u, v] = soldierWorld(s);
            changed->push_back({i, originX_ + u - v, (u + v) / 2, s.direction, 0, false, s.hp, true,
                                s.owner, s.group, false, nowVisible, 0, 0, false,
                                s.expa, s.kills, false, s.atGrenades, false, camo});
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        const bool wasVisible = v.visible;
        if (!v.alive || v.hp <= 0) {
            v.spottedMask = 0;
            continue;
        }
        const std::uint8_t tileFog = fog_->tile(v.cell.x / 2, v.cell.y / 2) & 0x0fu;
        v.spottedMask = static_cast<std::uint8_t>(tileFog & ~(1u << v.owner));
        if (v.muzzleRevealTicks > 0u && isHostile(0, v.owner)) {
            v.spottedMask = static_cast<std::uint8_t>(v.spottedMask | 1u);
        }
        const bool nowVisible = isAlliedWithPlayer(v.owner) || ((v.spottedMask & 1u) != 0);
        v.visible = nowVisible;
        if (emitVehiclePoses && nowVisible != wasVisible) {
            emitVehiclePose(i);
        }
    }
    fogDirty_ = false;
    fogSnapshotDirty_ = true;
}

std::optional<std::vector<std::uint8_t>> TutorialUnits::takeFogSnapshot() {
    if (!fog_ || !fogSnapshotDirty_) return std::nullopt;
    fogSnapshotDirty_ = false;
    const auto raw = fog_->raw();
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

std::uint32_t TutorialUnits::nextCombatRandom(std::uint32_t mask) {
    combatRandomState_ = combatRandomState_ * 0x41c64e6du + 0x3039u;
    const auto shifted = static_cast<std::int32_t>(combatRandomState_) >> 16;
    return static_cast<std::uint32_t>(shifted) & mask;
}

int TutorialUnits::Soldier::movementPriority() const {
    // 0x10021cb0, standing Tutorial I player infantry: movespeed 0.5 * 256, then >> 3.
    if (hp <= 0 || !goal || (!motion && needsRoute && planning.backoff)) return 0;
    return 16;
}

void TutorialUnits::refreshRoutes() {
    // The retail path scheduler stamps lower-priority footprints before searching
    // the next bucket. Equally ranked soldiers can follow one another's routes.
    if (std::ranges::none_of(soldiers_, [](const auto& s) {
        return s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.goal && s.needsRoute && !s.planning.backoff;
    })) return;
    std::map<int, std::vector<size_t>> requests;
    std::vector<int> priorities;
    priorities.reserve(soldiers_.size());
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        const int base = (s.hp > 0 && !s.embarked && !s.garrisonBuilding) ? s.movementPriority() : 0;
        priorities.push_back(base);
        if (s.hp > 0 && !s.embarked && !s.garrisonBuilding && s.goal && s.needsRoute && !s.planning.backoff)
            requests[std::min(255, base + (s.planning.wait >> 4))].push_back(i);
    }
    for (const auto& [priority, ids] : requests) {
        auto shadow = grid_.liveSlots();
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            if (soldiers_[i].hp <= 0 || soldiers_[i].embarked || soldiers_[i].garrisonBuilding || priorities[i] < priority) continue;
            const auto cell = soldiers_[i].cell;
            const unsigned slot = (cell.x&1) + 2*(cell.y&1);
            shadow[(cell.y/2)*(grid_.width()/2) + cell.x/2] &= ~(1u << slot);
        }
        // Gold prepends requests to each bucket before draining it.
        for (auto id = ids.rbegin(); id != ids.rend(); ++id) {
            auto& s = soldiers_[*id];
            if (sameTile(s.cell, *s.goal)) {
                s.needsRoute = false;
                continue;
            }
            std::optional<std::uint8_t> next;
            if (s.cursor < s.route.size()) next = s.route[s.cursor];
            auto plan = grid_.plan(s.cell, *s.goal, next, shadow, s.planning,
                                   std::uint8_t(infantryRandom(randomState_,255)));
            using Action = InfantryPathGrid::PlanAction;
            s.needsRoute = plan.action == Action::wait;
            if (plan.action == Action::keepQueue) continue;
            s.route = std::move(plan.directions);
            s.cursor = 0;
            if (plan.action == Action::giveUp) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
            }
        }
    }
}

bool TutorialUnits::startNext(Soldier& soldier) {
    if (soldier.hp <= 0 || !soldier.goal || soldier.needsRoute) return false;
    if (soldier.cursor == soldier.route.size()) {
        if (sameTile(soldier.cell, *soldier.goal)) {
            soldier.goal.reset();
            soldier.route.clear();
            soldier.cursor = 0;
        } else {
            soldier.needsRoute = true;
        }
        return false;
    }
    const auto next = grid_.acceptStep(soldier.cell,soldier.route[soldier.cursor]);
    if (!next) {
        if (sameTile(soldier.cell, *soldier.goal)) {
            soldier.goal.reset();
            soldier.route.clear();
            soldier.cursor = 0;
        } else {
            soldier.needsRoute = true;
        }
        return false;
    }
    if (next->crossesTile) {
        ++soldier.cursor;
        // 0x10021b70 always draws this bit on crossing a tile for its visibility update.
        infantryRandom(randomState_, 1);
        fogDirty_ = true;
    }
    grid_.setOccupied(soldier.cell,false);
    grid_.setOccupied(next->cell,true);
    soldier.cell = next->cell;
    soldier.direction = next->direction;
    soldier.motion = InfantryStep{next->cell.x/2, next->cell.y/2,
        unsigned(next->cell.x%2 + 2*(next->cell.y%2)), next->direction};
    if (soldier.binoculars) {
        soldier.binoculars = false;
        fogDirty_ = true;
    }
    return true;
}

bool TutorialUnits::canVehicleStepTo(const Vehicle& v, double nextFx, double nextFy, SubCell& outCell) const {
    const int ix = static_cast<int>(std::lround(nextFx));
    const int iy = static_cast<int>(std::lround(nextFy));
    if (!mapBounds(ix, iy)) return false;
    outCell = placementCell(ix, iy, originX_);
    if (outCell == v.cell) return true;
    if (!grid_.inBounds(v.cell)) {
        return outCell.x >= 0 && outCell.y >= 0 && outCell.x < grid_.width() && outCell.y < grid_.height();
    }
    return grid_.inBounds(outCell) && grid_.walkableMask(outCell, v.forbiddenTerrainMask);
}

bool TutorialUnits::isVehicleTargetValidFor(int attackerOwner, bool targetIsVehicle, size_t targetIdx) const {
    if (targetIsVehicle) {
        if (targetIdx >= vehicles_.size()) return false;
        const auto& tv = vehicles_[targetIdx];
        if (!tv.alive || tv.hp <= 0 || !isHostile(attackerOwner, tv.owner)) return false;
        if (!fog_) return tv.visible;
        return attackerOwner == 0 ? tv.visible : (tv.spottedMask & (1u << attackerOwner)) != 0;
    }
    if (targetIdx >= soldiers_.size()) return false;
    const auto& ts = soldiers_[targetIdx];
    if (ts.hp <= 0 || ts.embarked || !isHostile(attackerOwner, ts.owner)) return false;
    return !fog_ || (ts.spottedMask & (1u << attackerOwner)) != 0;
}

void TutorialUnits::bailoutPassengers(size_t vehicleIdx, std::vector<SoldierPose>& changed) {
    if (vehicleIdx >= vehicles_.size()) return;
    auto& tv = vehicles_[vehicleIdx];
    if (tv.passengers.empty()) return;
    for (size_t sIdx : tv.passengers) {
        if (sIdx >= soldiers_.size()) continue;
        auto& ps = soldiers_[sIdx];
        if (ps.hp <= 0) continue;
        const SubCell spot = findSpawnCell(grid_, tv.cell);
        ps.embarked = false;
        ps.cell = spot;
        ps.hp = std::max(1, (ps.hp * 60) / 100);
        ps.motion.reset();
        ps.goal.reset();
        ps.route.clear();
        ps.cursor = 0;
        ps.needsRoute = false;
        ps.boardVehicleTarget.reset();
        grid_.setOccupied(spot, true);
        const auto [pu, pv] = soldierWorld(ps);
        const bool pVis = !fog_ || ps.owner == 0 || (ps.spottedMask & 1u) != 0;
        pushOrUpdateSoldierPose(changed, {
            sIdx, originX_ + pu - pv, (pu + pv) / 2, ps.direction, 0, false,
            ps.hp, true, ps.owner, ps.group, false, pVis
        });
    }
    tv.passengers.clear();
}

void TutorialUnits::stepBoarding(std::vector<SoldierPose>& changed) {
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || !s.boardVehicleTarget) continue;
        const size_t vid = *s.boardVehicleTarget;
        if (!canBoardVehicle(vid, s.owner)) {
            s.boardVehicleTarget.reset();
            continue;
        }
        const auto& v = vehicles_[vid];
        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double dist = std::hypot(double(v.x - sx), double(v.y - sy));
        if (dist <= 56.0) {
            if (embarkImmediate(i, vid)) {
                pushOrUpdateSoldierPose(changed, {
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false
                });
            }
            continue;
        }
        if (!s.goal || !sameTile(*s.goal, v.cell)) {
            s.goal = v.cell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
    }
}

void TutorialUnits::stepCaptures(std::vector<SoldierPose>& changed) {
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || !s.captureVehicleTarget) continue;
        const size_t vid = *s.captureVehicleTarget;
        if (!canCaptureVehicle(vid, s.owner)) {
            s.captureVehicleTarget.reset();
            continue;
        }
        auto& v = vehicles_[vid];
        const auto [su, sv] = soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        if (std::hypot(double(v.x - sx), double(v.y - sy)) <= 56.0) {
            v.owner = s.owner;
            v.hasCrew = true;
            const bool needsQualifiedCrew = v.hasCrewProtection && v.attackCrew >= 2;
            if (s.canBeCrew > 0 || !needsQualifiedCrew) {
                v.crewHp = v.maxCrewHp;
                if (s.canBeCrew > 0 && s.expa > v.expa) {
                    v.expa = s.expa;
                }
            } else {
                const int skeletonCrew = std::max(1, v.attackCrew - 1);
                v.crewHp = std::max(1, (v.maxCrewHp * skeletonCrew) / std::max(1, v.crewNumber));
            }
            s.captureVehicleTarget.reset();
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            fogDirty_ = true;
            if (fog_) refreshSpotting(&changed, false);
            emitVehiclePose(vid, false, 0, 0, false, {}, false, false, false, false, true);
            continue;
        }
        if (!s.goal || !sameTile(*s.goal, v.cell)) {
            s.goal = v.cell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
    }
}

void TutorialUnits::triggerMineAtTile(int tx, int ty, std::vector<SoldierPose>& changed, bool forceAny) {
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    if (mines_.empty() || tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) return;
    const std::size_t idx = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx);
    const std::uint8_t m = mines_[idx];
    if ((m & 0x01u) == 0) return;
    const bool antiTank = (m & 0x04u) != 0;
    mines_[idx] = 0;
    const int wu = tx * 32 + 16, wv = ty * 32 + 16;
    const int mx = originX_ + wu - wv;
    const int my = (wu + wv) / 2;
    mineEvents_.push_back({MineEvent::Detonated, mx, my, antiTank});

    const auto mSpec = catalog_ ? catalog_->mineSpec() : assets::MineSpec{};
    if (antiTank || forceAny) {
        const double vehRadius = std::max(40.0, static_cast<double>(mSpec.radius) * 1.5);
        for (size_t j = 0; j < vehicles_.size(); ++j) {
            auto& tv = vehicles_[j];
            if (!tv.alive || tv.hp <= 0) continue;
            if (std::hypot(tv.fx - double(mx), tv.fy - double(my)) > vehRadius) continue;
            const int dmg = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             mx, my, 260, assets::HitType::Mine));
            tv.hp = std::max(0, tv.hp - dmg);
            if (dmg > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
                tv.expa = std::min(0x8000, tv.expa + std::min(200, dmg * tv.expaOnHit));
            }
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            if (tv.hp == 0) {
                destroyVehicle(j, changed);
            } else {
                applyVehicleCrewDamage(j, mx, my, 260, assets::HitType::Mine, changed);
                applyVehicleSuppression(j, 28, mx, my);
            }
            emitVehiclePose(j);
        }
    }
    const double infRadius = antiTank
        ? std::max(48.0, static_cast<double>(mSpec.radius) * 1.8)
        : std::max(52.0, static_cast<double>(mSpec.radius) * 2.0);
    const double baseInfDmg = antiTank
        ? 95.0
        : std::max(85.0, static_cast<double>(mSpec.damage) * 1.9);
    for (size_t k = 0; k < soldiers_.size(); ++k) {
        auto& ss = soldiers_[k];
        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding) continue;
        const auto [su, sv] = soldierWorld(ss);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double d = std::hypot(double(sx - mx), double(sy - my));
        if (d > infRadius) continue;
        const int sdmg = std::max(1, static_cast<int>(std::lround(baseInfDmg * (1.0 - d / (infRadius + 2.0)))));
        ss.hp = std::max(0, ss.hp - sdmg);
        recordHit(ss.owner, ss.group);
        if (ss.hp > 0) {
            applySuppression(k, antiTank ? 24 : 30);
        } else {
            if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
            ss.motion.reset();
            ss.goal.reset();
            ss.route.clear();
            ss.cursor = 0;
            ss.queuedGoals.clear();
            ss.attackMove = false;
            ss.attackTarget.reset();
            ss.attackVehicleTarget.reset();
            ss.attackBuildingTarget.reset();
            ss.autoTarget.reset();
            ss.binoculars = false;
            ss.spottedMask = 0;
            fogDirty_ = true;
        }
        const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
        SoldierPose sp{k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis};
        sp.suppressed = (ss.hp > 0 && ss.isSuppressed());
        pushOrUpdateSoldierPose(changed, sp);
    }
    if (antiTank || forceAny) {
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            const auto& tb = buildings_[bi];
            if (tb.destroyed) continue;
            const double bd = std::max(0.0, std::hypot(double(tb.x - mx), double(tb.y - my)) -
                                            std::min(tb.width, tb.height) * 0.3);
            if (bd <= 56.0) {
                damageBuilding(bi, 140, assets::HitType::Mine, -1, changed);
            }
        }
    }
    if (antiTank) {
        for (const auto [dx, dy] : std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
            const int ntx = tx + dx;
            const int nty = ty + dy;
            if (ntx < 0 || nty < 0 || ntx >= tileW || nty >= tileH) continue;
            const std::size_t nIdx = static_cast<std::size_t>(nty) * static_cast<std::size_t>(tileW) +
                                     static_cast<std::size_t>(ntx);
            if ((mines_[nIdx] & 0x01u) != 0) {
                triggerMineAtTile(ntx, nty, changed, true);
            }
        }
    }
}

void TutorialUnits::stepMines(std::vector<SoldierPose>& changed) {
    if (mines_.empty()) return;
    const int tileW = grid_.width() / 2;
    const int tileH = grid_.height() / 2;
    const auto mineOwner = [](std::uint8_t b) {
        return (b & 0x40u) ? 1 : ((b & 0x20u) ? 2 : 0);
    };
    const auto mineTileAt = [&](int tx, int ty) -> std::uint8_t* {
        if (tx < 0 || ty < 0 || tx >= tileW || ty >= tileH) return nullptr;
        const std::size_t idx = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tileW) + static_cast<std::size_t>(tx);
        return idx < mines_.size() ? &mines_[idx] : nullptr;
    };
    const auto tileCenter = [&](int tx, int ty) -> std::pair<int, int> {
        const int wu = tx * 32 + 16, wv = ty * 32 + 16;
        return {originX_ + wu - wv, (wu + wv) / 2};
    };

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        const auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0) continue;
        const int tx = v.cell.x / 2, ty = v.cell.y / 2;
        if (const auto* mp = mineTileAt(tx, ty);
            mp && ((*mp & 0x05u) == 0x05u) && isHostile(mineOwner(*mp), v.owner)) {
            triggerMineAtTile(tx, ty, changed, false);
        }
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
        const int tx = s.cell.x / 2, ty = s.cell.y / 2;
        if (const auto* mp = mineTileAt(tx, ty);
            mp && ((*mp & 0x05u) == 0x01u) && isHostile(mineOwner(*mp), s.owner) &&
            !(s.isSapper && (*mp & 0x80u) != 0)) {
            triggerMineAtTile(tx, ty, changed, false);
        }
    }

    const auto routeSapperToMine = [&](size_t sIdx, Soldier& s, int mtx, int mty) {
        const auto [su, sv] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sv);
        const double sy = static_cast<double>(su + sv) * 0.5;
        const auto [nmx, nmy] = tileCenter(mtx, mty);
        const auto ap = approachPointNear(sx, sy, static_cast<double>(nmx), static_cast<double>(nmy),
                                          40.0, mapPixelW_, mapPixelH_);
        if (!orderSoldierInternal(sIdx, ap.first, ap.second)) {
            (void)orderSoldierInternal(sIdx, nmx, nmy);
        }
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.disarmMineTarget = std::pair{mtx, mty};
        s.attackMove = false;
    };

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (!s.disarmMineTarget) continue;
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isSapper) {
            s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            continue;
        }
        const int tx = s.disarmMineTarget->first;
        const int ty = s.disarmMineTarget->second;
        const auto* mp = mineTileAt(tx, ty);
        const bool targetAlive = mp && ((*mp & 0x81u) == 0x81u) && isHostile(mineOwner(*mp), 0);
        if (!targetAlive) {
            const int cx = s.cell.x / 2;
            const int cy = s.cell.y / 2;
            std::optional<std::pair<int, int>> nextMine;
            int bestD2 = std::numeric_limits<int>::max();
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const int ntx = cx + dx;
                    const int nty = cy + dy;
                    const auto* cand = mineTileAt(ntx, nty);
                    if (!cand || ((*cand & 0x81u) != 0x81u) || !isHostile(mineOwner(*cand), 0)) continue;
                    const int d2 = dx * dx + dy * dy;
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        nextMine = std::pair{ntx, nty};
                    }
                }
            }
            if (nextMine) {
                s.disarmMineTarget = nextMine;
                const int td = std::max(std::abs(cx - nextMine->first), std::abs(cy - nextMine->second));
                if (td > 2 && !s.motion) {
                    routeSapperToMine(i, s, nextMine->first, nextMine->second);
                }
            } else {
                s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            }
            continue;
        }
        const int tileDist = std::max(std::abs(s.cell.x / 2 - tx), std::abs(s.cell.y / 2 - ty));
        if (tileDist <= 2) {
            if (!s.motion && s.goal) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                s.planning = {};
                s.needsRoute = false;
            }
        } else if (!s.motion && !s.goal && ((tick_ + i) % 6u == 0u)) {
            routeSapperToMine(i, s, tx, ty);
        }
    }

    if (tick_ % 15 == 0) {
        for (auto& s : soldiers_) {
            if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
            if (!s.isSapper && !s.isOfficer && !s.isMedic) continue;
            const int cx = s.cell.x / 2, cy = s.cell.y / 2;
            const bool canInstantDisarm = s.isSapper && s.getMineTime <= 0 &&
                ((!s.motion && s.route.empty()) || s.disarmMineTarget.has_value() || s.attackMove);
            bool disarmedOne = false;
            if (canInstantDisarm && s.disarmMineTarget) {
                const int tx = s.disarmMineTarget->first;
                const int ty = s.disarmMineTarget->second;
                if (std::abs(tx - cx) <= 2 && std::abs(ty - cy) <= 2) {
                    if (auto* tp = mineTileAt(tx, ty); tp && (*tp & 0x01u) != 0 && isHostile(mineOwner(*tp), 0)) {
                        const auto [mx, my] = tileCenter(tx, ty);
                        const bool antiTank = (*tp & 0x04u) != 0;
                        *tp = 0;
                        mineEvents_.push_back({MineEvent::Disarmed, mx, my, antiTank});
                        disarmedOne = true;
                    }
                }
            }
            for (int dy = -3; dy <= 3; ++dy) {
                for (int dx = -3; dx <= 3; ++dx) {
                    auto* mp = mineTileAt(cx + dx, cy + dy);
                    if (!mp || (*mp & 0x01u) == 0 || !isHostile(mineOwner(*mp), 0)) continue;
                    const auto [mx, my] = tileCenter(cx + dx, cy + dy);
                    if ((*mp & 0x80u) == 0) {
                        *mp |= 0x80u;
                        mineEvents_.push_back({MineEvent::Detected, mx, my, (*mp & 0x04u) != 0});
                    }
                    if (!disarmedOne && canInstantDisarm && std::abs(dx) <= 2 && std::abs(dy) <= 2) {
                        const bool antiTank = (*mp & 0x04u) != 0;
                        *mp = 0;
                        mineEvents_.push_back({MineEvent::Disarmed, mx, my, antiTank});
                        disarmedOne = true;
                    }
                }
            }
        }
    }

    for (auto& s : soldiers_) {
        if (!s.isSapper || s.getMineTime <= 0) continue;
        if (s.owner != 0 || s.hp <= 0 || s.embarked || s.garrisonBuilding) {
            s.disarmWorkTicks = 0;
            continue;
        }
        const bool canDisarmNow = (!s.motion && s.route.empty()) || s.disarmMineTarget.has_value() || s.attackMove;
        if (!canDisarmNow) {
            s.disarmWorkTicks = 0;
            continue;
        }
        const int cx = s.cell.x / 2, cy = s.cell.y / 2;
        std::optional<std::pair<int, int>> targetTile;
        if (s.disarmMineTarget) {
            const int tx = s.disarmMineTarget->first;
            const int ty = s.disarmMineTarget->second;
            if (std::abs(tx - cx) <= 2 && std::abs(ty - cy) <= 2) {
                if (const auto* tp = mineTileAt(tx, ty); tp && (*tp & 0x01u) != 0 && isHostile(mineOwner(*tp), 0)) {
                    targetTile = std::pair{tx, ty};
                }
            }
        }
        if (!targetTile) {
            int bestD2 = std::numeric_limits<int>::max();
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const auto* mp = mineTileAt(cx + dx, cy + dy);
                    if (!mp || (*mp & 0x01u) == 0 || !isHostile(mineOwner(*mp), 0)) continue;
                    const int d2 = dx * dx + dy * dy;
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        targetTile = std::pair{cx + dx, cy + dy};
                    }
                }
            }
        }
        if (!targetTile) {
            s.disarmWorkTicks = 0;
            continue;
        }
        const double vetFrac = static_cast<double>(std::clamp(s.expa, 0, 0x8000)) / 32768.0;
        const double effTime = static_cast<double>(s.getMineTime) -
                               vetFrac * static_cast<double>(std::max(0, s.getMineTime - s.getMineTimeMax));
        const int needed = std::clamp(static_cast<int>(std::lround(effTime / 10.0)), 2, 10);
        ++s.disarmWorkTicks;
        if (s.disarmWorkTicks >= needed) {
            if (auto* mp = mineTileAt(targetTile->first, targetTile->second)) {
                const auto [mx, my] = tileCenter(targetTile->first, targetTile->second);
                const bool antiTank = (*mp & 0x04u) != 0;
                if ((*mp & 0x80u) == 0) {
                    mineEvents_.push_back({MineEvent::Detected, mx, my, antiTank});
                }
                *mp = 0;
                mineEvents_.push_back({MineEvent::Disarmed, mx, my, antiTank});
            }
            s.disarmWorkTicks = 0;
        }
    }
}

void TutorialUnits::detonateKamikazeSoldier(size_t index, int impactX, int impactY,
                                            std::optional<size_t> directVehIdx,
                                            std::optional<size_t> directBldIdx,
                                            std::vector<SoldierPose>& changed) {
    if (index >= soldiers_.size()) return;
    auto& s = soldiers_[index];
    if (s.hp <= 0) return;
    const auto [su, sv] = soldierWorld(s);
    const int sx = originX_ + su - sv;
    const int sy = (su + sv) / 2;
    const int blastDmg = std::max(300, scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa));
    if (directVehIdx && *directVehIdx < vehicles_.size()) {
        auto& tv = vehicles_[*directVehIdx];
        if (tv.alive && tv.hp > 0 && isHostile(s.owner, tv.owner)) {
            const int dmg = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             sx, sy, blastDmg, assets::HitType::Explosive));
            const bool wasAlive = tv.alive && tv.hp > 0;
            tv.hp = std::max(0, tv.hp - dmg);
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            reactToAttack(tv.owner, tv.group, sx, sy, *directVehIdx);
            if (wasAlive && tv.hp == 0) {
                destroyVehicle(*directVehIdx, changed);
                ++s.kills;
                s.expa = std::min(0x8000, s.expa + 4096);
            } else if (tv.hp > 0) {
                applyVehicleCrewDamage(*directVehIdx, sx, sy, blastDmg, assets::HitType::Explosive,
                                       changed, static_cast<int>(index), -1);
            }
            emitVehiclePose(*directVehIdx);
        }
    }
    if (directBldIdx && *directBldIdx < buildings_.size() && !buildings_[*directBldIdx].destroyed) {
        damageBuilding(*directBldIdx, blastDmg, assets::HitType::Explosive, s.owner, changed);
    }
    applyMortarSplash(impactX, impactY, blastDmg, s.owner, static_cast<int>(index),
                      static_cast<int>(index), changed,
                      directVehIdx ? static_cast<int>(*directVehIdx) : -1);
    if (!mines_.empty()) {
        const SubCell impCell = pointerCell(impactX, impactY, originX_);
        triggerMineAtTile(impCell.x / 2, impCell.y / 2, changed, true);
    }
    if (!vehicleAtCell(s.cell)) grid_.setOccupied(s.cell, false);
    s.hp = 0;
    s.ammo = 0;
    s.motion.reset();
    s.goal.reset();
    s.route.clear();
    s.cursor = 0;
    s.needsRoute = false;
    s.queuedGoals.clear();
    s.patrolOrigin.reset();
    s.patrolDest.reset();
    s.attackMove = false;
    s.boardVehicleTarget.reset();
    s.captureVehicleTarget.reset();
    s.garrisonTarget.reset();
    s.attackTarget.reset();
    s.attackVehicleTarget.reset();
    s.attackBuildingTarget.reset();
    s.attackGround.reset();
    s.autoTarget.reset();
    s.healTarget.reset();
    s.escortVehicleTarget.reset();
    s.escortSoldierTarget.reset();
    s.disarmMineTarget.reset();
    s.binoculars = false;
    s.spottedMask = 0;
    fogDirty_ = true;
    const bool vis = !s.garrisonBuilding && (!fog_ || isAlliedWithPlayer(s.owner) || (s.spottedMask & 1u) != 0);
    pushOrUpdateSoldierPose(changed, {
        index, sx, sy, s.direction, 0, false, 0, false, s.owner, s.group,
        true, vis, impactX, impactY, false, s.expa, s.kills, false,
        s.atGrenades, false, false, s.mineAmmo, 0, true
    });
}

void TutorialUnits::damageTankDesantRiders(size_t vehicleIdx, int rawDamage, assets::HitType hitType,
                                           std::vector<SoldierPose>& changed,
                                           int attackerSoldierIdx, int attackerVehicleIdx) {
    if (vehicleIdx >= vehicles_.size() || rawDamage <= 0) return;
    auto& tv = vehicles_[vehicleIdx];
    if (!tv.alive || tv.hp <= 0 || !tv.soldiersOnArmor || tv.passengers.empty()) return;

    const int riderProt = catalog_
        ? catalog_->tankArmorProtection(hitType)
        : ((hitType == assets::HitType::Pierce || hitType == assets::HitType::Transpierce ||
            hitType == assets::HitType::Explosive || hitType == assets::HitType::Mine) ? 3 : 0);
    if (rawDamage <= riderProt) return;

    const bool splashAll = (hitType == assets::HitType::Explosive ||
                            hitType == assets::HitType::Fire ||
                            hitType == assets::HitType::Mine);
    int riderDmg = 0;
    if (splashAll) {
        riderDmg = std::clamp(rawDamage / 4, 12, 65);
    } else if (hitType == assets::HitType::Machine || hitType == assets::HitType::Sniper) {
        riderDmg = std::clamp(rawDamage / 2, 10, 45);
    } else {
        riderDmg = std::clamp(rawDamage / 8, 6, 25);
    }
    riderDmg = std::max(1, riderDmg - riderProt);

    std::vector<size_t> remaining;
    remaining.reserve(tv.passengers.size());
    bool anyKilled = false;
    const size_t nPass = tv.passengers.size();
    for (size_t idx = 0; idx < nPass; ++idx) {
        const size_t pIdx = tv.passengers[idx];
        const bool targeted = splashAll || (idx == nPass - 1);
        if (!targeted || pIdx >= soldiers_.size() || soldiers_[pIdx].hp <= 0) {
            remaining.push_back(pIdx);
            continue;
        }
        auto& ps = soldiers_[pIdx];
        ps.hp = std::max(0, ps.hp - riderDmg);
        recordHit(ps.owner, ps.group);
        if (ps.hp > 0) {
            const int addSup = splashAll ? 25 : 18;
            const int effSup = ps.moraleResist >= 5000
                ? std::max(1, addSup / 4)
                : (ps.moraleResist >= 1000 ? std::max(1, addSup / 2) : addSup);
            ps.suppressionTicks = std::min(35, ps.suppressionTicks + effSup);
            remaining.push_back(pIdx);
            continue;
        }
        ps.embarked = false;
        ps.cell = tv.cell;
        ps.motion.reset();
        ps.goal.reset();
        ps.route.clear();
        ps.cursor = 0;
        ps.needsRoute = false;
        ps.queuedGoals.clear();
        ps.patrolOrigin.reset();
        ps.patrolDest.reset();
        ps.attackMove = false;
        ps.boardVehicleTarget.reset();
        ps.captureVehicleTarget.reset();
        ps.garrisonTarget.reset();
        ps.attackTarget.reset();
        ps.attackVehicleTarget.reset();
        ps.attackBuildingTarget.reset();
        ps.attackGround.reset();
        ps.autoTarget.reset();
        ps.healTarget.reset();
        ps.escortVehicleTarget.reset();
        ps.escortSoldierTarget.reset();
        ps.disarmMineTarget.reset();
        ps.binoculars = false;
        ps.suppressionTicks = 0;
        ps.spottedMask = 0;
        fogDirty_ = true;
        if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size() &&
            isHostile(soldiers_[static_cast<size_t>(attackerSoldierIdx)].owner, ps.owner)) {
            auto& attS = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
            ++attS.kills;
            attS.expa = std::min(0x8000, attS.expa + 1500);
        } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size() &&
                   isHostile(vehicles_[static_cast<size_t>(attackerVehicleIdx)].owner, ps.owner)) {
            auto& attV = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
            ++attV.kills;
            attV.expa = std::min(0x8000, attV.expa + 1500);
        }
        const bool vis = !fog_ || isAlliedWithPlayer(ps.owner) || tv.visible;
        pushOrUpdateSoldierPose(changed, {
            pIdx, tv.x, tv.y, ps.direction, 0, false, 0, false, ps.owner, ps.group, false, vis
        });
        anyKilled = true;
    }
    tv.passengers = std::move(remaining);
    if (anyKilled) {
        emitVehiclePose(vehicleIdx);
    }
}

void TutorialUnits::applyVehicleCrewDamage(size_t vehicleIdx, int attackerX, int attackerY,
                                           int rawDamage, assets::HitType hitType,
                                           std::vector<SoldierPose>& changed,
                                           int attackerSoldierIdx, int attackerVehicleIdx) {
    if (vehicleIdx >= vehicles_.size() || rawDamage <= 0) return;
    auto& tv = vehicles_[vehicleIdx];
    if (!tv.alive || tv.hp <= 0 || tv.owner == 3) return;
    damageTankDesantRiders(vehicleIdx, rawDamage, hitType, changed, attackerSoldierIdx, attackerVehicleIdx);
    const bool unbuttonedCommander = (hitType == assets::HitType::Sniper) && isVehicleUnbuttoned(tv);
    if (!tv.hasCrew || (!tv.hasCrewProtection && !unbuttonedCommander)) return;
    int rawCrewDmg = tv.hasCrewProtection
        ? applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.crewProtection, true,
                             attackerX, attackerY, rawDamage, hitType)
        : 0;
    if (rawCrewDmg <= 0 && unbuttonedCommander) {
        rawCrewDmg = std::max(1, rawDamage / 2);
    }
    if (rawCrewDmg <= 0) return;

    int fracDmg = rawCrewDmg;
    if (hitType == assets::HitType::Explosive) {
        fracDmg = std::max(1, rawCrewDmg / 2);
    } else if (hitType == assets::HitType::Pierce ||
               hitType == assets::HitType::Transpierce ||
               hitType == assets::HitType::Mine) {
        fracDmg = std::max(1, rawCrewDmg / 4);
    }
    if (tv.crewVulnerability > 0.0) {
        const double mult = std::clamp(tv.crewVulnerability, 0.10, 2.0);
        fracDmg = std::max(1, static_cast<int>(std::lround(static_cast<double>(fracDmg) * mult)));
    }
    const int scaled = scaleVehicleDamageForDifficulty(tv.owner, fracDmg);
    if (scaled <= 0) return;
    tv.crewHp = std::max(0, tv.crewHp - scaled);
    if (tv.crewHp > 0) {
        applyVehicleSuppression(vehicleIdx,
                                (hitType == assets::HitType::Explosive || hitType == assets::HitType::Fire ||
                                 hitType == assets::HitType::Mine)
                                    ? 24 : 16,
                                attackerX, attackerY);
        return;
    }

    if (!tv.passengers.empty()) {
        bailoutPassengers(vehicleIdx, changed);
    }
    if (tv.towedVehicle) detachTowedGun(vehicleIdx);
    else if (tv.towedBy) detachTowedGun(*tv.towedBy);

    tv.hasCrew = false;
    tv.suppressionTicks = 0;
    tv.wasSuppressed = false;
    tv.owner = 3;
    tv.moveTarget.reset();
    tv.waypoints.clear();
    tv.queuedOrders.clear();
    tv.patrolOrigin.reset();
    tv.patrolDest.reset();
    tv.patrolToDest = true;
    tv.attackTarget.reset();
    tv.attackGround.reset();
    tv.attackBuildingTarget.reset();
    tv.pendingTowGun.reset();
    tv.chasingAttackTarget = false;
    tv.attackMove = false;
    tv.attackMoveEngaging = false;
    tv.reverseMove = false;
    tv.facingTargetDir.reset();
    tv.supportTargetVehicle.reset();
    tv.supportTargetBuilding.reset();
    tv.supportTargetSoldier.reset();
    tv.escortVehicleTarget.reset();
    tv.escortSoldierTarget.reset();
    tv.wasMoving = false;
    tv.fireCooldown = 0;
    fogDirty_ = true;

    for (auto& ss : soldiers_) {
        if (ss.attackVehicleTarget && *ss.attackVehicleTarget == vehicleIdx) {
            ss.attackVehicleTarget.reset();
        }
    }
    for (auto& ov : vehicles_) {
        if (ov.attackTarget && ov.attackTarget->first && ov.attackTarget->second == vehicleIdx) {
            ov.attackTarget.reset();
        }
    }
    if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
        auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
        ++att.kills;
        att.expa = std::min(0x8000, att.expa + 3072);
    } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size()) {
        auto& av = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
        ++av.kills;
        av.expa = std::min(0x8000, av.expa + 3500);
    }
    emitVehiclePose(vehicleIdx, false, 0, 0, false, {}, false, false, false, false, false,
                    false, false, true);
}

void TutorialUnits::stepSoldierAttackVehicle(Soldier& s, size_t index, std::vector<SoldierPose>& changed) {
    constexpr double kAtGrenadeRangePx = 88.0;
    const bool canGrenade = !s.isAntiVehicle && !s.isKamikaze && !s.isSniper && !s.isFlamer && s.atGrenades > 0;
    if (s.ammo <= 0 && !canGrenade) {
        s.attackVehicleTarget.reset();
        return;
    }
    std::optional<size_t> targetVid;
    bool explicitOrder = false;
    const bool isGar = s.garrisonBuilding.has_value() &&
                       *s.garrisonBuilding < buildings_.size() &&
                       !buildings_[*s.garrisonBuilding].destroyed;
    int sx = 0;
    int sy = 0;
    int su = 0;
    int sv = 0;
    if (isGar) {
        const auto& gb = buildings_[*s.garrisonBuilding];
        sx = gb.x;
        sy = gb.y;
        const int duv = sx - originX_;
        const int suv = 2 * sy;
        su = (suv + duv) / 2;
        sv = (suv - duv) / 2;
    } else {
        std::tie(su, sv) = soldierWorld(s);
        sx = originX_ + su - sv;
        sy = (su + sv) / 2;
    }

    const double sniperReach = std::max(
        kInfantryVsVehicleRangePx,
        static_cast<double>(scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa)) + (isGar ? 48.0 : 0.0));
    const double atGrenadeReach = kAtGrenadeRangePx + (isGar ? 24.0 : 0.0);
    if (s.attackVehicleTarget) {
        const size_t vid = *s.attackVehicleTarget;
        if (!isVehicleTargetValidFor(s.owner, true, vid)) {
            s.attackVehicleTarget.reset();
            return;
        }
        targetVid = vid;
        explicitOrder = true;
    } else if (fog_ && (autoEngage_ || s.attackMove) && !s.holdFire &&
               (s.isAntiVehicle || s.isKamikaze || s.isSniper || canGrenade) && !vehicles_.empty()) {
        const double autoRange = s.isSniper
            ? sniperReach
            : ((s.isAntiVehicle || s.isKamikaze) ? kInfantryVsVehicleRangePx : atGrenadeReach);
        const int sniperRaw = std::max(85, scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa));
        int bestTier = 99;
        double bestDist = autoRange + 1.0;
        for (size_t j = 0; j < vehicles_.size(); ++j) {
            if (!isVehicleTargetValidFor(s.owner, true, j)) continue;
            const auto& tv = vehicles_[j];
            const double d = std::hypot(double(tv.x - sx), double(tv.y - sy));
            if (d > autoRange) continue;
            if (s.isSniper && !isVehicleUnbuttoned(tv) &&
                (!tv.hasCrewProtection || !tv.hasCrew ||
                 applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.crewProtection, true,
                                    sx, sy, sniperRaw, assets::HitType::Sniper) <= 0)) {
                continue;
            }
            int tier = 0;
            if (s.hasAttackPref) {
                const int cls = assets::UnitSpec::targetClassIndex(tv.effectiveTargetType());
                const int pref = (cls >= 0) ? s.attackPref[static_cast<std::size_t>(cls)] : 0;
                if (pref == 1) continue;
                tier = (pref >= 2) ? (256 - pref) : 300;
            } else {
                const bool targetArmed = tv.armed && (tv.ammo > 0 || tv.ammo2 > 0);
                tier = targetArmed ? 0 : 1;
            }
            if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                bestTier = tier;
                bestDist = d;
                targetVid = j;
            }
        }
    }
    if (!targetVid) return;

    auto& tv = vehicles_[*targetVid];
    const double distMap = std::hypot(double(tv.x - sx), double(tv.y - sy));
    const double engageRange = s.isKamikaze ? 56.0 : (s.isSniper ? sniperReach : (canGrenade ? atGrenadeReach : kInfantryVsVehicleRangePx));
    if ((explicitOrder || s.isKamikaze) && distMap > engageRange) {
        if (!isGar && (!s.goal || !sameTile(*s.goal, tv.cell))) {
            s.goal = tv.cell;
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
        }
        return;
    }

    if (!isGar && (explicitOrder || s.isKamikaze)) {
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
    }

    if (!s.motion && distMap <= engageRange && s.attackReload <= 0) {
        if (s.isKamikaze) {
            const auto [vu, vv] = tutorialFootWorld(tv.x, tv.y, originX_);
            s.direction = directionFromDelta(vu - su, vv - sv);
            detonateKamikazeSoldier(index, tv.x, tv.y, *targetVid, std::nullopt, changed);
            return;
        }
        const bool throwGrenade = canGrenade && distMap <= atGrenadeReach;
        if (!throwGrenade && s.ammo <= 0) return;
        if (!throwGrenade && !isGar && s.crouchToFire && !s.crouched) {
            const auto [vu, vv] = tutorialFootWorld(tv.x, tv.y, originX_);
            s.crouched = true;
            s.direction = directionFromDelta(vu - su, vv - sv);
            s.attackReload = 4;
            fogDirty_ = true;
            const bool vis = s.owner == 0 || (s.spottedMask & 1u) != 0;
            pushOrUpdateSoldierPose(changed, {
                index, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, false, vis,
                0, 0, false, s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s),
                s.mineAmmo, s.ammo, false, s.isSuppressed(), std::int8_t{1}
            });
            return;
        }

        const auto [vu, vv] = tutorialFootWorld(tv.x, tv.y, originX_);
        s.direction = directionFromDelta(vu - su, vv - sv);
        s.firing = true;
        s.fireAnim = 0;
        s.lastFireTick = tick_;
        if (s.isSniper) fogDirty_ = true;

        const int rawDmg = throwGrenade
            ? 260
            : (tv.hasCatalogArmor
                   ? (s.isFlamer ? 140
                                 : (s.isAntiVehicle ? 420
                                                    : (s.isSniper ? std::max(85, scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa)) : 30)))
                   : (s.isAntiVehicle ? 35 : (s.isSniper ? 15 : 5)));
        const auto hitType = throwGrenade
            ? assets::HitType::Explosive
            : (s.isFlamer ? assets::HitType::Fire
                          : (s.isAntiVehicle ? assets::HitType::Pierce
                                             : (s.isSniper ? assets::HitType::Sniper : assets::HitType::Machine)));
        const int dmg = scaleVehicleDamageForDifficulty(
            tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor, sx, sy, rawDmg, hitType));
        const bool targetWasAlive = tv.alive && tv.hp > 0;
        tv.hp = std::max(0, tv.hp - dmg);
        if (dmg > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
            tv.expa = std::min(0x8000, tv.expa + std::min(200, dmg * tv.expaOnHit));
        }
        recordHit(tv.owner, tv.group);
        if (targetWasAlive && tv.hp == 0) {
            destroyVehicle(*targetVid, changed);
            ++s.kills;
            s.expa = std::min(0x8000, s.expa + 4096);
        } else if (tv.hp > 0) {
            applyVehicleCrewDamage(*targetVid, sx, sy, rawDmg, hitType, changed, static_cast<int>(index), -1);
        }
        markVehicleCombat(tv);
        reactToAttack(tv.owner, tv.group, sx, sy, *targetVid);
        emitVehiclePose(*targetVid);
        if (!throwGrenade && s.isFlamer) {
            applyFlameSplash(tv.x, tv.y, rawDmg, s.owner, static_cast<int>(index), -1, -1, changed,
                             static_cast<int>(*targetVid));
        }

        if (throwGrenade) {
            --s.atGrenades;
        } else {
            --s.ammo;
        }
        applySoldierShotReload(s);
        const bool vis = !isGar && (!fog_ || s.owner == 0 || (s.spottedMask & 1u) != 0);
        pushOrUpdateSoldierPose(changed, {
            index, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, true, vis, tv.x, tv.y,
            isGar, s.expa, s.kills, throwGrenade, s.atGrenades, !throwGrenade && s.isFlamer,
            isSoldierCamouflaged(s), s.mineAmmo, s.ammo, !throwGrenade && s.isAntiVehicle && !s.isFlamer
        });
    }
}

void TutorialUnits::applyFlameSplash(int impactX, int impactY, int rawDamage, int attackerOwner,
                                     int attackerSoldierIdx, int attackerVehicleIdx, int skipSoldierIdx,
                                     std::vector<SoldierPose>& changed, int skipVehicleIdx) {
    constexpr double kFlameSplashR = 44.0;
    for (size_t k = 0; k < soldiers_.size(); ++k) {
        if (skipSoldierIdx >= 0 && k == static_cast<size_t>(skipSoldierIdx)) continue;
        auto& ss = soldiers_[k];
        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(attackerOwner, ss.owner)) continue;
        const auto [su, sv] = soldierWorld(ss);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double sd = std::hypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
        if (sd > kFlameSplashR) continue;
        const int splashDmg = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(rawDamage) * 0.45 * (1.0 - sd / 45.0))));
        ss.hp = std::max(0, ss.hp - splashDmg);
        recordHit(ss.owner, ss.group);
        reactToAttack(ss.owner, ss.group, impactX, impactY);
        if (ss.hp == 0) {
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 2048);
            } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size()) {
                auto& av = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
                ++av.kills;
                av.expa = std::min(0x8000, av.expa + 1500);
            }
            if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
            ss.motion.reset();
            ss.goal.reset();
            ss.route.clear();
            ss.cursor = 0;
            ss.queuedGoals.clear();
            ss.attackMove = false;
            ss.attackTarget.reset();
            ss.attackVehicleTarget.reset();
            ss.attackBuildingTarget.reset();
            ss.autoTarget.reset();
            ss.binoculars = false;
            ss.spottedMask = 0;
            fogDirty_ = true;
        }
        const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
        pushOrUpdateSoldierPose(changed, {
            k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis
        });
    }
    if (skipVehicleIdx == -2) return;
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        if (skipVehicleIdx >= 0 && vi == static_cast<size_t>(skipVehicleIdx)) continue;
        auto& tv = vehicles_[vi];
        if (!tv.alive || tv.hp <= 0 || !isHostile(attackerOwner, tv.owner)) continue;
        const double vd = std::hypot(tv.fx - static_cast<double>(impactX), tv.fy - static_cast<double>(impactY));
        if (vd > kFlameSplashR) continue;
        const int rawSplash = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(rawDamage) * 0.45 * (1.0 - vd / 45.0))));
        const int dmg = scaleVehicleDamageForDifficulty(
            tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                           impactX, impactY, rawSplash, assets::HitType::Fire));
        const int prevCrewHp = tv.crewHp;
        const int prevOwner = tv.owner;
        const bool wasAlive = tv.alive && tv.hp > 0;
        if (dmg > 0) {
            tv.hp = std::max(0, tv.hp - dmg);
        }
        if (wasAlive && tv.hp == 0) {
            destroyVehicle(vi, changed);
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 4096);
            } else if (attackerVehicleIdx >= 0 && static_cast<size_t>(attackerVehicleIdx) < vehicles_.size()) {
                auto& av = vehicles_[static_cast<size_t>(attackerVehicleIdx)];
                ++av.kills;
                av.expa = std::min(0x8000, av.expa + 4500);
            }
        } else if (tv.hp > 0) {
            applyVehicleCrewDamage(vi, impactX, impactY, rawSplash, assets::HitType::Fire,
                                   changed, attackerSoldierIdx, attackerVehicleIdx);
        }
        if (dmg > 0 || tv.crewHp != prevCrewHp) {
            recordHit(prevOwner, tv.group);
            markVehicleCombat(tv);
            if (tv.hasCrew) {
                reactToAttack(prevOwner, tv.group, impactX, impactY, vi);
            }
            emitVehiclePose(vi);
        }
    }
}

void TutorialUnits::applyMortarSplash(int impactX, int impactY, int rawDamage, int attackerOwner,
                                      int attackerSoldierIdx, int skipSoldierIdx,
                                      std::vector<SoldierPose>& changed, int skipVehicleIdx) {
    constexpr double kMortarSplashR = 52.0;
    for (size_t k = 0; k < soldiers_.size(); ++k) {
        if (skipSoldierIdx >= 0 && k == static_cast<size_t>(skipSoldierIdx)) continue;
        auto& ss = soldiers_[k];
        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(attackerOwner, ss.owner)) continue;
        const auto [su, sv] = soldierWorld(ss);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const double sd = std::hypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
        if (sd > kMortarSplashR) continue;
        const double falloff = 1.0 - 0.55 * (sd / kMortarSplashR);
        const int splashDmg = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(rawDamage) * 0.65 * falloff)));
        ss.hp = std::max(0, ss.hp - splashDmg);
        if (ss.hp > 0) applySuppression(k, 25);
        recordHit(ss.owner, ss.group);
        reactToAttack(ss.owner, ss.group, impactX, impactY);
        if (ss.hp == 0) {
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 2048);
            }
            if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
            ss.motion.reset();
            ss.goal.reset();
            ss.route.clear();
            ss.cursor = 0;
            ss.queuedGoals.clear();
            ss.attackMove = false;
            ss.attackTarget.reset();
            ss.attackVehicleTarget.reset();
            ss.attackBuildingTarget.reset();
            ss.autoTarget.reset();
            ss.binoculars = false;
            ss.spottedMask = 0;
            fogDirty_ = true;
        }
        const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
        pushOrUpdateSoldierPose(changed, {
            k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis
        });
    }
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        if (skipVehicleIdx >= 0 && vi == static_cast<size_t>(skipVehicleIdx)) continue;
        auto& tv = vehicles_[vi];
        if (!tv.alive || tv.hp <= 0 || !isHostile(attackerOwner, tv.owner)) continue;
        const double vd = std::hypot(tv.fx - static_cast<double>(impactX), tv.fy - static_cast<double>(impactY));
        if (vd > kMortarSplashR) continue;
        const double falloff = 1.0 - 0.55 * (vd / kMortarSplashR);
        const int rawSplash = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(rawDamage) * 0.65 * falloff)));
        const int dmg = scaleVehicleDamageForDifficulty(
            tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                           impactX, impactY, rawSplash, assets::HitType::Explosive));
        const int prevCrewHp = tv.crewHp;
        const int prevOwner = tv.owner;
        const bool wasAlive = tv.alive && tv.hp > 0;
        if (dmg > 0) {
            tv.hp = std::max(0, tv.hp - dmg);
        }
        if (wasAlive && tv.hp == 0) {
            destroyVehicle(vi, changed);
            if (attackerSoldierIdx >= 0 && static_cast<size_t>(attackerSoldierIdx) < soldiers_.size()) {
                auto& att = soldiers_[static_cast<size_t>(attackerSoldierIdx)];
                ++att.kills;
                att.expa = std::min(0x8000, att.expa + 4096);
            }
        } else if (tv.hp > 0) {
            applyVehicleCrewDamage(vi, impactX, impactY, rawSplash, assets::HitType::Explosive,
                                   changed, attackerSoldierIdx, -1);
        }
        if (dmg > 0 || tv.crewHp != prevCrewHp) {
            recordHit(prevOwner, tv.group);
            markVehicleCombat(tv);
            if (tv.hasCrew) {
                reactToAttack(prevOwner, tv.group, impactX, impactY, vi);
            }
            emitVehiclePose(vi);
        }
    }
}

void TutorialUnits::stepCombat(std::vector<SoldierPose>& changed) {
    if (!fog_) return;
    if (fogDirty_) refreshSpotting(&changed);

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked) continue;
        if (s.attackReload > 0) --s.attackReload;
        if (s.firing) {
            if (s.fireAnim == 255) {
                s.firing = false;
                s.fireAnim = 0;
            } else {
                s.fireAnim = static_cast<std::uint8_t>(std::min(255, int(s.fireAnim) + 15));
            }
        }

        const bool isGar = s.garrisonBuilding.has_value();
        if (isGar && !s.canFireFromInside) {
            s.attackTarget.reset();
            s.attackVehicleTarget.reset();
            s.attackBuildingTarget.reset();
            s.autoTarget.reset();
            continue;
        }
        const int effX = isGar ? buildings_[*s.garrisonBuilding].x : 0;
        const int effY = isGar ? buildings_[*s.garrisonBuilding].y : 0;
        const auto [su, sv] = isGar ? tutorialFootWorld(effX, effY, originX_) : soldierWorld(s);
        const int sx = originX_ + su - sv;
        const int sy = (su + sv) / 2;
        const int garRangeBonus = catalog_
            ? std::clamp(catalog_->buildingDefense().bonusShotRange / 3, 32, 64)
            : 48;
        const int maxRange = scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa) + (isGar ? garRangeBonus : 0);
        const int baseDeadzone = scaleByExpa(s.desc.deadzoneBase, s.desc.deadzoneDelta, s.expa);
        const int garDeadzoneBonus = (isGar && baseDeadzone >= 40)
            ? std::clamp((catalog_ ? catalog_->buildingDefense().bonusShotDeadZone : 150) / 5, 16, 48)
            : 0;
        const int minRange = isGar ? (baseDeadzone >= 40 ? baseDeadzone + garDeadzoneBonus : 0) : baseDeadzone;

        // Explicit attack order takes precedence over transient auto-engage.
        std::optional<size_t> activeTarget;
        bool explicitOrder = false;
        if (s.attackTarget) {
            const size_t tid = *s.attackTarget;
            if (tid >= soldiers_.size() || !isValidAttackTarget(tid, s.owner) || s.ammo <= 0) {
                s.attackTarget.reset();
            } else {
                activeTarget = tid;
                explicitOrder = true;
                s.autoTarget.reset();
            }
        }
        if (!activeTarget && s.attackVehicleTarget) {
            s.autoTarget.reset();
            stepSoldierAttackVehicle(s, i, changed);
            continue;
        }
        std::optional<size_t> activeBldTarget;
        bool explicitBldOrder = false;
        if (!activeTarget && s.attackBuildingTarget) {
            const size_t bid = *s.attackBuildingTarget;
            if (bid >= buildings_.size() || buildings_[bid].destroyed ||
                buildings_[bid].owner < 0 || !isHostile(s.owner, buildings_[bid].owner) || s.ammo <= 0) {
                s.attackBuildingTarget.reset();
            } else {
                activeBldTarget = bid;
                explicitBldOrder = true;
            }
        }
        if (!activeTarget && !activeBldTarget && !s.attackGround && (autoEngage_ || s.attackMove) && !s.holdFire &&
            (s.isAntiVehicle || s.isKamikaze) && s.ammo > 0 && !vehicles_.empty()) {
            bool vehicleInReach = false;
            int maxVehPref = -1;
            for (size_t j = 0; j < vehicles_.size(); ++j) {
                if (!isVehicleTargetValidFor(s.owner, true, j)) continue;
                const double d = std::hypot(double(vehicles_[j].x - sx), double(vehicles_[j].y - sy));
                if (d <= kInfantryVsVehicleRangePx) {
                    if (s.hasAttackPref) {
                        const int cls = assets::UnitSpec::targetClassIndex(vehicles_[j].effectiveTargetType());
                        const int pref = (cls >= 0) ? s.attackPref[static_cast<std::size_t>(cls)] : 0;
                        if (pref == 1) continue;
                        maxVehPref = std::max(maxVehPref, pref);
                    }
                    vehicleInReach = true;
                }
            }
            if (vehicleInReach && s.hasAttackPref) {
                const int manPref = s.attackPref[static_cast<std::size_t>(assets::TargetClass::Man)];
                if (manPref >= 2 && manPref > maxVehPref) {
                    const double manLimit = double((maxRange * 112) / 100);
                    for (size_t j = 0; j < soldiers_.size(); ++j) {
                        if (j == i || !isValidAttackTarget(j, s.owner)) continue;
                        const auto [tu, tv] = soldierWorld(soldiers_[j]);
                        const double d = std::hypot(double(tu - su), double(tv - sv));
                        if (d >= double(minRange) && d <= manLimit) {
                            vehicleInReach = false;
                            break;
                        }
                    }
                }
            }
            if (vehicleInReach) {
                s.autoTarget.reset();
                stepSoldierAttackVehicle(s, i, changed);
                continue;
            }
        }
        if (!activeTarget && !activeBldTarget && !isGar && s.attackGround &&
            (((s.isMortar || s.isFlamer) && s.ammo > 0) || (s.canGrenadeGround && s.atGrenades > 0))) {
            const bool mortarOrFlamer = (s.isMortar || s.isFlamer);
            const int groundMaxR = mortarOrFlamer ? maxRange : 140;
            const int groundMinR = mortarOrFlamer ? minRange : 0;
            const auto [gx, gy] = *s.attackGround;
            const auto [gu, gv] = tutorialFootWorld(gx, gy, originX_);
            const double dist = std::hypot(double(gx - sx), double(gy - sy));
            if (dist > double(groundMaxR)) {
                const SubCell targetCell = pointerCell(gx, gy, originX_);
                if (!s.goal || !sameTile(*s.goal, targetCell)) {
                    s.goal = targetCell;
                    s.route.clear();
                    s.cursor = 0;
                    s.planning = {};
                    s.needsRoute = false;
                }
                continue;
            }
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
            if (dist < double(groundMinR)) {
                continue;
            }
            if (!s.motion && s.attackReload <= 0) {
                s.direction = directionFromDelta(gu - su, gv - sv);
                s.firing = true;
                s.fireAnim = 0;
                s.lastFireTick = tick_;
                bool threwGrenade = false;
                if (s.isMortar) {
                    const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
                    applyMortarSplash(gx, gy, rawDamage, s.owner, static_cast<int>(i), -1, changed);
                    if (!mines_.empty()) {
                        const SubCell impCell = pointerCell(gx, gy, originX_);
                        const int mtx = impCell.x / 2;
                        const int mty = impCell.y / 2;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                triggerMineAtTile(mtx + dx, mty + dy, changed, true);
                            }
                        }
                    }
                    --s.ammo;
                } else if (s.isFlamer) {
                    const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
                    applyFlameSplash(gx, gy, rawDamage, s.owner, static_cast<int>(i), -1, -1, changed);
                    --s.ammo;
                } else {
                    threwGrenade = true;
                    --s.atGrenades;
                    applyMortarSplash(gx, gy, 85, s.owner, static_cast<int>(i), static_cast<int>(i), changed);
                    if (!mines_.empty()) {
                        const SubCell impCell = pointerCell(gx, gy, originX_);
                        triggerMineAtTile(impCell.x / 2, impCell.y / 2, changed, true);
                    }
                }
                applySoldierShotReload(s);
                const bool vis = s.owner == 0 || (s.spottedMask & 1u) != 0;
                changed.push_back({
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group,
                    true, vis, gx, gy, false, s.expa, s.kills, threwGrenade, s.atGrenades,
                    s.isFlamer, isSoldierCamouflaged(s), s.mineAmmo, s.ammo, s.isMortar
                });
                if (threwGrenade || s.ammo <= 0) s.attackGround.reset();
            }
            continue;
        }
        const int acquireMaxRange = s.isKamikaze ? std::max(maxRange, 180) : maxRange;
        const bool attackerElevated = !isGar && fog_ && fog_->isElevated(s.cell.x / 2, s.cell.y / 2);
        const auto effectiveSoldRange = [&](int baseR, const Soldier& target) {
            if (!attackerElevated || fog_->isElevated(target.cell.x / 2, target.cell.y / 2)) {
                return baseR;
            }
            return (baseR * 112) / 100;
        };
        if (!activeTarget && !activeBldTarget && (autoEngage_ || s.attackMove) && !s.holdFire && s.ammo > 0) {
            const int manPref = s.hasAttackPref
                ? s.attackPref[static_cast<std::size_t>(assets::TargetClass::Man)] : 0;
            const int bldPref = s.hasAttackPref
                ? s.attackPref[static_cast<std::size_t>(assets::TargetClass::Building)] : 0;
            const bool allowInfAuto = !s.hasAttackPref || manPref != 1;
            const bool allowBldAuto = !s.hasAttackPref || bldPref != 1;
            // Keep existing autoTarget while alive, spotted, and in range; otherwise acquire best target.
            if (s.autoTarget) {
                const size_t tid = *s.autoTarget;
                if (!allowInfAuto || tid >= soldiers_.size() || !isValidAttackTarget(tid, s.owner)) {
                    s.autoTarget.reset();
                } else {
                    const auto [tu, tv] = soldierWorld(soldiers_[tid]);
                    const double d = std::hypot(double(tu - su), double(tv - sv));
                    if (d < double(minRange) || d > double(effectiveSoldRange(acquireMaxRange, soldiers_[tid]))) {
                        s.autoTarget.reset();
                    }
                }
            }
            if (!s.autoTarget && allowInfAuto) {
                double bestDist = double((acquireMaxRange * 112) / 100) + 1.0;
                std::optional<size_t> bestTarget;
                for (size_t j = 0; j < soldiers_.size(); ++j) {
                    if (j == i || !isValidAttackTarget(j, s.owner)) continue;
                    const auto [tu, tv] = soldierWorld(soldiers_[j]);
                    const double d = std::hypot(double(tu - su), double(tv - sv));
                    if (d >= double(minRange) && d <= double(effectiveSoldRange(acquireMaxRange, soldiers_[j])) && d < bestDist) {
                        bestDist = d;
                        bestTarget = j;
                    }
                }
                s.autoTarget = bestTarget;
            }
            if (s.autoTarget) activeTarget = *s.autoTarget;
            const bool bldOutranksMan = s.hasAttackPref && bldPref >= 2 && (manPref < 2 || bldPref > manPref);
            if ((!activeTarget || bldOutranksMan) && allowBldAuto && !buildings_.empty()) {
                double bestBDist = double(acquireMaxRange) + 1.0;
                std::optional<size_t> bestBld;
                for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                    const auto& tb = buildings_[bi];
                    if (tb.destroyed || tb.owner < 0 || !isHostile(s.owner, tb.owner) || tb.occupants.empty()) continue;
                    if ((fog_->tile(tb.entryCell.x / 2, tb.entryCell.y / 2) & (1u << s.owner)) == 0) continue;
                    const double d = std::max(0.0, std::hypot(double(tb.x - sx), double(tb.y - sy)) -
                                                   std::min(tb.width, tb.height) * 0.4);
                    if (d <= double(acquireMaxRange) && d < bestBDist) {
                        bestBDist = d;
                        bestBld = bi;
                    }
                }
                if (bestBld) {
                    if (bldOutranksMan) activeTarget.reset();
                    activeBldTarget = bestBld;
                }
            }
        }
        if (!activeTarget && activeBldTarget) {
            const auto& tb = buildings_[*activeBldTarget];
            const auto [bu, bv] = tutorialFootWorld(tb.x, tb.y, originX_);
            const double dist = std::max(0.0, std::hypot(double(tb.x - sx), double(tb.y - sy)) -
                                              std::min(tb.width, tb.height) * 0.4);
            const double bldReach = s.isKamikaze ? std::max(56.0, double(maxRange)) : double(maxRange);
            if ((explicitBldOrder || s.isKamikaze) && dist > bldReach) {
                if (!isGar && (!s.goal || !sameTile(*s.goal, tb.entryCell))) {
                    s.goal = tb.entryCell;
                    s.route.clear();
                    s.cursor = 0;
                    s.planning = {};
                    s.needsRoute = false;
                }
                continue;
            }
            if ((explicitBldOrder || s.isKamikaze) && !isGar) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                s.needsRoute = false;
            }
            if (!s.motion && dist <= bldReach && s.attackReload <= 0) {
                if (s.isKamikaze) {
                    s.direction = directionFromDelta(bu - su, bv - sv);
                    detonateKamikazeSoldier(i, tb.x, tb.y, std::nullopt, *activeBldTarget, changed);
                    continue;
                }
                s.direction = directionFromDelta(bu - su, bv - sv);
                s.firing = true;
                s.fireAnim = 0;
                s.lastFireTick = tick_;
                if (s.isSniper) fogDirty_ = true;
                const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
                const auto hitType = (s.isFlamer || s.isMortar)
                    ? (s.isFlamer ? assets::HitType::Fire : assets::HitType::Explosive)
                    : (s.isAntiVehicle ? assets::HitType::Pierce : assets::HitType::Machine);
                damageBuilding(*activeBldTarget, rawDamage, hitType, s.owner, changed);
                --s.ammo;
                applySoldierShotReload(s);
                const bool vis = !isGar && (s.owner == 0 || (s.spottedMask & 1u) != 0);
                changed.push_back({
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group,
                    true, vis, tb.x, tb.y, isGar, s.expa, s.kills, false, s.atGrenades,
                    s.isFlamer, isSoldierCamouflaged(s), s.mineAmmo, s.ammo, s.isMortar
                });
            }
            continue;
        }
        if (!activeTarget) {
            if ((s.isAntiVehicle || s.isKamikaze || s.isSniper || s.atGrenades > 0) && !vehicles_.empty()) {
                stepSoldierAttackVehicle(s, i, changed);
            }
            continue;
        }

        const size_t tid = *activeTarget;
        const auto& t = soldiers_[tid];
        const auto [tu, tv] = soldierWorld(t);
        const double dist = std::hypot(double(tu - su), double(tv - sv));
        const int effMaxRange = effectiveSoldRange(maxRange, t);
        const double infReach = s.isKamikaze ? std::max(56.0, double(effMaxRange)) : double(effMaxRange);

        if ((explicitOrder || s.isKamikaze) && dist > infReach) {
            if (!isGar && (!s.goal || !sameTile(*s.goal, t.cell))) {
                s.goal = t.cell;
                s.route.clear();
                s.cursor = 0;
                s.planning = {};
                s.needsRoute = false;
            }
            continue;
        }

        if ((explicitOrder || s.isKamikaze) && !isGar) {
            // Explicit attack in range: halt movement permanently.
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
        }

        if (!s.motion && dist >= double(minRange) && dist <= infReach && s.attackReload <= 0) {
            if (s.isKamikaze) {
                s.direction = directionFromDelta(tu - su, tv - sv);
                detonateKamikazeSoldier(i, originX_ + tu - tv, (tu + tv) / 2, std::nullopt, std::nullopt, changed);
                continue;
            }
            if (!isGar && s.crouchToFire && !s.crouched) {
                s.crouched = true;
                s.direction = directionFromDelta(tu - su, tv - sv);
                s.attackReload = 4;
                fogDirty_ = true;
                const bool vis = s.owner == 0 || (s.spottedMask & 1u) != 0;
                pushOrUpdateSoldierPose(changed, {
                    i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, false, vis,
                    0, 0, false, s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s),
                    s.mineAmmo, s.ammo, false, s.isSuppressed(), std::int8_t{1}
                });
                continue;
            }
            s.direction = directionFromDelta(tu - su, tv - sv);
            s.firing = true;
            s.fireAnim = 0;
            s.lastFireTick = tick_;
            if (s.isSniper) fogDirty_ = true;

            // Exact 0x10059df6..0x10059e59 two-draw scatter
            (void)nextCombatRandom(0x7fffu);
            (void)nextCombatRandom(0x7fffu);

            const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
            const int speed = std::max(1, s.desc.projectileSpeed);
            const int flightTicks = std::max(1, int(std::ceil(dist / double(speed))));
            const int baseHitPct = sniperHitPercent(t.desc.armorSniper);
            const int marksmanBonus = (s.isSniper && s.crouched && !s.motion) ? 20 : 0;
            const int hitPct = s.isSniper
                ? std::clamp(baseHitPct + marksmanBonus - (t.crouched ? 25 : 0), 10, 95)
                : (t.crouched ? std::max(10, baseHitPct - 25) : baseHitPct);
            projectiles_.push_back(Projectile{
                i, tid, rawDamage, hitPct, std::max(1, flightTicks - 1), s.isFlamer, s.isMortar, s.isHeavyMg
            });

            const int expaGain = scaleByExpa(s.desc.expaBase, s.desc.expaDelta, s.expa);
            s.expa = std::min(0x8000, s.expa + expaGain);
            --s.ammo;
            applySoldierShotReload(s);
            const bool vis = !isGar && (s.owner == 0 || (s.spottedMask & 1u) != 0);
            changed.push_back({
                i, sx, sy, s.direction, 0, false, s.hp, true, s.owner, s.group, true, vis,
                originX_ + tu - tv, (tu + tv) / 2, isGar, s.expa, s.kills, false, s.atGrenades,
                s.isFlamer, isSoldierCamouflaged(s), s.mineAmmo, s.ammo, s.isMortar
            });
        }
    }

    stepOpenTopPassengerFire(changed);

    // Advance in-flight projectiles (0x10088100 -> 0x10057040 -> 0x100488f0)
    std::vector<Projectile> alive;
    alive.reserve(projectiles_.size());
    for (auto& p : projectiles_) {
        --p.ticksRemaining;
        if (p.ticksRemaining > 0) {
            alive.push_back(p);
            continue;
        }
        (void)nextCombatRandom(0xffu);
        const std::uint32_t rHit = nextCombatRandom(0x7fffu);
        const int roll = (int(rHit) * 100) >> 15;
        if (roll <= p.hitPct && p.target < soldiers_.size()) {
            auto& tgt = soldiers_[p.target];
            if (tgt.hp > 0) {
                tgt.hp = std::max(0, tgt.hp - p.rawDamage);
                if (tgt.hp > 0 && tgt.expaOnHit > 0) {
                    tgt.expa = std::min(0x8000, tgt.expa + tgt.expaOnHit);
                }
                if (tgt.hp > 0 && (p.isHeavyMg || p.isMortar)) {
                    applySuppression(p.target, p.isMortar ? 25 : 20);
                }
                recordHit(tgt.owner, tgt.group);
                if (p.attacker < soldiers_.size()) {
                    const auto [au, av] = soldierWorld(soldiers_[p.attacker]);
                    reactToAttack(tgt.owner, tgt.group, originX_ + au - av, (au + av) / 2);
                }
                if (tgt.hp > 0 && tgt.hp < (tgt.desc.maxHp >> 1) && p.rawDamage > (tgt.desc.maxHp >> 4)) {
                    (void)nextCombatRandom(1u);
                }
                const auto [tu, tv] = soldierWorld(tgt);
                const int impX = originX_ + tu - tv;
                const int impY = (tu + tv) / 2;
                const int attOwner = (p.attacker < soldiers_.size()) ? soldiers_[p.attacker].owner : -1;
                if (tgt.hp == 0) {
                    if (p.attacker < soldiers_.size()) {
                        auto& att = soldiers_[p.attacker];
                        ++att.kills;
                        att.expa = std::min(0x8000, att.expa + 2048);
                        if (att.hp > 0) {
                            const auto [au, av] = soldierWorld(att);
                            const bool aVis = !att.embarked && !att.garrisonBuilding &&
                                (!fog_ || isAlliedWithPlayer(att.owner) || (att.spottedMask & 1u) != 0);
                            pushOrUpdateSoldierPose(changed, {
                                p.attacker, originX_ + au - av, (au + av) / 2,
                                att.motion ? att.motion->direction : att.direction,
                                att.motion ? att.motion->progress : 0u,
                                att.motion ? att.motion->running : false,
                                att.hp, true, att.owner, att.group, false, aVis, 0, 0,
                                att.garrisonBuilding.has_value(), att.expa, att.kills, false, att.atGrenades,
                                false, isSoldierCamouflaged(att)
                            });
                        }
                    }
                    if (!vehicleAtCell(tgt.cell)) grid_.setOccupied(tgt.cell, false);
                    tgt.motion.reset();
                    tgt.goal.reset();
                    tgt.route.clear();
                    tgt.cursor = 0;
                    tgt.attackTarget.reset();
                    tgt.attackVehicleTarget.reset();
                    tgt.autoTarget.reset();
                    tgt.binoculars = false;
                    tgt.spottedMask = 0;
                    fogDirty_ = true;
                }
                const bool tVis = !tgt.garrisonBuilding &&
                    (!fog_ || isAlliedWithPlayer(tgt.owner) || (tgt.spottedMask & 1u) != 0);
                changed.push_back({
                    p.target, impX, impY, tgt.direction, 0, false, tgt.hp, tgt.hp > 0, tgt.owner, tgt.group,
                    false, tVis, 0, 0, tgt.garrisonBuilding.has_value(), tgt.expa, tgt.kills, false,
                    tgt.atGrenades, false, isSoldierCamouflaged(tgt)
                });
                if (p.isFlamer && attOwner >= 0) {
                    applyFlameSplash(impX, impY, p.rawDamage, attOwner,
                                     static_cast<int>(p.attacker), -1, static_cast<int>(p.target), changed);
                } else if (p.isMortar && attOwner >= 0) {
                    applyMortarSplash(impX, impY, p.rawDamage, attOwner,
                                      static_cast<int>(p.attacker), static_cast<int>(p.target), changed);
                }
            }
        } else if (p.isMortar && p.target < soldiers_.size() && p.attacker < soldiers_.size()) {
            const auto& tgt = soldiers_[p.target];
            const int attOwner = soldiers_[p.attacker].owner;
            if (tgt.hp > 0 && !tgt.embarked && !tgt.garrisonBuilding && attOwner >= 0) {
                const auto [tu, tv] = soldierWorld(tgt);
                applyMortarSplash(originX_ + tu - tv, (tu + tv) / 2, std::max(1, p.rawDamage / 2),
                                  attOwner, static_cast<int>(p.attacker), -1, changed);
            }
        }
    }
    projectiles_ = std::move(alive);
    if (fogDirty_) refreshSpotting(&changed);
}

void TutorialUnits::stepOpenTopPassengerFire(std::vector<SoldierPose>& changed) {
    (void)changed;
    if (!fog_) return;
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        const auto& v = vehicles_[vi];
        if (!v.alive || v.hp <= 0 || !v.passengersCanFire || v.passengers.empty() || v.holdFire) {
            continue;
        }
        const auto [su, sv] = tutorialFootWorld(v.x, v.y, originX_);
        const bool vehicleWantsCombat = autoEngage_ || v.attackMove || v.attackTarget.has_value();
        for (size_t pIdx : v.passengers) {
            if (pIdx >= soldiers_.size()) continue;
            auto& p = soldiers_[pIdx];
            if (p.hp <= 0 || !p.embarked) continue;
            if (p.attackReload > 0) --p.attackReload;
            if (p.holdFire || p.ammo <= 0 || p.isMedic || p.isOfficer || p.isKamikaze ||
                p.attackReload > 0 || !vehicleWantsCombat) {
                continue;
            }
            const int maxR = std::min(
                260, scaleByExpa(p.desc.rangeBase, p.desc.rangeDelta, p.expa) + v.bonusShotRange);
            const int minR = std::max(
                0, scaleByExpa(p.desc.deadzoneBase, p.desc.deadzoneDelta, p.expa) + v.bonusShotDeadZone);
            double bestDist = static_cast<double>(maxR) + 1.0;
            std::optional<size_t> bestTarget;
            for (size_t j = 0; j < soldiers_.size(); ++j) {
                if (!isValidAttackTarget(j, p.owner)) continue;
                const auto [tu, tv] = soldierWorld(soldiers_[j]);
                const double d = std::hypot(double(tu - su), double(tv - sv));
                if (d >= static_cast<double>(minR) && d <= static_cast<double>(maxR) && d < bestDist) {
                    bestDist = d;
                    bestTarget = j;
                }
            }
            if (!bestTarget) continue;
            const size_t tid = *bestTarget;
            const auto& t = soldiers_[tid];
            const auto [tu, tv] = soldierWorld(t);
            const int tx = originX_ + tu - tv;
            const int ty = (tu + tv) / 2;
            p.direction = directionFromDelta(tu - su, tv - sv);
            p.lastFireTick = tick_;

            (void)nextCombatRandom(0x7fffu);
            (void)nextCombatRandom(0x7fffu);
            const int rawDamage = scaleByExpa(p.desc.damageBase, p.desc.damageDelta, p.expa);
            const int speed = std::max(1, p.desc.projectileSpeed);
            const int flightTicks = std::max(1, int(std::ceil(bestDist / double(speed))));
            const int baseHitPct = sniperHitPercent(t.desc.armorSniper);
            const int hitPct = t.crouched ? std::max(10, baseHitPct - 25) : baseHitPct;
            projectiles_.push_back(Projectile{
                pIdx, tid, rawDamage, hitPct, std::max(1, flightTicks - 1), p.isFlamer, p.isMortar, p.isHeavyMg
            });
            const int expaGain = scaleByExpa(p.desc.expaBase, p.desc.expaDelta, p.expa);
            p.expa = std::min(0x8000, p.expa + expaGain);
            --p.ammo;
            applySoldierShotReload(p);
            emitVehiclePose(vi, true, tx, ty, true, "fire", false, false);
        }
    }
}

void TutorialUnits::stepVehicles(std::vector<SoldierPose>& changed) {
    static constexpr std::array<double, 3> kSubtleSteer{{0.0, 0.55, -0.55}};
    const auto stepFacing = [&](size_t idx, Vehicle& veh) {
        if (!veh.facingTargetDir) return;
        const unsigned targetDir = *veh.facingTargetDir & 31u;
        const unsigned turnStep = std::max(1u, veh.turnStep32);
        const unsigned prevHull = veh.hullDir;
        const unsigned prevTurret = veh.turretDir;
        veh.hullDir = turnTowardDir32(veh.hullDir, targetDir, turnStep);
        if (!veh.attackTarget) {
            veh.turretDir = turnTowardDir32(veh.turretDir, targetDir, 2u);
        }
        if (veh.hullDir == targetDir && (veh.attackTarget || veh.turretDir == targetDir)) {
            veh.facingTargetDir.reset();
        }
        if (veh.hullDir != prevHull || veh.turretDir != prevTurret) {
            emitVehiclePose(idx);
        }
    };
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || !v.hasCrew) continue;
        if (v.towedBy) {
            v.facingTargetDir.reset();
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (v.moveTarget || v.wasMoving) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.queuedOrders.clear();
                v.attackMove = false;
                v.attackMoveEngaging = false;
                v.chasingAttackTarget = false;
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            continue;
        }
        if (v.speedPx <= 0.0 || v.isMoveImpaired()) {
            if (v.moveTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.queuedOrders.clear();
                v.attackMove = false;
                v.attackMoveEngaging = false;
                v.chasingAttackTarget = false;
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(i);
                }
            }
            stepFacing(i, v);
            continue;
        }
        if (!v.moveTarget) {
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            stepFacing(i, v);
            continue;
        }
        if (v.attackMoveEngaging) {
            if (v.wasMoving) {
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            continue;
        }
        const double vetFrac = static_cast<double>(std::clamp(v.expa, 0, 0x8000)) / 32768.0;
        const double fwdSpeed = (v.speedPxMax > v.speedPx)
            ? (v.speedPx + (v.speedPxMax - v.speedPx) * vetFrac)
            : v.speedPx;
        const double revSpeed = (v.backSpeedPx > 0.0 && v.speedPx > 0.0 && v.speedPxMax > v.speedPx)
            ? v.backSpeedPx * (fwdSpeed / v.speedPx)
            : v.backSpeedPx;
        const double rawSpeed = v.reverseMove ? revSpeed : fwdSpeed;
        const double baseSpeed = (v.moveDamageHp > 0 && v.hp < v.moveDamageHp)
            ? std::max(0.45, rawSpeed * 0.35)
            : rawSpeed;
        double towMult = 1.0;
        if (v.towedVehicle && *v.towedVehicle < vehicles_.size()) {
            const auto& gun = vehicles_[*v.towedVehicle];
            const int tIdx = assets::UnitSpec::towClassIndex(gun.towWeightClass);
            if (tIdx >= 0 && v.towSpeedByClass[static_cast<std::size_t>(tIdx)] > 0.0) {
                towMult = v.towSpeedByClass[static_cast<std::size_t>(tIdx)];
            } else {
                towMult = 0.6;
            }
        }
        const bool shallowBog = v.isWheeled && !v.isNaval && grid_.isShallows(v.cell);
        double stepSpeed = v.towedVehicle ? std::max(0.45, baseSpeed * towMult) : baseSpeed;
        if (shallowBog) {
            stepSpeed = std::max(0.35, stepSpeed * 0.70);
        }
        const auto advanceNextOrderOrPatrol = [&](Vehicle& veh) -> bool {
            if (!veh.queuedOrders.empty()) {
                const auto [nx, ny] = veh.queuedOrders.front();
                veh.queuedOrders.pop_front();
                moveVehicleInternal(veh, nx, ny);
                veh.wasMoving = true;
                return true;
            }
            if (veh.patrolOrigin && veh.patrolDest) {
                const auto origin = *veh.patrolOrigin;
                const auto dest = *veh.patrolDest;
                const bool nextToDest = !veh.patrolToDest;
                const auto nextPt = nextToDest ? dest : origin;
                moveVehicleInternal(veh, nextPt.first, nextPt.second);
                veh.patrolOrigin = origin;
                veh.patrolDest = dest;
                veh.patrolToDest = nextToDest;
                veh.attackMove = true;
                veh.wasMoving = true;
                return true;
            }
            return false;
        };
        const auto [tx, ty] = *v.moveTarget;
        const double finalDist = std::hypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
        if (finalDist <= kVehicleArrivePx) {
            if (!advanceNextOrderOrPatrol(v)) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                if (!v.escortVehicleTarget && !v.escortSoldierTarget) {
                    v.attackMove = false;
                }
                v.attackMoveEngaging = false;
                v.reverseMove = false;
                v.wasMoving = false;
                emitVehiclePose(i);
            }
            continue;
        }

        while (v.waypoints.size() > 1) {
            const auto [wx, wy] = v.waypoints.front();
            if (std::hypot(static_cast<double>(wx) - v.fx, static_cast<double>(wy) - v.fy) > stepSpeed) {
                break;
            }
            v.waypoints.pop_front();
        }
        if (v.waypoints.size() == 1) {
            const auto [wx, wy] = v.waypoints.front();
            if (std::hypot(static_cast<double>(wx) - v.fx, static_cast<double>(wy) - v.fy) <= kVehicleArrivePx) {
                v.waypoints.pop_front();
            }
        }

        const auto tryStepToward = [&](int targetX, int targetY, bool allowSteer,
                                       double& outFx, double& outFy, SubCell& outCell) -> bool {
            const double dx = static_cast<double>(targetX) - v.fx;
            const double dy = static_cast<double>(targetY) - v.fy;
            const double dist = std::hypot(dx, dy);
            if (dist <= 1e-6) return false;
            const double stepLen = std::min(stepSpeed, dist);
            const double ux = dx / dist;
            const double uy = dy / dist;
            const std::size_t maxIdx = allowSteer ? kSubtleSteer.size() : 1u;
            for (std::size_t k = 0; k < maxIdx; ++k) {
                const double angle = kSubtleSteer[k];
                const double c = std::cos(angle);
                const double s = std::sin(angle);
                const double candFx = v.fx + (ux * c - uy * s) * stepLen;
                const double candFy = v.fy + (ux * s + uy * c) * stepLen;
                if (angle != 0.0) {
                    const double candDist = std::hypot(static_cast<double>(targetX) - candFx,
                                                       static_cast<double>(targetY) - candFy);
                    if (candDist > dist - 0.1) continue;
                }
                SubCell candCell{};
                if (canVehicleStepTo(v, candFx, candFy, candCell)) {
                    outFx = candFx;
                    outFy = candFy;
                    outCell = candCell;
                    return true;
                }
            }
            return false;
        };

        auto [stepTx, stepTy] = v.waypoints.empty() ? *v.moveTarget : v.waypoints.front();
        double nextFx = v.fx;
        double nextFy = v.fy;
        SubCell nextCell = v.cell;
        bool moved = tryStepToward(stepTx, stepTy, !v.waypoints.empty(), nextFx, nextFy, nextCell);

        if (!moved && v.waypoints.empty()) {
            const SubCell destCell = pointerCell(tx, ty, originX_);
            if (grid_.inBounds(destCell)) {
                const auto dirs = grid_.findTileDirectionsForMask(v.cell, destCell, v.forbiddenTerrainMask, 256);
                if (!dirs.empty()) {
                    int curTx = v.cell.x / 2;
                    int curTy = v.cell.y / 2;
                    const auto pushTileCenter = [&](int tileX, int tileY) {
                        const int wu = tileX * 32 + 16;
                        const int wv = tileY * 32 + 16;
                        v.waypoints.emplace_back(originX_ + wu - wv, (wu + wv) / 2);
                    };
                    for (std::uint8_t d : dirs) {
                        if (d >= 8u) break;
                        const int dxTile = kTileDx[d];
                        const int dyTile = kTileDy[d];
                        if (dxTile != 0 && dyTile != 0) {
                            const SubCell sideA{(curTx + dxTile) * 2, curTy * 2};
                            const SubCell sideB{curTx * 2, (curTy + dyTile) * 2};
                            if (!grid_.walkableMask(sideA, v.forbiddenTerrainMask) ||
                                !grid_.walkableMask(sideB, v.forbiddenTerrainMask)) {
                                if (grid_.walkableMask(sideA, v.forbiddenTerrainMask)) {
                                    pushTileCenter(curTx + dxTile, curTy);
                                } else if (grid_.walkableMask(sideB, v.forbiddenTerrainMask)) {
                                    pushTileCenter(curTx, curTy + dyTile);
                                }
                            }
                        }
                        curTx += dxTile;
                        curTy += dyTile;
                        pushTileCenter(curTx, curTy);
                    }
                    if (!v.waypoints.empty()) {
                        v.waypoints.back() = {tx, ty};
                        while (v.waypoints.size() > 1) {
                            const auto [wx, wy] = v.waypoints.front();
                            if (std::hypot(static_cast<double>(wx) - v.fx, static_cast<double>(wy) - v.fy) > stepSpeed) {
                                break;
                            }
                            v.waypoints.pop_front();
                        }
                        if (!v.waypoints.empty()) {
                            stepTx = v.waypoints.front().first;
                            stepTy = v.waypoints.front().second;
                            moved = tryStepToward(stepTx, stepTy, true, nextFx, nextFy, nextCell);
                        }
                    }
                }
            }
        }

        if (!moved) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.queuedOrders.clear();
            v.patrolOrigin.reset();
            v.patrolDest.reset();
            v.attackMove = false;
            v.attackMoveEngaging = false;
            v.chasingAttackTarget = false;
            v.reverseMove = false;
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            v.wasMoving = false;
            emitVehiclePose(i);
            continue;
        }

        const double prevFx = v.fx;
        const double prevFy = v.fy;
        const unsigned moveDir = vehicleDirFromMapDelta(nextFx - v.fx, nextFy - v.fy);
        const unsigned desiredHull = v.reverseMove ? ((moveDir + 16u) & 31u) : moveDir;
        v.hullDir = turnTowardDir32(v.hullDir, desiredHull, std::max(1u, v.turnStep32));
        if (!v.attackTarget) {
            v.turretDir = turnTowardDir32(
                v.turretDir, v.hullDir, v.hasGunTurnDelay ? v.turretStep32 : std::max(2u, v.turnStep32));
        }
        v.fx = nextFx;
        v.fy = nextFy;
        v.x = static_cast<int>(std::lround(nextFx));
        v.y = static_cast<int>(std::lround(nextFy));
        if (v.gunShotWait > 0) {
            v.gunSettleTicks = v.gunShotWait;
        }

        if (nextCell != v.cell) {
            if (nextCell.x / 2 != v.cell.x / 2 || nextCell.y / 2 != v.cell.y / 2) {
                fogDirty_ = true;
            }
            const SubCell oldCell = v.cell;
            v.cell = nextCell;
            for (size_t pIdx : v.passengers) {
                if (pIdx < soldiers_.size()) soldiers_[pIdx].cell = nextCell;
            }
            if (!soldierAtCell(oldCell) && !vehicleAtCell(oldCell)) {
                grid_.setOccupied(oldCell, false);
            }
            grid_.setOccupied(nextCell, true);
        }

        if (v.canCrushInfantry && v.hasCrew) {
            for (size_t k = 0; k < soldiers_.size(); ++k) {
                auto& ss = soldiers_[k];
                if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(v.owner, ss.owner)) continue;
                const auto [su, sv] = soldierWorld(ss);
                const int sx = originX_ + su - sv;
                const int sy = (su + sv) / 2;
                const double infCrushR = (v.unitRadius > 0)
                    ? std::clamp(static_cast<double>(v.unitRadius + ss.unitRadius), 18.0, 32.0)
                    : 22.0;
                if (std::hypot(static_cast<double>(sx) - v.fx, static_cast<double>(sy) - v.fy) > infCrushR) continue;
                ss.hp = 0;
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 1500 + v.crushExpaBonus * 10);
                recordHit(ss.owner, ss.group);
                if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
                ss.motion.reset();
                ss.goal.reset();
                ss.route.clear();
                ss.cursor = 0;
                ss.queuedGoals.clear();
                ss.patrolOrigin.reset();
                ss.patrolDest.reset();
                ss.attackMove = false;
                ss.attackTarget.reset();
                ss.attackVehicleTarget.reset();
                ss.attackBuildingTarget.reset();
                ss.autoTarget.reset();
                ss.boardVehicleTarget.reset();
                ss.captureVehicleTarget.reset();
                ss.garrisonTarget.reset();
                ss.binoculars = false;
                ss.spottedMask = 0;
                fogDirty_ = true;
                const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
                pushOrUpdateSoldierPose(changed, {
                    k, sx, sy, ss.direction, 0, false, 0, false, ss.owner, ss.group, false, sVis
                });
            }
            for (size_t k = 0; k < vehicles_.size(); ++k) {
                if (k == i) continue;
                auto& tv = vehicles_[k];
                if (!tv.alive || tv.hp <= 0 || !tv.canBeCrushed || !isHostile(v.owner, tv.owner)) continue;
                const double gunCrushR = (v.unitRadius > 0 || tv.unitRadius > 0)
                    ? std::clamp(static_cast<double>((v.unitRadius > 0 ? v.unitRadius : 12) +
                                                     (tv.unitRadius > 0 ? tv.unitRadius : 12)),
                                 24.0, 38.0)
                    : 26.0;
                if (std::hypot(tv.fx - v.fx, tv.fy - v.fy) > gunCrushR) continue;
                tv.hp = 0;
                destroyVehicle(k, changed);
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 2500);
                emitVehiclePose(k);
            }
        }

        if (v.towedVehicle && *v.towedVehicle < vehicles_.size()) {
            const double mdx = v.fx - prevFx;
            const double mdy = v.fy - prevFy;
            const double mlen = std::hypot(mdx, mdy);
            if (mlen > 1e-4) {
                auto& gun = vehicles_[*v.towedVehicle];
                const double gx = std::clamp(v.fx - (mdx / mlen) * 36.0, 0.0, static_cast<double>(std::max(1, mapPixelW_ - 1)));
                const double gy = std::clamp(v.fy - (mdy / mlen) * 36.0, 0.0, static_cast<double>(std::max(1, mapPixelH_ - 1)));
                gun.fx = gx;
                gun.fy = gy;
                gun.x = static_cast<int>(std::lround(gx));
                gun.y = static_cast<int>(std::lround(gy));
                gun.hullDir = v.hullDir;
                gun.turretDir = v.hullDir;
                const SubCell newGunCell = placementCell(gun.x, gun.y, originX_);
                if (newGunCell != gun.cell) {
                    if (newGunCell.x / 2 != gun.cell.x / 2 || newGunCell.y / 2 != gun.cell.y / 2) {
                        fogDirty_ = true;
                    }
                    const SubCell oldGunCell = gun.cell;
                    gun.cell = newGunCell;
                    if (!soldierAtCell(oldGunCell) && !vehicleAtCell(oldGunCell)) {
                        grid_.setOccupied(oldGunCell, false);
                    }
                    grid_.setOccupied(newGunCell, true);
                }
                emitVehiclePose(*v.towedVehicle);
            }
        }

        const double newDist = std::hypot(static_cast<double>(tx) - v.fx, static_cast<double>(ty) - v.fy);
        if (newDist <= kVehicleArrivePx) {
            if (!advanceNextOrderOrPatrol(v)) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                v.attackMove = false;
                v.attackMoveEngaging = false;
                v.reverseMove = false;
                v.wasMoving = false;
            }
        } else {
            v.wasMoving = true;
        }
        emitVehiclePose(i);
    }
}

void TutorialUnits::stepVehicleCombat(std::vector<SoldierPose>& changed) {
    struct ActiveWeapon {
        bool valid{false};
        bool secondary{false};
        double range{0.0};
        double deadzone{0.0};
        int damage{0};
        int cooldown{35};
        assets::HitType hitType{assets::HitType::Pierce};
        const std::string* sound{nullptr};
    };
    const auto selectWeapon = [](const Vehicle& v, bool targetIsVehicle) -> ActiveWeapon {
        const bool canPrimary = v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0);
        const bool canSecondary = v.hasSecondary && (v.ammoPerShot2 == 0 || v.ammo2 > 0);
        if (!targetIsVehicle) {
            if (canSecondary) {
                return {true, true, v.range2, 0.0, v.damage2, v.cooldown2, v.hitType2, &v.sound2};
            }
            if (canPrimary) {
                return {true, false, v.range, v.deadzone, v.damage, v.cooldown, v.hitType, &v.sound1};
            }
            return {};
        }
        if (canPrimary) {
            return {true, false, v.range, v.deadzone, v.damage, v.cooldown, v.hitType, &v.sound1};
        }
        if (canSecondary) {
            return {true, true, v.range2, 0.0, v.damage2, v.cooldown2, v.hitType2, &v.sound2};
        }
        return {};
    };
    const auto scaleVehCooldown = [&](const Vehicle& veh, int baseCd) {
        const double vet = static_cast<double>(std::clamp(veh.expa, 0, 0x8000)) / 32768.0;
        double mult = 1.0 - 0.25 * vet;
        if (veh.isCrewImpaired()) {
            mult *= 1.75;
        }
        if (veh.isSuppressed()) {
            mult *= 1.35;
        }
        if (veh.moraleRage > 0 && !veh.isSuppressed() && officerAuraDecayAt(veh.owner, veh.fx, veh.fy) >= 2) {
            mult *= 0.88;
        }
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(baseCd) * mult)));
    };
    const auto applyWeaponShotAndCooldown = [&](Vehicle& veh, bool secondary, int baseCd) {
        if (secondary) {
            veh.ammo2 = std::max(0, veh.ammo2 - veh.ammoPerShot2);
            ++veh.burstCount2;
            if (veh.burstCount2 >= veh.burstShots2) {
                veh.burstCount2 = 0;
                veh.fireCooldown = scaleVehCooldown(veh, veh.burstReload2);
            } else {
                veh.fireCooldown = scaleVehCooldown(veh, baseCd);
            }
            if (veh.shotExpa2 > 0) {
                veh.expa = std::min(0x8000, veh.expa + veh.shotExpa2);
            }
        } else {
            veh.ammo = std::max(0, veh.ammo - veh.ammoPerShot1);
            ++veh.burstCount1;
            if (veh.burstCount1 >= veh.burstShots1) {
                veh.burstCount1 = 0;
                veh.fireCooldown = scaleVehCooldown(veh, veh.burstReload1);
            } else {
                veh.fireCooldown = scaleVehCooldown(veh, baseCd);
            }
            if (veh.shotExpa1 > 0) {
                veh.expa = std::min(0x8000, veh.expa + veh.shotExpa1);
            }
        }
    };
    const auto markMuzzleFlashReveal = [&](Vehicle& veh, bool isSecondary, bool isHeavy, int dmg) {
        if (!fog_ || isSecondary || !isHostile(0, veh.owner)) return;
        if (!veh.isArtillery && !veh.isRocketArtillery && !isHeavy && dmg < 80) return;
        const unsigned dur = (veh.isArtillery || veh.isRocketArtillery) ? 125u : 75u;
        veh.muzzleRevealTicks = std::max(veh.muzzleRevealTicks, dur);
        if (!veh.visible) {
            veh.visible = true;
            veh.spottedMask = static_cast<std::uint8_t>(veh.spottedMask | 1u);
            fogDirty_ = true;
        }
    };

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        v.attackMoveEngaging = false;
        if (!v.alive || v.hp <= 0 || !v.hasCrew || v.towedBy) continue;
        if (v.fireCooldown > 0) --v.fireCooldown;
        if (!v.wasMoving && v.gunSettleTicks > 0) --v.gunSettleTicks;
        const bool anyAmmo = (v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0)) ||
                             (v.hasSecondary && (v.ammoPerShot2 == 0 || v.ammo2 > 0));
        if (!v.armed || !anyAmmo) {
            v.attackTarget.reset();
            v.attackBuildingTarget.reset();
            if (v.chasingAttackTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(i);
                }
            }
            continue;
        }

        std::optional<std::pair<bool, size_t>> activeTarget;
        bool explicitOrder = false;
        if (v.attackTarget) {
            const auto [isVeh, tid] = *v.attackTarget;
            if (!isVehicleTargetValidFor(v.owner, isVeh, tid) || !selectWeapon(v, isVeh).valid) {
                v.attackTarget.reset();
                if (v.chasingAttackTarget) {
                    v.moveTarget.reset();
                    v.waypoints.clear();
                    v.chasingAttackTarget = false;
                    if (v.wasMoving) {
                        v.wasMoving = false;
                        emitVehiclePose(i);
                    }
                }
            } else {
                activeTarget = *v.attackTarget;
                explicitOrder = true;
            }
        }

        std::optional<size_t> activeBldTarget;
        bool explicitBldOrder = false;
        if (!activeTarget && v.attackBuildingTarget) {
            const size_t bid = *v.attackBuildingTarget;
            if (bid >= buildings_.size() || buildings_[bid].destroyed ||
                buildings_[bid].owner < 0 || !isHostile(v.owner, buildings_[bid].owner)) {
                v.attackBuildingTarget.reset();
                if (v.chasingAttackTarget) {
                    v.moveTarget.reset();
                    v.waypoints.clear();
                    v.chasingAttackTarget = false;
                    if (v.wasMoving) {
                        v.wasMoving = false;
                        emitVehiclePose(i);
                    }
                }
            } else {
                activeBldTarget = bid;
                explicitBldOrder = true;
            }
        }

        const bool vehElevated = fog_ && fog_->isElevated(v.cell.x / 2, v.cell.y / 2);
        const auto effectiveVehRange = [&](double baseR, int targetMapX, int targetMapY) {
            if (!vehElevated) return baseR;
            const SubCell tCell = pointerCell(targetMapX, targetMapY, originX_);
            return fog_->isElevated(tCell.x / 2, tCell.y / 2) ? baseR : baseR * 1.12;
        };

        const bool scanReady = (v.scanDelayTicks <= 0) || (v.lastAutoScanTick == 0u) ||
                               (tick_ >= v.lastAutoScanTick + static_cast<unsigned>(v.scanDelayTicks));
        if (!activeTarget && !activeBldTarget && !v.attackGround && fog_ && (autoEngage_ || v.attackMove) &&
            !v.holdFire && scanReady) {
            v.lastAutoScanTick = tick_;
            const auto vehWeap = selectWeapon(v, true);
            const auto infWeap = selectWeapon(v, false);
            const bool primaryAP = vehWeap.valid && !vehWeap.secondary &&
                (vehWeap.hitType == assets::HitType::Pierce || vehWeap.hitType == assets::HitType::Transpierce);
            const bool primaryHeavyHE = vehWeap.valid && !vehWeap.secondary &&
                vehWeap.hitType == assets::HitType::Explosive && vehWeap.damage >= 120;
            const bool primaryAntiArmor = primaryAP || primaryHeavyHE;
            int bestTier = 99;
            double bestDist = 1e9;
            std::optional<std::pair<bool, size_t>> bestTarget;
            std::optional<size_t> bestBldTarget;
            if (vehWeap.valid) {
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == i || !isVehicleTargetValidFor(v.owner, true, j)) continue;
                    const auto& tv = vehicles_[j];
                    const double d = std::hypot(tv.fx - v.fx, tv.fy - v.fy);
                    if (d < vehWeap.deadzone || d > effectiveVehRange(vehWeap.range, tv.x, tv.y)) continue;
                    const bool targetArmed = tv.armed && (tv.ammo > 0 || tv.ammo2 > 0);
                    const bool targetArmored = tv.hasCatalogArmor
                        ? (tv.armor[static_cast<size_t>(assets::HitType::Machine)].front >= 20 ||
                           tv.armor[static_cast<size_t>(assets::HitType::Pierce)].front >= 20)
                        : (tv.maxHp >= 250);
                    int tier = 0;
                    if (v.hasAttackPref) {
                        const int cls = assets::UnitSpec::targetClassIndex(tv.effectiveTargetType());
                        const int pref = (cls >= 0) ? v.attackPref[static_cast<std::size_t>(cls)] : 0;
                        if (pref == 1 || (pref == 0 && targetArmored && !primaryAntiArmor)) continue;
                        tier = (pref >= 2) ? (256 - pref) : 300;
                    } else {
                        tier = primaryAntiArmor
                            ? (targetArmed ? 0 : 1)
                            : (targetArmored ? 3 : (targetArmed ? 1 : 2));
                    }
                    if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                        bestTier = tier;
                        bestDist = d;
                        bestTarget = std::pair{true, j};
                        bestBldTarget.reset();
                    }
                }
            }
            if (infWeap.valid) {
                const int manPref = v.hasAttackPref
                    ? v.attackPref[static_cast<std::size_t>(assets::TargetClass::Man)] : 0;
                if (!v.hasAttackPref || manPref != 1) {
                    const int infTierPref = v.hasAttackPref ? ((manPref >= 2) ? (256 - manPref) : 300) : 0;
                    for (size_t j = 0; j < soldiers_.size(); ++j) {
                        if (!isVehicleTargetValidFor(v.owner, false, j)) continue;
                        const auto [tu, tv] = soldierWorld(soldiers_[j]);
                        const int tx = originX_ + tu - tv;
                        const int ty = (tu + tv) / 2;
                        const double d = std::hypot(double(tx) - v.fx, double(ty) - v.fy);
                        if (d < infWeap.deadzone || d > effectiveVehRange(infWeap.range, tx, ty)) continue;
                        const int tier = v.hasAttackPref
                            ? infTierPref
                            : (primaryAntiArmor
                                   ? (soldiers_[j].isAntiVehicle ? 1 : 2)
                                   : (soldiers_[j].isAntiVehicle ? 0 : 1));
                        if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                            bestTier = tier;
                            bestDist = d;
                            bestTarget = std::pair{false, j};
                            bestBldTarget.reset();
                        }
                    }
                }
            }
            if ((vehWeap.valid || (v.weapon2Flamer && v.ammo2 > 0)) && !buildings_.empty()) {
                const int bldPref = v.hasAttackPref
                    ? v.attackPref[static_cast<std::size_t>(assets::TargetClass::Building)] : 0;
                for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                    const auto& tb = buildings_[bi];
                    if (tb.destroyed || tb.owner < 0 || !isHostile(v.owner, tb.owner) || tb.occupants.empty()) continue;
                    if ((fog_->tile(tb.entryCell.x / 2, tb.entryCell.y / 2) & (1u << v.owner)) == 0) continue;
                    const double d = std::max(0.0, std::hypot(double(tb.x) - v.fx, double(tb.y) - v.fy) -
                                                   std::min(tb.width, tb.height) * 0.35);
                    const bool canFlameBld = (v.weapon1Flamer && vehWeap.valid && !vehWeap.secondary &&
                                              d >= vehWeap.deadzone && d <= vehWeap.range) ||
                                             (v.weapon2Flamer && v.ammo2 > 0 && d <= v.range2);
                    const bool canVehWeapBld = vehWeap.valid && d >= vehWeap.deadzone && d <= vehWeap.range;
                    if (!canFlameBld && !canVehWeapBld) continue;
                    int tier = 0;
                    if (v.hasAttackPref) {
                        if (bldPref == 1 || (bldPref == 0 && !canFlameBld && !primaryHeavyHE)) continue;
                        tier = (bldPref >= 2) ? (256 - bldPref) : 302;
                    } else {
                        tier = canFlameBld ? 0 : (primaryHeavyHE ? 1 : 3);
                    }
                    if (tier < bestTier || (tier == bestTier && d < bestDist)) {
                        bestTier = tier;
                        bestDist = d;
                        bestBldTarget = bi;
                        bestTarget.reset();
                    }
                }
            }
            activeTarget = bestTarget;
            activeBldTarget = bestBldTarget;
        }

        const auto applyExplosiveSplash = [&](int impactX, int impactY, int dmgBase, int skipSoldierIdx, int skipBuildingIdx) {
            const double splashR = v.indirectArc1 ? 92.0 : 72.0;
            const double bldSplashR = v.indirectArc1 ? 104.0 : 84.0;
            for (size_t k = 0; k < soldiers_.size(); ++k) {
                if (skipSoldierIdx >= 0 && k == static_cast<size_t>(skipSoldierIdx)) continue;
                auto& ss = soldiers_[k];
                if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(v.owner, ss.owner)) continue;
                const auto [su, sv] = soldierWorld(ss);
                const int sx = originX_ + su - sv;
                const int sy = (su + sv) / 2;
                const double sd = std::hypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
                if (sd > splashR) continue;
                const int splashDmg = std::max(1, static_cast<int>(std::lround(
                    static_cast<double>(dmgBase) * 0.55 * (1.0 - sd / (splashR + 1.0)))));
                ss.hp = std::max(0, ss.hp - splashDmg);
                if (ss.hp > 0) applySuppression(k, 25);
                recordHit(ss.owner, ss.group);
                reactToAttack(ss.owner, ss.group, v.x, v.y);
                if (ss.hp == 0) {
                    ++v.kills;
                    v.expa = std::min(0x8000, v.expa + 1500);
                    if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
                    ss.motion.reset();
                    ss.goal.reset();
                    ss.route.clear();
                    ss.cursor = 0;
                    ss.queuedGoals.clear();
                    ss.attackMove = false;
                    ss.attackTarget.reset();
                    ss.attackVehicleTarget.reset();
                    ss.attackBuildingTarget.reset();
                    ss.autoTarget.reset();
                    ss.binoculars = false;
                    ss.spottedMask = 0;
                    fogDirty_ = true;
                }
                const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
                pushOrUpdateSoldierPose(changed, {
                    k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis
                });
            }
            for (size_t bi = 0; bi < buildings_.size(); ++bi) {
                if (skipBuildingIdx >= 0 && bi == static_cast<size_t>(skipBuildingIdx)) continue;
                const auto& tb = buildings_[bi];
                if (tb.destroyed) continue;
                const double bd = std::max(0.0, std::hypot(double(tb.x - impactX), double(tb.y - impactY)) -
                                                std::min(tb.width, tb.height) * 0.3);
                if (bd <= bldSplashR) {
                    damageBuilding(bi, std::max(10, dmgBase / 2), assets::HitType::Explosive, v.owner, changed);
                }
            }
            if (!mines_.empty()) {
                const SubCell impCell = pointerCell(impactX, impactY, originX_);
                const int itx = impCell.x / 2, ity = impCell.y / 2;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        triggerMineAtTile(itx + dx, ity + dy, changed, true);
                    }
                }
            }
        };

        const auto aimVehicleAt = [&](unsigned desiredDir, bool snap, bool forceEmit) {
            const unsigned prevTurret = v.turretDir;
            const unsigned prevHull = v.hullDir;
            const bool pivotHull = !v.towedBy && (v.speedPx <= 0.0 || (v.isCasemate && !v.moveTarget));
            if (snap) {
                v.turretDir = desiredDir;
                if (pivotHull) {
                    v.hullDir = desiredDir;
                }
                return;
            }
            v.turretDir = turnTowardDir32(v.turretDir, desiredDir, std::max(1u, v.turretStep32));
            if (pivotHull) {
                v.hullDir = turnTowardDir32(v.hullDir, desiredDir, std::max(1u, v.turnStep32));
            }
            if (v.turretDir != prevTurret || v.hullDir != prevHull || forceEmit) {
                emitVehiclePose(i);
            }
        };

        if (!activeTarget && activeBldTarget) {
            const size_t bid = *activeBldTarget;
            const auto& tb = buildings_[bid];
            const int tx = tb.x;
            const int ty = tb.y;
            const double dist = std::max(0.0, std::hypot(double(tx) - v.fx, double(ty) - v.fy) -
                                              std::min(tb.width, tb.height) * 0.35);
            auto weapon = selectWeapon(v, true);
            if (v.weapon2Flamer && v.ammo2 > 0 && dist <= v.range2) {
                weapon = {true, true, v.range2, 0.0, v.damage2, v.cooldown2, v.hitType2, &v.sound2};
            }
            if (!weapon.valid) {
                v.attackBuildingTarget.reset();
                continue;
            }
            const unsigned desiredTurret = vehicleDirFromMapDelta(double(tx) - v.fx, double(ty) - v.fy);
            if (explicitBldOrder && dist > weapon.range && v.speedPx > 0.0) {
                const int clampedX = std::clamp(tx, 0, std::max(0, mapPixelW_ - 1));
                const int clampedY = std::clamp(ty, 0, std::max(0, mapPixelH_ - 1));
                const auto nextMove = std::pair{clampedX, clampedY};
                if (v.moveTarget != nextMove) {
                    v.moveTarget = nextMove;
                    v.waypoints.clear();
                }
                v.chasingAttackTarget = true;
                const unsigned prevTurret = v.turretDir;
                v.turretDir = turnTowardDir32(v.turretDir, desiredTurret, std::max(1u, v.turretStep32));
                if (v.turretDir != prevTurret) emitVehiclePose(i);
                continue;
            }
            if (explicitBldOrder && v.chasingAttackTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                v.chasingAttackTarget = false;
                v.wasMoving = false;
            }
            if (dist > weapon.range || dist < weapon.deadzone) continue;
            if (v.attackMove && !explicitBldOrder) v.attackMoveEngaging = true;
            if (v.fireCooldown > 0) {
                aimVehicleAt(desiredTurret, false, explicitBldOrder);
                continue;
            }
            if (v.hasGunTurnDelay && !v.isCasemate &&
                dirDelta32(v.turretDir, desiredTurret) > v.turretStep32) {
                aimVehicleAt(desiredTurret, false, true);
                continue;
            }
            if (!weapon.secondary && v.gunShotWait > 0 && v.gunSettleTicks > 0) {
                aimVehicleAt(desiredTurret, false, explicitBldOrder);
                continue;
            }
            aimVehicleAt(desiredTurret, true, false);
            applyWeaponShotAndCooldown(v, weapon.secondary, weapon.cooldown);
            const bool isFlameShot = (weapon.secondary ? v.weapon2Flamer : v.weapon1Flamer) ||
                                     weapon.hitType == assets::HitType::Fire;
            const bool heavyShot = !weapon.secondary &&
                (weapon.hitType == assets::HitType::Explosive || weapon.damage >= 180);
            if (!weapon.secondary && weapon.hitType == assets::HitType::Explosive) {
                applyExplosiveSplash(tx, ty, weapon.damage, -1, static_cast<int>(bid));
            }
            damageBuilding(bid, weapon.damage, weapon.hitType, v.owner, changed);
            markVehicleCombat(v);
            markMuzzleFlashReveal(v, weapon.secondary, heavyShot, weapon.damage);
            emitVehiclePose(i, true, tx, ty, weapon.secondary,
                            weapon.sound ? *weapon.sound : std::string_view{},
                            heavyShot, false, false, false, false,
                            !weapon.secondary && v.isRocketArtillery, isFlameShot);
            if (v.isKamikaze) {
                destroyVehicle(i, changed);
                emitVehiclePose(i);
            }
            continue;
        }

        if (!activeTarget && v.attackGround && v.hasPrimary && (v.ammoPerShot1 == 0 || v.ammo > 0)) {
            const auto [gx, gy] = *v.attackGround;
            const double gDist = std::hypot(static_cast<double>(gx) - v.fx, static_cast<double>(gy) - v.fy);
            const unsigned gTurret = vehicleDirFromMapDelta(static_cast<double>(gx) - v.fx, static_cast<double>(gy) - v.fy);
            if (gDist > v.range && v.speedPx > 0.0) {
                const auto nextMove = std::pair{gx, gy};
                if (v.moveTarget != nextMove) {
                    v.moveTarget = nextMove;
                    v.waypoints.clear();
                    v.reverseMove = false;
                }
                const unsigned prevTurret = v.turretDir;
                v.turretDir = turnTowardDir32(v.turretDir, gTurret, std::max(1u, v.turretStep32));
                if (v.turretDir != prevTurret) emitVehiclePose(i);
                continue;
            }
            v.moveTarget.reset();
            v.waypoints.clear();
            v.reverseMove = false;
            v.wasMoving = false;
            if (gDist >= v.deadzone && gDist <= v.range) {
                if (v.fireCooldown > 0) {
                    aimVehicleAt(gTurret, false, false);
                    continue;
                }
                if (v.hasGunTurnDelay && !v.isCasemate &&
                    dirDelta32(v.turretDir, gTurret) > v.turretStep32) {
                    aimVehicleAt(gTurret, false, true);
                    continue;
                }
                if (v.gunShotWait > 0 && v.gunSettleTicks > 0) {
                    aimVehicleAt(gTurret, false, false);
                    continue;
                }
                aimVehicleAt(gTurret, true, false);
                applyWeaponShotAndCooldown(v, false, v.cooldown);
                int impX = gx;
                int impY = gy;
                if (v.isRocketArtillery) {
                    const int ox = static_cast<int>(nextCombatRandom(73u)) - 36;
                    const int oy = static_cast<int>(nextCombatRandom(73u)) - 36;
                    impX = std::clamp(gx + ox, 0, std::max(0, mapPixelW_ - 1));
                    impY = std::clamp(gy + oy, 0, std::max(0, mapPixelH_ - 1));
                } else if (v.accuracy1 > 0) {
                    const double vet = static_cast<double>(std::clamp(v.expa, 0, 0x8000)) / 32768.0;
                    const double effAcc = static_cast<double>(v.accuracy1) -
                                          vet * static_cast<double>(std::max(0, v.accuracy1 - v.accuracy1Min));
                    const int supExtra = v.isSuppressed() ? 8 : 0;
                    const int scatterR = std::clamp(static_cast<int>(std::lround(effAcc * 0.25)) + supExtra, 4, 56);
                    const std::uint32_t span = static_cast<std::uint32_t>(scatterR * 2 + 1);
                    const int ox = static_cast<int>(nextCombatRandom(0x7fffu) % span) - scatterR;
                    const int oy = static_cast<int>(nextCombatRandom(0x7fffu) % span) - scatterR;
                    impX = std::clamp(gx + ox, 0, std::max(0, mapPixelW_ - 1));
                    impY = std::clamp(gy + oy, 0, std::max(0, mapPixelH_ - 1));
                }
                const bool isFlameGround = v.weapon1Flamer || v.hitType == assets::HitType::Fire;
                if (isFlameGround) {
                    applyFlameSplash(impX, impY, v.damage, v.owner, -1, static_cast<int>(i), -1, changed, -2);
                } else {
                    applyExplosiveSplash(impX, impY, v.damage, -1, -1);
                }
                for (size_t k = 0; k < vehicles_.size(); ++k) {
                    auto& tv = vehicles_[k];
                    if (!tv.alive || tv.hp <= 0 || !isHostile(v.owner, tv.owner)) continue;
                    const double vd = std::hypot(tv.fx - static_cast<double>(impX), tv.fy - static_cast<double>(impY));
                    if (vd > 48.0) continue;
                    const int dealt = scaleVehicleDamageForDifficulty(
                        tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                                     v.x, v.y, v.damage, v.hitType));
                    tv.hp = std::max(0, tv.hp - dealt);
                    recordHit(tv.owner, tv.group);
                    markVehicleCombat(tv);
                    reactToAttack(tv.owner, tv.group, v.x, v.y, k);
                    if (tv.hp == 0) {
                        destroyVehicle(k, changed);
                        ++v.kills;
                        v.expa = std::min(0x8000, v.expa + 4500);
                    } else {
                        applyVehicleCrewDamage(k, impX, impY, v.damage,
                                               isFlameGround ? assets::HitType::Fire : v.hitType,
                                               changed, -1, static_cast<int>(i));
                        applyVehicleSuppression(k, 24, impX, impY);
                    }
                    emitVehiclePose(k);
                }
                markVehicleCombat(v);
                markMuzzleFlashReveal(v, false, !isFlameGround, v.damage);
                emitVehiclePose(i, true, impX, impY, false, v.sound1, !isFlameGround, false, false, false, false,
                                v.isRocketArtillery, isFlameGround);
                if (v.isKamikaze) {
                    destroyVehicle(i, changed);
                    emitVehiclePose(i);
                }
                if (v.ammoPerShot1 > 0 && v.ammo <= 0) v.attackGround.reset();
            }
            continue;
        }

        if (!activeTarget) continue;

        const auto [targetIsVehicle, tid] = *activeTarget;
        const auto weapon = selectWeapon(v, targetIsVehicle);
        if (!weapon.valid) continue;

        double targetFx = 0.0;
        double targetFy = 0.0;
        int tx = 0;
        int ty = 0;
        if (targetIsVehicle) {
            targetFx = vehicles_[tid].fx;
            targetFy = vehicles_[tid].fy;
            tx = vehicles_[tid].x;
            ty = vehicles_[tid].y;
        } else {
            const auto [tu, tv] = soldierWorld(soldiers_[tid]);
            tx = originX_ + tu - tv;
            ty = (tu + tv) / 2;
            targetFx = static_cast<double>(tx);
            targetFy = static_cast<double>(ty);
        }
        const double dist = std::hypot(targetFx - v.fx, targetFy - v.fy);
        const double effRange = effectiveVehRange(weapon.range, tx, ty);
        const unsigned desiredTurret = vehicleDirFromMapDelta(targetFx - v.fx, targetFy - v.fy);

        if (explicitOrder && dist > effRange && v.speedPx > 0.0) {
            const int clampedX = std::clamp(tx, 0, std::max(0, mapPixelW_ - 1));
            const int clampedY = std::clamp(ty, 0, std::max(0, mapPixelH_ - 1));
            const auto nextMove = std::pair{clampedX, clampedY};
            if (v.moveTarget != nextMove) {
                v.moveTarget = nextMove;
                v.waypoints.clear();
                v.reverseMove = false;
            }
            v.chasingAttackTarget = true;
            const unsigned prevTurret = v.turretDir;
            v.turretDir = turnTowardDir32(v.turretDir, desiredTurret, std::max(1u, v.turretStep32));
            if (v.turretDir != prevTurret) emitVehiclePose(i);
            continue;
        }

        if (explicitOrder && v.chasingAttackTarget) {
            v.moveTarget.reset();
            v.waypoints.clear();
            v.chasingAttackTarget = false;
            v.wasMoving = false;
        }

        if (dist > effRange || dist < weapon.deadzone) continue;

        if (v.attackMove && !explicitOrder) {
            v.attackMoveEngaging = true;
        }

        if (v.fireCooldown > 0) {
            aimVehicleAt(desiredTurret, false, explicitOrder);
            continue;
        }
        if (v.hasGunTurnDelay && !v.isCasemate &&
            dirDelta32(v.turretDir, desiredTurret) > v.turretStep32) {
            aimVehicleAt(desiredTurret, false, true);
            continue;
        }
        if (!weapon.secondary && v.gunShotWait > 0 && v.gunSettleTicks > 0) {
            aimVehicleAt(desiredTurret, false, explicitOrder);
            continue;
        }

        aimVehicleAt(desiredTurret, true, false);
        applyWeaponShotAndCooldown(v, weapon.secondary, weapon.cooldown);

        const bool heavyShot = !weapon.secondary &&
            (weapon.hitType == assets::HitType::Explosive || weapon.damage >= 180);
        const bool isFlameShot = (weapon.secondary ? v.weapon2Flamer : v.weapon1Flamer) ||
                                 weapon.hitType == assets::HitType::Fire;
        const bool heSplash = !weapon.secondary && weapon.hitType == assets::HitType::Explosive;
        if (heSplash) {
            applyExplosiveSplash(tx, ty, weapon.damage, targetIsVehicle ? -1 : static_cast<int>(tid), -1);
        } else if (isFlameShot) {
            applyFlameSplash(tx, ty, weapon.damage, v.owner, -1, static_cast<int>(i),
                             targetIsVehicle ? -1 : static_cast<int>(tid), changed,
                             targetIsVehicle ? static_cast<int>(tid) : -1);
        }
        if (targetIsVehicle) {
            auto& tv = vehicles_[tid];
            const int dealt = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             v.x, v.y, weapon.damage, weapon.hitType));
            tv.hp = std::max(0, tv.hp - dealt);
            if (dealt > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
                tv.expa = std::min(0x8000, tv.expa + std::min(200, dealt * tv.expaOnHit));
            }
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            reactToAttack(tv.owner, tv.group, v.x, v.y, tid);
            if (tv.hp == 0) {
                destroyVehicle(tid, changed);
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 4500);
            } else {
                applyVehicleCrewDamage(tid, v.x, v.y, weapon.damage, weapon.hitType, changed, -1, static_cast<int>(i));
                if (weapon.hitType == assets::HitType::Explosive || weapon.hitType == assets::HitType::Fire ||
                    weapon.damage >= 180) {
                    applyVehicleSuppression(tid, weapon.hitType == assets::HitType::Explosive ? 24 : 16, v.x, v.y);
                }
            }
            emitVehiclePose(tid);
        } else {
            auto& ts = soldiers_[tid];
            ts.hp = std::max(0, ts.hp - weapon.damage);
            if (ts.hp > 0 && ts.expaOnHit > 0) {
                ts.expa = std::min(0x8000, ts.expa + ts.expaOnHit);
            }
            if (ts.hp > 0 && (weapon.hitType == assets::HitType::Machine ||
                              weapon.hitType == assets::HitType::Explosive)) {
                applySuppression(tid, weapon.hitType == assets::HitType::Explosive ? 25 : 20);
            }
            recordHit(ts.owner, ts.group);
            reactToAttack(ts.owner, ts.group, v.x, v.y);
            const auto [tu, tv] = soldierWorld(ts);
            if (ts.hp == 0) {
                ++v.kills;
                v.expa = std::min(0x8000, v.expa + 1500);
                if (!vehicleAtCell(ts.cell)) grid_.setOccupied(ts.cell, false);
                ts.motion.reset();
                ts.goal.reset();
                ts.route.clear();
                ts.cursor = 0;
                ts.queuedGoals.clear();
                ts.attackMove = false;
                ts.attackTarget.reset();
                ts.attackVehicleTarget.reset();
                ts.autoTarget.reset();
                ts.binoculars = false;
                ts.spottedMask = 0;
                fogDirty_ = true;
            }
            const bool tVis = !fog_ || isAlliedWithPlayer(ts.owner) || (ts.spottedMask & 1u) != 0;
            pushOrUpdateSoldierPose(changed, {
                tid, originX_ + tu - tv, (tu + tv) / 2, ts.direction, 0, false,
                ts.hp, ts.hp > 0, ts.owner, ts.group, false, tVis
            });
        }
        markVehicleCombat(v);
        markMuzzleFlashReveal(v, weapon.secondary, heavyShot, weapon.damage);
        emitVehiclePose(i, true, tx, ty, weapon.secondary,
                        weapon.sound ? *weapon.sound : std::string_view{},
                        heavyShot, targetIsVehicle, false, false, false,
                        !weapon.secondary && v.isRocketArtillery, isFlameShot);
        if (v.isKamikaze) {
            destroyVehicle(i, changed);
            emitVehiclePose(i);
        }
    }
}

void TutorialUnits::stepMedicHeal(std::vector<SoldierPose>& changed) {
    constexpr std::int64_t kHealRadiusSq = 96 * 96;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        const auto& s = soldiers_[i];
        if (!s.isMedic || s.hp <= 0 || s.embarked) continue;
        if (s.garrisonBuilding) {
            const size_t bIdx = *s.garrisonBuilding;
            if (bIdx < buildings_.size() && !buildings_[bIdx].destroyed) {
                const auto& b = buildings_[bIdx];
                std::optional<size_t> bestOcc;
                int lowestHp = 1000000;
                for (size_t occId : b.occupants) {
                    if (occId >= soldiers_.size()) continue;
                    const auto& other = soldiers_[occId];
                    if (other.owner != s.owner || other.hp <= 0 || other.hp >= other.maxHp) continue;
                    if (other.hp < lowestHp) {
                        lowestHp = other.hp;
                        bestOcc = occId;
                    }
                }
                if (bestOcc) {
                    auto& target = soldiers_[*bestOcc];
                    target.hp = std::min(target.maxHp, target.hp + 8);
                    pushOrUpdateSoldierPose(changed, {
                        *bestOcc, b.x, b.y, target.direction, 0, false, target.hp, true,
                        target.owner, target.group, false, false, 0, 0, true
                    });
                }
            }
            continue;
        }
        if (s.motion || !s.route.empty()) continue;
        const auto [su, sv] = soldierWorld(s);
        std::optional<size_t> bestIdx;
        std::int64_t bestD2 = kHealRadiusSq + 1;
        for (size_t j = 0; j < soldiers_.size(); ++j) {
            const auto& other = soldiers_[j];
            if (other.owner != s.owner || other.hp <= 0 || other.embarked || other.garrisonBuilding || other.hp >= other.maxHp) continue;
            const auto [ou, ov] = soldierWorld(other);
            const std::int64_t du = std::int64_t(ou) - su;
            const std::int64_t dv = std::int64_t(ov) - sv;
            const std::int64_t d2 = du * du + dv * dv;
            if (d2 <= kHealRadiusSq && d2 < bestD2) {
                bestD2 = d2;
                bestIdx = j;
            }
        }
        if (!bestIdx) {
            if (autoEngage_ && !s.holdFire && !s.goal && !s.healTarget &&
                !s.boardVehicleTarget && !s.captureVehicleTarget && !s.garrisonTarget &&
                !s.escortVehicleTarget && !s.escortSoldierTarget && !s.patrolOrigin &&
                !s.disarmMineTarget && s.queuedGoals.empty()) {
                constexpr std::int64_t kAutoTriageRadiusSq = 280 * 280;
                std::optional<size_t> autoIdx;
                std::int64_t autoBestD2 = kAutoTriageRadiusSq + 1;
                int autoTx = 0;
                int autoTy = 0;
                for (size_t j = 0; j < soldiers_.size(); ++j) {
                    if (j == i) continue;
                    const auto& other = soldiers_[j];
                    if (other.owner != s.owner || other.hp <= 0 || other.embarked ||
                        other.garrisonBuilding || other.hp >= other.maxHp) {
                        continue;
                    }
                    const auto [ou, ov] = soldierWorld(other);
                    const std::int64_t du = std::int64_t(ou) - su;
                    const std::int64_t dv = std::int64_t(ov) - sv;
                    const std::int64_t d2 = du * du + dv * dv;
                    if (d2 <= kAutoTriageRadiusSq && d2 < autoBestD2) {
                        autoBestD2 = d2;
                        autoIdx = j;
                        autoTx = originX_ + ou - ov;
                        autoTy = (ou + ov) / 2;
                    }
                }
                if (autoIdx && orderSoldierInternal(i, autoTx, autoTy) &&
                    soldiers_[i].goal && !sameTile(*soldiers_[i].goal, soldiers_[i].cell)) {
                    soldiers_[i].healTarget = *autoIdx;
                }
            }
            const int msx = originX_ + su - sv;
            const int msy = (su + sv) / 2;
            for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
                auto& tv = vehicles_[vi];
                if (!tv.alive || tv.hp <= 0 || !tv.hasCrew || tv.owner != s.owner || tv.crewHp >= tv.maxCrewHp) {
                    continue;
                }
                if (std::hypot(tv.fx - static_cast<double>(msx), tv.fy - static_cast<double>(msy)) <= 96.0) {
                    const int healAmt = tv.crewCanBeHealed ? 70 : 35;
                    tv.crewHp = std::min(tv.maxCrewHp, tv.crewHp + healAmt);
                    emitVehiclePose(vi, false, 0, 0, false, {}, false, false, true, false);
                    break;
                }
            }
            continue;
        }
        auto& target = soldiers_[*bestIdx];
        target.hp = std::min(target.maxHp, target.hp + 8);
        bool updatedExisting = false;
        for (auto& pose : changed) {
            if (pose.index == *bestIdx) {
                pose.hp = target.hp;
                updatedExisting = true;
            }
        }
        if (!updatedExisting) {
            const auto [tu, tv] = soldierWorld(target);
            const unsigned dir = target.motion ? target.motion->direction : target.direction;
            const unsigned prog = target.motion ? target.motion->progress : 0u;
            const bool walking = target.motion ? target.motion->running : false;
            const bool vis = !fog_ || target.owner == 0 || (target.spottedMask & 1u) != 0;
            changed.push_back({*bestIdx, originX_ + tu - tv, (tu + tv) / 2, dir, prog, walking,
                               target.hp, true, target.owner, target.group, false, vis});
        }
    }
}

void TutorialUnits::stepSupplyAndRepair(std::vector<SoldierPose>& changed) {
    constexpr double kSupportRadius = 140.0;
    const auto pushHealed = [&](size_t idx, const Soldier& s) {
        for (auto& pose : changed) {
            if (pose.index == idx) {
                pose.hp = s.hp;
                pose.ammo = s.ammo;
                pose.atGrenades = s.atGrenades;
                pose.mineAmmo = s.mineAmmo;
                return;
            }
        }
        const auto [tu, tv] = soldierWorld(s);
        const unsigned dir = s.motion ? s.motion->direction : s.direction;
        const unsigned prog = s.motion ? s.motion->progress : 0u;
        const bool walking = s.motion ? s.motion->running : false;
        const bool vis = !fog_ || s.owner == 0 || (s.spottedMask & 1u) != 0;
        changed.push_back({idx, originX_ + tu - tv, (tu + tv) / 2, dir, prog, walking,
                           s.hp, true, s.owner, s.group, false, vis, 0, 0, false,
                           s.expa, s.kills, false, s.atGrenades, false, isSoldierCamouflaged(s),
                           s.mineAmmo, s.ammo, false});
    };

    const auto veteranRateMult = [](int baseDelay, int maxDelay, int expa) {
        if (baseDelay <= 0 || maxDelay <= 0 || maxDelay >= baseDelay) return 1.0;
        const double vetFrac = static_cast<double>(std::clamp(expa, 0, 0x8000)) / 32768.0;
        const double effDelay = static_cast<double>(baseDelay) -
                                vetFrac * static_cast<double>(baseDelay - maxDelay);
        return static_cast<double>(baseDelay) / std::max(1.0, effDelay);
    };
    constexpr double kAutoSupportRadius = 280.0;
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.alive || v.hp <= 0 || (v.moveTarget && !v.patrolOrigin)) continue;
        if (v.hasCrew && v.owner != 3 && v.canSelfRepair && !v.isKamikaze && !v.towedBy &&
            !v.moveTarget && (v.lastCombatTick == 0u || tick_ >= v.lastCombatTick + 125u)) {
            const int minMobilityHp = v.moveDamageHp > 0 ? std::min(v.maxHp, v.moveDamageHp + 1) : 0;
            const int emergencyCap = std::clamp(std::max(minMobilityHp, (v.maxHp * 35) / 100), 1, v.maxHp);
            if (v.hp < emergencyCap) {
                v.hp = std::min(emergencyCap, v.hp + std::max(6, v.maxHp / 45));
                emitVehiclePose(i, false, 0, 0, false, {}, false, false, true, false);
            }
        }
        const bool canAutoDispatch =
            autoEngage_ && !v.holdFire && v.hasCrew && v.speedPx > 0.0 && !v.towedBy &&
            !v.moveTarget && v.waypoints.empty() && v.queuedOrders.empty() &&
            !v.supportTargetVehicle && !v.supportTargetBuilding && !v.supportTargetSoldier &&
            !v.escortVehicleTarget && !v.escortSoldierTarget && !v.patrolOrigin &&
            !v.pendingTowGun && !v.facingTargetDir;
        if (v.isSupply) {
            int repairsLeft = 1;
            int resuppliesLeft = 2;
            const bool canResupplyAmmo = !v.hasFiniteSupply || v.ammo > 0;
            const bool canRepairParts = !v.hasFiniteSupply || v.ammo2 > 0;
            const double supplyMult = veteranRateMult(v.ammoRegenDelay, v.ammoRegenDelayMax, v.expa);
            bool truckStockChanged = false;
            bool didAnyService = false;
            if (!v.isCrate && v.hasFiniteSupply) {
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == i) continue;
                    auto& crate = vehicles_[j];
                    if (!crate.alive || crate.hp <= 0 || !crate.isCrate) continue;
                    if (crate.owner != v.owner && crate.owner != 3) continue;
                    if (std::hypot(crate.fx - v.fx, crate.fy - v.fy) > kSupportRadius) continue;
                    bool didCrateTransfer = false;
                    if (v.ammo < v.maxAmmo && crate.ammo > 0) {
                        const int xfer = std::min({500, crate.ammo, v.maxAmmo - v.ammo});
                        crate.ammo -= xfer;
                        v.ammo += xfer;
                        didCrateTransfer = true;
                    }
                    if (v.ammo2 < v.maxAmmo2 && crate.ammo2 > 0) {
                        const int xfer2 = std::min({500, crate.ammo2, v.maxAmmo2 - v.ammo2});
                        crate.ammo2 -= xfer2;
                        v.ammo2 += xfer2;
                        didCrateTransfer = true;
                    }
                    if (didCrateTransfer) {
                        truckStockChanged = true;
                        emitVehiclePose(j, false, 0, 0, false, {}, false, false, false, true);
                        emitVehiclePose(i, false, 0, 0, false, {}, false, false, false, true);
                    }
                }
            }
            for (size_t j = 0; j < vehicles_.size() && (repairsLeft > 0 || resuppliesLeft > 0); ++j) {
                if (j == i) continue;
                auto& other = vehicles_[j];
                if (!other.alive || other.hp <= 0 || other.owner != v.owner) continue;
                if (std::hypot(other.fx - v.fx, other.fy - v.fy) > kSupportRadius) continue;
                bool didRepair = false;
                bool didResupply = false;
                if (repairsLeft > 0 && !other.isCrate && other.hp < other.maxHp &&
                    (!v.hasFiniteSupply || v.ammo2 > 0)) {
                    const int maxRepairStep = std::max(1, static_cast<int>(std::lround(18.0 * supplyMult)));
                    int gain = std::min(maxRepairStep, other.maxHp - other.hp);
                    if (v.hasFiniteSupply) {
                        const int partsPerHp = other.repairCost > 0 ? other.repairCost : 10;
                        if (v.ammo2 < partsPerHp) {
                            gain = 1;
                            v.ammo2 = 0;
                        } else {
                            gain = std::min(gain, v.ammo2 / partsPerHp);
                            v.ammo2 -= gain * partsPerHp;
                        }
                        truckStockChanged = true;
                    }
                    other.hp += gain;
                    --repairsLeft;
                    didRepair = true;
                    didAnyService = true;
                }
                const bool needsAmmo1 = other.maxAmmo > 0 && other.ammo < other.maxAmmo;
                const bool needsAmmo2 = other.maxAmmo2 > 0 && other.ammo2 < other.maxAmmo2;
                if (resuppliesLeft > 0 && !other.isSupply && (needsAmmo1 || needsAmmo2) &&
                    (!v.hasFiniteSupply || v.ammo > 0)) {
                    const int prevA1 = other.ammo;
                    const int prevA2 = other.ammo2;
                    const int unitCost1 = other.resupplyCost1 > 0 ? other.resupplyCost1 : 15;
                    const int unitCost2 = other.resupplyCost2 > 0 ? other.resupplyCost2 : 2;
                    const int baseStep1 = std::max(1, other.maxAmmo / 6);
                    const int scaledStep1 = std::max(1, static_cast<int>(std::lround(static_cast<double>(baseStep1) * supplyMult)));
                    const int scaledStep2 = std::max(5, static_cast<int>(std::lround(static_cast<double>(std::max(5, other.maxAmmo2 / 6)) * supplyMult)));
                    int add1 = needsAmmo1 ? std::min(other.maxAmmo - other.ammo, scaledStep1) : 0;
                    int add2 = needsAmmo2 ? std::min(other.maxAmmo2 - other.ammo2, scaledStep2) : 0;
                    if (v.hasFiniteSupply && other.resupplyCost1 > 0 && add1 > 1 && v.ammo < add1 * unitCost1) {
                        add1 = std::max(1, v.ammo / unitCost1);
                    }
                    if (add1 > 0) other.ammo += add1;
                    if (add2 > 0) other.ammo2 += add2;
                    const int d1 = std::max(0, other.ammo - prevA1);
                    const int d2 = std::max(0, other.ammo2 - prevA2);
                    if (v.hasFiniteSupply) {
                        const int cost = std::max(1, d1 * unitCost1 + d2 * unitCost2);
                        v.ammo = std::max(0, v.ammo - cost);
                        truckStockChanged = true;
                    }
                    --resuppliesLeft;
                    didResupply = true;
                    didAnyService = true;
                }
                if (didRepair || didResupply) {
                    emitVehiclePose(j, false, 0, 0, false, {}, false, false, didRepair, didResupply);
                }
            }
            if (repairsLeft > 0 && canRepairParts && (!v.hasFiniteSupply || v.ammo2 > 0)) {
                for (size_t bi = 0; bi < buildings_.size() && repairsLeft > 0 && (!v.hasFiniteSupply || v.ammo2 > 0); ++bi) {
                    auto& b = buildings_[bi];
                    if (b.destroyed || b.hp >= b.maxHp) continue;
                    if (b.owner >= 0 && b.owner != v.owner) continue;
                    const double bReach = kSupportRadius + static_cast<double>(std::max(b.width, b.height)) * 0.5;
                    if (std::hypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) > bReach) continue;
                    const int maxBldStep = std::max(1, static_cast<int>(std::lround(25.0 * supplyMult)));
                    int gain = std::min(maxBldStep, b.maxHp - b.hp);
                    if (v.hasFiniteSupply) {
                        if (v.ammo2 < 10) {
                            gain = 1;
                            v.ammo2 = 0;
                        } else {
                            gain = std::min(gain, v.ammo2 / 10);
                            v.ammo2 -= gain * 10;
                        }
                        truckStockChanged = true;
                    }
                    b.hp += gain;
                    didAnyService = true;
                    emitBuildingUpdate(bi);
                    emitVehiclePose(i, false, 0, 0, false, {}, false, false, true, false);
                    --repairsLeft;
                }
            }
            auto resupplySoldier = [&](Soldier& s) -> std::pair<bool, bool> {
                if (v.hasFiniteSupply && v.ammo <= 0) return {false, false};
                const bool canCarryGrenades = !s.isAntiVehicle && !s.isMedic && !s.isOfficer && !s.isMortar;
                const int maxInfAmmo = std::max(1, s.desc.maxAmmo);
                const int ammoStep = std::max(1, (maxInfAmmo + 3) / 4);
                const bool needsAmmo = s.ammo < maxInfAmmo;
                const bool needsGrenade = canCarryGrenades && s.atGrenades < 2;
                const int maxMines = s.maxMineAmmo > 0 ? s.maxMineAmmo : 5;
                const bool needsMine = s.canLayMines && s.mineAmmo < maxMines;
                if (!needsAmmo && !needsGrenade && !needsMine) return {false, false};
                const int prevSA = s.ammo;
                if (needsAmmo) s.ammo = std::min(maxInfAmmo, s.ammo + ammoStep);
                if (needsGrenade) s.atGrenades = std::min(2, s.atGrenades + 1);
                if (needsMine) s.mineAmmo = std::min(maxMines, s.mineAmmo + 1);
                if (v.hasFiniteSupply) {
                    const int da = std::max(0, s.ammo - prevSA);
                    const int unitCost = s.resupplyCost > 0 ? s.resupplyCost : 3;
                    const int cost = std::max(1, da * unitCost + ((needsGrenade || needsMine) ? 25 : 0));
                    v.ammo = std::max(0, v.ammo - cost);
                    truckStockChanged = true;
                }
                return {true, needsGrenade || needsMine};
            };
            int infLeft = 3;
            bool didInfResupply = false;
            if (canResupplyAmmo) {
                for (size_t sIdx = 0; sIdx < soldiers_.size() && infLeft > 0; ++sIdx) {
                    auto& s = soldiers_[sIdx];
                    if (s.owner != v.owner || s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
                    const auto [su, sv] = soldierWorld(s);
                    const int sx = originX_ + su - sv;
                    const int sy = (su + sv) / 2;
                    if (std::hypot(double(sx) - v.fx, double(sy) - v.fy) > kSupportRadius) continue;
                    const auto [didRes, gearChanged] = resupplySoldier(s);
                    (void)gearChanged;
                    if (!didRes) continue;
                    pushHealed(sIdx, s);
                    didInfResupply = true;
                    didAnyService = true;
                    --infLeft;
                }
                for (size_t bi = 0; bi < buildings_.size() && infLeft > 0; ++bi) {
                    const auto& b = buildings_[bi];
                    if (b.destroyed || b.owner != v.owner || b.occupants.empty()) continue;
                    const double bReach = kSupportRadius + static_cast<double>(std::max(b.width, b.height)) * 0.5;
                    if (std::hypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) > bReach) continue;
                    for (size_t sIdx : b.occupants) {
                        if (infLeft <= 0) break;
                        if (sIdx >= soldiers_.size()) continue;
                        auto& s = soldiers_[sIdx];
                        if (s.owner != v.owner || s.hp <= 0) continue;
                        const auto [didRes, gearChanged] = resupplySoldier(s);
                        (void)gearChanged;
                        if (!didRes) continue;
                        pushOrUpdateSoldierPose(changed, {
                            sIdx, b.x, b.y, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false,
                            0, 0, true, s.expa, s.kills, false, s.atGrenades, false, false, s.mineAmmo, s.ammo
                        });
                        didInfResupply = true;
                        didAnyService = true;
                        --infLeft;
                    }
                }
            }
            if (didAnyService && v.expaReload > 0) {
                const double vetFrac = static_cast<double>(std::clamp(v.expa, 0, 0x8000)) / 32768.0;
                const double xpUnits = static_cast<double>(v.expaReload) +
                                       vetFrac * static_cast<double>(v.expaReloadMax - v.expaReload);
                v.expa = std::min(0x8000, v.expa + std::max(1, static_cast<int>(std::lround(xpUnits * 64.0))));
                truckStockChanged = true;
            }
            if (didInfResupply) {
                emitVehiclePose(i, false, 0, 0, false, {}, false, false, false, true);
            } else if (truckStockChanged) {
                emitVehiclePose(i);
            }
            if (canAutoDispatch && repairsLeft == 1 && resuppliesLeft == 2 && !didInfResupply) {
                std::optional<size_t> bestVeh;
                double bestDist = kAutoSupportRadius + 1.0;
                for (size_t j = 0; j < vehicles_.size(); ++j) {
                    if (j == i) continue;
                    const auto& other = vehicles_[j];
                    if (!other.alive || other.hp <= 0 || other.owner != v.owner) continue;
                    const bool canFixOther = !other.isCrate && other.hp < other.maxHp && (!v.hasFiniteSupply || v.ammo2 > 0);
                    const bool otherNeedsAmmo = !other.isSupply &&
                        ((other.maxAmmo > 0 && other.ammo < other.maxAmmo) ||
                         (other.maxAmmo2 > 0 && other.ammo2 < other.maxAmmo2));
                    const bool canFeedOther = otherNeedsAmmo && (!v.hasFiniteSupply || v.ammo > 0);
                    if (!canFixOther && !canFeedOther) {
                        continue;
                    }
                    const double d = std::hypot(other.fx - v.fx, other.fy - v.fy);
                    if (d <= kAutoSupportRadius && d < bestDist) {
                        bestDist = d;
                        bestVeh = j;
                    }
                }
                if (bestVeh) {
                    const auto& t = vehicles_[*bestVeh];
                    const auto approach = approachPointNear(v.fx, v.fy, t.fx, t.fy, 64.0, mapPixelW_, mapPixelH_);
                    moveVehicleInternal(v, approach.first, approach.second);
                    v.supportTargetVehicle = *bestVeh;
                }
            }
        }
        if (v.isAmbulance) {
            int healsLeft = 2;
            const double healMult = veteranRateMult(v.healDelay, v.healDelayMax, v.expa);
            const int infHeal = std::max(1, static_cast<int>(std::lround(12.0 * healMult)));
            const int crewHeal = std::max(1, static_cast<int>(std::lround(45.0 * healMult)));
            bool didAmbulanceHeal = false;
            for (size_t sIdx = 0; sIdx < soldiers_.size() && healsLeft > 0; ++sIdx) {
                auto& s = soldiers_[sIdx];
                if (s.owner != v.owner || s.hp <= 0 || s.embarked || s.garrisonBuilding || s.hp >= s.maxHp) continue;
                const auto [su, sv] = soldierWorld(s);
                const int sx = originX_ + su - sv;
                const int sy = (su + sv) / 2;
                if (std::hypot(double(sx) - v.fx, double(sy) - v.fy) > kSupportRadius) continue;
                s.hp = std::min(s.maxHp, s.hp + infHeal);
                pushHealed(sIdx, s);
                --healsLeft;
                didAmbulanceHeal = true;
            }
            for (size_t bi = 0; bi < buildings_.size() && healsLeft > 0; ++bi) {
                const auto& b = buildings_[bi];
                if (b.destroyed || b.owner != v.owner || b.occupants.empty()) continue;
                const double bReach = kSupportRadius + static_cast<double>(std::max(b.width, b.height)) * 0.5;
                if (std::hypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) > bReach) continue;
                for (size_t sIdx : b.occupants) {
                    if (healsLeft <= 0) break;
                    if (sIdx >= soldiers_.size()) continue;
                    auto& s = soldiers_[sIdx];
                    if (s.owner != v.owner || s.hp <= 0 || s.hp >= s.maxHp) continue;
                    s.hp = std::min(s.maxHp, s.hp + infHeal);
                    pushOrUpdateSoldierPose(changed, {
                        sIdx, b.x, b.y, s.direction, 0, false, s.hp, true, s.owner, s.group, false, false,
                        0, 0, true
                    });
                    --healsLeft;
                    didAmbulanceHeal = true;
                }
            }
            if (healsLeft > 0) {
                for (size_t j = 0; j < vehicles_.size() && healsLeft > 0; ++j) {
                    if (j == i) continue;
                    auto& other = vehicles_[j];
                    if (!other.alive || other.hp <= 0 || !other.hasCrew || other.owner != v.owner ||
                        other.crewHp >= other.maxCrewHp) {
                        continue;
                    }
                    if (std::hypot(other.fx - v.fx, other.fy - v.fy) > kSupportRadius) continue;
                    other.crewHp = std::min(other.maxCrewHp, other.crewHp + crewHeal);
                    emitVehiclePose(j, false, 0, 0, false, {}, false, false, true, false);
                    --healsLeft;
                    didAmbulanceHeal = true;
                }
            }
            if (didAmbulanceHeal) {
                v.expa = std::min(0x8000, v.expa + 128);
                emitVehiclePose(i);
            }
            if (canAutoDispatch && healsLeft == 2) {
                std::optional<size_t> bestSoldier;
                double bestDist = kAutoSupportRadius + 1.0;
                double bestX = 0.0;
                double bestY = 0.0;
                for (size_t sIdx = 0; sIdx < soldiers_.size(); ++sIdx) {
                    const auto& s = soldiers_[sIdx];
                    if (s.owner != v.owner || s.hp <= 0 || s.embarked || s.garrisonBuilding || s.hp >= s.maxHp) {
                        continue;
                    }
                    const auto [su, sv] = soldierWorld(s);
                    const double sx = static_cast<double>(originX_ + su - sv);
                    const double sy = static_cast<double>(su + sv) * 0.5;
                    const double d = std::hypot(sx - v.fx, sy - v.fy);
                    if (d <= kAutoSupportRadius && d < bestDist) {
                        bestDist = d;
                        bestSoldier = sIdx;
                        bestX = sx;
                        bestY = sy;
                    }
                }
                if (bestSoldier) {
                    const auto approach = approachPointNear(v.fx, v.fy, bestX, bestY, 56.0, mapPixelW_, mapPixelH_);
                    moveVehicleInternal(v, approach.first, approach.second);
                    v.supportTargetSoldier = *bestSoldier;
                }
            }
        }
    }
}

void TutorialUnits::stepSupportApproach() {
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (!s.healTarget) continue;
        const size_t tIdx = *s.healTarget;
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || !s.isMedic || tIdx >= soldiers_.size()) {
            s.healTarget.reset();
            continue;
        }
        const auto& t = soldiers_[tIdx];
        if (t.owner != s.owner || t.hp <= 0 || t.embarked || t.garrisonBuilding || t.hp >= t.maxHp) {
            s.healTarget.reset();
            continue;
        }
        const auto [su, sv] = soldierWorld(s);
        const auto [tu, tv] = soldierWorld(t);
        const double worldDist = std::hypot(static_cast<double>(su - tu), static_cast<double>(sv - tv));
        if (worldDist <= 84.0) {
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.planning = {};
            s.needsRoute = false;
            s.healTarget.reset();
            continue;
        }
        if (!s.motion &&
            (!s.goal || (std::abs(s.goal->x / 2 - t.cell.x / 2) + std::abs(s.goal->y / 2 - t.cell.y / 2) > 3))) {
            const int tx = originX_ + tu - tv;
            const int ty = (tu + tv) / 2;
            if (orderSoldierInternal(i, tx, ty) && s.goal && !sameTile(*s.goal, s.cell)) {
                s.healTarget = tIdx;
            } else {
                s.healTarget.reset();
            }
        }
    }

    const auto haltVehicleSupport = [&](size_t idx, Vehicle& veh) {
        veh.moveTarget.reset();
        veh.waypoints.clear();
        veh.queuedOrders.clear();
        veh.supportTargetVehicle.reset();
        veh.supportTargetBuilding.reset();
        veh.supportTargetSoldier.reset();
        if (veh.wasMoving) {
            veh.wasMoving = false;
            emitVehiclePose(idx);
        }
    };

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.supportTargetVehicle && !v.supportTargetBuilding && !v.supportTargetSoldier) continue;
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy) {
            v.supportTargetVehicle.reset();
            v.supportTargetBuilding.reset();
            v.supportTargetSoldier.reset();
            continue;
        }
        if (v.supportTargetVehicle) {
            const size_t tIdx = *v.supportTargetVehicle;
            if (!v.isSupply || tIdx >= vehicles_.size()) {
                v.supportTargetVehicle.reset();
                continue;
            }
            const auto& t = vehicles_[tIdx];
            if (!t.alive || t.hp <= 0 || t.owner != v.owner ||
                !needsSupplyService(t.hp, t.maxHp, t.ammo, t.maxAmmo, t.ammo2, t.maxAmmo2)) {
                v.supportTargetVehicle.reset();
                continue;
            }
            const double dist = std::hypot(t.fx - v.fx, t.fy - v.fy);
            if (dist <= 120.0) {
                haltVehicleSupport(i, v);
                continue;
            }
            if (!v.moveTarget ||
                std::hypot(t.fx - static_cast<double>(v.moveTarget->first),
                           t.fy - static_cast<double>(v.moveTarget->second)) > 96.0) {
                const auto approach = approachPointNear(v.fx, v.fy, t.fx, t.fy, 64.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, approach.first, approach.second);
                v.supportTargetVehicle = tIdx;
            }
        } else if (v.supportTargetBuilding) {
            const size_t bIdx = *v.supportTargetBuilding;
            if ((!v.isSupply && !v.isAmbulance) || bIdx >= buildings_.size()) {
                v.supportTargetBuilding.reset();
                continue;
            }
            const auto& b = buildings_[bIdx];
            if (b.destroyed || (b.owner >= 0 && b.owner != v.owner)) {
                v.supportTargetBuilding.reset();
                continue;
            }
            bool needsBld = v.isSupply && (b.hp < b.maxHp);
            if (b.owner == v.owner) {
                for (size_t sIdx : b.occupants) {
                    if (sIdx >= soldiers_.size()) continue;
                    const auto& occ = soldiers_[sIdx];
                    if (occ.owner != v.owner || occ.hp <= 0) continue;
                    if (v.isAmbulance && occ.hp < occ.maxHp) needsBld = true;
                    if (v.isSupply) {
                        const bool canCarryGrenades = !occ.isAntiVehicle && !occ.isMedic && !occ.isOfficer && !occ.isMortar;
                        const int maxInfAmmo = std::max(1, occ.desc.maxAmmo);
                        const int occMaxMines = occ.maxMineAmmo > 0 ? occ.maxMineAmmo : 5;
                        if (occ.ammo < maxInfAmmo || (canCarryGrenades && occ.atGrenades < 2) ||
                            (occ.canLayMines && occ.mineAmmo < occMaxMines)) {
                            needsBld = true;
                        }
                    }
                }
            }
            if (!needsBld) {
                v.supportTargetBuilding.reset();
                continue;
            }
            const double bReach = 125.0 + static_cast<double>(std::max(b.width, b.height)) * 0.5;
            if (std::hypot(static_cast<double>(b.x) - v.fx, static_cast<double>(b.y) - v.fy) <= bReach) {
                haltVehicleSupport(i, v);
            }
        } else if (v.supportTargetSoldier) {
            const size_t tIdx = *v.supportTargetSoldier;
            if (!v.isAmbulance || tIdx >= soldiers_.size()) {
                v.supportTargetSoldier.reset();
                continue;
            }
            const auto& t = soldiers_[tIdx];
            if (t.owner != v.owner || t.hp <= 0 || t.embarked || t.garrisonBuilding || t.hp >= t.maxHp) {
                v.supportTargetSoldier.reset();
                continue;
            }
            const auto [tu, tv] = soldierWorld(t);
            const double tx = static_cast<double>(originX_ + tu - tv);
            const double ty = static_cast<double>(tu + tv) * 0.5;
            if (std::hypot(tx - v.fx, ty - v.fy) <= 120.0) {
                haltVehicleSupport(i, v);
                continue;
            }
            if (!v.moveTarget ||
                std::hypot(tx - static_cast<double>(v.moveTarget->first),
                           ty - static_cast<double>(v.moveTarget->second)) > 84.0) {
                const auto approach = approachPointNear(v.fx, v.fy, tx, ty, 56.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, approach.first, approach.second);
                v.supportTargetSoldier = tIdx;
            }
        }
    }
}

void TutorialUnits::stepEscort() {
    const auto resolveEscortPoint = [&](int owner, std::optional<size_t> vehIdx, std::optional<size_t> solIdx,
                                        double& outX, double& outY) -> bool {
        if (vehIdx && *vehIdx < vehicles_.size()) {
            const auto& tv = vehicles_[*vehIdx];
            if (!tv.alive || tv.hp <= 0 || tv.owner != owner) return false;
            outX = tv.fx;
            outY = tv.fy;
            return true;
        }
        if (solIdx && *solIdx < soldiers_.size()) {
            const auto& ts = soldiers_[*solIdx];
            if (ts.owner != owner || ts.hp <= 0 || ts.embarked || ts.garrisonBuilding) return false;
            const auto [tu, tv] = soldierWorld(ts);
            outX = static_cast<double>(originX_ + tu - tv);
            outY = static_cast<double>(tu + tv) * 0.5;
            return true;
        }
        return false;
    };

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (!s.escortVehicleTarget && !s.escortSoldierTarget) continue;
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding) {
            s.escortVehicleTarget.reset();
            s.escortSoldierTarget.reset();
            s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            continue;
        }
        double tx = 0.0, ty = 0.0;
        if (!resolveEscortPoint(s.owner, s.escortVehicleTarget, s.escortSoldierTarget, tx, ty) ||
            (s.escortSoldierTarget && *s.escortSoldierTarget == i)) {
            s.escortVehicleTarget.reset();
            s.escortSoldierTarget.reset();
            s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
            if (!s.goal) s.attackMove = false;
            continue;
        }
        const auto [su, sv] = soldierWorld(s);
        const double sx = static_cast<double>(originX_ + su - sv);
        const double sy = static_cast<double>(su + sv) * 0.5;
        const double dist = std::hypot(tx - sx, ty - sy);
        if (s.attackTarget || s.attackVehicleTarget || s.attackBuildingTarget || s.autoTarget) {
            if (s.autoTarget && !s.attackTarget && !s.attackVehicleTarget && !s.attackBuildingTarget &&
                dist > 260.0) {
                s.autoTarget.reset();
            } else {
                continue;
            }
        }
        if (dist <= 82.0) {
            if (!s.motion && s.goal) {
                s.goal.reset();
                s.route.clear();
                s.cursor = 0;
                s.planning = {};
                s.needsRoute = false;
            }
            continue;
        }
        if (dist > 115.0 && !s.motion) {
            bool goalStale = false;
            if (s.goal) {
                const auto [gu, gv] = InfantryPathGrid::toWorldCenter(*s.goal);
                const double gx = static_cast<double>(originX_ + gu - gv);
                const double gy = static_cast<double>(gu + gv) * 0.5;
                goalStale = std::hypot(tx - gx, ty - gy) > 72.0;
            }
            if (goalStale || (!s.goal && ((tick_ + i) % 6u == 0u))) {
                const auto savedVeh = s.escortVehicleTarget;
                const auto savedSol = s.escortSoldierTarget;
                const auto ap = approachPointNear(sx, sy, tx, ty, 52.0, mapPixelW_, mapPixelH_);
                (void)orderSoldierInternal(i, ap.first, ap.second);
                s.queuedGoals.clear();
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.escortVehicleTarget = savedVeh;
                s.escortSoldierTarget = savedSol;
                s.attackMove = true;
            }
        }
    }

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (!v.escortVehicleTarget && !v.escortSoldierTarget) continue;
        if (!v.alive || v.hp <= 0 || v.speedPx <= 0.0 || v.towedBy) {
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            continue;
        }
        double tx = 0.0, ty = 0.0;
        if (!resolveEscortPoint(v.owner, v.escortVehicleTarget, v.escortSoldierTarget, tx, ty) ||
            (v.escortVehicleTarget && (*v.escortVehicleTarget == i || v.towedVehicle == v.escortVehicleTarget))) {
            v.escortVehicleTarget.reset();
            v.escortSoldierTarget.reset();
            if (!v.moveTarget) v.attackMove = false;
            continue;
        }
        const double dist = std::hypot(tx - v.fx, ty - v.fy);
        if (v.attackMoveEngaging) {
            if (dist > 300.0) {
                v.attackMoveEngaging = false;
            } else {
                continue;
            }
        }
        if (dist <= 92.0) {
            if (v.moveTarget) {
                v.moveTarget.reset();
                v.waypoints.clear();
                if (v.wasMoving) {
                    v.wasMoving = false;
                    emitVehiclePose(i);
                }
            }
            continue;
        }
        if (dist > 125.0) {
            const bool goalStale = v.moveTarget &&
                std::hypot(tx - static_cast<double>(v.moveTarget->first),
                           ty - static_cast<double>(v.moveTarget->second)) > 96.0;
            if (goalStale || (!v.moveTarget && ((tick_ + i) % 6u == 0u))) {
                const auto savedVeh = v.escortVehicleTarget;
                const auto savedSol = v.escortSoldierTarget;
                const auto ap = approachPointNear(v.fx, v.fy, tx, ty, 76.0, mapPixelW_, mapPixelH_);
                moveVehicleInternal(v, ap.first, ap.second);
                v.escortVehicleTarget = savedVeh;
                v.escortSoldierTarget = savedSol;
                v.attackMove = true;
            }
        }
    }
}

std::vector<SoldierPose> TutorialUnits::step() {
    ++tick_;
    std::vector<SoldierPose> changed;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& soldier = soldiers_[i];
        if (soldier.hp <= 0 || soldier.embarked || soldier.garrisonBuilding) continue;
        if (soldier.planning.backoff) --soldier.planning.backoff;
        if (!soldier.motion) {
            if (soldier.autoTarget) continue;
            if (!startNext(soldier)) continue;
        } else {
            soldier.motion->advance(soldier.crouched, randomState_, soldier.stepProgressBase());
            if (!soldier.motion->running && !soldier.autoTarget) startNext(soldier);
        }
        const auto& motion = *soldier.motion;
        const auto [u,v] = motion.position();
        changed.push_back({i,originX_+u-v,(u+v)/2,motion.direction,motion.progress,motion.running,soldier.hp,true,soldier.owner,soldier.group});
        if (!motion.running) {
            soldier.motion.reset();
            if (soldier.cursor >= soldier.route.size()) {
                soldier.route.clear();
                soldier.cursor = 0;
            }
        }
    }
    // The unit dispatcher 0x10055ff0 performs the pathfinding sweep after the unit updates.
    refreshRoutes();
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0 || s.embarked || s.garrisonBuilding || s.motion || s.goal || !s.route.empty()) continue;
        if (!s.queuedGoals.empty()) {
            const bool keepAttackMove = s.attackMove;
            while (!s.queuedGoals.empty()) {
                const auto [gx, gy] = s.queuedGoals.front();
                s.queuedGoals.pop_front();
                if (orderSoldierInternal(i, gx, gy)) {
                    s.attackMove = keepAttackMove;
                    break;
                }
            }
            if (!s.goal && s.queuedGoals.empty()) s.attackMove = false;
        } else if (s.patrolOrigin && s.patrolDest && !s.autoTarget) {
            s.patrolToDest = !s.patrolToDest;
            const auto nextPt = s.patrolToDest ? *s.patrolDest : *s.patrolOrigin;
            if (orderSoldierInternal(i, nextPt.first, nextPt.second)) {
                s.attackMove = true;
            } else {
                s.patrolOrigin.reset();
                s.patrolDest.reset();
                s.attackMove = false;
            }
        } else if (s.attackMove && !s.autoTarget && !s.escortVehicleTarget && !s.escortSoldierTarget) {
            s.attackMove = false;
        }
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.binoculars && (s.motion || !s.route.empty())) {
            s.binoculars = false;
            fogDirty_ = true;
        }
        if (s.isSniper && s.crouched && s.lastFireTick > 0 && tick_ >= s.lastFireTick &&
            tick_ - s.lastFireTick == 35u) {
            fogDirty_ = true;
        }
        if (s.suppressionTicks > 0) {
            const int decay = (s.hp > 0) ? officerAuraDecay(i) : 1;
            s.suppressionTicks = std::max(0, s.suppressionTicks - decay);
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        if (v.suppressionTicks > 0) {
            const int decay = (v.alive && v.hp > 0 && v.hasCrew)
                ? officerAuraDecayAt(v.owner, v.fx, v.fy) : 1;
            v.suppressionTicks = std::max(0, v.suppressionTicks - decay);
        }
    }
    stepSupportApproach();
    stepEscort();
    stepTowing();
    stepVehicles(changed);
    stepMines(changed);
    stepBoarding(changed);
    stepCaptures(changed);
    stepGarrisons(changed);
    if (fog_) {
        stepCombat(changed);
    } else {
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.embarked || s.garrisonBuilding) continue;
            if (s.attackReload > 0) --s.attackReload;
            if (s.firing) {
                if (s.fireAnim == 255) {
                    s.firing = false;
                    s.fireAnim = 0;
                } else {
                    s.fireAnim = static_cast<std::uint8_t>(std::min(255, int(s.fireAnim) + 15));
                }
            }
            if (s.attackVehicleTarget) {
                stepSoldierAttackVehicle(s, i, changed);
            }
        }
    }
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        auto& v = vehicles_[vi];
        if (v.muzzleRevealTicks > 0u) {
            --v.muzzleRevealTicks;
            if (v.muzzleRevealTicks == 0u) fogDirty_ = true;
        }
        if (v.lastCombatTick != 0u && tick_ >= v.lastCombatTick + 125u) {
            v.lastCombatTick = 0u;
            if (v.marchEnabled && v.hatchOpen) {
                fogDirty_ = true;
                emitVehiclePose(vi);
            }
        }
    }
    stepVehicleCombat(changed);
    stepAirSupport(changed);
    if (fog_ && fogDirty_) refreshSpotting(&changed);
    if (tick_ % 25 == 0) {
        stepMedicHeal(changed);
        stepSupplyAndRepair(changed);
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        const bool nowCamo = s.isSniper && isSoldierCamouflaged(s);
        const bool nowSup = s.hp > 0 && s.isSuppressed();
        if (nowCamo != s.wasCamouflaged || nowSup != s.wasSuppressed) {
            s.wasCamouflaged = nowCamo;
            s.wasSuppressed = nowSup;
            const auto [su, sv] = soldierWorld(s);
            const bool vis = s.hp > 0 && !s.embarked && !s.garrisonBuilding &&
                (!fog_ || isAlliedWithPlayer(s.owner) || (s.spottedMask & 1u) != 0);
            pushOrUpdateSoldierPose(changed, {
                i, originX_ + su - sv, (su + sv) / 2, s.direction, 0, false, s.hp, s.hp > 0,
                s.owner, s.group, false, vis, 0, 0, s.garrisonBuilding.has_value(),
                s.expa, s.kills, false, s.atGrenades, false, nowCamo, s.mineAmmo, s.ammo, false,
                nowSup
            });
        }
    }
    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        const bool nowSup = v.alive && v.hp > 0 && v.isSuppressed();
        if (nowSup != v.wasSuppressed) {
            v.wasSuppressed = nowSup;
            emitVehiclePose(i);
        }
    }
    for (auto& pose : changed) {
        if (pose.index < soldiers_.size()) {
            auto& s = soldiers_[pose.index];
            pose.expa = s.expa;
            pose.kills = s.kills;
            pose.atGrenades = s.atGrenades;
            pose.camouflaged = isSoldierCamouflaged(s);
            pose.mineAmmo = s.mineAmmo;
            pose.ammo = s.ammo;
            pose.suppressed = s.hp > 0 && s.isSuppressed();
            s.wasCamouflaged = pose.camouflaged;
            s.wasSuppressed = pose.suppressed;
        }
    }
    return changed;
}

unsigned TutorialUnits::countInZone(std::span<const std::uint8_t> zones, unsigned zone, int owner) const {
    const std::size_t tileW = std::size_t(grid_.width() / 2), tileH = std::size_t(grid_.height() / 2);
    if (zones.size() != tileW * tileH) throw std::invalid_argument("Mission location grid size mismatch");
    const auto inside = [&](SubCell cell) {
        if (cell.x < 0 || cell.y < 0) return false;
        const std::size_t tx = std::size_t(cell.x / 2);
        const std::size_t ty = std::size_t(cell.y / 2);
        if (tx >= tileW || ty >= tileH) return false;
        return (zones[ty * tileW + tx] & 63u) == zone;
    };
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) { return s.hp > 0 && s.owner == owner && inside(s.cell); })
                  + std::ranges::count_if(vehicles_, [&](const auto& v) { return v.alive && v.hp > 0 && v.owner == owner && inside(v.cell); }));
}

unsigned TutorialUnits::countGroupInZone(std::span<const std::uint8_t> zones, unsigned zone, unsigned group) const {
    const std::size_t tileW = std::size_t(grid_.width() / 2), tileH = std::size_t(grid_.height() / 2);
    if (zones.size() != tileW * tileH) throw std::invalid_argument("Mission location grid size mismatch");
    const auto inside = [&](SubCell cell) {
        if (cell.x < 0 || cell.y < 0) return false;
        const std::size_t tx = std::size_t(cell.x / 2);
        const std::size_t ty = std::size_t(cell.y / 2);
        if (tx >= tileW || ty >= tileH) return false;
        return (zones[ty * tileW + tx] & 63u) == zone;
    };
    const int g = int(group);
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) { return s.hp > 0 && s.group == g && inside(s.cell); })
                  + std::ranges::count_if(vehicles_, [&](const auto& v) { return v.alive && v.hp > 0 && v.group == g && inside(v.cell); }));
}

TutorialUnits::Snapshot TutorialUnits::takeSnapshot() const {
    Snapshot snap{};
    snap.tick = tick_;
    snap.playerTeams = playerTeams_;
    snap.mines = mines_;
    if (fog_) {
        const auto raw = fog_->raw();
        snap.fog.assign(raw.begin(), raw.end());
    }
    snap.soldiers.reserve(soldiers_.size());
    for (const auto& s : soldiers_) {
        snap.soldiers.push_back(Snapshot::SoldierState{
            s.cell.x, s.cell.y,
            s.hp, s.maxHp, s.ammo, s.expa, s.kills, s.atGrenades, s.mineAmmo,
            s.owner, s.group,
            s.direction,
            s.crouched, s.holdFire, s.binoculars, s.embarked,
            s.garrisonBuilding ? static_cast<std::int32_t>(*s.garrisonBuilding) : -1,
            s.suppressionTicks
        });
    }
    snap.vehicles.reserve(vehicles_.size());
    for (const auto& v : vehicles_) {
        Snapshot::VehicleState vs{};
        vs.x = v.x;
        vs.y = v.y;
        vs.hullDir = v.hullDir;
        vs.turretDir = v.turretDir;
        vs.hp = v.hp;
        vs.maxHp = v.maxHp;
        vs.ammo = v.ammo;
        vs.ammo2 = v.ammo2;
        vs.expa = v.expa;
        vs.kills = v.kills;
        vs.owner = v.owner;
        vs.group = v.group;
        vs.alive = v.alive;
        vs.holdFire = v.holdFire;
        vs.passengers.reserve(v.passengers.size());
        for (size_t pIdx : v.passengers) {
            vs.passengers.push_back(static_cast<std::uint32_t>(pIdx));
        }
        vs.towedVehicle = v.towedVehicle ? static_cast<std::int32_t>(*v.towedVehicle) : -1;
        vs.towedBy = v.towedBy ? static_cast<std::int32_t>(*v.towedBy) : -1;
        vs.hasCrew = v.hasCrew;
        vs.crewHp = v.crewHp;
        snap.vehicles.push_back(std::move(vs));
    }
    snap.buildings.reserve(buildings_.size());
    for (const auto& b : buildings_) {
        Snapshot::BuildingState bs{};
        bs.hp = b.hp;
        bs.owner = b.owner;
        bs.destroyed = b.destroyed;
        bs.occupants.reserve(b.occupants.size());
        for (size_t sIdx : b.occupants) {
            bs.occupants.push_back(static_cast<std::uint32_t>(sIdx));
        }
        snap.buildings.push_back(std::move(bs));
    }
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) {
            snap.airReady[p][k] = airSupport_[p][k].ready;
        }
    }
    return snap;
}

bool TutorialUnits::loadSnapshot(const Snapshot& snap, std::vector<SoldierPose>* outSoldiers) {
    if (snap.soldiers.size() > soldiers_.size() || snap.vehicles.size() > vehicles_.size()) {
        return false;
    }
    if (!snap.buildings.empty() && snap.buildings.size() != buildings_.size()) {
        return false;
    }
    if (fog_ && !snap.fog.empty() && snap.fog.size() != fog_->raw().size()) {
        return false;
    }
    if (!mines_.empty() && !snap.mines.empty() && snap.mines.size() != mines_.size()) {
        return false;
    }
    for (const auto& ss : snap.soldiers) {
        if (ss.garrisonBuilding >= 0 &&
            static_cast<size_t>(ss.garrisonBuilding) >= buildings_.size()) {
            return false;
        }
    }
    for (const auto& vs : snap.vehicles) {
        if (vs.towedVehicle >= 0 && static_cast<size_t>(vs.towedVehicle) >= snap.vehicles.size()) return false;
        if (vs.towedBy >= 0 && static_cast<size_t>(vs.towedBy) >= snap.vehicles.size()) return false;
        for (std::uint32_t p : vs.passengers) {
            if (static_cast<size_t>(p) >= snap.soldiers.size()) return false;
        }
    }
    for (const auto& bs : snap.buildings) {
        for (std::uint32_t occ : bs.occupants) {
            if (static_cast<size_t>(occ) >= snap.soldiers.size()) return false;
        }
    }

    grid_.clearOccupancy();
    projectiles_.clear();
    mineEvents_.clear();
    buildingUpdates_.clear();
    vehiclePoses_.clear();
    sorties_.clear();
    sortiePoses_.clear();
    airEvents_.clear();
    lastHitTick_.fill(std::nullopt);
    lastGroupHitTick_.fill(std::nullopt);
    reserveUnits_.clear();
    tick_ = snap.tick;
    playerTeams_ = snap.playerTeams;
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) {
            airSupport_[p][k].ready = snap.airReady[p][k];
        }
    }
    if (!snap.mines.empty() || mines_.empty()) {
        mines_ = snap.mines;
    } else {
        std::ranges::fill(mines_, std::uint8_t{0});
    }

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        s.route.clear();
        s.cursor = 0;
        s.motion.reset();
        s.goal.reset();
        s.planning = {};
        s.needsRoute = false;
        s.attackTarget.reset();
        s.attackVehicleTarget.reset();
        s.attackGround.reset();
        s.autoTarget.reset();
        s.attackReload = 0;
        s.attackBurst = 0;
        s.firing = false;
        s.fireAnim = 0;
        s.boardVehicleTarget.reset();
        s.captureVehicleTarget.reset();
        s.garrisonTarget.reset();
        s.attackBuildingTarget.reset();
        s.healTarget.reset();
        s.escortVehicleTarget.reset();
        s.escortSoldierTarget.reset();
        s.disarmMineTarget.reset();
        s.disarmWorkTicks = 0;
        s.attackMove = false;
        s.queuedGoals.clear();
        s.patrolOrigin.reset();
        s.patrolDest.reset();
        s.patrolToDest = true;
        s.lastReactTick = 0;
        s.lastFireTick = 0;
        s.wasCamouflaged = false;
        s.suppressionTicks = 0;
        s.wasSuppressed = false;
        s.spottedMask = 0;
        if (i >= snap.soldiers.size()) {
            s.hp = 0;
            s.embarked = false;
            s.garrisonBuilding.reset();
            continue;
        }
        const auto& ss = snap.soldiers[i];
        s.cell = {ss.cellX, ss.cellY};
        s.hp = ss.hp;
        s.maxHp = std::max(1, ss.maxHp);
        s.ammo = ss.ammo;
        s.expa = ss.expa;
        s.kills = ss.kills;
        s.atGrenades = ss.atGrenades;
        s.mineAmmo = ss.mineAmmo;
        s.owner = ss.owner;
        s.group = ss.group;
        s.direction = static_cast<std::uint8_t>(ss.direction & 7u);
        s.crouched = ss.crouched;
        s.holdFire = ss.holdFire;
        s.binoculars = ss.binoculars;
        s.embarked = ss.embarked;
        s.garrisonBuilding = ss.garrisonBuilding >= 0
            ? std::optional<size_t>(static_cast<size_t>(ss.garrisonBuilding))
            : std::nullopt;
        s.suppressionTicks = (s.hp > 0 && !s.embarked && !s.garrisonBuilding)
            ? std::clamp(ss.suppressionTicks, 0, 35) : 0;
        s.wasSuppressed = s.hp > 0 && s.isSuppressed();
        if (s.hp > 0 && !s.embarked && !s.garrisonBuilding) {
            grid_.setOccupied(s.cell, true);
        }
    }

    for (size_t i = 0; i < vehicles_.size(); ++i) {
        auto& v = vehicles_[i];
        v.moveTarget.reset();
        v.waypoints.clear();
        v.queuedOrders.clear();
        v.pendingTowGun.reset();
        v.attackMove = false;
        v.attackMoveEngaging = false;
        v.attackGround.reset();
        v.attackTarget.reset();
        v.attackBuildingTarget.reset();
        v.chasingAttackTarget = false;
        v.wasMoving = false;
        v.fireCooldown = 0;
        v.burstCount1 = 0;
        v.burstCount2 = 0;
        v.patrolOrigin.reset();
        v.patrolDest.reset();
        v.patrolToDest = true;
        v.reverseMove = false;
        v.facingTargetDir.reset();
        v.supportTargetVehicle.reset();
        v.supportTargetBuilding.reset();
        v.supportTargetSoldier.reset();
        v.escortVehicleTarget.reset();
        v.escortSoldierTarget.reset();
        v.lastReactTick = 0;
        v.aaCooldown = 0;
        v.spottedMask = 0;
        v.muzzleRevealTicks = 0u;
        v.lastCombatTick = 0u;
        v.hatchOpen = v.marchEnabled;
        v.lastPanicTick = 0u;
        v.suppressionTicks = 0;
        v.wasSuppressed = false;
        if (i >= snap.vehicles.size()) {
            v.alive = false;
            v.hp = 0;
            v.visible = false;
            v.passengers.clear();
            v.towedVehicle.reset();
            v.towedBy.reset();
            continue;
        }
        const auto& vs = snap.vehicles[i];
        v.x = vs.x;
        v.y = vs.y;
        v.fx = static_cast<double>(vs.x);
        v.fy = static_cast<double>(vs.y);
        v.cell = placementCell(v.x, v.y, originX_);
        v.hullDir = vs.hullDir & 31u;
        v.turretDir = vs.turretDir & 31u;
        v.hp = vs.hp;
        v.maxHp = std::max(1, vs.maxHp);
        v.ammo = vs.ammo;
        v.ammo2 = vs.ammo2;
        v.expa = vs.expa;
        v.kills = vs.kills;
        v.owner = vs.owner;
        v.group = vs.group;
        v.alive = vs.alive;
        v.holdFire = vs.holdFire;
        v.visible = true;
        v.passengers.clear();
        v.passengers.reserve(vs.passengers.size());
        for (std::uint32_t pIdx : vs.passengers) {
            v.passengers.push_back(static_cast<size_t>(pIdx));
        }
        if (vs.crewHp >= 0) {
            v.hasCrew = vs.hasCrew;
            v.crewHp = v.hasCrew ? std::clamp(vs.crewHp, 0, v.maxCrewHp) : 0;
        } else {
            v.hasCrew = (v.owner != 3) || !v.passengers.empty();
            v.crewHp = v.hasCrew ? v.maxCrewHp : 0;
        }
        v.towedVehicle = vs.towedVehicle >= 0
            ? std::optional<size_t>(static_cast<size_t>(vs.towedVehicle))
            : std::nullopt;
        v.towedBy = vs.towedBy >= 0
            ? std::optional<size_t>(static_cast<size_t>(vs.towedBy))
            : std::nullopt;
        if (v.alive && v.hp > 0) {
            grid_.setOccupied(v.cell, true);
        }
    }

    for (size_t i = 0; i < snap.buildings.size(); ++i) {
        auto& b = buildings_[i];
        const auto& bs = snap.buildings[i];
        b.hp = bs.hp;
        b.owner = bs.owner;
        b.destroyed = bs.destroyed;
        b.occupants.clear();
        b.occupants.reserve(bs.occupants.size());
        for (std::uint32_t sIdx : bs.occupants) {
            b.occupants.push_back(static_cast<size_t>(sIdx));
        }
        emitBuildingUpdate(i, false, false);
    }

    if (fog_) {
        if (!snap.fog.empty()) {
            fog_->clearAll();
            const int tileW = fog_->width();
            const int tileH = fog_->height();
            for (int ty = 0; ty < tileH; ++ty) {
                for (int tx = 0; tx < tileW; ++tx) {
                    const std::uint8_t bits = snap.fog[static_cast<size_t>(ty * tileW + tx)];
                    if (bits != 0) fog_->castRay(tx, ty, 0, 0, bits);
                }
            }
        }
        fogDirty_ = true;
        refreshSpotting(nullptr, false);
    }

    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        emitVehiclePose(vi);
    }
    if (outSoldiers) {
        outSoldiers->clear();
        outSoldiers->reserve(soldiers_.size());
        for (size_t si = 0; si < soldiers_.size(); ++si) {
            const auto& s = soldiers_[si];
            const auto [u, v] = soldierWorld(s);
            const bool isGar = s.garrisonBuilding.has_value();
            const bool alive = s.hp > 0;
            const bool vis = alive && !s.embarked && !isGar &&
                (!fog_ || isAlliedWithPlayer(s.owner) || (s.spottedMask & 1u) != 0);
            const bool camo = isSoldierCamouflaged(s);
            const bool sup = alive && s.isSuppressed();
            soldiers_[si].wasCamouflaged = camo;
            soldiers_[si].wasSuppressed = sup;
            outSoldiers->push_back(SoldierPose{
                si, originX_ + u - v, (u + v) / 2, s.direction, 0u, false,
                s.hp, alive, s.owner, s.group, false, vis, 0, 0, isGar,
                s.expa, s.kills, false, s.atGrenades, false, camo, s.mineAmmo, s.ammo, false, sup,
                static_cast<std::int8_t>(s.crouched ? 1 : 0)
            });
        }
    }
    return true;
}

std::vector<std::uint8_t> TutorialUnits::serializeSnapshot(const Snapshot& snap) {
    std::vector<std::uint8_t> out;
    out.reserve(160 + snap.mines.size() + snap.fog.size() +
                snap.soldiers.size() * 56 + snap.vehicles.size() * 73 + snap.buildings.size() * 16);
    out.insert(out.end(), kSnapshotMagicV3.begin(), kSnapshotMagicV3.end());
    appendU32(out, snap.tick);
    for (int team : snap.playerTeams) {
        appendI32(out, static_cast<std::int32_t>(team));
    }
    appendU32(out, static_cast<std::uint32_t>(snap.mines.size()));
    out.insert(out.end(), snap.mines.begin(), snap.mines.end());
    appendU32(out, static_cast<std::uint32_t>(snap.fog.size()));
    out.insert(out.end(), snap.fog.begin(), snap.fog.end());

    appendU32(out, static_cast<std::uint32_t>(snap.soldiers.size()));
    for (const auto& s : snap.soldiers) {
        appendI32(out, s.cellX);
        appendI32(out, s.cellY);
        appendI32(out, s.hp);
        appendI32(out, s.maxHp);
        appendI32(out, s.ammo);
        appendI32(out, s.expa);
        appendI32(out, s.kills);
        appendI32(out, s.atGrenades);
        appendI32(out, s.mineAmmo);
        appendI32(out, s.owner);
        appendI32(out, s.group);
        appendU8(out, s.direction);
        appendU8(out, s.crouched ? 1u : 0u);
        appendU8(out, s.holdFire ? 1u : 0u);
        appendU8(out, s.binoculars ? 1u : 0u);
        appendU8(out, s.embarked ? 1u : 0u);
        appendI32(out, s.garrisonBuilding);
    }

    appendU32(out, static_cast<std::uint32_t>(snap.vehicles.size()));
    for (const auto& v : snap.vehicles) {
        appendI32(out, v.x);
        appendI32(out, v.y);
        appendU32(out, v.hullDir);
        appendU32(out, v.turretDir);
        appendI32(out, v.hp);
        appendI32(out, v.maxHp);
        appendI32(out, v.ammo);
        appendI32(out, v.ammo2);
        appendI32(out, v.expa);
        appendI32(out, v.kills);
        appendI32(out, v.owner);
        appendI32(out, v.group);
        appendU8(out, v.alive ? 1u : 0u);
        appendU8(out, v.holdFire ? 1u : 0u);
        appendI32(out, v.towedVehicle);
        appendI32(out, v.towedBy);
        appendU32(out, static_cast<std::uint32_t>(v.passengers.size()));
        for (std::uint32_t p : v.passengers) {
            appendU32(out, p);
        }
    }

    appendU32(out, static_cast<std::uint32_t>(snap.buildings.size()));
    for (const auto& b : snap.buildings) {
        appendI32(out, b.hp);
        appendI32(out, b.owner);
        appendU8(out, b.destroyed ? 1u : 0u);
        appendU32(out, static_cast<std::uint32_t>(b.occupants.size()));
        for (std::uint32_t occ : b.occupants) {
            appendU32(out, occ);
        }
    }
    for (std::size_t p = 0; p < 4u; ++p) {
        for (std::size_t k = 0; k < 5u; ++k) {
            appendI32(out, static_cast<std::int32_t>(snap.airReady[p][k]));
        }
    }
    for (const auto& s : snap.soldiers) {
        appendI32(out, static_cast<std::int32_t>(s.suppressionTicks));
    }
    for (const auto& v : snap.vehicles) {
        appendU8(out, v.hasCrew ? 1u : 0u);
        appendI32(out, static_cast<std::int32_t>(v.crewHp));
    }
    return out;
}

std::optional<TutorialUnits::Snapshot> TutorialUnits::deserializeSnapshot(std::span<const std::uint8_t> bytes) {
    int version = 0;
    if (bytes.size() >= kSnapshotMagicV3.size() &&
        std::equal(kSnapshotMagicV3.begin(), kSnapshotMagicV3.end(), bytes.begin())) {
        version = 3;
    } else if (bytes.size() >= kSnapshotMagicV2.size() &&
               std::equal(kSnapshotMagicV2.begin(), kSnapshotMagicV2.end(), bytes.begin())) {
        version = 2;
    } else if (bytes.size() >= kSnapshotMagicV1.size() &&
               std::equal(kSnapshotMagicV1.begin(), kSnapshotMagicV1.end(), bytes.begin())) {
        version = 1;
    } else {
        return std::nullopt;
    }
    SnapshotReader r{bytes, kSnapshotMagicV1.size()};
    Snapshot snap{};
    std::uint32_t tickU32 = 0;
    if (!r.readU32(tickU32)) return std::nullopt;
    snap.tick = tickU32;
    for (int& team : snap.playerTeams) {
        std::int32_t tv = 0;
        if (!r.readI32(tv)) return std::nullopt;
        team = tv;
    }
    std::uint32_t mineLen = 0;
    if (!r.readU32(mineLen) || !r.readBytes(snap.mines, mineLen)) return std::nullopt;
    std::uint32_t fogLen = 0;
    if (!r.readU32(fogLen) || !r.readBytes(snap.fog, fogLen)) return std::nullopt;

    std::uint32_t soldierCount = 0;
    if (!r.readU32(soldierCount) || soldierCount > (bytes.size() - r.pos) / 53u) return std::nullopt;
    snap.soldiers.resize(soldierCount);
    for (auto& s : snap.soldiers) {
        if (!r.readI32(s.cellX) || !r.readI32(s.cellY) ||
            !r.readI32(s.hp) || !r.readI32(s.maxHp) || !r.readI32(s.ammo) ||
            !r.readI32(s.expa) || !r.readI32(s.kills) || !r.readI32(s.atGrenades) ||
            !r.readI32(s.mineAmmo) || !r.readI32(s.owner) || !r.readI32(s.group) ||
            !r.readU8(s.direction) || !r.readBool(s.crouched) || !r.readBool(s.holdFire) ||
            !r.readBool(s.binoculars) || !r.readBool(s.embarked) || !r.readI32(s.garrisonBuilding)) {
            return std::nullopt;
        }
    }

    std::uint32_t vehicleCount = 0;
    if (!r.readU32(vehicleCount) || vehicleCount > (bytes.size() - r.pos) / 62u) return std::nullopt;
    snap.vehicles.resize(vehicleCount);
    for (auto& v : snap.vehicles) {
        std::uint32_t hull = 0, turret = 0, passCount = 0;
        if (!r.readI32(v.x) || !r.readI32(v.y) ||
            !r.readU32(hull) || !r.readU32(turret) ||
            !r.readI32(v.hp) || !r.readI32(v.maxHp) || !r.readI32(v.ammo) || !r.readI32(v.ammo2) ||
            !r.readI32(v.expa) || !r.readI32(v.kills) || !r.readI32(v.owner) || !r.readI32(v.group) ||
            !r.readBool(v.alive) || !r.readBool(v.holdFire) ||
            !r.readI32(v.towedVehicle) || !r.readI32(v.towedBy) || !r.readU32(passCount)) {
            return std::nullopt;
        }
        v.hullDir = hull;
        v.turretDir = turret;
        if (passCount > (bytes.size() - r.pos) / 4u) return std::nullopt;
        v.passengers.resize(passCount);
        for (auto& p : v.passengers) {
            if (!r.readU32(p)) return std::nullopt;
        }
    }

    std::uint32_t bldCount = 0;
    if (!r.readU32(bldCount) || bldCount > (bytes.size() - r.pos) / 13u) return std::nullopt;
    snap.buildings.resize(bldCount);
    for (auto& b : snap.buildings) {
        std::uint32_t occCount = 0;
        if (!r.readI32(b.hp) || !r.readI32(b.owner) || !r.readBool(b.destroyed) || !r.readU32(occCount)) {
            return std::nullopt;
        }
        if (occCount > (bytes.size() - r.pos) / 4u) return std::nullopt;
        b.occupants.resize(occCount);
        for (auto& occ : b.occupants) {
            if (!r.readU32(occ)) return std::nullopt;
        }
    }

    if (version >= 2) {
        for (std::size_t p = 0; p < 4u; ++p) {
            for (std::size_t k = 0; k < 5u; ++k) {
                std::int32_t rv = 0;
                if (!r.readI32(rv)) return std::nullopt;
                snap.airReady[p][k] = rv;
            }
        }
    }
    if (version >= 3) {
        for (auto& s : snap.soldiers) {
            std::int32_t st = 0;
            if (!r.readI32(st)) return std::nullopt;
            s.suppressionTicks = st;
        }
        for (auto& v : snap.vehicles) {
            std::int32_t ch = 0;
            if (!r.readBool(v.hasCrew) || !r.readI32(ch)) return std::nullopt;
            v.crewHp = ch;
        }
    }

    if (r.pos != bytes.size()) return std::nullopt;
    return snap;
}

void TutorialUnits::setAirSupport(std::array<std::array<assets::AirSupportSlot, 5>, 4> slots,
                                  std::array<int, 4> paraGroups,
                                  std::array<std::vector<assets::ParatrooperEntry>, 4> paraRosters) {
    airSupport_ = std::move(slots);
    paraGroups_ = std::move(paraGroups);
    paraRosters_ = std::move(paraRosters);
}

void TutorialUnits::addAirSupportPlanes(int owner, assets::AirMissionKind kind, int count) {
    if (owner < 0 || owner >= 4 || count <= 0) return;
    const std::size_t k = static_cast<std::size_t>(kind);
    if (k >= 5u) return;
    auto& slot = airSupport_[static_cast<std::size_t>(owner)][k];
    slot.ready = std::clamp(slot.ready + count, 0, 999);
}

void TutorialUnits::addAirSupportSorties(int owner, assets::AirMissionKind kind, int count) {
    if (owner < 0 || owner >= 4 || count <= 0) return;
    const std::size_t k = static_cast<std::size_t>(kind);
    if (k >= 5u) return;
    auto& slot = airSupport_[static_cast<std::size_t>(owner)][k];
    slot.total = std::clamp(slot.total + count, 0, 999);
}

std::array<assets::AirSupportSlot, 5> TutorialUnits::airSupportFor(int owner) const {
    if (owner < 0 || owner >= 4) return {};
    return airSupport_[static_cast<std::size_t>(owner)];
}

std::array<assets::AirSupportSlot, 5> TutorialUnits::playerAirSupport() const {
    return airSupport_[0];
}

bool TutorialUnits::callAirSupport(assets::AirMissionKind kind, int targetX, int targetY, int owner) {
    if (owner < 0 || owner >= 4) return false;
    const std::size_t k = static_cast<std::size_t>(kind);
    if (k >= 5u) return false;
    auto& slot = airSupport_[static_cast<std::size_t>(owner)][k];
    if (slot.ready <= 0) return false;
    --slot.ready;
    if (slot.total > 0) --slot.total;

    const int tx = std::clamp(targetX, 0, std::max(0, mapPixelW_ - 1));
    const int ty = std::clamp(targetY, 0, std::max(0, mapPixelH_ - 1));
    const bool fromLeft = tx <= mapPixelW_ / 2;
    const double entryX = fromLeft ? 0.0 : static_cast<double>(std::max(1, mapPixelW_ - 1));
    const double entryY = static_cast<double>(
        std::clamp(ty - 120, std::min(64, std::max(0, mapPixelH_ - 1)), std::max(64, mapPixelH_ - 64)));
    const double dx = static_cast<double>(tx) - entryX;
    const double dy = static_cast<double>(ty) - entryY;
    const double dist = std::hypot(dx, dy);
    const double ux = dist < 1.0 ? (fromLeft ? 1.0 : -1.0) : (dx / dist);
    const double uy = dist < 1.0 ? 0.0 : (dy / dist);
    const std::string resolvedPlaneId = slot.planeId.empty() ? "ju-87" : slot.planeId;
    const auto* pSpec = catalog_ ? catalog_->findPlane(resolvedPlaneId) : nullptr;
    const double speed = pSpec && pSpec->flySpeed > 0.1
        ? std::clamp(pSpec->flySpeed * 1.45, 5.5, 19.0)
        : ((kind == assets::AirMissionKind::Bomber) ? 9.5
           : (kind == assets::AirMissionKind::Paratrooper || kind == assets::AirMissionKind::Supply) ? 9.0
           : 12.5);
    const bool isRocketAttacker = (pSpec && pSpec->isRocketAttacker());
    const int altitude = (pSpec && pSpec->altitude > 0) ? pSpec->altitude : 280;
    const int payloads = (kind == assets::AirMissionKind::Bomber)
        ? (pSpec && pSpec->bombsNumber > 0 ? std::clamp(pSpec->bombsNumber, 3, 12) : 5)
        : (kind == assets::AirMissionKind::Fighter)
            ? (isRocketAttacker ? (pSpec->groundAmmo > 0 ? std::clamp(pSpec->groundAmmo, 3, 6) : 6) : 3)
        : (kind == assets::AirMissionKind::Paratrooper || kind == assets::AirMissionKind::Supply) ? 1
        : 0;
    const int maxHp = pSpec && pSpec->health > 0
        ? std::clamp(pSpec->health, 120, 2500)
        : ((kind == assets::AirMissionKind::Bomber || kind == assets::AirMissionKind::Paratrooper ||
            kind == assets::AirMissionKind::Supply)
               ? 220
               : 150);
    const bool isLeafletBomber = (pSpec && pSpec->isLeafletBomber());
    const int bombDamage = isLeafletBomber ? 0
        : ((pSpec && pSpec->bombDamage > 0) ? std::clamp(pSpec->bombDamage / 6, 280, 650) : 380);
    const int strafeDamage = (pSpec && pSpec->groundDamage > 0) ? std::clamp(pSpec->groundDamage * 4, 120, 260) : 180;
    const int airToAirDamage = (pSpec && pSpec->airDamage > 0) ? std::clamp(pSpec->airDamage * 2, 55, 140) : 75;
    const int sightTiles = (pSpec && pSpec->sight > 0) ? std::clamp(pSpec->sight, 12, 28) : 18;
    const int scoreValue = pSpec ? pSpec->effectiveScoreValue() : 500;

    ActiveSortie st{};
    st.id = nextSortieId_++;
    st.kind = kind;
    st.planeId = resolvedPlaneId;
    st.owner = owner;
    st.fx = entryX;
    st.fy = entryY;
    st.vx = ux * speed;
    st.vy = uy * speed;
    st.targetX = tx;
    st.targetY = ty;
    st.dir32 = vehicleDirFromMapDelta(st.vx, st.vy);
    st.hp = maxHp;
    st.maxHp = maxHp;
    st.payloadsLeft = payloads;
    st.totalPayloads = payloads;
    st.bombDamage = bombDamage;
    st.strafeDamage = strafeDamage;
    st.airToAirDamage = airToAirDamage;
    st.sightTiles = sightTiles;
    st.scoreValue = scoreValue;
    st.isLeafletBomber = isLeafletBomber;
    st.altitude = altitude;
    st.isRocketAttacker = isRocketAttacker;
    st.strafeHitType = pSpec ? pSpec->effectiveGroundHitType() : assets::HitType::Machine;
    sorties_.push_back(st);

    sortiePoses_.push_back(AirSortiePose{
        st.id, st.kind, st.planeId, st.owner,
        static_cast<int>(std::lround(st.fx)), static_cast<int>(std::lround(st.fy)),
        st.dir32, st.hp, st.maxHp, true, true, false, 0, 0
    });
    return true;
}

std::vector<AirSortiePose> TutorialUnits::takeAirSorties() {
    return std::exchange(sortiePoses_, {});
}

std::vector<AirStrikeEvent> TutorialUnits::takeAirStrikeEvents() {
    return std::exchange(airEvents_, {});
}

void TutorialUnits::reactToAttack(int victimOwner, int victimGroup, int attackerX, int attackerY,
                                  std::optional<size_t> victimVehicleIdx) {
    if (!autoEngage_ || victimOwner < 0 || victimOwner == 0 || victimOwner == 3 ||
        attackerX < 0 || attackerY < 0) {
        return;
    }
    if (victimVehicleIdx && *victimVehicleIdx < vehicles_.size()) {
        auto& v = vehicles_[*victimVehicleIdx];
        if (v.alive && v.hp > 0 && !v.towedBy && !v.moveTarget && !v.attackTarget) {
            const unsigned desiredHull = vehicleDirFromMapDelta(
                static_cast<double>(attackerX) - v.fx, static_cast<double>(attackerY) - v.fy);
            const unsigned prevHull = v.hullDir;
            v.hullDir = turnTowardDir32(v.hullDir, desiredHull, 6u);
            if (!v.attackBuildingTarget && !v.attackGround) {
                v.turretDir = turnTowardDir32(v.turretDir, desiredHull, 6u);
            }
            if (v.hullDir != prevHull) emitVehiclePose(*victimVehicleIdx);
            if (v.armed && v.speedPx > 0.0 && (v.lastReactTick == 0 || tick_ - v.lastReactTick >= 50u)) {
                moveVehicleInternal(v, attackerX, attackerY);
                v.attackMove = true;
                v.lastReactTick = tick_;
            }
        }
    }
    if (victimGroup > 0) {
        int alerted = 0;
        for (size_t i = 0; i < soldiers_.size() && alerted < 6; ++i) {
            auto& s = soldiers_[i];
            if (s.hp <= 0 || s.owner != victimOwner || s.group != victimGroup) continue;
            if (s.embarked || s.garrisonBuilding || s.motion || s.goal ||
                s.attackTarget || s.autoTarget || s.holdFire) {
                continue;
            }
            if (s.lastReactTick != 0 && tick_ - s.lastReactTick < 50u) continue;
            const auto [su, sv] = soldierWorld(s);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            if (std::hypot(static_cast<double>(attackerX - sx), static_cast<double>(attackerY - sy)) > 760.0) {
                continue;
            }
            if (orderSoldierInternal(i, attackerX, attackerY)) {
                s.attackMove = true;
                s.lastReactTick = tick_;
                ++alerted;
            }
        }
        int vehAlerted = 0;
        for (size_t i = 0; i < vehicles_.size() && vehAlerted < 2; ++i) {
            if (victimVehicleIdx && i == *victimVehicleIdx) continue;
            auto& v = vehicles_[i];
            if (!v.alive || v.hp <= 0 || v.owner != victimOwner || v.group != victimGroup) continue;
            if (!v.armed || v.speedPx <= 0.0 || v.towedBy || v.moveTarget || v.attackTarget || v.attackGround) continue;
            if (v.lastReactTick != 0 && tick_ - v.lastReactTick < 50u) continue;
            if (std::hypot(static_cast<double>(attackerX) - v.fx, static_cast<double>(attackerY) - v.fy) > 560.0) {
                continue;
            }
            const unsigned desiredHull = vehicleDirFromMapDelta(
                static_cast<double>(attackerX) - v.fx, static_cast<double>(attackerY) - v.fy);
            v.hullDir = turnTowardDir32(v.hullDir, desiredHull, 6u);
            if (!v.attackBuildingTarget) v.turretDir = turnTowardDir32(v.turretDir, desiredHull, 6u);
            moveVehicleInternal(v, attackerX, attackerY);
            v.attackMove = true;
            v.lastReactTick = tick_;
            emitVehiclePose(i);
            ++vehAlerted;
        }
    }
}

void TutorialUnits::stepAirSupport(std::vector<SoldierPose>& changed) {
    for (auto& v : vehicles_) {
        if (v.aaCooldown > 0u) --v.aaCooldown;
    }
    if (sorties_.empty()) return;

    const auto pushSortiePose = [&](const AirSortiePose& pose) {
        const auto it = std::ranges::find(sortiePoses_, pose.id, &AirSortiePose::id);
        if (it == sortiePoses_.end()) {
            sortiePoses_.push_back(pose);
            return;
        }
        const bool hadAa = it->aaFired;
        const int aaX = it->aaOriginX;
        const int aaY = it->aaOriginY;
        std::string aaSnd = std::move(it->aaSoundGroup);
        *it = pose;
        if (hadAa && !pose.aaFired) {
            it->aaFired = true;
            it->aaOriginX = aaX;
            it->aaOriginY = aaY;
            it->aaSoundGroup = std::move(aaSnd);
        }
    };

    const auto applyAirSplash = [&](int impactX, int impactY, double radius, int dmgBase,
                                    assets::HitType hitType, int owner) {
        for (size_t k = 0; k < vehicles_.size(); ++k) {
            auto& tv = vehicles_[k];
            if (!tv.alive || tv.hp <= 0 || !isHostile(owner, tv.owner)) continue;
            const double d = std::hypot(tv.fx - static_cast<double>(impactX), tv.fy - static_cast<double>(impactY));
            if (d > radius) continue;
            const int scaledDmg = std::max(20, static_cast<int>(std::lround(
                static_cast<double>(dmgBase) * (1.0 - 0.55 * (d / (radius + 1.0))))));
            const int dealt = scaleVehicleDamageForDifficulty(
                tv.owner, applyVehicleDamage(tv.x, tv.y, tv.hullDir, tv.armor, tv.hasCatalogArmor,
                                             impactX, impactY, scaledDmg, hitType));
            tv.hp = std::max(0, tv.hp - dealt);
            if (dealt > 0 && tv.hp > 0 && tv.expaOnHit > 0) {
                tv.expa = std::min(0x8000, tv.expa + std::min(200, dealt * tv.expaOnHit));
            }
            recordHit(tv.owner, tv.group);
            markVehicleCombat(tv);
            if (tv.hp == 0) destroyVehicle(k, changed);
            else {
                applyVehicleCrewDamage(k, impactX, impactY, scaledDmg, hitType, changed, -1, -1);
                if (dealt > 0 || hitType == assets::HitType::Explosive) {
                    applyVehicleSuppression(k, 24, impactX, impactY);
                }
            }
            emitVehiclePose(k);
        }
        for (size_t k = 0; k < soldiers_.size(); ++k) {
            auto& ss = soldiers_[k];
            if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(owner, ss.owner)) continue;
            const auto [su, sv] = soldierWorld(ss);
            const int sx = originX_ + su - sv;
            const int sy = (su + sv) / 2;
            const double sd = std::hypot(static_cast<double>(sx - impactX), static_cast<double>(sy - impactY));
            if (sd > radius) continue;
            const int splashDmg = std::max(10, static_cast<int>(std::lround(
                static_cast<double>(dmgBase) * 0.65 * (1.0 - sd / (radius + 1.0)))));
            ss.hp = std::max(0, ss.hp - splashDmg);
            if (ss.hp > 0) {
                applySuppression(k, hitType == assets::HitType::Explosive ? 30 : 18);
            }
            recordHit(ss.owner, ss.group);
            if (ss.hp == 0) {
                if (!vehicleAtCell(ss.cell)) grid_.setOccupied(ss.cell, false);
                ss.motion.reset();
                ss.goal.reset();
                ss.route.clear();
                ss.cursor = 0;
                ss.queuedGoals.clear();
                ss.attackMove = false;
                ss.attackTarget.reset();
                ss.attackVehicleTarget.reset();
                ss.attackBuildingTarget.reset();
                ss.autoTarget.reset();
                ss.binoculars = false;
                ss.spottedMask = 0;
                fogDirty_ = true;
            }
            const bool sVis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
            SoldierPose sp{k, sx, sy, ss.direction, 0, false, ss.hp, ss.hp > 0, ss.owner, ss.group, false, sVis};
            sp.suppressed = (ss.hp > 0 && ss.isSuppressed());
            pushOrUpdateSoldierPose(changed, sp);
        }
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            const auto& tb = buildings_[bi];
            if (tb.destroyed) continue;
            const double bd = std::max(0.0, std::hypot(double(tb.x - impactX), double(tb.y - impactY)) -
                                            std::min(tb.width, tb.height) * 0.3);
            if (bd <= radius + 16.0) {
                const int bldDmg = hitType == assets::HitType::Explosive
                    ? std::max(25, dmgBase / 2)
                    : std::max(10, dmgBase / 4);
                damageBuilding(bi, bldDmg, hitType, owner, changed);
            }
        }
        if (hitType == assets::HitType::Explosive && !mines_.empty()) {
            const SubCell impCell = pointerCell(impactX, impactY, originX_);
            const int itx = impCell.x / 2;
            const int ity = impCell.y / 2;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    triggerMineAtTile(itx + dx, ity + dy, changed, true);
                }
            }
        }
    };

    for (auto& st : sorties_) {
        st.fx += st.vx;
        st.fy += st.vy;
    }

    // Air-to-air interception: active Fighter sorties engage hostile aircraft within 420px.
    struct AirIntercept { int x{}; int y{}; std::string sound; };
    std::vector<std::optional<AirIntercept>> interceptedFrom(sorties_.size());
    for (size_t i = 0; i < sorties_.size(); ++i) {
        auto& fighter = sorties_[i];
        if (fighter.kind != assets::AirMissionKind::Fighter || fighter.hp <= 0) continue;
        if ((fighter.reconTick++ % 8u) != 0u) continue;
        std::optional<size_t> bestVictim;
        double bestDist = 420.0 + 1.0;
        for (size_t j = 0; j < sorties_.size(); ++j) {
            if (j == i) continue;
            const auto& other = sorties_[j];
            if (other.hp <= 0 || !isHostile(fighter.owner, other.owner)) continue;
            const double d = std::hypot(other.fx - fighter.fx, other.fy - fighter.fy);
            if (d <= 420.0 && d < bestDist) {
                bestDist = d;
                bestVictim = j;
            }
        }
        if (bestVictim) {
            auto& victim = sorties_[*bestVictim];
            victim.hp = std::max(0, victim.hp - fighter.airToAirDamage);
            const auto* fSpec = catalog_ ? catalog_->findPlane(fighter.planeId) : nullptr;
            std::string fSound = (fSpec && !fSpec->airShotSound.empty()) ? fSpec->airShotSound : std::string("mg_aa");
            interceptedFrom[*bestVictim] = AirIntercept{
                static_cast<int>(std::lround(fighter.fx)),
                static_cast<int>(std::lround(fighter.fy)),
                std::move(fSound)
            };
        }
    }

    std::vector<ActiveSortie> aliveSorties;
    aliveSorties.reserve(sorties_.size());
    for (size_t si = 0; si < sorties_.size(); ++si) {
        auto& st = sorties_[si];
        const int ix = static_cast<int>(std::lround(st.fx));
        const int iy = static_cast<int>(std::lround(st.fy));

        if (st.kind == assets::AirMissionKind::Recon && fog_ && (st.reconTick++ % 3u) == 0u) {
            fogDirty_ = true;
        }

        bool aaFired = interceptedFrom[si].has_value();
        int aaOriginX = aaFired ? interceptedFrom[si]->x : 0;
        int aaOriginY = aaFired ? interceptedFrom[si]->y : 0;
        std::string aaSound = aaFired ? interceptedFrom[si]->sound : std::string{};
        const auto isAaSoundName = [](std::string_view snd) {
            return snd.find("flak") != std::string_view::npos || snd.find("aa") != std::string_view::npos;
        };
        for (size_t vi = 0; vi < vehicles_.size() && st.hp > 0; ++vi) {
            auto& av = vehicles_[vi];
            if (!av.alive || av.hp <= 0 || !av.isAntiAir || (av.ammo <= 0 && av.ammo2 <= 0) || av.towedBy) continue;
            if (!isHostile(av.owner, st.owner) || av.aaCooldown > 0u) continue;
            const double maxAaRange = av.isHeavyAntiAir ? 600.0
                                                        : (st.altitude >= 350 ? 440.0 : 520.0);
            const double ad = std::hypot(st.fx - av.fx, st.fy - av.fy);
            if (ad > maxAaRange) continue;
            av.aaCooldown = 14u;
            if (av.ammo > 0) --av.ammo;
            else --av.ammo2;
            av.turretDir = vehicleDirFromMapDelta(st.fx - av.fx, st.fy - av.fy);
            emitVehiclePose(vi);
            if (!aaFired) {
                aaFired = true;
                aaOriginX = av.x;
                aaOriginY = av.y;
                if (isAaSoundName(av.sound2)) aaSound = av.sound2;
                else if (!av.sound1.empty()) aaSound = av.sound1;
                else if (!av.sound2.empty()) aaSound = av.sound2;
                else aaSound = "flak";
            }
            const int aaDmg = av.isHeavyAntiAir ? 55
                                                : (st.altitude >= 350 ? 20
                                                                      : (st.altitude <= 250 ? 40 : 35));
            st.hp = std::max(0, st.hp - aaDmg);
            if (st.hp == 0) break;
        }
        if (st.hp <= 0) {
            applyAirSplash(ix, iy, 60.0, 175, assets::HitType::Explosive, -1);
            airEvents_.push_back(AirStrikeEvent{
                AirStrikeEvent::PlaneShotDown, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId
            });
            pushSortiePose(AirSortiePose{
                st.id, st.kind, st.planeId, st.owner, ix, iy, st.dir32, 0, st.maxHp, false, false,
                aaFired, aaOriginX, aaOriginY, aaSound
            });
            continue;
        }

        const double spd = std::max(1e-4, std::hypot(st.vx, st.vy));
        const double ux = st.vx / spd;
        const double uy = st.vy / spd;
        const double along = (st.fx - static_cast<double>(st.targetX)) * ux +
                             (st.fy - static_cast<double>(st.targetY)) * uy;

        if (st.kind == assets::AirMissionKind::Bomber) {
            const int totalDrops = std::max(1, st.totalPayloads);
            while (st.payloadsLeft > 0) {
                const int dropIdx = totalDrops - st.payloadsLeft;
                const double step = totalDrops > 1 ? (192.0 / static_cast<double>(totalDrops - 1)) : 0.0;
                const double threshold = -96.0 + step * static_cast<double>(dropIdx);
                if (along < threshold) break;
                --st.payloadsLeft;
                if (st.isLeafletBomber) {
                    for (size_t k = 0; k < soldiers_.size(); ++k) {
                        auto& ss = soldiers_[k];
                        if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding || !isHostile(st.owner, ss.owner)) continue;
                        const auto [su, sv] = soldierWorld(ss);
                        const int sx = originX_ + su - sv;
                        const int sy = (su + sv) / 2;
                        if (std::hypot(static_cast<double>(sx - ix), static_cast<double>(sy - iy)) <= 180.0) {
                            applySuppression(k, 35);
                        }
                    }
                    for (size_t k = 0; k < vehicles_.size(); ++k) {
                        auto& tv = vehicles_[k];
                        if (!tv.alive || tv.hp <= 0 || !tv.hasCrew || !isHostile(st.owner, tv.owner)) continue;
                        if (std::hypot(tv.fx - static_cast<double>(ix), tv.fy - static_cast<double>(iy)) <= 180.0) {
                            applyVehicleSuppression(k, 32, ix, iy);
                        }
                    }
                    airEvents_.push_back(AirStrikeEvent{
                        AirStrikeEvent::BombImpact, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId, true
                    });
                } else {
                    applyAirSplash(ix, iy, 100.0, st.bombDamage, assets::HitType::Explosive, st.owner);
                    airEvents_.push_back(AirStrikeEvent{
                        AirStrikeEvent::BombImpact, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId, false
                    });
                }
            }
        } else if (st.kind == assets::AirMissionKind::Fighter) {
            const int totalStrafes = std::max(1, st.totalPayloads);
            const double spanHalf = st.isRocketAttacker ? 96.0 : 64.0;
            while (st.payloadsLeft > 0) {
                const int dropIdx = totalStrafes - st.payloadsLeft;
                const double step = totalStrafes > 1
                    ? ((spanHalf * 2.0) / static_cast<double>(totalStrafes - 1)) : 0.0;
                const double threshold = -spanHalf + step * static_cast<double>(dropIdx);
                if (along < threshold) break;
                --st.payloadsLeft;
                applyAirSplash(ix, iy, st.isRocketAttacker ? 96.0 : 80.0,
                               st.strafeDamage, st.strafeHitType, st.owner);
                airEvents_.push_back(AirStrikeEvent{
                    AirStrikeEvent::StrafeImpact, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId
                });
            }
        } else if (st.kind == assets::AirMissionKind::Paratrooper) {
            if (st.payloadsLeft > 0 && along >= 0.0) {
                st.payloadsLeft = 0;
                const auto& rVec = paraRosters_[static_cast<std::size_t>(st.owner)];
                std::vector<assets::ParatrooperEntry> dropRoster = rVec;
                if (dropRoster.empty()) dropRoster.push_back({"dsmguner", 6});
                airEvents_.push_back(AirStrikeEvent{
                    AirStrikeEvent::ParatrooperDrop, st.targetX, st.targetY, st.owner,
                    paraGroups_[static_cast<std::size_t>(st.owner)], std::move(dropRoster),
                    st.scoreValue, st.planeId
                });
            }
        } else if (st.kind == assets::AirMissionKind::Supply) {
            if (st.payloadsLeft > 0 && along >= 0.0) {
                st.payloadsLeft = 0;
                for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
                    auto& tv = vehicles_[vi];
                    if (!tv.alive || tv.hp <= 0 || isHostile(st.owner, tv.owner) || tv.owner >= 3) continue;
                    if (std::hypot(tv.fx - static_cast<double>(st.targetX),
                                   tv.fy - static_cast<double>(st.targetY)) > 220.0) {
                        continue;
                    }
                    bool didRep = false;
                    bool didRes = false;
                    if (tv.hp < tv.maxHp) {
                        tv.hp = std::min(tv.maxHp, tv.hp + 80);
                        didRep = true;
                    }
                    if (tv.hasCrew && tv.crewHp < tv.maxCrewHp) {
                        tv.crewHp = tv.maxCrewHp;
                        didRep = true;
                    }
                    if (tv.maxAmmo > 0 && tv.ammo < tv.maxAmmo) {
                        tv.ammo = std::min(tv.maxAmmo, tv.ammo + std::max(1, tv.maxAmmo / 2));
                        didRes = true;
                    }
                    if (tv.maxAmmo2 > 0 && tv.ammo2 < tv.maxAmmo2) {
                        tv.ammo2 = std::min(tv.maxAmmo2, tv.ammo2 + std::max(10, tv.maxAmmo2 / 2));
                        didRes = true;
                    }
                    if (didRep || didRes) {
                        emitVehiclePose(vi, false, 0, 0, false, {}, false, false, didRep, didRes);
                    }
                }
                for (size_t si = 0; si < soldiers_.size(); ++si) {
                    auto& ss = soldiers_[si];
                    if (ss.hp <= 0 || ss.embarked || ss.garrisonBuilding ||
                        isHostile(st.owner, ss.owner) || ss.owner >= 3) {
                        continue;
                    }
                    const auto [su, sv] = soldierWorld(ss);
                    const int sx = originX_ + su - sv;
                    const int sy = (su + sv) / 2;
                    if (std::hypot(static_cast<double>(sx - st.targetX),
                                   static_cast<double>(sy - st.targetY)) > 220.0) {
                        continue;
                    }
                    const int maxInfAmmo = std::max(1, ss.desc.maxAmmo);
                    if (ss.hp < ss.maxHp || ss.ammo < maxInfAmmo) {
                        ss.hp = std::min(ss.maxHp, ss.hp + 40);
                        ss.ammo = std::max(ss.ammo, std::min(maxInfAmmo, ss.ammo + std::max(1, maxInfAmmo / 2)));
                        const bool vis = !fog_ || isAlliedWithPlayer(ss.owner) || (ss.spottedMask & 1u) != 0;
                        pushOrUpdateSoldierPose(changed, {
                            si, sx, sy, ss.direction, 0, false, ss.hp, true, ss.owner, ss.group, false, vis
                        });
                    }
                }
                airEvents_.push_back(AirStrikeEvent{
                    AirStrikeEvent::SupplyDrop, ix, iy, st.owner, 0, {}, st.scoreValue, st.planeId
                });
            }
        }

        const bool outOfBounds = st.fx < -160.0 || st.fx > static_cast<double>(mapPixelW_ + 160) ||
                                 st.fy < -160.0 || st.fy > static_cast<double>(mapPixelH_ + 160);
        pushSortiePose(AirSortiePose{
            st.id, st.kind, st.planeId, st.owner, ix, iy, st.dir32, st.hp, st.maxHp, true, !outOfBounds,
            aaFired, aaOriginX, aaOriginY, aaSound
        });
        if (!outOfBounds) aliveSorties.push_back(std::move(st));
    }
    sorties_ = std::move(aliveSorties);
}
}
