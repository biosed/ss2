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

std::string GameSession::inferNation(const std::string& id) const {
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id); spec && !spec->nation.empty() && spec->nation != "none") {
            return spec->nation;
        }
    }
    if (!id.empty()) {
        switch (static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(id.front())))) {
        case 'd': return "german";
        case 'r': return "russian";
        case 'b': return "british";
        case 'a': return "american";
        case 'j': return "japanese";
        case 'f': return "french";
        default: break;
        }
    }
    return "german";
}

std::string GameSession::inferVoiceRole(const std::string& id, bool isVehicle) const {
    std::string low = id;
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id)) {
            if (spec->isMedicInfantry()) return "med";
            if (spec->isSupplyTruck()) return "sap";
            if (spec->soundType == "gun") return "art";
            if (spec->soundType == "transport") return "trans";
            if (spec->soundType == "officer") return "off";
            if (spec->soundType == "sapper") return "sap";
            if (spec->soundType == "tank" || spec->soundType == "ftank" || spec->soundType == "itank") return "tank";
            if (spec->soundType == "soldier" || spec->soundType == "fsold" || spec->soundType == "isold") return "sold";
            if (spec->isArtillery()) return "art";
            if (spec->selType == "transport" || spec->selType == "apc" || spec->selType == "jeep") return "trans";
            if (spec->roleClass == "tank" || spec->roleClass == "stug") return "tank";
            if (spec->isOfficerInfantry()) return "off";
            if (spec->selType == "sapper" || spec->selType == "sappers") return "sap";
        }
    }
    if (low.find("officer") != std::string::npos || low.find("general") != std::string::npos) return "off";
    if (low.find("medic") != std::string::npos || low.ends_with(".med")) return "med";
    if (low.find("sapper") != std::string::npos || low.find("miner") != std::string::npos ||
        low.find("sdkfz.11") != std::string::npos || low.find("morris.c8") != std::string::npos) return "sap";
    if (low.find("pak") != std::string::npos || low.find("flak") != std::string::npos ||
        low.find("howitz") != std::string::npos || low.find("mortar") != std::string::npos) return "art";
    if (low.find("opel") != std::string::npos || low.find("truck") != std::string::npos ||
        low.find("bedford") != std::string::npos || low.find("zis") != std::string::npos) return "trans";
    return isVehicle ? "tank" : "sold";
}

void GameSession::playUnitVoiceForId(std::string_view verb, const std::string& id, bool isVehicle, bool force, bool isVeteran) {
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
    const auto* vVoiceSpec = unitCatalog ? unitCatalog->find(id) : nullptr;
    const bool isFrenchVoice = vVoiceSpec &&
        (vVoiceSpec->soundType == "fsold" || vVoiceSpec->soundType == "ftank");
    const bool isItalianVoice = vVoiceSpec && vVoiceSpec->soundType == "isold";
    const std::string pfx = isFrenchVoice ? "f_"
                          : isItalianVoice ? "i_"
                          : (nation == "russian" ? "r_" : nation == "british" ? "b_"
                          : nation == "american" ? "a_" : nation == "japanese" ? "j_"
                          : nation == "french" ? "f_" : "d_");
    const bool suppressGenericAck =
        (verb == "noammo" || verb == "laymine" || verb == "getmine" || verb == "repair" || verb == "close");
    const auto tryPlay = [&](const std::string& g) -> bool {
        const bool urgent = (verb == "underattack" || verb == "close");
        return audio->playGroup(g, urgent ? 0.72f : 0.58f,
                                urgent ? ss2::VoicePriority::High : ss2::VoicePriority::Normal);
    };
    auto tryRole = [&](std::string_view r, std::string_view voicePfx) -> bool {
        const std::string vp(voicePfx);
        if (verb == "close") {
            if (vp == "d_" && tryPlay("d_close")) return true;
            if (tryPlay(vp + "sold_close")) return true;
            return false;
        }
        if (verb == "noammo") {
            if (tryPlay(vp + std::string(r) + "_noammo")) return true;
            if (vp == "d_" && tryPlay("d_noammo")) return true;
            if (tryPlay(vp + "sold_noammo") || tryPlay(vp + "tank_noammo")) return true;
            return false;
        }
        if (verb == "laymine") {
            if (vp == "d_" && tryPlay("d_laymine")) return true;
            if (tryPlay(vp + "sap_mine") || tryPlay(vp + "off_mine") ||
                tryPlay(vp + "off_mines") || tryPlay(vp + "sold_laymine")) return true;
            return false;
        }
        if (verb == "getmine") {
            if (vp == "d_" && tryPlay("d_getmine")) return true;
            if (tryPlay(vp + "sap_search") || tryPlay(vp + "sold_getmine")) return true;
            return false;
        }
        if (verb == "repair") {
            if (tryPlay(vp + "sap_repair") || tryPlay(vp + "tank_repair")) return true;
            return false;
        }
        if (isVeteran && r == "sold" && vp == "d_") {
            if (verb == "select" && tryPlay("d_vet_select")) return true;
            if (verb == "move" && tryPlay("d_vet_move")) return true;
            if (verb == "attack" && tryPlay("d_vet_attack")) return true;
            if (verb == "stop" && tryPlay("d_vet_ack")) return true;
        }
        if (r == "sold" && vp == "d_") {
            if (verb == "select" && tryPlay("d_select")) return true;
            if (verb == "move" && tryPlay("d_move")) return true;
            if (verb == "attack" && tryPlay("d_attack")) return true;
            if (verb == "stop" && tryPlay("d_stop")) return true;
            if (verb == "underattack" && tryPlay("d_underattack")) return true;
        }
        if (r == "off" && vp == "b_" && verb == "select" && tryPlay("b_off_sele")) return true;
        if (r == "tank" && verb == "move" && tryPlay(vp + "tank_ack")) return true;
        if (r == "trans" && verb == "move" && tryPlay(vp + "trans_ack")) return true;
        if (r == "art" && verb == "move" && tryPlay(vp + "art_ack")) return true;
        if (tryPlay(vp + std::string(r) + "_" + std::string(verb))) return true;
        if (verb == "move" && tryPlay(vp + std::string(r) + "_ack")) return true;
        if (verb == "attack" && tryPlay(vp + std::string(r) + "_ack")) return true;
        return false;
    };
    if (tryRole(role, pfx) || (role != "sold" && tryRole("sold", pfx)) ||
        (isFrenchVoice && (tryRole(role, "b_") || (role != "sold" && tryRole("sold", "b_")))) ||
        (!suppressGenericAck &&
         tryPlay(nation == "russian" ? "ack_rus" : nation == "german" ? "ack_deu" : "ack_ally"))) {
        if (verb != "underattack" && verb != "close") lastUnitVoiceAt = now;
    }
}

void GameSession::playSelectionVoice(std::string_view verb) {
    if (!audio || !selection) return;
    if (!selection->selectedVehicles().empty() && vehicleScene) {
        const size_t vid = *selection->selectedVehicles().begin();
        if (vid < vehicleScene->vehicles().size()) {
            playUnitVoiceForId(verb, vehicleScene->vehicles()[vid].id, true);
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
        }
    }
}

std::tuple<std::string, int, float> GameSession::weaponSoundForUnit(const std::string& id) const {
    std::string low = id;
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (low == "dsmguner" || low == "rsmguner" || low == "aofficer") return {"smg_burst", 420, 0.55f};
    if (low.find("hmg") != std::string::npos) return {"smg_burst", 240, 0.50f};
    if (low.find("flam") != std::string::npos) return {"fmw", 450, 0.48f};
    if (low.find("bazooka") != std::string::npos || low.find("panzerfaust") != std::string::npos ||
        low.find("piat") != std::string::npos) {
        return {"bazooka", 360, 0.52f};
    }
    if (low.find("mortar") != std::string::npos) return {"mortar", 450, 0.52f};
    if (low == "dofficer") return {"gunshot", 200, 0.42f};
    if (unitCatalog) {
        if (const auto* spec = unitCatalog->find(id); spec && !spec->shot1.soundGroup.empty()) {
            return {spec->shot1.soundGroup, 260, 0.42f};
        }
    }
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
    const double halfDiag = 0.5 * std::hypot(mx1 - mx0, my1 - my0);
    const float g = ss2::spatialGain(baseGain, static_cast<double>(simX * mapScale) - camX,
                                     static_cast<double>(simY * mapScale) - camY, halfDiag);
    if (g <= 0.0f) return false;
    return audio->playGroup(group, g, prio);
}

} // namespace ss2
