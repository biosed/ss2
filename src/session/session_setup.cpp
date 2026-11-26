#include "session/game_session.hpp"
#include "ui/cursor_bank.hpp"
#include "ui/gold_frontend.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace ss2 {

bool FrameSample::frame() {
    const auto now = Clock::now();
    if (now < warmupEnd_) return false;
    if (previous_ == Clock::time_point{}) first_ = now;
    else intervals_.push_back(std::chrono::duration<double, std::milli>(now - previous_).count());
    previous_ = now;
    complete_ = now - first_ >= std::chrono::seconds(10);
    return complete_;
}

bool FrameSample::report(float refreshRate) {
    if (!complete_ || intervals_.empty()) {
        std::cerr << "Timing sample ended before ten seconds of presentation\n";
        return false;
    }
    std::ranges::sort(intervals_);
    const auto percentile = [&](double p) {
        return intervals_[static_cast<size_t>(std::ceil(p * intervals_.size())) - 1];
    };
    std::cout << "Present-return intervals (ms), n=" << intervals_.size()
              << " p50=" << percentile(.50) << " p95=" << percentile(.95)
              << " p99=" << percentile(.99) << " max=" << intervals_.back() << '\n';
    if (refreshRate > 0) {
        const double thresholdMs = 1500.0 / refreshRate;
        const auto late = intervals_.end() - std::upper_bound(intervals_.begin(), intervals_.end(), thresholdMs);
        std::cout << "Present-return intervals above " << thresholdMs << " ms: " << late << '\n';
    }
    return true;
}

assets::Infantry scaleInfantry(assets::Infantry unit, int scale) {
    if (scale == 1) return unit;
    unit.x *= scale;
    unit.y *= scale;
    unit.left *= scale;
    unit.top *= scale;
    unit.width *= scale;
    unit.height *= scale;
    return unit;
}

assets::VehicleUnit scaleVehicle(assets::VehicleUnit unit, int scale) {
    if (scale == 1) return unit;
    unit.x *= scale;
    unit.y *= scale;
    unit.left *= scale;
    unit.top *= scale;
    unit.width *= scale;
    unit.height *= scale;
    return unit;
}

GameSession::GameSession(SDL_Window* win, ValidationReport* rep, int lim, bool tim,
                         const std::filesystem::path& gDir, const std::filesystem::path& mis,
                         const std::filesystem::path& hd, bool origInf, bool autoEng,
                         UserSettings& cfg, bool persistCfg,
                         bool ovOnly, std::optional<std::filesystem::path>* nextOut,
                         const std::filesystem::path& recPath, const cmd::ReplayFile* repFile,
                         bool manualClk, const std::filesystem::path& loadSave,
                         MultiplayerSessionConfig mpConfig,
                         std::map<unsigned, std::vector<assets::ReinforcementUnit>>* campaignPools,
                         bool liveRep)
    : window(win),
      report(rep),
      limit(lim),
      timing(tim),
      gameDir(gDir),
      mission(mis),
      hdPack(hd),
      originalInfantry(origInf),
      autoEngage(autoEng),
      settings(cfg),
      persistSettings(persistCfg),
      overviewOnly(ovOnly),
      nextMissionOut(nextOut),
      recordPath(recPath),
      replayFile(repFile),
      manualClock(manualClk),
      liveReplay(liveRep),
      loadSavePath(loadSave),
      campaignUnitPools(campaignPools),
      multiplayer(std::move(mpConfig)),
      renderer(window, report),
      menu(window),
      tooltip(window),
      combatEffects(settings.combatEffects),
      smoothTerrain(settings.smoothTerrain),
      enhancedGraphics(settings.enhancedGraphics),
      briefing(window),
      orderNotice(window),
      moveCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR), SDL_DestroyCursor),
      attackCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER), SDL_DestroyCursor),
      panCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE), SDL_DestroyCursor) {
    if (timing) {
        const auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        if (mode) {
            refreshRate = mode->refresh_rate;
            std::cout << "Display " << mode->w << 'x' << mode->h << " at " << refreshRate << " Hz\n";
        }
        std::cout << "Window pixel density " << SDL_GetWindowPixelDensity(window) << '\n';
        if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP")) std::cout << "Desktop " << desktop << '\n';
    }
    installTacticalCursorBank(window, gameDir);
    initializeMission();
    resetCursor();
    if (!pendingMissionBriefing.empty()) {
        logHud(pendingMissionBriefing);
        briefing.open(pendingMissionBriefing);
        pendingMissionBriefing.clear();
    }
    lastNoticeFrame = std::chrono::steady_clock::now();
}

void GameSession::initializeMission() {
    if (!mission.empty() && (gameDir.empty() || overviewOnly)) {
        renderer.setMap(ss2::assets::readMissionOverview(mission));
        const auto title = "Sudden Strike II | " + mission.stem().string() + " | Mission overview";
        SDL_SetWindowTitle(window, title.c_str());
    } else if (!gameDir.empty()) {
        const bool showLoadingScreen =
            window && !limit && !timing && !replayFile && !overviewOnly && !manualClock && !gameDir.empty();
        std::optional<GoldLoadingScreen> loadingScreen;
        if (showLoadingScreen) {
            loadingScreen.emplace(window, gameDir);
        }
        struct LoadingOverlayClearGuard {
            VulkanWindow& renderer;
            bool active;
            ~LoadingOverlayClearGuard() {
                if (active) renderer.clearOverlay(OverlaySlot::panel);
            }
        } loadingClearGuard{renderer, showLoadingScreen};
        const auto reportLoadProgress = [&](float progress01) {
            if (loadingScreen) {
                loadingScreen->drawAndPresent(renderer, currentMissionTitle, progress01);
            }
        };

        const auto missionPath = mission.empty()
            ? ss2::assets::resolveCaseInsensitivePath(gameDir, "maps/single/--- TUTORIAL I ---.SSM")
            : mission;
        currentMissionPath = missionPath;
        const bool isTutorial1 = mission.empty() ||
            ss2::assets::equalsIgnoreCaseAscii(missionPath.filename().string(), "--- TUTORIAL I ---.SSM");
        if (!isTutorial1) {
            const auto normalizeMissionIdentityPath = [](const std::filesystem::path& p) {
                std::error_code normEc;
                auto norm = std::filesystem::weakly_canonical(p, normEc);
                if (normEc || norm.empty()) norm = p.lexically_normal();
                return norm;
            };
            const auto normCur = normalizeMissionIdentityPath(missionPath);
            for (const auto& c : ss2::assets::listCampaigns(gameDir)) {
                for (std::size_t mi = 0; mi < c.missions.size(); ++mi) {
                    const auto normM = normalizeMissionIdentityPath(c.missions[mi].path);
                    const auto normE = !c.missions[mi].easyPath.empty()
                        ? normalizeMissionIdentityPath(c.missions[mi].easyPath)
                        : std::filesystem::path{};
                    if (normM == normCur || (!normE.empty() && normE == normCur)) {
                        currentMissionTitle = c.missions[mi].title;
                        const bool isEasy = (!normE.empty() && normE == normCur);
                        if (mi + 1 < c.missions.size()) {
                            nextCampaignMissionPath = (isEasy && !c.missions[mi + 1].easyPath.empty())
                                ? c.missions[mi + 1].easyPath
                                : c.missions[mi + 1].path;
                            nextCampaignMissionTitle = c.missions[mi + 1].title;
                        }
                        break;
                    }
                }
                if (!currentMissionTitle.empty()) break;
            }
            if (currentMissionTitle.empty()) {
                for (const auto& m : ss2::assets::listSinglePlayerMissions(gameDir)) {
                    const auto normM = normalizeMissionIdentityPath(m.path);
                    if (normM == normCur) {
                        currentMissionTitle = m.name;
                        break;
                    }
                }
            }
            if (currentMissionTitle.empty()) {
                currentMissionTitle = missionPath.stem().string();
            }
        } else {
            currentMissionTitle = "Tutorial I";
        }
        auto data = ss2::assets::readMissionData(missionPath);
        if (!data.description.empty()) {
            missionBriefingText = data.description;
        }
        if (!isTutorial1 && !limit && !timing && !replayFile && !data.description.empty()) {
            pendingMissionBriefing = data.description;
        }
        fogGridW = data.width;
        fogGridH = data.height;
        mapScheme = data.scheme;
        missionWeather = data.weather;
        unitCatalog = ss2::assets::UnitCatalog::load(gameDir, data.scheme);
        reportLoadProgress(0.10f);
        auto map = isTutorial1
            ? ss2::assets::loadTutorialTerrain(gameDir)
            : ss2::assets::loadMissionTerrain(gameDir, missionPath);
        ss2::assets::SceneryDepth depth(map.width, map.height);
        std::vector<ss2::assets::BuildingPlacement> missionBuildings;
        std::vector<ss2::assets::BridgePlacement> missionBridges;
        bool expectHd2xMap = false;
        if (isTutorial1 && !hdPack.empty()) {
            std::error_code hdEc;
            const bool hdIsDir = std::filesystem::is_directory(hdPack, hdEc);
            const auto groundPath = hdIsDir ? (hdPack / "tutorial1_ground_2x.ss2hd") : std::filesystem::path{};
            const auto bakedPath = hdIsDir ? (hdPack / "tutorial1_2x.ss2hd") : hdPack;
            const bool hasGround = !groundPath.empty() && std::filesystem::is_regular_file(groundPath, hdEc);
            expectHd2xMap = hasGround || !hdIsDir || std::filesystem::is_regular_file(bakedPath, hdEc);
        }
        (void)expectHd2xMap;
        destructibleTrees.clear();
        treeFallen.clear();
        std::vector<ss2::assets::TreePlacement>* treeOut = &destructibleTrees;
        const auto objects = isTutorial1
            ? ss2::assets::drawTutorialScenery(map, gameDir, &depth, 0, &missionBuildings, &missionBridges, treeOut)
            : ss2::assets::drawMissionScenery(map, gameDir, missionPath, &depth, &missionBuildings, &missionBridges, treeOut);
        reportLoadProgress(0.45f);
        const auto placements = isTutorial1
            ? ss2::assets::readTutorialPlacements(gameDir)
            : ss2::assets::readMissionPlacements(missionPath);
        vehicleScene.emplace(gameDir, placements);
        const auto& vehicleUnits = vehicleScene->vehicles();
        std::vector<ss2::assets::Placement> vehicles;
        vehicles.reserve(vehicleUnits.size());
        for (const auto& v : vehicleUnits) {
            vehicles.push_back({v.id, v.x, v.y, int(v.hullDir), v.hp, v.ammo, v.group, v.owner});
        }
        std::size_t drawnVehicles = vehicleUnits.size();
        const auto mapWidth = map.width, mapHeight = map.height;
        ss2::assets::MapImage hdImage;
        std::filesystem::path infantryArt;
        bool cleanGround = false;
        std::set<std::string> preloadRosterIds;
        {
            auto collectId = [&](const std::string& id) {
                if (!id.empty()) preloadRosterIds.insert(id);
            };
            auto collectWave = [&](const std::vector<ss2::assets::ReinforcementUnit>& wave) {
                for (const auto& ru : wave) {
                    collectId(ru.id);
                    for (const auto& pax : ru.passengers) {
                        if (!pax.isCrew) collectId(pax.id);
                    }
                }
            };
            for (const auto& wave : data.reinforcements) collectWave(wave);
            for (const auto& roster : data.paratrooperRoster) {
                for (const auto& pe : roster) collectId(pe.unitId);
            }
            if (campaignUnitPools) {
                for (const auto& [poolIdx, wave] : *campaignUnitPools) {
                    (void)poolIdx;
                    collectWave(wave);
                }
            }
            if (replayFile) {
                for (const auto& [poolIdx, wave] : replayFile->initialSavedPools) {
                    (void)poolIdx;
                    collectWave(wave);
                }
            }
        }
        std::set<std::string> vehicleIds;
        for (const auto& v : vehicleUnits) vehicleIds.insert(v.id);
        gpuArtDir.clear();
        gpuVehicleArtDir.clear();
        if (!hdPack.empty() && !originalInfantry) {
            const auto candInfArt = hdPack / "infantry-art-4x";
            std::error_code infEc;
            if (std::filesystem::is_directory(candInfArt, infEc)) {
                gpuArtDir = candInfArt;
            }
            const auto candVehArt = hdPack / "vehicles-art-4x";
            std::error_code vehEc;
            if (std::filesystem::is_directory(candVehArt, vehEc)) {
                gpuVehicleArtDir = candVehArt;
            }
        }
        if (vehicleScene) {
            for (const auto& v : vehicleUnits) {
                (void)ensureGpuVehicleSlot(v.id);
            }
            for (const auto& rid : preloadRosterIds) {
                const auto* sp = unitCatalog ? unitCatalog->find(rid) : nullptr;
                if (vehicleIds.contains(rid) || (sp && sp->isVehicle)) {
                    (void)ensureGpuVehicleSlot(rid);
                }
            }
            if (!gpuVehicleIds.empty()) {
                vehicleScene->setGpuRenderedIds(&gpuVehicleIds);
                std::cout << "GPU vehicles: " << gpuHdVehicles.size() << " 4x HD + "
                          << gpuPckVehicleSlots.size() << " 2x EPX classes\n";
            }
        }
        for (const auto& ownerSlots : data.airSupport) {
            for (const auto& slot : ownerSlots) {
                if (slot.planeId.empty()) continue;
                if (ensureAircraftAtlas(slot.planeId)) {
                    const unsigned pSlot = planeAtlas.acquire(slot.planeId);
                    if (pSlot != planeAtlas.overflowSlot() && !boundPlaneSlots.contains(pSlot)) {
                        renderer.setSpriteAtlas(pSlot, gpuAircraftAtlases.at(slot.planeId).atlas);
                        boundPlaneSlots[pSlot] = slot.planeId;
                    }
                }
            }
        }
        if (isTutorial1 && !hdPack.empty()) {
            std::error_code hdEc;
            const bool hdIsDir = std::filesystem::is_directory(hdPack, hdEc);
            const auto groundPath = hdIsDir ? (hdPack / "tutorial1_ground_2x.ss2hd") : std::filesystem::path{};
            const auto bakedPath = hdIsDir ? (hdPack / "tutorial1_2x.ss2hd") : hdPack;
            cleanGround = !groundPath.empty() && std::filesystem::is_regular_file(groundPath, hdEc);
            const bool hasTutorialHdMap = cleanGround || !hdIsDir || std::filesystem::is_regular_file(bakedPath, hdEc);
            if (hasTutorialHdMap) {
                if (cleanGround && !originalInfantry && std::filesystem::is_directory(hdPack / "infantry-art", hdEc))
                    infantryArt = hdPack / "infantry-art";
                auto hd = ss2::assets::loadHdPackMap(cleanGround ? groundPath : bakedPath);
                if (hd.scale != 2) throw std::runtime_error("Tutorial I requires a 2x HD pack");
                mapScale = 2;
                hdImage = std::move(hd.image);
                const auto worldDir = hdPack / "world-sprites-4x";
                const auto manifestPath = worldDir / "manifest.txt";
                if (cleanGround && std::filesystem::is_regular_file(manifestPath)) {
                    std::map<std::string, unsigned> worldSlots;
                    std::ifstream mf(manifestPath);
                    std::string line;
                    while (std::getline(mf, line)) {
                        std::istringstream iss(line);
                        std::string name;
                        float dx, dy, dw, dh, u0, v0, u1, v1;
                        int sy = 0;
                        if (!(iss >> name >> dx >> dy >> dw >> dh >> u0 >> v0 >> u1 >> v1 >> sy)) continue;
                        if (!worldSlots.contains(name)) {
                            if (gpuNextSlot >= planeAtlasBase - 2u) continue;
                            const auto ap = worldDir / (name + ".png");
                            if (!std::filesystem::is_regular_file(ap)) continue;
                            auto atlas = ss2::assets::loadPngRgba(ap);
                            renderer.setSpriteAtlas(gpuNextSlot, atlas);
                            worldSlots.emplace(name, gpuNextSlot++);
                        }
                        worldGpuSprites.push_back({
                            {dx, dy, dw, dh, u0, v0, u1, v1, worldSlots.at(name), 1.0f, 1.0f},
                            sy,
                            static_cast<int>((dx + dw * 0.5f) * 0.5f),
                            static_cast<int>((dy + dh * 0.5f) * 0.5f)
                        });
                    }
                    if (!worldGpuSprites.empty()) {
                        std::cout << "GPU 4x HD world sprites: " << worldGpuSprites.size()
                                  << " placements across " << worldSlots.size() << " atlases\n";
                    }
                }
            }
        }
        if (isTutorial1 && mapScale == 2 && !cleanGround) {
            ss2::assets::drawTutorialVehicles(map, gameDir, placements);
        }
        scene.emplace(std::move(map), gameDir, placements, originalInfantry, std::move(hdImage), infantryArt,
                      cleanGround, std::move(depth), &gpuAtlasSlots, data.width, data.height);
        treeFallen.assign(destructibleTrees.size(), false);
        if (mapScale == 1) {
            scene->setVehicleLayer([&](ss2::assets::MapImage& patch, int px, int py, const ss2::assets::SceneryDepth* d) {
                if (vehicleScene) vehicleScene->drawRegion(patch, px, py, d);
            });
        }
        for (const auto& p : placements) {
            if (!vehicleIds.contains(p.id)) (void)ensureGpuInfantrySlot(p.id);
        }
        for (const auto& rid : preloadRosterIds) {
            const auto* sp = unitCatalog ? unitCatalog->find(rid) : nullptr;
            if (!vehicleIds.contains(rid) && !(sp && sp->isVehicle)) {
                (void)ensureGpuInfantrySlot(rid);
            }
        }
        std::vector<std::pair<std::size_t, std::size_t>> preloadedRiders;
        {
            std::size_t pCursor = 0;
            for (std::size_t vIdx = 0; vIdx < vehicleUnits.size(); ++vIdx) {
                const auto& vu = vehicleUnits[vIdx];
                while (pCursor < placements.size() &&
                       (placements[pCursor].id != vu.id || placements[pCursor].x != vu.x ||
                        placements[pCursor].y != vu.y || placements[pCursor].owner != vu.owner)) {
                    ++pCursor;
                }
                if (pCursor >= placements.size()) break;
                for (const auto& pax : placements[pCursor].passengers) {
                    if (pax.isCrew) continue;
                    ss2::assets::Placement rp{pax.id, vu.x, vu.y, 0, pax.hp, pax.ammo, pax.group, vu.owner, {}, pax.expa};
                    (void)ensureGpuInfantrySlot(pax.id);
                    if (auto spawned = scene->spawn(rp)) {
                        const auto sIdx = spawned->first;
                        const auto& u = scene->units().at(sIdx);
                        (void)scene->update(sIdx, u.x, u.y, u.direction, u.progress, u.walking, u.hp, 0);
                        preloadedRiders.emplace_back(sIdx, vIdx);
                    }
                }
                ++pCursor;
            }
        }
        if (!gpuAtlasSlots.empty()) {
            const std::size_t cnt2x = gpuInfantry2xIds.size();
            const std::size_t cnt4x = gpuAtlasSlots.size() > cnt2x ? (gpuAtlasSlots.size() - cnt2x) : 0u;
            std::cout << "GPU infantry atlases: " << cnt4x << " 4x HD + " << cnt2x << " 2x EPX classes\n";
        } else if (!infantryArt.empty()) {
            std::cout << "Painted HD infantry: " << scene->paintedCount() << "/" << scene->units().size() << '\n';
        }
        gpuPreloadLogged = true;
        reportLoadProgress(0.75f);
        const auto& infantry = scene->units();
        if (!isTutorial1) {
            if (!data.description.empty()) {
                const auto eol = data.description.find_first_of("\r\n");
                currentObjective = data.description.substr(0, eol);
            } else {
                currentObjective = missionPath.stem().string();
            }
        }
        initialUnitHp.clear();
        initialUnitHp.reserve(infantry.size());
        unitExpa.clear();
        unitExpa.reserve(infantry.size());
        unitKills.assign(infantry.size(), 0);
        unitGrenades.clear();
        unitGrenades.reserve(infantry.size());
        unitMineAmmo.clear();
        unitMineAmmo.reserve(infantry.size());
        for (const auto& u : infantry) {
            initialUnitHp.push_back(std::max(1, u.hp));
            unitExpa.push_back(ss2::TutorialUnits::missionExpaToSim(u.expa));
            unitGrenades.push_back(initialGrenadesForId(u.id));
            unitMineAmmo.push_back(initialMinesForId(u.id));
        }
        initialVehicleHp.clear();
        initialVehicleHp.reserve(vehicleUnits.size());
        vehExpa.clear();
        vehExpa.reserve(vehicleUnits.size());
        vehKills.assign(vehicleUnits.size(), 0);
        vehicleAmmo2.assign(vehicleUnits.size(), 0);
        vehicleCargo.assign(vehicleUnits.size(), {0, 0});
        vehicleTowed.assign(vehicleUnits.size(), -1);
        vehicleTowedBy.assign(vehicleUnits.size(), -1);
        vehicleHatchOpen.assign(vehicleUnits.size(), false);
        liveBuildings.clear();
        liveBuildings.reserve(missionBuildings.size());
        for (auto& bp : missionBuildings) {
            ss2::assets::SpriteCoverage initShadow{};
            bool hasInitShadow = false;
            const auto& sh0 = bp.stageBlocks[3];
            if (sh0.size() >= 9u && sh0[8] == 0xa3u &&
                (sh0[4] != 0u || sh0[5] != 0u) && (sh0[6] != 0u || sh0[7] != 0u)) {
                initShadow = ss2::assets::readShadowCoverage(bp.x, bp.y, sh0);
                hasInitShadow = (initShadow.width > 0 && initShadow.height > 0);
            }
            liveBuildings.push_back({
                bp.x, bp.y, bp.left, bp.top, bp.width, bp.height,
                bp.maxHp, bp.maxHp, bp.capacity, 0, -1, false,
                bp.ruinLeft, bp.ruinTop, std::move(bp.ruinedPatch),
                std::move(bp.basePatch), {}, bp.palette, std::move(bp.stageBlocks),
                0, initShadow, hasInitShadow, {}
            });
        }
        liveBridges.clear();
        liveBridges.reserve(missionBridges.size());
        for (auto& br : missionBridges) {
            assets::BridgePlacement renderBp;
            renderBp.x = br.x;
            renderBp.y = br.y;
            renderBp.u = br.u;
            renderBp.v = br.v;
            renderBp.left = br.left;
            renderBp.top = br.top;
            renderBp.width = br.width;
            renderBp.height = br.height;
            renderBp.maxHp = br.maxHp;
            renderBp.patchLeft = br.patchLeft;
            renderBp.patchTop = br.patchTop;
            renderBp.basePatch = std::move(br.basePatch);
            renderBp.palette = br.palette;
            renderBp.stageFrames = std::move(br.stageFrames);
            renderBp.stageShadows = std::move(br.stageShadows);
            liveBridges.push_back({
                br.x, br.y, br.left, br.top, br.width, br.height,
                br.maxHp, br.maxHp, 0, false,
                std::move(renderBp), 0
            });
        }
        craterCatalog = ss2::assets::loadCraterCatalog(gameDir, mapScheme);
        for (std::size_t vi = 0; vi < vehicleUnits.size(); ++vi) {
            const auto& v = vehicleUnits[vi];
            vehExpa.push_back(ss2::TutorialUnits::missionExpaToSim(v.expa));
            int maxHp = std::max(1, v.hp);
            if (unitCatalog) {
                if (const auto* spec = unitCatalog->find(v.id)) {
                    if (spec->health > 0) maxHp = spec->health;
                    vehicleCargo[vi].second = std::clamp(spec->stock, 0, 44);
                    vehicleHatchOpen[vi] = spec->marchEnabled || (spec->marchSightBonus > 0);
                    vehicleAmmo2[vi] = spec->ammo2 > 0
                        ? std::max(0, (spec->ammo2 * std::clamp(v.ammo, 0, 100)) / 100)
                        : (spec->shot2.damage > 0 && spec->shot2.range > 0 ? std::max(1, v.ammo) : 0);
                }
            }
            initialVehicleHp.push_back(maxHp);
        }
        std::vector<ss2::assets::Infantry> viewInfantry = infantry;
        auto viewVehicles = vehicles;
        std::vector<ss2::assets::VehicleUnit> viewVehicleUnits;
        viewVehicleUnits.reserve(vehicleUnits.size());
        for (const auto& v : vehicleUnits) viewVehicleUnits.push_back(scaleVehicle(v, mapScale));
        const auto composedMap = scene->compose();
        hudMapW = mapWidth;
        hudMapH = mapHeight;
        hud.emplace(window);
        hud->loadRetailAssets(gameDir);
        hud->setWeather(data.weather, 0u);
        baseMinimap = downsampleOverview(composedMap, 768u, 384u);
        hud->setMinimap(baseMinimap);
        if (!hdPack.empty() && !originalInfantry && std::filesystem::is_directory(hdPack / "hud-portraits")) {
            std::vector<std::filesystem::path> portraitFiles;
            for (const auto& entry : std::filesystem::directory_iterator(hdPack / "hud-portraits")) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (ext == ".png") portraitFiles.push_back(entry.path());
            }
            std::ranges::sort(portraitFiles);
            for (const auto& pp : portraitFiles) {
                const std::string cls = pp.stem().string();
                if (cls.empty()) continue;
                const auto img = ss2::assets::loadPngRgba(pp);
                const std::string lbl = hudClassLabel(cls);
                hud->setPortrait(lbl, img);
                registeredPortraits.insert(lbl);
                const auto disp = hudDisplayName(cls);
                if (disp != lbl) {
                    hud->setPortrait(disp, img);
                    registeredPortraits.insert(disp);
                }
            }
        }
        for (const auto& u : infantry) ensureUnitPortrait(u.id, false);
        for (const auto& v : vehicleUnits) ensureUnitPortrait(v.id, true);
        renderer.setMap(composedMap);
        if (hud && hud->hasAirVfxAtlas() && gpuNextSlot + 1u < planeAtlasBase) {
            const unsigned airVfxSlot = gpuNextSlot++;
            renderer.setSpriteAtlas(airVfxSlot, hud->airVfxAtlas());
            hud->bindAirVfxAtlasSlot(airVfxSlot);
        }
        (void)combatVfx.loadFromArchives(ss2::assets::resolveGameArchives(gameDir));
        if (combatVfx.hasAtlas() && gpuNextSlot < planeAtlasBase) {
            const unsigned vfxSlot = gpuNextSlot++;
            renderer.setSpriteAtlas(vfxSlot, combatVfx.atlas());
            combatVfx.bindAtlasSlot(vfxSlot);
        }
        if (isTutorial1 && mapScale == 2 && !smoothTerrain) {
            smoothTerrain = true;
        }
        applyMapGraphics();
        if (isTutorial1 && mapScale == 2) {
            for (auto& u : viewInfantry) u = scaleInfantry(u, mapScale);
            for (auto& v : viewVehicles) {
                v.x *= mapScale;
                v.y *= mapScale;
            }
            SDL_SetWindowTitle(window, "Sudden Strike II | Tutorial I (2x HD)");
            std::cout << "Tutorial I (2x HD): " << mapWidth*2 << 'x' << mapHeight*2 << ", " << objects
                      << " scenery placements, " << infantry.size() << " infantry, " << drawnVehicles << " vehicle and gun placements\n";
        } else if (isTutorial1) {
            std::cout << "Tutorial I: " << mapWidth << 'x' << mapHeight << ", " << objects
                      << " scenery placements, " << infantry.size() << " infantry, " << drawnVehicles << " vehicle and gun placements\n";
        } else {
            const auto title = "Sudden Strike II | " + missionPath.stem().string() +
                (multiplayer.enabled
                    ? (multiplayer.playerCount > 2
                        ? (" [MP Seat " + std::to_string(localOwner() + 1) + "/" +
                           std::to_string(multiplayer.playerCount) + " T" +
                           std::to_string(multiplayer.playerTeams[localOwner() & 3] + 1) + "]")
                        : (" [MP Seat " + std::to_string(localOwner() + 1) + "]"))
                    : "");
            SDL_SetWindowTitle(window, title.c_str());
            std::cout << "Mission " << missionPath.stem().string() << ": " << mapWidth << 'x' << mapHeight << ", "
                      << objects << " scenery placements, " << infantry.size() << " infantry, "
                      << drawnVehicles << " vehicle placements\n";
        }
        if (scene) scene->setFogViewerOwner(localOwner());
        ss2::FogGrid fog(data.width, data.height, data.flags);
        fog.applyMapPerimeterFlags();
        if (!fog.loadCirc64FromGame(gameDir)) {
            std::cerr << "Warning: failed to load CIRC64 visibility table from "
                      << gameDir << "; falling back to approximate fog sight rings.\n";
        }
        terrainObstacles = data.obstacles;
        auto navigation = ss2::InfantryPathGrid::fromSubCellMasks(data.width, data.height, std::move(data.flags), std::move(data.obstacles));
        missionPlayerTeams = multiplayer.enabled ? multiplayer.playerTeams : data.playerTeams;
        const auto initMines = data.mines;
        const int effDiff = multiplayer.enabled ? 2 : std::clamp(settings.difficulty, 1, 3);
        ss2::TutorialUnits soldiers(std::move(navigation), infantry, vehicles, std::move(fog),
                                    effDiff, autoEngage, vehicleUnits, unitCatalog);
        soldiers.setPlayerTeams(missionPlayerTeams);
        soldiers.setViewerOwner(localOwner());
        soldiers.setMultiplayerSymmetry(multiplayer.enabled);
        soldiers.setMines(initMines);
        soldiers.setBuildings(std::move(missionBuildings));
        soldiers.setBridges(std::move(missionBridges));
        soldiers.setAirSupport(data.airSupport, data.paratrooperGroup, data.paratrooperRoster);
        const auto initAir = soldiers.airSupportFor(localOwner());
        for (const auto& [sIdx, vIdx] : preloadedRiders) {
            if (soldiers.embarkImmediate(sIdx, vIdx)) {
                if (vIdx < vehicleCargo.size()) ++vehicleCargo[vIdx].first;
            }
        }
        // Hide unspotted non-allied enemies from frame 0 and seed real catalog soldier ammo counts.
        const auto initSnap = soldiers.takeSnapshot();
        for (size_t i = 0; i < infantry.size(); ++i) {
            const int initAmmo = (i < initSnap.soldiers.size()) ? initSnap.soldiers[i].ammo : -1;
            const bool hiddenEnemy = !isAllyOwner(infantry[i].owner) && !soldiers.isValidAttackTarget(i, localOwner());
            if (hiddenEnemy || initAmmo >= 0) {
                const auto& u = scene->units().at(i);
                scene->update(i, u.x, u.y, u.direction, u.progress, u.walking, u.hp,
                              hiddenEnemy ? 0 : (u.visible ? 1 : 0), initAmmo);
                if (hiddenEnemy) viewInfantry[i].visible = false;
                if (initAmmo >= 0) viewInfantry[i].ammo = initAmmo;
            }
        }
        for (size_t i = 0; i < viewVehicleUnits.size(); ++i) {
            if (!isAllyOwner(viewVehicleUnits[i].owner) && !soldiers.isValidVehicleAttackTarget(i, localOwner())) {
                const auto& v = vehicleScene->vehicles().at(i);
                vehicleScene->update(i, v.x, v.y, v.hullDir, v.turretDir, v.hp, 0);
                viewVehicleUnits[i].visible = false;
            }
        }
        for (const auto& vp : soldiers.takeVehiclePoses()) {
            vehicleScene->update(vp.index, vp.x, vp.y, vp.hullDir, vp.turretDir,
                                 vp.hp, vp.visible ? 1 : 0, vp.ammo);
            if (vp.index < viewVehicleUnits.size()) {
                viewVehicleUnits[vp.index].visible = vp.visible;
                viewVehicleUnits[vp.index].hp = vp.hp;
                viewVehicleUnits[vp.index].ammo = vp.ammo;
            }
            if (vp.index < vehicleAmmo2.size()) {
                vehicleAmmo2[vp.index] = vp.ammo2;
            }
        }
        if (auto initialFog = soldiers.takeFogSnapshot()) {
            latestFog = *initialFog;
            const auto fogPatch = scene->updateFog(*initialFog);
            if (fogPatch.pixels.width && fogPatch.pixels.height)
                renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
            updateMinimapFog(*initialFog, true);
        }
        syncGpuSprites();
        try {
            audio.emplace(ss2::assets::SoundBank::load(gameDir));
            if (!hdPack.empty() && std::filesystem::is_directory(hdPack / "audio-hd"))
                audio->setHdAudioDir(hdPack / "audio-hd");
            const auto hdMusic = (!hdPack.empty() && std::filesystem::is_directory(hdPack / "audio-hd" / "music"))
                                     ? (hdPack / "audio-hd" / "music")
                                     : ss2::assets::resolveMusicDir(gameDir);
            audio->setSfxVolume(settings.sfxVolume);
            audio->startMusic(hdMusic, settings.musicVolume);
            if (settings.musicMuted) (void)audio->toggleMusicMute();
        } catch (const std::exception&) {}
        const int selOriginX = int(data.height) * 32;
        const std::string selTitle = isTutorial1 ? (mapScale == 2 ? "Tutorial I (2x HD)" : "Tutorial I")
                                                 : missionPath.stem().string();
        currentMissionTitle = selTitle;
        initialSavedPools.clear();
        const auto& rawPools = replayFile
            ? replayFile->initialSavedPools
            : (campaignUnitPools ? *campaignUnitPools
                                 : std::map<unsigned, std::vector<assets::ReinforcementUnit>>{});
        for (const auto& [poolIdx, wave] : rawPools) {
            if (!wave.empty()) initialSavedPools.emplace(poolIdx, wave);
        }
        if (replayFile && !liveReplay) {
            simulation.emplace(ss2::MissionRunner::ManualClock{}, std::move(data), std::move(soldiers),
                               initialSavedPools);
            if (!simulation->replay(replayFile->commands, replayFile->endTick)) {
                throw std::runtime_error("Replay log failed before reaching end_tick " +
                                         std::to_string(replayFile->endTick));
            }
            const auto actualHash = simulation->stateHash();
            if (replayFile->endStateHash && actualHash != *replayFile->endStateHash) {
                std::ostringstream oss;
                oss << "Replay stateHash mismatch at tick " << simulation->simTick()
                    << ": expected 0x" << std::hex << *replayFile->endStateHash
                    << ", got 0x" << actualHash;
                throw std::runtime_error(oss.str());
            }
            std::cout << "Replay verified: " << replayFile->commands.size()
                      << " commands, tick " << simulation->simTick()
                      << ", stateHash 0x" << std::hex << actualHash << std::dec << '\n';
        } else if (replayFile && liveReplay) {
            simulation.emplace(ss2::MissionRunner::ManualClock{}, std::move(data), std::move(soldiers),
                               initialSavedPools);
            replayTransport = cmd::ReplayTransport{};
            lastReplayStepFrame = std::chrono::steady_clock::now();
            pendingMissionBriefing.clear();
        } else if (multiplayer.enabled) {
            simulation.emplace(ss2::MissionRunner::ManualClock{}, std::move(data), std::move(soldiers));
            if (!recordPath.empty()) simulation->startRecording();
            mpTransport.emplace(
                multiplayer.isHost,
                static_cast<std::uint8_t>(localOwner()),
                static_cast<std::uint8_t>(multiplayer.playerCount),
                missionPlayerTeams,
                multiplayer.peers);
            lockstep.emplace(ss2::LockstepConfig{
                static_cast<std::uint8_t>(localOwner()),
                static_cast<std::uint8_t>(multiplayer.playerCount),
                multiplayer.turnTicks,
                multiplayer.inputDelayTurns,
                multiplayer.hashIntervalTurns,
            });
            simulation->setCommandSink([this](ss2::cmd::Command c) {
                if (lockstep) lockstep->queueLocalCommand(std::move(c));
            });
            nextLockstepTickAt = std::chrono::steady_clock::now();
        } else if (manualClock) {
            simulation.emplace(ss2::MissionRunner::ManualClock{}, std::move(data), std::move(soldiers),
                               initialSavedPools);
            simulation->startRecording();
        } else {
            simulation.emplace(std::move(data), ss2::tutorialTurnDelay(gameDir), std::move(soldiers),
                               !recordPath.empty(), initialSavedPools);
            if (settings.speedPercent != 100) simulation->setSpeedPercent(settings.speedPercent);
        }
        if (simulation) {
            const bool useRealism = (replayFile && replayFile->realismDoctrine)
                ? *replayFile->realismDoctrine
                : (!multiplayer.enabled && !replayFile && settings.realismDoctrine);
            if (useRealism) {
                simulation->setRealismDoctrine(true);
            }
        }
        liveVisibleMines = simulation->visibleMines();
        selection.emplace(window, renderer, std::move(viewInfantry), std::move(viewVehicles), mapScale, selOriginX, selTitle);
        selection->setVehicles(std::move(viewVehicleUnits));
        selection->setLocalPlayer(localOwner());
        if (multiplayer.enabled && simulation) {
            simulation->configureMultiplayerSkirmish({
                multiplayer.playerCount,
                multiplayer.playerNations,
                missionPlayerTeams,
                375u,
                0u,
                4,
            });
            processLockstepPostTickSpawns();
            bool hasLocalUnit = false;
            for (const auto& u : scene->units()) {
                if (u.owner == localOwner() && u.hp > 0) { hasLocalUnit = true; break; }
            }
            if (!hasLocalUnit && vehicleScene) {
                for (const auto& v : vehicleScene->vehicles()) {
                    if (v.owner == localOwner() && v.alive && v.hp > 0) { hasLocalUnit = true; break; }
                }
            }
            if (!hasLocalUnit) {
                const int gate = simulation->assignedGateForSeat(localOwner());
                const auto& gates = simulation->data().gates;
                if (gate >= 0 && static_cast<std::size_t>(gate) < gates.size() &&
                    (gates[gate].first != 0 || gates[gate].second != 0)) {
                    renderer.focusMap(static_cast<double>(gates[gate].first * mapScale),
                                      static_cast<double>(gates[gate].second * mapScale), true);
                }
            }
        }
        {
            int airReadyCount = 0;
            for (const auto& sl : initAir) airReadyCount += sl.ready;
            if (airReadyCount > 0) {
                std::cout << "Air Support ready: " << airReadyCount << " sorties\n";
            }
        }
        if (!loadSavePath.empty()) {
            if (!loadSaveFromPath(loadSavePath, ss2::tr("Loaded saved game."), true)) {
                throw std::runtime_error("Failed to load saved game: " + loadSavePath.string());
            }
            pendingMissionBriefing.clear();
            bool focused = false;
            for (const auto& u : scene->units()) {
                if (u.owner == localOwner() && u.hp > 0 && u.visible) {
                    renderer.focusMap(static_cast<double>(u.x * mapScale), static_cast<double>(u.y * mapScale));
                    focused = true;
                    break;
                }
            }
            if (!focused && vehicleScene) {
                for (const auto& v : vehicleScene->vehicles()) {
                    if (v.alive && v.hp > 0 && v.owner == localOwner()) {
                        renderer.focusMap(static_cast<double>(v.x * mapScale), static_cast<double>(v.y * mapScale));
                        break;
                    }
                }
            }
        } else if (!isTutorial1) {
            bool focused = false;
            for (const auto& u : infantry) {
                if (u.owner == localOwner()) {
                    renderer.focusMap(static_cast<double>(u.x * mapScale), static_cast<double>(u.y * mapScale));
                    focused = true;
                    break;
                }
            }
            if (!focused) {
                for (const auto& v : vehicleUnits) {
                    if (v.owner == localOwner()) {
                        renderer.focusMap(static_cast<double>(v.x * mapScale), static_cast<double>(v.y * mapScale));
                        break;
                    }
                }
            }
        }
        reportLoadProgress(1.00f);
        if (!multiplayer.enabled && !limit && !timing && !replayFile && nextMissionOut &&
            loadSavePath.empty() && simulation) {
            doSaveSlot(-2, true);
        }
    }
}

void GameSession::writeRecordingIfRequested() {
    if (!recordPath.empty() && simulation) {
        simulation->stopWorker();
        ss2::cmd::ReplayFile recorded;
        if (!currentMissionPath.empty()) {
            std::error_code relEc;
            const auto rel = !gameDir.empty() ? std::filesystem::relative(currentMissionPath, gameDir, relEc)
                                              : std::filesystem::path{};
            recorded.mission = (!relEc && !rel.empty() && !rel.generic_string().starts_with(".."))
                ? rel.generic_string()
                : currentMissionPath.filename().string();
        }
        recorded.difficulty = std::clamp(settings.difficulty, 1, 3);
        recorded.autoEngage = autoEngage;
        if (simulation->realismDoctrine()) recorded.realismDoctrine = true;
        recorded.endTick = simulation->simTick();
        recorded.endStateHash = simulation->stateHash();
        recorded.initialSavedPools = initialSavedPools;
        recorded.commands = simulation->takeRecording();
        if (recordPath.has_parent_path()) {
            std::error_code dirEc;
            std::filesystem::create_directories(recordPath.parent_path(), dirEc);
        }
        std::ofstream out(recordPath, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Failed to open --record file for writing: " + recordPath.string());
        const auto text = ss2::cmd::toReplayFileText(recorded);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) throw std::runtime_error("Failed to write --record file: " + recordPath.string());
        std::cout << "Recorded " << recorded.commands.size() << " commands through tick " << recorded.endTick
                  << " (stateHash 0x" << std::hex << *recorded.endStateHash << std::dec << ") to "
                  << recordPath << '\n';
    }
}

int GameSession::run() {
    while (running && (!limit || frames < static_cast<unsigned>(limit))) {
        stepSimulationEvents();
        pollInputEvents();
        if (!running) break;
        updateHudAndBeams();
        if (updateViewTooltipAndPresent()) break;
    }
    resetCursor();
    if (audio) audio->stopAllLoops();
    if (multiplayer.enabled && mpTransport && mpTransport->isActive()) {
        (void)mpTransport->sendBye(ss2::ByePacket{static_cast<std::uint8_t>(localOwner()), "Session ended"});
    }
    writeRecordingIfRequested();
    if (campaignUnitPools && simulation && (!nextMissionOut || !nextMissionOut->has_value())) {
        *campaignUnitPools = simulation->savedUnitPools();
    }
    std::cout << "Presented " << frames << " frames\n";
    if (timing && !sample.report(refreshRate)) return 1;
    return limit && frames < static_cast<unsigned>(limit) ? 1 : 0;
}

int runGameSession(SDL_Window* window, ValidationReport* report, int limit, bool timing,
                   const std::filesystem::path& gameDir, const std::filesystem::path& mission,
                   const std::filesystem::path& hdPack, bool originalInfantry, bool autoEngage,
                   UserSettings& settings, bool persistSettings,
                   bool overviewOnly, std::optional<std::filesystem::path>* nextMissionOut,
                   const std::filesystem::path& recordPath,
                   const cmd::ReplayFile* replayFile,
                   bool manualClock,
                   const std::filesystem::path& loadSavePath,
                   MultiplayerSessionConfig multiplayer,
                   std::map<unsigned, std::vector<assets::ReinforcementUnit>>* campaignPools,
                   bool liveReplay) {
    GameSession session(window, report, limit, timing, gameDir, mission, hdPack, originalInfantry,
                        autoEngage, settings, persistSettings, overviewOnly, nextMissionOut,
                        recordPath, replayFile, manualClock, loadSavePath, std::move(multiplayer),
                        campaignPools, liveReplay);
    return session.run();
}

int verifyReplayHeadless(const std::filesystem::path& gameDir, const std::filesystem::path& mission,
                         bool originalInfantry, int difficulty, bool autoEngage,
                         const cmd::ReplayFile& replayFile) {
    const auto missionPath = mission.empty()
        ? ss2::assets::resolveTutorialMissionPath(gameDir)
        : mission;
    const bool isTutorial1 = mission.empty() ||
        ss2::assets::equalsIgnoreCaseAscii(missionPath.filename().string(), "--- TUTORIAL I ---.SSM");
    auto data = ss2::assets::readMissionData(missionPath);
    const auto unitCatalog = ss2::assets::UnitCatalog::load(gameDir, data.scheme);
    auto map = isTutorial1
        ? ss2::assets::loadTutorialTerrain(gameDir)
        : ss2::assets::loadMissionTerrain(gameDir, missionPath);
    ss2::assets::SceneryDepth depth(map.width, map.height);
    std::vector<ss2::assets::BuildingPlacement> missionBuildings;
    std::vector<ss2::assets::BridgePlacement> missionBridges;
    if (isTutorial1) {
        (void)ss2::assets::drawTutorialScenery(map, gameDir, &depth, 0, &missionBuildings, &missionBridges);
    } else {
        (void)ss2::assets::drawMissionScenery(map, gameDir, missionPath, &depth, &missionBuildings, &missionBridges);
    }
    const auto placements = isTutorial1
        ? ss2::assets::readTutorialPlacements(gameDir)
        : ss2::assets::readMissionPlacements(missionPath);
    ss2::assets::VehicleScene vehicleScene(gameDir, placements);
    const auto& vehicleUnits = vehicleScene.vehicles();
    std::vector<ss2::assets::Placement> vehicles;
    vehicles.reserve(vehicleUnits.size());
    for (const auto& v : vehicleUnits) {
        vehicles.push_back({v.id, v.x, v.y, int(v.hullDir), v.hp, v.ammo, v.group, v.owner});
    }
    ss2::assets::InfantryScene scene(std::move(map), gameDir, placements, originalInfantry, {}, {}, false,
                                     std::move(depth), nullptr, data.width, data.height);
    std::vector<std::pair<std::size_t, std::size_t>> preloadedRiders;
    {
        std::size_t pCursor = 0;
        for (std::size_t vIdx = 0; vIdx < vehicleUnits.size(); ++vIdx) {
            const auto& vu = vehicleUnits[vIdx];
            while (pCursor < placements.size() &&
                   (placements[pCursor].id != vu.id || placements[pCursor].x != vu.x ||
                    placements[pCursor].y != vu.y || placements[pCursor].owner != vu.owner)) {
                ++pCursor;
            }
            if (pCursor >= placements.size()) break;
            for (const auto& pax : placements[pCursor].passengers) {
                if (pax.isCrew) continue;
                ss2::assets::Placement rp{pax.id, vu.x, vu.y, 0, pax.hp, pax.ammo, pax.group, vu.owner, {}, pax.expa};
                if (auto spawned = scene.spawn(rp)) {
                    preloadedRiders.emplace_back(spawned->first, vIdx);
                }
            }
            ++pCursor;
        }
    }
    ss2::FogGrid fog(data.width, data.height, data.flags);
    fog.applyMapPerimeterFlags();
    (void)fog.loadCirc64FromGame(gameDir);
    auto navigation = ss2::InfantryPathGrid::fromSubCellMasks(data.width, data.height,
                                                              std::move(data.flags), std::move(data.obstacles));
    const auto missionPlayerTeams = data.playerTeams;
    const auto initMines = data.mines;
    ss2::TutorialUnits soldiers(std::move(navigation), scene.units(), vehicles, std::move(fog),
                                std::clamp(difficulty, 1, 3), autoEngage, vehicleUnits, unitCatalog);
    soldiers.setPlayerTeams(missionPlayerTeams);
    soldiers.setMines(initMines);
    soldiers.setBuildings(std::move(missionBuildings));
    soldiers.setBridges(std::move(missionBridges));
    soldiers.setAirSupport(data.airSupport, data.paratrooperGroup, data.paratrooperRoster);
    for (const auto& [sIdx, vIdx] : preloadedRiders) {
        (void)soldiers.embarkImmediate(sIdx, vIdx);
    }
    std::map<unsigned, std::vector<assets::ReinforcementUnit>> initialSavedPools;
    for (const auto& [poolIdx, wave] : replayFile.initialSavedPools) {
        if (!wave.empty()) initialSavedPools.emplace(poolIdx, wave);
    }
    ss2::MissionRunner simulation(ss2::MissionRunner::ManualClock{}, std::move(data), std::move(soldiers),
                                  initialSavedPools);
    if (!simulation.replay(replayFile.commands, replayFile.endTick)) {
        throw std::runtime_error("Replay log failed before reaching end_tick " +
                                 std::to_string(replayFile.endTick));
    }
    const auto actualHash = simulation.stateHash();
    if (replayFile.endStateHash && actualHash != *replayFile.endStateHash) {
        std::ostringstream oss;
        oss << "Replay stateHash mismatch at tick " << simulation.simTick()
            << ": expected 0x" << std::hex << *replayFile.endStateHash
            << ", got 0x" << actualHash;
        throw std::runtime_error(oss.str());
    }
    std::cout << "Replay verified: " << replayFile.commands.size()
              << " commands, tick " << simulation.simTick()
              << ", stateHash 0x" << std::hex << actualHash << std::dec << '\n';
    return 0;
}

void GameSession::persistNow() {
    if (persistSettings) {
        (void)ss2::saveUserSettings(ss2::defaultSettingsPath(), settings);
    }
}

float GameSession::fogBrightnessAt(int mx, int my) const {
    if (replayAllVision()) return 1.0f;
    if (latestFog.size() != std::size_t(fogGridW) * std::size_t(fogGridH)) return 1.0f;
    const int originX = int(fogGridH) * 32;
    const int halfX = (mx - originX) >> 1;
    const int tx = std::clamp((my + halfX) >> 5, 0, int(fogGridW) - 1);
    const int ty = std::clamp((my - halfX) >> 5, 0, int(fogGridH) - 1);
    const std::uint8_t f = latestFog[std::size_t(ty) * std::size_t(fogGridW) + std::size_t(tx)];
    const int viewer = localOwner();
    if (f & static_cast<std::uint8_t>(1u << viewer)) return 1.0f;
    return (f & static_cast<std::uint8_t>(0x10u << viewer)) ? 0.70f : 0.44f;
}

void GameSession::applyMapGraphics() {
    renderer.setMapFilterLinear(smoothTerrain);
    renderer.setSpriteSharpness(enhancedGraphics ? 1.0f : 0.0f);
    if (!enhancedGraphics) {
        renderer.setMapGrade({});
        return;
    }
    const float tSec = std::fmod(
        std::chrono::duration<float>(std::chrono::steady_clock::now() - animStart).count(), 3600.0f);
    const float shimmer = (mapScheme == 3u) ? 0.45f : 0.85f;
    MapGrade g{};
    switch (mapScheme) {
    case 0u: g = {true, 0.42f, 0.14f, 1.02f, 1.01f, 0.97f, 1.05f, tSec, shimmer}; break; // Summer
    case 1u: g = {true, 0.40f, 0.15f, 0.96f, 0.99f, 1.05f, 1.06f, tSec, shimmer}; break; // Winter
    case 2u: g = {true, 0.42f, 0.14f, 1.01f, 1.02f, 0.98f, 1.05f, tSec, shimmer}; break; // Beach / Pacific
    case 3u: g = {true, 0.38f, 0.15f, 1.04f, 1.00f, 0.93f, 1.05f, tSec, shimmer}; break; // Desert
    default: g = {true, 0.40f, 0.14f, 1.00f, 1.00f, 1.00f, 1.04f, tSec, shimmer}; break;
    }
    if (missionWeather == assets::WeatherKind::Fog) {
        g.contrast = 1.01f;
        g.vignette = 0.17f;
        g.bGain = std::min(1.10f, g.bGain + 0.03f);
    } else if (missionWeather == assets::WeatherKind::Rain) {
        g.contrast = 1.06f;
        g.vignette = 0.16f;
        g.rGain *= 0.97f;
        g.bGain = std::min(1.10f, g.bGain * 1.03f);
    } else if (missionWeather == assets::WeatherKind::Snow) {
        g.contrast = 1.04f;
        g.vignette = 0.16f;
        g.rGain *= 0.96f;
        g.bGain = std::min(1.10f, g.bGain * 1.04f);
    }
    renderer.setMapGrade(g);
}

bool GameSession::ensureGpuInfantrySlot(const std::string& id) {
    if (gpuAtlasSlots.contains(id)) return true;
    if (unitCatalog) {
        if (const auto* sp = unitCatalog->find(id); sp && sp->isVehicle) return false;
    }
    const auto logFallback = [&](const char* reason) {
        if (gpuFallbackIds.insert(id).second)
            std::cout << "GPU infantry fallback CPU: " << id << " (" << reason << ")\n";
    };
    const unsigned reserved = (hud && hud->airVfxAtlasSlot().has_value() ? 0u : 1u) +
                              (combatVfx.atlasSlot().has_value() ? 0u : 1u);
    const unsigned maxInfSlot = planeAtlasBase - reserved;
    if (gpuNextSlot >= maxInfSlot) {
        logFallback("slot budget full");
        return false;
    }
    if (!gpuArtDir.empty() && !originalInfantry) {
        const auto path = gpuArtDir / (id + ".png");
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            auto atlas = ss2::assets::loadPngRgba(path);
            if (atlas.width == 1024 && atlas.height == 640) {
                renderer.setSpriteAtlas(gpuNextSlot, atlas);
                gpuAtlasSlots.emplace(id, gpuNextSlot++);
                if (gpuPreloadLogged)
                    std::cout << "GPU 4x HD infantry lazy slot: " << id << " (slot " << (gpuNextSlot - 1) << ")\n";
                return true;
            }
        }
    }
    if (scene) {
        if (auto epxSheet = scene->buildGpuSheet2xEpx(id);
            epxSheet && epxSheet->width > 0 && epxSheet->height > 0) {
            renderer.setSpriteAtlas(gpuNextSlot, *epxSheet);
            gpuAtlasSlots.emplace(id, gpuNextSlot++);
            gpuInfantry2xIds.insert(id);
            if (gpuPreloadLogged)
                std::cout << "GPU 2x EPX infantry lazy slot: " << id << " (slot " << (gpuNextSlot - 1) << ")\n";
            return true;
        }
    }
    logFallback("no 4x or 2x sheet");
    return false;
}

bool GameSession::ensureGpuVehicleSlot(const std::string& id) {
    if (gpuVehicleIds.contains(id)) return true;
    const auto* sp = unitCatalog ? unitCatalog->find(id) : nullptr;
    if ((sp && sp->isSupplyCrate()) || id == "ammo.box" || id == "spare.parts") return false;
    const unsigned reserved = (hud && hud->airVfxAtlasSlot().has_value() ? 0u : 1u) +
                              (combatVfx.atlasSlot().has_value() ? 0u : 1u);
    const unsigned maxWorldSlot = planeAtlasBase - reserved;
    if (gpuNextSlot >= maxWorldSlot) {
        if (gpuFallbackIds.insert("veh:" + id).second) {
            std::cout << "GPU vehicle slot cap reached (" << maxWorldSlot << "): " << id << " using fallback\n";
        }
        return false;
    }
    if (!gpuVehicleArtDir.empty()) {
        const auto path = gpuVehicleArtDir / (id + ".png");
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            auto atlas = ss2::assets::loadPngRgba(path);
            if (atlas.width == 2048 && atlas.height == 768) {
                bool hasTurret = false;
                for (unsigned y = 256u; y < 512u && !hasTurret; y += 2u) {
                    for (unsigned x = 0u; x < 2048u; x += 2u) {
                        if (atlas.rgba[(std::size_t(y) * 2048u + x) * 4u + 3u] > 20u) {
                            hasTurret = true;
                            break;
                        }
                    }
                }
                renderer.setSpriteAtlas(gpuNextSlot, atlas);
                gpuHdVehicles.emplace(id, GpuHdVehicleMeta{gpuNextSlot++, hasTurret});
                gpuVehicleIds.insert(id);
                if (gpuPreloadLogged)
                    std::cout << "GPU 4x HD vehicle lazy slot: " << id << " (slot " << (gpuNextSlot - 1) << ")\n";
                return true;
            }
        }
    }
    if (!vehicleScene) return false;
    if (auto pckAtlas = vehicleScene->buildGpuAtlas(id, true)) {
        renderer.setSpriteAtlas(gpuNextSlot, pckAtlas->atlas);
        gpuPckVehicleSlots.emplace(id, gpuNextSlot++);
        gpuPckVehicleAtlases.emplace(id, std::move(*pckAtlas));
        gpuVehicleIds.insert(id);
        if (gpuPreloadLogged)
            std::cout << "GPU 2x EPX vehicle lazy slot: " << id << " (slot " << (gpuNextSlot - 1) << ")\n";
        return true;
    }
    return false;
}

void GameSession::syncGpuSprites() {
    if (!scene) return;
    const bool hasAirTrails = hud && hud->hasActiveAirTrails() && hud->airVfxAtlasSlot().has_value();
    bool hasLiveCrates = false;
    if (vehicleScene && combatVfx.atlasSlot().has_value()) {
        for (const auto& v : vehicleScene->vehicles()) {
            if (!v.alive || v.hp <= 0 || !v.visible) continue;
            const auto* sp = unitCatalog ? unitCatalog->find(v.id) : nullptr;
            if ((sp && sp->isSupplyCrate()) || v.id == "ammo.box" || v.id == "spare.parts") {
                hasLiveCrates = true;
                break;
            }
        }
    }
    const bool hasLiveParas = !liveParachutes.empty() && combatVfx.atlasSlot().has_value();
    const bool hasCombatVfx = combatVfx.atlasSlot().has_value() &&
                              ((combatEffects && combatVfx.hasActiveSprites()) || hasLiveCrates || hasLiveParas);
    if (gpuAtlasSlots.empty() && gpuVehicleIds.empty() && worldGpuSprites.empty() && livePlanes.empty() && !hasAirTrails && !hasCombatVfx) {
        if (planesClearedNeeded) {
            renderer.setSprites({});
            planesClearedNeeded = false;
        }
        return;
    }
    planesClearedNeeded = !livePlanes.empty() || hasAirTrails || hasCombatVfx;
    const float fMapScale = static_cast<float>(std::max(1, mapScale));
    bool cullViewport = false;
    float cullMinX = 0.0f, cullMaxX = 0.0f, cullMinY = 0.0f, cullMaxY = 0.0f;
    {
        int winW = 0, winH = 0;
        if (window && SDL_GetWindowSize(window, &winW, &winH) && winW > 0 && winH > 0) {
            const auto [vx0, vy0] = renderer.mapPosition(0.0f, 0.0f);
            const auto [vx1, vy1] = renderer.mapPosition(static_cast<float>(winW), static_cast<float>(winH));
            const double spanX = std::abs(vx1 - vx0);
            const double spanY = std::abs(vy1 - vy0);
            if (std::isfinite(vx0) && std::isfinite(vy0) && std::isfinite(vx1) && std::isfinite(vy1) &&
                spanX > 16.0 && spanY > 16.0) {
                const float pad = 160.0f * fMapScale;
                cullMinX = static_cast<float>(std::min(vx0, vx1)) - pad;
                cullMaxX = static_cast<float>(std::max(vx0, vx1)) + pad;
                cullMinY = static_cast<float>(std::min(vy0, vy1)) - pad;
                cullMaxY = static_cast<float>(std::max(vy0, vy1)) + pad;
                cullViewport = true;
            }
        }
    }
    const auto inFrustumRect = [&](float x, float y, float w, float h) noexcept {
        if (!cullViewport) return true;
        return (x + w) >= cullMinX && x <= cullMaxX && (y + h) >= cullMinY && y <= cullMaxY;
    };
    const auto now = std::chrono::steady_clock::now();
    const float lightRadiusSq = (140.0f * fMapScale) * (140.0f * fMapScale);
    const bool hasDynamicLights = enhancedGraphics && combatEffects && (!activeVfx.empty() || !burningWrecks.empty());
    const auto explosionLightAt = [&](float wx, float wy, float maxCap, bool includeTracer, bool includeWrecks) {
        if (!hasDynamicLights) return 0.0f;
        float boost = 0.0f;
        const auto addPointLight = [&](float lx, float ly, float peak) {
            const float dx = wx - lx;
            const float dy = wy - ly;
            const float distSq = dx * dx + dy * dy;
            if (distSq < lightRadiusSq) {
                const float falloff = 1.0f - std::sqrt(distSq / lightRadiusSq);
                boost = std::max(boost, peak * falloff);
            }
        };
        for (const auto& vfx : activeVfx) {
            if (boost >= maxCap) break;
            const float p = std::clamp(
                std::chrono::duration<float>(now - vfx.start).count() /
                    std::max(0.01f, static_cast<float>(vfx.duration.count()) * 0.001f),
                0.0f, 1.0f);
            if (vfx.kind == ActiveVfx::Kind::Explosion || vfx.kind == ActiveVfx::Kind::VehicleExplosion ||
                vfx.kind == ActiveVfx::Kind::Flamer) {
                const float peak = (vfx.kind == ActiveVfx::Kind::Flamer) ? std::min(maxCap, 0.28f) : maxCap;
                addPointLight(vfx.x0, vfx.y0, peak);
            } else if (vfx.kind == ActiveVfx::Kind::CannonShell) {
                if (p < 0.35f) {
                    const float muzzleFade = 1.0f - p / 0.35f;
                    addPointLight(vfx.x0, vfx.y0, std::min(maxCap, 0.34f) * muzzleFade);
                }
            } else if (vfx.kind == ActiveVfx::Kind::Tracer) {
                if (includeTracer && p < 0.35f) {
                    const float muzzleFade = 1.0f - p / 0.35f;
                    addPointLight(vfx.x0, vfx.y0, std::min(maxCap, 0.24f) * muzzleFade);
                }
            } else if (vfx.kind == ActiveVfx::Kind::Rocket) {
                if (p < 0.35f) {
                    const float muzzleFade = 1.0f - p / 0.35f;
                    addPointLight(vfx.x0, vfx.y0, std::min(maxCap, 0.34f) * muzzleFade);
                }
                const float arcY = -20.0f * fMapScale * std::sin(p * 3.14159f);
                const float rx = vfx.x0 + (vfx.x1 - vfx.x0) * p;
                const float ry = vfx.y0 + (vfx.y1 - vfx.y0) * p + arcY;
                addPointLight(rx, ry, std::min(maxCap, 0.30f));
            }
        }
        if (includeWrecks) {
            for (const auto& bw : burningWrecks) {
                if (boost >= 0.32f) break;
                const float dx = wx - bw.x;
                const float dy = wy - bw.y;
                const float distSq = dx * dx + dy * dy;
                if (distSq < lightRadiusSq) {
                    const float falloff = 1.0f - std::sqrt(distSq / lightRadiusSq);
                    boost = std::max(boost, 0.26f * falloff);
                }
            }
        }
        return boost;
    };

    struct DrawItem {
        ss2::Sprite sprite;
        int sortY;
    };
    std::vector<DrawItem> items;
    items.reserve(scene->units().size() + worldGpuSprites.size());
    for (const auto& ws : worldGpuSprites) {
        if (!inFrustumRect(ws.sprite.x, ws.sprite.y, ws.sprite.width, ws.sprite.height)) continue;
        auto sp = ws.sprite;
        const float cx = sp.x + sp.width * 0.5f;
        const float cy = sp.y + sp.height * 0.5f;
        const float lightBoost = explosionLightAt(cx, cy, 0.36f, true, true);
        sp.brightness = std::min(1.42f, fogBrightnessAt(ws.mapX1x, ws.mapY1x) + lightBoost);
        items.push_back({sp, ws.sortY});
    }
    const float dstSize = 32.0f * static_cast<float>(mapScale);
    constexpr float footRatioY = 116.0f / 128.0f;
    for (std::size_t ci = 0; ci < fallenSoldiers.size(); ++ci) {
        const auto& c = fallenSoldiers[ci];
        if (!gpuAtlasSlots.contains(c.id)) continue;
        const float cellPx = gpuInfantry2xIds.contains(c.id) ? 64.0f : 128.0f;
        const unsigned col = c.direction & 7u;
        constexpr unsigned row = 4u;
        const float cw = dstSize * 0.96f;
        const float ch = dstSize * 0.36f;
        const float cx = static_cast<float>(c.x * mapScale) - 0.5f * cw;
        const float cy = static_cast<float>(c.y * mapScale) - 0.65f * ch;
        if (!inFrustumRect(cx, cy, cw, ch)) continue;
        const std::size_t distFromNewest = fallenSoldiers.size() - ci;
        const float fade = (distFromNewest > 224u)
            ? std::clamp(static_cast<float>(257u - std::min<std::size_t>(256u, distFromNewest)) / 33.0f, 0.04f, 1.0f)
            : 1.0f;
        const float baseBright = fogBrightnessAt(c.x, c.y);
        const unsigned slot = gpuAtlasSlots.at(c.id);
        const float u0 = static_cast<float>(col) * cellPx;
        const float v0 = static_cast<float>(row) * cellPx;
        const float u1 = static_cast<float>(col + 1u) * cellPx;
        const float v1 = static_cast<float>(row + 1u) * cellPx;
        const float shW = cw * 1.06f;
        const float shH = ch * 0.55f;
        items.push_back({
            {
                cx + 2.0f * fMapScale,
                cy + 0.48f * ch + 1.0f * fMapScale,
                shW,
                shH,
                u0, v0, u1, v1,
                slot,
                -101.0f,
                0.28f * baseBright * fade
            },
            -10001 + c.y
        });
        items.push_back({
            {
                cx,
                cy,
                cw,
                ch,
                u0, v0, u1, v1,
                slot,
                baseBright * 0.78f,
                0.92f * fade
            },
            -10000 + c.y
        });
    }
    constexpr float recoilDx[8] = {-1.8f, 0.0f, 1.8f, 2.4f, 1.8f, 0.0f, -1.8f, -2.4f};
    constexpr float recoilDy[8] = {-1.2f, -1.8f, -1.2f, 0.0f, 1.2f, 1.8f, 1.2f, 0.0f};
    const float elapsed = std::chrono::duration<float>(now - animStart).count();
    const auto& units = scene->units();
    for (size_t idx = 0; idx < units.size(); ++idx) {
        const auto& u = units[idx];
        if (u.hp <= 0 || !u.visible || !gpuAtlasSlots.contains(u.id)) continue;
        const float ux = static_cast<float>(u.x * mapScale);
        const float uy = static_cast<float>(u.y * mapScale);
        if (!inFrustumRect(ux - dstSize, uy - dstSize, dstSize * 2.0f, dstSize * 2.0f)) continue;
        const float cellPx = gpuInfantry2xIds.contains(u.id) ? 64.0f : 128.0f;
        const unsigned col = u.direction & 7u;
        const unsigned row = u.walking ? (1u + ((4u * u.progress) >> 8u)) : 0u;
        const bool crouched = crouchedUnits.contains(idx);
        float w = dstSize * (crouched ? 1.04f : 1.0f);
        float h = dstSize * (crouched ? 0.78f : 1.0f);
        float ox = 0.0f, oy = 0.0f;
        if (!u.walking) {
            const float breath = std::sin(elapsed * 2.4f + static_cast<float>(idx) * 0.85f);
            h *= (1.0f + 0.018f * breath);
            ox += 0.45f * std::sin(elapsed * 1.05f + static_cast<float>(idx) * 1.37f);
        }
        bool firingNow = false;
        if (const auto it = recoilUntil.find(idx); it != recoilUntil.end() && now < it->second) {
            ox += recoilDx[col];
            oy += recoilDy[col];
            firingNow = true;
        }
        const float animScale = static_cast<float>(mapScale) * 0.5f;
        ox *= animScale;
        oy *= animScale;
        const float u0 = static_cast<float>(col) * cellPx;
        const float v0 = static_cast<float>(row) * cellPx;
        const float u1 = static_cast<float>(col + 1u) * cellPx;
        const float v1 = static_cast<float>(row + 1u) * cellPx;
        const unsigned slot = gpuAtlasSlots.at(u.id);
        const float baseBright = fogBrightnessAt(u.x, u.y);
        if (enhancedGraphics) {
            const float shW = w * 1.05f;
            const float shH = h * 0.42f;
            items.push_back({
                {
                    ux - 0.5f * shW + 4.5f * fMapScale + ox,
                    uy - footRatioY * shH + 1.5f * fMapScale + oy,
                    shW, shH, u0, v0, u1, v1, slot, -101.0f, 0.30f * baseBright
                },
                u.y - 2
            });
        }
        float lightBoost = (enhancedGraphics && combatEffects && firingNow) ? 0.26f : 0.0f;
        lightBoost = std::max(lightBoost, explosionLightAt(ux, uy, 0.42f, true, true));
        items.push_back({
            {
                ux - 0.5f * w + ox,
                uy - footRatioY * h + oy,
                w, h, u0, v0, u1, v1, slot, std::min(1.45f, baseBright + lightBoost), 1.0f
            },
            u.y
        });
    }
    if (vehicleScene && !gpuVehicleIds.empty()) {
        constexpr float vRecoilDx[8] = {-2.2f, 0.0f, 2.2f, 3.0f, 2.2f, 0.0f, -2.2f, -3.0f};
        constexpr float vRecoilDy[8] = {-1.4f, -2.2f, -1.4f, 0.0f, 1.4f, 2.2f, 1.4f, 0.0f};
        const auto& vehs = vehicleScene->vehicles();
        std::vector<ss2::assets::GpuVehicleSprite> pckSprites;
        for (std::size_t vi = 0; vi < vehs.size(); ++vi) {
            const auto& v = vehs[vi];
            if (!v.visible) continue;
            const bool hasHd = gpuHdVehicles.contains(v.id);
            const bool hasPck = gpuPckVehicleSlots.contains(v.id);
            if (!hasHd && !hasPck) continue;
            const bool alive = v.alive && v.hp > 0;
            const float vx = static_cast<float>(v.x) * fMapScale;
            const float vy = static_cast<float>(v.y) * fMapScale;
            const float maxExtent = 96.0f * fMapScale;
            if (!inFrustumRect(vx - maxExtent, vy - maxExtent, maxExtent * 2.0f, maxExtent * 2.0f)) continue;
            const float baseBright = fogBrightnessAt(v.x, v.y);
            const float lightBoost = alive ? explosionLightAt(vx, vy, 0.36f, true, true) : 0.0f;
            const float litBright = std::min(1.42f, baseBright + lightBoost);
            float rDx = 0.0f, rDy = 0.0f;
            if (alive) {
                if (const auto rit = vehRecoilUntil.find(vi); rit != vehRecoilUntil.end() && now < rit->second) {
                    const unsigned tcol = ((v.turretDir + 2u) >> 2u) & 7u;
                    rDx = vRecoilDx[tcol] * fMapScale;
                    rDy = vRecoilDy[tcol] * fMapScale;
                }
            }
            if (hasHd) {
                const auto& meta = gpuHdVehicles.at(v.id);
                const unsigned hcol = ((v.hullDir + 2u) >> 2u) & 7u;
                const unsigned tcol = ((v.turretDir + 2u) >> 2u) & 7u;
                const float maxRetail = static_cast<float>(std::max(v.width, v.height));
                const float cellOut = (maxRetail > 4.0f ? std::clamp(maxRetail * 1.25f, 38.0f, 88.0f) : 68.0f) * fMapScale;
                constexpr float hdCell = 256.0f;
                if (enhancedGraphics) {
                    const float shW = cellOut * 1.04f;
                    const float shH = cellOut * 0.50f;
                    items.push_back({
                        {
                            vx - 0.5f * shW + 5.0f * fMapScale,
                            vy - 0.5f * shH + 2.5f * fMapScale,
                            shW, shH,
                            static_cast<float>(hcol) * hdCell, alive ? 0.0f : 512.0f,
                            static_cast<float>(hcol + 1u) * hdCell, alive ? 256.0f : 768.0f,
                            meta.atlasSlot, -101.0f, 0.34f * baseBright
                        },
                        alive ? (v.y - 3) : (-9502 + v.y)
                    });
                }
                if (alive) {
                    const float hullOffX = meta.hasTurret ? 0.0f : rDx;
                    const float hullOffY = meta.hasTurret ? 0.0f : rDy;
                    items.push_back({
                        {
                            vx - 0.5f * cellOut + hullOffX,
                            vy - 0.5f * cellOut + hullOffY,
                            cellOut, cellOut,
                            static_cast<float>(hcol) * hdCell, 0.0f,
                            static_cast<float>(hcol + 1u) * hdCell, 256.0f,
                            meta.atlasSlot, litBright, 1.0f
                        },
                        v.y
                    });
                    if (meta.hasTurret) {
                        items.push_back({
                            {
                                vx - 0.5f * cellOut + rDx,
                                vy - 0.5f * cellOut + rDy,
                                cellOut, cellOut,
                                static_cast<float>(tcol) * hdCell, 256.0f,
                                static_cast<float>(tcol + 1u) * hdCell, 512.0f,
                                meta.atlasSlot, litBright, 1.0f
                            },
                            v.y + 1
                        });
                    }
                } else {
                    items.push_back({
                        {
                            vx - 0.5f * cellOut,
                            vy - 0.5f * cellOut,
                            cellOut, cellOut,
                            static_cast<float>(hcol) * hdCell, 512.0f,
                            static_cast<float>(hcol + 1u) * hdCell, 768.0f,
                            meta.atlasSlot, baseBright * 0.78f, 0.96f
                        },
                        -9500 + v.y
                    });
                }
            } else {
                const unsigned slot = gpuPckVehicleSlots.at(v.id);
                const auto& atlas = gpuPckVehicleAtlases.at(v.id);
                pckSprites.clear();
                vehicleScene->appendGpuSprites(vi, atlas, mapScale, rDx, rDy, pckSprites);
                for (const auto& gs : pckSprites) {
                    if (gs.isShadow && !enhancedGraphics) continue;
                    items.push_back({
                        {
                            gs.x, gs.y, gs.width, gs.height,
                            gs.u0, gs.v0, gs.u1, gs.v1,
                            slot,
                            gs.isShadow ? 1.0f : (alive ? litBright : (baseBright * 0.94f)),
                            gs.isShadow ? (0.36f * baseBright) : 1.0f
                        },
                        v.y + gs.sortYOffset
                    });
                }
            }
        }
    }
    for (const auto& [sid, lp] : livePlanes) {
        (void)sid;
        const float altScale = lp.shadowScale;
        const float px = static_cast<float>(lp.pose.x * mapScale);
        const float py = static_cast<float>(lp.pose.y * mapScale);
        float sprX = px - lp.sprW * 0.5f;
        float sprY = py - lp.sprH * 0.5f;
        float sprW = lp.sprW;
        float sprH = lp.sprH;
        float u0 = 0.0f, v0 = 0.0f, u1 = lp.texW, v1 = lp.texH;
        if (const auto ait = gpuAircraftAtlases.find(lp.spriteKey); ait != gpuAircraftAtlases.end()) {
            const auto& atlas = ait->second;
            unsigned d = lp.pose.dir32 & 31u;
            if (atlas.frameW[d] <= 0 || atlas.frameH[d] <= 0) {
                for (unsigned off = 1u; off < 32u; ++off) {
                    const unsigned cand = (d + off) & 31u;
                    if (atlas.frameW[cand] > 0 && atlas.frameH[cand] > 0) {
                        d = cand;
                        break;
                    }
                }
            }
            if (atlas.frameW[d] > 0 && atlas.frameH[d] > 0) {
                const float gs = static_cast<float>(std::max(1, atlas.scale));
                sprW = (static_cast<float>(atlas.frameW[d]) / gs) * fMapScale;
                sprH = (static_cast<float>(atlas.frameH[d]) / gs) * fMapScale;
                sprX = px + static_cast<float>(atlas.offsetX[d]) * fMapScale;
                sprY = py + static_cast<float>(atlas.offsetY[d]) * fMapScale;
                u0 = static_cast<float>(atlas.cellX[d]);
                v0 = static_cast<float>(atlas.cellY[d]);
                u1 = u0 + static_cast<float>(atlas.frameW[d]);
                v1 = v0 + static_cast<float>(atlas.frameH[d]);
            }
        }
        const int shadowSimX = lp.pose.x + static_cast<int>(std::lround(44.0f * altScale));
        const int shadowSimY = lp.pose.y + static_cast<int>(std::lround(66.0f * altScale));
        const float shadowFog = fogBrightnessAt(shadowSimX, shadowSimY);
        if (shadowFog >= 0.55f) {
            const float shScale = 0.88f / std::sqrt(std::max(0.25f, altScale));
            const float shW = sprW * shScale;
            const float shH = sprH * shScale;
            const float shCx = sprX + sprW * 0.5f + 44.0f * altScale * fMapScale;
            const float shCy = sprY + sprH * 0.5f + 66.0f * altScale * fMapScale;
            const int shSortY = 180000 + lp.pose.y;
            if (enhancedGraphics) {
                const float penW = shW * 1.12f;
                const float penH = shH * 1.12f;
                if (inFrustumRect(shCx - penW * 0.5f, shCy - penH * 0.5f, penW, penH)) {
                    items.push_back({
                        {
                            shCx - penW * 0.5f, shCy - penH * 0.5f,
                            penW, penH, u0, v0, u1, v1,
                            lp.atlasSlot, -101.0f, 0.18f * shadowFog
                        },
                        shSortY
                    });
                }
                if (inFrustumRect(shCx - shW * 0.5f, shCy - shH * 0.5f, shW, shH)) {
                    items.push_back({
                        {
                            shCx - shW * 0.5f, shCy - shH * 0.5f,
                            shW, shH, u0, v0, u1, v1,
                            lp.atlasSlot, -101.0f, 0.26f * shadowFog
                        },
                        shSortY
                    });
                }
            } else if (inFrustumRect(shCx - shW * 0.5f, shCy - shH * 0.5f, shW, shH)) {
                items.push_back({
                    {
                        shCx - shW * 0.5f, shCy - shH * 0.5f,
                        shW, shH, u0, v0, u1, v1,
                        lp.atlasSlot, -101.0f, 0.38f * shadowFog
                    },
                    shSortY
                });
            }
        }
        if (inFrustumRect(sprX, sprY, sprW, sprH)) {
            items.push_back({
                {
                    sprX, sprY, sprW, sprH,
                    u0, v0, u1, v1,
                    lp.atlasSlot,
                    1.0f,
                    1.0f
                },
                1000000
            });
        }
    }
    if (hasAirTrails) {
        std::vector<ss2::Sprite> trailSprites;
        hud->appendAirTrailSprites(mapScale, trailSprites);
        for (const auto& ts : trailSprites) {
            if (inFrustumRect(ts.x, ts.y, ts.width, ts.height)) {
                items.push_back({ts, 999999});
            }
        }
    }
    if (hasCombatVfx) {
        std::vector<CombatVfxSystem::RankedSprite> vfxItems;
        if (hasLiveCrates && vehicleScene) {
            for (const auto& v : vehicleScene->vehicles()) {
                if (!v.alive || v.hp <= 0 || !v.visible) continue;
                const auto* sp = unitCatalog ? unitCatalog->find(v.id) : nullptr;
                const bool isCrate = (sp && sp->isSupplyCrate()) || v.id == "ammo.box" || v.id == "spare.parts";
                if (!isCrate) continue;
                const float vx = static_cast<float>(v.x) * fMapScale;
                const float vy = static_cast<float>(v.y) * fMapScale;
                if (!inFrustumRect(vx - 32.0f * fMapScale, vy - 32.0f * fMapScale,
                                   64.0f * fMapScale, 64.0f * fMapScale)) continue;
                const float bright = fogBrightnessAt(v.x, v.y);
                if (bright < 0.55f) continue;
                const bool parts = (sp ? (sp->ammo1 <= 0 && sp->ammo2 > 0) : (v.id == "spare.parts"));
                combatVfx.appendCrateSprite(static_cast<float>(v.x), static_cast<float>(v.y),
                                            (v.hullDir >> 2u) & 7u, parts, mapScale, bright, 1.0f, vfxItems);
            }
        }
        if (hasLiveParas) {
            for (const auto& [pid, pp] : liveParachutes) {
                const float bright = fogBrightnessAt(pp.x, pp.y);
                if (bright < 0.55f) continue;
                const float alt = static_cast<float>(std::max(0, pp.altitudePx));
                const float ppx = static_cast<float>(pp.x) * fMapScale;
                const float ppy = static_cast<float>(pp.y) * fMapScale;
                if (!inFrustumRect(ppx - 48.0f * fMapScale, ppy - (alt + 64.0f) * fMapScale,
                                   96.0f * fMapScale, (alt + 128.0f) * fMapScale)) continue;
                float phase = 1.0f;
                if (pp.altitudePx > 0) {
                    phase = std::clamp(1.0f - alt / 196.0f, 0.05f, 1.0f);
                    if (pp.isSupplyCrate) {
                        const bool parts = (pp.unitId == "spare.parts");
                        combatVfx.appendCrateSprite(static_cast<float>(pp.x), static_cast<float>(pp.y) - alt,
                                                    static_cast<unsigned>(pid & 7u), parts,
                                                    mapScale, bright, 1.0f, vfxItems);
                    }
                } else {
                    phase = std::clamp(static_cast<float>(pp.collapseTick) / 12.0f, 0.0f, 0.9999f);
                }
                combatVfx.appendParachuteSprites(static_cast<float>(pp.x), static_cast<float>(pp.y),
                                                 alt, phase, mapScale, bright, vfxItems);
            }
        }
        if (combatEffects && combatVfx.hasActiveSprites()) {
            combatVfx.appendSprites(mapScale,
                                    [this](int sx, int sy) { return fogBrightnessAt(sx, sy); },
                                    vfxItems);
        }
        for (const auto& vi : vfxItems) {
            if (inFrustumRect(vi.sprite.x, vi.sprite.y, vi.sprite.width, vi.sprite.height)) {
                items.push_back({vi.sprite, vi.sortY});
            }
        }
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const DrawItem& a, const DrawItem& b) {
                         return a.sortY != b.sortY ? a.sortY < b.sortY : a.sprite.atlas < b.sprite.atlas;
                     });
    std::vector<ss2::Sprite> sprites;
    sprites.reserve(items.size());
    for (const auto& it : items) sprites.push_back(it.sprite);
    renderer.setSprites(std::move(sprites));
}

bool GameSession::isAllyOwner(int owner) const {
    const int me = localOwner();
    if (owner == me) return true;
    if (owner < 0 || owner >= 4) return false;
    if (!multiplayer.enabled && (owner == 3 || missionPlayerTeams[static_cast<std::size_t>(owner)] == 3)) {
        return false;
    }
    return missionPlayerTeams[static_cast<std::size_t>(owner)] ==
           missionPlayerTeams[static_cast<std::size_t>(me)];
}

bool GameSession::isHostileOwner(int owner) const {
    return (multiplayer.enabled || owner != 3) && !isAllyOwner(owner);
}

const char* GameSession::veterancyRank(int expa) {
    if (expa >= 24576) return "Elite";
    if (expa >= 12288) return "Veteran";
    if (expa >= 4096) return "Regular";
    return "Recruit";
}

int GameSession::initialGrenadesForId(std::string_view id) const {
    std::string low(id);
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (low.find("medic") != std::string::npos || low.find("officer") != std::string::npos ||
        low.find("general") != std::string::npos || low.find("mortar") != std::string::npos ||
        low.find("flam") != std::string::npos || low.find("baz") != std::string::npos ||
        low.find("faust") != std::string::npos || low.find("shreck") != std::string::npos ||
        low.find("piat") != std::string::npos || low.find("ptrs") != std::string::npos ||
        low.find("ptrd") != std::string::npos || low.find("at") == 0) {
        return 0;
    }
    return 2;
}

void GameSession::applyBuildingStage(std::size_t bIdx, int stage) {
    if (!scene || bIdx >= liveBuildings.size()) return;
    const int clamped = std::clamp(stage, 0, 3);
    auto& lb = liveBuildings[bIdx];
    if (lb.stampedStage == clamped) return;

    if (lb.basePatch.width == 0 || lb.basePatch.height == 0) {
        if (clamped == 3) {
            if (lb.ruinedPatch.width == 0 || lb.ruinedPatch.height == 0) return;
            if (lb.intactPatch.width == 0 || lb.intactPatch.height == 0) {
                lb.intactPatch = scene->copyGroundRect(
                    lb.ruinLeft, lb.ruinTop,
                    static_cast<int>(lb.ruinedPatch.width),
                    static_cast<int>(lb.ruinedPatch.height));
            }
            const auto patch = scene->stampGroundPatch(lb.ruinLeft, lb.ruinTop, lb.ruinedPatch);
            if (patch.pixels.width && patch.pixels.height) {
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
            lb.stampedStage = 3;
        } else if (lb.stampedStage == 3 && lb.intactPatch.width > 0 && lb.intactPatch.height > 0) {
            const auto patch = scene->stampGroundPatch(lb.ruinLeft, lb.ruinTop, lb.intactPatch, false);
            if (patch.pixels.width && patch.pixels.height) {
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
            lb.stampedStage = clamped;
        }
        return;
    }

    if (lb.stampedStage == 0 && clamped > 0 &&
        (lb.intactPatch.width == 0 || lb.intactPatch.height == 0)) {
        lb.intactPatch = scene->copyGroundRect(
            lb.ruinLeft, lb.ruinTop,
            static_cast<int>(lb.basePatch.width),
            static_cast<int>(lb.basePatch.height));
    }

    const auto rendered = ss2::assets::renderHouseStage(
        lb.x, lb.y, lb.ruinLeft, lb.ruinTop, lb.basePatch, lb.palette, lb.stageBlocks, clamped);
    const assets::MapImage* pixels = nullptr;
    if (clamped == 0 &&
        lb.intactPatch.width == lb.basePatch.width &&
        lb.intactPatch.height == lb.basePatch.height) {
        pixels = &lb.intactPatch;
    } else if (rendered.image.width > 0 && rendered.image.height > 0) {
        pixels = &rendered.image;
    } else if (clamped == 3 && lb.ruinedPatch.width > 0 && lb.ruinedPatch.height > 0) {
        pixels = &lb.ruinedPatch;
    } else {
        return;
    }

    const auto patch = scene->updateBuildingPatch(
        lb.ruinLeft, lb.ruinTop, *pixels, lb.y,
        std::span<const assets::SpriteCoverage>(rendered.bodyCoverages.data(), rendered.bodyCount),
        lb.hasLastShadow ? &lb.lastShadowCoverage : nullptr,
        rendered.hasShadow ? &rendered.shadowCoverage : nullptr);
    if (patch.pixels.width && patch.pixels.height) {
        renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
    }
    lb.stampedStage = clamped;
    lb.lastShadowCoverage = rendered.shadowCoverage;
    lb.hasLastShadow = rendered.hasShadow;
}

void GameSession::applyBridgeStage(std::size_t brIdx, int stage) {
    if (!scene || brIdx >= liveBridges.size()) return;
    const int clamped = std::clamp(stage, 0, 4);
    auto& lb = liveBridges[brIdx];
    if (lb.stampedStage == clamped ||
        lb.placement.basePatch.width == 0 || lb.placement.basePatch.height == 0) {
        return;
    }
    const assets::MapImage patch = assets::renderBridgeStage(lb.placement, clamped);
    if (patch.width == 0 || patch.height == 0) return;
    const auto upd = scene->stampGroundPatch(lb.placement.patchLeft, lb.placement.patchTop, patch, false);
    if (upd.pixels.width && upd.pixels.height) {
        renderer.updateMapRegion(upd.x, upd.y, upd.pixels);
    }
    lb.stampedStage = clamped;
}

bool GameSession::crushOrBlastTreesAt(int simX, int simY, int radiusPx, bool spawnDust) {
    if (!scene || destructibleTrees.empty() ||
        treeFallen.size() != destructibleTrees.size() || radiusPx <= 0) {
        return false;
    }
    const long long r2 = static_cast<long long>(radiusPx) * static_cast<long long>(radiusPx);
    bool anyFallen = false;
    for (std::size_t i = 0; i < destructibleTrees.size(); ++i) {
        if (treeFallen[i]) continue;
        const auto& tree = destructibleTrees[i];
        const long long dx = static_cast<long long>(tree.x) - simX;
        const long long dy = static_cast<long long>(tree.y) - simY;
        if (dx * dx + dy * dy > r2) continue;
        treeFallen[i] = true;
        anyFallen = true;
        if (tree.fallenPatch.width > 0 && tree.fallenPatch.height > 0) {
            const assets::SpriteCoverage* oldShadow =
                (tree.intactShadowCoverage.width > 0 && tree.intactShadowCoverage.height > 0)
                    ? &tree.intactShadowCoverage
                    : nullptr;
            const auto patch = scene->updateBuildingPatch(
                tree.patchLeft, tree.patchTop, tree.fallenPatch, tree.y,
                std::span<const assets::SpriteCoverage>{}, oldShadow, nullptr);
            if (patch.pixels.width && patch.pixels.height) {
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
        }
        if (spawnDust && combatEffects && isBuildingPointVisible(tree.x, tree.y)) {
            combatVfx.spawnDust(static_cast<float>(tree.x), static_cast<float>(tree.y), true);
        }
    }
    return anyFallen;
}

void GameSession::stampBattleCrater(int simX, int simY, bool large) {
    if (!scene) return;
    if (large) {
        (void)crushOrBlastTreesAt(simX, simY, 44, true);
    }
    const auto& vec = (large && !craterCatalog.largeCraters.empty())
        ? craterCatalog.largeCraters
        : (!craterCatalog.smallCraters.empty() ? craterCatalog.smallCraters : craterCatalog.largeCraters);
    if (vec.empty()) return;
    const auto& img = vec[(craterSeed++) % vec.size()];
    const auto patch = scene->stampAlphaOverlay(simX, simY, img);
    if (patch.pixels.width && patch.pixels.height) {
        renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
    }
}

int GameSession::initialMinesForId(std::string_view id) const {
    std::string low(id);
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr;
    if (low.find("sapper") != std::string::npos || low.find("miner") != std::string::npos ||
        (spec && (spec->selType == "sapper" || spec->selType == "sappers"))) {
        return 5;
    }
    if (spec && !spec->isVehicle && !spec->isMedicInfantry() &&
        spec->layMineTime > 0 && spec->mineAmmo > 0) {
        return std::clamp(spec->mineAmmo, 1, 5);
    }
    return 0;
}

bool GameSession::isCombatAmmoSoldierId(std::string_view id) {
    std::string low(id);
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return low.find("medic") == std::string::npos && low.find("officer") == std::string::npos;
}

bool GameSession::isArmedVehicleId(const std::string& id) const {
    const auto* vSpec = unitCatalog ? unitCatalog->find(id) : nullptr;
    return vSpec && ((vSpec->shot1.damage > 0 && vSpec->shot1.range > 0) ||
                     (vSpec->shot2.damage > 0 && vSpec->shot2.range > 0));
}

int GameSession::scaledSoldierAmmo(const std::string& id, int rawAmmo) const {
    const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr;
    if (spec && !spec->isVehicle && spec->ammo1 > 0) {
        return rawAmmo > 0 ? std::max(1, (spec->ammo1 * std::clamp(rawAmmo, 1, 100)) / 100) : spec->ammo1;
    }
    return rawAmmo;
}

bool GameSession::soldierHasBinoculars(const std::string& id) const {
    if (const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr) {
        if (!spec->isVehicle && spec->hasBinoculars()) return true;
    }
    std::string low(id);
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return low.find("officer") != std::string::npos || low.find("general") != std::string::npos ||
           low.find("sniper") != std::string::npos;
}

bool GameSession::isBombardInfantryId(const std::string& id) const {
    if (const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr) {
        if (!spec->isVehicle && (spec->isMortarInfantry() || spec->hasFlamethrower())) return true;
    }
    std::string low(id);
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return low.find("mortar") != std::string::npos || low.find("flam") != std::string::npos;
}

std::string GameSession::hudClassLabel(const std::string& id) {
    if (id == "dguner") return "Rifleman";
    if (id == "dsmguner") return "SMG";
    if (id == "dflamer") return "Flamer";
    if (id == "dofficer") return "Officer";
    if (id == "aguner") return "US Rifleman";
    if (id == "aofficer") return "US Officer";
    if (id == "cmedic") return "Medic";
    if (id == "rsmguner") return "Soviet SMG";
    if (id.size() >= 2) {
        std::string nation;
        switch (id[0]) {
        case 'a': nation = "US "; break;
        case 'b': case 'e': nation = "British "; break;
        case 'd': nation = "German "; break;
        case 'r': nation = "Soviet "; break;
        case 'j': nation = "Japanese "; break;
        default: break;
        }
        const std::string_view role(id.data() + (nation.empty() ? 0 : 1), id.size() - (nation.empty() ? 0 : 1));
        if (role.find("smgun") != std::string_view::npos) return nation + "SMG";
        if (role.find("guner") != std::string_view::npos || role.find("rifle") != std::string_view::npos) return nation + "Rifleman";
        if (role.find("sniper") != std::string_view::npos) return nation + "Sniper";
        if (role.find("officer") != std::string_view::npos) return nation + "Officer";
        if (role.find("flam") != std::string_view::npos) return nation + "Flamer";
        if (role.find("baz") != std::string_view::npos || role.find("faust") != std::string_view::npos ||
            role.find("at") != std::string_view::npos) return nation + "AT Soldier";
        if (role.find("medic") != std::string_view::npos) return "Medic";
        if (role.find("crew") != std::string_view::npos || role.find("pilot") != std::string_view::npos) return nation + "Crew";
        if (role.find("miner") != std::string_view::npos || role.find("sapper") != std::string_view::npos) return nation + "Sapper";
        if (role.find("para") != std::string_view::npos) return nation + "Paratrooper";
    }
    if (id == "jeep") return "Jeep";
    if (id == "kubel" || id == "kubelwagen") return "Kubelwagen";
    if (id.find("maultier") != std::string_view::npos) return "Maultier";
    if (id.find("sherman") != std::string_view::npos) return "Sherman";
    if (id == "panther") return "Panther";
    if (id == "tiger") return "Tiger";
    if (id == "hetzer") return "Hetzer";
    if (id == "wirbelwind") return "Wirbelwind";
    if (id.find("nebelwerfer") != std::string_view::npos) return "Nebelwerfer";
    if (id == "calliope") return "Calliope";
    if (id == "zis-3") return "ZiS-3";
    if (id == "m3a1") return "AT Gun";
    if (id == "ds39") return "Heavy MG";
    if (id == "m16") return "M16 Halftrack";
    return id;
}

std::string GameSession::hudDisplayName(const std::string& id) const {
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id)) {
            if (!spec->displayName.empty()) return spec->displayName;
            if (!spec->shortName.empty()) return spec->shortName;
        }
    }
    return hudClassLabel(id);
}

int GameSession::scoreValueForId(const std::string& id, bool isVehicle) const {
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id)) {
            return spec->effectiveScoreValue();
        }
    }
    return isVehicle ? 250 : 100;
}

void GameSession::ensureUnitPortrait(const std::string& id, bool isVehicle) {
    if (!hud || id.empty()) return;
    const std::string disp = hudDisplayName(id);
    if (registeredPortraits.contains(disp)) return;
    std::optional<ss2::assets::MapImage> portrait;
    if (!hdPack.empty() && !originalInfantry) {
        const auto pp = hdPack / "hud-portraits" / (id + ".png");
        std::error_code ec;
        if (std::filesystem::is_regular_file(pp, ec)) {
            portrait = ss2::assets::loadPngRgba(pp);
        }
    }
    if (!portrait) {
        if (isVehicle && vehicleScene) {
            portrait = vehicleScene->renderUnitPortrait(id, 48u);
        } else if (!isVehicle && scene) {
            portrait = scene->renderUnitPortrait(id, 48u);
        }
    }
    if (!portrait) return;
    hud->setPortrait(disp, *portrait);
    const std::string lbl = hudClassLabel(id);
    if (!lbl.empty() && lbl != disp && !registeredPortraits.contains(lbl)) {
        hud->setPortrait(lbl, *portrait);
        registeredPortraits.insert(lbl);
    }
    registeredPortraits.insert(disp);
}

assets::MapImage GameSession::downsampleOverview(const assets::MapImage& src, unsigned dstW, unsigned dstH) {
    ss2::assets::MapImage out{dstW, dstH, std::vector<std::uint8_t>(std::size_t(dstW) * dstH * 4u, 255u)};
    if (!dstW || !dstH || !src.width || !src.height ||
        src.rgba.size() < std::size_t(src.width) * src.height * 4u) {
        return out;
    }
    for (unsigned y = 0; y < dstH; ++y) {
        const unsigned sy0 = static_cast<unsigned>((std::uint64_t(y) * src.height) / dstH);
        const unsigned sy1 = std::max(sy0 + 1u, static_cast<unsigned>((std::uint64_t(y + 1u) * src.height) / dstH));
        const unsigned cy1 = std::min(sy1, src.height);
        for (unsigned x = 0; x < dstW; ++x) {
            const unsigned sx0 = static_cast<unsigned>((std::uint64_t(x) * src.width) / dstW);
            const unsigned sx1 = std::max(sx0 + 1u, static_cast<unsigned>((std::uint64_t(x + 1u) * src.width) / dstW));
            const unsigned cx1 = std::min(sx1, src.width);
            std::uint64_t sumR = 0, sumG = 0, sumB = 0, cnt = 0;
            for (unsigned sy = sy0; sy < cy1; ++sy) {
                const std::size_t rowOff = std::size_t(sy) * src.width;
                for (unsigned sx = sx0; sx < cx1; ++sx) {
                    const std::size_t si = (rowOff + sx) * 4u;
                    sumR += src.rgba[si + 0u];
                    sumG += src.rgba[si + 1u];
                    sumB += src.rgba[si + 2u];
                    ++cnt;
                }
            }
            const std::size_t di = (std::size_t(y) * dstW + x) * 4u;
            if (cnt > 0u) {
                out.rgba[di + 0u] = static_cast<std::uint8_t>((sumR + cnt / 2u) / cnt);
                out.rgba[di + 1u] = static_cast<std::uint8_t>((sumG + cnt / 2u) / cnt);
                out.rgba[di + 2u] = static_cast<std::uint8_t>((sumB + cnt / 2u) / cnt);
            }
            out.rgba[di + 3u] = 255u;
        }
    }
    return out;
}

void GameSession::updateMinimapFog(const std::vector<std::uint8_t>& fogGrid, bool force) {
    if (!hud || !baseMinimap.width || !baseMinimap.height || !hudMapW || !hudMapH ||
        !fogGridW || !fogGridH || fogGrid.size() != std::size_t(fogGridW) * fogGridH) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && lastMinimapFogUpdate.time_since_epoch().count() != 0 &&
        now - lastMinimapFogUpdate < std::chrono::milliseconds(200)) {
        return;
    }
    if (replayAllVision()) {
        hud->setMinimap(baseMinimap);
        lastMinimapFogUpdate = now;
        return;
    }
    ss2::assets::MapImage masked = baseMinimap;
    const int originX = int(fogGridH) * 32;
    const std::uint64_t mapW = hudMapW;
    const std::uint64_t mapH = hudMapH;
    for (unsigned py = 0; py < masked.height; ++py) {
        const int my = static_cast<int>((std::uint64_t(py) * mapH) / masked.height);
        for (unsigned px = 0; px < masked.width; ++px) {
            const int mx = static_cast<int>((std::uint64_t(px) * mapW) / masked.width);
            const int halfX = (mx - originX) >> 1;
            const int tx = (my + halfX) >> 5;
            const int ty = (my - halfX) >> 5;
            int num = 38;
            if (tx >= 0 && ty >= 0 && tx < int(fogGridW) && ty < int(fogGridH)) {
                const std::uint8_t f = fogGrid[std::size_t(ty) * fogGridW + std::size_t(tx)];
                const int viewer = localOwner();
                num = (f & static_cast<std::uint8_t>(1u << viewer))
                    ? 256
                    : ((f & static_cast<std::uint8_t>(0x10u << viewer)) ? 124 : 38);
            }
            if (num < 256) {
                const std::size_t idx = (std::size_t(py) * masked.width + px) * 4u;
                masked.rgba[idx + 0] = static_cast<std::uint8_t>((unsigned(masked.rgba[idx + 0]) * unsigned(num)) >> 8u);
                masked.rgba[idx + 1] = static_cast<std::uint8_t>((unsigned(masked.rgba[idx + 1]) * unsigned(num)) >> 8u);
                masked.rgba[idx + 2] = static_cast<std::uint8_t>((unsigned(masked.rgba[idx + 2]) * unsigned(num)) >> 8u);
            }
        }
    }
    hud->setMinimap(masked);
    lastMinimapFogUpdate = now;
}

void GameSession::resetCursor() {
    if (auto* bank = getTacticalCursorBank(window)) {
        bank->resetToField(moveCursor.get(), attackCursor.get());
        return;
    }
    SDL_SetCursor(SDL_GetDefaultCursor());
}

void GameSession::recordEventPos(double x, double y) {
    if (!eventHistory.empty()) {
        const double dx = eventHistory.front().first - x;
        const double dy = eventHistory.front().second - y;
        if (dx * dx + dy * dy <= 96.0 * 96.0) return;
    }
    eventHistory.push_front({x, y});
    if (eventHistory.size() > 6u) eventHistory.pop_back();
    eventHistoryCursor = 0;
}

void GameSession::syncMarkers() {
    std::vector<ss2::MapMarker> out;
    out.reserve(activeMarkers.size());
    for (const auto& m : activeMarkers) out.push_back(m.marker);
    renderer.setMarkers(std::move(out));
}

void GameSession::pushMinimapPing(int simX, int simY, bool hostile) {
    if (!hudMapW || !hudMapH) return;
    const float u = std::clamp(static_cast<float>(simX) / static_cast<float>(hudMapW), 0.0f, 1.0f);
    const float v = std::clamp(static_cast<float>(simY) / static_cast<float>(hudMapH), 0.0f, 1.0f);
    if (minimapPings.size() >= 6) minimapPings.erase(minimapPings.begin());
    minimapPings.push_back({u, v, hostile, std::chrono::steady_clock::now()});
}

void GameSession::showPause() {
    orderNotice.show(ss2::tr("Paused. Press Pause or Ctrl+P to resume."), ss2::NoticeKind::unsupported,
                     ss2::Notice::Duration::zero());
}

void GameSession::logHud(const std::string& msg) {
    if (hud && !msg.empty()) hud->log(msg);
}

void GameSession::showNotice(const std::string& msg, NoticeKind kind) {
    orderNotice.show(msg, kind);
    logHud(msg);
}

const char* GameSession::airKindLabel(assets::AirMissionKind kind) {
    switch (kind) {
    case ss2::assets::AirMissionKind::Bomber: return "Bomber Strike";
    case ss2::assets::AirMissionKind::Recon: return "Recon Overflight";
    case ss2::assets::AirMissionKind::Paratrooper: return "Paratrooper Drop";
    case ss2::assets::AirMissionKind::Supply: return "Air Supply Drop";
    case ss2::assets::AirMissionKind::Fighter: return "Fighter Strafe";
    }
    return "Air Support";
}

bool GameSession::ensureAircraftAtlas(const std::string& planeId) {
    if (planeId.empty()) return false;
    if (gpuAircraftAtlases.contains(planeId)) return true;
    if (failedAircraftIds.contains(planeId)) return false;
    if (!gameDir.empty()) {
        if (auto built = ss2::assets::buildAircraftGpuAtlas(gameDir, planeId, true)) {
            gpuAircraftAtlases.emplace(planeId, std::move(*built));
            return true;
        }
    }
    failedAircraftIds.insert(planeId);
    return false;
}

void GameSession::uploadPlaneSprite(LivePlane& lp) {
    if (const auto* pSpec = unitCatalog ? unitCatalog->findPlane(lp.pose.planeId) : nullptr;
        pSpec && pSpec->altitude > 0) {
        lp.shadowScale = std::clamp(static_cast<float>(pSpec->altitude) / 280.0f, 0.65f, 1.50f);
    }
    const std::string key = lp.pose.planeId.empty() ? std::string("ju-87") : lp.pose.planeId;
    lp.spriteKey = key;
    lp.atlasSlot = planeAtlas.acquire(key);
    if (lp.atlasSlot == planeAtlas.overflowSlot() && !planeOverflowLogged) {
        planeOverflowLogged = true;
        std::cerr << "Plane atlas overflow: more than " << planeAtlas.liveCount()
                  << " distinct live plane models; extra aircraft share the overflow slot.\n";
    }
    if (ensureAircraftAtlas(key)) {
        const auto& atlas = gpuAircraftAtlases.at(key);
        const auto bit = boundPlaneSlots.find(lp.atlasSlot);
        if (lp.atlasSlot == planeAtlas.overflowSlot() || bit == boundPlaneSlots.end() || bit->second != key) {
            renderer.setSpriteAtlas(lp.atlasSlot, atlas.atlas);
            boundPlaneSlots[lp.atlasSlot] = key;
        }
        lp.texW = static_cast<float>(atlas.atlas.width);
        lp.texH = static_cast<float>(atlas.atlas.height);
        const float gs = static_cast<float>(std::max(1, atlas.scale));
        lp.sprW = (static_cast<float>(atlas.cellW) / gs) * static_cast<float>(mapScale);
        lp.sprH = (static_cast<float>(atlas.cellH) / gs) * static_cast<float>(mapScale);
        return;
    }
    auto it = planeSpriteCache.find(key);
    if (it == planeSpriteCache.end()) {
        ss2::assets::MapImage fb{28, 28, std::vector<std::uint8_t>(28u * 28u * 4u, 0)};
        for (int py = 0; py < 28; ++py) {
            for (int px = 0; px < 28; ++px) {
                const bool body = std::abs(px - 14) <= 2 && std::abs(py - 14) <= 11;
                const bool wing = std::abs(py - 12) <= 2 && std::abs(px - 14) <= 12;
                if (body || wing) {
                    const std::size_t o = (std::size_t(py) * 28u + static_cast<std::size_t>(px)) * 4u;
                    fb.rgba[o] = 110;
                    fb.rgba[o + 1] = 120;
                    fb.rgba[o + 2] = 95;
                    fb.rgba[o + 3] = 255;
                }
            }
        }
        it = planeSpriteCache.emplace(key, std::move(fb)).first;
    }
    const auto& img = it->second;
    const auto bit = boundPlaneSlots.find(lp.atlasSlot);
    if (lp.atlasSlot == planeAtlas.overflowSlot() || bit == boundPlaneSlots.end() || bit->second != key) {
        renderer.setSpriteAtlas(lp.atlasSlot, img);
        boundPlaneSlots[lp.atlasSlot] = key;
    }
    lp.texW = static_cast<float>(img.width);
    lp.texH = static_cast<float>(img.height);
    lp.sprW = static_cast<float>(img.width * static_cast<unsigned>(mapScale));
    lp.sprH = static_cast<float>(img.height * static_cast<unsigned>(mapScale));
}

std::optional<std::pair<size_t, size_t>> GameSession::spawnAndRegisterVehicle(const assets::Placement& p) {
    if (!vehicleScene || !simulation || !selection) return std::nullopt;
    const auto vIdx = vehicleScene->spawn(p);
    if (!vIdx) return std::nullopt;
    auto v = vehicleScene->vehicles().at(*vIdx);
    if (multiplayer.enabled && p.owner >= 0 && p.owner < multiplayer.playerCount) {
        v.hasCrew = true;
    }
    ensureUnitPortrait(v.id, true);
    (void)ensureGpuVehicleSlot(v.id);
    const size_t simVIdx = simulation->spawnVehicle(v);
    int maxVehHp = std::max(1, v.hp);
    int realHp = v.hp;
    int realAmmo = v.ammo;
    int cap = 0;
    int a2 = 0;
    bool hatchOpen = false;
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(v.id)) {
            if (spec->health > 0) {
                maxVehHp = spec->health;
                if (v.alive && v.hp > 0) {
                    realHp = std::max(1, (maxVehHp * std::clamp(v.hp, 1, 100)) / 100);
                }
            }
            cap = std::clamp(spec->stock, 0, 44);
            hatchOpen = spec->marchEnabled || (spec->marchSightBonus > 0);
            realAmmo = spec->ammo1 > 0
                ? std::max(0, (spec->ammo1 * std::clamp(v.ammo, 0, 100)) / 100)
                : ((spec->shot1.damage > 0 && spec->shot1.range > 0) ? std::max(1, v.ammo) : v.ammo);
            a2 = spec->ammo2 > 0
                ? std::max(0, (spec->ammo2 * std::clamp(v.ammo, 0, 100)) / 100)
                : ((spec->shot2.damage > 0 && spec->shot2.range > 0) ? std::max(1, v.ammo) : 0);
        }
    }
    const int initVis = multiplayer.enabled ? (isAllyOwner(v.owner) ? 1 : 0) : (v.visible ? 1 : 0);
    vehicleScene->update(*vIdx, v.x, v.y, v.hullDir, v.turretDir,
                         realHp, initVis, realAmmo);
    selection->addVehicle(scaleVehicle(vehicleScene->vehicles().at(*vIdx), mapScale));
    initialVehicleHp.push_back(maxVehHp);
    vehExpa.push_back(ss2::TutorialUnits::missionExpaToSim(v.expa));
    vehKills.push_back(0);
    vehicleAmmo2.push_back(a2);
    vehicleCargo.emplace_back(0, cap);
    vehicleTowed.push_back(-1);
    vehicleTowedBy.push_back(-1);
    vehicleCrewImpaired.push_back(false);
    vehicleMoveImpaired.push_back(false);
    vehicleSuppressed.push_back(false);
    vehicleHatchOpen.push_back(hatchOpen);
    if (mapScale == 1 && scene && !gpuVehicleIds.contains(v.id)) {
        const int l = v.width > 0 ? v.left - 4 : v.x - 24;
        const int t = v.height > 0 ? v.top - 4 : v.y - 24;
        const int r = v.width > 0 ? v.left + v.width + 4 : v.x + 24;
        const int b = v.height > 0 ? v.top + v.height + 4 : v.y + 24;
        const auto patch = scene->repaintRegion(l, t, r, b);
        if (patch.pixels.width && patch.pixels.height) {
            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
        }
    } else if (gpuVehicleIds.contains(v.id)) {
        syncGpuSprites();
    }
    return std::pair{*vIdx, simVIdx};
}

void GameSession::registerSimSpawnedSoldier(std::size_t simIdx, const std::string& unitId,
                                            int x, int y, int owner, int group) {
    if (!scene || !selection) return;
    while (scene->units().size() <= simIdx) {
        const std::string wantId = !unitId.empty() ? unitId : std::string("dguner");
        ss2::assets::Placement p{wantId, x, y, 0, 100, 100, group, owner};
        (void)ensureGpuInfantrySlot(wantId);
        auto spawned = scene->spawn(p);
        if (!spawned && wantId != "dguner") {
            p.id = "dguner";
            (void)ensureGpuInfantrySlot("dguner");
            spawned = scene->spawn(p);
        }
        if (!spawned) break;
        const auto& [newIdx, patch] = *spawned;
        const auto& unit = scene->units().at(newIdx);
        ensureUnitPortrait(unit.id, false);
        const int realAmmo = scaledSoldierAmmo(unit.id, 100);
        const int initVis = (multiplayer.enabled || (replayFile && liveReplay && !replayAllVision()))
            ? (isAllyOwner(unit.owner) ? 1 : 0)
            : -1;
        (void)scene->update(newIdx, unit.x, unit.y, unit.direction, unit.progress, unit.walking,
                            unit.hp, initVis, realAmmo);
        selection->addUnit(scaleInfantry(scene->units().at(newIdx), mapScale));
        initialUnitHp.push_back(std::max(1, unit.hp));
        unitExpa.push_back(0);
        unitKills.push_back(0);
        unitGrenades.push_back(initialGrenadesForId(unit.id));
        unitMineAmmo.push_back(initialMinesForId(unit.id));
        if (patch.pixels.width && patch.pixels.height) {
            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
        }
    }
}

void GameSession::registerSimSpawnedVehicle(std::size_t simVIdx, const std::string& unitId,
                                            int x, int y, int owner, int group, unsigned dir32) {
    if (!vehicleScene || !selection) return;
    while (vehicleScene->vehicles().size() <= simVIdx) {
        const std::string wantId = !unitId.empty() ? unitId : std::string("ammo.box");
        ss2::assets::Placement p{wantId, x, y, static_cast<int>(dir32 & 31u), 100, 100, group, owner};
        std::size_t vIdx = 0;
        if (const auto spawned = vehicleScene->spawn(p)) {
            vIdx = *spawned;
        } else {
            vIdx = vehicleScene->spawnVisualPlaceholder(p, 24, 20);
        }
        const auto& v = vehicleScene->vehicles().at(vIdx);
        ensureUnitPortrait(v.id, true);
        (void)ensureGpuVehicleSlot(v.id);
        int maxVehHp = std::max(1, v.hp);
        int realHp = v.hp;
        int realAmmo = v.ammo;
        int cap = 0;
        int a2 = 0;
        bool hatchOpen = false;
        if (unitCatalog) {
            if (const auto* spec = unitCatalog->find(v.id)) {
                if (spec->health > 0) {
                    maxVehHp = spec->health;
                    realHp = maxVehHp;
                }
                cap = std::clamp(spec->stock, 0, 44);
                hatchOpen = spec->marchEnabled || (spec->marchSightBonus > 0);
                if (spec->ammo1 > 0) realAmmo = spec->ammo1;
                if (spec->ammo2 > 0) a2 = spec->ammo2;
            }
        }
        const int initVis = (multiplayer.enabled || (replayFile && liveReplay && !replayAllVision()))
            ? ((isAllyOwner(v.owner) || v.owner == 3) ? 1 : 0)
            : (v.visible ? 1 : 0);
        const auto dirty = vehicleScene->update(vIdx, v.x, v.y, v.hullDir, v.turretDir, realHp, initVis, realAmmo);
        selection->addVehicle(scaleVehicle(vehicleScene->vehicles().at(vIdx), mapScale));
        initialVehicleHp.push_back(maxVehHp);
        vehExpa.push_back(0);
        vehKills.push_back(0);
        vehicleAmmo2.push_back(a2);
        vehicleCargo.emplace_back(0, cap);
        vehicleTowed.push_back(-1);
        vehicleTowedBy.push_back(-1);
        vehicleCrewImpaired.push_back(false);
        vehicleMoveImpaired.push_back(false);
        vehicleSuppressed.push_back(false);
        vehicleHatchOpen.push_back(hatchOpen);
        if (mapScale == 1 && scene && dirty.valid() && !gpuVehicleIds.contains(v.id)) {
            const auto patch = scene->repaintRegion(dirty.left, dirty.top, dirty.right, dirty.bottom);
            if (patch.pixels.width && patch.pixels.height) {
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
        }
    }
}

void GameSession::syncReplayViewerPerspective() {
    if (!replayFile || !liveReplay || !simulation) return;
    const int simSeat = replayTransport.viewerSeat >= 0 ? (replayTransport.viewerSeat & 3) : 0;
    simulation->setViewerOwner(simSeat);
    if (scene) scene->setFogViewerOwner(localOwner());
    if (selection) {
        selection->setLocalPlayer(localOwner());
        selection->setSelection({}, {});
    }
    if (scene && !latestFog.empty()) {
        if (replayAllVision()) {
            const std::vector<std::uint8_t> allLit(latestFog.size(), 0xFFu);
            const auto fogPatch = scene->updateFog(allLit);
            if (fogPatch.pixels.width && fogPatch.pixels.height) {
                renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
            }
        } else {
            const auto fogPatch = scene->updateFog(latestFog);
            if (fogPatch.pixels.width && fogPatch.pixels.height) {
                renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
            }
        }
        updateMinimapFog(latestFog, true);
    }
    syncGpuSprites();
}

void GameSession::spawnReinforcementWave(const MissionEvent& ev) {
    if (!simulation || !scene || !selection) return;
    const auto& data = simulation->data();
    const auto wave = simulation->reinforcementWave(ev.index);
    if (wave.empty()) return;
    int gateX = 0, gateY = 0;
    if (ev.gate < data.gates.size() && (data.gates[ev.gate].first != 0 || data.gates[ev.gate].second != 0)) {
        gateX = data.gates[ev.gate].first;
        gateY = data.gates[ev.gate].second;
    } else if (ev.gate < data.markers.size() && (data.markers[ev.gate].first != 0 || data.markers[ev.gate].second != 0)) {
        gateX = data.markers[ev.gate].first;
        gateY = data.markers[ev.gate].second;
    } else if (ev.destZone < data.locations.size() &&
               (data.locations[ev.destZone].first != 0 || data.locations[ev.destZone].second != 0)) {
        gateX = data.locations[ev.destZone].first;
        gateY = data.locations[ev.destZone].second;
    } else {
        gateX = static_cast<int>(hudMapW / 2);
        gateY = static_cast<int>(hudMapH / 2);
    }
    bool hadPlayerInfantry = false;
    for (const auto& u : scene->units()) {
        if (u.owner == localOwner() && u.hp > 0) { hadPlayerInfantry = true; break; }
    }
    std::size_t spawnedInfantry = 0, spawnedVehicles = 0;
    std::vector<size_t> spawnedInfIndices, spawnedVehIndices;
    int firstX = gateX, firstY = gateY;
    const int maxMapX = std::max(32, static_cast<int>(hudMapW) - 16);
    const int maxMapY = std::max(32, static_cast<int>(hudMapH) - 16);
    for (std::size_t i = 0; i < wave.size(); ++i) {
        const auto& ru = wave[i];
        const int col = static_cast<int>(i % 6);
        const int row = static_cast<int>(i / 6);
        const int sx = std::clamp(gateX + (col - 2) * 20, 16, maxMapX);
        const int sy = std::clamp(gateY + row * 18, 16, maxMapY);
        ss2::assets::Placement p{ru.id, sx, sy, 0, ru.hp, ru.ammo, ru.group, static_cast<int>(ev.owner), ru.passengers, ru.expa};
        const auto* spec = unitCatalog ? unitCatalog->find(ru.id) : nullptr;
        const bool knownVehicle = spec && spec->isVehicle;
        bool spawnedAsInfantry = false;
        if (!knownVehicle) {
            (void)ensureGpuInfantrySlot(ru.id);
            if (auto spawned = scene->spawn(p)) {
                spawnedAsInfantry = true;
                const auto& [newIdx, patch] = *spawned;
                const auto& unit = scene->units().at(newIdx);
                ensureUnitPortrait(unit.id, false);
                const int simExpa = ss2::TutorialUnits::missionExpaToSim(ru.expa);
                const size_t simIdx = simulation->spawnInfantry(unit, simExpa);
                const int realAmmo = scaledSoldierAmmo(unit.id, ru.ammo);
                const int initWaveVis = multiplayer.enabled ? (isAllyOwner(unit.owner) ? 1 : 0) : -1;
                (void)scene->update(newIdx, unit.x, unit.y, unit.direction, unit.progress, unit.walking, unit.hp,
                                    initWaveVis, realAmmo);
                selection->addUnit(scaleInfantry(scene->units().at(newIdx), mapScale));
                initialUnitHp.push_back(std::max(1, unit.hp));
                unitExpa.push_back(simExpa);
                unitKills.push_back(0);
                unitGrenades.push_back(initialGrenadesForId(unit.id));
                unitMineAmmo.push_back(initialMinesForId(unit.id));
                spawnedInfIndices.push_back(simIdx);
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                if (spawnedInfantry + spawnedVehicles == 0) { firstX = unit.x; firstY = unit.y; }
                ++spawnedInfantry;
            }
        }
        if (!spawnedAsInfantry) {
            if (auto spawnedVeh = spawnAndRegisterVehicle(p)) {
            const auto [vIdx, simVIdx] = *spawnedVeh;
            const auto& v = vehicleScene->vehicles().at(vIdx);
            spawnedVehIndices.push_back(simVIdx);
            for (const auto& pax : ru.passengers) {
                if (pax.isCrew) continue;
                ss2::assets::Placement rp{pax.id, v.x, v.y, 0, pax.hp, pax.ammo, pax.group, static_cast<int>(ev.owner), {}, pax.expa};
                (void)ensureGpuInfantrySlot(pax.id);
                if (auto spawnedRider = scene->spawn(rp)) {
                    const auto rIdx = spawnedRider->first;
                    const auto& rUnit = scene->units().at(rIdx);
                    ensureUnitPortrait(rUnit.id, false);
                    const int simRiderExpa = ss2::TutorialUnits::missionExpaToSim(pax.expa);
                    const size_t simRIdx = simulation->spawnInfantry(rUnit, simRiderExpa);
                    const int realRiderAmmo = scaledSoldierAmmo(rUnit.id, pax.ammo);
                    if (const auto rPatch = scene->update(rIdx, rUnit.x, rUnit.y, rUnit.direction,
                                                          rUnit.progress, rUnit.walking, rUnit.hp, 0, realRiderAmmo);
                        rPatch.pixels.width && rPatch.pixels.height) {
                        renderer.updateMapRegion(rPatch.x, rPatch.y, rPatch.pixels);
                    }
                    auto scaledRider = scaleInfantry(scene->units().at(rIdx), mapScale);
                    scaledRider.visible = false;
                    selection->addUnit(std::move(scaledRider));
                    initialUnitHp.push_back(std::max(1, rUnit.hp));
                    unitExpa.push_back(simRiderExpa);
                    unitKills.push_back(0);
                    unitGrenades.push_back(initialGrenadesForId(rUnit.id));
                    unitMineAmmo.push_back(initialMinesForId(rUnit.id));
                    if (simulation->embarkImmediate(simRIdx, simVIdx) && simVIdx < vehicleCargo.size()) {
                        ++vehicleCargo[simVIdx].first;
                    }
                    ++spawnedInfantry;
                }
            }
            if (spawnedInfantry + spawnedVehicles == 0) { firstX = v.x; firstY = v.y; }
            ++spawnedVehicles;
            }
        }
    }
    const std::size_t totalSpawned = spawnedInfantry + spawnedVehicles;
    if (totalSpawned > 0) {
        const bool humanMpOwner = multiplayer.enabled && static_cast<int>(ev.owner) < multiplayer.playerCount;
        if ((ev.owner == 0 || humanMpOwner) && ev.gate < data.gateTargets.size() &&
            (data.gateTargets[ev.gate].first != 0 || data.gateTargets[ev.gate].second != 0)) {
            const auto [rx, ry] = data.gateTargets[ev.gate];
            if (multiplayer.enabled) {
                if (!spawnedInfIndices.empty()) {
                    simulation->applyAs(static_cast<int>(ev.owner), cmd::Move{spawnedInfIndices, rx, ry, false});
                }
                if (!spawnedVehIndices.empty()) {
                    simulation->applyAs(static_cast<int>(ev.owner), cmd::MoveVehicles{spawnedVehIndices, rx, ry, false});
                }
            } else {
                if (!spawnedInfIndices.empty()) (void)simulation->move(spawnedInfIndices, rx, ry);
                if (!spawnedVehIndices.empty()) (void)simulation->moveVehicles(spawnedVehIndices, rx, ry);
            }
        } else if (ev.owner != 0 && !humanMpOwner) {
            std::optional<std::pair<int, int>> rally;
            if (ev.destZone > 0 && ev.destZone < data.locations.size() &&
                (data.locations[ev.destZone].first != 0 || data.locations[ev.destZone].second != 0)) {
                rally = data.locations[ev.destZone];
            } else if (ev.gate < data.gateTargets.size() &&
                       (data.gateTargets[ev.gate].first != 0 || data.gateTargets[ev.gate].second != 0)) {
                rally = data.gateTargets[ev.gate];
            }
            if (rally) {
                std::set<int> waveGroups;
                for (const auto& ru : wave) {
                    if (ru.group > 0) waveGroups.insert(ru.group);
                    for (const auto& pax : ru.passengers) {
                        if (pax.group > 0) waveGroups.insert(pax.group);
                    }
                }
                for (int grp : waveGroups) {
                    (void)simulation->orderGroupMove(static_cast<unsigned>(grp), rally->first, rally->second, true);
                }
            }
        }
        std::cout << "Reinforcements arrived: " << spawnedInfantry << " infantry, "
                  << spawnedVehicles << " vehicles (wave=" << ev.index
                  << " owner=" << ev.owner << " gate=" << ev.gate << " at " << firstX << ',' << firstY << ")\n";
        if (static_cast<int>(ev.owner) == localOwner()) {
            recordEventPos(static_cast<double>(firstX * mapScale), static_cast<double>(firstY * mapScale));
            pushMinimapPing(firstX, firstY, false);
            if (audio) audio->playGroup("e_reenforcement", 0.72f, ss2::VoicePriority::High);
            showNotice(ss2::trf("Reinforcements arrived ({0} soldiers).", {std::to_string(totalSpawned)}),
                       ss2::NoticeKind::accepted);
            if (!hadPlayerInfantry) {
                renderer.focusMap(static_cast<double>(firstX * mapScale), static_cast<double>(firstY * mapScale));
            }
        }
    }
}

void GameSession::spawnAbandonedCrew(const std::vector<assets::Placement>& spawns) {
    if (!scene || !simulation || !selection || spawns.empty()) return;
    std::size_t crewCount = 0;
    for (const auto& rawCp : spawns) {
        auto cp = rawCp;
        (void)ensureGpuInfantrySlot(cp.id);
        auto spawned = scene->spawn(cp);
        if (!spawned) {
            for (const auto& existing : scene->units()) {
                if (existing.owner == cp.owner) {
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
            if (!multiplayer.enabled) {
                (void)simulation->spawnInfantry(unit, 0);
            } else {
                simulation->applyAs(unit.owner, cmd::SpawnInfantry{unit, 0});
            }
            const int realAmmo = scaledSoldierAmmo(unit.id, unit.ammo);
            const int visFlag = multiplayer.enabled ? (isAllyOwner(unit.owner) ? 1 : 0) : -1;
            (void)scene->update(newIdx, unit.x, unit.y, unit.direction, unit.progress, unit.walking, unit.hp,
                                visFlag, realAmmo);
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
    if (crewCount > 0) {
        syncGpuSprites();
        if (spawns.front().owner == localOwner()) {
            playSelectionVoice("move");
            showNotice(ss2::trf("Abandoned vehicle ({0} crew dismounted).", {std::to_string(crewCount)}),
                       ss2::NoticeKind::accepted);
        }
    }
}

bool GameSession::isBuildingPointVisible(int simX, int simY) const {
    if (replayAllVision()) return true;
    if (latestFog.empty() || fogGridW == 0 || fogGridH == 0) return true;
    const int originX = static_cast<int>(fogGridH) * 32;
    const auto [u, v] = ss2::tutorialFootWorld(simX, simY, originX);
    const int tx = std::clamp(u >> 5, 0, static_cast<int>(fogGridW) - 1);
    const int ty = std::clamp(v >> 5, 0, static_cast<int>(fogGridH) - 1);
    const std::size_t idx = static_cast<std::size_t>(ty) * fogGridW + static_cast<std::size_t>(tx);
    return idx < latestFog.size() && (latestFog[idx] & static_cast<std::uint8_t>(1u << localOwner())) != 0;
}

bool GameSession::isWaterPoint(int simX, int simY) const {
    if (terrainObstacles.empty() || fogGridW == 0 || fogGridH == 0) return false;
    if (const auto bi = bridgeAtMap(simX, simY); bi && *bi < liveBridges.size() && !liveBridges[*bi].destroyed) {
        return false;
    }
    const int originX = static_cast<int>(fogGridH) * 32;
    const auto [u, v] = ss2::tutorialFootWorld(simX, simY, originX);
    const int subX = u >> 4;
    const int subY = v >> 4;
    const int tx = subX >> 1;
    const int ty = subY >> 1;
    if (tx < 0 || ty < 0 || tx >= static_cast<int>(fogGridW) || ty >= static_cast<int>(fogGridH)) return false;
    const std::size_t idx = static_cast<std::size_t>(ty) * fogGridW + static_cast<std::size_t>(tx);
    if (idx >= terrainObstacles.size()) return false;
    const unsigned slotBit = 1u << static_cast<unsigned>((subX & 1) | ((subY & 1) << 1));
    return (terrainObstacles[idx] & (slotBit << 4u)) != 0u;
}

bool GameSession::isShallowsPoint(int simX, int simY) const {
    if (terrainObstacles.empty() || fogGridW == 0 || fogGridH == 0) return false;
    if (const auto bi = bridgeAtMap(simX, simY); bi && *bi < liveBridges.size() && !liveBridges[*bi].destroyed) {
        return false;
    }
    const int originX = static_cast<int>(fogGridH) * 32;
    const auto [u, v] = ss2::tutorialFootWorld(simX, simY, originX);
    const int subX = u >> 4;
    const int subY = v >> 4;
    const int tx = subX >> 1;
    const int ty = subY >> 1;
    if (tx < 0 || ty < 0 || tx >= static_cast<int>(fogGridW) || ty >= static_cast<int>(fogGridH)) return false;
    const std::size_t idx = static_cast<std::size_t>(ty) * fogGridW + static_cast<std::size_t>(tx);
    if (idx >= terrainObstacles.size()) return false;
    const unsigned slotBit = 1u << static_cast<unsigned>((subX & 1) | ((subY & 1) << 1));
    return (terrainObstacles[idx] & (slotBit << 8u)) != 0u;
}

} // namespace ss2
