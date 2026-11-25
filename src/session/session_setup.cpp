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
      briefing(window),
      orderNotice(window),
      moveCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR), SDL_DestroyCursor),
      attackCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER), SDL_DestroyCursor) {
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
        unitCatalog = ss2::assets::UnitCatalog::load(gameDir, data.scheme);
        reportLoadProgress(0.10f);
        auto map = isTutorial1
            ? ss2::assets::loadTutorialTerrain(gameDir)
            : ss2::assets::loadMissionTerrain(gameDir, missionPath);
        ss2::assets::SceneryDepth depth(map.width, map.height);
        std::vector<ss2::assets::BuildingPlacement> missionBuildings;
        std::vector<ss2::assets::BridgePlacement> missionBridges;
        const auto objects = isTutorial1
            ? ss2::assets::drawTutorialScenery(map, gameDir, &depth, 0, &missionBuildings, &missionBridges)
            : ss2::assets::drawMissionScenery(map, gameDir, missionPath, &depth, &missionBuildings, &missionBridges);
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
        if (!hdPack.empty() && !originalInfantry) {
            gpuArtDir = hdPack / "infantry-art-4x";
            if (std::filesystem::is_directory(gpuArtDir)) {
                std::set<std::string> vehicleIds;
                for (const auto& v : vehicleUnits) vehicleIds.insert(v.id);
                for (const auto& p : placements) {
                    if (!vehicleIds.contains(p.id)) ensureGpuInfantrySlot(p.id);
                }
                if (!gpuAtlasSlots.empty()) {
                    std::cout << "GPU 4x HD infantry: " << gpuAtlasSlots.size() << " classes (";
                    bool firstId = true;
                    for (const auto& [id, slot] : gpuAtlasSlots) {
                        (void)slot;
                        if (!firstId) std::cout << ", ";
                        firstId = false;
                        std::cout << id;
                    }
                    std::cout << ")\n";
                }
            } else {
                gpuArtDir.clear();
            }
        }
        if (isTutorial1 && !hdPack.empty()) {
            auto groundPath = hdPack / "tutorial1_ground_2x.ss2hd";
            cleanGround = std::filesystem::is_regular_file(groundPath);
            if (!originalInfantry && std::filesystem::is_directory(hdPack / "infantry-art"))
                infantryArt = hdPack / "infantry-art";
            auto hd = ss2::assets::loadHdPackMap(cleanGround ? groundPath : hdPack);
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
                        if (gpuNextSlot + 1u >= planeAtlasBase) continue;
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
        if (isTutorial1 && mapScale == 2 && !cleanGround) {
            ss2::assets::drawTutorialVehicles(map, gameDir, placements);
        }
        scene.emplace(std::move(map), gameDir, placements, originalInfantry, std::move(hdImage), infantryArt,
                      cleanGround, std::move(depth), &gpuAtlasSlots, data.width, data.height);
        if (mapScale == 1) {
            scene->setVehicleLayer([&](ss2::assets::MapImage& patch, int px, int py, const ss2::assets::SceneryDepth* d) {
                if (vehicleScene) vehicleScene->drawRegion(patch, px, py, d);
            });
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
        if (!gpuAtlasSlots.empty()) std::cout << "GPU 4x HD infantry atlases: " << gpuAtlasSlots.size() << " classes\n";
        else if (!infantryArt.empty()) std::cout << "Painted HD infantry: " << scene->paintedCount() << "/" << scene->units().size() << '\n';
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
            liveBridges.push_back({
                br.x, br.y, br.left, br.top, br.width, br.height,
                br.maxHp, br.maxHp, 0, false,
                br.patchLeft, br.patchTop, std::move(br.basePatch), br.palette, std::move(br.stageFrames), 0
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
        baseMinimap = downsampleOverview(composedMap, 512u, 256u);
        hud->setMinimap(baseMinimap);
        if (!hdPack.empty() && std::filesystem::is_directory(hdPack / "hud-portraits")) {
            for (const char* cls : {"aguner", "aofficer", "cmedic", "dflamer", "dguner", "dofficer", "dsmguner", "rsmguner"}) {
                const auto pp = hdPack / "hud-portraits" / (std::string(cls) + ".png");
                if (std::filesystem::is_regular_file(pp)) {
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
        renderer.setMapFilterLinear(smoothTerrain);
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

bool GameSession::ensureGpuInfantrySlot(const std::string& id) {
    if (gpuAtlasSlots.contains(id)) return true;
    const auto logFallback = [&](const char* reason) {
        if (!gpuArtDir.empty() && gpuFallbackIds.insert(id).second)
            std::cout << "GPU 4x HD infantry fallback Gold: " << id << " (" << reason << ")\n";
    };
    const unsigned maxInfSlot = combatVfx.atlasSlot().has_value() ? planeAtlasBase : (planeAtlasBase - 1u);
    if (gpuArtDir.empty() || gpuNextSlot >= maxInfSlot) {
        logFallback("slot budget full");
        return false;
    }
    const auto path = gpuArtDir / (id + ".png");
    if (!std::filesystem::is_regular_file(path)) {
        logFallback("no 4x sheet");
        return false;
    }
    auto atlas = ss2::assets::loadPngRgba(path);
    if (atlas.width != 1024 || atlas.height != 640) {
        logFallback("bad sheet dimensions");
        return false;
    }
    renderer.setSpriteAtlas(gpuNextSlot, atlas);
    gpuAtlasSlots.emplace(id, gpuNextSlot++);
    if (gpuPreloadLogged)
        std::cout << "GPU 4x HD infantry lazy slot: " << id << " (slot " << (gpuNextSlot - 1) << ")\n";
    return true;
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
    if (gpuAtlasSlots.empty() && worldGpuSprites.empty() && livePlanes.empty() && !hasAirTrails && !hasCombatVfx) {
        if (planesClearedNeeded) {
            renderer.setSprites({});
            planesClearedNeeded = false;
        }
        return;
    }
    planesClearedNeeded = !livePlanes.empty() || hasAirTrails || hasCombatVfx;
    struct DrawItem {
        ss2::Sprite sprite;
        int sortY;
    };
    std::vector<DrawItem> items;
    items.reserve(scene->units().size() + worldGpuSprites.size());
    for (const auto& ws : worldGpuSprites) {
        auto sp = ws.sprite;
        sp.brightness = fogBrightnessAt(ws.mapX1x, ws.mapY1x);
        items.push_back({sp, ws.sortY});
    }
    constexpr float cellPx = 128.0f;
    const float dstSize = 32.0f * static_cast<float>(mapScale);
    constexpr float footRatioY = 116.0f / 128.0f;
    for (const auto& c : fallenSoldiers) {
        if (!gpuAtlasSlots.contains(c.id)) continue;
        const unsigned col = c.direction & 7u;
        const float cw = dstSize * 0.96f;
        const float ch = dstSize * 0.36f;
        items.push_back({
            {
                static_cast<float>(c.x * mapScale) - 0.5f * cw,
                static_cast<float>(c.y * mapScale) - 0.65f * ch,
                cw,
                ch,
                static_cast<float>(col) * cellPx,
                0.0f,
                static_cast<float>(col + 1u) * cellPx,
                cellPx,
                gpuAtlasSlots.at(c.id),
                fogBrightnessAt(c.x, c.y) * 0.78f,
                0.92f
            },
            -10000 + c.y
        });
    }
    constexpr float recoilDx[8] = {-1.8f, 0.0f, 1.8f, 2.4f, 1.8f, 0.0f, -1.8f, -2.4f};
    constexpr float recoilDy[8] = {-1.2f, -1.8f, -1.2f, 0.0f, 1.2f, 1.8f, 1.2f, 0.0f};
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - animStart).count();
    const auto& units = scene->units();
    for (size_t idx = 0; idx < units.size(); ++idx) {
        const auto& u = units[idx];
        if (u.hp <= 0 || !u.visible || !gpuAtlasSlots.contains(u.id)) continue;
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
        if (const auto it = recoilUntil.find(idx); it != recoilUntil.end() && now < it->second) {
            ox += recoilDx[col];
            oy += recoilDy[col];
        }
        const float animScale = static_cast<float>(mapScale) * 0.5f;
        ox *= animScale;
        oy *= animScale;
        items.push_back({
            {
                static_cast<float>(u.x * mapScale) - 0.5f * w + ox,
                static_cast<float>(u.y * mapScale) - footRatioY * h + oy,
                w,
                h,
                static_cast<float>(col) * cellPx,
                static_cast<float>(row) * cellPx,
                static_cast<float>(col + 1u) * cellPx,
                static_cast<float>(row + 1u) * cellPx,
                gpuAtlasSlots.at(u.id),
                fogBrightnessAt(u.x, u.y),
                1.0f
            },
            u.y
        });
    }
    for (const auto& [sid, lp] : livePlanes) {
        (void)sid;
        const float altScale = lp.shadowScale;
        const float px = static_cast<float>(lp.pose.x * mapScale);
        const float py = static_cast<float>(lp.pose.y * mapScale);
        items.push_back({
            {
                px - lp.sprW * 0.5f + 44.0f * altScale * static_cast<float>(mapScale),
                py - lp.sprH * 0.5f + 66.0f * altScale * static_cast<float>(mapScale),
                lp.sprW,
                lp.sprH,
                0.0f, 0.0f, lp.texW, lp.texH,
                lp.atlasSlot,
                0.0f,
                0.40f
            },
            999998
        });
        items.push_back({
            {
                px - lp.sprW * 0.5f,
                py - lp.sprH * 0.5f,
                lp.sprW,
                lp.sprH,
                0.0f, 0.0f, lp.texW, lp.texH,
                lp.atlasSlot,
                1.0f,
                1.0f
            },
            1000000
        });
    }
    if (hasAirTrails) {
        std::vector<ss2::Sprite> trailSprites;
        hud->appendAirTrailSprites(mapScale, trailSprites);
        for (const auto& ts : trailSprites) {
            items.push_back({ts, 999999});
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
            items.push_back({vi.sprite, vi.sortY});
        }
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const DrawItem& a, const DrawItem& b) { return a.sortY < b.sortY; });
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
    if (lb.stampedStage == clamped || lb.basePatch.width == 0 || lb.basePatch.height == 0) return;
    assets::MapImage patch = lb.basePatch;
    const auto& frame = lb.stageFrames[static_cast<std::size_t>(clamped)];
    if (!frame.empty()) {
        assets::drawIndexedFrame(patch, lb.x - lb.patchLeft, lb.y - lb.patchTop, frame, lb.palette);
    }
    const auto upd = scene->stampGroundPatch(lb.patchLeft, lb.patchTop, patch, false);
    if (upd.pixels.width && upd.pixels.height) {
        renderer.updateMapRegion(upd.x, upd.y, upd.pixels);
    }
    lb.stampedStage = clamped;
}

void GameSession::stampBattleCrater(int simX, int simY, bool large) {
    if (!scene) return;
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
        case 'b': nation = "British "; break;
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
    if (isVehicle && vehicleScene) {
        portrait = vehicleScene->renderUnitPortrait(id, 48u);
    } else if (!isVehicle && scene) {
        portrait = scene->renderUnitPortrait(id, 48u);
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
    if (!src.width || !src.height || src.rgba.size() < std::size_t(src.width) * src.height * 4u) return out;
    for (unsigned y = 0; y < dstH; ++y) {
        const unsigned sy = (y * src.height) / dstH;
        for (unsigned x = 0; x < dstW; ++x) {
            const unsigned sx = (x * src.width) / dstW;
            const std::size_t si = (std::size_t(sy) * src.width + sx) * 4u;
            const std::size_t di = (std::size_t(y) * dstW + x) * 4u;
            out.rgba[di + 0] = src.rgba[si + 0];
            out.rgba[di + 1] = src.rgba[si + 1];
            out.rgba[di + 2] = src.rgba[si + 2];
            out.rgba[di + 3] = 255u;
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

void GameSession::uploadPlaneSprite(LivePlane& lp) {
    if (const auto* pSpec = unitCatalog ? unitCatalog->findPlane(lp.pose.planeId) : nullptr;
        pSpec && pSpec->altitude > 0) {
        lp.shadowScale = std::clamp(static_cast<float>(pSpec->altitude) / 280.0f, 0.65f, 1.50f);
    }
    const std::string key = lp.pose.planeId + "#" + std::to_string(lp.pose.dir32 & 31u);
    auto it = planeSpriteCache.find(key);
    if (it == planeSpriteCache.end()) {
        std::optional<ss2::assets::MapImage> loaded;
        if (!gameDir.empty()) {
            loaded = ss2::assets::loadAircraftSprite(gameDir, lp.pose.planeId, lp.pose.dir32);
        }
        if (!loaded) {
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
            loaded = std::move(fb);
        }
        it = planeSpriteCache.emplace(key, std::move(*loaded)).first;
    }
    const auto& img = it->second;
    lp.spriteKey = key;
    lp.atlasSlot = planeAtlas.acquire(key);
    if (lp.atlasSlot == planeAtlas.overflowSlot() && !planeOverflowLogged) {
        planeOverflowLogged = true;
        std::cerr << "Plane atlas overflow: more than " << planeAtlas.liveCount()
                  << " distinct live plane sprites; extra aircraft share the overflow slot.\n";
    }
    renderer.setSpriteAtlas(lp.atlasSlot, img);
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
    if (mapScale == 1 && scene) {
        const int l = v.width > 0 ? v.left - 4 : v.x - 24;
        const int t = v.height > 0 ? v.top - 4 : v.y - 24;
        const int r = v.width > 0 ? v.left + v.width + 4 : v.x + 24;
        const int b = v.height > 0 ? v.top + v.height + 4 : v.y + 24;
        const auto patch = scene->repaintRegion(l, t, r, b);
        if (patch.pixels.width && patch.pixels.height) {
            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
        }
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
        if (mapScale == 1 && scene && dirty.valid()) {
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
        (void)ensureGpuInfantrySlot(ru.id);
        if (auto spawned = scene->spawn(p)) {
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
        } else if (auto spawnedVeh = spawnAndRegisterVehicle(p)) {
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
