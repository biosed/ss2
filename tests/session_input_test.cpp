// Headless GameSession input-to-command and replay verification on Tutorial I.
// Exercises real SDL_Event -> GameSession::handleInputEvent() -> MissionRunner
// command recording, pause gating, Pause Menu Esc toggle, and full Replay parity
// on a second GameSession without opening a visible window.
#include "session/game_session.hpp"
#include "session/save_file.hpp"
#include "sim/command.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
namespace cmd = ss2::cmd;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

SDL_Event makeKeyDown(SDL_Window* win, SDL_Keycode key, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event ev{};
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.windowID = SDL_GetWindowID(win);
    ev.key.key = key;
    ev.key.mod = mod;
    ev.key.down = true;
    ev.key.repeat = false;
    return ev;
}

SDL_Event makeMouseButton(SDL_Window* win, SDL_EventType type, Uint8 button,
                          float x, float y, Uint8 clicks = 1) {
    SDL_Event ev{};
    ev.type = type;
    ev.button.windowID = SDL_GetWindowID(win);
    ev.button.button = button;
    ev.button.x = x;
    ev.button.y = y;
    ev.button.clicks = clicks;
    ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    return ev;
}

template <class T>
std::size_t countCommand(const std::vector<cmd::RecordedCommand>& log) {
    std::size_t n = 0;
    for (const auto& entry : log) {
        if (std::holds_alternative<T>(entry.command)) ++n;
    }
    return n;
}
} // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::cerr << "usage: session_input_test GAME_DIR [--validate]\n";
        return 1;
    }
    const std::filesystem::path gameDir(argv[1]);
    bool validate = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--validate") validate = true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    struct SdlGuard {
        ~SdlGuard() { SDL_Quit(); }
    } sdlGuard;

    SDL_Window* win = SDL_CreateWindow("session_input_test", 1280, 720,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) throw std::runtime_error(SDL_GetError());
    struct WinGuard {
        SDL_Window* w;
        ~WinGuard() { SDL_DestroyWindow(w); }
    } winGuard{win};

    ss2::ValidationReport report;
    std::uint32_t endTick = 0;
    std::uint64_t endHash = 0;
    ss2::TutorialUnits::Snapshot endSnap;
    std::vector<cmd::RecordedCommand> recordedLog;
    std::optional<cmd::ReplayFile> parsedReplay;

    {
        ss2::UserSettings settings{};
        ss2::GameSession session(
            win, validate ? &report : nullptr,
            0, false, gameDir, {}, {}, false, true,
            settings, false, false, nullptr, {}, nullptr, true);
        require(session.simulation && session.selection && session.hud,
                "Tutorial I GameSession failed to initialize core components");

        unsigned dialogsDismissed = 0;
        for (int t = 0; t < 520; ++t) {
            session.simulation->advanceTicks(1);
            session.stepSimulationEvents();
            while (session.briefing.active()) {
                session.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
                ++dialogsDismissed;
            }
        }
        require(dialogsDismissed >= 2 && !session.briefing.active(),
                "handleInputEvent must dismiss opening Tutorial I dialogs");

        // Select all 29 player infantry via Ctrl+Shift+A.
        session.handleInputEvent(
            makeKeyDown(win, SDLK_A, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)));
        require(session.selection->selected().size() == 29,
                "Ctrl+Shift+A must select all 29 player soldiers");

        // Position camera over the wreck objective and lay out HUD at 1280x720.
        session.renderer.focusMap(2960, 1776);
        session.updateHudAndBeams();
        session.renderer.present(session.empty);

        constexpr float clickX = 640.0f;
        constexpr float clickY = 300.0f;
        require(!session.hud->opaqueAtWindow(clickX, clickY),
                "Viewport target point must not be covered by HUD");

        // Right-click move order through handleInputEvent.
        session.handleInputEvent(
            makeMouseButton(win, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT, clickX, clickY));
        for (int i = 0; i < 5; ++i) {
            session.simulation->advanceTicks(1);
            session.stepSimulationEvents();
        }

        // Toggle hold fire on and off (H, H).
        session.handleInputEvent(makeKeyDown(win, SDLK_H));
        session.simulation->advanceTicks(2);
        session.stepSimulationEvents();
        session.handleInputEvent(makeKeyDown(win, SDLK_H));
        session.simulation->advanceTicks(2);
        session.stepSimulationEvents();

        // Crouch (C), Officer binoculars (B), Scatter (D), Stop (S).
        session.handleInputEvent(makeKeyDown(win, SDLK_C));
        session.simulation->advanceTicks(2);
        session.stepSimulationEvents();
        session.handleInputEvent(makeKeyDown(win, SDLK_B));
        session.simulation->advanceTicks(2);
        session.stepSimulationEvents();
        session.handleInputEvent(makeKeyDown(win, SDLK_D));
        session.simulation->advanceTicks(2);
        session.stepSimulationEvents();
        session.handleInputEvent(makeKeyDown(win, SDLK_S));
        session.simulation->advanceTicks(2);
        session.stepSimulationEvents();

        // Arm Attack-Move (A), then left-click map target.
        session.handleInputEvent(makeKeyDown(win, SDLK_A));
        require(session.attackMoveArmed, "A key must arm Attack-Move mode");
        session.handleInputEvent(
            makeMouseButton(win, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, clickX, clickY));
        require(!session.attackMoveArmed, "Left-click must dispatch and clear Attack-Move mode");
        for (int i = 0; i < 5; ++i) {
            session.simulation->advanceTicks(1);
            session.stepSimulationEvents();
        }

        // Pause gating via Ctrl+P, then unpause via Ctrl+P.
        session.handleInputEvent(makeKeyDown(win, SDLK_P, SDL_KMOD_CTRL));
        require(session.paused, "Ctrl+P must pause the session");
        const auto pausedTick = session.simulation->simTick();
        session.simulation->advanceTicks(10);
        require(session.simulation->simTick() == pausedTick,
                "Paused ManualClock session must hold simulation tick");
        session.handleInputEvent(makeKeyDown(win, SDLK_P, SDL_KMOD_CTRL));
        require(!session.paused, "Second Ctrl+P must resume the session");
        session.simulation->advanceTicks(10);
        session.stepSimulationEvents();
        require(session.simulation->simTick() == pausedTick + 10u,
                "Resumed ManualClock session must advance normally");

        // Escape opens Pause Menu (pausing simulation); second Escape resumes cleanly.
        session.handleInputEvent(makeKeyDown(win, SDLK_ESCAPE));
        require(session.menu.active() && session.paused, "Escape must open Pause Menu and pause");
        session.handleInputEvent(makeKeyDown(win, SDLK_ESCAPE));
        require(!session.menu.active() && !session.paused,
                "Second Escape must close Pause Menu and unpause");

        for (int i = 0; i < 20; ++i) {
            session.simulation->advanceTicks(1);
            session.stepSimulationEvents();
        }
        session.updateHudAndBeams();
        session.renderer.present(session.empty);

        endTick = session.simulation->simTick();
        endHash = session.simulation->stateHash();
        endSnap = session.simulation->takeSnapshot();
        recordedLog = session.simulation->takeRecording();

        require(countCommand<cmd::Acknowledge>(recordedLog) >= 2, "missing Acknowledge command in recording");
        require(countCommand<cmd::Move>(recordedLog) >= 1, "missing Move command in recording");
        require(countCommand<cmd::ToggleHoldFire>(recordedLog) >= 2, "missing ToggleHoldFire command in recording");
        require(countCommand<cmd::ToggleCrouch>(recordedLog) >= 1, "missing ToggleCrouch command in recording");
        require(countCommand<cmd::ToggleBinoculars>(recordedLog) >= 1, "missing ToggleBinoculars command in recording");
        require(countCommand<cmd::Scatter>(recordedLog) >= 1, "missing Scatter command in recording");
        require(countCommand<cmd::Stop>(recordedLog) >= 1, "missing Stop command in recording");
        require(countCommand<cmd::AttackMove>(recordedLog) >= 1, "missing AttackMove command in recording");
        require(countCommand<cmd::TogglePause>(recordedLog) >= 4, "missing TogglePause commands in recording");

        cmd::ReplayFile rep;
        rep.mission = "maps/single/--- TUTORIAL I ---.SSM";
        rep.difficulty = 2;
        rep.autoEngage = true;
        rep.endTick = endTick;
        rep.endStateHash = endHash;
        rep.commands = recordedLog;
        parsedReplay = cmd::fromReplayFileText(cmd::toReplayFileText(rep));
        require(parsedReplay && *parsedReplay == rep, "ReplayFile round trip failed");
    }

    // Construct a fresh replay GameSession on the same window and verify exact end-state parity.
    {
        ss2::UserSettings replaySettings{};
        ss2::GameSession replaySession(
            win, validate ? &report : nullptr,
            0, false, gameDir, {}, {}, false, true,
            replaySettings, false, false, nullptr, {}, &*parsedReplay, false);
        require(replaySession.simulation != std::nullopt, "Replay GameSession missing simulation");
        require(replaySession.simulation->simTick() == endTick,
                "Replay GameSession tick diverged from live input session");
        require(replaySession.simulation->stateHash() == endHash,
                "Replay GameSession stateHash diverged from live input session");
        require(replaySession.simulation->takeSnapshot() == endSnap,
                "Replay GameSession snapshot diverged from live input session");
        replaySession.updateHudAndBeams();
        replaySession.renderer.present(replaySession.empty);
    }

    // Save SS2SAVE1 snapshot with non-zero battle stats and verify headless startup --load-save restoration.
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto savePath = std::filesystem::temp_directory_path() /
                              ("ss2_session_input_" + std::to_string(nonce) + ".ss2save");
        struct SaveCleanup {
            std::filesystem::path p;
            ~SaveCleanup() {
                std::error_code ec;
                std::filesystem::remove(p, ec);
            }
        } cleanup{savePath};

        ss2::SaveFileMetadata meta{};
        meta.hasMetadata = true;
        meta.mission = "maps/single/--- TUTORIAL I ---.SSM";
        meta.missionTitle = "Tutorial I";
        meta.difficulty = 2;
        meta.savedEpochSec = 1795251234;
        meta.enemyInfKilled = 7;
        meta.enemyVehDestroyed = 2;
        meta.enemyTanksDestroyed = 1;
        meta.enemyGunsDestroyed = 1;
        meta.enemyPlanesShotDown = 1;
        meta.buildingsDemolished = 3;
        meta.airSortiesFlown = 2;
        meta.enemyScoreEarned = 3400;
        meta.friendlyScoreLost = 200;
        require(ss2::writeSaveFile(savePath, meta, endSnap), "writeSaveFile failed for session_input_test");

        ss2::UserSettings loadSettings{};
        ss2::GameSession loadedSession(
            win, validate ? &report : nullptr,
            0, false, gameDir, {}, {}, false, true,
            loadSettings, false, false, nullptr, {}, nullptr, true, savePath);
        require(loadedSession.simulation != std::nullopt, "Loaded GameSession missing simulation");
        require(loadedSession.simulation->simTick() == endTick,
                "Loaded GameSession tick diverged from saved snapshot");
        const auto loadedSnap = loadedSession.simulation->takeSnapshot();
        require(loadedSnap.tick == endSnap.tick &&
                    loadedSnap.playerTeams == endSnap.playerTeams &&
                    loadedSnap.mines == endSnap.mines &&
                    loadedSnap.soldiers == endSnap.soldiers &&
                    loadedSnap.vehicles == endSnap.vehicles &&
                    loadedSnap.buildings == endSnap.buildings &&
                    loadedSnap.airReady == endSnap.airReady &&
                    loadedSnap.fog.size() == endSnap.fog.size(),
                "Loaded GameSession core snapshot state diverged from saved snapshot");
        require(loadedSession.enemyInfKilled == 7u && loadedSession.enemyVehDestroyed == 2u &&
                    loadedSession.enemyTanksDestroyed == 1u && loadedSession.enemyGunsDestroyed == 1u &&
                    loadedSession.enemyPlanesShotDown == 1u && loadedSession.buildingsDemolished == 3u &&
                    loadedSession.airSortiesFlown == 2u && loadedSession.enemyScoreEarned == 3400 &&
                    loadedSession.friendlyScoreLost == 200,
                "Loaded GameSession did not restore SS2SAVE1 battle/debrief stats");
        require(!loadedSession.briefing.active(),
                "Startup save load must not reopen opening mission briefing");
        loadedSession.updateHudAndBeams();
        loadedSession.renderer.present(loadedSession.empty);
    }

    require(report.errors.load() == 0, "Vulkan validation errors observed during GameSession input test");
    std::cout << "GameSession headless input-to-command + replay parity (" << recordedLog.size()
              << " commands, tick " << endTick << ", stateHash 0x" << std::hex << endHash << std::dec
              << ") PASSED\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "session_input_test FAILED: " << e.what() << '\n';
    return 1;
}
