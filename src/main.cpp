#include "assets/mission.hpp"
#include "render/window.hpp"
#include "session/game_session.hpp"
#include "sim/command.hpp"
#include "ui/frontend.hpp"
#include "ui/gold_frontend.hpp"
#include "ui/settings.hpp"
#include "ui/strings.hpp"
#include "video/movie_player.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct SDLSession {
    SDLSession() {
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    }
    ~SDLSession() { SDL_Quit(); }
};

// The Gold frontend shown on ordinary launch, before any mission loads. Reuses the mission
// browser, the Menu/Briefing panels and (through the caller's runWindow loop) the loader, all
// on the same window. Returns the chosen mission path, or nullopt when the user quits.
std::optional<std::filesystem::path> runFrontEnd(SDL_Window* window, ss2::ValidationReport* report,
                                                 const std::filesystem::path& gameDir,
                                                 ss2::UserSettings& settings, bool persistSettings) {
    ss2::VulkanWindow renderer(window, report);
    ss2::GoldFrontEnd goldFe(window, gameDir);
    goldFe.setDifficulty(settings.difficulty);

    std::vector<ss2::GoldMissionEntry> entries;
    for (const auto& m : ss2::assets::listSinglePlayerMissions(gameDir)) {
        std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) +
                          (m.scheme == 2 ? " Beach" : " Summer");
        const std::string key = ss2::normalizeMissionKey(m.path.stem().string());
        const bool completed = settings.completedMissions.contains(key);
        int highScore = 0;
        if (completed) tag = ss2::tr("Completed") + " | " + tag;
        if (const auto sit = settings.missionHighScores.find(key);
            sit != settings.missionHighScores.end() && sit->second > 0) {
            highScore = sit->second;
            tag += " | " + std::to_string(highScore);
        }
        entries.push_back({m.name, m.path.string(), m.description, tag,
                           m.width, m.height, m.scheme, completed, highScore});
    }
    goldFe.setMissions(std::move(entries));

    std::vector<ss2::GoldCampaignEntry> campaignEntries;
    for (const auto& c : ss2::assets::listCampaigns(gameDir)) {
        ss2::GoldCampaignEntry ce;
        ce.name = c.name;
        ce.missions.reserve(c.missions.size());
        for (const auto& m : c.missions) {
            std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) +
                              (m.scheme == 2 ? " Beach" : " Summer");
            const std::string codeKey = ss2::normalizeMissionKey(m.code);
            const std::string stemKey = ss2::normalizeMissionKey(m.path.stem().string());
            const bool completed = settings.completedMissions.contains(codeKey) ||
                                   settings.completedMissions.contains(stemKey);
            if (completed) tag = ss2::tr("Completed") + " | " + tag;
            int bestScore = 0;
            if (const auto sit = settings.missionHighScores.find(codeKey); sit != settings.missionHighScores.end()) {
                bestScore = std::max(bestScore, sit->second);
            }
            if (const auto sit = settings.missionHighScores.find(stemKey); sit != settings.missionHighScores.end()) {
                bestScore = std::max(bestScore, sit->second);
            }
            if (bestScore > 0) tag += " | " + std::to_string(bestScore);
            ce.missions.push_back({m.code, m.title, m.path.string(), m.easyPath.string(),
                                   m.description, tag, m.width, m.height, m.scheme,
                                   completed, bestScore});
        }
        campaignEntries.push_back(std::move(ce));
    }
    goldFe.setCampaigns(std::move(campaignEntries));

    const ss2::Snapshot empty{};
    std::uint64_t lastTick = SDL_GetTicksNS();
    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) return std::nullopt;
            renderer.handleEvent(event);
            goldFe.handleEvent(event);
        }

        const std::uint64_t now = SDL_GetTicksNS();
        goldFe.advance(std::chrono::nanoseconds(now - lastTick));
        lastTick = now;

        const auto screen = goldFe.state().screen();
        if (screen == ss2::FrontEnd::Screen::Launch) {
            if (goldFe.difficulty() != settings.difficulty) {
                settings.difficulty = goldFe.difficulty();
                if (persistSettings) {
                    (void)ss2::saveUserSettings(ss2::defaultSettingsPath(), settings);
                }
            }
            return std::filesystem::path(goldFe.state().launchPath());
        }
        if (screen == ss2::FrontEnd::Screen::Quit) return std::nullopt;

        goldFe.draw(renderer);
        if (!renderer.present(empty)) SDL_Delay(5);
    }
}
}

int main(int argc, char** argv) try {
    std::filesystem::path gameDir, mission, hdPack, recordPath, replayPath;
    std::string campaignQuery;
    int limit = 0;
    bool fourK = false, validate = false, timing = false;
    std::optional<bool> fullscreen;
    bool originalInfantry = false;
    bool autoEngage = true;
    bool cliAutoEngageSet = false;
    bool languageChosen = false;
    bool listMissions = false;
    bool listCampaigns = false;
    bool overviewOnly = false;
    bool noMovies = false;
    std::optional<int> cliDifficulty;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "ss2 [--game-dir DIR] [--mission FILE.ssm|NAME|INDEX] [--campaign QUERY] [--overview] [--list-missions] [--list-campaigns] [--hd-pack DIR] [--original-infantry] [--no-auto-engage] [--difficulty easy|normal|hard] [--record FILE | --replay FILE] [--4k] [--fullscreen | --windowed] [--lang en|fr|es] [--frames N] [--no-movies] [--validate] [--timing]\n"
                         "--game-dir opens Tutorial I (or --mission) from an extracted Gold 2.2 game directory.\n"
                         "--list-missions lists all installed single-player missions, their map sizes and descriptions.\n"
                         "--list-campaigns lists all installed campaigns and missions.\n"
                         "--mission launches a playable single-player mission (file path, display name or 1-based index), or displays its overview if no game directory is given or --overview is set.\n"
                         "--campaign launches a playable campaign mission (e.g. german:1, british:5, 031000, or unique title substring).\n"
                         "--overview displays only the mission's JPEG overview instead of launching playable.\n"
                         "--record writes a deterministic command log and final stateHash to FILE on exit.\n"
                         "--replay replays a recorded command log on a manual clock and verifies its final stateHash.\n"
                         "--no-movies skips the opening logo and intro Bink movies.\n"
                         "--original-infantry uses the original infantry size. The default is 125%.\n"
                         "--no-auto-engage disables automatic firing on spotted enemies (manual attack orders only).\n"
                         "--difficulty selects Easy, Normal (default), or Hard gameplay scaling.\n"
                         "--hd-pack loads an optional 2x HD asset pack (tutorial1_2x.ss2hd) for --game-dir.\n"
                         "--lang sets the interface language (English, French or Spanish). The default follows LC_ALL, LC_MESSAGES or LANG. Mission text stays in English.\n";
            return 0;
        } else if (arg == "--game-dir" && i + 1 < argc) gameDir = argv[++i];
        else if (arg == "--hd-pack" && i + 1 < argc) hdPack = argv[++i];
        else if (arg == "--mission" && i + 1 < argc) mission = argv[++i];
        else if (arg == "--campaign" && i + 1 < argc) campaignQuery = argv[++i];
        else if (arg == "--record" && i + 1 < argc) recordPath = argv[++i];
        else if (arg == "--replay" && i + 1 < argc) replayPath = argv[++i];
        else if (arg == "--list-missions") listMissions = true;
        else if (arg == "--list-campaigns") listCampaigns = true;
        else if (arg == "--overview") overviewOnly = true;
        else if (arg == "--no-movies") noMovies = true;
        else if (arg == "--original-infantry") originalInfantry = true;
        else if (arg == "--no-auto-engage") { autoEngage = false; cliAutoEngageSet = true; }
        else if (arg == "--difficulty" && i + 1 < argc) {
            std::string low(argv[++i]);
            for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low == "easy" || low == "1") cliDifficulty = 1;
            else if (low == "normal" || low == "2") cliDifficulty = 2;
            else if (low == "hard" || low == "3") cliDifficulty = 3;
            else throw std::invalid_argument("--difficulty requires easy, normal or hard");
        }
        else if (arg == "--4k") fourK = true;
        else if (arg == "--fullscreen") fullscreen = true;
        else if (arg == "--windowed") fullscreen = false;
        else if (arg == "--lang" && i + 1 < argc) {
            const auto chosen = ss2::languageFromCode(argv[++i]);
            if (!chosen) throw std::invalid_argument("--lang requires en, fr or es");
            ss2::setLanguage(*chosen);
            languageChosen = true;
        }
        else if (arg == "--validate") validate = true;
        else if (arg == "--timing") timing = true;
        else if (arg == "--frames" && i + 1 < argc) {
            const std::string_view n(argv[++i]);
            const auto [end, err] = std::from_chars(n.data(), n.data() + n.size(), limit);
            if (err != std::errc{} || end != n.data() + n.size() || limit < 1)
                throw std::invalid_argument("--frames requires a positive integer");
        } else throw std::invalid_argument("Unknown or incomplete option: " + std::string(arg));
    }
    if (!languageChosen) ss2::setLanguage(ss2::languageFromEnvironment());
    if (timing && limit) throw std::invalid_argument("Choose either --timing or --frames N");
    if (!recordPath.empty() && !replayPath.empty())
        throw std::invalid_argument("Choose either --record or --replay");
    if (overviewOnly && (!recordPath.empty() || !replayPath.empty()))
        throw std::invalid_argument("--overview cannot be combined with --record or --replay");
    if (timing && !replayPath.empty())
        throw std::invalid_argument("--timing cannot be combined with --replay");
    std::optional<ss2::cmd::ReplayFile> parsedReplay;
    if (!replayPath.empty()) {
        std::ifstream in(replayPath, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open --replay file: " + replayPath.string());
        std::ostringstream buf;
        buf << in.rdbuf();
        parsedReplay = ss2::cmd::fromReplayFileText(buf.str());
        if (!parsedReplay) throw std::runtime_error("Malformed --replay file: " + replayPath.string());
        if (parsedReplay->difficulty && cliDifficulty && *cliDifficulty != *parsedReplay->difficulty)
            throw std::invalid_argument("--difficulty conflicts with recorded difficulty in --replay file");
        if (parsedReplay->autoEngage && cliAutoEngageSet && autoEngage != *parsedReplay->autoEngage)
            throw std::invalid_argument("--no-auto-engage conflicts with recorded auto_engage in --replay file");
    }
    std::filesystem::path dataHome;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) dataHome = xdg;
    else if (const char* home = std::getenv("HOME")) dataHome = std::filesystem::path(home) / ".local/share";
    const auto resolveGameDir = [&]() -> std::filesystem::path {
        if (!gameDir.empty()) return gameDir;
        if (const char* configured = std::getenv("SS2_GAME_DIR"); configured && *configured) return configured;
        if (!dataHome.empty() && std::filesystem::is_directory(dataHome / "ss2/game")) return dataHome / "ss2/game";
        return {};
    };
    if (listMissions) {
        const auto directory = resolveGameDir();
        if (directory.empty())
            throw std::runtime_error("--list-missions needs a game directory: pass --game-dir or set SS2_GAME_DIR");
        const auto installed = ss2::assets::listSinglePlayerMissions(directory);
        std::cout << "Single-player missions (" << installed.size() << "):\n";
        for (std::size_t i = 0; i < installed.size(); ++i) {
            const auto& m = installed[i];
            const char* schemeName = m.scheme == 2 ? "Beach" : "Summer";
            std::cout << ' ' << (i + 1) << "  " << m.name << " [" << m.width << 'x' << m.height << ", " << schemeName << "]\n";
            if (!m.description.empty()) {
                std::string desc = m.description;
                if (desc.size() > 110) { desc.resize(107); desc += "..."; }
                std::cout << "    " << desc << '\n';
            }
        }
        if (installed.empty()) std::cout << "  (none found)\n";
        else std::cout << "Open an overview with --mission NAME or --mission INDEX.\n";
        return 0;
    }
    if (listCampaigns) {
        const auto directory = resolveGameDir();
        if (directory.empty())
            throw std::runtime_error("--list-campaigns needs a game directory: pass --game-dir or set SS2_GAME_DIR");
        const auto installed = ss2::assets::listCampaigns(directory);
        std::cout << "Campaigns (" << installed.size() << "):\n";
        for (const auto& c : installed) {
            std::cout << "  " << c.name << " (" << c.missions.size() << "):\n";
            for (const auto& m : c.missions) {
                const char* schemeName = m.scheme == 2 ? "Beach" : "Summer";
                std::cout << "    [" << m.code << "] " << m.title << " [" << m.width << 'x' << m.height
                          << ", " << schemeName << "]\n";
            }
        }
        if (installed.empty()) std::cout << "  (none found)\n";
        return 0;
    }
    if (parsedReplay && mission.empty() && campaignQuery.empty() && !parsedReplay->mission.empty()) {
        const auto directory = resolveGameDir();
        if (!directory.empty()) {
            const auto candidate = ss2::assets::resolveCaseInsensitivePath(directory, parsedReplay->mission);
            if (!candidate.empty() && std::filesystem::is_regular_file(candidate)) {
                mission = candidate;
            } else {
                mission = parsedReplay->mission;
            }
            if (gameDir.empty()) gameDir = directory;
        }
    }
    if (!campaignQuery.empty()) {
        if (!mission.empty()) throw std::invalid_argument("Choose either --mission or --campaign");
        const auto directory = resolveGameDir();
        if (directory.empty())
            throw std::invalid_argument("--campaign requires a game directory: pass --game-dir or set SS2_GAME_DIR");
        const auto found = ss2::assets::findCampaignMission(directory, campaignQuery);
        if (!found) throw std::invalid_argument("Unknown campaign mission: " + campaignQuery);
        std::cout << "Selected campaign mission: " << found->title << " [" << found->code
                  << ", " << found->width << 'x' << found->height << "] (" << found->path << ")\n";
        mission = found->path;
        if (gameDir.empty() && !overviewOnly) gameDir = directory;
    } else if (!mission.empty() && !std::filesystem::is_regular_file(mission)) {
        const auto directory = resolveGameDir();
        if (directory.empty())
            throw std::invalid_argument("--mission needs a file path, or a game directory to resolve a mission name");
        const auto found = ss2::assets::findSinglePlayerMission(directory, mission.string());
        if (!found) throw std::invalid_argument("Unknown single-player mission: " + mission.string());
        std::cout << "Selected mission: " << found->name << " [" << found->width << 'x' << found->height << "] (" << found->path << ")\n";
        mission = found->path;
        if (gameDir.empty() && !overviewOnly) gameDir = directory;
    }
    if (!hdPack.empty() && resolveGameDir().empty()) throw std::invalid_argument("--hd-pack requires --game-dir");
    if (mission.empty() && !limit && !timing) {
        if (gameDir.empty()) gameDir = resolveGameDir();
        if (!originalInfantry && !gameDir.empty() && hdPack.empty() && !dataHome.empty()) {
            auto installed = dataHome / "ss2/hd-pack";
            if (std::filesystem::is_regular_file(installed / "tutorial1_ground_2x.ss2hd") &&
                std::filesystem::is_directory(installed / "infantry-art")) hdPack = installed;
        }
    }
    if ((!recordPath.empty() || parsedReplay) && gameDir.empty()) {
        gameDir = resolveGameDir();
        if (gameDir.empty())
            throw std::invalid_argument((!recordPath.empty() ? std::string("--record") : std::string("--replay")) +
                                        " requires a game directory: pass --game-dir or set SS2_GAME_DIR");
    }
    const auto settingsPath = ss2::defaultSettingsPath();
    const bool persistSettings = !limit && !timing && !overviewOnly && recordPath.empty() && !parsedReplay &&
                                 !settingsPath.empty();
    ss2::UserSettings settings = persistSettings ? ss2::loadUserSettings(settingsPath) : ss2::UserSettings{};
    if (!languageChosen && settings.language) {
        ss2::setLanguage(*settings.language);
    }
    if (cliDifficulty) {
        settings.difficulty = *cliDifficulty;
    }
    if (parsedReplay) {
        if (parsedReplay->difficulty) settings.difficulty = *parsedReplay->difficulty;
        if (parsedReplay->autoEngage) autoEngage = *parsedReplay->autoEngage;
    }
    SDLSession sdl;
    if (!noMovies && !gameDir.empty() && !limit && !timing && !overviewOnly && !parsedReplay)
        ss2::playOpeningMovies(gameDir, fullscreen.value_or(false), hdPack);
    auto flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen.value_or(false)) flags |= SDL_WINDOW_FULLSCREEN;
    int width = fourK ? 3840 : 1280, height = fourK ? 2160 : 720;
    if (!fullscreen.has_value() && !gameDir.empty() && !limit && !timing && !fourK) {
        SDL_Rect workArea{};
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &workArea)) {
            width = std::max(1, std::min(workArea.w * 4 / 5, workArea.h * 4 / 5 * 16 / 9));
            height = std::max(1, std::max(1, width) * 9 / 16);
        }
    }
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Sudden Strike II | Native renderer", width, height, flags), SDL_DestroyWindow);
    if (!window) throw std::runtime_error(SDL_GetError());
    ss2::ValidationReport report;
    int result = 0;
    if (mission.empty() && !gameDir.empty() && !overviewOnly && !limit && !timing && !parsedReplay) {
        const auto chosen = runFrontEnd(window.get(), validate ? &report : nullptr, gameDir, settings, persistSettings);
        if (!chosen) return 0;
        mission = *chosen;
    }
    for (;;) {
        std::optional<std::filesystem::path> nextMission;
        result = ss2::runGameSession(window.get(), validate ? &report : nullptr, limit, timing, gameDir, mission, hdPack,
                           originalInfantry, autoEngage, settings, persistSettings, overviewOnly, &nextMission,
                           recordPath, parsedReplay ? &*parsedReplay : nullptr);
        if (!nextMission || result != 0 || !recordPath.empty() || parsedReplay) break;
        mission = std::move(*nextMission);
        overviewOnly = false;
    }
    if (validate) {
        std::cout << "Validation errors: " << report.errors.load() << '\n';
        if (report.errors.load()) return 1;
    }
    return result;
} catch (const std::exception& e) {
    std::cerr << "ss2: " << e.what() << '\n';
    return 1;
}
