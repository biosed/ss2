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

namespace ss2 {

// Persistent player preferences and completed mission identifiers.
struct UserSettings {
    std::optional<Language> language;
    float sfxVolume{1.0f};
    float musicVolume{0.28f};
    bool musicMuted{false};
    bool smoothTerrain{false};
    bool combatEffects{true};
    int speedPercent{100};
    int difficulty{2}; // 1 = Easy, 2 = Normal, 3 = Hard
    std::set<std::string> completedMissions;
    std::map<std::string, int> missionHighScores;
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

inline std::filesystem::path defaultSettingsPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "ss2" / "settings.ini";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "ss2" / "settings.ini";
    }
    return {};
}

inline std::string serializeUserSettings(const UserSettings& s) {
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
    out << "combat_effects=" << (s.combatEffects ? 1 : 0) << '\n';
    out << "speed_percent=" << speed << '\n';
    out << "difficulty=" << diff << '\n';
    std::set<std::string> normalized;
    for (const auto& m : s.completedMissions) {
        const auto k = normalizeMissionKey(m);
        if (!k.empty()) normalized.insert(k);
    }
    out << "completed_missions=";
    bool first = true;
    for (const auto& m : normalized) {
        if (!first) out << ',';
        out << m;
        first = false;
    }
    out << '\n';
    out << "high_scores=";
    bool firstScore = true;
    for (const auto& [k, score] : s.missionHighScores) {
        const auto nk = normalizeMissionKey(k);
        if (nk.empty() || score <= 0) continue;
        if (!firstScore) out << ',';
        out << nk << ':' << score;
        firstScore = false;
    }
    out << '\n';
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
        } else if (key == "completed_missions") {
            std::size_t itemPos = 0;
            while (itemPos <= val.size()) {
                const std::size_t comma = val.find(',', itemPos);
                const std::string_view token = (comma == std::string_view::npos)
                    ? val.substr(itemPos) : val.substr(itemPos, comma - itemPos);
                const std::string norm = normalizeMissionKey(token);
                if (!norm.empty()) s.completedMissions.insert(norm);
                if (comma == std::string_view::npos) break;
                itemPos = comma + 1;
            }
        } else if (key == "high_scores") {
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
                        auto& cur = s.missionHighScores[nk];
                        cur = std::max(cur, sc);
                    }
                }
                if (comma == std::string_view::npos) break;
                itemPos = comma + 1;
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
