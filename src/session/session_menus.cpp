#include "session/game_session.hpp"
#include "session/save_file.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace ss2 {
namespace {
void ensureRetailMenuSkin(Menu& menu, const std::filesystem::path& gameDir) {
    if (!gameDir.empty() && !menu.hasRetailSkin()) {
        menu.loadRetailAssets(gameDir);
    }
}

std::string canonicalCampaignKey(const std::filesystem::path& path) {
    if (path.empty()) return "TUTORIAL1";
    std::string key = ss2::normalizeMissionKey(path.stem().string());
    std::string ext = path.extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".ssc" && key.size() >= 2u && key.back() == 'E') {
        const bool hasDigitBeforeE = std::isdigit(static_cast<unsigned char>(key[key.size() - 2u])) != 0;
        std::string parentName = path.parent_path().filename().string();
        for (char& ch : parentName) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (hasDigitBeforeE || parentName == "easy" || parentName == "easy_ssc") {
            key.pop_back();
        }
    }
    return key;
}

double difficultyScoreWeight(int difficulty, bool isMultiplayer) noexcept {
    if (isMultiplayer) return 1.0;
    if (difficulty <= 1) return 0.75;
    if (difficulty >= 3) return 1.25;
    return 1.0;
}
} // namespace

std::vector<MenuItem> GameSession::buildPauseItems() {
    ensureRetailMenuSkin(menu, gameDir);
    std::vector<ss2::MenuItem> items;
    items.push_back({"resume", ss2::tr("Resume"), "", true});
    if (simulation) {
        int airReady = 0;
        for (const auto& s : simulation->playerAirSupport()) airReady += s.ready;
        if (airReady > 0) {
            items.push_back({"airsupport", ss2::tr("Air Support"), std::to_string(airReady), true});
        }
        if (multiplayer.enabled) {
            const std::uint8_t nat = multiplayer.playerNations[localOwner() & 3];
            std::string natVal = ss2::tr(mpNationName(nat)) + " (#" + std::to_string(localOwner() + 1) + ")";
            if (multiplayer.playerCount > 2) {
                natVal += " T" + std::to_string(missionPlayerTeams[localOwner() & 3] + 1);
            }
            items.push_back({"", ss2::tr("Nation"), natVal, false});
            if (mpRttMs >= 0) {
                items.push_back({"", "RTT", std::to_string(mpRttMs) + " ms", false});
            }
            if (multiplayer.playerCount > 2) {
                for (int s = 0; s < std::min(multiplayer.playerCount, 4); ++s) {
                    const std::string pName = !multiplayer.playerNames[s].empty()
                        ? multiplayer.playerNames[s]
                        : ("Player " + std::to_string(s + 1));
                    const std::string pVal = ss2::tr(mpNationName(multiplayer.playerNations[s])) +
                        " • T" + std::to_string(missionPlayerTeams[s] + 1);
                    items.push_back({"", "#" + std::to_string(s + 1) + " " + pName, pVal, false});
                }
            }
        } else {
            items.push_back({"quicksave", ss2::tr("Quick Save"), "F9", true});
            items.push_back({"quickload", ss2::tr("Quick Load"), "F10", true});
            items.push_back({"saveload", ss2::tr("Save / Load..."), "", true});
        }
        items.push_back({"forces", ss2::tr("Force Status"), "O", true});
        items.push_back({"briefing", ss2::tr("Mission Briefing"), "J", true});
    }
    if (!multiplayer.enabled && !gameDir.empty() && nextMissionOut) {
        items.push_back({"campaigns", ss2::tr("Campaigns"), "", true});
        items.push_back({"missions", ss2::tr("Missions"), "", true});
    }
    items.push_back({"enhanced", ss2::tr("Enhanced graphics"), ss2::tr(enhancedGraphics ? "On" : "Off"), true});
    items.push_back({"smooth", ss2::tr("Smooth terrain"), ss2::tr(smoothTerrain ? "On" : "Off"), true});
    if (audio) {
        items.push_back({"music", ss2::tr("Music"), ss2::tr(audio->musicPlaying() ? "On" : "Muted"), true});
        const int sfxPct = static_cast<int>(std::lround(audio->sfxVolume() * 100.0f));
        const std::string sfxLbl = sfxPct <= 0 ? ss2::tr("Muted") : (std::to_string(sfxPct) + "%");
        items.push_back({"sfx_volume", ss2::tr("SFX volume"), sfxLbl, true});
        const float mg = audio->musicVolume();
        const std::string musLbl = mg >= 0.34f ? "100%" : mg >= 0.23f ? "75%" : mg >= 0.13f ? "50%" : "25%";
        items.push_back({"music_volume", ss2::tr("Music volume"), musLbl, true});
        items.push_back({"next_track", ss2::tr("Next soundtrack track"), "N", true});
    }
    items.push_back({"effects", ss2::tr("Tracers and flashes"), ss2::tr(combatEffects ? "On" : "Off"), true});
    if (!multiplayer.enabled && !replayFile) {
        items.push_back({"doctrine", ss2::tr("Tactical Doctrine"),
                         ss2::tr(settings.realismDoctrine ? "Realism (RWM/LRM)" : "Classic Gold"), true});
    }
    if (!multiplayer.enabled && simulation) {
        const int sp = simulation->speedPercent();
        const std::string spStr = sp == 50 ? "0.5x" : sp == 200 ? "2x" : sp == 300 ? "3x" : "1x";
        items.push_back({"speed", ss2::tr("Game speed"), spStr, true});
    }
    if (!multiplayer.enabled) {
        const char* diffKey = settings.difficulty == 1 ? "Easy" : settings.difficulty == 3 ? "Hard" : "Normal";
        items.push_back({"difficulty", ss2::tr("Difficulty"), ss2::tr(diffKey), true});
    }
    {
        // Each language is named in itself so it can be found whatever is showing.
        const auto current = ss2::language();
        items.push_back({"language", ss2::tr("Language"),
                         current == ss2::Language::french ? "Français" : current == ss2::Language::spanish ? "Español" : "English", true});
    }
    items.push_back({"controls", ss2::tr("Controls"), "", true});
    items.push_back({"log", ss2::tr("Message log"), "L", true});
    if (multiplayer.enabled && simulation) {
        items.push_back({"resign", ss2::tr("Surrender"), "", true});
    }
    items.push_back({"quit", ss2::tr("Quit to desktop"), "", true});
    return items;
}

void GameSession::dismissPostMissionScreen() {
    if (postMissionScreen && postMissionScreen->active()) {
        postMissionScreen->clear(renderer);
        renderer.setSpriteSharpness(enhancedGraphics ? 1.0f : 0.0f);
    }
}

void GameSession::syncPostMissionMenuOverlay() {
    if (!postMissionScreen || !postMissionScreen->active()) return;
    menu.draw(renderer);
    renderer.clearOverlay(OverlaySlot::menu);
}

void GameSession::openAirSupportMenu() {
    if (!simulation) return;
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    if (audio) audio->playGroup("choose", 0.42f, ss2::VoicePriority::High);
    const auto slots = simulation->playerAirSupport();
    std::vector<ss2::MenuItem> items;
    items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
    for (std::size_t k = 0; k < slots.size(); ++k) {
        const auto kind = static_cast<ss2::assets::AirMissionKind>(k);
        const std::string tag = slots[k].planeId.empty()
            ? std::to_string(slots[k].ready)
            : (slots[k].planeId + " x" + std::to_string(slots[k].ready));
        items.push_back({"aircall:" + std::to_string(k), ss2::tr(airKindLabel(kind)), tag, slots[k].ready > 0});
    }
    menu.open(ss2::tr("Air Support"), std::move(items));
}

void GameSession::openPauseMenu() {
    if (briefing.active()) return;
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    attackMoveArmed = false;
    attackGroundArmed = false;
    reverseMoveArmed = false;
    patrolArmed = false;
    faceArmed = false;
    smokeArmed = false;
    armedAirSupport.reset();
    resetCursor();
    if (simulation && !paused && !multiplayer.enabled) {
        paused = simulation->togglePause();
        menuPausedClock = true;
    } else {
        menuPausedClock = false;
    }
    if (audio) {
        audio->stopAllLoops();
        audio->playGroup("push", 0.42f, ss2::VoicePriority::High);
    }
    menu.open(ss2::tr("Paused"), buildPauseItems());
}

void GameSession::closeMenu() {
    dismissPostMissionScreen();
    if (menu.active() && audio) audio->playGroup("pull", 0.42f, ss2::VoicePriority::High);
    menu.close();
    if (simulation && menuPausedClock && paused) {
        paused = simulation->togglePause();
    }
    menuPausedClock = false;
}

void GameSession::openMissionsMenu() {
    if (gameDir.empty()) return;
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    menuMissions = ss2::assets::listSinglePlayerMissions(gameDir);
    std::vector<ss2::MenuItem> items;
    items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
    for (std::size_t i = 0; i < menuMissions.size(); ++i) {
        const auto& m = menuMissions[i];
        const char* schemeTag = m.scheme == 1 ? " Winter" : m.scheme == 2 ? " Beach" : m.scheme == 3 ? " Desert" : " Summer";
        std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) + schemeTag;
        const std::string mKey = ss2::normalizeMissionKey(m.path.stem().string());
        if (settings.completedMissions.contains(mKey)) {
            tag = ss2::tr("Completed") + " | " + tag;
        }
        if (const auto sit = settings.missionHighScores.find(mKey);
            sit != settings.missionHighScores.end() && sit->second > 0) {
            tag += " | " + std::to_string(sit->second);
        }
        items.push_back({"mission:" + std::to_string(i), m.name, tag, true});
    }
    menu.open(ss2::tr("Single-Player Missions"), std::move(items));
}

void GameSession::openCampaignsMenu() {
    if (gameDir.empty()) return;
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    menuCampaigns = ss2::assets::listCampaigns(gameDir);
    std::vector<ss2::MenuItem> items;
    items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
    for (std::size_t i = 0; i < menuCampaigns.size(); ++i) {
        const auto& c = menuCampaigns[i];
        std::size_t done = 0;
        for (const auto& m : c.missions) {
            const std::string codeKey = ss2::normalizeMissionKey(m.code);
            const std::string stemKey = canonicalCampaignKey(m.path);
            const std::string easyKey = !m.easyPath.empty() ? canonicalCampaignKey(m.easyPath) : std::string{};
            const std::string rawEasyKey = !m.easyPath.empty() ? ss2::normalizeMissionKey(m.easyPath.stem().string()) : std::string{};
            if (settings.completedMissions.contains(codeKey) ||
                settings.completedMissions.contains(stemKey) ||
                (!easyKey.empty() && settings.completedMissions.contains(easyKey)) ||
                (!rawEasyKey.empty() && settings.completedMissions.contains(rawEasyKey))) {
                ++done;
            }
        }
        items.push_back({"campaign:" + std::to_string(i), c.name,
                         std::to_string(done) + "/" + std::to_string(c.missions.size()), true});
    }
    if (menuCampaigns.empty()) {
        items.push_back({"", ss2::tr("No campaigns installed."), "", false});
    }
    menu.open(ss2::tr("Campaigns"), std::move(items));
}

void GameSession::openCampaignMissionsMenu(std::size_t campaignIdx) {
    if (campaignIdx >= menuCampaigns.size()) return;
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    const auto& c = menuCampaigns[campaignIdx];
    std::vector<ss2::MenuItem> items;
    items.push_back({"back_campaigns", ss2::tr("Back to campaigns"), "", true});
    for (std::size_t i = 0; i < c.missions.size(); ++i) {
        const auto& m = c.missions[i];
        const char* schemeTag = m.scheme == 1 ? " Winter" : m.scheme == 2 ? " Beach" : m.scheme == 3 ? " Desert" : " Summer";
        std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) + schemeTag;
        const std::string codeKey = ss2::normalizeMissionKey(m.code);
        const std::string stemKey = canonicalCampaignKey(m.path);
        const std::string easyKey = !m.easyPath.empty() ? canonicalCampaignKey(m.easyPath) : std::string{};
        const std::string rawEasyKey = !m.easyPath.empty() ? ss2::normalizeMissionKey(m.easyPath.stem().string()) : std::string{};
        if (settings.completedMissions.contains(codeKey) ||
            settings.completedMissions.contains(stemKey) ||
            (!easyKey.empty() && settings.completedMissions.contains(easyKey)) ||
            (!rawEasyKey.empty() && settings.completedMissions.contains(rawEasyKey))) {
            tag = ss2::tr("Completed") + " | " + tag;
        }
        int bestScore = 0;
        for (const auto& candidate : {codeKey, stemKey, easyKey, rawEasyKey}) {
            if (candidate.empty()) continue;
            if (const auto sit = settings.missionHighScores.find(candidate); sit != settings.missionHighScores.end()) {
                bestScore = std::max(bestScore, sit->second);
            }
        }
        if (bestScore > 0) {
            tag += " | " + std::to_string(bestScore);
        }
        items.push_back({"cmission:" + std::to_string(campaignIdx) + ":" + std::to_string(i), m.title, tag, true});
    }
    menu.open(c.name, std::move(items));
}

void GameSession::openControlsMenu() {
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    menu.open(ss2::tr("Controls"), {
        {"back", ss2::tr("Back to pause menu"), "", true},
        {"", ss2::tr("Selection & Orders"), "", false},
        {"", ss2::tr("Select / box select"), ss2::tr("Left click / drag"), false},
        {"", ss2::tr("Select class on screen / all"), ss2::tr("Double-click / Ctrl+DblClick"), false},
        {"", ss2::tr("Select on-screen / all units"), "Ctrl+A / Ctrl+Shift+A", false},
        {"", ss2::tr("Move / board / hitch / garrison"), ss2::tr("Right click"), false},
        {"", ss2::tr("Escort / guard friendly unit"), ss2::tr("Right click friendly"), false},
        {"", ss2::tr("Attack-move"), "A", false},
        {"", ss2::tr("Patrol route"), "Q + click", false},
        {"", ss2::tr("Face direction (rotate front armor)"), "T + click", false},
        {"", ss2::tr("Reverse tank (front armor facing)"), "R", false},
        {"", ss2::tr("Attack ground (armor / artillery)"), "G", false},
        {"", ss2::tr("Call air support (cycle / select)"), "F / Alt+1..5", false},
        {"", ss2::tr("Lay AT mine (Sapper)"), "E", false},
        {"", ss2::tr("Disarm minefield (Sapper)"), ss2::tr("Right click mine"), false},
        {"", ss2::tr("Scatter / disperse infantry"), "D", false},
        {"", ss2::tr("Queue waypoints"), ss2::tr("Shift + Right click"), false},
        {"", ss2::tr("Convoy move (match slowest)"), ss2::tr("Ctrl + Right click"), false},
        {"", ss2::tr("Order via minimap"), ss2::tr("Right-click / Armed click"), false},
        {"", ss2::tr("Unload / unhitch / evacuate"), "U", false},
        {"", ss2::tr("Abandon vehicle (bail out crew)"), "K", false},
        {"", ss2::tr("Deploy tactical smoke screen"), "Y + click", false},
        {"", ss2::tr("Game speed (- / +)"), "[ / ]", false},
        {"", ss2::tr("Quick Save / Quick Load"), "F9 / F10 (Ctrl+S / L)", false},
        {"", ss2::tr("Assign / recall / focus group"), "Ctrl+0..9 / 0..9 / 2x 0..9", false},
        {"", ss2::tr("Tactical Stance"), "", false},
        {"", ss2::tr("Stop selected units"), "S", false},
        {"", ss2::tr("Hold fire / Depot hold (trucks)"), "H", false},
        {"", ss2::tr("Toggle crouch stance"), "C", false},
        {"", ss2::tr("Officer binoculars"), "B", false},
        {"", ss2::tr("Camera & Interface"), "", false},
        {"", ss2::tr("Pan / Zoom"), ss2::tr("Edges / Arrows / Wheel"), false},
        {"", ss2::tr("Hide / restore HUD"), "Alt+Y", false},
        {"", ss2::tr("Save / jump bookmark"), "Ctrl+F1..F8 / F1..F8", false},
        {"", ss2::tr("Jump to recent alert (cycle)"), "Space", false},
        {"", ss2::tr("Focus troops / marker"), "Tab / M", false},
        {"", ss2::tr("Cycle player vehicle"), "V", false},
        {"", ss2::tr("Cycle idle unit"), "I", false},
        {"", ss2::tr("Select all idle units"), "Z", false},
        {"", ss2::tr("Cycle wounded / low-ammo unit"), "W", false},
        {"", ss2::tr("Force status / army roster"), "O", false},
        {"", ss2::tr("Mission briefing / objectives"), "J", false},
        {"", ss2::tr("Center / Fullscreen"), "Home / F11", false},
        {"", ss2::tr("Pause / Message log"), "Ctrl+P / L", false},
        {"", ss2::tr("Mute music / Next track"), "Ctrl+M / N", false},
    });
}

void GameSession::openDebriefMenu(bool victory) {
    if (limit || timing || !nextMissionOut || menu.active() || briefing.active()) return;
    if (simulation && !paused && !multiplayer.enabled) {
        paused = simulation->togglePause();
        menuPausedClock = true;
    } else {
        menuPausedClock = false;
    }
    std::vector<ss2::MenuItem> items;
    if (simulation) {
        const unsigned secs = simulation->simTick() / 25u;
        const unsigned mins = secs / 60u;
        const unsigned remS = secs % 60u;
        const std::string timeStr = (mins < 10u ? "0" : "") + std::to_string(mins) + ":" +
                                    (remS < 10u ? "0" : "") + std::to_string(remS);
        items.push_back({"", ss2::tr("Battle time"), timeStr, false});
    }
    items.push_back({"", ss2::tr("Enemy infantry eliminated"), std::to_string(enemyInfKilled), false});
    items.push_back({"", ss2::tr("Enemy vehicles destroyed"), std::to_string(enemyVehDestroyed), false});
    if (enemyTanksDestroyed > 0u || enemyGunsDestroyed > 0u) {
        items.push_back({"", ss2::tr("Enemy armor / guns"),
                         std::to_string(enemyTanksDestroyed) + " / " + std::to_string(enemyGunsDestroyed), false});
    }
    if (enemyPlanesShotDown > 0u) {
        items.push_back({"", ss2::tr("Enemy aircraft shot down"), std::to_string(enemyPlanesShotDown), false});
    }
    items.push_back({"", ss2::tr("Friendly casualties"),
                     std::to_string(selection ? selection->playerLosses() : 0), false});
    if (buildingsDemolished > 0u) {
        items.push_back({"", ss2::tr("Buildings demolished"), std::to_string(buildingsDemolished), false});
    }
    if (airSortiesFlown > 0u) {
        items.push_back({"", ss2::tr("Air sorties flown"), std::to_string(airSortiesFlown), false});
    }
    const int rawBattleScore = std::max(0, enemyScoreEarned - friendlyScoreLost / 2);
    const int netBattleScore = static_cast<int>(std::lround(
        static_cast<double>(rawBattleScore) * difficultyScoreWeight(settings.difficulty, multiplayer.enabled)));
    items.push_back({"", ss2::tr("Battle score"), std::to_string(netBattleScore), false});
    if (victory && unitCatalog) {
        const int stars = unitCatalog->starRatingForScore(netBattleScore);
        if (stars > 0) {
            items.push_back({"", ss2::tr("Mission rating"),
                             std::string(static_cast<std::size_t>(stars), '*') + " (" +
                                 std::to_string(stars) + "/10)",
                             false});
        }
    }
    const std::string completedKey = canonicalCampaignKey(currentMissionPath);
    int prevBestScore = 0;
    if (!completedKey.empty()) {
        if (const auto sit = settings.missionHighScores.find(completedKey); sit != settings.missionHighScores.end()) {
            prevBestScore = std::max(0, sit->second);
        }
    }
    bool shouldPersist = false;
    if (victory && !completedKey.empty() && !multiplayer.enabled) {
        if (settings.completedMissions.insert(completedKey).second) {
            shouldPersist = true;
        }
        if (netBattleScore > prevBestScore) {
            settings.missionHighScores[completedKey] = netBattleScore;
            shouldPersist = true;
        }
    }
    const int bestShown = std::max(prevBestScore, victory ? netBattleScore : 0);
    if (bestShown > 0) {
        const std::string bestStr = (victory && netBattleScore > prevBestScore)
            ? (std::to_string(bestShown) + " (" + ss2::tr("New Best!") + ")")
            : std::to_string(bestShown);
        items.push_back({"", ss2::tr("Best score"), bestStr, false});
    }
    if (shouldPersist) {
        persistNow();
    }
    if (victory && simulation && !gameDir.empty()) {
        if (const auto sig = simulation->nextMissionSignal(); sig && sig->first >= 1u && sig->first < 256u) {
            auto pad3 = [](std::uint32_t val) {
                std::string str = std::to_string(val);
                while (str.size() < 3u) str.insert(str.begin(), '0');
                return str;
            };
            if (const auto m = ss2::assets::findCampaignMission(gameDir, pad3(sig->first) + pad3(sig->second))) {
                nextCampaignMissionPath = (settings.difficulty <= 1 && !m->easyPath.empty()) ? m->easyPath : m->path;
                nextCampaignMissionTitle = m->title;
            }
        }
    }
    if (victory && simulation && !multiplayer.enabled) {
        const auto pools = simulation->savedUnitPools();
        if (campaignUnitPools) {
            *campaignUnitPools = pools;
        }
        if (persistSettings && nextCampaignMissionPath) {
            const auto poolPath = ss2::campaignPoolPathForMission(settings.activeProfile, *nextCampaignMissionPath);
            if (!poolPath.empty()) {
                (void)ss2::writeCampaignPoolFile(poolPath, pools);
            }
        }
    }
    if (victory && nextCampaignMissionPath) {
        items.push_back({"next_campaign", ss2::tr("Next mission") + ": " + nextCampaignMissionTitle, "", true});
    }
    if (!currentMissionPath.empty()) {
        items.push_back({"replay", ss2::tr("Replay mission"), "", true});
    }
    items.push_back({"campaigns", ss2::tr("Campaigns"), "", true});
    items.push_back({"missions", ss2::tr("Missions"), "", true});
    items.push_back({"resume", ss2::tr("Continue"), "", true});
    items.push_back({"quit", ss2::tr("Quit to desktop"), "", true});
    ensureRetailMenuSkin(menu, gameDir);
    menu.open(ss2::tr(victory ? "Mission Accomplished!" : "Mission Failed."), std::move(items));

    GoldMedalFaction medalFaction = GoldMedalFaction::Allied;
    bool isRusCampaign = false;
    bool isJapCampaign = false;
    if (multiplayer.enabled) {
        const std::uint8_t nat = multiplayer.playerNations[localOwner() & 3];
        if (nat == 0u) medalFaction = GoldMedalFaction::Soviet;
        else if (nat == 1u || nat == 4u) medalFaction = GoldMedalFaction::Axis;
        else medalFaction = GoldMedalFaction::Allied;
    } else {
        unsigned sovietUnits = 0u;
        unsigned axisUnits = 0u;
        unsigned alliedUnits = 0u;
        unsigned japUnits = 0u;
        auto tallyUnitNation = [&](const std::string& unitId) {
            const std::string nat = inferNation(unitId);
            if (nat == "russian") {
                ++sovietUnits;
            } else if (nat == "japanese") {
                ++axisUnits;
                ++japUnits;
            } else if (nat == "german") {
                ++axisUnits;
            } else {
                ++alliedUnits;
            }
        };
        if (scene) {
            for (const auto& u : scene->units()) {
                if (u.owner == localOwner()) tallyUnitNation(u.id);
            }
        }
        if (vehicleScene) {
            for (const auto& v : vehicleScene->vehicles()) {
                if (v.owner == localOwner()) tallyUnitNation(v.id);
            }
        }
        if (sovietUnits > axisUnits && sovietUnits >= alliedUnits) {
            medalFaction = GoldMedalFaction::Soviet;
        } else if (axisUnits >= sovietUnits && axisUnits > alliedUnits) {
            medalFaction = GoldMedalFaction::Axis;
        } else {
            medalFaction = GoldMedalFaction::Allied;
        }
        const std::string ext = currentMissionPath.extension().string();
        if (ext == ".ssc" || ext == ".SSC") {
            if (medalFaction == GoldMedalFaction::Soviet && sovietUnits > 0u) {
                isRusCampaign = true;
            } else if (medalFaction == GoldMedalFaction::Axis && japUnits * 2u >= axisUnits && japUnits > 0u) {
                isJapCampaign = true;
            }
        }
    }

    GoldPostMissionStats stats{};
    stats.victory = victory;
    stats.missionTitle = !currentMissionTitle.empty()
        ? currentMissionTitle
        : (currentMissionPath.empty() ? std::string("Tutorial I") : currentMissionPath.stem().string());
    stats.commanderName = settings.activeProfile.empty() ? std::string("YourID") : settings.activeProfile;
    stats.faction = medalFaction;
    stats.isRussianCampaign = isRusCampaign;
    stats.isJapaneseCampaign = isJapCampaign;
    stats.elapsedSeconds = simulation ? (simulation->simTick() / 25u) : 0u;
    stats.enemyInfantryKilled = enemyInfKilled;
    stats.enemyVehiclesDestroyed = enemyVehDestroyed;
    stats.enemyTanksDestroyed = enemyTanksDestroyed;
    stats.enemyGunsDestroyed = enemyGunsDestroyed;
    stats.enemyPlanesShotDown = enemyPlanesShotDown;
    stats.buildingsDemolished = buildingsDemolished;
    stats.airSortiesFlown = airSortiesFlown;
    stats.friendlyCasualties = static_cast<unsigned>(selection ? selection->playerLosses() : 0u);
    stats.friendlyInfantryLost = friendlyInfLost;
    stats.friendlyTanksLost = friendlyTanksLost;
    stats.friendlyVehiclesLost = friendlyVehiclesLost;
    stats.friendlyGunsLost = friendlyGunsLost;
    {
        unsigned bestScore = 0u;
        int bestKills = 0;
        int bestExpa = 0;
        std::string bestId;
        if (scene) {
            const auto& units = scene->units();
            for (std::size_t i = 0; i < units.size(); ++i) {
                if (units[i].owner != localOwner()) continue;
                const int k = i < unitKills.size() ? unitKills[i] : 0;
                const int e = i < unitExpa.size() ? unitExpa[i] : 0;
                if (k <= 0) continue;
                const unsigned rankMetric = static_cast<unsigned>(k) * 100000u + static_cast<unsigned>(std::max(0, e));
                if (rankMetric > bestScore) {
                    bestScore = rankMetric;
                    bestKills = k;
                    bestExpa = e;
                    bestId = units[i].id;
                }
            }
        }
        if (vehicleScene) {
            const auto& vehs = vehicleScene->vehicles();
            for (std::size_t i = 0; i < vehs.size(); ++i) {
                if (vehs[i].owner != localOwner()) continue;
                const int k = i < vehKills.size() ? vehKills[i] : 0;
                const int e = i < vehExpa.size() ? vehExpa[i] : 0;
                if (k <= 0) continue;
                const unsigned rankMetric = static_cast<unsigned>(k) * 100000u + static_cast<unsigned>(std::max(0, e));
                if (rankMetric > bestScore) {
                    bestScore = rankMetric;
                    bestKills = k;
                    bestExpa = e;
                    bestId = vehs[i].id;
                }
            }
        }
        if (bestKills > 0 && !bestId.empty()) {
            stats.mvpUnitName = ss2::tr(hudDisplayName(bestId));
            stats.mvpKills = static_cast<unsigned>(bestKills);
            stats.mvpStars = bestExpa >= 24576 ? 5 : bestExpa >= 16384 ? 4 : bestExpa >= 8192 ? 3 : bestExpa >= 4096 ? 2 : 1;
        }
    }
    stats.battleScore = netBattleScore;
    stats.bestScore = bestShown;
    stats.isNewBest = victory && netBattleScore > prevBestScore;
    stats.starRating = (victory && unitCatalog) ? unitCatalog->starRatingForScore(netBattleScore) : 0;
    stats.hasNextCampaign = victory && nextCampaignMissionPath.has_value();
    stats.nextCampaignTitle = nextCampaignMissionTitle;
    stats.canReplay = !currentMissionPath.empty();
    stats.canBrowseCampaigns = true;
    stats.canBrowseMissions = true;
    stats.sfxVolume = audio ? audio->sfxVolume() : settings.sfxVolume;

    if (!postMissionScreen) {
        const std::optional<int> fixedBg = manualClock ? std::optional<int>(0) : std::nullopt;
        postMissionScreen = std::make_unique<GoldPostMissionScreen>(window, gameDir, fixedBg);
    }
    if (audio) audio->stopAllLoops();
    renderer.setSpriteSharpness(0.0f);
    const bool playFanfare = audio && audio->available() && !manualClock && !limit && !timing && !replayFile;
    postMissionScreen->open(std::move(stats), playFanfare);
    menu.highlight(postMissionScreen->focusedId());
    syncPostMissionMenuOverlay();
    postMissionScreen->draw(renderer);
}

void GameSession::openLogMenu() {
    if (briefing.active()) return;
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    std::vector<ss2::MenuItem> items;
    items.push_back({"resume", ss2::tr("Close"), "", true});
    if (hud) {
        std::istringstream iss(hud->logText(24));
        std::vector<std::string> entries;
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty()) entries.push_back(line);
        }
        constexpr std::size_t kWrap = 68;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            const auto& entry = *it;
            std::size_t pos = 0;
            while (pos < entry.size()) {
                if (entry.size() - pos <= kWrap) {
                    items.push_back({"", entry.substr(pos), "", false});
                    break;
                }
                std::size_t cut = entry.rfind(' ', pos + kWrap);
                if (cut == std::string::npos || cut <= pos) cut = pos + kWrap;
                items.push_back({"", entry.substr(pos, cut - pos), "", false});
                pos = cut;
                while (pos < entry.size() && entry[pos] == ' ') ++pos;
            }
        }
    }
    if (items.size() == 1) items.push_back({"", ss2::tr("No messages yet."), "", false});
    menu.open(ss2::tr("Message Log"), std::move(items));
}

void GameSession::openForceOverviewMenu(bool fromPauseMenu) {
    if (!simulation || !scene || briefing.active()) return;
    ensureRetailMenuSkin(menu, gameDir);
    dismissPostMissionScreen();
    forceRosterGroups.clear();
    std::vector<ss2::MenuItem> items;
    items.push_back(fromPauseMenu
        ? ss2::MenuItem{"back", ss2::tr("Back to pause menu"), "", true}
        : ss2::MenuItem{"resume", ss2::tr("Close"), "", true});
    const unsigned secs = simulation->simTick() / 25u;
    const unsigned mins = secs / 60u;
    const unsigned remS = secs % 60u;
    const std::string timeStr = (mins < 10u ? "0" : "") + std::to_string(mins) + ":" +
                                (remS < 10u ? "0" : "") + std::to_string(remS);
    items.push_back({"", ss2::tr("Battle time"), timeStr, false});

    struct ClassStats {
        std::vector<size_t> infIds;
        std::vector<size_t> vehIds;
        unsigned woundedCount{0};
        unsigned damagedCount{0};
    };
    std::map<std::string, ClassStats> byClass;
    unsigned activeInfField = 0;
    const auto& units = scene->units();
    for (std::size_t idx = 0; idx < units.size(); ++idx) {
        const auto& u = units[idx];
        if (u.owner != localOwner() || u.hp <= 0 || !u.visible) continue;
        ++activeInfField;
        auto& entry = byClass[hudDisplayName(u.id)];
        entry.infIds.push_back(idx);
        const int maxHp = (idx < initialUnitHp.size() && initialUnitHp[idx] > 0) ? initialUnitHp[idx] : 100;
        if (u.hp < maxHp) ++entry.woundedCount;
    }
    unsigned activeVeh = 0;
    unsigned damagedVeh = 0;
    if (vehicleScene) {
        const auto& vehs = vehicleScene->vehicles();
        for (std::size_t idx = 0; idx < vehs.size(); ++idx) {
            const auto& v = vehs[idx];
            if (!v.alive || v.hp <= 0 || v.owner != localOwner() || !v.visible) continue;
            ++activeVeh;
            auto& entry = byClass[hudDisplayName(v.id)];
            entry.vehIds.push_back(idx);
            const int maxHp = (idx < initialVehicleHp.size() && initialVehicleHp[idx] > 0) ? initialVehicleHp[idx] : 100;
            if (v.hp < maxHp) {
                ++damagedVeh;
                ++entry.damagedCount;
            }
        }
    }
    unsigned garrisonedInf = 0;
    for (const auto& lb : liveBuildings) {
        if (!lb.destroyed && lb.owner == localOwner() && lb.occupantCount > 0) {
            garrisonedInf += static_cast<unsigned>(lb.occupantCount);
        }
    }
    items.push_back({"", ss2::tr("Active infantry"),
                     std::to_string(activeInfField) +
                         (garrisonedInf > 0 ? " (+" + std::to_string(garrisonedInf) + ")" : ""),
                     false});
    items.push_back({"", ss2::tr("Active vehicles"),
                     std::to_string(activeVeh) +
                         (damagedVeh > 0 ? " (" + std::to_string(damagedVeh) + " " + ss2::tr("damaged") + ")" : ""),
                     false});
    items.push_back({"", ss2::tr("Enemy losses"),
                     std::to_string(enemyInfKilled) + " / " + std::to_string(enemyVehDestroyed), false});
    items.push_back({"", ss2::tr("Friendly casualties"),
                     std::to_string(selection ? selection->playerLosses() : 0), false});
    const int rawOverviewScore = std::max(0, enemyScoreEarned - friendlyScoreLost / 2);
    const int weightedOverviewScore = static_cast<int>(std::lround(
        static_cast<double>(rawOverviewScore) * difficultyScoreWeight(settings.difficulty, multiplayer.enabled)));
    items.push_back({"", ss2::tr("Battle score"),
                     std::to_string(weightedOverviewScore), false});
    int totalAirReady = 0;
    for (const auto& as : simulation->airSupportFor(localOwner())) {
        totalAirReady += std::max(0, as.ready);
    }
    if (totalAirReady > 0) {
        items.push_back({"airsupport", ss2::tr("Air support ready"), std::to_string(totalAirReady), true});
    }

    for (auto& [label, st] : byClass) {
        const std::size_t rowIdx = forceRosterGroups.size();
        const std::size_t count = st.infIds.size() + st.vehIds.size();
        std::string tag = "x" + std::to_string(count);
        if (st.damagedCount > 0) {
            tag += " (" + std::to_string(st.damagedCount) + " " + ss2::tr("damaged") + ")";
        } else if (st.woundedCount > 0) {
            tag += " (" + std::to_string(st.woundedCount) + " " + ss2::tr("wounded") + ")";
        }
        forceRosterGroups.push_back({label, std::move(st.infIds), std::move(st.vehIds)});
        items.push_back({"force_select:" + std::to_string(rowIdx), ss2::tr(label), tag, true});
    }
    for (std::size_t bi = 0; bi < liveBuildings.size(); ++bi) {
        const auto& lb = liveBuildings[bi];
        if (lb.destroyed || lb.owner != localOwner() || lb.occupantCount <= 0) continue;
        items.push_back({"force_bldg:" + std::to_string(bi), ss2::tr("Garrisoned building"),
                         std::to_string(lb.occupantCount) + "/" + std::to_string(lb.capacity) +
                             " | HP " + std::to_string(lb.hp) + "/" + std::to_string(lb.maxHp),
                         true});
    }
    menu.open(ss2::tr("Force Status"), std::move(items));
}

void GameSession::openMissionBriefing() {
    std::string text = missionBriefingText;
    if (text.empty()) {
        text = currentObjective;
    } else if (!currentObjective.empty() && missionBriefingText != currentObjective) {
        text += "\n\n" + ss2::tr("Current objective") + ": " + currentObjective;
    }
    if (text.empty()) {
        orderNotice.show(ss2::tr("No mission briefing available."), ss2::NoticeKind::unsupported);
        return;
    }
    if (menu.active()) closeMenu();
    manualBriefingOpen = true;
    briefing.open(std::move(text));
}

std::filesystem::path GameSession::quickSavePath(int slot) const {
    std::filesystem::path dir = ss2::defaultSavesDir();
    if (dir.empty()) {
        dir = std::filesystem::temp_directory_path() / "ss2" / "saves";
    }
    const std::string stem = currentMissionPath.empty() ? std::string("tutorial1")
                                                        : currentMissionPath.stem().string();
    const std::string fname = (slot == -2) ? (stem + "_auto_start.ss2save")
                            : (slot == -3) ? (stem + "_auto_obj.ss2save")
                            : (slot < 0)   ? (stem + "_autosave.ss2save")
                            : (slot == 0)  ? (stem + ".ss2save")
                                           : (stem + "_slot" + std::to_string(slot) + ".ss2save");
    return dir / fname;
}

void GameSession::doSaveSlot(int slot, bool quiet) {
    if (!simulation) return;
    if (multiplayer.enabled) {
        if (!quiet) orderNotice.show(ss2::tr("Save/load disabled in multiplayer."), ss2::NoticeKind::unsupported);
        return;
    }
    const auto path = quickSavePath(slot);
    SaveFileMetadata meta{};
    meta.hasMetadata = true;
    if (!currentMissionPath.empty()) {
        std::error_code relEc;
        const auto rel = !gameDir.empty() ? std::filesystem::relative(currentMissionPath, gameDir, relEc)
                                          : std::filesystem::path{};
        meta.mission = (!relEc && !rel.empty() && !rel.generic_string().starts_with(".."))
            ? rel.generic_string()
            : currentMissionPath.filename().string();
    } else {
        meta.mission = "maps/single/--- TUTORIAL I ---.SSM";
    }
    meta.missionTitle = !currentMissionTitle.empty()
        ? currentMissionTitle
        : (currentMissionPath.empty() ? std::string("Tutorial I") : currentMissionPath.stem().string());
    meta.difficulty = std::clamp(settings.difficulty, 1, 3);
    meta.savedEpochSec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    meta.enemyInfKilled = enemyInfKilled;
    meta.enemyVehDestroyed = enemyVehDestroyed;
    meta.enemyTanksDestroyed = enemyTanksDestroyed;
    meta.enemyGunsDestroyed = enemyGunsDestroyed;
    meta.enemyPlanesShotDown = enemyPlanesShotDown;
    meta.buildingsDemolished = buildingsDemolished;
    meta.airSortiesFlown = airSortiesFlown;
    meta.enemyScoreEarned = enemyScoreEarned;
    meta.friendlyScoreLost = friendlyScoreLost;

    if (!ss2::writeSaveFile(path, meta, simulation->takeFullSnapshot())) {
        if (!quiet) orderNotice.show(ss2::tr("Could not write quick-save file."), ss2::NoticeKind::blocked);
        return;
    }
    if (slot < 0) {
        if (!quiet) orderNotice.show(ss2::tr("Auto-saved mission progress."), ss2::NoticeKind::accepted);
        return;
    }
    if (audio) audio->playGroup("accept", 0.45f);
    if (slot == 0) {
        showNotice(ss2::tr("Quick-saved mission state (F9)."), ss2::NoticeKind::accepted);
    } else {
        showNotice(ss2::trf("Saved mission state to Slot {0}.", {std::to_string(slot)}), ss2::NoticeKind::accepted);
    }
}

bool GameSession::loadSaveFromPath(const std::filesystem::path& path, const std::string& loadedNotice,
                                   bool isStartupLoad) {
    if (!simulation || !scene || !selection) return false;
    if (multiplayer.enabled) {
        if (!isStartupLoad) {
            orderNotice.show(ss2::tr("Save/load disabled in multiplayer."), ss2::NoticeKind::unsupported);
        }
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        if (!isStartupLoad) {
            orderNotice.show(ss2::tr("No quick-save found for this mission."), ss2::NoticeKind::unsupported);
        }
        return false;
    }
    const auto decoded = ss2::readSaveFile(path);
    if (!decoded) {
        if (!isStartupLoad) {
            orderNotice.show(ss2::tr("Quick-save file is empty or corrupt."), ss2::NoticeKind::blocked);
        }
        return false;
    }
    const auto& snap = decoded->snapshot;
    const bool loadedOk = decoded->fullSnapshot
        ? simulation->loadFullSnapshot(*decoded->fullSnapshot)
        : simulation->loadSnapshot(snap);
    if (!loadedOk) {
        if (!isStartupLoad) {
            orderNotice.show(ss2::tr("Quick-save data is incompatible with this mission."), ss2::NoticeKind::blocked);
        }
        return false;
    }

    if (campaignUnitPools) {
        *campaignUnitPools = simulation->savedUnitPools();
    }
    pendingDialogs.clear();
    manualBriefingOpen = false;
    paused = simulation->isPaused();
    menuPausedClock = false;
    fallenSoldiers.clear();
    recoilUntil.clear();
    unitReloadWindow.clear();
    vehReloadWindow.clear();
    woundedBarLogged.clear();
    activeVfx.clear();
    burningWrecks.clear();
    movingSmokeVehicles.clear();
    movingWaterVehicles.clear();
    livePlanes.clear();
    liveParachutes.clear();
    planeAtlas.reset();
    boundPlaneSlots.clear();
    planeOverflowLogged = false;
    activeMarkers.clear();
    orderPulses.clear();
    eventHistory.clear();
    eventHistoryCursor = 0;
    syncMarkers();
    selectedBuilding.reset();
    selection->setSelection({}, {});
    if (decoded->meta.hasMetadata) {
        enemyInfKilled = decoded->meta.enemyInfKilled;
        enemyVehDestroyed = decoded->meta.enemyVehDestroyed;
        enemyTanksDestroyed = decoded->meta.enemyTanksDestroyed;
        enemyGunsDestroyed = decoded->meta.enemyGunsDestroyed;
        enemyPlanesShotDown = decoded->meta.enemyPlanesShotDown;
        buildingsDemolished = decoded->meta.buildingsDemolished;
        airSortiesFlown = decoded->meta.airSortiesFlown;
        enemyScoreEarned = decoded->meta.enemyScoreEarned;
        friendlyScoreLost = decoded->meta.friendlyScoreLost;
    } else {
        enemyInfKilled = 0;
        enemyVehDestroyed = 0;
        buildingsDemolished = 0;
        airSortiesFlown = 0;
        enemyScoreEarned = 0;
        friendlyScoreLost = 0;
        enemyTanksDestroyed = 0;
        enemyGunsDestroyed = 0;
        enemyPlanesShotDown = 0;
    }
    vehicleCrewImpaired.clear();
    vehicleMoveImpaired.clear();
    vehicleSuppressed.clear();
    vehicleHatchOpen.clear();
    movingSmokeVehicles.clear();
    attackMoveArmed = false;
    attackGroundArmed = false;
    reverseMoveArmed = false;
    patrolArmed = false;
    faceArmed = false;
    smokeArmed = false;
    armedAirSupport.reset();
    resetCursor();
    crouchedUnits.clear();
    holdFireUnits.clear();
    binocularUnits.clear();
    vehicleHoldFireUnits.clear();
    camouflagedUnits.clear();
    suppressedUnits.clear();
    missionPlayerTeams = snap.playerTeams;
    for (std::size_t i = 0; i < snap.soldiers.size(); ++i) {
        const auto& ss = snap.soldiers[i];
        if (ss.crouched) crouchedUnits.insert(i);
        if (ss.holdFire) holdFireUnits.insert(i);
        if (ss.binoculars) binocularUnits.insert(i);
    }
    for (std::size_t i = 0; i < snap.vehicles.size(); ++i) {
        if (snap.vehicles[i].holdFire) vehicleHoldFireUnits.insert(i);
    }

    if (auto fogSnap = simulation->pollFog()) {
        latestFog = *fogSnap;
        const auto fogPatch = scene->updateFog(*fogSnap);
        if (fogPatch.pixels.width && fogPatch.pixels.height)
            renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
        updateMinimapFog(*fogSnap, true);
    }
    auto sortedSoldierPoses = simulation->pollPoses();
    std::ranges::sort(sortedSoldierPoses, {}, &SoldierPose::index);
    if (initialUnitHp.size() < snap.soldiers.size()) initialUnitHp.resize(snap.soldiers.size(), 100);
    if (unitExpa.size() < snap.soldiers.size()) unitExpa.resize(snap.soldiers.size(), 0);
    if (unitKills.size() < snap.soldiers.size()) unitKills.resize(snap.soldiers.size(), 0);
    if (unitGrenades.size() < snap.soldiers.size()) unitGrenades.resize(snap.soldiers.size(), 0);
    if (unitMineAmmo.size() < snap.soldiers.size()) unitMineAmmo.resize(snap.soldiers.size(), 0);
    for (const auto& pose : sortedSoldierPoses) {
        if (decoded->fullSnapshot && pose.index >= scene->units().size()) {
            const auto& ids = decoded->fullSnapshot->unitsExtra.soldierIds;
            const std::string wantId = (pose.index < ids.size() && !ids[pose.index].empty())
                ? ids[pose.index]
                : std::string("dguner");
            ss2::assets::Placement p{wantId, pose.x, pose.y, static_cast<int>(pose.direction),
                                     std::max(1, pose.hp), pose.ammo, pose.group, pose.owner, {}, pose.expa};
            (void)ensureGpuInfantrySlot(wantId);
            auto spawned = scene->spawn(p);
            if (!spawned && wantId != "dguner") {
                p.id = "dguner";
                (void)ensureGpuInfantrySlot("dguner");
                spawned = scene->spawn(p);
            }
            if (spawned) {
                const auto& [newIdx, patch] = *spawned;
                const auto& u = scene->units().at(newIdx);
                ensureUnitPortrait(u.id, false);
                selection->addUnit(scaleInfantry(u, mapScale));
                if (newIdx >= initialUnitHp.size()) initialUnitHp.resize(newIdx + 1, 100);
                if (newIdx >= unitGrenades.size()) unitGrenades.resize(newIdx + 1, 0);
                if (newIdx >= unitMineAmmo.size()) unitMineAmmo.resize(newIdx + 1, 0);
                unitGrenades[newIdx] = initialGrenadesForId(u.id);
                unitMineAmmo[newIdx] = initialMinesForId(u.id);
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
        }
        if (pose.index < scene->units().size()) {
            const auto patch = scene->update(pose.index, pose.x, pose.y, pose.direction,
                                             pose.progress, pose.walking, pose.hp,
                                             pose.visible ? 1 : 0, pose.ammo);
            if (patch.pixels.width && patch.pixels.height)
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            selection->update(pose.index, scaleInfantry(scene->units().at(pose.index), mapScale));
        }
        if (pose.index < snap.soldiers.size()) {
            initialUnitHp[pose.index] = std::max(1, snap.soldiers[pose.index].maxHp);
        }
        if (pose.index >= unitExpa.size()) unitExpa.resize(pose.index + 1, 0);
        if (pose.index >= unitKills.size()) unitKills.resize(pose.index + 1, 0);
        if (pose.index >= unitGrenades.size()) unitGrenades.resize(pose.index + 1, 0);
        if (pose.index >= unitMineAmmo.size()) unitMineAmmo.resize(pose.index + 1, 0);
        unitExpa[pose.index] = pose.expa;
        unitKills[pose.index] = pose.kills;
        unitGrenades[pose.index] = pose.atGrenades;
        unitMineAmmo[pose.index] = pose.mineAmmo;
        if (pose.camouflaged) camouflagedUnits.insert(pose.index);
        if (pose.alive && pose.suppressed) suppressedUnits.insert(pose.index);
    }
    for (std::size_t i = snap.soldiers.size(); i < scene->units().size(); ++i) {
        const auto& u = scene->units().at(i);
        const auto patch = scene->update(i, u.x, u.y, u.direction, 0u, false, 0, 0, 0);
        if (patch.pixels.width && patch.pixels.height)
            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
        auto deadInf = scaleInfantry(scene->units().at(i), mapScale);
        deadInf.hp = 0;
        deadInf.visible = false;
        selection->update(i, deadInf);
    }
    if (vehicleScene) {
        auto sortedVehPoses = simulation->pollVehiclePoses();
        std::ranges::sort(sortedVehPoses, {}, &VehiclePose::index);
        const std::size_t needVeh = std::max(snap.vehicles.size(),
                                             sortedVehPoses.empty() ? 0u : (sortedVehPoses.back().index + 1u));
        if (initialVehicleHp.size() < needVeh) initialVehicleHp.resize(needVeh, 100);
        if (vehicleCargo.size() < needVeh) vehicleCargo.resize(needVeh, {0, 0});
        if (vehicleTowed.size() < needVeh) vehicleTowed.resize(needVeh, -1);
        if (vehicleTowedBy.size() < needVeh) vehicleTowedBy.resize(needVeh, -1);
        if (vehicleAmmo2.size() < needVeh) vehicleAmmo2.resize(needVeh, 0);
        if (vehExpa.size() < needVeh) vehExpa.resize(needVeh, 0);
        if (vehKills.size() < needVeh) vehKills.resize(needVeh, 0);
        if (vehicleCrewImpaired.size() < needVeh) vehicleCrewImpaired.resize(needVeh, false);
        if (vehicleMoveImpaired.size() < needVeh) vehicleMoveImpaired.resize(needVeh, false);
        if (vehicleSuppressed.size() < needVeh) vehicleSuppressed.resize(needVeh, false);
        if (vehicleHatchOpen.size() < needVeh) vehicleHatchOpen.resize(needVeh, false);
        for (const auto& vp : sortedVehPoses) {
            if (decoded->fullSnapshot && vp.index >= vehicleScene->vehicles().size()) {
                const auto& vids = decoded->fullSnapshot->unitsExtra.vehicleIds;
                const std::string wantVeh = (vp.index < vids.size() && !vids[vp.index].empty())
                    ? vids[vp.index]
                    : std::string("tank");
                ss2::assets::Placement vpPlace{wantVeh, vp.x, vp.y, static_cast<int>(vp.hullDir & 31u),
                                               std::clamp(vp.hp, 1, 100), vp.ammo, vp.group, vp.owner, {}, 0};
                if (const auto vIdx = vehicleScene->spawn(vpPlace)) {
                    ensureUnitPortrait(wantVeh, true);
                    (void)ensureGpuVehicleSlot(wantVeh);
                    selection->addVehicle(scaleVehicle(vehicleScene->vehicles().at(*vIdx), mapScale));
                } else {
                    const auto phIdx = vehicleScene->spawnVisualPlaceholder(vpPlace, 24, 20);
                    ensureUnitPortrait(wantVeh, true);
                    selection->addVehicle(scaleVehicle(vehicleScene->vehicles().at(phIdx), mapScale));
                }
            }
            if (vp.index < vehicleScene->vehicles().size()) {
                vehicleScene->setOwner(vp.index, vp.owner);
                const bool isGpuVeh = gpuVehicleIds.contains(vehicleScene->vehicles().at(vp.index).id);
                const auto dirty = vehicleScene->update(vp.index, vp.x, vp.y, vp.hullDir, vp.turretDir,
                                                        vp.hp, vp.visible ? 1 : 0, vp.ammo);
                if (mapScale == 1 && dirty.valid() && !isGpuVeh) {
                    const auto patch = scene->repaintRegion(dirty.left, dirty.top, dirty.right, dirty.bottom);
                    if (patch.pixels.width && patch.pixels.height)
                        renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                }
                selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
            }
            if (vp.maxHp > 0) initialVehicleHp[vp.index] = vp.maxHp;
            vehicleCargo[vp.index] = {vp.passengerCount, vp.passengerCapacity};
            vehicleTowed[vp.index] = vp.towedIndex;
            vehicleTowedBy[vp.index] = vp.towedByIndex;
            vehicleAmmo2[vp.index] = vp.ammo2;
            vehExpa[vp.index] = vp.expa;
            vehKills[vp.index] = vp.kills;
            vehicleCrewImpaired[vp.index] = vp.crewImpaired;
            vehicleMoveImpaired[vp.index] = vp.moveImpaired;
            vehicleSuppressed[vp.index] = vp.suppressed;
            vehicleHatchOpen[vp.index] = vp.hatchOpen;
        }
        for (std::size_t i = snap.vehicles.size(); i < vehicleScene->vehicles().size(); ++i) {
            const auto& v = vehicleScene->vehicles().at(i);
            const bool isGpuVeh = gpuVehicleIds.contains(v.id);
            const auto dirty = vehicleScene->update(i, v.x, v.y, v.hullDir, v.turretDir, 0, 0, 0);
            if (mapScale == 1 && dirty.valid() && !isGpuVeh) {
                const auto patch = scene->repaintRegion(dirty.left, dirty.top, dirty.right, dirty.bottom);
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
            auto deadVeh = scaleVehicle(vehicleScene->vehicles().at(i), mapScale);
            deadVeh.hp = 0;
            deadVeh.alive = false;
            deadVeh.visible = false;
            selection->updateVehicle(i, deadVeh);
        }
    }
    reportedLosses = selection->playerLosses();
    for (const auto& bu : simulation->pollBuildingUpdates()) {
        if (bu.index < liveBuildings.size()) {
            auto& lb = liveBuildings[bu.index];
            lb.hp = bu.hp;
            lb.maxHp = bu.maxHp;
            lb.occupantCount = bu.occupantCount;
            lb.occupantIds = bu.occupantIds;
            lb.owner = bu.owner;
            lb.destroyed = bu.destroyed;
            applyBuildingStage(bu.index, ss2::assets::buildingDamageStage(lb.hp, lb.maxHp, lb.destroyed));
        }
    }
    for (const auto& bru : simulation->pollBridgeUpdates()) {
        if (bru.index < liveBridges.size()) {
            auto& lb = liveBridges[bru.index];
            lb.hp = bru.hp;
            lb.maxHp = bru.maxHp;
            lb.stage = bru.stage;
            lb.destroyed = bru.destroyed;
            applyBridgeStage(bru.index, bru.stage);
        }
    }
    (void)simulation->pollMineEvents();
    liveVisibleMines = simulation->visibleMines();
    (void)simulation->pollAirSorties();
    (void)simulation->pollAirStrikeEvents();
    (void)simulation->poll();
    syncGpuSprites();
    friendlyInfLost = friendlyTanksLost = friendlyVehiclesLost = friendlyGunsLost = 0;
    activeSubgroupLabel.clear();
    rightDragActive = false;
    nextAutoSaveTick = simulation->simTick() + 7500u;
    nextObjAutoSaveTick = simulation->simTick() + 1500u;
    if (isStartupLoad) {
        std::cout << "Loaded saved game: " << path << " (tick " << simulation->simTick()
                  << ", " << decoded->meta.playerSoldiers << " infantry, "
                  << decoded->meta.playerVehicles << " vehicles)\n";
        if (!loadedNotice.empty()) {
            showNotice(loadedNotice, ss2::NoticeKind::accepted);
        }
    } else {
        if (audio) audio->playGroup("accept", 0.45f);
        if (!loadedNotice.empty()) {
            showNotice(loadedNotice, ss2::NoticeKind::accepted);
        }
    }
    return true;
}

void GameSession::doLoadSlot(int slot) {
    const std::string notice = (slot < 0)
        ? ss2::tr("Loaded Auto-Save.")
        : (slot == 0 ? ss2::tr("Quick-loaded mission state (F10).")
                     : ss2::trf("Loaded mission state from Slot {0}.", {std::to_string(slot)}));
    (void)loadSaveFromPath(quickSavePath(slot), notice, false);
}

void GameSession::doQuickSave() { doSaveSlot(0); }
void GameSession::doQuickLoad() { doLoadSlot(0); }

void GameSession::openSaveLoadMenu() {
    if (!simulation) return;
    if (multiplayer.enabled) {
        orderNotice.show(ss2::tr("Save/load disabled in multiplayer."), ss2::NoticeKind::unsupported);
        return;
    }
    dismissPostMissionScreen();
    if (audio) audio->playGroup("choose", 0.42f, ss2::VoicePriority::High);
    auto saveSlotStatus = [](const std::filesystem::path& path, bool& outExists) -> std::string {
        std::error_code ec;
        outExists = std::filesystem::is_regular_file(path, ec);
        if (!outExists) return ss2::tr("Empty");
        if (const auto decoded = ss2::readSaveFile(path)) {
            const unsigned secs = decoded->meta.tick / 25u;
            const unsigned mins = secs / 60u;
            const unsigned remS = secs % 60u;
            const std::string mmss = (mins < 10u ? "0" : "") + std::to_string(mins) + ":" +
                                     (remS < 10u ? "0" : "") + std::to_string(remS);
            return mmss + " | " + std::to_string(decoded->meta.playerSoldiers) + " inf / " +
                   std::to_string(decoded->meta.playerVehicles) + " veh";
        }
        const auto fsz = std::filesystem::file_size(path, ec);
        return std::to_string(std::max<std::uintmax_t>(1u, fsz / 1024u)) + " KB";
    };
    std::array<std::string, 4> slotTags{};
    std::array<bool, 4> slotExists{};
    for (int s = 0; s <= 3; ++s) {
        slotTags[static_cast<std::size_t>(s)] = saveSlotStatus(quickSavePath(s), slotExists[static_cast<std::size_t>(s)]);
    }
    std::vector<ss2::MenuItem> items;
    items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
    items.push_back({"", ss2::tr("Save Game"), "", false});
    for (int s = 0; s <= 3; ++s) {
        const std::string lbl = (s == 0) ? ss2::tr("Quick Save (F9)")
                                         : ss2::trf("Save Slot {0}", {std::to_string(s)});
        items.push_back({"save:" + std::to_string(s), lbl, slotTags[static_cast<std::size_t>(s)], true});
    }
    items.push_back({"", ss2::tr("Load Game"), "", false});
    {
        bool autoExists = false;
        const std::string autoTag = saveSlotStatus(quickSavePath(-1), autoExists);
        items.push_back({"load:-1", ss2::tr("Load Auto-Save"), autoTag, autoExists});
        bool objExists = false;
        const std::string objTag = saveSlotStatus(quickSavePath(-3), objExists);
        if (objExists) items.push_back({"load:-3", ss2::tr("Load Objective Auto-Save"), objTag, true});
        bool startExists = false;
        const std::string startTag = saveSlotStatus(quickSavePath(-2), startExists);
        if (startExists) items.push_back({"load:-2", ss2::tr("Load Mission-Start Auto-Save"), startTag, true});
    }
    for (int s = 0; s <= 3; ++s) {
        const std::string lbl = (s == 0) ? ss2::tr("Quick Load (F10)")
                                         : ss2::trf("Load Slot {0}", {std::to_string(s)});
        items.push_back({"load:" + std::to_string(s), lbl,
                         slotTags[static_cast<std::size_t>(s)], slotExists[static_cast<std::size_t>(s)]});
    }
    ensureRetailMenuSkin(menu, gameDir);
    menu.open(ss2::tr("Save / Load"), std::move(items));
}

} // namespace ss2
