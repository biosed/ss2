// Full-state hash of TutorialUnits. Kept apart from tutorial_units.cpp so the
// field list sits next to its completeness guards.
//
// When you add a field to any struct hashed here, hash it below (or document
// why it is excluded) and update the matching sizeof guard.
#include "sim/state_hash.hpp"
#include "sim/tutorial_units.hpp"

namespace ss2 {
namespace assets {
static void hashValue(StateHasher& h, const DirectionalArmor& a) { h.all(a.front, a.side, a.rear); }
static void hashValue(StateHasher& h, const AirSupportSlot& s) { h.all(s.planeId, s.total, s.ready); }
static void hashValue(StateHasher& h, const ParatrooperEntry& p) { h.all(p.unitId, p.count); }
} // namespace assets

static void hashValue(StateHasher& h, const SubCell& c) { h.all(c.x, c.y); }

static void hashValue(StateHasher& h, const InfantryStep& s) {
    h.all(s.tileU, s.tileV, s.slot, s.direction, s.progress, s.running);
}

static void hashValue(StateHasher& h, const InfantryPathGrid::RouteState& r) {
    h.all(r.baselineCost, r.baselineProgress, r.wait, r.penalty, r.backoff);
}

static void hashValue(StateHasher& h, const InfantryCombatDesc& d) {
    h.all(d.maxHp, d.maxAmmo, d.sightBase, d.sightDelta, d.crouchSightBase, d.crouchSightDelta, d.radius,
          d.damageBase, d.damageDelta, d.rangeBase, d.rangeDelta, d.accuracyBase, d.accuracyDelta,
          d.deadzoneBase, d.deadzoneDelta, d.projectileSpeed, d.expaBase, d.expaDelta, d.reloadBase,
          d.reloadDelta, d.burstCount, d.burstReloadBase, d.burstReloadDelta, d.armorSniper);
}

static void hashValue(StateHasher& h, const TutorialUnits::ReserveUnit& r) {
    h.all(r.id, r.owner, r.group, r.hp, r.ammo, r.expa, r.pool);
}

static void hashValue(StateHasher& h, const FogGrid& f) { f.hashState(h); }
static void hashValue(StateHasher& h, const InfantryPathGrid& g) { g.hashState(h); }

void FogGrid::hashState(StateHasher& h) const {
    h.all(tileWidth_, tileHeight_, flags_, fog_, circ64_, hasCirc64_);
}

void InfantryPathGrid::hashState(StateHasher& h) const {
    // Terrain never changes after construction but is cheap to include; occupancy is live state.
    h.all(tileWidth_, tileHeight_, subWidth_, subHeight_, borderMargin_, flags_, obstacles_, walkable_, occupancy_);
}

// Completeness guards. sizeof changes whenever a field is added or removed, so
// these fail loudly until stateHash() is updated to match. Types holding
// std::string/vector/deque/optional only have a stable size with libstdc++ on
// an LP64 ABI (x86-64 and aarch64 agree); libc++ and MSVC lay those out
// differently, so the guards for them are limited to that configuration.
static_assert(sizeof(SubCell) == 8, "update hashValue(SubCell) for the new field");
static_assert(sizeof(InfantryStep) == 20, "update hashValue(InfantryStep) for the new field");
static_assert(sizeof(InfantryCombatDesc) == 24 * sizeof(int), "update hashValue(InfantryCombatDesc)");
static_assert(sizeof(assets::DirectionalArmor) == 3 * sizeof(int), "update hashValue(DirectionalArmor)");
#if defined(__GLIBCXX__) && defined(__LP64__)
#define SS2_STATE_HASH_SIZE_GUARDS 1
static_assert(sizeof(InfantryPathGrid::RouteState) == 24, "update hashValue(RouteState)");
static_assert(sizeof(InfantryPathGrid) == 120, "update InfantryPathGrid::hashState");
static_assert(sizeof(FogGrid) == 1600, "update FogGrid::hashState");
static_assert(sizeof(TutorialUnits::ReserveUnit) == 56, "update hashValue(ReserveUnit)");
static_assert(sizeof(assets::AirSupportSlot) == 40, "update hashValue(AirSupportSlot)");
static_assert(sizeof(assets::ParatrooperEntry) == 40, "update hashValue(ParatrooperEntry)");
static_assert(sizeof(TutorialUnits) == 4064, "update TutorialUnits::stateHash for the new member");
#endif

std::uint64_t TutorialUnits::stateHash() const {
#ifdef SS2_STATE_HASH_SIZE_GUARDS
    static_assert(sizeof(Soldier) == 848, "update the Soldier hash below for the new field");
    static_assert(sizeof(Vehicle) == 1584, "update the Vehicle hash below for the new field");
    static_assert(sizeof(Building) == 80, "update the Building hash below for the new field");
    static_assert(sizeof(Bridge) == 64, "update the Bridge hash below for the new field");
    static_assert(sizeof(Projectile) == 32, "update the Projectile hash below for the new field");
    static_assert(sizeof(ActiveSortie) == 152, "update the ActiveSortie hash below for the new field");
    static_assert(sizeof(ActiveParachute) == 80, "update the ActiveParachute hash below for the new field");
#endif
    StateHasher h;

    h.add(grid_);
    h.all(originX_, mapPixelW_, mapPixelH_);

    h.each(soldiers_, [&h](const Soldier& s) {
        h.all(s.cell, s.owner, s.route, s.cursor, s.motion, s.goal, s.planning, s.needsRoute);
        h.all(s.hp, s.maxHp, s.ammo, s.expa, s.group, s.desc, s.spottedMask);
        h.all(s.attackTarget, s.attackVehicleTarget, s.autoTarget, s.attackReload, s.attackBurst, s.direction);
        h.all(s.firing, s.fireAnim, s.crouched, s.holdFire);
        h.all(s.isMedic, s.isOfficer, s.isAntiVehicle, s.isFlamer, s.isSniper, s.isSapper, s.isMortar,
              s.isHeavyMg, s.hasBinoculars, s.binoSightBonus, s.isKamikaze);
        h.all(s.mineAmmo, s.kills, s.atGrenades, s.binoculars, s.embarked, s.lastFireTick, s.wasCamouflaged);
        h.all(s.boardVehicleTarget, s.captureVehicleTarget, s.garrisonBuilding, s.garrisonTarget,
              s.attackBuildingTarget, s.attackGround, s.healTarget, s.escortVehicleTarget,
              s.escortSoldierTarget, s.disarmMineTarget);
        h.all(s.attackMove, s.queuedGoals, s.patrolOrigin, s.patrolDest, s.patrolToDest, s.lastReactTick);
        h.all(s.attackPref, s.hasAttackPref, s.suppressionTicks, s.wasSuppressed);
        h.all(s.officerAuraRadius, s.moraleRallyRate, s.moraleResist, s.fearless, s.resupplyCost, s.canBeCrew,
              s.unitRadius, s.canFireFromInside, s.crouchToFire, s.expaOnHit, s.canGrenadeGround);
        h.all(s.canLayMines, s.movingSightPenalty, s.maxMineAmmo, s.getMineTime, s.getMineTimeMax, s.disarmWorkTicks);
        h.all(s.standStepBase, s.standStepMax, s.crouchStepBase, s.crouchStepMax);
    });

    h.each(vehicles_, [&h](const Vehicle& v) {
        h.all(v.id, v.cell, v.x, v.y, v.fx, v.fy, v.hullDir, v.turretDir);
        h.all(v.hp, v.maxHp, v.ammo, v.owner, v.group, v.alive, v.visible, v.holdFire, v.armed);
        h.all(v.speedPx, v.speedPxMax, v.sightTiles, v.ammo2, v.hasPrimary, v.hasSecondary);
        h.all(v.range, v.deadzone, v.cooldown, v.damage, v.hitType, v.sound1);
        h.all(v.range2, v.cooldown2, v.burstShots2, v.burstReload2, v.burstCount2, v.damage2, v.hitType2, v.sound2);
        h.all(v.armor, v.hasCatalogArmor, v.passengerCapacity, v.passengers, v.spottedMask);
        h.all(v.moveTarget, v.waypoints, v.attackTarget, v.fireCooldown, v.chasingAttackTarget, v.wasMoving);
        h.all(v.maxAmmo, v.maxAmmo2, v.moveDamageHp, v.isSupply, v.isAmbulance, v.isArtillery);
        h.all(v.attackMove, v.attackMoveEngaging, v.queuedOrders, v.attackGround);
        h.all(v.towWeightClass, v.towSpeedByClass, v.towedVehicle, v.towedBy, v.pendingTowGun,
              v.attackBuildingTarget);
        h.all(v.canMoveBackward, v.backSpeedPx, v.reverseMove, v.expa, v.kills, v.lastReactTick);
        h.all(v.isAntiAir, v.aaCooldown, v.hasCrew, v.burstShots1, v.burstReload1, v.burstCount1);
        h.all(v.isRocketArtillery, v.isCasemate, v.isKamikaze, v.turnStep32, v.canCrushInfantry,
              v.weapon1Flamer, v.weapon2Flamer);
        h.all(v.patrolOrigin, v.patrolDest, v.patrolToDest, v.facingTargetDir);
        h.all(v.supportTargetVehicle, v.supportTargetBuilding, v.supportTargetSoldier, v.escortVehicleTarget,
              v.escortSoldierTarget);
        h.all(v.muzzleRevealTicks, v.bonusSight, v.marchSightBonus, v.marchEnabled, v.hatchOpen,
              v.lastCombatTick);
        h.all(v.crewProtection, v.hasCrewProtection, v.crewHp, v.maxCrewHp, v.crewUnit, v.canSelfRepair);
        h.all(v.crewNumber, v.attackCrew, v.moveCrew, v.cookOffDamage, v.cookOffRadius);
        h.all(v.forbiddenTerrainMask, v.isNaval, v.isCrate, v.hasFiniteSupply, v.targetType);
        h.all(v.attackPref, v.hasAttackPref, v.isWheeled, v.isTracked, v.isTrain, v.turretStep32);
        h.all(v.hasGunTurnDelay, v.canBeCrushed, v.passengersCanFire, v.accuracy1, v.accuracy1Min);
        h.all(v.shotExpa1, v.shotExpa2, v.bonusShotRange, v.isPropaganda, v.soldiersOnArmor);
        h.all(v.resupplyCost1, v.resupplyCost2, v.repairCost, v.gunShotWait, v.gunSettleTicks, v.unitRadius);
        h.all(v.ammoPerShot1, v.ammoPerShot2, v.crewVulnerability, v.expaOnHit, v.moraleResist);
        h.all(v.suppressionTicks, v.wasSuppressed, v.crewCanBeHealed, v.indirectArc1, v.craterScale1);
        h.all(v.isHeavyAntiAir, v.bonusShotDeadZone, v.scanDelayTicks, v.lastAutoScanTick, v.crushExpaBonus);
        h.all(v.expaReload, v.expaReloadMax, v.ammoRegenDelay, v.ammoRegenDelayMax, v.healDelay, v.healDelayMax);
        h.all(v.moraleRndMove, v.moraleRage, v.lastPanicTick);
    });

    // Excluded: vehiclePoses_ is a presentation buffer drained by takeVehiclePoses(); the sim only merges into it.
    h.add(randomState_);
    h.add(fog_);
    h.all(difficulty_, autoEngage_);
    // Excluded: catalog_ is immutable shared configuration (shared_ptr<const>), fixed at construction.
    h.add(fogDirty_);
    // Excluded: fogSnapshotDirty_ only gates takeFogSnapshot() output; the sim never reads it.
    h.all(combatRandomState_, tick_);

    h.each(projectiles_, [&h](const Projectile& p) {
        h.all(p.attacker, p.target, p.rawDamage, p.hitPct, p.ticksRemaining, p.isFlamer, p.isMortar, p.isHeavyMg);
    });

    h.all(playerTeams_, lastHitTick_, lastGroupHitTick_, mines_);
    // Excluded: mineEvents_ is a presentation buffer drained by takeMineEvents().

    h.each(buildings_, [&h](const Building& b) {
        h.all(b.x, b.y, b.left, b.top, b.width, b.height, b.hp, b.maxHp, b.capacity, b.owner, b.destroyed);
        h.all(b.entryCell, b.occupants);
    });
    // Excluded: buildingUpdates_ is a presentation buffer drained by takeBuildingUpdates().

    h.all(airSupport_, paraGroups_, paraRosters_);
    h.each(sorties_, [&h](const ActiveSortie& s) {
        h.all(s.id, s.kind, s.planeId, s.owner, s.fx, s.fy, s.vx, s.vy, s.targetX, s.targetY, s.dir32);
        h.all(s.hp, s.maxHp, s.payloadsLeft, s.totalPayloads, s.bombDamage, s.strafeDamage, s.airToAirDamage);
        h.all(s.sightTiles, s.scoreValue, s.isLeafletBomber, s.altitude, s.isRocketAttacker, s.strafeHitType);
        h.add(s.reconTick);
    });
    // Excluded: sortiePoses_, parachutePoses_, and airEvents_ are presentation buffers drained by
    // takeAirSorties(), takeParachutes(), and takeAirStrikeEvents().
    h.all(nextSortieId_, reserveUnits_, cookOffDepth_, weather_);
    if (!parachutes_.empty()) {
        h.each(parachutes_, [&h](const ActiveParachute& p) {
            h.all(p.id, p.unitId, p.owner, p.group, p.x, p.y, p.altitude256, p.fallSpeed256,
                  p.collapseTicks, p.hp, p.maxHp, p.isSupplyCrate, p.emittedBoxDust);
        });
    }
    // Excluded: soldierIds_ holds immutable spawn-time unit-type IDs (like catalog_).
    // Only hash evacuating orders, bridge repairs, or damaged bridges when active so unaffected scenarios keep identical hashes.
    if (!evacuatingSoldiers_.empty() || !evacuatingVehicles_.empty()) {
        h.each(evacuatingSoldiers_, [&h](const EvacuationOrder& e) { h.all(e.index, e.targetX, e.targetY); });
        h.each(evacuatingVehicles_, [&h](const EvacuationOrder& e) { h.all(e.index, e.targetX, e.targetY); });
    }
    if (std::ranges::any_of(vehicles_, [](const Vehicle& v) { return v.supportTargetBridge.has_value(); })) {
        for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
            if (vehicles_[vi].supportTargetBridge) h.all(vi, *vehicles_[vi].supportTargetBridge);
        }
    }
    if (std::ranges::any_of(bridges_, [](const Bridge& b) {
            return b.hp != b.maxHp || b.stage != 0 || b.destroyed;
        })) {
        h.each(bridges_, [&h](const Bridge& b) {
            h.all(b.x, b.y, b.left, b.top, b.width, b.height, b.hp, b.maxHp, b.stage, b.destroyed);
        });
    }
    // Excluded: viewerOwner_ is presentation-only (fog/pose visibility perspective) and never affects
    // simulation state; commandActor_ is transient RAII context during applyAs() and always 0 at rest.
    // Keep SP stateHash() identical when multiplayerSymmetry_ is false.
    if (multiplayerSymmetry_) h.add(multiplayerSymmetry_);
    if (std::ranges::any_of(soldiers_, [](const Soldier& s) {
            return s.primaryHitType != assets::HitType::Machine || s.hasCatalogAtShot;
        })) {
        for (size_t si = 0; si < soldiers_.size(); ++si) {
            const auto& s = soldiers_[si];
            if (s.primaryHitType != assets::HitType::Machine || s.hasCatalogAtShot) {
                h.all(si, s.primaryHitType, s.hasCatalogAtShot);
            }
        }
    }
    if (std::ranges::any_of(vehicles_, [](const Vehicle& v) {
            return !v.embarkedCrew.empty() || v.unlimberTicks > 0;
        })) {
        for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
            const auto& v = vehicles_[vi];
            if (!v.embarkedCrew.empty() || v.unlimberTicks > 0) {
                h.all(vi, v.unlimberTicks);
                h.each(v.embarkedCrew, [&h](const CrewMember& cm) {
                    h.all(cm.id, cm.hp, cm.ammo, cm.expa);
                });
            }
        }
    }
    if (std::ranges::any_of(buildings_, [](const Building& b) { return b.sightBonus != 3; })) {
        for (size_t bi = 0; bi < buildings_.size(); ++bi) {
            if (buildings_[bi].sightBonus != 3) {
                h.all(bi, buildings_[bi].sightBonus);
            }
        }
    }
    for (size_t si = 0; si < soldiers_.size(); ++si) {
        const auto& s = soldiers_[si];
        if (!s.pendingActions.empty()) {
            h.all(si, s.pendingActions.size());
            for (const auto& qa : s.pendingActions) {
                h.all(static_cast<std::uint8_t>(qa.op), qa.x, qa.y, qa.targetIndex);
            }
        }
        if (s.canCrouchExpa) {
            h.all(si, s.canCrouchExpa, s.lastEmittedCrouch);
        }
    }
    for (size_t vi = 0; vi < vehicles_.size(); ++vi) {
        const auto& v = vehicles_[vi];
        if (!v.pendingActions.empty()) {
            h.all(vi, v.pendingActions.size());
            for (const auto& qv : v.pendingActions) {
                h.all(static_cast<std::uint8_t>(qv.op), qv.x, qv.y, qv.dir32);
            }
        }
    }
    for (size_t ai = 0; ai < sorties_.size(); ++ai) {
        const auto& st = sorties_[ai];
        if (st.loiterTicksLeft != 0 || st.bombScatterPx != 0) {
            h.all(ai, st.loiterTicksLeft, st.bombScatterPx);
        }
    }
    if (!craterTiles_.empty()) {
        h.all(craterTiles_);
    }
    return h.value();
}

} // namespace ss2
