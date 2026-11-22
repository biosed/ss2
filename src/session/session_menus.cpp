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

std::vector<MenuItem> GameSession::buildPauseItems() {
    std::vector<ss2::MenuItem> items;
    items.push_back({"resume", ss2::tr("Resume"), "", true});
    if (simulation) {
        int airReady = 0;
        for (const auto& s : simulation->playerAirSupport()) airReady += s.ready;
        if (airReady > 0) {
            items.push_back({"airsupport", ss2::tr("Air Support"), std::to_string(airReady), true});
        }
        items.push_back({"quicksave", ss2::tr("Quick Save"), "F9", true});
        items.push_back({"quickload", ss2::tr("Quick Load"), "F10", true});
        items.push_back({"saveload", ss2::tr("Save / Load..."), "", true});
        items.push_back({"forces", ss2::tr("Force Status"), "O", true});
        items.push_back({"briefing", ss2::tr("Mission Briefing"), "J", true});
    }
    if (!gameDir.empty() && nextMissionOut) {
        items.push_back({"campaigns", ss2::tr("Campaigns"), "", true});
        items.push_back({"missions", ss2::tr("Missions"), "", true});
    }
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
    if (simulation) {
        const int sp = simulation->speedPercent();
        const std::string spStr = sp == 50 ? "0.5x" : sp == 200 ? "2x" : sp == 300 ? "3x" : "1x";
        items.push_back({"speed", ss2::tr("Game speed"), spStr, true});
    }
    {
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
    items.push_back({"quit", ss2::tr("Quit to desktop"), "", true});
    return items;
}

void GameSession::openAirSupportMenu() {
    if (!simulation) return;
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
    attackMoveArmed = false;
    attackGroundArmed = false;
    reverseMoveArmed = false;
    patrolArmed = false;
    faceArmed = false;
    armedAirSupport.reset();
    resetCursor();
    if (simulation && !paused) {
        paused = simulation->togglePause();
        menuPausedClock = true;
    } else {
        menuPausedClock = false;
    }
    menu.open(ss2::tr("Paused"), buildPauseItems());
}

void GameSession::closeMenu() {
    menu.close();
    if (simulation && menuPausedClock && paused) {
        paused = simulation->togglePause();
    }
    menuPausedClock = false;
}

void GameSession::openMissionsMenu() {
    if (gameDir.empty()) return;
    menuMissions = ss2::assets::listSinglePlayerMissions(gameDir);
    std::vector<ss2::MenuItem> items;
    items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
    for (std::size_t i = 0; i < menuMissions.size(); ++i) {
        const auto& m = menuMissions[i];
        std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) +
                          (m.scheme == 2 ? " Beach" : " Summer");
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
    menuCampaigns = ss2::assets::listCampaigns(gameDir);
    std::vector<ss2::MenuItem> items;
    items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
    for (std::size_t i = 0; i < menuCampaigns.size(); ++i) {
        const auto& c = menuCampaigns[i];
        std::size_t done = 0;
        for (const auto& m : c.missions) {
            if (settings.completedMissions.contains(ss2::normalizeMissionKey(m.code)) ||
                settings.completedMissions.contains(ss2::normalizeMissionKey(m.path.stem().string()))) {
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
    const auto& c = menuCampaigns[campaignIdx];
    std::vector<ss2::MenuItem> items;
    items.push_back({"back_campaigns", ss2::tr("Back to campaigns"), "", true});
    for (std::size_t i = 0; i < c.missions.size(); ++i) {
        const auto& m = c.missions[i];
        std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) +
                          (m.scheme == 2 ? " Beach" : " Summer");
        const std::string codeKey = ss2::normalizeMissionKey(m.code);
        const std::string stemKey = ss2::normalizeMissionKey(m.path.stem().string());
        if (settings.completedMissions.contains(codeKey) ||
            settings.completedMissions.contains(stemKey)) {
            tag = ss2::tr("Completed") + " | " + tag;
        }
        int bestScore = 0;
        if (const auto sit = settings.missionHighScores.find(codeKey); sit != settings.missionHighScores.end()) {
            bestScore = std::max(bestScore, sit->second);
        }
        if (const auto sit = settings.missionHighScores.find(stemKey); sit != settings.missionHighScores.end()) {
            bestScore = std::max(bestScore, sit->second);
        }
        if (bestScore > 0) {
            tag += " | " + std::to_string(bestScore);
        }
        items.push_back({"cmission:" + std::to_string(campaignIdx) + ":" + std::to_string(i), m.title, tag, true});
    }
    menu.open(c.name, std::move(items));
}

void GameSession::openControlsMenu() {
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
        {"", ss2::tr("Order via minimap"), ss2::tr("Right-click / Armed click"), false},
        {"", ss2::tr("Unload / unhitch / evacuate"), "U", false},
        {"", ss2::tr("Abandon vehicle (bail out crew)"), "K", false},
        {"", ss2::tr("Game speed (- / +)"), "[ / ]", false},
        {"", ss2::tr("Quick Save / Quick Load"), "F9 / F10 (Ctrl+S / L)", false},
        {"", ss2::tr("Assign / recall / focus group"), "Ctrl+0..9 / 0..9 / 2x 0..9", false},
        {"", ss2::tr("Tactical Stance"), "", false},
        {"", ss2::tr("Stop selected units"), "S", false},
        {"", ss2::tr("Toggle hold fire"), "H", false},
        {"", ss2::tr("Toggle crouch stance"), "C", false},
        {"", ss2::tr("Officer binoculars"), "B", false},
        {"", ss2::tr("Camera & Interface"), "", false},
        {"", ss2::tr("Pan / Zoom"), ss2::tr("Edges / Arrows / Wheel"), false},
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
    if (simulation && !paused) {
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
    const int netBattleScore = std::max(0, enemyScoreEarned - friendlyScoreLost / 2);
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
    const std::string completedKey = ss2::normalizeMissionKey(
        currentMissionPath.empty() ? std::string("TUTORIAL1") : currentMissionPath.stem().string());
    int prevBestScore = 0;
    if (!completedKey.empty()) {
        if (const auto sit = settings.missionHighScores.find(completedKey); sit != settings.missionHighScores.end()) {
            prevBestScore = std::max(0, sit->second);
        }
    }
    bool shouldPersist = false;
    if (victory && !completedKey.empty()) {
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
                nextCampaignMissionPath = m->path;
                nextCampaignMissionTitle = m->title;
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
    menu.open(ss2::tr(victory ? "Mission Accomplished!" : "Mission Failed."), std::move(items));
}

void GameSession::openLogMenu() {
    if (briefing.active()) return;
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
        if (u.owner != 0 || u.hp <= 0 || !u.visible) continue;
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
            if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.visible) continue;
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
        if (!lb.destroyed && lb.owner == 0 && lb.occupantCount > 0) {
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
    items.push_back({"", ss2::tr("Battle score"),
                     std::to_string(std::max(0, enemyScoreEarned - friendlyScoreLost / 2)), false});
    int totalAirReady = 0;
    for (const auto& as : simulation->playerAirSupport()) {
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
        if (lb.destroyed || lb.owner != 0 || lb.occupantCount <= 0) continue;
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
    const std::string fname = (slot < 0)
        ? (stem + "_autosave.ss2save")
        : (slot == 0 ? (stem + ".ss2save") : (stem + "_slot" + std::to_string(slot) + ".ss2save"));
    return dir / fname;
}

void GameSession::doSaveSlot(int slot) {
    if (!simulation) return;
    const auto path = quickSavePath(slot);
    SaveFileMetadata meta{};
    meta.hasMetadata = true;
    if (!currentMissionPath.empty()) {
        std::error_code relEc;
        const auto rel = !gameDir.empty() ? std::filesystem::relative(currentMissionPath, gameDir, relEc)
                                          : std::filesystem::path{};
        meta.mission = (!relEc && !rel.empty() && !rel.native().starts_with(".."))
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

    if (!ss2::writeSaveFile(path, meta, simulation->takeSnapshot())) {
        orderNotice.show(ss2::tr("Could not write quick-save file."), ss2::NoticeKind::blocked);
        return;
    }
    if (slot < 0) {
        orderNotice.show(ss2::tr("Auto-saved mission progress."), ss2::NoticeKind::accepted);
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
    if (!simulation->loadSnapshot(snap)) {
        if (!isStartupLoad) {
            orderNotice.show(ss2::tr("Quick-save data is incompatible with this mission."), ss2::NoticeKind::blocked);
        }
        return false;
    }

    fallenSoldiers.clear();
    recoilUntil.clear();
    woundedBarLogged.clear();
    activeVfx.clear();
    burningWrecks.clear();
    livePlanes.clear();
    planeAtlas.reset();
    planeOverflowLogged = false;
    activeMarkers.clear();
    eventHistory.clear();
    eventHistoryCursor = 0;
    syncMarkers();
    selectedBuilding.reset();
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
    for (const auto& pose : simulation->pollPoses()) {
        if (pose.index < scene->units().size()) {
            const auto patch = scene->update(pose.index, pose.x, pose.y, pose.direction,
                                             pose.progress, pose.walking, pose.hp,
                                             pose.visible ? 1 : 0, pose.ammo);
            if (patch.pixels.width && patch.pixels.height)
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            selection->update(pose.index, scaleInfantry(scene->units().at(pose.index), mapScale));
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
    if (vehicleScene) {
        for (const auto& vp : simulation->pollVehiclePoses()) {
            if (vp.index < vehicleScene->vehicles().size()) {
                vehicleScene->setOwner(vp.index, vp.owner);
                const auto dirty = vehicleScene->update(vp.index, vp.x, vp.y, vp.hullDir, vp.turretDir,
                                                        vp.hp, vp.visible ? 1 : 0, vp.ammo);
                if (mapScale == 1 && dirty.valid()) {
                    const auto patch = scene->repaintRegion(dirty.left, dirty.top, dirty.right, dirty.bottom);
                    if (patch.pixels.width && patch.pixels.height)
                        renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                }
                selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
            }
            if (vp.maxHp > 0 && vp.index < initialVehicleHp.size()) initialVehicleHp[vp.index] = vp.maxHp;
            if (vp.index < vehicleCargo.size()) vehicleCargo[vp.index] = {vp.passengerCount, vp.passengerCapacity};
            if (vp.index < vehicleTowed.size()) vehicleTowed[vp.index] = vp.towedIndex;
            if (vp.index < vehicleTowedBy.size()) vehicleTowedBy[vp.index] = vp.towedByIndex;
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
        }
    }
    for (const auto& bu : simulation->pollBuildingUpdates()) {
        if (bu.index < liveBuildings.size()) {
            auto& lb = liveBuildings[bu.index];
            lb.hp = bu.hp;
            lb.maxHp = bu.maxHp;
            lb.occupantCount = bu.occupantCount;
            lb.occupantIds = bu.occupantIds;
            lb.owner = bu.owner;
            lb.destroyed = bu.destroyed;
            if (lb.destroyed) applyBuildingRubble(bu.index);
            else restoreBuildingIntact(bu.index);
        }
    }
    (void)simulation->pollMineEvents();
    liveVisibleMines = simulation->visibleMines();
    (void)simulation->pollAirSorties();
    (void)simulation->pollAirStrikeEvents();
    (void)simulation->poll();
    syncGpuSprites();
    nextAutoSaveTick = simulation->simTick() + 7500u;
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
    std::vector<ss2::MenuItem> items;
    items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
    items.push_back({"", ss2::tr("Save Game"), "", false});
    for (int s = 0; s <= 3; ++s) {
        std::error_code ec;
        const auto p = quickSavePath(s);
        const bool exists = std::filesystem::is_regular_file(p, ec);
        const auto fsz = exists ? std::filesystem::file_size(p, ec) : 0u;
        const std::string tag = exists ? (std::to_string(std::max<std::uintmax_t>(1u, fsz / 1024u)) + " KB")
                                       : ss2::tr("Empty");
        const std::string lbl = (s == 0) ? ss2::tr("Quick Save (F9)")
                                         : ss2::trf("Save Slot {0}", {std::to_string(s)});
        items.push_back({"save:" + std::to_string(s), lbl, tag, true});
    }
    items.push_back({"", ss2::tr("Load Game"), "", false});
    {
        std::error_code ec;
        const auto p = quickSavePath(-1);
        const bool exists = std::filesystem::is_regular_file(p, ec);
        const auto fsz = exists ? std::filesystem::file_size(p, ec) : 0u;
        const std::string tag = exists ? (std::to_string(std::max<std::uintmax_t>(1u, fsz / 1024u)) + " KB")
                                       : ss2::tr("Empty");
        items.push_back({"load:-1", ss2::tr("Load Auto-Save"), tag, exists});
    }
    for (int s = 0; s <= 3; ++s) {
        std::error_code ec;
        const auto p = quickSavePath(s);
        const bool exists = std::filesystem::is_regular_file(p, ec);
        const auto fsz = exists ? std::filesystem::file_size(p, ec) : 0u;
        const std::string tag = exists ? (std::to_string(std::max<std::uintmax_t>(1u, fsz / 1024u)) + " KB")
                                       : ss2::tr("Empty");
        const std::string lbl = (s == 0) ? ss2::tr("Quick Load (F10)")
                                         : ss2::trf("Load Slot {0}", {std::to_string(s)});
        items.push_back({"load:" + std::to_string(s), lbl, tag, exists});
    }
    menu.open(ss2::tr("Save / Load"), std::move(items));
}

} // namespace ss2
