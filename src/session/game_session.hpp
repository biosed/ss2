#pragma once
#include "assets/hd_pack.hpp"
#include "assets/infantry.hpp"
#include "assets/mission.hpp"
#include "assets/scenery.hpp"
#include "assets/sound_bank.hpp"
#include "assets/sprite.hpp"
#include "assets/terrain.hpp"
#include "assets/unit_catalog.hpp"
#include "assets/vehicles.hpp"
#include "audio/sound_player.hpp"
#include "net/tcp_transport.hpp"
#include "render/plane_atlas.hpp"
#include "render/window.hpp"
#include "sim/command.hpp"
#include "sim/lockstep.hpp"
#include "sim/mission_runner.hpp"
#include "sim/tutorial_coordinates.hpp"
#include "ui/briefing.hpp"
#include "ui/combat_vfx.hpp"
#include "ui/gold_frontend.hpp"
#include "ui/hidpi.hpp"
#include "ui/hud.hpp"
#include "ui/menu.hpp"
#include "ui/settings.hpp"
#include "ui/strings.hpp"
#include "ui/tooltip.hpp"
#include "ui/tutorial_selection.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ss2 {

struct MultiplayerSessionConfig {
    bool enabled{false};
    bool isHost{false};
    int localSeat{0};
    int playerCount{2};
    std::uint32_t turnTicks{kDefaultTurnTicks};
    std::uint32_t inputDelayTurns{kDefaultInputDelayTurns};
    std::uint32_t hashIntervalTurns{kDefaultHashIntervalTurns};
    std::array<std::uint8_t, 4> playerNations{1, 0, 2, 3};
    std::array<int, 4> playerTeams{0, 1, 2, 3};
    std::array<std::string, 4> playerNames{"Player 1", "Player 2", "", ""};
    std::vector<std::shared_ptr<TcpConnection>> peers;
};

int runGameSession(SDL_Window* window, ValidationReport* report, int limit, bool timing,
                   const std::filesystem::path& gameDir, const std::filesystem::path& mission,
                   const std::filesystem::path& hdPack, bool originalInfantry, bool autoEngage,
                   UserSettings& settings, bool persistSettings,
                   bool overviewOnly = false, std::optional<std::filesystem::path>* nextMissionOut = nullptr,
                   const std::filesystem::path& recordPath = {},
                   const cmd::ReplayFile* replayFile = nullptr,
                   bool manualClock = false,
                   const std::filesystem::path& loadSavePath = {},
                   MultiplayerSessionConfig multiplayer = {},
                   std::map<unsigned, std::vector<assets::ReinforcementUnit>>* campaignPools = nullptr,
                   bool liveReplay = false,
                   bool* returnToMainMenuOut = nullptr);

int verifyReplayHeadless(const std::filesystem::path& gameDir, const std::filesystem::path& mission,
                         bool originalInfantry, int difficulty, bool autoEngage,
                         const cmd::ReplayFile& replayFile);

class FrameSample {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point warmupEnd_ = Clock::now() + std::chrono::seconds(1);
    Clock::time_point first_{};
    Clock::time_point previous_{};
    std::vector<double> intervals_;
    bool complete_ = false;

public:
    bool frame();
    bool report(float refreshRate);
};

assets::Infantry scaleInfantry(assets::Infantry unit, int scale);
assets::VehicleUnit scaleVehicle(assets::VehicleUnit unit, int scale);

struct GameSession {
    static constexpr unsigned planeAtlasBase = 232u;
    static constexpr unsigned planeAtlasCount = 24u;
    static_assert(planeAtlasBase + planeAtlasCount <= VulkanWindow::spriteAtlasSlots,
                  "plane atlas range must fit below the beam/hud slots");

    struct WorldGpuSprite {
        Sprite sprite;
        int sortY{0};
        int mapX1x{0}, mapY1x{0};
    };
    struct LivePlane {
        AirSortiePose pose;
        unsigned atlasSlot{planeAtlasBase};
        std::string spriteKey;
        float sprW{32.0f}, sprH{32.0f};
        float texW{32.0f}, texH{32.0f};
        float shadowScale{1.0f};
    };
    struct FallenCorpse {
        int x{0}, y{0};
        std::string id;
        unsigned direction{0};
    };
    struct ActiveVfx {
        enum class Kind { Tracer, CannonShell, Rocket, Flamer, Explosion, VehicleExplosion, HealPulse, RepairPulse, Parachute, Ricochet, MuzzleDust };
        Kind kind{Kind::Tracer};
        float x0{}, y0{}, x1{}, y1{};
        std::chrono::steady_clock::time_point start;
        std::chrono::milliseconds duration{};
    };
    struct ActiveBeacon {
        int x{0}, y{0};
        std::uint8_t seat{0};
        std::chrono::steady_clock::time_point start{};
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
    struct LiveBuilding {
        int x{}, y{}, left{}, top{}, width{}, height{};
        int hp{}, maxHp{}, capacity{}, occupantCount{};
        int owner{-1};
        bool destroyed{false};
        int ruinLeft{}, ruinTop{};
        assets::MapImage ruinedPatch{};
        assets::MapImage basePatch{};
        assets::MapImage intactPatch{};
        std::array<std::uint8_t, 512> palette{};
        std::array<std::vector<std::uint8_t>, 16> stageBlocks{};
        int stampedStage{0};
        assets::SpriteCoverage lastShadowCoverage{};
        bool hasLastShadow{false};
        std::vector<size_t> occupantIds;
    };
    struct LiveBridge {
        int x{}, y{}, left{}, top{}, width{}, height{};
        int hp{3500}, maxHp{3500};
        int stage{0};
        bool destroyed{false};
        assets::BridgePlacement placement{};
        int stampedStage{0};
    };
    struct TimedMarker {
        MapMarker marker;
        std::chrono::steady_clock::time_point until;
        std::optional<size_t> targetUid;
        bool isOrder{false};
    };
    struct OrderPulse {
        enum class Kind : std::uint8_t { Move, Support, Attack };
        int srcSimX{0}, srcSimY{0};
        bool hasSource{false};
        int dstSimX{0}, dstSimY{0};
        Kind kind{Kind::Move};
        std::chrono::steady_clock::time_point start{};
    };
    struct ReloadWindow {
        std::uint32_t startTick{0};
        std::uint32_t durationTicks{0};
    };
    struct ForceRosterGroup {
        std::string displayName;
        std::vector<size_t> infIds;
        std::vector<size_t> vehIds;
    };

    SDL_Window* window;
    ValidationReport* report;
    int limit;
    bool timing;
    const std::filesystem::path& gameDir;
    const std::filesystem::path& mission;
    const std::filesystem::path& hdPack;
    bool originalInfantry;
    bool autoEngage;
    UserSettings& settings;
    bool persistSettings;
    bool overviewOnly;
    std::optional<std::filesystem::path>* nextMissionOut;
    bool* returnToMainMenuOut = nullptr;
    const std::filesystem::path& recordPath;
    const cmd::ReplayFile* replayFile;
    bool manualClock = false;
    bool liveReplay = false;
    cmd::ReplayTransport replayTransport{};
    std::chrono::steady_clock::time_point lastReplayStepFrame{};
    std::filesystem::path loadSavePath;
    std::map<unsigned, std::vector<assets::ReinforcementUnit>>* campaignUnitPools{nullptr};
    std::map<unsigned, std::vector<assets::ReinforcementUnit>> initialSavedPools;
    MultiplayerSessionConfig multiplayer;
    std::optional<LockstepCoordinator> lockstep;
    std::optional<StarRelayTransport> mpTransport;
    std::chrono::steady_clock::time_point nextLockstepTickAt{};
    std::vector<MissionEvent> deferredLockstepEvents;
    bool lockstepWaitingPeer = false;
    bool lockstepDesyncReported = false;
    std::array<bool, 4> mpSeatDisconnected{};
    bool mpChatActive = false;
    bool mpChatTeamOnly = false;
    std::string mpChatDraft;
    int mpRttMs = -1;
    std::uint32_t mpPingSeq = 0;
    std::array<int, 32> lastZeppelinHolder{-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2};
    std::vector<ActiveBeacon> activeBeacons;

    float refreshRate = 0.0f;
    VulkanWindow renderer;
    std::optional<assets::VehicleScene> vehicleScene;
    std::optional<assets::InfantryScene> scene;
    std::map<std::string, unsigned> gpuAtlasSlots;
    std::filesystem::path gpuArtDir;
    std::filesystem::path gpuVehicleArtDir;
    std::set<std::string> gpuInfantry2xIds;
    struct GpuHdVehicleMeta {
        unsigned atlasSlot{0};
        bool hasTurret{false};
    };
    std::map<std::string, GpuHdVehicleMeta> gpuHdVehicles;
    std::map<std::string, unsigned> gpuPckVehicleSlots;
    std::map<std::string, ss2::assets::GpuVehicleAtlas> gpuPckVehicleAtlases;
    std::set<std::string> gpuVehicleIds;
    unsigned gpuNextSlot = 0;
    bool gpuPreloadLogged = false;
    std::set<std::string> gpuFallbackIds;
    std::map<std::size_t, std::chrono::steady_clock::time_point> vehRecoilUntil;
    std::vector<WorldGpuSprite> worldGpuSprites;
    std::map<std::uint32_t, LivePlane> livePlanes;
    std::map<std::uint32_t, ParachutePose> liveParachutes;
    bool planesClearedNeeded = false;
    std::vector<FallenCorpse> fallenSoldiers;
    std::set<size_t> woundedBarLogged;
    std::vector<std::uint8_t> latestFog;
    unsigned fogGridW = 128, fogGridH = 128;
    int mapScale = 1;
    std::set<size_t> crouchedUnits;
    std::set<size_t> holdFireUnits;
    std::map<size_t, std::chrono::steady_clock::time_point> recoilUntil;
    std::unordered_map<std::size_t, ReloadWindow> unitReloadWindow;
    std::unordered_map<std::size_t, ReloadWindow> vehReloadWindow;
    std::chrono::steady_clock::time_point animStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastAnimSync = animStart;

    std::optional<TutorialSelection> selection;
    std::optional<MissionRunner> simulation;
    std::optional<SoundPlayer> audio;
    std::optional<Hud> hud;
    Menu menu;
    enum class MenuPage : std::uint8_t {
        None,
        Pause,
        Options,
        SaveLoad,
        Controls,
        ForceStatus,
        ForceStatusStandalone,
        AirSupport,
        Log,
        Campaigns,
        Missions,
        CampaignMissions,
        Debrief,
        ConfirmRestart,
        ConfirmMainMenu,
        ConfirmQuit,
    };
    MenuPage menuPage = MenuPage::None;
    std::unique_ptr<GoldPostMissionScreen> postMissionScreen;
    Tooltip tooltip;
    std::vector<ActiveVfx> activeVfx;
    std::vector<BurningWreck> burningWrecks;
    CombatVfxSystem combatVfx;
    std::vector<std::uint16_t> terrainObstacles;
    unsigned lastCombatVfxSimTick = 0;
    std::vector<MinimapPing> minimapPings;
    bool vfxWasNonEmpty = false;
    bool attackMoveArmed = false;
    bool attackGroundArmed = false;
    bool reverseMoveArmed = false;
    bool patrolArmed = false;
    bool faceArmed = false;
    bool smokeArmed = false;
    bool hudHidden = false;
    std::set<std::string> registeredPortraits;
    std::optional<assets::AirMissionKind> armedAirSupport;
    std::array<int, 4> missionPlayerTeams{0, 1, 2, 3};
    bool combatEffects;
    bool menuPausedClock = false;
    bool smoothTerrain;
    bool enhancedGraphics;
    std::string currentObjective = "Follow Tutorial I orders";
    std::array<std::set<size_t>, 10> hudGroups;
    std::array<std::set<size_t>, 10> hudVehicleGroups;
    std::vector<MapBar> lastUnitBars;
    std::vector<int> initialUnitHp;
    std::vector<int> initialVehicleHp;
    std::vector<int> unitExpa;
    std::vector<int> unitKills;
    std::vector<int> unitGrenades;
    std::vector<int> unitMineAmmo;
    std::vector<VisibleMine> liveVisibleMines;
    std::size_t idleUnitCursor = 0;
    std::size_t triageCursor = 0;
    std::vector<int> vehExpa;
    std::vector<int> vehKills;
    std::vector<int> vehicleAmmo2;
    std::vector<std::pair<int, int>> vehicleCargo;
    std::vector<int> vehicleTowed;
    std::vector<int> vehicleTowedBy;
    std::vector<LiveBuilding> liveBuildings;
    std::optional<std::size_t> selectedBuilding;
    std::vector<LiveBridge> liveBridges;
    std::vector<assets::TreePlacement> destructibleTrees;
    std::vector<bool> treeFallen;
    assets::CraterCatalog craterCatalog;
    unsigned craterSeed = 1;
    std::map<std::string, assets::MapImage> planeSpriteCache;
    std::unordered_map<std::string, assets::AircraftGpuAtlas> gpuAircraftAtlases;
    std::unordered_set<std::string> failedAircraftIds;
    std::unordered_map<unsigned, std::string> boundPlaneSlots;
    PlaneAtlasAllocator planeAtlas{planeAtlasBase, planeAtlasCount};
    bool planeOverflowLogged = false;
    unsigned mapScheme = 0;
    assets::WeatherKind missionWeather{assets::WeatherKind::Clear};
    std::shared_ptr<const assets::UnitCatalog> unitCatalog;
    std::string pendingMissionBriefing;
    std::string missionBriefingText;
    bool manualBriefingOpen = false;
    std::optional<std::filesystem::path> nextCampaignMissionPath;
    std::string nextCampaignMissionTitle;
    std::string currentMissionTitle;
    std::filesystem::path currentMissionPath;
    unsigned hudMapW = 0, hudMapH = 0;
    assets::MapImage baseMinimap;
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
    unsigned friendlyInfLost = 0;
    unsigned friendlyTanksLost = 0;
    unsigned friendlyVehiclesLost = 0;
    unsigned friendlyGunsLost = 0;
    std::string activeSubgroupLabel;
    std::size_t specialistCursor = 0;
    bool rightDragActive = false;
    float rightDragStartScreenX = 0.0f;
    float rightDragStartScreenY = 0.0f;
    float rightDragCurScreenX = 0.0f;
    float rightDragCurScreenY = 0.0f;
    std::chrono::steady_clock::time_point lastHoverChimeAt = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    std::chrono::steady_clock::time_point lastWeatherLightningAt = std::chrono::steady_clock::now() - std::chrono::seconds(30);
    unsigned nextObjAutoSaveTick = 1500u;
    std::vector<bool> vehicleCrewImpaired;
    std::vector<bool> vehicleMoveImpaired;
    std::vector<bool> vehicleSuppressed;
    std::vector<bool> vehicleHatchOpen;
    std::set<std::size_t> movingSmokeVehicles;
    std::set<std::size_t> movingWaterVehicles;
    Briefing briefing;
    Notice orderNotice;
    std::deque<std::string> pendingDialogs;
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> moveCursor;
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> attackCursor;
    std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> panCursor;
    bool mmbPanningCursor = false;

    FrameSample sample;
    const Snapshot empty{};
    std::uint64_t frames = 0;
    bool running = true;
    bool gestureInterrupted = false;
    bool markerHint = false;
    std::optional<std::pair<int, int>> objectiveMarkerSim;
    bool paused = false;
    std::size_t reportedLosses = 0;
    std::vector<TimedMarker> activeMarkers;
    std::vector<OrderPulse> orderPulses;
    std::set<std::size_t> binocularUnits;
    std::set<std::size_t> vehicleHoldFireUnits;
    std::set<std::size_t> camouflagedUnits;
    std::set<std::size_t> suppressedUnits;
    std::array<std::optional<std::pair<double, double>>, 8> cameraBookmarks;
    std::deque<std::pair<double, double>> eventHistory;
    std::size_t eventHistoryCursor = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> lastWeaponAudio;
    std::chrono::steady_clock::time_point lastUnitVoiceAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::chrono::steady_clock::time_point lastUnderAttackAt = std::chrono::steady_clock::now() - std::chrono::seconds(30);
    std::vector<assets::MissionSummary> menuMissions;
    std::vector<assets::CampaignSummary> menuCampaigns;
    std::vector<ForceRosterGroup> forceRosterGroups;
    unsigned nextAutoSaveTick = 7500u;
    std::chrono::steady_clock::time_point lastRepairAudioAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::chrono::steady_clock::time_point lastArtilleryAlertAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::chrono::steady_clock::time_point lastEngineAudioAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::chrono::steady_clock::time_point nextAmbientAt = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    unsigned ambientPhase = 0;
    std::set<size_t> prevSelInf, prevSelVeh;
    std::chrono::steady_clock::time_point lastNoticeFrame = std::chrono::steady_clock::now();

    GameSession(SDL_Window* window, ValidationReport* report, int limit, bool timing,
                const std::filesystem::path& gameDir, const std::filesystem::path& mission,
                const std::filesystem::path& hdPack, bool originalInfantry, bool autoEngage,
                UserSettings& settings, bool persistSettings,
                bool overviewOnly, std::optional<std::filesystem::path>* nextMissionOut,
                const std::filesystem::path& recordPath, const cmd::ReplayFile* replayFile,
                bool manualClock = false, const std::filesystem::path& loadSavePath = {},
                MultiplayerSessionConfig multiplayer = {},
                std::map<unsigned, std::vector<assets::ReinforcementUnit>>* campaignPools = nullptr,
                bool liveReplay = false,
                bool* returnToMainMenuOut = nullptr);

    int run();
    int localOwner() const noexcept {
        if (replayFile && liveReplay) {
            return replayTransport.viewerSeat >= 0 ? (replayTransport.viewerSeat & 3) : 0;
        }
        return multiplayer.enabled ? multiplayer.localSeat : 0;
    }
    bool replayAllVision() const noexcept { return replayFile && liveReplay && replayTransport.viewerSeat < 0; }
    static const char* mpNationName(std::uint8_t nation) noexcept {
        switch (nation) {
            case 0: return "Russian";
            case 1: return "German";
            case 2: return "British";
            case 3: return "US";
            case 4: return "Japanese";
            default: return "German";
        }
    }
    void processLockstepPostTickSpawns();
    void placeMpBeacon(int simX, int simY);
    void spawnAbandonedCrew(const std::vector<assets::Placement>& spawns);

    // Setup, sprites, terrain decals, and reinforcement spawns (session_setup.cpp).
    void initializeMission();
    void writeRecordingIfRequested();
    void persistNow();
    void applyMapGraphics();
    float fogBrightnessAt(int mx, int my) const;
    bool ensureGpuInfantrySlot(const std::string& id);
    bool ensureGpuVehicleSlot(const std::string& id);
    void syncGpuSprites();
    bool isAllyOwner(int owner) const;
    bool isHostileOwner(int owner) const;
    static const char* veterancyRank(int expa);
    int initialGrenadesForId(std::string_view id) const;
    void applyBuildingStage(std::size_t bIdx, int stage);
    void applyBridgeStage(std::size_t brIdx, int stage);
    bool crushOrBlastTreesAt(int simX, int simY, int radiusPx, bool spawnDust);
    void stampBattleCrater(int simX, int simY, bool large);
    int initialMinesForId(std::string_view id) const;
    static bool isCombatAmmoSoldierId(std::string_view id);
    bool isArmedVehicleId(const std::string& id) const;
    int scaledSoldierAmmo(const std::string& id, int rawAmmo) const;
    bool soldierHasBinoculars(const std::string& id) const;
    bool isBombardInfantryId(const std::string& id) const;
    static std::string hudClassLabel(const std::string& id);
    std::string hudDisplayName(const std::string& id) const;
    int scoreValueForId(const std::string& id, bool isVehicle) const;
    void ensureUnitPortrait(const std::string& id, bool isVehicle);
    static assets::MapImage downsampleOverview(const assets::MapImage& src, unsigned dstW, unsigned dstH);
    void updateMinimapFog(const std::vector<std::uint8_t>& fogGrid, bool force);
    void resetCursor();
    void recordEventPos(double x, double y);
    void syncMarkers();
    void pushMinimapPing(int simX, int simY, bool hostile);
    void showPause();
    void logHud(const std::string& msg);
    void showNotice(const std::string& msg, NoticeKind kind);
    static const char* airKindLabel(assets::AirMissionKind kind);
    bool ensureAircraftAtlas(const std::string& planeId);
    void uploadPlaneSprite(LivePlane& lp);
    std::optional<std::pair<size_t, size_t>> spawnAndRegisterVehicle(const assets::Placement& p);
    void registerSimSpawnedSoldier(std::size_t simIdx, const std::string& unitId, int x, int y, int owner, int group);
    void registerSimSpawnedVehicle(std::size_t simVIdx, const std::string& unitId, int x, int y, int owner, int group,
                                   unsigned dir32 = 0u);
    void syncReplayViewerPerspective();
    void spawnReinforcementWave(const MissionEvent& ev);
    bool isBuildingPointVisible(int simX, int simY) const;
    bool isWaterPoint(int simX, int simY) const;
    bool isShallowsPoint(int simX, int simY) const;

    // Audio & voices (session_audio.cpp).
    std::string inferNation(const std::string& id) const;
    std::string inferVoiceRole(const std::string& id, bool isVehicle) const;
    void playUnitVoiceForId(std::string_view verb, const std::string& id, bool isVehicle,
                            bool force = false, bool isVeteran = false, float pan = 0.0f);
    void playSelectionVoice(std::string_view verb);
    std::tuple<std::string, int, float> weaponSoundForUnit(const std::string& id) const;
    bool playWorldSound(const std::string& group, float baseGain, int simX, int simY,
                        VoicePriority prio = VoicePriority::Normal);

    // Pause/debrief/roster menus and save/load slots (session_menus.cpp).
    std::vector<MenuItem> buildPauseItems();
    std::vector<MenuItem> buildOptionsItems();
    void openAirSupportMenu();
    void openPauseMenu();
    void openOptionsMenu();
    void openConfirmMenu(MenuPage page, const std::string& title,
                         const std::string& confirmId, const std::string& confirmLabel);
    void closeMenu();
    void openMissionsMenu();
    void openCampaignsMenu();
    void openCampaignMissionsMenu(std::size_t campaignIdx);
    void openControlsMenu();
    void openDebriefMenu(bool victory);
    void dismissPostMissionScreen();
    void syncPostMissionMenuOverlay();
    void openLogMenu();
    void openForceOverviewMenu(bool fromPauseMenu);
    void openMissionBriefing();
    std::filesystem::path quickSavePath(int slot) const;
    void doSaveSlot(int slot, bool quiet = false);
    bool loadSaveFromPath(const std::filesystem::path& path, const std::string& loadedNotice,
                          bool isStartupLoad = false);
    void doLoadSlot(int slot);
    void doQuickSave();
    void doQuickLoad();
    void openSaveLoadMenu();

    // Tactical unit/vehicle/air orders (session_orders.cpp).
    void syncActiveSubgroup();
    std::vector<std::size_t> activeInfantrySelection() const;
    std::vector<std::size_t> activeVehicleSelection() const;
    std::vector<std::string> selectionClassOrder() const;
    void armAirSupportKind(assets::AirMissionKind kind);
    void doTogglePause();
    void stepGameSpeed(int delta);
    void pushOrderPulse(int dstSimX, int dstSimY, OrderPulse::Kind kind);
    void doStop();
    void doToggleHoldFire();
    void doToggleCrouch();
    void doToggleBinoculars();
    void doArmAttackMove();
    void doArmPatrol();
    void doArmAttackGround();
    std::optional<std::size_t> buildingAtMap(int simX, int simY) const;
    std::optional<std::size_t> bridgeAtMap(int simX, int simY) const;
    void doUnloadOrUnhitch();
    void doLayMine();
    void doScatter();
    void doAbandonVehicles();
    bool issueGroundOrder(int simX, int simY, bool isAttackMove, bool append);
    void dispatchAirSupportAt(assets::AirMissionKind kind, int simX, int simY);
    void dispatchPatrolAt(int simX, int simY);

    // Per-frame loop stages (session_tick.cpp, session_input.cpp, session_hud.cpp, session_view.cpp).
    void stepSimulationEvents();
    void handleInputEvent(const SDL_Event& event);
    void pollInputEvents();
    void updateHudAndBeams();
    bool updateViewTooltipAndPresent();
};

} // namespace ss2
