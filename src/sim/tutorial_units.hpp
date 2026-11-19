#pragma once
#include "assets/infantry.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/infantry_path.hpp"
#include "sim/infantry_step.hpp"
#include <optional>
#include <span>
#include <vector>

namespace ss2 {
struct SoldierPose {
    size_t index;
    int x, y;
    unsigned direction, progress;
    bool walking;
    int hp{100};
    bool alive{true};
    int owner{0};
    int group{0};
    bool fired{false};
    bool visible{true};
    int targetX{0};
    int targetY{0};
};

class TutorialUnits {
public:
    TutorialUnits(InfantryPathGrid, std::span<const assets::Infantry>, std::span<const assets::Placement> vehicles,
                  std::optional<FogGrid> fog = std::nullopt, int difficulty = 2, bool autoEngage = false);
    bool canMove(size_t index, int mapX, int mapY) const;
    bool order(size_t index, int mapX, int mapY);
    bool canMoveSelection(std::span<const size_t> ids, int mapX, int mapY) const;
    size_t orderSelection(std::span<const size_t> ids, int mapX, int mapY);

    bool isValidAttackTarget(size_t targetIndex, int viewerOwner = 0) const;
    size_t orderAttack(std::span<const size_t> ids, size_t targetIndex);
    size_t stopSelection(std::span<const size_t> ids);
    bool toggleHoldFire(std::span<const size_t> ids);
    bool toggleCrouch(std::span<const size_t> ids);
    unsigned countAliveInGroup(int owner, int group) const;
    unsigned countAliveByGroup(unsigned group) const;

    std::vector<SoldierPose> step();
    std::optional<std::vector<std::uint8_t>> takeFogSnapshot();
    unsigned countInZone(std::span<const std::uint8_t> zones, unsigned zone, int owner = 0) const;
private:
    struct Soldier {
        SubCell cell;
        int owner;
        std::vector<std::uint8_t> route;
        size_t cursor{};
        std::optional<InfantryStep> motion;
        std::optional<SubCell> goal;
        InfantryPathGrid::RouteState planning;
        bool needsRoute{};
        int hp{100};
        int ammo{40};
        int expa{0};
        int group{0};
        InfantryCombatDesc desc{};
        std::uint8_t spottedMask{0};
        std::optional<size_t> attackTarget;
        std::optional<size_t> autoTarget;
        std::int16_t attackReload{0};
        std::uint8_t attackBurst{0};
        std::uint8_t direction{0};
        bool firing{false};
        std::uint8_t fireAnim{0};
        bool crouched{false};
        bool holdFire{false};
        int movementPriority() const;
    };
    struct Projectile {
        size_t attacker{};
        size_t target{};
        int rawDamage{};
        int hitPct{};
        int ticksRemaining{};
    };
    struct Vehicle { SubCell cell; int owner; int group; };
    InfantryPathGrid grid_;
    std::vector<Soldier> soldiers_;
    std::vector<Vehicle> vehicles_;
    std::uint32_t randomState_{0x76321f};
    std::optional<FogGrid> fog_;
    int difficulty_{2};
    bool autoEngage_{false};
    bool fogDirty_{false};
    bool fogSnapshotDirty_{false};
    std::uint32_t combatRandomState_{0x12345678u};
    std::vector<Projectile> projectiles_;
    struct Destination { size_t soldier; int mapX, mapY; };
    std::vector<Destination> formationTargets(std::span<const size_t>, int mapX, int mapY) const;
    void refreshRoutes();
    void refreshSpotting(std::vector<SoldierPose>* changed = nullptr);
    bool startNext(Soldier&);
    std::uint32_t nextCombatRandom(std::uint32_t mask);
    std::pair<int, int> soldierWorld(const Soldier&) const;
    void stepCombat(std::vector<SoldierPose>& changed);
};
}
