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

void GameSession::armAirSupportKind(assets::AirMissionKind kind) {
    if (!simulation) return;
    const auto slots = simulation->playerAirSupport();
    const std::size_t idx = static_cast<std::size_t>(kind);
    if (idx >= slots.size() || slots[idx].ready <= 0) {
        orderNotice.show(ss2::trf("No {0} sorties available.", {ss2::tr(airKindLabel(kind))}),
                         ss2::NoticeKind::unsupported);
        return;
    }
    attackMoveArmed = false;
    attackGroundArmed = false;
    reverseMoveArmed = false;
    patrolArmed = false;
    faceArmed = false;
    armedAirSupport = kind;
    const bool strike = kind == ss2::assets::AirMissionKind::Bomber ||
                        kind == ss2::assets::AirMissionKind::Fighter;
    SDL_SetCursor(strike ? (attackCursor ? attackCursor.get() : SDL_GetDefaultCursor())
                         : (moveCursor ? moveCursor.get() : SDL_GetDefaultCursor()));
    orderNotice.show(ss2::trf("{0} ({1} ready): left-click target area.",
                              {ss2::tr(airKindLabel(kind)), std::to_string(slots[idx].ready)}),
                     ss2::NoticeKind::accepted);
}

void GameSession::doTogglePause() {
    if (!simulation) return;
    paused = simulation->togglePause();
    if (paused) showPause();
    else orderNotice.show(ss2::tr("Resumed."), ss2::NoticeKind::accepted);
    std::cout << (paused ? "Mission clock paused" : "Mission clock resumed") << '\n';
}

void GameSession::doStop() {
    if (!simulation || !selection) return;
    const auto& sel = selection->selected();
    const auto& vsel = selection->selectedVehicles();
    if (sel.empty() && vsel.empty()) return;
    size_t n = 0;
    if (!sel.empty()) {
        const std::vector<size_t> ids(sel.begin(), sel.end());
        n += simulation->stop(ids);
    }
    if (!vsel.empty()) {
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        n += simulation->stopVehicles(vids);
    }
    if (n) {
        playSelectionVoice("stop");
        attackMoveArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        patrolArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        resetCursor();
        showNotice(ss2::trf("Stop order ({0} soldiers).", {std::to_string(n)}), ss2::NoticeKind::accepted);
        std::cout << "Tutorial I tactical S (stop): " << n << " units\n";
    }
}

void GameSession::doToggleHoldFire() {
    if (!simulation || !selection) return;
    const auto& sel = selection->selected();
    const auto& vsel = selection->selectedVehicles();
    if (sel.empty() && vsel.empty()) {
        if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
            const auto& sb = liveBuildings[*selectedBuilding];
            if (!sb.destroyed && sb.owner == 0 && sb.occupantCount > 0) {
                if (const auto hold = simulation->toggleBuildingHoldFire(*selectedBuilding)) {
                    for (const size_t sid : sb.occupantIds) {
                        if (*hold) holdFireUnits.insert(sid);
                        else holdFireUnits.erase(sid);
                    }
                    showNotice(ss2::tr(*hold ? "Garrison hold fire enabled."
                                             : "Garrison fire at will enabled."),
                               ss2::NoticeKind::accepted);
                }
            }
        }
        return;
    }
    std::optional<bool> holdState;
    if (!sel.empty()) {
        const std::vector<size_t> ids(sel.begin(), sel.end());
        if (const auto hold = simulation->toggleHoldFire(ids)) {
            holdState = *hold;
            for (const auto id : ids) {
                if (*hold) holdFireUnits.insert(id);
                else holdFireUnits.erase(id);
            }
        }
    }
    if (!vsel.empty()) {
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        if (const auto vhold = simulation->toggleVehicleHoldFire(vids)) {
            if (!holdState) holdState = *vhold;
            for (const auto id : vids) {
                if (*vhold) vehicleHoldFireUnits.insert(id);
                else vehicleHoldFireUnits.erase(id);
            }
        }
    }
    if (holdState) {
        showNotice(ss2::tr(*holdState ? "Hold fire enabled." : "Fire at will enabled."), ss2::NoticeKind::accepted);
        std::cout << "Tutorial I tactical H (hold-fire=" << (*holdState ? "on" : "off") << "): "
                  << (sel.size() + vsel.size()) << " units\n";
    }
}

void GameSession::doToggleCrouch() {
    if (!simulation || !selection || selection->selected().empty()) return;
    const auto& sel = selection->selected();
    const std::vector<size_t> ids(sel.begin(), sel.end());
    if (const auto crouched = simulation->toggleCrouch(ids)) {
        for (const auto id : ids) {
            if (*crouched) crouchedUnits.insert(id);
            else {
                crouchedUnits.erase(id);
                camouflagedUnits.erase(id);
            }
        }
        syncGpuSprites();
        showNotice(ss2::tr(*crouched ? "Stance: Crouching (cover bonus, shorter sight)." : "Stance: Standing."),
                   ss2::NoticeKind::accepted);
        std::cout << "Tutorial I tactical C (stance=" << (*crouched ? "crouch" : "stand") << "): " << ids.size() << " units\n";
    }
}

void GameSession::doToggleBinoculars() {
    if (!simulation || !selection) return;
    if (selection->selected().empty()) {
        if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
            const auto& sb = liveBuildings[*selectedBuilding];
            if (!sb.destroyed && sb.owner == 0 && sb.occupantCount > 0) {
                if (const auto active = simulation->toggleBuildingBinoculars(*selectedBuilding)) {
                    for (const size_t sid : sb.occupantIds) {
                        const bool hasBino = scene && sid < scene->units().size() &&
                                             soldierHasBinoculars(scene->units()[sid].id);
                        if (hasBino) {
                            if (*active) binocularUnits.insert(sid);
                            else binocularUnits.erase(sid);
                        }
                    }
                    showNotice(ss2::tr(*active ? "Garrison binoculars active (elevated vision)."
                                               : "Garrison binoculars stowed."),
                               ss2::NoticeKind::accepted);
                } else {
                    orderNotice.show(ss2::tr("No officer or sniper garrisoned in building."),
                                     ss2::NoticeKind::unsupported);
                }
            }
        }
        return;
    }
    const auto& sel = selection->selected();
    const std::vector<size_t> ids(sel.begin(), sel.end());
    if (const auto active = simulation->toggleBinoculars(ids)) {
        for (const auto id : ids) {
            if (scene && id < scene->units().size() && !soldierHasBinoculars(scene->units()[id].id)) continue;
            if (*active) binocularUnits.insert(id);
            else binocularUnits.erase(id);
        }
        showNotice(ss2::tr(*active ? "Binoculars active (extended vision)." : "Binoculars stowed."),
                   ss2::NoticeKind::accepted);
        std::cout << "Mission tactical B (binoculars=" << (*active ? "on" : "off") << "): " << ids.size() << " units\n";
    }
}

void GameSession::doArmAttackMove() {
    if (!simulation || !selection || (selection->selected().empty() && selection->selectedVehicles().empty())) return;
    attackGroundArmed = false;
    reverseMoveArmed = false;
    patrolArmed = false;
    faceArmed = false;
    armedAirSupport.reset();
    attackMoveArmed = !attackMoveArmed;
    if (attackMoveArmed) {
        SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
        orderNotice.show(ss2::tr("Attack-move: click target location."), ss2::NoticeKind::accepted);
    } else {
        resetCursor();
    }
}

void GameSession::doArmPatrol() {
    if (!simulation || !selection || (selection->selected().empty() && selection->selectedVehicles().empty())) return;
    attackMoveArmed = false;
    attackGroundArmed = false;
    reverseMoveArmed = false;
    faceArmed = false;
    armedAirSupport.reset();
    patrolArmed = !patrolArmed;
    if (patrolArmed) {
        SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
        orderNotice.show(ss2::tr("Patrol: click destination on map (Esc/right-click to cancel)."),
                         ss2::NoticeKind::accepted);
    } else {
        resetCursor();
    }
}

void GameSession::doArmAttackGround() {
    if (!simulation || !selection) return;
    const bool hasBombardInf = scene && std::ranges::any_of(selection->selected(), [&](size_t id) {
        return id < scene->units().size() && isBombardInfantryId(scene->units()[id].id);
    });
    if (selection->selectedVehicles().empty() && !hasBombardInf) return;
    attackMoveArmed = false;
    reverseMoveArmed = false;
    patrolArmed = false;
    faceArmed = false;
    armedAirSupport.reset();
    attackGroundArmed = !attackGroundArmed;
    if (attackGroundArmed) {
        SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
        orderNotice.show(ss2::tr("Attack-ground: click coordinates to bombard."), ss2::NoticeKind::accepted);
    } else {
        resetCursor();
    }
}

std::optional<std::size_t> GameSession::buildingAtMap(int simX, int simY) const {
    std::optional<std::size_t> best;
    int bestY = -100000;
    for (std::size_t i = 0; i < liveBuildings.size(); ++i) {
        const auto& b = liveBuildings[i];
        if (b.destroyed) continue;
        if (simX >= b.left && simX < b.left + b.width && simY >= b.top && simY < b.top + b.height) {
            if (b.y >= bestY) {
                bestY = b.y;
                best = i;
            }
        }
    }
    return best;
}

void GameSession::doUnloadOrUnhitch() {
    if (!simulation || !selection) return;
    bool handledU = false;
    if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
        const auto& lb = liveBuildings[*selectedBuilding];
        if (!lb.destroyed && lb.owner == 0 && lb.occupantCount > 0) {
            const size_t evac = simulation->evacuateBuilding(*selectedBuilding);
            if (evac > 0) {
                selectedBuilding.reset();
                playSelectionVoice("move");
                showNotice(ss2::trf("Evacuated {0} soldiers from building.", {std::to_string(evac)}),
                           ss2::NoticeKind::accepted);
                handledU = true;
            }
        }
    }
    if (!handledU && !selection->selectedVehicles().empty()) {
        const auto& vsel = selection->selectedVehicles();
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        const size_t unhitched = simulation->unhitchVehicles(vids);
        if (unhitched > 0) {
            playSelectionVoice("move");
            showNotice(ss2::trf("Unhitched {0} field guns.", {std::to_string(unhitched)}),
                       ss2::NoticeKind::accepted);
            handledU = true;
        } else {
            const size_t unloaded = simulation->unloadVehicles(vids);
            if (unloaded > 0) {
                playSelectionVoice("move");
                showNotice(ss2::trf("Unloaded {0} soldiers.", {std::to_string(unloaded)}), ss2::NoticeKind::accepted);
                handledU = true;
            }
        }
    }
    if (!handledU && !liveBuildings.empty()) {
        float mx{}, my{};
        SDL_GetMouseState(&mx, &my);
        const auto [mapX, mapY] = renderer.mapPosition(mx, my);
        const auto hoveredBld = buildingAtMap(int(std::floor(mapX / mapScale)), int(std::floor(mapY / mapScale)));
        if (hoveredBld && liveBuildings[*hoveredBld].owner == 0 && liveBuildings[*hoveredBld].occupantCount > 0) {
            const size_t evac = simulation->evacuateBuilding(*hoveredBld);
            if (evac > 0) {
                if (selectedBuilding && *selectedBuilding == *hoveredBld) selectedBuilding.reset();
                playSelectionVoice("move");
                showNotice(ss2::trf("Evacuated {0} soldiers from building.", {std::to_string(evac)}),
                           ss2::NoticeKind::accepted);
                handledU = true;
            }
        }
    }
    if (!handledU && !selection->selectedVehicles().empty()) {
        orderNotice.show(ss2::tr("No passengers or towed guns to unload."), ss2::NoticeKind::unsupported);
    }
}

void GameSession::doLayMine() {
    if (!simulation || !selection || selection->selected().empty()) return;
    const auto& sel = selection->selected();
    const std::vector<size_t> ids(sel.begin(), sel.end());
    const size_t laid = simulation->layMine(ids);
    if (laid > 0) {
        liveVisibleMines = simulation->visibleMines();
        playSelectionVoice("laymine");
    } else {
        orderNotice.show(ss2::tr("Select a Sapper with AT mines to lay mines here."), ss2::NoticeKind::unsupported);
    }
}

void GameSession::doScatter() {
    if (!simulation || !selection || selection->selected().empty()) return;
    const auto& sel = selection->selected();
    const std::vector<size_t> ids(sel.begin(), sel.end());
    const size_t n = simulation->scatter(ids);
    if (n > 0) {
        attackMoveArmed = false;
        patrolArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        resetCursor();
        playSelectionVoice("move");
        showNotice(ss2::trf("Scattering {0} infantry.", {std::to_string(n)}), ss2::NoticeKind::accepted);
    } else {
        orderNotice.show(ss2::tr("Unable to scatter on this terrain."), ss2::NoticeKind::unsupported);
    }
}

void GameSession::doAbandonVehicles() {
    if (!simulation || !selection || !scene || !vehicleScene) return;
    const auto& vsel = selection->selectedVehicles();
    if (vsel.empty()) return;
    const std::vector<size_t> vids(vsel.begin(), vsel.end());
    const auto spawns = simulation->abandonVehicles(vids);
    if (spawns.empty()) {
        orderNotice.show(ss2::tr("Selected vehicles have no crew to abandon."), ss2::NoticeKind::unsupported);
        return;
    }
    for (const auto& vid : vids) {
        if (vid < vehicleScene->vehicles().size()) {
            vehicleScene->setOwner(vid, 3);
            selection->updateVehicle(vid, scaleVehicle(vehicleScene->vehicles().at(vid), mapScale));
        }
    }
    std::size_t crewCount = 0;
    for (const auto& rawCp : spawns) {
        auto cp = rawCp;
        (void)ensureGpuInfantrySlot(cp.id);
        auto spawned = scene->spawn(cp);
        if (!spawned) {
            for (const auto& existing : scene->units()) {
                if (existing.owner == 0) {
                    cp.id = existing.id;
                    (void)ensureGpuInfantrySlot(cp.id);
                    spawned = scene->spawn(cp);
                    if (spawned) break;
                }
            }
        }
        if (spawned) {
            const auto& [newIdx, patch] = *spawned;
            const auto& unit = scene->units().at(newIdx);
            ensureUnitPortrait(unit.id, false);
            (void)simulation->spawnInfantry(unit, 0);
            const int realAmmo = scaledSoldierAmmo(unit.id, unit.ammo);
            (void)scene->update(newIdx, unit.x, unit.y, unit.direction, unit.progress, unit.walking, unit.hp,
                                -1, realAmmo);
            selection->addUnit(scaleInfantry(scene->units().at(newIdx), mapScale));
            initialUnitHp.push_back(std::max(1, unit.hp));
            unitExpa.push_back(0);
            unitKills.push_back(0);
            unitGrenades.push_back(initialGrenadesForId(unit.id));
            unitMineAmmo.push_back(initialMinesForId(unit.id));
            if (patch.pixels.width && patch.pixels.height) {
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
            ++crewCount;
        }
    }
    syncGpuSprites();
    playSelectionVoice("move");
    showNotice(ss2::trf("Crew abandoned {0} vehicle(s).", {std::to_string(crewCount)}), ss2::NoticeKind::accepted);
}

bool GameSession::issueGroundOrder(int simX, int simY, bool isAttackMove, bool append) {
    if (!simulation || !selection) return false;
    const auto& selected = selection->selected();
    const auto& selectedVeh = selection->selectedVehicles();
    const std::vector<size_t> ids(selected.begin(), selected.end());
    const std::vector<size_t> vids(selectedVeh.begin(), selectedVeh.end());
    std::optional<size_t> infMove = size_t(0), vehMove = size_t(0);
    if (!ids.empty()) {
        infMove = isAttackMove ? simulation->attackMove(ids, simX, simY, append)
                               : simulation->move(ids, simX, simY, append);
    }
    if (infMove && !vids.empty()) {
        vehMove = isAttackMove ? simulation->attackMoveVehicles(vids, simX, simY, append)
                               : simulation->moveVehicles(vids, simX, simY, append);
    }
    if (!infMove || !vehMove) {
        gestureInterrupted = true;
        std::cout << "Tutorial I move interrupted by a pending briefing\n";
        return false;
    }
    const auto count = *infMove + *vehMove;
    const auto total = ids.size() + vids.size();
    if (count) {
        if (*infMove) {
            for (const auto id : ids) binocularUnits.erase(id);
        }
        std::string label;
        if (isAttackMove) {
            label = count == 1 && total == 1 ? ss2::tr("Attack-move order accepted")
                  : ss2::trf("Attack-move order accepted by {0} of {1} units", {std::to_string(count), std::to_string(total)});
        } else if (append) {
            label = ss2::trf("Waypoint queued ({0} units)", {std::to_string(count)});
        } else {
            label = count == 1 && total == 1 ? ss2::tr("Move order accepted")
                  : ss2::trf("Move order accepted by {0} of {1} soldiers", {std::to_string(count), std::to_string(total)});
        }
        if (paused) label = ss2::trf("{0}. Press Pause or Ctrl+P to resume.", {label});
        showNotice(label, count == total ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
        activeMarkers.push_back({
            {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
             isAttackMove ? ss2::MarkerKind::attack : ss2::MarkerKind::move},
            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
            std::nullopt,
            true
        });
        syncMarkers();
        playSelectionVoice(isAttackMove ? "attack" : "move");
        std::cout << "Tutorial I accepted move to map pixel " << simX << ',' << simY
                  << " for " << count << '/' << total << (vids.empty() ? " selected infantry\n" : " selected units\n");
        return true;
    }
    std::cout << "Tutorial I rejected move to map pixel " << simX << ',' << simY
              << " for " << total << (vids.empty() ? " selected infantry\n" : " selected units\n");
    const auto [u, v] = ss2::tutorialPointerWorld(simX, simY);
    const bool outside = std::min(u, v) < 0 || std::max(u, v) >= 4096;
    orderNotice.show(ss2::tr(total == 0
                         ? (selection->playerLosses() > 0
                                ? "Your selected soldiers were lost. Select remaining troops."
                                : "Select your infantry, then right-click the destination.")
                         : outside ? "That point is outside the map."
                                   : "No route from this position. Choose another destination."),
                     total == 0 ? ss2::NoticeKind::unsupported : ss2::NoticeKind::blocked);
    return false;
}

void GameSession::dispatchAirSupportAt(assets::AirMissionKind kind, int simX, int simY) {
    if (!simulation) return;
    const auto slots = simulation->playerAirSupport();
    const std::string planeId = slots[static_cast<std::size_t>(kind)].planeId;
    if (simulation->callAirSupport(kind, simX, simY)) {
        ++airSortiesFlown;
        const bool strike = kind == ss2::assets::AirMissionKind::Bomber ||
                            kind == ss2::assets::AirMissionKind::Fighter;
        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
        activeMarkers.push_back({
            {{(simX - 10) * mapScale, (simY - 10) * mapScale, 20 * mapScale, 20 * mapScale},
             strike ? ss2::MarkerKind::attack : ss2::MarkerKind::move},
            std::chrono::steady_clock::now() + std::chrono::milliseconds(800),
            std::nullopt,
            true
        });
        syncMarkers();
        pushMinimapPing(simX, simY, strike);
        if (audio) {
            if (strike) (void)audio->playGroup("e_airraid_siren", 0.55f, ss2::VoicePriority::High);
            const auto* pSpec = unitCatalog ? unitCatalog->findPlane(planeId) : nullptr;
            const std::string flySnd = (pSpec && !pSpec->idleSound.empty())
                ? pSpec->idleSound
                : ((strike || kind == ss2::assets::AirMissionKind::Paratrooper) ? std::string("fly_heavy")
                                                                                : std::string("fly_small"));
            (void)audio->playGroup(flySnd, 0.45f, ss2::VoicePriority::Normal);
            const std::string nation = inferNation(planeId.empty() ? "ju-87" : planeId);
            const std::string pfx = nation == "russian" ? "r_" : nation == "british" ? "b_"
                                  : nation == "american" ? "a_" : nation == "japanese" ? "j_" : "d_";
            if (!audio->playGroup(pfx + "plane_ack", 0.60f, ss2::VoicePriority::High) &&
                !audio->playGroup(pfx + "pilot_ack", 0.60f, ss2::VoicePriority::High)) {
                const char* airFallback = (nation == "american" || nation == "british") ? "b_plane_ack"
                    : (nation == "russian" ? "r_plane_ack" : "d_plane_ack");
                (void)audio->playGroup(airFallback, 0.60f, ss2::VoicePriority::High);
            }
        }
        showNotice(ss2::trf("{0} dispatched!", {ss2::tr(airKindLabel(kind))}),
                   ss2::NoticeKind::accepted);
    } else {
        orderNotice.show(ss2::tr("Air support sortie unavailable."), ss2::NoticeKind::blocked);
    }
}

void GameSession::dispatchPatrolAt(int simX, int simY) {
    if (!simulation || !selection) return;
    const auto& selInf = selection->selected();
    const auto& selVeh = selection->selectedVehicles();
    const std::vector<size_t> infIds(selInf.begin(), selInf.end());
    const std::vector<size_t> vehIds(selVeh.begin(), selVeh.end());
    size_t patrolled = 0;
    if (!infIds.empty()) {
        if (const auto res = simulation->patrol(infIds, simX, simY)) patrolled += *res;
    }
    if (!vehIds.empty()) {
        if (const auto res = simulation->patrolVehicles(vehIds, simX, simY)) patrolled += *res;
    }
    if (patrolled > 0) {
        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
        activeMarkers.push_back({
            {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
             ss2::MarkerKind::move},
            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
            std::nullopt,
            true
        });
        syncMarkers();
        pushMinimapPing(simX, simY, false);
        playSelectionVoice("move");
        showNotice(ss2::tr("Patrol route set."), ss2::NoticeKind::accepted);
    } else {
        orderNotice.show(ss2::tr("Selected units cannot patrol there."), ss2::NoticeKind::blocked);
    }
}

} // namespace ss2
