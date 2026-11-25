#include "assets/sound_bank.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

void pushU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(std::uint8_t(v & 0xffu));
    out.push_back(std::uint8_t((v >> 8) & 0xffu));
    out.push_back(std::uint8_t((v >> 16) & 0xffu));
    out.push_back(std::uint8_t((v >> 24) & 0xffu));
}

ss2::assets::SoundBank buildSyntheticBank(const std::vector<std::string>& groupNames) {
    std::vector<std::uint8_t> hdr;
    pushU32(hdr, static_cast<std::uint32_t>(groupNames.size()));
    for (std::size_t i = 0; i < groupNames.size(); ++i) {
        char name[64]{};
        std::strncpy(name, groupNames[i].c_str(), sizeof(name) - 1);
        hdr.insert(hdr.end(), name, name + 64);
        pushU32(hdr, 1u);
        pushU32(hdr, static_cast<std::uint32_t>(i));
    }

    std::vector<std::uint8_t> rus;
    const std::uint32_t clipCount = static_cast<std::uint32_t>(groupNames.size());
    pushU32(rus, clipCount);
    const std::uint32_t tableEnd = 4u + (clipCount + 1u) * 4u;
    constexpr std::uint32_t kClipStride = 16u; // 12-byte header + 4 PCM bytes
    for (std::uint32_t i = 0; i <= clipCount; ++i) {
        pushU32(rus, tableEnd + i * kClipStride);
    }
    for (std::uint32_t i = 0; i < clipCount; ++i) {
        pushU32(rus, 22050u);
        pushU32(rus, 16u);
        pushU32(rus, 0u);
        rus.push_back(std::uint8_t((i + 1u) & 0xffu));
        rus.push_back(0x02u);
        rus.push_back(0x03u);
        rus.push_back(0x04u);
    }
    return ss2::assets::SoundBank::fromMemory(hdr, rus);
}

void testSyntheticBank() {
    const auto bank = buildSyntheticBank({"rifle"});
    assert(bank.groupCount() == 1);
    assert(bank.clipCount() == 1);
    assert(bank.variantCount("RIFLE") == 1);
    const auto* c = bank.clip("Rifle", 0);
    assert(c != nullptr);
    assert(c->sampleRate == 22050 && c->bitsPerSample == 16 && !c->loop);
    assert(c->pcm.size() == 4 && c->pcm[0] == 0x01 && c->pcm[3] == 0x04);
    assert(bank.clip("nonexistent") == nullptr);

    // `$`-prefixed event/narrator cues and `@`-prefixed ambient loops resolve without prefix.
    const auto prefixedBank = buildSyntheticBank({"$e_test", "@river_test"});
    assert(prefixedBank.groupCount() == 2);
    assert(prefixedBank.variantCount("e_test") == 1);
    assert(prefixedBank.clip("e_test", 0) != nullptr);
    assert(prefixedBank.variantCount("$e_test") == 1);
    assert(prefixedBank.variantCount("river_test") == 1);
    assert(prefixedBank.clip("river_test", 0) != nullptr);
}

void testSyntheticFallbackWithoutDescriptors() {
    // Synthetic banks created via `fromMemory` without `desc_common.sue` must fall back
    // cleanly to `ack_deu`, `ack_rus`, and `ack_ally` while suppressing generic acks on `noammo`.
    const auto bank = buildSyntheticBank({"ack_deu", "ack_rus", "ack_ally", "d_vet_select"});
    assert(!bank.hasVoiceTables());
    assert(bank.resolveUnitVoice("german", "soldier", "move", false) == "ack_deu");
    assert(bank.resolveUnitVoice("german", "soldier", "select", true) == "d_vet_select");
    assert(bank.resolveUnitVoice("russian", "tank", "attack", false) == "ack_rus");
    assert(bank.resolveUnitVoice("british", "officer", "select", false) == "ack_ally");
    assert(bank.resolveUnitVoice("french", "fsold", "move", false) == "ack_ally");
    assert(bank.resolveUnitVoice("italian", "itank", "move", false) == "ack_deu");
    assert(bank.resolveUnitVoice("german", "soldier", "noammo", false).empty());
    assert(bank.resolveUnitVoice("german", "soldier", "close", false).empty());
}

void testSyntheticDescriptorParserAndResolution() {
    auto bank = buildSyntheticBank({
        "d_select", "d_vet_select", "d_move", "d_vet_move", "d_attack", "d_vet_attack",
        "d_stop", "d_vet_ack", "d_vet_search", "d_getmine", "d_laymine", "d_sap_mine",
        "d_sap_search", "d_sap_repair", "d_med_select", "d_med_move", "d_med_ack",
        "d_underattack", "d_close", "d_noammo",
        "f_sold_select", "f_sold_move", "f_sold_attack", "f_tank_select", "f_tank_stop",
        "b_sold_ack", "b_off_sele", "b_sap_search",
        "i_sold_select", "i_sold_move", "i_tank_select", "i_tank_pickup",
        "r_sold_assault", "r_sold_ack", "r_off_mine", "r_off_select",
        "screamofpain", "ack_deu", "ack_rus", "ack_ally"
    });

    constexpr std::string_view kSoundTypes =
        "none\ngun\nofficer\npilot\nplane\nsapper\nsoldier\ntank\ntransport\nftank\nfsold\nitank\nisold\nspec\n";
    constexpr std::string_view kSoundSchemes =
        "NONE ; comment\nrifle\nsmg_burst\ndefaultexplosion\n";
    constexpr std::string_view kVoiceList =
        "NONE ; Pocomaxa\ncustom_voice_marker\n";

    std::map<std::string, std::string> includes{
        {"misc\\german", R"(
sayselect soldier german 1 d_select
sayselect soldier german 1 d_vet_select
sayack soldier german 1 move d_move
sayack soldier german 1 goin d_move
sayack soldier german 2 goin d_vet_move
sayack soldier german 1 attack_unit d_attack
sayack soldier german 2 attack_unit d_vet_attack
sayack soldier german 1 stop d_stop
sayack soldier german 2 stop d_vet_ack
sayack soldier german 2 attack_move d_vet_ack
sayack soldier german 1 lay_mine d_laymine
sayack soldier german 1 get_mine d_getmine
sayack soldier german 2 get_mine d_vet_search
sayack sapper german 1 fixmost d_sap_repair
sayack sapper german 1 reload d_sap_repair
scream soldier german 1 1 underattack d_underattack
scream soldier german 1 1 outofammo d_noammo
scream soldier german 1 1 closetodeath d_close 60
scream soldier german 2 1 closetodeath none
diesound soldier german screamofpain
diesound officer german none
)"},
        {"misc\\british", R"(
sayselect officer british 1 b_off_sele 5
sayack soldier british 1 stop b_sold_ack
sayack soldier british 1 get_mine b_sap_search
)"},
        {"misc\\french", R"(
sayselect fsold british 1 f_sold_select
sayack fsold british 1 move f_sold_move
sayack fsold british 1 attack_unit f_sold_attack
sayselect ftank british 1 f_tank_select
sayack ftank british 1 stop f_tank_stop
)"},
        {"misc\\italian", R"(
sayselect isold german 1 i_sold_select
sayack isold german 1 move i_sold_move
sayselect itank german 1 i_tank_select
sayack itank german 1 pickup i_tank_pickup
)"},
        {"misc\\russian", R"(
sayack soldier russian 1 move r_sold_ack
sayack soldier russian 1 attack_move r_sold_assault
sayselect officer russian 1 r_off_select
sayack spec russian 1 get_mine r_off_mine 30
)"}
    };

    constexpr std::string_view kMiscSounds = R"(
defaultpriority 0.45
defaultdelay 12
-
#misc\german
#misc\british
#misc\french
#misc\italian
#misc\russian
)";

    bank.loadVoiceTablesFromText(
        kMiscSounds, kSoundTypes, kSoundSchemes, kVoiceList,
        [&](std::string_view key) -> std::string {
            const auto it = includes.find(std::string(key));
            return it == includes.end() ? std::string{} : it->second;
        });

    assert(bank.hasVoiceTables());
    assert(std::abs(bank.defaultVoicePriority() - 0.45f) < 1e-4f);
    assert(bank.defaultVoiceDelaySec() == 12);
    assert(bank.soundTypeCount() == 14);
    assert(bank.hasSoundType("fsold") && bank.hasSoundType("itank") && bank.hasSoundType("spec"));
    assert(bank.soundSchemeCount() == 3);
    assert(bank.hasSoundScheme("rifle") && !bank.hasSoundScheme("none"));
    assert(bank.hasDeclaredVoice("custom_voice_marker"));

    // Regular vs veteran German infantry (including rank-1 `d_vet_select` fix & explicit `none` on close).
    assert(bank.resolveUnitVoice("german", "soldier", "select", false) == "d_select");
    assert(bank.resolveUnitVoice("german", "soldier", "select", true) == "d_vet_select");
    assert(bank.resolveUnitVoice("german", "soldier", "attack", false) == "d_attack");
    assert(bank.resolveUnitVoice("german", "soldier", "attack", true) == "d_vet_attack");
    assert(bank.resolveUnitVoice("german", "soldier", "garrison", false) == "d_move");
    assert(bank.resolveUnitVoice("german", "soldier", "garrison", true) == "d_vet_move");
    assert(bank.resolveUnitVoice("german", "soldier", "stop", false) == "d_stop");
    assert(bank.resolveUnitVoice("german", "soldier", "stop", true) == "d_vet_ack");
    assert(bank.resolveUnitVoice("german", "soldier", "crouch", true) == "d_vet_ack");
    assert(bank.resolveUnitVoice("german", "soldier", "attack_move", true) == "d_vet_ack");
    assert(bank.resolveUnitVoice("german", "soldier", "getmine", false) == "d_getmine");
    assert(bank.resolveUnitVoice("german", "soldier", "getmine", true) == "d_vet_search");
    assert(bank.resolveUnitVoice("german", "soldier", "underattack", true) == "d_underattack");
    assert(bank.resolveUnitVoice("german", "soldier", "noammo", true) == "d_noammo");
    assert(bank.resolveUnitVoice("german", "soldier", "close", false) == "d_close");
    assert(bank.resolveUnitVoice("german", "soldier", "close", true).empty());

    // German medic & sapper recordings in SOUNDS.HDR + repair/resupply.
    assert(bank.resolveUnitVoice("german", "med", "select", false) == "d_med_select");
    assert(bank.resolveUnitVoice("german", "medic", "move", false) == "d_med_move");
    assert(bank.resolveUnitVoice("german", "medic", "heal", false) == "d_med_ack");
    assert(bank.resolveUnitVoice("german", "sap", "laymine", false) == "d_sap_mine");
    assert(bank.resolveUnitVoice("german", "sapper", "getmine", false) == "d_sap_search");
    assert(bank.resolveUnitVoice("german", "sapper", "repair", false) == "d_sap_repair");
    assert(bank.resolveUnitVoice("german", "sapper", "resupply", false) == "d_sap_repair");

    // French (`fsold` / `ftank` + British Allied fallback when a verb is absent in `MISC\french`).
    assert(bank.resolveUnitVoice("french", "soldier", "select", false) == "f_sold_select");
    assert(bank.resolveUnitVoice("french", "fsold", "move", false) == "f_sold_move");
    assert(bank.resolveUnitVoice("french", "soldier", "attack", false) == "f_sold_attack");
    assert(bank.resolveUnitVoice("french", "soldier", "stop", false) == "b_sold_ack");
    assert(bank.resolveUnitVoice("french", "officer", "getmine", false) == "b_sap_search");
    assert(bank.resolveUnitVoice("french", "tank", "select", false) == "f_tank_select");
    assert(bank.resolveUnitVoice("french", "ftank", "stop", false) == "f_tank_stop");

    // Italian (`isold` / `itank` + German Axis fallback when absent in `MISC\italian`).
    assert(bank.resolveUnitVoice("italian", "soldier", "select", false) == "i_sold_select");
    assert(bank.resolveUnitVoice("italian", "isold", "move", false) == "i_sold_move");
    assert(bank.resolveUnitVoice("italian", "tank", "select", false) == "i_tank_select");
    assert(bank.resolveUnitVoice("italian", "itank", "embark", false) == "i_tank_pickup");
    assert(bank.resolveUnitVoice("italian", "sapper", "repair", false) == "d_sap_repair");

    // Russian assault (`attack_move`) and General `spec` -> `officer` fallback chain.
    assert(bank.resolveUnitVoice("russian", "soldier", "attack_move", false) == "r_sold_assault");
    assert(bank.resolveUnitVoice("russian", "spec", "getmine", false) == "r_off_mine");
    assert(bank.resolveUnitVoice("russian", "spec", "select", false) == "r_off_select");

    // Death sound (`diesound`) resolution including explicit `none` on officers.
    assert(bank.resolveDeathSound("german", "soldier") == "screamofpain");
    assert(bank.resolveDeathSound("german", "officer").empty());
}

void testRetailGoldBank(const std::filesystem::path& gameDir) {
    const auto bank = ss2::assets::SoundBank::load(gameDir);
    assert(bank.groupCount() == 330);
    assert(bank.clipCount() == 693);
    assert(bank.hasVoiceTables());
    assert(bank.soundTypeCount() == 14);
    assert(bank.soundSchemeCount() == 79);
    assert(bank.hasSoundScheme("rifle") && bank.hasSoundScheme("smg_burst") &&
           bank.hasSoundScheme("defaultexplosion") && bank.hasSoundScheme("wheel_move"));

    const auto* rifle = bank.clip("rifle", 0);
    assert(rifle && rifle->sampleRate == 22050 && rifle->bitsPerSample == 16 && !rifle->pcm.empty());
    assert(bank.variantCount("e_telegram") > 0);
    assert(bank.clip("e_victory", 0) != nullptr);

    // Exact retail spot-checks across nations, veterans, French/Italian, officers, sappers, medics, planes.
    assert(bank.resolveUnitVoice("german", "soldier", "select", false) == "d_select");
    assert(bank.resolveUnitVoice("german", "soldier", "select", true) == "d_vet_select");
    assert(bank.resolveUnitVoice("german", "soldier", "attack", true) == "d_vet_attack");
    assert(bank.resolveUnitVoice("german", "soldier", "garrison", true) == "d_vet_move");
    assert(bank.resolveUnitVoice("german", "soldier", "stop", true) == "d_vet_ack");
    assert(bank.resolveUnitVoice("german", "soldier", "getmine", true) == "d_vet_search");
    assert(bank.resolveUnitVoice("german", "medic", "select", false) == "d_med_select");
    assert(bank.resolveUnitVoice("german", "sapper", "repair", false) == "d_sap_repair");
    assert(bank.resolveUnitVoice("british", "officer", "select", false) == "b_off_sele");
    assert(bank.resolveUnitVoice("russian", "soldier", "attack_move", false) == "r_sold_assault");
    assert(bank.resolveUnitVoice("russian", "spec", "getmine", false) == "r_off_mine");
    assert(bank.resolveUnitVoice("american", "officer", "getmine", false) == "a_off_mines");
    assert(bank.resolveUnitVoice("french", "soldier", "move", false) == "f_sold_move");
    assert(bank.resolveUnitVoice("french", "tank", "attack", false) == "f_tank_attack");
    assert(bank.resolveUnitVoice("italian", "soldier", "select", false) == "i_sold_select");
    assert(bank.resolveUnitVoice("italian", "tank", "select", false) == "i_tank_select");
    assert(bank.resolveUnitVoice("german", "plane", "select", false) == "d_plane_select");
    assert(bank.resolveUnitVoice("british", "plane", "select", false) == "b_plane_select");

    static constexpr std::array<std::string_view, 7> kNations{
        "german", "russian", "british", "american", "japanese", "french", "italian"
    };
    static constexpr std::array<std::string_view, 7> kRoles{
        "soldier", "officer", "sapper", "medic", "tank", "transport", "gun"
    };
    static constexpr std::array<std::string_view, 13> kVerbs{
        "select", "move", "attack", "attack_move", "stop", "hold",
        "embark", "unload", "garrison", "heal", "binoculars", "crouch", "underattack"
    };

    for (std::string_view nation : kNations) {
        for (std::string_view role : kRoles) {
            for (bool vet : {false, true}) {
                for (std::string_view verb : kVerbs) {
                    const std::string grp = bank.resolveUnitVoice(nation, role, verb, vet);
                    assert(!grp.empty());
                    assert(bank.variantCount(grp) >= 1u);
                    const auto* clip = bank.clip(grp, 0);
                    assert(clip != nullptr && clip->bitsPerSample == 16 && !clip->pcm.empty());
                }
            }
        }
        // Sapper mine-lay, mine-sweep, and repair/resupply must resolve to real PCM clips for every nation.
        for (std::string_view sapVerb : {"laymine", "getmine", "repair", "resupply"}) {
            const std::string grp = bank.resolveUnitVoice(nation, "sapper", sapVerb, false);
            assert(!grp.empty());
            const auto* clip = bank.clip(grp, 0);
            assert(clip != nullptr && !clip->pcm.empty());
        }
    }

    std::cout << "gold_sound_bank: PASS (" << bank.groupCount() << " groups, "
              << bank.clipCount() << " clips, " << bank.soundSchemeCount()
              << " soundschemes, " << bank.soundTypeCount() << " soundtypes)\n";
}

} // namespace

int main(int argc, char** argv) {
    testSyntheticBank();
    testSyntheticFallbackWithoutDescriptors();
    testSyntheticDescriptorParserAndResolution();

    std::filesystem::path retailDir;
    if (argc > 1) {
        retailDir = argv[1];
    } else if (const char* envDir = std::getenv("SS2_PRIVATE_GAME"); envDir && *envDir) {
        retailDir = envDir;
    }
    if (!retailDir.empty() && std::filesystem::is_directory(retailDir)) {
        testRetailGoldBank(retailDir);
        return 0;
    }
    std::cout << "sound_bank: PASS (synthetic)\n";
    return 0;
}
