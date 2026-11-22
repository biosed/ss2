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

bool GameSession::updateViewTooltipAndPresent() {
    const auto noticeFrame = std::chrono::steady_clock::now();
    const double frameDt = std::clamp(std::chrono::duration<double>(noticeFrame - lastNoticeFrame).count(), 0.0, 0.1);
    bool needVfxSync = false;
    if (!combatEffects && combatVfx.hasActiveSprites()) {
        combatVfx.clearActive();
        needVfxSync = true;
    }
    if (!paused && frameDt > 0.0) {
        if (hud && hud->hasActiveAirTrails() && hud->stepAirTrails(static_cast<float>(frameDt))) {
            needVfxSync = true;
        }
        if (combatEffects && combatVfx.atlasSlot().has_value()) {
            std::vector<std::pair<float, float>> visWrecks;
            const float invScale = 1.0f / static_cast<float>(std::max(1, mapScale));
            for (const auto& bw : burningWrecks) {
                const float sx = bw.x * invScale;
                const float sy = bw.y * invScale;
                if (isBuildingPointVisible(static_cast<int>(std::lround(sx)),
                                           static_cast<int>(std::lround(sy)))) {
                    visWrecks.emplace_back(sx, sy);
                }
            }
            if ((combatVfx.hasActiveSprites() || !visWrecks.empty()) &&
                combatVfx.step(static_cast<float>(frameDt), visWrecks)) {
                needVfxSync = true;
            }
        }
    }
    if (needVfxSync) {
        syncGpuSprites();
    }
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) && !briefing.active() && !menu.active()) {
        const bool* kb = SDL_GetKeyboardState(nullptr);
        double kdx = 0.0, kdy = 0.0;
        if (kb[SDL_SCANCODE_LEFT]) kdx -= 1.0;
        if (kb[SDL_SCANCODE_RIGHT]) kdx += 1.0;
        if (kb[SDL_SCANCODE_UP]) kdy -= 1.0;
        if (kb[SDL_SCANCODE_DOWN]) kdy += 1.0;
        if (kdx != 0.0 && kdy != 0.0) { kdx *= 0.70710678; kdy *= 0.70710678; }
        if (kdx != 0.0 || kdy != 0.0) {
            const bool fast = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
            const double keyPanSpeed = fast ? 1140.0 : 760.0;
            renderer.panMap(kdx * keyPanSpeed * frameDt, kdy * keyPanSpeed * frameDt);
        }
    }
    if (scene && selection) {
        bool showTip = false;
        if (SDL_GetMouseFocus() == window && !briefing.active() && !menu.active()) {
            float mx{}, my{};
            const auto buttons = SDL_GetMouseState(&mx, &my);
            int winW = 1, winH = 1;
            SDL_GetWindowSize(window, &winW, &winH);
            // Classic RTS mouse edge-scrolling when cursor is near the window edges (not while dragging).
            if (!(buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK)) && winW > 80 && winH > 80) {
                constexpr float edgeMargin = 24.0f;
                double dx = 0.0, dy = 0.0;
                if (mx <= edgeMargin) dx = -1.0;
                else if (mx >= static_cast<float>(winW) - edgeMargin) dx = 1.0;
                if (my <= edgeMargin) dy = -1.0;
                else if (my >= static_cast<float>(winH) - edgeMargin) dy = 1.0;
                if (dx != 0.0 && dy != 0.0) { dx *= 0.70710678; dy *= 0.70710678; }
                if (dx != 0.0 || dy != 0.0) {
                    constexpr double panSpeedPtsPerSec = 760.0;
                    renderer.panMap(dx * panSpeedPtsPerSec * frameDt, dy * panSpeedPtsPerSec * frameDt);
                }
            }
            if (!hud || !hud->opaqueAtWindow(mx, my)) {
                const auto [mapX, mapY] = renderer.mapPosition(mx, my);
                const auto appendArmorLine = [&](ss2::TooltipContent& tip, const ss2::assets::UnitSpec* spec) {
                    if (!spec) return;
                    const auto& ap = spec->armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)];
                    if (ap.front > 0 || ap.side > 0 || ap.rear > 0) {
                        tip.lines.push_back(ss2::trf("Armor F/S/R: {0} / {1} / {2}",
                                                     {std::to_string(ap.front), std::to_string(ap.side),
                                                      std::to_string(ap.rear)}));
                    }
                };
                if (const auto hovered = selection->infantryAt(int(std::floor(mapX)), int(std::floor(mapY)))) {
                    const auto& u = scene->units().at(*hovered);
                    if (u.hp > 0 && u.visible) {
                        const int maxHp = (*hovered < initialUnitHp.size()) ? initialUnitHp[*hovered] : 100;
                        const auto* uSpec = unitCatalog ? unitCatalog->find(u.id) : nullptr;
                        std::string lowId = u.id;
                        for (char& ch : lowId) {
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        }
                        ss2::TooltipContent tc;
                        tc.title = ss2::tr(hudDisplayName(u.id));
                        tc.gauge = std::clamp(static_cast<float>(u.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
                        if (u.owner == localOwner()) {
                            tc.lines.push_back(ss2::trf("Health {0} / {1}", {std::to_string(u.hp), std::to_string(maxHp)}));
                            const int maxInfAmmo = (uSpec && !uSpec->isVehicle && uSpec->ammo1 > 0) ? uSpec->ammo1 : 40;
                            tc.lines.push_back(ss2::trf("Ammo {0} / {1}", {std::to_string(u.ammo), std::to_string(maxInfAmmo)}));
                            const int expVal = (*hovered < unitExpa.size()) ? unitExpa[*hovered] : 0;
                            const int killsVal = (*hovered < unitKills.size()) ? unitKills[*hovered] : 0;
                            if (killsVal > 0) {
                                tc.lines.push_back(ss2::trf("Rank: {0} ({1} kills)", {ss2::tr(veterancyRank(expVal)), std::to_string(killsVal)}));
                            } else {
                                tc.lines.push_back(ss2::trf("Rank: {0}", {ss2::tr(veterancyRank(expVal))}));
                            }
                            if (lowId.find("officer") != std::string::npos ||
                                lowId.find("general") != std::string::npos ||
                                (uSpec && uSpec->isOfficerInfantry())) {
                                tc.lines.push_back(ss2::tr(binocularUnits.contains(*hovered)
                                                               ? "Binoculars active"
                                                               : "Officer — press B for binoculars"));
                                const bool isGeneralUnit = lowId.find("general") != std::string::npos ||
                                                           (uSpec && uSpec->selType == "general");
                                tc.lines.push_back(ss2::tr(isGeneralUnit
                                                               ? "General Command Aura — wide morale rally"
                                                               : "Officer Aura — rallies nearby troops"));
                            } else if (lowId.find("medic") != std::string::npos ||
                                       (uSpec && uSpec->isMedicInfantry())) {
                                tc.lines.push_back(ss2::tr("Field Medic — heals nearby troops"));
                                if (!holdFireUnits.contains(*hovered)) {
                                    tc.lines.push_back(ss2::tr("Auto-support active (H to hold)"));
                                }
                            } else if (lowId.find("sapper") != std::string::npos ||
                                       lowId.find("miner") != std::string::npos ||
                                       (uSpec && (uSpec->selType == "sapper" || uSpec->selType == "sappers"))) {
                                tc.lines.push_back(ss2::tr("Sapper — E to lay mine / right-click mine to disarm"));
                            } else if ((uSpec && uSpec->isSniper()) ||
                                       lowId.find("sniper") != std::string::npos) {
                                tc.lines.push_back(ss2::tr(binocularUnits.contains(*hovered)
                                                               ? "Sniper — binoculars active (C to camouflage)"
                                                               : "Sniper — C camouflage / B binoculars"));
                            } else if ((uSpec && uSpec->isHeavyMgInfantry()) ||
                                       lowId.find("hmg") != std::string::npos) {
                                tc.lines.push_back(ss2::tr("Heavy Machine Gunner — high-rate suppression"));
                            } else if ((uSpec && uSpec->isMortarInfantry()) ||
                                       lowId.find("mortar") != std::string::npos) {
                                tc.lines.push_back(ss2::tr("Light Mortar — G to bombard / explosive splash"));
                            } else if ((uSpec && uSpec->hasFlamethrower()) ||
                                       lowId.find("flam") != std::string::npos) {
                                tc.lines.push_back(ss2::tr("Flamethrower — clears buildings & trenches"));
                            } else if ((uSpec && uSpec->isAntiTankInfantry()) ||
                                       lowId.find("bazooka") != std::string::npos ||
                                       lowId.find("panzerfaust") != std::string::npos ||
                                       lowId.find("piat") != std::string::npos) {
                                tc.lines.push_back(ss2::tr("Anti-Tank Infantry — armor-piercing rockets"));
                            } else if ((uSpec && uSpec->isKamikaze()) ||
                                       lowId.find("kamikaze") != std::string::npos) {
                                tc.lines.push_back(ss2::tr("Kamikaze — explosive suicide charge"));
                            }
                            if (*hovered < unitGrenades.size() && unitGrenades[*hovered] > 0) {
                                tc.lines.push_back(ss2::trf("AT grenades: {0}", {std::to_string(unitGrenades[*hovered])}));
                            }
                            if (*hovered < unitMineAmmo.size() && unitMineAmmo[*hovered] > 0) {
                                tc.lines.push_back(ss2::trf("AT mines: {0}", {std::to_string(unitMineAmmo[*hovered])}));
                            }
                            tc.lines.push_back(ss2::tr(crouchedUnits.contains(*hovered) ? "Crouching" : "Standing"));
                            if (holdFireUnits.contains(*hovered)) tc.lines.push_back(ss2::tr("Holding fire"));
                            if (camouflagedUnits.contains(*hovered)) {
                                tc.lines.push_back(ss2::tr("Status: Camouflaged (hidden)"));
                            }
                            if (uSpec && uSpec->moraleResist >= 1000 && !uSpec->isOfficerInfantry()) {
                                tc.lines.push_back(ss2::tr("Elite Morale — resists suppression"));
                            }
                            if (uSpec && uSpec->isFearless()) {
                                tc.lines.push_back(ss2::tr("Fearless — maintains fire rate under suppression"));
                            }
                            if (uSpec && uSpec->isQualifiedVehicleCrew() && !uSpec->isOfficerInfantry()) {
                                tc.lines.push_back(ss2::tr("Qualified Crew — full efficiency in captured vehicles"));
                            }
                            if (uSpec && !uSpec->canFireFromInside) {
                                tc.lines.push_back(ss2::tr("Cannot fire from inside buildings"));
                            } else if (uSpec && uSpec->hasExplicitInsideRule && uSpec->canFireFromInside) {
                                tc.lines.push_back(ss2::tr("Safe to fire from inside buildings (no backblast)"));
                            }
                            if (uSpec && uSpec->crouchToFire) {
                                tc.lines.push_back(ss2::tr("Prone Deployment — crouches to fire AT rifle"));
                            }
                            if (suppressedUnits.contains(*hovered)) {
                                tc.lines.push_back(ss2::tr("Status: Suppressed (slower fire — rally with Officer)"));
                            }
                            if (simulation && simulation->isElevatedMapPoint(u.x, u.y)) {
                                tc.lines.push_back(ss2::tr("High Ground (+vision, +range)"));
                            }
                        } else if (isAllyOwner(u.owner)) {
                            tc.lines.push_back(ss2::tr("Allied"));
                        } else if (!multiplayer.enabled && u.owner == 3) {
                            tc.lines.push_back(ss2::tr("Neutral"));
                        } else {
                            tc.lines.push_back(ss2::tr("Hostile"));
                        }
                        tooltip.show(tc, mx, my);
                        showTip = true;
                    }
                } else if (vehicleScene) {
                    if (const auto vHovered = selection->vehicleAt(int(std::floor(mapX)), int(std::floor(mapY)))) {
                        const auto& v = vehicleScene->vehicles().at(*vHovered);
                        if (v.alive && v.hp > 0 && v.visible) {
                            const int maxHp = (*vHovered < initialVehicleHp.size()) ? initialVehicleHp[*vHovered] : 100;
                            const auto* vSpec = unitCatalog ? unitCatalog->find(v.id) : nullptr;
                            ss2::TooltipContent tc;
                            tc.title = ss2::tr(hudDisplayName(v.id));
                            tc.gauge = std::clamp(static_cast<float>(v.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
                            if (v.owner == localOwner()) {
                                tc.lines.push_back(ss2::trf("Health {0} / {1}", {std::to_string(v.hp), std::to_string(maxHp)}));
                                if (vSpec && vSpec->shot1.damage > 0 && vSpec->shot2.damage > 0) {
                                    const int a2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                    const int maxA1 = vSpec->ammo1 > 0 ? vSpec->ammo1 : std::max(1, v.ammo);
                                    const char* fmt = vSpec->shot2.isFlamethrower()
                                        ? "Ammo {0}/{1} | Flame {2}"
                                        : "Ammo {0}/{1} | MG {2}";
                                    tc.lines.push_back(ss2::trf(fmt,
                                                                {std::to_string(v.ammo), std::to_string(maxA1), std::to_string(a2)}));
                                } else if (vSpec && vSpec->shot1.damage > 0) {
                                    const int maxA1 = vSpec->ammo1 > 0 ? vSpec->ammo1 : std::max(1, v.ammo);
                                    tc.lines.push_back(ss2::trf("Ammo {0} / {1}", {std::to_string(v.ammo), std::to_string(maxA1)}));
                                } else if (vSpec && vSpec->shot2.damage > 0) {
                                    if (vSpec->ammo2 > 0) {
                                        const int a2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                        tc.lines.push_back(ss2::trf("Ammo {0} / {1}",
                                                                    {std::to_string(a2), std::to_string(vSpec->ammo2)}));
                                    } else {
                                        const int maxA1 = vSpec->ammo1 > 0 ? vSpec->ammo1 : std::max(1, v.ammo);
                                        tc.lines.push_back(ss2::trf("Ammo {0} / {1}",
                                                                    {std::to_string(v.ammo), std::to_string(maxA1)}));
                                    }
                                }
                                appendArmorLine(tc, vSpec);
                                const int vExpVal = (*vHovered < vehExpa.size()) ? vehExpa[*vHovered] : 0;
                                const int vKillsVal = (*vHovered < vehKills.size()) ? vehKills[*vHovered] : 0;
                                if (vKillsVal > 0) {
                                    tc.lines.push_back(ss2::trf("Rank: {0} ({1} kills)", {ss2::tr(veterancyRank(vExpVal)), std::to_string(vKillsVal)}));
                                } else {
                                    tc.lines.push_back(ss2::trf("Rank: {0}", {ss2::tr(veterancyRank(vExpVal))}));
                                }
                                if (vSpec && vSpec->canMoveBackward) {
                                    tc.lines.push_back(ss2::tr("Press R + click to reverse"));
                                }
                                if (vSpec && vSpec->moveDamageHp > 0 && v.hp < vSpec->moveDamageHp) {
                                    tc.lines.push_back(ss2::tr("Tracks damaged — needs supply truck"));
                                }
                                if (vSpec && vSpec->isSupplyCrate()) {
                                    if (vSpec->ammo1 > 0) {
                                        tc.lines.push_back(ss2::tr("Ammo Crate"));
                                        tc.lines.push_back(v.ammo > 0
                                            ? ss2::trf("Ammo Stock: {0} / {1}", {std::to_string(v.ammo), std::to_string(vSpec->ammo1)})
                                            : ss2::tr("Depleted"));
                                    } else {
                                        tc.lines.push_back(ss2::tr("Spare Parts Crate"));
                                        const int p2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                        const int maxP2 = std::max(1, vSpec->ammo2);
                                        tc.lines.push_back(p2 > 0
                                            ? ss2::trf("Spare Parts: {0} / {1}", {std::to_string(p2), std::to_string(maxP2)})
                                            : ss2::tr("Depleted"));
                                    }
                                } else if (vSpec && vSpec->isSupplyTruck()) {
                                    tc.lines.push_back(ss2::tr("Supply & Repair Truck"));
                                    if (vSpec->ammo1 > 0 || vSpec->ammo2 > 0) {
                                        const int a2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                        tc.lines.push_back(ss2::trf("Supply: Ammo {0}/{1} | Parts {2}/{3}",
                                                                    {std::to_string(v.ammo), std::to_string(vSpec->ammo1),
                                                                     std::to_string(a2), std::to_string(vSpec->ammo2)}));
                                    }
                                    if (!vehicleHoldFireUnits.contains(*vHovered)) {
                                        tc.lines.push_back(ss2::tr("Auto-support active (H to hold)"));
                                    }
                                } else if (vSpec && vSpec->isAmbulance()) {
                                    tc.lines.push_back(ss2::tr("Mobile Hospital"));
                                    if (!vehicleHoldFireUnits.contains(*vHovered)) {
                                        tc.lines.push_back(ss2::tr("Auto-support active (H to hold)"));
                                    }
                                } else if (vSpec && vSpec->isRocketArtillery()) {
                                    tc.lines.push_back(ss2::tr("Rocket Artillery"));
                                } else if (vSpec && vSpec->hasFlamethrower()) {
                                    tc.lines.push_back(ss2::tr("Flamethrower Vehicle"));
                                } else if (vSpec && vSpec->isHeavyAntiAir()) {
                                    tc.lines.push_back(ss2::tr("Heavy High-Altitude Flak (effective vs all aircraft)"));
                                } else if (vSpec && vSpec->isAntiAir()) {
                                    tc.lines.push_back(ss2::tr("Anti-Aircraft / Flak (best vs low-flying aircraft)"));
                                } else if (vSpec && vSpec->isCasemate() && vSpec->selType != "howitzer") {
                                    tc.lines.push_back(ss2::tr("Assault Gun / Tank Destroyer"));
                                } else if (vSpec && vSpec->isKamikaze()) {
                                    tc.lines.push_back(ss2::tr("Demolition Charge Vehicle (self-destructs)"));
                                } else if (vSpec && vSpec->isArtillery()) {
                                    tc.lines.push_back(ss2::tr("Artillery / Field Gun"));
                                } else if (vSpec && vSpec->isFortification()) {
                                    tc.lines.push_back(ss2::tr("Emplaced Machine-Gun Nest"));
                                }
                                if (vSpec && (vSpec->turretType == "gaub" || vSpec->turretType == "atg_gaub")) {
                                    tc.lines.push_back(ss2::tr("Heavy Turret Artillery — high-explosive bombardment"));
                                }
                                if (vSpec && vSpec->isGunboat()) {
                                    tc.lines.push_back(ss2::tr("Naval Gunboat"));
                                } else if (vSpec && vSpec->isNavalCraft()) {
                                    tc.lines.push_back(ss2::tr("Naval / Landing Craft"));
                                }
                                if (vSpec && vSpec->isTrain()) {
                                    tc.lines.push_back(ss2::tr("Armored Train"));
                                }
                                if (vSpec && vSpec->passengersCanFire) {
                                    tc.lines.push_back(ss2::tr("Open-Top — passengers can fire"));
                                }
                                if (vSpec && vSpec->soldiersOnArmor) {
                                    tc.lines.push_back(ss2::tr("Tank Desant — riders exposed on hull armor"));
                                }
                                if (vSpec && vSpec->gunShotWait > 0) {
                                    tc.lines.push_back(ss2::tr("Heavy Gun — stabilizes briefly after moving"));
                                }
                                if (vSpec && vSpec->isPropaganda()) {
                                    tc.lines.push_back(ss2::tr("Command / Morale Vehicle — rallies nearby troops"));
                                }
                                if (*vHovered < vehicleCrewImpaired.size() && vehicleCrewImpaired[*vHovered]) {
                                    tc.lines.push_back(ss2::tr("Crew depleted — slower reload (heal with Medic)"));
                                }
                                if (*vHovered < vehicleMoveImpaired.size() && vehicleMoveImpaired[*vHovered]) {
                                    if (vSpec && vSpec->soldToMove > vSpec->crewNumber && vSpec->isTowable()) {
                                        tc.lines.push_back(ss2::tr("Heavy Towed Gun — requires tractor to relocate"));
                                    } else {
                                        tc.lines.push_back(ss2::tr("Crew depleted — cannot move (heal or tow)"));
                                    }
                                }
                                if (*vHovered < vehicleSuppressed.size() && vehicleSuppressed[*vHovered]) {
                                    tc.lines.push_back(ss2::tr("Status: Crew Shocked (reload slowed — rally with Officer)"));
                                }
                                if (vSpec && (vSpec->marchEnabled || vSpec->marchSightBonus > 0)) {
                                    const bool hatchIsOpen = *vHovered < vehicleHatchOpen.size() &&
                                                             vehicleHatchOpen[*vHovered];
                                    tc.lines.push_back(ss2::tr(
                                        hatchIsOpen ? "Commander Hatch: Open (+sight, X to button up)"
                                                    : "Commander Hatch: Buttoned (X to open)"));
                                }
                                if (vSpec && vSpec->moraleResist >= 10000) {
                                    tc.lines.push_back(ss2::tr("Heavy Armor Morale — resists crew shock"));
                                }
                                if (vSpec && vSpec->shot1.indirectArc) {
                                    tc.lines.push_back(ss2::tr("Indirect Fire — high-arc barrage (+splash radius)"));
                                }
                                if (vSpec && vSpec->crewCanBeHealed) {
                                    tc.lines.push_back(ss2::tr("Open Cab — field medics rapidly treat crew"));
                                }
                                if (vSpec && vSpec->canBeParadropped) {
                                    tc.lines.push_back(ss2::tr("Airborne — paradroppable equipment"));
                                }
                                if (simulation && simulation->isElevatedMapPoint(v.x, v.y)) {
                                    tc.lines.push_back(ss2::tr("High Ground (+vision, +range)"));
                                }
                                if (*vHovered < vehicleCargo.size() && vehicleCargo[*vHovered].second > 0) {
                                    const auto [cnt, cap] = vehicleCargo[*vHovered];
                                    tc.lines.push_back(ss2::trf("Passengers {0} / {1}",
                                                                {std::to_string(cnt), std::to_string(cap)}));
                                    if (cnt > 0) tc.lines.push_back(ss2::tr("Press U to unload"));
                                }
                                if (*vHovered < vehicleTowed.size() && vehicleTowed[*vHovered] >= 0) {
                                    const std::size_t gi = static_cast<std::size_t>(vehicleTowed[*vHovered]);
                                    if (gi < vehicleScene->vehicles().size()) {
                                        tc.lines.push_back(ss2::trf("Towing: {0} (U to unhitch)",
                                                                    {hudDisplayName(vehicleScene->vehicles()[gi].id)}));
                                    }
                                } else if (*vHovered < vehicleTowedBy.size() && vehicleTowedBy[*vHovered] >= 0) {
                                    tc.lines.push_back(ss2::tr("Towed by tractor (U to unhitch)"));
                                } else if (vSpec && vSpec->isTowable()) {
                                    tc.lines.push_back(ss2::tr("Towable field gun — right-click with truck to hitch"));
                                }
                                if (vehicleHoldFireUnits.contains(*vHovered)) tc.lines.push_back(ss2::tr("Holding fire"));
                            } else if (isAllyOwner(v.owner)) {
                                tc.lines.push_back(ss2::tr("Allied"));
                            } else if (v.owner == 3 && (!multiplayer.enabled || (simulation && simulation->canCaptureVehicle(*vHovered)))) {
                                tc.lines.push_back(ss2::tr("Neutral — right-click with infantry to capture"));
                            } else {
                                const bool tracksDmg = vSpec && vSpec->moveDamageHp > 0 && v.hp < vSpec->moveDamageHp;
                                tc.lines.push_back(ss2::tr(tracksDmg ? "Hostile — Tracks damaged" : "Hostile"));
                                if (vSpec && vSpec->hasExposedCrew()) {
                                    tc.lines.push_back(ss2::tr("Exposed crew — vulnerable to sniper/flank fire"));
                                }
                                appendArmorLine(tc, vSpec);
                            }
                            tooltip.show(tc, mx, my);
                            showTip = true;
                        }
                    }
                }
                if (!showTip && !liveBuildings.empty()) {
                    const int simX = int(std::floor(mapX / mapScale));
                    const int simY = int(std::floor(mapY / mapScale));
                    if (const auto bHovered = buildingAtMap(simX, simY)) {
                        const auto& b = liveBuildings[*bHovered];
                        if (!b.destroyed && (b.owner == localOwner() || isAllyOwner(b.owner) || isBuildingPointVisible(b.x, b.y))) {
                            ss2::TooltipContent tc;
                            tc.title = ss2::tr("Building");
                            tc.gauge = std::clamp(static_cast<float>(b.hp) / static_cast<float>(std::max(1, b.maxHp)),
                                                  0.0f, 1.0f);
                            tc.lines.push_back(ss2::trf("Integrity {0} / {1}",
                                                        {std::to_string(b.hp), std::to_string(b.maxHp)}));
                            if (b.occupantCount > 0) {
                                if (b.owner == localOwner()) {
                                    tc.lines.push_back(ss2::trf("Garrison {0} / {1}",
                                                                {std::to_string(b.occupantCount), std::to_string(b.capacity)}));
                                    if (scene && !b.occupantIds.empty()) {
                                        std::map<std::string, int> counts;
                                        bool anyHold = false;
                                        bool anyBino = false;
                                        bool anySup = false;
                                        bool anyBlockedInside = false;
                                        for (const size_t sid : b.occupantIds) {
                                            if (sid < scene->units().size()) {
                                                const auto& occId = scene->units()[sid].id;
                                                ++counts[ss2::tr(hudDisplayName(occId))];
                                                if (const auto* uSpec = unitCatalog ? unitCatalog->find(occId) : nullptr;
                                                    uSpec && !uSpec->canFireFromInside) {
                                                    anyBlockedInside = true;
                                                }
                                            }
                                            if (holdFireUnits.contains(sid)) anyHold = true;
                                            if (binocularUnits.contains(sid)) anyBino = true;
                                            if (suppressedUnits.contains(sid)) anySup = true;
                                        }
                                        std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
                                        std::ranges::sort(sorted, [](const auto& a, const auto& c) {
                                            return a.second != c.second ? a.second > c.second : a.first < c.first;
                                        });
                                        std::string roster;
                                        const std::size_t shown = std::min<std::size_t>(3, sorted.size());
                                        for (std::size_t k = 0; k < shown; ++k) {
                                            if (k > 0) roster += ", ";
                                            roster += std::to_string(sorted[k].second) + "x " + sorted[k].first;
                                        }
                                        if (sorted.size() > 3) {
                                            int extra = 0;
                                            for (std::size_t k = 3; k < sorted.size(); ++k) extra += sorted[k].second;
                                            roster += " (+" + std::to_string(extra) + ")";
                                        }
                                        if (!roster.empty()) tc.lines.push_back(std::move(roster));
                                        if (anyHold) tc.lines.push_back(ss2::tr("Garrison holding fire"));
                                        if (anyBino) tc.lines.push_back(ss2::tr("Binoculars active"));
                                        if (anySup) tc.lines.push_back(ss2::tr("Garrison suppressed — reload slowed"));
                                        if (anyBlockedInside) {
                                            tc.lines.push_back(ss2::tr("Some weapons cannot fire indoors (backblast)"));
                                        }
                                    }
                                    tc.lines.push_back(ss2::tr("Click: H hold / B optics / U evac / R-click fire"));
                                } else if (isAllyOwner(b.owner)) {
                                    tc.lines.push_back(ss2::tr("Allied Garrison"));
                                } else {
                                    tc.lines.push_back(ss2::tr("Hostile Garrison"));
                                }
                            } else {
                                tc.lines.push_back(ss2::tr("Right-click with infantry to garrison"));
                            }
                            const int critThresh = std::min(
                                unitCatalog ? unitCatalog->buildingDefense().unloadHp : 150,
                                std::max(1, b.maxHp / 4));
                            const int bStage = ss2::assets::buildingDamageStage(b.hp, b.maxHp, b.destroyed);
                            if (b.hp > 0 && b.hp <= critThresh) {
                                tc.lines.push_back(ss2::tr("Critical structure — collapse imminent (U to evacuate)"));
                            } else if (bStage >= 2) {
                                tc.lines.push_back(ss2::tr("Heavily damaged"));
                            } else if (bStage == 1) {
                                tc.lines.push_back(ss2::tr("Damaged"));
                            }
                            if (b.hp < b.maxHp && (b.owner == -1 || b.owner == localOwner())) {
                                tc.lines.push_back(ss2::tr("Right-click with supply truck to repair"));
                            }
                            tooltip.show(tc, mx, my);
                            showTip = true;
                        }
                    }
                }
                if (!showTip && !liveBridges.empty()) {
                    const int simX = int(std::floor(mapX / mapScale));
                    const int simY = int(std::floor(mapY / mapScale));
                    if (const auto brHovered = bridgeAtMap(simX, simY)) {
                        const auto& br = liveBridges[*brHovered];
                        if (isBuildingPointVisible(br.x, br.y)) {
                            ss2::TooltipContent tc;
                            tc.title = ss2::tr("Bridge");
                            tc.gauge = std::clamp(static_cast<float>(br.hp) / static_cast<float>(std::max(1, br.maxHp)),
                                                  0.0f, 1.0f);
                            tc.lines.push_back(ss2::trf("Integrity {0} / {1}",
                                                        {std::to_string(br.hp), std::to_string(br.maxHp)}));
                            if (br.destroyed) {
                                tc.lines.push_back(ss2::tr("Destroyed — impassable"));
                            } else if (br.stage >= 2) {
                                tc.lines.push_back(ss2::tr("Heavily damaged"));
                            } else if (br.stage == 1) {
                                tc.lines.push_back(ss2::tr("Damaged"));
                            } else {
                                tc.lines.push_back(ss2::tr("Intact"));
                            }
                            tc.lines.push_back(ss2::tr(
                                (br.hp < br.maxHp)
                                    ? "Right-click with supply truck to repair"
                                    : "Press G (Attack Ground) or bombers to demolish"));
                            tooltip.show(tc, mx, my);
                            showTip = true;
                        }
                    }
                }
                if (!showTip && !liveVisibleMines.empty()) {
                    const int simX = int(std::floor(mapX / mapScale));
                    const int simY = int(std::floor(mapY / mapScale));
                    const ss2::VisibleMine* bestMine = nullptr;
                    long long bestD2 = 18 * 18;
                    for (const auto& vm : liveVisibleMines) {
                        const long long dx = vm.x - simX;
                        const long long dy = vm.y - simY;
                        const long long d2 = dx * dx + dy * dy;
                        if (d2 <= bestD2) {
                            bestD2 = d2;
                            bestMine = &vm;
                        }
                    }
                    if (bestMine) {
                        ss2::TooltipContent tc;
                        tc.title = ss2::tr(bestMine->antiTank ? "Anti-Tank Mine" : "Anti-Personnel Mine");
                        tc.lines.push_back(ss2::tr(
                            bestMine->hostile ? "Detected hostile mine — right-click with Sapper to disarm"
                                              : "Friendly mine"));
                        tooltip.show(tc, mx, my);
                        showTip = true;
                    }
                }
            } else if (hud) {
                if (const auto ht = hud->tooltipAtWindow(mx, my)) {
                    tooltip.show({ht->title, ht->lines, -1.0f}, mx, my);
                    showTip = true;
                }
            }
        }
        if (!showTip) tooltip.hide();
    }
    if (audio) audio->updateMusic();
    if (orderNotice.advance(noticeFrame - lastNoticeFrame) && paused && !menu.active()) showPause();
    lastNoticeFrame = noticeFrame;
    tooltip.draw(renderer);
    orderNotice.draw(renderer);
    briefing.draw(renderer);
    menu.draw(renderer);
    if (renderer.present(empty)) {
        ++frames;
        if (timing && sample.frame()) return true;
    } else {
        SDL_Delay(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED ? 30 : 5);
    }
    return false;
}

} // namespace ss2
