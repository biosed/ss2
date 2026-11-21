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

void GameSession::pollInputEvents() {
    SDL_Event event;
    while (running && SDL_PollEvent(&event)) {
        handleInputEvent(event);
    }
}

void GameSession::handleInputEvent(const SDL_Event& event) {
    tooltip.handle(event);
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        running = false;
        return;
    }
    if (replayFile) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            running = false;
            return;
        }
        renderer.handleEvent(event);
        return;
    }
    if (hud && (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE || event.type == SDL_EVENT_WINDOW_FOCUS_LOST)) {
        hud->handle(event);
    }
    if (menu.active()) {
        const auto mr = menu.handle(event);
        if (audio && (!mr.activated.empty() || mr.closed)) {
            audio->playGroup("accept", 0.42f);
        }
        if (mr.closed || mr.activated == "resume") {
            closeMenu();
        } else if (mr.activated == "airsupport" && simulation) {
            openAirSupportMenu();
        } else if (mr.activated.rfind("aircall:", 0) == 0 && simulation) {
            const std::string_view idxStr(mr.activated.data() + 8, mr.activated.size() - 8);
            std::size_t idx = 0;
            const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
            if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < 5u) {
                closeMenu();
                armAirSupportKind(static_cast<ss2::assets::AirMissionKind>(idx));
            }
        } else if (mr.activated == "quicksave" && simulation) {
            doQuickSave();
            closeMenu();
        } else if (mr.activated == "quickload" && simulation) {
            doQuickLoad();
            closeMenu();
        } else if (mr.activated == "saveload" && simulation) {
            openSaveLoadMenu();
        } else if ((mr.activated.rfind("save:", 0) == 0 || mr.activated.rfind("load:", 0) == 0) && simulation) {
            const bool isLoad = (mr.activated[0] == 'l');
            const std::string_view slotStr(mr.activated.data() + 5, mr.activated.size() - 5);
            int slot = -1;
            const auto [end, ec] = std::from_chars(slotStr.data(), slotStr.data() + slotStr.size(), slot);
            if (ec == std::errc{} && end == slotStr.data() + slotStr.size() &&
                slot >= (isLoad ? -1 : 0) && slot <= 3) {
                if (isLoad) doLoadSlot(slot);
                else doSaveSlot(slot);
                closeMenu();
            }
        } else if (mr.activated == "effects") {
            combatEffects = !combatEffects;
            settings.combatEffects = combatEffects;
            persistNow();
            if (!combatEffects) {
                activeVfx.clear();   // the next frame sends an empty list
                burningWrecks.clear();
            }
            std::cout << "Tutorial I combat effects " << (combatEffects ? "on" : "off") << '\n';
            menu.setItems(buildPauseItems());
        } else if (mr.activated == "speed" && simulation) {
            const int cur = simulation->speedPercent();
            const int next = cur == 50 ? 100 : cur == 100 ? 200 : cur == 200 ? 300 : 50;
            simulation->setSpeedPercent(next);
            settings.speedPercent = next;
            persistNow();
            menu.setItems(buildPauseItems());
        } else if (mr.activated == "difficulty") {
            settings.difficulty = settings.difficulty == 1 ? 2 : settings.difficulty == 2 ? 3 : 1;
            persistNow();
            const char* diffKey = settings.difficulty == 1 ? "Easy" : settings.difficulty == 3 ? "Hard" : "Normal";
            menu.setItems(buildPauseItems());
            orderNotice.show(ss2::trf("Difficulty set to {0} (applies on restart or next mission).",
                                      {ss2::tr(diffKey)}),
                             ss2::NoticeKind::accepted);
        } else if (mr.activated == "language") {
            const auto current = ss2::language();
            const auto nextLang = current == ss2::Language::english ? ss2::Language::french
                                : current == ss2::Language::french ? ss2::Language::spanish : ss2::Language::english;
            ss2::setLanguage(nextLang);
            settings.language = nextLang;
            persistNow();
            menu.open(ss2::tr("Paused"), buildPauseItems());
            for (int step = 0; step < 16 && menu.highlighted() != "language"; ++step) {
                SDL_Event down{};
                down.type = SDL_EVENT_KEY_DOWN;
                down.key.key = SDLK_DOWN;
                menu.handle(down);
            }
        } else if (mr.activated == "smooth") {
            smoothTerrain = !smoothTerrain;
            settings.smoothTerrain = smoothTerrain;
            persistNow();
            renderer.setMapFilterLinear(smoothTerrain);
            menu.setItems(buildPauseItems());
        } else if (mr.activated == "music" && audio) {
            settings.musicMuted = audio->toggleMusicMute();
            persistNow();
            menu.setItems(buildPauseItems());
        } else if (mr.activated == "sfx_volume" && audio) {
            const float cur = audio->sfxVolume();
            const float next = cur > 0.85f ? 0.75f : cur > 0.60f ? 0.50f : cur > 0.35f ? 0.25f : cur > 0.05f ? 0.0f : 1.0f;
            audio->setSfxVolume(next);
            settings.sfxVolume = next;
            persistNow();
            if (next > 0.0f) audio->playGroup("accept", 0.5f);
            menu.setItems(buildPauseItems());
        } else if (mr.activated == "music_volume" && audio) {
            const float cur = audio->musicVolume();
            const float next = cur >= 0.34f ? 0.28f : cur >= 0.23f ? 0.18f : cur >= 0.13f ? 0.09f : 0.40f;
            audio->setMusicVolume(next);
            settings.musicVolume = next;
            persistNow();
            menu.setItems(buildPauseItems());
        } else if (mr.activated == "next_track" && audio) {
            audio->nextMusicTrack();
            logHud(ss2::tr("Switched to next soundtrack track."));
        } else if (mr.activated == "next_campaign" && nextCampaignMissionPath && nextMissionOut) {
            *nextMissionOut = *nextCampaignMissionPath;
            menu.close();
            running = false;
        } else if (mr.activated == "replay" && !currentMissionPath.empty() && nextMissionOut) {
            *nextMissionOut = currentMissionPath;
            menu.close();
            running = false;
        } else if (mr.activated == "campaigns") {
            openCampaignsMenu();
        } else if (mr.activated.rfind("campaign:", 0) == 0) {
            const std::string_view idxStr(mr.activated.data() + 9, mr.activated.size() - 9);
            std::size_t idx = 0;
            const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
            if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < menuCampaigns.size()) {
                openCampaignMissionsMenu(idx);
            }
        } else if (mr.activated.rfind("cmission:", 0) == 0 && nextMissionOut) {
            const std::string_view rest(mr.activated.data() + 9, mr.activated.size() - 9);
            const auto sep = rest.find(':');
            if (sep != std::string_view::npos) {
                std::size_t cIdx = 0, mIdx = 0;
                const auto [e1, ec1] = std::from_chars(rest.data(), rest.data() + sep, cIdx);
                const auto [e2, ec2] = std::from_chars(rest.data() + sep + 1, rest.data() + rest.size(), mIdx);
                if (ec1 == std::errc{} && e1 == rest.data() + sep &&
                    ec2 == std::errc{} && e2 == rest.data() + rest.size() &&
                    cIdx < menuCampaigns.size() && mIdx < menuCampaigns[cIdx].missions.size()) {
                    *nextMissionOut = menuCampaigns[cIdx].missions[mIdx].path;
                    menu.close();
                    running = false;
                }
            }
        } else if (mr.activated == "back_campaigns") {
            openCampaignsMenu();
        } else if (mr.activated == "missions") {
            openMissionsMenu();
        } else if (mr.activated.rfind("mission:", 0) == 0 && nextMissionOut) {
            const std::string_view idxStr(mr.activated.data() + 8, mr.activated.size() - 8);
            std::size_t idx = 0;
            const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
            if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < menuMissions.size()) {
                *nextMissionOut = menuMissions[idx].path;
                menu.close();
                running = false;
            }
        } else if (mr.activated == "forces") {
            openForceOverviewMenu(true);
        } else if (mr.activated == "briefing") {
            openMissionBriefing();
        } else if (mr.activated.rfind("force_select:", 0) == 0 && selection) {
            const std::string_view idxStr(mr.activated.data() + 13, mr.activated.size() - 13);
            std::size_t idx = 0;
            const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
            if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < forceRosterGroups.size()) {
                const auto& grp = forceRosterGroups[idx];
                std::set<size_t> infSet(grp.infIds.begin(), grp.infIds.end());
                std::set<size_t> vehSet(grp.vehIds.begin(), grp.vehIds.end());
                closeMenu();
                selection->setSelection(std::move(infSet), std::move(vehSet));
                selectedBuilding.reset();
                prevSelInf = selection->selected();
                prevSelVeh = selection->selectedVehicles();
                if (const auto ctr = selection->selectionCentroid()) {
                    renderer.focusMap(ctr->first, ctr->second, true);
                }
                const std::size_t selTotal = selection->selected().size() + selection->selectedVehicles().size();
                showNotice(ss2::trf("Selected {0} ({1} units).",
                                    {ss2::tr(grp.displayName), std::to_string(selTotal)}),
                           ss2::NoticeKind::accepted);
            }
        } else if (mr.activated.rfind("force_bldg:", 0) == 0 && selection) {
            const std::string_view idxStr(mr.activated.data() + 11, mr.activated.size() - 11);
            std::size_t bIdx = 0;
            const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), bIdx);
            if (ec == std::errc{} && end == idxStr.data() + idxStr.size() &&
                bIdx < liveBuildings.size() && !liveBuildings[bIdx].destroyed) {
                const auto& lb = liveBuildings[bIdx];
                closeMenu();
                selection->setSelection({}, {});
                prevSelInf.clear();
                prevSelVeh.clear();
                selectedBuilding = bIdx;
                renderer.focusMap(lb.x * mapScale, lb.y * mapScale, true);
                showNotice(ss2::trf("Garrisoned building ({0}/{1}) — press U to evacuate.",
                                    {std::to_string(lb.occupantCount), std::to_string(lb.capacity)}),
                           ss2::NoticeKind::accepted);
            }
        } else if (mr.activated == "controls") {
            openControlsMenu();
        } else if (mr.activated == "log") {
            openLogMenu();
        } else if (mr.activated == "back") {
            menu.open(ss2::tr("Paused"), buildPauseItems());
        } else if (mr.activated == "quit") {
            running = false;
        }
        if (mr.consumed) return;
    }
    if (hud && !briefing.active()) {
        const auto hr = hud->handle(event);
        if (hr.action == ss2::HudAction::moveCamera && hudMapW && hudMapH) {
            if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                (armedAirSupport || attackMoveArmed || patrolArmed)) {
                hud->cancelMinimapDrag();
                const int simX = std::clamp(static_cast<int>(std::lround(hr.u * static_cast<float>(hudMapW))),
                                            0, static_cast<int>(hudMapW) - 1);
                const int simY = std::clamp(static_cast<int>(std::lround(hr.v * static_cast<float>(hudMapH))),
                                            0, static_cast<int>(hudMapH) - 1);
                if (armedAirSupport) {
                    dispatchAirSupportAt(*armedAirSupport, simX, simY);
                    armedAirSupport.reset();
                } else if (patrolArmed) {
                    dispatchPatrolAt(simX, simY);
                    patrolArmed = false;
                } else if (attackMoveArmed) {
                    const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                    if (issueGroundOrder(simX, simY, true, append)) {
                        pushMinimapPing(simX, simY, true);
                    }
                    attackMoveArmed = false;
                }
                resetCursor();
            } else {
                renderer.focusMap(hr.u * static_cast<double>(hudMapW * mapScale),
                                  hr.v * static_cast<double>(hudMapH * mapScale));
            }
        } else if (hr.action == ss2::HudAction::orderMinimap && simulation && hudMapW && hudMapH) {
            const int simX = std::clamp(static_cast<int>(std::lround(hr.u * static_cast<float>(hudMapW))),
                                        0, static_cast<int>(hudMapW) - 1);
            const int simY = std::clamp(static_cast<int>(std::lround(hr.v * static_cast<float>(hudMapH))),
                                        0, static_cast<int>(hudMapH) - 1);
            if (patrolArmed) {
                dispatchPatrolAt(simX, simY);
                patrolArmed = false;
            } else {
                const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                if (issueGroundOrder(simX, simY, attackMoveArmed, append)) {
                    pushMinimapPing(simX, simY, attackMoveArmed);
                }
                attackMoveArmed = false;
            }
            resetCursor();
        } else if (hr.action == ss2::HudAction::stop) {
            doStop();
        } else if (hr.action == ss2::HudAction::holdFire) {
            doToggleHoldFire();
        } else if (hr.action == ss2::HudAction::crouch) {
            doToggleCrouch();
        } else if (hr.action == ss2::HudAction::recon) {
            doToggleBinoculars();
        } else if (hr.action == ss2::HudAction::attackMove) {
            doArmAttackMove();
        } else if (hr.action == ss2::HudAction::patrol) {
            doArmPatrol();
        } else if (hr.action == ss2::HudAction::attackGround) {
            doArmAttackGround();
        } else if (hr.action == ss2::HudAction::unload) {
            doUnloadOrUnhitch();
        } else if (hr.action == ss2::HudAction::mine) {
            doLayMine();
        } else if (hr.action == ss2::HudAction::scatter) {
            doScatter();
        } else if (hr.action == ss2::HudAction::recallGroup && hr.groupDigit >= 0 && hr.groupDigit < 10 && selection) {
            const auto beforeInf = selection->selected();
            const auto beforeVeh = selection->selectedVehicles();
            selection->recallControlGroup(static_cast<unsigned>(hr.groupDigit), hr.shiftHeld);
            if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                selectedBuilding.reset();
            }
            if (!hr.shiftHeld && selection->selected() == beforeInf && selection->selectedVehicles() == beforeVeh) {
                if (const auto ctr = selection->selectionCentroid()) {
                    renderer.focusMap(ctr->first, ctr->second, true);
                }
            }
        } else if (hr.action == ss2::HudAction::selectClass && !hr.classLabel.empty() && selection && scene) {
            std::set<size_t> newInf, newVeh;
            const auto& units = scene->units();
            for (const auto id : selection->selected()) {
                if (id >= units.size()) continue;
                const bool match = (hudDisplayName(units[id].id) == hr.classLabel);
                if (hr.shiftHeld ? !match : match) newInf.insert(id);
            }
            if (vehicleScene) {
                const auto& vehs = vehicleScene->vehicles();
                for (const auto id : selection->selectedVehicles()) {
                    if (id >= vehs.size()) continue;
                    const bool match = (hudDisplayName(vehs[id].id) == hr.classLabel);
                    if (hr.shiftHeld ? !match : match) newVeh.insert(id);
                }
            }
            selection->setSelection(std::move(newInf), std::move(newVeh));
            if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                selectedBuilding.reset();
            }
        } else if (hr.action == ss2::HudAction::airSupport && hr.airSlot >= 0 && hr.airSlot < 5 && simulation) {
            const auto kind = static_cast<ss2::assets::AirMissionKind>(hr.airSlot);
            if (armedAirSupport && *armedAirSupport == kind) {
                armedAirSupport.reset();
                resetCursor();
                orderNotice.show(ss2::tr("Air support cancelled."), ss2::NoticeKind::unsupported);
            } else {
                armAirSupportKind(kind);
            }
        } else if (hr.action == ss2::HudAction::pause) {
            doTogglePause();
        } else if (hr.action == ss2::HudAction::log) {
            openLogMenu();
        } else if (hr.action == ss2::HudAction::menu) {
            openPauseMenu();
        } else if (hr.action == ss2::HudAction::mission && selection) {
            SDL_Event mKey{};
            mKey.type = SDL_EVENT_KEY_DOWN;
            mKey.key.key = SDLK_M;
            selection->handle(mKey);
            std::cout << "Tutorial I HUD Mission marker jump\n";
        }
        if (hr.consumed) return;
    }
    orderNotice.handle(event);
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        running = false;
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11 && !event.key.repeat) {
        const bool isFullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        if (!SDL_SetWindowFullscreen(window, !isFullscreen)) SDL_Log("Fullscreen switch failed: %s", SDL_GetError());
        return;
    }
    if (briefing.active()) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT)
            std::cout << "Tutorial I right-click received while briefing open\n";
        if (briefing.handle(event)) {
            if (manualBriefingOpen) {
                manualBriefingOpen = false;
                if (!pendingDialogs.empty()) {
                    briefing.open(std::move(pendingDialogs.front()));
                    pendingDialogs.pop_front();
                }
            } else if (pendingDialogs.empty()) {
                simulation->acknowledge();
            } else {
                briefing.open(std::move(pendingDialogs.front()));
                pendingDialogs.pop_front();
            }
        }
        if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
            renderer.handleEvent(event);
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_ESCAPE) {
        if (simulation) openPauseMenu();
        else running = false;
        return;
    }
    if (hud && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_L && !(event.key.mod & SDL_KMOD_CTRL)) {
        openLogMenu();
        return;
    }
    if (simulation && selection && scene && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_O && !(event.key.mod & SDL_KMOD_CTRL)) {
        openForceOverviewMenu(false);
        return;
    }
    if (simulation && !menu.active() && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_J && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
        openMissionBriefing();
        return;
    }
    if (simulation && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        (event.key.key == SDLK_PAUSE || (event.key.key == SDLK_P && (event.key.mod & SDL_KMOD_CTRL)))) {
        doTogglePause();
    }
    if (audio && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_M && (event.key.mod & SDL_KMOD_CTRL)) {
        const bool muted = audio->toggleMusicMute();
        settings.musicMuted = muted;
        persistNow();
        orderNotice.show(ss2::tr(muted ? "Music muted (Ctrl+M)." : "Music unmuted."), ss2::NoticeKind::accepted);
        return;
    }
    if (audio && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_N && !(event.key.mod & SDL_KMOD_CTRL)) {
        if (audio->nextMusicTrack()) orderNotice.show(ss2::tr("Next soundtrack track."), ss2::NoticeKind::accepted);
    }
    if (simulation && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        (event.key.key == SDLK_LEFTBRACKET || event.key.key == SDLK_RIGHTBRACKET) &&
        !(event.key.mod & SDL_KMOD_CTRL)) {
        static constexpr std::array<int, 4> kSpeeds{{50, 100, 200, 300}};
        const int cur = simulation->speedPercent();
        std::size_t idx = 1;
        for (std::size_t i = 0; i < kSpeeds.size(); ++i) {
            if (kSpeeds[i] == cur) { idx = i; break; }
        }
        if (event.key.key == SDLK_LEFTBRACKET && idx > 0) --idx;
        else if (event.key.key == SDLK_RIGHTBRACKET && idx + 1 < kSpeeds.size()) ++idx;
        const int next = kSpeeds[idx];
        simulation->setSpeedPercent(next);
        settings.speedPercent = next;
        persistNow();
        const std::string spStr = next == 50 ? "0.5x" : next == 200 ? "2x" : next == 300 ? "3x" : "1x";
        orderNotice.show(ss2::trf("Game speed: {0}", {spStr}), ss2::NoticeKind::accepted);
        return;
    }
    if (simulation && selection && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_U && !(event.key.mod & SDL_KMOD_CTRL)) {
        doUnloadOrUnhitch();
        return;
    }
    if (simulation && selection && (!selection->selected().empty() || !selection->selectedVehicles().empty()) &&
        event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !(event.key.mod & SDL_KMOD_CTRL)) {
        if (event.key.key == SDLK_S) {
            attackGroundArmed = false;
            reverseMoveArmed = false;
            patrolArmed = false;
            faceArmed = false;
            armedAirSupport.reset();
            doStop();
        } else if (event.key.key == SDLK_A) {
            doArmAttackMove();
            return;
        } else if (event.key.key == SDLK_Q) {
            doArmPatrol();
            return;
        } else if (event.key.key == SDLK_R && !selection->selectedVehicles().empty()) {
            attackMoveArmed = false;
            attackGroundArmed = false;
            patrolArmed = false;
            faceArmed = false;
            armedAirSupport.reset();
            reverseMoveArmed = !reverseMoveArmed;
            if (reverseMoveArmed) {
                SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                orderNotice.show(ss2::tr("Reverse move: click destination (keeps front armor facing away)."),
                                 ss2::NoticeKind::accepted);
            } else {
                resetCursor();
            }
            return;
        } else if (event.key.key == SDLK_T && !selection->selectedVehicles().empty()) {
            attackMoveArmed = false;
            attackGroundArmed = false;
            reverseMoveArmed = false;
            patrolArmed = false;
            armedAirSupport.reset();
            faceArmed = !faceArmed;
            if (faceArmed) {
                SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                orderNotice.show(ss2::tr("Face direction: click map point to rotate front armor."),
                                 ss2::NoticeKind::accepted);
            } else {
                resetCursor();
            }
            return;
        } else if (event.key.key == SDLK_G && !selection->selectedVehicles().empty()) {
            doArmAttackGround();
            return;
        } else if (event.key.key == SDLK_E && !selection->selected().empty()) {
            doLayMine();
            return;
        } else if (event.key.key == SDLK_D && !selection->selected().empty()) {
            doScatter();
            return;
        } else if (event.key.key == SDLK_K && !selection->selectedVehicles().empty()) {
            doAbandonVehicles();
            return;
        } else if (event.key.key == SDLK_X && !selection->selectedVehicles().empty()) {
            const auto& vsel = selection->selectedVehicles();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            if (const auto opened = simulation->toggleMarchMode(vids)) {
                playSelectionVoice("select");
                orderNotice.show(
                    ss2::tr(*opened
                                ? "Commander hatch opened (+sight, exposed to snipers — X to close)."
                                : "Commander hatch closed (buttoned — protected from snipers)."),
                    ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Selected vehicles have no commander hatch."),
                                 ss2::NoticeKind::unsupported);
            }
            return;
        } else if (event.key.key == SDLK_H) doToggleHoldFire();
        else if (event.key.key == SDLK_C && !selection->selected().empty()) doToggleCrouch();
        else if (event.key.key == SDLK_B && (!selection->selected().empty() || selectedBuilding)) doToggleBinoculars();
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (simulation && selection && scene && event.key.key == SDLK_I &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            const auto idle = simulation->idlePlayerUnits();
            if (idle.empty()) {
                orderNotice.show(ss2::tr("All units are busy."), ss2::NoticeKind::unsupported);
            } else {
                idleUnitCursor %= idle.size();
                if (idle.size() > 1u) {
                    const auto& cur = idle[idleUnitCursor];
                    const bool sameInf = !cur.isVehicle && selection->selected().size() == 1u &&
                                         selection->selectedVehicles().empty() &&
                                         selection->selected().contains(cur.index);
                    const bool sameVeh = cur.isVehicle && selection->selected().empty() &&
                                         selection->selectedVehicles().size() == 1u &&
                                         selection->selectedVehicles().contains(cur.index);
                    if (sameInf || sameVeh) {
                        idleUnitCursor = (idleUnitCursor + 1u) % idle.size();
                    }
                }
                const auto picked = idle[idleUnitCursor];
                const std::size_t shownIdx = idleUnitCursor + 1u;
                idleUnitCursor = (idleUnitCursor + 1u) % idle.size();
                std::string unitName;
                if (picked.isVehicle && vehicleScene && picked.index < vehicleScene->vehicles().size()) {
                    selection->setSelection({}, {picked.index});
                    unitName = hudDisplayName(vehicleScene->vehicles()[picked.index].id);
                } else if (!picked.isVehicle && picked.index < scene->units().size()) {
                    selection->setSelection({picked.index}, {});
                    unitName = hudDisplayName(scene->units()[picked.index].id);
                }
                selectedBuilding.reset();
                if (const auto ctr = selection->selectionCentroid()) {
                    renderer.focusMap(ctr->first, ctr->second, true);
                }
                orderNotice.show(ss2::trf("Idle unit {0}/{1}: {2} (I for next)",
                                          {std::to_string(shownIdx), std::to_string(idle.size()),
                                           ss2::tr(unitName)}),
                                 ss2::NoticeKind::accepted);
            }
            return;
        }
        if (simulation && selection && scene && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            event.key.key == SDLK_W && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            struct TriageEntry { bool isVehicle{false}; size_t index{0}; };
            std::vector<TriageEntry> needy;
            const auto& units = scene->units();
            for (size_t idx = 0; idx < units.size(); ++idx) {
                const auto& u = units[idx];
                if (u.owner != 0 || u.hp <= 0 || !u.visible) continue;
                const int maxHp = (idx < initialUnitHp.size() && initialUnitHp[idx] > 0) ? initialUnitHp[idx] : 100;
                const bool dry = (u.ammo <= 0) && isCombatAmmoSoldierId(u.id);
                if (u.hp < maxHp || dry) needy.push_back({false, idx});
            }
            if (vehicleScene) {
                const auto& vehs = vehicleScene->vehicles();
                for (size_t idx = 0; idx < vehs.size(); ++idx) {
                    const auto& v = vehs[idx];
                    if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.visible) continue;
                    const int maxHp = (idx < initialVehicleHp.size() && initialVehicleHp[idx] > 0) ? initialVehicleHp[idx] : 100;
                    const bool dry = (v.ammo <= 0) && isArmedVehicleId(v.id);
                    if (v.hp < maxHp || dry) needy.push_back({true, idx});
                }
            }
            if (needy.empty()) {
                orderNotice.show(ss2::tr("All units are healthy and supplied."), ss2::NoticeKind::accepted);
            } else {
                triageCursor %= needy.size();
                if (needy.size() > 1u) {
                    const auto& cur = needy[triageCursor];
                    const bool sameInf = !cur.isVehicle && selection->selected().size() == 1u &&
                                         selection->selectedVehicles().empty() &&
                                         selection->selected().contains(cur.index);
                    const bool sameVeh = cur.isVehicle && selection->selected().empty() &&
                                         selection->selectedVehicles().size() == 1u &&
                                         selection->selectedVehicles().contains(cur.index);
                    if (sameInf || sameVeh) {
                        triageCursor = (triageCursor + 1u) % needy.size();
                    }
                }
                const auto picked = needy[triageCursor];
                const std::size_t shownIdx = triageCursor + 1u;
                triageCursor = (triageCursor + 1u) % needy.size();
                std::string unitName;
                if (picked.isVehicle && vehicleScene && picked.index < vehicleScene->vehicles().size()) {
                    selection->setSelection({}, {picked.index});
                    unitName = hudDisplayName(vehicleScene->vehicles()[picked.index].id);
                } else if (!picked.isVehicle && picked.index < units.size()) {
                    selection->setSelection({picked.index}, {});
                    unitName = hudDisplayName(units[picked.index].id);
                }
                selectedBuilding.reset();
                prevSelInf = selection->selected();
                prevSelVeh = selection->selectedVehicles();
                if (const auto ctr = selection->selectionCentroid()) {
                    renderer.focusMap(ctr->first, ctr->second, true);
                }
                orderNotice.show(ss2::trf("Needs support {0}/{1}: {2} (W for next)",
                                          {std::to_string(shownIdx), std::to_string(needy.size()),
                                           ss2::tr(unitName)}),
                                 ss2::NoticeKind::unsupported);
            }
            return;
        }
        if (simulation && selection && scene && event.key.key == SDLK_A &&
            (event.key.mod & SDL_KMOD_CTRL) && !(event.key.mod & SDL_KMOD_ALT)) {
            const bool allMap = (event.key.mod & SDL_KMOD_SHIFT) != 0;
            const size_t count = selection->selectAllPlayerUnits(!allMap);
            if (count > 0) {
                selectedBuilding.reset();
                prevSelInf = selection->selected();
                prevSelVeh = selection->selectedVehicles();
                playSelectionVoice("select");
                orderNotice.show(ss2::trf(allMap ? "Selected all {0} units." : "Selected {0} units on screen.",
                                          {std::to_string(count)}),
                                 ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr(allMap ? "No player units available." : "No player units on screen."),
                                 ss2::NoticeKind::unsupported);
            }
            return;
        }
        if (simulation && selection && scene && event.key.key == SDLK_Z &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            const auto idle = simulation->idlePlayerUnits();
            if (idle.empty()) {
                orderNotice.show(ss2::tr("All units are busy."), ss2::NoticeKind::unsupported);
            } else {
                std::set<size_t> infSet, vehSet;
                const size_t maxInf = scene->units().size();
                const size_t maxVeh = vehicleScene ? vehicleScene->vehicles().size() : 0u;
                for (const auto& iu : idle) {
                    if (iu.isVehicle && iu.index < maxVeh) vehSet.insert(iu.index);
                    else if (!iu.isVehicle && iu.index < maxInf) infSet.insert(iu.index);
                }
                selection->setSelection(std::move(infSet), std::move(vehSet));
                const size_t count = selection->selected().size() + selection->selectedVehicles().size();
                if (count > 0) {
                    selectedBuilding.reset();
                    prevSelInf = selection->selected();
                    prevSelVeh = selection->selectedVehicles();
                    playSelectionVoice("select");
                    if (const auto ctr = selection->selectionCentroid()) {
                        renderer.focusMap(ctr->first, ctr->second, true);
                    }
                    orderNotice.show(ss2::trf("Selected {0} idle units.", {std::to_string(count)}),
                                     ss2::NoticeKind::accepted);
                } else {
                    orderNotice.show(ss2::tr("All units are busy."), ss2::NoticeKind::unsupported);
                }
            }
            return;
        }
        if (simulation && event.key.key == SDLK_F && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            const auto slots = simulation->playerAirSupport();
            const std::size_t startIdx = armedAirSupport ? ((static_cast<std::size_t>(*armedAirSupport) + 1u) % 5u) : 0u;
            bool found = false;
            for (std::size_t off = 0; off < 5u; ++off) {
                const std::size_t k = (startIdx + off) % 5u;
                if (slots[k].ready > 0) {
                    armAirSupportKind(static_cast<ss2::assets::AirMissionKind>(k));
                    found = true;
                    break;
                }
            }
            if (!found) {
                orderNotice.show(ss2::tr("No air support sorties available in this mission."),
                                 ss2::NoticeKind::unsupported);
            }
            return;
        }
        if (simulation && (event.key.mod & SDL_KMOD_ALT) && !(event.key.mod & SDL_KMOD_CTRL) &&
            event.key.key >= SDLK_1 && event.key.key <= SDLK_5) {
            const auto idx = static_cast<std::size_t>(event.key.key - SDLK_1);
            armAirSupportKind(static_cast<ss2::assets::AirMissionKind>(idx));
            return;
        }
        if (simulation &&
            ((event.key.key == SDLK_F9 && !(event.key.mod & SDL_KMOD_CTRL)) ||
             (event.key.key == SDLK_S && (event.key.mod & SDL_KMOD_CTRL)))) {
            doQuickSave();
            return;
        }
        if (simulation &&
            ((event.key.key == SDLK_F10 && !(event.key.mod & SDL_KMOD_CTRL)) ||
             (event.key.key == SDLK_L && (event.key.mod & SDL_KMOD_CTRL)))) {
            doQuickLoad();
            return;
        }
        const auto fSlot = [](SDL_Keycode k) -> int {
            switch (k) {
            case SDLK_F1: return 0; case SDLK_F2: return 1; case SDLK_F3: return 2; case SDLK_F4: return 3;
            case SDLK_F5: return 4; case SDLK_F6: return 5; case SDLK_F7: return 6; case SDLK_F8: return 7;
            default: return -1;
            }
        }(event.key.key);
        if (fSlot >= 0) {
            if (event.key.mod & SDL_KMOD_CTRL) {
                int winW = 1, winH = 1;
                SDL_GetWindowSize(window, &winW, &winH);
                cameraBookmarks[static_cast<std::size_t>(fSlot)] =
                    renderer.mapPosition(static_cast<double>(winW) * 0.5, static_cast<double>(winH) * 0.5);
                orderNotice.show(ss2::trf("Camera bookmark F{0} saved.", {std::to_string(fSlot + 1)}),
                                 ss2::NoticeKind::accepted);
            } else if (cameraBookmarks[static_cast<std::size_t>(fSlot)]) {
                const auto [bx, by] = *cameraBookmarks[static_cast<std::size_t>(fSlot)];
                renderer.focusMap(bx, by, true);
            }
            return;
        }
        if (event.key.key == SDLK_SPACE && !(event.key.mod & SDL_KMOD_CTRL) && !eventHistory.empty()) {
            const auto [ex, ey] = eventHistory[eventHistoryCursor % eventHistory.size()];
            renderer.focusMap(ex, ey, true);
            eventHistoryCursor = (eventHistoryCursor + 1u) % eventHistory.size();
            return;
        }
    }
    if (simulation && (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed || armedAirSupport) &&
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        (void)selection->cancelDrag();
        const auto [x, y] = renderer.mapPosition(event.button.x, event.button.y);
        const int simX = int(std::floor(x / mapScale));
        const int simY = int(std::floor(y / mapScale));
        if (armedAirSupport) {
            dispatchAirSupportAt(*armedAirSupport, simX, simY);
            armedAirSupport.reset();
        } else if (reverseMoveArmed) {
            const auto& vsel = selection->selectedVehicles();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            if (const auto res = simulation->reverseMoveVehicles(vids, simX, simY); res && *res > 0) {
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::trf("Reverse move ordered ({0} vehicles).", {std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Selected vehicles cannot reverse."), ss2::NoticeKind::blocked);
            }
            reverseMoveArmed = false;
        } else if (faceArmed) {
            const auto& vsel = selection->selectedVehicles();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            if (const auto res = simulation->faceVehicles(vids, simX, simY); res && *res > 0) {
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::trf("Rotating front armor ({0} vehicles).", {std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Selected vehicles cannot rotate."), ss2::NoticeKind::blocked);
            }
            faceArmed = false;
        } else if (attackGroundArmed) {
            const auto& vsel = selection->selectedVehicles();
            const auto& sel = selection->selected();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            const std::vector<size_t> sids(sel.begin(), sel.end());
            size_t totalBombard = 0;
            if (!vids.empty()) {
                if (const auto res = simulation->attackGroundVehicles(vids, simX, simY)) totalBombard += *res;
            }
            if (!sids.empty()) {
                if (const auto res = simulation->attackGroundSoldiers(sids, simX, simY)) totalBombard += *res;
            }
            if (totalBombard > 0) {
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{(simX - 10) * mapScale, (simY - 10) * mapScale, 20 * mapScale, 20 * mapScale},
                     ss2::MarkerKind::attack},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("attack");
                showNotice(ss2::trf("Attack ground ordered ({0} units).", {std::to_string(totalBombard)}),
                           ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Selected units cannot bombard that location."), ss2::NoticeKind::blocked);
            }
            attackGroundArmed = false;
        } else if (patrolArmed) {
            dispatchPatrolAt(simX, simY);
            patrolArmed = false;
        } else {
            const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            (void)issueGroundOrder(simX, simY, true, append);
            attackMoveArmed = false;
        }
        resetCursor();
        return;
    }
    if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
        if (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed || armedAirSupport) {
            attackMoveArmed = false;
            attackGroundArmed = false;
            reverseMoveArmed = false;
            patrolArmed = false;
            faceArmed = false;
            armedAirSupport.reset();
            resetCursor();
            return;
        }
        // Gold's viewport cancels a left selection box or orders at the press coordinates.
        if (selection->cancelDrag()) return;
        const auto [x,y] = renderer.mapPosition(event.button.x, event.button.y);
        const int simX = int(std::floor(x / mapScale)), simY = int(std::floor(y / mapScale));
        const auto& selected = selection->selected();
        const auto& selectedVeh = selection->selectedVehicles();
        const std::vector<size_t> ids(selected.begin(), selected.end());
        const std::vector<size_t> vids(selectedVeh.begin(), selectedVeh.end());
        const auto targetInf = selection->infantryAt(int(std::floor(x)), int(std::floor(y)));
        const auto targetVeh = selection->vehicleAt(int(std::floor(x)), int(std::floor(y)));
        if (ids.empty() && vids.empty() && selectedBuilding && *selectedBuilding < liveBuildings.size()) {
            const auto& sb = liveBuildings[*selectedBuilding];
            if (!sb.destroyed && sb.owner == 0 && sb.occupantCount > 0) {
                std::optional<size_t> bOrder;
                int mLeft = 0, mTop = 0, mW = 16, mH = 16;
                if (targetVeh && vehicleScene &&
                    isHostileOwner(vehicleScene->vehicles().at(*targetVeh).owner) &&
                    simulation->isValidVehicleAttackTarget(*targetVeh)) {
                    bOrder = simulation->orderBuildingAttack(*selectedBuilding, true, *targetVeh);
                    const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                    mW = tv.width > 0 ? tv.width : 24;
                    mH = tv.height > 0 ? tv.height : 24;
                    mLeft = tv.width > 0 ? tv.left : tv.x - 12;
                    mTop = tv.height > 0 ? tv.top : tv.y - 12;
                } else if (targetInf && isHostileOwner(scene->units().at(*targetInf).owner) &&
                           simulation->isValidAttackTarget(*targetInf)) {
                    bOrder = simulation->orderBuildingAttack(*selectedBuilding, false, *targetInf);
                    const auto& tu = scene->units().at(*targetInf);
                    mW = std::max(12, tu.width);
                    mH = std::max(14, tu.height);
                    mLeft = tu.left;
                    mTop = tu.top;
                } else if (const auto targetBld = buildingAtMap(simX, simY)) {
                    const auto& tb = liveBuildings[*targetBld];
                    if (*targetBld != *selectedBuilding && tb.owner >= 0 && isHostileOwner(tb.owner)) {
                        bOrder = simulation->orderBuildingAttackBuilding(*selectedBuilding, *targetBld);
                        mLeft = tb.left;
                        mTop = tb.top;
                        mW = tb.width;
                        mH = tb.height;
                    }
                }
                if (bOrder && *bOrder > 0) {
                    std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                    activeMarkers.push_back({
                        {{mLeft * mapScale, mTop * mapScale, mW * mapScale, mH * mapScale},
                         ss2::MarkerKind::attack},
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                        std::nullopt,
                        true
                    });
                    syncMarkers();
                    playSelectionVoice("attack");
                    showNotice(ss2::tr("Garrison engaging target!"), ss2::NoticeKind::accepted);
                    return;
                }
            }
        }
        if (!vids.empty() && targetVeh && vehicleScene) {
            std::optional<std::pair<size_t, size_t>> towPair;
            for (size_t sv : vids) {
                if (simulation->canTowVehicle(sv, *targetVeh)) {
                    towPair = {sv, *targetVeh};
                    break;
                }
                if (simulation->canTowVehicle(*targetVeh, sv)) {
                    towPair = {*targetVeh, sv};
                    break;
                }
            }
            if (towPair) {
                const auto [trIdx, gIdx] = *towPair;
                if (const auto res = simulation->towVehicle(trIdx, gIdx); res && *res > 0) {
                    const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                    const int mw = tv.width > 0 ? tv.width : 24;
                    const int mh = tv.height > 0 ? tv.height : 24;
                    const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                    const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                    std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                    activeMarkers.push_back({
                        {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                         ss2::MarkerKind::move},
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                        std::nullopt,
                        true
                    });
                    syncMarkers();
                    playSelectionVoice("move");
                    showNotice(ss2::trf("Hitching {0}...", {hudDisplayName(vehicleScene->vehicles().at(gIdx).id)}),
                               ss2::NoticeKind::accepted);
                    return;
                }
            }
        }
        if (!vids.empty() && targetVeh && vehicleScene &&
            vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
            simulation->canSupportVehicle(vids, *targetVeh)) {
            if (const auto res = simulation->orderSupportVehicle(vids, *targetVeh); res && *res > 0) {
                const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                const int mw = tv.width > 0 ? tv.width : 24;
                const int mh = tv.height > 0 ? tv.height : 24;
                const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::trf("Supply truck servicing {0}...", {ss2::tr(hudDisplayName(tv.id))}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if ((!ids.empty() || !vids.empty()) && targetInf &&
            scene->units().at(*targetInf).owner == 0 &&
            simulation->canHealSoldier(ids, vids, *targetInf)) {
            if (const auto res = simulation->orderHealSoldier(ids, vids, *targetInf); res && *res > 0) {
                const auto& tu = scene->units().at(*targetInf);
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{tu.left * mapScale, tu.top * mapScale, tu.width * mapScale, tu.height * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    *targetInf,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::tr("Moving to heal wounded soldier."), ss2::NoticeKind::accepted);
                return;
            }
        }
        if (!ids.empty() && targetVeh && vehicleScene &&
            vehicleScene->vehicles().at(*targetVeh).owner == 3 &&
            simulation->canCaptureVehicle(*targetVeh)) {
            if (const auto res = simulation->captureVehicle(ids, *targetVeh); res && *res > 0) {
                const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                const int mw = tv.width > 0 ? tv.width : 24;
                const int mh = tv.height > 0 ? tv.height : 24;
                const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::trf("Capturing vehicle ({0} soldiers)...", {std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if (!ids.empty() && targetVeh && vehicleScene &&
            vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
            simulation->canBoardVehicle(*targetVeh)) {
            if (const auto res = simulation->boardVehicle(ids, *targetVeh); res && *res > 0) {
                const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                const int mw = tv.width > 0 ? tv.width : 24;
                const int mh = tv.height > 0 ? tv.height : 24;
                const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::trf("Boarding transport ({0} soldiers).", {std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if ((!ids.empty() || !vids.empty()) && targetVeh && vehicleScene &&
            vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
            simulation->canEscortVehicle(ids, vids, *targetVeh)) {
            if (const auto res = simulation->orderEscortVehicle(ids, vids, *targetVeh); res && *res > 0) {
                const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                const int mw = tv.width > 0 ? tv.width : 24;
                const int mh = tv.height > 0 ? tv.height : 24;
                const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::trf("Escorting {0} ({1} units).",
                                    {ss2::tr(hudDisplayName(tv.id)), std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if ((!ids.empty() || !vids.empty()) && targetInf &&
            scene->units().at(*targetInf).owner == 0 &&
            simulation->canEscortSoldier(ids, vids, *targetInf)) {
            if (const auto res = simulation->orderEscortSoldier(ids, vids, *targetInf); res && *res > 0) {
                const auto& tu = scene->units().at(*targetInf);
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{tu.left * mapScale, tu.top * mapScale, tu.width * mapScale, tu.height * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    *targetInf,
                    true
                });
                syncMarkers();
                playSelectionVoice("move");
                showNotice(ss2::trf("Escorting {0} ({1} units).",
                                    {ss2::tr(hudDisplayName(tu.id)), std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        // The original Tutorial I player table assigns player 1 to the enemy team.
        if ((!ids.empty() || !vids.empty()) && targetInf &&
            isHostileOwner(scene->units().at(*targetInf).owner) &&
            simulation->isValidAttackTarget(*targetInf)) {
            std::optional<size_t> infRes = size_t(0), vehRes = size_t(0);
            if (!ids.empty()) infRes = simulation->attack(ids, *targetInf);
            if (infRes && !vids.empty()) vehRes = simulation->attackWithVehicles(vids, false, *targetInf);
            if (!infRes || !vehRes) {
                gestureInterrupted = true;
                std::cout << "Tutorial I attack interrupted by a pending briefing\n";
                return;
            }
            const auto count = *infRes + *vehRes;
            const auto total = ids.size() + vids.size();
            if (count) {
                auto label = count == 1 && total == 1 ? ss2::tr("Attack order accepted") :
                    ss2::trf("Attack order accepted by {0} of {1} soldiers", {std::to_string(count), std::to_string(total)});
                if (paused) label = ss2::trf("{0}. Press Pause or Ctrl+P to resume.", {label});
                showNotice(label, count == total ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
                const auto& u = scene->units().at(*targetInf);
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{u.left * mapScale, u.top * mapScale, u.width * mapScale, u.height * mapScale},
                     ss2::MarkerKind::attack},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    *targetInf,
                    true
                });
                syncMarkers();
                playSelectionVoice("attack");
                std::cout << "Tutorial I accepted attack on unit " << *targetInf << " at map pixel " << simX << ',' << simY
                          << " for " << count << '/' << total << (vids.empty() ? " selected infantry\n" : " selected units\n");
            } else {
                orderNotice.show(ss2::tr("Selected soldiers cannot engage that target."), ss2::NoticeKind::blocked);
                std::cout << "Tutorial I rejected attack on unit " << *targetInf << " for " << total
                          << (vids.empty() ? " selected infantry\n" : " selected units\n");
            }
            return;
        }
        if ((!ids.empty() || !vids.empty()) && targetVeh && vehicleScene &&
            isHostileOwner(vehicleScene->vehicles().at(*targetVeh).owner) &&
            simulation->isValidVehicleAttackTarget(*targetVeh)) {
            std::optional<size_t> infRes = size_t(0), vehRes = size_t(0);
            if (!ids.empty()) infRes = simulation->attackVehicle(ids, *targetVeh);
            if (infRes && !vids.empty()) vehRes = simulation->attackWithVehicles(vids, true, *targetVeh);
            if (!infRes || !vehRes) {
                gestureInterrupted = true;
                std::cout << "Tutorial I attack interrupted by a pending briefing\n";
                return;
            }
            const auto count = *infRes + *vehRes;
            const auto total = ids.size() + vids.size();
            if (count) {
                auto label = count == 1 && total == 1 ? ss2::tr("Attack order accepted") :
                    ss2::trf("Attack order accepted by {0} of {1} soldiers", {std::to_string(count), std::to_string(total)});
                if (paused) label = ss2::trf("{0}. Press Pause or Ctrl+P to resume.", {label});
                showNotice(label, count == total ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
                const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                const int mw = tv.width > 0 ? tv.width : 24;
                const int mh = tv.height > 0 ? tv.height : 24;
                const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                     ss2::MarkerKind::attack},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                playSelectionVoice("attack");
                std::cout << "Accepted attack on vehicle " << *targetVeh << " at map pixel " << simX << ',' << simY
                          << " for " << count << '/' << total << " selected units\n";
            } else {
                orderNotice.show(ss2::tr("Selected soldiers cannot engage that target."), ss2::NoticeKind::blocked);
            }
            return;
        }
        if (!targetInf && !targetVeh) {
            const auto targetBld = buildingAtMap(simX, simY);
            if (targetBld) {
                const auto& lb = liveBuildings[*targetBld];
                if (!ids.empty() && simulation->canGarrisonBuilding(*targetBld)) {
                    if (const auto res = simulation->garrisonBuilding(ids, *targetBld); res && *res > 0) {
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Garrisoning building ({0} soldiers).", {std::to_string(*res)}),
                                   ss2::NoticeKind::accepted);
                        return;
                    }
                }
                if (!vids.empty() && simulation->canSupportBuilding(vids, *targetBld)) {
                    if (const auto res = simulation->orderSupportBuilding(vids, *targetBld); res && *res > 0) {
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        bool hasSupTruck = false, hasAmbulance = false;
                        if (vehicleScene && unitCatalog) {
                            for (const size_t sv : vids) {
                                if (sv >= vehicleScene->vehicles().size()) continue;
                                if (const auto* spec = unitCatalog->find(vehicleScene->vehicles()[sv].id)) {
                                    if (spec->isSupplyTruck()) hasSupTruck = true;
                                    if (spec->isAmbulance()) hasAmbulance = true;
                                }
                            }
                        }
                        showNotice(ss2::tr((hasAmbulance && !hasSupTruck)
                                               ? "Ambulance treating garrisoned troops..."
                                               : "Supply truck repairing building..."),
                                   ss2::NoticeKind::accepted);
                        return;
                    }
                }
                if ((!ids.empty() || !vids.empty()) && lb.owner >= 0 && isHostileOwner(lb.owner)) {
                    std::optional<size_t> infRes = size_t(0), vehRes = size_t(0);
                    if (!ids.empty()) infRes = simulation->attackBuilding(ids, *targetBld);
                    if (infRes && !vids.empty()) vehRes = simulation->attackBuildingWithVehicles(vids, *targetBld);
                    const size_t count = (infRes ? *infRes : 0) + (vehRes ? *vehRes : 0);
                    if (count > 0) {
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                             ss2::MarkerKind::attack},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("attack");
                        showNotice(ss2::tr("Attacking occupied building!"), ss2::NoticeKind::accepted);
                        return;
                    }
                }
            }
            if (!ids.empty() && simulation->canDisarmMineAt(ids, simX, simY)) {
                if (const auto res = simulation->orderDisarmMine(ids, simX, simY); res && *res > 0) {
                    std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                    activeMarkers.push_back({
                        {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                         ss2::MarkerKind::move},
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                        std::nullopt,
                        true
                    });
                    syncMarkers();
                    playSelectionVoice("move");
                    showNotice(ss2::trf("Sapper clearing mines ({0} soldiers)...", {std::to_string(*res)}),
                               ss2::NoticeKind::accepted);
                    return;
                }
            }
        }
        const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        (void)issueGroundOrder(simX, simY, false, append);
    }
    if (simulation && event.type == SDL_EVENT_MOUSE_MOTION && !event.motion.state) {
        const auto [x,y] = renderer.mapPosition(event.motion.x,event.motion.y);
        const int simX = int(std::floor(x / mapScale)), simY = int(std::floor(y / mapScale));
        const auto& picked = selection->selected();
        const auto& pickedVeh = selection->selectedVehicles();
        const std::vector<size_t> ids(picked.begin(), picked.end());
        const std::vector<size_t> pvids(pickedVeh.begin(), pickedVeh.end());
        const auto targetInf = selection->infantryAt(int(std::floor(x)), int(std::floor(y)));
        const auto targetVeh = selection->vehicleAt(int(std::floor(x)), int(std::floor(y)));
        const auto hoverBld = (!targetInf && !targetVeh) ? buildingAtMap(simX, simY) : std::nullopt;
        const bool hasAny = !ids.empty() || !pickedVeh.empty();
        const bool hasGarrisonCmd = !hasAny && selectedBuilding &&
                                    *selectedBuilding < liveBuildings.size() &&
                                    !liveBuildings[*selectedBuilding].destroyed &&
                                    liveBuildings[*selectedBuilding].owner == 0 &&
                                    liveBuildings[*selectedBuilding].occupantCount > 0;
        const bool hasCmd = hasAny || hasGarrisonCmd;
        const bool validInfTarget = hasCmd && targetInf && isHostileOwner(scene->units().at(*targetInf).owner) &&
                                    simulation->isValidAttackTarget(*targetInf);
        const bool validVehTarget = hasCmd && targetVeh && vehicleScene &&
                                    isHostileOwner(vehicleScene->vehicles().at(*targetVeh).owner) &&
                                    simulation->isValidVehicleAttackTarget(*targetVeh);
        const bool validBldTarget = hasCmd && hoverBld &&
                                    (!selectedBuilding || *hoverBld != *selectedBuilding) &&
                                    liveBuildings[*hoverBld].owner >= 0 &&
                                    isHostileOwner(liveBuildings[*hoverBld].owner);
        bool canTowHover = false;
        if (!pickedVeh.empty() && targetVeh && vehicleScene) {
            for (size_t sv : pickedVeh) {
                if (simulation->canTowVehicle(sv, *targetVeh) || simulation->canTowVehicle(*targetVeh, sv)) {
                    canTowHover = true;
                    break;
                }
            }
        }
        const bool canGarrisonHover = !ids.empty() && hoverBld && simulation->canGarrisonBuilding(*hoverBld);
        const bool canSupportHover =
            (!pvids.empty() && targetVeh && vehicleScene &&
             vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
             simulation->canSupportVehicle(pvids, *targetVeh)) ||
            (!pvids.empty() && hoverBld && simulation->canSupportBuilding(pvids, *hoverBld)) ||
            (hasAny && targetInf && scene->units().at(*targetInf).owner == 0 &&
             simulation->canHealSoldier(ids, pvids, *targetInf));
        const bool canEscortHover =
            (hasAny && targetVeh && vehicleScene &&
             vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
             simulation->canEscortVehicle(ids, pvids, *targetVeh)) ||
            (hasAny && targetInf && scene->units().at(*targetInf).owner == 0 &&
             simulation->canEscortSoldier(ids, pvids, *targetInf));
        const bool canDisarmHover =
            !ids.empty() && !targetInf && !targetVeh && simulation->canDisarmMineAt(ids, simX, simY);
        if (armedAirSupport) {
            const bool strike = *armedAirSupport == ss2::assets::AirMissionKind::Bomber ||
                                *armedAirSupport == ss2::assets::AirMissionKind::Fighter;
            SDL_SetCursor(strike ? (attackCursor ? attackCursor.get() : SDL_GetDefaultCursor())
                                 : (moveCursor ? moveCursor.get() : SDL_GetDefaultCursor()));
        } else if (attackMoveArmed || attackGroundArmed || patrolArmed || validInfTarget || validVehTarget || validBldTarget) {
            SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
        } else if (reverseMoveArmed || faceArmed || canSupportHover || canEscortHover || canDisarmHover) {
            SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
        } else if (canTowHover || canGarrisonHover ||
                   (!ids.empty() && targetVeh && vehicleScene &&
                    ((vehicleScene->vehicles().at(*targetVeh).owner == 0 && simulation->canBoardVehicle(*targetVeh)) ||
                     (vehicleScene->vehicles().at(*targetVeh).owner == 3 && simulation->canCaptureVehicle(*targetVeh))))) {
            SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
        } else if ((!ids.empty() && simulation->canMove(ids, simX, simY)) || !pickedVeh.empty()) {
            SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
        } else {
            SDL_SetCursor(SDL_GetDefaultCursor());
        }
    }
    if (selection) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.mod & SDL_KMOD_CTRL) && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) {
            const auto grpIdx = static_cast<std::size_t>(event.key.key - SDLK_0);
            hudGroups[grpIdx] = selection->selected();
            hudVehicleGroups[grpIdx] = selection->selectedVehicles();
        }
        selection->handle(event);
        if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
            selectedBuilding.reset();
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
                   !liveBuildings.empty()) {
            const auto [bx, by] = renderer.mapPosition(event.button.x, event.button.y);
            const auto clickedBld = buildingAtMap(int(std::floor(bx / mapScale)), int(std::floor(by / mapScale)));
            if (clickedBld && liveBuildings[*clickedBld].owner == 0 && liveBuildings[*clickedBld].occupantCount > 0) {
                selectedBuilding = *clickedBld;
                const auto& lb = liveBuildings[*clickedBld];
                showNotice(ss2::trf("Garrisoned building ({0}/{1}) — press U to evacuate.",
                                    {std::to_string(lb.occupantCount), std::to_string(lb.capacity)}),
                           ss2::NoticeKind::accepted);
            } else {
                selectedBuilding.reset();
            }
        }
        if (selection->selected() != prevSelInf || selection->selectedVehicles() != prevSelVeh) {
            const bool grewOrChanged = !selection->selected().empty() || !selection->selectedVehicles().empty();
            prevSelInf = selection->selected();
            prevSelVeh = selection->selectedVehicles();
            if (grewOrChanged &&
                (event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                 (event.type == SDL_EVENT_KEY_DOWN &&
                  (event.key.key == SDLK_TAB || event.key.key == SDLK_V ||
                   (event.key.key >= SDLK_0 && event.key.key <= SDLK_9 && !(event.key.mod & SDL_KMOD_CTRL)))))) {
                playSelectionVoice("select");
            }
            if (!grewOrChanged && (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed)) {
                attackMoveArmed = false;
                attackGroundArmed = false;
                reverseMoveArmed = false;
                patrolArmed = false;
                faceArmed = false;
                resetCursor();
            }
        }
    }
    if (selection && event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) return;
    if (event.type == SDL_EVENT_KEY_DOWN &&
        (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT ||
         event.key.key == SDLK_UP || event.key.key == SDLK_DOWN)) return;
    renderer.handleEvent(event);
}

} // namespace ss2
