// Headless GameSession input-to-command and replay verification on Tutorial I.
// Exercises real SDL_Event -> GameSession::handleInputEvent() -> MissionRunner
// command recording, pause gating, Pause Menu Esc toggle, and full Replay parity
// on a second GameSession without opening a visible window.
#include "session/game_session.hpp"
#include "session/save_file.hpp"
#include "sim/command.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

    // Live interactive replay viewer (liveReplay = true): Space pause/resume, +/- speed, . single-step,
    // Tab viewer-seat / All-Vision cycling, and bit-exact endStateHash verification on completion.
    {
        ss2::UserSettings liveRepSettings{};
        ss2::GameSession liveRep(
            win, validate ? &report : nullptr,
            0, false, gameDir, {}, {}, false, true,
            liveRepSettings, false, false, nullptr, {}, &*parsedReplay, true, {}, {}, nullptr, true);
        require(liveRep.simulation != std::nullopt && liveRep.liveReplay,
                "Live replay GameSession failed to initialize in interactive mode");
        require(liveRep.simulation->simTick() == 0u,
                "Live replay GameSession must start at tick 0 instead of batch fast-forwarding");

        // Pause via Space, single-step 3 ticks via '.', cycle speed (+ / -) and Tab perspective through All Vision.
        liveRep.handleInputEvent(makeKeyDown(win, SDLK_SPACE));
        require(liveRep.replayTransport.paused && liveRep.paused, "Space must pause live replay");
        for (int i = 0; i < 3; ++i) {
            liveRep.handleInputEvent(makeKeyDown(win, SDLK_PERIOD));
            liveRep.stepSimulationEvents();
        }
        require(liveRep.simulation->simTick() == 3u, "Period key must single-step paused live replay");

        liveRep.handleInputEvent(makeKeyDown(win, SDLK_EQUALS));
        require(liveRep.replayTransport.speedPercent() == 200, "Equals key must step replay faster to 2x");
        liveRep.handleInputEvent(makeKeyDown(win, SDLK_MINUS));
        require(liveRep.replayTransport.speedPercent() == 100, "Minus key must step replay slower back to 1x");

        // Tab cycles P1 -> P2 -> P3 -> P4 -> All Vision (-1) -> P1 (0).
        for (int i = 0; i < 4; ++i) {
            liveRep.handleInputEvent(makeKeyDown(win, SDLK_TAB));
        }
        require(liveRep.replayAllVision(), "4x Tab must reach All Vision perspective in live replay");
        liveRep.updateHudAndBeams();
        liveRep.renderer.present(liveRep.empty);
        liveRep.handleInputEvent(makeKeyDown(win, SDLK_TAB));
        require(liveRep.replayTransport.viewerSeat == 0 && !liveRep.replayAllVision(),
                "5th Tab must wrap back to Player 1 perspective");

        // Resume and step to endTick; verify exact stateHash parity with live input session.
        liveRep.handleInputEvent(makeKeyDown(win, SDLK_SPACE));
        require(!liveRep.replayTransport.paused, "Second Space must resume live replay");
        unsigned guard = 0;
        while (!liveRep.replayTransport.finished && guard++ < endTick + 64u) {
            require(liveRep.simulation->replayStep(
                        parsedReplay->commands, liveRep.replayTransport.cursor, parsedReplay->endTick),
                    "Live replayStep failed before endTick");
            liveRep.stepSimulationEvents();
        }
        require(liveRep.replayTransport.finished && liveRep.replayTransport.hashVerified &&
                    !liveRep.replayTransport.hashMismatch,
                "Live replay must finish and verify endStateHash");
        require(liveRep.simulation->simTick() == endTick &&
                    liveRep.simulation->stateHash() == endHash &&
                    liveRep.simulation->takeSnapshot() == endSnap,
                "Live replay end state diverged from live input session");
        liveRep.updateHudAndBeams();
        liveRep.renderer.present(liveRep.empty);
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

    // Retail SS2SAVE2 test on ALONE AGAINST THE WORLD.SSM: save after OP(40) Wave 0 spawns (157->174 inf, 73->76 veh),
    // reload into a fresh pre-reinforcement GameSession, verify counts/stateHash/fog match and Wave 0 does not duplicate.
    {
        const auto aloneMission = gameDir / "maps/single/ALONE AGAINST THE WORLD.SSM";
        require(std::filesystem::is_regular_file(aloneMission), "Missing retail ALONE AGAINST THE WORLD.SSM");
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto savePath2 = std::filesystem::temp_directory_path() /
                               ("ss2_save2_alone_" + std::to_string(nonce) + ".ss2save");
        struct Save2Cleanup {
            std::filesystem::path p;
            ~Save2Cleanup() {
                std::error_code ec;
                std::filesystem::remove(p, ec);
            }
        } cleanup2{savePath2};

        std::uint32_t tick1 = 0;
        ss2::MissionRunner::FullSnapshot full1{};
        std::size_t savedSolCount = 0;
        std::size_t savedVehCount = 0;
        {
            ss2::UserSettings s1Settings{};
            ss2::GameSession s1(
                win, validate ? &report : nullptr,
                0, false, gameDir, aloneMission, {}, false, true,
                s1Settings, false, false, nullptr, {}, nullptr, true);
            require(s1.simulation && s1.scene && s1.vehicleScene && s1.selection,
                    "ALONE GameSession failed to initialize");
            while (s1.briefing.active()) s1.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            for (int t = 0; t < 16 && (s1.scene->units().size() <= 157u || s1.vehicleScene->vehicles().size() <= 73u); ++t) {
                s1.simulation->advanceTicks(1);
                s1.stepSimulationEvents();
                while (s1.briefing.active()) s1.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            }
            for (int t = 0; t < 8; ++t) {
                s1.simulation->advanceTicks(1);
                s1.stepSimulationEvents();
                while (s1.briefing.active()) s1.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            }
            full1 = s1.simulation->takeFullSnapshot();
            tick1 = s1.simulation->simTick();
            savedSolCount = s1.scene->units().size();
            savedVehCount = s1.vehicleScene->vehicles().size();
            require(savedSolCount == 174u && savedVehCount == 76u &&
                        full1.units.soldiers.size() == 174u && full1.units.vehicles.size() == 76u,
                    "ALONE Wave 0 reinforcement counts unexpected before SS2SAVE2 write");

            ss2::SaveFileMetadata meta2{};
            meta2.hasMetadata = true;
            meta2.mission = "maps/single/ALONE AGAINST THE WORLD.SSM";
            meta2.missionTitle = "ALONE AGAINST THE WORLD";
            meta2.difficulty = 2;
            meta2.savedEpochSec = 1795253000;
            require(ss2::writeSaveFile(savePath2, meta2, full1), "writeSaveFile(FullSnapshot) failed on ALONE");
        }

        ss2::UserSettings s2Settings{};
        ss2::GameSession s2(
            win, validate ? &report : nullptr,
            0, false, gameDir, aloneMission, {}, false, true,
            s2Settings, false, false, nullptr, {}, nullptr, true, savePath2);
        require(s2.simulation && s2.scene && s2.vehicleScene && s2.selection,
                "Loaded ALONE GameSession missing core components");
        const auto loadedFull = s2.simulation->takeFullSnapshot();
        require(s2.simulation->simTick() == tick1 &&
                    loadedFull.units.soldiers.size() == full1.units.soldiers.size() &&
                    loadedFull.units.vehicles.size() == full1.units.vehicles.size() &&
                    loadedFull.tutorial == full1.tutorial &&
                    loadedFull.spawnedReinforcementWaves == full1.spawnedReinforcementWaves &&
                    loadedFull.scheduledReinforcements == full1.scheduledReinforcements,
                "Loaded ALONE GameSession tick or FullSnapshot VM state diverged from SS2SAVE2");
        require(s2.scene->units().size() == savedSolCount && s2.vehicleScene->vehicles().size() == savedVehCount,
                "Loaded ALONE GameSession did not restore visual reinforcement counts");
        require(!s2.briefing.active(), "Startup SS2SAVE2 load must not reopen opening mission briefing");

        for (int t = 0; t < 80; ++t) {
            s2.simulation->advanceTicks(1);
            s2.stepSimulationEvents();
            while (s2.briefing.active()) s2.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
        }
        require(s2.scene->units().size() == savedSolCount && s2.vehicleScene->vehicles().size() == savedVehCount,
                "Reloaded ALONE GameSession re-fired OP(40) Wave 0");
        s2.updateHudAndBeams();
        s2.renderer.present(s2.empty);
    }

    // End-to-end British Campaign carryover test (030000.ssc -> 030001.ssc):
    // real OP(84) player pool save + OP(17) next-mission signal + Easy/Normal resolution +
    // per-profile SS2POOL1 disk persistence + 030001.ssc OP(72)/OP(40) carryover spawn +
    // --record / ReplayFile initialSavedPools stateHash parity.
    {
        const auto britM1 = gameDir / "maps/campaign/030000.ssc";
        const auto britM2 = gameDir / "maps/campaign/030001.ssc";
        const auto britM2Easy = gameDir / "maps/campaign/EASY/030001.ssc";
        require(std::filesystem::is_regular_file(britM1) &&
                    std::filesystem::is_regular_file(britM2) &&
                    std::filesystem::is_regular_file(britM2Easy),
                "Missing retail British Campaign 030000.ssc / 030001.ssc / EASY/030001.ssc");

        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto xdgRoot = std::filesystem::temp_directory_path() /
                             ("ss2_camp_e2e_" + std::to_string(nonce));
        const char* prevData = std::getenv("XDG_DATA_HOME");
        const char* prevCfg = std::getenv("XDG_CONFIG_HOME");
        struct XdgGuard {
            std::filesystem::path root;
            std::optional<std::string> oldData;
            std::optional<std::string> oldCfg;
            ~XdgGuard() {
                if (oldData) ::setenv("XDG_DATA_HOME", oldData->c_str(), 1);
                else ::unsetenv("XDG_DATA_HOME");
                if (oldCfg) ::setenv("XDG_CONFIG_HOME", oldCfg->c_str(), 1);
                else ::unsetenv("XDG_CONFIG_HOME");
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
            }
        } xdgGuard{
            xdgRoot,
            prevData ? std::optional<std::string>(prevData) : std::nullopt,
            prevCfg ? std::optional<std::string>(prevCfg) : std::nullopt,
        };
        const auto xdgData = xdgRoot / "data";
        const auto xdgCfg = xdgRoot / "config";
        ::setenv("XDG_DATA_HOME", xdgData.c_str(), 1);
        ::setenv("XDG_CONFIG_HOME", xdgCfg.c_str(), 1);

        std::optional<std::filesystem::path> nextMission;
        ss2::SavedUnitPoolMap inMemPools;
        {
            ss2::UserSettings m1Settings{};
            m1Settings.activeProfile = "BritCmdr";
            m1Settings.difficulty = 2;
            ss2::GameSession m1(
                win, validate ? &report : nullptr,
                0, false, gameDir, britM1, {}, false, true,
                m1Settings, true, false, &nextMission, {}, nullptr, true, {}, {}, &inMemPools);
            require(m1.simulation && m1.scene && m1.selection,
                    "British 030000.ssc GameSession failed to initialize");
            while (m1.briefing.active()) m1.handleInputEvent(makeKeyDown(win, SDLK_RETURN));

            // Step 030000.ssc so Script 0's immediate Wave 0 spawns Player 0 units.
            for (int t = 0; t < 30; ++t) {
                m1.simulation->advanceTicks(1);
                m1.stepSimulationEvents();
                while (m1.briefing.active()) m1.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            }
            const auto preWinSnap = m1.simulation->takeSnapshot();
            std::size_t aliveP0BeforeWin = 0;
            for (const auto& s : preWinSnap.soldiers) {
                if (s.owner == 0 && s.hp > 0 && !s.embarked) ++aliveP0BeforeWin;
            }
            for (const auto& v : preWinSnap.vehicles) {
                if (v.owner == 0 && v.alive && v.hp > 0) ++aliveP0BeforeWin;
            }
            require(aliveP0BeforeWin > 0u, "British 030000.ssc must have living Player 0 units after Wave 0");

            // Advance timer 64 to fire real Script 15 (#64 OP(27) ... #7681 OP(17) #0 #0 OP(84) #1 OP(18)).
            auto full = m1.simulation->takeFullSnapshot();
            require(full.tutorial.active.size() > 15u && full.tutorial.active[15],
                    "British 030000.ssc Script 15 must be active");
            full.tutorial.timers[64] = m1.simulation->simTick() + 1u;
            require(m1.simulation->loadFullSnapshot(full), "loadFullSnapshot failed on British 030000.ssc");

            for (int t = 0; t < 80 && !m1.menu.active(); ++t) {
                m1.simulation->advanceTicks(1);
                m1.stepSimulationEvents();
                while (m1.briefing.active()) m1.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            }
            require(m1.menu.active(), "British 030000.ssc Script 15 OP(18) must open Victory Debrief Menu");
            require(inMemPools.contains(0u) && inMemPools[0u].size() == aliveP0BeforeWin,
                    "OP(84) on British 030000.ssc must save all surviving top-level Player 0 units into Pool 0");
            require(m1.nextCampaignMissionPath.has_value() &&
                        std::filesystem::weakly_canonical(*m1.nextCampaignMissionPath) ==
                            std::filesystem::weakly_canonical(britM2),
                    "Normal-difficulty OP(17) #7681 must resolve to maps/campaign/030001.ssc");

            // Verify Easy difficulty OP(17) resolves to EASY/030001.ssc, then restore Normal.
            m1.closeMenu();
            m1Settings.difficulty = 1;
            m1.openDebriefMenu(true);
            require(m1.nextCampaignMissionPath.has_value() &&
                        std::filesystem::weakly_canonical(*m1.nextCampaignMissionPath) ==
                            std::filesystem::weakly_canonical(britM2Easy),
                    "Easy-difficulty OP(17) #7681 must resolve to maps/campaign/EASY/030001.ssc");
            m1.closeMenu();
            m1Settings.difficulty = 2;
            m1.openDebriefMenu(true);

            // Verify SS2POOL1 disk persistence under XDG_DATA_HOME/ss2/campaigns/.
            const auto poolDiskPath = ss2::campaignPoolPathForMission("BritCmdr", *m1.nextCampaignMissionPath);
            const auto diskPools = ss2::readCampaignPoolFile(poolDiskPath);
            require(diskPools.has_value() && *diskPools == inMemPools,
                    "Debrief Menu Victory must persist SS2POOL1 carryover file for next campaign mission");

            // Activate "next_campaign" from the Debrief Menu via keyboard navigation.
            for (int step = 0; step < 16 && m1.menu.highlighted() != "next_campaign"; ++step) {
                m1.handleInputEvent(makeKeyDown(win, SDLK_DOWN));
            }
            require(m1.menu.highlighted() == "next_campaign", "Debrief Menu must highlight next_campaign item");
            m1.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            require(!m1.running && nextMission.has_value() &&
                        std::filesystem::weakly_canonical(*nextMission) ==
                            std::filesystem::weakly_canonical(britM2),
                    "Activating next_campaign must exit Mission 1 and output 030001.ssc");
        }

        // Control check on 030001.ssc static placements + Wave 1: Group 70 carryover wave must be empty without pools.
        std::size_t emptyTotalP0 = 0;
        {
            const auto m2Data = ss2::assets::readMissionData(*nextMission);
            require(m2Data.reinforcements[1].empty(),
                    "030001.ssc static Wave 1 must be empty before OP(72) pool materialization");
            std::size_t grp70Static = 0;
            for (const auto& p : ss2::assets::readMissionPlacements(*nextMission)) {
                if (p.owner == 0 && p.hp > 0) {
                    ++emptyTotalP0;
                    if (p.group == 70) ++grp70Static;
                    for (const auto& pax : p.passengers) {
                        if (!pax.isCrew && pax.hp > 0) {
                            ++emptyTotalP0;
                            if (pax.group == 70) ++grp70Static;
                        }
                    }
                }
            }
            require(grp70Static == 0u, "030001.ssc static placements must have 0 Group 70 units");
        }

        // Carryover session on 030001.ssc loaded from disk SS2POOL1 (simulating process restart) + --record.
        const auto m2RepPath = xdgRoot / "brit_030001.ss2rep";
        auto reloadedPools = ss2::readCampaignPoolFile(
            ss2::campaignPoolPathForMission("BritCmdr", *nextMission));
        require(reloadedPools.has_value() && !reloadedPools->empty(),
                "Failed to reload persisted SS2POOL1 for 030001.ssc");

        std::uint32_t m2EndTick = 0;
        std::uint64_t m2EndHash = 0;
        ss2::TutorialUnits::Snapshot m2EndSnap{};
        {
            ss2::UserSettings m2Settings{};
            m2Settings.activeProfile = "BritCmdr";
            m2Settings.difficulty = 2;
            ss2::GameSession m2Carry(
                win, validate ? &report : nullptr,
                0, false, gameDir, *nextMission, {}, false, true,
                m2Settings, false, false, nullptr, m2RepPath, nullptr, true, {}, {}, &*reloadedPools);
            while (m2Carry.briefing.active()) m2Carry.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            for (int t = 0; t < 25; ++t) {
                m2Carry.simulation->advanceTicks(1);
                m2Carry.stepSimulationEvents();
                while (m2Carry.briefing.active()) m2Carry.handleInputEvent(makeKeyDown(win, SDLK_RETURN));
            }
            m2Carry.updateHudAndBeams();
            m2Carry.renderer.present(m2Carry.empty);

            m2EndTick = m2Carry.simulation->simTick();
            m2EndHash = m2Carry.simulation->stateHash();
            m2EndSnap = m2Carry.simulation->takeSnapshot();
            std::size_t carryTotalP0 = 0;
            std::size_t grp70Carry = 0;
            for (const auto& s : m2EndSnap.soldiers) {
                if (s.owner == 0 && s.hp > 0) {
                    ++carryTotalP0;
                    if (s.group == 70) ++grp70Carry;
                }
            }
            for (const auto& v : m2EndSnap.vehicles) {
                if (v.owner == 0 && v.alive && v.hp > 0) {
                    ++carryTotalP0;
                    if (v.group == 70) ++grp70Carry;
                }
            }
            require(grp70Carry > 0u && carryTotalP0 > emptyTotalP0,
                    "030001.ssc OP(72)+OP(40) must spawn carried-over units in Group 70");
            m2Carry.writeRecordingIfRequested();
        }

        // Replay 030001.ssc solely from the recorded .ss2rep (nullptr campaignPools) and verify bit-identical stateHash.
        std::ifstream repIn(m2RepPath, std::ios::binary);
        require(static_cast<bool>(repIn), "Recorded 030001.ss2rep file missing");
        std::ostringstream repBuf;
        repBuf << repIn.rdbuf();
        const auto parsedM2Rep = cmd::fromReplayFileText(repBuf.str());
        require(parsedM2Rep.has_value() && parsedM2Rep->initialSavedPools == *reloadedPools,
                "Recorded 030001.ss2rep must preserve initialSavedPools");

        ss2::UserSettings m2RepSettings{};
        ss2::GameSession m2Replay(
            win, validate ? &report : nullptr,
            0, false, gameDir, *nextMission, {}, false, true,
            m2RepSettings, false, false, nullptr, {}, &*parsedM2Rep, false);
        require(m2Replay.simulation != std::nullopt, "030001.ssc Replay GameSession missing simulation");
        require(m2Replay.simulation->simTick() == m2EndTick &&
                    m2Replay.simulation->stateHash() == m2EndHash &&
                    m2Replay.simulation->takeSnapshot() == m2EndSnap,
                "030001.ssc Replay GameSession stateHash/snapshot diverged from carryover live session");
        m2Replay.updateHudAndBeams();
        m2Replay.renderer.present(m2Replay.empty);
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
