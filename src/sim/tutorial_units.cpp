#include "sim/tutorial_units.hpp"
#include "sim/tutorial_coordinates.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace ss2 {
namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;

SubCell pointerCell(int x, int y) {
    const auto [u,v] = tutorialPointerWorld(x,y);
    return InfantryPathGrid::fromWorld(u,v);
}
SubCell placementCell(int x, int y) {
    const auto [u,v] = tutorialFootWorld(x,y);
    return InfantryPathGrid::fromWorld(u,v);
}
bool mapBounds(int x, int y) {
    return x >= 0 && x < 8192 && y >= 0 && y < 4096;
}
bool sameTile(SubCell a, SubCell b) {
    return a.x/2 == b.x/2 && a.y/2 == b.y/2;
}
std::uint8_t directionFromDelta(int du, int dv) {
    if (du == 0 && dv == 0) return 0;
    const double angle = std::atan2(double(dv), double(du));
    const int oct = int(std::llround(angle / (kTwoPi / 8.0)));
    return static_cast<std::uint8_t>((oct % 8 + 8) % 8);
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
}
TutorialUnits::TutorialUnits(InfantryPathGrid grid, std::span<const assets::Infantry> units,
                             std::span<const assets::Placement> vehicles,
                             std::optional<FogGrid> fog, int difficulty, bool autoEngage)
    : grid_(std::move(grid)), fog_(std::move(fog)), difficulty_(difficulty), autoEngage_(autoEngage) {
    for (const auto& unit : units) {
        const auto cell = placementCell(unit.x, unit.y);
        const auto desc = InfantryCombatDesc::forUnitId(unit.id);
        Soldier s{};
        s.cell = cell;
        s.owner = unit.owner;
        s.hp = unit.hp > 0 ? unit.hp : desc.maxHp;
        s.ammo = unit.ammo > 0 ? unit.ammo : desc.maxAmmo;
        s.group = unit.group;
        s.desc = desc;
        s.direction = static_cast<std::uint8_t>(unit.direction & 7u);
        soldiers_.push_back(std::move(s));
        grid_.setOccupied(cell, true);
    }
    for (const auto& unit : vehicles) {
        const auto cell = placementCell(unit.x, unit.y);
        grid_.setOccupied(cell, true);
        vehicles_.push_back({cell, unit.owner, unit.group});
    }
    if (fog_) refreshSpotting();
}

bool TutorialUnits::canMove(size_t index, int x, int y) const {
    if (!mapBounds(x,y) || index >= soldiers_.size()) return false;
    const auto& soldier = soldiers_[index];
    if (soldier.owner != 0 || soldier.hp <= 0) return false;
    const auto dest = pointerCell(x,y);
    if (!grid_.inBounds(dest)) return false;
    return (soldier.goal && sameTile(dest, *soldier.goal)) || grid_.hasTerrainRoute(soldier.cell,dest);
}

bool TutorialUnits::order(size_t index, int x, int y) {
    if (!canMove(index,x,y)) return false;
    auto& soldier = soldiers_[index];
    soldier.attackTarget.reset();
    const auto dest = pointerCell(x,y);
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

std::vector<TutorialUnits::Destination> TutorialUnits::formationTargets(
        std::span<const size_t> ids, int mapX, int mapY) const {
    if (ids.empty() || ids.size() > soldiers_.size() || !mapBounds(mapX, mapY))
        return {};
    std::vector<SubCell> sources;
    sources.reserve(ids.size());
    for (size_t id : ids) {
        if (id >= soldiers_.size() || soldiers_[id].owner != 0 || soldiers_[id].hp <= 0) return {};
        sources.push_back(soldiers_[id].cell);
    }
    const auto tiles = grid_.formationDestinations(sources, pointerCell(mapX,mapY));
    std::vector<Destination> result;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const int x = tiles[i].x/2, y = tiles[i].y/2;
        result.push_back({ids[i],4096+(x-y)*32,(x+y)*16+16});
    }
    return result;
}

bool TutorialUnits::canMoveSelection(std::span<const size_t> ids, int x, int y) const {
    const auto targets = formationTargets(ids,x,y);
    return std::ranges::any_of(targets, [&](const auto& t) { return canMove(t.soldier,t.mapX,t.mapY); });
}

size_t TutorialUnits::orderSelection(std::span<const size_t> ids, int x, int y) {
    size_t accepted = 0;
    for (const auto& t : formationTargets(ids,x,y))
        if (order(t.soldier,t.mapX,t.mapY)) ++accepted;
    return accepted;
}

bool TutorialUnits::isValidAttackTarget(size_t targetIndex, int viewerOwner) const {
    if (!fog_ || targetIndex >= soldiers_.size()) return false;
    const auto& target = soldiers_[targetIndex];
    if (target.hp <= 0 || target.owner == viewerOwner) return false;
    return (target.spottedMask & (1u << viewerOwner)) != 0;
}

size_t TutorialUnits::orderAttack(std::span<const size_t> ids, size_t targetIndex) {
    if (!isValidAttackTarget(targetIndex, 0)) return 0;
    size_t accepted = 0;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0 || s.ammo <= 0) continue;
        s.attackTarget = targetIndex;
        ++accepted;
    }
    return accepted;
}

size_t TutorialUnits::stopSelection(std::span<const size_t> ids) {
    size_t stopped = 0;
    for (size_t id : ids) {
        if (id >= soldiers_.size()) continue;
        auto& s = soldiers_[id];
        if (s.owner != 0 || s.hp <= 0) continue;
        s.goal.reset();
        s.route.clear();
        s.cursor = 0;
        s.needsRoute = false;
        s.attackTarget.reset();
        s.autoTarget.reset();
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

unsigned TutorialUnits::countAliveInGroup(int owner, int group) const {
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.owner == owner && s.group == group;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.owner == owner && v.group == group;
    }));
}

unsigned TutorialUnits::countAliveByGroup(unsigned group) const {
    const int g = int(group);
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) {
        return s.hp > 0 && s.group == g;
    }) + std::ranges::count_if(vehicles_, [&](const auto& v) {
        return v.group == g;
    }));
}

std::pair<int, int> TutorialUnits::soldierWorld(const Soldier& s) const {
    if (s.motion) return s.motion->position();
    return InfantryPathGrid::toWorldCenter(s.cell);
}

void TutorialUnits::refreshSpotting(std::vector<SoldierPose>* changed) {
    if (!fog_) return;
    fog_->clearCurrentVisibility();
    for (const auto& s : soldiers_) {
        if (s.hp <= 0) continue;
        const int r = effectiveSightRadius(s.desc, s.expa, s.crouched, s.owner, difficulty_);
        fog_->revealCircle(s.cell.x / 2, s.cell.y / 2, r, s.owner);
    }
    for (const auto& v : vehicles_) {
        if (v.owner == 0) fog_->revealCircle(v.cell.x / 2, v.cell.y / 2, 12, 0);
    }
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        const bool wasVisible = s.owner == 0 || (s.spottedMask & 1u) != 0;
        if (s.hp <= 0) {
            s.spottedMask = 0;
            continue;
        }
        const std::uint8_t tileFog = fog_->tile(s.cell.x / 2, s.cell.y / 2) & 0x0fu;
        s.spottedMask = static_cast<std::uint8_t>(tileFog & ~(1u << s.owner));
        const bool nowVisible = s.owner == 0 || (s.spottedMask & 1u) != 0;
        if (changed && nowVisible != wasVisible) {
            const auto [u, v] = soldierWorld(s);
            changed->push_back({i, 4096 + u - v, (u + v) / 2, s.direction, 0, false, s.hp, true, s.owner, s.group, false, nowVisible});
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
        return s.hp > 0 && s.goal && s.needsRoute && !s.planning.backoff;
    })) return;
    std::map<int, std::vector<size_t>> requests;
    std::vector<int> priorities;
    priorities.reserve(soldiers_.size());
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        const int base = s.movementPriority();
        priorities.push_back(base);
        if (s.hp > 0 && s.goal && s.needsRoute && !s.planning.backoff)
            requests[std::min(255, base + (s.planning.wait >> 4))].push_back(i);
    }
    for (const auto& [priority, ids] : requests) {
        auto shadow = grid_.liveSlots();
        for (size_t i = 0; i < soldiers_.size(); ++i) {
            if (soldiers_[i].hp <= 0 || priorities[i] < priority) continue;
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
            if (plan.action == Action::giveUp) s.goal.reset();
        }
    }
}

bool TutorialUnits::startNext(Soldier& soldier) {
    if (soldier.hp <= 0 || !soldier.goal || soldier.needsRoute) return false;
    if (soldier.cursor == soldier.route.size()) {
        if (sameTile(soldier.cell, *soldier.goal)) soldier.goal.reset();
        else soldier.needsRoute = true;
        return false;
    }
    const auto next = grid_.acceptStep(soldier.cell,soldier.route[soldier.cursor]);
    if (!next) {
        if (sameTile(soldier.cell, *soldier.goal)) soldier.goal.reset();
        else soldier.needsRoute = true;
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
    return true;
}

void TutorialUnits::stepCombat(std::vector<SoldierPose>& changed) {
    if (!fog_) return;
    if (fogDirty_) refreshSpotting(&changed);

    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& s = soldiers_[i];
        if (s.hp <= 0) continue;
        if (s.attackReload > 0) --s.attackReload;
        if (s.firing) {
            if (s.fireAnim == 255) {
                s.firing = false;
                s.fireAnim = 0;
            } else {
                s.fireAnim = static_cast<std::uint8_t>(std::min(255, int(s.fireAnim) + 15));
            }
        }

        const auto [su, sv] = soldierWorld(s);
        const int maxRange = scaleByExpa(s.desc.rangeBase, s.desc.rangeDelta, s.expa);
        const int minRange = scaleByExpa(s.desc.deadzoneBase, s.desc.deadzoneDelta, s.expa);

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
        if (!activeTarget && autoEngage_ && !s.holdFire && s.ammo > 0) {
            // Keep existing autoTarget while alive, spotted, and in range; otherwise acquire nearest.
            if (s.autoTarget) {
                const size_t tid = *s.autoTarget;
                if (tid >= soldiers_.size() || !isValidAttackTarget(tid, s.owner)) {
                    s.autoTarget.reset();
                } else {
                    const auto [tu, tv] = soldierWorld(soldiers_[tid]);
                    const double d = std::hypot(double(tu - su), double(tv - sv));
                    if (d < double(minRange) || d > double(maxRange)) s.autoTarget.reset();
                }
            }
            if (!s.autoTarget) {
                double bestDist = double(maxRange) + 1.0;
                std::optional<size_t> bestTarget;
                for (size_t j = 0; j < soldiers_.size(); ++j) {
                    if (j == i || !isValidAttackTarget(j, s.owner)) continue;
                    const auto [tu, tv] = soldierWorld(soldiers_[j]);
                    const double d = std::hypot(double(tu - su), double(tv - sv));
                    if (d >= double(minRange) && d <= double(maxRange) && d < bestDist) {
                        bestDist = d;
                        bestTarget = j;
                    }
                }
                s.autoTarget = bestTarget;
            }
            activeTarget = s.autoTarget;
        }
        if (!activeTarget) continue;

        const size_t tid = *activeTarget;
        const auto& t = soldiers_[tid];
        const auto [tu, tv] = soldierWorld(t);
        const double dist = std::hypot(double(tu - su), double(tv - sv));

        if (explicitOrder && dist > double(maxRange)) {
            if (!s.goal || !sameTile(*s.goal, t.cell)) {
                s.goal = t.cell;
                s.route.clear();
                s.cursor = 0;
                s.planning = {};
                s.needsRoute = false;
            }
            continue;
        }

        if (explicitOrder) {
            // Explicit attack in range: halt movement permanently.
            s.goal.reset();
            s.route.clear();
            s.cursor = 0;
            s.needsRoute = false;
        }

        if (!s.motion && dist >= double(minRange) && dist <= double(maxRange) && s.attackReload <= 0) {
            s.direction = directionFromDelta(tu - su, tv - sv);
            s.firing = true;
            s.fireAnim = 0;

            // Exact 0x10059df6..0x10059e59 two-draw scatter
            (void)nextCombatRandom(0x7fffu);
            (void)nextCombatRandom(0x7fffu);

            const int rawDamage = scaleByExpa(s.desc.damageBase, s.desc.damageDelta, s.expa);
            const int speed = std::max(1, s.desc.projectileSpeed);
            const int flightTicks = std::max(1, int(std::ceil(dist / double(speed))));
            const int baseHitPct = sniperHitPercent(t.desc.armorSniper);
            const int hitPct = t.crouched ? std::max(10, baseHitPct - 25) : baseHitPct;
            projectiles_.push_back(Projectile{
                i, tid, rawDamage, hitPct, std::max(1, flightTicks - 1)
            });

            const int expaGain = scaleByExpa(s.desc.expaBase, s.desc.expaDelta, s.expa);
            s.expa = std::min(0x8000, s.expa + expaGain);
            --s.ammo;
            ++s.attackBurst;
            int reloadVal = scaleByExpa(s.desc.reloadBase, s.desc.reloadDelta, s.expa);
            if (s.attackBurst >= s.desc.burstCount) {
                s.attackBurst = 0;
                reloadVal = scaleByExpa(s.desc.burstReloadBase, s.desc.burstReloadDelta, s.expa);
            }
            s.attackReload = static_cast<std::int16_t>(std::max(1, reloadVal));
            const bool vis = s.owner == 0 || (s.spottedMask & 1u) != 0;
            changed.push_back({i, 4096 + su - sv, (su + sv) / 2, s.direction, 0, false, s.hp, true, s.owner, s.group, true, vis,
                               4096 + tu - tv, (tu + tv) / 2});
        }
    }

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
                if (tgt.hp > 0 && tgt.hp < (tgt.desc.maxHp >> 1) && p.rawDamage > (tgt.desc.maxHp >> 4)) {
                    (void)nextCombatRandom(1u);
                }
                const auto [tu, tv] = soldierWorld(tgt);
                if (tgt.hp == 0) {
                    grid_.setOccupied(tgt.cell, false);
                    tgt.motion.reset();
                    tgt.goal.reset();
                    tgt.attackTarget.reset();
                    tgt.autoTarget.reset();
                    tgt.spottedMask = 0;
                    fogDirty_ = true;
                }
                changed.push_back({p.target, 4096 + tu - tv, (tu + tv) / 2, tgt.direction, 0, false, tgt.hp, tgt.hp > 0, tgt.owner, tgt.group});
            }
        }
    }
    projectiles_ = std::move(alive);
    if (fogDirty_) refreshSpotting(&changed);
}

std::vector<SoldierPose> TutorialUnits::step() {
    std::vector<SoldierPose> changed;
    for (size_t i = 0; i < soldiers_.size(); ++i) {
        auto& soldier = soldiers_[i];
        if (soldier.hp <= 0) continue;
        if (soldier.planning.backoff) --soldier.planning.backoff;
        if (!soldier.motion) {
            if (soldier.autoTarget) continue;
            if (!startNext(soldier)) continue;
        } else {
            soldier.motion->advance(soldier.crouched, randomState_);
            if (!soldier.motion->running && !soldier.autoTarget) startNext(soldier);
        }
        const auto& motion = *soldier.motion;
        const auto [u,v] = motion.position();
        changed.push_back({i,4096+u-v,(u+v)/2,motion.direction,motion.progress,motion.running,soldier.hp,true,soldier.owner,soldier.group});
        if (!motion.running) soldier.motion.reset();
    }
    // The unit dispatcher 0x10055ff0 performs the pathfinding sweep after the unit updates.
    refreshRoutes();
    if (fog_) stepCombat(changed);
    return changed;
}

unsigned TutorialUnits::countInZone(std::span<const std::uint8_t> zones, unsigned zone, int owner) const {
    if (zones.size() != 128*128) throw std::invalid_argument("Expected Tutorial I location grid");
    const auto inside = [&](SubCell cell) { return (zones[(cell.y / 2)*128 + cell.x / 2] & 63u) == zone; };
    return unsigned(std::ranges::count_if(soldiers_, [&](const auto& s) { return s.hp > 0 && s.owner == owner && inside(s.cell); })
                  + std::ranges::count_if(vehicles_, [&](const auto& v) { return v.owner == owner && inside(v.cell); }));
}
}
