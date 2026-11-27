#include "session/game_session.hpp"
#include "session/save_file.hpp"
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

void GameSession::placeMpBeacon(int simX, int simY) {
    if (!multiplayer.enabled || !simulation) return;
    const auto now = std::chrono::steady_clock::now();
    if (mpTransport && mpTransport->isActive()) {
        (void)mpTransport->sendBeacon(
            ss2::BeaconPacket{static_cast<std::uint8_t>(localOwner()), simX, simY, true});
        mpTransport->flushAll();
    }
    if (activeBeacons.size() >= 6u) activeBeacons.erase(activeBeacons.begin());
    activeBeacons.push_back({simX, simY, static_cast<std::uint8_t>(localOwner()), now});
    pushMinimapPing(simX, simY, false);
    if (audio) audio->playGroup("accept", 0.55f);
    showNotice(ss2::tr("Beacon placed."), ss2::NoticeKind::accepted);
}

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
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_ESCAPE) {
            if (returnToMainMenuOut) *returnToMainMenuOut = true;
            running = false;
            return;
        }
        if (liveReplay && simulation) {
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                switch (event.key.key) {
                case SDLK_F11: {
                    const bool isFullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                    if (!SDL_SetWindowFullscreen(window, !isFullscreen)) {
                        SDL_Log("Fullscreen switch failed: %s", SDL_GetError());
                    }
                    return;
                }
                case SDLK_Y:
                    if ((event.key.mod & SDL_KMOD_ALT) != 0 && !(event.key.mod & SDL_KMOD_CTRL)) {
                        hudHidden = !hudHidden;
                        if (hudHidden) {
                            if (hud) hud->cancelMinimapDrag();
                            renderer.clearOverlay(OverlaySlot::hud);
                            renderer.setHudBarHeight(0);
                            orderNotice.show(ss2::tr("HUD hidden (Alt+Y to restore)."), ss2::NoticeKind::accepted);
                        } else {
                            if (hud) hud->forceRedraw();
                            orderNotice.show(ss2::tr("HUD restored."), ss2::NoticeKind::accepted);
                        }
                        return;
                    }
                    break;
                case SDLK_M:
                    if (audio && (event.key.mod & SDL_KMOD_CTRL) != 0) {
                        const bool muted = audio->toggleMusicMute();
                        settings.musicMuted = muted;
                        persistNow();
                        orderNotice.show(ss2::tr(muted ? "Music muted (Ctrl+M)." : "Music unmuted."),
                                         ss2::NoticeKind::accepted);
                        return;
                    }
                    break;
                case SDLK_N:
                    if (audio && !(event.key.mod & SDL_KMOD_CTRL)) {
                        if (audio->nextMusicTrack()) {
                            orderNotice.show(ss2::tr("Next soundtrack track."), ss2::NoticeKind::accepted);
                        }
                        return;
                    }
                    break;
                case SDLK_BACKSPACE:
                case SDLK_KP_0:
                    if (!(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                        renderer.resetZoom();
                        orderNotice.show(ss2::tr("Zoom reset (1:1)."), ss2::NoticeKind::accepted);
                        return;
                    }
                    break;
                case SDLK_HOME:
                    if ((event.key.mod & SDL_KMOD_ALT) != 0 && !(event.key.mod & SDL_KMOD_CTRL)) {
                        renderer.fitMapOverview();
                        orderNotice.show(ss2::tr("Camera: full map overview."), ss2::NoticeKind::accepted);
                        return;
                    }
                    break;
                case SDLK_SPACE:
                case SDLK_PAUSE:
                    replayTransport.togglePause();
                    paused = replayTransport.paused || simulation->isPaused();
                    showNotice(ss2::tr(replayTransport.paused ? "Replay paused." : "Replay resumed."),
                               ss2::NoticeKind::accepted);
                    return;
                case SDLK_P:
                    if ((event.key.mod & SDL_KMOD_CTRL) != 0) {
                        replayTransport.togglePause();
                        paused = replayTransport.paused || simulation->isPaused();
                        showNotice(ss2::tr(replayTransport.paused ? "Replay paused." : "Replay resumed."),
                                   ss2::NoticeKind::accepted);
                        return;
                    }
                    break;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                case SDLK_KP_PLUS:
                case SDLK_RIGHTBRACKET:
                    replayTransport.stepFaster();
                    showNotice(ss2::trf("Replay speed: {0}", {std::string(replayTransport.speedLabel())}),
                               ss2::NoticeKind::accepted);
                    return;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                case SDLK_LEFTBRACKET:
                    replayTransport.stepSlower();
                    showNotice(ss2::trf("Replay speed: {0}", {std::string(replayTransport.speedLabel())}),
                               ss2::NoticeKind::accepted);
                    return;
                case SDLK_PERIOD:
                    if (replayTransport.paused && !replayTransport.finished) {
                        (void)simulation->replayStep(replayFile->commands, replayTransport.cursor, replayFile->endTick);
                        return;
                    }
                    break;
                case SDLK_TAB:
                    replayTransport.cycleViewerSeat(4);
                    syncReplayViewerPerspective();
                    showNotice(replayTransport.viewerSeat < 0
                                   ? ss2::tr("Replay perspective: All Vision")
                                   : ss2::trf("Replay perspective: Player {0}",
                                              {std::to_string(replayTransport.viewerSeat + 1)}),
                               ss2::NoticeKind::accepted);
                    return;
                default:
                    break;
                }
            }
            if (hud && !hudHidden) {
                const auto hr = hud->handle(event);
                if (hr.action == ss2::HudAction::moveCamera && hudMapW && hudMapH) {
                    const double targetX = hr.u * static_cast<double>(hudMapW * mapScale);
                    const double targetY = hr.v * static_cast<double>(hudMapH * mapScale);
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                        renderer.smoothFocusMap(targetX, targetY);
                    } else {
                        renderer.focusMap(targetX, targetY);
                    }
                    return;
                }
                if (hr.action == ss2::HudAction::menu) {
                    if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
                    if (returnToMainMenuOut) *returnToMainMenuOut = true;
                    running = false;
                    return;
                }
                if (hr.action == ss2::HudAction::pause) {
                    if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
                    replayTransport.togglePause();
                    paused = replayTransport.paused || simulation->isPaused();
                    return;
                }
                if (hr.action == ss2::HudAction::speedDown) {
                    if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
                    replayTransport.stepSlower();
                    showNotice(ss2::trf("Replay speed: {0}", {std::string(replayTransport.speedLabel())}),
                               ss2::NoticeKind::accepted);
                    return;
                }
                if (hr.action == ss2::HudAction::speedUp) {
                    if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
                    replayTransport.stepFaster();
                    showNotice(ss2::trf("Replay speed: {0}", {std::string(replayTransport.speedLabel())}),
                               ss2::NoticeKind::accepted);
                    return;
                }
                if (hr.consumed) return;
            }
        }
        if (event.type == SDL_EVENT_KEY_DOWN &&
            (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT ||
             event.key.key == SDLK_UP || event.key.key == SDLK_DOWN)) return;
        renderer.handleEvent(event);
        return;
    }
    if (multiplayer.enabled && simulation && !menu.active() && !briefing.active()) {
        const std::string senderName = (localOwner() >= 0 && localOwner() < 4 &&
                                        !multiplayer.playerNames[localOwner()].empty())
            ? multiplayer.playerNames[localOwner()]
            : (settings.activeProfile.empty() ? std::string("Player") : settings.activeProfile);
        if (mpChatActive) {
            const char* promptKey = mpChatTeamOnly ? "Say Allies ({0}): {1}" : "Say ({0}): {1}";
            if (event.type == SDL_EVENT_TEXT_INPUT && event.text.text) {
                for (const unsigned char uc : std::string_view(event.text.text)) {
                    if (uc >= 0x20u && uc != 0x7fu && mpChatDraft.size() < 120u) {
                        mpChatDraft.push_back(static_cast<char>(uc));
                    }
                }
                showNotice(ss2::trf(promptKey, {senderName, mpChatDraft + "_"}), ss2::NoticeKind::accepted);
                return;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    mpChatActive = false;
                    mpChatTeamOnly = false;
                    mpChatDraft.clear();
                    if (window) SDL_StopTextInput(window);
                    showNotice("", ss2::NoticeKind::accepted);
                    return;
                }
                if (event.key.key == SDLK_BACKSPACE) {
                    while (!mpChatDraft.empty() &&
                           (static_cast<unsigned char>(mpChatDraft.back()) & 0xc0u) == 0x80u) {
                        mpChatDraft.pop_back();
                    }
                    if (!mpChatDraft.empty()) mpChatDraft.pop_back();
                    showNotice(ss2::trf(promptKey, {senderName, mpChatDraft + "_"}), ss2::NoticeKind::accepted);
                    return;
                }
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                    std::string msg = std::move(mpChatDraft);
                    bool teamOnly = mpChatTeamOnly;
                    mpChatDraft.clear();
                    mpChatActive = false;
                    mpChatTeamOnly = false;
                    if (window) SDL_StopTextInput(window);
                    if (msg.starts_with("/a ")) {
                        teamOnly = true;
                        msg.erase(0, 3);
                    } else if (msg == "/a") {
                        msg.clear();
                    }
                    if (!msg.empty()) {
                        if (mpTransport && mpTransport->isActive()) {
                            (void)mpTransport->sendChat(
                                ss2::ChatPacket{static_cast<std::uint8_t>(localOwner()), senderName, msg, teamOnly});
                            mpTransport->flushAll();
                        }
                        const std::string line = (teamOnly ? ("[" + ss2::tr("Allies") + "] ") : std::string{}) +
                                                 senderName + ": " + msg;
                        logHud(line);
                        showNotice(ss2::trf(teamOnly ? "Team Chat: {0}" : "Chat: {0}", {line}),
                                   ss2::NoticeKind::accepted);
                    } else {
                        showNotice("", ss2::NoticeKind::accepted);
                    }
                    return;
                }
                return;
            }
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                   (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
            mpChatActive = true;
            mpChatTeamOnly = (event.key.mod & SDL_KMOD_SHIFT) != 0;
            mpChatDraft.clear();
            if (window) SDL_StartTextInput(window);
            showNotice(ss2::trf(mpChatTeamOnly ? "Say Allies ({0}): {1}" : "Say ({0}): {1}", {senderName, "_"}),
                       ss2::NoticeKind::accepted);
            return;
        }
    }
    if (hud && (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE || event.type == SDL_EVENT_WINDOW_FOCUS_LOST)) {
        hud->handle(event);
    }
    if (menu.active()) {
        MenuResult mr{};
        if (postMissionScreen && postMissionScreen->active()) {
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                (void)menu.handle(event);
            }
            const auto pmr = postMissionScreen->handleEvent(event);
            mr.consumed = pmr.consumed;
            mr.closed = pmr.closed;
            mr.activated = pmr.activated;
            if (!pmr.closed && postMissionScreen->active()) {
                menu.highlight(postMissionScreen->focusedId());
                syncPostMissionMenuOverlay();
                postMissionScreen->draw(renderer);
            }
        } else {
            mr = menu.handle(event);
        }
        if (audio && !mr.activated.empty() && mr.activated != "resume") {
            audio->playGroup("choose", 0.42f, ss2::VoicePriority::High);
        }
        const auto adjustPauseOption = [&](std::string_view optId, int delta) -> bool {
            const int step = delta >= 0 ? 1 : -1;
            if (optId == "fullscreen") {
                const bool isFullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                if (!SDL_SetWindowFullscreen(window, !isFullscreen)) {
                    SDL_Log("Fullscreen switch failed: %s", SDL_GetError());
                }
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "effects") {
                combatEffects = !combatEffects;
                settings.combatEffects = combatEffects;
                persistNow();
                if (!combatEffects) {
                    activeVfx.clear();
                    burningWrecks.clear();
                }
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "doctrine" && !multiplayer.enabled && !replayFile) {
                settings.realismDoctrine = !settings.realismDoctrine;
                persistNow();
                if (settings.realismDoctrine && simulation) {
                    simulation->setRealismDoctrine(true);
                }
                menu.setItems(buildOptionsItems());
                orderNotice.show(
                    ss2::tr(settings.realismDoctrine
                                ? "Realism Doctrine (RWM/LRM) enabled (+sight & standoff range)."
                                : "Classic Gold Doctrine selected (applies on next mission)."),
                    ss2::NoticeKind::accepted);
                return true;
            }
            if (optId == "speed" && simulation && !multiplayer.enabled) {
                static constexpr std::array<int, 4> kSpeeds{{50, 100, 200, 300}};
                const int cur = simulation->speedPercent();
                std::size_t idx = 1;
                for (std::size_t k = 0; k < kSpeeds.size(); ++k) {
                    if (kSpeeds[k] == cur) { idx = k; break; }
                }
                const int next = kSpeeds[(idx + std::size_t(step + int(kSpeeds.size()))) % kSpeeds.size()];
                simulation->setSpeedPercent(next);
                settings.speedPercent = next;
                persistNow();
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "difficulty" && !multiplayer.enabled) {
                settings.difficulty = 1 + ((settings.difficulty - 1 + step + 3) % 3);
                persistNow();
                const char* diffKey = settings.difficulty == 1 ? "Easy" : settings.difficulty == 3 ? "Hard" : "Normal";
                menu.setItems(buildOptionsItems());
                orderNotice.show(ss2::trf("Difficulty set to {0} (applies on restart or next mission).",
                                          {ss2::tr(diffKey)}),
                                 ss2::NoticeKind::accepted);
                return true;
            }
            if (optId == "language") {
                static constexpr std::array<ss2::Language, 3> kLangs{{
                    ss2::Language::english, ss2::Language::french, ss2::Language::spanish
                }};
                const auto cur = ss2::language();
                std::size_t idx = 0;
                for (std::size_t k = 0; k < kLangs.size(); ++k) {
                    if (kLangs[k] == cur) { idx = k; break; }
                }
                const auto nextLang = kLangs[(idx + std::size_t(step + int(kLangs.size()))) % kLangs.size()];
                ss2::setLanguage(nextLang);
                settings.language = nextLang;
                persistNow();
                menu.open(ss2::tr("Options"), buildOptionsItems());
                menu.highlight("language");
                return true;
            }
            if (optId == "scroll_speed") {
                static constexpr std::array<int, 4> kScrollSpeeds{{60, 100, 150, 200}};
                std::size_t idx = 1;
                for (std::size_t k = 0; k < kScrollSpeeds.size(); ++k) {
                    if (kScrollSpeeds[k] == settings.scrollSpeedPercent) { idx = k; break; }
                }
                settings.scrollSpeedPercent =
                    kScrollSpeeds[(idx + std::size_t(step + int(kScrollSpeeds.size()))) % kScrollSpeeds.size()];
                persistNow();
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "edge_scroll") {
                settings.edgeScroll = !settings.edgeScroll;
                persistNow();
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "enhanced") {
                enhancedGraphics = !enhancedGraphics;
                settings.enhancedGraphics = enhancedGraphics;
                persistNow();
                applyMapGraphics();
                syncGpuSprites();
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "smooth") {
                smoothTerrain = !smoothTerrain;
                settings.smoothTerrain = smoothTerrain;
                persistNow();
                applyMapGraphics();
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "music" && audio) {
                settings.musicMuted = audio->toggleMusicMute();
                persistNow();
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "sfx_volume" && audio) {
                static constexpr std::array<float, 5> kSfx{{0.0f, 0.25f, 0.50f, 0.75f, 1.0f}};
                const float cur = audio->sfxVolume();
                std::size_t best = 0;
                float bestDiff = 10.0f;
                for (std::size_t k = 0; k < kSfx.size(); ++k) {
                    if (const float d = std::abs(cur - kSfx[k]); d < bestDiff) { bestDiff = d; best = k; }
                }
                const float next = kSfx[(best + std::size_t(step + int(kSfx.size()))) % kSfx.size()];
                audio->setSfxVolume(next);
                settings.sfxVolume = next;
                persistNow();
                if (next > 0.0f) audio->playGroup("accept", 0.5f);
                menu.setItems(buildOptionsItems());
                return true;
            }
            if (optId == "music_volume" && audio) {
                static constexpr std::array<float, 4> kMus{{0.09f, 0.18f, 0.28f, 0.40f}};
                const float cur = audio->musicVolume();
                std::size_t best = 0;
                float bestDiff = 10.0f;
                for (std::size_t k = 0; k < kMus.size(); ++k) {
                    if (const float d = std::abs(cur - kMus[k]); d < bestDiff) { bestDiff = d; best = k; }
                }
                const float next = kMus[(best + std::size_t(step + int(kMus.size()))) % kMus.size()];
                audio->setMusicVolume(next);
                settings.musicVolume = next;
                persistNow();
                menu.setItems(buildOptionsItems());
                return true;
            }
            return false;
        };

        if (mr.closed) {
            if (menuPage == MenuPage::Options || menuPage == MenuPage::SaveLoad ||
                menuPage == MenuPage::Controls || menuPage == MenuPage::ForceStatus ||
                menuPage == MenuPage::AirSupport || menuPage == MenuPage::Campaigns ||
                menuPage == MenuPage::Missions || menuPage == MenuPage::CampaignMissions ||
                menuPage == MenuPage::ConfirmRestart || menuPage == MenuPage::ConfirmMainMenu ||
                menuPage == MenuPage::ConfirmQuit) {
                if (audio) audio->playGroup("pull", 0.42f, ss2::VoicePriority::High);
                openPauseMenu();
            } else {
                closeMenu();
            }
        } else if (mr.step != 0 && menuPage == MenuPage::Options) {
            if (adjustPauseOption(menu.highlighted(), mr.step) && audio) {
                audio->playGroup("choose", 0.35f, ss2::VoicePriority::High);
            }
        } else if (mr.activated == "resume") {
            closeMenu();
        } else if (mr.activated == "options") {
            openOptionsMenu();
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
                slot >= (isLoad ? -3 : 0) && slot <= 3) {
                if (isLoad) doLoadSlot(slot);
                else doSaveSlot(slot);
                closeMenu();
            }
        } else if (adjustPauseOption(mr.activated, 1)) {
        } else if (mr.activated == "next_track" && audio) {
            audio->nextMusicTrack();
            logHud(ss2::tr("Switched to next soundtrack track."));
        } else if (mr.activated == "next_campaign" && nextCampaignMissionPath && nextMissionOut) {
            if (campaignUnitPools && simulation) {
                *campaignUnitPools = simulation->savedUnitPools();
            }
            *nextMissionOut = *nextCampaignMissionPath;
            dismissPostMissionScreen();
            menu.close();
            menuPage = MenuPage::None;
            running = false;
        } else if (mr.activated == "restart_mission" && !multiplayer.enabled && !currentMissionPath.empty() && nextMissionOut) {
            if (simulation && simulation->simTick() > 25u) {
                openConfirmMenu(MenuPage::ConfirmRestart, ss2::tr("Restart mission?"),
                                "confirm_restart", ss2::tr("Yes, restart"));
            } else {
                if (campaignUnitPools) *campaignUnitPools = initialSavedPools;
                *nextMissionOut = currentMissionPath;
                dismissPostMissionScreen();
                menu.close();
                menuPage = MenuPage::None;
                running = false;
            }
        } else if (mr.activated == "confirm_restart" && !multiplayer.enabled && !currentMissionPath.empty() && nextMissionOut) {
            if (campaignUnitPools) *campaignUnitPools = initialSavedPools;
            *nextMissionOut = currentMissionPath;
            dismissPostMissionScreen();
            menu.close();
            menuPage = MenuPage::None;
            running = false;
        } else if (mr.activated == "replay" && !currentMissionPath.empty() && nextMissionOut) {
            if (campaignUnitPools) {
                *campaignUnitPools = initialSavedPools;
            }
            *nextMissionOut = currentMissionPath;
            dismissPostMissionScreen();
            menu.close();
            menuPage = MenuPage::None;
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
                    const auto& cm = menuCampaigns[cIdx].missions[mIdx];
                    const auto chosenPath = (settings.difficulty <= 1 && !cm.easyPath.empty()) ? cm.easyPath : cm.path;
                    if (campaignUnitPools) {
                        campaignUnitPools->clear();
                        if (persistSettings) {
                            const auto poolPath = ss2::campaignPoolPathForMission(settings.activeProfile, chosenPath);
                            if (auto diskPools = ss2::readCampaignPoolFile(poolPath)) {
                                *campaignUnitPools = std::move(*diskPools);
                            }
                        }
                    }
                    *nextMissionOut = chosenPath;
                    menu.close();
                    menuPage = MenuPage::None;
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
                if (campaignUnitPools) campaignUnitPools->clear();
                *nextMissionOut = menuMissions[idx].path;
                menu.close();
                menuPage = MenuPage::None;
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
            openPauseMenu();
        } else if (mr.activated == "resign" && simulation && multiplayer.enabled) {
            simulation->resign(static_cast<std::uint8_t>(localOwner()));
            closeMenu();
            showNotice(ss2::tr("You surrendered."), ss2::NoticeKind::blocked);
        } else if (mr.activated == "main_menu") {
            if (menuPage == MenuPage::Pause && simulation && simulation->simTick() > 25u) {
                openConfirmMenu(MenuPage::ConfirmMainMenu, ss2::tr("Return to main menu?"),
                                "confirm_main_menu", ss2::tr("Yes, exit to main menu"));
            } else {
                if (returnToMainMenuOut) *returnToMainMenuOut = true;
                dismissPostMissionScreen();
                menu.close();
                menuPage = MenuPage::None;
                running = false;
            }
        } else if (mr.activated == "confirm_main_menu") {
            if (returnToMainMenuOut) *returnToMainMenuOut = true;
            dismissPostMissionScreen();
            menu.close();
            menuPage = MenuPage::None;
            running = false;
        } else if (mr.activated == "quit") {
            if (menuPage == MenuPage::Pause && simulation && simulation->simTick() > 25u) {
                openConfirmMenu(MenuPage::ConfirmQuit, ss2::tr("Quit to desktop?"),
                                "confirm_quit", ss2::tr("Yes, quit to desktop"));
            } else {
                dismissPostMissionScreen();
                menu.close();
                menuPage = MenuPage::None;
                running = false;
            }
        } else if (mr.activated == "confirm_quit") {
            dismissPostMissionScreen();
            menu.close();
            menuPage = MenuPage::None;
            running = false;
        }
        if (mr.consumed) return;
    }
    if (hud && !hudHidden && !briefing.active()) {
        const auto hr = hud->handle(event);
        if (hr.hoverEntered && audio) {
            const auto hNow = std::chrono::steady_clock::now();
            if (hNow - lastHoverChimeAt >= std::chrono::milliseconds(45)) {
                (void)audio->playGroup("choose", 0.20f, ss2::VoicePriority::Low);
                lastHoverChimeAt = hNow;
            }
        }
        if (hr.action == ss2::HudAction::moveCamera && hudMapW && hudMapH) {
            if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT && (SDL_GetModState() & SDL_KMOD_ALT) != 0) {
                hud->cancelMinimapDrag();
                const int simX = std::clamp(static_cast<int>(std::lround(hr.u * static_cast<float>(hudMapW))),
                                            0, static_cast<int>(hudMapW) - 1);
                const int simY = std::clamp(static_cast<int>(std::lround(hr.v * static_cast<float>(hudMapH))),
                                            0, static_cast<int>(hudMapH) - 1);
                if (multiplayer.enabled) {
                    placeMpBeacon(simX, simY);
                } else {
                    pushMinimapPing(simX, simY, false);
                    recordEventPos(static_cast<double>(simX * mapScale), static_cast<double>(simY * mapScale));
                    if (audio) audio->playGroup("e_telegram", 0.42f, ss2::VoicePriority::Normal);
                    orderNotice.show(ss2::tr("Tactical ping placed on map."), ss2::NoticeKind::accepted);
                }
            } else if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                (armedAirSupport || attackMoveArmed || patrolArmed || attackGroundArmed || smokeArmed)) {
                hud->cancelMinimapDrag();
                const int simX = std::clamp(static_cast<int>(std::lround(hr.u * static_cast<float>(hudMapW))),
                                            0, static_cast<int>(hudMapW) - 1);
                const int simY = std::clamp(static_cast<int>(std::lround(hr.v * static_cast<float>(hudMapH))),
                                            0, static_cast<int>(hudMapH) - 1);
                if (armedAirSupport) {
                    dispatchAirSupportAt(*armedAirSupport, simX, simY);
                    armedAirSupport.reset();
                } else if (smokeArmed) {
                    const auto vids = activeVehicleSelection();
                    const auto sids = activeInfantrySelection();
                    if (simulation->deploySmoke(sids, vids, simX, simY) > 0) {
                        pushOrderPulse(simX, simY, OrderPulse::Kind::Support);
                        if (audio) audio->playGroup("lightfire", 0.45f, ss2::VoicePriority::Normal);
                        showNotice(ss2::tr("Tactical smoke screen deployed."), ss2::NoticeKind::accepted);
                    } else {
                        orderNotice.show(ss2::tr("Smoke screen out of range or out of smoke rounds."),
                                         ss2::NoticeKind::blocked);
                    }
                    smokeArmed = false;
                } else if (attackGroundArmed) {
                    const auto vids = activeVehicleSelection();
                    const auto sids = activeInfantrySelection();
                    size_t totalBombard = 0;
                    if (!vids.empty()) {
                        if (const auto res = simulation->attackGroundVehicles(vids, simX, simY)) totalBombard += *res;
                    }
                    if (!sids.empty()) {
                        if (const auto res = simulation->attackGroundSoldiers(sids, simX, simY)) totalBombard += *res;
                    }
                    if (totalBombard > 0) {
                        pushOrderPulse(simX, simY, OrderPulse::Kind::Attack);
                        pushMinimapPing(simX, simY, true);
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
                } else if (attackMoveArmed) {
                    const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                    if (issueGroundOrder(simX, simY, true, append)) {
                        pushMinimapPing(simX, simY, true);
                    }
                    attackMoveArmed = false;
                }
                resetCursor();
            } else {
                const double targetX = hr.u * static_cast<double>(hudMapW * mapScale);
                const double targetY = hr.v * static_cast<double>(hudMapH * mapScale);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    renderer.smoothFocusMap(targetX, targetY);
                } else {
                    renderer.focusMap(targetX, targetY);
                }
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
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doStop();
        } else if (hr.action == ss2::HudAction::holdFire) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doToggleHoldFire();
        } else if (hr.action == ss2::HudAction::crouch) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doToggleCrouch();
        } else if (hr.action == ss2::HudAction::recon) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doToggleBinoculars();
        } else if (hr.action == ss2::HudAction::attackMove) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doArmAttackMove();
        } else if (hr.action == ss2::HudAction::patrol) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doArmPatrol();
        } else if (hr.action == ss2::HudAction::attackGround) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doArmAttackGround();
        } else if (hr.action == ss2::HudAction::unload) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doUnloadOrUnhitch();
        } else if (hr.action == ss2::HudAction::mine) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doLayMine();
        } else if (hr.action == ss2::HudAction::scatter) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doScatter();
        } else if (hr.action == ss2::HudAction::recallGroup && hr.groupDigit >= 0 && hr.groupDigit < 10 && selection) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            const auto beforeInf = selection->selected();
            const auto beforeVeh = selection->selectedVehicles();
            selection->recallControlGroup(static_cast<unsigned>(hr.groupDigit), hr.shiftHeld);
            if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                selectedBuilding.reset();
            }
            if (!hr.shiftHeld && selection->selected() == beforeInf && selection->selectedVehicles() == beforeVeh) {
                if (const auto ctr = selection->selectionCentroid()) {
                    renderer.smoothFocusMap(ctr->first, ctr->second);
                }
            }
        } else if (hr.action == ss2::HudAction::focusGroup && hr.groupDigit >= 0 && hr.groupDigit < 10 && selection) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            selection->recallControlGroup(static_cast<unsigned>(hr.groupDigit), false);
            if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                selectedBuilding.reset();
            }
            prevSelInf = selection->selected();
            prevSelVeh = selection->selectedVehicles();
            if (const auto ctr = selection->selectionCentroid()) {
                renderer.smoothFocusMap(ctr->first, ctr->second);
            }
        } else if (hr.action == ss2::HudAction::selectClass && !hr.classLabel.empty() && selection && scene) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            if (selection->selected().empty() && selection->selectedVehicles().empty() &&
                selectedBuilding && *selectedBuilding < liveBuildings.size() && simulation) {
                for (const size_t sid : liveBuildings[*selectedBuilding].occupantIds) {
                    const std::string uid = simulation->soldierUnitId(sid);
                    if (!uid.empty() && hudDisplayName(uid) == hr.classLabel) {
                        if (simulation->evacuateSingleOccupant(*selectedBuilding, sid)) {
                            playUnitVoiceForId("unload", uid, false);
                            showNotice(ss2::trf("Evacuated 1 {0}.", {ss2::tr(hr.classLabel)}),
                                       ss2::NoticeKind::accepted);
                        }
                        break;
                    }
                }
            } else if (selection->selected().empty() && selection->selectedVehicles().size() == 1u &&
                       simulation && hr.classLabel.rfind("[Cargo] ", 0) == 0) {
                const size_t vid = *selection->selectedVehicles().begin();
                const std::string wantClass = hr.classLabel.substr(8);
                for (const size_t sid : simulation->vehiclePassengerIds(vid)) {
                    const std::string uid = simulation->soldierUnitId(sid);
                    if (!uid.empty() && hudDisplayName(uid) == wantClass) {
                        if (simulation->unloadSinglePassenger(vid, sid)) {
                            playUnitVoiceForId("unload", uid, false);
                            showNotice(ss2::trf("Unloaded 1 {0}.", {ss2::tr(wantClass)}),
                                       ss2::NoticeKind::accepted);
                        }
                        break;
                    }
                }
            } else {
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
                activeSubgroupLabel.clear();
                selection->setSelection(std::move(newInf), std::move(newVeh));
                if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                    selectedBuilding.reset();
                }
            }
        } else if (hr.action == ss2::HudAction::airSupport && hr.airSlot >= 0 && hr.airSlot < 5 && simulation) {
            const auto kind = static_cast<ss2::assets::AirMissionKind>(hr.airSlot);
            if (armedAirSupport && *armedAirSupport == kind) {
                if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
                armedAirSupport.reset();
                resetCursor();
                orderNotice.show(ss2::tr("Air support cancelled."), ss2::NoticeKind::unsupported);
            } else {
                armAirSupportKind(kind);
            }
        } else if (hr.action == ss2::HudAction::speedDown) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            stepGameSpeed(-1);
        } else if (hr.action == ss2::HudAction::speedUp) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            stepGameSpeed(+1);
        } else if (hr.action == ss2::HudAction::pause) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            doTogglePause();
        } else if (hr.action == ss2::HudAction::log) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            openLogMenu();
        } else if (hr.action == ss2::HudAction::menu) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            openPauseMenu();
        } else if (hr.action == ss2::HudAction::mission && selection) {
            if (audio) audio->playGroup("switch", 0.42f, ss2::VoicePriority::High);
            SDL_Event mKey{};
            mKey.type = SDL_EVENT_KEY_DOWN;
            mKey.key.key = SDLK_M;
            selection->handle(mKey);
            std::cout << "Tutorial I HUD Mission marker jump\n";
        }
        if (hr.consumed) {
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                applyTacticalCursor(window, TacticalCursorKind::Field, moveCursor.get(), attackCursor.get());
            }
            return;
        }
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
    if (simulation && !menu.active() && !briefing.active() && event.type == SDL_EVENT_KEY_DOWN &&
        !event.key.repeat && event.key.key == SDLK_Y && (event.key.mod & SDL_KMOD_ALT) != 0) {
        hudHidden = !hudHidden;
        if (hudHidden) {
            if (hud) hud->cancelMinimapDrag();
            renderer.clearOverlay(OverlaySlot::hud);
            renderer.setHudBarHeight(0);
            orderNotice.show(ss2::tr("HUD hidden (Alt+Y to restore)."), ss2::NoticeKind::accepted);
        } else {
            if (hud) hud->forceRedraw();
            orderNotice.show(ss2::tr("HUD restored."), ss2::NoticeKind::accepted);
        }
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
        stepGameSpeed(event.key.key == SDLK_LEFTBRACKET ? -1 : +1);
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
            smokeArmed = false;
            armedAirSupport.reset();
            doStop();
        } else if (event.key.key == SDLK_A) {
            doArmAttackMove();
            return;
        } else if (event.key.key == SDLK_Q) {
            doArmPatrol();
            return;
        } else if (event.key.key == SDLK_Y && !(event.key.mod & SDL_KMOD_ALT)) {
            syncActiveSubgroup();
            const auto vids = activeVehicleSelection();
            const auto sids = activeInfantrySelection();
            if (smokeArmed) {
                smokeArmed = false;
                resetCursor();
            } else if (simulation->canDeploySmoke(sids, vids)) {
                attackMoveArmed = false;
                attackGroundArmed = false;
                reverseMoveArmed = false;
                patrolArmed = false;
                faceArmed = false;
                armedAirSupport.reset();
                smokeArmed = true;
                applyTacticalCursor(window, TacticalCursorKind::AttackGround, moveCursor.get(), attackCursor.get());
                orderNotice.show(ss2::tr("Smoke screen: left-click target area (blocks line of sight)."),
                                 ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("No officer, mortar, or howitzer with smoke rounds selected."),
                                 ss2::NoticeKind::unsupported);
            }
            return;
        } else if (event.key.key == SDLK_R && !selection->selectedVehicles().empty()) {
            attackMoveArmed = false;
            attackGroundArmed = false;
            patrolArmed = false;
            faceArmed = false;
            smokeArmed = false;
            armedAirSupport.reset();
            reverseMoveArmed = !reverseMoveArmed;
            if (reverseMoveArmed) {
                applyTacticalCursor(window, TacticalCursorKind::Move, moveCursor.get(), attackCursor.get());
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
            smokeArmed = false;
            armedAirSupport.reset();
            faceArmed = !faceArmed;
            if (faceArmed) {
                applyTacticalCursor(window, TacticalCursorKind::Rotate, moveCursor.get(), attackCursor.get());
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
            syncActiveSubgroup();
            const auto vids = activeVehicleSelection();
            if (!vids.empty()) {
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
            }
            return;
        } else if (event.key.key == SDLK_H) doToggleHoldFire();
        else if (event.key.key == SDLK_C && !selection->selected().empty()) doToggleCrouch();
        else if (event.key.key == SDLK_B && (!selection->selected().empty() || selectedBuilding)) doToggleBinoculars();
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (!replayFile && simulation && selection && scene && vehicleScene && event.key.key == SDLK_TAB &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            const auto classes = selectionClassOrder();
            if (classes.size() >= 2u) {
                const bool reverse = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                std::size_t nextPos = 0u;
                if (activeSubgroupLabel.empty()) {
                    nextPos = reverse ? (classes.size() - 1u) : 0u;
                    activeSubgroupLabel = classes[nextPos];
                } else {
                    const auto it = std::ranges::find(classes, activeSubgroupLabel);
                    if (it == classes.end()) {
                        activeSubgroupLabel = classes.front();
                    } else {
                        const std::size_t idx = static_cast<std::size_t>(std::distance(classes.begin(), it));
                        if (!reverse) {
                            activeSubgroupLabel = (idx + 1u < classes.size()) ? classes[idx + 1u] : std::string{};
                        } else {
                            activeSubgroupLabel = (idx > 0u) ? classes[idx - 1u] : std::string{};
                        }
                    }
                }
                if (audio) audio->playGroup("switch", 0.35f, ss2::VoicePriority::Low);
                orderNotice.show(
                    activeSubgroupLabel.empty()
                        ? ss2::tr("Active subgroup: All selected classes")
                        : ss2::trf("Active subgroup: {0}", {ss2::tr(activeSubgroupLabel)}),
                    ss2::NoticeKind::accepted);
                return;
            }
        }
        if (simulation && selection && scene && event.key.key == SDLK_HOME &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            if (const auto ctr = selection->selectionCentroid()) {
                renderer.smoothFocusMap(ctr->first, ctr->second);
                return;
            }
        }
        if (simulation && event.key.key == SDLK_HOME &&
            (event.key.mod & SDL_KMOD_ALT) != 0 && !(event.key.mod & SDL_KMOD_CTRL)) {
            renderer.fitMapOverview();
            orderNotice.show(ss2::tr("Camera: full map overview."), ss2::NoticeKind::accepted);
            return;
        }
        if (simulation && (event.key.key == SDLK_BACKSPACE || event.key.key == SDLK_KP_0) &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            renderer.resetZoom();
            orderNotice.show(ss2::tr("Zoom reset (1:1)."), ss2::NoticeKind::accepted);
            return;
        }
        if (simulation && event.key.key == SDLK_KP_PLUS &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            renderer.zoomBy(1.25);
            return;
        }
        if (simulation && event.key.key == SDLK_KP_MINUS &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            renderer.zoomBy(0.8);
            return;
        }
        if (simulation && selection && scene && event.key.key == SDLK_COMMA &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            struct SpecEntry { bool isVehicle{false}; size_t index{0}; };
            std::vector<SpecEntry> specs;
            const auto& units = scene->units();
            for (size_t idx = 0; idx < units.size(); ++idx) {
                const auto& u = units[idx];
                if (u.owner != localOwner() || u.hp <= 0 || !u.visible) continue;
                const auto* sp = unitCatalog ? unitCatalog->find(u.id) : nullptr;
                const bool isSpec = soldierHasBinoculars(u.id) || (idx < unitMineAmmo.size() && unitMineAmmo[idx] > 0) ||
                                    (sp && (sp->healDelay > 0 || sp->selType == "medic" || sp->selType == "sapper" ||
                                            sp->selType == "sappers" || sp->selType == "officer"));
                if (isSpec) specs.push_back({false, idx});
            }
            if (vehicleScene) {
                const auto& vehs = vehicleScene->vehicles();
                for (size_t idx = 0; idx < vehs.size(); ++idx) {
                    const auto& v = vehs[idx];
                    if (!v.alive || v.hp <= 0 || v.owner != localOwner() || !v.visible) continue;
                    const auto* sp = unitCatalog ? unitCatalog->find(v.id) : nullptr;
                    if (sp && (sp->isSupplyTruck() || sp->healDelay > 0 || sp->selType == "ambulance")) {
                        specs.push_back({true, idx});
                    }
                }
            }
            if (specs.empty()) {
                orderNotice.show(ss2::tr("No specialists or support vehicles active."), ss2::NoticeKind::unsupported);
            } else {
                specialistCursor %= specs.size();
                const auto picked = specs[specialistCursor];
                const std::size_t shownIdx = specialistCursor + 1u;
                specialistCursor = (specialistCursor + 1u) % specs.size();
                std::string unitName;
                if (picked.isVehicle && vehicleScene && picked.index < vehicleScene->vehicles().size()) {
                    selection->setSelection({}, {picked.index});
                    unitName = hudDisplayName(vehicleScene->vehicles()[picked.index].id);
                } else if (!picked.isVehicle && picked.index < units.size()) {
                    selection->setSelection({picked.index}, {});
                    unitName = hudDisplayName(units[picked.index].id);
                }
                selectedBuilding.reset();
                activeSubgroupLabel.clear();
                prevSelInf = selection->selected();
                prevSelVeh = selection->selectedVehicles();
                if (const auto ctr = selection->selectionCentroid()) {
                    renderer.smoothFocusMap(ctr->first, ctr->second);
                }
                orderNotice.show(ss2::trf("Specialist {0}/{1}: {2} (, for next)",
                                          {std::to_string(shownIdx), std::to_string(specs.size()),
                                           ss2::tr(unitName)}),
                                 ss2::NoticeKind::accepted);
            }
            return;
        }
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
                if (u.owner != localOwner() || u.hp <= 0 || !u.visible) continue;
                const int maxHp = (idx < initialUnitHp.size() && initialUnitHp[idx] > 0) ? initialUnitHp[idx] : 100;
                const bool dry = (u.ammo <= 0) && isCombatAmmoSoldierId(u.id);
                if (u.hp < maxHp || dry) needy.push_back({false, idx});
            }
            if (vehicleScene) {
                const auto& vehs = vehicleScene->vehicles();
                for (size_t idx = 0; idx < vehs.size(); ++idx) {
                    const auto& v = vehs[idx];
                    if (!v.alive || v.hp <= 0 || v.owner != localOwner() || !v.visible) continue;
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
            renderer.smoothFocusMap(ex, ey);
            eventHistoryCursor = (eventHistoryCursor + 1u) % eventHistory.size();
            return;
        }
    }
    if (simulation && (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed || smokeArmed || armedAirSupport) &&
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        (void)selection->cancelDrag();
        const auto [x, y] = renderer.mapPosition(event.button.x, event.button.y);
        const int simX = int(std::floor(x / mapScale));
        const int simY = int(std::floor(y / mapScale));
        const bool shiftQueue = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        if (armedAirSupport) {
            dispatchAirSupportAt(*armedAirSupport, simX, simY);
            armedAirSupport.reset();
        } else if (reverseMoveArmed) {
            const auto& vsel = selection->selectedVehicles();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            const auto res = shiftQueue
                ? std::optional<size_t>(simulation->queueVehicleOrder(
                      vids, ss2::QueuedVehicleOp::ReverseMove, simX, simY))
                : simulation->reverseMoveVehicles(vids, simX, simY);
            if (res && *res > 0) {
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                pushOrderPulse(simX, simY, OrderPulse::Kind::Move);
                playSelectionVoice("move");
                showNotice(ss2::trf(shiftQueue ? "Queued reverse move ({0} vehicles)."
                                               : "Reverse move ordered ({0} vehicles).",
                                    {std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Selected vehicles cannot reverse."), ss2::NoticeKind::blocked);
            }
            reverseMoveArmed = false;
        } else if (faceArmed) {
            const auto& vsel = selection->selectedVehicles();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            const auto res = shiftQueue
                ? std::optional<size_t>(simulation->queueVehicleOrder(
                      vids, ss2::QueuedVehicleOp::FaceDir, simX, simY))
                : simulation->faceVehicles(vids, simX, simY);
            if (res && *res > 0) {
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                pushOrderPulse(simX, simY, OrderPulse::Kind::Move);
                playSelectionVoice("move");
                showNotice(ss2::trf(shiftQueue ? "Queued facing rotation ({0} vehicles)."
                                               : "Rotating front armor ({0} vehicles).",
                                    {std::to_string(*res)}),
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
                if (shiftQueue) {
                    totalBombard += simulation->queueVehicleOrder(
                        vids, ss2::QueuedVehicleOp::AttackGround, simX, simY);
                } else if (const auto res = simulation->attackGroundVehicles(vids, simX, simY)) {
                    totalBombard += *res;
                }
            }
            if (!sids.empty()) {
                if (shiftQueue) {
                    totalBombard += simulation->queueSoldierOrder(
                        sids, ss2::QueuedSoldierOp::AttackGround, simX, simY);
                } else if (const auto res = simulation->attackGroundSoldiers(sids, simX, simY)) {
                    totalBombard += *res;
                }
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
                pushOrderPulse(simX, simY, OrderPulse::Kind::Attack);
                playSelectionVoice("attack");
                showNotice(ss2::trf(shiftQueue ? "Queued attack ground ({0} units)."
                                               : "Attack ground ordered ({0} units).",
                                    {std::to_string(totalBombard)}),
                           ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Selected units cannot bombard that location."), ss2::NoticeKind::blocked);
            }
            attackGroundArmed = false;
        } else if (patrolArmed) {
            dispatchPatrolAt(simX, simY);
            patrolArmed = false;
        } else if (smokeArmed) {
            const auto vids = activeVehicleSelection();
            const auto sids = activeInfantrySelection();
            if (simulation->deploySmoke(sids, vids, simX, simY) > 0) {
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{(simX - 10) * mapScale, (simY - 10) * mapScale, 20 * mapScale, 20 * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                pushOrderPulse(simX, simY, OrderPulse::Kind::Support);
                if (combatEffects && combatVfx.atlasSlot().has_value()) {
                    combatVfx.spawnClip(CombatVfxClip::BoxDust, static_cast<float>(simX), static_cast<float>(simY),
                                        0u, 1.0f, 1.35f, 0.72f, 200050);
                    syncGpuSprites();
                }
                if (audio) audio->playGroup("lightfire", 0.48f, ss2::VoicePriority::Normal);
                showNotice(ss2::tr("Tactical smoke screen deployed."), ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Smoke screen out of range or out of smoke rounds."),
                                 ss2::NoticeKind::blocked);
            }
            smokeArmed = false;
        } else {
            const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            (void)issueGroundOrder(simX, simY, true, append);
            attackMoveArmed = false;
        }
        resetCursor();
        return;
    }
    if (simulation && !menu.active() && !briefing.active() && !mpChatActive &&
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT &&
        (SDL_GetModState() & SDL_KMOD_ALT) != 0) {
        (void)selection->cancelDrag();
        const auto [x, y] = renderer.mapPosition(event.button.x, event.button.y);
        const int simX = std::clamp(static_cast<int>(std::floor(x / mapScale)), 0, static_cast<int>(hudMapW > 0 ? hudMapW - 1 : 0));
        const int simY = std::clamp(static_cast<int>(std::floor(y / mapScale)), 0, static_cast<int>(hudMapH > 0 ? hudMapH - 1 : 0));
        if (multiplayer.enabled) {
            placeMpBeacon(simX, simY);
        } else {
            pushMinimapPing(simX, simY, false);
            recordEventPos(static_cast<double>(simX * mapScale), static_cast<double>(simY * mapScale));
            if (audio) audio->playGroup("e_telegram", 0.42f, ss2::VoicePriority::Normal);
            orderNotice.show(ss2::tr("Tactical ping placed on map."), ss2::NoticeKind::accepted);
        }
        return;
    }
    if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
        if (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed || smokeArmed || armedAirSupport) {
            attackMoveArmed = false;
            attackGroundArmed = false;
            reverseMoveArmed = false;
            patrolArmed = false;
            faceArmed = false;
            smokeArmed = false;
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
            if (!sb.destroyed && sb.owner == localOwner() && sb.occupantCount > 0) {
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
                    pushOrderPulse(simX, simY, OrderPulse::Kind::Attack);
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
                    pushOrderPulse(tv.x, tv.y, OrderPulse::Kind::Move);
                    playSelectionVoice("move");
                    showNotice(ss2::trf("Hitching {0}...", {hudDisplayName(vehicleScene->vehicles().at(gIdx).id)}),
                               ss2::NoticeKind::accepted);
                    return;
                }
            }
        }
        if (!vids.empty() && targetVeh && vehicleScene &&
            vehicleScene->vehicles().at(*targetVeh).owner == localOwner() &&
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
                pushOrderPulse(tv.x, tv.y, OrderPulse::Kind::Support);
                playSelectionVoice("move");
                showNotice(ss2::trf("Supply truck servicing {0}...", {ss2::tr(hudDisplayName(tv.id))}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if ((!ids.empty() || !vids.empty()) && targetInf &&
            scene->units().at(*targetInf).owner == localOwner() &&
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
                pushOrderPulse(tu.x, tu.y, OrderPulse::Kind::Support);
                playSelectionVoice("move");
                showNotice(ss2::tr("Moving to heal wounded soldier."), ss2::NoticeKind::accepted);
                return;
            }
        }
        const bool shiftQueue = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        if (!ids.empty() && targetVeh && vehicleScene &&
            (vehicleScene->vehicles().at(*targetVeh).owner == 3 ||
             isAllyOwner(vehicleScene->vehicles().at(*targetVeh).owner)) &&
            simulation->canCaptureVehicle(*targetVeh)) {
            const auto res = shiftQueue
                ? std::optional<size_t>(simulation->queueSoldierOrder(
                      ids, ss2::QueuedSoldierOp::CaptureVehicle, 0, 0, *targetVeh))
                : simulation->captureVehicle(ids, *targetVeh);
            if (res && *res > 0) {
                const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                const bool isFriendlyRecrew = (tv.owner != 3 && isAllyOwner(tv.owner));
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
                pushOrderPulse(tv.x, tv.y, OrderPulse::Kind::Move);
                playSelectionVoice("move");
                showNotice(ss2::trf(isFriendlyRecrew
                                        ? "Re-crewing vehicle ({0} soldiers)..."
                                        : "Capturing vehicle ({0} soldiers)...",
                                    {std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if (!ids.empty() && targetVeh && vehicleScene &&
            vehicleScene->vehicles().at(*targetVeh).owner == localOwner() &&
            simulation->canBoardVehicle(*targetVeh)) {
            const auto res = shiftQueue
                ? std::optional<size_t>(simulation->queueSoldierOrder(
                      ids, ss2::QueuedSoldierOp::BoardVehicle, 0, 0, *targetVeh))
                : simulation->boardVehicle(ids, *targetVeh);
            if (res && *res > 0) {
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
                pushOrderPulse(tv.x, tv.y, OrderPulse::Kind::Move);
                playSelectionVoice("move");
                showNotice(ss2::trf("Boarding transport ({0} soldiers).", {std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if ((!ids.empty() || !vids.empty()) && targetVeh && vehicleScene &&
            vehicleScene->vehicles().at(*targetVeh).owner == localOwner() &&
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
                pushOrderPulse(tv.x, tv.y, OrderPulse::Kind::Move);
                playSelectionVoice("move");
                showNotice(ss2::trf("Escorting {0} ({1} units).",
                                    {ss2::tr(hudDisplayName(tv.id)), std::to_string(*res)}),
                           ss2::NoticeKind::accepted);
                return;
            }
        }
        if ((!ids.empty() || !vids.empty()) && targetInf &&
            scene->units().at(*targetInf).owner == localOwner() &&
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
                pushOrderPulse(tu.x, tu.y, OrderPulse::Kind::Move);
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
                pushOrderPulse(u.x, u.y, OrderPulse::Kind::Attack);
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
                pushOrderPulse(tv.x, tv.y, OrderPulse::Kind::Attack);
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
                    const auto res = shiftQueue
                        ? std::optional<size_t>(simulation->queueSoldierOrder(
                              ids, ss2::QueuedSoldierOp::GarrisonBuilding, 0, 0, *targetBld))
                        : simulation->garrisonBuilding(ids, *targetBld);
                    if (res && *res > 0) {
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        pushOrderPulse(lb.x, lb.y, OrderPulse::Kind::Move);
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
                        pushOrderPulse(lb.x, lb.y, OrderPulse::Kind::Support);
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
                        pushOrderPulse(lb.x, lb.y, OrderPulse::Kind::Attack);
                        playSelectionVoice("attack");
                        showNotice(ss2::tr("Attacking occupied building!"), ss2::NoticeKind::accepted);
                        return;
                    }
                }
            }
            if (!vids.empty()) {
                if (const auto targetBr = bridgeAtMap(simX, simY);
                    targetBr && simulation->canSupportBridge(vids, *targetBr)) {
                    if (const auto res = simulation->orderSupportBridge(vids, *targetBr); res && *res > 0) {
                        const auto& lb = liveBridges[*targetBr];
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        pushOrderPulse(lb.x, lb.y, OrderPulse::Kind::Support);
                        playSelectionVoice("move");
                        showNotice(ss2::tr("Supply truck repairing bridge..."), ss2::NoticeKind::accepted);
                        return;
                    }
                }
            }
            if (!ids.empty() && simulation->canDisarmMineAt(ids, simX, simY)) {
                const auto res = shiftQueue
                    ? std::optional<size_t>(simulation->queueSoldierOrder(
                          ids, ss2::QueuedSoldierOp::DisarmMine, simX, simY))
                    : simulation->orderDisarmMine(ids, simX, simY);
                if (res && *res > 0) {
                    std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                    activeMarkers.push_back({
                        {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                         ss2::MarkerKind::move},
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                        std::nullopt,
                        true
                    });
                    syncMarkers();
                    pushOrderPulse(simX, simY, OrderPulse::Kind::Move);
                    playSelectionVoice("move");
                    showNotice(ss2::trf("Sapper clearing mines ({0} soldiers)...", {std::to_string(*res)}),
                               ss2::NoticeKind::accepted);
                    return;
                }
            }
        }
        const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        const bool convoyMod = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
        if (convoyMod && !vids.empty() && (ids.size() + vids.size()) >= 2u) {
            const size_t moved = simulation->convoyMove(ids, vids, simX, simY, append);
            if (moved > 0u) {
                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                activeMarkers.push_back({
                    {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                     ss2::MarkerKind::move},
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                    std::nullopt,
                    true
                });
                syncMarkers();
                pushOrderPulse(simX, simY, OrderPulse::Kind::Move);
                playSelectionVoice("move");
                showNotice(ss2::trf("Convoy move ({0} units matching slowest speed).", {std::to_string(moved)}),
                           ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::tr("Convoy move unavailable for that destination."), ss2::NoticeKind::blocked);
            }
            return;
        }
        if (issueGroundOrder(simX, simY, false, append) && (!ids.empty() || !vids.empty()) && !append) {
            rightDragActive = true;
            rightDragStartScreenX = rightDragCurScreenX = event.button.x;
            rightDragStartScreenY = rightDragCurScreenY = event.button.y;
        }
    }
    if (rightDragActive && event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & SDL_BUTTON_RMASK)) {
        rightDragCurScreenX = event.motion.x;
        rightDragCurScreenY = event.motion.y;
        return;
    }
    if (rightDragActive && event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
        rightDragActive = false;
        const float dragDist = std::hypot(event.button.x - rightDragStartScreenX, event.button.y - rightDragStartScreenY);
        if (simulation && selection && dragDist >= 28.0f &&
            (!selection->selected().empty() || !selection->selectedVehicles().empty())) {
            const auto [p0x, p0y] = renderer.mapPosition(rightDragStartScreenX, rightDragStartScreenY);
            const auto [p1x, p1y] = renderer.mapPosition(event.button.x, event.button.y);
            const int x0 = int(std::floor(p0x / mapScale)), y0 = int(std::floor(p0y / mapScale));
            const int x1 = int(std::floor(p1x / mapScale)), y1 = int(std::floor(p1y / mapScale));
            const double dx = double(x1 - x0), dy = double(y1 - y0);
            const std::size_t nInf = selection->selected().size();
            const std::size_t nVeh = selection->selectedVehicles().size();
            const std::size_t totalUnits = nInf + nVeh;
            if (totalUnits > 0u) {
                std::size_t slot = 0u;
                const auto slotPos = [&](std::size_t k) {
                    const double frac = totalUnits == 1u ? 0.5 : static_cast<double>(k) / static_cast<double>(totalUnits - 1u);
                    return std::pair{
                        std::clamp(static_cast<int>(std::lround(double(x0) + dx * frac)), 0, std::max(0, int(hudMapW) - 1)),
                        std::clamp(static_cast<int>(std::lround(double(y0) + dy * frac)), 0, std::max(0, int(hudMapH) - 1))
                    };
                };
                for (const size_t sid : selection->selected()) {
                    const auto [tx, ty] = slotPos(slot++);
                    (void)simulation->move(std::array<size_t, 1>{sid}, tx, ty, false);
                }
                const int fx = static_cast<int>(std::lround(dy));
                const int fy = static_cast<int>(std::lround(-dx));
                for (const size_t vid : selection->selectedVehicles()) {
                    const auto [tx, ty] = slotPos(slot++);
                    const std::array<size_t, 1> one{vid};
                    (void)simulation->moveVehicles(one, tx, ty, false);
                    (void)simulation->faceVehicles(one, tx + fx, ty + fy);
                }
                showNotice(ss2::trf("Battle line formation ({0} units).", {std::to_string(totalUnits)}),
                           ss2::NoticeKind::accepted);
            }
            return;
        }
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
        const auto hoverBridge = (!targetInf && !targetVeh && !hoverBld) ? bridgeAtMap(simX, simY) : std::nullopt;
        const bool hasAny = !ids.empty() || !pickedVeh.empty();
        const bool hasGarrisonCmd = !hasAny && selectedBuilding &&
                                    *selectedBuilding < liveBuildings.size() &&
                                    !liveBuildings[*selectedBuilding].destroyed &&
                                    liveBuildings[*selectedBuilding].owner == localOwner() &&
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
             vehicleScene->vehicles().at(*targetVeh).owner == localOwner() &&
             simulation->canSupportVehicle(pvids, *targetVeh)) ||
            (!pvids.empty() && hoverBld && simulation->canSupportBuilding(pvids, *hoverBld)) ||
            (!pvids.empty() && hoverBridge && simulation->canSupportBridge(pvids, *hoverBridge)) ||
            (hasAny && targetInf && scene->units().at(*targetInf).owner == 0 &&
             simulation->canHealSoldier(ids, pvids, *targetInf));
        const bool canEscortHover =
            (hasAny && targetVeh && vehicleScene &&
             vehicleScene->vehicles().at(*targetVeh).owner == localOwner() &&
             simulation->canEscortVehicle(ids, pvids, *targetVeh)) ||
            (hasAny && targetInf && scene->units().at(*targetInf).owner == 0 &&
             simulation->canEscortSoldier(ids, pvids, *targetInf));
        const bool canDisarmHover =
            !ids.empty() && !targetInf && !targetVeh && simulation->canDisarmMineAt(ids, simX, simY);
        const bool canPickupHover =
            !ids.empty() && targetVeh && vehicleScene &&
            (((vehicleScene->vehicles().at(*targetVeh).owner == 3 ||
               isAllyOwner(vehicleScene->vehicles().at(*targetVeh).owner)) &&
              simulation->canCaptureVehicle(*targetVeh)) ||
             (vehicleScene->vehicles().at(*targetVeh).owner == localOwner() &&
              simulation->canBoardVehicle(*targetVeh)));
        if (armedAirSupport) {
            const TacticalCursorKind airCur =
                *armedAirSupport == ss2::assets::AirMissionKind::Bomber ? TacticalCursorKind::AirBomb :
                *armedAirSupport == ss2::assets::AirMissionKind::Recon ? TacticalCursorKind::AirSpy :
                *armedAirSupport == ss2::assets::AirMissionKind::Paratrooper ? TacticalCursorKind::AirPara :
                TacticalCursorKind::AirFly;
            applyTacticalCursor(window, airCur, moveCursor.get(), attackCursor.get());
        } else if (attackGroundArmed) {
            bool anyRocket = false;
            bool anyHowitzer = scene && std::ranges::any_of(picked, [&](size_t id) {
                return id < scene->units().size() && isBombardInfantryId(scene->units()[id].id);
            });
            if (unitCatalog && vehicleScene) {
                for (const size_t vid : pickedVeh) {
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
        } else if (smokeArmed) {
            applyTacticalCursor(window, TacticalCursorKind::AttackGround, moveCursor.get(), attackCursor.get());
        } else if (attackMoveArmed || patrolArmed) {
            applyTacticalCursor(window, TacticalCursorKind::AttackMove, moveCursor.get(), attackCursor.get());
        } else if (validInfTarget || validVehTarget || validBldTarget) {
            applyTacticalCursor(window, TacticalCursorKind::AttackUnit, moveCursor.get(), attackCursor.get());
        } else if (faceArmed) {
            applyTacticalCursor(window, TacticalCursorKind::Rotate, moveCursor.get(), attackCursor.get());
        } else if (reverseMoveArmed) {
            applyTacticalCursor(window, TacticalCursorKind::Move, moveCursor.get(), attackCursor.get());
        } else if (canTowHover) {
            applyTacticalCursor(window, TacticalCursorKind::TowGun, moveCursor.get(), attackCursor.get());
        } else if (canPickupHover) {
            applyTacticalCursor(window, TacticalCursorKind::Pickup, moveCursor.get(), attackCursor.get());
        } else if (canSupportHover || canEscortHover || canDisarmHover) {
            TacticalCursorKind supCur = TacticalCursorKind::Move;
            if (!pvids.empty() && hoverBridge && simulation->canSupportBridge(pvids, *hoverBridge)) {
                supCur = TacticalCursorKind::FixBridge;
            } else if (hasAny && targetInf && scene->units().at(*targetInf).owner == 0 &&
                       simulation->canHealSoldier(ids, pvids, *targetInf)) {
                supCur = TacticalCursorKind::Heal;
            } else if (canDisarmHover) {
                supCur = TacticalCursorKind::Mine;
            } else if (canSupportHover) {
                supCur = TacticalCursorKind::Reload;
            }
            applyTacticalCursor(window, supCur, moveCursor.get(), attackCursor.get());
        } else if (canGarrisonHover) {
            applyTacticalCursor(window, TacticalCursorKind::EnterBuilding, moveCursor.get(), attackCursor.get());
        } else if ((!ids.empty() && simulation->canMove(ids, simX, simY)) || !pickedVeh.empty()) {
            applyTacticalCursor(window, TacticalCursorKind::Move, moveCursor.get(), attackCursor.get());
        } else {
            TacticalCursorKind idleCur = TacticalCursorKind::Field;
            int hoverOwner = -1;
            if (targetInf && scene && *targetInf < scene->units().size()) {
                hoverOwner = scene->units()[*targetInf].owner;
            } else if (targetVeh && vehicleScene && *targetVeh < vehicleScene->vehicles().size()) {
                hoverOwner = vehicleScene->vehicles()[*targetVeh].owner;
            } else if (hoverBld && *hoverBld < liveBuildings.size() && liveBuildings[*hoverBld].occupantCount > 0) {
                hoverOwner = liveBuildings[*hoverBld].owner;
            }
            if (hoverOwner >= 0) {
                idleCur = (hoverOwner == localOwner()) ? TacticalCursorKind::OwnUnit
                        : isHostileOwner(hoverOwner)   ? TacticalCursorKind::EnemyUnit
                                                       : TacticalCursorKind::FriendUnit;
            } else if (hasAny) {
                idleCur = TacticalCursorKind::Forbidden;
            }
            applyTacticalCursor(window, idleCur, moveCursor.get(), attackCursor.get());
        }
    }
    if (selection) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.mod & SDL_KMOD_CTRL) && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) {
            const auto grpIdx = static_cast<std::size_t>(event.key.key - SDLK_0);
            const bool append = (event.key.mod & SDL_KMOD_SHIFT) != 0;
            if (!append) {
                hudGroups[grpIdx] = selection->selected();
                hudVehicleGroups[grpIdx] = selection->selectedVehicles();
            } else {
                if (scene) {
                    const auto& units = scene->units();
                    std::erase_if(hudGroups[grpIdx], [&](size_t i) {
                        return i >= units.size() || units[i].hp <= 0 || units[i].owner != localOwner();
                    });
                }
                if (vehicleScene) {
                    const auto& vehs = vehicleScene->vehicles();
                    std::erase_if(hudVehicleGroups[grpIdx], [&](size_t i) {
                        return i >= vehs.size() || !vehs[i].alive || vehs[i].hp <= 0 || vehs[i].owner != localOwner();
                    });
                }
                hudGroups[grpIdx].insert(selection->selected().begin(), selection->selected().end());
                hudVehicleGroups[grpIdx].insert(selection->selectedVehicles().begin(),
                                                selection->selectedVehicles().end());
            }
            const std::size_t total = hudGroups[grpIdx].size() + hudVehicleGroups[grpIdx].size();
            if (audio) audio->playGroup("switch", 0.30f, ss2::VoicePriority::Low);
            if (total == 0u) {
                orderNotice.show(ss2::trf("Group {0} cleared.", {std::to_string(grpIdx)}),
                                 ss2::NoticeKind::accepted);
            } else if (append) {
                orderNotice.show(ss2::trf("Added to group {0} ({1} units).",
                                          {std::to_string(grpIdx), std::to_string(total)}),
                                 ss2::NoticeKind::accepted);
            } else {
                orderNotice.show(ss2::trf("Group {0} assigned ({1} units).",
                                          {std::to_string(grpIdx), std::to_string(total)}),
                                 ss2::NoticeKind::accepted);
            }
        }
        selection->handle(event);
        if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
            selectedBuilding.reset();
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
                   !liveBuildings.empty()) {
            const auto [bx, by] = renderer.mapPosition(event.button.x, event.button.y);
            const auto clickedBld = buildingAtMap(int(std::floor(bx / mapScale)), int(std::floor(by / mapScale)));
            if (clickedBld && liveBuildings[*clickedBld].owner == localOwner() && liveBuildings[*clickedBld].occupantCount > 0) {
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
            if (!grewOrChanged && (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed || smokeArmed)) {
                attackMoveArmed = false;
                attackGroundArmed = false;
                reverseMoveArmed = false;
                patrolArmed = false;
                faceArmed = false;
                smokeArmed = false;
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
