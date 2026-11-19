#include "render/window.hpp"
#include "sim/tutorial_coordinates.hpp"
#include "assets/hd_pack.hpp"
#include "assets/infantry.hpp"
#include "assets/mission.hpp"
#include "assets/scenery.hpp"
#include "assets/terrain.hpp"
#include "assets/vehicles.hpp"
#include "assets/sound_bank.hpp"
#include "audio/sound_player.hpp"
#include "ui/tutorial_selection.hpp"
#include "ui/briefing.hpp"
#include "ui/hud.hpp"
#include "ui/menu.hpp"
#include "ui/tooltip.hpp"
#include "sim/mission_runner.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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

ss2::assets::Infantry scaleInfantry(ss2::assets::Infantry unit, int scale) {
    if (scale == 1) return unit;
    unit.x *= scale;
    unit.y *= scale;
    unit.left *= scale;
    unit.top *= scale;
    unit.width *= scale;
    unit.height *= scale;
    return unit;
}

int runWindow(SDL_Window* window, ss2::ValidationReport* report, int limit, bool timing,
              const std::filesystem::path& gameDir, const std::filesystem::path& mission,
              const std::filesystem::path& hdPack, bool originalInfantry, bool autoEngage) {
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
    std::map<std::string, unsigned> gpuAtlasSlots;
    struct WorldGpuSprite {
        ss2::Sprite sprite;
        int sortY{0};
        int mapX1x{0}, mapY1x{0};
    };
    std::vector<WorldGpuSprite> worldGpuSprites;
    struct FallenCorpse {
        int x{0}, y{0};
        std::string id;
        unsigned direction{0};
    };
    std::vector<FallenCorpse> fallenSoldiers;
    std::set<size_t> woundedBarLogged;
    std::vector<std::uint8_t> latestFog;
    std::set<size_t> crouchedUnits;
    std::set<size_t> holdFireUnits;
    std::map<size_t, std::chrono::steady_clock::time_point> recoilUntil;
    const auto animStart = std::chrono::steady_clock::now();
    auto lastAnimSync = animStart;
    const auto fogBrightnessAt = [&](int mx, int my) -> float {
        if (latestFog.size() != 128u * 128u) return 1.0f;
        const int d = mx - 4096;
        const int tx = std::clamp((my + d / 2) / 32, 0, 127);
        const int ty = std::clamp((my - d / 2) / 32, 0, 127);
        const std::uint8_t f = latestFog[std::size_t(ty) * 128u + std::size_t(tx)];
        if (f & 0x01u) return 1.0f;
        return (f & 0x10u) ? 0.70f : 0.44f;
    };
    const auto syncGpuSprites = [&] {
        if (!scene || (gpuAtlasSlots.empty() && worldGpuSprites.empty())) return;
        struct DrawItem {
            ss2::Sprite sprite;
            int sortY;
        };
        std::vector<DrawItem> items;
        items.reserve(scene->units().size() + worldGpuSprites.size());
        for (const auto& ws : worldGpuSprites) {
            auto sp = ws.sprite;
            sp.brightness = fogBrightnessAt(ws.mapX1x, ws.mapY1x);
            items.push_back({sp, ws.sortY});
        }
        constexpr float cellPx = 128.0f;
        constexpr float dstSize = 64.0f;
        constexpr float footRatioY = 116.0f / 128.0f;
        for (const auto& c : fallenSoldiers) {
            if (!gpuAtlasSlots.contains(c.id)) continue;
            const unsigned col = c.direction & 7u;
            const float cw = dstSize * 0.96f;
            const float ch = dstSize * 0.36f;
            items.push_back({
                {
                    static_cast<float>(c.x * 2) - 0.5f * cw,
                    static_cast<float>(c.y * 2) - 0.65f * ch,
                    cw,
                    ch,
                    static_cast<float>(col) * cellPx,
                    0.0f,
                    static_cast<float>(col + 1u) * cellPx,
                    cellPx,
                    gpuAtlasSlots.at(c.id),
                    fogBrightnessAt(c.x, c.y) * 0.78f,
                    0.92f
                },
                -10000 + c.y
            });
        }
        constexpr float recoilDx[8] = {-1.8f, 0.0f, 1.8f, 2.4f, 1.8f, 0.0f, -1.8f, -2.4f};
        constexpr float recoilDy[8] = {-1.2f, -1.8f, -1.2f, 0.0f, 1.2f, 1.8f, 1.2f, 0.0f};
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - animStart).count();
        const auto& units = scene->units();
        for (size_t idx = 0; idx < units.size(); ++idx) {
            const auto& u = units[idx];
            if (u.hp <= 0 || !u.visible || !gpuAtlasSlots.contains(u.id)) continue;
            const unsigned col = u.direction & 7u;
            const unsigned row = u.walking ? (1u + ((4u * u.progress) >> 8u)) : 0u;
            const bool crouched = crouchedUnits.contains(idx);
            float w = dstSize * (crouched ? 1.04f : 1.0f);
            float h = dstSize * (crouched ? 0.78f : 1.0f);
            float ox = 0.0f, oy = 0.0f;
            if (!u.walking) {
                const float breath = std::sin(elapsed * 2.4f + static_cast<float>(idx) * 0.85f);
                h *= (1.0f + 0.018f * breath);
                ox += 0.45f * std::sin(elapsed * 1.05f + static_cast<float>(idx) * 1.37f);
            }
            if (const auto it = recoilUntil.find(idx); it != recoilUntil.end() && now < it->second) {
                ox += recoilDx[col];
                oy += recoilDy[col];
            }
            items.push_back({
                {
                    static_cast<float>(u.x * 2) - 0.5f * w + ox,
                    static_cast<float>(u.y * 2) - footRatioY * h + oy,
                    w,
                    h,
                    static_cast<float>(col) * cellPx,
                    static_cast<float>(row) * cellPx,
                    static_cast<float>(col + 1u) * cellPx,
                    static_cast<float>(row + 1u) * cellPx,
                    gpuAtlasSlots.at(u.id),
                    fogBrightnessAt(u.x, u.y),
                    1.0f
                },
                u.y
            });
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const DrawItem& a, const DrawItem& b) { return a.sortY < b.sortY; });
        std::vector<ss2::Sprite> sprites;
        sprites.reserve(items.size());
        for (const auto& it : items) sprites.push_back(it.sprite);
        renderer.setSprites(std::move(sprites));
    };
    std::optional<ss2::TutorialSelection> selection;
    std::optional<ss2::MissionRunner> simulation;
    std::optional<ss2::SoundPlayer> audio;
    std::optional<ss2::Hud> hud;
    ss2::Menu menu(window);
    ss2::Tooltip tooltip(window);
    struct ActiveVfx {
        float x0, y0, x1, y1;
        bool isFlamer{false};
        std::chrono::steady_clock::time_point start;
        std::chrono::milliseconds duration;
    };
    std::vector<ActiveVfx> activeVfx;
    bool vfxWasNonEmpty = false;
    bool menuPausedClock = false;
    bool smoothTerrain = false;
    std::string currentObjective = "Follow Tutorial I orders";
    std::array<std::set<size_t>, 10> hudGroups;
    std::vector<ss2::MapBar> lastUnitBars;
    std::vector<int> initialUnitHp;
    unsigned hudMapW = 0, hudMapH = 0;
    const auto hudClassLabel = [](const std::string& id) -> std::string {
        if (id == "dguner") return "Rifleman";
        if (id == "dsmguner") return "SMG";
        if (id == "dflamer") return "Flamer";
        if (id == "dofficer") return "Officer";
        if (id == "aguner") return "US Rifleman";
        if (id == "aofficer") return "US Officer";
        if (id == "cmedic") return "Medic";
        if (id == "rsmguner") return "Soviet SMG";
        return id;
    };
    const auto downsampleOverview = [](const ss2::assets::MapImage& src, unsigned dstW, unsigned dstH) {
        ss2::assets::MapImage out{dstW, dstH, std::vector<std::uint8_t>(std::size_t(dstW) * dstH * 4u, 255u)};
        if (!src.width || !src.height || src.rgba.size() < std::size_t(src.width) * src.height * 4u) return out;
        for (unsigned y = 0; y < dstH; ++y) {
            const unsigned sy = (y * src.height) / dstH;
            for (unsigned x = 0; x < dstW; ++x) {
                const unsigned sx = (x * src.width) / dstW;
                const std::size_t si = (std::size_t(sy) * src.width + sx) * 4u;
                const std::size_t di = (std::size_t(y) * dstW + x) * 4u;
                out.rgba[di + 0] = src.rgba[si + 0];
                out.rgba[di + 1] = src.rgba[si + 1];
                out.rgba[di + 2] = src.rgba[si + 2];
                out.rgba[di + 3] = 255u;
            }
        }
        return out;
    };
    ss2::Briefing briefing(window);
    ss2::Notice orderNotice(window);
    std::deque<std::string> pendingDialogs;
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> moveCursor(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER), SDL_DestroyCursor);
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> attackCursor(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR), SDL_DestroyCursor);
    const auto resetCursor = [&] { SDL_SetCursor(SDL_GetDefaultCursor()); };
    int mapScale = 1;
    if (!mission.empty()) {
        renderer.setMap(ss2::assets::readMissionOverview(mission));
        const auto title = "Sudden Strike II | " + mission.stem().string() + " | Mission overview";
        SDL_SetWindowTitle(window, title.c_str());
    } else if (!gameDir.empty()) {
        auto map = ss2::assets::loadTutorialTerrain(gameDir);
        ss2::assets::SceneryDepth depth(map.width,map.height);
        const auto objects = ss2::assets::drawTutorialScenery(map, gameDir, &depth);
        const auto placements = ss2::assets::readTutorialPlacements(gameDir);
        auto vehicles = ss2::assets::drawTutorialVehicles(map, gameDir, placements);
        const auto mapWidth = map.width, mapHeight = map.height;
        ss2::assets::MapImage hdImage;
        std::filesystem::path infantryArt;
        bool cleanGround = false;
        if (!hdPack.empty()) {
            auto groundPath = hdPack / "tutorial1_ground_2x.ss2hd";
            cleanGround = std::filesystem::is_regular_file(groundPath);
            if (!originalInfantry && std::filesystem::is_directory(hdPack / "infantry-art"))
                infantryArt = hdPack / "infantry-art";
            auto hd = ss2::assets::loadHdPackMap(cleanGround ? groundPath : hdPack);
            if (hd.scale != 2) throw std::runtime_error("Tutorial I requires a 2x HD pack");
            mapScale = 2;
            hdImage = std::move(hd.image);
            const auto gpuArtDir = hdPack / "infantry-art-4x";
            unsigned nextSlot = 0;
            if (!originalInfantry && cleanGround && std::filesystem::is_directory(gpuArtDir)) {
                for (const auto& p : placements) {
                    if (gpuAtlasSlots.contains(p.id) || nextSlot >= ss2::VulkanWindow::spriteAtlasSlots) continue;
                    const auto path = gpuArtDir / (p.id + ".png");
                    if (!std::filesystem::is_regular_file(path)) continue;
                    auto atlas = ss2::assets::loadPngRgba(path);
                    if (atlas.width == 1024 && atlas.height == 640) {
                        renderer.setSpriteAtlas(nextSlot, atlas);
                        gpuAtlasSlots.emplace(p.id, nextSlot++);
                    }
                }
            }
            const auto worldDir = hdPack / "world-sprites-4x";
            const auto manifestPath = worldDir / "manifest.txt";
            if (cleanGround && std::filesystem::is_regular_file(manifestPath)) {
                std::map<std::string, unsigned> worldSlots;
                std::ifstream mf(manifestPath);
                std::string line;
                while (std::getline(mf, line)) {
                    std::istringstream iss(line);
                    std::string name;
                    float dx, dy, dw, dh, u0, v0, u1, v1;
                    int sy = 0;
                    if (!(iss >> name >> dx >> dy >> dw >> dh >> u0 >> v0 >> u1 >> v1 >> sy)) continue;
                    if (!worldSlots.contains(name)) {
                        if (nextSlot >= ss2::VulkanWindow::spriteAtlasSlots) continue;
                        const auto ap = worldDir / (name + ".png");
                        if (!std::filesystem::is_regular_file(ap)) continue;
                        auto atlas = ss2::assets::loadPngRgba(ap);
                        renderer.setSpriteAtlas(nextSlot, atlas);
                        worldSlots.emplace(name, nextSlot++);
                    }
                    worldGpuSprites.push_back({
                        {dx, dy, dw, dh, u0, v0, u1, v1, worldSlots.at(name), 1.0f, 1.0f},
                        sy,
                        static_cast<int>((dx + dw * 0.5f) * 0.5f),
                        static_cast<int>((dy + dh * 0.5f) * 0.5f)
                    });
                }
                if (!worldGpuSprites.empty()) {
                    std::cout << "GPU 4x HD world sprites: " << worldGpuSprites.size()
                              << " placements across " << worldSlots.size() << " atlases\n";
                }
            }
        }
        scene.emplace(std::move(map), gameDir, placements, originalInfantry, std::move(hdImage), infantryArt,
                      cleanGround, std::move(depth), !gpuAtlasSlots.empty());
        if (!gpuAtlasSlots.empty()) std::cout << "GPU 4x HD infantry atlases: " << gpuAtlasSlots.size() << " classes\n";
        else if (!infantryArt.empty()) std::cout << "Painted HD infantry: " << scene->paintedCount() << "/" << scene->units().size() << '\n';
        const auto& infantry = scene->units();
        initialUnitHp.clear();
        initialUnitHp.reserve(infantry.size());
        for (const auto& u : infantry) initialUnitHp.push_back(std::max(1, u.hp));
        std::vector<ss2::assets::Infantry> viewInfantry = infantry;
        auto viewVehicles = vehicles;
        const auto composedMap = scene->compose();
        hudMapW = mapWidth;
        hudMapH = mapHeight;
        hud.emplace(window);
        hud->setMinimap(downsampleOverview(composedMap, 512u, 256u));
        if (!hdPack.empty() && std::filesystem::is_directory(hdPack / "hud-portraits")) {
            for (const char* cls : {"aguner", "aofficer", "cmedic", "dflamer", "dguner", "dofficer", "dsmguner", "rsmguner"}) {
                const auto pp = hdPack / "hud-portraits" / (std::string(cls) + ".png");
                if (std::filesystem::is_regular_file(pp)) {
                    hud->setPortrait(hudClassLabel(cls), ss2::assets::loadPngRgba(pp));
                }
            }
        }
        renderer.setMap(composedMap);
        if (mapScale == 2) {
            smoothTerrain = true;
            renderer.setMapFilterLinear(true);
            for (auto& u : viewInfantry) u = scaleInfantry(u, mapScale);
            for (auto& v : viewVehicles) {
                v.x *= mapScale;
                v.y *= mapScale;
            }
            SDL_SetWindowTitle(window, "Sudden Strike II | Tutorial I (2x HD)");
            std::cout << "Tutorial I (2x HD): " << mapWidth*2 << 'x' << mapHeight*2 << ", " << objects
                      << " scenery placements, " << infantry.size() << " infantry, " << vehicles.size() << " vehicle and gun placements\n";
        } else {
            std::cout << "Tutorial I: " << mapWidth << 'x' << mapHeight << ", " << objects
                      << " scenery placements, " << infantry.size() << " infantry, " << vehicles.size() << " vehicle and gun placements\n";
        }
        auto data = ss2::assets::readTutorialData(gameDir / "maps/single/--- TUTORIAL I ---.SSM");
        ss2::FogGrid fog(128, 128, data.flags);
        fog.loadCirc64FromGame(gameDir);
        auto navigation = ss2::InfantryPathGrid::fromSubCellMasks(128,128,std::move(data.flags),std::move(data.obstacles));
        ss2::TutorialUnits soldiers(std::move(navigation), infantry, vehicles, std::move(fog), 2, autoEngage);
        // Hide unspotted enemies from frame 0 until scouts spot them, then apply initial fog shroud.
        for (size_t i = 0; i < infantry.size(); ++i) {
            if (infantry[i].owner == 1 && !soldiers.isValidAttackTarget(i, 0)) {
                const auto& u = scene->units().at(i);
                scene->update(i, u.x, u.y, u.direction, u.progress, u.walking, u.hp, 0);
            }
        }
        if (auto initialFog = soldiers.takeFogSnapshot()) {
            latestFog = *initialFog;
            const auto fogPatch = scene->updateFog(*initialFog);
            if (fogPatch.pixels.width && fogPatch.pixels.height)
                renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
        }
        syncGpuSprites();
        try {
            audio.emplace(ss2::assets::SoundBank::load(gameDir));
            if (!hdPack.empty() && std::filesystem::is_directory(hdPack / "audio-hd"))
                audio->setHdAudioDir(hdPack / "audio-hd");
            const auto hdMusic = (!hdPack.empty() && std::filesystem::is_directory(hdPack / "audio-hd" / "music"))
                                     ? (hdPack / "audio-hd" / "music")
                                     : (gameDir / "MUSIC");
            audio->startMusic(hdMusic, 0.28f);
        } catch (const std::exception&) {}
        simulation.emplace(std::move(data), ss2::tutorialTurnDelay(gameDir), std::move(soldiers));
        selection.emplace(window, renderer, std::move(viewInfantry), std::move(viewVehicles), mapScale);
    }
    FrameSample sample;
    const ss2::Snapshot empty{};
    std::uint64_t frames = 0;
    bool running = true;
    bool gestureInterrupted = false;
    bool markerHint = false;
    bool paused = false;
    std::size_t reportedLosses = 0;
    struct TimedMarker {
        ss2::MapMarker marker;
        std::chrono::steady_clock::time_point until;
        std::optional<size_t> targetUid;
        bool isOrder{false};
    };
    std::vector<TimedMarker> activeMarkers;
    const auto syncMarkers = [&] {
        std::vector<ss2::MapMarker> out;
        out.reserve(activeMarkers.size());
        for (const auto& m : activeMarkers) out.push_back(m.marker);
        renderer.setMarkers(std::move(out));
    };
    std::map<std::string, std::chrono::steady_clock::time_point> lastWeaponAudio;
    const auto weaponSoundForUnit = [](const std::string& id) -> std::tuple<const char*, int, float> {
        if (id == "dsmguner" || id == "rsmguner" || id == "aofficer") return {"smg_burst", 420, 0.55f};
        if (id == "dflamer") return {"fire", 1800, 0.48f};
        if (id == "dofficer") return {"gunshot", 200, 0.42f};
        return {"rifle", 180, 0.40f};
    };
    const auto showPause = [&] {
        orderNotice.show("Paused. Press Pause or Ctrl+P to resume.", ss2::NoticeKind::unsupported,
                         ss2::Notice::Duration::zero());
    };
    const auto logHud = [&](const std::string& msg) {
        if (hud && !msg.empty()) hud->log(msg);
    };
    const auto showNotice = [&](const std::string& msg, ss2::NoticeKind kind) {
        orderNotice.show(msg, kind);
        logHud(msg);
    };
    const auto buildPauseItems = [&] {
        std::vector<ss2::MenuItem> items;
        items.push_back({"resume", "Resume", "", true});
        items.push_back({"smooth", "Smooth terrain", smoothTerrain ? "On" : "Off", true});
        if (audio) {
            items.push_back({"music", "Music", audio->musicPlaying() ? "On" : "Muted", true});
            items.push_back({"next_track", "Next soundtrack track", "N", true});
        }
        items.push_back({"controls", "Controls", "", true});
        items.push_back({"log", "Message log", "L", true});
        items.push_back({"quit", "Quit to desktop", "", true});
        return items;
    };
    const auto openPauseMenu = [&] {
        if (briefing.active()) return;
        if (simulation && !paused) {
            paused = simulation->togglePause();
            menuPausedClock = true;
        } else {
            menuPausedClock = false;
        }
        menu.open("Paused", buildPauseItems());
    };
    const auto closeMenu = [&] {
        menu.close();
        if (simulation && menuPausedClock && paused) {
            paused = simulation->togglePause();
        }
        menuPausedClock = false;
    };
    const auto openControlsMenu = [&] {
        menu.open("Controls", {
            {"back", "Back to pause menu", "", true},
            {"", "Selection & Orders", "", false},
            {"", "Select / box select", "Left click / drag", false},
            {"", "Move or attack enemy", "Right click", false},
            {"", "Assign / recall group", "Ctrl+0..9 / 0..9", false},
            {"", "Tactical Stance", "", false},
            {"", "Stop selected units", "S", false},
            {"", "Toggle hold fire", "H", false},
            {"", "Toggle crouch stance", "C", false},
            {"", "Camera & Interface", "", false},
            {"", "Pan / Zoom", "Edges / Wheel", false},
            {"", "Focus troops / marker", "Tab / M", false},
            {"", "Center / Fullscreen", "Home / F11", false},
            {"", "Pause / Message log", "Ctrl+P / L", false},
            {"", "Mute music / Next track", "Ctrl+M / N", false},
        });
    };
    const auto openLogMenu = [&] {
        if (briefing.active()) return;
        std::vector<ss2::MenuItem> items;
        items.push_back({"resume", "Close", "", true});
        if (hud) {
            std::istringstream iss(hud->logText(24));
            std::vector<std::string> entries;
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) entries.push_back(line);
            }
            constexpr std::size_t kWrap = 68;
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                const auto& entry = *it;
                std::size_t pos = 0;
                while (pos < entry.size()) {
                    if (entry.size() - pos <= kWrap) {
                        items.push_back({"", entry.substr(pos), "", false});
                        break;
                    }
                    std::size_t cut = entry.rfind(' ', pos + kWrap);
                    if (cut == std::string::npos || cut <= pos) cut = pos + kWrap;
                    items.push_back({"", entry.substr(pos, cut - pos), "", false});
                    pos = cut;
                    while (pos < entry.size() && entry[pos] == ' ') ++pos;
                }
            }
        }
        if (items.size() == 1) items.push_back({"", "No messages yet.", "", false});
        menu.open("Message Log", std::move(items));
    };
    const auto doTogglePause = [&] {
        if (!simulation) return;
        paused = simulation->togglePause();
        if (paused) showPause();
        else orderNotice.show("Resumed.", ss2::NoticeKind::accepted);
        std::cout << (paused ? "Mission clock paused" : "Mission clock resumed") << '\n';
    };
    const auto doStop = [&] {
        if (!simulation || !selection || selection->selected().empty()) return;
        const auto& sel = selection->selected();
        const std::vector<size_t> ids(sel.begin(), sel.end());
        if (const auto n = simulation->stop(ids)) {
            showNotice("Stop order (" + std::to_string(n) + " soldiers).", ss2::NoticeKind::accepted);
            std::cout << "Tutorial I tactical S (stop): " << n << " units\n";
        }
    };
    const auto doToggleHoldFire = [&] {
        if (!simulation || !selection || selection->selected().empty()) return;
        const auto& sel = selection->selected();
        const std::vector<size_t> ids(sel.begin(), sel.end());
        if (const auto hold = simulation->toggleHoldFire(ids)) {
            for (const auto id : ids) {
                if (*hold) holdFireUnits.insert(id);
                else holdFireUnits.erase(id);
            }
            showNotice(*hold ? "Hold fire enabled." : "Fire at will enabled.", ss2::NoticeKind::accepted);
            std::cout << "Tutorial I tactical H (hold-fire=" << (*hold ? "on" : "off") << "): " << ids.size() << " units\n";
        }
    };
    const auto doToggleCrouch = [&] {
        if (!simulation || !selection || selection->selected().empty()) return;
        const auto& sel = selection->selected();
        const std::vector<size_t> ids(sel.begin(), sel.end());
        if (const auto crouched = simulation->toggleCrouch(ids)) {
            for (const auto id : ids) {
                if (*crouched) crouchedUnits.insert(id);
                else crouchedUnits.erase(id);
            }
            syncGpuSprites();
            showNotice(*crouched ? "Stance: Crouching (cover bonus, shorter sight)." : "Stance: Standing.",
                       ss2::NoticeKind::accepted);
            std::cout << "Tutorial I tactical C (stance=" << (*crouched ? "crouch" : "stand") << "): " << ids.size() << " units\n";
        }
    };
    auto lastNoticeFrame = std::chrono::steady_clock::now();
    while (running && (!limit || frames < static_cast<unsigned>(limit))) {
        if (simulation) {
            bool markersDirty = false;
            bool spritesDirty = false;
            if (auto fogSnap = simulation->pollFog()) {
                latestFog = *fogSnap;
                const auto fogPatch = scene->updateFog(*fogSnap);
                if (fogPatch.pixels.width && fogPatch.pixels.height)
                    renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
                spritesDirty = true;
            }
            for (const auto pose : simulation->pollPoses()) {
                spritesDirty = true;
                const auto patch = scene->update(pose.index, pose.x, pose.y, pose.direction, pose.progress, pose.walking, pose.hp, pose.visible ? 1 : 0);
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                selection->update(pose.index, scaleInfantry(scene->units().at(pose.index), mapScale));
                if (pose.fired) {
                    const auto now = std::chrono::steady_clock::now();
                    recoilUntil[pose.index] = now + std::chrono::milliseconds(120);
                    if (audio) {
                        const auto& uid = scene->units().at(pose.index).id;
                        const auto [sndGroup, cooldownMs, gain] = weaponSoundForUnit(uid);
                        auto& lastAt = lastWeaponAudio[sndGroup];
                        if (now - lastAt >= std::chrono::milliseconds(cooldownMs)) {
                            audio->playGroup(sndGroup, gain);
                            lastAt = now;
                        }
                    }
                    if (pose.visible && activeVfx.size() < 48) {
                        const float sx = static_cast<float>(pose.x * mapScale);
                        const float sy = static_cast<float>(pose.y * mapScale) - 18.0f * static_cast<float>(mapScale);
                        const float tx = static_cast<float>(pose.targetX * mapScale);
                        const float ty = static_cast<float>(pose.targetY * mapScale) - 14.0f * static_cast<float>(mapScale);
                        const bool flamer = (scene->units().at(pose.index).id == "dflamer");
                        activeVfx.push_back({
                            sx, sy, tx, ty, flamer, now,
                            std::chrono::milliseconds(flamer ? 220 : 95)
                        });
                    }
                }
                if (!pose.alive) {
                    const auto& deadUnit = scene->units().at(pose.index);
                    fallenSoldiers.push_back({pose.x, pose.y, deadUnit.id, pose.direction});
                    const auto before = activeMarkers.size();
                    std::erase_if(activeMarkers, [&](const TimedMarker& m) {
                        return m.targetUid && *m.targetUid == pose.index;
                    });
                    if (activeMarkers.size() != before) markersDirty = true;
                    if (audio) audio->playGroup("fall_small");
                    std::cout << "Tutorial I unit " << pose.index
                              << " (owner=" << pose.owner << " group=" << pose.group << ") eliminated\n";
                }
            }
            if (!paused && !gpuAtlasSlots.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastAnimSync >= std::chrono::milliseconds(50)) {
                    spritesDirty = true;
                    lastAnimSync = now;
                }
            }
            if (spritesDirty) syncGpuSprites();
            if (selection && selection->playerLosses() > reportedLosses) {
                const auto total = selection->playerLosses();
                const auto delta = total - reportedLosses;
                reportedLosses = total;
                showNotice(
                    (delta == 1 ? std::string("1 soldier lost") : std::to_string(delta) + " soldiers lost") +
                    " (" + std::to_string(total) + " total).", ss2::NoticeKind::blocked);
            }
            if (!activeMarkers.empty()) {
                const auto now = std::chrono::steady_clock::now();
                const auto before = activeMarkers.size();
                std::erase_if(activeMarkers, [&](const TimedMarker& m) { return now >= m.until; });
                if (activeMarkers.size() != before) markersDirty = true;
            }
            if (markersDirty) syncMarkers();
            for (const auto event : simulation->poll()) {
                const auto& data = simulation->data();
                if (event.kind == ss2::MissionEvent::Dialog || event.kind == ss2::MissionEvent::Message)
                    std::cout << "Tutorial I phrase " << event.index << '\n';
                if (event.kind == ss2::MissionEvent::Dialog || event.kind == ss2::MissionEvent::Camera) {
                    if (selection->cancelDrag()) {
                        gestureInterrupted = true;
                        std::cout << "Tutorial I pointer gesture interrupted by "
                                  << (event.kind == ss2::MissionEvent::Dialog ? "briefing " : "camera move ")
                                  << event.index << '\n';
                    }
                }
                if (event.kind == ss2::MissionEvent::Dialog) {
                    resetCursor();
                    if (!activeMarkers.empty()) {
                        activeMarkers.clear();
                        syncMarkers();
                    }
                    const auto& phrase = data.phrases.at(event.index);
                    logHud(phrase);
                    if (briefing.active()) pendingDialogs.push_back(phrase);
                    else briefing.open(phrase);
                } else if (event.kind == ss2::MissionEvent::Camera) {
                    const auto [x, y] = data.locations.at(event.index);
                    renderer.focusMap(x * mapScale, y * mapScale);
                } else if (event.kind == ss2::MissionEvent::Message) {
                    const auto& message = data.phrases.at(event.index);
                    currentObjective = message;
                    logHud(message);
                    SDL_SetWindowTitle(window, message.c_str());
                    orderNotice.show(message, ss2::NoticeKind::accepted);
                } else if (event.kind == ss2::MissionEvent::Marker && selection) {
                    const auto [x, y] = data.markers.at(event.index);
                    selection->mark(x * mapScale, y * mapScale);
                    markerHint = true;
                } else if (event.kind == ss2::MissionEvent::Victory) {
                    std::cout << "Tutorial I victory (" << event.index << ")\n";
                    currentObjective = "Mission Accomplished!";
                    logHud(currentObjective);
                    SDL_SetWindowTitle(window, "Sudden Strike II | Tutorial I — Victory!");
                    orderNotice.show("Mission Accomplished!", ss2::NoticeKind::accepted, ss2::Notice::Duration::zero());
                }
            }
        }
        if (gestureInterrupted && !briefing.active()) {
            orderNotice.show("A mission event interrupted your input. Select or issue the order again.",
                             ss2::NoticeKind::unsupported);
            gestureInterrupted = false;
            markerHint = false;
        } else if (markerHint && !briefing.active()) {
            orderNotice.show("M shows the mission marker. Tab focuses your troops.", ss2::NoticeKind::unsupported);
            markerHint = false;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            tooltip.handle(event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
                continue;
            }
            if (hud && (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE || event.type == SDL_EVENT_WINDOW_FOCUS_LOST)) {
                hud->handle(event);
            }
            if (menu.active()) {
                const auto mr = menu.handle(event);
                if (mr.closed || mr.activated == "resume") {
                    closeMenu();
                } else if (mr.activated == "smooth") {
                    smoothTerrain = !smoothTerrain;
                    renderer.setMapFilterLinear(smoothTerrain);
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "music" && audio) {
                    audio->toggleMusicMute();
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "next_track" && audio) {
                    audio->nextMusicTrack();
                    logHud("Switched to next soundtrack track.");
                } else if (mr.activated == "controls") {
                    openControlsMenu();
                } else if (mr.activated == "log") {
                    openLogMenu();
                } else if (mr.activated == "back") {
                    menu.open("Paused", buildPauseItems());
                } else if (mr.activated == "quit") {
                    running = false;
                }
                if (mr.consumed) continue;
            }
            if (hud && !briefing.active()) {
                const auto hr = hud->handle(event);
                if (hr.action == ss2::HudAction::moveCamera && hudMapW && hudMapH) {
                    renderer.focusMap(hr.u * static_cast<double>(hudMapW * mapScale),
                                      hr.v * static_cast<double>(hudMapH * mapScale));
                } else if (hr.action == ss2::HudAction::stop) {
                    doStop();
                } else if (hr.action == ss2::HudAction::holdFire) {
                    doToggleHoldFire();
                } else if (hr.action == ss2::HudAction::crouch) {
                    doToggleCrouch();
                } else if (hr.action == ss2::HudAction::pause) {
                    doTogglePause();
                } else if (hr.action == ss2::HudAction::log) {
                    openLogMenu();
                } else if (hr.action == ss2::HudAction::mission && selection) {
                    SDL_Event mKey{};
                    mKey.type = SDL_EVENT_KEY_DOWN;
                    mKey.key.key = SDLK_M;
                    selection->handle(mKey);
                    std::cout << "Tutorial I HUD Mission marker jump\n";
                }
                if (hr.consumed) continue;
            }
            orderNotice.handle(event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
                break;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11 && !event.key.repeat) {
                const bool isFullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                if (!SDL_SetWindowFullscreen(window, !isFullscreen)) SDL_Log("Fullscreen switch failed: %s", SDL_GetError());
                continue;
            }
            if (briefing.active()) {
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT)
                    std::cout << "Tutorial I right-click received while briefing open\n";
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
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_ESCAPE) {
                if (simulation) openPauseMenu();
                else running = false;
                continue;
            }
            if (hud && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_L && !(event.key.mod & SDL_KMOD_CTRL)) {
                openLogMenu();
                continue;
            }
            if (simulation && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                (event.key.key == SDLK_PAUSE || (event.key.key == SDLK_P && (event.key.mod & SDL_KMOD_CTRL)))) {
                doTogglePause();
            }
            if (audio && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_M && (event.key.mod & SDL_KMOD_CTRL)) {
                const bool muted = audio->toggleMusicMute();
                orderNotice.show(muted ? "Music muted (Ctrl+M)." : "Music unmuted.", ss2::NoticeKind::accepted);
                continue;
            }
            if (audio && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_N && !(event.key.mod & SDL_KMOD_CTRL)) {
                if (audio->nextMusicTrack()) orderNotice.show("Next soundtrack track.", ss2::NoticeKind::accepted);
            }
            if (simulation && selection && !selection->selected().empty() &&
                event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !(event.key.mod & SDL_KMOD_CTRL)) {
                if (event.key.key == SDLK_S) doStop();
                else if (event.key.key == SDLK_H) doToggleHoldFire();
                else if (event.key.key == SDLK_C) doToggleCrouch();
            }
            if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                // Gold's viewport cancels a left selection box or orders at the press coordinates.
                if (selection->cancelDrag()) continue;
                const auto [x,y] = renderer.mapPosition(event.button.x, event.button.y);
                const int simX = int(std::floor(x / mapScale)), simY = int(std::floor(y / mapScale));
                const auto& selected = selection->selected();
                const std::vector<size_t> ids(selected.begin(), selected.end());
                const auto target = selection->infantryAt(int(std::floor(x)), int(std::floor(y)));
                // The original Tutorial I player table assigns player 1 to the enemy team.
                if (!ids.empty() && target && scene->units().at(*target).owner == 1 &&
                    simulation->isValidAttackTarget(*target)) {
                    const auto attackResult = simulation->attack(ids, *target);
                    if (!attackResult) {
                        gestureInterrupted = true;
                        std::cout << "Tutorial I attack interrupted by a pending briefing\n";
                        continue;
                    }
                    const auto count = *attackResult;
                    if (count) {
                        auto label = count == 1 && ids.size() == 1 ? std::string("Attack order accepted") :
                            "Attack order accepted by " + std::to_string(count) + " of " + std::to_string(ids.size()) + " soldiers";
                        if (paused) label += ". Press Pause or Ctrl+P to resume.";
                        showNotice(label, count == ids.size() ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
                        const auto& u = scene->units().at(*target);
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{u.left * mapScale, u.top * mapScale, u.width * mapScale, u.height * mapScale},
                             ss2::MarkerKind::attack},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            *target,
                            true
                        });
                        syncMarkers();
                        if (audio) audio->playGroup("f_sold_attack");
                        std::cout << "Tutorial I accepted attack on unit " << *target << " at map pixel " << simX << ',' << simY
                                  << " for " << count << '/' << ids.size() << " selected infantry\n";
                    } else {
                        orderNotice.show("Selected soldiers cannot engage that target.", ss2::NoticeKind::blocked);
                        std::cout << "Tutorial I rejected attack on unit " << *target << " for " << ids.size() << " selected infantry\n";
                    }
                    continue;
                }
                const auto result = simulation->move(ids, simX, simY);
                if (!result) {
                    // The worker opened a modal before its presentation event reached SDL.
                    gestureInterrupted = true;
                    std::cout << "Tutorial I move interrupted by a pending briefing\n";
                    continue;
                }
                const auto count = *result;
                if (count) {
                    auto label = count == 1 && ids.size() == 1 ? std::string("Move order accepted") :
                        "Move order accepted by " + std::to_string(count) + " of " + std::to_string(ids.size()) + " soldiers";
                    if (paused) label += ". Press Pause or Ctrl+P to resume.";
                    showNotice(label, count == ids.size() ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
                    std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                    activeMarkers.push_back({
                        {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                         ss2::MarkerKind::move},
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                        std::nullopt,
                        true
                    });
                    syncMarkers();
                    if (audio) audio->playGroup("f_sold_move");
                    std::cout << "Tutorial I accepted move to map pixel " << simX << ',' << simY
                              << " for " << count << '/' << ids.size() << " selected infantry\n";
                } else {
                    std::cout << "Tutorial I rejected move to map pixel " << simX << ',' << simY
                              << " for " << ids.size() << " selected infantry\n";
                    const auto [u,v] = ss2::tutorialPointerWorld(simX,simY);
                    const bool outside = std::min(u,v) < 0 || std::max(u,v) >= 4096;
                    orderNotice.show(ids.empty()
                                         ? (selection->playerLosses() > 0
                                                ? "Your selected soldiers were lost. Select remaining troops."
                                                : "Select your infantry, then right-click the destination.")
                                         : outside ? "That point is outside the map."
                                                   : "No route from this position. Choose another destination.",
                                     ids.empty() ? ss2::NoticeKind::unsupported : ss2::NoticeKind::blocked);
                }
            }
            if (simulation && event.type == SDL_EVENT_MOUSE_MOTION && !event.motion.state) {
                const auto [x,y] = renderer.mapPosition(event.motion.x,event.motion.y);
                const int simX = int(std::floor(x / mapScale)), simY = int(std::floor(y / mapScale));
                const auto& picked = selection->selected();
                const std::vector<size_t> ids(picked.begin(), picked.end());
                const auto target = selection->infantryAt(int(std::floor(x)), int(std::floor(y)));
                if (!ids.empty() && target && scene->units().at(*target).owner == 1 &&
                    simulation->isValidAttackTarget(*target)) {
                    SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
                } else if (simulation->canMove(ids, simX, simY)) {
                    SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                } else {
                    SDL_SetCursor(SDL_GetDefaultCursor());
                }
            }
            if (selection) {
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                    (event.key.mod & SDL_KMOD_CTRL) && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) {
                    hudGroups[static_cast<std::size_t>(event.key.key - SDLK_0)] = selection->selected();
                }
                selection->handle(event);
            }
            if (selection && event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) continue;
            renderer.handleEvent(event);
        }
        if (!running) break;
        if (scene) {
            ss2::assets::InfantryScene::Cursor cursor;
            if (SDL_GetMouseFocus() == window && !briefing.active()) {
                float mouseX{}, mouseY{};
                SDL_GetMouseState(&mouseX,&mouseY);
                const auto [x,y] = renderer.mapPosition(mouseX,mouseY);
                cursor.emplace(int(std::floor(x/mapScale)),int(std::floor(y/mapScale)));
            }
            const auto [left,top] = renderer.mapPosition(0,0);
            const auto phase = (int(std::floor(left/mapScale)) ^ int(std::floor(top/mapScale))) & 1;
            for (const auto& patch : scene->setInspection(selection->selected(),cursor,phase)) {
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x,patch.y,patch.pixels);
            }
        }
        if (hud && scene && selection && hudMapW && hudMapH) {
            ss2::HudState hs;
            hs.objective = currentObjective;
            hs.losses = static_cast<unsigned>(selection->playerLosses());
            hs.paused = paused;
            struct Agg { unsigned count = 0; float hpSum = 0.0f; float ammoSum = 0.0f; };
            std::map<std::string, Agg> byClass;
            const auto& units = scene->units();
            for (unsigned d = 1; d <= 10; ++d) {
                const unsigned digit = d % 10u;
                unsigned aliveInGrp = 0;
                for (const auto id : hudGroups[digit]) {
                    if (id < units.size() && units[id].hp > 0) ++aliveInGrp;
                }
                if (aliveInGrp > 0) hs.groups.push_back({digit, aliveInGrp});
            }
            for (const auto id : selection->selected()) {
                if (id >= units.size()) continue;
                const auto& u = units[id];
                if (u.hp <= 0) continue;
                auto& a = byClass[hudClassLabel(u.id)];
                ++a.count;
                a.hpSum += std::clamp(static_cast<float>(u.hp) / 100.0f, 0.0f, 1.0f);
                a.ammoSum += std::clamp(static_cast<float>(u.ammo) / 100.0f, 0.0f, 1.0f);
                if (crouchedUnits.contains(id)) hs.crouched = true;
                if (holdFireUnits.contains(id)) hs.holdFire = true;
            }
            for (const auto& [label, a] : byClass) {
                const float avgHp = std::round((a.hpSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
                const float avgAmmo = std::round((a.ammoSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
                hs.selection.push_back({label, a.count, avgHp, avgAmmo});
            }
            hs.dots.reserve(units.size());
            std::vector<ss2::MapBar> unitBars;
            unitBars.reserve(selection->selected().size() + 16);
            const float invW = 1.0f / static_cast<float>(hudMapW);
            const float invH = 1.0f / static_cast<float>(hudMapH);
            for (std::size_t idx = 0; idx < units.size(); ++idx) {
                const auto& u = units[idx];
                if (u.hp <= 0 || !u.visible) continue;
                const float du = std::round(std::clamp(static_cast<float>(u.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
                const float dv = std::round(std::clamp(static_cast<float>(u.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
                hs.dots.push_back({du, dv, u.owner != 0});
                const int maxHp = (idx < initialUnitHp.size()) ? initialUnitHp[idx] : 100;
                if (selection->selected().contains(idx) || u.hp < maxHp) {
                    if (u.hp < maxHp && woundedBarLogged.insert(idx).second) {
                        std::cout << "Tutorial I wounded gauge: unit " << idx
                                  << " hp=" << u.hp << "/" << maxHp << '\n';
                    }
                    const float barW = 16.0f * static_cast<float>(mapScale);
                    const float barY = static_cast<float>(u.y * mapScale) - (crouchedUnits.contains(idx) ? 24.0f : 30.0f) * static_cast<float>(mapScale);
                    unitBars.push_back({
                        static_cast<float>(u.x * mapScale),
                        barY,
                        barW,
                        std::clamp(static_cast<float>(u.hp) / static_cast<float>(maxHp), 0.0f, 1.0f)
                    });
                }
            }
            bool barsChanged = unitBars.size() != lastUnitBars.size();
            if (!barsChanged) {
                for (std::size_t i = 0; i < unitBars.size(); ++i) {
                    if (unitBars[i].x != lastUnitBars[i].x || unitBars[i].y != lastUnitBars[i].y ||
                        unitBars[i].width != lastUnitBars[i].width || unitBars[i].value != lastUnitBars[i].value) {
                        barsChanged = true;
                        break;
                    }
                }
            }
            if (barsChanged) {
                lastUnitBars = unitBars;
                renderer.setBars(std::move(unitBars));
            }
            int winW = 1, winH = 1;
            SDL_GetWindowSize(window, &winW, &winH);
            const int mapViewH = std::max(1, winH - static_cast<int>(hud->height()));
            const auto [mx0, my0] = renderer.mapPosition(0.0, 0.0);
            const auto [mx1, my1] = renderer.mapPosition(static_cast<double>(winW), static_cast<double>(mapViewH));
            const float totalW = static_cast<float>(hudMapW * mapScale);
            const float totalH = static_cast<float>(hudMapH * mapScale);
            hs.viewU = std::round(std::clamp(static_cast<float>(mx0) / totalW, 0.0f, 1.0f) * 256.0f) / 256.0f;
            hs.viewV = std::round(std::clamp(static_cast<float>(my0) / totalH, 0.0f, 1.0f) * 256.0f) / 256.0f;
            hs.viewW = std::round(std::clamp(static_cast<float>(mx1 - mx0) / totalW, 0.01f, 1.0f) * 256.0f) / 256.0f;
            hs.viewH = std::round(std::clamp(static_cast<float>(my1 - my0) / totalH, 0.01f, 1.0f) * 256.0f) / 256.0f;
            hud->update(hs);
            hud->draw(renderer);
        }
        if (!activeVfx.empty() || vfxWasNonEmpty) {
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(activeVfx, [&](const ActiveVfx& v) { return now - v.start >= v.duration; });
            std::vector<ss2::MapBeam> beams;
            beams.reserve(activeVfx.size() * 3u);
            const float s = static_cast<float>(mapScale);
            for (const auto& v : activeVfx) {
                const float t = std::clamp(
                    std::chrono::duration<float>(now - v.start).count() /
                        (static_cast<float>(v.duration.count()) * 0.001f),
                    0.0f, 1.0f);
                if (v.isFlamer) {
                    const float flick = 1.0f + 0.16f * std::sin(t * 28.0f);
                    const float fade = 1.0f - 0.65f * t;
                    const float reach = std::min(1.0f, t * 2.4f);
                    const float ex = v.x0 + (v.x1 - v.x0) * reach;
                    const float ey = v.y0 + (v.y1 - v.y0) * reach;
                    beams.push_back({v.x0, v.y0, ex, ey, 11.0f * s * flick, 255, 135, 35, 0.90f * fade, 0.25f * fade});
                    beams.push_back({v.x0, v.y0, ex, ey, 5.5f * s * flick, 255, 220, 110, 0.95f * fade, 0.45f * fade});
                } else {
                    if (t < 0.45f) {
                        const float fa = 1.0f - (t / 0.45f);
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, 8.5f * s, 255, 220, 110, fa, fa});
                    }
                    const float tail = std::max(0.0f, t - 0.32f);
                    const float bx0 = v.x0 + (v.x1 - v.x0) * tail;
                    const float by0 = v.y0 + (v.y1 - v.y0) * tail;
                    const float bx1 = v.x0 + (v.x1 - v.x0) * t;
                    const float by1 = v.y0 + (v.y1 - v.y0) * t;
                    beams.push_back({bx0, by0, bx1, by1, 2.0f * s, 255, 240, 190, 0.0f, 1.0f});
                    if (t > 0.78f) {
                        const float ia = (1.0f - t) / 0.22f;
                        beams.push_back({v.x1, v.y1, v.x1, v.y1, 5.5f * s, 255, 210, 120, ia * 0.85f, ia * 0.85f});
                    }
                }
            }
            vfxWasNonEmpty = !beams.empty();
            renderer.setBeams(std::move(beams));
        }
        if (scene && selection) {
            bool showTip = false;
            if (SDL_GetMouseFocus() == window && !briefing.active() && !menu.active()) {
                float mx{}, my{};
                SDL_GetMouseState(&mx, &my);
                int winW = 1, winH = 1;
                SDL_GetWindowSize(window, &winW, &winH);
                const float barTop = hud ? static_cast<float>(winH) - static_cast<float>(hud->height()) : static_cast<float>(winH);
                if (my < barTop) {
                    const auto [mapX, mapY] = renderer.mapPosition(mx, my);
                    if (const auto hovered = selection->infantryAt(int(std::floor(mapX)), int(std::floor(mapY)))) {
                        const auto& u = scene->units().at(*hovered);
                        if (u.hp > 0 && u.visible) {
                            const int maxHp = (*hovered < initialUnitHp.size()) ? initialUnitHp[*hovered] : 100;
                            ss2::TooltipContent tc;
                            tc.title = hudClassLabel(u.id);
                            tc.gauge = std::clamp(static_cast<float>(u.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
                            if (u.owner == 0) {
                                tc.lines.push_back("Health " + std::to_string(u.hp) + " / " + std::to_string(maxHp));
                                tc.lines.push_back(crouchedUnits.contains(*hovered) ? "Crouching" : "Standing");
                                if (holdFireUnits.contains(*hovered)) tc.lines.push_back("Holding fire");
                            } else {
                                tc.lines.push_back("Hostile");
                            }
                            tooltip.show(tc, mx, my);
                            showTip = true;
                        }
                    }
                }
            }
            if (!showTip) tooltip.hide();
        }
        if (audio) audio->updateMusic();
        const auto noticeFrame = std::chrono::steady_clock::now();
        if (orderNotice.advance(noticeFrame - lastNoticeFrame) && paused && !menu.active()) showPause();
        lastNoticeFrame = noticeFrame;
        tooltip.draw(renderer);
        orderNotice.draw(renderer);
        briefing.draw(renderer);
        menu.draw(renderer);
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
    std::filesystem::path gameDir, mission, hdPack;
    int limit = 0;
    bool fourK = false, validate = false, timing = false;
    std::optional<bool> fullscreen;
    bool originalInfantry = false;
    bool autoEngage = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "ss2 [--game-dir DIR | --mission FILE.ssm] [--hd-pack DIR] [--original-infantry] [--no-auto-engage] [--4k] [--fullscreen | --windowed] [--frames N] [--validate] [--timing]\n"
                         "--game-dir opens Tutorial I from an extracted Gold 2.2 game directory.\n"
                         "--original-infantry uses the original infantry size. The default is 125%.\n"
                         "--no-auto-engage disables automatic firing on spotted enemies (manual attack orders only).\n"
                         "--hd-pack loads an optional 2x HD asset pack (tutorial1_2x.ss2hd) for --game-dir.\n"
                         "--mission displays an original mission overview.\n";
            return 0;
        } else if (arg == "--game-dir" && i + 1 < argc) gameDir = argv[++i];
        else if (arg == "--hd-pack" && i + 1 < argc) hdPack = argv[++i];
        else if (arg == "--mission" && i + 1 < argc) mission = argv[++i];
        else if (arg == "--original-infantry") originalInfantry = true;
        else if (arg == "--no-auto-engage") autoEngage = false;
        else if (arg == "--4k") fourK = true;
        else if (arg == "--fullscreen") fullscreen = true;
        else if (arg == "--windowed") fullscreen = false;
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
    if (!hdPack.empty() && !mission.empty()) throw std::invalid_argument("--hd-pack requires --game-dir");
    if (mission.empty() && !limit && !timing) {
        std::filesystem::path dataHome;
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) dataHome = xdg;
        else if (const char* home = std::getenv("HOME")) dataHome = std::filesystem::path(home) / ".local/share";
        if (gameDir.empty()) {
            if (const char* configured = std::getenv("SS2_GAME_DIR"); configured && *configured) gameDir = configured;
            else if (!dataHome.empty() && std::filesystem::is_directory(dataHome / "ss2/game"))
                gameDir = dataHome / "ss2/game";
        }
        if (!originalInfantry && !gameDir.empty() && hdPack.empty() && !dataHome.empty()) {
            auto installed = dataHome / "ss2/hd-pack";
            if (std::filesystem::is_regular_file(installed / "tutorial1_ground_2x.ss2hd") &&
                std::filesystem::is_directory(installed / "infantry-art")) hdPack = installed;
        }
    }
    SDLSession sdl;
    auto flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen.value_or(false)) flags |= SDL_WINDOW_FULLSCREEN;
    int width = fourK ? 3840 : 1280, height = fourK ? 2160 : 720;
    if (!fullscreen.has_value() && !gameDir.empty() && !limit && !timing && !fourK) {
        SDL_Rect workArea{};
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &workArea)) {
            width = std::max(1, std::min(workArea.w * 4 / 5, workArea.h * 4 / 5 * 16 / 9));
            height = std::max(1, width * 9 / 16);
        }
    }
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Sudden Strike II | Native renderer", width, height, flags), SDL_DestroyWindow);
    if (!window) throw std::runtime_error(SDL_GetError());
    ss2::ValidationReport report;
    int result = runWindow(window.get(), validate ? &report : nullptr, limit, timing, gameDir, mission, hdPack, originalInfantry, autoEngage);
    if (validate) {
        std::cout << "Validation errors: " << report.errors.load() << '\n';
        if (report.errors.load()) return 1;
    }
    return result;
} catch (const std::exception& e) {
    std::cerr << "ss2: " << e.what() << '\n';
    return 1;
}
