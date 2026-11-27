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
            if (replayFile && liveReplay) {
                hs.objective = "[" + replayTransport.formatBanner(simulation->simTick(), replayFile->endTick) +
                               "] " + ss2::tr(currentObjective);
            } else {
                const unsigned secs = simulation->simTick() / 25u;
                const unsigned mins = secs / 60u;
                const unsigned remS = secs % 60u;
                std::string prefix = "[" + (mins < 10u ? std::string("0") : std::string{}) + std::to_string(mins) +
                                     ":" + (remS < 10u ? std::string("0") : std::string{}) + std::to_string(remS);
                if (paused) {
                    prefix += " PAUSED";
                } else if (!multiplayer.enabled) {
                    if (const int sp = simulation->speedPercent(); sp != 100) {
                        prefix += (sp == 50 ? " 0.5x" : sp == 200 ? " 2x" : sp == 300 ? " 3x" : " 1x");
                    }
                }
                if (multiplayer.enabled) {
                    prefix += " | ";
                    prefix += ss2::tr(mpNationName(multiplayer.playerNations[localOwner() & 3]));
                    if (multiplayer.playerCount > 2) {
                        prefix += " T" + std::to_string(missionPlayerTeams[localOwner() & 3] + 1);
                    }
                    if (mpRttMs >= 0) {
                        prefix += " | " + std::to_string(mpRttMs) + "ms";
                    }
                }
                prefix += "] ";
                hs.objective = prefix + ss2::tr(currentObjective);
            }
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
        syncActiveSubgroup();
        hs.activeSubgroup = activeSubgroupLabel;
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
            if (!v.alive || v.hp <= 0 || v.owner != localOwner()) continue;
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
            if (!lb.destroyed && lb.owner == localOwner() && lb.occupantCount > 0) hs.hasCargo = true;
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
        if (selection->selected().empty() && selection->selectedVehicles().empty() &&
            selectedBuilding && *selectedBuilding < liveBuildings.size()) {
            const auto& lb = liveBuildings[*selectedBuilding];
            if (!lb.destroyed && lb.owner == localOwner() && lb.occupantCount > 0) {
                for (const size_t sid : lb.occupantIds) {
                    if (sid >= units.size()) continue;
                    const auto& u = units[sid];
                    const int maxHp = (sid < initialUnitHp.size() && initialUnitHp[sid] > 0) ? initialUnitHp[sid] : 100;
                    const auto* spec = unitCatalog ? unitCatalog->find(u.id) : nullptr;
                    const int maxInfAmmo = (spec && !spec->isVehicle && spec->ammo1 > 0) ? spec->ammo1 : 40;
                    auto& a = byClass[hudDisplayName(u.id)];
                    ++a.count;
                    a.hpSum += std::clamp(static_cast<float>(std::max(1, u.hp)) / static_cast<float>(maxHp), 0.0f, 1.0f);
                    a.ammoSum += std::clamp(static_cast<float>(u.ammo) / static_cast<float>(maxInfAmmo), 0.0f, 1.0f);
                }
            }
        }
        for (const auto& [label, a] : byClass) {
            const float avgHp = std::round((a.hpSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
            const float avgAmmo = std::round((a.ammoSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
            hs.selection.push_back({label, a.count, avgHp, avgAmmo});
        }
        if (selection->selected().empty() && selection->selectedVehicles().size() == 1u && simulation) {
            const size_t vid = *selection->selectedVehicles().begin();
            std::map<std::string, Agg> cargoByClass;
            for (const size_t sid : simulation->vehiclePassengerIds(vid)) {
                if (sid >= units.size()) continue;
                const auto& u = units[sid];
                const int maxHp = (sid < initialUnitHp.size() && initialUnitHp[sid] > 0) ? initialUnitHp[sid] : 100;
                auto& a = cargoByClass["[Cargo] " + hudDisplayName(u.id)];
                ++a.count;
                a.hpSum += std::clamp(static_cast<float>(std::max(1, u.hp)) / static_cast<float>(maxHp), 0.0f, 1.0f);
                a.ammoSum += 1.0f;
            }
            for (const auto& [label, a] : cargoByClass) {
                const float avgHp = std::round((a.hpSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
                hs.selection.push_back({label, a.count, avgHp, 1.0f});
            }
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
            hs.dots.push_back({du, dv, isHostileOwner(u.owner), isAllyOwner(u.owner) && u.owner != localOwner(),
                               !multiplayer.enabled && u.owner == 3});
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
            hs.dots.push_back({du, dv, isHostileOwner(v.owner), isAllyOwner(v.owner) && v.owner != localOwner(),
                               (!multiplayer.enabled || multiplayer.playerCount < 4) && v.owner == 3,
                               ss2::HudDotKind::Armor});
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
            if (b.owner != localOwner() && !isAllyOwner(b.owner) && !isBuildingPointVisible(b.x, b.y)) continue;
            if (b.occupantCount > 0 && b.owner >= 0) {
                const float du = std::round(std::clamp(static_cast<float>(b.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
                const float dv = std::round(std::clamp(static_cast<float>(b.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
                hs.dots.push_back({du, dv, isHostileOwner(b.owner), isAllyOwner(b.owner) && b.owner != localOwner(),
                                   false, ss2::HudDotKind::Structure});
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
        for (std::size_t bri = 0; bri < liveBridges.size(); ++bri) {
            const auto& br = liveBridges[bri];
            if (br.hp >= br.maxHp) continue;
            if (!isBuildingPointVisible(br.x, br.y)) continue;
            const float barW = static_cast<float>(std::clamp(br.width / 2, 28, 60) * mapScale);
            const float barY = static_cast<float>((br.top - 5) * mapScale);
            unitBars.push_back({
                static_cast<float>(br.x * mapScale),
                barY,
                barW,
                std::clamp(static_cast<float>(br.hp) / static_cast<float>(std::max(1, br.maxHp)), 0.0f, 1.0f)
            });
        }
        std::vector<ss2::ZeppelinMarkerStatus> mpZeppelins;
        if (multiplayer.enabled && simulation) {
            mpZeppelins = simulation->zeppelinStatuses();
            const int myTeam = (localOwner() >= 0 && localOwner() < 4) ? missionPlayerTeams[localOwner()] : 0;
            for (const auto& z : mpZeppelins) {
                const float du = std::round(std::clamp(static_cast<float>(z.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
                const float dv = std::round(std::clamp(static_cast<float>(z.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
                const bool heldByMe = (z.holderTeam >= 0 && z.holderTeam == myTeam);
                const bool heldByEnemy = (z.holderTeam >= 0 && z.holderTeam != myTeam);
                hs.dots.push_back({du, dv, heldByEnemy, false, !heldByMe && !heldByEnemy, ss2::HudDotKind::Zeppelin});
                if (z.captureProgress > 0.0f && z.capturingTeam >= 0) {
                    hs.pings.push_back({du, dv, z.capturingTeam != myTeam, z.captureProgress});
                }
            }
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
        const unsigned effHudH = (hudHidden || !hud) ? 0u : hud->height();
        const double mapViewH = ss2::hidpi::logicalMapViewHeight(winH, pixelH, static_cast<int>(effHudH));
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
        if (simulation) {
            hud->setWeather(simulation->data().weather, simulation->simTick());
        }
        hud->update(hs);
        if (hudHidden) {
            renderer.clearOverlay(OverlaySlot::hud);
            renderer.setHudBarHeight(0);
        } else {
            hud->draw(renderer);
        }
        {
            const auto wNow = std::chrono::steady_clock::now();
            const float wtSec = paused
                ? std::chrono::duration<float>(lastNoticeFrame - animStart).count()
                : std::chrono::duration<float>(wNow - animStart).count();
            const float lightElapsed = std::chrono::duration<float>(wNow - lastWeatherLightningAt).count();
            const float flash = (missionWeather == assets::WeatherKind::Rain && lightElapsed >= 0.0f && lightElapsed < 0.35f)
                ? (1.0f - lightElapsed / 0.35f)
                : 0.0f;
            renderer.setWeatherEffect(static_cast<int>(missionWeather), wtSec * 25.0f, flash);
        }
    }
    std::vector<std::size_t> lightDamageSmokeVehicles;
    std::vector<std::size_t> heavyDamageSmokeVehicles;
    if (combatEffects && vehicleScene && unitCatalog) {
        const auto& vehs = vehicleScene->vehicles();
        for (std::size_t vi = 0; vi < vehs.size(); ++vi) {
            const auto& v = vehs[vi];
            if (!v.alive || v.hp <= 0 || !v.visible) continue;
            const auto* spec = unitCatalog->find(v.id);
            if (spec && spec->isSupplyCrate()) continue;
            const int maxHp = (vi < initialVehicleHp.size() && initialVehicleHp[vi] > 0) ? initialVehicleHp[vi] : 100;
            const bool moveImp = (vi < vehicleMoveImpaired.size() && vehicleMoveImpaired[vi]) ||
                                 (spec && spec->moveDamageHp > 0 && v.hp < spec->moveDamageHp);
            if (static_cast<std::int64_t>(v.hp) * 4 < maxHp || moveImp) {
                if (heavyDamageSmokeVehicles.size() < 12u) heavyDamageSmokeVehicles.push_back(vi);
            } else if (static_cast<std::int64_t>(v.hp) * 2 < maxHp) {
                if (lightDamageSmokeVehicles.size() < 12u) lightDamageSmokeVehicles.push_back(vi);
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
    if (simulation && selection &&
        (!selection->selectedVehicles().empty() || !selection->selected().empty())) {
        const auto& vsel = selection->selectedVehicles();
        const auto& sel = selection->selected();
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        const std::vector<size_t> sids(sel.begin(), sel.end());
        rangePreviews = simulation->selectionRangePreviews(vids, sids);
        if (rangePreviews.size() > 4u) rangePreviews.resize(4u);
    }
    const bool altHeld = (SDL_GetModState() & SDL_KMOD_ALT) != 0 || tacticalOverlayPinned;
    struct SupportAuraRing {
        float x{0.0f}, y{0.0f}, radiusPx{0.0f}, outerRadiusPx{0.0f};
        std::uint8_t r{244}, g{198}, b{92};
    };
    std::vector<SupportAuraRing> supportAuras;
    if (selection) {
        if (vehicleScene && unitCatalog) {
            const auto& vehs = vehicleScene->vehicles();
            for (const size_t vid : selection->selectedVehicles()) {
                if (supportAuras.size() >= 6u) break;
                if (vid >= vehs.size()) continue;
                const auto& v = vehs[vid];
                if (!v.alive || v.hp <= 0 || !v.visible || v.owner != localOwner()) continue;
                const auto* vSpec = unitCatalog->find(v.id);
                if (!vSpec) continue;
                if ((vSpec->isSupplyTruck() && !vSpec->isSupplyCrate()) || vSpec->isAmbulance()) {
                    const float outer = vehicleHoldFireUnits.contains(vid) ? 0.0f : 280.0f;
                    supportAuras.push_back({static_cast<float>(v.x), static_cast<float>(v.y),
                                            140.0f, outer, 72, 224, 180});
                } else if (vSpec->isPropaganda()) {
                    supportAuras.push_back({static_cast<float>(v.x), static_cast<float>(v.y),
                                            200.0f, 0.0f, 244, 198, 92});
                }
            }
        }
        if (scene) {
            const auto& units = scene->units();
            for (const size_t sid : selection->selected()) {
                if (supportAuras.size() >= 6u) break;
                if (sid >= units.size()) continue;
                const auto& u = units[sid];
                if (u.hp <= 0 || !u.visible || u.owner != localOwner()) continue;
                std::string lowId = u.id;
                for (char& ch : lowId) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const auto* uSpec = unitCatalog ? unitCatalog->find(u.id) : nullptr;
                if (lowId.find("officer") != std::string::npos ||
                    lowId.find("general") != std::string::npos ||
                    (uSpec && uSpec->isOfficerInfantry())) {
                    const bool isGen = lowId.find("general") != std::string::npos ||
                                       (uSpec && uSpec->selType == "general");
                    const float rPx = static_cast<float>(
                        (uSpec && uSpec->officerRadius > 0) ? uSpec->officerRadius : (isGen ? 350 : 160));
                    supportAuras.push_back({static_cast<float>(u.x), static_cast<float>(u.y),
                                            rPx, 0.0f, 244, 198, 92});
                } else if (lowId.find("medic") != std::string::npos ||
                           (uSpec && uSpec->isMedicInfantry())) {
                    supportAuras.push_back({static_cast<float>(u.x), static_cast<float>(u.y),
                                            96.0f, 0.0f, 88, 226, 132});
                }
            }
        }
    }
    struct ThreatRingPreview {
        bool active{false};
        float x{0.0f}, y{0.0f}, rangePx{0.0f}, deadzonePx{0.0f};
    } threatRing;
    if (altHeld && selection && SDL_GetMouseFocus() == window && !briefing.active() && !menu.active()) {
        float mx{}, my{};
        SDL_GetMouseState(&mx, &my);
        if (!hud || hudHidden || !hud->opaqueAtWindow(mx, my)) {
            const auto [mapX, mapY] = renderer.mapPosition(mx, my);
            if (vehicleScene) {
                if (const auto vHov = selection->vehicleAt(int(std::floor(mapX)), int(std::floor(mapY)))) {
                    const auto& tv = vehicleScene->vehicles().at(*vHov);
                    if (tv.alive && tv.hp > 0 && tv.visible && isHostileOwner(tv.owner)) {
                        const auto* vSpec = unitCatalog ? unitCatalog->find(tv.id) : nullptr;
                        float rPx = 0.0f;
                        float dzPx = 0.0f;
                        if (vSpec) {
                            if (vSpec->shot1.damage > 0 && vSpec->shot1.range > 0) {
                                rPx = static_cast<float>(vSpec->isKamikaze()
                                    ? std::clamp(static_cast<double>(vSpec->shot1.range), 48.0, 96.0)
                                    : std::clamp(vSpec->shot1.range * 0.65, 140.0, 560.0));
                                dzPx = static_cast<float>(std::max(0.0, vSpec->shot1.deadzone * 0.5));
                            } else if (vSpec->shot2.damage > 0 && vSpec->shot2.range > 0) {
                                rPx = static_cast<float>(std::clamp(vSpec->shot2.range * 0.65, 140.0, 520.0));
                            }
                        } else {
                            rPx = 260.0f;
                        }
                        if (rPx > 12.0f) {
                            threatRing = {true, static_cast<float>(tv.x), static_cast<float>(tv.y), rPx, dzPx};
                        }
                    }
                }
            }
            if (!threatRing.active && scene) {
                if (const auto iHov = selection->infantryAt(int(std::floor(mapX)), int(std::floor(mapY)))) {
                    const auto& tu = scene->units().at(*iHov);
                    if (tu.hp > 0 && tu.visible && isHostileOwner(tu.owner)) {
                        const auto* uSpec = unitCatalog ? unitCatalog->find(tu.id) : nullptr;
                        if (uSpec && uSpec->shot1.damage > 0 && uSpec->shot1.range > 0) {
                            const float rPx = static_cast<float>(uSpec->shot1.range) *
                                              (settings.realismDoctrine ? 1.35f : 1.0f);
                            const float dzPx = static_cast<float>(std::max(0, uSpec->shot1.deadzone));
                            if (rPx > 12.0f) {
                                threatRing = {true, static_cast<float>(tu.x), static_cast<float>(tu.y), rPx, dzPx};
                            }
                        }
                    }
                }
            }
            if (!threatRing.active && !liveBuildings.empty()) {
                const int simX = int(std::floor(mapX / mapScale));
                const int simY = int(std::floor(mapY / mapScale));
                if (const auto bHov = buildingAtMap(simX, simY)) {
                    const auto& tb = liveBuildings[*bHov];
                    if (!tb.destroyed && tb.owner >= 0 && isHostileOwner(tb.owner) &&
                        tb.occupantCount > 0 && isBuildingPointVisible(tb.x, tb.y)) {
                        const int garBonus = unitCatalog
                            ? std::clamp(unitCatalog->buildingDefense().bonusShotRange / 3, 32, 64)
                            : 48;
                        threatRing = {true, static_cast<float>(tb.x), static_cast<float>(tb.y),
                                      static_cast<float>(200 + garBonus), 0.0f};
                    }
                }
            }
        }
    }
    bool previewRing = false;
    float ringX = 0.0f, ringY = 0.0f, ringRad = 0.0f;
    std::uint8_t ringR = 244, ringG = 186, ringB = 78;
    if ((attackGroundArmed || smokeArmed || armedAirSupport.has_value()) && SDL_GetMouseFocus() == window &&
        !briefing.active() && !menu.active()) {
        float mx{}, my{};
        SDL_GetMouseState(&mx, &my);
        if (!hud || hudHidden || !hud->opaqueAtWindow(mx, my)) {
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
            } else if (smokeArmed) {
                ringRad = 72.0f; ringR = 200; ringG = 216; ringB = 228;
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
        if (!hud || hudHidden || !hud->opaqueAtWindow(mx, my)) {
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
            if (u.owner != localOwner() || u.hp <= 0 || !u.visible) continue;
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
                if (v.owner != localOwner() || !v.alive || v.hp <= 0 || !v.visible) continue;
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
    const std::vector<ss2::ZeppelinMarkerStatus> beamZeppelins =
        (multiplayer.enabled && simulation) ? simulation->zeppelinStatuses() : std::vector<ss2::ZeppelinMarkerStatus>{};
    const bool hasAirTrailBeams = combatEffects && hud && hud->hasActiveAirTrails();
    const bool hasSelectedUnits = selection && (!selection->selected().empty() || !selection->selectedVehicles().empty());
    const bool hasSelectionRings = enhancedGraphics && hasSelectedUnits;
    const bool hasSelectedBuilding = !hasSelectedUnits && selectedBuilding.has_value() &&
                                     *selectedBuilding < liveBuildings.size() &&
                                     !liveBuildings[*selectedBuilding].destroyed &&
                                     liveBuildings[*selectedBuilding].owner == localOwner() &&
                                     liveBuildings[*selectedBuilding].occupantCount > 0;
    const float rightDragDistPx = rightDragActive
        ? std::hypot(rightDragCurScreenX - rightDragStartScreenX, rightDragCurScreenY - rightDragStartScreenY)
        : 0.0f;
    const bool showRightDragPreview = rightDragActive && rightDragDistPx >= 28.0f && hasSelectedUnits;
    const bool showAltSalvage = altHeld && vehicleScene && simulation;
    if (!activeVfx.empty() || !orderPulses.empty() || !burningWrecks.empty() ||
        !lightDamageSmokeVehicles.empty() || !heavyDamageSmokeVehicles.empty() ||
        !movingSmokeVehicles.empty() || !movingWaterVehicles.empty() || !livePlanes.empty() ||
        !liveParachutes.empty() || hasAirTrailBeams || !orderLines.empty() ||
        previewRing || facePreview || showRightDragPreview || !rangePreviews.empty() ||
        !supportAuras.empty() || threatRing.active ||
        objectiveMarkerSim.has_value() || !visibleOnScreenMines.empty() ||
        !statusBadges.empty() || !beamZeppelins.empty() || !activeBeacons.empty() ||
        hasSelectedUnits || hasSelectionRings || hasSelectedBuilding || showAltSalvage || vfxWasNonEmpty) {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(activeVfx, [&](const ActiveVfx& v) { return now - v.start >= v.duration; });
        std::erase_if(orderPulses, [&](const OrderPulse& op) {
            return now - op.start >= std::chrono::milliseconds(600);
        });
        std::erase_if(burningWrecks, [&](const BurningWreck& w) { return now >= w.until; });
        std::erase_if(activeBeacons, [&](const ActiveBeacon& b) {
            return now - b.start >= std::chrono::milliseconds(4500);
        });
        std::vector<ss2::MapBeam> beams;
        beams.reserve(visibleOnScreenMines.size() * 2u + statusBadges.size() * 3u + orderLines.size() * 2u +
                      orderPulses.size() * 24u + (previewRing ? 18u : 0u) + (facePreview ? 8u : 0u) +
                      rangePreviews.size() * 64u + (threatRing.active ? 40u : 0u) +
                      (objectiveMarkerSim ? 20u : 0u) + (hasSelectionRings ? 256u : 0u) +
                      activeVfx.size() * 6u + burningWrecks.size() * 3u +
                      lightDamageSmokeVehicles.size() + heavyDamageSmokeVehicles.size() * 2u +
                      std::min<std::size_t>(movingSmokeVehicles.size(), 6u));
        const float s = static_cast<float>(mapScale);
        const auto pushIsoEllipse = [&](float cx, float cy, float radiusSimPx, int segments, float lineW,
                                        std::uint8_t r, std::uint8_t g, std::uint8_t b, float alpha,
                                        bool additiveGlow = false, bool underSprites = false) {
            constexpr float kTwoPi = 6.28318530717958647692f;
            const float rx = radiusSimPx * s;
            const float ry = radiusSimPx * 0.5f * s;
            for (int seg = 0; seg < segments; ++seg) {
                const float a0 = (static_cast<float>(seg) / static_cast<float>(segments)) * kTwoPi;
                const float a1 = (static_cast<float>(seg + 1) / static_cast<float>(segments)) * kTwoPi;
                beams.push_back({cx + std::cos(a0) * rx, cy + std::sin(a0) * ry,
                                 cx + std::cos(a1) * rx, cy + std::sin(a1) * ry,
                                 lineW, r, g, b, alpha, alpha, additiveGlow, underSprites});
            }
        };
        if (hasSelectionRings) {
            std::size_t ringUnits = 0;
            if (scene) {
                const auto& units = scene->units();
                for (const size_t sid : selection->selected()) {
                    if (ringUnits >= 96u) break;
                    if (sid >= units.size()) continue;
                    const auto& u = units[sid];
                    if (u.hp <= 0 || !u.visible) continue;
                    const float ux = static_cast<float>(u.x) * s;
                    const float uy = static_cast<float>(u.y) * s;
                    if (!inViewBounds(ux, uy)) continue;
                    const bool bino = binocularUnits.contains(sid);
                    pushIsoEllipse(ux, uy, 11.0f, 12, 1.5f * s,
                                   bino ? 244 : 110, bino ? 186 : 224, bino ? 78 : 120, 0.48f, false, true);
                    ++ringUnits;
                }
            }
            if (vehicleScene) {
                const auto& vehs = vehicleScene->vehicles();
                for (const size_t vid : selection->selectedVehicles()) {
                    if (ringUnits >= 96u) break;
                    if (vid >= vehs.size()) continue;
                    const auto& v = vehs[vid];
                    if (!v.alive || v.hp <= 0 || !v.visible) continue;
                    const float vx = static_cast<float>(v.x) * s;
                    const float vy = static_cast<float>(v.y) * s;
                    if (!inViewBounds(vx, vy)) continue;
                    pushIsoEllipse(vx, vy, 22.0f, 16, 1.8f * s, 110, 224, 120, 0.52f, false, true);
                    ++ringUnits;
                }
            }
        }
        if (hasSelectedBuilding) {
            const auto& lb = liveBuildings[*selectedBuilding];
            const float bx = static_cast<float>(lb.x) * s;
            const float by = static_cast<float>(lb.y) * s;
            const float rad = std::clamp(static_cast<float>(std::max(lb.width, lb.height)) * 0.45f, 22.0f, 52.0f);
            pushIsoEllipse(bx, by, rad, 20, 2.0f * s, 110, 224, 120, 0.62f, false, true);
            if (altHeld && beams.size() + 24u < 768u) {
                const int garBonus = unitCatalog
                    ? std::clamp(unitCatalog->buildingDefense().bonusShotRange / 3, 32, 64)
                    : 48;
                pushIsoEllipse(bx, by, static_cast<float>(200 + garBonus), 24, 1.5f * s, 244, 186, 78, 0.36f);
            }
        }
        if (hasSelectedUnits) {
            const std::uint32_t curTick = simulation ? simulation->simTick() : 0u;
            if (scene) {
                const auto& units = scene->units();
                for (const size_t sid : selection->selected()) {
                    if (beams.size() >= 768u) break;
                    if (sid >= units.size()) continue;
                    const auto& u = units[sid];
                    if (u.hp <= 0 || !u.visible) continue;
                    const float ux = static_cast<float>(u.x) * s;
                    const float uy = static_cast<float>(u.y) * s;
                    if (!inViewBounds(ux, uy)) continue;
                    const float barY = uy - (crouchedUnits.contains(sid) ? 24.0f : 30.0f) * s;
                    if (selection->controlGroupOfInfantry(sid).has_value() && beams.size() < 768u) {
                        beams.push_back({ux - 10.5f * s, barY + 1.5f * s,
                                         ux - 10.5f * s, barY + 1.5f * s,
                                         3.8f * s, 244, 186, 78, 0.92f, 0.92f});
                    }
                    if (simulation && beams.size() < 768u) {
                        if (const auto rit = unitReloadWindow.find(sid);
                            rit != unitReloadWindow.end() && rit->second.durationTicks > 0u &&
                            curTick >= rit->second.startTick &&
                            curTick < rit->second.startTick + rit->second.durationTicks) {
                            const float rem = static_cast<float>(
                                rit->second.startTick + rit->second.durationTicks - curTick);
                            const float frac = std::clamp(rem / static_cast<float>(rit->second.durationTicks), 0.05f, 1.0f);
                            const float ry = barY + 4.0f * s;
                            beams.push_back({ux - 7.0f * s, ry, ux - 7.0f * s + 14.0f * s * frac, ry,
                                             1.8f * s, 105, 212, 235, 0.90f, 0.90f});
                        }
                    }
                }
            }
            if (vehicleScene) {
                const auto& vehs = vehicleScene->vehicles();
                for (const size_t vid : selection->selectedVehicles()) {
                    if (beams.size() >= 768u) break;
                    if (vid >= vehs.size()) continue;
                    const auto& v = vehs[vid];
                    if (!v.alive || v.hp <= 0 || !v.visible) continue;
                    const float vx = static_cast<float>(v.x) * s;
                    const float vy = static_cast<float>(v.y) * s;
                    if (!inViewBounds(vx, vy)) continue;
                    const float barY = static_cast<float>(v.height > 0 ? v.top - 5 : v.y - 26) * s;
                    if (selection->controlGroupOfVehicle(vid).has_value() && beams.size() < 768u) {
                        beams.push_back({vx - 17.0f * s, barY + 1.5f * s,
                                         vx - 17.0f * s, barY + 1.5f * s,
                                         4.2f * s, 244, 186, 78, 0.92f, 0.92f});
                    }
                    if (simulation && beams.size() < 768u) {
                        if (const auto rit = vehReloadWindow.find(vid);
                            rit != vehReloadWindow.end() && rit->second.durationTicks > 0u &&
                            curTick >= rit->second.startTick &&
                            curTick < rit->second.startTick + rit->second.durationTicks) {
                            const float rem = static_cast<float>(
                                rit->second.startTick + rit->second.durationTicks - curTick);
                            const float frac = std::clamp(rem / static_cast<float>(rit->second.durationTicks), 0.05f, 1.0f);
                            const float ry = barY + 4.2f * s;
                            beams.push_back({vx - 12.0f * s, ry, vx - 12.0f * s + 24.0f * s * frac, ry,
                                             2.0f * s, 105, 212, 235, 0.90f, 0.90f});
                        }
                    }
                }
            }
        }
        for (const auto& op : orderPulses) {
            if (beams.size() >= 768u) break;
            const float t = std::clamp(std::chrono::duration<float>(now - op.start).count() / 0.60f, 0.0f, 1.0f);
            const float fade = 1.0f - t;
            std::uint8_t r = 110, g = 224, b = 120;
            if (op.kind == OrderPulse::Kind::Support) { r = 244; g = 186; b = 78; }
            else if (op.kind == OrderPulse::Kind::Attack) { r = 235; g = 92; b = 68; }
            const float dx = static_cast<float>(op.dstSimX) * s;
            const float dy = static_cast<float>(op.dstSimY) * s;
            if (beams.size() + 12u < 768u) {
                pushIsoEllipse(dx, dy, 8.0f + 22.0f * t, 12, 1.8f * s, r, g, b, 0.78f * fade);
            }
            if (beams.size() + 10u < 768u) {
                pushIsoEllipse(dx, dy, 3.0f + 12.0f * t, 10, 1.4f * s, r, g, b, 0.55f * fade);
            }
            if (op.hasSource && beams.size() < 768u) {
                beams.push_back({static_cast<float>(op.srcSimX) * s, static_cast<float>(op.srcSimY) * s,
                                 dx, dy, 1.4f * s, r, g, b, 0.08f * fade, 0.38f * fade});
            }
        }
        {
            const bool anySelBino = selection && std::ranges::any_of(selection->selected(), [&](size_t id) {
                return binocularUnits.contains(id);
            });
            const bool showSightRings = altHeld || anySelBino;
            const float rangeAlpha = attackGroundArmed ? 0.58f : 0.28f;
            const float deadzoneAlpha = attackGroundArmed ? 0.55f : 0.34f;
            for (const auto& rp : rangePreviews) {
                if (beams.size() + 24u >= 768u) break;
                const float vx = static_cast<float>(rp.x) * s;
                const float vy = static_cast<float>(rp.y) * s;
                pushIsoEllipse(vx, vy, rp.rangePx, 24, 1.5f * s, 244, 186, 78, rangeAlpha);
                if (rp.deadzonePx > 12.0f && beams.size() + 16u < 768u) {
                    pushIsoEllipse(vx, vy, rp.deadzonePx, 16, 1.5f * s, 235, 92, 68, deadzoneAlpha);
                }
                if (showSightRings && rp.sightPx > 12.0f && beams.size() + 24u < 768u) {
                    pushIsoEllipse(vx, vy, rp.sightPx, 24, 1.4f * s, 105, 212, 235, 0.34f);
                }
            }
        }
        for (const auto& aura : supportAuras) {
            if (beams.size() + 24u >= 768u) break;
            const float ax = aura.x * s;
            const float ay = aura.y * s;
            pushIsoEllipse(ax, ay, aura.radiusPx, 24, 1.6f * s, aura.r, aura.g, aura.b, 0.36f, false, true);
            if (altHeld && aura.outerRadiusPx > aura.radiusPx && beams.size() + 24u < 768u) {
                pushIsoEllipse(ax, ay, aura.outerRadiusPx, 24, 1.3f * s, 244, 186, 78, 0.24f, false, true);
            }
        }
        if (threatRing.active && beams.size() + 24u < 768u) {
            const float tx = threatRing.x * s;
            const float ty = threatRing.y * s;
            pushIsoEllipse(tx, ty, threatRing.rangePx, 24, 1.8f * s, 235, 78, 68, 0.56f);
            if (threatRing.deadzonePx > 12.0f && beams.size() + 16u < 768u) {
                pushIsoEllipse(tx, ty, threatRing.deadzonePx, 16, 1.4f * s, 235, 78, 68, 0.42f);
            }
        }
        if (objectiveMarkerSim && beams.size() + 18u < 768u) {
            const float objX = static_cast<float>(objectiveMarkerSim->first) * s;
            const float objY = static_cast<float>(objectiveMarkerSim->second) * s;
            const float animT = std::chrono::duration<float>(now - animStart).count();
            const float pulse = 0.5f + 0.5f * std::sin(animT * 4.2f);
            int objWinH = viewWinH;
            int objPixelH = viewWinH;
            (void)SDL_GetWindowSizeInPixels(window, nullptr, &objPixelH);
            const double mapViewH = hud
                ? ss2::hidpi::logicalMapViewHeight(viewWinH, objPixelH, static_cast<int>(hud->height()))
                : static_cast<double>(viewWinH);
            const auto [cx0, cy0] = renderer.mapPosition(0.0f, 0.0f);
            const auto [cx1, cy1] = renderer.mapPosition(static_cast<float>(viewWinW), static_cast<float>(mapViewH));
            const float vpMinX = static_cast<float>(std::min(cx0, cx1));
            const float vpMaxX = static_cast<float>(std::max(cx0, cx1));
            const float vpMinY = static_cast<float>(std::min(cy0, cy1));
            const float vpMaxY = static_cast<float>(std::max(cy0, cy1));
            if (objX >= vpMinX && objX <= vpMaxX && objY >= vpMinY && objY <= vpMaxY) {
                pushIsoEllipse(objX, objY, 18.0f + 10.0f * pulse, 16, 2.0f * s, 244, 186, 78, 0.50f + 0.30f * pulse);
                const float tipY = objY - (16.0f + 5.0f * pulse) * s;
                beams.push_back({objX - 7.5f * s, tipY - 7.5f * s, objX, tipY, 2.2f * s, 244, 186, 78, 0.88f, 0.88f});
                beams.push_back({objX + 7.5f * s, tipY - 7.5f * s, objX, tipY, 2.2f * s, 244, 186, 78, 0.88f, 0.88f});
            } else {
                const float vpCx = 0.5f * (vpMinX + vpMaxX);
                const float vpCy = 0.5f * (vpMinY + vpMaxY);
                const float dx = objX - vpCx;
                const float dy = objY - vpCy;
                const float len = std::hypot(dx, dy);
                if (len > 1.0f) {
                    const float padX = std::max(14.0f * s, 0.035f * (vpMaxX - vpMinX));
                    const float padY = std::max(14.0f * s, 0.055f * (vpMaxY - vpMinY));
                    const float halfX = std::max(10.0f, 0.5f * (vpMaxX - vpMinX) - padX);
                    const float halfY = std::max(10.0f, 0.5f * (vpMaxY - vpMinY) - padY);
                    const float tx = (std::abs(dx) > 1e-4f) ? (halfX / std::abs(dx)) : 1e9f;
                    const float ty = (std::abs(dy) > 1e-4f) ? (halfY / std::abs(dy)) : 1e9f;
                    const float tEdge = std::min(tx, ty);
                    const float tipX = vpCx + dx * tEdge;
                    const float tipY = vpCy + dy * tEdge;
                    const float nx = dx / len;
                    const float ny = dy / len;
                    const float px = -ny;
                    const float py = nx;
                    const float chev = std::clamp(0.015f * (vpMaxX - vpMinX), 8.0f * s, 18.0f * s);
                    const float alpha = 0.55f + 0.25f * pulse;
                    beams.push_back({tipX - nx * chev + px * chev * 0.65f, tipY - ny * chev + py * chev * 0.65f,
                                     tipX, tipY, 2.2f * s, 244, 186, 78, alpha, alpha});
                    beams.push_back({tipX - nx * chev - px * chev * 0.65f, tipY - ny * chev - py * chev * 0.65f,
                                     tipX, tipY, 2.2f * s, 244, 186, 78, alpha, alpha});
                }
            }
            (void)objWinH;
        }
        for (const auto& vm : visibleOnScreenMines) {
            if (beams.size() + 2u >= 768u) break;
            const float mx = static_cast<float>(vm.x) * s;
            const float my = static_cast<float>(vm.y) * s;
            if (vm.hostile) {
                beams.push_back({mx, my, mx, my, (vm.antiTank ? 8.0f : 6.0f) * s, 235, 92, 68, 0.88f, 0.88f});
                beams.push_back({mx - 5.0f * s, my, mx + 5.0f * s, my, 2.0f * s, 235, 92, 68, 0.85f, 0.85f});
            } else {
                beams.push_back({mx, my, mx, my, (vm.antiTank ? 7.0f : 5.5f) * s, 105, 212, 235, 0.78f, 0.78f});
            }
        }
        {
            const int myTeam = (localOwner() >= 0 && localOwner() < 4) ? missionPlayerTeams[localOwner()] : 0;
            for (const auto& z : beamZeppelins) {
                if (beams.size() >= 768u) break;
                const float zx = static_cast<float>(z.x) * s;
                const float zy = static_cast<float>(z.y) * s;
                const bool heldByMe = (z.holderTeam >= 0 && z.holderTeam == myTeam);
                const bool heldByEnemy = (z.holderTeam >= 0 && z.holderTeam != myTeam);
                const std::uint8_t zr = heldByEnemy ? 235 : (heldByMe ? 110 : 244);
                const std::uint8_t zg = heldByEnemy ? 92  : (heldByMe ? 224 : 186);
                const std::uint8_t zb = heldByEnemy ? 68  : (heldByMe ? 120 : 78);
                pushIsoEllipse(zx, zy, 128.0f, 16, 1.8f * s, zr, zg, zb, 0.45f);
                beams.push_back({zx, zy, zx, zy - 26.0f * s, 2.2f * s, zr, zg, zb, 0.90f, 0.90f});
                beams.push_back({zx, zy - 26.0f * s, zx + 12.0f * s, zy - 22.0f * s,
                                 3.5f * s, zr, zg, zb, 0.92f, 0.92f});
                if (z.captureProgress > 0.0f && z.capturingTeam >= 0) {
                    const bool capMine = (z.capturingTeam == myTeam);
                    const std::uint8_t cr = capMine ? 110 : 235;
                    const std::uint8_t cg = capMine ? 224 : 92;
                    const std::uint8_t cb = capMine ? 120 : 68;
                    const float fillW = 28.0f * s * std::clamp(z.captureProgress, 0.0f, 1.0f);
                    beams.push_back({zx - 14.0f * s, zy - 32.0f * s,
                                     zx - 14.0f * s + fillW, zy - 32.0f * s,
                                     3.0f * s, cr, cg, cb, 0.95f, 0.95f});
                }
            }
            for (const auto& ab : activeBeacons) {
                if (beams.size() >= 768u) break;
                const float bx = static_cast<float>(ab.x) * s;
                const float by = static_cast<float>(ab.y) * s;
                const float bt = std::clamp(
                    std::chrono::duration<float>(now - ab.start).count() / 4.5f, 0.0f, 1.0f);
                const float pulse = 28.0f + 46.0f * std::abs(std::sin(bt * 9.424778f));
                const float alpha = 0.85f * (1.0f - 0.55f * bt);
                pushIsoEllipse(bx, by, pulse, 16, 2.0f * s, 105, 212, 235, alpha);
                beams.push_back({bx, by, bx, by - (34.0f - 10.0f * bt) * s,
                                 2.6f * s, 244, 186, 78, alpha, alpha * 0.65f});
            }
        }
        for (const auto& ol : orderLines) {
            if (beams.size() >= 768u) break;
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
                if (!v.alive || v.hp <= 0 || v.owner != localOwner()) continue;
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
                if (beams.size() >= 768u) break;
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
        if (previewRing && beams.size() + 18u < 768u) {
            pushIsoEllipse(ringX, ringY, ringRad, 16, 2.0f * s, ringR, ringG, ringB, 0.62f);
            beams.push_back({ringX - 10.0f * s, ringY, ringX + 10.0f * s, ringY,
                             1.6f * s, ringR, ringG, ringB, 0.62f, 0.62f});
            beams.push_back({ringX, ringY - 5.0f * s, ringX, ringY + 5.0f * s,
                             1.6f * s, ringR, ringG, ringB, 0.62f, 0.62f});
        }
        if (showAltSalvage) {
            const auto& vehs = vehicleScene->vehicles();
            for (std::size_t vi = 0; vi < vehs.size() && beams.size() + 8u < 768u; ++vi) {
                const auto& v = vehs[vi];
                if (!v.alive || v.hp <= 0 || !v.visible) continue;
                if (v.owner != 3 && !isAllyOwner(v.owner)) continue;
                if (!simulation->canCaptureVehicle(vi)) continue;
                const float vx = static_cast<float>(v.x) * s;
                const float vy = static_cast<float>(v.y) * s;
                if (!inViewBounds(vx, vy)) continue;
                const float d = 10.0f * s;
                const float py = vy - 18.0f * s;
                beams.push_back({vx, py - d * 0.6f, vx + d, py, 2.0f * s, 105, 212, 235, 0.88f, 0.88f});
                beams.push_back({vx + d, py, vx, py + d * 0.6f, 2.0f * s, 105, 212, 235, 0.88f, 0.88f});
                beams.push_back({vx, py + d * 0.6f, vx - d, py, 2.0f * s, 105, 212, 235, 0.88f, 0.88f});
                beams.push_back({vx - d, py, vx, py - d * 0.6f, 2.0f * s, 105, 212, 235, 0.88f, 0.88f});
            }
        }
        if (showRightDragPreview && selection) {
            const auto [mx0, my0] = renderer.mapPosition(rightDragStartScreenX, rightDragStartScreenY);
            const auto [mx1, my1] = renderer.mapPosition(rightDragCurScreenX, rightDragCurScreenY);
            const float x0 = static_cast<float>(mx0), y0 = static_cast<float>(my0);
            const float x1 = static_cast<float>(mx1), y1 = static_cast<float>(my1);
            beams.push_back({x0, y0, x1, y1, 2.2f * s, 110, 224, 120, 0.72f, 0.72f});
            const std::size_t totalSel = std::clamp<std::size_t>(
                selection->selected().size() + selection->selectedVehicles().size(), 2u, 24u);
            const float dx = x1 - x0, dy = y1 - y0;
            const float len = std::max(1.0f, std::hypot(dx, dy));
            const float fx = dy / len, fy = -dx / len;
            for (std::size_t k = 0; k < totalSel && beams.size() + 4u < 768u; ++k) {
                const float frac = static_cast<float>(k) / static_cast<float>(totalSel - 1u);
                const float px = x0 + dx * frac;
                const float py = y0 + dy * frac;
                beams.push_back({px, py, px, py, 6.5f * s, 110, 224, 120, 0.88f, 0.88f});
                beams.push_back({px, py, px + fx * 12.0f * s, py + fy * 12.0f * s,
                                 2.0f * s, 105, 212, 235, 0.75f, 0.25f});
            }
        }
        if (combatEffects) {
            constexpr std::size_t kCombatBeamCap = 2040u;
            const bool addGlow = enhancedGraphics;
            if (vehicleScene && !heavyDamageSmokeVehicles.empty()) {
                const auto& vehs = vehicleScene->vehicles();
                const float animPhase = std::chrono::duration<float>(now - animStart).count();
                for (const std::size_t vi : heavyDamageSmokeVehicles) {
                    if (beams.size() >= kCombatBeamCap || vi >= vehs.size()) break;
                    const auto& v = vehs[vi];
                    const float vx = static_cast<float>(v.x * mapScale);
                    const float vy = static_cast<float>(v.y * mapScale);
                    const float sway = std::sin(animPhase * 2.1f + static_cast<float>(vi) * 1.1f) * 4.5f * s;
                    const float rise = (16.0f + 6.0f * std::sin(animPhase * 3.4f + static_cast<float>(vi))) * s;
                    beams.push_back({vx, vy - 5.0f * s, vx + sway, vy - rise, 8.5f * s, 60, 55, 50, 0.48f, 0.06f});
                    const float sparkPulse = std::sin(animPhase * 13.0f + static_cast<float>(vi) * 2.7f);
                    if (sparkPulse > 0.25f && beams.size() < kCombatBeamCap) {
                        const float sxOff = std::cos(animPhase * 9.0f + static_cast<float>(vi)) * 3.5f * s;
                        const float sa = std::clamp((sparkPulse - 0.25f) * 1.2f, 0.25f, 0.92f);
                        beams.push_back({vx + sxOff, vy - 4.0f * s, vx + sxOff * 1.6f, vy - 9.0f * s,
                                         2.6f * s, 255, 145, 40, sa, sa * 0.25f, addGlow});
                    }
                }
            }
            if (vehicleScene && !lightDamageSmokeVehicles.empty()) {
                const auto& vehs = vehicleScene->vehicles();
                const float animPhase = std::chrono::duration<float>(now - animStart).count();
                for (const std::size_t vi : lightDamageSmokeVehicles) {
                    if (beams.size() >= kCombatBeamCap || vi >= vehs.size()) break;
                    const auto& v = vehs[vi];
                    const float vx = static_cast<float>(v.x * mapScale);
                    const float vy = static_cast<float>(v.y * mapScale);
                    const float sway = std::sin(animPhase * 2.6f + static_cast<float>(vi) * 1.4f) * 3.5f * s;
                    const float rise = (11.0f + 4.0f * std::sin(animPhase * 3.8f + static_cast<float>(vi))) * s;
                    beams.push_back({vx, vy - 4.0f * s, vx + sway, vy - rise, 5.2f * s, 210, 210, 210, 0.24f, 0.03f});
                }
            }
            if (vehicleScene && !movingSmokeVehicles.empty()) {
                const auto& vehs = vehicleScene->vehicles();
                const float animPhase = std::chrono::duration<float>(now - animStart).count();
                std::size_t drawn = 0;
                for (const std::size_t vi : movingSmokeVehicles) {
                    if (drawn >= 6u || beams.size() >= kCombatBeamCap || vi >= vehs.size()) break;
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
                constexpr float kTwoPi = 6.28318530717958647692f;
                for (const auto& [sid, lp] : livePlanes) {
                    if (beams.size() >= kCombatBeamCap) break;
                    const float px = static_cast<float>(lp.pose.x * mapScale);
                    const float py = static_cast<float>(lp.pose.y * mapScale);
                    if (lp.pose.active && lp.pose.alive && enhancedGraphics && beams.size() + 3u <= kCombatBeamCap) {
                        const float ang = (static_cast<float>(lp.pose.dir32 & 31u) / 32.0f) * kTwoPi;
                        const float du = std::cos(ang);
                        const float dv = std::sin(ang);
                        const float fwdX = (du - dv) * 0.70710678f;
                        const float fwdY = (du + dv) * 0.35355339f;
                        const float wingX = (-dv - du) * 0.70710678f;
                        const float wingY = (-dv + du) * 0.35355339f;
                        const float sc = std::max(0.75f, lp.shadowScale);
                        const float noseX = px + fwdX * 14.0f * sc * s;
                        const float noseY = py + fwdY * 14.0f * sc * s;
                        const float propA = 0.16f + 0.06f * std::sin(animPhase * 42.0f + static_cast<float>(sid));
                        beams.push_back({noseX, noseY, noseX, noseY, 5.5f * s, 255, 240, 190, propA, propA});
                        const float tipSpan = 16.0f * sc * s;
                        const float trailLen = 22.0f * sc * s;
                        for (const float side : {-1.0f, 1.0f}) {
                            const float wx = px + wingX * tipSpan * side;
                            const float wy = py + wingY * tipSpan * side;
                            beams.push_back({wx, wy, wx - fwdX * trailLen, wy - fwdY * trailLen,
                                             1.6f * s, 255, 240, 190, 0.14f, 0.0f});
                        }
                    }
                    if (!lp.pose.alive || lp.pose.maxHp <= 0 || lp.pose.hp * 2 > lp.pose.maxHp) continue;
                    const auto* pSpec = unitCatalog ? unitCatalog->findPlane(lp.pose.planeId) : nullptr;
                    if (pSpec && !pSpec->hasFallSmoke) continue;
                    const float sway = std::sin(animPhase * 3.8f + static_cast<float>(sid)) * 5.0f * s;
                    const float rise = (16.0f + 8.0f * std::sin(animPhase * 4.5f + static_cast<float>(sid))) * s;
                    beams.push_back({px, py, px + sway, py - rise, 7.0f * s, 60, 55, 50, 0.42f, 0.06f});
                }
            }
            if (hud && hud->hasActiveAirTrails() && beams.size() < kCombatBeamCap) {
                hud->appendAirTrailBeams(mapScale, kCombatBeamCap - beams.size(), beams);
            }
            if (vehicleScene && !movingWaterVehicles.empty()) {
                const auto& vehs = vehicleScene->vehicles();
                const float animPhase = std::chrono::duration<float>(now - animStart).count();
                std::size_t wDrawn = 0;
                for (const std::size_t vi : movingWaterVehicles) {
                    if (wDrawn >= 8u || beams.size() + 6u >= kCombatBeamCap || vi >= vehs.size()) break;
                    const auto& v = vehs[vi];
                    if (!v.alive || !v.visible) continue;
                    const float vx = static_cast<float>(v.x * mapScale);
                    const float vy = static_cast<float>(v.y * mapScale);
                    if (!inViewBounds(vx, vy)) continue;
                    constexpr float kTwoPi = 6.28318530717958647692f;
                    const float dirAng = (static_cast<float>(v.hullDir & 31u) / 32.0f) * kTwoPi;
                    const float fwdU = std::cos(dirAng);
                    const float fwdV = std::sin(dirAng);
                    float bfx = fwdU - fwdV;
                    float bfy = 0.5f * (fwdU + fwdV);
                    const float bLen = std::max(1e-3f, std::hypot(bfx, bfy));
                    bfx /= bLen;
                    bfy /= bLen;
                    const float latX = -bfy;
                    const float latY = bfx * 0.65f;
                    const float pulse = 0.90f + 0.12f * std::sin(animPhase * 6.0f + static_cast<float>(vi));
                    const float bowX = vx + bfx * 8.0f * s;
                    const float bowY = vy + bfy * 8.0f * s;
                    const float trailL = 24.0f * s * pulse;
                    const float spread = 11.0f * s * pulse;
                    // Left & right V-wake foam arms behind stern/bow, rendered under unit sprites
                    beams.push_back({bowX, bowY, vx - bfx * trailL + latX * spread, vy - bfy * trailL + latY * spread,
                                     2.8f * s, 210, 210, 210, 0.42f, 0.04f, false, true});
                    beams.push_back({bowX, bowY, vx - bfx * trailL - latX * spread, vy - bfy * trailL - latY * spread,
                                     2.8f * s, 210, 210, 210, 0.42f, 0.04f, false, true});
                    beams.push_back({vx - bfx * 4.0f * s, vy - bfy * 4.0f * s,
                                     vx - bfx * 16.0f * s, vy - bfy * 16.0f * s,
                                     5.5f * s, 210, 210, 210, 0.22f, 0.03f, false, true});
                    ++wDrawn;
                }
            }
            const bool hasRetailVfx = combatVfx.hasRetailAssets();
            for (const auto& pp : liveParachutes) {
                if (beams.size() + 6u >= kCombatBeamCap || !pp.second.active) continue;
                const auto& p = pp.second;
                const float px = static_cast<float>(p.x) * s;
                const float py = static_cast<float>(p.y) * s;
                const float cy = py - static_cast<float>(std::max(0, p.altitudePx)) * s;
                if (!inViewBounds(px, py) && !inViewBounds(px, cy)) continue;
                const float altFrac = std::clamp(static_cast<float>(p.altitudePx) / 280.0f, 0.0f, 1.0f);
                beams.push_back({px, py, px, py, (6.0f + 5.0f * (1.0f - altFrac)) * s, 60, 55, 50,
                                 0.34f * (1.0f - 0.35f * altFrac), 0.34f * (1.0f - 0.35f * altFrac), false, true});
                if (!hasRetailVfx) {
                    const float topY = cy - 16.0f * s;
                    const float rimY = topY + 2.6f * s;
                    beams.push_back({px - 8.0f * s, rimY, px - 3.0f * s, topY, 3.8f * s, 255, 240, 190, 0.90f, 0.90f});
                    beams.push_back({px - 3.0f * s, topY, px + 3.0f * s, topY, 4.2f * s, 255, 240, 190, 0.94f, 0.94f});
                    beams.push_back({px + 3.0f * s, topY, px + 8.0f * s, rimY, 3.8f * s, 255, 240, 190, 0.90f, 0.90f});
                    beams.push_back({px - 7.0f * s, rimY, px, cy, 1.1f * s, 210, 210, 210, 0.65f, 0.55f});
                    beams.push_back({px, topY + 1.0f * s, px, cy, 1.1f * s, 210, 210, 210, 0.60f, 0.50f});
                    beams.push_back({px + 7.0f * s, rimY, px, cy, 1.1f * s, 210, 210, 210, 0.65f, 0.55f});
                }
            }
            for (const auto& bw : burningWrecks) {
                if (beams.size() >= kCombatBeamCap) break;
                const float tSec = std::chrono::duration<float>(bw.until - now).count();
                const float sway = std::sin(tSec * 2.5f + bw.x * 0.03f) * 5.0f * s;
                const float rise = (18.0f + 10.0f * std::sin(tSec * 4.0f + bw.y * 0.05f)) * s;
                const float glow = 0.55f + 0.35f * std::sin(tSec * 9.0f);
                beams.push_back({bw.x, bw.y - 6.0f * s, bw.x + sway, bw.y - rise, 10.0f * s, 60, 55, 50, 0.45f, 0.08f});
                if (addGlow) {
                    beams.push_back({bw.x, bw.y, bw.x, bw.y, 40.0f * s, 255, 145, 40, glow * 0.30f, glow * 0.30f, true, true});
                }
                beams.push_back({bw.x, bw.y - 4.0f * s, bw.x, bw.y - 4.0f * s, 5.0f * s, 235, 100, 30, glow, glow, addGlow});
            }
            for (const auto& v : activeVfx) {
                if (beams.size() >= kCombatBeamCap) break;
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
                    if (addGlow) {
                        const float mx = (v.x0 + ex) * 0.5f;
                        const float my = (v.y0 + ey) * 0.5f;
                        beams.push_back({mx, my, mx, my, 30.0f * s, 255, 145, 40, 0.28f * fade, 0.28f * fade, true, true});
                    }
                    if (!hasRetailVfx) {
                        beams.push_back({v.x0, v.y0, ex, ey, 11.0f * s * flick, 255, 145, 40, 0.90f * fade, 0.25f * fade, addGlow});
                        beams.push_back({v.x0, v.y0, ex, ey, 5.5f * s * flick, 255, 230, 130, 0.95f * fade, 0.45f * fade, addGlow});
                    }
                    break;
                }
                case ActiveVfx::Kind::Tracer:
                case ActiveVfx::Kind::CannonShell: {
                    const bool heavy = (v.kind == ActiveVfx::Kind::CannonShell);
                    if (t < 0.45f) {
                        const float fa = 1.0f - (t / 0.45f);
                        if (addGlow) {
                            beams.push_back({v.x0, v.y0, v.x0, v.y0, (heavy ? 32.0f : 18.0f) * s, 255, 145, 40, fa * 0.30f, fa * 0.30f, true, true});
                        }
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, (heavy ? 12.0f : 8.5f) * s, 255, 230, 130, fa, fa, addGlow});
                    }
                    const float tail = std::max(0.0f, t - 0.32f);
                    const float bx0 = v.x0 + (v.x1 - v.x0) * tail;
                    const float by0 = v.y0 + (v.y1 - v.y0) * tail;
                    const float bx1 = v.x0 + (v.x1 - v.x0) * t;
                    const float by1 = v.y0 + (v.y1 - v.y0) * t;
                    if (heavy) {
                        const float smokeTail = std::max(0.0f, t - 0.52f);
                        const float sx0 = v.x0 + (v.x1 - v.x0) * smokeTail;
                        const float sy0 = v.y0 + (v.y1 - v.y0) * smokeTail;
                        beams.push_back({sx0, sy0, bx1, by1, 4.4f * s, 210, 210, 210, 0.02f, 0.24f * (1.0f - 0.3f * t)});
                        beams.push_back({bx0, by0, bx1, by1, 4.2f * s, 255, 145, 40, 0.15f, 0.95f, addGlow});
                        beams.push_back({bx0, by0, bx1, by1, 1.8f * s, 255, 250, 232, 0.25f, 1.0f, addGlow});
                    } else {
                        beams.push_back({bx0, by0, bx1, by1, 3.0f * s, 255, 145, 40, 0.05f, 0.85f, addGlow});
                        beams.push_back({bx0, by0, bx1, by1, 1.4f * s, 255, 250, 232, 0.10f, 1.0f, addGlow});
                    }
                    if (t > 0.78f) {
                        const float ia = (1.0f - t) / 0.22f;
                        beams.push_back({v.x1, v.y1, v.x1, v.y1, (heavy ? 9.0f : 5.5f) * s, 255, 230, 130, ia * 0.85f, ia * 0.85f, addGlow});
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
                    beams.push_back({sx0, sy0, hx, hy, 8.0f * s, 210, 210, 210, 0.04f, 0.30f * (1.0f - 0.35f * t)});
                    beams.push_back({sx0, sy0, hx, hy, 5.5f * s, 60, 55, 50, 0.08f, 0.48f * (1.0f - 0.35f * t)});
                    const float fTail = std::max(0.0f, t - 0.14f);
                    const float fx0 = v.x0 + (v.x1 - v.x0) * fTail;
                    const float fy0 = v.y0 + (v.y1 - v.y0) * fTail - 20.0f * s * std::sin(fTail * 3.14159f);
                    beams.push_back({fx0, fy0, hx, hy, 4.8f * s, 255, 145, 40, 0.35f, 1.0f, addGlow});
                    beams.push_back({fx0, fy0, hx, hy, 2.0f * s, 255, 250, 232, 0.50f, 1.0f, addGlow});
                    if (t > 0.78f) {
                        const float ia = (1.0f - t) / 0.22f;
                        beams.push_back({v.x1, v.y1, v.x1, v.y1, 14.0f * s, 255, 230, 130, ia * 0.9f, ia * 0.9f, addGlow});
                    }
                    break;
                }
                case ActiveVfx::Kind::Explosion:
                case ActiveVfx::Kind::VehicleExplosion: {
                    const float sc = (v.kind == ActiveVfx::Kind::VehicleExplosion) ? 1.55f : 1.0f;
                    const float fade = 1.0f - t;
                    const float fireR = (10.0f + 22.0f * std::min(1.0f, t * 2.0f)) * sc * s;
                    if (addGlow) {
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, fireR * 2.5f, 255, 145, 40, fade * 0.36f, fade * 0.36f, true, true});
                        if (beams.size() + 14u < kCombatBeamCap) {
                            pushIsoEllipse(v.x0, v.y0, (12.0f + 44.0f * t) * sc, 12, 1.8f * s,
                                           255, 230, 130, fade * 0.42f, true, true);
                        }
                    }
                    if (!hasRetailVfx) {
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, fireR, 220, 65, 25, fade * 0.85f, fade * 0.85f, addGlow});
                        if (t < 0.65f) {
                            const float coreA = (1.0f - t / 0.65f);
                            beams.push_back({v.x0, v.y0, v.x0, v.y0, fireR * 0.55f, 255, 230, 130, coreA * 0.9f, coreA * 0.9f, addGlow});
                        }
                        beams.push_back({v.x0, v.y0 - 4.0f * s, v.x0, v.y0 - (20.0f + 16.0f * t) * sc * s,
                                         14.0f * sc * s, 60, 55, 50, fade * 0.50f, 0.08f});
                    }
                    break;
                }
                case ActiveVfx::Kind::HealPulse: {
                    const float fade = 1.0f - t;
                    beams.push_back({v.x0, v.y0 - 12.0f * t * s, v.x0, v.y0 - (8.0f + 16.0f * t) * s,
                                     7.0f * s, 80, 235, 120, fade * 0.9f, fade * 0.3f, addGlow});
                    break;
                }
                case ActiveVfx::Kind::RepairPulse: {
                    const float fade = 1.0f - t;
                    beams.push_back({v.x0, v.y0 - 10.0f * t * s, v.x0, v.y0 - (8.0f + 16.0f * t) * s,
                                     8.0f * s, 90, 210, 255, fade * 0.9f, fade * 0.3f, addGlow});
                    break;
                }
                case ActiveVfx::Kind::Parachute: {
                    const float fade = 1.0f - 0.35f * t;
                    const float sway = std::sin(t * 9.0f) * 4.5f * s * (1.0f - t);
                    const float px = v.x1 + sway;
                    const float py = v.y0 + (v.y1 - v.y0) * t;
                    const float cy = py - 15.0f * s;
                    beams.push_back({v.x1, v.y1, v.x1, v.y1, (10.0f - 4.0f * t) * s, 60, 55, 50,
                                     0.38f * (1.0f - 0.5f * t), 0.38f * (1.0f - 0.5f * t), false, true});
                    beams.push_back({px - 8.5f * s, cy + 2.0f * s, px - 3.2f * s, cy - 1.5f * s,
                                     4.2f * s, 255, 240, 190, 0.88f * fade, 0.92f * fade});
                    beams.push_back({px - 3.2f * s, cy - 1.5f * s, px + 3.2f * s, cy - 1.5f * s,
                                     4.8f * s, 255, 240, 190, 0.94f * fade, 0.94f * fade});
                    beams.push_back({px + 3.2f * s, cy - 1.5f * s, px + 8.5f * s, cy + 2.0f * s,
                                     4.2f * s, 255, 240, 190, 0.92f * fade, 0.88f * fade});
                    beams.push_back({px - 7.5f * s, cy + 2.0f * s, px, py, 1.15f * s, 210, 210, 210,
                                     0.68f * fade, 0.48f * fade});
                    beams.push_back({px, cy - 0.5f * s, px, py, 1.15f * s, 210, 210, 210,
                                     0.62f * fade, 0.45f * fade});
                    beams.push_back({px + 7.5f * s, cy + 2.0f * s, px, py, 1.15f * s, 210, 210, 210,
                                     0.68f * fade, 0.48f * fade});
                    break;
                }
                case ActiveVfx::Kind::Ricochet: {
                    const float fade = 1.0f - t;
                    const float px = v.x0 + (v.x1 - v.x0) * t;
                    const float py = v.y0 + (v.y1 - v.y0) * t;
                    const float tx0 = v.x0 + (v.x1 - v.x0) * std::max(0.0f, t - 0.45f);
                    const float ty0 = v.y0 + (v.y1 - v.y0) * std::max(0.0f, t - 0.45f);
                    beams.push_back({v.x0, v.y0, v.x0, v.y0, 7.0f * s, 255, 230, 130, fade * 0.85f, fade * 0.85f, addGlow});
                    beams.push_back({tx0, ty0, px, py, 2.2f * s, 255, 195, 90, fade * 0.25f, fade * 0.95f, addGlow});
                    break;
                }
                case ActiveVfx::Kind::MuzzleDust: {
                    const float fade = (1.0f - t) * (1.0f - t);
                    const float dx = v.x1 - v.x0;
                    const float dy = v.y1 - v.y0;
                    const float reach = 0.25f + 0.85f * t;
                    const float ex = v.x0 + dx * reach;
                    const float ey = v.y0 + dy * reach;
                    const float len = std::max(1.0f, std::hypot(dx, dy));
                    const float latX = (-dy / len) * (4.0f + 10.0f * t) * s;
                    const float latY = (dx / len) * (4.0f + 10.0f * t) * 0.65f * s;
                    const float w = (5.5f + 11.0f * t) * s;
                    beams.push_back({v.x0, v.y0, ex, ey, w, 210, 210, 210, 0.36f * fade, 0.04f, false, true});
                    beams.push_back({v.x0, v.y0, ex + latX, ey + latY, w * 0.72f, 210, 210, 210, 0.28f * fade, 0.03f, false, true});
                    beams.push_back({v.x0, v.y0, ex - latX, ey - latY, w * 0.72f, 210, 210, 210, 0.28f * fade, 0.03f, false, true});
                    if (addGlow && t < 0.35f) {
                        const float gf = 1.0f - (t / 0.35f);
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, 24.0f * s, 255, 145, 40, 0.26f * gf, 0.26f * gf, true, true});
                    }
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
