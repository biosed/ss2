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

void GameSession::updateHudAndBeams() {
    if (scene) {
        ss2::assets::InfantryScene::Cursor cursor;
        if (SDL_GetMouseFocus() == window && !briefing.active()) {
            float mouseX{}, mouseY{};
            SDL_GetMouseState(&mouseX,&mouseY);
            const auto [x,y] = renderer.mapPosition(mouseX,mouseY);
            cursor.emplace(int(std::floor(x/mapScale)),int(std::floor(y/mapScale)));
        }
        const auto [left,top] = renderer.mapPosition(0,0);
        const auto phase = (int(std::floor(left/mapScale)) ^ int(std::floor(top/mapScale))) & 1;
        for (const auto& patch : scene->setInspection(selection->selected(),cursor,phase)) {
            if (patch.pixels.width && patch.pixels.height)
                renderer.updateMapRegion(patch.x,patch.y,patch.pixels);
        }
    }
    if (hud && scene && selection && hudMapW && hudMapH) {
        ss2::HudState hs;
        if (simulation) {
            const unsigned secs = simulation->simTick() / 25u;
            const unsigned mins = secs / 60u;
            const unsigned remS = secs % 60u;
            std::string prefix = "[" + (mins < 10u ? std::string("0") : std::string{}) + std::to_string(mins) +
                                 ":" + (remS < 10u ? std::string("0") : std::string{}) + std::to_string(remS);
            if (paused) {
                prefix += " PAUSED";
            } else if (const int sp = simulation->speedPercent(); sp != 100) {
                prefix += (sp == 50 ? " 0.5x" : sp == 200 ? " 2x" : sp == 300 ? " 3x" : " 1x");
            }
            prefix += "] ";
            hs.objective = prefix + ss2::tr(currentObjective);
        } else {
            hs.objective = ss2::tr(currentObjective);   // mission text has no entry and passes through
        }
        hs.losses = static_cast<unsigned>(selection->playerLosses());
        hs.paused = paused;
        if (simulation) {
            const auto slots = simulation->playerAirSupport();
            for (std::size_t k = 0; k < 5u; ++k) hs.airReady[k] = std::max(0, slots[k].ready);
        }
        hs.armedAir = armedAirSupport ? static_cast<int>(*armedAirSupport) : -1;
        hs.planes.reserve(livePlanes.size());
        for (const auto& [sid, lp] : livePlanes) {
            (void)sid;
            const float pu = std::clamp(static_cast<float>(lp.pose.x) / static_cast<float>(hudMapW), 0.0f, 1.0f);
            const float pv = std::clamp(static_cast<float>(lp.pose.y) / static_cast<float>(hudMapH), 0.0f, 1.0f);
            hs.planes.push_back({pu, pv, !isAllyOwner(lp.pose.owner)});
        }
        if (objectiveMarkerSim && hudMapW && hudMapH) {
            hs.hasObjectiveMarker = true;
            hs.objectiveU = std::clamp(static_cast<float>(objectiveMarkerSim->first) / static_cast<float>(hudMapW),
                                       0.0f, 1.0f);
            hs.objectiveV = std::clamp(static_cast<float>(objectiveMarkerSim->second) / static_cast<float>(hudMapH),
                                       0.0f, 1.0f);
        }
        struct Agg { unsigned count = 0; float hpSum = 0.0f; float ammoSum = 0.0f; };
        std::map<std::string, Agg> byClass;
        const auto& units = scene->units();
        const std::vector<ss2::assets::VehicleUnit> noVehicles;
        const auto& vehUnits = vehicleScene ? vehicleScene->vehicles() : noVehicles;
        for (unsigned d = 1; d <= 10; ++d) {
            const unsigned digit = d % 10u;
            unsigned aliveInGrp = 0;
            for (const auto id : hudGroups[digit]) {
                if (id < units.size() && units[id].hp > 0) ++aliveInGrp;
            }
            for (const auto id : hudVehicleGroups[digit]) {
                if (id < vehUnits.size() && vehUnits[id].alive && vehUnits[id].hp > 0) ++aliveInGrp;
            }
            if (aliveInGrp > 0) hs.groups.push_back({digit, aliveInGrp});
        }
        unsigned selCount = 0;
        float selHpSum = 0.0f, selAmmoSum = 0.0f, selReadySum = 0.0f, selExpSum = 0.0f, selMoraleSum = 0.0f;
        for (const auto id : selection->selected()) {
            if (id >= units.size()) continue;
            const auto& u = units[id];
            if (u.hp <= 0) continue;
            const int maxHp = (id < initialUnitHp.size() && initialUnitHp[id] > 0) ? initialUnitHp[id] : 100;
            const auto* spec = unitCatalog ? unitCatalog->find(u.id) : nullptr;
            const int maxInfAmmo = (spec && !spec->isVehicle && spec->ammo1 > 0) ? spec->ammo1 : 40;
            const float hpFrac = std::clamp(static_cast<float>(u.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
            const float ammoFrac = std::clamp(static_cast<float>(u.ammo) / static_cast<float>(maxInfAmmo), 0.0f, 1.0f);
            auto& a = byClass[hudDisplayName(u.id)];
            ++a.count;
            a.hpSum += hpFrac;
            a.ammoSum += ammoFrac;
            ++selCount;
            selHpSum += hpFrac;
            selAmmoSum += ammoFrac;
            const bool suppressed = suppressedUnits.contains(id);
            selReadySum += holdFireUnits.contains(id) ? 0.35f
                : (suppressed ? 0.65f : (crouchedUnits.contains(id) ? 0.85f : 1.0f));
            const float expFrac = (id < unitExpa.size())
                ? std::clamp(static_cast<float>(unitExpa[id]) / 32768.0f, 0.0f, 1.0f) : 0.0f;
            selExpSum += expFrac;
            float unitMorale = std::clamp(0.45f + 0.55f * hpFrac + 0.15f * expFrac, 0.0f, 1.0f);
            if (suppressed) unitMorale *= 0.45f;
            selMoraleSum += unitMorale;
            if (crouchedUnits.contains(id)) hs.crouched = true;
            if (holdFireUnits.contains(id)) hs.holdFire = true;
            if (binocularUnits.contains(id)) hs.binoculars = true;
            std::string lowId = u.id;
            for (char& ch : lowId) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lowId.find("sapper") != std::string::npos || lowId.find("miner") != std::string::npos ||
                (spec && (spec->selType == "sapper" || spec->selType == "sappers")) ||
                (id < unitMineAmmo.size() && unitMineAmmo[id] > 0)) {
                hs.hasSapper = true;
            }
            if (isBombardInfantryId(u.id)) {
                hs.hasBombard = true;
            }
        }
        for (const auto id : selection->selectedVehicles()) {
            if (id >= vehUnits.size()) continue;
            const auto& v = vehUnits[id];
            if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
            const int maxHp = (id < initialVehicleHp.size() && initialVehicleHp[id] > 0) ? initialVehicleHp[id] : 100;
            const auto* vSpec = unitCatalog ? unitCatalog->find(v.id) : nullptr;
            const bool vArmed = !vSpec || (vSpec->shot1.damage > 0 && vSpec->shot1.range > 0) ||
                                (vSpec->shot2.damage > 0 && vSpec->shot2.range > 0);
            const int maxVehAmmo = (vSpec && vSpec->ammo1 > 0) ? vSpec->ammo1 : 100;
            const float hpFrac = std::clamp(static_cast<float>(v.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
            float ammoFrac = 1.0f;
            if (vSpec && vSpec->isSupplyTruck() && (vSpec->ammo1 > 0 || vSpec->ammo2 > 0)) {
                const int a2 = (id < vehicleAmmo2.size()) ? vehicleAmmo2[id] : 0;
                const int maxTotal = std::max(1, vSpec->ammo1 + vSpec->ammo2);
                ammoFrac = std::clamp(static_cast<float>(v.ammo + a2) / static_cast<float>(maxTotal), 0.0f, 1.0f);
            } else if (vArmed) {
                ammoFrac = std::clamp(static_cast<float>(v.ammo) / static_cast<float>(maxVehAmmo), 0.0f, 1.0f);
            }
            auto& a = byClass[hudDisplayName(v.id)];
            ++a.count;
            a.hpSum += hpFrac;
            a.ammoSum += ammoFrac;
            ++selCount;
            selHpSum += hpFrac;
            selAmmoSum += ammoFrac;
            const bool impaired = id < vehicleCrewImpaired.size() && vehicleCrewImpaired[id];
            const bool moveImp = id < vehicleMoveImpaired.size() && vehicleMoveImpaired[id];
            const bool vehSup = id < vehicleSuppressed.size() && vehicleSuppressed[id];
            selReadySum += vehicleHoldFireUnits.contains(id) ? 0.35f
                : ((impaired || moveImp) ? 0.50f : (vehSup ? 0.65f : 1.0f));
            const float expFrac = (id < vehExpa.size())
                ? std::clamp(static_cast<float>(vehExpa[id]) / 32768.0f, 0.0f, 1.0f) : 0.0f;
            selExpSum += expFrac;
            float vehMorale = std::clamp(0.45f + 0.55f * hpFrac + 0.15f * expFrac, 0.0f, 1.0f);
            if (impaired) vehMorale *= 0.65f;
            if (vehSup) vehMorale *= 0.50f;
            selMoraleSum += vehMorale;
            if (vehicleHoldFireUnits.contains(id)) hs.holdFire = true;
            if (id < vehicleCargo.size() && vehicleCargo[id].first > 0) hs.hasCargo = true;
            if (id < vehicleTowed.size() && vehicleTowed[id] >= 0) hs.hasCargo = true;
            if (!vSpec || vSpec->shot1.damage > 0 || vSpec->shot2.damage > 0) hs.hasBombard = true;
        }
        if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
            const auto& lb = liveBuildings[*selectedBuilding];
            if (!lb.destroyed && lb.owner == 0 && lb.occupantCount > 0) hs.hasCargo = true;
        }
        hs.attackMoveArmed = attackMoveArmed;
        hs.patrolArmed = patrolArmed;
        hs.attackGroundArmed = attackGroundArmed;
        hs.hasInfantry = !selection->selected().empty();
        if (selCount > 0) {
            const float invSel = 1.0f / static_cast<float>(selCount);
            const float avgHp = std::round(selHpSum * invSel * 100.0f) / 100.0f;
            const float avgAmmo = std::round(selAmmoSum * invSel * 100.0f) / 100.0f;
            const float avgReady = std::round(selReadySum * invSel * 100.0f) / 100.0f;
            const float avgMorale = std::round(std::clamp(selMoraleSum * invSel, 0.0f, 1.0f) * 100.0f) / 100.0f;
            const float avgExp = std::round(std::clamp(selExpSum * invSel, 0.0f, 1.0f) * 100.0f) / 100.0f;
            hs.hasSelection = true;
            hs.meters = {avgExp, avgHp, avgMorale, avgAmmo, avgReady};
        }
        for (const auto& [label, a] : byClass) {
            const float avgHp = std::round((a.hpSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
            const float avgAmmo = std::round((a.ammoSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
            hs.selection.push_back({label, a.count, avgHp, avgAmmo});
        }
        hs.dots.reserve(units.size() + vehUnits.size());
        std::vector<ss2::MapBar> unitBars;
        unitBars.reserve(selection->selected().size() + selection->selectedVehicles().size() + 16);
        const float invW = 1.0f / static_cast<float>(hudMapW);
        const float invH = 1.0f / static_cast<float>(hudMapH);
        for (std::size_t idx = 0; idx < units.size(); ++idx) {
            const auto& u = units[idx];
            if (u.hp <= 0 || !u.visible) continue;
            const float du = std::round(std::clamp(static_cast<float>(u.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
            const float dv = std::round(std::clamp(static_cast<float>(u.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
            hs.dots.push_back({du, dv, isHostileOwner(u.owner), isAllyOwner(u.owner) && u.owner != 0, u.owner == 3});
            const int maxHp = (idx < initialUnitHp.size()) ? initialUnitHp[idx] : 100;
            if (selection->selected().contains(idx) || u.hp < maxHp) {
                if (u.hp < maxHp && woundedBarLogged.insert(idx).second) {
                    std::cout << "Tutorial I wounded gauge: unit " << idx
                              << " hp=" << u.hp << "/" << maxHp << '\n';
                }
                const float barW = 16.0f * static_cast<float>(mapScale);
                const float barY = static_cast<float>(u.y * mapScale) - (crouchedUnits.contains(idx) ? 24.0f : 30.0f) * static_cast<float>(mapScale);
                unitBars.push_back({
                    static_cast<float>(u.x * mapScale),
                    barY,
                    barW,
                    std::clamp(static_cast<float>(u.hp) / static_cast<float>(maxHp), 0.0f, 1.0f)
                });
            }
        }
        for (std::size_t idx = 0; idx < vehUnits.size(); ++idx) {
            const auto& v = vehUnits[idx];
            if (!v.alive || v.hp <= 0 || !v.visible) continue;
            const float du = std::round(std::clamp(static_cast<float>(v.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
            const float dv = std::round(std::clamp(static_cast<float>(v.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
            hs.dots.push_back({du, dv, isHostileOwner(v.owner), isAllyOwner(v.owner) && v.owner != 0, v.owner == 3});
            const int maxHp = (idx < initialVehicleHp.size()) ? initialVehicleHp[idx] : 100;
            if (selection->selectedVehicles().contains(idx) || v.hp < maxHp) {
                const float barW = 28.0f * static_cast<float>(mapScale);
                const float barY = static_cast<float>((v.height > 0 ? v.top - 5 : v.y - 26) * mapScale);
                unitBars.push_back({
                    static_cast<float>(v.x * mapScale),
                    barY,
                    barW,
                    std::clamp(static_cast<float>(v.hp) / static_cast<float>(maxHp), 0.0f, 1.0f)
                });
            }
        }
        for (std::size_t bi = 0; bi < liveBuildings.size(); ++bi) {
            const auto& b = liveBuildings[bi];
            if (b.destroyed || (b.occupantCount == 0 && b.hp >= b.maxHp)) continue;
            if (b.owner != 0 && !isAllyOwner(b.owner) && !isBuildingPointVisible(b.x, b.y)) continue;
            if (b.occupantCount > 0 && b.owner >= 0) {
                const float du = std::round(std::clamp(static_cast<float>(b.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
                const float dv = std::round(std::clamp(static_cast<float>(b.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
                hs.dots.push_back({du, dv, isHostileOwner(b.owner), isAllyOwner(b.owner) && b.owner != 0, false});
            }
            const float barW = static_cast<float>(std::clamp(b.width / 2, 24, 52) * mapScale);
            const float barY = static_cast<float>((b.top - 5) * mapScale);
            unitBars.push_back({
                static_cast<float>(b.x * mapScale),
                barY,
                barW,
                std::clamp(static_cast<float>(b.hp) / static_cast<float>(std::max(1, b.maxHp)), 0.0f, 1.0f)
            });
        }
        bool barsChanged = unitBars.size() != lastUnitBars.size();
        if (!barsChanged) {
            for (std::size_t i = 0; i < unitBars.size(); ++i) {
                if (unitBars[i].x != lastUnitBars[i].x || unitBars[i].y != lastUnitBars[i].y ||
                    unitBars[i].width != lastUnitBars[i].width || unitBars[i].value != lastUnitBars[i].value) {
                    barsChanged = true;
                    break;
                }
            }
        }
        if (barsChanged) {
            lastUnitBars = unitBars;
            renderer.setBars(std::move(unitBars));
        }
        int winW = 1, winH = 1, pixelH = 1;
        if (!SDL_GetWindowSize(window, &winW, &winH)) { winW = 1; winH = 1; }
        if (!SDL_GetWindowSizeInPixels(window, nullptr, &pixelH)) pixelH = winH;
        const double mapViewH = ss2::hidpi::logicalMapViewHeight(winH, pixelH, static_cast<int>(hud->height()));
        const auto [mx0, my0] = renderer.mapPosition(0.0f, 0.0f);
        const auto [mx1, my1] = renderer.mapPosition(static_cast<float>(winW), static_cast<float>(mapViewH));
        const float totalW = static_cast<float>(hudMapW * mapScale);
        const float totalH = static_cast<float>(hudMapH * mapScale);
        hs.viewU = std::round(std::clamp(static_cast<float>(mx0) / totalW, 0.0f, 1.0f) * 256.0f) / 256.0f;
        hs.viewV = std::round(std::clamp(static_cast<float>(my0) / totalH, 0.0f, 1.0f) * 256.0f) / 256.0f;
        hs.viewW = std::round(std::clamp(static_cast<float>(mx1 - mx0) / totalW, 0.01f, 1.0f) * 256.0f) / 256.0f;
        hs.viewH = std::round(std::clamp(static_cast<float>(my1 - my0) / totalH, 0.01f, 1.0f) * 256.0f) / 256.0f;
        if (!minimapPings.empty()) {
            const auto pingNow = std::chrono::steady_clock::now();
            std::erase_if(minimapPings, [&](const MinimapPing& mp) {
                return pingNow - mp.start >= std::chrono::milliseconds(1800);
            });
            hs.pings.reserve(minimapPings.size());
            for (const auto& mp : minimapPings) {
                const float age = std::chrono::duration<float>(pingNow - mp.start).count();
                const float q = std::round(std::clamp(1.0f - age / 1.8f, 0.0f, 1.0f) * 8.0f) / 8.0f;
                if (q > 0.0f) hs.pings.push_back({mp.u, mp.v, mp.hostile, q});
            }
        }
        hud->update(hs);
        hud->draw(renderer);
    }
    std::vector<std::size_t> crippledSmokeVehicles;
    if (combatEffects && vehicleScene && unitCatalog) {
        const auto& vehs = vehicleScene->vehicles();
        for (std::size_t vi = 0; vi < vehs.size() && crippledSmokeVehicles.size() < 8u; ++vi) {
            const auto& v = vehs[vi];
            if (!v.alive || v.hp <= 0 || !v.visible) continue;
            if (const auto* spec = unitCatalog->find(v.id); spec && spec->moveDamageHp > 0 && v.hp < spec->moveDamageHp) {
                crippledSmokeVehicles.push_back(vi);
            }
        }
    }
    std::vector<ss2::TacticalOrderLine> orderLines;
    if (simulation && selection && (!selection->selected().empty() || !selection->selectedVehicles().empty())) {
        const auto& sel = selection->selected();
        const auto& vsel = selection->selectedVehicles();
        const std::vector<size_t> sids(sel.begin(), sel.end());
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        orderLines = simulation->selectionOrderLines(sids, vids);
    }
    std::vector<ss2::VehicleRangePreview> rangePreviews;
    if (attackGroundArmed && simulation && selection &&
        (!selection->selectedVehicles().empty() || !selection->selected().empty())) {
        const auto& vsel = selection->selectedVehicles();
        const auto& sel = selection->selected();
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        const std::vector<size_t> sids(sel.begin(), sel.end());
        rangePreviews = simulation->selectionRangePreviews(vids, sids);
        if (rangePreviews.size() > 3u) rangePreviews.resize(3u);
    }
    bool previewRing = false;
    float ringX = 0.0f, ringY = 0.0f, ringRad = 0.0f;
    std::uint8_t ringR = 244, ringG = 186, ringB = 78;
    if ((attackGroundArmed || armedAirSupport.has_value()) && SDL_GetMouseFocus() == window &&
        !briefing.active() && !menu.active()) {
        float mx{}, my{};
        SDL_GetMouseState(&mx, &my);
        if (!hud || !hud->opaqueAtWindow(mx, my)) {
            const auto [mapX, mapY] = renderer.mapPosition(mx, my);
            previewRing = true;
            ringX = static_cast<float>(mapX);
            ringY = static_cast<float>(mapY);
            if (armedAirSupport) {
                switch (*armedAirSupport) {
                case ss2::assets::AirMissionKind::Bomber:
                    ringRad = 96.0f; ringR = 235; ringG = 92; ringB = 68;
                    break;
                case ss2::assets::AirMissionKind::Fighter:
                    ringRad = 64.0f; ringR = 235; ringG = 92; ringB = 68;
                    break;
                case ss2::assets::AirMissionKind::Recon:
                    ringRad = 110.0f; ringR = 105; ringG = 212; ringB = 235;
                    break;
                case ss2::assets::AirMissionKind::Paratrooper:
                case ss2::assets::AirMissionKind::Supply:
                    ringRad = 72.0f; ringR = 105; ringG = 212; ringB = 235;
                    break;
                }
            } else {
                ringRad = 56.0f; ringR = 244; ringG = 186; ringB = 78;
                if (!rangePreviews.empty()) {
                    const float sScale = static_cast<float>(std::max(1, mapScale));
                    const double simCx = static_cast<double>(ringX / sScale);
                    const double simCy = static_cast<double>(ringY / sScale);
                    bool inRange = false;
                    for (const auto& rp : rangePreviews) {
                        const double dist = std::hypot(simCx - static_cast<double>(rp.x),
                                                       simCy - static_cast<double>(rp.y));
                        if (dist >= static_cast<double>(rp.deadzonePx) &&
                            dist <= static_cast<double>(rp.rangePx)) {
                            inRange = true;
                            break;
                        }
                    }
                    if (!inRange) {
                        ringR = 235; ringG = 92; ringB = 68;
                    }
                }
            }
        }
    }
    bool facePreview = false;
    float faceTargetX = 0.0f, faceTargetY = 0.0f;
    if (faceArmed && vehicleScene && selection && !selection->selectedVehicles().empty() &&
        SDL_GetMouseFocus() == window && !briefing.active() && !menu.active()) {
        float mx{}, my{};
        SDL_GetMouseState(&mx, &my);
        if (!hud || !hud->opaqueAtWindow(mx, my)) {
            const auto [mapX, mapY] = renderer.mapPosition(mx, my);
            facePreview = true;
            faceTargetX = static_cast<float>(mapX);
            faceTargetY = static_cast<float>(mapY);
        }
    }
    int viewWinW = 1, viewWinH = 1;
    SDL_GetWindowSize(window, &viewWinW, &viewWinH);
    const auto [vx0, vy0] = renderer.mapPosition(0.0f, 0.0f);
    const auto [vx1, vy1] = renderer.mapPosition(static_cast<float>(viewWinW), static_cast<float>(viewWinH));
    const float viewMargin = 64.0f * static_cast<float>(mapScale);
    const float minViewX = static_cast<float>(std::min(vx0, vx1)) - viewMargin;
    const float maxViewX = static_cast<float>(std::max(vx0, vx1)) + viewMargin;
    const float minViewY = static_cast<float>(std::min(vy0, vy1)) - viewMargin;
    const float maxViewY = static_cast<float>(std::max(vy0, vy1)) + viewMargin;
    const auto inViewBounds = [&](float wx, float wy) {
        return wx >= minViewX && wx <= maxViewX && wy >= minViewY && wy <= maxViewY;
    };

    std::vector<ss2::VisibleMine> visibleOnScreenMines;
    for (const auto& vm : liveVisibleMines) {
        const float wx = static_cast<float>(vm.x * mapScale);
        const float wy = static_cast<float>(vm.y * mapScale);
        if (inViewBounds(wx, wy)) {
            visibleOnScreenMines.push_back(vm);
            if (visibleOnScreenMines.size() >= 48u) break;
        }
    }
    struct UnitStatusBadge {
        float x{}, y{};
        bool outOfAmmo{false};
        int stars{0};
    };
    std::vector<UnitStatusBadge> statusBadges;
    if (scene) {
        const float sScale = static_cast<float>(mapScale);
        const auto& units = scene->units();
        for (std::size_t idx = 0; idx < units.size() && statusBadges.size() < 24u; ++idx) {
            const auto& u = units[idx];
            if (u.owner != 0 || u.hp <= 0 || !u.visible) continue;
            const float wx = static_cast<float>(u.x) * sScale;
            const float wy = static_cast<float>(u.y) * sScale;
            if (!inViewBounds(wx, wy)) continue;
            const bool dry = (u.ammo <= 0) && isCombatAmmoSoldierId(u.id);
            const int expVal = (idx < unitExpa.size()) ? unitExpa[idx] : 0;
            const int stars = expVal >= 24000 ? 2 : (expVal >= 12000 ? 1 : 0);
            if (dry || stars > 0) {
                statusBadges.push_back({wx, wy - 34.0f * sScale, dry, stars});
            }
        }
        if (vehicleScene) {
            const auto& vehs = vehicleScene->vehicles();
            for (std::size_t idx = 0; idx < vehs.size() && statusBadges.size() < 24u; ++idx) {
                const auto& v = vehs[idx];
                if (v.owner != 0 || !v.alive || v.hp <= 0 || !v.visible) continue;
                const float wx = static_cast<float>(v.x) * sScale;
                const float wy = static_cast<float>(v.y) * sScale;
                if (!inViewBounds(wx, wy)) continue;
                const bool dry = (v.ammo <= 0) && isArmedVehicleId(v.id);
                const int expVal = (idx < vehExpa.size()) ? vehExpa[idx] : 0;
                const int stars = expVal >= 24000 ? 2 : (expVal >= 12000 ? 1 : 0);
                if (dry || stars > 0) {
                    statusBadges.push_back({wx, wy - 32.0f * sScale, dry, stars});
                }
            }
        }
    }
    if (!activeVfx.empty() || !burningWrecks.empty() || !crippledSmokeVehicles.empty() ||
        !movingSmokeVehicles.empty() || !livePlanes.empty() || !orderLines.empty() ||
        previewRing || facePreview || !rangePreviews.empty() || !visibleOnScreenMines.empty() ||
        !statusBadges.empty() || vfxWasNonEmpty) {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(activeVfx, [&](const ActiveVfx& v) { return now - v.start >= v.duration; });
        std::erase_if(burningWrecks, [&](const BurningWreck& w) { return now >= w.until; });
        std::vector<ss2::MapBeam> beams;
        beams.reserve(visibleOnScreenMines.size() * 2u + statusBadges.size() * 3u + orderLines.size() * 2u +
                      (previewRing ? 18u : 0u) + (facePreview ? 8u : 0u) + rangePreviews.size() * 40u +
                      activeVfx.size() * 3u + burningWrecks.size() * 2u + crippledSmokeVehicles.size() +
                      std::min<std::size_t>(movingSmokeVehicles.size(), 6u));
        const float s = static_cast<float>(mapScale);
        const auto pushIsoEllipse = [&](float cx, float cy, float radiusSimPx, int segments, float lineW,
                                        std::uint8_t r, std::uint8_t g, std::uint8_t b, float alpha) {
            constexpr float kTwoPi = 6.28318530717958647692f;
            const float rx = radiusSimPx * s;
            const float ry = radiusSimPx * 0.5f * s;
            for (int seg = 0; seg < segments; ++seg) {
                const float a0 = (static_cast<float>(seg) / static_cast<float>(segments)) * kTwoPi;
                const float a1 = (static_cast<float>(seg + 1) / static_cast<float>(segments)) * kTwoPi;
                beams.push_back({cx + std::cos(a0) * rx, cy + std::sin(a0) * ry,
                                 cx + std::cos(a1) * rx, cy + std::sin(a1) * ry,
                                 lineW, r, g, b, alpha, alpha});
            }
        };
        for (const auto& rp : rangePreviews) {
            const float vx = static_cast<float>(rp.x) * s;
            const float vy = static_cast<float>(rp.y) * s;
            pushIsoEllipse(vx, vy, rp.rangePx, 24, 1.5f * s, 244, 186, 78, 0.34f);
            if (rp.deadzonePx > 12.0f) {
                pushIsoEllipse(vx, vy, rp.deadzonePx, 16, 1.5f * s, 235, 92, 68, 0.42f);
            }
        }
        for (const auto& vm : visibleOnScreenMines) {
            const float mx = static_cast<float>(vm.x) * s;
            const float my = static_cast<float>(vm.y) * s;
            if (vm.hostile) {
                beams.push_back({mx, my, mx, my, (vm.antiTank ? 8.0f : 6.0f) * s, 235, 92, 68, 0.88f, 0.88f});
                beams.push_back({mx - 5.0f * s, my, mx + 5.0f * s, my, 2.0f * s, 235, 92, 68, 0.85f, 0.85f});
            } else {
                beams.push_back({mx, my, mx, my, (vm.antiTank ? 7.0f : 5.5f) * s, 105, 212, 235, 0.78f, 0.78f});
            }
        }
        for (const auto& ol : orderLines) {
            if (beams.size() >= 112u) break;
            std::uint8_t r = 110, g = 224, b = 120;
            float alpha = 0.45f, width = 1.8f * s, pipW = 4.5f * s;
            switch (ol.kind) {
            case ss2::TacticalOrderLine::Kind::Move:
                break;
            case ss2::TacticalOrderLine::Kind::AttackMove:
            case ss2::TacticalOrderLine::Kind::Attack:
                r = 235; g = 92; b = 68; alpha = 0.55f; width = 2.0f * s; pipW = 5.2f * s;
                break;
            case ss2::TacticalOrderLine::Kind::Patrol:
                r = 105; g = 212; b = 235; alpha = 0.55f; width = 2.0f * s; pipW = 5.0f * s;
                break;
            case ss2::TacticalOrderLine::Kind::Support:
                r = 105; g = 212; b = 235; alpha = 0.58f; width = 2.0f * s; pipW = 5.2f * s;
                break;
            case ss2::TacticalOrderLine::Kind::AttackGround:
                r = 244; g = 186; b = 78; alpha = 0.58f; width = 2.2f * s; pipW = 5.6f * s;
                break;
            }
            const float x0 = static_cast<float>(ol.x0) * s;
            const float y0 = static_cast<float>(ol.y0) * s;
            const float x1 = static_cast<float>(ol.x1) * s;
            const float y1 = static_cast<float>(ol.y1) * s;
            beams.push_back({x0, y0, x1, y1, width, r, g, b, alpha * 0.65f, alpha});
            beams.push_back({x1, y1, x1, y1, pipW, r, g, b, alpha, alpha});
        }
        if (facePreview && vehicleScene && selection) {
            const auto& vehs = vehicleScene->vehicles();
            std::size_t rays = 0;
            for (const size_t vid : selection->selectedVehicles()) {
                if (rays >= 6u) break;
                if (vid >= vehs.size()) continue;
                const auto& v = vehs[vid];
                if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
                const float vx = static_cast<float>(v.x) * s;
                const float vy = static_cast<float>(v.y) * s;
                beams.push_back({vx, vy, faceTargetX, faceTargetY, 2.0f * s, 105, 212, 235, 0.45f, 0.72f});
                ++rays;
            }
            if (rays > 0) {
                beams.push_back({faceTargetX, faceTargetY, faceTargetX, faceTargetY,
                                 6.0f * s, 105, 212, 235, 0.78f, 0.78f});
            }
        }
        if (!statusBadges.empty()) {
            const float badgeAnim = std::chrono::duration<float>(now - animStart).count();
            const float dryAlpha = 0.45f + 0.45f * (0.5f + 0.5f * std::sin(badgeAnim * 6.0f));
            for (const auto& sb : statusBadges) {
                if (beams.size() >= 112u) break;
                if (sb.outOfAmmo) {
                    beams.push_back({sb.x - 5.0f * s, sb.y, sb.x + 5.0f * s, sb.y,
                                     3.2f * s, 244, 186, 78, dryAlpha, dryAlpha});
                }
                const float starY = sb.y - (sb.outOfAmmo ? 6.0f * s : 0.0f);
                if (sb.stars == 1) {
                    beams.push_back({sb.x, starY, sb.x, starY, 4.2f * s, 244, 186, 78, 0.90f, 0.90f});
                } else if (sb.stars >= 2) {
                    beams.push_back({sb.x - 3.5f * s, starY, sb.x - 3.5f * s, starY,
                                     4.2f * s, 244, 186, 78, 0.95f, 0.95f});
                    beams.push_back({sb.x + 3.5f * s, starY, sb.x + 3.5f * s, starY,
                                     4.2f * s, 244, 186, 78, 0.95f, 0.95f});
                }
            }
        }
        if (previewRing) {
            pushIsoEllipse(ringX, ringY, ringRad, 16, 2.0f * s, ringR, ringG, ringB, 0.62f);
            beams.push_back({ringX - 10.0f * s, ringY, ringX + 10.0f * s, ringY,
                             1.6f * s, ringR, ringG, ringB, 0.62f, 0.62f});
            beams.push_back({ringX, ringY - 5.0f * s, ringX, ringY + 5.0f * s,
                             1.6f * s, ringR, ringG, ringB, 0.62f, 0.62f});
        }
        if (combatEffects) {
            // Strictly <= 8 distinct RGB colors so VulkanWindow::maxBeamColours (16) is never exceeded.
            if (vehicleScene && !crippledSmokeVehicles.empty()) {
                const auto& vehs = vehicleScene->vehicles();
                const float animPhase = std::chrono::duration<float>(now - animStart).count();
                for (const std::size_t vi : crippledSmokeVehicles) {
                    if (beams.size() >= 64) break;
                    const auto& v = vehs[vi];
                    const float vx = static_cast<float>(v.x * mapScale);
                    const float vy = static_cast<float>(v.y * mapScale);
                    const float sway = std::sin(animPhase * 2.1f + static_cast<float>(vi) * 1.1f) * 4.0f * s;
                    const float rise = (14.0f + 6.0f * std::sin(animPhase * 3.4f + static_cast<float>(vi))) * s;
                    beams.push_back({vx, vy - 5.0f * s, vx + sway, vy - rise, 7.5f * s, 60, 55, 50, 0.36f, 0.05f});
                }
            }
            if (vehicleScene && !movingSmokeVehicles.empty()) {
                const auto& vehs = vehicleScene->vehicles();
                const float animPhase = std::chrono::duration<float>(now - animStart).count();
                std::size_t drawn = 0;
                for (const std::size_t vi : movingSmokeVehicles) {
                    if (drawn >= 6u || beams.size() >= 64u || vi >= vehs.size()) break;
                    const auto& v = vehs[vi];
                    if (!v.alive || !v.visible) continue;
                    const float vx = static_cast<float>(v.x * mapScale);
                    const float vy = static_cast<float>(v.y * mapScale);
                    const float sway = std::sin(animPhase * 4.2f + static_cast<float>(vi)) * 3.0f * s;
                    const float rise = (8.0f + 4.0f * std::sin(animPhase * 5.1f + static_cast<float>(vi))) * s;
                    beams.push_back({vx, vy - 2.0f * s, vx + sway, vy - rise, 4.5f * s, 60, 55, 50, 0.22f, 0.03f});
                    ++drawn;
                }
            }
            if (!livePlanes.empty()) {
                const float animPhase = std::chrono::duration<float>(now - animStart).count();
                for (const auto& [sid, lp] : livePlanes) {
                    if (beams.size() >= 64u) break;
                    if (!lp.pose.alive || lp.pose.maxHp <= 0 || lp.pose.hp * 2 > lp.pose.maxHp) continue;
                    const auto* pSpec = unitCatalog ? unitCatalog->findPlane(lp.pose.planeId) : nullptr;
                    if (pSpec && !pSpec->hasFallSmoke) continue;
                    const float px = static_cast<float>(lp.pose.x * mapScale);
                    const float py = static_cast<float>(lp.pose.y * mapScale);
                    const float sway = std::sin(animPhase * 3.8f + static_cast<float>(sid)) * 5.0f * s;
                    const float rise = (16.0f + 8.0f * std::sin(animPhase * 4.5f + static_cast<float>(sid))) * s;
                    beams.push_back({px, py, px + sway, py - rise, 7.0f * s, 60, 55, 50, 0.42f, 0.06f});
                }
            }
            for (const auto& bw : burningWrecks) {
                if (beams.size() >= 64) break;
                const float tSec = std::chrono::duration<float>(bw.until - now).count();
                const float sway = std::sin(tSec * 2.5f + bw.x * 0.03f) * 5.0f * s;
                const float rise = (18.0f + 10.0f * std::sin(tSec * 4.0f + bw.y * 0.05f)) * s;
                const float glow = 0.55f + 0.35f * std::sin(tSec * 9.0f);
                beams.push_back({bw.x, bw.y - 6.0f * s, bw.x + sway, bw.y - rise, 10.0f * s, 60, 55, 50, 0.45f, 0.08f});
                beams.push_back({bw.x, bw.y - 4.0f * s, bw.x, bw.y - 4.0f * s, 5.0f * s, 235, 100, 30, glow, glow});
            }
            for (const auto& v : activeVfx) {
                if (beams.size() >= 80) break;
                const float t = std::clamp(
                    std::chrono::duration<float>(now - v.start).count() /
                        std::max(0.01f, static_cast<float>(v.duration.count()) * 0.001f),
                    0.0f, 1.0f);
                switch (v.kind) {
                case ActiveVfx::Kind::Flamer: {
                    const float flick = 1.0f + 0.16f * std::sin(t * 28.0f);
                    const float fade = 1.0f - 0.65f * t;
                    const float reach = std::min(1.0f, t * 2.4f);
                    const float ex = v.x0 + (v.x1 - v.x0) * reach;
                    const float ey = v.y0 + (v.y1 - v.y0) * reach;
                    beams.push_back({v.x0, v.y0, ex, ey, 11.0f * s * flick, 255, 145, 40, 0.90f * fade, 0.25f * fade});
                    beams.push_back({v.x0, v.y0, ex, ey, 5.5f * s * flick, 255, 230, 130, 0.95f * fade, 0.45f * fade});
                    break;
                }
                case ActiveVfx::Kind::Tracer:
                case ActiveVfx::Kind::CannonShell: {
                    const bool heavy = (v.kind == ActiveVfx::Kind::CannonShell);
                    if (t < 0.45f) {
                        const float fa = 1.0f - (t / 0.45f);
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, (heavy ? 12.0f : 8.5f) * s, 255, 230, 130, fa, fa});
                    }
                    const float tail = std::max(0.0f, t - 0.32f);
                    const float bx0 = v.x0 + (v.x1 - v.x0) * tail;
                    const float by0 = v.y0 + (v.y1 - v.y0) * tail;
                    const float bx1 = v.x0 + (v.x1 - v.x0) * t;
                    const float by1 = v.y0 + (v.y1 - v.y0) * t;
                    if (heavy) {
                        beams.push_back({bx0, by0, bx1, by1, 3.6f * s, 255, 145, 40, 0.10f, 1.0f});
                    } else {
                        beams.push_back({bx0, by0, bx1, by1, 2.0f * s, 255, 240, 190, 0.0f, 1.0f});
                    }
                    if (t > 0.78f) {
                        const float ia = (1.0f - t) / 0.22f;
                        beams.push_back({v.x1, v.y1, v.x1, v.y1, (heavy ? 9.0f : 5.5f) * s, 255, 230, 130, ia * 0.85f, ia * 0.85f});
                    }
                    break;
                }
                case ActiveVfx::Kind::Rocket: {
                    const float smokeTail = std::max(0.0f, t - 0.55f);
                    const float sx0 = v.x0 + (v.x1 - v.x0) * smokeTail;
                    const float sy0 = v.y0 + (v.y1 - v.y0) * smokeTail - 20.0f * s * std::sin(smokeTail * 3.14159f);
                    const float arcY = -20.0f * s * std::sin(t * 3.14159f);
                    const float hx = v.x0 + (v.x1 - v.x0) * t;
                    const float hy = v.y0 + (v.y1 - v.y0) * t + arcY;
                    beams.push_back({sx0, sy0, hx, hy, 7.5f * s, 60, 55, 50, 0.08f, 0.52f * (1.0f - 0.35f * t)});
                    const float fTail = std::max(0.0f, t - 0.14f);
                    const float fx0 = v.x0 + (v.x1 - v.x0) * fTail;
                    const float fy0 = v.y0 + (v.y1 - v.y0) * fTail - 20.0f * s * std::sin(fTail * 3.14159f);
                    beams.push_back({fx0, fy0, hx, hy, 4.2f * s, 255, 145, 40, 0.35f, 1.0f});
                    if (t > 0.78f) {
                        const float ia = (1.0f - t) / 0.22f;
                        beams.push_back({v.x1, v.y1, v.x1, v.y1, 14.0f * s, 255, 230, 130, ia * 0.9f, ia * 0.9f});
                    }
                    break;
                }
                case ActiveVfx::Kind::Explosion:
                case ActiveVfx::Kind::VehicleExplosion: {
                    const float sc = (v.kind == ActiveVfx::Kind::VehicleExplosion) ? 1.55f : 1.0f;
                    const float fade = 1.0f - t;
                    const float fireR = (10.0f + 22.0f * std::min(1.0f, t * 2.0f)) * sc * s;
                    beams.push_back({v.x0, v.y0, v.x0, v.y0, fireR, 220, 65, 25, fade * 0.85f, fade * 0.85f});
                    if (t < 0.65f) {
                        const float coreA = (1.0f - t / 0.65f);
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, fireR * 0.55f, 255, 230, 130, coreA * 0.9f, coreA * 0.9f});
                    }
                    beams.push_back({v.x0, v.y0 - 4.0f * s, v.x0, v.y0 - (20.0f + 16.0f * t) * sc * s,
                                     14.0f * sc * s, 60, 55, 50, fade * 0.50f, 0.08f});
                    break;
                }
                case ActiveVfx::Kind::HealPulse: {
                    const float fade = 1.0f - t;
                    beams.push_back({v.x0, v.y0 - 12.0f * t * s, v.x0, v.y0 - (8.0f + 16.0f * t) * s,
                                     7.0f * s, 80, 235, 120, fade * 0.9f, fade * 0.3f});
                    break;
                }
                case ActiveVfx::Kind::RepairPulse: {
                    const float fade = 1.0f - t;
                    beams.push_back({v.x0, v.y0 - 10.0f * t * s, v.x0, v.y0 - (8.0f + 16.0f * t) * s,
                                     8.0f * s, 90, 210, 255, fade * 0.9f, fade * 0.3f});
                    break;
                }
                case ActiveVfx::Kind::Parachute: {
                    const float fade = 1.0f - 0.35f * t;
                    const float sway = std::sin(t * 9.0f) * 4.5f * s * (1.0f - t);
                    const float px = v.x1 + sway;
                    const float py = v.y0 + (v.y1 - v.y0) * t;
                    const float cy = py - 14.0f * s;
                    beams.push_back({v.x1, v.y1, v.x1, v.y1, (10.0f - 4.0f * t) * s, 60, 55, 50,
                                     0.38f * (1.0f - 0.5f * t), 0.38f * (1.0f - 0.5f * t)});
                    beams.push_back({px - 6.0f * s, cy, px + 6.0f * s, cy, 9.0f * s, 255, 240, 190,
                                     0.88f * fade, 0.88f * fade});
                    beams.push_back({px, cy + 2.0f * s, px, py, 1.8f * s, 255, 240, 190,
                                     0.65f * fade, 0.45f * fade});
                    break;
                }
                }
            }
        }
        vfxWasNonEmpty = !beams.empty();
        renderer.setBeams(std::move(beams));
    }
}

} // namespace ss2
