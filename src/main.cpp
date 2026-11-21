#include "render/window.hpp"
#include "render/plane_atlas.hpp"
#include "sim/tutorial_coordinates.hpp"
#include "assets/hd_pack.hpp"
#include "assets/infantry.hpp"
#include "assets/mission.hpp"
#include "assets/scenery.hpp"
#include "assets/sprite.hpp"
#include "assets/terrain.hpp"
#include "assets/unit_catalog.hpp"
#include "assets/vehicles.hpp"
#include "assets/sound_bank.hpp"
#include "audio/sound_player.hpp"
#include "ui/tutorial_selection.hpp"
#include "ui/briefing.hpp"
#include "ui/hidpi.hpp"
#include "ui/hud.hpp"
#include "ui/menu.hpp"
#include "ui/frontend.hpp"
#include "ui/gold_frontend.hpp"
#include "ui/settings.hpp"
#include "ui/tooltip.hpp"
#include "ui/strings.hpp"
#include "sim/mission_runner.hpp"
#include "video/movie_player.hpp"
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

ss2::assets::VehicleUnit scaleVehicle(ss2::assets::VehicleUnit unit, int scale) {
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
              const std::filesystem::path& hdPack, bool originalInfantry, bool autoEngage,
              ss2::UserSettings& settings, bool persistSettings,
              bool overviewOnly = false, std::optional<std::filesystem::path>* nextMissionOut = nullptr) {
    const auto persistNow = [&] {
        if (persistSettings) {
            (void)ss2::saveUserSettings(ss2::defaultSettingsPath(), settings);
        }
    };
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
    std::optional<ss2::assets::VehicleScene> vehicleScene;
    std::map<std::string, unsigned> gpuAtlasSlots;
    std::filesystem::path gpuArtDir;
    unsigned gpuNextSlot = 0;
    constexpr unsigned planeAtlasBase = 16u;
    constexpr unsigned planeAtlasCount = 8u;
    static_assert(planeAtlasBase + planeAtlasCount <= ss2::VulkanWindow::spriteAtlasSlots,
                  "plane atlas range must fit below the beam/hud slots");
    bool gpuPreloadLogged = false;
    std::set<std::string> gpuFallbackIds;
    struct WorldGpuSprite {
        ss2::Sprite sprite;
        int sortY{0};
        int mapX1x{0}, mapY1x{0};
    };
    std::vector<WorldGpuSprite> worldGpuSprites;
    struct LivePlane {
        ss2::AirSortiePose pose;
        unsigned atlasSlot{planeAtlasBase};
        std::string spriteKey;
        float sprW{32.0f}, sprH{32.0f};
        float texW{32.0f}, texH{32.0f};
        float shadowScale{1.0f};
    };
    std::map<std::uint32_t, LivePlane> livePlanes;
    bool planesClearedNeeded = false;
    struct FallenCorpse {
        int x{0}, y{0};
        std::string id;
        unsigned direction{0};
    };
    std::vector<FallenCorpse> fallenSoldiers;
    std::set<size_t> woundedBarLogged;
    std::vector<std::uint8_t> latestFog;
    unsigned fogGridW = 128, fogGridH = 128;
    int mapScale = 1;
    std::set<size_t> crouchedUnits;
    std::set<size_t> holdFireUnits;
    std::map<size_t, std::chrono::steady_clock::time_point> recoilUntil;
    const auto animStart = std::chrono::steady_clock::now();
    auto lastAnimSync = animStart;
    const auto fogBrightnessAt = [&](int mx, int my) -> float {
        if (latestFog.size() != std::size_t(fogGridW) * std::size_t(fogGridH)) return 1.0f;
        const int originX = int(fogGridH) * 32;
        const int d = mx - originX;
        const int tx = std::clamp((my + d / 2) / 32, 0, int(fogGridW) - 1);
        const int ty = std::clamp((my - d / 2) / 32, 0, int(fogGridH) - 1);
        const std::uint8_t f = latestFog[std::size_t(ty) * std::size_t(fogGridW) + std::size_t(tx)];
        if (f & 0x01u) return 1.0f;
        return (f & 0x10u) ? 0.70f : 0.44f;
    };
    const auto ensureGpuInfantrySlot = [&](const std::string& id) -> bool {
        if (gpuAtlasSlots.contains(id)) return true;
        const auto logFallback = [&](const char* reason) {
            if (!gpuArtDir.empty() && gpuFallbackIds.insert(id).second)
                std::cout << "GPU 4x HD infantry fallback Gold: " << id << " (" << reason << ")\n";
        };
        if (gpuArtDir.empty() || gpuNextSlot >= planeAtlasBase) {
            logFallback("slot budget full");
            return false;
        }
        const auto path = gpuArtDir / (id + ".png");
        if (!std::filesystem::is_regular_file(path)) {
            logFallback("no 4x sheet");
            return false;
        }
        auto atlas = ss2::assets::loadPngRgba(path);
        if (atlas.width != 1024 || atlas.height != 640) {
            logFallback("bad sheet dimensions");
            return false;
        }
        renderer.setSpriteAtlas(gpuNextSlot, atlas);
        gpuAtlasSlots.emplace(id, gpuNextSlot++);
        if (gpuPreloadLogged)
            std::cout << "GPU 4x HD infantry lazy slot: " << id << " (slot " << (gpuNextSlot - 1) << ")\n";
        return true;
    };
    const auto syncGpuSprites = [&] {
        if (!scene) return;
        if (gpuAtlasSlots.empty() && worldGpuSprites.empty() && livePlanes.empty()) {
            if (planesClearedNeeded) {
                renderer.setSprites({});
                planesClearedNeeded = false;
            }
            return;
        }
        planesClearedNeeded = !livePlanes.empty();
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
        const float dstSize = 32.0f * static_cast<float>(mapScale);
        constexpr float footRatioY = 116.0f / 128.0f;
        for (const auto& c : fallenSoldiers) {
            if (!gpuAtlasSlots.contains(c.id)) continue;
            const unsigned col = c.direction & 7u;
            const float cw = dstSize * 0.96f;
            const float ch = dstSize * 0.36f;
            items.push_back({
                {
                    static_cast<float>(c.x * mapScale) - 0.5f * cw,
                    static_cast<float>(c.y * mapScale) - 0.65f * ch,
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
            const float animScale = static_cast<float>(mapScale) * 0.5f;
            ox *= animScale;
            oy *= animScale;
            items.push_back({
                {
                    static_cast<float>(u.x * mapScale) - 0.5f * w + ox,
                    static_cast<float>(u.y * mapScale) - footRatioY * h + oy,
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
        for (const auto& [sid, lp] : livePlanes) {
            (void)sid;
            const float altScale = lp.shadowScale;
            const float px = static_cast<float>(lp.pose.x * mapScale);
            const float py = static_cast<float>(lp.pose.y * mapScale);
            items.push_back({
                {
                    px - lp.sprW * 0.5f + 44.0f * altScale * static_cast<float>(mapScale),
                    py - lp.sprH * 0.5f + 66.0f * altScale * static_cast<float>(mapScale),
                    lp.sprW,
                    lp.sprH,
                    0.0f, 0.0f, lp.texW, lp.texH,
                    lp.atlasSlot,
                    0.0f,
                    0.40f
                },
                999999
            });
            items.push_back({
                {
                    px - lp.sprW * 0.5f,
                    py - lp.sprH * 0.5f,
                    lp.sprW,
                    lp.sprH,
                    0.0f, 0.0f, lp.texW, lp.texH,
                    lp.atlasSlot,
                    1.0f,
                    1.0f
                },
                1000000
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
        enum class Kind { Tracer, CannonShell, Rocket, Flamer, Explosion, VehicleExplosion, HealPulse, RepairPulse, Parachute };
        Kind kind{Kind::Tracer};
        float x0{}, y0{}, x1{}, y1{};
        std::chrono::steady_clock::time_point start;
        std::chrono::milliseconds duration{};
    };
    struct BurningWreck {
        float x{}, y{};
        std::chrono::steady_clock::time_point until;
    };
    struct MinimapPing {
        float u{}, v{};
        bool hostile{false};
        std::chrono::steady_clock::time_point start;
    };
    std::vector<ActiveVfx> activeVfx;
    std::vector<BurningWreck> burningWrecks;
    std::vector<MinimapPing> minimapPings;
    bool vfxWasNonEmpty = false;
    bool attackMoveArmed = false;
    bool attackGroundArmed = false;
    bool reverseMoveArmed = false;
    bool patrolArmed = false;
    bool faceArmed = false;
    std::set<std::string> registeredPortraits;
    std::optional<ss2::assets::AirMissionKind> armedAirSupport;
    std::array<int, 4> missionPlayerTeams{0, 1, 2, 3};
    const auto isAllyOwner = [&](int owner) -> bool {
        return owner == 0 || (owner >= 0 && owner < 4 && owner != 3 &&
                              missionPlayerTeams[static_cast<std::size_t>(owner)] != 3 &&
                              missionPlayerTeams[static_cast<std::size_t>(owner)] == missionPlayerTeams[0]);
    };
    const auto isHostileOwner = [&](int owner) -> bool {
        return owner != 3 && !isAllyOwner(owner);
    };
    bool combatEffects = settings.combatEffects;   // tracers, muzzle and impact flashes, flame jets; a pause-menu setting
    bool menuPausedClock = false;
    bool smoothTerrain = settings.smoothTerrain;
    std::string currentObjective = "Follow Tutorial I orders";
    std::array<std::set<size_t>, 10> hudGroups;
    std::array<std::set<size_t>, 10> hudVehicleGroups;
    std::vector<ss2::MapBar> lastUnitBars;
    std::vector<int> initialUnitHp;
    std::vector<int> initialVehicleHp;
    std::vector<int> unitExpa;
    std::vector<int> unitKills;
    std::vector<int> unitGrenades;
    std::vector<int> unitMineAmmo;
    std::vector<ss2::VisibleMine> liveVisibleMines;
    std::size_t idleUnitCursor = 0;
    std::size_t triageCursor = 0;
    std::vector<int> vehExpa;
    std::vector<int> vehKills;
    std::vector<int> vehicleAmmo2;
    std::vector<std::pair<int, int>> vehicleCargo;
    std::vector<int> vehicleTowed;
    std::vector<int> vehicleTowedBy;
    struct LiveBuilding {
        int x{}, y{}, left{}, top{}, width{}, height{};
        int hp{}, maxHp{}, capacity{}, occupantCount{};
        int owner{-1};
        bool destroyed{false};
        int ruinLeft{}, ruinTop{};
        ss2::assets::MapImage ruinedPatch{};
        ss2::assets::MapImage intactPatch{};
        bool rubbleStamped{false};
        std::vector<size_t> occupantIds;
    };
    std::vector<LiveBuilding> liveBuildings;
    std::optional<std::size_t> selectedBuilding;
    ss2::assets::CraterCatalog craterCatalog;
    unsigned craterSeed = 1;
    std::map<std::string, ss2::assets::MapImage> planeSpriteCache;
    ss2::PlaneAtlasAllocator planeAtlas(planeAtlasBase, planeAtlasCount);
    bool planeOverflowLogged = false;
    const auto veterancyRank = [](int expa) -> const char* {
        if (expa >= 24576) return "Elite";
        if (expa >= 12288) return "Veteran";
        if (expa >= 4096) return "Regular";
        return "Recruit";
    };
    const auto initialGrenadesForId = [](std::string_view id) -> int {
        std::string low(id);
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low.find("medic") != std::string::npos || low.find("officer") != std::string::npos ||
            low.find("general") != std::string::npos || low.find("mortar") != std::string::npos ||
            low.find("flam") != std::string::npos || low.find("baz") != std::string::npos ||
            low.find("faust") != std::string::npos || low.find("shreck") != std::string::npos ||
            low.find("piat") != std::string::npos || low.find("ptrs") != std::string::npos ||
            low.find("ptrd") != std::string::npos || low.find("at") == 0) {
            return 0;
        }
        return 2;
    };
    const auto applyBuildingRubble = [&](std::size_t bIdx) {
        if (!scene || bIdx >= liveBuildings.size()) return;
        auto& lb = liveBuildings[bIdx];
        if (lb.rubbleStamped || lb.ruinedPatch.width == 0 || lb.ruinedPatch.height == 0) return;
        if (lb.intactPatch.width == 0 || lb.intactPatch.height == 0) {
            lb.intactPatch = scene->copyGroundRect(
                lb.ruinLeft, lb.ruinTop, static_cast<int>(lb.ruinedPatch.width), static_cast<int>(lb.ruinedPatch.height));
        }
        const auto patch = scene->stampGroundPatch(lb.ruinLeft, lb.ruinTop, lb.ruinedPatch);
        if (patch.pixels.width && patch.pixels.height) {
            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
        }
        lb.rubbleStamped = true;
    };
    const auto restoreBuildingIntact = [&](std::size_t bIdx) {
        if (!scene || bIdx >= liveBuildings.size()) return;
        auto& lb = liveBuildings[bIdx];
        if (!lb.rubbleStamped || lb.intactPatch.width == 0 || lb.intactPatch.height == 0) return;
        const auto patch = scene->stampGroundPatch(lb.ruinLeft, lb.ruinTop, lb.intactPatch);
        if (patch.pixels.width && patch.pixels.height) {
            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
        }
        lb.rubbleStamped = false;
    };
    const auto stampBattleCrater = [&](int simX, int simY, bool large) {
        if (!scene) return;
        const auto& vec = (large && !craterCatalog.largeCraters.empty())
            ? craterCatalog.largeCraters
            : (!craterCatalog.smallCraters.empty() ? craterCatalog.smallCraters : craterCatalog.largeCraters);
        if (vec.empty()) return;
        const auto& img = vec[(craterSeed++) % vec.size()];
        const auto patch = scene->stampAlphaOverlay(simX, simY, img);
        if (patch.pixels.width && patch.pixels.height) {
            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
        }
    };
    unsigned mapScheme = 0;
    std::shared_ptr<const ss2::assets::UnitCatalog> unitCatalog;
    const auto initialMinesForId = [&](std::string_view id) -> int {
        std::string low(id);
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr;
        if (low.find("sapper") != std::string::npos || low.find("miner") != std::string::npos ||
            (spec && (spec->selType == "sapper" || spec->selType == "sappers"))) {
            return 5;
        }
        if (spec && !spec->isVehicle && !spec->isMedicInfantry() &&
            spec->layMineTime > 0 && spec->mineAmmo > 0) {
            return std::clamp(spec->mineAmmo, 1, 5);
        }
        return 0;
    };
    std::string pendingMissionBriefing;
    std::string missionBriefingText;
    bool manualBriefingOpen = false;
    const auto isCombatAmmoSoldierId = [](std::string_view id) -> bool {
        std::string low(id);
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return low.find("medic") == std::string::npos && low.find("officer") == std::string::npos;
    };
    const auto isArmedVehicleId = [&](const std::string& id) -> bool {
        const auto* vSpec = unitCatalog ? unitCatalog->find(id) : nullptr;
        return vSpec && ((vSpec->shot1.damage > 0 && vSpec->shot1.range > 0) ||
                         (vSpec->shot2.damage > 0 && vSpec->shot2.range > 0));
    };
    const auto scaledSoldierAmmo = [&](const std::string& id, int rawAmmo) -> int {
        const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr;
        if (spec && !spec->isVehicle && spec->ammo1 > 0) {
            return rawAmmo > 0 ? std::max(1, (spec->ammo1 * std::clamp(rawAmmo, 1, 100)) / 100) : spec->ammo1;
        }
        return rawAmmo;
    };
    const auto soldierHasBinoculars = [&](const std::string& id) -> bool {
        if (const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr) {
            if (!spec->isVehicle && spec->hasBinoculars()) return true;
        }
        std::string low(id);
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return low.find("officer") != std::string::npos || low.find("general") != std::string::npos ||
               low.find("sniper") != std::string::npos;
    };
    const auto isBombardInfantryId = [&](const std::string& id) -> bool {
        if (const auto* spec = unitCatalog ? unitCatalog->find(id) : nullptr) {
            if (!spec->isVehicle && (spec->isMortarInfantry() || spec->hasFlamethrower())) return true;
        }
        std::string low(id);
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return low.find("mortar") != std::string::npos || low.find("flam") != std::string::npos;
    };
    std::optional<std::filesystem::path> nextCampaignMissionPath;
    std::string nextCampaignMissionTitle;
    std::filesystem::path currentMissionPath;
    unsigned hudMapW = 0, hudMapH = 0;
    ss2::assets::MapImage baseMinimap;
    std::chrono::steady_clock::time_point lastMinimapFogUpdate{};
    unsigned enemyInfKilled = 0;
    unsigned enemyVehDestroyed = 0;
    unsigned buildingsDemolished = 0;
    unsigned airSortiesFlown = 0;
    int enemyScoreEarned = 0;
    int friendlyScoreLost = 0;
    unsigned enemyTanksDestroyed = 0;
    unsigned enemyGunsDestroyed = 0;
    unsigned enemyPlanesShotDown = 0;
    std::vector<bool> vehicleCrewImpaired;
    std::vector<bool> vehicleMoveImpaired;
    std::vector<bool> vehicleSuppressed;
    std::vector<bool> vehicleHatchOpen;
    std::set<std::size_t> movingSmokeVehicles;
    const auto hudClassLabel = [](const std::string& id) -> std::string {
        if (id == "dguner") return "Rifleman";
        if (id == "dsmguner") return "SMG";
        if (id == "dflamer") return "Flamer";
        if (id == "dofficer") return "Officer";
        if (id == "aguner") return "US Rifleman";
        if (id == "aofficer") return "US Officer";
        if (id == "cmedic") return "Medic";
        if (id == "rsmguner") return "Soviet SMG";
        if (id.size() >= 2) {
            std::string nation;
            switch (id[0]) {
            case 'a': nation = "US "; break;
            case 'b': nation = "British "; break;
            case 'd': nation = "German "; break;
            case 'r': nation = "Soviet "; break;
            case 'j': nation = "Japanese "; break;
            default: break;
            }
            const std::string_view role(id.data() + (nation.empty() ? 0 : 1), id.size() - (nation.empty() ? 0 : 1));
            if (role.find("smgun") != std::string_view::npos) return nation + "SMG";
            if (role.find("guner") != std::string_view::npos || role.find("rifle") != std::string_view::npos) return nation + "Rifleman";
            if (role.find("sniper") != std::string_view::npos) return nation + "Sniper";
            if (role.find("officer") != std::string_view::npos) return nation + "Officer";
            if (role.find("flam") != std::string_view::npos) return nation + "Flamer";
            if (role.find("baz") != std::string_view::npos || role.find("faust") != std::string_view::npos ||
                role.find("at") != std::string_view::npos) return nation + "AT Soldier";
            if (role.find("medic") != std::string_view::npos) return "Medic";
            if (role.find("crew") != std::string_view::npos || role.find("pilot") != std::string_view::npos) return nation + "Crew";
            if (role.find("miner") != std::string_view::npos || role.find("sapper") != std::string_view::npos) return nation + "Sapper";
            if (role.find("para") != std::string_view::npos) return nation + "Paratrooper";
        }
        if (id == "jeep") return "Jeep";
        if (id == "kubel" || id == "kubelwagen") return "Kubelwagen";
        if (id.find("maultier") != std::string_view::npos) return "Maultier";
        if (id.find("sherman") != std::string_view::npos) return "Sherman";
        if (id == "panther") return "Panther";
        if (id == "tiger") return "Tiger";
        if (id == "hetzer") return "Hetzer";
        if (id == "wirbelwind") return "Wirbelwind";
        if (id.find("nebelwerfer") != std::string_view::npos) return "Nebelwerfer";
        if (id == "calliope") return "Calliope";
        if (id == "zis-3") return "ZiS-3";
        if (id == "m3a1") return "AT Gun";
        if (id == "ds39") return "Heavy MG";
        if (id == "m16") return "M16 Halftrack";
        return id;
    };
    const auto hudDisplayName = [&](const std::string& id) -> std::string {
        if (unitCatalog) {
            if (const auto* spec = unitCatalog->find(id)) {
                if (!spec->displayName.empty()) return spec->displayName;
                if (!spec->shortName.empty()) return spec->shortName;
            }
        }
        return hudClassLabel(id);
    };
    const auto scoreValueForId = [&](const std::string& id, bool isVehicle) -> int {
        if (unitCatalog) {
            if (const auto* spec = unitCatalog->find(id)) {
                return spec->effectiveScoreValue();
            }
        }
        return isVehicle ? 250 : 100;
    };
    const auto ensureUnitPortrait = [&](const std::string& id, bool isVehicle) {
        if (!hud || id.empty()) return;
        const std::string disp = hudDisplayName(id);
        if (registeredPortraits.contains(disp)) return;
        std::optional<ss2::assets::MapImage> portrait;
        if (isVehicle && vehicleScene) {
            portrait = vehicleScene->renderUnitPortrait(id, 48u);
        } else if (!isVehicle && scene) {
            portrait = scene->renderUnitPortrait(id, 48u);
        }
        if (!portrait) return;
        hud->setPortrait(disp, *portrait);
        const std::string lbl = hudClassLabel(id);
        if (!lbl.empty() && lbl != disp && !registeredPortraits.contains(lbl)) {
            hud->setPortrait(lbl, *portrait);
            registeredPortraits.insert(lbl);
        }
        registeredPortraits.insert(disp);
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
    const auto updateMinimapFog = [&](const std::vector<std::uint8_t>& fogGrid, bool force) {
        if (!hud || !baseMinimap.width || !baseMinimap.height || !hudMapW || !hudMapH ||
            !fogGridW || !fogGridH || fogGrid.size() != std::size_t(fogGridW) * fogGridH) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastMinimapFogUpdate.time_since_epoch().count() != 0 &&
            now - lastMinimapFogUpdate < std::chrono::milliseconds(200)) {
            return;
        }
        ss2::assets::MapImage masked = baseMinimap;
        const int originX = int(fogGridH) * 32;
        const std::uint64_t mapW = hudMapW;
        const std::uint64_t mapH = hudMapH;
        for (unsigned py = 0; py < masked.height; ++py) {
            const int my = static_cast<int>((std::uint64_t(py) * mapH) / masked.height);
            const int my2 = 2 * my;
            for (unsigned px = 0; px < masked.width; ++px) {
                const int mx = static_cast<int>((std::uint64_t(px) * mapW) / masked.width);
                const int d = mx - originX;
                const int u = (d + my2) / 2;
                const int v = (my2 - d) / 2;
                const int tx = u >> 5;
                const int ty = v >> 5;
                int num = 38;
                if (tx >= 0 && ty >= 0 && tx < int(fogGridW) && ty < int(fogGridH)) {
                    const std::uint8_t f = fogGrid[std::size_t(ty) * fogGridW + std::size_t(tx)];
                    num = (f & 0x01u) ? 256 : ((f & 0x10u) ? 124 : 38);
                }
                if (num < 256) {
                    const std::size_t idx = (std::size_t(py) * masked.width + px) * 4u;
                    masked.rgba[idx + 0] = static_cast<std::uint8_t>((unsigned(masked.rgba[idx + 0]) * unsigned(num)) >> 8u);
                    masked.rgba[idx + 1] = static_cast<std::uint8_t>((unsigned(masked.rgba[idx + 1]) * unsigned(num)) >> 8u);
                    masked.rgba[idx + 2] = static_cast<std::uint8_t>((unsigned(masked.rgba[idx + 2]) * unsigned(num)) >> 8u);
                }
            }
        }
        hud->setMinimap(masked);
        lastMinimapFogUpdate = now;
    };
    ss2::Briefing briefing(window);
    ss2::Notice orderNotice(window);
    std::deque<std::string> pendingDialogs;
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> moveCursor(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER), SDL_DestroyCursor);
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> attackCursor(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR), SDL_DestroyCursor);
    const auto resetCursor = [&] { SDL_SetCursor(SDL_GetDefaultCursor()); };
    if (!mission.empty() && (gameDir.empty() || overviewOnly)) {
        renderer.setMap(ss2::assets::readMissionOverview(mission));
        const auto title = "Sudden Strike II | " + mission.stem().string() + " | Mission overview";
        SDL_SetWindowTitle(window, title.c_str());
    } else if (!gameDir.empty()) {
        unitCatalog = ss2::assets::UnitCatalog::load(gameDir);
        const auto missionPath = mission.empty()
            ? ss2::assets::resolveCaseInsensitivePath(gameDir, "maps/single/--- TUTORIAL I ---.SSM")
            : mission;
        currentMissionPath = missionPath;
        const bool isTutorial1 = mission.empty() ||
            ss2::assets::equalsIgnoreCaseAscii(missionPath.filename().string(), "--- TUTORIAL I ---.SSM");
        if (!isTutorial1) {
            std::error_code normEc;
            const auto normCur = std::filesystem::weakly_canonical(missionPath, normEc);
            for (const auto& c : ss2::assets::listCampaigns(gameDir)) {
                for (std::size_t mi = 0; mi + 1 < c.missions.size(); ++mi) {
                    const auto normM = std::filesystem::weakly_canonical(c.missions[mi].path, normEc);
                    const auto normE = !c.missions[mi].easyPath.empty()
                        ? std::filesystem::weakly_canonical(c.missions[mi].easyPath, normEc)
                        : std::filesystem::path{};
                    if (normM == normCur || (!normE.empty() && normE == normCur)) {
                        const bool isEasy = (!normE.empty() && normE == normCur);
                        nextCampaignMissionPath = (isEasy && !c.missions[mi + 1].easyPath.empty())
                            ? c.missions[mi + 1].easyPath
                            : c.missions[mi + 1].path;
                        nextCampaignMissionTitle = c.missions[mi + 1].title;
                        break;
                    }
                }
                if (nextCampaignMissionPath) break;
            }
        }
        auto data = ss2::assets::readMissionData(missionPath);
        if (!data.description.empty()) {
            missionBriefingText = data.description;
        }
        if (!isTutorial1 && !limit && !timing && !data.description.empty()) {
            pendingMissionBriefing = data.description;
        }
        fogGridW = data.width;
        fogGridH = data.height;
        mapScheme = data.scheme;
        auto map = isTutorial1
            ? ss2::assets::loadTutorialTerrain(gameDir)
            : ss2::assets::loadMissionTerrain(gameDir, missionPath);
        ss2::assets::SceneryDepth depth(map.width, map.height);
        std::vector<ss2::assets::BuildingPlacement> missionBuildings;
        const auto objects = isTutorial1
            ? ss2::assets::drawTutorialScenery(map, gameDir, &depth, 0, &missionBuildings)
            : ss2::assets::drawMissionScenery(map, gameDir, missionPath, &depth, &missionBuildings);
        const auto placements = isTutorial1
            ? ss2::assets::readTutorialPlacements(gameDir)
            : ss2::assets::readMissionPlacements(missionPath);
        vehicleScene.emplace(gameDir, placements);
        const auto& vehicleUnits = vehicleScene->vehicles();
        std::vector<ss2::assets::Placement> vehicles;
        vehicles.reserve(vehicleUnits.size());
        for (const auto& v : vehicleUnits) {
            vehicles.push_back({v.id, v.x, v.y, int(v.hullDir), v.hp, v.ammo, v.group, v.owner});
        }
        std::size_t drawnVehicles = vehicleUnits.size();
        const auto mapWidth = map.width, mapHeight = map.height;
        ss2::assets::MapImage hdImage;
        std::filesystem::path infantryArt;
        bool cleanGround = false;
        if (!hdPack.empty() && !originalInfantry) {
            gpuArtDir = hdPack / "infantry-art-4x";
            if (std::filesystem::is_directory(gpuArtDir)) {
                std::set<std::string> vehicleIds;
                for (const auto& v : vehicleUnits) vehicleIds.insert(v.id);
                for (const auto& p : placements) {
                    if (!vehicleIds.contains(p.id)) ensureGpuInfantrySlot(p.id);
                }
                if (!gpuAtlasSlots.empty()) {
                    std::cout << "GPU 4x HD infantry: " << gpuAtlasSlots.size() << " classes (";
                    bool firstId = true;
                    for (const auto& [id, slot] : gpuAtlasSlots) {
                        (void)slot;
                        if (!firstId) std::cout << ", ";
                        firstId = false;
                        std::cout << id;
                    }
                    std::cout << ")\n";
                }
            } else {
                gpuArtDir.clear();
            }
        }
        if (isTutorial1 && !hdPack.empty()) {
            auto groundPath = hdPack / "tutorial1_ground_2x.ss2hd";
            cleanGround = std::filesystem::is_regular_file(groundPath);
            if (!originalInfantry && std::filesystem::is_directory(hdPack / "infantry-art"))
                infantryArt = hdPack / "infantry-art";
            auto hd = ss2::assets::loadHdPackMap(cleanGround ? groundPath : hdPack);
            if (hd.scale != 2) throw std::runtime_error("Tutorial I requires a 2x HD pack");
            mapScale = 2;
            hdImage = std::move(hd.image);
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
                        if (gpuNextSlot >= planeAtlasBase) continue;
                        const auto ap = worldDir / (name + ".png");
                        if (!std::filesystem::is_regular_file(ap)) continue;
                        auto atlas = ss2::assets::loadPngRgba(ap);
                        renderer.setSpriteAtlas(gpuNextSlot, atlas);
                        worldSlots.emplace(name, gpuNextSlot++);
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
        if (isTutorial1 && mapScale == 2 && !cleanGround) {
            ss2::assets::drawTutorialVehicles(map, gameDir, placements);
        }
        scene.emplace(std::move(map), gameDir, placements, originalInfantry, std::move(hdImage), infantryArt,
                      cleanGround, std::move(depth), &gpuAtlasSlots, data.width, data.height);
        if (mapScale == 1) {
            scene->setVehicleLayer([&](ss2::assets::MapImage& patch, int px, int py, const ss2::assets::SceneryDepth* d) {
                if (vehicleScene) vehicleScene->drawRegion(patch, px, py, d);
            });
        }
        std::vector<std::pair<std::size_t, std::size_t>> preloadedRiders;
        {
            std::size_t pCursor = 0;
            for (std::size_t vIdx = 0; vIdx < vehicleUnits.size(); ++vIdx) {
                const auto& vu = vehicleUnits[vIdx];
                while (pCursor < placements.size() &&
                       (placements[pCursor].id != vu.id || placements[pCursor].x != vu.x ||
                        placements[pCursor].y != vu.y || placements[pCursor].owner != vu.owner)) {
                    ++pCursor;
                }
                if (pCursor >= placements.size()) break;
                for (const auto& pax : placements[pCursor].passengers) {
                    if (pax.isCrew) continue;
                    ss2::assets::Placement rp{pax.id, vu.x, vu.y, 0, pax.hp, pax.ammo, pax.group, vu.owner, {}, pax.expa};
                    (void)ensureGpuInfantrySlot(pax.id);
                    if (auto spawned = scene->spawn(rp)) {
                        const auto sIdx = spawned->first;
                        const auto& u = scene->units().at(sIdx);
                        (void)scene->update(sIdx, u.x, u.y, u.direction, u.progress, u.walking, u.hp, 0);
                        preloadedRiders.emplace_back(sIdx, vIdx);
                    }
                }
                ++pCursor;
            }
        }
        if (!gpuAtlasSlots.empty()) std::cout << "GPU 4x HD infantry atlases: " << gpuAtlasSlots.size() << " classes\n";
        else if (!infantryArt.empty()) std::cout << "Painted HD infantry: " << scene->paintedCount() << "/" << scene->units().size() << '\n';
        gpuPreloadLogged = true;
        const auto& infantry = scene->units();
        if (!isTutorial1) {
            if (!data.description.empty()) {
                const auto eol = data.description.find_first_of("\r\n");
                currentObjective = data.description.substr(0, eol);
            } else {
                currentObjective = missionPath.stem().string();
            }
        }
        initialUnitHp.clear();
        initialUnitHp.reserve(infantry.size());
        unitExpa.clear();
        unitExpa.reserve(infantry.size());
        unitKills.assign(infantry.size(), 0);
        unitGrenades.clear();
        unitGrenades.reserve(infantry.size());
        unitMineAmmo.clear();
        unitMineAmmo.reserve(infantry.size());
        for (const auto& u : infantry) {
            initialUnitHp.push_back(std::max(1, u.hp));
            unitExpa.push_back(ss2::TutorialUnits::missionExpaToSim(u.expa));
            unitGrenades.push_back(initialGrenadesForId(u.id));
            unitMineAmmo.push_back(initialMinesForId(u.id));
        }
        initialVehicleHp.clear();
        initialVehicleHp.reserve(vehicleUnits.size());
        vehExpa.clear();
        vehExpa.reserve(vehicleUnits.size());
        vehKills.assign(vehicleUnits.size(), 0);
        vehicleAmmo2.assign(vehicleUnits.size(), 0);
        vehicleCargo.assign(vehicleUnits.size(), {0, 0});
        vehicleTowed.assign(vehicleUnits.size(), -1);
        vehicleTowedBy.assign(vehicleUnits.size(), -1);
        vehicleHatchOpen.assign(vehicleUnits.size(), false);
        liveBuildings.clear();
        liveBuildings.reserve(missionBuildings.size());
        for (auto& bp : missionBuildings) {
            liveBuildings.push_back({
                bp.x, bp.y, bp.left, bp.top, bp.width, bp.height,
                bp.maxHp, bp.maxHp, bp.capacity, 0, -1, false,
                bp.ruinLeft, bp.ruinTop, std::move(bp.ruinedPatch), {}, false, {}
            });
        }
        craterCatalog = ss2::assets::loadCraterCatalog(gameDir, mapScheme);
        for (std::size_t vi = 0; vi < vehicleUnits.size(); ++vi) {
            const auto& v = vehicleUnits[vi];
            vehExpa.push_back(ss2::TutorialUnits::missionExpaToSim(v.expa));
            int maxHp = std::max(1, v.hp);
            if (unitCatalog) {
                if (const auto* spec = unitCatalog->find(v.id)) {
                    if (spec->health > 0) maxHp = spec->health;
                    vehicleCargo[vi].second = std::clamp(spec->stock, 0, 44);
                    vehicleHatchOpen[vi] = spec->marchEnabled || (spec->marchSightBonus > 0);
                    vehicleAmmo2[vi] = spec->ammo2 > 0
                        ? std::max(0, (spec->ammo2 * std::clamp(v.ammo, 0, 100)) / 100)
                        : (spec->shot2.damage > 0 && spec->shot2.range > 0 ? std::max(1, v.ammo) : 0);
                }
            }
            initialVehicleHp.push_back(maxHp);
        }
        std::vector<ss2::assets::Infantry> viewInfantry = infantry;
        auto viewVehicles = vehicles;
        std::vector<ss2::assets::VehicleUnit> viewVehicleUnits;
        viewVehicleUnits.reserve(vehicleUnits.size());
        for (const auto& v : vehicleUnits) viewVehicleUnits.push_back(scaleVehicle(v, mapScale));
        const auto composedMap = scene->compose();
        hudMapW = mapWidth;
        hudMapH = mapHeight;
        hud.emplace(window);
        baseMinimap = downsampleOverview(composedMap, 512u, 256u);
        hud->setMinimap(baseMinimap);
        if (!hdPack.empty() && std::filesystem::is_directory(hdPack / "hud-portraits")) {
            for (const char* cls : {"aguner", "aofficer", "cmedic", "dflamer", "dguner", "dofficer", "dsmguner", "rsmguner"}) {
                const auto pp = hdPack / "hud-portraits" / (std::string(cls) + ".png");
                if (std::filesystem::is_regular_file(pp)) {
                    const auto img = ss2::assets::loadPngRgba(pp);
                    const std::string lbl = hudClassLabel(cls);
                    hud->setPortrait(lbl, img);
                    registeredPortraits.insert(lbl);
                    const auto disp = hudDisplayName(cls);
                    if (disp != lbl) {
                        hud->setPortrait(disp, img);
                        registeredPortraits.insert(disp);
                    }
                }
            }
        }
        for (const auto& u : infantry) ensureUnitPortrait(u.id, false);
        for (const auto& v : vehicleUnits) ensureUnitPortrait(v.id, true);
        renderer.setMap(composedMap);
        if (isTutorial1 && mapScale == 2 && !smoothTerrain) {
            smoothTerrain = true;
        }
        renderer.setMapFilterLinear(smoothTerrain);
        if (isTutorial1 && mapScale == 2) {
            for (auto& u : viewInfantry) u = scaleInfantry(u, mapScale);
            for (auto& v : viewVehicles) {
                v.x *= mapScale;
                v.y *= mapScale;
            }
            SDL_SetWindowTitle(window, "Sudden Strike II | Tutorial I (2x HD)");
            std::cout << "Tutorial I (2x HD): " << mapWidth*2 << 'x' << mapHeight*2 << ", " << objects
                      << " scenery placements, " << infantry.size() << " infantry, " << drawnVehicles << " vehicle and gun placements\n";
        } else if (isTutorial1) {
            std::cout << "Tutorial I: " << mapWidth << 'x' << mapHeight << ", " << objects
                      << " scenery placements, " << infantry.size() << " infantry, " << drawnVehicles << " vehicle and gun placements\n";
        } else {
            const auto title = "Sudden Strike II | " + missionPath.stem().string();
            SDL_SetWindowTitle(window, title.c_str());
            std::cout << "Mission " << missionPath.stem().string() << ": " << mapWidth << 'x' << mapHeight << ", "
                      << objects << " scenery placements, " << infantry.size() << " infantry, "
                      << drawnVehicles << " vehicle placements\n";
        }
        ss2::FogGrid fog(data.width, data.height, data.flags);
        fog.loadCirc64FromGame(gameDir);
        auto navigation = ss2::InfantryPathGrid::fromSubCellMasks(data.width, data.height, std::move(data.flags), std::move(data.obstacles));
        missionPlayerTeams = data.playerTeams;
        const auto initMines = data.mines;
        ss2::TutorialUnits soldiers(std::move(navigation), infantry, vehicles, std::move(fog),
                                    std::clamp(settings.difficulty, 1, 3), autoEngage, vehicleUnits, unitCatalog);
        soldiers.setPlayerTeams(missionPlayerTeams);
        soldiers.setMines(initMines);
        soldiers.setBuildings(std::move(missionBuildings));
        soldiers.setAirSupport(data.airSupport, data.paratrooperGroup, data.paratrooperRoster);
        const auto initAir = soldiers.playerAirSupport();
        for (const auto& [sIdx, vIdx] : preloadedRiders) {
            if (soldiers.embarkImmediate(sIdx, vIdx)) {
                if (vIdx < vehicleCargo.size()) ++vehicleCargo[vIdx].first;
            }
        }
        // Hide unspotted non-allied enemies from frame 0 and seed real catalog soldier ammo counts.
        const auto initSnap = soldiers.takeSnapshot();
        for (size_t i = 0; i < infantry.size(); ++i) {
            const int initAmmo = (i < initSnap.soldiers.size()) ? initSnap.soldiers[i].ammo : -1;
            const bool hiddenEnemy = !isAllyOwner(infantry[i].owner) && !soldiers.isValidAttackTarget(i, 0);
            if (hiddenEnemy || initAmmo >= 0) {
                const auto& u = scene->units().at(i);
                scene->update(i, u.x, u.y, u.direction, u.progress, u.walking, u.hp,
                              hiddenEnemy ? 0 : (u.visible ? 1 : 0), initAmmo);
                if (hiddenEnemy) viewInfantry[i].visible = false;
                if (initAmmo >= 0) viewInfantry[i].ammo = initAmmo;
            }
        }
        for (size_t i = 0; i < viewVehicleUnits.size(); ++i) {
            if (!isAllyOwner(viewVehicleUnits[i].owner) && !soldiers.isValidVehicleAttackTarget(i)) {
                const auto& v = vehicleScene->vehicles().at(i);
                vehicleScene->update(i, v.x, v.y, v.hullDir, v.turretDir, v.hp, 0);
                viewVehicleUnits[i].visible = false;
            }
        }
        for (const auto& vp : soldiers.takeVehiclePoses()) {
            vehicleScene->update(vp.index, vp.x, vp.y, vp.hullDir, vp.turretDir,
                                 vp.hp, vp.visible ? 1 : 0, vp.ammo);
            if (vp.index < viewVehicleUnits.size()) {
                viewVehicleUnits[vp.index].visible = vp.visible;
                viewVehicleUnits[vp.index].hp = vp.hp;
                viewVehicleUnits[vp.index].ammo = vp.ammo;
            }
            if (vp.index < vehicleAmmo2.size()) {
                vehicleAmmo2[vp.index] = vp.ammo2;
            }
        }
        if (auto initialFog = soldiers.takeFogSnapshot()) {
            latestFog = *initialFog;
            const auto fogPatch = scene->updateFog(*initialFog);
            if (fogPatch.pixels.width && fogPatch.pixels.height)
                renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
            updateMinimapFog(*initialFog, true);
        }
        syncGpuSprites();
        try {
            audio.emplace(ss2::assets::SoundBank::load(gameDir));
            if (!hdPack.empty() && std::filesystem::is_directory(hdPack / "audio-hd"))
                audio->setHdAudioDir(hdPack / "audio-hd");
            const auto hdMusic = (!hdPack.empty() && std::filesystem::is_directory(hdPack / "audio-hd" / "music"))
                                     ? (hdPack / "audio-hd" / "music")
                                     : ss2::assets::resolveCaseInsensitivePath(gameDir, "MUSIC");
            audio->setSfxVolume(settings.sfxVolume);
            audio->startMusic(hdMusic, settings.musicVolume);
            if (settings.musicMuted) (void)audio->toggleMusicMute();
        } catch (const std::exception&) {}
        const int selOriginX = int(data.height) * 32;
        const std::string selTitle = isTutorial1 ? (mapScale == 2 ? "Tutorial I (2x HD)" : "Tutorial I")
                                                 : missionPath.stem().string();
        simulation.emplace(std::move(data), ss2::tutorialTurnDelay(gameDir), std::move(soldiers));
        if (settings.speedPercent != 100) simulation->setSpeedPercent(settings.speedPercent);
        liveVisibleMines = simulation->visibleMines();
        selection.emplace(window, renderer, std::move(viewInfantry), std::move(viewVehicles), mapScale, selOriginX, selTitle);
        selection->setVehicles(std::move(viewVehicleUnits));
        {
            int airReadyCount = 0;
            for (const auto& sl : initAir) airReadyCount += sl.ready;
            if (airReadyCount > 0) {
                std::cout << "Air Support ready: " << airReadyCount << " sorties\n";
            }
        }
        if (!isTutorial1) {
            bool focused = false;
            for (const auto& u : infantry) {
                if (u.owner == 0) {
                    renderer.focusMap(static_cast<double>(u.x * mapScale), static_cast<double>(u.y * mapScale));
                    focused = true;
                    break;
                }
            }
            if (!focused) {
                for (const auto& v : vehicleUnits) {
                    if (v.owner == 0) {
                        renderer.focusMap(static_cast<double>(v.x * mapScale), static_cast<double>(v.y * mapScale));
                        break;
                    }
                }
            }
        }
    }
    FrameSample sample;
    const ss2::Snapshot empty{};
    std::uint64_t frames = 0;
    bool running = true;
    bool gestureInterrupted = false;
    bool markerHint = false;
    std::optional<std::pair<int, int>> objectiveMarkerSim;
    bool paused = false;
    std::size_t reportedLosses = 0;
    struct TimedMarker {
        ss2::MapMarker marker;
        std::chrono::steady_clock::time_point until;
        std::optional<size_t> targetUid;
        bool isOrder{false};
    };
    std::vector<TimedMarker> activeMarkers;
    std::set<std::size_t> binocularUnits;
    std::set<std::size_t> vehicleHoldFireUnits;
    std::set<std::size_t> camouflagedUnits;
    std::set<std::size_t> suppressedUnits;
    std::array<std::optional<std::pair<double, double>>, 8> cameraBookmarks;
    std::deque<std::pair<double, double>> eventHistory;
    std::size_t eventHistoryCursor = 0;
    const auto recordEventPos = [&](double x, double y) {
        if (!eventHistory.empty()) {
            const double dx = eventHistory.front().first - x;
            const double dy = eventHistory.front().second - y;
            if (dx * dx + dy * dy <= 96.0 * 96.0) return;
        }
        eventHistory.push_front({x, y});
        if (eventHistory.size() > 6u) eventHistory.pop_back();
        eventHistoryCursor = 0;
    };
    const auto syncMarkers = [&] {
        std::vector<ss2::MapMarker> out;
        out.reserve(activeMarkers.size());
        for (const auto& m : activeMarkers) out.push_back(m.marker);
        renderer.setMarkers(std::move(out));
    };
    std::map<std::string, std::chrono::steady_clock::time_point> lastWeaponAudio;
    auto lastUnitVoiceAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto lastUnderAttackAt = std::chrono::steady_clock::now() - std::chrono::seconds(30);
    const auto inferNation = [&](const std::string& id) -> std::string {
        if (unitCatalog) {
            if (const auto* spec = unitCatalog->find(id); spec && !spec->nation.empty() && spec->nation != "none") {
                return spec->nation;
            }
        }
        if (!id.empty()) {
            switch (static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(id.front())))) {
            case 'd': return "german";
            case 'r': return "russian";
            case 'b': return "british";
            case 'a': return "american";
            case 'j': return "japanese";
            case 'f': return "french";
            default: break;
            }
        }
        return "german";
    };
    const auto inferVoiceRole = [&](const std::string& id, bool isVehicle) -> std::string {
        std::string low = id;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (unitCatalog) {
            if (const auto* spec = unitCatalog->find(id)) {
                if (spec->isMedicInfantry()) return "med";
                if (spec->isSupplyTruck()) return "sap";
                if (spec->soundType == "gun") return "art";
                if (spec->soundType == "transport") return "trans";
                if (spec->soundType == "officer") return "off";
                if (spec->soundType == "sapper") return "sap";
                if (spec->soundType == "tank" || spec->soundType == "ftank" || spec->soundType == "itank") return "tank";
                if (spec->soundType == "soldier" || spec->soundType == "fsold" || spec->soundType == "isold") return "sold";
                if (spec->isArtillery()) return "art";
                if (spec->selType == "transport" || spec->selType == "apc" || spec->selType == "jeep") return "trans";
                if (spec->roleClass == "tank" || spec->roleClass == "stug") return "tank";
                if (spec->isOfficerInfantry()) return "off";
                if (spec->selType == "sapper" || spec->selType == "sappers") return "sap";
            }
        }
        if (low.find("officer") != std::string::npos || low.find("general") != std::string::npos) return "off";
        if (low.find("medic") != std::string::npos || low.ends_with(".med")) return "med";
        if (low.find("sapper") != std::string::npos || low.find("miner") != std::string::npos ||
            low.find("sdkfz.11") != std::string::npos || low.find("morris.c8") != std::string::npos) return "sap";
        if (low.find("pak") != std::string::npos || low.find("flak") != std::string::npos ||
            low.find("howitz") != std::string::npos || low.find("mortar") != std::string::npos) return "art";
        if (low.find("opel") != std::string::npos || low.find("truck") != std::string::npos ||
            low.find("bedford") != std::string::npos || low.find("zis") != std::string::npos) return "trans";
        return isVehicle ? "tank" : "sold";
    };
    const auto playUnitVoiceForId = [&](std::string_view verb, const std::string& id, bool isVehicle,
                                        bool force = false, bool isVeteran = false) {
        if (!audio || id.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (verb == "underattack" || verb == "close") {
            const int alertIntervalSec = unitCatalog ? unitCatalog->underAttackIntervalSec() : 15;
            if (now - lastUnderAttackAt < std::chrono::seconds(alertIntervalSec)) return;
            lastUnderAttackAt = now;
        } else if (!force && now - lastUnitVoiceAt < std::chrono::milliseconds(650)) {
            return;
        }
        const std::string nation = inferNation(id);
        const std::string role = inferVoiceRole(id, isVehicle);
        const auto* vVoiceSpec = unitCatalog ? unitCatalog->find(id) : nullptr;
        const bool isFrenchVoice = vVoiceSpec &&
            (vVoiceSpec->soundType == "fsold" || vVoiceSpec->soundType == "ftank");
        const bool isItalianVoice = vVoiceSpec && vVoiceSpec->soundType == "isold";
        const std::string pfx = isFrenchVoice ? "f_"
                              : isItalianVoice ? "i_"
                              : (nation == "russian" ? "r_" : nation == "british" ? "b_"
                              : nation == "american" ? "a_" : nation == "japanese" ? "j_"
                              : nation == "french" ? "f_" : "d_");
        const bool suppressGenericAck =
            (verb == "noammo" || verb == "laymine" || verb == "getmine" || verb == "repair" || verb == "close");
        const auto tryPlay = [&](const std::string& g) -> bool {
            const bool urgent = (verb == "underattack" || verb == "close");
            return audio->playGroup(g, urgent ? 0.72f : 0.58f,
                                    urgent ? ss2::VoicePriority::High : ss2::VoicePriority::Normal);
        };
        auto tryRole = [&](std::string_view r, std::string_view voicePfx) -> bool {
            const std::string vp(voicePfx);
            if (verb == "close") {
                if (vp == "d_" && tryPlay("d_close")) return true;
                if (tryPlay(vp + "sold_close")) return true;
                return false;
            }
            if (verb == "noammo") {
                if (tryPlay(vp + std::string(r) + "_noammo")) return true;
                if (vp == "d_" && tryPlay("d_noammo")) return true;
                if (tryPlay(vp + "sold_noammo") || tryPlay(vp + "tank_noammo")) return true;
                return false;
            }
            if (verb == "laymine") {
                if (vp == "d_" && tryPlay("d_laymine")) return true;
                if (tryPlay(vp + "sap_mine") || tryPlay(vp + "off_mine") ||
                    tryPlay(vp + "off_mines") || tryPlay(vp + "sold_laymine")) return true;
                return false;
            }
            if (verb == "getmine") {
                if (vp == "d_" && tryPlay("d_getmine")) return true;
                if (tryPlay(vp + "sap_search") || tryPlay(vp + "sold_getmine")) return true;
                return false;
            }
            if (verb == "repair") {
                if (tryPlay(vp + "sap_repair") || tryPlay(vp + "tank_repair")) return true;
                return false;
            }
            if (isVeteran && r == "sold" && vp == "d_") {
                if (verb == "select" && tryPlay("d_vet_select")) return true;
                if (verb == "move" && tryPlay("d_vet_move")) return true;
                if (verb == "attack" && tryPlay("d_vet_attack")) return true;
                if (verb == "stop" && tryPlay("d_vet_ack")) return true;
            }
            if (r == "sold" && vp == "d_") {
                if (verb == "select" && tryPlay("d_select")) return true;
                if (verb == "move" && tryPlay("d_move")) return true;
                if (verb == "attack" && tryPlay("d_attack")) return true;
                if (verb == "stop" && tryPlay("d_stop")) return true;
                if (verb == "underattack" && tryPlay("d_underattack")) return true;
            }
            if (r == "off" && vp == "b_" && verb == "select" && tryPlay("b_off_sele")) return true;
            if (r == "tank" && verb == "move" && tryPlay(vp + "tank_ack")) return true;
            if (r == "trans" && verb == "move" && tryPlay(vp + "trans_ack")) return true;
            if (r == "art" && verb == "move" && tryPlay(vp + "art_ack")) return true;
            if (tryPlay(vp + std::string(r) + "_" + std::string(verb))) return true;
            if (verb == "move" && tryPlay(vp + std::string(r) + "_ack")) return true;
            if (verb == "attack" && tryPlay(vp + std::string(r) + "_ack")) return true;
            return false;
        };
        if (tryRole(role, pfx) || (role != "sold" && tryRole("sold", pfx)) ||
            (isFrenchVoice && (tryRole(role, "b_") || (role != "sold" && tryRole("sold", "b_")))) ||
            (!suppressGenericAck &&
             tryPlay(nation == "russian" ? "ack_rus" : nation == "german" ? "ack_deu" : "ack_ally"))) {
            if (verb != "underattack" && verb != "close") lastUnitVoiceAt = now;
        }
    };
    const auto playSelectionVoice = [&](std::string_view verb) {
        if (!audio || !selection) return;
        if (!selection->selectedVehicles().empty() && vehicleScene) {
            const size_t vid = *selection->selectedVehicles().begin();
            if (vid < vehicleScene->vehicles().size()) {
                playUnitVoiceForId(verb, vehicleScene->vehicles()[vid].id, true);
                if (verb == "select") {
                    const auto* vSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles()[vid].id) : nullptr;
                    if (vSpec && !vSpec->idleSound.empty()) {
                        (void)audio->playGroup(vSpec->idleSound, 0.18f, ss2::VoicePriority::Low);
                    }
                }
                return;
            }
        }
        if (!selection->selected().empty() && scene) {
            const size_t sid = *selection->selected().begin();
            if (sid < scene->units().size()) {
                const bool isVet = (sid < unitExpa.size() && unitExpa[sid] >= 16384);
                playUnitVoiceForId(verb, scene->units()[sid].id, false, false, isVet);
            }
        }
    };
    const auto pushMinimapPing = [&](int simX, int simY, bool hostile) {
        if (!hudMapW || !hudMapH) return;
        const float u = std::clamp(static_cast<float>(simX) / static_cast<float>(hudMapW), 0.0f, 1.0f);
        const float v = std::clamp(static_cast<float>(simY) / static_cast<float>(hudMapH), 0.0f, 1.0f);
        if (minimapPings.size() >= 6) minimapPings.erase(minimapPings.begin());
        minimapPings.push_back({u, v, hostile, std::chrono::steady_clock::now()});
    };
    const auto weaponSoundForUnit = [&](const std::string& id) -> std::tuple<std::string, int, float> {
        std::string low = id;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low == "dsmguner" || low == "rsmguner" || low == "aofficer") return {"smg_burst", 420, 0.55f};
        if (low.find("hmg") != std::string::npos) return {"smg_burst", 240, 0.50f};
        if (low.find("flam") != std::string::npos) return {"fmw", 450, 0.48f};
        if (low.find("bazooka") != std::string::npos || low.find("panzerfaust") != std::string::npos ||
            low.find("piat") != std::string::npos) {
            return {"bazooka", 360, 0.52f};
        }
        if (low.find("mortar") != std::string::npos) return {"mortar", 450, 0.52f};
        if (low == "dofficer") return {"gunshot", 200, 0.42f};
        if (unitCatalog) {
            if (const auto* spec = unitCatalog->find(id); spec && !spec->shot1.soundGroup.empty()) {
                return {spec->shot1.soundGroup, 260, 0.42f};
            }
        }
        return {"rifle", 180, 0.40f};
    };
    const auto playWorldSound = [&](const std::string& group, float baseGain, int simX, int simY,
                                    ss2::VoicePriority prio = ss2::VoicePriority::Normal) -> bool {
        if (!audio) return false;
        int winW = 1, winH = 1;
        SDL_GetWindowSize(window, &winW, &winH);
        const auto [mx0, my0] = renderer.mapPosition(0.0, 0.0);
        const auto [mx1, my1] = renderer.mapPosition(static_cast<double>(winW), static_cast<double>(winH));
        const double camX = 0.5 * (mx0 + mx1);
        const double camY = 0.5 * (my0 + my1);
        const double halfDiag = 0.5 * std::hypot(mx1 - mx0, my1 - my0);
        const float g = ss2::spatialGain(baseGain, static_cast<double>(simX * mapScale) - camX,
                                         static_cast<double>(simY * mapScale) - camY, halfDiag);
        if (g <= 0.0f) return false;
        return audio->playGroup(group, g, prio);
    };
    const auto showPause = [&] {
        orderNotice.show(ss2::tr("Paused. Press Pause or Ctrl+P to resume."), ss2::NoticeKind::unsupported,
                         ss2::Notice::Duration::zero());
    };
    const auto logHud = [&](const std::string& msg) {
        if (hud && !msg.empty()) hud->log(msg);
    };
    const auto showNotice = [&](const std::string& msg, ss2::NoticeKind kind) {
        orderNotice.show(msg, kind);
        logHud(msg);
    };
    if (!pendingMissionBriefing.empty()) {
        logHud(pendingMissionBriefing);
        briefing.open(pendingMissionBriefing);
        pendingMissionBriefing.clear();
    }
    struct ForceRosterGroup {
        std::string displayName;
        std::vector<size_t> infIds;
        std::vector<size_t> vehIds;
    };
    std::vector<ss2::assets::MissionSummary> menuMissions;
    std::vector<ss2::assets::CampaignSummary> menuCampaigns;
    std::vector<ForceRosterGroup> forceRosterGroups;
    const auto airKindLabel = [](ss2::assets::AirMissionKind kind) -> const char* {
        switch (kind) {
        case ss2::assets::AirMissionKind::Bomber: return "Bomber Strike";
        case ss2::assets::AirMissionKind::Recon: return "Recon Overflight";
        case ss2::assets::AirMissionKind::Paratrooper: return "Paratrooper Drop";
        case ss2::assets::AirMissionKind::Supply: return "Air Supply Drop";
        case ss2::assets::AirMissionKind::Fighter: return "Fighter Strafe";
        }
        return "Air Support";
    };
    const auto armAirSupportKind = [&](ss2::assets::AirMissionKind kind) {
        if (!simulation) return;
        const auto slots = simulation->playerAirSupport();
        const std::size_t idx = static_cast<std::size_t>(kind);
        if (idx >= slots.size() || slots[idx].ready <= 0) {
            orderNotice.show(ss2::trf("No {0} sorties available.", {ss2::tr(airKindLabel(kind))}),
                             ss2::NoticeKind::unsupported);
            return;
        }
        attackMoveArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        patrolArmed = false;
        faceArmed = false;
        armedAirSupport = kind;
        const bool strike = kind == ss2::assets::AirMissionKind::Bomber ||
                            kind == ss2::assets::AirMissionKind::Fighter;
        SDL_SetCursor(strike ? (attackCursor ? attackCursor.get() : SDL_GetDefaultCursor())
                             : (moveCursor ? moveCursor.get() : SDL_GetDefaultCursor()));
        orderNotice.show(ss2::trf("{0} ({1} ready): left-click target area.",
                                  {ss2::tr(airKindLabel(kind)), std::to_string(slots[idx].ready)}),
                         ss2::NoticeKind::accepted);
    };
    const auto uploadPlaneSprite = [&](LivePlane& lp) {
        if (const auto* pSpec = unitCatalog ? unitCatalog->findPlane(lp.pose.planeId) : nullptr;
            pSpec && pSpec->altitude > 0) {
            lp.shadowScale = std::clamp(static_cast<float>(pSpec->altitude) / 280.0f, 0.65f, 1.50f);
        }
        const std::string key = lp.pose.planeId + "#" + std::to_string(lp.pose.dir32 & 31u);
        auto it = planeSpriteCache.find(key);
        if (it == planeSpriteCache.end()) {
            std::optional<ss2::assets::MapImage> loaded;
            if (!gameDir.empty()) {
                loaded = ss2::assets::loadAircraftSprite(gameDir, lp.pose.planeId, lp.pose.dir32);
            }
            if (!loaded) {
                ss2::assets::MapImage fb{28, 28, std::vector<std::uint8_t>(28u * 28u * 4u, 0)};
                for (int py = 0; py < 28; ++py) {
                    for (int px = 0; px < 28; ++px) {
                        const bool body = std::abs(px - 14) <= 2 && std::abs(py - 14) <= 11;
                        const bool wing = std::abs(py - 12) <= 2 && std::abs(px - 14) <= 12;
                        if (body || wing) {
                            const std::size_t o = (std::size_t(py) * 28u + static_cast<std::size_t>(px)) * 4u;
                            fb.rgba[o] = 110;
                            fb.rgba[o + 1] = 120;
                            fb.rgba[o + 2] = 95;
                            fb.rgba[o + 3] = 255;
                        }
                    }
                }
                loaded = std::move(fb);
            }
            it = planeSpriteCache.emplace(key, std::move(*loaded)).first;
        }
        const auto& img = it->second;
        lp.spriteKey = key;
        lp.atlasSlot = planeAtlas.acquire(key);
        if (lp.atlasSlot == planeAtlas.overflowSlot() && !planeOverflowLogged) {
            planeOverflowLogged = true;
            std::cerr << "Plane atlas overflow: more than " << planeAtlas.liveCount()
                      << " distinct live plane sprites; extra aircraft share the overflow slot.\n";
        }
        renderer.setSpriteAtlas(lp.atlasSlot, img);
        lp.texW = static_cast<float>(img.width);
        lp.texH = static_cast<float>(img.height);
        lp.sprW = static_cast<float>(img.width * static_cast<unsigned>(mapScale));
        lp.sprH = static_cast<float>(img.height * static_cast<unsigned>(mapScale));
    };
    const auto buildPauseItems = [&] {
        std::vector<ss2::MenuItem> items;
        items.push_back({"resume", ss2::tr("Resume"), "", true});
        if (simulation) {
            int airReady = 0;
            for (const auto& s : simulation->playerAirSupport()) airReady += s.ready;
            if (airReady > 0) {
                items.push_back({"airsupport", ss2::tr("Air Support"), std::to_string(airReady), true});
            }
            items.push_back({"quicksave", ss2::tr("Quick Save"), "F9", true});
            items.push_back({"quickload", ss2::tr("Quick Load"), "F10", true});
            items.push_back({"saveload", ss2::tr("Save / Load..."), "", true});
            items.push_back({"forces", ss2::tr("Force Status"), "O", true});
            items.push_back({"briefing", ss2::tr("Mission Briefing"), "J", true});
        }
        if (!gameDir.empty() && nextMissionOut) {
            items.push_back({"campaigns", ss2::tr("Campaigns"), "", true});
            items.push_back({"missions", ss2::tr("Missions"), "", true});
        }
        items.push_back({"smooth", ss2::tr("Smooth terrain"), ss2::tr(smoothTerrain ? "On" : "Off"), true});
        if (audio) {
            items.push_back({"music", ss2::tr("Music"), ss2::tr(audio->musicPlaying() ? "On" : "Muted"), true});
            const int sfxPct = static_cast<int>(std::lround(audio->sfxVolume() * 100.0f));
            const std::string sfxLbl = sfxPct <= 0 ? ss2::tr("Muted") : (std::to_string(sfxPct) + "%");
            items.push_back({"sfx_volume", ss2::tr("SFX volume"), sfxLbl, true});
            const float mg = audio->musicVolume();
            const std::string musLbl = mg >= 0.34f ? "100%" : mg >= 0.23f ? "75%" : mg >= 0.13f ? "50%" : "25%";
            items.push_back({"music_volume", ss2::tr("Music volume"), musLbl, true});
            items.push_back({"next_track", ss2::tr("Next soundtrack track"), "N", true});
        }
        items.push_back({"effects", ss2::tr("Tracers and flashes"), ss2::tr(combatEffects ? "On" : "Off"), true});
        if (simulation) {
            const int sp = simulation->speedPercent();
            const std::string spStr = sp == 50 ? "0.5x" : sp == 200 ? "2x" : sp == 300 ? "3x" : "1x";
            items.push_back({"speed", ss2::tr("Game speed"), spStr, true});
        }
        {
            const char* diffKey = settings.difficulty == 1 ? "Easy" : settings.difficulty == 3 ? "Hard" : "Normal";
            items.push_back({"difficulty", ss2::tr("Difficulty"), ss2::tr(diffKey), true});
        }
        {
            // Each language is named in itself so it can be found whatever is showing.
            const auto current = ss2::language();
            items.push_back({"language", ss2::tr("Language"),
                             current == ss2::Language::french ? "Français" : current == ss2::Language::spanish ? "Español" : "English", true});
        }
        items.push_back({"controls", ss2::tr("Controls"), "", true});
        items.push_back({"log", ss2::tr("Message log"), "L", true});
        items.push_back({"quit", ss2::tr("Quit to desktop"), "", true});
        return items;
    };
    const auto openAirSupportMenu = [&] {
        if (!simulation) return;
        const auto slots = simulation->playerAirSupport();
        std::vector<ss2::MenuItem> items;
        items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
        for (std::size_t k = 0; k < slots.size(); ++k) {
            const auto kind = static_cast<ss2::assets::AirMissionKind>(k);
            const std::string tag = slots[k].planeId.empty()
                ? std::to_string(slots[k].ready)
                : (slots[k].planeId + " x" + std::to_string(slots[k].ready));
            items.push_back({"aircall:" + std::to_string(k), ss2::tr(airKindLabel(kind)), tag, slots[k].ready > 0});
        }
        menu.open(ss2::tr("Air Support"), std::move(items));
    };
    const auto openPauseMenu = [&] {
        if (briefing.active()) return;
        attackMoveArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        patrolArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        resetCursor();
        if (simulation && !paused) {
            paused = simulation->togglePause();
            menuPausedClock = true;
        } else {
            menuPausedClock = false;
        }
        menu.open(ss2::tr("Paused"), buildPauseItems());
    };
    const auto closeMenu = [&] {
        menu.close();
        if (simulation && menuPausedClock && paused) {
            paused = simulation->togglePause();
        }
        menuPausedClock = false;
    };
    const auto openMissionsMenu = [&] {
        if (gameDir.empty()) return;
        menuMissions = ss2::assets::listSinglePlayerMissions(gameDir);
        std::vector<ss2::MenuItem> items;
        items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
        for (std::size_t i = 0; i < menuMissions.size(); ++i) {
            const auto& m = menuMissions[i];
            std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) +
                              (m.scheme == 2 ? " Beach" : " Summer");
            const std::string mKey = ss2::normalizeMissionKey(m.path.stem().string());
            if (settings.completedMissions.contains(mKey)) {
                tag = ss2::tr("Completed") + " | " + tag;
            }
            if (const auto sit = settings.missionHighScores.find(mKey);
                sit != settings.missionHighScores.end() && sit->second > 0) {
                tag += " | " + std::to_string(sit->second);
            }
            items.push_back({"mission:" + std::to_string(i), m.name, tag, true});
        }
        menu.open(ss2::tr("Single-Player Missions"), std::move(items));
    };
    const auto openCampaignsMenu = [&] {
        if (gameDir.empty()) return;
        menuCampaigns = ss2::assets::listCampaigns(gameDir);
        std::vector<ss2::MenuItem> items;
        items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
        for (std::size_t i = 0; i < menuCampaigns.size(); ++i) {
            const auto& c = menuCampaigns[i];
            std::size_t done = 0;
            for (const auto& m : c.missions) {
                if (settings.completedMissions.contains(ss2::normalizeMissionKey(m.code)) ||
                    settings.completedMissions.contains(ss2::normalizeMissionKey(m.path.stem().string()))) {
                    ++done;
                }
            }
            items.push_back({"campaign:" + std::to_string(i), c.name,
                             std::to_string(done) + "/" + std::to_string(c.missions.size()), true});
        }
        if (menuCampaigns.empty()) {
            items.push_back({"", ss2::tr("No campaigns installed."), "", false});
        }
        menu.open(ss2::tr("Campaigns"), std::move(items));
    };
    const auto openCampaignMissionsMenu = [&](std::size_t campaignIdx) {
        if (campaignIdx >= menuCampaigns.size()) return;
        const auto& c = menuCampaigns[campaignIdx];
        std::vector<ss2::MenuItem> items;
        items.push_back({"back_campaigns", ss2::tr("Back to campaigns"), "", true});
        for (std::size_t i = 0; i < c.missions.size(); ++i) {
            const auto& m = c.missions[i];
            std::string tag = std::to_string(m.width) + "x" + std::to_string(m.height) +
                              (m.scheme == 2 ? " Beach" : " Summer");
            const std::string codeKey = ss2::normalizeMissionKey(m.code);
            const std::string stemKey = ss2::normalizeMissionKey(m.path.stem().string());
            if (settings.completedMissions.contains(codeKey) ||
                settings.completedMissions.contains(stemKey)) {
                tag = ss2::tr("Completed") + " | " + tag;
            }
            int bestScore = 0;
            if (const auto sit = settings.missionHighScores.find(codeKey); sit != settings.missionHighScores.end()) {
                bestScore = std::max(bestScore, sit->second);
            }
            if (const auto sit = settings.missionHighScores.find(stemKey); sit != settings.missionHighScores.end()) {
                bestScore = std::max(bestScore, sit->second);
            }
            if (bestScore > 0) {
                tag += " | " + std::to_string(bestScore);
            }
            items.push_back({"cmission:" + std::to_string(campaignIdx) + ":" + std::to_string(i), m.title, tag, true});
        }
        menu.open(c.name, std::move(items));
    };
    const auto openControlsMenu = [&] {
        menu.open(ss2::tr("Controls"), {
            {"back", ss2::tr("Back to pause menu"), "", true},
            {"", ss2::tr("Selection & Orders"), "", false},
            {"", ss2::tr("Select / box select"), ss2::tr("Left click / drag"), false},
            {"", ss2::tr("Select class on screen / all"), ss2::tr("Double-click / Ctrl+DblClick"), false},
            {"", ss2::tr("Select on-screen / all units"), "Ctrl+A / Ctrl+Shift+A", false},
            {"", ss2::tr("Move / board / hitch / garrison"), ss2::tr("Right click"), false},
            {"", ss2::tr("Escort / guard friendly unit"), ss2::tr("Right click friendly"), false},
            {"", ss2::tr("Attack-move"), "A", false},
            {"", ss2::tr("Patrol route"), "Q + click", false},
            {"", ss2::tr("Face direction (rotate front armor)"), "T + click", false},
            {"", ss2::tr("Reverse tank (front armor facing)"), "R", false},
            {"", ss2::tr("Attack ground (armor / artillery)"), "G", false},
            {"", ss2::tr("Call air support (cycle / select)"), "F / Alt+1..5", false},
            {"", ss2::tr("Lay AT mine (Sapper)"), "E", false},
            {"", ss2::tr("Disarm minefield (Sapper)"), ss2::tr("Right click mine"), false},
            {"", ss2::tr("Scatter / disperse infantry"), "D", false},
            {"", ss2::tr("Queue waypoints"), ss2::tr("Shift + Right click"), false},
            {"", ss2::tr("Order via minimap"), ss2::tr("Right-click / Armed click"), false},
            {"", ss2::tr("Unload / unhitch / evacuate"), "U", false},
            {"", ss2::tr("Abandon vehicle (bail out crew)"), "K", false},
            {"", ss2::tr("Game speed (- / +)"), "[ / ]", false},
            {"", ss2::tr("Quick Save / Quick Load"), "F9 / F10 (Ctrl+S / L)", false},
            {"", ss2::tr("Assign / recall / focus group"), "Ctrl+0..9 / 0..9 / 2x 0..9", false},
            {"", ss2::tr("Tactical Stance"), "", false},
            {"", ss2::tr("Stop selected units"), "S", false},
            {"", ss2::tr("Toggle hold fire"), "H", false},
            {"", ss2::tr("Toggle crouch stance"), "C", false},
            {"", ss2::tr("Officer binoculars"), "B", false},
            {"", ss2::tr("Camera & Interface"), "", false},
            {"", ss2::tr("Pan / Zoom"), ss2::tr("Edges / Arrows / Wheel"), false},
            {"", ss2::tr("Save / jump bookmark"), "Ctrl+F1..F8 / F1..F8", false},
            {"", ss2::tr("Jump to recent alert (cycle)"), "Space", false},
            {"", ss2::tr("Focus troops / marker"), "Tab / M", false},
            {"", ss2::tr("Cycle player vehicle"), "V", false},
            {"", ss2::tr("Cycle idle unit"), "I", false},
            {"", ss2::tr("Select all idle units"), "Z", false},
            {"", ss2::tr("Cycle wounded / low-ammo unit"), "W", false},
            {"", ss2::tr("Force status / army roster"), "O", false},
            {"", ss2::tr("Mission briefing / objectives"), "J", false},
            {"", ss2::tr("Center / Fullscreen"), "Home / F11", false},
            {"", ss2::tr("Pause / Message log"), "Ctrl+P / L", false},
            {"", ss2::tr("Mute music / Next track"), "Ctrl+M / N", false},
        });
    };
    const auto openDebriefMenu = [&](bool victory) {
        if (limit || timing || !nextMissionOut || menu.active() || briefing.active()) return;
        if (simulation && !paused) {
            paused = simulation->togglePause();
            menuPausedClock = true;
        } else {
            menuPausedClock = false;
        }
        std::vector<ss2::MenuItem> items;
        if (simulation) {
            const unsigned secs = simulation->simTick() / 25u;
            const unsigned mins = secs / 60u;
            const unsigned remS = secs % 60u;
            const std::string timeStr = (mins < 10u ? "0" : "") + std::to_string(mins) + ":" +
                                        (remS < 10u ? "0" : "") + std::to_string(remS);
            items.push_back({"", ss2::tr("Battle time"), timeStr, false});
        }
        items.push_back({"", ss2::tr("Enemy infantry eliminated"), std::to_string(enemyInfKilled), false});
        items.push_back({"", ss2::tr("Enemy vehicles destroyed"), std::to_string(enemyVehDestroyed), false});
        if (enemyTanksDestroyed > 0u || enemyGunsDestroyed > 0u) {
            items.push_back({"", ss2::tr("Enemy armor / guns"),
                             std::to_string(enemyTanksDestroyed) + " / " + std::to_string(enemyGunsDestroyed), false});
        }
        if (enemyPlanesShotDown > 0u) {
            items.push_back({"", ss2::tr("Enemy aircraft shot down"), std::to_string(enemyPlanesShotDown), false});
        }
        items.push_back({"", ss2::tr("Friendly casualties"),
                         std::to_string(selection ? selection->playerLosses() : 0), false});
        if (buildingsDemolished > 0u) {
            items.push_back({"", ss2::tr("Buildings demolished"), std::to_string(buildingsDemolished), false});
        }
        if (airSortiesFlown > 0u) {
            items.push_back({"", ss2::tr("Air sorties flown"), std::to_string(airSortiesFlown), false});
        }
        const int netBattleScore = std::max(0, enemyScoreEarned - friendlyScoreLost / 2);
        items.push_back({"", ss2::tr("Battle score"), std::to_string(netBattleScore), false});
        if (victory && unitCatalog) {
            const int stars = unitCatalog->starRatingForScore(netBattleScore);
            if (stars > 0) {
                items.push_back({"", ss2::tr("Mission rating"),
                                 std::string(static_cast<std::size_t>(stars), '*') + " (" +
                                     std::to_string(stars) + "/10)",
                                 false});
            }
        }
        const std::string completedKey = ss2::normalizeMissionKey(
            currentMissionPath.empty() ? std::string("TUTORIAL1") : currentMissionPath.stem().string());
        int prevBestScore = 0;
        if (!completedKey.empty()) {
            if (const auto sit = settings.missionHighScores.find(completedKey); sit != settings.missionHighScores.end()) {
                prevBestScore = std::max(0, sit->second);
            }
        }
        bool shouldPersist = false;
        if (victory && !completedKey.empty()) {
            if (settings.completedMissions.insert(completedKey).second) {
                shouldPersist = true;
            }
            if (netBattleScore > prevBestScore) {
                settings.missionHighScores[completedKey] = netBattleScore;
                shouldPersist = true;
            }
        }
        const int bestShown = std::max(prevBestScore, victory ? netBattleScore : 0);
        if (bestShown > 0) {
            const std::string bestStr = (victory && netBattleScore > prevBestScore)
                ? (std::to_string(bestShown) + " (" + ss2::tr("New Best!") + ")")
                : std::to_string(bestShown);
            items.push_back({"", ss2::tr("Best score"), bestStr, false});
        }
        if (shouldPersist) {
            persistNow();
        }
        if (victory && simulation && !gameDir.empty()) {
            if (const auto sig = simulation->nextMissionSignal(); sig && sig->first >= 1u && sig->first < 256u) {
                auto pad3 = [](std::uint32_t val) {
                    std::string str = std::to_string(val);
                    while (str.size() < 3u) str.insert(str.begin(), '0');
                    return str;
                };
                if (const auto m = ss2::assets::findCampaignMission(gameDir, pad3(sig->first) + pad3(sig->second))) {
                    nextCampaignMissionPath = m->path;
                    nextCampaignMissionTitle = m->title;
                }
            }
        }
        if (victory && nextCampaignMissionPath) {
            items.push_back({"next_campaign", ss2::tr("Next mission") + ": " + nextCampaignMissionTitle, "", true});
        }
        if (!currentMissionPath.empty()) {
            items.push_back({"replay", ss2::tr("Replay mission"), "", true});
        }
        items.push_back({"campaigns", ss2::tr("Campaigns"), "", true});
        items.push_back({"missions", ss2::tr("Missions"), "", true});
        items.push_back({"resume", ss2::tr("Continue"), "", true});
        items.push_back({"quit", ss2::tr("Quit to desktop"), "", true});
        menu.open(ss2::tr(victory ? "Mission Accomplished!" : "Mission Failed."), std::move(items));
    };
    const auto openLogMenu = [&] {
        if (briefing.active()) return;
        std::vector<ss2::MenuItem> items;
        items.push_back({"resume", ss2::tr("Close"), "", true});
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
        if (items.size() == 1) items.push_back({"", ss2::tr("No messages yet."), "", false});
        menu.open(ss2::tr("Message Log"), std::move(items));
    };
    const auto openForceOverviewMenu = [&](bool fromPauseMenu) {
        if (!simulation || !scene || briefing.active()) return;
        forceRosterGroups.clear();
        std::vector<ss2::MenuItem> items;
        items.push_back(fromPauseMenu
            ? ss2::MenuItem{"back", ss2::tr("Back to pause menu"), "", true}
            : ss2::MenuItem{"resume", ss2::tr("Close"), "", true});
        const unsigned secs = simulation->simTick() / 25u;
        const unsigned mins = secs / 60u;
        const unsigned remS = secs % 60u;
        const std::string timeStr = (mins < 10u ? "0" : "") + std::to_string(mins) + ":" +
                                    (remS < 10u ? "0" : "") + std::to_string(remS);
        items.push_back({"", ss2::tr("Battle time"), timeStr, false});

        struct ClassStats {
            std::vector<size_t> infIds;
            std::vector<size_t> vehIds;
            unsigned woundedCount{0};
            unsigned damagedCount{0};
        };
        std::map<std::string, ClassStats> byClass;
        unsigned activeInfField = 0;
        const auto& units = scene->units();
        for (std::size_t idx = 0; idx < units.size(); ++idx) {
            const auto& u = units[idx];
            if (u.owner != 0 || u.hp <= 0 || !u.visible) continue;
            ++activeInfField;
            auto& entry = byClass[hudDisplayName(u.id)];
            entry.infIds.push_back(idx);
            const int maxHp = (idx < initialUnitHp.size() && initialUnitHp[idx] > 0) ? initialUnitHp[idx] : 100;
            if (u.hp < maxHp) ++entry.woundedCount;
        }
        unsigned activeVeh = 0;
        unsigned damagedVeh = 0;
        if (vehicleScene) {
            const auto& vehs = vehicleScene->vehicles();
            for (std::size_t idx = 0; idx < vehs.size(); ++idx) {
                const auto& v = vehs[idx];
                if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.visible) continue;
                ++activeVeh;
                auto& entry = byClass[hudDisplayName(v.id)];
                entry.vehIds.push_back(idx);
                const int maxHp = (idx < initialVehicleHp.size() && initialVehicleHp[idx] > 0) ? initialVehicleHp[idx] : 100;
                if (v.hp < maxHp) {
                    ++damagedVeh;
                    ++entry.damagedCount;
                }
            }
        }
        unsigned garrisonedInf = 0;
        for (const auto& lb : liveBuildings) {
            if (!lb.destroyed && lb.owner == 0 && lb.occupantCount > 0) {
                garrisonedInf += static_cast<unsigned>(lb.occupantCount);
            }
        }
        items.push_back({"", ss2::tr("Active infantry"),
                         std::to_string(activeInfField) +
                             (garrisonedInf > 0 ? " (+" + std::to_string(garrisonedInf) + ")" : ""),
                         false});
        items.push_back({"", ss2::tr("Active vehicles"),
                         std::to_string(activeVeh) +
                             (damagedVeh > 0 ? " (" + std::to_string(damagedVeh) + " " + ss2::tr("damaged") + ")" : ""),
                         false});
        items.push_back({"", ss2::tr("Enemy losses"),
                         std::to_string(enemyInfKilled) + " / " + std::to_string(enemyVehDestroyed), false});
        items.push_back({"", ss2::tr("Friendly casualties"),
                         std::to_string(selection ? selection->playerLosses() : 0), false});
        items.push_back({"", ss2::tr("Battle score"),
                         std::to_string(std::max(0, enemyScoreEarned - friendlyScoreLost / 2)), false});
        int totalAirReady = 0;
        for (const auto& as : simulation->playerAirSupport()) {
            totalAirReady += std::max(0, as.ready);
        }
        if (totalAirReady > 0) {
            items.push_back({"airsupport", ss2::tr("Air support ready"), std::to_string(totalAirReady), true});
        }

        for (auto& [label, st] : byClass) {
            const std::size_t rowIdx = forceRosterGroups.size();
            const std::size_t count = st.infIds.size() + st.vehIds.size();
            std::string tag = "x" + std::to_string(count);
            if (st.damagedCount > 0) {
                tag += " (" + std::to_string(st.damagedCount) + " " + ss2::tr("damaged") + ")";
            } else if (st.woundedCount > 0) {
                tag += " (" + std::to_string(st.woundedCount) + " " + ss2::tr("wounded") + ")";
            }
            forceRosterGroups.push_back({label, std::move(st.infIds), std::move(st.vehIds)});
            items.push_back({"force_select:" + std::to_string(rowIdx), ss2::tr(label), tag, true});
        }
        for (std::size_t bi = 0; bi < liveBuildings.size(); ++bi) {
            const auto& lb = liveBuildings[bi];
            if (lb.destroyed || lb.owner != 0 || lb.occupantCount <= 0) continue;
            items.push_back({"force_bldg:" + std::to_string(bi), ss2::tr("Garrisoned building"),
                             std::to_string(lb.occupantCount) + "/" + std::to_string(lb.capacity) +
                                 " | HP " + std::to_string(lb.hp) + "/" + std::to_string(lb.maxHp),
                             true});
        }
        menu.open(ss2::tr("Force Status"), std::move(items));
    };
    const auto openMissionBriefing = [&] {
        std::string text = missionBriefingText;
        if (text.empty()) {
            text = currentObjective;
        } else if (!currentObjective.empty() && missionBriefingText != currentObjective) {
            text += "\n\n" + ss2::tr("Current objective") + ": " + currentObjective;
        }
        if (text.empty()) {
            orderNotice.show(ss2::tr("No mission briefing available."), ss2::NoticeKind::unsupported);
            return;
        }
        if (menu.active()) closeMenu();
        manualBriefingOpen = true;
        briefing.open(std::move(text));
    };
    const auto doTogglePause = [&] {
        if (!simulation) return;
        paused = simulation->togglePause();
        if (paused) showPause();
        else orderNotice.show(ss2::tr("Resumed."), ss2::NoticeKind::accepted);
        std::cout << (paused ? "Mission clock paused" : "Mission clock resumed") << '\n';
    };
    const auto doStop = [&] {
        if (!simulation || !selection) return;
        const auto& sel = selection->selected();
        const auto& vsel = selection->selectedVehicles();
        if (sel.empty() && vsel.empty()) return;
        size_t n = 0;
        if (!sel.empty()) {
            const std::vector<size_t> ids(sel.begin(), sel.end());
            n += simulation->stop(ids);
        }
        if (!vsel.empty()) {
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            n += simulation->stopVehicles(vids);
        }
        if (n) {
            playSelectionVoice("stop");
            attackMoveArmed = false;
            attackGroundArmed = false;
            reverseMoveArmed = false;
            patrolArmed = false;
            faceArmed = false;
            armedAirSupport.reset();
            resetCursor();
            showNotice(ss2::trf("Stop order ({0} soldiers).", {std::to_string(n)}), ss2::NoticeKind::accepted);
            std::cout << "Tutorial I tactical S (stop): " << n << " units\n";
        }
    };
    const auto doToggleHoldFire = [&] {
        if (!simulation || !selection) return;
        const auto& sel = selection->selected();
        const auto& vsel = selection->selectedVehicles();
        if (sel.empty() && vsel.empty()) {
            if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
                const auto& sb = liveBuildings[*selectedBuilding];
                if (!sb.destroyed && sb.owner == 0 && sb.occupantCount > 0) {
                    if (const auto hold = simulation->toggleBuildingHoldFire(*selectedBuilding)) {
                        for (const size_t sid : sb.occupantIds) {
                            if (*hold) holdFireUnits.insert(sid);
                            else holdFireUnits.erase(sid);
                        }
                        showNotice(ss2::tr(*hold ? "Garrison hold fire enabled."
                                                 : "Garrison fire at will enabled."),
                                   ss2::NoticeKind::accepted);
                    }
                }
            }
            return;
        }
        std::optional<bool> holdState;
        if (!sel.empty()) {
            const std::vector<size_t> ids(sel.begin(), sel.end());
            if (const auto hold = simulation->toggleHoldFire(ids)) {
                holdState = *hold;
                for (const auto id : ids) {
                    if (*hold) holdFireUnits.insert(id);
                    else holdFireUnits.erase(id);
                }
            }
        }
        if (!vsel.empty()) {
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            if (const auto vhold = simulation->toggleVehicleHoldFire(vids)) {
                if (!holdState) holdState = *vhold;
                for (const auto id : vids) {
                    if (*vhold) vehicleHoldFireUnits.insert(id);
                    else vehicleHoldFireUnits.erase(id);
                }
            }
        }
        if (holdState) {
            showNotice(ss2::tr(*holdState ? "Hold fire enabled." : "Fire at will enabled."), ss2::NoticeKind::accepted);
            std::cout << "Tutorial I tactical H (hold-fire=" << (*holdState ? "on" : "off") << "): "
                      << (sel.size() + vsel.size()) << " units\n";
        }
    };
    const auto doToggleCrouch = [&] {
        if (!simulation || !selection || selection->selected().empty()) return;
        const auto& sel = selection->selected();
        const std::vector<size_t> ids(sel.begin(), sel.end());
        if (const auto crouched = simulation->toggleCrouch(ids)) {
            for (const auto id : ids) {
                if (*crouched) crouchedUnits.insert(id);
                else {
                    crouchedUnits.erase(id);
                    camouflagedUnits.erase(id);
                }
            }
            syncGpuSprites();
            showNotice(ss2::tr(*crouched ? "Stance: Crouching (cover bonus, shorter sight)." : "Stance: Standing."),
                       ss2::NoticeKind::accepted);
            std::cout << "Tutorial I tactical C (stance=" << (*crouched ? "crouch" : "stand") << "): " << ids.size() << " units\n";
        }
    };
    const auto doToggleBinoculars = [&] {
        if (!simulation || !selection) return;
        if (selection->selected().empty()) {
            if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
                const auto& sb = liveBuildings[*selectedBuilding];
                if (!sb.destroyed && sb.owner == 0 && sb.occupantCount > 0) {
                    if (const auto active = simulation->toggleBuildingBinoculars(*selectedBuilding)) {
                        for (const size_t sid : sb.occupantIds) {
                            const bool hasBino = scene && sid < scene->units().size() &&
                                                 soldierHasBinoculars(scene->units()[sid].id);
                            if (hasBino) {
                                if (*active) binocularUnits.insert(sid);
                                else binocularUnits.erase(sid);
                            }
                        }
                        showNotice(ss2::tr(*active ? "Garrison binoculars active (elevated vision)."
                                                   : "Garrison binoculars stowed."),
                                   ss2::NoticeKind::accepted);
                    } else {
                        orderNotice.show(ss2::tr("No officer or sniper garrisoned in building."),
                                         ss2::NoticeKind::unsupported);
                    }
                }
            }
            return;
        }
        const auto& sel = selection->selected();
        const std::vector<size_t> ids(sel.begin(), sel.end());
        if (const auto active = simulation->toggleBinoculars(ids)) {
            for (const auto id : ids) {
                if (scene && id < scene->units().size() && !soldierHasBinoculars(scene->units()[id].id)) continue;
                if (*active) binocularUnits.insert(id);
                else binocularUnits.erase(id);
            }
            showNotice(ss2::tr(*active ? "Binoculars active (extended vision)." : "Binoculars stowed."),
                       ss2::NoticeKind::accepted);
            std::cout << "Mission tactical B (binoculars=" << (*active ? "on" : "off") << "): " << ids.size() << " units\n";
        }
    };
    const auto doArmAttackMove = [&] {
        if (!simulation || !selection || (selection->selected().empty() && selection->selectedVehicles().empty())) return;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        patrolArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        attackMoveArmed = !attackMoveArmed;
        if (attackMoveArmed) {
            SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
            orderNotice.show(ss2::tr("Attack-move: click target location."), ss2::NoticeKind::accepted);
        } else {
            resetCursor();
        }
    };
    const auto doArmPatrol = [&] {
        if (!simulation || !selection || (selection->selected().empty() && selection->selectedVehicles().empty())) return;
        attackMoveArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        patrolArmed = !patrolArmed;
        if (patrolArmed) {
            SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
            orderNotice.show(ss2::tr("Patrol: click destination on map (Esc/right-click to cancel)."),
                             ss2::NoticeKind::accepted);
        } else {
            resetCursor();
        }
    };
    const auto doArmAttackGround = [&] {
        if (!simulation || !selection) return;
        const bool hasBombardInf = scene && std::ranges::any_of(selection->selected(), [&](size_t id) {
            return id < scene->units().size() && isBombardInfantryId(scene->units()[id].id);
        });
        if (selection->selectedVehicles().empty() && !hasBombardInf) return;
        attackMoveArmed = false;
        reverseMoveArmed = false;
        patrolArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        attackGroundArmed = !attackGroundArmed;
        if (attackGroundArmed) {
            SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
            orderNotice.show(ss2::tr("Attack-ground: click coordinates to bombard."), ss2::NoticeKind::accepted);
        } else {
            resetCursor();
        }
    };
    const auto buildingAtMap = [&](int simX, int simY) -> std::optional<std::size_t> {
        std::optional<std::size_t> best;
        int bestY = -100000;
        for (std::size_t i = 0; i < liveBuildings.size(); ++i) {
            const auto& b = liveBuildings[i];
            if (b.destroyed) continue;
            if (simX >= b.left && simX < b.left + b.width && simY >= b.top && simY < b.top + b.height) {
                if (b.y >= bestY) {
                    bestY = b.y;
                    best = i;
                }
            }
        }
        return best;
    };
    const auto doUnloadOrUnhitch = [&] {
        if (!simulation || !selection) return;
        bool handledU = false;
        if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
            const auto& lb = liveBuildings[*selectedBuilding];
            if (!lb.destroyed && lb.owner == 0 && lb.occupantCount > 0) {
                const size_t evac = simulation->evacuateBuilding(*selectedBuilding);
                if (evac > 0) {
                    selectedBuilding.reset();
                    playSelectionVoice("move");
                    showNotice(ss2::trf("Evacuated {0} soldiers from building.", {std::to_string(evac)}),
                               ss2::NoticeKind::accepted);
                    handledU = true;
                }
            }
        }
        if (!handledU && !selection->selectedVehicles().empty()) {
            const auto& vsel = selection->selectedVehicles();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            const size_t unhitched = simulation->unhitchVehicles(vids);
            if (unhitched > 0) {
                playSelectionVoice("move");
                showNotice(ss2::trf("Unhitched {0} field guns.", {std::to_string(unhitched)}),
                           ss2::NoticeKind::accepted);
                handledU = true;
            } else {
                const size_t unloaded = simulation->unloadVehicles(vids);
                if (unloaded > 0) {
                    playSelectionVoice("move");
                    showNotice(ss2::trf("Unloaded {0} soldiers.", {std::to_string(unloaded)}), ss2::NoticeKind::accepted);
                    handledU = true;
                }
            }
        }
        if (!handledU && !liveBuildings.empty()) {
            float mx{}, my{};
            SDL_GetMouseState(&mx, &my);
            const auto [mapX, mapY] = renderer.mapPosition(mx, my);
            const auto hoveredBld = buildingAtMap(int(std::floor(mapX / mapScale)), int(std::floor(mapY / mapScale)));
            if (hoveredBld && liveBuildings[*hoveredBld].owner == 0 && liveBuildings[*hoveredBld].occupantCount > 0) {
                const size_t evac = simulation->evacuateBuilding(*hoveredBld);
                if (evac > 0) {
                    if (selectedBuilding && *selectedBuilding == *hoveredBld) selectedBuilding.reset();
                    playSelectionVoice("move");
                    showNotice(ss2::trf("Evacuated {0} soldiers from building.", {std::to_string(evac)}),
                               ss2::NoticeKind::accepted);
                    handledU = true;
                }
            }
        }
        if (!handledU && !selection->selectedVehicles().empty()) {
            orderNotice.show(ss2::tr("No passengers or towed guns to unload."), ss2::NoticeKind::unsupported);
        }
    };
    const auto doLayMine = [&] {
        if (!simulation || !selection || selection->selected().empty()) return;
        const auto& sel = selection->selected();
        const std::vector<size_t> ids(sel.begin(), sel.end());
        const size_t laid = simulation->layMine(ids);
        if (laid > 0) {
            liveVisibleMines = simulation->visibleMines();
            playSelectionVoice("laymine");
        } else {
            orderNotice.show(ss2::tr("Select a Sapper with AT mines to lay mines here."), ss2::NoticeKind::unsupported);
        }
    };
    const auto doScatter = [&] {
        if (!simulation || !selection || selection->selected().empty()) return;
        const auto& sel = selection->selected();
        const std::vector<size_t> ids(sel.begin(), sel.end());
        const size_t n = simulation->scatter(ids);
        if (n > 0) {
            attackMoveArmed = false;
            patrolArmed = false;
            attackGroundArmed = false;
            reverseMoveArmed = false;
            faceArmed = false;
            armedAirSupport.reset();
            resetCursor();
            playSelectionVoice("move");
            showNotice(ss2::trf("Scattering {0} infantry.", {std::to_string(n)}), ss2::NoticeKind::accepted);
        } else {
            orderNotice.show(ss2::tr("Unable to scatter on this terrain."), ss2::NoticeKind::unsupported);
        }
    };
    const auto doAbandonVehicles = [&] {
        if (!simulation || !selection || !scene || !vehicleScene) return;
        const auto& vsel = selection->selectedVehicles();
        if (vsel.empty()) return;
        const std::vector<size_t> vids(vsel.begin(), vsel.end());
        const auto spawns = simulation->abandonVehicles(vids);
        if (spawns.empty()) {
            orderNotice.show(ss2::tr("Selected vehicles have no crew to abandon."), ss2::NoticeKind::unsupported);
            return;
        }
        for (const auto& vid : vids) {
            if (vid < vehicleScene->vehicles().size()) {
                vehicleScene->setOwner(vid, 3);
                selection->updateVehicle(vid, scaleVehicle(vehicleScene->vehicles().at(vid), mapScale));
            }
        }
        std::size_t crewCount = 0;
        for (const auto& rawCp : spawns) {
            auto cp = rawCp;
            (void)ensureGpuInfantrySlot(cp.id);
            auto spawned = scene->spawn(cp);
            if (!spawned) {
                for (const auto& existing : scene->units()) {
                    if (existing.owner == 0) {
                        cp.id = existing.id;
                        (void)ensureGpuInfantrySlot(cp.id);
                        spawned = scene->spawn(cp);
                        if (spawned) break;
                    }
                }
            }
            if (spawned) {
                const auto& [newIdx, patch] = *spawned;
                const auto& unit = scene->units().at(newIdx);
                ensureUnitPortrait(unit.id, false);
                (void)simulation->spawnInfantry(unit, 0);
                const int realAmmo = scaledSoldierAmmo(unit.id, unit.ammo);
                (void)scene->update(newIdx, unit.x, unit.y, unit.direction, unit.progress, unit.walking, unit.hp,
                                    -1, realAmmo);
                selection->addUnit(scaleInfantry(scene->units().at(newIdx), mapScale));
                initialUnitHp.push_back(std::max(1, unit.hp));
                unitExpa.push_back(0);
                unitKills.push_back(0);
                unitGrenades.push_back(initialGrenadesForId(unit.id));
                unitMineAmmo.push_back(initialMinesForId(unit.id));
                if (patch.pixels.width && patch.pixels.height) {
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                }
                ++crewCount;
            }
        }
        syncGpuSprites();
        playSelectionVoice("move");
        showNotice(ss2::trf("Crew abandoned {0} vehicle(s).", {std::to_string(crewCount)}), ss2::NoticeKind::accepted);
    };
    unsigned nextAutoSaveTick = 7500u;
    const auto quickSavePath = [&](int slot) -> std::filesystem::path {
        std::filesystem::path base;
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
            base = std::filesystem::path(xdg);
        } else if (const char* home = std::getenv("HOME"); home && *home) {
            base = std::filesystem::path(home) / ".local" / "share";
        } else {
            base = std::filesystem::temp_directory_path();
        }
        const std::string stem = currentMissionPath.empty() ? std::string("tutorial1")
                                                            : currentMissionPath.stem().string();
        const std::string fname = (slot < 0)
            ? (stem + "_autosave.ss2save")
            : (slot == 0 ? (stem + ".ss2save") : (stem + "_slot" + std::to_string(slot) + ".ss2save"));
        return base / "ss2" / "saves" / fname;
    };
    const auto doSaveSlot = [&](int slot) {
        if (!simulation) return;
        const auto path = quickSavePath(slot);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            orderNotice.show(ss2::tr("Could not create save directory."), ss2::NoticeKind::blocked);
            return;
        }
        const auto bytes = ss2::TutorialUnits::serializeSnapshot(simulation->takeSnapshot());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            orderNotice.show(ss2::tr("Could not write quick-save file."), ss2::NoticeKind::blocked);
            return;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            orderNotice.show(ss2::tr("Quick-save write failed."), ss2::NoticeKind::blocked);
            return;
        }
        if (slot < 0) {
            orderNotice.show(ss2::tr("Auto-saved mission progress."), ss2::NoticeKind::accepted);
            return;
        }
        if (audio) audio->playGroup("accept", 0.45f);
        if (slot == 0) {
            showNotice(ss2::tr("Quick-saved mission state (F9)."), ss2::NoticeKind::accepted);
        } else {
            showNotice(ss2::trf("Saved mission state to Slot {0}.", {std::to_string(slot)}), ss2::NoticeKind::accepted);
        }
    };
    const auto doLoadSlot = [&](int slot) {
        if (!simulation || !scene || !selection) return;
        const auto path = quickSavePath(slot);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            orderNotice.show(ss2::tr("No quick-save found for this mission."), ss2::NoticeKind::unsupported);
            return;
        }
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            orderNotice.show(ss2::tr("Could not read quick-save file."), ss2::NoticeKind::blocked);
            return;
        }
        const auto endPos = in.tellg();
        if (endPos <= 0) {
            orderNotice.show(ss2::tr("Quick-save file is empty or corrupt."), ss2::NoticeKind::blocked);
            return;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(endPos));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            orderNotice.show(ss2::tr("Could not read quick-save file."), ss2::NoticeKind::blocked);
            return;
        }
        const auto snap = ss2::TutorialUnits::deserializeSnapshot(bytes);
        if (!snap || !simulation->loadSnapshot(*snap)) {
            orderNotice.show(ss2::tr("Quick-save data is incompatible with this mission."), ss2::NoticeKind::blocked);
            return;
        }

        fallenSoldiers.clear();
        recoilUntil.clear();
        woundedBarLogged.clear();
        activeVfx.clear();
        burningWrecks.clear();
        livePlanes.clear();
        planeAtlas.reset();
        planeOverflowLogged = false;
        activeMarkers.clear();
        eventHistory.clear();
        eventHistoryCursor = 0;
        syncMarkers();
        selectedBuilding.reset();
        enemyInfKilled = 0;
        enemyVehDestroyed = 0;
        buildingsDemolished = 0;
        airSortiesFlown = 0;
        enemyScoreEarned = 0;
        friendlyScoreLost = 0;
        enemyTanksDestroyed = 0;
        enemyGunsDestroyed = 0;
        enemyPlanesShotDown = 0;
        vehicleCrewImpaired.clear();
        vehicleMoveImpaired.clear();
        vehicleSuppressed.clear();
        vehicleHatchOpen.clear();
        movingSmokeVehicles.clear();
        attackMoveArmed = false;
        attackGroundArmed = false;
        reverseMoveArmed = false;
        patrolArmed = false;
        faceArmed = false;
        armedAirSupport.reset();
        resetCursor();
        crouchedUnits.clear();
        holdFireUnits.clear();
        binocularUnits.clear();
        vehicleHoldFireUnits.clear();
        camouflagedUnits.clear();
        suppressedUnits.clear();
        missionPlayerTeams = snap->playerTeams;
        for (std::size_t i = 0; i < snap->soldiers.size(); ++i) {
            const auto& ss = snap->soldiers[i];
            if (ss.crouched) crouchedUnits.insert(i);
            if (ss.holdFire) holdFireUnits.insert(i);
            if (ss.binoculars) binocularUnits.insert(i);
        }
        for (std::size_t i = 0; i < snap->vehicles.size(); ++i) {
            if (snap->vehicles[i].holdFire) vehicleHoldFireUnits.insert(i);
        }

        if (auto fogSnap = simulation->pollFog()) {
            latestFog = *fogSnap;
            const auto fogPatch = scene->updateFog(*fogSnap);
            if (fogPatch.pixels.width && fogPatch.pixels.height)
                renderer.updateMapRegion(fogPatch.x, fogPatch.y, fogPatch.pixels);
            updateMinimapFog(*fogSnap, true);
        }
        for (const auto& pose : simulation->pollPoses()) {
            if (pose.index < scene->units().size()) {
                const auto patch = scene->update(pose.index, pose.x, pose.y, pose.direction,
                                                 pose.progress, pose.walking, pose.hp,
                                                 pose.visible ? 1 : 0, pose.ammo);
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                selection->update(pose.index, scaleInfantry(scene->units().at(pose.index), mapScale));
            }
            if (pose.index >= unitExpa.size()) unitExpa.resize(pose.index + 1, 0);
            if (pose.index >= unitKills.size()) unitKills.resize(pose.index + 1, 0);
            if (pose.index >= unitGrenades.size()) unitGrenades.resize(pose.index + 1, 0);
            if (pose.index >= unitMineAmmo.size()) unitMineAmmo.resize(pose.index + 1, 0);
            unitExpa[pose.index] = pose.expa;
            unitKills[pose.index] = pose.kills;
            unitGrenades[pose.index] = pose.atGrenades;
            unitMineAmmo[pose.index] = pose.mineAmmo;
            if (pose.camouflaged) camouflagedUnits.insert(pose.index);
            if (pose.alive && pose.suppressed) suppressedUnits.insert(pose.index);
        }
        if (vehicleScene) {
            for (const auto& vp : simulation->pollVehiclePoses()) {
                if (vp.index < vehicleScene->vehicles().size()) {
                    vehicleScene->setOwner(vp.index, vp.owner);
                    const auto dirty = vehicleScene->update(vp.index, vp.x, vp.y, vp.hullDir, vp.turretDir,
                                                            vp.hp, vp.visible ? 1 : 0, vp.ammo);
                    if (mapScale == 1 && dirty.valid()) {
                        const auto patch = scene->repaintRegion(dirty.left, dirty.top, dirty.right, dirty.bottom);
                        if (patch.pixels.width && patch.pixels.height)
                            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                    }
                    selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
                }
                if (vp.maxHp > 0 && vp.index < initialVehicleHp.size()) initialVehicleHp[vp.index] = vp.maxHp;
                if (vp.index < vehicleCargo.size()) vehicleCargo[vp.index] = {vp.passengerCount, vp.passengerCapacity};
                if (vp.index < vehicleTowed.size()) vehicleTowed[vp.index] = vp.towedIndex;
                if (vp.index < vehicleTowedBy.size()) vehicleTowedBy[vp.index] = vp.towedByIndex;
                if (vp.index >= vehicleAmmo2.size()) vehicleAmmo2.resize(vp.index + 1, 0);
                vehicleAmmo2[vp.index] = vp.ammo2;
                if (vp.index >= vehExpa.size()) vehExpa.resize(vp.index + 1, 0);
                if (vp.index >= vehKills.size()) vehKills.resize(vp.index + 1, 0);
                if (vp.index >= vehicleCrewImpaired.size()) vehicleCrewImpaired.resize(vp.index + 1, false);
                if (vp.index >= vehicleMoveImpaired.size()) vehicleMoveImpaired.resize(vp.index + 1, false);
                if (vp.index >= vehicleSuppressed.size()) vehicleSuppressed.resize(vp.index + 1, false);
                if (vp.index >= vehicleHatchOpen.size()) vehicleHatchOpen.resize(vp.index + 1, false);
                vehExpa[vp.index] = vp.expa;
                vehKills[vp.index] = vp.kills;
                vehicleCrewImpaired[vp.index] = vp.crewImpaired;
                vehicleMoveImpaired[vp.index] = vp.moveImpaired;
                vehicleSuppressed[vp.index] = vp.suppressed;
                vehicleHatchOpen[vp.index] = vp.hatchOpen;
            }
        }
        for (const auto& bu : simulation->pollBuildingUpdates()) {
            if (bu.index < liveBuildings.size()) {
                auto& lb = liveBuildings[bu.index];
                lb.hp = bu.hp;
                lb.maxHp = bu.maxHp;
                lb.occupantCount = bu.occupantCount;
                lb.occupantIds = bu.occupantIds;
                lb.owner = bu.owner;
                lb.destroyed = bu.destroyed;
                if (lb.destroyed) applyBuildingRubble(bu.index);
                else restoreBuildingIntact(bu.index);
            }
        }
        (void)simulation->pollMineEvents();
        liveVisibleMines = simulation->visibleMines();
        (void)simulation->pollAirSorties();
        (void)simulation->pollAirStrikeEvents();
        (void)simulation->poll();
        syncGpuSprites();
        nextAutoSaveTick = simulation->simTick() + 7500u;
        if (audio) audio->playGroup("accept", 0.45f);
        if (slot < 0) {
            showNotice(ss2::tr("Loaded Auto-Save."), ss2::NoticeKind::accepted);
        } else if (slot == 0) {
            showNotice(ss2::tr("Quick-loaded mission state (F10)."), ss2::NoticeKind::accepted);
        } else {
            showNotice(ss2::trf("Loaded mission state from Slot {0}.", {std::to_string(slot)}), ss2::NoticeKind::accepted);
        }
    };
    const auto doQuickSave = [&] { doSaveSlot(0); };
    const auto doQuickLoad = [&] { doLoadSlot(0); };
    const auto openSaveLoadMenu = [&] {
        if (!simulation) return;
        std::vector<ss2::MenuItem> items;
        items.push_back({"back", ss2::tr("Back to pause menu"), "", true});
        items.push_back({"", ss2::tr("Save Game"), "", false});
        for (int s = 0; s <= 3; ++s) {
            std::error_code ec;
            const auto p = quickSavePath(s);
            const bool exists = std::filesystem::is_regular_file(p, ec);
            const auto fsz = exists ? std::filesystem::file_size(p, ec) : 0u;
            const std::string tag = exists ? (std::to_string(std::max<std::uintmax_t>(1u, fsz / 1024u)) + " KB")
                                           : ss2::tr("Empty");
            const std::string lbl = (s == 0) ? ss2::tr("Quick Save (F9)")
                                             : ss2::trf("Save Slot {0}", {std::to_string(s)});
            items.push_back({"save:" + std::to_string(s), lbl, tag, true});
        }
        items.push_back({"", ss2::tr("Load Game"), "", false});
        {
            std::error_code ec;
            const auto p = quickSavePath(-1);
            const bool exists = std::filesystem::is_regular_file(p, ec);
            const auto fsz = exists ? std::filesystem::file_size(p, ec) : 0u;
            const std::string tag = exists ? (std::to_string(std::max<std::uintmax_t>(1u, fsz / 1024u)) + " KB")
                                           : ss2::tr("Empty");
            items.push_back({"load:-1", ss2::tr("Load Auto-Save"), tag, exists});
        }
        for (int s = 0; s <= 3; ++s) {
            std::error_code ec;
            const auto p = quickSavePath(s);
            const bool exists = std::filesystem::is_regular_file(p, ec);
            const auto fsz = exists ? std::filesystem::file_size(p, ec) : 0u;
            const std::string tag = exists ? (std::to_string(std::max<std::uintmax_t>(1u, fsz / 1024u)) + " KB")
                                           : ss2::tr("Empty");
            const std::string lbl = (s == 0) ? ss2::tr("Quick Load (F10)")
                                             : ss2::trf("Load Slot {0}", {std::to_string(s)});
            items.push_back({"load:" + std::to_string(s), lbl, tag, exists});
        }
        menu.open(ss2::tr("Save / Load"), std::move(items));
    };
    const auto spawnAndRegisterVehicle = [&](const ss2::assets::Placement& p)
        -> std::optional<std::pair<size_t, size_t>> {
        if (!vehicleScene || !simulation || !selection) return std::nullopt;
        const auto vIdx = vehicleScene->spawn(p);
        if (!vIdx) return std::nullopt;
        const auto& v = vehicleScene->vehicles().at(*vIdx);
        ensureUnitPortrait(v.id, true);
        const size_t simVIdx = simulation->spawnVehicle(v);
        int maxVehHp = std::max(1, v.hp);
        int realHp = v.hp;
        int realAmmo = v.ammo;
        int cap = 0;
        int a2 = 0;
        bool hatchOpen = false;
        if (unitCatalog) {
            if (const auto* spec = unitCatalog->find(v.id)) {
                if (spec->health > 0) {
                    maxVehHp = spec->health;
                    if (v.alive && v.hp > 0) {
                        realHp = std::max(1, (maxVehHp * std::clamp(v.hp, 1, 100)) / 100);
                    }
                }
                cap = std::clamp(spec->stock, 0, 44);
                hatchOpen = spec->marchEnabled || (spec->marchSightBonus > 0);
                realAmmo = spec->ammo1 > 0
                    ? std::max(0, (spec->ammo1 * std::clamp(v.ammo, 0, 100)) / 100)
                    : ((spec->shot1.damage > 0 && spec->shot1.range > 0) ? std::max(1, v.ammo) : v.ammo);
                a2 = spec->ammo2 > 0
                    ? std::max(0, (spec->ammo2 * std::clamp(v.ammo, 0, 100)) / 100)
                    : ((spec->shot2.damage > 0 && spec->shot2.range > 0) ? std::max(1, v.ammo) : 0);
            }
        }
        vehicleScene->update(*vIdx, v.x, v.y, v.hullDir, v.turretDir,
                             realHp, v.visible ? 1 : 0, realAmmo);
        selection->addVehicle(scaleVehicle(vehicleScene->vehicles().at(*vIdx), mapScale));
        initialVehicleHp.push_back(maxVehHp);
        vehExpa.push_back(ss2::TutorialUnits::missionExpaToSim(v.expa));
        vehKills.push_back(0);
        vehicleAmmo2.push_back(a2);
        vehicleCargo.emplace_back(0, cap);
        vehicleTowed.push_back(-1);
        vehicleTowedBy.push_back(-1);
        vehicleCrewImpaired.push_back(false);
        vehicleMoveImpaired.push_back(false);
        vehicleSuppressed.push_back(false);
        vehicleHatchOpen.push_back(hatchOpen);
        if (mapScale == 1 && scene) {
            const int l = v.width > 0 ? v.left - 4 : v.x - 24;
            const int t = v.height > 0 ? v.top - 4 : v.y - 24;
            const int r = v.width > 0 ? v.left + v.width + 4 : v.x + 24;
            const int b = v.height > 0 ? v.top + v.height + 4 : v.y + 24;
            const auto patch = scene->repaintRegion(l, t, r, b);
            if (patch.pixels.width && patch.pixels.height) {
                renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
            }
        }
        return std::pair{*vIdx, simVIdx};
    };
    const auto spawnReinforcementWave = [&](const ss2::MissionEvent& ev) {
        if (!simulation || !scene || !selection) return;
        const auto& data = simulation->data();
        if (ev.index >= data.reinforcements.size()) return;
        const auto& wave = data.reinforcements[ev.index];
        if (wave.empty()) return;
        int gateX = 0, gateY = 0;
        if (ev.gate < data.gates.size() && (data.gates[ev.gate].first != 0 || data.gates[ev.gate].second != 0)) {
            gateX = data.gates[ev.gate].first;
            gateY = data.gates[ev.gate].second;
        } else if (ev.gate < data.markers.size() && (data.markers[ev.gate].first != 0 || data.markers[ev.gate].second != 0)) {
            gateX = data.markers[ev.gate].first;
            gateY = data.markers[ev.gate].second;
        } else if (ev.destZone < data.locations.size() &&
                   (data.locations[ev.destZone].first != 0 || data.locations[ev.destZone].second != 0)) {
            gateX = data.locations[ev.destZone].first;
            gateY = data.locations[ev.destZone].second;
        } else {
            gateX = static_cast<int>(hudMapW / 2);
            gateY = static_cast<int>(hudMapH / 2);
        }
        bool hadPlayerInfantry = false;
        for (const auto& u : scene->units()) {
            if (u.owner == 0 && u.hp > 0) { hadPlayerInfantry = true; break; }
        }
        std::size_t spawnedInfantry = 0, spawnedVehicles = 0;
        std::vector<size_t> spawnedInfIndices, spawnedVehIndices;
        int firstX = gateX, firstY = gateY;
        const int maxMapX = std::max(32, static_cast<int>(hudMapW) - 16);
        const int maxMapY = std::max(32, static_cast<int>(hudMapH) - 16);
        for (std::size_t i = 0; i < wave.size(); ++i) {
            const auto& ru = wave[i];
            const int col = static_cast<int>(i % 6);
            const int row = static_cast<int>(i / 6);
            const int sx = std::clamp(gateX + (col - 2) * 20, 16, maxMapX);
            const int sy = std::clamp(gateY + row * 18, 16, maxMapY);
            ss2::assets::Placement p{ru.id, sx, sy, 0, ru.hp, ru.ammo, ru.group, static_cast<int>(ev.owner), ru.passengers, ru.expa};
            (void)ensureGpuInfantrySlot(ru.id);
            if (auto spawned = scene->spawn(p)) {
                const auto& [newIdx, patch] = *spawned;
                const auto& unit = scene->units().at(newIdx);
                ensureUnitPortrait(unit.id, false);
                const int simExpa = ss2::TutorialUnits::missionExpaToSim(ru.expa);
                const size_t simIdx = simulation->spawnInfantry(unit, simExpa);
                const int realAmmo = scaledSoldierAmmo(unit.id, ru.ammo);
                (void)scene->update(newIdx, unit.x, unit.y, unit.direction, unit.progress, unit.walking, unit.hp,
                                    -1, realAmmo);
                selection->addUnit(scaleInfantry(scene->units().at(newIdx), mapScale));
                initialUnitHp.push_back(std::max(1, unit.hp));
                unitExpa.push_back(simExpa);
                unitKills.push_back(0);
                unitGrenades.push_back(initialGrenadesForId(unit.id));
                unitMineAmmo.push_back(initialMinesForId(unit.id));
                spawnedInfIndices.push_back(simIdx);
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                if (spawnedInfantry + spawnedVehicles == 0) { firstX = unit.x; firstY = unit.y; }
                ++spawnedInfantry;
            } else if (auto spawnedVeh = spawnAndRegisterVehicle(p)) {
                const auto [vIdx, simVIdx] = *spawnedVeh;
                const auto& v = vehicleScene->vehicles().at(vIdx);
                spawnedVehIndices.push_back(simVIdx);
                for (const auto& pax : ru.passengers) {
                    if (pax.isCrew) continue;
                    ss2::assets::Placement rp{pax.id, v.x, v.y, 0, pax.hp, pax.ammo, pax.group, static_cast<int>(ev.owner), {}, pax.expa};
                    (void)ensureGpuInfantrySlot(pax.id);
                    if (auto spawnedRider = scene->spawn(rp)) {
                        const auto rIdx = spawnedRider->first;
                        const auto& rUnit = scene->units().at(rIdx);
                        ensureUnitPortrait(rUnit.id, false);
                        const int simRiderExpa = ss2::TutorialUnits::missionExpaToSim(pax.expa);
                        const size_t simRIdx = simulation->spawnInfantry(rUnit, simRiderExpa);
                        const int realRiderAmmo = scaledSoldierAmmo(rUnit.id, pax.ammo);
                        if (const auto rPatch = scene->update(rIdx, rUnit.x, rUnit.y, rUnit.direction,
                                                              rUnit.progress, rUnit.walking, rUnit.hp, 0, realRiderAmmo);
                            rPatch.pixels.width && rPatch.pixels.height) {
                            renderer.updateMapRegion(rPatch.x, rPatch.y, rPatch.pixels);
                        }
                        auto scaledRider = scaleInfantry(scene->units().at(rIdx), mapScale);
                        scaledRider.visible = false;
                        selection->addUnit(std::move(scaledRider));
                        initialUnitHp.push_back(std::max(1, rUnit.hp));
                        unitExpa.push_back(simRiderExpa);
                        unitKills.push_back(0);
                        unitGrenades.push_back(initialGrenadesForId(rUnit.id));
                        unitMineAmmo.push_back(initialMinesForId(rUnit.id));
                        if (simulation->embarkImmediate(simRIdx, simVIdx) && simVIdx < vehicleCargo.size()) {
                            ++vehicleCargo[simVIdx].first;
                        }
                        ++spawnedInfantry;
                    }
                }
                if (spawnedInfantry + spawnedVehicles == 0) { firstX = v.x; firstY = v.y; }
                ++spawnedVehicles;
            }
        }
        const std::size_t totalSpawned = spawnedInfantry + spawnedVehicles;
        if (totalSpawned > 0) {
            if (ev.owner == 0 && ev.gate < data.gateTargets.size() &&
                (data.gateTargets[ev.gate].first != 0 || data.gateTargets[ev.gate].second != 0)) {
                const auto [rx, ry] = data.gateTargets[ev.gate];
                if (!spawnedInfIndices.empty()) (void)simulation->move(spawnedInfIndices, rx, ry);
                if (!spawnedVehIndices.empty()) (void)simulation->moveVehicles(spawnedVehIndices, rx, ry);
            } else if (ev.owner != 0) {
                std::optional<std::pair<int, int>> rally;
                if (ev.destZone > 0 && ev.destZone < data.locations.size() &&
                    (data.locations[ev.destZone].first != 0 || data.locations[ev.destZone].second != 0)) {
                    rally = data.locations[ev.destZone];
                } else if (ev.gate < data.gateTargets.size() &&
                           (data.gateTargets[ev.gate].first != 0 || data.gateTargets[ev.gate].second != 0)) {
                    rally = data.gateTargets[ev.gate];
                }
                if (rally) {
                    std::set<int> waveGroups;
                    for (const auto& ru : wave) {
                        if (ru.group > 0) waveGroups.insert(ru.group);
                        for (const auto& pax : ru.passengers) {
                            if (pax.group > 0) waveGroups.insert(pax.group);
                        }
                    }
                    for (int grp : waveGroups) {
                        (void)simulation->orderGroupMove(static_cast<unsigned>(grp), rally->first, rally->second, true);
                    }
                }
            }
            std::cout << "Reinforcements arrived: " << spawnedInfantry << " infantry, "
                      << spawnedVehicles << " vehicles (wave=" << ev.index
                      << " owner=" << ev.owner << " gate=" << ev.gate << " at " << firstX << ',' << firstY << ")\n";
            if (ev.owner == 0) {
                recordEventPos(static_cast<double>(firstX * mapScale), static_cast<double>(firstY * mapScale));
                pushMinimapPing(firstX, firstY, false);
                if (audio) audio->playGroup("e_reenforcement", 0.72f, ss2::VoicePriority::High);
                showNotice(ss2::trf("Reinforcements arrived ({0} soldiers).", {std::to_string(totalSpawned)}),
                           ss2::NoticeKind::accepted);
                if (!hadPlayerInfantry) {
                    renderer.focusMap(static_cast<double>(firstX * mapScale), static_cast<double>(firstY * mapScale));
                }
            }
        }
    };
    auto lastRepairAudioAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto lastArtilleryAlertAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto lastEngineAudioAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto nextAmbientAt = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    unsigned ambientPhase = 0;
    const auto isBuildingPointVisible = [&](int simX, int simY) -> bool {
        if (latestFog.empty() || fogGridW == 0 || fogGridH == 0) return true;
        const int originX = static_cast<int>(fogGridH) * 32;
        const auto [u, v] = ss2::tutorialFootWorld(simX, simY, originX);
        const int tx = std::clamp(u / 32, 0, static_cast<int>(fogGridW) - 1);
        const int ty = std::clamp(v / 32, 0, static_cast<int>(fogGridH) - 1);
        const std::size_t idx = static_cast<std::size_t>(ty) * fogGridW + static_cast<std::size_t>(tx);
        return idx < latestFog.size() && (latestFog[idx] & 1u) != 0;
    };
    std::set<size_t> prevSelInf, prevSelVeh;
    const auto issueGroundOrder = [&](int simX, int simY, bool isAttackMove, bool append) -> bool {
        if (!simulation || !selection) return false;
        const auto& selected = selection->selected();
        const auto& selectedVeh = selection->selectedVehicles();
        const std::vector<size_t> ids(selected.begin(), selected.end());
        const std::vector<size_t> vids(selectedVeh.begin(), selectedVeh.end());
        std::optional<size_t> infMove = size_t(0), vehMove = size_t(0);
        if (!ids.empty()) {
            infMove = isAttackMove ? simulation->attackMove(ids, simX, simY, append)
                                   : simulation->move(ids, simX, simY, append);
        }
        if (infMove && !vids.empty()) {
            vehMove = isAttackMove ? simulation->attackMoveVehicles(vids, simX, simY, append)
                                   : simulation->moveVehicles(vids, simX, simY, append);
        }
        if (!infMove || !vehMove) {
            gestureInterrupted = true;
            std::cout << "Tutorial I move interrupted by a pending briefing\n";
            return false;
        }
        const auto count = *infMove + *vehMove;
        const auto total = ids.size() + vids.size();
        if (count) {
            if (*infMove) {
                for (const auto id : ids) binocularUnits.erase(id);
            }
            std::string label;
            if (isAttackMove) {
                label = count == 1 && total == 1 ? ss2::tr("Attack-move order accepted")
                      : ss2::trf("Attack-move order accepted by {0} of {1} units", {std::to_string(count), std::to_string(total)});
            } else if (append) {
                label = ss2::trf("Waypoint queued ({0} units)", {std::to_string(count)});
            } else {
                label = count == 1 && total == 1 ? ss2::tr("Move order accepted")
                      : ss2::trf("Move order accepted by {0} of {1} soldiers", {std::to_string(count), std::to_string(total)});
            }
            if (paused) label = ss2::trf("{0}. Press Pause or Ctrl+P to resume.", {label});
            showNotice(label, count == total ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
            std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
            activeMarkers.push_back({
                {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                 isAttackMove ? ss2::MarkerKind::attack : ss2::MarkerKind::move},
                std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                std::nullopt,
                true
            });
            syncMarkers();
            playSelectionVoice(isAttackMove ? "attack" : "move");
            std::cout << "Tutorial I accepted move to map pixel " << simX << ',' << simY
                      << " for " << count << '/' << total << (vids.empty() ? " selected infantry\n" : " selected units\n");
            return true;
        }
        std::cout << "Tutorial I rejected move to map pixel " << simX << ',' << simY
                  << " for " << total << (vids.empty() ? " selected infantry\n" : " selected units\n");
        const auto [u, v] = ss2::tutorialPointerWorld(simX, simY);
        const bool outside = std::min(u, v) < 0 || std::max(u, v) >= 4096;
        orderNotice.show(ss2::tr(total == 0
                             ? (selection->playerLosses() > 0
                                    ? "Your selected soldiers were lost. Select remaining troops."
                                    : "Select your infantry, then right-click the destination.")
                             : outside ? "That point is outside the map."
                                       : "No route from this position. Choose another destination."),
                         total == 0 ? ss2::NoticeKind::unsupported : ss2::NoticeKind::blocked);
        return false;
    };
    const auto dispatchAirSupportAt = [&](ss2::assets::AirMissionKind kind, int simX, int simY) {
        if (!simulation) return;
        const auto slots = simulation->playerAirSupport();
        const std::string planeId = slots[static_cast<std::size_t>(kind)].planeId;
        if (simulation->callAirSupport(kind, simX, simY)) {
            ++airSortiesFlown;
            const bool strike = kind == ss2::assets::AirMissionKind::Bomber ||
                                kind == ss2::assets::AirMissionKind::Fighter;
            std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
            activeMarkers.push_back({
                {{(simX - 10) * mapScale, (simY - 10) * mapScale, 20 * mapScale, 20 * mapScale},
                 strike ? ss2::MarkerKind::attack : ss2::MarkerKind::move},
                std::chrono::steady_clock::now() + std::chrono::milliseconds(800),
                std::nullopt,
                true
            });
            syncMarkers();
            pushMinimapPing(simX, simY, strike);
            if (audio) {
                if (strike) (void)audio->playGroup("e_airraid_siren", 0.55f, ss2::VoicePriority::High);
                const auto* pSpec = unitCatalog ? unitCatalog->findPlane(planeId) : nullptr;
                const std::string flySnd = (pSpec && !pSpec->idleSound.empty())
                    ? pSpec->idleSound
                    : ((strike || kind == ss2::assets::AirMissionKind::Paratrooper) ? std::string("fly_heavy")
                                                                                    : std::string("fly_small"));
                (void)audio->playGroup(flySnd, 0.45f, ss2::VoicePriority::Normal);
                const std::string nation = inferNation(planeId.empty() ? "ju-87" : planeId);
                const std::string pfx = nation == "russian" ? "r_" : nation == "british" ? "b_"
                                      : nation == "american" ? "a_" : nation == "japanese" ? "j_" : "d_";
                if (!audio->playGroup(pfx + "plane_ack", 0.60f, ss2::VoicePriority::High) &&
                    !audio->playGroup(pfx + "pilot_ack", 0.60f, ss2::VoicePriority::High)) {
                    const char* airFallback = (nation == "american" || nation == "british") ? "b_plane_ack"
                        : (nation == "russian" ? "r_plane_ack" : "d_plane_ack");
                    (void)audio->playGroup(airFallback, 0.60f, ss2::VoicePriority::High);
                }
            }
            showNotice(ss2::trf("{0} dispatched!", {ss2::tr(airKindLabel(kind))}),
                       ss2::NoticeKind::accepted);
        } else {
            orderNotice.show(ss2::tr("Air support sortie unavailable."), ss2::NoticeKind::blocked);
        }
    };
    const auto dispatchPatrolAt = [&](int simX, int simY) {
        if (!simulation || !selection) return;
        const auto& selInf = selection->selected();
        const auto& selVeh = selection->selectedVehicles();
        const std::vector<size_t> infIds(selInf.begin(), selInf.end());
        const std::vector<size_t> vehIds(selVeh.begin(), selVeh.end());
        size_t patrolled = 0;
        if (!infIds.empty()) {
            if (const auto res = simulation->patrol(infIds, simX, simY)) patrolled += *res;
        }
        if (!vehIds.empty()) {
            if (const auto res = simulation->patrolVehicles(vehIds, simX, simY)) patrolled += *res;
        }
        if (patrolled > 0) {
            std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
            activeMarkers.push_back({
                {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                 ss2::MarkerKind::move},
                std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                std::nullopt,
                true
            });
            syncMarkers();
            pushMinimapPing(simX, simY, false);
            playSelectionVoice("move");
            showNotice(ss2::tr("Patrol route set."), ss2::NoticeKind::accepted);
        } else {
            orderNotice.show(ss2::tr("Selected units cannot patrol there."), ss2::NoticeKind::blocked);
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
                updateMinimapFog(*fogSnap, false);
                spritesDirty = true;
            }
            for (const auto& pose : simulation->pollPoses()) {
                spritesDirty = true;
                const int prevHp = scene->units().at(pose.index).hp;
                const int prevInfAmmo = scene->units().at(pose.index).ammo;
                if (pose.index >= unitExpa.size()) unitExpa.resize(pose.index + 1, 0);
                if (pose.index >= unitKills.size()) unitKills.resize(pose.index + 1, 0);
                if (pose.index >= unitGrenades.size()) unitGrenades.resize(pose.index + 1, 0);
                if (pose.index >= unitMineAmmo.size()) unitMineAmmo.resize(pose.index + 1, 0);
                unitExpa[pose.index] = pose.expa;
                unitKills[pose.index] = pose.kills;
                unitGrenades[pose.index] = pose.atGrenades;
                unitMineAmmo[pose.index] = pose.mineAmmo;
                if (pose.camouflaged) camouflagedUnits.insert(pose.index);
                else camouflagedUnits.erase(pose.index);
                if (pose.alive && pose.suppressed) suppressedUnits.insert(pose.index);
                else suppressedUnits.erase(pose.index);
                if (pose.crouchedState == 1) {
                    if (crouchedUnits.insert(pose.index).second) spritesDirty = true;
                } else if (pose.crouchedState == 0) {
                    if (crouchedUnits.erase(pose.index) > 0) spritesDirty = true;
                }
                const auto patch = scene->update(pose.index, pose.x, pose.y, pose.direction, pose.progress,
                                                 pose.walking, pose.hp, pose.visible ? 1 : 0, pose.ammo);
                if (patch.pixels.width && patch.pixels.height)
                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                selection->update(pose.index, scaleInfantry(scene->units().at(pose.index), mapScale));
                const auto now = std::chrono::steady_clock::now();
                if (prevHp > 0 && (!pose.alive || pose.hp <= 0) && audio && (pose.visible || pose.owner == 0)) {
                    auto& lastDeathSnd = lastWeaponAudio["screamofpain"];
                    if (now - lastDeathSnd >= std::chrono::milliseconds(450)) {
                        if (playWorldSound("screamofpain", 0.42f, pose.x, pose.y, ss2::VoicePriority::Low)) {
                            lastDeathSnd = now;
                        }
                    }
                }
                if (pose.alive && pose.hp > prevHp && combatEffects && pose.visible && activeVfx.size() < 64) {
                    const float hx = static_cast<float>(pose.x * mapScale);
                    const float hy = static_cast<float>(pose.y * mapScale) - 10.0f * static_cast<float>(mapScale);
                    activeVfx.push_back({ActiveVfx::Kind::HealPulse, hx, hy, hx, hy, now, std::chrono::milliseconds(550)});
                } else if (pose.alive && pose.hp < prevHp && pose.owner == 0) {
                    const auto& uid = scene->units().at(pose.index).id;
                    const auto* uSpec = unitCatalog ? unitCatalog->find(uid) : nullptr;
                    const int maxInfHp = (uSpec && uSpec->health > 0) ? uSpec->health : 100;
                    if (prevHp > maxInfHp / 3 && pose.hp <= maxInfHp / 3) {
                        playUnitVoiceForId("close", uid, false);
                    } else {
                        playUnitVoiceForId("underattack", uid, false);
                    }
                }
                if (pose.alive && pose.owner == 0 && prevInfAmmo > 0 && pose.ammo == 0) {
                    playUnitVoiceForId("noammo", scene->units().at(pose.index).id, false);
                }
                if (pose.fired) {
                    recoilUntil[pose.index] = now + std::chrono::milliseconds(120);
                    if (audio) {
                        const auto& uid = scene->units().at(pose.index).id;
                        const auto [rawGroup, cooldownMs, gain] = weaponSoundForUnit(uid);
                        const std::string sndGroup = pose.firedFlamer ? std::string("fmw") : rawGroup;
                        auto& lastAt = lastWeaponAudio[sndGroup];
                        if (now - lastAt >= std::chrono::milliseconds(cooldownMs)) {
                            bool played = playWorldSound(sndGroup, gain, pose.x, pose.y);
                            if (!played && sndGroup == "fmw") {
                                played = playWorldSound("fire", gain, pose.x, pose.y);
                            } else if (!played && (sndGroup == "bazooka" || sndGroup == "mortar")) {
                                played = playWorldSound("gunshot", gain, pose.x, pose.y);
                            }
                            if (played) lastAt = now;
                        }
                        if (!pose.threwGrenade && !pose.firedFlamer) {
                            const auto* uSpec = unitCatalog ? unitCatalog->find(uid) : nullptr;
                            if (uSpec && !uSpec->shot1.endBurstSound.empty()) {
                                auto& lastEb = lastWeaponAudio["eb:" + uSpec->shot1.endBurstSound];
                                if (now - lastEb >= std::chrono::milliseconds(750)) {
                                    if (playWorldSound(uSpec->shot1.endBurstSound, 0.22f, pose.x, pose.y,
                                                       ss2::VoicePriority::Low)) {
                                        lastEb = now;
                                    }
                                }
                            }
                        }
                        if (pose.threwGrenade || pose.firedExplosive) {
                            auto& lastExp = lastWeaponAudio["explo_tankdir"];
                            if (now - lastExp >= std::chrono::milliseconds(250)) {
                                if (playWorldSound("explo_tankdir", 0.52f, pose.targetX, pose.targetY)) {
                                    lastExp = now;
                                }
                            }
                        }
                    }
                    if (combatEffects && (pose.visible || (pose.garrisoned && (isAllyOwner(pose.owner) || isBuildingPointVisible(pose.x, pose.y)))) && activeVfx.size() < 64) {
                        const float sx = static_cast<float>(pose.x * mapScale);
                        const float sy = static_cast<float>(pose.y * mapScale) - 18.0f * static_cast<float>(mapScale);
                        const float tx = static_cast<float>(pose.targetX * mapScale);
                        const float ty = static_cast<float>(pose.targetY * mapScale) - 14.0f * static_cast<float>(mapScale);
                        const bool flamer = pose.firedFlamer ||
                                            (scene->units().at(pose.index).id.find("flam") != std::string::npos);
                        activeVfx.push_back({
                            flamer ? ActiveVfx::Kind::Flamer : ActiveVfx::Kind::Tracer,
                            sx, sy, tx, ty, now,
                            std::chrono::milliseconds(flamer ? 220 : 95)
                        });
                        if ((pose.threwGrenade || pose.firedExplosive) && activeVfx.size() < 64) {
                            activeVfx.push_back({
                                ActiveVfx::Kind::Explosion, tx, ty, tx, ty, now, std::chrono::milliseconds(360)
                            });
                        }
                    }
                }
                if (!pose.alive && prevHp > 0) {
                    const auto& deadUnit = scene->units().at(pose.index);
                    fallenSoldiers.push_back({pose.x, pose.y, deadUnit.id, pose.direction});
                    if (isHostileOwner(pose.owner)) {
                        ++enemyInfKilled;
                        enemyScoreEarned += scoreValueForId(deadUnit.id, false);
                    } else if (pose.owner == 0) {
                        friendlyScoreLost += scoreValueForId(deadUnit.id, false);
                    }
                    if (pose.owner == 0) {
                        recordEventPos(static_cast<double>(pose.x * mapScale), static_cast<double>(pose.y * mapScale));
                        pushMinimapPing(pose.x, pose.y, true);
                    }
                    const auto before = activeMarkers.size();
                    std::erase_if(activeMarkers, [&](const TimedMarker& m) {
                        return m.targetUid && *m.targetUid == pose.index;
                    });
                    if (activeMarkers.size() != before) markersDirty = true;
                    (void)playWorldSound("fall_small", 0.65f, pose.x, pose.y);
                    std::cout << "Tutorial I unit " << pose.index
                              << " (owner=" << pose.owner << " group=" << pose.group << ") eliminated\n";
                }
            }
            if (vehicleScene) {
                for (const auto& vp : simulation->pollVehiclePoses()) {
                    const int prevVehHp = vehicleScene->vehicles().at(vp.index).hp;
                    const bool prevVehVisible = vehicleScene->vehicles().at(vp.index).visible;
                    const int prevVehOwner = vehicleScene->vehicles().at(vp.index).owner;
                    const int prevVehAmmo = vehicleScene->vehicles().at(vp.index).ammo;
                    const int prevVehAmmo2 = (vp.index < vehicleAmmo2.size()) ? vehicleAmmo2[vp.index] : 0;
                    if (vp.maxHp > 0 && vp.index < initialVehicleHp.size()) {
                        initialVehicleHp[vp.index] = vp.maxHp;
                    }
                    if (vp.index < vehicleCargo.size()) {
                        vehicleCargo[vp.index] = {vp.passengerCount, vp.passengerCapacity};
                    }
                    if (vp.index < vehicleTowed.size()) {
                        vehicleTowed[vp.index] = vp.towedIndex;
                    }
                    if (vp.index < vehicleTowedBy.size()) {
                        vehicleTowedBy[vp.index] = vp.towedByIndex;
                    }
                    if (vp.index >= vehicleAmmo2.size()) vehicleAmmo2.resize(vp.index + 1, 0);
                    vehicleAmmo2[vp.index] = vp.ammo2;
                    if (vp.index >= vehExpa.size()) vehExpa.resize(vp.index + 1, 0);
                    if (vp.index >= vehKills.size()) vehKills.resize(vp.index + 1, 0);
                    if (vp.index >= vehicleCrewImpaired.size()) vehicleCrewImpaired.resize(vp.index + 1, false);
                    if (vp.index >= vehicleMoveImpaired.size()) vehicleMoveImpaired.resize(vp.index + 1, false);
                    if (vp.index >= vehicleSuppressed.size()) vehicleSuppressed.resize(vp.index + 1, false);
                    if (vp.index >= vehicleHatchOpen.size()) vehicleHatchOpen.resize(vp.index + 1, false);
                    vehExpa[vp.index] = vp.expa;
                    vehKills[vp.index] = vp.kills;
                    vehicleCrewImpaired[vp.index] = vp.crewImpaired;
                    vehicleMoveImpaired[vp.index] = vp.moveImpaired;
                    vehicleSuppressed[vp.index] = vp.suppressed;
                    vehicleHatchOpen[vp.index] = vp.hatchOpen;
                    if (vp.alive && vp.moving && vp.visible) {
                        const auto* smSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                        if (smSpec && smSpec->hasMoveSmoke) movingSmokeVehicles.insert(vp.index);
                        else movingSmokeVehicles.erase(vp.index);
                    } else {
                        movingSmokeVehicles.erase(vp.index);
                    }
                    const auto dirty = vehicleScene->update(vp.index, vp.x, vp.y, vp.hullDir, vp.turretDir,
                                                            vp.hp, vp.visible ? 1 : 0, vp.ammo);
                    if (mapScale == 1 && dirty.valid()) {
                        const auto patch = scene->repaintRegion(dirty.left, dirty.top, dirty.right, dirty.bottom);
                        if (patch.pixels.width && patch.pixels.height)
                            renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                    }
                    selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
                    if (vp.captured) {
                        vehicleScene->setOwner(vp.index, vp.owner);
                        selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
                        if (audio) audio->playGroup("e_captured", 0.72f, ss2::VoicePriority::High);
                        playUnitVoiceForId("select", vehicleScene->vehicles().at(vp.index).id, true, true);
                        showNotice(ss2::trf("Captured {0}!", {hudDisplayName(vehicleScene->vehicles().at(vp.index).id)}),
                                   ss2::NoticeKind::accepted);
                    }
                    if (vp.decrewed) {
                        vehicleHoldFireUnits.erase(vp.index);
                        vehicleScene->setOwner(vp.index, vp.owner);
                        selection->updateVehicle(vp.index, scaleVehicle(vehicleScene->vehicles().at(vp.index), mapScale));
                        if (isHostileOwner(prevVehOwner)) {
                            enemyScoreEarned += std::max(50, scoreValueForId(vehicleScene->vehicles().at(vp.index).id, true) / 2);
                        }
                        if (vp.visible || prevVehOwner == 0) {
                            recordEventPos(static_cast<double>(vp.x * mapScale), static_cast<double>(vp.y * mapScale));
                            pushMinimapPing(vp.x, vp.y, prevVehOwner == 0);
                            if (prevVehOwner == 0) {
                                showNotice(ss2::trf("Crew lost on {0}!",
                                                    {ss2::tr(hudDisplayName(vehicleScene->vehicles().at(vp.index).id))}),
                                           ss2::NoticeKind::unsupported);
                            } else if (isHostileOwner(prevVehOwner)) {
                                showNotice(ss2::trf("Enemy {0} crew neutralized — ready to capture!",
                                                    {ss2::tr(hudDisplayName(vehicleScene->vehicles().at(vp.index).id))}),
                                           ss2::NoticeKind::accepted);
                            }
                        }
                    }
                    if (vp.owner == 0 && vp.alive) {
                        if (const auto* sSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr) {
                            if (sSpec->isSupplyTruck() && !sSpec->isSupplyCrate()) {
                                if (sSpec->ammo1 > 0 && prevVehAmmo > 0 && vp.ammo == 0) {
                                    showNotice(ss2::tr("Supply truck out of ammo!"), ss2::NoticeKind::unsupported);
                                }
                                if (sSpec->ammo2 > 0 && prevVehAmmo2 > 0 && vp.ammo2 == 0) {
                                    showNotice(ss2::tr("Supply truck out of spare parts!"), ss2::NoticeKind::unsupported);
                                }
                            } else if (prevVehAmmo > 0 && vp.ammo == 0) {
                                playUnitVoiceForId("noammo", vehicleScene->vehicles().at(vp.index).id, true);
                            }
                        } else if (prevVehAmmo > 0 && vp.ammo == 0) {
                            playUnitVoiceForId("noammo", vehicleScene->vehicles().at(vp.index).id, true);
                        }
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (!prevVehVisible && vp.visible && vp.fired && isHostileOwner(vp.owner) &&
                        (vp.heavyImpact || vp.firedRocket)) {
                        recordEventPos(static_cast<double>(vp.x * mapScale), static_cast<double>(vp.y * mapScale));
                        pushMinimapPing(vp.x, vp.y, true);
                        if (now - lastArtilleryAlertAt >= std::chrono::seconds(6)) {
                            lastArtilleryAlertAt = now;
                            showNotice(ss2::tr("Enemy artillery flash spotted!"), ss2::NoticeKind::unsupported);
                        }
                    }
                    if (vp.moving && vp.visible && audio && now - lastEngineAudioAt >= std::chrono::milliseconds(1800)) {
                        const auto* vSpec = unitCatalog ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                        const std::string engSnd = (vSpec && !vSpec->moveSound.empty()) ? vSpec->moveSound : "carb_move";
                        if (playWorldSound(engSnd, 0.24f, vp.x, vp.y, ss2::VoicePriority::Low)) {
                            lastEngineAudioAt = now;
                        }
                    }
                    if (vp.alive && vp.hp < prevVehHp && vp.owner == 0) {
                        playUnitVoiceForId("underattack", vehicleScene->vehicles().at(vp.index).id, true);
                    }
                    if ((vp.repaired || vp.resupplied) && vp.visible) {
                        if (combatEffects && activeVfx.size() < 64) {
                            const float rx = static_cast<float>(vp.x * mapScale);
                            const float ry = static_cast<float>(vp.y * mapScale) - 10.0f * static_cast<float>(mapScale);
                            activeVfx.push_back({ActiveVfx::Kind::RepairPulse, rx, ry, rx, ry, now,
                                                 std::chrono::milliseconds(550)});
                        }
                        if (audio && now - lastRepairAudioAt >= std::chrono::milliseconds(1500)) {
                            if (playWorldSound("construct", 0.40f, vp.x, vp.y, ss2::VoicePriority::Low)) {
                                lastRepairAudioAt = now;
                            }
                        }
                    }
                    if (vp.fired) {
                        if (audio) {
                            const std::string sndGroup = !vp.fireSound.empty()
                                ? vp.fireSound
                                : (vp.firedFlamer ? std::string("fmw")
                                                  : (vp.firedRocket ? std::string("katyusha")
                                                                    : (vp.firedSecondary ? std::string("mg_tank_burst")
                                                                                         : std::string("howitzer"))));
                            const int cdMs = vp.firedFlamer ? 400 : (vp.firedRocket ? 140 : (vp.firedSecondary ? 260 : 320));
                            auto& lastAt = lastWeaponAudio[sndGroup];
                            if (now - lastAt >= std::chrono::milliseconds(cdMs)) {
                                const float gain = vp.firedFlamer ? 0.52f : (vp.firedSecondary ? 0.45f : 0.55f);
                                bool played = playWorldSound(sndGroup, gain, vp.x, vp.y);
                                if (!played && sndGroup == "fmw") {
                                    played = playWorldSound("fire", gain, vp.x, vp.y);
                                }
                                if (played) lastAt = now;
                            }
                            if (const auto* vSpec = unitCatalog
                                    ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr) {
                                const std::string ebSnd = vp.firedSecondary
                                    ? vSpec->shot2.endBurstSound : vSpec->shot1.endBurstSound;
                                if (!ebSnd.empty()) {
                                    auto& lastEb = lastWeaponAudio["eb:" + ebSnd];
                                    if (now - lastEb >= std::chrono::milliseconds(750)) {
                                        if (playWorldSound(ebSnd, 0.22f, vp.x, vp.y, ss2::VoicePriority::Low)) {
                                            lastEb = now;
                                        }
                                    }
                                }
                            }
                            if (vp.heavyImpact) {
                                const auto* vSpec = unitCatalog
                                    ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                                const std::string retailImpact = vSpec
                                    ? (vp.firedSecondary ? vSpec->shot2.impactSound : vSpec->shot1.impactSound)
                                    : std::string{};
                                const std::string expSnd = vp.hitVehicle ? "explo_tankdir"
                                    : (!retailImpact.empty() ? retailImpact
                                                             : (sndGroup.find("howitzer") != std::string::npos
                                                                    ? "howitzerexplosion" : "defaultexplosion"));
                                auto& lastExp = lastWeaponAudio[expSnd];
                                if (now - lastExp >= std::chrono::milliseconds(300)) {
                                    if (playWorldSound(expSnd, 0.56f, vp.targetX, vp.targetY)) {
                                        lastExp = now;
                                    }
                                }
                            }
                        }
                        if (combatEffects && vp.visible && activeVfx.size() < 64) {
                            const float sx = static_cast<float>(vp.x * mapScale);
                            const float sy = static_cast<float>(vp.y * mapScale) - 12.0f * static_cast<float>(mapScale);
                            const float tx = static_cast<float>(vp.targetX * mapScale);
                            const float ty = static_cast<float>(vp.targetY * mapScale) - 10.0f * static_cast<float>(mapScale);
                            activeVfx.push_back({
                                vp.firedFlamer ? ActiveVfx::Kind::Flamer
                                               : (vp.firedRocket ? ActiveVfx::Kind::Rocket
                                                                 : (vp.heavyImpact ? ActiveVfx::Kind::CannonShell
                                                                                   : ActiveVfx::Kind::Tracer)),
                                sx, sy, tx, ty, now,
                                std::chrono::milliseconds(vp.firedFlamer ? 260 : (vp.firedRocket ? 420 : (vp.firedSecondary ? 105 : 155)))
                            });
                            if (vp.heavyImpact && !vp.firedRocket && !vp.firedFlamer) {
                                activeVfx.push_back({
                                    ActiveVfx::Kind::Explosion, tx, ty, tx, ty, now, std::chrono::milliseconds(420)
                                });
                            }
                        }
                        if (vp.heavyImpact) {
                            const auto* cSpec = unitCatalog
                                ? unitCatalog->find(vehicleScene->vehicles().at(vp.index).id) : nullptr;
                            if (!cSpec || vp.craterScale > 0 || cSpec->shot1.craterScale > 0) {
                                const int cScale = vp.craterScale > 0
                                    ? int(vp.craterScale)
                                    : (cSpec ? cSpec->shot1.craterScale : 1);
                                stampBattleCrater(vp.targetX, vp.targetY, cScale >= 3);
                            }
                        }
                    }
                    if (!vp.alive && prevVehHp > 0) {
                        vehicleHoldFireUnits.erase(vp.index);
                        stampBattleCrater(vp.x, vp.y, true);
                        const auto& deadVeh = vehicleScene->vehicles().at(vp.index);
                        const int sval = scoreValueForId(deadVeh.id, true);
                        if (isHostileOwner(vp.owner)) {
                            ++enemyVehDestroyed;
                            enemyScoreEarned += sval;
                            if (const auto* spec = unitCatalog ? unitCatalog->find(deadVeh.id) : nullptr) {
                                if (spec->scoreType == "tank") ++enemyTanksDestroyed;
                                else if (spec->scoreType == "gun" || spec->scoreType == "aa") ++enemyGunsDestroyed;
                            }
                        } else if (vp.owner == 0) {
                            friendlyScoreLost += sval;
                        }
                        if (vp.owner == 0) {
                            recordEventPos(static_cast<double>(vp.x * mapScale), static_cast<double>(vp.y * mapScale));
                            pushMinimapPing(vp.x, vp.y, true);
                        }
                        if (combatEffects && vp.visible) {
                            const float wx = static_cast<float>(vp.x * mapScale);
                            const float wy = static_cast<float>(vp.y * mapScale);
                            if (activeVfx.size() < 64) {
                                activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, wx, wy, wx, wy, now,
                                                     std::chrono::milliseconds(650)});
                            }
                            if (burningWrecks.size() >= 8) burningWrecks.erase(burningWrecks.begin());
                            burningWrecks.push_back({wx, wy, now + std::chrono::seconds(18)});
                        }
                        (void)playWorldSound("defaultexplosion", 0.62f, vp.x, vp.y, ss2::VoicePriority::High);
                        std::cout << "Vehicle " << vp.index << " (owner=" << vp.owner << " group=" << vp.group
                                  << ") eliminated\n";
                    }
                }
            }
            const auto mEvents = simulation->pollMineEvents();
            for (const auto& me : mEvents) {
                const auto now = std::chrono::steady_clock::now();
                const float mx = static_cast<float>(me.x * mapScale);
                const float my = static_cast<float>(me.y * mapScale);
                if (me.kind == ss2::MineEvent::Detonated) {
                    recordEventPos(static_cast<double>(mx), static_cast<double>(my));
                    pushMinimapPing(me.x, me.y, true);
                    stampBattleCrater(me.x, me.y, me.antiTank);
                    if (combatEffects && activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::Explosion, mx, my, mx, my, now,
                                             std::chrono::milliseconds(me.antiTank ? 520 : 380)});
                    }
                    if (audio) {
                        const std::string mineSnd = unitCatalog
                            ? (me.antiTank ? unitCatalog->mineSpec().atSound : unitCatalog->mineSpec().apSound)
                            : (me.antiTank ? std::string("defaultexplosion") : std::string("smallexplosion"));
                        (void)playWorldSound(mineSnd.empty() ? "defaultexplosion" : mineSnd,
                                             me.antiTank ? 0.62f : 0.54f, me.x, me.y, ss2::VoicePriority::High);
                        audio->playGroup("e_mines", 0.65f, ss2::VoicePriority::High);
                    }
                    showNotice(ss2::tr(me.antiTank ? "Anti-tank mine detonated!" : "Anti-personnel mine detonated!"),
                               ss2::NoticeKind::blocked);
                } else if (me.kind == ss2::MineEvent::Detected) {
                    pushMinimapPing(me.x, me.y, true);
                    if (audio) audio->playGroup("e_mines", 0.60f, ss2::VoicePriority::High);
                    showNotice(ss2::tr("Enemy minefield detected!"), ss2::NoticeKind::unsupported);
                } else if (me.kind == ss2::MineEvent::Disarmed || me.kind == ss2::MineEvent::Laid) {
                    std::string sapId;
                    if (selection && !selection->selected().empty() && scene) {
                        const size_t sid = *selection->selected().begin();
                        if (sid < scene->units().size()) sapId = scene->units()[sid].id;
                    }
                    if (sapId.empty() && scene) {
                        for (const auto& u : scene->units()) {
                            if (u.hp > 0 && u.owner == 0) { sapId = u.id; break; }
                        }
                    }
                    if (me.kind == ss2::MineEvent::Disarmed) {
                        if (!sapId.empty()) playUnitVoiceForId("getmine", sapId, false, true);
                        else (void)playWorldSound("d_getmine", 0.55f, me.x, me.y);
                        showNotice(ss2::tr("Mine disarmed."), ss2::NoticeKind::accepted);
                    } else {
                        if (!sapId.empty()) playUnitVoiceForId("laymine", sapId, false, true);
                        showNotice(ss2::tr("Anti-tank mine laid."), ss2::NoticeKind::accepted);
                    }
                }
            }
            if (!mEvents.empty()) liveVisibleMines = simulation->visibleMines();
            for (const auto& bu : simulation->pollBuildingUpdates()) {
                if (bu.index < liveBuildings.size()) {
                    auto& lb = liveBuildings[bu.index];
                    lb.hp = bu.hp;
                    lb.maxHp = bu.maxHp;
                    lb.occupantCount = bu.occupantCount;
                    lb.occupantIds = bu.occupantIds;
                    lb.owner = bu.owner;
                    lb.destroyed = bu.destroyed;
                    if (selectedBuilding && *selectedBuilding == bu.index && (lb.destroyed || lb.occupantCount == 0 || lb.owner != 0)) {
                        selectedBuilding.reset();
                    }
                    if (bu.collapsedThisTick || bu.destroyed) {
                        applyBuildingRubble(bu.index);
                    }
                    const auto now = std::chrono::steady_clock::now();
                    const float bx = static_cast<float>(bu.x * mapScale);
                    const float by = static_cast<float>(bu.y * mapScale);
                    if (bu.collapsedThisTick) {
                        ++buildingsDemolished;
                        if (isHostileOwner(bu.owner)) enemyScoreEarned += 150;
                        recordEventPos(static_cast<double>(bx), static_cast<double>(by));
                        pushMinimapPing(bu.x, bu.y, true);
                        if (combatEffects) {
                            if (activeVfx.size() < 64) {
                                activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, bx, by, bx, by, now,
                                                     std::chrono::milliseconds(650)});
                            }
                            if (burningWrecks.size() >= 8) burningWrecks.erase(burningWrecks.begin());
                            burningWrecks.push_back({bx, by, now + std::chrono::seconds(20)});
                        }
                        (void)playWorldSound("defaultexplosion", 0.68f, bu.x, bu.y, ss2::VoicePriority::High);
                        showNotice(ss2::tr("Building destroyed!"), ss2::NoticeKind::blocked);
                    } else if (bu.damagedThisTick && combatEffects && activeVfx.size() < 64 &&
                               (bu.owner == 0 || isBuildingPointVisible(bu.x, bu.y))) {
                        activeVfx.push_back({ActiveVfx::Kind::Explosion, bx, by, bx, by, now,
                                             std::chrono::milliseconds(320)});
                    }
                }
            }
            if (!paused && audio) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= nextAmbientAt) {
                    const ss2::assets::AmbientWoofer* best = nullptr;
                    double bestDist = 1e12;
                    double bestRad = 1.0;
                    const auto& woofers = simulation->data().woofers;
                    if (!woofers.empty()) {
                        int winW = 1280, winH = 720;
                        if (!SDL_GetWindowSize(window, &winW, &winH)) { winW = 1280; winH = 720; }
                        const auto center = ss2::hidpi::logicalWindowCenter(winW, winH);
                        const auto [camPxX, camPxY] = renderer.mapPosition(
                            static_cast<float>(center.x), static_cast<float>(center.y));
                        const double camX = camPxX / static_cast<double>(mapScale);
                        const double camY = camPxY / static_cast<double>(mapScale);
                        for (const auto& w : woofers) {
                            const double radPx = std::max(320.0, static_cast<double>(w.radiusTiles) * 32.0);
                            const double d = std::hypot(static_cast<double>(w.x) - camX,
                                                        static_cast<double>(w.y) - camY);
                            if (d <= radPx && d < bestDist) {
                                best = &w;
                                bestDist = d;
                                bestRad = radPx;
                            }
                        }
                    }
                    if (best) {
                        const std::string ambSnd = ss2::assets::resolveAmbientWooferSound(best->sound);
                        const float gain = std::clamp(
                            best->volume * static_cast<float>(1.0 - 0.65 * (bestDist / bestRad)),
                            0.05f, 0.35f);
                        (void)audio->playGroup(ambSnd, gain, ss2::VoicePriority::Ambient);
                        const unsigned periodMs = (best->maxPeriodMs >= 500u)
                            ? std::clamp<unsigned>(
                                  ((ambientPhase & 1u) != 0u && best->minPeriodMs >= 500u)
                                      ? best->minPeriodMs : best->maxPeriodMs,
                                  1500u, 12000u)
                            : 6000u;
                        ++ambientPhase;
                        nextAmbientAt = now + std::chrono::milliseconds(periodMs);
                    } else {
                        const char* ambGroup = (!burningWrecks.empty() && (ambientPhase & 1u) != 0u)
                            ? "@fire_loop"
                            : (mapScheme == 2 ? "@cicades_loop" : "@birds");
                        (void)audio->playGroup(ambGroup, 0.14f, ss2::VoicePriority::Ambient);
                        ++ambientPhase;
                        nextAmbientAt = now + std::chrono::seconds(8);
                    }
                }
            }
            for (const auto& ap : simulation->pollAirSorties()) {
                spritesDirty = true;
                const auto now = std::chrono::steady_clock::now();
                if (ap.aaFired) {
                    if (audio) {
                        const std::string aaSnd = !ap.aaSoundGroup.empty() ? ap.aaSoundGroup : std::string("gunshot");
                        auto& lastAa = lastWeaponAudio[aaSnd];
                        if (now - lastAa >= std::chrono::milliseconds(180)) {
                            bool played = playWorldSound(aaSnd, 0.48f, ap.aaOriginX, ap.aaOriginY);
                            if (!played && aaSnd != "gunshot") {
                                played = playWorldSound("gunshot", 0.48f, ap.aaOriginX, ap.aaOriginY);
                            }
                            if (played) lastAa = now;
                        }
                    }
                    if (combatEffects && activeVfx.size() < 64) {
                        activeVfx.push_back({
                            ActiveVfx::Kind::Tracer,
                            static_cast<float>(ap.aaOriginX * mapScale),
                            static_cast<float>(ap.aaOriginY * mapScale) - 12.0f * static_cast<float>(mapScale),
                            static_cast<float>(ap.x * mapScale),
                            static_cast<float>(ap.y * mapScale),
                            now,
                            std::chrono::milliseconds(140)
                        });
                    }
                }
                if (ap.active) {
                    auto [it, inserted] = livePlanes.try_emplace(ap.id);
                    it->second.pose = ap;
                    if (inserted) uploadPlaneSprite(it->second);
                } else {
                    const auto it = livePlanes.find(ap.id);
                    if (it != livePlanes.end()) {
                        planeAtlas.release(it->second.spriteKey);
                        livePlanes.erase(it);
                    }
                }
            }
            for (const auto& ae : simulation->pollAirStrikeEvents()) {
                const auto now = std::chrono::steady_clock::now();
                const float ax = static_cast<float>(ae.x * mapScale);
                const float ay = static_cast<float>(ae.y * mapScale);
                if (ae.kind == ss2::AirStrikeEvent::BombImpact) {
                    if (ae.isLeaflet) {
                        pushMinimapPing(ae.x, ae.y, false);
                        if (combatEffects && activeVfx.size() < 64) {
                            activeVfx.push_back({ActiveVfx::Kind::RepairPulse, ax, ay, ax, ay, now,
                                                 std::chrono::milliseconds(500)});
                        }
                        if (audio) {
                            auto& lastLf = lastWeaponAudio["leaflet"];
                            if (now - lastLf >= std::chrono::milliseconds(600)) {
                                if (playWorldSound("e_telegram", 0.32f, ae.x, ae.y, ss2::VoicePriority::Low)) {
                                    lastLf = now;
                                }
                            }
                        }
                        if (ae.owner == 0) {
                            showNotice(ss2::tr("Propaganda leaflets dropped — enemy troops suppressed!"),
                                       ss2::NoticeKind::accepted);
                        }
                    } else {
                        stampBattleCrater(ae.x, ae.y, true);
                        pushMinimapPing(ae.x, ae.y, true);
                        if (combatEffects && activeVfx.size() < 64) {
                            activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, ax, ay, ax, ay, now,
                                                 std::chrono::milliseconds(620)});
                        }
                        if (audio) {
                            const auto* pSpec = unitCatalog ? unitCatalog->findPlane(ae.planeId) : nullptr;
                            const std::string bSnd = (pSpec && !pSpec->bombSound.empty())
                                ? pSpec->bombSound : std::string("bomb");
                            auto& lastB = lastWeaponAudio[bSnd];
                            if (now - lastB >= std::chrono::milliseconds(140)) {
                                if (playWorldSound(bSnd, 0.62f, ae.x, ae.y, ss2::VoicePriority::High)) {
                                    lastB = now;
                                }
                            }
                        }
                    }
                } else if (ae.kind == ss2::AirStrikeEvent::StrafeImpact) {
                    const auto* pSpec = unitCatalog ? unitCatalog->findPlane(ae.planeId) : nullptr;
                    const bool rocketStrafe =
                        pSpec && pSpec->effectiveGroundHitType() == ss2::assets::HitType::Explosive;
                    if (rocketStrafe) {
                        stampBattleCrater(ae.x, ae.y, false);
                    }
                    if (combatEffects && activeVfx.size() < 64) {
                        if (rocketStrafe) {
                            activeVfx.push_back({ActiveVfx::Kind::Explosion, ax, ay, ax, ay, now,
                                                 std::chrono::milliseconds(360)});
                        } else {
                            activeVfx.push_back({ActiveVfx::Kind::Tracer,
                                                 ax - 20.0f * static_cast<float>(mapScale),
                                                 ay - 28.0f * static_cast<float>(mapScale),
                                                 ax, ay, now, std::chrono::milliseconds(240)});
                        }
                    }
                    if (audio) {
                        const std::string stSnd = (pSpec && !pSpec->groundShotSound.empty())
                            ? pSpec->groundShotSound : std::string("mg2_aa");
                        auto& lastSt = lastWeaponAudio[stSnd];
                        if (now - lastSt >= std::chrono::milliseconds(180)) {
                            if (playWorldSound(stSnd, 0.50f, ae.x, ae.y)) {
                                lastSt = now;
                            }
                        }
                    }
                } else if (ae.kind == ss2::AirStrikeEvent::ParatrooperDrop && selection && scene) {
                    std::size_t droppedInf = 0;
                    std::size_t droppedVeh = 0;
                    const int maxMapX = std::max(32, static_cast<int>(hudMapW) - 16);
                    const int maxMapY = std::max(32, static_cast<int>(hudMapH) - 16);
                    for (const auto& entry : ae.paratroopers) {
                        for (int c = 0; c < entry.count; ++c) {
                            const std::size_t droppedTotal = droppedInf + droppedVeh;
                            const int col = static_cast<int>(droppedTotal % 5u);
                            const int row = static_cast<int>(droppedTotal / 5u);
                            const int px = std::clamp(ae.x + (col - 2) * 18, 16, maxMapX);
                            const int py = std::clamp(ae.y + (row - 1) * 16, 16, maxMapY);
                            ss2::assets::Placement p{entry.unitId, px, py, 0, 100, 100, ae.group, ae.owner};
                            (void)ensureGpuInfantrySlot(entry.unitId);
                            if (auto spawned = scene->spawn(p)) {
                                const auto& [newIdx, patch] = *spawned;
                                const auto& unit = scene->units().at(newIdx);
                                ensureUnitPortrait(unit.id, false);
                                (void)simulation->spawnInfantry(unit, 0);
                                const int realAmmo = scaledSoldierAmmo(unit.id, 100);
                                (void)scene->update(newIdx, unit.x, unit.y, unit.direction,
                                                    unit.progress, unit.walking, unit.hp, -1, realAmmo);
                                selection->addUnit(scaleInfantry(scene->units().at(newIdx), mapScale));
                                initialUnitHp.push_back(std::max(1, unit.hp));
                                unitExpa.push_back(0);
                                unitKills.push_back(0);
                                unitGrenades.push_back(initialGrenadesForId(unit.id));
                                unitMineAmmo.push_back(initialMinesForId(unit.id));
                                if (patch.pixels.width && patch.pixels.height)
                                    renderer.updateMapRegion(patch.x, patch.y, patch.pixels);
                                if (combatEffects && activeVfx.size() < 64) {
                                    const float cx = static_cast<float>(unit.x * mapScale);
                                    const float cy = static_cast<float>(unit.y * mapScale);
                                    activeVfx.push_back({
                                        ActiveVfx::Kind::Parachute, cx, cy - 68.0f * static_cast<float>(mapScale),
                                        cx, cy, now, std::chrono::milliseconds(1100)
                                    });
                                }
                                spritesDirty = true;
                                ++droppedInf;
                            } else if (auto spawnedVeh = spawnAndRegisterVehicle(p)) {
                                const auto& v = vehicleScene->vehicles().at(spawnedVeh->first);
                                if (combatEffects && activeVfx.size() < 64) {
                                    const float vx = static_cast<float>(v.x * mapScale);
                                    const float vy = static_cast<float>(v.y * mapScale);
                                    activeVfx.push_back({
                                        ActiveVfx::Kind::Parachute, vx, vy - 74.0f * static_cast<float>(mapScale),
                                        vx, vy, now, std::chrono::milliseconds(1250)
                                    });
                                }
                                spritesDirty = true;
                                ++droppedVeh;
                            }
                        }
                    }
                    if (ae.owner == 0 && (droppedInf + droppedVeh) > 0) {
                        recordEventPos(static_cast<double>(ax), static_cast<double>(ay));
                        pushMinimapPing(ae.x, ae.y, false);
                        if (audio) audio->playGroup("e_reenforcement", 0.72f, ss2::VoicePriority::High);
                        if (droppedInf > 0 && droppedVeh > 0) {
                            showNotice(ss2::trf("Airborne drop landed ({0} soldiers, {1} guns/equipment)!",
                                                {std::to_string(droppedInf), std::to_string(droppedVeh)}),
                                       ss2::NoticeKind::accepted);
                        } else if (droppedVeh > 0) {
                            showNotice(ss2::trf("Airborne equipment dropped ({0} units)!",
                                                {std::to_string(droppedVeh)}),
                                       ss2::NoticeKind::accepted);
                        } else {
                            showNotice(ss2::trf("Paratroopers dropped ({0} soldiers)!",
                                                {std::to_string(droppedInf)}),
                                       ss2::NoticeKind::accepted);
                        }
                    }
                } else if (ae.kind == ss2::AirStrikeEvent::SupplyDrop) {
                    if (combatEffects && activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::Parachute, ax, ay - 68.0f * static_cast<float>(mapScale),
                                             ax, ay, now, std::chrono::milliseconds(1050)});
                        if (activeVfx.size() < 64) {
                            activeVfx.push_back({ActiveVfx::Kind::RepairPulse, ax, ay, ax, ay, now,
                                                 std::chrono::milliseconds(650)});
                        }
                    }
                    (void)playWorldSound("construct", 0.50f, ae.x, ae.y);
                    if (ae.owner == 0) {
                        showNotice(ss2::tr("Air supply drop landed!"), ss2::NoticeKind::accepted);
                    }
                } else if (ae.kind == ss2::AirStrikeEvent::PlaneShotDown) {
                    const int pScore = ae.scoreValue > 0 ? ae.scoreValue : 500;
                    if (isHostileOwner(ae.owner)) {
                        ++enemyPlanesShotDown;
                        enemyScoreEarned += pScore;
                    } else if (ae.owner == 0) {
                        friendlyScoreLost += pScore;
                    }
                    stampBattleCrater(ae.x, ae.y, true);
                    pushMinimapPing(ae.x, ae.y, true);
                    if (combatEffects && activeVfx.size() < 64) {
                        activeVfx.push_back({ActiveVfx::Kind::VehicleExplosion, ax, ay, ax, ay, now,
                                             std::chrono::milliseconds(700)});
                    }
                    const auto* pSpec = unitCatalog ? unitCatalog->findPlane(ae.planeId) : nullptr;
                    const std::string fallSnd = (pSpec && !pSpec->fallSound.empty())
                        ? pSpec->fallSound : std::string("falling_heavy");
                    (void)playWorldSound(fallSnd, 0.58f, ae.x, ae.y, ss2::VoicePriority::High);
                    (void)playWorldSound("defaultexplosion", 0.68f, ae.x, ae.y, ss2::VoicePriority::High);
                    showNotice(ss2::tr("Aircraft shot down by AA flak!"), ss2::NoticeKind::blocked);
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
                    delta == 1 ? ss2::trf("1 soldier lost ({0} total).", {std::to_string(total)})
                               : ss2::trf("{0} soldiers lost ({1} total).", {std::to_string(delta), std::to_string(total)}),
                    ss2::NoticeKind::blocked);
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
                    if (audio) {
                        if (event.index < data.phraseSounds.size() && !data.phraseSounds[event.index].empty()) {
                            audio->playGroup(data.phraseSounds[event.index], 0.90f, ss2::VoicePriority::High);
                        } else {
                            audio->playGroup("e_telegram", 0.55f);
                        }
                    }
                    const auto& phrase = data.phrases.at(event.index);
                    logHud(phrase);
                    if (briefing.active()) pendingDialogs.push_back(phrase);
                    else briefing.open(phrase);
                } else if (event.kind == ss2::MissionEvent::Camera) {
                    const auto [x, y] = data.locations.at(event.index);
                    recordEventPos(x * mapScale, y * mapScale);
                    renderer.focusMap(x * mapScale, y * mapScale);
                } else if (event.kind == ss2::MissionEvent::Message) {
                    if (audio) {
                        if (event.index < data.phraseSounds.size() && !data.phraseSounds[event.index].empty()) {
                            audio->playGroup(data.phraseSounds[event.index], 0.90f, ss2::VoicePriority::High);
                        } else {
                            audio->playGroup("e_telegram", 0.50f);
                        }
                    }
                    const auto& message = data.phrases.at(event.index);
                    currentObjective = message;
                    logHud(message);
                    SDL_SetWindowTitle(window, message.c_str());
                    orderNotice.show(message, ss2::NoticeKind::accepted);
                } else if (event.kind == ss2::MissionEvent::Marker && selection) {
                    const auto [x, y] = data.markers.at(event.index);
                    objectiveMarkerSim = {x, y};
                    recordEventPos(x * mapScale, y * mapScale);
                    selection->mark(x * mapScale, y * mapScale);
                    markerHint = true;
                } else if (event.kind == ss2::MissionEvent::Victory) {
                    std::cout << "Tutorial I victory (" << event.index << ")\n";
                    if (audio) audio->playGroup("e_victory", 0.80f, ss2::VoicePriority::High);
                    currentObjective = "Mission Accomplished!";
                    logHud(currentObjective);
                    SDL_SetWindowTitle(window, "Sudden Strike II | Tutorial I — Victory!");
                    orderNotice.show(ss2::tr("Mission Accomplished!"), ss2::NoticeKind::accepted, ss2::Notice::Duration::zero());
                    openDebriefMenu(true);
                } else if (event.kind == ss2::MissionEvent::Defeat) {
                    std::cout << "Mission defeat (" << event.index << ")\n";
                    if (audio) audio->playGroup("e_defeat", 0.80f, ss2::VoicePriority::High);
                    currentObjective = "Mission Failed.";
                    logHud(currentObjective);
                    SDL_SetWindowTitle(window, "Sudden Strike II — Defeat");
                    orderNotice.show(ss2::tr("Mission Failed."), ss2::NoticeKind::blocked, ss2::Notice::Duration::zero());
                    openDebriefMenu(false);
                } else if (event.kind == ss2::MissionEvent::Reinforcement) {
                    spawnReinforcementWave(event);
                    spritesDirty = true;
                }
            }
            if (spritesDirty) syncGpuSprites();
            if (!limit && !timing && nextMissionOut && !paused && !briefing.active() && !menu.active() &&
                simulation->simTick() >= nextAutoSaveTick) {
                nextAutoSaveTick = simulation->simTick() + 7500u;
                doSaveSlot(-1);
            }
        }
        if (gestureInterrupted && !briefing.active()) {
            orderNotice.show(ss2::tr("A mission event interrupted your input. Select or issue the order again."),
                             ss2::NoticeKind::unsupported);
            gestureInterrupted = false;
            markerHint = false;
        } else if (markerHint && !briefing.active()) {
            orderNotice.show(ss2::tr("M shows the mission marker. Tab focuses your troops."), ss2::NoticeKind::unsupported);
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
                if (audio && (!mr.activated.empty() || mr.closed)) {
                    audio->playGroup("accept", 0.42f);
                }
                if (mr.closed || mr.activated == "resume") {
                    closeMenu();
                } else if (mr.activated == "airsupport" && simulation) {
                    openAirSupportMenu();
                } else if (mr.activated.rfind("aircall:", 0) == 0 && simulation) {
                    const std::string_view idxStr(mr.activated.data() + 8, mr.activated.size() - 8);
                    std::size_t idx = 0;
                    const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
                    if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < 5u) {
                        closeMenu();
                        armAirSupportKind(static_cast<ss2::assets::AirMissionKind>(idx));
                    }
                } else if (mr.activated == "quicksave" && simulation) {
                    doQuickSave();
                    closeMenu();
                } else if (mr.activated == "quickload" && simulation) {
                    doQuickLoad();
                    closeMenu();
                } else if (mr.activated == "saveload" && simulation) {
                    openSaveLoadMenu();
                } else if ((mr.activated.rfind("save:", 0) == 0 || mr.activated.rfind("load:", 0) == 0) && simulation) {
                    const bool isLoad = (mr.activated[0] == 'l');
                    const std::string_view slotStr(mr.activated.data() + 5, mr.activated.size() - 5);
                    int slot = -1;
                    const auto [end, ec] = std::from_chars(slotStr.data(), slotStr.data() + slotStr.size(), slot);
                    if (ec == std::errc{} && end == slotStr.data() + slotStr.size() &&
                        slot >= (isLoad ? -1 : 0) && slot <= 3) {
                        if (isLoad) doLoadSlot(slot);
                        else doSaveSlot(slot);
                        closeMenu();
                    }
                } else if (mr.activated == "effects") {
                    combatEffects = !combatEffects;
                    settings.combatEffects = combatEffects;
                    persistNow();
                    if (!combatEffects) {
                        activeVfx.clear();   // the next frame sends an empty list
                        burningWrecks.clear();
                    }
                    std::cout << "Tutorial I combat effects " << (combatEffects ? "on" : "off") << '\n';
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "speed" && simulation) {
                    const int cur = simulation->speedPercent();
                    const int next = cur == 50 ? 100 : cur == 100 ? 200 : cur == 200 ? 300 : 50;
                    simulation->setSpeedPercent(next);
                    settings.speedPercent = next;
                    persistNow();
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "difficulty") {
                    settings.difficulty = settings.difficulty == 1 ? 2 : settings.difficulty == 2 ? 3 : 1;
                    persistNow();
                    const char* diffKey = settings.difficulty == 1 ? "Easy" : settings.difficulty == 3 ? "Hard" : "Normal";
                    menu.setItems(buildPauseItems());
                    orderNotice.show(ss2::trf("Difficulty set to {0} (applies on restart or next mission).",
                                              {ss2::tr(diffKey)}),
                                     ss2::NoticeKind::accepted);
                } else if (mr.activated == "language") {
                    const auto current = ss2::language();
                    const auto nextLang = current == ss2::Language::english ? ss2::Language::french
                                        : current == ss2::Language::french ? ss2::Language::spanish : ss2::Language::english;
                    ss2::setLanguage(nextLang);
                    settings.language = nextLang;
                    persistNow();
                    menu.open(ss2::tr("Paused"), buildPauseItems());
                    for (int step = 0; step < 16 && menu.highlighted() != "language"; ++step) {
                        SDL_Event down{};
                        down.type = SDL_EVENT_KEY_DOWN;
                        down.key.key = SDLK_DOWN;
                        menu.handle(down);
                    }
                } else if (mr.activated == "smooth") {
                    smoothTerrain = !smoothTerrain;
                    settings.smoothTerrain = smoothTerrain;
                    persistNow();
                    renderer.setMapFilterLinear(smoothTerrain);
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "music" && audio) {
                    settings.musicMuted = audio->toggleMusicMute();
                    persistNow();
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "sfx_volume" && audio) {
                    const float cur = audio->sfxVolume();
                    const float next = cur > 0.85f ? 0.75f : cur > 0.60f ? 0.50f : cur > 0.35f ? 0.25f : cur > 0.05f ? 0.0f : 1.0f;
                    audio->setSfxVolume(next);
                    settings.sfxVolume = next;
                    persistNow();
                    if (next > 0.0f) audio->playGroup("accept", 0.5f);
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "music_volume" && audio) {
                    const float cur = audio->musicVolume();
                    const float next = cur >= 0.34f ? 0.28f : cur >= 0.23f ? 0.18f : cur >= 0.13f ? 0.09f : 0.40f;
                    audio->setMusicVolume(next);
                    settings.musicVolume = next;
                    persistNow();
                    menu.setItems(buildPauseItems());
                } else if (mr.activated == "next_track" && audio) {
                    audio->nextMusicTrack();
                    logHud(ss2::tr("Switched to next soundtrack track."));
                } else if (mr.activated == "next_campaign" && nextCampaignMissionPath && nextMissionOut) {
                    *nextMissionOut = *nextCampaignMissionPath;
                    menu.close();
                    running = false;
                } else if (mr.activated == "replay" && !currentMissionPath.empty() && nextMissionOut) {
                    *nextMissionOut = currentMissionPath;
                    menu.close();
                    running = false;
                } else if (mr.activated == "campaigns") {
                    openCampaignsMenu();
                } else if (mr.activated.rfind("campaign:", 0) == 0) {
                    const std::string_view idxStr(mr.activated.data() + 9, mr.activated.size() - 9);
                    std::size_t idx = 0;
                    const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
                    if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < menuCampaigns.size()) {
                        openCampaignMissionsMenu(idx);
                    }
                } else if (mr.activated.rfind("cmission:", 0) == 0 && nextMissionOut) {
                    const std::string_view rest(mr.activated.data() + 9, mr.activated.size() - 9);
                    const auto sep = rest.find(':');
                    if (sep != std::string_view::npos) {
                        std::size_t cIdx = 0, mIdx = 0;
                        const auto [e1, ec1] = std::from_chars(rest.data(), rest.data() + sep, cIdx);
                        const auto [e2, ec2] = std::from_chars(rest.data() + sep + 1, rest.data() + rest.size(), mIdx);
                        if (ec1 == std::errc{} && e1 == rest.data() + sep &&
                            ec2 == std::errc{} && e2 == rest.data() + rest.size() &&
                            cIdx < menuCampaigns.size() && mIdx < menuCampaigns[cIdx].missions.size()) {
                            *nextMissionOut = menuCampaigns[cIdx].missions[mIdx].path;
                            menu.close();
                            running = false;
                        }
                    }
                } else if (mr.activated == "back_campaigns") {
                    openCampaignsMenu();
                } else if (mr.activated == "missions") {
                    openMissionsMenu();
                } else if (mr.activated.rfind("mission:", 0) == 0 && nextMissionOut) {
                    const std::string_view idxStr(mr.activated.data() + 8, mr.activated.size() - 8);
                    std::size_t idx = 0;
                    const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
                    if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < menuMissions.size()) {
                        *nextMissionOut = menuMissions[idx].path;
                        menu.close();
                        running = false;
                    }
                } else if (mr.activated == "forces") {
                    openForceOverviewMenu(true);
                } else if (mr.activated == "briefing") {
                    openMissionBriefing();
                } else if (mr.activated.rfind("force_select:", 0) == 0 && selection) {
                    const std::string_view idxStr(mr.activated.data() + 13, mr.activated.size() - 13);
                    std::size_t idx = 0;
                    const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
                    if (ec == std::errc{} && end == idxStr.data() + idxStr.size() && idx < forceRosterGroups.size()) {
                        const auto& grp = forceRosterGroups[idx];
                        std::set<size_t> infSet(grp.infIds.begin(), grp.infIds.end());
                        std::set<size_t> vehSet(grp.vehIds.begin(), grp.vehIds.end());
                        closeMenu();
                        selection->setSelection(std::move(infSet), std::move(vehSet));
                        selectedBuilding.reset();
                        prevSelInf = selection->selected();
                        prevSelVeh = selection->selectedVehicles();
                        if (const auto ctr = selection->selectionCentroid()) {
                            renderer.focusMap(ctr->first, ctr->second, true);
                        }
                        const std::size_t selTotal = selection->selected().size() + selection->selectedVehicles().size();
                        showNotice(ss2::trf("Selected {0} ({1} units).",
                                            {ss2::tr(grp.displayName), std::to_string(selTotal)}),
                                   ss2::NoticeKind::accepted);
                    }
                } else if (mr.activated.rfind("force_bldg:", 0) == 0 && selection) {
                    const std::string_view idxStr(mr.activated.data() + 11, mr.activated.size() - 11);
                    std::size_t bIdx = 0;
                    const auto [end, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), bIdx);
                    if (ec == std::errc{} && end == idxStr.data() + idxStr.size() &&
                        bIdx < liveBuildings.size() && !liveBuildings[bIdx].destroyed) {
                        const auto& lb = liveBuildings[bIdx];
                        closeMenu();
                        selection->setSelection({}, {});
                        prevSelInf.clear();
                        prevSelVeh.clear();
                        selectedBuilding = bIdx;
                        renderer.focusMap(lb.x * mapScale, lb.y * mapScale, true);
                        showNotice(ss2::trf("Garrisoned building ({0}/{1}) — press U to evacuate.",
                                            {std::to_string(lb.occupantCount), std::to_string(lb.capacity)}),
                                   ss2::NoticeKind::accepted);
                    }
                } else if (mr.activated == "controls") {
                    openControlsMenu();
                } else if (mr.activated == "log") {
                    openLogMenu();
                } else if (mr.activated == "back") {
                    menu.open(ss2::tr("Paused"), buildPauseItems());
                } else if (mr.activated == "quit") {
                    running = false;
                }
                if (mr.consumed) continue;
            }
            if (hud && !briefing.active()) {
                const auto hr = hud->handle(event);
                if (hr.action == ss2::HudAction::moveCamera && hudMapW && hudMapH) {
                    if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                        event.button.button == SDL_BUTTON_LEFT &&
                        (armedAirSupport || attackMoveArmed || patrolArmed)) {
                        hud->cancelMinimapDrag();
                        const int simX = std::clamp(static_cast<int>(std::lround(hr.u * static_cast<float>(hudMapW))),
                                                    0, static_cast<int>(hudMapW) - 1);
                        const int simY = std::clamp(static_cast<int>(std::lround(hr.v * static_cast<float>(hudMapH))),
                                                    0, static_cast<int>(hudMapH) - 1);
                        if (armedAirSupport) {
                            dispatchAirSupportAt(*armedAirSupport, simX, simY);
                            armedAirSupport.reset();
                        } else if (patrolArmed) {
                            dispatchPatrolAt(simX, simY);
                            patrolArmed = false;
                        } else if (attackMoveArmed) {
                            const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                            if (issueGroundOrder(simX, simY, true, append)) {
                                pushMinimapPing(simX, simY, true);
                            }
                            attackMoveArmed = false;
                        }
                        resetCursor();
                    } else {
                        renderer.focusMap(hr.u * static_cast<double>(hudMapW * mapScale),
                                          hr.v * static_cast<double>(hudMapH * mapScale));
                    }
                } else if (hr.action == ss2::HudAction::orderMinimap && simulation && hudMapW && hudMapH) {
                    const int simX = std::clamp(static_cast<int>(std::lround(hr.u * static_cast<float>(hudMapW))),
                                                0, static_cast<int>(hudMapW) - 1);
                    const int simY = std::clamp(static_cast<int>(std::lround(hr.v * static_cast<float>(hudMapH))),
                                                0, static_cast<int>(hudMapH) - 1);
                    if (patrolArmed) {
                        dispatchPatrolAt(simX, simY);
                        patrolArmed = false;
                    } else {
                        const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                        if (issueGroundOrder(simX, simY, attackMoveArmed, append)) {
                            pushMinimapPing(simX, simY, attackMoveArmed);
                        }
                        attackMoveArmed = false;
                    }
                    resetCursor();
                } else if (hr.action == ss2::HudAction::stop) {
                    doStop();
                } else if (hr.action == ss2::HudAction::holdFire) {
                    doToggleHoldFire();
                } else if (hr.action == ss2::HudAction::crouch) {
                    doToggleCrouch();
                } else if (hr.action == ss2::HudAction::recon) {
                    doToggleBinoculars();
                } else if (hr.action == ss2::HudAction::attackMove) {
                    doArmAttackMove();
                } else if (hr.action == ss2::HudAction::patrol) {
                    doArmPatrol();
                } else if (hr.action == ss2::HudAction::attackGround) {
                    doArmAttackGround();
                } else if (hr.action == ss2::HudAction::unload) {
                    doUnloadOrUnhitch();
                } else if (hr.action == ss2::HudAction::mine) {
                    doLayMine();
                } else if (hr.action == ss2::HudAction::scatter) {
                    doScatter();
                } else if (hr.action == ss2::HudAction::recallGroup && hr.groupDigit >= 0 && hr.groupDigit < 10 && selection) {
                    const auto beforeInf = selection->selected();
                    const auto beforeVeh = selection->selectedVehicles();
                    selection->recallControlGroup(static_cast<unsigned>(hr.groupDigit), hr.shiftHeld);
                    if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                        selectedBuilding.reset();
                    }
                    if (!hr.shiftHeld && selection->selected() == beforeInf && selection->selectedVehicles() == beforeVeh) {
                        if (const auto ctr = selection->selectionCentroid()) {
                            renderer.focusMap(ctr->first, ctr->second, true);
                        }
                    }
                } else if (hr.action == ss2::HudAction::selectClass && !hr.classLabel.empty() && selection && scene) {
                    std::set<size_t> newInf, newVeh;
                    const auto& units = scene->units();
                    for (const auto id : selection->selected()) {
                        if (id >= units.size()) continue;
                        const bool match = (hudDisplayName(units[id].id) == hr.classLabel);
                        if (hr.shiftHeld ? !match : match) newInf.insert(id);
                    }
                    if (vehicleScene) {
                        const auto& vehs = vehicleScene->vehicles();
                        for (const auto id : selection->selectedVehicles()) {
                            if (id >= vehs.size()) continue;
                            const bool match = (hudDisplayName(vehs[id].id) == hr.classLabel);
                            if (hr.shiftHeld ? !match : match) newVeh.insert(id);
                        }
                    }
                    selection->setSelection(std::move(newInf), std::move(newVeh));
                    if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                        selectedBuilding.reset();
                    }
                } else if (hr.action == ss2::HudAction::airSupport && hr.airSlot >= 0 && hr.airSlot < 5 && simulation) {
                    const auto kind = static_cast<ss2::assets::AirMissionKind>(hr.airSlot);
                    if (armedAirSupport && *armedAirSupport == kind) {
                        armedAirSupport.reset();
                        resetCursor();
                        orderNotice.show(ss2::tr("Air support cancelled."), ss2::NoticeKind::unsupported);
                    } else {
                        armAirSupportKind(kind);
                    }
                } else if (hr.action == ss2::HudAction::pause) {
                    doTogglePause();
                } else if (hr.action == ss2::HudAction::log) {
                    openLogMenu();
                } else if (hr.action == ss2::HudAction::menu) {
                    openPauseMenu();
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
                    if (manualBriefingOpen) {
                        manualBriefingOpen = false;
                        if (!pendingDialogs.empty()) {
                            briefing.open(std::move(pendingDialogs.front()));
                            pendingDialogs.pop_front();
                        }
                    } else if (pendingDialogs.empty()) {
                        simulation->acknowledge();
                    } else {
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
            if (simulation && selection && scene && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_O && !(event.key.mod & SDL_KMOD_CTRL)) {
                openForceOverviewMenu(false);
                continue;
            }
            if (simulation && !menu.active() && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_J && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                openMissionBriefing();
                continue;
            }
            if (simulation && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                (event.key.key == SDLK_PAUSE || (event.key.key == SDLK_P && (event.key.mod & SDL_KMOD_CTRL)))) {
                doTogglePause();
            }
            if (audio && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_M && (event.key.mod & SDL_KMOD_CTRL)) {
                const bool muted = audio->toggleMusicMute();
                settings.musicMuted = muted;
                persistNow();
                orderNotice.show(ss2::tr(muted ? "Music muted (Ctrl+M)." : "Music unmuted."), ss2::NoticeKind::accepted);
                continue;
            }
            if (audio && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_N && !(event.key.mod & SDL_KMOD_CTRL)) {
                if (audio->nextMusicTrack()) orderNotice.show(ss2::tr("Next soundtrack track."), ss2::NoticeKind::accepted);
            }
            if (simulation && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                (event.key.key == SDLK_LEFTBRACKET || event.key.key == SDLK_RIGHTBRACKET) &&
                !(event.key.mod & SDL_KMOD_CTRL)) {
                static constexpr std::array<int, 4> kSpeeds{{50, 100, 200, 300}};
                const int cur = simulation->speedPercent();
                std::size_t idx = 1;
                for (std::size_t i = 0; i < kSpeeds.size(); ++i) {
                    if (kSpeeds[i] == cur) { idx = i; break; }
                }
                if (event.key.key == SDLK_LEFTBRACKET && idx > 0) --idx;
                else if (event.key.key == SDLK_RIGHTBRACKET && idx + 1 < kSpeeds.size()) ++idx;
                const int next = kSpeeds[idx];
                simulation->setSpeedPercent(next);
                settings.speedPercent = next;
                persistNow();
                const std::string spStr = next == 50 ? "0.5x" : next == 200 ? "2x" : next == 300 ? "3x" : "1x";
                orderNotice.show(ss2::trf("Game speed: {0}", {spStr}), ss2::NoticeKind::accepted);
                continue;
            }
            if (simulation && selection && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_U && !(event.key.mod & SDL_KMOD_CTRL)) {
                doUnloadOrUnhitch();
                continue;
            }
            if (simulation && selection && (!selection->selected().empty() || !selection->selectedVehicles().empty()) &&
                event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !(event.key.mod & SDL_KMOD_CTRL)) {
                if (event.key.key == SDLK_S) {
                    attackGroundArmed = false;
                    reverseMoveArmed = false;
                    patrolArmed = false;
                    faceArmed = false;
                    armedAirSupport.reset();
                    doStop();
                } else if (event.key.key == SDLK_A) {
                    doArmAttackMove();
                    continue;
                } else if (event.key.key == SDLK_Q) {
                    doArmPatrol();
                    continue;
                } else if (event.key.key == SDLK_R && !selection->selectedVehicles().empty()) {
                    attackMoveArmed = false;
                    attackGroundArmed = false;
                    patrolArmed = false;
                    faceArmed = false;
                    armedAirSupport.reset();
                    reverseMoveArmed = !reverseMoveArmed;
                    if (reverseMoveArmed) {
                        SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                        orderNotice.show(ss2::tr("Reverse move: click destination (keeps front armor facing away)."),
                                         ss2::NoticeKind::accepted);
                    } else {
                        resetCursor();
                    }
                    continue;
                } else if (event.key.key == SDLK_T && !selection->selectedVehicles().empty()) {
                    attackMoveArmed = false;
                    attackGroundArmed = false;
                    reverseMoveArmed = false;
                    patrolArmed = false;
                    armedAirSupport.reset();
                    faceArmed = !faceArmed;
                    if (faceArmed) {
                        SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                        orderNotice.show(ss2::tr("Face direction: click map point to rotate front armor."),
                                         ss2::NoticeKind::accepted);
                    } else {
                        resetCursor();
                    }
                    continue;
                } else if (event.key.key == SDLK_G && !selection->selectedVehicles().empty()) {
                    doArmAttackGround();
                    continue;
                } else if (event.key.key == SDLK_E && !selection->selected().empty()) {
                    doLayMine();
                    continue;
                } else if (event.key.key == SDLK_D && !selection->selected().empty()) {
                    doScatter();
                    continue;
                } else if (event.key.key == SDLK_K && !selection->selectedVehicles().empty()) {
                    doAbandonVehicles();
                    continue;
                } else if (event.key.key == SDLK_X && !selection->selectedVehicles().empty()) {
                    const auto& vsel = selection->selectedVehicles();
                    const std::vector<size_t> vids(vsel.begin(), vsel.end());
                    if (const auto opened = simulation->toggleMarchMode(vids)) {
                        playSelectionVoice("select");
                        orderNotice.show(
                            ss2::tr(*opened
                                        ? "Commander hatch opened (+sight, exposed to snipers — X to close)."
                                        : "Commander hatch closed (buttoned — protected from snipers)."),
                            ss2::NoticeKind::accepted);
                    } else {
                        orderNotice.show(ss2::tr("Selected vehicles have no commander hatch."),
                                         ss2::NoticeKind::unsupported);
                    }
                    continue;
                } else if (event.key.key == SDLK_H) doToggleHoldFire();
                else if (event.key.key == SDLK_C && !selection->selected().empty()) doToggleCrouch();
                else if (event.key.key == SDLK_B && (!selection->selected().empty() || selectedBuilding)) doToggleBinoculars();
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (simulation && selection && scene && event.key.key == SDLK_I &&
                    !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                    const auto idle = simulation->idlePlayerUnits();
                    if (idle.empty()) {
                        orderNotice.show(ss2::tr("All units are busy."), ss2::NoticeKind::unsupported);
                    } else {
                        idleUnitCursor %= idle.size();
                        if (idle.size() > 1u) {
                            const auto& cur = idle[idleUnitCursor];
                            const bool sameInf = !cur.isVehicle && selection->selected().size() == 1u &&
                                                 selection->selectedVehicles().empty() &&
                                                 selection->selected().contains(cur.index);
                            const bool sameVeh = cur.isVehicle && selection->selected().empty() &&
                                                 selection->selectedVehicles().size() == 1u &&
                                                 selection->selectedVehicles().contains(cur.index);
                            if (sameInf || sameVeh) {
                                idleUnitCursor = (idleUnitCursor + 1u) % idle.size();
                            }
                        }
                        const auto picked = idle[idleUnitCursor];
                        const std::size_t shownIdx = idleUnitCursor + 1u;
                        idleUnitCursor = (idleUnitCursor + 1u) % idle.size();
                        std::string unitName;
                        if (picked.isVehicle && vehicleScene && picked.index < vehicleScene->vehicles().size()) {
                            selection->setSelection({}, {picked.index});
                            unitName = hudDisplayName(vehicleScene->vehicles()[picked.index].id);
                        } else if (!picked.isVehicle && picked.index < scene->units().size()) {
                            selection->setSelection({picked.index}, {});
                            unitName = hudDisplayName(scene->units()[picked.index].id);
                        }
                        selectedBuilding.reset();
                        if (const auto ctr = selection->selectionCentroid()) {
                            renderer.focusMap(ctr->first, ctr->second, true);
                        }
                        orderNotice.show(ss2::trf("Idle unit {0}/{1}: {2} (I for next)",
                                                  {std::to_string(shownIdx), std::to_string(idle.size()),
                                                   ss2::tr(unitName)}),
                                         ss2::NoticeKind::accepted);
                    }
                    continue;
                }
                if (simulation && selection && scene && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                    event.key.key == SDLK_W && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                    struct TriageEntry { bool isVehicle{false}; size_t index{0}; };
                    std::vector<TriageEntry> needy;
                    const auto& units = scene->units();
                    for (size_t idx = 0; idx < units.size(); ++idx) {
                        const auto& u = units[idx];
                        if (u.owner != 0 || u.hp <= 0 || !u.visible) continue;
                        const int maxHp = (idx < initialUnitHp.size() && initialUnitHp[idx] > 0) ? initialUnitHp[idx] : 100;
                        const bool dry = (u.ammo <= 0) && isCombatAmmoSoldierId(u.id);
                        if (u.hp < maxHp || dry) needy.push_back({false, idx});
                    }
                    if (vehicleScene) {
                        const auto& vehs = vehicleScene->vehicles();
                        for (size_t idx = 0; idx < vehs.size(); ++idx) {
                            const auto& v = vehs[idx];
                            if (!v.alive || v.hp <= 0 || v.owner != 0 || !v.visible) continue;
                            const int maxHp = (idx < initialVehicleHp.size() && initialVehicleHp[idx] > 0) ? initialVehicleHp[idx] : 100;
                            const bool dry = (v.ammo <= 0) && isArmedVehicleId(v.id);
                            if (v.hp < maxHp || dry) needy.push_back({true, idx});
                        }
                    }
                    if (needy.empty()) {
                        orderNotice.show(ss2::tr("All units are healthy and supplied."), ss2::NoticeKind::accepted);
                    } else {
                        triageCursor %= needy.size();
                        if (needy.size() > 1u) {
                            const auto& cur = needy[triageCursor];
                            const bool sameInf = !cur.isVehicle && selection->selected().size() == 1u &&
                                                 selection->selectedVehicles().empty() &&
                                                 selection->selected().contains(cur.index);
                            const bool sameVeh = cur.isVehicle && selection->selected().empty() &&
                                                 selection->selectedVehicles().size() == 1u &&
                                                 selection->selectedVehicles().contains(cur.index);
                            if (sameInf || sameVeh) {
                                triageCursor = (triageCursor + 1u) % needy.size();
                            }
                        }
                        const auto picked = needy[triageCursor];
                        const std::size_t shownIdx = triageCursor + 1u;
                        triageCursor = (triageCursor + 1u) % needy.size();
                        std::string unitName;
                        if (picked.isVehicle && vehicleScene && picked.index < vehicleScene->vehicles().size()) {
                            selection->setSelection({}, {picked.index});
                            unitName = hudDisplayName(vehicleScene->vehicles()[picked.index].id);
                        } else if (!picked.isVehicle && picked.index < units.size()) {
                            selection->setSelection({picked.index}, {});
                            unitName = hudDisplayName(units[picked.index].id);
                        }
                        selectedBuilding.reset();
                        prevSelInf = selection->selected();
                        prevSelVeh = selection->selectedVehicles();
                        if (const auto ctr = selection->selectionCentroid()) {
                            renderer.focusMap(ctr->first, ctr->second, true);
                        }
                        orderNotice.show(ss2::trf("Needs support {0}/{1}: {2} (W for next)",
                                                  {std::to_string(shownIdx), std::to_string(needy.size()),
                                                   ss2::tr(unitName)}),
                                         ss2::NoticeKind::unsupported);
                    }
                    continue;
                }
                if (simulation && selection && scene && event.key.key == SDLK_A &&
                    (event.key.mod & SDL_KMOD_CTRL) && !(event.key.mod & SDL_KMOD_ALT)) {
                    const bool allMap = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                    const size_t count = selection->selectAllPlayerUnits(!allMap);
                    if (count > 0) {
                        selectedBuilding.reset();
                        prevSelInf = selection->selected();
                        prevSelVeh = selection->selectedVehicles();
                        playSelectionVoice("select");
                        orderNotice.show(ss2::trf(allMap ? "Selected all {0} units." : "Selected {0} units on screen.",
                                                  {std::to_string(count)}),
                                         ss2::NoticeKind::accepted);
                    } else {
                        orderNotice.show(ss2::tr(allMap ? "No player units available." : "No player units on screen."),
                                         ss2::NoticeKind::unsupported);
                    }
                    continue;
                }
                if (simulation && selection && scene && event.key.key == SDLK_Z &&
                    !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                    const auto idle = simulation->idlePlayerUnits();
                    if (idle.empty()) {
                        orderNotice.show(ss2::tr("All units are busy."), ss2::NoticeKind::unsupported);
                    } else {
                        std::set<size_t> infSet, vehSet;
                        const size_t maxInf = scene->units().size();
                        const size_t maxVeh = vehicleScene ? vehicleScene->vehicles().size() : 0u;
                        for (const auto& iu : idle) {
                            if (iu.isVehicle && iu.index < maxVeh) vehSet.insert(iu.index);
                            else if (!iu.isVehicle && iu.index < maxInf) infSet.insert(iu.index);
                        }
                        selection->setSelection(std::move(infSet), std::move(vehSet));
                        const size_t count = selection->selected().size() + selection->selectedVehicles().size();
                        if (count > 0) {
                            selectedBuilding.reset();
                            prevSelInf = selection->selected();
                            prevSelVeh = selection->selectedVehicles();
                            playSelectionVoice("select");
                            if (const auto ctr = selection->selectionCentroid()) {
                                renderer.focusMap(ctr->first, ctr->second, true);
                            }
                            orderNotice.show(ss2::trf("Selected {0} idle units.", {std::to_string(count)}),
                                             ss2::NoticeKind::accepted);
                        } else {
                            orderNotice.show(ss2::tr("All units are busy."), ss2::NoticeKind::unsupported);
                        }
                    }
                    continue;
                }
                if (simulation && event.key.key == SDLK_F && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                    const auto slots = simulation->playerAirSupport();
                    const std::size_t startIdx = armedAirSupport ? ((static_cast<std::size_t>(*armedAirSupport) + 1u) % 5u) : 0u;
                    bool found = false;
                    for (std::size_t off = 0; off < 5u; ++off) {
                        const std::size_t k = (startIdx + off) % 5u;
                        if (slots[k].ready > 0) {
                            armAirSupportKind(static_cast<ss2::assets::AirMissionKind>(k));
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        orderNotice.show(ss2::tr("No air support sorties available in this mission."),
                                         ss2::NoticeKind::unsupported);
                    }
                    continue;
                }
                if (simulation && (event.key.mod & SDL_KMOD_ALT) && !(event.key.mod & SDL_KMOD_CTRL) &&
                    event.key.key >= SDLK_1 && event.key.key <= SDLK_5) {
                    const auto idx = static_cast<std::size_t>(event.key.key - SDLK_1);
                    armAirSupportKind(static_cast<ss2::assets::AirMissionKind>(idx));
                    continue;
                }
                if (simulation &&
                    ((event.key.key == SDLK_F9 && !(event.key.mod & SDL_KMOD_CTRL)) ||
                     (event.key.key == SDLK_S && (event.key.mod & SDL_KMOD_CTRL)))) {
                    doQuickSave();
                    continue;
                }
                if (simulation &&
                    ((event.key.key == SDLK_F10 && !(event.key.mod & SDL_KMOD_CTRL)) ||
                     (event.key.key == SDLK_L && (event.key.mod & SDL_KMOD_CTRL)))) {
                    doQuickLoad();
                    continue;
                }
                const auto fSlot = [](SDL_Keycode k) -> int {
                    switch (k) {
                    case SDLK_F1: return 0; case SDLK_F2: return 1; case SDLK_F3: return 2; case SDLK_F4: return 3;
                    case SDLK_F5: return 4; case SDLK_F6: return 5; case SDLK_F7: return 6; case SDLK_F8: return 7;
                    default: return -1;
                    }
                }(event.key.key);
                if (fSlot >= 0) {
                    if (event.key.mod & SDL_KMOD_CTRL) {
                        int winW = 1, winH = 1;
                        SDL_GetWindowSize(window, &winW, &winH);
                        cameraBookmarks[static_cast<std::size_t>(fSlot)] =
                            renderer.mapPosition(static_cast<double>(winW) * 0.5, static_cast<double>(winH) * 0.5);
                        orderNotice.show(ss2::trf("Camera bookmark F{0} saved.", {std::to_string(fSlot + 1)}),
                                         ss2::NoticeKind::accepted);
                    } else if (cameraBookmarks[static_cast<std::size_t>(fSlot)]) {
                        const auto [bx, by] = *cameraBookmarks[static_cast<std::size_t>(fSlot)];
                        renderer.focusMap(bx, by, true);
                    }
                    continue;
                }
                if (event.key.key == SDLK_SPACE && !(event.key.mod & SDL_KMOD_CTRL) && !eventHistory.empty()) {
                    const auto [ex, ey] = eventHistory[eventHistoryCursor % eventHistory.size()];
                    renderer.focusMap(ex, ey, true);
                    eventHistoryCursor = (eventHistoryCursor + 1u) % eventHistory.size();
                    continue;
                }
            }
            if (simulation && (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed || armedAirSupport) &&
                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                (void)selection->cancelDrag();
                const auto [x, y] = renderer.mapPosition(event.button.x, event.button.y);
                const int simX = int(std::floor(x / mapScale));
                const int simY = int(std::floor(y / mapScale));
                if (armedAirSupport) {
                    dispatchAirSupportAt(*armedAirSupport, simX, simY);
                    armedAirSupport.reset();
                } else if (reverseMoveArmed) {
                    const auto& vsel = selection->selectedVehicles();
                    const std::vector<size_t> vids(vsel.begin(), vsel.end());
                    if (const auto res = simulation->reverseMoveVehicles(vids, simX, simY); res && *res > 0) {
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Reverse move ordered ({0} vehicles).", {std::to_string(*res)}),
                                   ss2::NoticeKind::accepted);
                    } else {
                        orderNotice.show(ss2::tr("Selected vehicles cannot reverse."), ss2::NoticeKind::blocked);
                    }
                    reverseMoveArmed = false;
                } else if (faceArmed) {
                    const auto& vsel = selection->selectedVehicles();
                    const std::vector<size_t> vids(vsel.begin(), vsel.end());
                    if (const auto res = simulation->faceVehicles(vids, simX, simY); res && *res > 0) {
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Rotating front armor ({0} vehicles).", {std::to_string(*res)}),
                                   ss2::NoticeKind::accepted);
                    } else {
                        orderNotice.show(ss2::tr("Selected vehicles cannot rotate."), ss2::NoticeKind::blocked);
                    }
                    faceArmed = false;
                } else if (attackGroundArmed) {
                    const auto& vsel = selection->selectedVehicles();
                    const auto& sel = selection->selected();
                    const std::vector<size_t> vids(vsel.begin(), vsel.end());
                    const std::vector<size_t> sids(sel.begin(), sel.end());
                    size_t totalBombard = 0;
                    if (!vids.empty()) {
                        if (const auto res = simulation->attackGroundVehicles(vids, simX, simY)) totalBombard += *res;
                    }
                    if (!sids.empty()) {
                        if (const auto res = simulation->attackGroundSoldiers(sids, simX, simY)) totalBombard += *res;
                    }
                    if (totalBombard > 0) {
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{(simX - 10) * mapScale, (simY - 10) * mapScale, 20 * mapScale, 20 * mapScale},
                             ss2::MarkerKind::attack},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("attack");
                        showNotice(ss2::trf("Attack ground ordered ({0} units).", {std::to_string(totalBombard)}),
                                   ss2::NoticeKind::accepted);
                    } else {
                        orderNotice.show(ss2::tr("Selected units cannot bombard that location."), ss2::NoticeKind::blocked);
                    }
                    attackGroundArmed = false;
                } else if (patrolArmed) {
                    dispatchPatrolAt(simX, simY);
                    patrolArmed = false;
                } else {
                    const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                    (void)issueGroundOrder(simX, simY, true, append);
                    attackMoveArmed = false;
                }
                resetCursor();
                continue;
            }
            if (simulation && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                if (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed || armedAirSupport) {
                    attackMoveArmed = false;
                    attackGroundArmed = false;
                    reverseMoveArmed = false;
                    patrolArmed = false;
                    faceArmed = false;
                    armedAirSupport.reset();
                    resetCursor();
                    continue;
                }
                // Gold's viewport cancels a left selection box or orders at the press coordinates.
                if (selection->cancelDrag()) continue;
                const auto [x,y] = renderer.mapPosition(event.button.x, event.button.y);
                const int simX = int(std::floor(x / mapScale)), simY = int(std::floor(y / mapScale));
                const auto& selected = selection->selected();
                const auto& selectedVeh = selection->selectedVehicles();
                const std::vector<size_t> ids(selected.begin(), selected.end());
                const std::vector<size_t> vids(selectedVeh.begin(), selectedVeh.end());
                const auto targetInf = selection->infantryAt(int(std::floor(x)), int(std::floor(y)));
                const auto targetVeh = selection->vehicleAt(int(std::floor(x)), int(std::floor(y)));
                if (ids.empty() && vids.empty() && selectedBuilding && *selectedBuilding < liveBuildings.size()) {
                    const auto& sb = liveBuildings[*selectedBuilding];
                    if (!sb.destroyed && sb.owner == 0 && sb.occupantCount > 0) {
                        std::optional<size_t> bOrder;
                        int mLeft = 0, mTop = 0, mW = 16, mH = 16;
                        if (targetVeh && vehicleScene &&
                            isHostileOwner(vehicleScene->vehicles().at(*targetVeh).owner) &&
                            simulation->isValidVehicleAttackTarget(*targetVeh)) {
                            bOrder = simulation->orderBuildingAttack(*selectedBuilding, true, *targetVeh);
                            const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                            mW = tv.width > 0 ? tv.width : 24;
                            mH = tv.height > 0 ? tv.height : 24;
                            mLeft = tv.width > 0 ? tv.left : tv.x - 12;
                            mTop = tv.height > 0 ? tv.top : tv.y - 12;
                        } else if (targetInf && isHostileOwner(scene->units().at(*targetInf).owner) &&
                                   simulation->isValidAttackTarget(*targetInf)) {
                            bOrder = simulation->orderBuildingAttack(*selectedBuilding, false, *targetInf);
                            const auto& tu = scene->units().at(*targetInf);
                            mW = std::max(12, tu.width);
                            mH = std::max(14, tu.height);
                            mLeft = tu.left;
                            mTop = tu.top;
                        } else if (const auto targetBld = buildingAtMap(simX, simY)) {
                            const auto& tb = liveBuildings[*targetBld];
                            if (*targetBld != *selectedBuilding && tb.owner >= 0 && isHostileOwner(tb.owner)) {
                                bOrder = simulation->orderBuildingAttackBuilding(*selectedBuilding, *targetBld);
                                mLeft = tb.left;
                                mTop = tb.top;
                                mW = tb.width;
                                mH = tb.height;
                            }
                        }
                        if (bOrder && *bOrder > 0) {
                            std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                            activeMarkers.push_back({
                                {{mLeft * mapScale, mTop * mapScale, mW * mapScale, mH * mapScale},
                                 ss2::MarkerKind::attack},
                                std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                                std::nullopt,
                                true
                            });
                            syncMarkers();
                            playSelectionVoice("attack");
                            showNotice(ss2::tr("Garrison engaging target!"), ss2::NoticeKind::accepted);
                            continue;
                        }
                    }
                }
                if (!vids.empty() && targetVeh && vehicleScene) {
                    std::optional<std::pair<size_t, size_t>> towPair;
                    for (size_t sv : vids) {
                        if (simulation->canTowVehicle(sv, *targetVeh)) {
                            towPair = {sv, *targetVeh};
                            break;
                        }
                        if (simulation->canTowVehicle(*targetVeh, sv)) {
                            towPair = {*targetVeh, sv};
                            break;
                        }
                    }
                    if (towPair) {
                        const auto [trIdx, gIdx] = *towPair;
                        if (const auto res = simulation->towVehicle(trIdx, gIdx); res && *res > 0) {
                            const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                            const int mw = tv.width > 0 ? tv.width : 24;
                            const int mh = tv.height > 0 ? tv.height : 24;
                            const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                            const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                            std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                            activeMarkers.push_back({
                                {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                                 ss2::MarkerKind::move},
                                std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                                std::nullopt,
                                true
                            });
                            syncMarkers();
                            playSelectionVoice("move");
                            showNotice(ss2::trf("Hitching {0}...", {hudDisplayName(vehicleScene->vehicles().at(gIdx).id)}),
                                       ss2::NoticeKind::accepted);
                            continue;
                        }
                    }
                }
                if (!vids.empty() && targetVeh && vehicleScene &&
                    vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
                    simulation->canSupportVehicle(vids, *targetVeh)) {
                    if (const auto res = simulation->orderSupportVehicle(vids, *targetVeh); res && *res > 0) {
                        const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                        const int mw = tv.width > 0 ? tv.width : 24;
                        const int mh = tv.height > 0 ? tv.height : 24;
                        const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                        const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Supply truck servicing {0}...", {ss2::tr(hudDisplayName(tv.id))}),
                                   ss2::NoticeKind::accepted);
                        continue;
                    }
                }
                if ((!ids.empty() || !vids.empty()) && targetInf &&
                    scene->units().at(*targetInf).owner == 0 &&
                    simulation->canHealSoldier(ids, vids, *targetInf)) {
                    if (const auto res = simulation->orderHealSoldier(ids, vids, *targetInf); res && *res > 0) {
                        const auto& tu = scene->units().at(*targetInf);
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{tu.left * mapScale, tu.top * mapScale, tu.width * mapScale, tu.height * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            *targetInf,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::tr("Moving to heal wounded soldier."), ss2::NoticeKind::accepted);
                        continue;
                    }
                }
                if (!ids.empty() && targetVeh && vehicleScene &&
                    vehicleScene->vehicles().at(*targetVeh).owner == 3 &&
                    simulation->canCaptureVehicle(*targetVeh)) {
                    if (const auto res = simulation->captureVehicle(ids, *targetVeh); res && *res > 0) {
                        const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                        const int mw = tv.width > 0 ? tv.width : 24;
                        const int mh = tv.height > 0 ? tv.height : 24;
                        const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                        const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Capturing vehicle ({0} soldiers)...", {std::to_string(*res)}),
                                   ss2::NoticeKind::accepted);
                        continue;
                    }
                }
                if (!ids.empty() && targetVeh && vehicleScene &&
                    vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
                    simulation->canBoardVehicle(*targetVeh)) {
                    if (const auto res = simulation->boardVehicle(ids, *targetVeh); res && *res > 0) {
                        const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                        const int mw = tv.width > 0 ? tv.width : 24;
                        const int mh = tv.height > 0 ? tv.height : 24;
                        const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                        const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Boarding transport ({0} soldiers).", {std::to_string(*res)}),
                                   ss2::NoticeKind::accepted);
                        continue;
                    }
                }
                if ((!ids.empty() || !vids.empty()) && targetVeh && vehicleScene &&
                    vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
                    simulation->canEscortVehicle(ids, vids, *targetVeh)) {
                    if (const auto res = simulation->orderEscortVehicle(ids, vids, *targetVeh); res && *res > 0) {
                        const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                        const int mw = tv.width > 0 ? tv.width : 24;
                        const int mh = tv.height > 0 ? tv.height : 24;
                        const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                        const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Escorting {0} ({1} units).",
                                            {ss2::tr(hudDisplayName(tv.id)), std::to_string(*res)}),
                                   ss2::NoticeKind::accepted);
                        continue;
                    }
                }
                if ((!ids.empty() || !vids.empty()) && targetInf &&
                    scene->units().at(*targetInf).owner == 0 &&
                    simulation->canEscortSoldier(ids, vids, *targetInf)) {
                    if (const auto res = simulation->orderEscortSoldier(ids, vids, *targetInf); res && *res > 0) {
                        const auto& tu = scene->units().at(*targetInf);
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{tu.left * mapScale, tu.top * mapScale, tu.width * mapScale, tu.height * mapScale},
                             ss2::MarkerKind::move},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            *targetInf,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("move");
                        showNotice(ss2::trf("Escorting {0} ({1} units).",
                                            {ss2::tr(hudDisplayName(tu.id)), std::to_string(*res)}),
                                   ss2::NoticeKind::accepted);
                        continue;
                    }
                }
                // The original Tutorial I player table assigns player 1 to the enemy team.
                if ((!ids.empty() || !vids.empty()) && targetInf &&
                    isHostileOwner(scene->units().at(*targetInf).owner) &&
                    simulation->isValidAttackTarget(*targetInf)) {
                    std::optional<size_t> infRes = size_t(0), vehRes = size_t(0);
                    if (!ids.empty()) infRes = simulation->attack(ids, *targetInf);
                    if (infRes && !vids.empty()) vehRes = simulation->attackWithVehicles(vids, false, *targetInf);
                    if (!infRes || !vehRes) {
                        gestureInterrupted = true;
                        std::cout << "Tutorial I attack interrupted by a pending briefing\n";
                        continue;
                    }
                    const auto count = *infRes + *vehRes;
                    const auto total = ids.size() + vids.size();
                    if (count) {
                        auto label = count == 1 && total == 1 ? ss2::tr("Attack order accepted") :
                            ss2::trf("Attack order accepted by {0} of {1} soldiers", {std::to_string(count), std::to_string(total)});
                        if (paused) label = ss2::trf("{0}. Press Pause or Ctrl+P to resume.", {label});
                        showNotice(label, count == total ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
                        const auto& u = scene->units().at(*targetInf);
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{u.left * mapScale, u.top * mapScale, u.width * mapScale, u.height * mapScale},
                             ss2::MarkerKind::attack},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            *targetInf,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("attack");
                        std::cout << "Tutorial I accepted attack on unit " << *targetInf << " at map pixel " << simX << ',' << simY
                                  << " for " << count << '/' << total << (vids.empty() ? " selected infantry\n" : " selected units\n");
                    } else {
                        orderNotice.show(ss2::tr("Selected soldiers cannot engage that target."), ss2::NoticeKind::blocked);
                        std::cout << "Tutorial I rejected attack on unit " << *targetInf << " for " << total
                                  << (vids.empty() ? " selected infantry\n" : " selected units\n");
                    }
                    continue;
                }
                if ((!ids.empty() || !vids.empty()) && targetVeh && vehicleScene &&
                    isHostileOwner(vehicleScene->vehicles().at(*targetVeh).owner) &&
                    simulation->isValidVehicleAttackTarget(*targetVeh)) {
                    std::optional<size_t> infRes = size_t(0), vehRes = size_t(0);
                    if (!ids.empty()) infRes = simulation->attackVehicle(ids, *targetVeh);
                    if (infRes && !vids.empty()) vehRes = simulation->attackWithVehicles(vids, true, *targetVeh);
                    if (!infRes || !vehRes) {
                        gestureInterrupted = true;
                        std::cout << "Tutorial I attack interrupted by a pending briefing\n";
                        continue;
                    }
                    const auto count = *infRes + *vehRes;
                    const auto total = ids.size() + vids.size();
                    if (count) {
                        auto label = count == 1 && total == 1 ? ss2::tr("Attack order accepted") :
                            ss2::trf("Attack order accepted by {0} of {1} soldiers", {std::to_string(count), std::to_string(total)});
                        if (paused) label = ss2::trf("{0}. Press Pause or Ctrl+P to resume.", {label});
                        showNotice(label, count == total ? ss2::NoticeKind::accepted : ss2::NoticeKind::unsupported);
                        const auto& tv = vehicleScene->vehicles().at(*targetVeh);
                        const int mw = tv.width > 0 ? tv.width : 24;
                        const int mh = tv.height > 0 ? tv.height : 24;
                        const int ml = tv.width > 0 ? tv.left : tv.x - 12;
                        const int mt = tv.height > 0 ? tv.top : tv.y - 12;
                        std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                        activeMarkers.push_back({
                            {{ml * mapScale, mt * mapScale, mw * mapScale, mh * mapScale},
                             ss2::MarkerKind::attack},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                            std::nullopt,
                            true
                        });
                        syncMarkers();
                        playSelectionVoice("attack");
                        std::cout << "Accepted attack on vehicle " << *targetVeh << " at map pixel " << simX << ',' << simY
                                  << " for " << count << '/' << total << " selected units\n";
                    } else {
                        orderNotice.show(ss2::tr("Selected soldiers cannot engage that target."), ss2::NoticeKind::blocked);
                    }
                    continue;
                }
                if (!targetInf && !targetVeh) {
                    const auto targetBld = buildingAtMap(simX, simY);
                    if (targetBld) {
                        const auto& lb = liveBuildings[*targetBld];
                        if (!ids.empty() && simulation->canGarrisonBuilding(*targetBld)) {
                            if (const auto res = simulation->garrisonBuilding(ids, *targetBld); res && *res > 0) {
                                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                                activeMarkers.push_back({
                                    {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                                     ss2::MarkerKind::move},
                                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                                    std::nullopt,
                                    true
                                });
                                syncMarkers();
                                playSelectionVoice("move");
                                showNotice(ss2::trf("Garrisoning building ({0} soldiers).", {std::to_string(*res)}),
                                           ss2::NoticeKind::accepted);
                                continue;
                            }
                        }
                        if (!vids.empty() && simulation->canSupportBuilding(vids, *targetBld)) {
                            if (const auto res = simulation->orderSupportBuilding(vids, *targetBld); res && *res > 0) {
                                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                                activeMarkers.push_back({
                                    {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                                     ss2::MarkerKind::move},
                                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                                    std::nullopt,
                                    true
                                });
                                syncMarkers();
                                playSelectionVoice("move");
                                bool hasSupTruck = false, hasAmbulance = false;
                                if (vehicleScene && unitCatalog) {
                                    for (const size_t sv : vids) {
                                        if (sv >= vehicleScene->vehicles().size()) continue;
                                        if (const auto* spec = unitCatalog->find(vehicleScene->vehicles()[sv].id)) {
                                            if (spec->isSupplyTruck()) hasSupTruck = true;
                                            if (spec->isAmbulance()) hasAmbulance = true;
                                        }
                                    }
                                }
                                showNotice(ss2::tr((hasAmbulance && !hasSupTruck)
                                                       ? "Ambulance treating garrisoned troops..."
                                                       : "Supply truck repairing building..."),
                                           ss2::NoticeKind::accepted);
                                continue;
                            }
                        }
                        if ((!ids.empty() || !vids.empty()) && lb.owner >= 0 && isHostileOwner(lb.owner)) {
                            std::optional<size_t> infRes = size_t(0), vehRes = size_t(0);
                            if (!ids.empty()) infRes = simulation->attackBuilding(ids, *targetBld);
                            if (infRes && !vids.empty()) vehRes = simulation->attackBuildingWithVehicles(vids, *targetBld);
                            const size_t count = (infRes ? *infRes : 0) + (vehRes ? *vehRes : 0);
                            if (count > 0) {
                                std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                                activeMarkers.push_back({
                                    {{lb.left * mapScale, lb.top * mapScale, lb.width * mapScale, lb.height * mapScale},
                                     ss2::MarkerKind::attack},
                                    std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                                    std::nullopt,
                                    true
                                });
                                syncMarkers();
                                playSelectionVoice("attack");
                                showNotice(ss2::tr("Attacking occupied building!"), ss2::NoticeKind::accepted);
                                continue;
                            }
                        }
                    }
                    if (!ids.empty() && simulation->canDisarmMineAt(ids, simX, simY)) {
                        if (const auto res = simulation->orderDisarmMine(ids, simX, simY); res && *res > 0) {
                            std::erase_if(activeMarkers, [](const TimedMarker& m) { return m.isOrder; });
                            activeMarkers.push_back({
                                {{(simX - 8) * mapScale, (simY - 8) * mapScale, 16 * mapScale, 16 * mapScale},
                                 ss2::MarkerKind::move},
                                std::chrono::steady_clock::now() + std::chrono::milliseconds(700),
                                std::nullopt,
                                true
                            });
                            syncMarkers();
                            playSelectionVoice("move");
                            showNotice(ss2::trf("Sapper clearing mines ({0} soldiers)...", {std::to_string(*res)}),
                                       ss2::NoticeKind::accepted);
                            continue;
                        }
                    }
                }
                const bool append = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                (void)issueGroundOrder(simX, simY, false, append);
            }
            if (simulation && event.type == SDL_EVENT_MOUSE_MOTION && !event.motion.state) {
                const auto [x,y] = renderer.mapPosition(event.motion.x,event.motion.y);
                const int simX = int(std::floor(x / mapScale)), simY = int(std::floor(y / mapScale));
                const auto& picked = selection->selected();
                const auto& pickedVeh = selection->selectedVehicles();
                const std::vector<size_t> ids(picked.begin(), picked.end());
                const std::vector<size_t> pvids(pickedVeh.begin(), pickedVeh.end());
                const auto targetInf = selection->infantryAt(int(std::floor(x)), int(std::floor(y)));
                const auto targetVeh = selection->vehicleAt(int(std::floor(x)), int(std::floor(y)));
                const auto hoverBld = (!targetInf && !targetVeh) ? buildingAtMap(simX, simY) : std::nullopt;
                const bool hasAny = !ids.empty() || !pickedVeh.empty();
                const bool hasGarrisonCmd = !hasAny && selectedBuilding &&
                                            *selectedBuilding < liveBuildings.size() &&
                                            !liveBuildings[*selectedBuilding].destroyed &&
                                            liveBuildings[*selectedBuilding].owner == 0 &&
                                            liveBuildings[*selectedBuilding].occupantCount > 0;
                const bool hasCmd = hasAny || hasGarrisonCmd;
                const bool validInfTarget = hasCmd && targetInf && isHostileOwner(scene->units().at(*targetInf).owner) &&
                                            simulation->isValidAttackTarget(*targetInf);
                const bool validVehTarget = hasCmd && targetVeh && vehicleScene &&
                                            isHostileOwner(vehicleScene->vehicles().at(*targetVeh).owner) &&
                                            simulation->isValidVehicleAttackTarget(*targetVeh);
                const bool validBldTarget = hasCmd && hoverBld &&
                                            (!selectedBuilding || *hoverBld != *selectedBuilding) &&
                                            liveBuildings[*hoverBld].owner >= 0 &&
                                            isHostileOwner(liveBuildings[*hoverBld].owner);
                bool canTowHover = false;
                if (!pickedVeh.empty() && targetVeh && vehicleScene) {
                    for (size_t sv : pickedVeh) {
                        if (simulation->canTowVehicle(sv, *targetVeh) || simulation->canTowVehicle(*targetVeh, sv)) {
                            canTowHover = true;
                            break;
                        }
                    }
                }
                const bool canGarrisonHover = !ids.empty() && hoverBld && simulation->canGarrisonBuilding(*hoverBld);
                const bool canSupportHover =
                    (!pvids.empty() && targetVeh && vehicleScene &&
                     vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
                     simulation->canSupportVehicle(pvids, *targetVeh)) ||
                    (!pvids.empty() && hoverBld && simulation->canSupportBuilding(pvids, *hoverBld)) ||
                    (hasAny && targetInf && scene->units().at(*targetInf).owner == 0 &&
                     simulation->canHealSoldier(ids, pvids, *targetInf));
                const bool canEscortHover =
                    (hasAny && targetVeh && vehicleScene &&
                     vehicleScene->vehicles().at(*targetVeh).owner == 0 &&
                     simulation->canEscortVehicle(ids, pvids, *targetVeh)) ||
                    (hasAny && targetInf && scene->units().at(*targetInf).owner == 0 &&
                     simulation->canEscortSoldier(ids, pvids, *targetInf));
                const bool canDisarmHover =
                    !ids.empty() && !targetInf && !targetVeh && simulation->canDisarmMineAt(ids, simX, simY);
                if (armedAirSupport) {
                    const bool strike = *armedAirSupport == ss2::assets::AirMissionKind::Bomber ||
                                        *armedAirSupport == ss2::assets::AirMissionKind::Fighter;
                    SDL_SetCursor(strike ? (attackCursor ? attackCursor.get() : SDL_GetDefaultCursor())
                                         : (moveCursor ? moveCursor.get() : SDL_GetDefaultCursor()));
                } else if (attackMoveArmed || attackGroundArmed || patrolArmed || validInfTarget || validVehTarget || validBldTarget) {
                    SDL_SetCursor(attackCursor ? attackCursor.get() : SDL_GetDefaultCursor());
                } else if (reverseMoveArmed || faceArmed || canSupportHover || canEscortHover || canDisarmHover) {
                    SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                } else if (canTowHover || canGarrisonHover ||
                           (!ids.empty() && targetVeh && vehicleScene &&
                            ((vehicleScene->vehicles().at(*targetVeh).owner == 0 && simulation->canBoardVehicle(*targetVeh)) ||
                             (vehicleScene->vehicles().at(*targetVeh).owner == 3 && simulation->canCaptureVehicle(*targetVeh))))) {
                    SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                } else if ((!ids.empty() && simulation->canMove(ids, simX, simY)) || !pickedVeh.empty()) {
                    SDL_SetCursor(moveCursor ? moveCursor.get() : SDL_GetDefaultCursor());
                } else {
                    SDL_SetCursor(SDL_GetDefaultCursor());
                }
            }
            if (selection) {
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                    (event.key.mod & SDL_KMOD_CTRL) && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) {
                    const auto grpIdx = static_cast<std::size_t>(event.key.key - SDLK_0);
                    hudGroups[grpIdx] = selection->selected();
                    hudVehicleGroups[grpIdx] = selection->selectedVehicles();
                }
                selection->handle(event);
                if (!selection->selected().empty() || !selection->selectedVehicles().empty()) {
                    selectedBuilding.reset();
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
                           !liveBuildings.empty()) {
                    const auto [bx, by] = renderer.mapPosition(event.button.x, event.button.y);
                    const auto clickedBld = buildingAtMap(int(std::floor(bx / mapScale)), int(std::floor(by / mapScale)));
                    if (clickedBld && liveBuildings[*clickedBld].owner == 0 && liveBuildings[*clickedBld].occupantCount > 0) {
                        selectedBuilding = *clickedBld;
                        const auto& lb = liveBuildings[*clickedBld];
                        showNotice(ss2::trf("Garrisoned building ({0}/{1}) — press U to evacuate.",
                                            {std::to_string(lb.occupantCount), std::to_string(lb.capacity)}),
                                   ss2::NoticeKind::accepted);
                    } else {
                        selectedBuilding.reset();
                    }
                }
                if (selection->selected() != prevSelInf || selection->selectedVehicles() != prevSelVeh) {
                    const bool grewOrChanged = !selection->selected().empty() || !selection->selectedVehicles().empty();
                    prevSelInf = selection->selected();
                    prevSelVeh = selection->selectedVehicles();
                    if (grewOrChanged &&
                        (event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                         (event.type == SDL_EVENT_KEY_DOWN &&
                          (event.key.key == SDLK_TAB || event.key.key == SDLK_V ||
                           (event.key.key >= SDLK_0 && event.key.key <= SDLK_9 && !(event.key.mod & SDL_KMOD_CTRL)))))) {
                        playSelectionVoice("select");
                    }
                    if (!grewOrChanged && (attackMoveArmed || attackGroundArmed || reverseMoveArmed || patrolArmed || faceArmed)) {
                        attackMoveArmed = false;
                        attackGroundArmed = false;
                        reverseMoveArmed = false;
                        patrolArmed = false;
                        faceArmed = false;
                        resetCursor();
                    }
                }
            }
            if (selection && event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_0 && event.key.key <= SDLK_9) continue;
            if (event.type == SDL_EVENT_KEY_DOWN &&
                (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT ||
                 event.key.key == SDLK_UP || event.key.key == SDLK_DOWN)) continue;
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
            if (simulation) {
                const unsigned secs = simulation->simTick() / 25u;
                const unsigned mins = secs / 60u;
                const unsigned remS = secs % 60u;
                std::string prefix = "[" + (mins < 10u ? std::string("0") : std::string{}) + std::to_string(mins) +
                                     ":" + (remS < 10u ? std::string("0") : std::string{}) + std::to_string(remS);
                if (paused) {
                    prefix += " PAUSED";
                } else if (const int sp = simulation->speedPercent(); sp != 100) {
                    prefix += (sp == 50 ? " 0.5x" : sp == 200 ? " 2x" : sp == 300 ? " 3x" : " 1x");
                }
                prefix += "] ";
                hs.objective = prefix + ss2::tr(currentObjective);
            } else {
                hs.objective = ss2::tr(currentObjective);   // mission text has no entry and passes through
            }
            hs.losses = static_cast<unsigned>(selection->playerLosses());
            hs.paused = paused;
            if (simulation) {
                const auto slots = simulation->playerAirSupport();
                for (std::size_t k = 0; k < 5u; ++k) hs.airReady[k] = std::max(0, slots[k].ready);
            }
            hs.armedAir = armedAirSupport ? static_cast<int>(*armedAirSupport) : -1;
            hs.planes.reserve(livePlanes.size());
            for (const auto& [sid, lp] : livePlanes) {
                (void)sid;
                const float pu = std::clamp(static_cast<float>(lp.pose.x) / static_cast<float>(hudMapW), 0.0f, 1.0f);
                const float pv = std::clamp(static_cast<float>(lp.pose.y) / static_cast<float>(hudMapH), 0.0f, 1.0f);
                hs.planes.push_back({pu, pv, !isAllyOwner(lp.pose.owner)});
            }
            if (objectiveMarkerSim && hudMapW && hudMapH) {
                hs.hasObjectiveMarker = true;
                hs.objectiveU = std::clamp(static_cast<float>(objectiveMarkerSim->first) / static_cast<float>(hudMapW),
                                           0.0f, 1.0f);
                hs.objectiveV = std::clamp(static_cast<float>(objectiveMarkerSim->second) / static_cast<float>(hudMapH),
                                           0.0f, 1.0f);
            }
            struct Agg { unsigned count = 0; float hpSum = 0.0f; float ammoSum = 0.0f; };
            std::map<std::string, Agg> byClass;
            const auto& units = scene->units();
            const std::vector<ss2::assets::VehicleUnit> noVehicles;
            const auto& vehUnits = vehicleScene ? vehicleScene->vehicles() : noVehicles;
            for (unsigned d = 1; d <= 10; ++d) {
                const unsigned digit = d % 10u;
                unsigned aliveInGrp = 0;
                for (const auto id : hudGroups[digit]) {
                    if (id < units.size() && units[id].hp > 0) ++aliveInGrp;
                }
                for (const auto id : hudVehicleGroups[digit]) {
                    if (id < vehUnits.size() && vehUnits[id].alive && vehUnits[id].hp > 0) ++aliveInGrp;
                }
                if (aliveInGrp > 0) hs.groups.push_back({digit, aliveInGrp});
            }
            unsigned selCount = 0;
            float selHpSum = 0.0f, selAmmoSum = 0.0f, selReadySum = 0.0f, selExpSum = 0.0f, selMoraleSum = 0.0f;
            for (const auto id : selection->selected()) {
                if (id >= units.size()) continue;
                const auto& u = units[id];
                if (u.hp <= 0) continue;
                const int maxHp = (id < initialUnitHp.size() && initialUnitHp[id] > 0) ? initialUnitHp[id] : 100;
                const auto* spec = unitCatalog ? unitCatalog->find(u.id) : nullptr;
                const int maxInfAmmo = (spec && !spec->isVehicle && spec->ammo1 > 0) ? spec->ammo1 : 40;
                const float hpFrac = std::clamp(static_cast<float>(u.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
                const float ammoFrac = std::clamp(static_cast<float>(u.ammo) / static_cast<float>(maxInfAmmo), 0.0f, 1.0f);
                auto& a = byClass[hudDisplayName(u.id)];
                ++a.count;
                a.hpSum += hpFrac;
                a.ammoSum += ammoFrac;
                ++selCount;
                selHpSum += hpFrac;
                selAmmoSum += ammoFrac;
                const bool suppressed = suppressedUnits.contains(id);
                selReadySum += holdFireUnits.contains(id) ? 0.35f
                    : (suppressed ? 0.65f : (crouchedUnits.contains(id) ? 0.85f : 1.0f));
                const float expFrac = (id < unitExpa.size())
                    ? std::clamp(static_cast<float>(unitExpa[id]) / 32768.0f, 0.0f, 1.0f) : 0.0f;
                selExpSum += expFrac;
                float unitMorale = std::clamp(0.45f + 0.55f * hpFrac + 0.15f * expFrac, 0.0f, 1.0f);
                if (suppressed) unitMorale *= 0.45f;
                selMoraleSum += unitMorale;
                if (crouchedUnits.contains(id)) hs.crouched = true;
                if (holdFireUnits.contains(id)) hs.holdFire = true;
                if (binocularUnits.contains(id)) hs.binoculars = true;
                std::string lowId = u.id;
                for (char& ch : lowId) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lowId.find("sapper") != std::string::npos || lowId.find("miner") != std::string::npos ||
                    (spec && (spec->selType == "sapper" || spec->selType == "sappers")) ||
                    (id < unitMineAmmo.size() && unitMineAmmo[id] > 0)) {
                    hs.hasSapper = true;
                }
                if (isBombardInfantryId(u.id)) {
                    hs.hasBombard = true;
                }
            }
            for (const auto id : selection->selectedVehicles()) {
                if (id >= vehUnits.size()) continue;
                const auto& v = vehUnits[id];
                if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
                const int maxHp = (id < initialVehicleHp.size() && initialVehicleHp[id] > 0) ? initialVehicleHp[id] : 100;
                const auto* vSpec = unitCatalog ? unitCatalog->find(v.id) : nullptr;
                const bool vArmed = !vSpec || (vSpec->shot1.damage > 0 && vSpec->shot1.range > 0) ||
                                    (vSpec->shot2.damage > 0 && vSpec->shot2.range > 0);
                const int maxVehAmmo = (vSpec && vSpec->ammo1 > 0) ? vSpec->ammo1 : 100;
                const float hpFrac = std::clamp(static_cast<float>(v.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
                float ammoFrac = 1.0f;
                if (vSpec && vSpec->isSupplyTruck() && (vSpec->ammo1 > 0 || vSpec->ammo2 > 0)) {
                    const int a2 = (id < vehicleAmmo2.size()) ? vehicleAmmo2[id] : 0;
                    const int maxTotal = std::max(1, vSpec->ammo1 + vSpec->ammo2);
                    ammoFrac = std::clamp(static_cast<float>(v.ammo + a2) / static_cast<float>(maxTotal), 0.0f, 1.0f);
                } else if (vArmed) {
                    ammoFrac = std::clamp(static_cast<float>(v.ammo) / static_cast<float>(maxVehAmmo), 0.0f, 1.0f);
                }
                auto& a = byClass[hudDisplayName(v.id)];
                ++a.count;
                a.hpSum += hpFrac;
                a.ammoSum += ammoFrac;
                ++selCount;
                selHpSum += hpFrac;
                selAmmoSum += ammoFrac;
                const bool impaired = id < vehicleCrewImpaired.size() && vehicleCrewImpaired[id];
                const bool moveImp = id < vehicleMoveImpaired.size() && vehicleMoveImpaired[id];
                const bool vehSup = id < vehicleSuppressed.size() && vehicleSuppressed[id];
                selReadySum += vehicleHoldFireUnits.contains(id) ? 0.35f
                    : ((impaired || moveImp) ? 0.50f : (vehSup ? 0.65f : 1.0f));
                const float expFrac = (id < vehExpa.size())
                    ? std::clamp(static_cast<float>(vehExpa[id]) / 32768.0f, 0.0f, 1.0f) : 0.0f;
                selExpSum += expFrac;
                float vehMorale = std::clamp(0.45f + 0.55f * hpFrac + 0.15f * expFrac, 0.0f, 1.0f);
                if (impaired) vehMorale *= 0.65f;
                if (vehSup) vehMorale *= 0.50f;
                selMoraleSum += vehMorale;
                if (vehicleHoldFireUnits.contains(id)) hs.holdFire = true;
                if (id < vehicleCargo.size() && vehicleCargo[id].first > 0) hs.hasCargo = true;
                if (id < vehicleTowed.size() && vehicleTowed[id] >= 0) hs.hasCargo = true;
                if (!vSpec || vSpec->shot1.damage > 0 || vSpec->shot2.damage > 0) hs.hasBombard = true;
            }
            if (selectedBuilding && *selectedBuilding < liveBuildings.size()) {
                const auto& lb = liveBuildings[*selectedBuilding];
                if (!lb.destroyed && lb.owner == 0 && lb.occupantCount > 0) hs.hasCargo = true;
            }
            hs.attackMoveArmed = attackMoveArmed;
            hs.patrolArmed = patrolArmed;
            hs.attackGroundArmed = attackGroundArmed;
            hs.hasInfantry = !selection->selected().empty();
            if (selCount > 0) {
                const float invSel = 1.0f / static_cast<float>(selCount);
                const float avgHp = std::round(selHpSum * invSel * 100.0f) / 100.0f;
                const float avgAmmo = std::round(selAmmoSum * invSel * 100.0f) / 100.0f;
                const float avgReady = std::round(selReadySum * invSel * 100.0f) / 100.0f;
                const float avgMorale = std::round(std::clamp(selMoraleSum * invSel, 0.0f, 1.0f) * 100.0f) / 100.0f;
                const float avgExp = std::round(std::clamp(selExpSum * invSel, 0.0f, 1.0f) * 100.0f) / 100.0f;
                hs.hasSelection = true;
                hs.meters = {avgExp, avgHp, avgMorale, avgAmmo, avgReady};
            }
            for (const auto& [label, a] : byClass) {
                const float avgHp = std::round((a.hpSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
                const float avgAmmo = std::round((a.ammoSum / static_cast<float>(a.count)) * 100.0f) / 100.0f;
                hs.selection.push_back({label, a.count, avgHp, avgAmmo});
            }
            hs.dots.reserve(units.size() + vehUnits.size());
            std::vector<ss2::MapBar> unitBars;
            unitBars.reserve(selection->selected().size() + selection->selectedVehicles().size() + 16);
            const float invW = 1.0f / static_cast<float>(hudMapW);
            const float invH = 1.0f / static_cast<float>(hudMapH);
            for (std::size_t idx = 0; idx < units.size(); ++idx) {
                const auto& u = units[idx];
                if (u.hp <= 0 || !u.visible) continue;
                const float du = std::round(std::clamp(static_cast<float>(u.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
                const float dv = std::round(std::clamp(static_cast<float>(u.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
                hs.dots.push_back({du, dv, isHostileOwner(u.owner), isAllyOwner(u.owner) && u.owner != 0, u.owner == 3});
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
            for (std::size_t idx = 0; idx < vehUnits.size(); ++idx) {
                const auto& v = vehUnits[idx];
                if (!v.alive || v.hp <= 0 || !v.visible) continue;
                const float du = std::round(std::clamp(static_cast<float>(v.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
                const float dv = std::round(std::clamp(static_cast<float>(v.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
                hs.dots.push_back({du, dv, isHostileOwner(v.owner), isAllyOwner(v.owner) && v.owner != 0, v.owner == 3});
                const int maxHp = (idx < initialVehicleHp.size()) ? initialVehicleHp[idx] : 100;
                if (selection->selectedVehicles().contains(idx) || v.hp < maxHp) {
                    const float barW = 28.0f * static_cast<float>(mapScale);
                    const float barY = static_cast<float>((v.height > 0 ? v.top - 5 : v.y - 26) * mapScale);
                    unitBars.push_back({
                        static_cast<float>(v.x * mapScale),
                        barY,
                        barW,
                        std::clamp(static_cast<float>(v.hp) / static_cast<float>(maxHp), 0.0f, 1.0f)
                    });
                }
            }
            for (std::size_t bi = 0; bi < liveBuildings.size(); ++bi) {
                const auto& b = liveBuildings[bi];
                if (b.destroyed || (b.occupantCount == 0 && b.hp >= b.maxHp)) continue;
                if (b.owner != 0 && !isAllyOwner(b.owner) && !isBuildingPointVisible(b.x, b.y)) continue;
                if (b.occupantCount > 0 && b.owner >= 0) {
                    const float du = std::round(std::clamp(static_cast<float>(b.x) * invW, 0.0f, 1.0f) * 256.0f) / 256.0f;
                    const float dv = std::round(std::clamp(static_cast<float>(b.y) * invH, 0.0f, 1.0f) * 128.0f) / 128.0f;
                    hs.dots.push_back({du, dv, isHostileOwner(b.owner), isAllyOwner(b.owner) && b.owner != 0, false});
                }
                const float barW = static_cast<float>(std::clamp(b.width / 2, 24, 52) * mapScale);
                const float barY = static_cast<float>((b.top - 5) * mapScale);
                unitBars.push_back({
                    static_cast<float>(b.x * mapScale),
                    barY,
                    barW,
                    std::clamp(static_cast<float>(b.hp) / static_cast<float>(std::max(1, b.maxHp)), 0.0f, 1.0f)
                });
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
            int winW = 1, winH = 1, pixelH = 1;
            if (!SDL_GetWindowSize(window, &winW, &winH)) { winW = 1; winH = 1; }
            if (!SDL_GetWindowSizeInPixels(window, nullptr, &pixelH)) pixelH = winH;
            const double mapViewH = ss2::hidpi::logicalMapViewHeight(winH, pixelH, static_cast<int>(hud->height()));
            const auto [mx0, my0] = renderer.mapPosition(0.0f, 0.0f);
            const auto [mx1, my1] = renderer.mapPosition(static_cast<float>(winW), static_cast<float>(mapViewH));
            const float totalW = static_cast<float>(hudMapW * mapScale);
            const float totalH = static_cast<float>(hudMapH * mapScale);
            hs.viewU = std::round(std::clamp(static_cast<float>(mx0) / totalW, 0.0f, 1.0f) * 256.0f) / 256.0f;
            hs.viewV = std::round(std::clamp(static_cast<float>(my0) / totalH, 0.0f, 1.0f) * 256.0f) / 256.0f;
            hs.viewW = std::round(std::clamp(static_cast<float>(mx1 - mx0) / totalW, 0.01f, 1.0f) * 256.0f) / 256.0f;
            hs.viewH = std::round(std::clamp(static_cast<float>(my1 - my0) / totalH, 0.01f, 1.0f) * 256.0f) / 256.0f;
            if (!minimapPings.empty()) {
                const auto pingNow = std::chrono::steady_clock::now();
                std::erase_if(minimapPings, [&](const MinimapPing& mp) {
                    return pingNow - mp.start >= std::chrono::milliseconds(1800);
                });
                hs.pings.reserve(minimapPings.size());
                for (const auto& mp : minimapPings) {
                    const float age = std::chrono::duration<float>(pingNow - mp.start).count();
                    const float q = std::round(std::clamp(1.0f - age / 1.8f, 0.0f, 1.0f) * 8.0f) / 8.0f;
                    if (q > 0.0f) hs.pings.push_back({mp.u, mp.v, mp.hostile, q});
                }
            }
            hud->update(hs);
            hud->draw(renderer);
        }
        std::vector<std::size_t> crippledSmokeVehicles;
        if (combatEffects && vehicleScene && unitCatalog) {
            const auto& vehs = vehicleScene->vehicles();
            for (std::size_t vi = 0; vi < vehs.size() && crippledSmokeVehicles.size() < 8u; ++vi) {
                const auto& v = vehs[vi];
                if (!v.alive || v.hp <= 0 || !v.visible) continue;
                if (const auto* spec = unitCatalog->find(v.id); spec && spec->moveDamageHp > 0 && v.hp < spec->moveDamageHp) {
                    crippledSmokeVehicles.push_back(vi);
                }
            }
        }
        std::vector<ss2::TacticalOrderLine> orderLines;
        if (simulation && selection && (!selection->selected().empty() || !selection->selectedVehicles().empty())) {
            const auto& sel = selection->selected();
            const auto& vsel = selection->selectedVehicles();
            const std::vector<size_t> sids(sel.begin(), sel.end());
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            orderLines = simulation->selectionOrderLines(sids, vids);
        }
        std::vector<ss2::VehicleRangePreview> rangePreviews;
        if (attackGroundArmed && simulation && selection &&
            (!selection->selectedVehicles().empty() || !selection->selected().empty())) {
            const auto& vsel = selection->selectedVehicles();
            const auto& sel = selection->selected();
            const std::vector<size_t> vids(vsel.begin(), vsel.end());
            const std::vector<size_t> sids(sel.begin(), sel.end());
            rangePreviews = simulation->selectionRangePreviews(vids, sids);
            if (rangePreviews.size() > 3u) rangePreviews.resize(3u);
        }
        bool previewRing = false;
        float ringX = 0.0f, ringY = 0.0f, ringRad = 0.0f;
        std::uint8_t ringR = 244, ringG = 186, ringB = 78;
        if ((attackGroundArmed || armedAirSupport.has_value()) && SDL_GetMouseFocus() == window &&
            !briefing.active() && !menu.active()) {
            float mx{}, my{};
            SDL_GetMouseState(&mx, &my);
            if (!hud || !hud->opaqueAtWindow(mx, my)) {
                const auto [mapX, mapY] = renderer.mapPosition(mx, my);
                previewRing = true;
                ringX = static_cast<float>(mapX);
                ringY = static_cast<float>(mapY);
                if (armedAirSupport) {
                    switch (*armedAirSupport) {
                    case ss2::assets::AirMissionKind::Bomber:
                        ringRad = 96.0f; ringR = 235; ringG = 92; ringB = 68;
                        break;
                    case ss2::assets::AirMissionKind::Fighter:
                        ringRad = 64.0f; ringR = 235; ringG = 92; ringB = 68;
                        break;
                    case ss2::assets::AirMissionKind::Recon:
                        ringRad = 110.0f; ringR = 105; ringG = 212; ringB = 235;
                        break;
                    case ss2::assets::AirMissionKind::Paratrooper:
                    case ss2::assets::AirMissionKind::Supply:
                        ringRad = 72.0f; ringR = 105; ringG = 212; ringB = 235;
                        break;
                    }
                } else {
                    ringRad = 56.0f; ringR = 244; ringG = 186; ringB = 78;
                    if (!rangePreviews.empty()) {
                        const float sScale = static_cast<float>(std::max(1, mapScale));
                        const double simCx = static_cast<double>(ringX / sScale);
                        const double simCy = static_cast<double>(ringY / sScale);
                        bool inRange = false;
                        for (const auto& rp : rangePreviews) {
                            const double dist = std::hypot(simCx - static_cast<double>(rp.x),
                                                           simCy - static_cast<double>(rp.y));
                            if (dist >= static_cast<double>(rp.deadzonePx) &&
                                dist <= static_cast<double>(rp.rangePx)) {
                                inRange = true;
                                break;
                            }
                        }
                        if (!inRange) {
                            ringR = 235; ringG = 92; ringB = 68;
                        }
                    }
                }
            }
        }
        bool facePreview = false;
        float faceTargetX = 0.0f, faceTargetY = 0.0f;
        if (faceArmed && vehicleScene && selection && !selection->selectedVehicles().empty() &&
            SDL_GetMouseFocus() == window && !briefing.active() && !menu.active()) {
            float mx{}, my{};
            SDL_GetMouseState(&mx, &my);
            if (!hud || !hud->opaqueAtWindow(mx, my)) {
                const auto [mapX, mapY] = renderer.mapPosition(mx, my);
                facePreview = true;
                faceTargetX = static_cast<float>(mapX);
                faceTargetY = static_cast<float>(mapY);
            }
        }
        int viewWinW = 1, viewWinH = 1;
        SDL_GetWindowSize(window, &viewWinW, &viewWinH);
        const auto [vx0, vy0] = renderer.mapPosition(0.0f, 0.0f);
        const auto [vx1, vy1] = renderer.mapPosition(static_cast<float>(viewWinW), static_cast<float>(viewWinH));
        const float viewMargin = 64.0f * static_cast<float>(mapScale);
        const float minViewX = static_cast<float>(std::min(vx0, vx1)) - viewMargin;
        const float maxViewX = static_cast<float>(std::max(vx0, vx1)) + viewMargin;
        const float minViewY = static_cast<float>(std::min(vy0, vy1)) - viewMargin;
        const float maxViewY = static_cast<float>(std::max(vy0, vy1)) + viewMargin;
        const auto inViewBounds = [&](float wx, float wy) {
            return wx >= minViewX && wx <= maxViewX && wy >= minViewY && wy <= maxViewY;
        };

        std::vector<ss2::VisibleMine> visibleOnScreenMines;
        for (const auto& vm : liveVisibleMines) {
            const float wx = static_cast<float>(vm.x * mapScale);
            const float wy = static_cast<float>(vm.y * mapScale);
            if (inViewBounds(wx, wy)) {
                visibleOnScreenMines.push_back(vm);
                if (visibleOnScreenMines.size() >= 48u) break;
            }
        }
        struct UnitStatusBadge {
            float x{}, y{};
            bool outOfAmmo{false};
            int stars{0};
        };
        std::vector<UnitStatusBadge> statusBadges;
        if (scene) {
            const float sScale = static_cast<float>(mapScale);
            const auto& units = scene->units();
            for (std::size_t idx = 0; idx < units.size() && statusBadges.size() < 24u; ++idx) {
                const auto& u = units[idx];
                if (u.owner != 0 || u.hp <= 0 || !u.visible) continue;
                const float wx = static_cast<float>(u.x) * sScale;
                const float wy = static_cast<float>(u.y) * sScale;
                if (!inViewBounds(wx, wy)) continue;
                const bool dry = (u.ammo <= 0) && isCombatAmmoSoldierId(u.id);
                const int expVal = (idx < unitExpa.size()) ? unitExpa[idx] : 0;
                const int stars = expVal >= 24000 ? 2 : (expVal >= 12000 ? 1 : 0);
                if (dry || stars > 0) {
                    statusBadges.push_back({wx, wy - 34.0f * sScale, dry, stars});
                }
            }
            if (vehicleScene) {
                const auto& vehs = vehicleScene->vehicles();
                for (std::size_t idx = 0; idx < vehs.size() && statusBadges.size() < 24u; ++idx) {
                    const auto& v = vehs[idx];
                    if (v.owner != 0 || !v.alive || v.hp <= 0 || !v.visible) continue;
                    const float wx = static_cast<float>(v.x) * sScale;
                    const float wy = static_cast<float>(v.y) * sScale;
                    if (!inViewBounds(wx, wy)) continue;
                    const bool dry = (v.ammo <= 0) && isArmedVehicleId(v.id);
                    const int expVal = (idx < vehExpa.size()) ? vehExpa[idx] : 0;
                    const int stars = expVal >= 24000 ? 2 : (expVal >= 12000 ? 1 : 0);
                    if (dry || stars > 0) {
                        statusBadges.push_back({wx, wy - 32.0f * sScale, dry, stars});
                    }
                }
            }
        }
        if (!activeVfx.empty() || !burningWrecks.empty() || !crippledSmokeVehicles.empty() ||
            !movingSmokeVehicles.empty() || !livePlanes.empty() || !orderLines.empty() ||
            previewRing || facePreview || !rangePreviews.empty() || !visibleOnScreenMines.empty() ||
            !statusBadges.empty() || vfxWasNonEmpty) {
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(activeVfx, [&](const ActiveVfx& v) { return now - v.start >= v.duration; });
            std::erase_if(burningWrecks, [&](const BurningWreck& w) { return now >= w.until; });
            std::vector<ss2::MapBeam> beams;
            beams.reserve(visibleOnScreenMines.size() * 2u + statusBadges.size() * 3u + orderLines.size() * 2u +
                          (previewRing ? 18u : 0u) + (facePreview ? 8u : 0u) + rangePreviews.size() * 40u +
                          activeVfx.size() * 3u + burningWrecks.size() * 2u + crippledSmokeVehicles.size() +
                          std::min<std::size_t>(movingSmokeVehicles.size(), 6u));
            const float s = static_cast<float>(mapScale);
            const auto pushIsoEllipse = [&](float cx, float cy, float radiusSimPx, int segments, float lineW,
                                            std::uint8_t r, std::uint8_t g, std::uint8_t b, float alpha) {
                constexpr float kTwoPi = 6.28318530717958647692f;
                const float rx = radiusSimPx * s;
                const float ry = radiusSimPx * 0.5f * s;
                for (int seg = 0; seg < segments; ++seg) {
                    const float a0 = (static_cast<float>(seg) / static_cast<float>(segments)) * kTwoPi;
                    const float a1 = (static_cast<float>(seg + 1) / static_cast<float>(segments)) * kTwoPi;
                    beams.push_back({cx + std::cos(a0) * rx, cy + std::sin(a0) * ry,
                                     cx + std::cos(a1) * rx, cy + std::sin(a1) * ry,
                                     lineW, r, g, b, alpha, alpha});
                }
            };
            for (const auto& rp : rangePreviews) {
                const float vx = static_cast<float>(rp.x) * s;
                const float vy = static_cast<float>(rp.y) * s;
                pushIsoEllipse(vx, vy, rp.rangePx, 24, 1.5f * s, 244, 186, 78, 0.34f);
                if (rp.deadzonePx > 12.0f) {
                    pushIsoEllipse(vx, vy, rp.deadzonePx, 16, 1.5f * s, 235, 92, 68, 0.42f);
                }
            }
            for (const auto& vm : visibleOnScreenMines) {
                const float mx = static_cast<float>(vm.x) * s;
                const float my = static_cast<float>(vm.y) * s;
                if (vm.hostile) {
                    beams.push_back({mx, my, mx, my, (vm.antiTank ? 8.0f : 6.0f) * s, 235, 92, 68, 0.88f, 0.88f});
                    beams.push_back({mx - 5.0f * s, my, mx + 5.0f * s, my, 2.0f * s, 235, 92, 68, 0.85f, 0.85f});
                } else {
                    beams.push_back({mx, my, mx, my, (vm.antiTank ? 7.0f : 5.5f) * s, 105, 212, 235, 0.78f, 0.78f});
                }
            }
            for (const auto& ol : orderLines) {
                if (beams.size() >= 112u) break;
                std::uint8_t r = 110, g = 224, b = 120;
                float alpha = 0.45f, width = 1.8f * s, pipW = 4.5f * s;
                switch (ol.kind) {
                case ss2::TacticalOrderLine::Kind::Move:
                    break;
                case ss2::TacticalOrderLine::Kind::AttackMove:
                case ss2::TacticalOrderLine::Kind::Attack:
                    r = 235; g = 92; b = 68; alpha = 0.55f; width = 2.0f * s; pipW = 5.2f * s;
                    break;
                case ss2::TacticalOrderLine::Kind::Patrol:
                    r = 105; g = 212; b = 235; alpha = 0.55f; width = 2.0f * s; pipW = 5.0f * s;
                    break;
                case ss2::TacticalOrderLine::Kind::Support:
                    r = 105; g = 212; b = 235; alpha = 0.58f; width = 2.0f * s; pipW = 5.2f * s;
                    break;
                case ss2::TacticalOrderLine::Kind::AttackGround:
                    r = 244; g = 186; b = 78; alpha = 0.58f; width = 2.2f * s; pipW = 5.6f * s;
                    break;
                }
                const float x0 = static_cast<float>(ol.x0) * s;
                const float y0 = static_cast<float>(ol.y0) * s;
                const float x1 = static_cast<float>(ol.x1) * s;
                const float y1 = static_cast<float>(ol.y1) * s;
                beams.push_back({x0, y0, x1, y1, width, r, g, b, alpha * 0.65f, alpha});
                beams.push_back({x1, y1, x1, y1, pipW, r, g, b, alpha, alpha});
            }
            if (facePreview && vehicleScene && selection) {
                const auto& vehs = vehicleScene->vehicles();
                std::size_t rays = 0;
                for (const size_t vid : selection->selectedVehicles()) {
                    if (rays >= 6u) break;
                    if (vid >= vehs.size()) continue;
                    const auto& v = vehs[vid];
                    if (!v.alive || v.hp <= 0 || v.owner != 0) continue;
                    const float vx = static_cast<float>(v.x) * s;
                    const float vy = static_cast<float>(v.y) * s;
                    beams.push_back({vx, vy, faceTargetX, faceTargetY, 2.0f * s, 105, 212, 235, 0.45f, 0.72f});
                    ++rays;
                }
                if (rays > 0) {
                    beams.push_back({faceTargetX, faceTargetY, faceTargetX, faceTargetY,
                                     6.0f * s, 105, 212, 235, 0.78f, 0.78f});
                }
            }
            if (!statusBadges.empty()) {
                const float badgeAnim = std::chrono::duration<float>(now - animStart).count();
                const float dryAlpha = 0.45f + 0.45f * (0.5f + 0.5f * std::sin(badgeAnim * 6.0f));
                for (const auto& sb : statusBadges) {
                    if (beams.size() >= 112u) break;
                    if (sb.outOfAmmo) {
                        beams.push_back({sb.x - 5.0f * s, sb.y, sb.x + 5.0f * s, sb.y,
                                         3.2f * s, 244, 186, 78, dryAlpha, dryAlpha});
                    }
                    const float starY = sb.y - (sb.outOfAmmo ? 6.0f * s : 0.0f);
                    if (sb.stars == 1) {
                        beams.push_back({sb.x, starY, sb.x, starY, 4.2f * s, 244, 186, 78, 0.90f, 0.90f});
                    } else if (sb.stars >= 2) {
                        beams.push_back({sb.x - 3.5f * s, starY, sb.x - 3.5f * s, starY,
                                         4.2f * s, 244, 186, 78, 0.95f, 0.95f});
                        beams.push_back({sb.x + 3.5f * s, starY, sb.x + 3.5f * s, starY,
                                         4.2f * s, 244, 186, 78, 0.95f, 0.95f});
                    }
                }
            }
            if (previewRing) {
                pushIsoEllipse(ringX, ringY, ringRad, 16, 2.0f * s, ringR, ringG, ringB, 0.62f);
                beams.push_back({ringX - 10.0f * s, ringY, ringX + 10.0f * s, ringY,
                                 1.6f * s, ringR, ringG, ringB, 0.62f, 0.62f});
                beams.push_back({ringX, ringY - 5.0f * s, ringX, ringY + 5.0f * s,
                                 1.6f * s, ringR, ringG, ringB, 0.62f, 0.62f});
            }
            if (combatEffects) {
                // Strictly <= 8 distinct RGB colors so VulkanWindow::maxBeamColours (16) is never exceeded.
                if (vehicleScene && !crippledSmokeVehicles.empty()) {
                    const auto& vehs = vehicleScene->vehicles();
                    const float animPhase = std::chrono::duration<float>(now - animStart).count();
                    for (const std::size_t vi : crippledSmokeVehicles) {
                        if (beams.size() >= 64) break;
                        const auto& v = vehs[vi];
                        const float vx = static_cast<float>(v.x * mapScale);
                        const float vy = static_cast<float>(v.y * mapScale);
                        const float sway = std::sin(animPhase * 2.1f + static_cast<float>(vi) * 1.1f) * 4.0f * s;
                        const float rise = (14.0f + 6.0f * std::sin(animPhase * 3.4f + static_cast<float>(vi))) * s;
                        beams.push_back({vx, vy - 5.0f * s, vx + sway, vy - rise, 7.5f * s, 60, 55, 50, 0.36f, 0.05f});
                    }
                }
                if (vehicleScene && !movingSmokeVehicles.empty()) {
                    const auto& vehs = vehicleScene->vehicles();
                    const float animPhase = std::chrono::duration<float>(now - animStart).count();
                    std::size_t drawn = 0;
                    for (const std::size_t vi : movingSmokeVehicles) {
                        if (drawn >= 6u || beams.size() >= 64u || vi >= vehs.size()) break;
                        const auto& v = vehs[vi];
                        if (!v.alive || !v.visible) continue;
                        const float vx = static_cast<float>(v.x * mapScale);
                        const float vy = static_cast<float>(v.y * mapScale);
                        const float sway = std::sin(animPhase * 4.2f + static_cast<float>(vi)) * 3.0f * s;
                        const float rise = (8.0f + 4.0f * std::sin(animPhase * 5.1f + static_cast<float>(vi))) * s;
                        beams.push_back({vx, vy - 2.0f * s, vx + sway, vy - rise, 4.5f * s, 60, 55, 50, 0.22f, 0.03f});
                        ++drawn;
                    }
                }
                if (!livePlanes.empty()) {
                    const float animPhase = std::chrono::duration<float>(now - animStart).count();
                    for (const auto& [sid, lp] : livePlanes) {
                        if (beams.size() >= 64u) break;
                        if (!lp.pose.alive || lp.pose.maxHp <= 0 || lp.pose.hp * 2 > lp.pose.maxHp) continue;
                        const auto* pSpec = unitCatalog ? unitCatalog->findPlane(lp.pose.planeId) : nullptr;
                        if (pSpec && !pSpec->hasFallSmoke) continue;
                        const float px = static_cast<float>(lp.pose.x * mapScale);
                        const float py = static_cast<float>(lp.pose.y * mapScale);
                        const float sway = std::sin(animPhase * 3.8f + static_cast<float>(sid)) * 5.0f * s;
                        const float rise = (16.0f + 8.0f * std::sin(animPhase * 4.5f + static_cast<float>(sid))) * s;
                        beams.push_back({px, py, px + sway, py - rise, 7.0f * s, 60, 55, 50, 0.42f, 0.06f});
                    }
                }
                for (const auto& bw : burningWrecks) {
                    if (beams.size() >= 64) break;
                    const float tSec = std::chrono::duration<float>(bw.until - now).count();
                    const float sway = std::sin(tSec * 2.5f + bw.x * 0.03f) * 5.0f * s;
                    const float rise = (18.0f + 10.0f * std::sin(tSec * 4.0f + bw.y * 0.05f)) * s;
                    const float glow = 0.55f + 0.35f * std::sin(tSec * 9.0f);
                    beams.push_back({bw.x, bw.y - 6.0f * s, bw.x + sway, bw.y - rise, 10.0f * s, 60, 55, 50, 0.45f, 0.08f});
                    beams.push_back({bw.x, bw.y - 4.0f * s, bw.x, bw.y - 4.0f * s, 5.0f * s, 235, 100, 30, glow, glow});
                }
                for (const auto& v : activeVfx) {
                    if (beams.size() >= 80) break;
                    const float t = std::clamp(
                        std::chrono::duration<float>(now - v.start).count() /
                            std::max(0.01f, static_cast<float>(v.duration.count()) * 0.001f),
                        0.0f, 1.0f);
                    switch (v.kind) {
                    case ActiveVfx::Kind::Flamer: {
                        const float flick = 1.0f + 0.16f * std::sin(t * 28.0f);
                        const float fade = 1.0f - 0.65f * t;
                        const float reach = std::min(1.0f, t * 2.4f);
                        const float ex = v.x0 + (v.x1 - v.x0) * reach;
                        const float ey = v.y0 + (v.y1 - v.y0) * reach;
                        beams.push_back({v.x0, v.y0, ex, ey, 11.0f * s * flick, 255, 145, 40, 0.90f * fade, 0.25f * fade});
                        beams.push_back({v.x0, v.y0, ex, ey, 5.5f * s * flick, 255, 230, 130, 0.95f * fade, 0.45f * fade});
                        break;
                    }
                    case ActiveVfx::Kind::Tracer:
                    case ActiveVfx::Kind::CannonShell: {
                        const bool heavy = (v.kind == ActiveVfx::Kind::CannonShell);
                        if (t < 0.45f) {
                            const float fa = 1.0f - (t / 0.45f);
                            beams.push_back({v.x0, v.y0, v.x0, v.y0, (heavy ? 12.0f : 8.5f) * s, 255, 230, 130, fa, fa});
                        }
                        const float tail = std::max(0.0f, t - 0.32f);
                        const float bx0 = v.x0 + (v.x1 - v.x0) * tail;
                        const float by0 = v.y0 + (v.y1 - v.y0) * tail;
                        const float bx1 = v.x0 + (v.x1 - v.x0) * t;
                        const float by1 = v.y0 + (v.y1 - v.y0) * t;
                        if (heavy) {
                            beams.push_back({bx0, by0, bx1, by1, 3.6f * s, 255, 145, 40, 0.10f, 1.0f});
                        } else {
                            beams.push_back({bx0, by0, bx1, by1, 2.0f * s, 255, 240, 190, 0.0f, 1.0f});
                        }
                        if (t > 0.78f) {
                            const float ia = (1.0f - t) / 0.22f;
                            beams.push_back({v.x1, v.y1, v.x1, v.y1, (heavy ? 9.0f : 5.5f) * s, 255, 230, 130, ia * 0.85f, ia * 0.85f});
                        }
                        break;
                    }
                    case ActiveVfx::Kind::Rocket: {
                        const float smokeTail = std::max(0.0f, t - 0.55f);
                        const float sx0 = v.x0 + (v.x1 - v.x0) * smokeTail;
                        const float sy0 = v.y0 + (v.y1 - v.y0) * smokeTail - 20.0f * s * std::sin(smokeTail * 3.14159f);
                        const float arcY = -20.0f * s * std::sin(t * 3.14159f);
                        const float hx = v.x0 + (v.x1 - v.x0) * t;
                        const float hy = v.y0 + (v.y1 - v.y0) * t + arcY;
                        beams.push_back({sx0, sy0, hx, hy, 7.5f * s, 60, 55, 50, 0.08f, 0.52f * (1.0f - 0.35f * t)});
                        const float fTail = std::max(0.0f, t - 0.14f);
                        const float fx0 = v.x0 + (v.x1 - v.x0) * fTail;
                        const float fy0 = v.y0 + (v.y1 - v.y0) * fTail - 20.0f * s * std::sin(fTail * 3.14159f);
                        beams.push_back({fx0, fy0, hx, hy, 4.2f * s, 255, 145, 40, 0.35f, 1.0f});
                        if (t > 0.78f) {
                            const float ia = (1.0f - t) / 0.22f;
                            beams.push_back({v.x1, v.y1, v.x1, v.y1, 14.0f * s, 255, 230, 130, ia * 0.9f, ia * 0.9f});
                        }
                        break;
                    }
                    case ActiveVfx::Kind::Explosion:
                    case ActiveVfx::Kind::VehicleExplosion: {
                        const float sc = (v.kind == ActiveVfx::Kind::VehicleExplosion) ? 1.55f : 1.0f;
                        const float fade = 1.0f - t;
                        const float fireR = (10.0f + 22.0f * std::min(1.0f, t * 2.0f)) * sc * s;
                        beams.push_back({v.x0, v.y0, v.x0, v.y0, fireR, 220, 65, 25, fade * 0.85f, fade * 0.85f});
                        if (t < 0.65f) {
                            const float coreA = (1.0f - t / 0.65f);
                            beams.push_back({v.x0, v.y0, v.x0, v.y0, fireR * 0.55f, 255, 230, 130, coreA * 0.9f, coreA * 0.9f});
                        }
                        beams.push_back({v.x0, v.y0 - 4.0f * s, v.x0, v.y0 - (20.0f + 16.0f * t) * sc * s,
                                         14.0f * sc * s, 60, 55, 50, fade * 0.50f, 0.08f});
                        break;
                    }
                    case ActiveVfx::Kind::HealPulse: {
                        const float fade = 1.0f - t;
                        beams.push_back({v.x0, v.y0 - 12.0f * t * s, v.x0, v.y0 - (8.0f + 16.0f * t) * s,
                                         7.0f * s, 80, 235, 120, fade * 0.9f, fade * 0.3f});
                        break;
                    }
                    case ActiveVfx::Kind::RepairPulse: {
                        const float fade = 1.0f - t;
                        beams.push_back({v.x0, v.y0 - 10.0f * t * s, v.x0, v.y0 - (8.0f + 16.0f * t) * s,
                                         8.0f * s, 90, 210, 255, fade * 0.9f, fade * 0.3f});
                        break;
                    }
                    case ActiveVfx::Kind::Parachute: {
                        const float fade = 1.0f - 0.35f * t;
                        const float sway = std::sin(t * 9.0f) * 4.5f * s * (1.0f - t);
                        const float px = v.x1 + sway;
                        const float py = v.y0 + (v.y1 - v.y0) * t;
                        const float cy = py - 14.0f * s;
                        beams.push_back({v.x1, v.y1, v.x1, v.y1, (10.0f - 4.0f * t) * s, 60, 55, 50,
                                         0.38f * (1.0f - 0.5f * t), 0.38f * (1.0f - 0.5f * t)});
                        beams.push_back({px - 6.0f * s, cy, px + 6.0f * s, cy, 9.0f * s, 255, 240, 190,
                                         0.88f * fade, 0.88f * fade});
                        beams.push_back({px, cy + 2.0f * s, px, py, 1.8f * s, 255, 240, 190,
                                         0.65f * fade, 0.45f * fade});
                        break;
                    }
                    }
                }
            }
            vfxWasNonEmpty = !beams.empty();
            renderer.setBeams(std::move(beams));
        }
        const auto noticeFrame = std::chrono::steady_clock::now();
        const double frameDt = std::clamp(std::chrono::duration<double>(noticeFrame - lastNoticeFrame).count(), 0.0, 0.1);
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) && !briefing.active() && !menu.active()) {
            const bool* kb = SDL_GetKeyboardState(nullptr);
            double kdx = 0.0, kdy = 0.0;
            if (kb[SDL_SCANCODE_LEFT]) kdx -= 1.0;
            if (kb[SDL_SCANCODE_RIGHT]) kdx += 1.0;
            if (kb[SDL_SCANCODE_UP]) kdy -= 1.0;
            if (kb[SDL_SCANCODE_DOWN]) kdy += 1.0;
            if (kdx != 0.0 && kdy != 0.0) { kdx *= 0.70710678; kdy *= 0.70710678; }
            if (kdx != 0.0 || kdy != 0.0) {
                const bool fast = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
                const double keyPanSpeed = fast ? 1140.0 : 760.0;
                renderer.panMap(kdx * keyPanSpeed * frameDt, kdy * keyPanSpeed * frameDt);
            }
        }
        if (scene && selection) {
            bool showTip = false;
            if (SDL_GetMouseFocus() == window && !briefing.active() && !menu.active()) {
                float mx{}, my{};
                const auto buttons = SDL_GetMouseState(&mx, &my);
                int winW = 1, winH = 1;
                SDL_GetWindowSize(window, &winW, &winH);
                // Classic RTS mouse edge-scrolling when cursor is near the window edges (not while dragging).
                if (!(buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK)) && winW > 80 && winH > 80) {
                    constexpr float edgeMargin = 24.0f;
                    double dx = 0.0, dy = 0.0;
                    if (mx <= edgeMargin) dx = -1.0;
                    else if (mx >= static_cast<float>(winW) - edgeMargin) dx = 1.0;
                    if (my <= edgeMargin) dy = -1.0;
                    else if (my >= static_cast<float>(winH) - edgeMargin) dy = 1.0;
                    if (dx != 0.0 && dy != 0.0) { dx *= 0.70710678; dy *= 0.70710678; }
                    if (dx != 0.0 || dy != 0.0) {
                        constexpr double panSpeedPtsPerSec = 760.0;
                        renderer.panMap(dx * panSpeedPtsPerSec * frameDt, dy * panSpeedPtsPerSec * frameDt);
                    }
                }
                if (!hud || !hud->opaqueAtWindow(mx, my)) {
                    const auto [mapX, mapY] = renderer.mapPosition(mx, my);
                    const auto appendArmorLine = [&](ss2::TooltipContent& tip, const ss2::assets::UnitSpec* spec) {
                        if (!spec) return;
                        const auto& ap = spec->armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)];
                        if (ap.front > 0 || ap.side > 0 || ap.rear > 0) {
                            tip.lines.push_back(ss2::trf("Armor F/S/R: {0} / {1} / {2}",
                                                         {std::to_string(ap.front), std::to_string(ap.side),
                                                          std::to_string(ap.rear)}));
                        }
                    };
                    if (const auto hovered = selection->infantryAt(int(std::floor(mapX)), int(std::floor(mapY)))) {
                        const auto& u = scene->units().at(*hovered);
                        if (u.hp > 0 && u.visible) {
                            const int maxHp = (*hovered < initialUnitHp.size()) ? initialUnitHp[*hovered] : 100;
                            const auto* uSpec = unitCatalog ? unitCatalog->find(u.id) : nullptr;
                            std::string lowId = u.id;
                            for (char& ch : lowId) {
                                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            }
                            ss2::TooltipContent tc;
                            tc.title = ss2::tr(hudDisplayName(u.id));
                            tc.gauge = std::clamp(static_cast<float>(u.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
                            if (u.owner == 0) {
                                tc.lines.push_back(ss2::trf("Health {0} / {1}", {std::to_string(u.hp), std::to_string(maxHp)}));
                                const int maxInfAmmo = (uSpec && !uSpec->isVehicle && uSpec->ammo1 > 0) ? uSpec->ammo1 : 40;
                                tc.lines.push_back(ss2::trf("Ammo {0} / {1}", {std::to_string(u.ammo), std::to_string(maxInfAmmo)}));
                                const int expVal = (*hovered < unitExpa.size()) ? unitExpa[*hovered] : 0;
                                const int killsVal = (*hovered < unitKills.size()) ? unitKills[*hovered] : 0;
                                if (killsVal > 0) {
                                    tc.lines.push_back(ss2::trf("Rank: {0} ({1} kills)", {ss2::tr(veterancyRank(expVal)), std::to_string(killsVal)}));
                                } else {
                                    tc.lines.push_back(ss2::trf("Rank: {0}", {ss2::tr(veterancyRank(expVal))}));
                                }
                                if (lowId.find("officer") != std::string::npos ||
                                    lowId.find("general") != std::string::npos ||
                                    (uSpec && uSpec->isOfficerInfantry())) {
                                    tc.lines.push_back(ss2::tr(binocularUnits.contains(*hovered)
                                                                   ? "Binoculars active"
                                                                   : "Officer — press B for binoculars"));
                                    const bool isGeneralUnit = lowId.find("general") != std::string::npos ||
                                                               (uSpec && uSpec->selType == "general");
                                    tc.lines.push_back(ss2::tr(isGeneralUnit
                                                                   ? "General Command Aura — wide morale rally"
                                                                   : "Officer Aura — rallies nearby troops"));
                                } else if (lowId.find("medic") != std::string::npos ||
                                           (uSpec && uSpec->isMedicInfantry())) {
                                    tc.lines.push_back(ss2::tr("Field Medic — heals nearby troops"));
                                    if (!holdFireUnits.contains(*hovered)) {
                                        tc.lines.push_back(ss2::tr("Auto-support active (H to hold)"));
                                    }
                                } else if (lowId.find("sapper") != std::string::npos ||
                                           lowId.find("miner") != std::string::npos ||
                                           (uSpec && (uSpec->selType == "sapper" || uSpec->selType == "sappers"))) {
                                    tc.lines.push_back(ss2::tr("Sapper — E to lay mine / right-click mine to disarm"));
                                } else if ((uSpec && uSpec->isSniper()) ||
                                           lowId.find("sniper") != std::string::npos) {
                                    tc.lines.push_back(ss2::tr(binocularUnits.contains(*hovered)
                                                                   ? "Sniper — binoculars active (C to camouflage)"
                                                                   : "Sniper — C camouflage / B binoculars"));
                                } else if ((uSpec && uSpec->isHeavyMgInfantry()) ||
                                           lowId.find("hmg") != std::string::npos) {
                                    tc.lines.push_back(ss2::tr("Heavy Machine Gunner — high-rate suppression"));
                                } else if ((uSpec && uSpec->isMortarInfantry()) ||
                                           lowId.find("mortar") != std::string::npos) {
                                    tc.lines.push_back(ss2::tr("Light Mortar — G to bombard / explosive splash"));
                                } else if ((uSpec && uSpec->hasFlamethrower()) ||
                                           lowId.find("flam") != std::string::npos) {
                                    tc.lines.push_back(ss2::tr("Flamethrower — clears buildings & trenches"));
                                } else if ((uSpec && uSpec->isAntiTankInfantry()) ||
                                           lowId.find("bazooka") != std::string::npos ||
                                           lowId.find("panzerfaust") != std::string::npos ||
                                           lowId.find("piat") != std::string::npos) {
                                    tc.lines.push_back(ss2::tr("Anti-Tank Infantry — armor-piercing rockets"));
                                } else if ((uSpec && uSpec->isKamikaze()) ||
                                           lowId.find("kamikaze") != std::string::npos) {
                                    tc.lines.push_back(ss2::tr("Kamikaze — explosive suicide charge"));
                                }
                                if (*hovered < unitGrenades.size() && unitGrenades[*hovered] > 0) {
                                    tc.lines.push_back(ss2::trf("AT grenades: {0}", {std::to_string(unitGrenades[*hovered])}));
                                }
                                if (*hovered < unitMineAmmo.size() && unitMineAmmo[*hovered] > 0) {
                                    tc.lines.push_back(ss2::trf("AT mines: {0}", {std::to_string(unitMineAmmo[*hovered])}));
                                }
                                tc.lines.push_back(ss2::tr(crouchedUnits.contains(*hovered) ? "Crouching" : "Standing"));
                                if (holdFireUnits.contains(*hovered)) tc.lines.push_back(ss2::tr("Holding fire"));
                                if (camouflagedUnits.contains(*hovered)) {
                                    tc.lines.push_back(ss2::tr("Status: Camouflaged (hidden)"));
                                }
                                if (uSpec && uSpec->moraleResist >= 1000 && !uSpec->isOfficerInfantry()) {
                                    tc.lines.push_back(ss2::tr("Elite Morale — resists suppression"));
                                }
                                if (uSpec && uSpec->isFearless()) {
                                    tc.lines.push_back(ss2::tr("Fearless — maintains fire rate under suppression"));
                                }
                                if (uSpec && uSpec->isQualifiedVehicleCrew() && !uSpec->isOfficerInfantry()) {
                                    tc.lines.push_back(ss2::tr("Qualified Crew — full efficiency in captured vehicles"));
                                }
                                if (uSpec && !uSpec->canFireFromInside) {
                                    tc.lines.push_back(ss2::tr("Cannot fire from inside buildings"));
                                } else if (uSpec && uSpec->hasExplicitInsideRule && uSpec->canFireFromInside) {
                                    tc.lines.push_back(ss2::tr("Safe to fire from inside buildings (no backblast)"));
                                }
                                if (uSpec && uSpec->crouchToFire) {
                                    tc.lines.push_back(ss2::tr("Prone Deployment — crouches to fire AT rifle"));
                                }
                                if (suppressedUnits.contains(*hovered)) {
                                    tc.lines.push_back(ss2::tr("Status: Suppressed (slower fire — rally with Officer)"));
                                }
                                if (simulation && simulation->isElevatedMapPoint(u.x, u.y)) {
                                    tc.lines.push_back(ss2::tr("High Ground (+vision, +range)"));
                                }
                            } else if (isAllyOwner(u.owner)) {
                                tc.lines.push_back(ss2::tr("Allied"));
                            } else if (u.owner == 3) {
                                tc.lines.push_back(ss2::tr("Neutral"));
                            } else {
                                tc.lines.push_back(ss2::tr("Hostile"));
                            }
                            tooltip.show(tc, mx, my);
                            showTip = true;
                        }
                    } else if (vehicleScene) {
                        if (const auto vHovered = selection->vehicleAt(int(std::floor(mapX)), int(std::floor(mapY)))) {
                            const auto& v = vehicleScene->vehicles().at(*vHovered);
                            if (v.alive && v.hp > 0 && v.visible) {
                                const int maxHp = (*vHovered < initialVehicleHp.size()) ? initialVehicleHp[*vHovered] : 100;
                                const auto* vSpec = unitCatalog ? unitCatalog->find(v.id) : nullptr;
                                ss2::TooltipContent tc;
                                tc.title = ss2::tr(hudDisplayName(v.id));
                                tc.gauge = std::clamp(static_cast<float>(v.hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
                                if (v.owner == 0) {
                                    tc.lines.push_back(ss2::trf("Health {0} / {1}", {std::to_string(v.hp), std::to_string(maxHp)}));
                                    if (vSpec && vSpec->shot1.damage > 0 && vSpec->shot2.damage > 0) {
                                        const int a2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                        const int maxA1 = vSpec->ammo1 > 0 ? vSpec->ammo1 : std::max(1, v.ammo);
                                        const char* fmt = vSpec->shot2.isFlamethrower()
                                            ? "Ammo {0}/{1} | Flame {2}"
                                            : "Ammo {0}/{1} | MG {2}";
                                        tc.lines.push_back(ss2::trf(fmt,
                                                                    {std::to_string(v.ammo), std::to_string(maxA1), std::to_string(a2)}));
                                    } else if (vSpec && vSpec->shot1.damage > 0) {
                                        const int maxA1 = vSpec->ammo1 > 0 ? vSpec->ammo1 : std::max(1, v.ammo);
                                        tc.lines.push_back(ss2::trf("Ammo {0} / {1}", {std::to_string(v.ammo), std::to_string(maxA1)}));
                                    } else if (vSpec && vSpec->shot2.damage > 0) {
                                        if (vSpec->ammo2 > 0) {
                                            const int a2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                            tc.lines.push_back(ss2::trf("Ammo {0} / {1}",
                                                                        {std::to_string(a2), std::to_string(vSpec->ammo2)}));
                                        } else {
                                            const int maxA1 = vSpec->ammo1 > 0 ? vSpec->ammo1 : std::max(1, v.ammo);
                                            tc.lines.push_back(ss2::trf("Ammo {0} / {1}",
                                                                        {std::to_string(v.ammo), std::to_string(maxA1)}));
                                        }
                                    }
                                    appendArmorLine(tc, vSpec);
                                    const int vExpVal = (*vHovered < vehExpa.size()) ? vehExpa[*vHovered] : 0;
                                    const int vKillsVal = (*vHovered < vehKills.size()) ? vehKills[*vHovered] : 0;
                                    if (vKillsVal > 0) {
                                        tc.lines.push_back(ss2::trf("Rank: {0} ({1} kills)", {ss2::tr(veterancyRank(vExpVal)), std::to_string(vKillsVal)}));
                                    } else {
                                        tc.lines.push_back(ss2::trf("Rank: {0}", {ss2::tr(veterancyRank(vExpVal))}));
                                    }
                                    if (vSpec && vSpec->canMoveBackward) {
                                        tc.lines.push_back(ss2::tr("Press R + click to reverse"));
                                    }
                                    if (vSpec && vSpec->moveDamageHp > 0 && v.hp < vSpec->moveDamageHp) {
                                        tc.lines.push_back(ss2::tr("Tracks damaged — needs supply truck"));
                                    }
                                    if (vSpec && vSpec->isSupplyCrate()) {
                                        if (vSpec->ammo1 > 0) {
                                            tc.lines.push_back(ss2::tr("Ammo Crate"));
                                            tc.lines.push_back(v.ammo > 0
                                                ? ss2::trf("Ammo Stock: {0} / {1}", {std::to_string(v.ammo), std::to_string(vSpec->ammo1)})
                                                : ss2::tr("Depleted"));
                                        } else {
                                            tc.lines.push_back(ss2::tr("Spare Parts Crate"));
                                            const int p2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                            const int maxP2 = std::max(1, vSpec->ammo2);
                                            tc.lines.push_back(p2 > 0
                                                ? ss2::trf("Spare Parts: {0} / {1}", {std::to_string(p2), std::to_string(maxP2)})
                                                : ss2::tr("Depleted"));
                                        }
                                    } else if (vSpec && vSpec->isSupplyTruck()) {
                                        tc.lines.push_back(ss2::tr("Supply & Repair Truck"));
                                        if (vSpec->ammo1 > 0 || vSpec->ammo2 > 0) {
                                            const int a2 = (*vHovered < vehicleAmmo2.size()) ? vehicleAmmo2[*vHovered] : 0;
                                            tc.lines.push_back(ss2::trf("Supply: Ammo {0}/{1} | Parts {2}/{3}",
                                                                        {std::to_string(v.ammo), std::to_string(vSpec->ammo1),
                                                                         std::to_string(a2), std::to_string(vSpec->ammo2)}));
                                        }
                                        if (!vehicleHoldFireUnits.contains(*vHovered)) {
                                            tc.lines.push_back(ss2::tr("Auto-support active (H to hold)"));
                                        }
                                    } else if (vSpec && vSpec->isAmbulance()) {
                                        tc.lines.push_back(ss2::tr("Mobile Hospital"));
                                        if (!vehicleHoldFireUnits.contains(*vHovered)) {
                                            tc.lines.push_back(ss2::tr("Auto-support active (H to hold)"));
                                        }
                                    } else if (vSpec && vSpec->isRocketArtillery()) {
                                        tc.lines.push_back(ss2::tr("Rocket Artillery"));
                                    } else if (vSpec && vSpec->hasFlamethrower()) {
                                        tc.lines.push_back(ss2::tr("Flamethrower Vehicle"));
                                    } else if (vSpec && vSpec->isHeavyAntiAir()) {
                                        tc.lines.push_back(ss2::tr("Heavy High-Altitude Flak (effective vs all aircraft)"));
                                    } else if (vSpec && vSpec->isAntiAir()) {
                                        tc.lines.push_back(ss2::tr("Anti-Aircraft / Flak (best vs low-flying aircraft)"));
                                    } else if (vSpec && vSpec->isCasemate() && vSpec->selType != "howitzer") {
                                        tc.lines.push_back(ss2::tr("Assault Gun / Tank Destroyer"));
                                    } else if (vSpec && vSpec->isKamikaze()) {
                                        tc.lines.push_back(ss2::tr("Demolition Charge Vehicle (self-destructs)"));
                                    } else if (vSpec && vSpec->isArtillery()) {
                                        tc.lines.push_back(ss2::tr("Artillery / Field Gun"));
                                    } else if (vSpec && vSpec->isFortification()) {
                                        tc.lines.push_back(ss2::tr("Emplaced Machine-Gun Nest"));
                                    }
                                    if (vSpec && (vSpec->turretType == "gaub" || vSpec->turretType == "atg_gaub")) {
                                        tc.lines.push_back(ss2::tr("Heavy Turret Artillery — high-explosive bombardment"));
                                    }
                                    if (vSpec && vSpec->isGunboat()) {
                                        tc.lines.push_back(ss2::tr("Naval Gunboat"));
                                    } else if (vSpec && vSpec->isNavalCraft()) {
                                        tc.lines.push_back(ss2::tr("Naval / Landing Craft"));
                                    }
                                    if (vSpec && vSpec->isTrain()) {
                                        tc.lines.push_back(ss2::tr("Armored Train"));
                                    }
                                    if (vSpec && vSpec->passengersCanFire) {
                                        tc.lines.push_back(ss2::tr("Open-Top — passengers can fire"));
                                    }
                                    if (vSpec && vSpec->soldiersOnArmor) {
                                        tc.lines.push_back(ss2::tr("Tank Desant — riders exposed on hull armor"));
                                    }
                                    if (vSpec && vSpec->gunShotWait > 0) {
                                        tc.lines.push_back(ss2::tr("Heavy Gun — stabilizes briefly after moving"));
                                    }
                                    if (vSpec && vSpec->isPropaganda()) {
                                        tc.lines.push_back(ss2::tr("Command / Morale Vehicle — rallies nearby troops"));
                                    }
                                    if (*vHovered < vehicleCrewImpaired.size() && vehicleCrewImpaired[*vHovered]) {
                                        tc.lines.push_back(ss2::tr("Crew depleted — slower reload (heal with Medic)"));
                                    }
                                    if (*vHovered < vehicleMoveImpaired.size() && vehicleMoveImpaired[*vHovered]) {
                                        if (vSpec && vSpec->soldToMove > vSpec->crewNumber && vSpec->isTowable()) {
                                            tc.lines.push_back(ss2::tr("Heavy Towed Gun — requires tractor to relocate"));
                                        } else {
                                            tc.lines.push_back(ss2::tr("Crew depleted — cannot move (heal or tow)"));
                                        }
                                    }
                                    if (*vHovered < vehicleSuppressed.size() && vehicleSuppressed[*vHovered]) {
                                        tc.lines.push_back(ss2::tr("Status: Crew Shocked (reload slowed — rally with Officer)"));
                                    }
                                    if (vSpec && (vSpec->marchEnabled || vSpec->marchSightBonus > 0)) {
                                        const bool hatchIsOpen = *vHovered < vehicleHatchOpen.size() &&
                                                                 vehicleHatchOpen[*vHovered];
                                        tc.lines.push_back(ss2::tr(
                                            hatchIsOpen ? "Commander Hatch: Open (+sight, X to button up)"
                                                        : "Commander Hatch: Buttoned (X to open)"));
                                    }
                                    if (vSpec && vSpec->moraleResist >= 10000) {
                                        tc.lines.push_back(ss2::tr("Heavy Armor Morale — resists crew shock"));
                                    }
                                    if (vSpec && vSpec->shot1.indirectArc) {
                                        tc.lines.push_back(ss2::tr("Indirect Fire — high-arc barrage (+splash radius)"));
                                    }
                                    if (vSpec && vSpec->crewCanBeHealed) {
                                        tc.lines.push_back(ss2::tr("Open Cab — field medics rapidly treat crew"));
                                    }
                                    if (vSpec && vSpec->canBeParadropped) {
                                        tc.lines.push_back(ss2::tr("Airborne — paradroppable equipment"));
                                    }
                                    if (simulation && simulation->isElevatedMapPoint(v.x, v.y)) {
                                        tc.lines.push_back(ss2::tr("High Ground (+vision, +range)"));
                                    }
                                    if (*vHovered < vehicleCargo.size() && vehicleCargo[*vHovered].second > 0) {
                                        const auto [cnt, cap] = vehicleCargo[*vHovered];
                                        tc.lines.push_back(ss2::trf("Passengers {0} / {1}",
                                                                    {std::to_string(cnt), std::to_string(cap)}));
                                        if (cnt > 0) tc.lines.push_back(ss2::tr("Press U to unload"));
                                    }
                                    if (*vHovered < vehicleTowed.size() && vehicleTowed[*vHovered] >= 0) {
                                        const std::size_t gi = static_cast<std::size_t>(vehicleTowed[*vHovered]);
                                        if (gi < vehicleScene->vehicles().size()) {
                                            tc.lines.push_back(ss2::trf("Towing: {0} (U to unhitch)",
                                                                        {hudDisplayName(vehicleScene->vehicles()[gi].id)}));
                                        }
                                    } else if (*vHovered < vehicleTowedBy.size() && vehicleTowedBy[*vHovered] >= 0) {
                                        tc.lines.push_back(ss2::tr("Towed by tractor (U to unhitch)"));
                                    } else if (vSpec && vSpec->isTowable()) {
                                        tc.lines.push_back(ss2::tr("Towable field gun — right-click with truck to hitch"));
                                    }
                                    if (vehicleHoldFireUnits.contains(*vHovered)) tc.lines.push_back(ss2::tr("Holding fire"));
                                } else if (isAllyOwner(v.owner)) {
                                    tc.lines.push_back(ss2::tr("Allied"));
                                } else if (v.owner == 3) {
                                    tc.lines.push_back(ss2::tr("Neutral — right-click with infantry to capture"));
                                } else {
                                    const bool tracksDmg = vSpec && vSpec->moveDamageHp > 0 && v.hp < vSpec->moveDamageHp;
                                    tc.lines.push_back(ss2::tr(tracksDmg ? "Hostile — Tracks damaged" : "Hostile"));
                                    if (vSpec && vSpec->hasExposedCrew()) {
                                        tc.lines.push_back(ss2::tr("Exposed crew — vulnerable to sniper/flank fire"));
                                    }
                                    appendArmorLine(tc, vSpec);
                                }
                                tooltip.show(tc, mx, my);
                                showTip = true;
                            }
                        }
                    }
                    if (!showTip && !liveBuildings.empty()) {
                        const int simX = int(std::floor(mapX / mapScale));
                        const int simY = int(std::floor(mapY / mapScale));
                        if (const auto bHovered = buildingAtMap(simX, simY)) {
                            const auto& b = liveBuildings[*bHovered];
                            if (!b.destroyed && (b.owner == 0 || isBuildingPointVisible(b.x, b.y))) {
                                ss2::TooltipContent tc;
                                tc.title = ss2::tr("Building");
                                tc.gauge = std::clamp(static_cast<float>(b.hp) / static_cast<float>(std::max(1, b.maxHp)),
                                                      0.0f, 1.0f);
                                tc.lines.push_back(ss2::trf("Integrity {0} / {1}",
                                                            {std::to_string(b.hp), std::to_string(b.maxHp)}));
                                if (b.occupantCount > 0) {
                                    if (b.owner == 0) {
                                        tc.lines.push_back(ss2::trf("Garrison {0} / {1}",
                                                                    {std::to_string(b.occupantCount), std::to_string(b.capacity)}));
                                        if (scene && !b.occupantIds.empty()) {
                                            std::map<std::string, int> counts;
                                            bool anyHold = false;
                                            bool anyBino = false;
                                            bool anySup = false;
                                            bool anyBlockedInside = false;
                                            for (const size_t sid : b.occupantIds) {
                                                if (sid < scene->units().size()) {
                                                    const auto& occId = scene->units()[sid].id;
                                                    ++counts[ss2::tr(hudDisplayName(occId))];
                                                    if (const auto* uSpec = unitCatalog ? unitCatalog->find(occId) : nullptr;
                                                        uSpec && !uSpec->canFireFromInside) {
                                                        anyBlockedInside = true;
                                                    }
                                                }
                                                if (holdFireUnits.contains(sid)) anyHold = true;
                                                if (binocularUnits.contains(sid)) anyBino = true;
                                                if (suppressedUnits.contains(sid)) anySup = true;
                                            }
                                            std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
                                            std::ranges::sort(sorted, [](const auto& a, const auto& c) {
                                                return a.second != c.second ? a.second > c.second : a.first < c.first;
                                            });
                                            std::string roster;
                                            const std::size_t shown = std::min<std::size_t>(3, sorted.size());
                                            for (std::size_t k = 0; k < shown; ++k) {
                                                if (k > 0) roster += ", ";
                                                roster += std::to_string(sorted[k].second) + "x " + sorted[k].first;
                                            }
                                            if (sorted.size() > 3) {
                                                int extra = 0;
                                                for (std::size_t k = 3; k < sorted.size(); ++k) extra += sorted[k].second;
                                                roster += " (+" + std::to_string(extra) + ")";
                                            }
                                            if (!roster.empty()) tc.lines.push_back(std::move(roster));
                                            if (anyHold) tc.lines.push_back(ss2::tr("Garrison holding fire"));
                                            if (anyBino) tc.lines.push_back(ss2::tr("Binoculars active"));
                                            if (anySup) tc.lines.push_back(ss2::tr("Garrison suppressed — reload slowed"));
                                            if (anyBlockedInside) {
                                                tc.lines.push_back(ss2::tr("Some weapons cannot fire indoors (backblast)"));
                                            }
                                        }
                                        tc.lines.push_back(ss2::tr("Click: H hold / B optics / U evac / R-click fire"));
                                    } else if (isAllyOwner(b.owner)) {
                                        tc.lines.push_back(ss2::tr("Allied Garrison"));
                                    } else {
                                        tc.lines.push_back(ss2::tr("Hostile Garrison"));
                                    }
                                } else {
                                    tc.lines.push_back(ss2::tr("Right-click with infantry to garrison"));
                                }
                                const int critThresh = std::min(
                                    unitCatalog ? unitCatalog->buildingDefense().unloadHp : 150,
                                    std::max(1, b.maxHp / 4));
                                if (b.hp > 0 && b.hp <= critThresh) {
                                    tc.lines.push_back(ss2::tr("Critical structure — collapse imminent (U to evacuate)"));
                                }
                                if (b.hp < b.maxHp && (b.owner == -1 || b.owner == 0)) {
                                    tc.lines.push_back(ss2::tr("Right-click with supply truck to repair"));
                                }
                                tooltip.show(tc, mx, my);
                                showTip = true;
                            }
                        }
                    }
                    if (!showTip && !liveVisibleMines.empty()) {
                        const int simX = int(std::floor(mapX / mapScale));
                        const int simY = int(std::floor(mapY / mapScale));
                        const ss2::VisibleMine* bestMine = nullptr;
                        long long bestD2 = 18 * 18;
                        for (const auto& vm : liveVisibleMines) {
                            const long long dx = vm.x - simX;
                            const long long dy = vm.y - simY;
                            const long long d2 = dx * dx + dy * dy;
                            if (d2 <= bestD2) {
                                bestD2 = d2;
                                bestMine = &vm;
                            }
                        }
                        if (bestMine) {
                            ss2::TooltipContent tc;
                            tc.title = ss2::tr(bestMine->antiTank ? "Anti-Tank Mine" : "Anti-Personnel Mine");
                            tc.lines.push_back(ss2::tr(
                                bestMine->hostile ? "Detected hostile mine — right-click with Sapper to disarm"
                                                  : "Friendly mine"));
                            tooltip.show(tc, mx, my);
                            showTip = true;
                        }
                    }
                } else if (hud) {
                    if (const auto ht = hud->tooltipAtWindow(mx, my)) {
                        tooltip.show({ht->title, ht->lines, -1.0f}, mx, my);
                        showTip = true;
                    }
                }
            }
            if (!showTip) tooltip.hide();
        }
        if (audio) audio->updateMusic();
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
    std::filesystem::path gameDir, mission, hdPack;
    std::string campaignQuery;
    int limit = 0;
    bool fourK = false, validate = false, timing = false;
    std::optional<bool> fullscreen;
    bool originalInfantry = false;
    bool autoEngage = true;
    bool languageChosen = false;
    bool listMissions = false;
    bool listCampaigns = false;
    bool overviewOnly = false;
    bool noMovies = false;
    std::optional<int> cliDifficulty;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "ss2 [--game-dir DIR] [--mission FILE.ssm|NAME|INDEX] [--campaign QUERY] [--overview] [--list-missions] [--list-campaigns] [--hd-pack DIR] [--original-infantry] [--no-auto-engage] [--difficulty easy|normal|hard] [--4k] [--fullscreen | --windowed] [--lang en|fr|es] [--frames N] [--no-movies] [--validate] [--timing]\n"
                         "--game-dir opens Tutorial I (or --mission) from an extracted Gold 2.2 game directory.\n"
                         "--list-missions lists all installed single-player missions, their map sizes and descriptions.\n"
                         "--list-campaigns lists all installed campaigns and missions.\n"
                         "--mission launches a playable single-player mission (file path, display name or 1-based index), or displays its overview if no game directory is given or --overview is set.\n"
                         "--campaign launches a playable campaign mission (e.g. german:1, british:5, 031000, or unique title substring).\n"
                         "--overview displays only the mission's JPEG overview instead of launching playable.\n"
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
        else if (arg == "--list-missions") listMissions = true;
        else if (arg == "--list-campaigns") listCampaigns = true;
        else if (arg == "--overview") overviewOnly = true;
        else if (arg == "--no-movies") noMovies = true;
        else if (arg == "--original-infantry") originalInfantry = true;
        else if (arg == "--no-auto-engage") autoEngage = false;
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
    const auto settingsPath = ss2::defaultSettingsPath();
    const bool persistSettings = !limit && !timing && !overviewOnly && !settingsPath.empty();
    ss2::UserSettings settings = persistSettings ? ss2::loadUserSettings(settingsPath) : ss2::UserSettings{};
    if (!languageChosen && settings.language) {
        ss2::setLanguage(*settings.language);
    }
    if (cliDifficulty) {
        settings.difficulty = *cliDifficulty;
    }
    SDLSession sdl;
    if (!noMovies && !gameDir.empty() && !limit && !timing && !overviewOnly)
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
    if (mission.empty() && !gameDir.empty() && !overviewOnly && !limit && !timing) {
        const auto chosen = runFrontEnd(window.get(), validate ? &report : nullptr, gameDir, settings, persistSettings);
        if (!chosen) return 0;
        mission = *chosen;
    }
    for (;;) {
        std::optional<std::filesystem::path> nextMission;
        result = runWindow(window.get(), validate ? &report : nullptr, limit, timing, gameDir, mission, hdPack,
                           originalInfantry, autoEngage, settings, persistSettings, overviewOnly, &nextMission);
        if (!nextMission || result != 0) break;
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
