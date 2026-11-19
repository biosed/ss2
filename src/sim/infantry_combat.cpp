#include "sim/infantry_combat.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace ss2 {
namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;

int octDistance(int du, int dv) {
    const int ax = std::abs(du);
    const int ay = std::abs(dv);
    return ax > ay ? (ax + ay / 2) : (ay + ax / 2);
}

// 8-direction quantization matching FUN_10021400 (0=+U, 1=+U+V, 2=+V, 3=-U+V, 4=-U, 5=-U-V, 6=-V, 7=+U-V).
std::uint8_t directionFromDelta(int du, int dv) {
    if (du == 0 && dv == 0) return 0;
    const double angle = std::atan2(double(dv), double(du));
    const int oct = int(std::llround(angle / (kTwoPi / 8.0)));
    return static_cast<std::uint8_t>((oct % 8 + 8) % 8);
}

// Standing rifle muzzle offset in sub-pixels for each of the 8 facings.
std::pair<int, int> muzzleOffset(std::uint8_t dir) {
    static constexpr std::array<std::pair<int, int>, 8> kOffsets{{
        {10, 0}, {10, 10}, {0, 10}, {-10, 10},
        {-10, 0}, {-10, -10}, {-10, -10}, {10, -10}
    }};
    return kOffsets[dir & 7u];
}

int scaleByExpa(int base, int delta, int expa) {
    return base + ((expa * delta) >> 15);
}

// Hit probability percentage from Gold hit_table (SNIPER hitid=7 vs armor level).
int sniperHitPercent(int armor) {
    switch (armor) {
    case 0: return 100;
    case 1: return 75;
    case 2: return 50;
    case 3: return 25;
    default: return 10;
    }
}
} // namespace

FogGrid::FogGrid(int tileWidth, int tileHeight, std::vector<std::uint32_t> flags)
    : tileWidth_(tileWidth), tileHeight_(tileHeight), flags_(std::move(flags)),
      fog_(static_cast<std::size_t>(tileWidth) * static_cast<std::size_t>(tileHeight), 0) {
    if (tileWidth_ <= 0 || tileHeight_ <= 0)
        throw std::invalid_argument("FogGrid dimensions must be positive");
    if (flags_.size() != fog_.size())
        throw std::invalid_argument("FogGrid P-FLAGS size mismatch");
    buildFallbackCirc64();
}

bool FogGrid::loadCirc64(std::span<const std::uint8_t> data) {
    std::size_t pos = 0;
    std::array<std::vector<std::pair<std::int8_t, std::int8_t>>, 64> parsed{};
    for (int r = 1; r < 64; ++r) {
        if (pos + 2 > data.size()) return false;
        const std::uint16_t cnt = static_cast<std::uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        if (pos + std::size_t(cnt) * 3 > data.size()) return false;
        parsed[r].reserve(cnt);
        for (std::uint16_t i = 0; i < cnt; ++i) {
            const auto dx = static_cast<std::int8_t>(data[pos + i * 3]);
            const auto dy = static_cast<std::int8_t>(data[pos + i * 3 + 1]);
            parsed[r].emplace_back(dx, dy);
        }
        pos += std::size_t(cnt) * 3;
    }
    circ64_ = std::move(parsed);
    hasCirc64_ = true;
    return true;
}

bool FogGrid::loadCirc64FromGame(const std::filesystem::path& gameDir) {
    const auto bytes = assets::readSueEntry(gameDir / "game_common.sue", "CIRC64.DAT");
    return loadCirc64(bytes);
}

void FogGrid::buildFallbackCirc64() {
    for (int r = 1; r < 64; ++r) {
        auto& ring = circ64_[r];
        ring.clear();
        const int r2 = r * r;
        const int inner2 = (r - 1) * (r - 1);
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                const int d2 = dx * dx + dy * dy;
                if (d2 <= r2 && d2 >= inner2)
                    ring.emplace_back(static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dy));
            }
        }
    }
    hasCirc64_ = true;
}

void FogGrid::clearAll() {
    std::fill(fog_.begin(), fog_.end(), 0);
}

void FogGrid::clearCurrentVisibility() {
    for (auto& cell : fog_) cell &= 0xf0u;
}

void FogGrid::castRay(int startX, int startY, int dx, int dy, std::uint8_t mask) {
    if (startX < 0 || startX >= tileWidth_ || startY < 0 || startY >= tileHeight_) return;

    int stepX = 1;
    int absX = dx;
    if (dx < 1) {
        stepX = -1;
        absX = -dx;
    }

    int stepY = 1;
    int absY = dy;
    if (dy < 1) {
        stepY = -1;
        absY = -dy;
    }

    int majorDx = stepX, majorDy = 0;
    int minorDx = 0, minorDy = stepY;
    int fracStep = 0;
    int steps = absX;

    if (absY < absX) {
        fracStep = (absY * 0xffff) / absX;
        steps = absX;
    } else if (absX < absY) {
        fracStep = (absX * 0xffff) / absY;
        majorDx = 0;
        majorDy = stepY;
        minorDx = stepX;
        minorDy = 0;
        steps = absY;
    } else {
        fracStep = 0;
        majorDx = stepX;
        majorDy = stepY;
        minorDx = 0;
        minorDy = stepY;
        steps = absX;
    }

    const auto idxOf = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(tileWidth_) + static_cast<std::size_t>(x);
    };

    const std::size_t startIdx = idxOf(startX, startY);
    fog_[startIdx] |= mask;
    if (steps <= 0) return;

    int curX = startX + majorDx;
    int curY = startY + majorDy;
    unsigned acc = static_cast<unsigned>(fracStep) + 0x8000u;
    if (acc > 0x10000u) {
        acc -= 0x10000u;
        curX += minorDx;
        curY += minorDy;
    }

    bool elevated = (flags_[startIdx] & 0x2000u) != 0;
    while (steps > 0) {
        if (curX < 0 || curX >= tileWidth_ || curY < 0 || curY >= tileHeight_) return;
        const std::size_t idx = idxOf(curX, curY);
        fog_[idx] |= mask;
        const std::uint32_t f = flags_[idx];
        if (!elevated) {
            if ((f & 0x1800u) != 0) return;
            if ((f & 0x2000u) != 0) {
                elevated = true;
                if ((f & 0x0800u) != 0) return;
            }
        } else {
            if ((f & 0x0800u) != 0) return;
        }
        curX += majorDx;
        curY += majorDy;
        acc += static_cast<unsigned>(fracStep);
        if (acc > 0x10000u) {
            acc -= 0x10000u;
            curX += minorDx;
            curY += minorDy;
        }
        --steps;
    }
}

void FogGrid::revealCircle(int tileX, int tileY, int radius, int side) {
    if (side < 0 || side >= 4 || radius <= 0 || !hasCirc64_) return;
    const int r = std::min(radius, 63);
    const auto mask = static_cast<std::uint8_t>(0x11u << side);
    for (const auto& [dx, dy] : circ64_[r]) {
        castRay(tileX, tileY, dx, dy, mask);
    }
}

std::uint8_t FogGrid::tile(int tileX, int tileY) const {
    if (tileX < 0 || tileX >= tileWidth_ || tileY < 0 || tileY >= tileHeight_) return 0;
    return fog_[static_cast<std::size_t>(tileY) * static_cast<std::size_t>(tileWidth_) + static_cast<std::size_t>(tileX)];
}

InfantryCombatDesc InfantryCombatDesc::dguner() {
    InfantryCombatDesc d;
    d.maxHp = 100;
    d.maxAmmo = 40;
    d.sightBase = 12;
    d.sightDelta = 1;
    d.crouchSightBase = 9;
    d.crouchSightDelta = 1;
    d.radius = 5;
    d.damageBase = 49;
    d.damageDelta = 5;
    d.rangeBase = 665;
    d.rangeDelta = 66;
    d.accuracyBase = 9;
    d.accuracyDelta = -1;
    d.deadzoneBase = 8;
    d.deadzoneDelta = 0;
    d.projectileSpeed = 64;
    d.expaBase = 7;
    d.expaDelta = -7;
    d.reloadBase = 39;
    d.reloadDelta = -6;
    d.burstCount = 5;
    d.burstReloadBase = 67;
    d.burstReloadDelta = -11;
    d.armorSniper = 1;
    return d;
}

InfantryCombatDesc InfantryCombatDesc::aguner() {
    InfantryCombatDesc d = dguner();
    d.maxAmmo = 64;
    d.damageBase = 41;
    d.damageDelta = 4;
    d.rangeBase = 691;
    d.rangeDelta = 69;
    d.accuracyBase = 8;
    d.reloadBase = 38;
    d.burstCount = 8;
    return d;
}

InfantryCombatDesc InfantryCombatDesc::forUnitId(const std::string& id) {
    if (id == "aguner") return aguner();
    if (id == "dsmguner" || id == "rsmguner") {
        auto d = dguner();
        d.sightBase = 11;
        d.crouchSightBase = 8;
        return d;
    }
    if (id == "dofficer" || id == "aofficer") {
        auto d = dguner();
        d.sightBase = 13;
        d.crouchSightBase = 10;
        return d;
    }
    return dguner();
}

int effectiveSightRadius(const InfantryCombatDesc& desc, int expa, bool crouched, int owner, int difficulty) {
    const int base = crouched ? desc.crouchSightBase : desc.sightBase;
    const int delta = crouched ? desc.crouchSightDelta : desc.sightDelta;
    int r = scaleByExpa(base, delta, expa);
    if (difficulty == 1 && owner == 0) {
        r = (r * 130) / 100;
    }
    return std::clamp(r, 1, 63);
}

CombatWorld::CombatWorld(FogGrid fog, int difficulty, std::uint32_t rngSeed)
    : fog_(std::move(fog)), difficulty_(difficulty), rngState_(rngSeed) {}

std::size_t CombatWorld::addUnit(CombatUnit unit) {
    units_.push_back(std::move(unit));
    return units_.size() - 1;
}

CombatUnit* CombatWorld::findUnit(std::uint32_t uid) {
    for (auto& u : units_) {
        if (u.uid == uid) return &u;
    }
    return nullptr;
}

const CombatUnit* CombatWorld::findUnit(std::uint32_t uid) const {
    for (const auto& u : units_) {
        if (u.uid == uid) return &u;
    }
    return nullptr;
}

void CombatWorld::setUnitPosition(std::uint32_t uid, int u, int v) {
    if (auto* unit = findUnit(uid)) {
        unit->u = u;
        unit->v = v;
    }
}

void CombatWorld::refreshSpotting() {
    fog_.clearAll();
    for (const auto& u : units_) {
        if (u.hp <= 0) continue;
        const int r = effectiveSightRadius(u.desc, u.expa, u.crouched, u.owner, difficulty_);
        fog_.revealCircle(u.u >> 5, u.v >> 5, r, u.owner);
    }
    for (auto& u : units_) {
        if (u.hp <= 0) {
            u.spottedMask = 0;
            continue;
        }
        const std::uint8_t tileFog = fog_.tile(u.u >> 5, u.v >> 5) & 0x0fu;
        // Enemy spotted mask excludes own owner bit when queried for hostility, but stores full visibility nibble.
        u.spottedMask = static_cast<std::uint8_t>(tileFog & ~(1u << u.owner));
    }
}

bool CombatWorld::isValidEnemyTarget(std::uint32_t targetUid, int viewerOwner) const {
    const auto* target = findUnit(targetUid);
    if (!target || target->hp <= 0) return false;
    if (target->owner == viewerOwner) return false;
    return (target->spottedMask & (1u << viewerOwner)) != 0;
}

std::uint32_t CombatWorld::hitTestEnemy(int screenX, int screenY, int camX, int camY, int viewerOwner) const {
    const int mx = screenX + camX;
    const int my = screenY + camY;
    const int halfX = mx >> 1;
    const int wu = my + halfX;
    const int wv = my - halfX;

    int bestScore = 24;
    std::uint32_t bestUid = 0;
    for (const auto& u : units_) {
        if (u.hp <= 0 || u.owner == viewerOwner) continue;
        if ((u.spottedMask & (1u << viewerOwner)) == 0) continue;
        const int score = octDistance(u.u - wu, u.v - wv) - (u.desc.radius >> 1);
        if (score < bestScore) {
            bestScore = score;
            bestUid = u.uid;
        }
    }
    return bestUid;
}

bool CombatWorld::issueAttackOrder(std::uint32_t attackerUid, std::uint32_t targetUid) {
    auto* attacker = findUnit(attackerUid);
    if (!attacker || attacker->hp <= 0) return false;
    if (!isValidEnemyTarget(targetUid, attacker->owner)) return false;
    attacker->pendingTargetUid = targetUid;
    return true;
}

std::uint32_t CombatWorld::nextRandom(std::uint32_t mask) {
    rngState_ = rngState_ * 0x41c64e6du + 0x3039u;
    const auto shifted = static_cast<std::int32_t>(rngState_) >> 16;
    return static_cast<std::uint32_t>(shifted) & mask;
}

void CombatWorld::launchShot(CombatUnit& attacker, const CombatUnit& target, int tick) {
    const std::uint32_t rngBefore = rngState_;
    events_.push_back({tick, "FIRE_START", int(attacker.uid), int(target.uid), 0, 0, rngBefore});

    attacker.direction = directionFromDelta(target.u - attacker.u, target.v - attacker.v);
    attacker.actionState = 1;
    attacker.animTicks = 0;

    const auto [mdu, mdv] = muzzleOffset(attacker.direction);
    const int fromU = attacker.u + mdu;
    const int fromV = attacker.v + mdv;
    const int toU = target.u;
    const int toV = target.v;

    const int rawDamage = scaleByExpa(attacker.desc.damageBase, attacker.desc.damageDelta, attacker.expa);
    const int maxRange = scaleByExpa(attacker.desc.rangeBase, attacker.desc.rangeDelta, attacker.expa);
    const int acc = scaleByExpa(attacker.desc.accuracyBase, attacker.desc.accuracyDelta, attacker.expa);

    const double dist = std::hypot(double(toU - fromU), double(toV - fromV));
    const int spread = maxRange > 0 ? int((double(acc) * dist) / double(maxRange)) : 0;

    // Exact 0x10059df6..0x10059e59 two-draw scatter
    const std::uint32_t rAngle = nextRandom(0x7fffu);
    const std::uint32_t rRad = nextRandom(0x7fffu);
    const double angle = (double(rAngle) / 32768.0) * kTwoPi;
    const int radius = (int(rRad) * spread) >> 15;
    const int impactU = toU + int(std::llround(std::sin(angle) * double(radius)));
    const int impactV = toV + int(std::llround(std::cos(angle) * double(radius)));

    const int speed = std::max(1, attacker.desc.projectileSpeed);
    const int flightTicks = std::max(1, int(std::ceil(dist / double(speed))));

    events_.push_back({tick, "PROJ_LAUNCH", fromU, fromV, toU, toV, static_cast<std::uint32_t>(rawDamage)});

    projectiles_.push_back(Projectile{
        attacker.uid,
        target.uid,
        fromU, fromV,
        toU, toV,
        impactU, impactV,
        rawDamage,
        sniperHitPercent(target.desc.armorSniper),
        flightTicks,
        std::max(1, flightTicks - 1) // launch tick executes first step immediately, then stepProjectiles runs at end of tick
    });

    const int expaGain = scaleByExpa(attacker.desc.expaBase, attacker.desc.expaDelta, attacker.expa);
    attacker.expa = std::min(0x8000, attacker.expa + expaGain);
    if (attacker.ammo > 0) --attacker.ammo;
    ++attacker.burst;

    int reloadVal = scaleByExpa(attacker.desc.reloadBase, attacker.desc.reloadDelta, attacker.expa);
    if (attacker.burst >= attacker.desc.burstCount) {
        attacker.burst = 0;
        reloadVal = scaleByExpa(attacker.desc.burstReloadBase, attacker.desc.burstReloadDelta, attacker.expa);
    }
    attacker.reload = static_cast<std::int16_t>(std::max(1, reloadVal));
}

void CombatWorld::stepUnit(CombatUnit& unit, int tick) {
    if (unit.hp <= 0) return;

    // Accept queued attack order on first tick after issueAttackOrder (0x100867f0 -> 0x1001ebb0)
    if (unit.pendingTargetUid != 0) {
        const std::uint32_t targetUid = unit.pendingTargetUid;
        unit.pendingTargetUid = 0;
        unit.orderId = 3;
        unit.orderQueue = 0xffffu;
        unit.targetUid = targetUid;
        events_.push_back({tick, "ORDER_ENQUEUE", 3, int(targetUid), 0, 0, rngState_});
        events_.push_back({tick, "ORDER_ACCEPT", 3, int(targetUid), 0, 0, rngState_});
    }

    // Advance reload timer
    if (unit.reload > 0) {
        --unit.reload;
    }

    // Advance firing animation (15 per tick up to 255, then return to idle)
    if (unit.actionState == 1) {
        if (unit.animTicks == 255) {
            unit.actionState = 0;
            unit.animTicks = 0;
        } else {
            unit.animTicks = static_cast<std::uint8_t>(std::min(255, int(unit.animTicks) + 15));
        }
    }

    // Execute attack order if ready to fire
    if (unit.orderId == 3 && unit.targetUid != 0 && unit.reload <= 0 && unit.ammo > 0) {
        const auto* target = findUnit(unit.targetUid);
        if (!target || target->hp <= 0 || !isValidEnemyTarget(target->uid, unit.owner)) {
            unit.orderId = 1;
            unit.orderQueue = 0;
            unit.targetUid = 0;
            return;
        }
        const int maxRange = scaleByExpa(unit.desc.rangeBase, unit.desc.rangeDelta, unit.expa);
        const int minRange = scaleByExpa(unit.desc.deadzoneBase, unit.desc.deadzoneDelta, unit.expa);
        const double dist = std::hypot(double(target->u - unit.u), double(target->v - unit.v));
        if (dist >= double(minRange) && dist <= double(maxRange)) {
            launchShot(unit, *target, tick);
        }
    }
}

void CombatWorld::stepProjectiles(int tick) {
    std::vector<Projectile> alive;
    alive.reserve(projectiles_.size());
    for (auto& proj : projectiles_) {
        --proj.ticksRemaining;
        if (proj.ticksRemaining > 0) {
            alive.push_back(proj);
            continue;
        }

        // Exact 0x1005a0c9 impact draw (mask=0xff)
        (void)nextRandom(0xffu);
        events_.push_back({tick, "PROJ_IMPACT", proj.impactU, proj.impactV, proj.flightTicks, proj.rawDamage, rngState_});

        // Exact 0x10057120..0x1005714e hit roll
        const std::uint32_t rHit = nextRandom(0x7fffu);
        const int roll = (int(rHit) * 100) >> 15;
        const bool hit = roll <= proj.hitPct;
        events_.push_back({tick, "HIT_ROLL", roll, proj.hitPct, hit ? 1 : 0, 0, rngState_});

        if (hit) {
            if (auto* target = findUnit(proj.targetUid)) {
                if (target->hp > 0) {
                    const int oldHp = target->hp;
                    const int rawNewHp = oldHp - proj.rawDamage;
                    target->hp = std::max(0, rawNewHp);
                    events_.push_back({tick, "HP_APPLY", int(target->uid), -proj.rawDamage, oldHp, rawNewHp, rngState_});

                    // Exact 0x10048952..0x10048992 hurt voice-line coin flip
                    if (target->hp > 0 && target->hp < (target->desc.maxHp >> 1) &&
                        proj.rawDamage > (target->desc.maxHp >> 4)) {
                        (void)nextRandom(1u);
                    }
                }
            }
        }
    }
    projectiles_ = std::move(alive);
}

void CombatWorld::step(std::uint32_t activeAttackerUid) {
    const int curTick = tick_;
    if (activeAttackerUid != 0) {
        if (auto* unit = findUnit(activeAttackerUid)) {
            stepUnit(*unit, curTick);
        }
    } else {
        for (auto& unit : units_) {
            stepUnit(unit, curTick);
        }
    }
    stepProjectiles(curTick);
    ++tick_;
}

CombatTickSnapshot CombatWorld::snapshot(int tick, std::uint32_t attackerUid, std::uint32_t targetUid) const {
    CombatTickSnapshot s{};
    s.tick = tick;
    s.rng = rngState_;
    if (const auto* a = findUnit(attackerUid)) {
        s.orderId = a->orderId;
        s.orderQueue = a->orderQueue;
        s.direction = a->direction;
        s.actionState = a->actionState;
        s.animTicks = a->animTicks;
        s.reload = a->reload;
        s.burst = a->burst;
        s.ammo = a->ammo;
        s.expa = a->expa;
    }
    if (const auto* t = findUnit(targetUid)) {
        s.targetSpotted = t->spottedMask;
        s.targetHp = t->hp;
    }
    return s;
}

} // namespace ss2
