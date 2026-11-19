#include "render/window.hpp"
#include "assets/infantry.hpp"
#include "assets/mission.hpp"
#include "assets/scenery.hpp"
#include "assets/terrain.hpp"
#include "assets/vehicles.hpp"
#include "ui/tutorial_selection.hpp"
#include "ui/briefing.hpp"
#include "sim/mission_runner.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

namespace {
struct SDLSession {
    SDLSession() {
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    }
    ~SDLSession() { SDL_Quit(); }
};
class FrameSample {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point warmupEnd_ = Clock::now() + std::chrono::seconds(1);
    Clock::time_point first_{};
    Clock::time_point previous_{};
    std::vector<double> intervals_;
    bool complete_ = false;

public:
    bool frame() {
        const auto now = Clock::now();
        if (now < warmupEnd_) return false;
        if (previous_ == Clock::time_point{}) first_ = now;
        else intervals_.push_back(std::chrono::duration<double, std::milli>(now - previous_).count());
        previous_ = now;
        complete_ = now - first_ >= std::chrono::seconds(10);
        return complete_;
    }

    bool report(float refreshRate) {
        if (!complete_ || intervals_.empty()) {
            std::cerr << "Timing sample ended before ten seconds of presentation\n";
            return false;
        }
        std::ranges::sort(intervals_);
        const auto percentile = [&](double p) {
            return intervals_[static_cast<size_t>(std::ceil(p * intervals_.size())) - 1];
        };
        std::cout << "Present-return intervals (ms), n=" << intervals_.size()
                  << " p50=" << percentile(.50) << " p95=" << percentile(.95)
                  << " p99=" << percentile(.99) << " max=" << intervals_.back() << '\n';
        if (refreshRate > 0) {
            const double thresholdMs = 1500.0 / refreshRate;
            const auto late = intervals_.end() - std::upper_bound(intervals_.begin(), intervals_.end(), thresholdMs);
            std::cout << "Present-return intervals above " << thresholdMs << " ms: " << late << '\n';
        }
        return true;
    }
};

int runWindow(SDL_Window* window, ss2::ValidationReport* report, int limit, bool timing,
              const std::filesystem::path& gameDir, const std::filesystem::path& mission) {
    float refreshRate = 0;
    if (timing) {
        const auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        if (mode) {
            refreshRate = mode->refresh_rate;
            std::cout << "Display " << mode->w << 'x' << mode->h << " at " << refreshRate << " Hz\n";
        }
        std::cout << "Window pixel density " << SDL_GetWindowPixelDensity(window) << '\n';
        if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP")) std::cout << "Desktop " << desktop << '\n';
    }
    ss2::VulkanWindow renderer(window, report);
    std::optional<ss2::assets::InfantryScene> scene;
    std::optional<ss2::TutorialSelection> selection;
    std::optional<ss2::MissionRunner> simulation;
    ss2::Briefing briefing(window);
    std::deque<std::string> pendingDialogs;
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> moveCursor(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR), SDL_DestroyCursor);
    const auto resetCursor = [&] { if (moveCursor) SDL_SetCursor(SDL_GetDefaultCursor()); };
    if (!mission.empty()) {
        renderer.setMap(ss2::assets::readMissionOverview(mission));
        const auto title = "Sudden Strike II | " + mission.stem().string() + " | Mission overview";
        SDL_SetWindowTitle(window, title.c_str());
    } else if (!gameDir.empty()) {
        auto map = ss2::assets::loadTutorialTerrain(gameDir);
        const auto objects = ss2::assets::drawTutorialScenery(map, gameDir);
        const auto placements = ss2::assets::readTutorialPlacements(gameDir);
        auto vehicles = ss2::assets::drawTutorialVehicles(map, gameDir, placements);
        const auto mapWidth = map.width, mapHeight = map.height;
        scene.emplace(std::move(map), gameDir, placements);
        const auto& infantry = scene->units();
        renderer.setMap(scene->compose());
        std::cout << "Tutorial I: " << mapWidth << 'x' << mapHeight << ", " << objects
                  << " scenery placements, " << infantry.size() << " infantry, " << vehicles.size() << " vehicle and gun placements\n";
        auto data = ss2::assets::readTutorialData(gameDir / "maps/single/--- TUTORIAL I ---.SSM");
        auto navigation = ss2::InfantryPathGrid::fromSubCellMasks(128,128,std::move(data.flags),std::move(data.obstacles));
        ss2::TutorialUnits soldiers(std::move(navigation), infantry, vehicles);
        simulation.emplace(std::move(data), ss2::tutorialTurnDelay(gameDir), std::move(soldiers));
        selection.emplace(window, renderer, infantry, std::move(vehicles));
    }
    FrameSample sample;
    const ss2::Snapshot empty{};
    std::uint64_t frames = 0;
    bool running = true;
    SDL_FPoint rightPress{};
    bool pendingMove = false;
    while (running && (!limit || frames < static_cast<unsigned>(limit))) {
        if (simulation) {
            for (const auto pose : simulation->pollPoses()) {
                const auto patch = scene->update(pose.index, pose.x, pose.y, pose.direction, pose.progress, pose.walking);
                if (patch.pixels.width && patch.pixels.height) renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                selection->update(pose.index, scene->units().at(pose.index));
            }
            for (const auto event : simulation->poll()) {
                const auto& data = simulation->data();
                if (event.kind == ss2::MissionEvent::Dialog || event.kind == ss2::MissionEvent::Message)
                    std::cout << "Tutorial I phrase " << event.index << '\n';
                if (event.kind == ss2::MissionEvent::Dialog) {
                    pendingMove = false;
                    resetCursor();
                    if (briefing.active()) pendingDialogs.push_back(data.phrases.at(event.index));
                    else briefing.open(data.phrases.at(event.index));
                } else if (event.kind == ss2::MissionEvent::Camera) {
                    const auto [x, y] = data.locations.at(event.index);
                    renderer.focusMap(x, y);
                } else if (event.kind == ss2::MissionEvent::Message)
                    SDL_SetWindowTitle(window, data.phrases.at(event.index).c_str());
                else if (event.kind == ss2::MissionEvent::Marker && selection) {
                    const auto [x, y] = data.markers.at(event.index);
                    selection->mark(x, y);
                }
            }
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
                break;
            }
            if (briefing.active()) {
                if (briefing.handle(event)) {
                    if (pendingDialogs.empty()) simulation->acknowledge();
                    else {
                        briefing.open(std::move(pendingDialogs.front()));
                        pendingDialogs.pop_front();
                    }
                }
                if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
                    renderer.handleEvent(event);
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) running = false;
            if (simulation && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_PAUSE && !event.key.repeat)
                simulation->togglePause();
            if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                pendingMove = true;
                rightPress = {event.button.x, event.button.y};
            }
            if (pendingMove && event.type == SDL_EVENT_MOUSE_MOTION &&
                std::hypot(event.motion.x-rightPress.x, event.motion.y-rightPress.y) > 3) pendingMove = false;
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) pendingMove = false;
            if (pendingMove && event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
                pendingMove = false;
                const auto [x,y] = renderer.mapPosition(event.button.x, event.button.y);
                if (selection->selected().size() == 1) {
                    if (!simulation->move(*selection->selected().begin(), int(std::floor(x)), int(std::floor(y))))
                        SDL_SetWindowTitle(window, "Tutorial I | Choose an idle player soldier and a clear straight route");
                    else std::cout << "Tutorial I accepted move to map pixel " << int(x) << ',' << int(y) << '\n';
                } else SDL_SetWindowTitle(window, "Tutorial I | Select one soldier to issue a move");
            }
            if (simulation && moveCursor && event.type == SDL_EVENT_MOUSE_MOTION && !event.motion.state) {
                const auto [x,y] = renderer.mapPosition(event.motion.x,event.motion.y);
                const auto& picked = selection->selected();
                const bool legal = picked.size() == 1 && simulation->canMove(*picked.begin(), int(std::floor(x)), int(std::floor(y)));
                SDL_SetCursor(legal ? moveCursor.get() : SDL_GetDefaultCursor());
            }
            if (selection) selection->handle(event);
            if (selection && event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) continue;
            renderer.handleEvent(event);
        }
        if (!running) break;
        briefing.draw(renderer);
        if (renderer.present(empty)) {
            ++frames;
            if (timing && sample.frame()) break;
        } else {
            SDL_Delay(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED ? 30 : 5);
        }
    }
    resetCursor();
    std::cout << "Presented " << frames << " frames\n";
    if (timing && !sample.report(refreshRate)) return 1;
    return limit && frames < static_cast<unsigned>(limit) ? 1 : 0;
}
}

int main(int argc, char** argv) try {
    std::filesystem::path gameDir, mission;
    int limit = 0;
    bool fourK = false, fullscreen = false, validate = false, timing = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "ss2 [--game-dir DIR | --mission FILE.ssm] [--4k] [--fullscreen] [--frames N] [--validate] [--timing]\n"
                         "--game-dir opens Tutorial I from an extracted Gold 2.2 game directory.\n"
                         "--mission displays an original mission overview.\n";
            return 0;
        } else if (arg == "--game-dir" && i + 1 < argc) gameDir = argv[++i];
        else if (arg == "--mission" && i + 1 < argc) mission = argv[++i];
        else if (arg == "--4k") fourK = true;
        else if (arg == "--fullscreen") fullscreen = true;
        else if (arg == "--validate") validate = true;
        else if (arg == "--timing") timing = true;
        else if (arg == "--frames" && i + 1 < argc) {
            const std::string_view n(argv[++i]);
            const auto [end, err] = std::from_chars(n.data(), n.data() + n.size(), limit);
            if (err != std::errc{} || end != n.data() + n.size() || limit < 1)
                throw std::invalid_argument("--frames requires a positive integer");
        } else throw std::invalid_argument("Unknown or incomplete option: " + std::string(arg));
    }
    if (timing && limit) throw std::invalid_argument("Choose either --timing or --frames N");
    if (!gameDir.empty() && !mission.empty()) throw std::invalid_argument("Choose either --game-dir or --mission");
    if (gameDir.empty() && mission.empty() && !limit && !timing) {
        if (const char* configured = std::getenv("SS2_GAME_DIR"); configured && *configured) gameDir = configured;
        else {
            std::filesystem::path dataHome;
            if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) dataHome = xdg;
            else if (const char* home = std::getenv("HOME")) dataHome = std::filesystem::path(home) / ".local/share";
            if (!dataHome.empty() && std::filesystem::is_directory(dataHome / "ss2/game"))
                gameDir = dataHome / "ss2/game";
        }
    }
    SDLSession sdl;
    auto flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Sudden Strike II | Native renderer", fourK ? 3840 : 1280,
                         fourK ? 2160 : 720, flags), SDL_DestroyWindow);
    if (!window) throw std::runtime_error(SDL_GetError());
    ss2::ValidationReport report;
    int result = runWindow(window.get(), validate ? &report : nullptr, limit, timing, gameDir, mission);
    if (validate) {
        std::cout << "Validation errors: " << report.errors.load() << '\n';
        if (report.errors.load()) return 1;
    }
    return result;
} catch (const std::exception& e) {
    std::cerr << "ss2: " << e.what() << '\n';
    return 1;
}
