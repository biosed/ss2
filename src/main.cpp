#include "assets/mission.hpp"
#include "net/tcp_transport.hpp"
#include "render/window.hpp"
#include "session/game_session.hpp"
#include "session/save_file.hpp"
#include "sim/command.hpp"
#include "sim/lockstep.hpp"
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

std::filesystem::path resolveReplayMissionPath(const std::filesystem::path& gameDir,
                                               const std::string& recordedMission) {
    if (recordedMission.empty() || gameDir.empty()) return recordedMission;
    const auto candidate = ss2::assets::resolveCaseInsensitivePath(gameDir, recordedMission);
    std::error_code ec;
    if (!candidate.empty() && std::filesystem::is_regular_file(candidate, ec)) return candidate;
    return recordedMission;
}

std::optional<ss2::cmd::ReplayFile> loadReplayFileFromDisk(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return ss2::cmd::fromReplayFileText(buf.str());
}

std::vector<ss2::FrontEnd::ReplayEntry> scanSavedReplays() {
    std::vector<ss2::FrontEnd::ReplayEntry> out;
    const auto dir = ss2::defaultReplaysDir();
    if (dir.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return out;

    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) continue;
        const auto ext = it->path().extension().string();
        if (!ss2::assets::equalsIgnoreCaseAscii(ext, ".ss2rep")) continue;
        const auto parsed = loadReplayFileFromDisk(it->path());
        if (!parsed) continue;

        ss2::FrontEnd::ReplayEntry entry;
        entry.name = it->path().stem().string();
        entry.path = it->path().string();
        entry.mission = parsed->mission;
        entry.commandCount = parsed->commands.size();
        entry.endTick = parsed->endTick;
        entry.difficulty = parsed->difficulty.value_or(2);
        entry.autoEngage = parsed->autoEngage.value_or(true);
        if (parsed->endStateHash) {
            std::ostringstream hs;
            hs << "0x" << std::hex << *parsed->endStateHash;
            entry.hashHex = hs.str();
        }
        out.push_back(std::move(entry));
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

std::filesystem::path resolveSaveMissionPath(
    const std::filesystem::path& gameDir,
    const ss2::SaveFileMetadata& meta,
    const std::filesystem::path& savePath,
    const std::vector<ss2::assets::MissionSummary>& singleMissions,
    const std::vector<ss2::assets::CampaignSummary>& campaigns) {
    if (gameDir.empty()) return {};
    if (!meta.mission.empty()) {
        const auto candidate = ss2::assets::resolveCaseInsensitivePath(gameDir, meta.mission);
        std::error_code ec;
        if (!candidate.empty() && std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    const std::string baseStem = ss2::stripSaveSlotSuffix(savePath.stem().string());
    if (baseStem.empty()) return {};
    if (ss2::assets::equalsIgnoreCaseAscii(baseStem, "tutorial1") ||
        ss2::assets::equalsIgnoreCaseAscii(baseStem, "--- TUTORIAL I ---")) {
        const auto tut = ss2::assets::resolveCaseInsensitivePath(gameDir, "maps/single/--- TUTORIAL I ---.SSM");
        std::error_code ec;
        if (!tut.empty() && std::filesystem::is_regular_file(tut, ec)) return tut;
    }
    for (const auto& m : singleMissions) {
        if (ss2::assets::equalsIgnoreCaseAscii(m.path.stem().string(), baseStem) ||
            ss2::assets::equalsIgnoreCaseAscii(m.name, baseStem)) {
            return m.path;
        }
    }
    for (const auto& c : campaigns) {
        for (const auto& m : c.missions) {
            if (ss2::assets::equalsIgnoreCaseAscii(m.code, baseStem) ||
                ss2::assets::equalsIgnoreCaseAscii(m.path.stem().string(), baseStem)) {
                if (meta.difficulty == 1 && !m.easyPath.empty()) return m.easyPath;
                return m.path;
            }
        }
    }
    return {};
}

std::vector<ss2::FrontEnd::SaveEntry> scanSavedGames(
    const std::filesystem::path& gameDir,
    const std::vector<ss2::assets::MissionSummary>& singleMissions,
    const std::vector<ss2::assets::CampaignSummary>& campaigns) {
    struct SortItem {
        std::int64_t epoch{0};
        ss2::FrontEnd::SaveEntry entry;
    };
    std::vector<SortItem> items;
    const auto dir = ss2::defaultSavesDir();
    if (dir.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return {};

    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) continue;
        const auto ext = it->path().extension().string();
        if (!ss2::assets::equalsIgnoreCaseAscii(ext, ".ss2save")) continue;
        const auto decoded = ss2::readSaveFile(it->path());
        if (!decoded) continue;

        const auto& meta = decoded->meta;
        const std::string fileStem = it->path().stem().string();
        const std::string slotLabel = ss2::inferSaveSlotLabel(fileStem);
        std::string missionTitle = meta.missionTitle;
        const std::string baseStem = ss2::stripSaveSlotSuffix(fileStem);

        if (missionTitle.empty()) {
            const auto resolved = resolveSaveMissionPath(gameDir, meta, it->path(), singleMissions, campaigns);
            const std::string matchStem = !resolved.empty() ? resolved.stem().string() : baseStem;
            for (const auto& m : singleMissions) {
                if (ss2::assets::equalsIgnoreCaseAscii(m.path.stem().string(), matchStem)) {
                    missionTitle = m.name;
                    break;
                }
            }
            if (missionTitle.empty()) {
                for (const auto& c : campaigns) {
                    for (const auto& m : c.missions) {
                        if (ss2::assets::equalsIgnoreCaseAscii(m.code, matchStem) ||
                            ss2::assets::equalsIgnoreCaseAscii(m.path.stem().string(), matchStem)) {
                            missionTitle = m.title;
                            break;
                        }
                    }
                    if (!missionTitle.empty()) break;
                }
            }
            if (missionTitle.empty()) missionTitle = baseStem;
        }

        ss2::FrontEnd::SaveEntry entry;
        entry.name = !missionTitle.empty() ? (missionTitle + " \xE2\x80\x94 " + slotLabel) : fileStem;
        entry.path = it->path().string();
        entry.mission = meta.mission;
        entry.missionTitle = missionTitle;
        entry.slotLabel = slotLabel;
        entry.timestamp = ss2::formatSaveTimestamp(meta.savedEpochSec);
        entry.tick = meta.tick;
        entry.playerSoldiers = meta.playerSoldiers;
        entry.playerVehicles = meta.playerVehicles;
        entry.difficulty = meta.difficulty;
        entry.fileBytes = meta.fileBytes;
        items.push_back(SortItem{meta.savedEpochSec, std::move(entry)});
    }

    std::ranges::sort(items, [](const SortItem& a, const SortItem& b) {
        if (a.epoch != b.epoch) return a.epoch > b.epoch;
        return a.entry.name < b.entry.name;
    });
    std::vector<ss2::FrontEnd::SaveEntry> out;
    out.reserve(items.size());
    for (auto& item : items) out.push_back(std::move(item.entry));
    return out;
}

std::optional<std::uint16_t> parsePortArg(std::string_view text) {
    if (text.empty()) return std::nullopt;
    unsigned v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc{} || end != text.data() + text.size() || v == 0 || v > 65535u) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(v);
}

std::optional<std::pair<std::string, std::uint16_t>> parseJoinEndpoint(std::string_view spec) {
    const auto colon = spec.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    std::string_view host = spec.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    const auto port = parsePortArg(spec.substr(colon + 1));
    if (host.empty() || !port) return std::nullopt;
    return std::pair<std::string, std::uint16_t>{std::string(host), *port};
}

std::filesystem::path resolveMultiplayerMissionFromSetup(const std::filesystem::path& gameDir,
                                                         const ss2::MatchSetupPacket& setup,
                                                         const std::filesystem::path& explicitMission) {
    if (!explicitMission.empty()) {
        const auto md = ss2::assets::readMissionData(explicitMission);
        if (ss2::computeMissionMapHash(md) != setup.mapHash) {
            throw std::runtime_error("Multiplayer map hash mismatch between host and joiner");
        }
        return explicitMission;
    }
    if (gameDir.empty()) {
        throw std::runtime_error("Cannot resolve multiplayer map '" + setup.missionSpec + "' without --game-dir");
    }
    const auto maps = ss2::assets::listMultiplayerMissions(gameDir);
    std::optional<std::filesystem::path> hashOnlyMatch;
    bool sawStemMismatch = false;
    for (const auto& m : maps) {
        try {
            const auto md = ss2::assets::readMissionData(m.path);
            const bool hashOk = (ss2::computeMissionMapHash(md) == setup.mapHash);
            const bool stemOk = !setup.missionSpec.empty() &&
                                ss2::assets::equalsIgnoreCaseAscii(m.path.stem().string(), setup.missionSpec);
            if (hashOk && stemOk) return m.path;
            if (hashOk && !hashOnlyMatch) hashOnlyMatch = m.path;
            if (stemOk && !hashOk) sawStemMismatch = true;
        } catch (...) {
        }
    }
    if (hashOnlyMatch) return *hashOnlyMatch;
    if (sawStemMismatch) {
        throw std::runtime_error("Multiplayer map hash mismatch for '" + setup.missionSpec + "'");
    }
    throw std::runtime_error("Host selected unknown multiplayer map: " + setup.missionSpec);
}

ss2::MultiplayerSessionConfig makeMultiplayerConfig(const ss2::MultiplayerHandshake& hs,
                                                    std::shared_ptr<ss2::TcpConnection> peer) {
    ss2::MultiplayerSessionConfig cfg;
    cfg.enabled = true;
    cfg.isHost = hs.isHost();
    cfg.localSeat = hs.isHost() ? 0 : hs.matchSetup().assignedSeat;
    cfg.playerCount = hs.matchSetup().playerCount;
    cfg.turnTicks = hs.matchSetup().turnTicks;
    cfg.inputDelayTurns = hs.matchSetup().inputDelayTurns;
    cfg.hashIntervalTurns = hs.matchSetup().hashIntervalTurns;
    cfg.playerNations = hs.matchSetup().playerNations;
    cfg.playerNames = hs.matchSetup().playerNames;
    cfg.peer = std::move(peer);
    return cfg;
}

std::optional<std::uint8_t> parseMpNationArg(std::string_view raw) {
    std::string low;
    low.reserve(raw.size());
    for (unsigned char c : raw) low.push_back(static_cast<char>(std::tolower(c)));
    if (low == "russian" || low == "ru" || low == "ussr" || low == "soviet" || low == "0") return 0u;
    if (low == "german" || low == "de" || low == "germany" || low == "1") return 1u;
    if (low == "british" || low == "gb" || low == "uk" || low == "2") return 2u;
    if (low == "us" || low == "usa" || low == "american" || low == "3") return 3u;
    if (low == "japanese" || low == "jp" || low == "japan" || low == "4") return 4u;
    return std::nullopt;
}

struct FrontEndResult {
    std::filesystem::path missionPath;
    std::optional<ss2::cmd::ReplayFile> replayFile;
    std::filesystem::path savePath;
    std::optional<int> saveDifficulty;
    std::optional<ss2::MultiplayerSessionConfig> multiplayerConfig;
};

// The Gold frontend shown on ordinary launch, before any mission loads. Reuses the mission
// browser, the Menu/Briefing/Options/Replay/Load panels and (through the caller's runGameSession loop)
// the loader, all on the same window. Returns the chosen mission/replay/save, or nullopt when quitting.
std::optional<FrontEndResult> runFrontEnd(SDL_Window* window, ss2::ValidationReport* report,
                                          const std::filesystem::path& gameDir,
                                          ss2::UserSettings& settings, bool persistSettings) {
    ss2::VulkanWindow renderer(window, report);
    ss2::GoldFrontEnd goldFe(window, gameDir);
    goldFe.setSettings(settings);

    const auto installedMissions = ss2::assets::listSinglePlayerMissions(gameDir);
    const auto installedCampaigns = ss2::assets::listCampaigns(gameDir);
    goldFe.setReplays(scanSavedReplays());
    goldFe.setSaves(scanSavedGames(gameDir, installedMissions, installedCampaigns));

    const auto installedCustomCampaigns = ss2::assets::listCustomCampaigns(gameDir, ss2::defaultCustomCampaignsDir());
    const auto refreshFrontEndProgress = [&] {
        std::vector<ss2::GoldMissionEntry> entries;
        for (const auto& m : installedMissions) {
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

        auto buildCampaignEntries = [&](const std::vector<ss2::assets::CampaignSummary>& src) {
            std::vector<ss2::GoldCampaignEntry> out;
            out.reserve(src.size());
            for (const auto& c : src) {
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
                    if (const auto sit = settings.missionHighScores.find(codeKey);
                        sit != settings.missionHighScores.end()) {
                        bestScore = std::max(bestScore, sit->second);
                    }
                    if (const auto sit = settings.missionHighScores.find(stemKey);
                        sit != settings.missionHighScores.end()) {
                        bestScore = std::max(bestScore, sit->second);
                    }
                    if (bestScore > 0) tag += " | " + std::to_string(bestScore);
                    ce.missions.push_back({m.code, m.title, m.path.string(), m.easyPath.string(),
                                           m.description, tag, m.width, m.height, m.scheme,
                                           completed, bestScore});
                }
                out.push_back(std::move(ce));
            }
            return out;
        };
        goldFe.setCampaigns(buildCampaignEntries(installedCampaigns));
        goldFe.setCustomCampaigns(buildCampaignEntries(installedCustomCampaigns));
    };
    refreshFrontEndProgress();

    const auto installedMp = ss2::assets::listMultiplayerMissions(gameDir);
    std::vector<ss2::GoldMissionEntry> mpEntries;
    mpEntries.reserve(installedMp.size());
    for (const auto& m : installedMp) {
        std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) +
                          (m.scheme == 2 ? " Beach" : " Summer");
        mpEntries.push_back({m.name, m.path.string(), m.description, std::move(tag),
                             m.width, m.height, m.scheme, false, 0});
    }
    goldFe.setMultiplayerMissions(std::move(mpEntries));

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

        if (goldFe.takeSettingsSaved()) {
            settings = goldFe.settings();
            if (persistSettings) {
                (void)ss2::saveUserSettings(ss2::defaultSettingsPath(), settings);
            }
            refreshFrontEndProgress();
        }
        if (auto delPath = goldFe.takeDeleteSaveRequest()) {
            std::error_code ec;
            const auto savesRoot = std::filesystem::weakly_canonical(ss2::defaultSavesDir(), ec);
            const auto cand = std::filesystem::weakly_canonical(*delPath, ec);
            if (!savesRoot.empty() && cand.parent_path() == savesRoot &&
                std::filesystem::is_regular_file(cand, ec) &&
                std::filesystem::remove(cand, ec)) {
                goldFe.setSaves(scanSavedGames(gameDir, installedMissions, installedCampaigns));
                goldFe.showNotice(ss2::tr("Saved game deleted."));
            } else {
                goldFe.showNotice(ss2::tr("Could not delete saved game."));
            }
        }

        const auto screen = goldFe.state().screen();
        if (screen == ss2::FrontEnd::Screen::Launch) {
            if (goldFe.difficulty() != settings.difficulty) {
                settings.difficulty = goldFe.difficulty();
                if (persistSettings) {
                    (void)ss2::saveUserSettings(ss2::defaultSettingsPath(), settings);
                }
            }
            return FrontEndResult{std::filesystem::path(goldFe.state().launchPath()), std::nullopt, {}, std::nullopt, std::nullopt};
        }
        if (screen == ss2::FrontEnd::Screen::LaunchReplay) {
            if (auto loaded = loadReplayFileFromDisk(goldFe.state().launchReplayPath())) {
                return FrontEndResult{resolveReplayMissionPath(gameDir, loaded->mission), std::move(*loaded), {}, std::nullopt, std::nullopt};
            }
            goldFe.state().back();
        }
        if (screen == ss2::FrontEnd::Screen::LaunchSave) {
            const std::filesystem::path savePath(goldFe.state().launchSavePath());
            if (auto decoded = ss2::readSaveFile(savePath)) {
                const auto resolved = resolveSaveMissionPath(
                    gameDir, decoded->meta, savePath, installedMissions, installedCampaigns);
                if (!resolved.empty()) {
                    return FrontEndResult{
                        resolved,
                        std::nullopt,
                        savePath,
                        decoded->meta.hasMetadata ? std::optional<int>(decoded->meta.difficulty) : std::nullopt,
                        std::nullopt
                    };
                }
            }
            goldFe.handleKey(SDLK_ESCAPE);
            goldFe.showNotice(ss2::tr("Failed to load saved game."));
        }
        if (screen == ss2::FrontEnd::Screen::LaunchMultiplayer) {
            ss2::MultiplayerHandshake hs;
            std::filesystem::path mpMissionPath;
            bool started = false;
            try {
                if (goldFe.state().mpIsHost()) {
                    mpMissionPath = goldFe.state().launchMultiplayerPath();
                    if (mpMissionPath.empty() || !std::filesystem::is_regular_file(mpMissionPath)) {
                        throw std::runtime_error(ss2::tr("No multiplayer maps installed."));
                    }
                    const auto md = ss2::assets::readMissionData(mpMissionPath);
                    ss2::MatchSetupPacket setup;
                    setup.assignedSeat = 1;
                    setup.playerCount = 2;
                    setup.mapHash = ss2::computeMissionMapHash(md);
                    setup.missionSpec = mpMissionPath.stem().string();
                    setup.autoEngage = true;
                    setup.playerNations[0] = goldFe.state().mpNation();
                    setup.playerNames[0] = settings.activeProfile.empty() ? std::string("Player 1") : settings.activeProfile;
                    started = hs.startHost(goldFe.state().mpPort(), std::move(setup));
                } else {
                    const std::string playerName = settings.activeProfile.empty()
                        ? std::string("Player 2") : settings.activeProfile;
                    started = hs.startJoin(goldFe.state().mpHostAddress(), goldFe.state().mpPort(),
                                           playerName, 0u, goldFe.state().mpNation());
                }
            } catch (const std::exception& e) {
                goldFe.handleKey(SDLK_ESCAPE);
                goldFe.showNotice(e.what());
                started = false;
            }
            if (started) {
                std::string prevStatus;
                bool cancelled = false;
                while (hs.state() != ss2::MultiplayerHandshake::State::Ready &&
                       hs.state() != ss2::MultiplayerHandshake::State::Failed) {
                    SDL_Event ev;
                    while (SDL_PollEvent(&ev)) {
                        if (ev.type == SDL_EVENT_QUIT) {
                            hs.cancel();
                            return std::nullopt;
                        }
                        renderer.handleEvent(ev);
                        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_ESCAPE) {
                            hs.cancel();
                            cancelled = true;
                            break;
                        }
                    }
                    if (cancelled) break;
                    hs.poll();
                    if (hs.statusText() != prevStatus && !hs.statusText().empty()) {
                        prevStatus = hs.statusText();
                        goldFe.showNotice(ss2::tr(prevStatus));
                    }
                    goldFe.draw(renderer);
                    if (!renderer.present(empty)) SDL_Delay(10);
                    else SDL_Delay(5);
                }
                if (cancelled) {
                    goldFe.handleKey(SDLK_ESCAPE);
                } else if (hs.state() == ss2::MultiplayerHandshake::State::Ready) {
                    try {
                        if (!hs.isHost()) {
                            mpMissionPath = resolveMultiplayerMissionFromSetup(gameDir, hs.matchSetup(), {});
                        }
                        auto mpCfg = makeMultiplayerConfig(hs, hs.takeConnection());
                        return FrontEndResult{std::move(mpMissionPath), std::nullopt, {}, std::nullopt, std::move(mpCfg)};
                    } catch (const std::exception& e) {
                        goldFe.handleKey(SDLK_ESCAPE);
                        goldFe.showNotice(e.what());
                    }
                } else {
                    goldFe.handleKey(SDLK_ESCAPE);
                    goldFe.showNotice(hs.errorText().empty() ? ss2::tr("Connection to peer lost.") : hs.errorText());
                }
            } else if (!hs.errorText().empty()) {
                goldFe.handleKey(SDLK_ESCAPE);
                goldFe.showNotice(hs.errorText());
            }
        }
        if (screen == ss2::FrontEnd::Screen::Quit) return std::nullopt;

        goldFe.draw(renderer);
        if (!renderer.present(empty)) SDL_Delay(5);
    }
}
}

int main(int argc, char** argv) try {
    std::filesystem::path gameDir, mission, hdPack, recordPath, replayPath, loadSavePath;
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
    bool listMultiplayer = false;
    bool overviewOnly = false;
    bool noMovies = false;
    std::optional<int> cliDifficulty;
    std::optional<std::uint16_t> mpHostPort;
    std::optional<std::pair<std::string, std::uint16_t>> mpJoinEndpoint;
    std::optional<std::uint8_t> cliMpNation;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "ss2 [--game-dir DIR] [--mission FILE.ssm|NAME|INDEX] [--campaign QUERY] [--overview] [--list-missions] [--list-campaigns] [--list-multiplayer] [--host PORT | --join HOST:PORT] [--mp-nation NATION] [--hd-pack DIR] [--original-infantry] [--no-auto-engage] [--difficulty easy|normal|hard] [--record FILE | --replay FILE | --load-save FILE] [--4k] [--fullscreen | --windowed] [--lang en|fr|es] [--frames N] [--no-movies] [--validate] [--timing]\n"
                         "--game-dir opens Tutorial I (or --mission) from an extracted Gold 2.2 game directory.\n"
                         "--list-missions lists all installed single-player missions, their map sizes and descriptions.\n"
                         "--list-campaigns lists all installed campaigns and missions.\n"
                         "--list-multiplayer lists all installed multiplayer missions (maps/mplay/*.smm).\n"
                         "--host PORT hosts a 2-player lockstep TCP match on PORT with --mission.\n"
                         "--join HOST:PORT joins a 2-player lockstep TCP match at HOST:PORT.\n"
                         "--mp-nation NATION selects the multiplayer faction (russian, german, british, us, japanese).\n"
                         "--mission launches a playable mission (file path, display name or 1-based index), or displays its overview if no game directory is given or --overview is set.\n"
                         "--campaign launches a playable campaign mission (e.g. german:1, british:5, 031000, or unique title substring).\n"
                         "--overview displays only the mission's JPEG overview instead of launching playable.\n"
                         "--record writes a deterministic command log and final stateHash to FILE on exit.\n"
                         "--replay replays a recorded command log on a manual clock and verifies its final stateHash.\n"
                         "--load-save launches a saved mission snapshot (.ss2save) directly into playable simulation.\n"
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
        else if (arg == "--load-save" && i + 1 < argc) loadSavePath = argv[++i];
        else if (arg == "--list-missions") listMissions = true;
        else if (arg == "--list-campaigns") listCampaigns = true;
        else if (arg == "--list-multiplayer") listMultiplayer = true;
        else if (arg == "--host" && i + 1 < argc) {
            mpHostPort = parsePortArg(argv[++i]);
            if (!mpHostPort) throw std::invalid_argument("--host requires a TCP port (1..65535)");
        } else if (arg == "--join" && i + 1 < argc) {
            mpJoinEndpoint = parseJoinEndpoint(argv[++i]);
            if (!mpJoinEndpoint) throw std::invalid_argument("--join requires HOST:PORT");
        } else if (arg == "--mp-nation" && i + 1 < argc) {
            cliMpNation = parseMpNationArg(argv[++i]);
            if (!cliMpNation) throw std::invalid_argument("--mp-nation requires russian, german, british, us or japanese");
        }
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
    if (!loadSavePath.empty() && (!recordPath.empty() || !replayPath.empty() || overviewOnly || timing))
        throw std::invalid_argument("--load-save cannot be combined with --record, --replay, --overview or --timing");
    if (mpHostPort && mpJoinEndpoint)
        throw std::invalid_argument("Choose either --host or --join");
    const bool mpRequested = mpHostPort.has_value() || mpJoinEndpoint.has_value();
    if (mpRequested && (!recordPath.empty() || !replayPath.empty() || !loadSavePath.empty() ||
                        overviewOnly || timing || !campaignQuery.empty())) {
        throw std::invalid_argument("--host/--join cannot be combined with --record, --replay, --load-save, --overview, --timing or --campaign");
    }
    if (mpHostPort && mission.empty()) {
        throw std::invalid_argument("--host requires --mission");
    }
    std::optional<ss2::DecodedSaveFile> parsedSave;
    if (!loadSavePath.empty()) {
        parsedSave = ss2::readSaveFile(loadSavePath);
        if (!parsedSave) throw std::runtime_error("Malformed or unreadable --load-save file: " + loadSavePath.string());
    }
    std::optional<ss2::cmd::ReplayFile> parsedReplay;
    if (!replayPath.empty()) {
        parsedReplay = loadReplayFileFromDisk(replayPath);
        if (!parsedReplay) throw std::runtime_error("Malformed or unreadable --replay file: " + replayPath.string());
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
        const auto custom = ss2::assets::listCustomCampaigns(directory, ss2::defaultCustomCampaignsDir());
        if (!custom.empty()) {
            std::cout << "Custom Campaigns (" << custom.size() << "):\n";
            for (const auto& c : custom) {
                std::cout << "  " << c.name << " (" << c.missions.size() << "):\n";
                for (const auto& m : c.missions) {
                    const char* schemeName = m.scheme == 2 ? "Beach" : "Summer";
                    std::cout << "    [" << m.code << "] " << m.title << " [" << m.width << 'x' << m.height
                              << ", " << schemeName << "]\n";
                }
            }
        }
        return 0;
    }
    if (listMultiplayer) {
        const auto directory = resolveGameDir();
        if (directory.empty())
            throw std::runtime_error("--list-multiplayer needs a game directory: pass --game-dir or set SS2_GAME_DIR");
        const auto installed = ss2::assets::listMultiplayerMissions(directory);
        std::cout << "Multiplayer missions (" << installed.size() << "):\n";
        for (std::size_t i = 0; i < installed.size(); ++i) {
            const auto& m = installed[i];
            const char* schemeName = m.scheme == 2 ? "Beach" : "Summer";
            std::cout << ' ' << (i + 1) << "  " << m.name << " [" << m.width << 'x' << m.height
                      << ", " << schemeName << "]\n";
            if (!m.description.empty()) {
                std::string desc = m.description;
                if (desc.size() > 110) { desc.resize(107); desc += "..."; }
                std::cout << "    " << desc << '\n';
            }
        }
        if (installed.empty()) std::cout << "  (none found)\n";
        const auto custom = ss2::assets::listCustomCampaigns(directory, ss2::defaultCustomCampaignsDir());
        if (!custom.empty()) {
            std::cout << "Custom Campaigns (" << custom.size() << "):\n";
            for (const auto& c : custom) {
                std::cout << "  " << c.name << " (" << c.missions.size() << "):\n";
                for (const auto& m : c.missions) {
                    const char* schemeName = m.scheme == 2 ? "Beach" : "Summer";
                    std::cout << "    [" << m.code << "] " << m.title << " [" << m.width << 'x' << m.height
                              << ", " << schemeName << "]\n";
                }
            }
        }
        return 0;
    }
    if (parsedReplay && mission.empty() && campaignQuery.empty() && !parsedReplay->mission.empty()) {
        const auto directory = resolveGameDir();
        if (!directory.empty()) {
            mission = resolveReplayMissionPath(directory, parsedReplay->mission);
            if (gameDir.empty()) gameDir = directory;
        }
    }
    if (!campaignQuery.empty()) {
        if (!mission.empty()) throw std::invalid_argument("Choose either --mission or --campaign");
        const auto directory = resolveGameDir();
        if (directory.empty())
            throw std::invalid_argument("--campaign requires a game directory: pass --game-dir or set SS2_GAME_DIR");
        auto found = ss2::assets::findCampaignMission(directory, campaignQuery);
        if (!found) {
            found = ss2::assets::findCustomCampaignMission(
                directory, campaignQuery, false, ss2::defaultCustomCampaignsDir());
        }
        if (!found) throw std::invalid_argument("Unknown campaign mission: " + campaignQuery);
        std::cout << "Selected campaign mission: " << found->title << " [" << found->code
                  << ", " << found->width << 'x' << found->height << "] (" << found->path << ")\n";
        mission = found->path;
        if (gameDir.empty() && !overviewOnly) gameDir = directory;
    } else if (!mission.empty() && !std::filesystem::is_regular_file(mission)) {
        const auto directory = resolveGameDir();
        if (directory.empty())
            throw std::invalid_argument("--mission needs a file path, or a game directory to resolve a mission name");
        if (mpRequested) {
            const auto found = ss2::assets::findMultiplayerMission(directory, mission.string());
            if (!found) throw std::invalid_argument("Unknown multiplayer mission: " + mission.string());
            std::cout << "Selected multiplayer mission: " << found->name << " [" << found->width << 'x' << found->height
                      << "] (" << found->path << ")\n";
            mission = found->path;
        } else {
            const auto found = ss2::assets::findSinglePlayerMission(directory, mission.string());
            if (!found) throw std::invalid_argument("Unknown single-player mission: " + mission.string());
            std::cout << "Selected mission: " << found->name << " [" << found->width << 'x' << found->height << "] (" << found->path << ")\n";
            mission = found->path;
        }
        if (gameDir.empty() && !overviewOnly) gameDir = directory;
    }
    if (mpJoinEndpoint && gameDir.empty() && mission.empty()) {
        gameDir = resolveGameDir();
        if (gameDir.empty()) {
            throw std::invalid_argument("--join requires --game-dir or --mission");
        }
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
    if ((!recordPath.empty() || parsedReplay || parsedSave) && gameDir.empty()) {
        gameDir = resolveGameDir();
        if (gameDir.empty())
            throw std::invalid_argument((!recordPath.empty() ? std::string("--record")
                                        : parsedReplay ? std::string("--replay")
                                                       : std::string("--load-save")) +
                                        " requires a game directory: pass --game-dir or set SS2_GAME_DIR");
    }
    if (parsedSave && mission.empty() && campaignQuery.empty()) {
        mission = resolveSaveMissionPath(
            gameDir, parsedSave->meta, loadSavePath,
            ss2::assets::listSinglePlayerMissions(gameDir),
            ss2::assets::listCampaigns(gameDir));
        if (mission.empty()) {
            throw std::runtime_error("Could not determine mission for saved game: " + loadSavePath.string());
        }
    }
    const auto settingsPath = ss2::defaultSettingsPath();
    const bool persistSettings = !limit && !timing && !overviewOnly && recordPath.empty() && !parsedReplay &&
                                 !settingsPath.empty();
    ss2::UserSettings settings = persistSettings ? ss2::loadUserSettings(settingsPath) : ss2::UserSettings{};
    if (!languageChosen && settings.language) {
        ss2::setLanguage(*settings.language);
    }
    if (parsedSave && !cliDifficulty && parsedSave->meta.hasMetadata) {
        settings.difficulty = std::clamp(parsedSave->meta.difficulty, 1, 3);
    }
    if (cliDifficulty) {
        settings.difficulty = *cliDifficulty;
    }
    if (parsedReplay) {
        if (parsedReplay->difficulty) settings.difficulty = *parsedReplay->difficulty;
        if (parsedReplay->autoEngage) autoEngage = *parsedReplay->autoEngage;
    }
    SDLSession sdl;
    if (!noMovies && !settings.skipIntro && !gameDir.empty() && !limit && !timing && !overviewOnly &&
        !parsedReplay && !parsedSave && !mpRequested)
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
    ss2::MultiplayerSessionConfig mpConfig;
    if (mpRequested) {
        ss2::MultiplayerHandshake hs;
        if (mpHostPort) {
            const auto md = ss2::assets::readMissionData(mission);
            ss2::MatchSetupPacket setup;
            setup.assignedSeat = 1;
            setup.playerCount = 2;
            setup.mapHash = ss2::computeMissionMapHash(md);
            setup.missionSpec = mission.stem().string();
            setup.autoEngage = autoEngage;
            setup.playerNations[0] = cliMpNation.value_or(1u);
            setup.playerNames[0] = settings.activeProfile.empty() ? std::string("Player 1") : settings.activeProfile;
            if (!hs.startHost(*mpHostPort, std::move(setup))) {
                throw std::runtime_error(hs.errorText());
            }
            std::cout << "Hosting lockstep multiplayer on port " << hs.listeningPort()
                      << " (" << hs.matchSetup().missionSpec << ")\n";
        } else {
            std::uint64_t expectedHash = 0;
            if (!mission.empty()) {
                expectedHash = ss2::computeMissionMapHash(ss2::assets::readMissionData(mission));
            }
            const std::string joinName = settings.activeProfile.empty() ? std::string("Player 2") : settings.activeProfile;
            if (!hs.startJoin(mpJoinEndpoint->first, mpJoinEndpoint->second, joinName, expectedHash,
                              cliMpNation.value_or(0u))) {
                throw std::runtime_error(hs.errorText());
            }
            std::cout << "Joining lockstep multiplayer at " << mpJoinEndpoint->first
                      << ':' << mpJoinEndpoint->second << '\n';
        }
        while (hs.state() != ss2::MultiplayerHandshake::State::Ready &&
               hs.state() != ss2::MultiplayerHandshake::State::Failed) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT ||
                    (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_ESCAPE)) {
                    hs.cancel();
                    return 0;
                }
            }
            hs.poll();
            SDL_Delay(5);
        }
        if (hs.state() != ss2::MultiplayerHandshake::State::Ready) {
            throw std::runtime_error(hs.errorText().empty() ? "Multiplayer handshake failed" : hs.errorText());
        }
        if (!hs.isHost()) {
            mission = resolveMultiplayerMissionFromSetup(gameDir, hs.matchSetup(), mission);
            autoEngage = hs.matchSetup().autoEngage;
        }
        mpConfig = makeMultiplayerConfig(hs, hs.takeConnection());
    } else if (mission.empty() && !gameDir.empty() && !overviewOnly && !limit && !timing && !parsedReplay && !parsedSave) {
        auto chosen = runFrontEnd(window.get(), validate ? &report : nullptr, gameDir, settings, persistSettings);
        if (!chosen) return 0;
        mission = std::move(chosen->missionPath);
        if (chosen->replayFile) {
            parsedReplay = std::move(chosen->replayFile);
            if (parsedReplay->difficulty) settings.difficulty = *parsedReplay->difficulty;
            if (parsedReplay->autoEngage) autoEngage = *parsedReplay->autoEngage;
        }
        if (!chosen->savePath.empty()) {
            loadSavePath = std::move(chosen->savePath);
            if (!cliDifficulty && chosen->saveDifficulty) {
                settings.difficulty = std::clamp(*chosen->saveDifficulty, 1, 3);
            }
        }
        if (chosen->multiplayerConfig) {
            mpConfig = std::move(*chosen->multiplayerConfig);
            autoEngage = true;
        }
    }
    for (;;) {
        std::optional<std::filesystem::path> nextMission;
        result = ss2::runGameSession(window.get(), validate ? &report : nullptr, limit, timing, gameDir, mission, hdPack,
                           originalInfantry, autoEngage, settings, persistSettings, overviewOnly, &nextMission,
                           recordPath, parsedReplay ? &*parsedReplay : nullptr, false, loadSavePath, mpConfig);
        loadSavePath.clear();
        if (!nextMission || result != 0 || !recordPath.empty() || parsedReplay || mpConfig.enabled) break;
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
