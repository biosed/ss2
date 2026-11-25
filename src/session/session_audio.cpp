#include "session/game_session.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace ss2 {
namespace {

bool isConcreteNation(std::string_view nat) {
    return nat == "german" || nat == "russian" || nat == "british" ||
           nat == "american" || nat == "japanese" || nat == "french" ||
           nat == "italian";
}

std::string mpSeatNationLower(std::uint8_t code) {
    switch (code) {
    case 0: return "russian";
    case 1: return "german";
    case 2: return "british";
    case 3: return "american";
    case 4: return "japanese";
    default: return "german";
    }
}

std::string lowerAsciiId(std::string_view id) {
    std::string low(id);
    for (char& ch : low) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return low;
}

} // namespace

std::string GameSession::inferNation(const std::string& id) const {
    const std::string low = lowerAsciiId(id);
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id)) {
            if (spec->soundType == "fsold" || spec->soundType == "ftank") return "french";
            if (spec->soundType == "isold" || spec->soundType == "itank") return "italian";
            if (isConcreteNation(spec->nation)) return spec->nation;
        }
    }

    // Aircraft fallback when UnitCatalog is unavailable or planeId is queried directly.
    if (low == "ju-87" || low == "ju-52" || low == "b_ju-52" || low == "he-111" ||
        low == "he_111_agit" || low == "bf-109" || low == "fw-189" || low == "fw190") {
        return "german";
    }
    if (low == "il-2" || low == "jak-3" || low == "la-5" || low == "pe-2" ||
        low == "pe-2_agit" || low == "i-153" || low == "li-2" || low == "b_li-2") {
        return "russian";
    }
    if (low.starts_with("spitfire") || low.starts_with("hurricane") ||
        low.find("mosquito") != std::string::npos || low == "audax" ||
        low == "c47b" || low == "b_c47b") {
        return "british";
    }
    if (low.starts_with("b-17") || low.starts_with("b17") || low == "p-51" ||
        low.starts_with("curtiss") || low == "o-52" || low == "c47" || low == "b_c47") {
        return "american";
    }
    if (low == "a-6m" || low.starts_with("ki-") || low == "g-3m" || low == "n1k1" ||
        low == "c47j" || low == "b_c47j") {
        return "japanese";
    }

    // Shared cross-nation support classes (`cmedic`, `eminer`, `sniper`, `commando`, `crew`, `para`)
    // carry `*nationtype other ...` in `desc_common.sue`; adopt the local player's army nation.
    const bool sharedClass = (low == "cmedic" || low == "eminer" || low == "sniper" ||
                              low == "commando" || low == "crew" || low == "para");
    if (sharedClass) {
        if (multiplayer.enabled) {
            return mpSeatNationLower(multiplayer.playerNations[localOwner() & 3]);
        }
        if (unitCatalog) {
            if (scene) {
                for (const auto& u : scene->units()) {
                    if (u.hp <= 0 || u.owner != localOwner() || u.id == id) continue;
                    if (const auto* s = unitCatalog->find(u.id)) {
                        if (s->soundType == "fsold" || s->soundType == "ftank") return "french";
                        if (s->soundType == "isold" || s->soundType == "itank") return "italian";
                        if (isConcreteNation(s->nation)) return s->nation;
                    }
                }
            }
            if (vehicleScene) {
                for (const auto& v : vehicleScene->vehicles()) {
                    if (!v.alive || v.hp <= 0 || v.owner != localOwner()) continue;
                    if (const auto* s = unitCatalog->find(v.id)) {
                        if (s->soundType == "ftank" || s->soundType == "fsold") return "french";
                        if (s->soundType == "itank" || s->soundType == "isold") return "italian";
                        if (isConcreteNation(s->nation)) return s->nation;
                    }
                }
            }
        }
        return "german";
    }

    if (!low.empty()) {
        switch (static_cast<unsigned char>(low.front())) {
        case 'd': return "german";
        case 'r': return "russian";
        case 'b':
        case 'e': return "british";
        case 'a': return "american";
        case 'j': return "japanese";
        case 'f': return "french";
        case 'i': return "italian";
        default: break;
        }
    }
    return "german";
}

std::string GameSession::inferVoiceRole(const std::string& id, bool isVehicle) const {
    const std::string low = lowerAsciiId(id);
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id)) {
            if (spec->isMedicInfantry()) return "med";
            if (spec->soundType == "fsold" || spec->soundType == "ftank" ||
                spec->soundType == "isold" || spec->soundType == "itank" ||
                spec->soundType == "spec" || spec->soundType == "plane") {
                return spec->soundType;
            }
            if (spec->soundType == "pilot" || spec->selType == "pilot") return "pilot";
            if (spec->isSupplyTruck()) return "sap";
            if (spec->selType == "sapper" || spec->selType == "sappers" || spec->soundType == "sapper") {
                return "sap";
            }
            if (spec->soundType == "officer" || spec->isOfficerInfantry()) return "off";
            if (spec->soundType == "gun") return "art";
            if (spec->soundType == "transport") return "trans";
            if (spec->soundType == "tank") return "tank";
            if (spec->soundType == "soldier") return "sold";
            if (spec->selType == "plane" || spec->roleClass == "plane") return "plane";
            if (spec->isArtillery()) return "art";
            if (spec->isAmbulance() || spec->selType == "transport" ||
                spec->selType == "apc" || spec->selType == "jeep") {
                return "trans";
            }
            if (spec->roleClass == "tank" || spec->roleClass == "stug") return "tank";
        }
    }
    if (low.find("officer") != std::string::npos || low.find("general") != std::string::npos) return "off";
    if (low.find("medic") != std::string::npos || low.ends_with(".med")) return "med";
    if (low.find("sapper") != std::string::npos || low.find("miner") != std::string::npos ||
        low.find("sdkfz.11") != std::string::npos || low.find("morris.c8") != std::string::npos) {
        return "sap";
    }
    if (low.ends_with("pilot")) return "pilot";
    if (low.find("pak") != std::string::npos || low.find("flak") != std::string::npos ||
        low.find("howitz") != std::string::npos || low.find("mortar") != std::string::npos) {
        return "art";
    }
    if (low.find("opel") != std::string::npos || low.find("truck") != std::string::npos ||
        low.find("bedford") != std::string::npos || low.find("zis") != std::string::npos) {
        return "trans";
    }
    return isVehicle ? "tank" : "sold";
}

void GameSession::playUnitVoiceForId(std::string_view verb, const std::string& id,
                                     bool isVehicle, bool force, bool isVeteran) {
    if (!audio || id.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (verb == "underattack" || verb == "close") {
        const int alertIntervalSec = unitCatalog ? unitCatalog->underAttackIntervalSec() : 15;
        if (now - lastUnderAttackAt < std::chrono::seconds(alertIntervalSec)) return;
        lastUnderAttackAt = now;
    } else if (!force && now - lastUnitVoiceAt < std::chrono::milliseconds(650)) {
        return;
    }

    const std::string nation = inferNation(id);
    const std::string role = inferVoiceRole(id, isVehicle);
    const bool urgent = (verb == "underattack" || verb == "close");
    const float gain = urgent ? 0.72f : 0.58f;
    const VoicePriority prio = urgent ? ss2::VoicePriority::High : ss2::VoicePriority::Normal;

    const std::string resolved = audio->bank().resolveUnitVoice(nation, role, verb, isVeteran);
    if (!resolved.empty() && audio->playGroup(resolved, gain, prio)) {
        if (!urgent) lastUnitVoiceAt = now;
        return;
    }

    // Fallback for HD-WAV-only packs when `SoundBank` itself has no embedded groups.
    if (audio->bank().groupCount() == 0u) {
        const std::string pfx = nation == "russian" ? "r_"
                              : nation == "british" ? "b_"
                              : nation == "american" ? "a_"
                              : nation == "japanese" ? "j_"
                              : nation == "french" ? "f_"
                              : nation == "italian" ? "i_"
                              : "d_";
        if (audio->playGroup(pfx + role + "_" + std::string(verb), gain, prio) ||
            audio->playGroup(nation == "russian" ? "ack_rus"
                                                 : (nation == "german" || nation == "italian")
                                                       ? "ack_deu"
                                                       : "ack_ally",
                             gain, prio)) {
            if (!urgent) lastUnitVoiceAt = now;
        }
    }
}

void GameSession::playSelectionVoice(std::string_view verb) {
    if (!audio || !selection) return;
    if (!selection->selectedVehicles().empty() && vehicleScene) {
        const size_t vid = *selection->selectedVehicles().begin();
        if (vid < vehicleScene->vehicles().size()) {
            const bool isVet = (vid < vehExpa.size() && vehExpa[vid] >= 16384);
            playUnitVoiceForId(verb, vehicleScene->vehicles()[vid].id, true, false, isVet);
            if (verb == "select") {
                const auto* vSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles()[vid].id) : nullptr;
                if (vSpec && !vSpec->idleSound.empty()) {
                    (void)audio->playGroup(vSpec->idleSound, 0.18f, ss2::VoicePriority::Low);
                }
            }
            return;
        }
    }
    if (!selection->selected().empty() && scene) {
        const size_t sid = *selection->selected().begin();
        if (sid < scene->units().size()) {
            const bool isVet = (sid < unitExpa.size() && unitExpa[sid] >= 16384);
            playUnitVoiceForId(verb, scene->units()[sid].id, false, false, isVet);
            return;
        }
    }
    if (selectedBuilding && *selectedBuilding < liveBuildings.size() && scene) {
        const auto& sb = liveBuildings[*selectedBuilding];
        if (!sb.destroyed && sb.owner == localOwner() && !sb.occupantIds.empty()) {
            const size_t sid = sb.occupantIds.front();
            if (sid < scene->units().size()) {
                const bool isVet = (sid < unitExpa.size() && unitExpa[sid] >= 16384);
                playUnitVoiceForId(verb, scene->units()[sid].id, false, false, isVet);
            }
        }
    }
}

std::tuple<std::string, int, float> GameSession::weaponSoundForUnit(const std::string& id) const {
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id); spec && !spec->shot1.soundGroup.empty()) {
            if (lowerAsciiId(spec->shot1.soundGroup) != "none") {
                return {spec->shot1.soundGroup, 260, 0.42f};
            }
        }
    }
    const std::string low = lowerAsciiId(id);
    if (low == "dsmguner" || low == "rsmguner" || low == "aofficer") return {"smg_burst", 420, 0.55f};
    if (low.find("hmg") != std::string::npos) return {"mg_tank_burst", 240, 0.50f};
    if (low.find("flam") != std::string::npos) return {"fmw", 450, 0.48f};
    if (low.find("piat") != std::string::npos) return {"piat", 360, 0.52f};
    if (low.find("bazooka") != std::string::npos || low.find("panzerfaust") != std::string::npos ||
        low.find("panzershrek") != std::string::npos) {
        return {"rocket", 360, 0.52f};
    }
    if (low.find("mortar") != std::string::npos) return {"howitzer", 450, 0.52f};
    if (low == "dofficer") return {"gunshot", 200, 0.42f};
    return {"rifle", 180, 0.40f};
}

bool GameSession::playWorldSound(const std::string& group, float baseGain, int simX, int simY, VoicePriority prio) {
    if (!audio) return false;
    int winW = 1, winH = 1;
    SDL_GetWindowSize(window, &winW, &winH);
    const auto [mx0, my0] = renderer.mapPosition(0.0, 0.0);
    const auto [mx1, my1] = renderer.mapPosition(static_cast<double>(winW), static_cast<double>(winH));
    const double camX = 0.5 * (mx0 + mx1);
    const double camY = 0.5 * (my0 + my1);
    const double dx = static_cast<double>(simX * mapScale) - camX;
    const double dy = static_cast<double>(simY * mapScale) - camY;
    const double halfDiag = 0.5 * std::hypot(mx1 - mx0, my1 - my0);
    const double halfWidth = 0.5 * std::abs(mx1 - mx0);
    const float g = ss2::spatialGain(baseGain, dx, dy, halfDiag);
    if (g <= 0.0f) return false;
    const float pan = ss2::viewportStereoPan(dx, halfWidth);
    return audio->playGroup(group, g, prio, pan);
}

} // namespace ss2
