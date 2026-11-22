// Interface string table: lookup, fallback, placeholders, language codes and table hygiene.
#include "ui/settings.hpp"
#include "ui/strings.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
// The {n} placeholders used in a string, as a set of digits.
std::set<char> placeholders(const std::string& text) {
    std::set<char> found;
    for (std::size_t i = 0; i + 2 < text.size(); ++i)
        if (text[i] == '{' && text[i + 1] >= '0' && text[i + 1] <= '9' && text[i + 2] == '}') found.insert(text[i + 1]);
    return found;
}
bool validUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + extra >= s.size() + (extra ? 0 : 1)) return false;
        for (int k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        i += std::size_t(extra) + 1;
    }
    return true;
}
}

int main() {
    using namespace ss2;
    try {
        check(language() == Language::english, "English is the default");
        check(tr("Stop") == "Stop" && tr("Not in the table") == "Not in the table", "English returns the key");

        setLanguage(Language::french);
        check(tr("Objective") == "Objectif" && tr("Officer") == "Officier", "French lookup");
        check(tr("Bridge") == "Pont" && tr("Heavily damaged") == "Gravement endommagé", "French bridge/damage lookup");
        check(trf("Paratroopers dropped ({0} soldiers)!", {"12"}) == "Parachutistes largués (12 soldats) !",
              "French paratrooper format lookup");
        check(tr("Not in the table") == "Not in the table", "unknown text falls back to English");
        check(trf("Move order accepted by {0} of {1} soldiers", {"3", "5"}) == "Ordre de déplacement accepté par 3 soldats sur 5",
              "French sentence with two numbers");
        setLanguage(Language::spanish);
        check(tr("Objective") == "Objetivo" && tr("Resume") == "Reanudar", "Spanish lookup");
        check(tr("Bridge") == "Puente" && tr("Heavily damaged") == "Gravemente dañado", "Spanish bridge/damage lookup");
        check(trf("Reverse move ordered ({0} vehicles).", {"4"}) == "Marcha atrás ordenada (4 vehículos).",
              "Spanish reverse move format lookup");
        check(trf("Health {0} / {1}", {"62", "100"}) == "Salud 62 / 100", "Spanish sentence with two numbers");
        check(trf("Losses {0}", {"4"}) == "Bajas 4", "one placeholder");

        // Placeholders: repeated, out of range, malformed, and text that is not a key.
        setLanguage(Language::english);
        check(trf("{0} and {0} and {1}", {"a", "b"}) == "a and a and b", "a placeholder may repeat");
        check(trf("{0} {3}", {"a"}) == "a {3}", "a placeholder with no argument is left as written");
        check(trf("{x} {} { 0} {0", {"a"}) == "{x} {} { 0} {0", "malformed braces are copied");
        check(trf("plain", {}) == "plain" && trf("", {"a"}).empty(), "no placeholders");
        check(trf("{0}", {"{1}"}) == "{1}", "arguments are not expanded again");

        // Language codes.
        check(languageFromCode("fr") == Language::french && languageFromCode("FR_fr.UTF-8") == Language::french, "French codes");
        check(languageFromCode("es-MX") == Language::spanish && languageFromCode("Spanish") == Language::spanish, "Spanish codes");
        check(languageFromCode("en_GB.UTF-8") == Language::english && languageFromCode("C") == Language::english, "English codes");
        check(!languageFromCode("de_DE") && !languageFromCode("") && !languageFromCode("12"), "unknown codes");
        setenv("LC_ALL", "", 1);
        setenv("LC_MESSAGES", "es_ES.UTF-8", 1);
        setenv("LANG", "fr_FR.UTF-8", 1);
        check(languageFromEnvironment() == Language::spanish, "LC_MESSAGES comes before LANG");
        setenv("LC_ALL", "fr_CA", 1);
        check(languageFromEnvironment() == Language::french, "LC_ALL comes first");
        setenv("LC_ALL", "de_DE.UTF-8", 1);
        check(languageFromEnvironment() == Language::english, "an unknown language means English");
        unsetenv("LC_ALL"); unsetenv("LC_MESSAGES"); unsetenv("LANG");
        check(languageFromEnvironment() == Language::english, "nothing set means English");

        // Every entry: unique key, both translations present, valid UTF-8, one line, and
        // the same placeholders as the English so no number can go missing.
        std::set<std::string> keys;
        check(translationCount() > 140, "table is populated with full UI coverage");
        check(translationKey(translationCount()).empty(), "index past the end is empty");
        for (std::size_t i = 0; i < translationCount(); ++i) {
            const std::string key(translationKey(i));
            check(!key.empty() && keys.insert(key).second, "duplicate or empty key: " + key);
            for (auto lang : {Language::french, Language::spanish}) {
                setLanguage(lang);
                const auto text = tr(key);
                check(!text.empty() && validUtf8(text), "bad text for: " + key);
                check(text.find('\n') == std::string::npos && text.find('\r') == std::string::npos, "line break in: " + key);
                check(placeholders(text) == placeholders(key), "placeholders differ for: " + key);
                check(text.front() != ' ' && text.back() != ' ', "stray space in: " + key);
            }
        }
        setLanguage(Language::english);

        // UserSettings serialize/parse/file round-trip & Mission completion normalization.
        {
            UserSettings us;
            us.language = Language::french;
            us.sfxVolume = 0.75f;
            us.musicVolume = 0.40f;
            us.musicMuted = true;
            us.smoothTerrain = true;
            us.combatEffects = false;
            us.speedPercent = 200;
            us.difficulty = 3;
            us.skipIntro = true;
            us.completedMissions = {"022000", "barbarossa"};
            us.missionHighScores = {{"022000", 4250}, {"barbarossa", 9100}};
            const std::string ini = serializeUserSettings(us);
            const UserSettings parsed = parseUserSettings(ini);
            check(parsed.language == Language::french, "settings language round-trip");
            check(std::abs(parsed.sfxVolume - 0.75f) < 0.01f, "settings sfxVolume round-trip");
            check(std::abs(parsed.musicVolume - 0.40f) < 0.01f, "settings musicVolume round-trip");
            check(parsed.musicMuted && parsed.smoothTerrain && !parsed.combatEffects, "settings booleans round-trip");
            check(parsed.speedPercent == 200 && parsed.difficulty == 3 && parsed.skipIntro,
                  "settings speed, difficulty & skipIntro round-trip");
            check(parsed.completedMissions.contains("022000") && parsed.completedMissions.contains("BARBAROSSA"),
                  "settings completed missions normalized & preserved");
            check(parsed.missionHighScores.at("022000") == 4250 && parsed.missionHighScores.at("BARBAROSSA") == 9100,
                  "settings mission high scores normalized & preserved");

            const UserSettings messy = parseUserSettings(
                "# comment\nsfx_volume=999\ndifficulty=9\nspeed_percent=77\nmusic_volume=18\n");
            check(std::abs(messy.sfxVolume - 1.0f) < 0.001f && messy.difficulty == 2 && messy.speedPercent == 100 &&
                  !messy.skipIntro && std::abs(messy.musicVolume - 0.18f) < 0.01f,
                  "out-of-range settings clamp or ignore safely");

            const auto tmpDir = std::filesystem::temp_directory_path() / "ss2-settings-test";
            const auto tmpPath = tmpDir / "settings.ini";
            check(saveUserSettings(tmpPath, us), "saveUserSettings wrote file");
            const UserSettings disk = loadUserSettings(tmpPath);
            check(disk.difficulty == 3 && disk.skipIntro && disk.completedMissions.contains("BARBAROSSA") &&
                  disk.missionHighScores.at("BARBAROSSA") == 9100 &&
                  disk.language == Language::french,
                  "loadUserSettings restored saved file");
            std::error_code ec;
            std::filesystem::remove_all(tmpDir, ec);
        }

        // Multi-profile player settings: legacy migration, create/rename/delete, isolation, INI round-trip.
        {
            const std::string legacyIni =
                "active_difficulty = 1\n"
                "completed_missions = 023000,alpha\n"
                "high_scores = 023000:7500,alpha:4200\n";
            UserSettings cfg = parseUserSettings(legacyIni);
            check(cfg.activeProfile == "YourID" && cfg.profiles.size() == 1 &&
                  cfg.completedMissions.count("023000") && cfg.missionHighScores["023000"] == 7500,
                  "legacy INI migrates progress into default YourID profile");
            check(sanitizeProfileName("  Bad:Name=1/2\\3  ") == "BadName123",
                  "sanitizeProfileName trims and strips delimiter characters");
            check(createProfile(cfg, "Rommel") && cfg.activeProfile == "Rommel" &&
                  cfg.profiles.size() == 2 && cfg.completedMissions.empty() && cfg.missionHighScores.empty(),
                  "createProfile creates isolated empty profile and activates it");
            check(!createProfile(cfg, "rOmMeL") && !createProfile(cfg, "   "),
                  "createProfile rejects duplicates (case-insensitive) and blanks");

            cfg.completedMissions.insert("01_DOVER");
            cfg.missionHighScores["01_DOVER"] = 9100;
            check(renameActiveProfile(cfg, "Manstein") && cfg.activeProfile == "Manstein" &&
                  cfg.missionHighScores["01_DOVER"] == 9100,
                  "renameActiveProfile renames active profile and keeps scores");
            check(!renameActiveProfile(cfg, "yourid"),
                  "renameActiveProfile rejects collision with existing profile");

            check(selectProfile(cfg, 0) && cfg.activeProfile == "YourID" &&
                  cfg.missionHighScores["023000"] == 7500 && !cfg.missionHighScores.count("01_DOVER"),
                  "selectProfile restores YourID progress without cross-profile bleed");

            const UserSettings roundTrip = parseUserSettings(serializeUserSettings(cfg));
            check(roundTrip.profiles.size() == 2 && roundTrip.activeProfile == "YourID" &&
                  roundTrip.profiles[0].missionHighScores.at("023000") == 7500 &&
                  roundTrip.profiles[1].name == "Manstein" &&
                  roundTrip.profiles[1].missionHighScores.at("01_DOVER") == 9100,
                  "multi-profile INI round-trip preserves all profiles and scores");

            UserSettings delCfg = roundTrip;
            check(selectProfile(delCfg, 1) && deleteActiveProfile(delCfg) &&
                  delCfg.profiles.size() == 1 && delCfg.activeProfile == "YourID",
                  "deleteActiveProfile removes selected profile and restores remaining profile");
            check(!deleteActiveProfile(delCfg),
                  "deleteActiveProfile refuses to delete the last remaining profile");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
