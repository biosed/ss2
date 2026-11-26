#include "session/game_session.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace ss2 {

void GameSession::processLockstepPostTickSpawns() {
    if (!simulation) return;
    for (const auto& ev : simulation->poll()) {
        if (ev.kind == ss2::MissionEvent::Reinforcement) {
            spawnReinforcementWave(ev);
        } else {
            deferredLockstepEvents.push_back(ev);
        }
    }
    const auto crewSpawns = simulation->pollCrewSpawns();
    if (!crewSpawns.empty()) {
        spawnAbandonedCrew(crewSpawns);
    }
}

void GameSession::stepSimulationEvents() {
    if (simulation) {
        if (replayFile && liveReplay) {
            const auto now = std::chrono::steady_clock::now();
            if (!manualClock) {
                const double dt = (lastReplayStepFrame.time_since_epoch().count() == 0)
                    ? 0.0
                    : std::clamp(std::chrono::duration<double>(now - lastReplayStepFrame).count(), 0.0, 0.1);
                lastReplayStepFrame = now;
                const double baseSec = std::max(0.005, static_cast<double>(ss2::tutorialTurnDelay(gameDir)) / 1000.0);
                const unsigned steps = std::min(16u, replayTransport.consumeTicks(dt, baseSec));
                for (unsigned s = 0; s < steps && !replayTransport.finished; ++s) {
                    if (!simulation->replayStep(replayFile->commands, replayTransport.cursor, replayFile->endTick)) {
                        replayTransport.finished = true;
                        showNotice(ss2::tr("Replay playback stopped early."), ss2::NoticeKind::blocked);
                        break;
                    }
                }
            }
            if (!replayTransport.finished && simulation->simTick() >= replayFile->endTick &&
                replayTransport.cursor >= replayFile->commands.size()) {
                replayTransport.finished = true;
                const auto actualHash = simulation->stateHash();
                if (replayFile->endStateHash) {
                    if (actualHash == *replayFile->endStateHash) {
                        replayTransport.hashVerified = true;
                        std::cout << "Live replay verified: " << replayFile->commands.size()
                                  << " commands, tick " << simulation->simTick()
                                  << ", stateHash 0x" << std::hex << actualHash << std::dec << '\n';
                        showNotice(ss2::tr("Replay complete — stateHash verified!"), ss2::NoticeKind::accepted);
                    } else {
                        replayTransport.hashMismatch = true;
                        showNotice(ss2::tr("Replay complete — stateHash mismatch!"), ss2::NoticeKind::blocked);
                    }
                } else {
                    showNotice(ss2::tr("Replay complete."), ss2::NoticeKind::accepted);
                }
            }
            paused = replayTransport.paused || simulation->isPaused();
        }
        if (multiplayer.enabled && lockstep) {
            const auto markSeatDisconnected = [&](std::uint8_t seat) {
                if (seat >= 4u || mpSeatDisconnected[seat]) return;
                mpSeatDisconnected[seat] = true;
                const std::string who = !multiplayer.playerNames[seat].empty()
                    ? multiplayer.playerNames[seat]
                    : ("Seat " + std::to_string(seat + 1));
                const std::string msg = ss2::trf("{0} disconnected.", {who});
                logHud(msg);
                showNotice(msg, ss2::NoticeKind::blocked);
            };
            if (mpTransport) {
                const auto nowMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                mpTransport->sendPingsIfDue(nowMs, mpPingSeq);
                auto pollRes = mpTransport->poll(nowMs);
                if (pollRes.maxRttMs >= 0) mpRttMs = pollRes.maxRttMs;
                for (auto& pkt : pollRes.localPackets) {
                    if (auto* bundle = std::get_if<ss2::TurnBundlePacket>(&pkt)) {
                        (void)lockstep->receiveTurnBundle(std::move(*bundle));
                    } else if (const auto* desyncPkt = std::get_if<ss2::DesyncPacket>(&pkt)) {
                        if (!lockstepDesyncReported) {
                            lockstepDesyncReported = true;
                            std::cerr << "Peer reported lockstep desync at tick " << desyncPkt->tick
                                      << " (remote=0x" << std::hex << desyncPkt->localHash
                                      << " expected=0x" << desyncPkt->remoteHash << std::dec << ")\n";
                            showNotice(ss2::tr("Lockstep desync detected!"), ss2::NoticeKind::blocked);
                        }
                    } else if (const auto* bye = std::get_if<ss2::ByePacket>(&pkt)) {
                        markSeatDisconnected(bye->seat);
                    } else if (const auto* chat = std::get_if<ss2::ChatPacket>(&pkt)) {
                        const std::string sender = !chat->sender.empty()
                            ? chat->sender
                            : (chat->seat < 4u && !multiplayer.playerNames[chat->seat].empty()
                                ? multiplayer.playerNames[chat->seat]
                                : ("Seat " + std::to_string(chat->seat + 1)));
                        const std::string line = (chat->teamOnly ? ("[" + ss2::tr("Allies") + "] ") : std::string{}) +
                                                 sender + ": " + chat->text;
                        logHud(line);
                        showNotice(ss2::trf(chat->teamOnly ? "Team Chat: {0}" : "Chat: {0}", {line}),
                                   ss2::NoticeKind::accepted);
                        if (audio) audio->playGroup("e_telegram", 0.55f);
                    } else if (const auto* beacon = std::get_if<ss2::BeaconPacket>(&pkt)) {
                        const std::string who = (beacon->seat < 4u && !multiplayer.playerNames[beacon->seat].empty())
                            ? multiplayer.playerNames[beacon->seat]
                            : ("Seat " + std::to_string(beacon->seat + 1));
                        if (activeBeacons.size() >= 6u) activeBeacons.erase(activeBeacons.begin());
                        activeBeacons.push_back({beacon->x, beacon->y, beacon->seat, std::chrono::steady_clock::now()});
                        pushMinimapPing(beacon->x, beacon->y, false);
                        const std::string bMsg = ss2::trf("Beacon from {0}!", {who});
                        logHud(bMsg);
                        showNotice(bMsg, ss2::NoticeKind::accepted);
                        if (audio) audio->playGroup("e_telegram", 0.65f);
                    }
                }
                for (const std::uint8_t dseat : pollRes.disconnectedSeats) {
                    markSeatDisconnected(dseat);
                }
                for (const auto& outBundle : lockstep->produceOutboundBundles()) {
                    (void)mpTransport->sendTurnBundle(outBundle);
                }
                mpTransport->flushAll();
            }

            const auto now = std::chrono::steady_clock::now();
            constexpr auto kMpTickStep = std::chrono::milliseconds(40);
            if (now - nextLockstepTickAt > std::chrono::milliseconds(200)) {
                nextLockstepTickAt = now - kMpTickStep;
            }
            for (int catchUp = 0; catchUp < 2 && now >= nextLockstepTickAt; ++catchUp) {
                const auto status = lockstep->stepTick(*simulation, [this]() {
                    processLockstepPostTickSpawns();
                });
                if (status == ss2::LockstepCoordinator::StepStatus::Stepped) {
                    nextLockstepTickAt += kMpTickStep;
                    if (lockstepWaitingPeer) {
                        lockstepWaitingPeer = false;
                        orderNotice.show(ss2::tr("Resumed."), ss2::NoticeKind::accepted);
                    }
                    if (mpTransport) {
                        for (const auto& outBundle : lockstep->produceOutboundBundles()) {
                            (void)mpTransport->sendTurnBundle(outBundle);
                        }
                        mpTransport->flushAll();
                    }
                } else if (status == ss2::LockstepCoordinator::StepStatus::WaitingForPeer) {
                    if (!lockstepWaitingPeer) {
                        lockstepWaitingPeer = true;
                        showNotice(ss2::tr("Waiting for peer..."), ss2::NoticeKind::unsupported);
                    }
                    break;
                } else {
                    if (!lockstepDesyncReported) {
                        lockstepDesyncReported = true;
                        if (const auto& di = lockstep->desync()) {
                            std::cerr << "Lockstep desync at tick " << di->tick
                                      << " (local=0x" << std::hex << di->localHash
                                      << " remote=0x" << di->remoteHash << std::dec << ")\n";
                            if (mpTransport && mpTransport->isActive()) {
                                (void)mpTransport->sendDesync(
                                    ss2::DesyncPacket{di->tick, di->localHash, di->remoteHash});
                                mpTransport->flushAll();
                            }
                        }
                        showNotice(ss2::tr("Lockstep desync detected!"), ss2::NoticeKind::blocked);
                    }
                    break;
                }
            }
            paused = simulation->isPaused();
            if (multiplayer.enabled) {
                const int myTeam = (localOwner() >= 0 && localOwner() < 4) ? missionPlayerTeams[localOwner()] : 0;
                for (const auto& z : simulation->zeppelinStatuses()) {
                    const std::size_t zi = z.index;
                    if (zi >= lastZeppelinHolder.size()) continue;
                    if (lastZeppelinHolder[zi] == -2) {
                        lastZeppelinHolder[zi] = z.holderTeam;
                        continue;
                    }
                    if (z.holderTeam != lastZeppelinHolder[zi]) {
                        lastZeppelinHolder[zi] = z.holderTeam;
                        if (z.holderTeam >= 0) {
                            const bool mine = (z.holderTeam == myTeam);
                            pushMinimapPing(z.x, z.y, !mine);
                            if (audio) audio->playGroup(mine ? "accept" : "e_telegram", 0.65f);
                            showNotice(ss2::tr(mine ? "Zeppelin captured!" : "Enemy captured a Zeppelin!"),
                                       mine ? ss2::NoticeKind::accepted : ss2::NoticeKind::blocked);
                        }
                    }
                }
            }
        }
        bool markersDirty = false;
        bool spritesDirty = false;
        {
            const unsigned curSimTick = simulation->simTick();
            if (curSimTick < lastCombatVfxSimTick ||
                (activeVfx.empty() && burningWrecks.empty() && curSimTick > lastCombatVfxSimTick + 250u)) {
                combatVfx.clearActive();
                spritesDirty = true;
            }
            lastCombatVfxSimTick = curSimTick;
        }
        if (auto fogSnap = simulation->pollFog()) {
            latestFog = *fogSnap;
            const auto fogPatch = replayAllVision()
                ? scene->updateFog(std::vector<std::uint8_t>(fogSnap->size(), 0xFFu))
                : scene->updateFog(*fogSnap);
            if (fogPatch.pixels.width && fogPatch.pixels.height)
                renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
            updateMinimapFog(*fogSnap, false);
            spritesDirty = true;
        }
        const auto airEvents = simulation->pollAirStrikeEvents();
        for (const auto& ae : airEvents) {
            if (ae.kind == ss2::AirStrikeEvent::ParatrooperLanded ||
                ae.kind == ss2::AirStrikeEvent::SupplyCrateLanded) {
                if (ae.landedIsVehicle) {
                    registerSimSpawnedVehicle(ae.landedUnitIndex, ae.planeId, ae.x, ae.y, ae.owner, ae.group);
                } else {
                    registerSimSpawnedSoldier(ae.landedUnitIndex, ae.planeId, ae.x, ae.y, ae.owner, ae.group);
                }
                spritesDirty = true;
            }
        }
        for (const auto& pose : simulation->pollPoses()) {
            if (pose.index >= scene->units().size()) {
                std::string wantId = simulation->soldierUnitId(pose.index);
                if (wantId.empty() && replayFile && liveReplay) {
                    std::size_t seenSpawns = 0;
                    const std::size_t baseCount = scene->units().size();
                    for (std::size_t ci = 0; ci < replayTransport.cursor && ci < replayFile->commands.size(); ++ci) {
                        if (const auto* sp = std::get_if<cmd::SpawnInfantry>(&replayFile->commands[ci].command)) {
                            if (baseCount + seenSpawns == pose.index) { wantId = sp->unit.id; break; }
                            ++seenSpawns;
                        }
                    }
                }
                if (wantId.empty()) wantId = "dguner";
                registerSimSpawnedSoldier(pose.index, wantId, pose.x, pose.y, pose.owner, pose.group);
            }
            if (pose.index >= scene->units().size()) continue;
            spritesDirty = true;
            const int prevHp = scene->units().at(pose.index).hp;
            const int prevInfAmmo = scene->units().at(pose.index).ammo;
            if (pose.index >= unitExpa.size()) unitExpa.resize(pose.index + 1, 0);
            if (pose.index >= unitKills.size()) unitKills.resize(pose.index + 1, 0);
            if (pose.index >= unitGrenades.size()) unitGrenades.resize(pose.index + 1, 0);
            if (pose.index >= unitMineAmmo.size()) unitMineAmmo.resize(pose.index + 1, 0);
            unitExpa[pose.index] = pose.expa;
            unitKills[pose.index] = pose.kills;
            unitGrenades[pose.index] = pose.atGrenades;
            unitMineAmmo[pose.index] = pose.mineAmmo;
            if (pose.camouflaged) camouflagedUnits.insert(pose.index);
            else camouflagedUnits.erase(pose.index);
            if (pose.alive && pose.suppressed) suppressedUnits.insert(pose.index);
            else suppressedUnits.erase(pose.index);
            if (pose.crouchedState == 1) {
                if (crouchedUnits.insert(pose.index).second) spritesDirty = true;
            } else if (pose.crouchedState == 0) {
                if (crouchedUnits.erase(pose.index) > 0) spritesDirty = true;
            }
            const auto patch = scene->update(pose.index, pose.x, pose.y, pose.direction, pose.progress,
                                             pose.walking, pose.hp, pose.visible ? 1 : 0, pose.ammo);
            if (patch.pixels.width && patch.pixels.height)
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            selection->update(pose.index, scaleInfantry(scene->units().at(pose.index), mapScale));
            const auto now = std::chrono::steady_clock::now();
            if (prevHp > 0 && (!pose.alive || pose.hp <= 0) && audio && (pose.visible || pose.owner == localOwner())) {
                auto& lastDeathSnd = lastWeaponAudio["screamofpain"];
                if (now - lastDeathSnd >= std::chrono::milliseconds(450)) {
                    if (playWorldSound("screamofpain", 0.42f, pose.x, pose.y, ss2::VoicePriority::Low)) {
                        lastDeathSnd = now;
                    }
                }
            }
            if (pose.alive && pose.hp > prevHp && combatEffects && pose.visible && activeVfx.size() < 64) {
                const float hx = static_cast<float>(pose.x * mapScale);
                const float hy = static_cast<float>(pose.y * mapScale) - 10.0f * static_cast<float>(mapScale);
                activeVfx.push_back({ActiveVfx::Kind::HealPulse, hx, hy, hx, hy, now, std::chrono::milliseconds(550)});
            } else if (pose.alive && pose.hp < prevHp && pose.owner == localOwner()) {
                const auto& uid = scene->units().at(pose.index).id;
                const auto* uSpec = unitCatalog ? unitCatalog->find(uid) : nullptr;
                const int maxInfHp = (uSpec && uSpec->health > 0) ? uSpec->health : 100;
                if (prevHp > maxInfHp / 3 && pose.hp <= maxInfHp / 3) {
                    playUnitVoiceForId("close", uid, false);
                } else {
                    playUnitVoiceForId("underattack", uid, false);
                }
            }
            if (pose.alive && pose.owner == localOwner() && prevInfAmmo > 0 && pose.ammo == 0) {
                playUnitVoiceForId("noammo", scene->units().at(pose.index).id, false);
            }
            if (pose.fired) {
                recoilUntil[pose.index] = now + std::chrono::milliseconds(120);
                {
                    const auto& uid = scene->units().at(pose.index).id;
                    const auto* uSpec = unitCatalog ? unitCatalog->find(uid) : nullptr;
                    if (uSpec && uSpec->shot1.reloadTicks > 0 && simulation) {
                        unitReloadWindow[pose.index] = {
                            simulation->simTick(), static_cast<std::uint32_t>(uSpec->shot1.reloadTicks)
                        };
                    }
                }
                if (audio) {
                    const auto& uid = scene->units().at(pose.index).id;
                    const auto [rawGroup, cooldownMs, gain] = weaponSoundForUnit(uid);
                    const std::string sndGroup = pose.firedFlamer ? std::string("fmw") : rawGroup;
                    auto& lastAt = lastWeaponAudio[sndGroup];
                    if (now - lastAt >= std::chrono::milliseconds(cooldownMs)) {
                        bool played = playWorldSound(sndGroup, gain, pose.x, pose.y);
                        if (!played && sndGroup == "fmw") {
                            played = playWorldSound("fire", gain, pose.x, pose.y);
                        } else if (!played && (sndGroup == "bazooka" || sndGroup == "mortar")) {
                            played = playWorldSound("gunshot", gain, pose.x, pose.y);
                        }
                        if (played) lastAt = now;
                    }
                    if (!pose.threwGrenade && !pose.firedFlamer) {
                        const auto* uSpec = unitCatalog ? unitCatalog->find(uid) : nullptr;
                        if (uSpec && !uSpec->shot1.endBurstSound.empty()) {
                            auto& lastEb = lastWeaponAudio["eb:" + uSpec->shot1.endBurstSound];
                            if (now - lastEb >= std::chrono::milliseconds(750)) {
                                if (playWorldSound(uSpec->shot1.endBurstSound, 0.22f, pose.x, pose.y,
                                                   ss2::VoicePriority::Low)) {
                                    lastEb = now;
                                }
                            }
                        }
                    }
                    if (pose.threwGrenade || pose.firedExplosive) {
                        auto& lastExp = lastWeaponAudio["explo_tankdir"];
                        if (now - lastExp >= std::chrono::milliseconds(250)) {
                            if (playWorldSound("explo_tankdir", 0.52f, pose.targetX, pose.targetY)) {
                                lastExp = now;
                            }
                        }
                    }
                }
                const bool shotVisible = pose.visible ||
                    (pose.garrisoned && (isAllyOwner(pose.owner) || isBuildingPointVisible(pose.x, pose.y)));
                if (combatEffects && shotVisible) {
                    const float sx = static_cast<float>(pose.x * mapScale);
                    const float sy = static_cast<float>(pose.y * mapScale) - 18.0f * static_cast<float>(mapScale);
                    const float tx = static_cast<float>(pose.targetX * mapScale);
                    const float ty = static_cast<float>(pose.targetY * mapScale) - 14.0f * static_cast<float>(mapScale);
                    const auto& uid = scene->units().at(pose.index).id;
                    const auto* uSpec = unitCatalog ? unitCatalog->find(uid) : nullptr;
                    const bool flamer = pose.firedFlamer || (uid.find("flam") != std::string::npos);
                    if (activeVfx.size() < 64) {
                        activeVfx.push_back({
                            flamer ? ActiveVfx::Kind::Flamer : ActiveVfx::Kind::Tracer,
                            sx, sy, tx, ty, now,
                            std::chrono::milliseconds(flamer ? 220 : 95)
                        });
                    }
                    if ((pose.threwGrenade || pose.firedExplosive) && activeVfx.size() < 64) {
                        activeVfx.push_back({
                            ActiveVfx::Kind::Explosion, tx, ty, tx, ty, now, std::chrono::milliseconds(360)
                        });
                    }
                    const unsigned shotDir32 = CombatVfxSystem::dir32FromMapDelta(
                        static_cast<double>(pose.targetX - pose.x),
                        static_cast<double>(pose.targetY - pose.y));
                    if (flamer) {
                        combatVfx.spawnFlameJet(static_cast<float>(pose.x), static_cast<float>(pose.y - 10),
                                                static_cast<float>(pose.targetX), static_cast<float>(pose.targetY - 6));
                    } else if (uSpec && uSpec->isMortarInfantry()) {
                        combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleMortar,
                                                   static_cast<float>(pose.x), static_cast<float>(pose.y - 10),
                                                   shotDir32, 4.0f);
                    } else if (!pose.threwGrenade) {
                        combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleRifle,
                                                   static_cast<float>(pose.x), static_cast<float>(pose.y - 12),
                                                   shotDir32, 6.0f);
                    }
                    if (isBuildingPointVisible(pose.targetX, pose.targetY)) {
                        const bool waterHit = isWaterPoint(pose.targetX, pose.targetY);
                        if (pose.threwGrenade || pose.firedExplosive) {
                            combatVfx.spawnExplosion(static_cast<float>(pose.targetX),
                                                     static_cast<float>(pose.targetY),
                                                     pose.firedExplosive ? 1 : 0, waterHit);
                        } else if (!flamer && waterHit) {
                            combatVfx.spawnWaterSplash(static_cast<float>(pose.targetX),
                                                       static_cast<float>(pose.targetY), 1);
                        } else if (!flamer && ((pose.index + simulation->simTick()) & 3u) == 0u) {
                            combatVfx.spawnDust(static_cast<float>(pose.targetX),
                                                static_cast<float>(pose.targetY), false);
                        }
                    }
                    spritesDirty = true;
                }
            }
            if (!pose.alive && prevHp > 0) {
                const auto& deadUnit = scene->units().at(pose.index);
                fallenSoldiers.push_back({pose.x, pose.y, deadUnit.id, pose.direction});
                if (fallenSoldiers.size() > 256u) {
                    fallenSoldiers.erase(
                        fallenSoldiers.begin(),
                        fallenSoldiers.begin() + static_cast<std::ptrdiff_t>(fallenSoldiers.size() - 256u));
                }
                if (isHostileOwner(pose.owner)) {
                    ++enemyInfKilled;
                    enemyScoreEarned += scoreValueForId(deadUnit.id, false);
                } else if (pose.owner == localOwner()) {
                    ++friendlyInfLost;
                    friendlyScoreLost += scoreValueForId(deadUnit.id, false);
                }
                if (pose.owner == localOwner()) {
                    recordEventPos(static_cast<double>(pose.x * mapScale), static_cast<double>(pose.y * mapScale));
                    pushMinimapPing(pose.x, pose.y, true);
                }
                const auto before = activeMarkers.size();
                std::erase_if(activeMarkers, [&](const TimedMarker& m) {
                    return m.targetUid && *m.targetUid == pose.index;
                });
                if (activeMarkers.size() != before) markersDirty = true;
                (void)playWorldSound("fall_small", 0.65f, pose.x, pose.y);
                std::cout << "Tutorial I unit " << pose.index
                          << " (owner=" << pose.owner << " group=" << pose.group << ") eliminated\n";
            }
        }
        if (vehicleScene) {
            for (const auto& vp : simulation->pollVehiclePoses()) {
                if (vp.index >= vehicleScene->vehicles().size() && replayFile && liveReplay) {
                    std::string wantVeh = "tank";
                    std::size_t seenSpawns = 0;
                    const std::size_t baseCount = vehicleScene->vehicles().size();
                    for (std::size_t ci = 0; ci < replayTransport.cursor && ci < replayFile->commands.size(); ++ci) {
                        if (const auto* sp = std::get_if<cmd::SpawnVehicle>(&replayFile->commands[ci].command)) {
                            if (baseCount + seenSpawns == vp.index) { wantVeh = sp->unit.id; break; }
                            ++seenSpawns;
                        }
                    }
                    registerSimSpawnedVehicle(vp.index, wantVeh, vp.x, vp.y, vp.owner, vp.group, vp.hullDir);
                }
                if (vp.index >= vehicleScene->vehicles().size()) continue;
                const int prevVehHp = vehicleScene->vehicles().at(vp.index).hp;
                const bool prevVehVisible = vehicleScene->vehicles().at(vp.index).visible;
                const int prevVehOwner = vehicleScene->vehicles().at(vp.index).owner;
                const int prevVehAmmo = vehicleScene->vehicles().at(vp.index).ammo;
                const int prevVehAmmo2 = (vp.index < vehicleAmmo2.size()) ? vehicleAmmo2[vp.index] : 0;
                if (vp.maxHp > 0 && vp.index < initialVehicleHp.size()) {
                    initialVehicleHp[vp.index] = vp.maxHp;
                }
                if (vp.index < vehicleCargo.size()) {
                    vehicleCargo[vp.index] = {vp.passengerCount, vp.passengerCapacity};
                }
                if (vp.index < vehicleTowed.size()) {
                    vehicleTowed[vp.index] = vp.towedIndex;
                }
                if (vp.index < vehicleTowedBy.size()) {
                    vehicleTowedBy[vp.index] = vp.towedByIndex;
                }
                if (vp.index >= vehicleAmmo2.size()) vehicleAmmo2.resize(vp.index + 1, 0);
                vehicleAmmo2[vp.index] = vp.ammo2;
                if (vp.index >= vehExpa.size()) vehExpa.resize(vp.index + 1, 0);
                if (vp.index >= vehKills.size()) vehKills.resize(vp.index + 1, 0);
                if (vp.index >= vehicleCrewImpaired.size()) vehicleCrewImpaired.resize(vp.index + 1, false);
                if (vp.index >= vehicleMoveImpaired.size()) vehicleMoveImpaired.resize(vp.index + 1, false);
                if (vp.index >= vehicleSuppressed.size()) vehicleSuppressed.resize(vp.index + 1, false);
                if (vp.index >= vehicleHatchOpen.size()) vehicleHatchOpen.resize(vp.index + 1, false);
                vehExpa[vp.index] = vp.expa;
                vehKills[vp.index] = vp.kills;
                vehicleCrewImpaired[vp.index] = vp.crewImpaired;
                vehicleMoveImpaired[vp.index] = vp.moveImpaired;
                vehicleSuppressed[vp.index] = vp.suppressed;
                vehicleHatchOpen[vp.index] = vp.hatchOpen;
                const auto* smSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                if (vp.alive && vp.moving && vp.visible) {
                    if (smSpec && smSpec->hasMoveSmoke) movingSmokeVehicles.insert(vp.index);
                    else movingSmokeVehicles.erase(vp.index);
                    if (isWaterPoint(vp.x, vp.y) || isShallowsPoint(vp.x, vp.y)) {
                        movingWaterVehicles.insert(vp.index);
                    } else {
                        movingWaterVehicles.erase(vp.index);
                    }
                } else {
                    movingSmokeVehicles.erase(vp.index);
                    movingWaterVehicles.erase(vp.index);
                }
                if (vp.alive && vp.hp > 0 && vp.moving && smSpec && smSpec->isTracked()) {
                    if (crushOrBlastTreesAt(vp.x, vp.y, 16, combatEffects && vp.visible)) {
                        spritesDirty = true;
                        const auto treeNow = std::chrono::steady_clock::now();
                        if (vp.visible && audio && treeNow - lastWeaponAudio["tree_crush"] >= std::chrono::milliseconds(350)) {
                            if (playWorldSound("fall_small", 0.38f, vp.x, vp.y, ss2::VoicePriority::Low)) {
                                lastWeaponAudio["tree_crush"] = treeNow;
                            }
                        }
                    }
                }
                if (combatEffects && (!smSpec || (!smSpec->isTrain() && !smSpec->isSupplyCrate()))) {
                    const std::size_t prevDecals = combatVfx.activeDecalCount();
                    combatVfx.recordVehicleMotion(
                        vp.index, vp.x, vp.y,
                        vp.alive && vp.hp > 0, vp.moving, vp.visible,
                        smSpec && smSpec->isNavalCraft(),
                        !smSpec || smSpec->isTracked(),
                        isWaterPoint(vp.x, vp.y),
                        isShallowsPoint(vp.x, vp.y));
                    if (combatVfx.activeDecalCount() != prevDecals) spritesDirty = true;
                }
                const bool effVehVis = vp.visible || (replayAllVision() && vp.alive && vp.hp > 0);
                const std::string& vehId = vehicleScene->vehicles().at(vp.index).id;
                const bool isGpuVeh = gpuVehicleIds.contains(vehId);
                const auto dirty = vehicleScene->update(vp.index, vp.x, vp.y, vp.hullDir, vp.turretDir,
                                                        vp.hp, effVehVis ? 1 : 0, vp.ammo);
                if (isGpuVeh) {
                    spritesDirty = true;
                } else if (mapScale == 1 && dirty.valid()) {
                    const auto patch = scene->repaintRegion(dirty.left, dirty.top, dirty.right, dirty.bottom);
                    if (patch.pixels.width && patch.pixels.height)
                        renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                }
                selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
                if (vp.captured) {
                    vehicleScene->setOwner(vp.index, vp.owner);
                    selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
                    if (audio) audio->playGroup("e_captured", 0.72f, ss2::VoicePriority::High);
                    playUnitVoiceForId("select", vehicleScene->vehicles().at(vp.index).id, true, true);
                    showNotice(ss2::trf("Captured {0}!", {hudDisplayName(vehicleScene->vehicles().at(vp.index).id)}),
                               ss2::NoticeKind::accepted);
                }
                if (vp.decrewed) {
                    vehicleHoldFireUnits.erase(vp.index);
                    vehicleScene->setOwner(vp.index, vp.owner);
                    selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
                    if (isHostileOwner(prevVehOwner)) {
                        enemyScoreEarned += std::max(50, scoreValueForId(vehicleScene->vehicles().at(vp.index).id, true) / 2);
                    }
                    if (vp.visible || prevVehOwner == localOwner()) {
                        recordEventPos(static_cast<double>(vp.x * mapScale), static_cast<double>(vp.y * mapScale));
                        pushMinimapPing(vp.x, vp.y, prevVehOwner == localOwner());
                        if (prevVehOwner == localOwner()) {
                            showNotice(ss2::trf("Crew lost on {0}!",
                                                {ss2::tr(hudDisplayName(vehicleScene->vehicles().at(vp.index).id))}),
                                       ss2::NoticeKind::unsupported);
                        } else if (isHostileOwner(prevVehOwner)) {
                            showNotice(ss2::trf("Enemy {0} crew neutralized — ready to capture!",
                                                {ss2::tr(hudDisplayName(vehicleScene->vehicles().at(vp.index).id))}),
                                       ss2::NoticeKind::accepted);
                        }
                    }
                }
                if (vp.owner == localOwner() && vp.alive) {
                    if (const auto* sSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr) {
                        if (sSpec->isSupplyTruck() && !sSpec->isSupplyCrate()) {
                            if (sSpec->ammo1 > 0 && prevVehAmmo > 0 && vp.ammo == 0) {
                                showNotice(ss2::tr("Supply truck out of ammo!"), ss2::NoticeKind::unsupported);
                            }
                            if (sSpec->ammo2 > 0 && prevVehAmmo2 > 0 && vp.ammo2 == 0) {
                                showNotice(ss2::tr("Supply truck out of spare parts!"), ss2::NoticeKind::unsupported);
                            }
                        } else if (prevVehAmmo > 0 && vp.ammo == 0) {
                            playUnitVoiceForId("noammo", vehicleScene->vehicles().at(vp.index).id, true);
                        }
                    } else if (prevVehAmmo > 0 && vp.ammo == 0) {
                        playUnitVoiceForId("noammo", vehicleScene->vehicles().at(vp.index).id, true);
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                if (!prevVehVisible && vp.visible && vp.fired && isHostileOwner(vp.owner) &&
                    (vp.heavyImpact || vp.firedRocket)) {
                    recordEventPos(static_cast<double>(vp.x * mapScale), static_cast<double>(vp.y * mapScale));
                    pushMinimapPing(vp.x, vp.y, true);
                    if (now - lastArtilleryAlertAt >= std::chrono::seconds(6)) {
                        lastArtilleryAlertAt = now;
                        showNotice(ss2::tr("Enemy artillery flash spotted!"), ss2::NoticeKind::unsupported);
                    }
                }
                if (vp.moving && vp.visible && audio && now - lastEngineAudioAt >= std::chrono::milliseconds(1800)) {
                    const auto* vSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                    const std::string engSnd = (vSpec && !vSpec->moveSound.empty()) ? vSpec->moveSound : "carb_move";
                    if (playWorldSound(engSnd, 0.24f, vp.x, vp.y, ss2::VoicePriority::Low)) {
                        lastEngineAudioAt = now;
                    }
                }
                if (vp.alive && vp.hp < prevVehHp && vp.owner == localOwner()) {
                    playUnitVoiceForId("underattack", vehicleScene->vehicles().at(vp.index).id, true);
                }
                if ((vp.repaired || vp.resupplied) && vp.visible) {
                    if (combatEffects && activeVfx.size() < 64) {
                        const float rx = static_cast<float>(vp.x * mapScale);
                        const float ry = static_cast<float>(vp.y * mapScale) - 10.0f * static_cast<float>(mapScale);
                        activeVfx.push_back({ActiveVfx::Kind::RepairPulse, rx, ry, rx, ry, now,
                                             std::chrono::milliseconds(550)});
                    }
                    if (audio && now - lastRepairAudioAt >= std::chrono::milliseconds(1500)) {
                        if (playWorldSound("construct", 0.40f, vp.x, vp.y, ss2::VoicePriority::Low)) {
                            lastRepairAudioAt = now;
                        }
                    }
                }
                if (vp.fired) {
                    if (!vp.firedSecondary) {
                        vehRecoilUntil[vp.index] = now + std::chrono::milliseconds(160);
                        spritesDirty = true;
                        if (smSpec && smSpec->shot1.reloadTicks > 0 && simulation) {
                            vehReloadWindow[vp.index] = {
                                simulation->simTick(), static_cast<std::uint32_t>(smSpec->shot1.reloadTicks)
                            };
                        }
                    }
                    if (audio) {
                        const std::string sndGroup = !vp.fireSound.empty()
                            ? vp.fireSound
                            : (vp.firedFlamer ? std::string("fmw")
                                              : (vp.firedRocket ? std::string("rocket")
                                                                : (vp.firedSecondary ? std::string("mg_tank_burst")
                                                                                     : std::string("howitzer"))));
                        const int cdMs = vp.firedFlamer ? 400 : (vp.firedRocket ? 140 : (vp.firedSecondary ? 260 : 320));
                        auto& lastAt = lastWeaponAudio[sndGroup];
                        if (now - lastAt >= std::chrono::milliseconds(cdMs)) {
                            const float gain = vp.firedFlamer ? 0.52f : (vp.firedSecondary ? 0.45f : 0.55f);
                            bool played = playWorldSound(sndGroup, gain, vp.x, vp.y);
                            if (!played && sndGroup == "fmw") {
                                played = playWorldSound("fire", gain, vp.x, vp.y);
                            }
                            if (played) lastAt = now;
                        }
                        if (const auto* vSpec = unitCatalog
                                ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr) {
                            const std::string ebSnd = vp.firedSecondary
                                ? vSpec->shot2.endBurstSound : vSpec->shot1.endBurstSound;
                            if (!ebSnd.empty()) {
                                auto& lastEb = lastWeaponAudio["eb:" + ebSnd];
                                if (now - lastEb >= std::chrono::milliseconds(750)) {
                                    if (playWorldSound(ebSnd, 0.22f, vp.x, vp.y, ss2::VoicePriority::Low)) {
                                        lastEb = now;
                                    }
                                }
                            }
                        }
                        if (vp.heavyImpact) {
                            const auto* vSpec = unitCatalog
                                ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                            const std::string retailImpact = vSpec
                                ? (vp.firedSecondary ? vSpec->shot2.impactSound : vSpec->shot1.impactSound)
                                : std::string{};
                            const std::string expSnd = vp.hitVehicle ? "explo_tankdir"
                                : (!retailImpact.empty() ? retailImpact
                                                         : (sndGroup.find("howitzer") != std::string::npos
                                                                ? "howitzerexplosion" : "defaultexplosion"));
                            auto& lastExp = lastWeaponAudio[expSnd];
                            if (now - lastExp >= std::chrono::milliseconds(300)) {
                                if (playWorldSound(expSnd, 0.56f, vp.targetX, vp.targetY)) {
                                    lastExp = now;
                                }
                            }
                        }
                    }
                    const bool waterImpact = isWaterPoint(vp.targetX, vp.targetY);
                    if (combatEffects && vp.visible) {
                        const float sx = static_cast<float>(vp.x * mapScale);
                        const float sy = static_cast<float>(vp.y * mapScale) - 12.0f * static_cast<float>(mapScale);
                        const float tx = static_cast<float>(vp.targetX * mapScale);
                        const float ty = static_cast<float>(vp.targetY * mapScale) - 10.0f * static_cast<float>(mapScale);
                        if (activeVfx.size() < 64) {
                            activeVfx.push_back({
                                vp.firedFlamer ? ActiveVfx::Kind::Flamer
                                               : (vp.firedRocket ? ActiveVfx::Kind::Rocket
                                                                 : (vp.heavyImpact ? ActiveVfx::Kind::CannonShell
                                                                                   : ActiveVfx::Kind::Tracer)),
                                sx, sy, tx, ty, now,
                                std::chrono::milliseconds(vp.firedFlamer ? 260 : (vp.firedRocket ? 420 : (vp.firedSecondary ? 105 : 155)))
                            });
                        }
                        if (vp.heavyImpact && !vp.firedRocket && !vp.firedFlamer && activeVfx.size() < 64) {
                            activeVfx.push_back({
                                ActiveVfx::Kind::Explosion, tx, ty, tx, ty, now, std::chrono::milliseconds(420)
                            });
                        }
                        if (!vp.firedSecondary && !vp.firedRocket && !vp.firedFlamer &&
                            (vp.heavyImpact || (smSpec && smSpec->shot1.hitType != assets::HitType::Machine)) &&
                            !isWaterPoint(vp.x, vp.y) && activeVfx.size() < 64) {
                            const float mdx = static_cast<float>(vp.targetX - vp.x);
                            const float mdy = static_cast<float>(vp.targetY - vp.y);
                            const float mlen = std::hypot(mdx, mdy);
                            if (mlen > 1.0f) {
                                const float nx = mdx / mlen;
                                const float ny = mdy / mlen;
                                const float fs = static_cast<float>(mapScale);
                                const float gx = static_cast<float>(vp.x) * fs;
                                const float gy = static_cast<float>(vp.y) * fs;
                                activeVfx.push_back({
                                    ActiveVfx::Kind::MuzzleDust,
                                    gx, gy, gx + nx * 24.0f * fs, gy + ny * 24.0f * fs,
                                    now, std::chrono::milliseconds(340)
                                });
                            }
                        }
                        const unsigned shotDir32 = CombatVfxSystem::dir32FromMapDelta(
                            static_cast<double>(vp.targetX - vp.x),
                            static_cast<double>(vp.targetY - vp.y));
                        if (vp.firedFlamer) {
                            combatVfx.spawnFlameJet(static_cast<float>(vp.x), static_cast<float>(vp.y - 8),
                                                    static_cast<float>(vp.targetX), static_cast<float>(vp.targetY - 6));
                        } else if (vp.firedRocket) {
                            combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleKatyusha,
                                                       static_cast<float>(vp.x), static_cast<float>(vp.y - 6),
                                                       vp.hullDir & 31u, 0.0f);
                            combatVfx.spawnRocketPlume(static_cast<float>(vp.x), static_cast<float>(vp.y - 10));
                        } else if (!vp.firedSecondary && smSpec &&
                                   (smSpec->selType == "howitzer" || smSpec->turretType == "gaub" ||
                                    smSpec->turretType == "atg_gaub" || smSpec->shot1.indirectArc)) {
                            combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleHowitzer,
                                                       static_cast<float>(vp.x), static_cast<float>(vp.y - 10),
                                                       shotDir32, 14.0f);
                        } else if (!vp.firedSecondary && smSpec && smSpec->selType == "mortar") {
                            combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleMortar,
                                                       static_cast<float>(vp.x), static_cast<float>(vp.y - 10),
                                                       shotDir32, 6.0f);
                        } else if (vp.firedSecondary) {
                            combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleRifle,
                                                       static_cast<float>(vp.x), static_cast<float>(vp.y - 10),
                                                       shotDir32, 10.0f);
                        } else {
                            combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleCannon,
                                                       static_cast<float>(vp.x), static_cast<float>(vp.y - 10),
                                                       shotDir32, 14.0f);
                        }
                        spritesDirty = true;
                    }
                    if (combatEffects && isBuildingPointVisible(vp.targetX, vp.targetY)) {
                        if (vp.heavyImpact || vp.firedRocket) {
                            const int cScale = vp.craterScale > 0
                                ? int(vp.craterScale)
                                : (smSpec ? smSpec->shot1.craterScale : 1);
                            const int tier = cScale >= 4 ? 3 : (cScale >= 2 ? 2 : 1);
                            combatVfx.spawnExplosion(static_cast<float>(vp.targetX),
                                                     static_cast<float>(vp.targetY),
                                                     tier, waterImpact);
                            spritesDirty = true;
                        } else if (!vp.firedFlamer) {
                            if (waterImpact) {
                                combatVfx.spawnWaterSplash(static_cast<float>(vp.targetX),
                                                           static_cast<float>(vp.targetY), 1);
                            } else {
                                combatVfx.spawnDust(static_cast<float>(vp.targetX),
                                                    static_cast<float>(vp.targetY), false);
                            }
                            spritesDirty = true;
                        }
                    }
                    if (vp.heavyImpact && !waterImpact) {
                        const auto* cSpec = unitCatalog
                            ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                        if (!cSpec || vp.craterScale > 0 || cSpec->shot1.craterScale > 0) {
                            const int cScale = vp.craterScale > 0
                                ? int(vp.craterScale)
                                : (cSpec ? cSpec->shot1.craterScale : 1);
                            stampBattleCrater(vp.targetX, vp.targetY, cScale >= 3);
                        }
                    }
                }
                if (vp.armorHitOutcome >= 2u && vp.visible) {
                    const auto ricNow = std::chrono::steady_clock::now();
                    if (combatEffects && activeVfx.size() < 64) {
                        constexpr float kTwoPi = 6.28318530717958647692f;
                        const float ang = (static_cast<float>((vp.hitImpactDir32 + 4u) & 31u) / 32.0f) * kTwoPi;
                        const float hx = static_cast<float>(vp.x * mapScale);
                        const float hy = static_cast<float>(vp.y * mapScale) - 10.0f * static_cast<float>(mapScale);
                        const float len = (vp.armorHitOutcome == 3u ? 26.0f : 16.0f) * static_cast<float>(mapScale);
                        activeVfx.push_back({
                            ActiveVfx::Kind::Ricochet,
                            hx, hy,
                            hx + std::cos(ang) * len,
                            hy + std::sin(ang) * len * 0.65f - 8.0f * static_cast<float>(mapScale),
                            ricNow,
                            std::chrono::milliseconds(vp.armorHitOutcome == 3u ? 220 : 150)
                        });
                        combatVfx.spawnDust(static_cast<float>(vp.x), static_cast<float>(vp.y), false);
                        spritesDirty = true;
                    }
                    if (audio && ricNow - lastWeaponAudio["ricochet"] >= std::chrono::milliseconds(160)) {
                        const bool played = playWorldSound("ricochet", vp.armorHitOutcome == 3u ? 0.52f : 0.34f,
                                                           vp.x, vp.y, ss2::VoicePriority::Normal) ||
                                            playWorldSound("gunshot", 0.24f, vp.x, vp.y, ss2::VoicePriority::Low);
                        if (played) lastWeaponAudio["ricochet"] = ricNow;
                    }
                }
                if (!vp.alive && prevVehHp > 0) {
                    vehicleHoldFireUnits.erase(vp.index);
                    const bool deadOnWater = isWaterPoint(vp.x, vp.y);
                    if (!deadOnWater) {
                        stampBattleCrater(vp.x, vp.y, true);
                    }
                    const auto& deadVeh = vehicleScene->vehicles().at(vp.index);
                    const auto* deadSpec = unitCatalog ? unitCatalog->find(deadVeh.id) : nullptr;
                    const int sval = scoreValueForId(deadVeh.id, true);
                    if (isHostileOwner(vp.owner)) {
                        ++enemyVehDestroyed;
                        enemyScoreEarned += sval;
                        if (deadSpec) {
                            if (deadSpec->scoreType == "tank") ++enemyTanksDestroyed;
                            else if (deadSpec->scoreType == "gun" || deadSpec->scoreType == "aa") ++enemyGunsDestroyed;
                        }
                    } else if (vp.owner == localOwner()) {
                        friendlyScoreLost += sval;
                        if (deadSpec && deadSpec->scoreType == "tank") ++friendlyTanksLost;
                        else if (deadSpec && (deadSpec->scoreType == "gun" || deadSpec->scoreType == "aa")) ++friendlyGunsLost;
                        else if (deadSpec && !deadSpec->isSupplyCrate()) ++friendlyVehiclesLost;
                    }
                    if (vp.owner == localOwner()) {
                        recordEventPos(static_cast<double>(vp.x * mapScale), static_cast<double>(vp.y * mapScale));
                        pushMinimapPing(vp.x, vp.y, true);
                    }
                    if (combatEffects && vp.visible) {
                        const float wx = static_cast<float>(vp.x * mapScale);
                        const float wy = static_cast<float>(vp.y * mapScale);
                        if (activeVfx.size() < 64) {
                            activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, wx, wy, wx, wy, now,
                                                 std::chrono::milliseconds(650)});
                        }
                        if (deadSpec && deadSpec->isSupplyCrate()) {
                            combatVfx.spawnCrateBlast(static_cast<float>(vp.x), static_cast<float>(vp.y));
                        } else if (deadOnWater) {
                            combatVfx.spawnExplosion(static_cast<float>(vp.x), static_cast<float>(vp.y), 3, true);
                        } else {
                            combatVfx.spawnExplosion(static_cast<float>(vp.x), static_cast<float>(vp.y), 3, false);
                            combatVfx.spawnCrateBlast(static_cast<float>(vp.x), static_cast<float>(vp.y));
                        }
                        spritesDirty = true;
                        if (!deadOnWater && (!deadSpec || !deadSpec->isSupplyCrate())) {
                            if (burningWrecks.size() >= 24) burningWrecks.erase(burningWrecks.begin());
                            burningWrecks.push_back({wx, wy, now + std::chrono::seconds(18)});
                        }
                    }
                    (void)playWorldSound("defaultexplosion", 0.62f, vp.x, vp.y, ss2::VoicePriority::High);
                    std::cout << "Vehicle " << vp.index << " (owner=" << vp.owner << " group=" << vp.group
                              << ") eliminated\n";
                }
            }
        }
        const auto mEvents = simulation->pollMineEvents();
        for (const auto& me : mEvents) {
            const auto now = std::chrono::steady_clock::now();
            const float mx = static_cast<float>(me.x * mapScale);
            const float my = static_cast<float>(me.y * mapScale);
            if (me.kind == ss2::MineEvent::Detonated) {
                recordEventPos(static_cast<double>(mx), static_cast<double>(my));
                pushMinimapPing(me.x, me.y, true);
                const bool mineWater = isWaterPoint(me.x, me.y);
                if (!mineWater) {
                    stampBattleCrater(me.x, me.y, me.antiTank);
                }
                if (combatEffects) {
                    if (activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::Explosion, mx, my, mx, my, now,
                                             std::chrono::milliseconds(me.antiTank ? 520 : 380)});
                    }
                    if (isBuildingPointVisible(me.x, me.y)) {
                        combatVfx.spawnExplosion(static_cast<float>(me.x), static_cast<float>(me.y),
                                                 me.antiTank ? 3 : 1, mineWater);
                        spritesDirty = true;
                    }
                }
                if (audio) {
                    const std::string mineSnd = unitCatalog
                        ? (me.antiTank ? unitCatalog->mineSpec().atSound : unitCatalog->mineSpec().apSound)
                        : (me.antiTank ? std::string("defaultexplosion") : std::string("smallexplosion"));
                    (void)playWorldSound(mineSnd.empty() ? "defaultexplosion" : mineSnd,
                                         me.antiTank ? 0.62f : 0.54f, me.x, me.y, ss2::VoicePriority::High);
                    audio->playGroup("e_mines", 0.65f, ss2::VoicePriority::High);
                }
                showNotice(ss2::tr(me.antiTank ? "Anti-tank mine detonated!" : "Anti-personnel mine detonated!"),
                           ss2::NoticeKind::blocked);
            } else if (me.kind == ss2::MineEvent::Detected) {
                pushMinimapPing(me.x, me.y, true);
                if (audio) audio->playGroup("e_mines", 0.60f, ss2::VoicePriority::High);
                showNotice(ss2::tr("Enemy minefield detected!"), ss2::NoticeKind::unsupported);
            } else if (me.kind == ss2::MineEvent::Disarmed || me.kind == ss2::MineEvent::Laid) {
                std::string sapId;
                if (selection && !selection->selected().empty() && scene) {
                    const size_t sid = *selection->selected().begin();
                    if (sid < scene->units().size()) sapId = scene->units()[sid].id;
                }
                if (sapId.empty() && scene) {
                    for (const auto& u : scene->units()) {
                        if (u.hp > 0 && u.owner == localOwner()) { sapId = u.id; break; }
                    }
                }
                if (me.kind == ss2::MineEvent::Disarmed) {
                    if (!sapId.empty()) playUnitVoiceForId("getmine", sapId, false, true);
                    else (void)playWorldSound("d_getmine", 0.55f, me.x, me.y);
                    showNotice(ss2::tr("Mine disarmed."), ss2::NoticeKind::accepted);
                } else {
                    if (!sapId.empty()) playUnitVoiceForId("laymine", sapId, false, true);
                    showNotice(ss2::tr("Anti-tank mine laid."), ss2::NoticeKind::accepted);
                }
            }
        }
        if (!mEvents.empty()) liveVisibleMines = simulation->visibleMines();
        for (const auto& bu : simulation->pollBuildingUpdates()) {
            if (bu.index < liveBuildings.size()) {
                auto& lb = liveBuildings[bu.index];
                lb.hp = bu.hp;
                lb.maxHp = bu.maxHp;
                lb.occupantCount = bu.occupantCount;
                lb.occupantIds = bu.occupantIds;
                lb.owner = bu.owner;
                lb.destroyed = bu.destroyed;
                if (selectedBuilding && *selectedBuilding == bu.index && (lb.destroyed || lb.occupantCount == 0 || lb.owner != localOwner())) {
                    selectedBuilding.reset();
                }
                const int prevStage = lb.stampedStage;
                const int newStage = ss2::assets::buildingDamageStage(lb.hp, lb.maxHp, lb.destroyed);
                applyBuildingStage(bu.index, newStage);
                const auto now = std::chrono::steady_clock::now();
                const float bx = static_cast<float>(bu.x * mapScale);
                const float by = static_cast<float>(bu.y * mapScale);
                if (bu.collapsedThisTick) {
                    ++buildingsDemolished;
                    if (isHostileOwner(bu.owner)) enemyScoreEarned += 150;
                    recordEventPos(static_cast<double>(bx), static_cast<double>(by));
                    pushMinimapPing(bu.x, bu.y, true);
                    if (combatEffects) {
                        if (activeVfx.size() < 64) {
                            activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, bx, by, bx, by, now,
                                                 std::chrono::milliseconds(650)});
                        }
                        if (bu.owner == localOwner() || isBuildingPointVisible(bu.x, bu.y)) {
                            combatVfx.spawnExplosion(static_cast<float>(bu.x), static_cast<float>(bu.y), 4, false);
                            spritesDirty = true;
                        }
                        if (burningWrecks.size() >= 24) burningWrecks.erase(burningWrecks.begin());
                        burningWrecks.push_back({bx, by, now + std::chrono::seconds(20)});
                    }
                    (void)playWorldSound("defaultexplosion", 0.68f, bu.x, bu.y, ss2::VoicePriority::High);
                    showNotice(ss2::tr("Building destroyed!"), ss2::NoticeKind::blocked);
                } else if (newStage > prevStage && newStage < 3) {
                    if (bu.owner == localOwner() || isBuildingPointVisible(bu.x, bu.y)) {
                        if (combatEffects) {
                            if (activeVfx.size() < 64) {
                                activeVfx.push_back({ActiveVfx::Kind::Explosion, bx, by, bx, by, now,
                                                     std::chrono::milliseconds(440)});
                            }
                            combatVfx.spawnExplosion(static_cast<float>(bu.x), static_cast<float>(bu.y), 2, false);
                            spritesDirty = true;
                        }
                        if (newStage == 2 && combatEffects) {
                            if (burningWrecks.size() >= 24) burningWrecks.erase(burningWrecks.begin());
                            burningWrecks.push_back({bx, by, now + std::chrono::seconds(8)});
                        }
                    }
                    (void)playWorldSound("smallexplosion", 0.50f, bu.x, bu.y);
                } else if (bu.damagedThisTick && combatEffects &&
                           (bu.owner == localOwner() || isBuildingPointVisible(bu.x, bu.y))) {
                    if (activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::Explosion, bx, by, bx, by, now,
                                             std::chrono::milliseconds(320)});
                    }
                    combatVfx.spawnExplosion(static_cast<float>(bu.x), static_cast<float>(bu.y), 1, false);
                    spritesDirty = true;
                }
            }
        }
        for (const auto& bru : simulation->pollBridgeUpdates()) {
            if (bru.index >= liveBridges.size()) continue;
            auto& lb = liveBridges[bru.index];
            const bool wasDestroyed = lb.destroyed;
            lb.hp = bru.hp;
            lb.maxHp = bru.maxHp;
            lb.stage = bru.stage;
            lb.destroyed = bru.destroyed;
            applyBridgeStage(bru.index, bru.stage);
            const auto now = std::chrono::steady_clock::now();
            const float bx = static_cast<float>(bru.x * mapScale);
            const float by = static_cast<float>(bru.y * mapScale);
            if (bru.collapsedThisTick) {
                recordEventPos(static_cast<double>(bx), static_cast<double>(by));
                pushMinimapPing(bru.x, bru.y, true);
                if (combatEffects) {
                    if (activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, bx, by, bx, by, now,
                                             std::chrono::milliseconds(700)});
                    }
                    if (isBuildingPointVisible(bru.x, bru.y)) {
                        const int span = std::max(32, lb.width - 24);
                        for (int k = 0; k < 4; ++k) {
                            const float sx = static_cast<float>(lb.left + 12 + (span * k) / 3);
                            combatVfx.spawnExplosion(sx, static_cast<float>(bru.y), k == 1 ? 4 : 3, false);
                            combatVfx.spawnWaterSplash(sx, static_cast<float>(bru.y + (k % 2 ? 12 : -8)), 3);
                        }
                        spritesDirty = true;
                    }
                    if (burningWrecks.size() >= 24) burningWrecks.erase(burningWrecks.begin());
                    burningWrecks.push_back({bx, by, now + std::chrono::seconds(20)});
                }
                (void)playWorldSound("defaultexplosion", 0.72f, bru.x, bru.y, ss2::VoicePriority::High);
                showNotice(ss2::tr("Bridge destroyed!"), ss2::NoticeKind::blocked);
            } else if (bru.damagedThisTick && combatEffects &&
                       isBuildingPointVisible(bru.x, bru.y)) {
                if (activeVfx.size() < 64) {
                    activeVfx.push_back({ActiveVfx::Kind::Explosion, bx, by, bx, by, now,
                                         std::chrono::milliseconds(340)});
                }
                combatVfx.spawnExplosion(static_cast<float>(bru.x), static_cast<float>(bru.y), 1, false);
                spritesDirty = true;
            } else if (bru.repairedThisTick || (wasDestroyed && !bru.destroyed)) {
                if (wasDestroyed && !bru.destroyed) {
                    showNotice(ss2::tr("Bridge repaired and passable!"), ss2::NoticeKind::accepted);
                    if (audio && playWorldSound("construct", 0.52f, bru.x, bru.y, ss2::VoicePriority::Normal)) {
                        lastRepairAudioAt = now;
                    }
                } else if (audio && now - lastRepairAudioAt >= std::chrono::milliseconds(1500)) {
                    if (playWorldSound("construct", 0.42f, bru.x, bru.y, ss2::VoicePriority::Low)) {
                        lastRepairAudioAt = now;
                    }
                }
                if (combatEffects && activeVfx.size() < 64 && isBuildingPointVisible(bru.x, bru.y)) {
                    activeVfx.push_back({ActiveVfx::Kind::RepairPulse, bx, by, bx, by, now,
                                         std::chrono::milliseconds(550)});
                }
            }
        }
        if (audio) {
            if (paused || menu.active() || briefing.active()) {
                audio->stopAllLoops();
            } else {
                const auto now = std::chrono::steady_clock::now();
                int winW = 1280, winH = 720;
                if (!SDL_GetWindowSize(window, &winW, &winH)) { winW = 1280; winH = 720; }
                const auto [mx0, my0] = renderer.mapPosition(0.0f, 0.0f);
                const auto [mx1, my1] = renderer.mapPosition(static_cast<float>(winW), static_cast<float>(winH));
                const double s = static_cast<double>(std::max(1, mapScale));
                const double camX = 0.5 * (mx0 + mx1) / s;
                const double camY = 0.5 * (my0 + my1) / s;
                const double halfDiag = 0.5 * std::hypot(mx1 - mx0, my1 - my0) / s;
                const double halfViewW = 0.5 * std::abs(mx1 - mx0) / s;

                // Continuous weather bed loop (slot 0)
                if (missionWeather == assets::WeatherKind::Rain) {
                    audio->setLoopChannel(0u, "@rain_loop", 0.14f, 0.0f, 1.0f);
                } else {
                    audio->setLoopChannel(0u, "", 0.0f);
                }
                // Continuous burning-wreck firebed loop (slot 1)
                float fireGain = 0.0f, firePan = 0.0f;
                for (const auto& bw : burningWrecks) {
                    const double bwx = static_cast<double>(bw.x) / s;
                    const double bwy = static_cast<double>(bw.y) / s;
                    const float g = ss2::spatialGain(0.18f, bwx - camX, bwy - camY, halfDiag);
                    if (g > fireGain) {
                        fireGain = g;
                        firePan = ss2::viewportStereoPan(bwx - camX, halfViewW, 0.55f);
                    }
                }
                audio->setLoopChannel(1u, fireGain > 0.01f ? "@fire_loop" : "", fireGain, firePan, 1.0f);

                // Continuous aircraft drone or moving armor rumble (slot 2)
                float airGain = 0.0f, airPan = 0.0f, airPitch = 1.0f;
                for (const auto& [sid, lp] : livePlanes) {
                    (void)sid;
                    if (!lp.pose.active || !lp.pose.alive) continue;
                    const float g = ss2::spatialGain(0.20f, static_cast<double>(lp.pose.x) - camX,
                                                     static_cast<double>(lp.pose.y) - camY, halfDiag * 1.4);
                    if (g > airGain) {
                        airGain = g;
                        const double dx = static_cast<double>(lp.pose.x) - camX;
                        airPan = ss2::viewportStereoPan(dx, halfViewW, 0.65f);
                        const float ang = (static_cast<float>(lp.pose.dir32 & 31u) / 32.0f) * 6.2831853f;
                        const double approach = ((dx >= 0.0 ? -1.0 : 1.0) * std::cos(ang));
                        airPitch = std::clamp(1.0f + static_cast<float>(approach) * 0.045f, 0.94f, 1.05f);
                    }
                }
                audio->setLoopChannel(2u, airGain > 0.01f ? "aircraft" : "", airGain, airPan, airPitch);
                audio->updateLoops();

                if (now >= nextAmbientAt) {
                    const ss2::assets::AmbientWoofer* best = nullptr;
                    double bestDist = 1e12;
                    double bestRad = 1.0;
                    const auto& woofers = simulation->data().woofers;
                    for (const auto& w : woofers) {
                        const double radPx = std::max(320.0, static_cast<double>(w.radiusTiles) * 32.0);
                        const double d = std::hypot(static_cast<double>(w.x) - camX,
                                                    static_cast<double>(w.y) - camY);
                        if (d <= radPx && d < bestDist) {
                            best = &w;
                            bestDist = d;
                            bestRad = radPx;
                        }
                    }
                    if (best) {
                        const std::string ambSnd = ss2::assets::resolveAmbientWooferSound(best->sound);
                        const float gain = std::clamp(
                            best->volume * static_cast<float>(1.0 - 0.65 * (bestDist / bestRad)),
                            0.05f, 0.35f);
                        const float pan = ss2::viewportStereoPan(static_cast<double>(best->x) - camX, halfViewW, 0.55f);
                        (void)audio->playGroup(ambSnd, gain, ss2::VoicePriority::Ambient, pan);
                        const unsigned periodMs = (best->maxPeriodMs >= 500u)
                            ? std::clamp<unsigned>(
                                  ((ambientPhase & 1u) != 0u && best->minPeriodMs >= 500u)
                                      ? best->minPeriodMs : best->maxPeriodMs,
                                  1500u, 12000u)
                            : 6000u;
                        ++ambientPhase;
                        nextAmbientAt = now + std::chrono::milliseconds(periodMs);
                    } else if (missionWeather == assets::WeatherKind::Rain) {
                        if ((ambientPhase % 5u) == 0u) {
                            const float thunderPan = ((ambientPhase & 2u) != 0u) ? 0.25f : -0.25f;
                            (void)audio->playGroup("@groza", 0.22f, ss2::VoicePriority::Ambient, thunderPan);
                            lastWeatherLightningAt = now;
                        }
                        ++ambientPhase;
                        nextAmbientAt = now + std::chrono::milliseconds(2200);
                    } else if (missionWeather == assets::WeatherKind::Clear &&
                               (mapScheme == 0u || mapScheme == 2u)) {
                        const char* ambGroup = (mapScheme == 2u ? "@cicades_loop" : "@birds");
                        (void)audio->playGroup(ambGroup, 0.14f, ss2::VoicePriority::Ambient);
                        ++ambientPhase;
                        nextAmbientAt = now + std::chrono::milliseconds(3800);
                    } else {
                        ++ambientPhase;
                        nextAmbientAt = now + std::chrono::milliseconds(4000);
                    }
                }
            }
        }
        for (const auto& ap : simulation->pollAirSorties()) {
            spritesDirty = true;
            const auto now = std::chrono::steady_clock::now();
            if (ap.aaFired) {
                if (audio) {
                    const std::string aaSnd = !ap.aaSoundGroup.empty() ? ap.aaSoundGroup : std::string("gunshot");
                    auto& lastAa = lastWeaponAudio[aaSnd];
                    if (now - lastAa >= std::chrono::milliseconds(180)) {
                        bool played = playWorldSound(aaSnd, 0.48f, ap.aaOriginX, ap.aaOriginY);
                        if (!played && aaSnd != "gunshot") {
                            played = playWorldSound("gunshot", 0.48f, ap.aaOriginX, ap.aaOriginY);
                        }
                        if (played) lastAa = now;
                    }
                }
                if (combatEffects) {
                    if (activeVfx.size() < 64) {
                        activeVfx.push_back({
                            ActiveVfx::Kind::Tracer,
                            static_cast<float>(ap.aaOriginX * mapScale),
                            static_cast<float>(ap.aaOriginY * mapScale) - 12.0f * static_cast<float>(mapScale),
                            static_cast<float>(ap.x * mapScale),
                            static_cast<float>(ap.y * mapScale),
                            now,
                            std::chrono::milliseconds(140)
                        });
                    }
                    if (isBuildingPointVisible(ap.aaOriginX, ap.aaOriginY)) {
                        const unsigned aaDir = CombatVfxSystem::dir32FromMapDelta(
                            static_cast<double>(ap.x - ap.aaOriginX),
                            static_cast<double>(ap.y - ap.aaOriginY));
                        combatVfx.spawnMuzzleFlash(CombatVfxClip::MuzzleCannon,
                                                   static_cast<float>(ap.aaOriginX),
                                                   static_cast<float>(ap.aaOriginY - 10),
                                                   aaDir, 10.0f);
                    }
                }
            }
            if (ap.active) {
                auto [it, inserted] = livePlanes.try_emplace(ap.id);
                const int prevPlaneHp = inserted ? ap.maxHp : it->second.pose.hp;
                if (hud && combatEffects) {
                    const auto* pSpec = unitCatalog ? unitCatalog->findPlane(ap.planeId) : nullptr;
                    if (!pSpec || pSpec->hasFallSmoke) {
                        hud->recordAirSortieTrail(ap.id, ap.x, ap.y, ap.dir32, ap.hp, ap.maxHp,
                                                  ap.alive, true, ap.hp < prevPlaneHp);
                    }
                }
                it->second.pose = ap;
                if (inserted) uploadPlaneSprite(it->second);
            } else {
                if (hud) {
                    hud->recordAirSortieTrail(ap.id, ap.x, ap.y, ap.dir32, ap.hp, ap.maxHp,
                                              false, false, false);
                }
                const auto it = livePlanes.find(ap.id);
                if (it != livePlanes.end()) {
                    planeAtlas.release(it->second.spriteKey);
                    livePlanes.erase(it);
                }
            }
        }
        for (const auto& pp : simulation->pollParachutes()) {
            spritesDirty = true;
            if (pp.active) {
                liveParachutes[pp.id] = pp;
            } else {
                liveParachutes.erase(pp.id);
                if (!pp.alive && combatEffects && isBuildingPointVisible(pp.x, pp.y)) {
                    if (pp.altitudePx == 0 && isWaterPoint(pp.x, pp.y)) {
                        combatVfx.spawnWaterSplash(static_cast<float>(pp.x), static_cast<float>(pp.y),
                                                   pp.isSupplyCrate ? 2 : 1);
                    } else if (pp.altitudePx > 0) {
                        combatVfx.spawnDust(static_cast<float>(pp.x),
                                            static_cast<float>(pp.y - pp.altitudePx), false);
                    }
                }
            }
        }
        for (const auto& ae : airEvents) {
            const auto now = std::chrono::steady_clock::now();
            const float ax = static_cast<float>(ae.x * mapScale);
            const float ay = static_cast<float>(ae.y * mapScale);
            if (ae.kind == ss2::AirStrikeEvent::BombImpact) {
                if (ae.isLeaflet) {
                    pushMinimapPing(ae.x, ae.y, false);
                    if (combatEffects && activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::RepairPulse, ax, ay, ax, ay, now,
                                             std::chrono::milliseconds(500)});
                    }
                    if (audio) {
                        auto& lastLf = lastWeaponAudio["leaflet"];
                        if (now - lastLf >= std::chrono::milliseconds(600)) {
                            if (playWorldSound("e_telegram", 0.32f, ae.x, ae.y, ss2::VoicePriority::Low)) {
                                lastLf = now;
                            }
                        }
                    }
                    if (ae.owner == localOwner()) {
                        showNotice(ss2::tr("Propaganda leaflets dropped — enemy troops suppressed!"),
                                   ss2::NoticeKind::accepted);
                    }
                } else {
                    const bool bombOnWater = isWaterPoint(ae.x, ae.y);
                    if (!bombOnWater) {
                        stampBattleCrater(ae.x, ae.y, true);
                    }
                    pushMinimapPing(ae.x, ae.y, true);
                    if (combatEffects) {
                        if (activeVfx.size() < 64) {
                            activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, ax, ay, ax, ay, now,
                                                 std::chrono::milliseconds(620)});
                        }
                        if (isBuildingPointVisible(ae.x, ae.y)) {
                            combatVfx.spawnExplosion(static_cast<float>(ae.x), static_cast<float>(ae.y),
                                                     4, bombOnWater);
                            spritesDirty = true;
                        }
                    }
                    if (audio) {
                        const auto* pSpec = unitCatalog ? unitCatalog->findPlane(ae.planeId) : nullptr;
                        const std::string bSnd = (pSpec && !pSpec->bombSound.empty())
                            ? pSpec->bombSound : std::string("bomb");
                        auto& lastB = lastWeaponAudio[bSnd];
                        if (now - lastB >= std::chrono::milliseconds(140)) {
                            if (playWorldSound(bSnd, 0.62f, ae.x, ae.y, ss2::VoicePriority::High)) {
                                lastB = now;
                            }
                        }
                    }
                }
            } else if (ae.kind == ss2::AirStrikeEvent::StrafeImpact) {
                const auto* pSpec = unitCatalog ? unitCatalog->findPlane(ae.planeId) : nullptr;
                const bool rocketStrafe =
                    pSpec && pSpec->effectiveGroundHitType() == ss2::assets::HitType::Explosive;
                const bool strafeWater = isWaterPoint(ae.x, ae.y);
                if (rocketStrafe && !strafeWater) {
                    stampBattleCrater(ae.x, ae.y, false);
                }
                if (combatEffects) {
                    if (activeVfx.size() < 64) {
                        if (rocketStrafe) {
                            activeVfx.push_back({ActiveVfx::Kind::Explosion, ax, ay, ax, ay, now,
                                                 std::chrono::milliseconds(360)});
                        } else {
                            activeVfx.push_back({ActiveVfx::Kind::Tracer,
                                                 ax - 20.0f * static_cast<float>(mapScale),
                                                 ay - 28.0f * static_cast<float>(mapScale),
                                                 ax, ay, now, std::chrono::milliseconds(240)});
                        }
                    }
                    if (isBuildingPointVisible(ae.x, ae.y)) {
                        if (rocketStrafe) {
                            combatVfx.spawnExplosion(static_cast<float>(ae.x), static_cast<float>(ae.y),
                                                     1, strafeWater);
                        } else if (strafeWater) {
                            combatVfx.spawnWaterSplash(static_cast<float>(ae.x), static_cast<float>(ae.y), 1);
                        } else {
                            combatVfx.spawnDust(static_cast<float>(ae.x), static_cast<float>(ae.y), false);
                        }
                        spritesDirty = true;
                    }
                }
                if (audio) {
                    const std::string stSnd = (pSpec && !pSpec->groundShotSound.empty())
                        ? pSpec->groundShotSound : std::string("mg2_aa");
                    auto& lastSt = lastWeaponAudio[stSnd];
                    if (now - lastSt >= std::chrono::milliseconds(180)) {
                        if (playWorldSound(stSnd, 0.50f, ae.x, ae.y)) {
                            lastSt = now;
                        }
                    }
                }
            } else if (ae.kind == ss2::AirStrikeEvent::ParatrooperDrop) {
                if (ae.owner == localOwner()) {
                    recordEventPos(static_cast<double>(ax), static_cast<double>(ay));
                    pushMinimapPing(ae.x, ae.y, false);
                    if (audio) audio->playGroup("e_reenforcement", 0.72f, ss2::VoicePriority::High);
                    std::size_t stickTotal = 0;
                    for (const auto& entry : ae.paratroopers) {
                        if (entry.count > 0) stickTotal += static_cast<std::size_t>(entry.count);
                    }
                    showNotice(ss2::trf("Paratroopers jumping ({0} airborne)!",
                                        {std::to_string(std::max<std::size_t>(1u, stickTotal))}),
                               ss2::NoticeKind::accepted);
                }
            } else if (ae.kind == ss2::AirStrikeEvent::SupplyDrop) {
                if (ae.owner == localOwner()) {
                    recordEventPos(static_cast<double>(ax), static_cast<double>(ay));
                    pushMinimapPing(ae.x, ae.y, false);
                    showNotice(ss2::tr("Air supply drop inbound!"), ss2::NoticeKind::accepted);
                }
            } else if (ae.kind == ss2::AirStrikeEvent::BoxDust) {
                if (combatEffects && isBuildingPointVisible(ae.x, ae.y)) {
                    combatVfx.spawnDust(static_cast<float>(ae.x), static_cast<float>(ae.y), true);
                    spritesDirty = true;
                }
            } else if (ae.kind == ss2::AirStrikeEvent::ParatrooperLanded) {
                spritesDirty = true;
                if (ae.owner == localOwner()) {
                    pushMinimapPing(ae.x, ae.y, false);
                }
            } else if (ae.kind == ss2::AirStrikeEvent::SupplyCrateLanded) {
                spritesDirty = true;
                if (combatEffects && activeVfx.size() < 64 && isBuildingPointVisible(ae.x, ae.y)) {
                    activeVfx.push_back({ActiveVfx::Kind::RepairPulse, ax, ay, ax, ay, now,
                                         std::chrono::milliseconds(650)});
                }
                (void)playWorldSound("construct", 0.50f, ae.x, ae.y);
            } else if (ae.kind == ss2::AirStrikeEvent::PlaneShotDown) {
                const int pScore = ae.scoreValue > 0 ? ae.scoreValue : 500;
                if (isHostileOwner(ae.owner)) {
                    ++enemyPlanesShotDown;
                    enemyScoreEarned += pScore;
                } else if (ae.owner == localOwner()) {
                    friendlyScoreLost += pScore;
                }
                const bool crashWater = isWaterPoint(ae.x, ae.y);
                if (!crashWater) {
                    stampBattleCrater(ae.x, ae.y, true);
                }
                pushMinimapPing(ae.x, ae.y, true);
                if (combatEffects) {
                    if (hud) {
                        hud->recordAirCrashTrail(ae.x, ae.y);
                        spritesDirty = true;
                    }
                    if (activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, ax, ay, ax, ay, now,
                                             std::chrono::milliseconds(700)});
                    }
                    if (isBuildingPointVisible(ae.x, ae.y)) {
                        combatVfx.spawnExplosion(static_cast<float>(ae.x), static_cast<float>(ae.y),
                                                 4, crashWater);
                        if (!crashWater) {
                            combatVfx.spawnCrateBlast(static_cast<float>(ae.x), static_cast<float>(ae.y));
                        }
                        spritesDirty = true;
                    }
                    if (!crashWater) {
                        if (burningWrecks.size() >= 24) burningWrecks.erase(burningWrecks.begin());
                        burningWrecks.push_back({ax, ay, now + std::chrono::seconds(14)});
                    }
                }
                const auto* pSpec = unitCatalog ? unitCatalog->findPlane(ae.planeId) : nullptr;
                const std::string fallSnd = (pSpec && !pSpec->fallSound.empty())
                    ? pSpec->fallSound : std::string("falling_heavy");
                (void)playWorldSound(fallSnd, 0.58f, ae.x, ae.y, ss2::VoicePriority::High);
                (void)playWorldSound("defaultexplosion", 0.68f, ae.x, ae.y, ss2::VoicePriority::High);
                showNotice(ss2::tr("Aircraft shot down by AA flak!"), ss2::NoticeKind::blocked);
            }
        }
        if (combatEffects && combatVfx.atlasSlot().has_value() && simulation &&
            (simulation->simTick() % 8u) == 0u) {
            const auto smokeClouds = simulation->activeSmokeClouds();
            const unsigned phase = (simulation->simTick() / 8u) & 3u;
            static constexpr std::array<std::pair<float, float>, 4> kSmokeOffsets{{
                {-14.0f, -8.0f}, {16.0f, -6.0f}, {-10.0f, 10.0f}, {12.0f, 12.0f}
            }};
            for (std::size_t sci = 0; sci < smokeClouds.size(); ++sci) {
                const auto& sc = smokeClouds[sci];
                if (!isBuildingPointVisible(sc.x, sc.y)) continue;
                const auto& off = kSmokeOffsets[(phase + sci) & 3u];
                const float puffScale = (sc.radiusPx >= 90) ? 1.45f : 1.15f;
                combatVfx.spawnClip(CombatVfxClip::BoxDust,
                                    static_cast<float>(sc.x) + off.first,
                                    static_cast<float>(sc.y) + off.second,
                                    0u, 0.95f, puffScale, 0.56f, 200050);
                spritesDirty = true;
            }
        }
        if (!paused && (!gpuAtlasSlots.empty() || !gpuVehicleIds.empty())) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastAnimSync >= std::chrono::milliseconds(50)) {
                spritesDirty = true;
                lastAnimSync = now;
            }
        }
        if (spritesDirty) syncGpuSprites();
        if (selection && selection->playerLosses() > reportedLosses) {
            const auto total = selection->playerLosses();
            const auto delta = total - reportedLosses;
            reportedLosses = total;
            showNotice(
                delta == 1 ? ss2::trf("1 soldier lost ({0} total).", {std::to_string(total)})
                           : ss2::trf("{0} soldiers lost ({1} total).", {std::to_string(delta), std::to_string(total)}),
                ss2::NoticeKind::blocked);
        }
        if (!activeMarkers.empty()) {
            const auto now = std::chrono::steady_clock::now();
            const auto before = activeMarkers.size();
            std::erase_if(activeMarkers, [&](const TimedMarker& m) { return now >= m.until; });
            if (activeMarkers.size() != before) markersDirty = true;
        }
        if (markersDirty) syncMarkers();
        std::vector<ss2::MissionEvent> eventsToProcess;
        if (multiplayer.enabled) {
            eventsToProcess = std::move(deferredLockstepEvents);
            deferredLockstepEvents.clear();
            for (const auto& ev : simulation->poll()) {
                if (ev.kind == ss2::MissionEvent::Reinforcement) {
                    spawnReinforcementWave(ev);
                    spritesDirty = true;
                } else {
                    eventsToProcess.push_back(ev);
                }
            }
        } else {
            eventsToProcess = simulation->poll();
        }
        for (const auto event : eventsToProcess) {
            const auto& data = simulation->data();
            if (event.kind == ss2::MissionEvent::Dialog || event.kind == ss2::MissionEvent::Message)
                std::cout << "Tutorial I phrase " << event.index << '\n';
            if (event.kind == ss2::MissionEvent::Dialog || event.kind == ss2::MissionEvent::Camera) {
                if (selection->cancelDrag()) {
                    gestureInterrupted = true;
                    std::cout << "Tutorial I pointer gesture interrupted by "
                              << (event.kind == ss2::MissionEvent::Dialog ? "briefing " : "camera move ")
                              << event.index << '\n';
                }
            }
            if (event.kind == ss2::MissionEvent::Dialog) {
                if (replayFile) {
                    logHud(data.phrases.at(event.index));
                    continue;
                }
                resetCursor();
                if (!activeMarkers.empty()) {
                    activeMarkers.clear();
                    syncMarkers();
                }
                if (audio) {
                    if (event.index < data.phraseSounds.size() && !data.phraseSounds[event.index].empty()) {
                        audio->playGroup(data.phraseSounds[event.index], 0.90f, ss2::VoicePriority::High);
                    } else {
                        audio->playGroup("e_telegram", 0.55f);
                    }
                }
                const auto& phrase = data.phrases.at(event.index);
                logHud(phrase);
                if (briefing.active()) pendingDialogs.push_back(phrase);
                else briefing.open(phrase);
            } else if (event.kind == ss2::MissionEvent::Camera) {
                const auto [x, y] = data.locations.at(event.index);
                recordEventPos(x * mapScale, y * mapScale);
                renderer.focusMap(x * mapScale, y * mapScale);
            } else if (event.kind == ss2::MissionEvent::Message) {
                if (audio) {
                    if (event.index < data.phraseSounds.size() && !data.phraseSounds[event.index].empty()) {
                        audio->playGroup(data.phraseSounds[event.index], 0.90f, ss2::VoicePriority::High);
                    } else {
                        audio->playGroup("e_telegram", 0.50f);
                    }
                }
                const auto& message = data.phrases.at(event.index);
                currentObjective = message;
                logHud(message);
                SDL_SetWindowTitle(window, message.c_str());
                orderNotice.show(message, ss2::NoticeKind::accepted);
                if (!multiplayer.enabled && !limit && !timing && !replayFile && nextMissionOut &&
                    !paused && simulation->simTick() >= nextObjAutoSaveTick) {
                    nextObjAutoSaveTick = simulation->simTick() + 1500u;
                    doSaveSlot(-3, true);
                }
            } else if (event.kind == ss2::MissionEvent::Marker && selection) {
                const auto [x, y] = data.markers.at(event.index);
                objectiveMarkerSim = {x, y};
                recordEventPos(x * mapScale, y * mapScale);
                selection->mark(x * mapScale, y * mapScale);
                markerHint = true;
            } else if (event.kind == ss2::MissionEvent::Victory) {
                std::cout << (currentMissionTitle.empty() ? "Tutorial I" : currentMissionTitle)
                          << " victory (" << event.index << ")\n";
                if (audio) audio->playGroup("e_victory", 0.80f, ss2::VoicePriority::High);
                currentObjective = ss2::tr(multiplayer.enabled ? "Multiplayer Victory!" : "Mission Accomplished!");
                logHud(currentObjective);
                const std::string title = "Sudden Strike II | " +
                    (currentMissionTitle.empty() ? std::string("Tutorial I") : currentMissionTitle) +
                    " — Victory!";
                SDL_SetWindowTitle(window, title.c_str());
                orderNotice.show(currentObjective, ss2::NoticeKind::accepted, ss2::Notice::Duration::zero());
                openDebriefMenu(true);
            } else if (event.kind == ss2::MissionEvent::Defeat) {
                std::cout << (currentMissionTitle.empty() ? "Mission" : currentMissionTitle)
                          << " defeat (" << event.index << ")\n";
                if (audio) audio->playGroup("e_defeat", 0.80f, ss2::VoicePriority::High);
                currentObjective = ss2::tr(multiplayer.enabled ? "Multiplayer Defeat." : "Mission Failed.");
                logHud(currentObjective);
                const std::string title = "Sudden Strike II | " +
                    (currentMissionTitle.empty() ? std::string("Mission") : currentMissionTitle) +
                    " — Defeat";
                SDL_SetWindowTitle(window, title.c_str());
                orderNotice.show(currentObjective, ss2::NoticeKind::blocked, ss2::Notice::Duration::zero());
                openDebriefMenu(false);
            } else if (event.kind == ss2::MissionEvent::Reinforcement) {
                if (!replayFile) {
                    spawnReinforcementWave(event);
                    spritesDirty = true;
                    if (!multiplayer.enabled && !limit && !timing && nextMissionOut &&
                        !paused && simulation->simTick() >= nextObjAutoSaveTick) {
                        nextObjAutoSaveTick = simulation->simTick() + 1500u;
                        doSaveSlot(-3, true);
                    }
                }
            }
        }
        if (spritesDirty) syncGpuSprites();
        if (!multiplayer.enabled && !limit && !timing && !replayFile && nextMissionOut &&
            !paused && !briefing.active() && !menu.active() &&
            simulation->simTick() >= nextAutoSaveTick) {
            nextAutoSaveTick = simulation->simTick() + 7500u;
            doSaveSlot(-1);
        }
    }
    if (gestureInterrupted && !briefing.active()) {
        orderNotice.show(ss2::tr("A mission event interrupted your input. Select or issue the order again."),
                         ss2::NoticeKind::unsupported);
        gestureInterrupted = false;
        markerHint = false;
    } else if (markerHint && !briefing.active()) {
        orderNotice.show(ss2::tr("M shows the mission marker. Tab focuses your troops."), ss2::NoticeKind::unsupported);
        markerHint = false;
    }
}

} // namespace ss2
