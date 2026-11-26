#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ss2 {
class StateHasher;

// Gold 2.2 fog-of-war grid (0x1064239c) and Bresenham LOS raycaster (0x1006c780 / 0x1006cab0).
class FogGrid {
public:
    FogGrid(int tileWidth, int tileHeight, std::vector<std::uint32_t> flags);

    bool loadCirc64(std::span<const std::uint8_t> data);
    bool loadCirc64FromGame(const std::filesystem::path& gameDir);
    void buildFallbackCirc64();
    void applyMapPerimeterFlags();

    static constexpr std::uint32_t kSmokeLosFlag = 0x00100000u;

    void clearAll();
    void clearCurrentVisibility();
    bool setRaw(std::span<const std::uint8_t> raw);
    void revealCircle(int tileX, int tileY, int radius, int side);
    void castRay(int startX, int startY, int dx, int dy, std::uint8_t mask);
    void setSmokeOccluder(int tileX, int tileY, bool blocked);
    bool hasSmokeOccluder(int tileX, int tileY) const;

    std::uint8_t tile(int tileX, int tileY) const;
    bool isElevated(int tileX, int tileY) const;
    bool isForestCover(int tileX, int tileY) const;
    std::span<const std::uint8_t> raw() const { return fog_; }
    int width() const { return tileWidth_; }
    int height() const { return tileHeight_; }

    // Feeds every member into a state hash (tutorial_units_hash.cpp).
    void hashState(StateHasher&) const;

private:
    int tileWidth_{};
    int tileHeight_{};
    std::vector<std::uint32_t> flags_;
    std::vector<std::uint8_t> fog_;
    std::array<std::vector<std::pair<std::int8_t, std::int8_t>>, 64> circ64_{};
    bool hasCirc64_{false};
};

// Infantry combat & spotting descriptor matching desc_common.sue UNITS\* fields.
struct InfantryCombatDesc {
    int maxHp{100};
    int maxAmmo{40};
    int sightBase{12};
    int sightDelta{1};
    int crouchSightBase{9};
    int crouchSightDelta{1};
    int radius{5};

    int damageBase{49};
    int damageDelta{5};
    int rangeBase{665};
    int rangeDelta{66};
    int accuracyBase{9};
    int accuracyDelta{-1};
    int deadzoneBase{8};
    int deadzoneDelta{0};
    int projectileSpeed{64};
    int expaBase{7};
    int expaDelta{-7};
    int reloadBase{39};
    int reloadDelta{-6};
    int burstCount{5};
    int burstReloadBase{67};
    int burstReloadDelta{-11};
    int armorSniper{1};

    static InfantryCombatDesc dguner();
    static InfantryCombatDesc aguner();
    static InfantryCombatDesc forUnitId(const std::string& id);
};

// Effective sight radius matching CSoldier::GetSightRadius (0x100212c0).
int effectiveSightRadius(const InfantryCombatDesc& desc, int expa, bool crouched, int owner,
                         int difficulty = 2, bool moving = false, int weatherVisPct = 100);

struct CombatUnit {
    std::uint32_t uid{};
    int owner{};
    int u{};
    int v{};
    int hp{100};
    int ammo{40};
    int expa{0};
    bool crouched{false};
    InfantryCombatDesc desc{};

    std::uint8_t spottedMask{0};
    std::uint16_t orderId{1};       // 1 = idle, 3 = attack unit
    std::uint16_t orderQueue{0};    // 0 = none, 0xffff = active order accepted
    std::uint32_t targetUid{0};
    std::uint8_t direction{0};      // 0..7 clockwise from +U
    std::uint8_t actionState{0};    // 0 = idle, 1 = firing
    std::uint8_t animTicks{0};      // remaining fire animation ticks
    std::int16_t reload{0};
    std::uint8_t burst{0};
    std::uint32_t pendingTargetUid{0};
};

struct CombatEvent {
    int tick{};
    std::string kind;
    int a{};
    int b{};
    int c{};
    int d{};
    std::uint32_t rng{};
};

struct CombatTickSnapshot {
    int tick{};
    std::uint16_t orderId{};
    std::uint16_t orderQueue{};
    std::uint8_t direction{};
    std::uint8_t actionState{};
    std::uint8_t animTicks{};
    std::int16_t reload{};
    std::uint8_t burst{};
    int ammo{};
    int expa{};
    std::uint8_t targetSpotted{};
    int targetHp{};
    std::uint32_t rng{};
};

// Gold 2.2 matched spotting + infantry combat simulation world.
class CombatWorld {
public:
    CombatWorld(FogGrid fog, int difficulty = 2, std::uint32_t rngSeed = 0x12345678u);

    std::size_t addUnit(CombatUnit unit);
    CombatUnit* findUnit(std::uint32_t uid);
    const CombatUnit* findUnit(std::uint32_t uid) const;
    const std::vector<CombatUnit>& units() const { return units_; }

    void setUnitPosition(std::uint32_t uid, int u, int v);
    void refreshSpotting();

    // Unit::IsValidEnemyTarget (0x10049320)
    bool isValidEnemyTarget(std::uint32_t targetUid, int viewerOwner) const;

    // MapPointerHitTest mode=2 (0x1009b880)
    std::uint32_t hitTestEnemy(int screenX, int screenY, int camX, int camY, int viewerOwner) const;

    // Right-click attack order (0x100867f0)
    bool issueAttackOrder(std::uint32_t attackerUid, std::uint32_t targetUid);

    // Advance one simulation tick (0x1001ebb0 + 0x10088100).
    // If activeAttackerUid != 0, ticks only that attacker (matching single-attacker oracle fixture).
    void step(std::uint32_t activeAttackerUid = 0);

    CombatTickSnapshot snapshot(int tick, std::uint32_t attackerUid, std::uint32_t targetUid) const;
    std::uint32_t rngState() const { return rngState_; }
    const std::vector<CombatEvent>& events() const { return events_; }
    void clearEvents() { events_.clear(); }
    const FogGrid& fog() const { return fog_; }

private:
    struct Projectile {
        std::uint32_t attackerUid{};
        std::uint32_t targetUid{};
        int fromU{}, fromV{};
        int toU{}, toV{};
        int impactU{}, impactV{};
        int rawDamage{};
        int hitPct{};
        int flightTicks{};
        int ticksRemaining{};
    };

    void stepUnit(CombatUnit& unit, int tick);
    void launchShot(CombatUnit& attacker, const CombatUnit& target, int tick);
    void stepProjectiles(int tick);
    std::uint32_t nextRandom(std::uint32_t mask);

    FogGrid fog_;
    int difficulty_{2};
    std::uint32_t rngState_{0x12345678u};
    int tick_{0};
    std::vector<CombatUnit> units_;
    std::vector<Projectile> projectiles_;
    std::vector<CombatEvent> events_;
};

} // namespace ss2
