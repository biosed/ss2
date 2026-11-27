#include "session/game_session.hpp"
#include "ui/cursor_bank.hpp"
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
    const auto slots = simulation->airSupportFor(localOwner());
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
    smokeArmed = false;
    armedAirSupport = kind;
    if (audio) {
        const std::string& planeId = slots[idx].planeId;
        const std::string nation = inferNation(planeId.empty() ? "ju-87" : planeId);
        const std::string airSelect = audio->bank().resolveUnitVoice(nation, "plane", "select");
        if (!airSelect.empty()) {
            (void)audio->playGroup(airSelect, 0.56f, ss2::VoicePriority::Normal);
        }
    }
    const TacticalCursorKind airCur =
        kind == ss2::assets::AirMissionKind::Bomber ? TacticalCursorKind::AirBomb :
        kind == ss2::assets::AirMissionKind::Recon ? TacticalCursorKind::AirSpy :
        kind == ss2::assets::AirMissionKind::Paratrooper ? TacticalCursorKind::AirPara :
        TacticalCursorKind::AirFly;
    applyTacticalCursor(window, airCur, moveCursor.get(), attackCursor.get());
    orderNotice.show(ss2::trf("{0} ({1} ready): left-click target area.",
                              {ss2::tr(airKindLabel(kind)), std::to_string(slots[idx].ready)}),
                     ss2::NoticeKind::accepted);
}

void GameSession::syncActiveSubgroup() {
    if (activeSubgroupLabel.empty()) return;
    const auto classes = selectionClassOrder();
    if (classes.size() < 2u || std::ranges::find(classes, activeSubgroupLabel) == classes.end()) {
        activeSubgroupLabel.clear();
    }
}

std::vector<std::string> GameSession::selectionClassOrder() const {
    std::set<std::string> names;
    if (scene && selection) {
        const auto& units = scene->units();
        for (const size_t sid : selection->selected()) {
            if (sid < units.size() && units[sid].hp > 0) {
                names.insert(hudDisplayName(units[sid].id));
            }
        }
    }
    if (vehicleScene && selection) {
        const auto& vehs = vehicleScene->vehicles();
        for (const size_t vid : selection->selectedVehicles()) {
            if (vid < vehs.size() && vehs[vid].alive && vehs[vid].hp > 0) {
                names.insert(hudDisplayName(vehs[vid].id));
            }
        }
    }
    return {names.begin(), names.end()};
}

std::vector<std::size_t> GameSession::activeInfantrySelection() const {
    if (!selection) return {};
    const auto& sel = selection->selected();
    if (activeSubgroupLabel.empty() || !scene) return {sel.begin(), sel.end()};
    std::vector<std::size_t> out;
    const auto& units = scene->units();
    for (const size_t sid : sel) {
        if (sid < units.size() && hudDisplayName(units[sid].id) == activeSubgroupLabel) {
            out.push_back(sid);
        }
    }
    return out.empty() && selection->selectedVehicles().empty() ? std::vector<std::size_t>(sel.begin(), sel.end()) : out;
}

std::vector<std::size_t> GameSession::activeVehicleSelection() const {
    if (!selection) return {};
    const auto& vsel = selection->selectedVehicles();
    if (activeSubgroupLabel.empty() || !vehicleScene) return {vsel.begin(), vsel.end()};
    std::vector<std::size_t> out;
    const auto& vehs = vehicleScene->vehicles();
    for (const size_t vid : vsel) {
        if (vid < vehs.size() && hudDisplayName(vehs[vid].id) == activeSubgroupLabel) {
            out.push_back(vid);
        }
    }
    return out.empty() && selection->selected().empty() ? std::vector<std::size_t>(vsel.begin(), vsel.end()) : out;
}

void GameSession::doTogglePause() {
    if (!simulation) return;
    paused = simulation->togglePause();
    if (paused) {
        if (audio) audio->stopAllLoops();
        showPause();
    } else {
        orderNotice.show(ss2::tr("Resumed."), ss2::NoticeKind::accepted);
    }
    std::cout << (paused ? "Mission clock paused" : "Mission clock resumed") << '\n';
}

void GameSession::stepGameSpeed(int delta) {
    if (!simulation || delta == 0) return;
    static constexpr std::array<int, 4> kSpeeds{{50, 100, 200, 300}};
    const int cur = simulation->speedPercent();
    std::size_t idx = 1;
    for (std::size_t i = 0; i < kSpeeds.size(); ++i) {
        if (kSpeeds[i] == cur) { idx = i; break; }
    }
    if (delta < 0 && idx > 0) --idx;
    else if (delta > 0 && idx + 1 < kSpeeds.size()) ++idx;
    const int next = kSpeeds[idx];
    simulation->setSpeedPercent(next);
    settings.speedPercent = next;
    persistNow();
    const std::string spStr = next == 50 ? "0.5x" : next == 200 ? "2x" : next == 300 ? "3x" : "1x";
    orderNotice.show(ss2::trf("Game speed: {0}", {spStr}), ss2::NoticeKind::accepted);
}

void GameSession::pushOrderPulse(int dstSimX, int dstSimY, OrderPulse::Kind kind) {
    int srcX = 0, srcY = 0;
    bool hasSrc = false;
    if (selection) {
        if (const auto ctr = selection->selectionCentroid()) {
            const int s = std::max(1, mapScale);
            srcX = ctr->first / s;
            srcY = ctr->second / s;
            hasSrc = true;
        }
    }
    if (orderPulses.size() >= 12u) orderPulses.erase(orderPulses.begin());
    orderPulses.push_back({srcX, srcY, hasSrc, dstSimX, dstSimY, kind, std::chrono::steady_clock::now()});
}

void GameSession::doStop() {
    if (!simulation || !selection) return;
    syncActiveSubgroup();
    const auto ids = activeInfantrySelection();
    const auto vids = activeVehicleSelection();
    if (ids.empty() && vids.empty()) return;
    size_t nInf = 0, nVeh = 0;
    if (!ids.empty()) {
        nInf = simulation->stop(ids);
    }
    if (!vids.empty()) {
        nVeh = simulation->stopVehicles(vids);
    }
    if (const size_t n = nInf + nVeh; n > 0) {
        playSelectionVoice("stop");
        attackMoveArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        patrolArmed = false;
        faceArmed = false;
        smokeArmed = false;
        armedAirSupport.reset();
        resetCursor();
        const char* msgKey = (nVeh == 0) ? "Stop order ({0} soldiers)."
                           : (nInf == 0) ? "Stop order ({0} vehicles)."
                                         : "Stop order ({0} units).";
        showNotice(ss2::trf(msgKey, {std::to_string(n)}), ss2::NoticeKind::accepted);
        std::cout << "Tutorial I tactical S (stop): " << n << " units\n";
    }
}

void GameSession::doToggleHoldFire() {
    if (!simulation || !selection) return;
    syncActiveSubgroup();
    const auto selIds = activeInfantrySelection();
    const auto selVids = activeVehicleSelection();
    if (selIds.empty() && selVids.empty()) {
        if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
            const auto& sb = liveBuildings[*selectedBuilding];
            if (!sb.destroyed && sb.owner == localOwner() && sb.occupantCount > 0) {
                if (const auto hold = simulation->toggleBuildingHoldFire(*selectedBuilding)) {
                    for (const size_t sid : sb.occupantIds) {
                        if (*hold) holdFireUnits.insert(sid);
                        else holdFireUnits.erase(sid);
                    }
                    playSelectionVoice("hold");
                    showNotice(ss2::tr(*hold ? "Garrison hold fire enabled."
                                             : "Garrison fire at will enabled."),
                               ss2::NoticeKind::accepted);
                }
            }
        }
        return;
    }
    std::optional<bool> holdState;
    if (!selIds.empty()) {
        if (const auto hold = simulation->toggleHoldFire(selIds)) {
            holdState = *hold;
            for (const auto id : selIds) {
                if (*hold) holdFireUnits.insert(id);
                else holdFireUnits.erase(id);
            }
        }
    }
    if (!selVids.empty()) {
        if (const auto vhold = simulation->toggleVehicleHoldFire(selVids)) {
            if (!holdState) holdState = *vhold;
            for (const auto id : selVids) {
                if (*vhold) vehicleHoldFireUnits.insert(id);
                else vehicleHoldFireUnits.erase(id);
            }
        }
    }
    if (holdState) {
        playSelectionVoice("hold");
        bool logisticsOnly = selIds.empty() && !selVids.empty() && vehicleScene && unitCatalog;
        if (logisticsOnly) {
            for (const size_t vid : selVids) {
                if (vid >= vehicleScene->vehicles().size()) { logisticsOnly = false; break; }
                const auto* spec = unitCatalog->find(vehicleScene->vehicles()[vid].id);
                if (!spec || (!spec->isSupplyTruck() && !spec->isAmbulance())) { logisticsOnly = false; break; }
            }
        }
        if (logisticsOnly) {
            showNotice(ss2::tr(*holdState ? "Depot mode: Holding position (services nearby units safely)."
                                          : "Logistics auto-roam enabled."),
                       ss2::NoticeKind::accepted);
        } else {
            showNotice(ss2::tr(*holdState ? "Hold fire enabled." : "Fire at will enabled."), ss2::NoticeKind::accepted);
        }
        std::cout << "Tutorial I tactical H (hold-fire=" << (*holdState ? "on" : "off") << "): "
                  << (selIds.size() + selVids.size()) << " units\n";
    }
}

void GameSession::doToggleCrouch() {
    if (!simulation || !selection || selection->selected().empty()) return;
    syncActiveSubgroup();
    const auto ids = activeInfantrySelection();
    if (ids.empty()) return;
    if (const auto crouched = simulation->toggleCrouch(ids)) {
        for (const auto id : ids) {
            if (*crouched) crouchedUnits.insert(id);
            else {
                crouchedUnits.erase(id);
                camouflagedUnits.erase(id);
            }
        }
        syncGpuSprites();
        playSelectionVoice(*crouched ? "crouch" : "stand");
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
            if (!sb.destroyed && sb.owner == localOwner() && sb.occupantCount > 0) {
                if (const auto active = simulation->toggleBuildingBinoculars(*selectedBuilding)) {
                    std::optional<size_t> binoSid;
                    for (const size_t sid : sb.occupantIds) {
                        const bool hasBino = scene && sid < scene->units().size() &&
                                             soldierHasBinoculars(scene->units()[sid].id);
                        if (hasBino) {
                            if (!binoSid) binoSid = sid;
                            if (*active) binocularUnits.insert(sid);
                            else binocularUnits.erase(sid);
                        }
                    }
                    if (binoSid && scene && *binoSid < scene->units().size()) {
                        const bool isVet = (*binoSid < unitExpa.size() && unitExpa[*binoSid] >= 16384);
                        playUnitVoiceForId("binoculars", scene->units()[*binoSid].id, false, false, isVet);
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
    syncActiveSubgroup();
    const auto ids = activeInfantrySelection();
    if (ids.empty()) return;
    if (const auto active = simulation->toggleBinoculars(ids)) {
        std::optional<size_t> binoSid;
        for (const auto id : ids) {
            if (scene && id < scene->units().size() && !soldierHasBinoculars(scene->units()[id].id)) continue;
            if (!binoSid) binoSid = id;
            if (*active) binocularUnits.insert(id);
            else binocularUnits.erase(id);
        }
        if (binoSid && scene && *binoSid < scene->units().size()) {
            const bool isVet = (*binoSid < unitExpa.size() && unitExpa[*binoSid] >= 16384);
            playUnitVoiceForId("binoculars", scene->units()[*binoSid].id, false, false, isVet);
        } else {
            playSelectionVoice("binoculars");
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
    smokeArmed = false;
    armedAirSupport.reset();
    attackMoveArmed = !attackMoveArmed;
    if (attackMoveArmed) {
        applyTacticalCursor(window, TacticalCursorKind::AttackMove, moveCursor.get(), attackCursor.get());
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
    smokeArmed = false;
    armedAirSupport.reset();
    patrolArmed = !patrolArmed;
    if (patrolArmed) {
        applyTacticalCursor(window, TacticalCursorKind::AttackMove, moveCursor.get(), attackCursor.get());
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
    smokeArmed = false;
    armedAirSupport.reset();
    attackGroundArmed = !attackGroundArmed;
    if (attackGroundArmed) {
        bool anyRocket = false;
        bool anyHowitzer = hasBombardInf;
        if (unitCatalog && vehicleScene) {
            for (const size_t vid : selection->selectedVehicles()) {
                if (vid >= vehicleScene->vehicles().size()) continue;
                if (const auto* spec = unitCatalog->find(vehicleScene->vehicles()[vid].id)) {
                    if (spec->isRocketArtillery()) anyRocket = true;
                    else if (spec->isArtillery()) anyHowitzer = true;
                }
            }
        }
        const TacticalCursorKind gndCur = anyRocket ? TacticalCursorKind::KatyushaFire
                                        : anyHowitzer ? TacticalCursorKind::HowitzerFire
                                                      : TacticalCursorKind::AttackGround;
        applyTacticalCursor(window, gndCur, moveCursor.get(), attackCursor.get());
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

std::optional<std::size_t> GameSession::bridgeAtMap(int simX, int simY) const {
    std::optional<std::size_t> best;
    long long bestDist2 = std::numeric_limits<long long>::max();
    for (std::size_t i = 0; i < liveBridges.size(); ++i) {
        const auto& br = liveBridges[i];
        if (simX >= br.left && simX < br.left + br.width && simY >= br.top && simY < br.top + br.height) {
            const long long dx = br.x - simX;
            const long long dy = br.y - simY;
            const long long d2 = dx * dx + dy * dy;
            if (!best || d2 < bestDist2) {
                bestDist2 = d2;
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
        if (!lb.destroyed && lb.owner == localOwner() && lb.occupantCount > 0) {
            std::string evacUnitId;
            bool evacVet = false;
            if (scene && !lb.occupantIds.empty() && lb.occupantIds.front() < scene->units().size()) {
                const size_t sid = lb.occupantIds.front();
                evacUnitId = scene->units()[sid].id;
                evacVet = (sid < unitExpa.size() && unitExpa[sid] >= 16384);
            }
            const size_t evac = simulation->evacuateBuilding(*selectedBuilding);
            if (evac > 0) {
                pushOrderPulse(lb.x, lb.y, OrderPulse::Kind::Support);
                selectedBuilding.reset();
                if (!evacUnitId.empty()) playUnitVoiceForId("unload", evacUnitId, false, false, evacVet);
                else playSelectionVoice("unload");
                showNotice(ss2::trf("Evacuated {0} soldiers from building.", {std::to_string(evac)}),
                           ss2::NoticeKind::accepted);
                handledU = true;
            }
        }
    }
    if (!handledU && !selection->selectedVehicles().empty()) {
        const auto& vsel = selection->selectedVehicles();
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0) {
            std::vector<size_t> towIds, cargoIds;
            for (const size_t id : vids) {
                if (id < vehicleTowed.size() && vehicleTowed[id] >= 0) towIds.push_back(id);
                else if (id < vehicleCargo.size() && vehicleCargo[id].first > 0) cargoIds.push_back(id);
            }
            size_t queued = 0;
            if (!towIds.empty()) queued += simulation->queueVehicleOrder(towIds, ss2::QueuedVehicleOp::Unhitch, 0, 0);
            if (!cargoIds.empty()) queued += simulation->queueVehicleOrder(cargoIds, ss2::QueuedVehicleOp::Unload, 0, 0);
            if (queued > 0) {
                playSelectionVoice("unload");
                showNotice(ss2::trf("Queued unload/unhitch ({0} vehicles).", {std::to_string(queued)}),
                           ss2::NoticeKind::accepted);
                handledU = true;
            }
        } else {
            const size_t unhitched = simulation->unhitchVehicles(vids);
            if (unhitched > 0) {
                if (vehicleScene && !vids.empty() && vids.front() < vehicleScene->vehicles().size()) {
                    const auto& v = vehicleScene->vehicles()[vids.front()];
                    pushOrderPulse(v.x, v.y, OrderPulse::Kind::Support);
                }
                playSelectionVoice("unload");
                showNotice(ss2::trf("Unhitched {0} field guns.", {std::to_string(unhitched)}),
                           ss2::NoticeKind::accepted);
                handledU = true;
            } else {
                const size_t unloaded = simulation->unloadVehicles(vids);
                if (unloaded > 0) {
                    if (vehicleScene && !vids.empty() && vids.front() < vehicleScene->vehicles().size()) {
                        const auto& v = vehicleScene->vehicles()[vids.front()];
                        pushOrderPulse(v.x, v.y, OrderPulse::Kind::Support);
                    }
                    playSelectionVoice("unload");
                    showNotice(ss2::trf("Unloaded {0} soldiers.", {std::to_string(unloaded)}), ss2::NoticeKind::accepted);
                    handledU = true;
                }
            }
        }
    }
    if (!handledU && !liveBuildings.empty()) {
        float mx{}, my{};
        SDL_GetMouseState(&mx, &my);
        const auto [mapX, mapY] = renderer.mapPosition(mx, my);
        const auto hoveredBld = buildingAtMap(int(std::floor(mapX / mapScale)), int(std::floor(mapY / mapScale)));
        if (hoveredBld && liveBuildings[*hoveredBld].owner == localOwner() && liveBuildings[*hoveredBld].occupantCount > 0) {
            const auto& hb = liveBuildings[*hoveredBld];
            std::string evacUnitId;
            bool evacVet = false;
            if (scene && !hb.occupantIds.empty() && hb.occupantIds.front() < scene->units().size()) {
                const size_t sid = hb.occupantIds.front();
                evacUnitId = scene->units()[sid].id;
                evacVet = (sid < unitExpa.size() && unitExpa[sid] >= 16384);
            }
            const size_t evac = simulation->evacuateBuilding(*hoveredBld);
            if (evac > 0) {
                pushOrderPulse(hb.x, hb.y, OrderPulse::Kind::Support);
                if (selectedBuilding && *selectedBuilding == *hoveredBld) selectedBuilding.reset();
                if (!evacUnitId.empty()) playUnitVoiceForId("unload", evacUnitId, false, false, evacVet);
                else playSelectionVoice("unload");
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
    syncActiveSubgroup();
    const auto ids = activeInfantrySelection();
    if (ids.empty()) return;
    const bool shiftQueue = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    const size_t laid = shiftQueue
        ? simulation->queueSoldierOrder(ids, ss2::QueuedSoldierOp::LayMine, 0, 0)
        : simulation->layMine(ids);
    if (laid > 0) {
        liveVisibleMines = simulation->visibleMines();
        if (const auto ctr = selection->selectionCentroid()) {
            const int s = std::max(1, mapScale);
            pushOrderPulse(ctr->first / s, ctr->second / s, OrderPulse::Kind::Support);
        }
        playSelectionVoice("laymine");
        if (shiftQueue) showNotice(ss2::tr("Queued mine-laying order."), ss2::NoticeKind::accepted);
    } else {
        orderNotice.show(ss2::tr("Select a Sapper with AT mines to lay mines here."), ss2::NoticeKind::unsupported);
    }
}

void GameSession::doScatter() {
    if (!simulation || !selection || selection->selected().empty()) return;
    syncActiveSubgroup();
    const auto ids = activeInfantrySelection();
    if (ids.empty()) return;
    const size_t n = simulation->scatter(ids);
    if (n > 0) {
        attackMoveArmed = false;
        patrolArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        resetCursor();
        if (const auto ctr = selection->selectionCentroid()) {
            const int s = std::max(1, mapScale);
            pushOrderPulse(ctr->first / s, ctr->second / s, OrderPulse::Kind::Support);
        }
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
    if (multiplayer.enabled) {
        (void)simulation->abandonVehicles(vids);
        playSelectionVoice("unload");
        showNotice(ss2::tr("Abandon order queued."), ss2::NoticeKind::accepted);
        return;
    }
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
    spawnAbandonedCrew(spawns);
    playUnitVoiceForId("unload", spawns.front().id, false);
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
        pushOrderPulse(simX, simY, isAttackMove ? OrderPulse::Kind::Attack : OrderPulse::Kind::Move);
        playSelectionVoice(isAttackMove ? "attack_move" : "move");
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
    const auto slots = simulation->airSupportFor(localOwner());
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
        pushOrderPulse(simX, simY, strike ? OrderPulse::Kind::Attack : OrderPulse::Kind::Move);
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
            const std::string_view airVerb =
                kind == ss2::assets::AirMissionKind::Bomber ? "attack_ground"
                : kind == ss2::assets::AirMissionKind::Fighter ? "attack_unit"
                : (kind == ss2::assets::AirMissionKind::Paratrooper ||
                   kind == ss2::assets::AirMissionKind::Supply) ? "unload"
                : "move";
            const std::string airVoice = audio->bank().resolveUnitVoice(nation, "plane", airVerb);
            if (airVoice.empty() || !audio->playGroup(airVoice, 0.60f, ss2::VoicePriority::High)) {
                const char* airFallback = (nation == "american" || nation == "british" || nation == "french")
                    ? "b_plane_ack"
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
        pushOrderPulse(simX, simY, OrderPulse::Kind::Support);
        pushMinimapPing(simX, simY, false);
        playSelectionVoice("move");
        showNotice(ss2::tr("Patrol route set."), ss2::NoticeKind::accepted);
    } else {
        orderNotice.show(ss2::tr("Selected units cannot patrol there."), ss2::NoticeKind::blocked);
    }
}

} // namespace ss2
