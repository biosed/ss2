#pragma once

#include "ui/strings.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <wchar.h>
#endif

namespace ss2 {

inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

inline std::filesystem::path getenvPath(const char* name, const wchar_t* wname) {
#ifdef _WIN32
    (void)name;
    if (const wchar_t* val = _wgetenv(wname); val && *val) {
        return std::filesystem::path(val);
    }
#else
    (void)wname;
    if (const char* val = std::getenv(name); val && *val) {
        return std::filesystem::path(val);
    }
#endif
    return {};
}

struct PlayerProfile {
    std::string name;
    std::set<std::string> completedMissions;
    std::map<std::string, int> missionHighScores;
    bool operator==(const PlayerProfile&) const = default;
};

// Persistent player preferences, profiles, and completed mission identifiers.
struct UserSettings {
    std::optional<Language> language;
    float sfxVolume{1.0f};
    float musicVolume{0.28f};
    bool musicMuted{false};
    bool smoothTerrain{false};
    bool enhancedGraphics{true};
    bool combatEffects{true};
    int speedPercent{100};
    int difficulty{2}; // 1 = Easy, 2 = Normal, 3 = Hard
    bool skipIntro{false};
    bool realismDoctrine{false};
    std::set<std::string> completedMissions;
    std::map<std::string, int> missionHighScores;
    std::string activeProfile{"YourID"};
    std::vector<PlayerProfile> profiles;
    std::string activeMod;
    std::string gameDir;
    bool operator==(const UserSettings&) const = default;
};

inline std::string_view trimAscii(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

// Normalizes a campaign mission code or mission stem (uppercase ASCII alnum/._- only).
inline std::string normalizeMissionKey(std::string_view raw) {
    const std::string_view trimmed = trimAscii(raw);
    std::string out;
    out.reserve(trimmed.size());
    for (const char rawCh : trimmed) {
        const unsigned char c = static_cast<unsigned char>(rawCh);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || rawCh == '_' || rawCh == '-' || rawCh == '.') {
            out.push_back(rawCh);
        } else if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        }
    }
    return out;
}

inline std::filesystem::path platformAppConfigRoot() {
    if (const auto xdg = getenvPath("XDG_CONFIG_HOME", L"XDG_CONFIG_HOME"); !xdg.empty()) {
        return xdg / "ss2";
    }
#ifdef _WIN32
    if (const auto appData = getenvPath("APPDATA", L"APPDATA"); !appData.empty()) {
        return appData / "ss2";
    }
    if (const auto profile = getenvPath("USERPROFILE", L"USERPROFILE"); !profile.empty()) {
        return profile / "AppData" / "Roaming" / "ss2";
    }
#else
    if (const auto home = getenvPath("HOME", L"HOME"); !home.empty()) {
        return home / ".config" / "ss2";
    }
#endif
    return {};
}

inline std::filesystem::path platformAppDataRoot() {
    if (const auto xdg = getenvPath("XDG_DATA_HOME", L"XDG_DATA_HOME"); !xdg.empty()) {
        return xdg / "ss2";
    }
#ifdef _WIN32
    if (const auto localAppData = getenvPath("LOCALAPPDATA", L"LOCALAPPDATA"); !localAppData.empty()) {
        return localAppData / "ss2";
    }
    if (const auto appData = getenvPath("APPDATA", L"APPDATA"); !appData.empty()) {
        return appData / "ss2";
    }
    if (const auto profile = getenvPath("USERPROFILE", L"USERPROFILE"); !profile.empty()) {
        return profile / "AppData" / "Local" / "ss2";
    }
#else
    if (const auto home = getenvPath("HOME", L"HOME"); !home.empty()) {
        return home / ".local" / "share" / "ss2";
    }
#endif
    return {};
}

inline std::filesystem::path defaultSettingsPath() {
    const auto root = platformAppConfigRoot();
    return root.empty() ? std::filesystem::path{} : (root / "settings.ini");
}

inline std::filesystem::path defaultReplaysDir() {
    const auto root = platformAppDataRoot();
    return root.empty() ? std::filesystem::path{} : (root / "replays");
}

inline std::filesystem::path defaultSavesDir() {
    const auto root = platformAppDataRoot();
    return root.empty() ? std::filesystem::path{} : (root / "saves");
}

inline std::filesystem::path defaultCustomCampaignsDir() {
    const auto root = platformAppDataRoot();
    return root.empty() ? std::filesystem::path{} : (root / "custom_campaigns");
}

inline std::filesystem::path defaultCampaignPoolsDir() {
    const auto root = platformAppDataRoot();
    return root.empty() ? std::filesystem::path{} : (root / "campaigns");
}

inline std::filesystem::path defaultModsDir() {
    const auto root = platformAppDataRoot();
    return root.empty() ? std::filesystem::path{} : (root / "mods");
}

inline std::filesystem::path defaultHdPackDir() {
    const auto root = platformAppDataRoot();
    return root.empty() ? std::filesystem::path{} : (root / "hd-pack");
}

inline bool hasInstalledHdPackAt(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) return false;
    return std::filesystem::is_directory(dir / "infantry-art-4x", ec) ||
           std::filesystem::is_directory(dir / "vehicles-art-4x", ec) ||
           std::filesystem::is_directory(dir / "infantry-art", ec) ||
           std::filesystem::is_directory(dir / "hud-portraits", ec) ||
           std::filesystem::is_directory(dir / "audio-hd", ec) ||
           std::filesystem::is_directory(dir / "movies-hd", ec) ||
           std::filesystem::is_regular_file(dir / "tutorial1_ground_2x.ss2hd", ec) ||
           std::filesystem::is_regular_file(dir / "tutorial1_2x.ss2hd", ec);
}

inline bool profileNameEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline std::string sanitizeProfileName(std::string_view raw) {
    const std::string_view trimmed = trimAscii(raw);
    std::string out;
    out.reserve(std::min<std::size_t>(trimmed.size(), 24u));
    for (std::size_t i = 0; i < trimmed.size();) {
        const unsigned char c = static_cast<unsigned char>(trimmed[i]);
        if (c < 0x20u || c == 0x7fu || c == '=' || c == ':' || c == ',' ||
            c == ';' || c == '|' || c == '/' || c == '\\') {
            ++i;
            continue;
        }
        std::size_t step = 1u;
        if (c >= 0x80u) {
            if ((c >> 5) == 0x6u) step = 2u;
            else if ((c >> 4) == 0xeu) step = 3u;
            else if ((c >> 3) == 0x1eu) step = 4u;
            else {
                ++i;
                continue;
            }
            if (i + step > trimmed.size()) break;
        }
        if (out.size() + step > 24u) break;
        out.append(trimmed.substr(i, step));
        i += step;
    }
    const std::string_view fin = trimAscii(out);
    return std::string(fin);
}

inline std::size_t findProfileIndex(const UserSettings& s, std::string_view name) {
    for (std::size_t i = 0; i < s.profiles.size(); ++i) {
        if (profileNameEquals(s.profiles[i].name, name)) return i;
    }
    return s.profiles.size();
}

inline void ensureProfilesInitialized(UserSettings& s) {
    std::vector<PlayerProfile> cleaned;
    cleaned.reserve(s.profiles.size());
    for (auto& p : s.profiles) {
        std::string nm = sanitizeProfileName(p.name);
        if (nm.empty()) continue;
        bool dup = false;
        for (const auto& existing : cleaned) {
            if (profileNameEquals(existing.name, nm)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            p.name = std::move(nm);
            cleaned.push_back(std::move(p));
        }
    }
    s.profiles = std::move(cleaned);

    std::string active = sanitizeProfileName(s.activeProfile);
    if (active.empty()) active = "YourID";
    if (s.profiles.empty()) {
        s.profiles.push_back(PlayerProfile{active, s.completedMissions, s.missionHighScores});
        s.activeProfile = active;
        return;
    }
    std::size_t idx = findProfileIndex(s, active);
    if (idx >= s.profiles.size()) idx = 0u;
    s.activeProfile = s.profiles[idx].name;
    if (s.profiles[idx].completedMissions.empty() && s.profiles[idx].missionHighScores.empty() &&
        (!s.completedMissions.empty() || !s.missionHighScores.empty())) {
        s.profiles[idx].completedMissions = s.completedMissions;
        s.profiles[idx].missionHighScores = s.missionHighScores;
    }
}

inline void syncActiveProfile(UserSettings& s) {
    ensureProfilesInitialized(s);
    const std::size_t idx = findProfileIndex(s, s.activeProfile);
    if (idx < s.profiles.size()) {
        s.profiles[idx].completedMissions = s.completedMissions;
        s.profiles[idx].missionHighScores = s.missionHighScores;
    }
}

inline bool selectProfile(UserSettings& s, std::size_t index) {
    syncActiveProfile(s);
    if (index >= s.profiles.size()) return false;
    s.activeProfile = s.profiles[index].name;
    s.completedMissions = s.profiles[index].completedMissions;
    s.missionHighScores = s.profiles[index].missionHighScores;
    return true;
}

inline bool createProfile(UserSettings& s, std::string_view rawName, std::string* errKey = nullptr) {
    syncActiveProfile(s);
    const std::string clean = sanitizeProfileName(rawName);
    if (clean.empty()) {
        if (errKey) *errKey = "Enter player name";
        return false;
    }
    if (findProfileIndex(s, clean) < s.profiles.size() || s.profiles.size() >= 64u) {
        if (errKey) *errKey = "Profile already exists.";
        return false;
    }
    s.profiles.push_back(PlayerProfile{clean, {}, {}});
    s.activeProfile = clean;
    s.completedMissions.clear();
    s.missionHighScores.clear();
    return true;
}

inline bool renameActiveProfile(UserSettings& s, std::string_view rawName, std::string* errKey = nullptr) {
    syncActiveProfile(s);
    const std::string clean = sanitizeProfileName(rawName);
    if (clean.empty()) {
        if (errKey) *errKey = "Enter player name";
        return false;
    }
    const std::size_t idx = findProfileIndex(s, s.activeProfile);
    if (idx >= s.profiles.size()) return false;
    for (std::size_t i = 0; i < s.profiles.size(); ++i) {
        if (i != idx && profileNameEquals(s.profiles[i].name, clean)) {
            if (errKey) *errKey = "Profile already exists.";
            return false;
        }
    }
    s.profiles[idx].name = clean;
    s.activeProfile = clean;
    return true;
}

inline bool deleteActiveProfile(UserSettings& s, std::string* errKey = nullptr) {
    syncActiveProfile(s);
    if (s.profiles.size() <= 1u) {
        if (errKey) *errKey = "Cannot delete the last profile.";
        return false;
    }
    const std::size_t idx = findProfileIndex(s, s.activeProfile);
    if (idx >= s.profiles.size()) return false;
    s.profiles.erase(s.profiles.begin() + static_cast<std::ptrdiff_t>(idx));
    const std::size_t next = std::min(idx, s.profiles.size() - 1u);
    s.activeProfile = s.profiles[next].name;
    s.completedMissions = s.profiles[next].completedMissions;
    s.missionHighScores = s.profiles[next].missionHighScores;
    return true;
}

inline std::string serializeUserSettings(const UserSettings& input) {
    UserSettings s = input;
    syncActiveProfile(s);
    std::ostringstream out;
    if (s.language) {
        const char* code = *s.language == Language::french ? "fr"
                         : *s.language == Language::spanish ? "es" : "en";
        out << "language=" << code << '\n';
    }
    const long sfxPct = std::lround(std::clamp(s.sfxVolume, 0.0f, 1.0f) * 100.0f);
    const long musPct = std::lround(std::clamp(s.musicVolume, 0.0f, 1.0f) * 100.0f);
    const int speed = (s.speedPercent == 50 || s.speedPercent == 100 || s.speedPercent == 200 || s.speedPercent == 300)
                    ? s.speedPercent : 100;
    const int diff = std::clamp(s.difficulty, 1, 3);
    out << "sfx_volume=" << sfxPct << '\n';
    out << "music_volume=" << musPct << '\n';
    out << "music_muted=" << (s.musicMuted ? 1 : 0) << '\n';
    out << "smooth_terrain=" << (s.smoothTerrain ? 1 : 0) << '\n';
    out << "enhanced_graphics=" << (s.enhancedGraphics ? 1 : 0) << '\n';
    out << "combat_effects=" << (s.combatEffects ? 1 : 0) << '\n';
    out << "speed_percent=" << speed << '\n';
    out << "difficulty=" << diff << '\n';
    out << "skip_intro=" << (s.skipIntro ? 1 : 0) << '\n';
    out << "realism_doctrine=" << (s.realismDoctrine ? 1 : 0) << '\n';
    auto writeCompleted = [&](std::string_view key, const std::set<std::string>& completed) {
        std::set<std::string> normalized;
        for (const auto& m : completed) {
            const auto k = normalizeMissionKey(m);
            if (!k.empty()) normalized.insert(k);
        }
        out << key << '=';
        bool first = true;
        for (const auto& m : normalized) {
            if (!first) out << ',';
            out << m;
            first = false;
        }
        out << '\n';
    };
    auto writeScores = [&](std::string_view key, const std::map<std::string, int>& scores) {
        out << key << '=';
        bool firstScore = true;
        for (const auto& [k, score] : scores) {
            const auto nk = normalizeMissionKey(k);
            if (nk.empty() || score <= 0) continue;
            if (!firstScore) out << ',';
            out << nk << ':' << score;
            firstScore = false;
        }
        out << '\n';
    };
    writeCompleted("completed_missions", s.completedMissions);
    writeScores("high_scores", s.missionHighScores);
    out << "active_profile=" << s.activeProfile << '\n';
    if (!s.activeMod.empty()) {
        out << "active_mod=" << s.activeMod << '\n';
    }
    if (!s.gameDir.empty()) {
        out << "game_dir=" << s.gameDir << '\n';
    }
    out << "profiles=";
    for (std::size_t i = 0; i < s.profiles.size(); ++i) {
        if (i > 0u) out << ',';
        out << s.profiles[i].name;
    }
    out << '\n';
    for (const auto& p : s.profiles) {
        writeCompleted("profile_completed:" + p.name, p.completedMissions);
        writeScores("profile_high_scores:" + p.name, p.missionHighScores);
    }
    return out.str();
}

inline UserSettings parseUserSettings(std::string_view text) {
    UserSettings s{};
    auto parseInt = [](std::string_view val, int& out) -> bool {
        int parsed = 0;
        const auto [ptr, err] = std::from_chars(val.data(), val.data() + val.size(), parsed);
        if (err != std::errc{} || ptr != val.data() + val.size()) return false;
        out = parsed;
        return true;
    };
    auto parseBool = [](std::string_view val, bool& out) -> bool {
        if (val == "1" || val == "true" || val == "TRUE") { out = true; return true; }
        if (val == "0" || val == "false" || val == "FALSE") { out = false; return true; }
        return false;
    };
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
        }
        return true;
    };
    auto parseCompletedList = [](std::string_view val, std::set<std::string>& dst) {
        std::size_t itemPos = 0;
        while (itemPos <= val.size()) {
            const std::size_t comma = val.find(',', itemPos);
            const std::string_view token = (comma == std::string_view::npos)
                ? val.substr(itemPos) : val.substr(itemPos, comma - itemPos);
            const std::string norm = normalizeMissionKey(token);
            if (!norm.empty()) dst.insert(norm);
            if (comma == std::string_view::npos) break;
            itemPos = comma + 1;
        }
    };
    auto parseHighScoreMap = [&](std::string_view val, std::map<std::string, int>& dst) {
        std::size_t itemPos = 0;
        while (itemPos <= val.size()) {
            const std::size_t comma = val.find(',', itemPos);
            const std::string_view token = (comma == std::string_view::npos)
                ? val.substr(itemPos) : val.substr(itemPos, comma - itemPos);
            const std::size_t colon = token.find(':');
            if (colon != std::string_view::npos) {
                const std::string nk = normalizeMissionKey(token.substr(0, colon));
                int sc = 0;
                if (!nk.empty() && parseInt(trimAscii(token.substr(colon + 1)), sc) && sc > 0) {
                    auto& cur = dst[nk];
                    cur = std::max(cur, sc);
                }
            }
            if (comma == std::string_view::npos) break;
            itemPos = comma + 1;
        }
    };

    std::vector<std::string> profileNames;
    std::map<std::string, std::set<std::string>> profileCompleted;
    std::map<std::string, std::map<std::string, int>> profileScores;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view rawLine = (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        const std::string_view line = trimAscii(rawLine);
        if (nl == std::string_view::npos) pos = text.size() + 1;
        else pos = nl + 1;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trimAscii(line.substr(0, eq));
        const std::string_view val = trimAscii(line.substr(eq + 1));

        if (key == "language") {
            if (const auto lang = languageFromCode(val)) s.language = *lang;
        } else if (key == "sfx_volume") {
            int pct = 100;
            if (parseInt(val, pct)) s.sfxVolume = static_cast<float>(std::clamp(pct, 0, 100)) / 100.0f;
        } else if (key == "music_volume") {
            int pct = 28;
            if (parseInt(val, pct)) s.musicVolume = static_cast<float>(std::clamp(pct, 0, 100)) / 100.0f;
        } else if (key == "music_muted") {
            bool b = false;
            if (parseBool(val, b)) s.musicMuted = b;
        } else if (key == "smooth_terrain") {
            bool b = false;
            if (parseBool(val, b)) s.smoothTerrain = b;
        } else if (key == "enhanced_graphics") {
            bool b = true;
            if (parseBool(val, b)) s.enhancedGraphics = b;
        } else if (key == "combat_effects") {
            bool b = true;
            if (parseBool(val, b)) s.combatEffects = b;
        } else if (key == "speed_percent") {
            int sp = 100;
            if (parseInt(val, sp) && (sp == 50 || sp == 100 || sp == 200 || sp == 300)) s.speedPercent = sp;
        } else if (key == "difficulty") {
            int d = 2;
            if (parseInt(val, d) && d >= 1 && d <= 3) s.difficulty = d;
            else if (ieq(val, "easy")) s.difficulty = 1;
            else if (ieq(val, "normal")) s.difficulty = 2;
            else if (ieq(val, "hard")) s.difficulty = 3;
        } else if (key == "skip_intro") {
            bool b = false;
            if (parseBool(val, b)) s.skipIntro = b;
        } else if (key == "realism_doctrine") {
            bool b = false;
            if (parseBool(val, b)) s.realismDoctrine = b;
        } else if (key == "completed_missions") {
            parseCompletedList(val, s.completedMissions);
        } else if (key == "high_scores") {
            parseHighScoreMap(val, s.missionHighScores);
        } else if (key == "active_profile") {
            const std::string clean = sanitizeProfileName(val);
            if (!clean.empty()) s.activeProfile = clean;
        } else if (key == "active_mod") {
            std::string modStr(trimAscii(val));
            if (!modStr.empty() && modStr != "none" && modStr != "vanilla" && modStr != "original") {
                s.activeMod = std::move(modStr);
            }
        } else if (key == "game_dir") {
            std::string gd(trimAscii(val));
            if (!gd.empty()) s.gameDir = std::move(gd);
        } else if (key == "profiles") {
            std::size_t itemPos = 0;
            while (itemPos <= val.size()) {
                const std::size_t comma = val.find(',', itemPos);
                const std::string_view token = (comma == std::string_view::npos)
                    ? val.substr(itemPos) : val.substr(itemPos, comma - itemPos);
                const std::string clean = sanitizeProfileName(token);
                if (!clean.empty()) {
                    bool dup = false;
                    for (const auto& existing : profileNames) {
                        if (profileNameEquals(existing, clean)) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) profileNames.push_back(clean);
                }
                if (comma == std::string_view::npos) break;
                itemPos = comma + 1;
            }
        } else if (key.starts_with("profile_completed:")) {
            const std::string pName = sanitizeProfileName(key.substr(18));
            if (!pName.empty()) parseCompletedList(val, profileCompleted[pName]);
        } else if (key.starts_with("profile_high_scores:")) {
            const std::string pName = sanitizeProfileName(key.substr(20));
            if (!pName.empty()) parseHighScoreMap(val, profileScores[pName]);
        }
    }

    auto findOrAddProfile = [&](const std::string& pName) -> PlayerProfile& {
        for (auto& existing : s.profiles) {
            if (profileNameEquals(existing.name, pName)) return existing;
        }
        s.profiles.push_back(PlayerProfile{pName, {}, {}});
        return s.profiles.back();
    };
    for (const auto& pName : profileNames) {
        (void)findOrAddProfile(pName);
    }
    for (const auto& [pName, completed] : profileCompleted) {
        auto& prof = findOrAddProfile(pName);
        prof.completedMissions.insert(completed.begin(), completed.end());
    }
    for (const auto& [pName, scores] : profileScores) {
        auto& prof = findOrAddProfile(pName);
        for (const auto& [k, sc] : scores) {
            auto& cur = prof.missionHighScores[k];
            cur = std::max(cur, sc);
        }
    }
    const bool hadProfileData = !profileNames.empty() || !profileCompleted.empty() || !profileScores.empty();
    ensureProfilesInitialized(s);
    if (hadProfileData) {
        const std::size_t activeIdx = findProfileIndex(s, s.activeProfile);
        if (activeIdx < s.profiles.size()) {
            const bool activeHadOwnKeys = profileCompleted.contains(s.profiles[activeIdx].name) ||
                                          profileScores.contains(s.profiles[activeIdx].name) ||
                                          s.profiles.size() > 1u;
            if (activeHadOwnKeys) {
                s.completedMissions = s.profiles[activeIdx].completedMissions;
                s.missionHighScores = s.profiles[activeIdx].missionHighScores;
            }
        }
    }
    return s;
}

inline UserSettings loadUserSettings(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseUserSettings(ss.str());
}

inline bool saveUserSettings(const std::filesystem::path& path, const UserSettings& s) {
    if (path.empty()) return false;
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string data = serializeUserSettings(s);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    return static_cast<bool>(out);
}

} // namespace ss2
