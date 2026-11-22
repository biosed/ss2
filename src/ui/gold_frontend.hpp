#pragma once

#include "ui/frontend.hpp"
#include "ui/settings.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ss2 {

class VulkanWindow;

struct GoldMissionEntry {
    std::string name;
    std::string path;
    std::string description;
    std::string tag;
    unsigned width{};
    unsigned height{};
    unsigned scheme{};
    bool completed{};
    int highScore{};
};

struct GoldCampaignMissionEntry {
    std::string code;
    std::string name;
    std::string path;
    std::string easyPath;
    std::string description;
    std::string tag;
    unsigned width{};
    unsigned height{};
    unsigned scheme{};
    bool completed{};
    int highScore{};
};

struct GoldCampaignEntry {
    std::string name;
    std::vector<GoldCampaignMissionEntry> missions;
};

struct GoldStageRect {
    double x{}, y{}, width{}, height{}, scale{1.0};
};

struct GoldPoint {
    double x{}, y{};
    bool inside{false};
};

// Authentic Sudden Strike II Gold 2.2 frontend renderer and controller for Screens 1-4
// plus Gold Campaigns, Options, Ratings, Replay Browser, and Credits screens. Composites real
// menu_common.sue artwork + mission I-JPG1024 overviews onto an aspect-fitted 800x600 stage
// inside OverlaySlot::panel so Briefing and Menu never overlap.
class GoldFrontEnd {
public:
    GoldFrontEnd(SDL_Window* window, const std::filesystem::path& gameDir,
                 std::optional<int> fixedBgIndex = std::nullopt);
    ~GoldFrontEnd();

    GoldFrontEnd(const GoldFrontEnd&) = delete;
    GoldFrontEnd& operator=(const GoldFrontEnd&) = delete;

    void setMissions(std::vector<GoldMissionEntry> missions);
    void setCampaigns(std::vector<GoldCampaignEntry> campaigns);
    void setCustomCampaigns(std::vector<GoldCampaignEntry> campaigns);
    void setReplays(std::vector<FrontEnd::ReplayEntry> replays);
    void setSaves(std::vector<FrontEnd::SaveEntry> saves);
    void setMultiplayerMissions(std::vector<GoldMissionEntry> missions);
    std::optional<std::string> takeDeleteSaveRequest();
    void showNotice(std::string text);
    void setDifficulty(int difficulty);
    int difficulty() const { return fe_.difficulty(); }

    void setSettings(const UserSettings& settings);
    const UserSettings& settings() const { return settings_; }
    bool takeSettingsSaved();

    const FrontEnd& state() const { return fe_; }
    FrontEnd& state() { return fe_; }

    void handleEvent(const SDL_Event& event);
    void handleKey(SDL_Keycode key);
    void handleMouseMove(double stageX, double stageY);
    void handleMouseDown(double stageX, double stageY, std::uint8_t clicks = 1);
    void handleMouseUp();
    void handleMouseWheel(float dy);
    void advance(std::chrono::nanoseconds dt);

    void renderToBuffer(int pixelW, int pixelH, std::vector<std::uint8_t>& outRgba);
    void draw(VulkanWindow& renderer);

    static GoldStageRect computeStage(int pixelW, int pixelH);
    static GoldPoint windowToStage(double windowX, double windowY,
                                   int winW, int winH, int pixelW, int pixelH);

    int scrollOffset() const { return scrollOffset_; }
    int ratingsScroll() const { return ratingsScroll_; }
    double creditsScroll() const { return creditsScroll_; }
    bool creditsPaused() const { return creditsPaused_; }
    std::string hoveredId() const { return hoveredId_; }
    std::string focusedId() const { return focusedId_; }
    bool hasMenuAssets() const;
    bool hasMenuCursor() const;
    bool hasCreditsArt() const;
    // menu_common.sue ramka/speedbar/speed_bt/rombik/colors_bt widgets all decoded.
    bool hasRetailWidgets() const;
    std::size_t creditsLineCount() const;
    bool hasBriefingMedia() const;
    std::size_t briefingPartCount() const;
    int briefingActivePart() const { return briefActivePart_; }
    std::size_t briefingCurrentFrame() const { return briefFrameIndex_; }
    bool briefingPaused() const { return briefPaused_; }
    std::string briefingDisplayedText() const;
    std::string activeNotice() const { return noticeText_; }

private:
    struct Assets;
    struct RenderEnv;

    // Lifecycle, assets, shared list/scroll/focus helpers, and Main Menu (gold_frontend.cpp)
    void loadAssets(std::optional<int> fixedBgIndex);
    void loadCreditsText();
    void ensureFocusedValid();
    void syncNoticeFromState();
    void onScreenChanged(FrontEnd::Screen previous);
    std::size_t activeListCount() const;
    std::size_t activeListSelected() const;
    void selectActiveRow(std::size_t index);
    const char* activeRowPrefix() const;
    void ensureSelectionVisible();
    void scrollBy(int deltaRows);
    void ensureOverviewLoaded(const std::string& path);
    void cycleFocus(const std::vector<std::string>& ids, int step);
    std::string hitTestActiveListAndScrollbar(double sx, double sy) const;
    void handleActiveListNavigationKey(SDL_Keycode key);
    void drawGoldScrollbar(const RenderEnv& env, double sx, double sy, double sw, double sh,
                           int visibleRows, bool showThumb, std::size_t itemCount, int offset,
                           bool upHov, bool downHov, bool thumbActive) const;
    std::string hitTestStage(double sx, double sy) const;
    void activateId(const std::string& id);
    void renderMainMenu(const RenderEnv& env);

    // Single Player, Campaigns, Custom Campaigns, Single Missions, Briefing (gold_frontend_campaigns.cpp)
    void loadBriefingMedia(const std::string& path);
    void clearBriefingMedia();
    void startBriefingVoice(const std::vector<std::int16_t>& pcm);
    void stopBriefingVoice();
    void setBriefingVoicePaused(bool paused);
    void selectBriefingPart(int part);
    void renderBriefingOverlayIfNeeded();
    void advanceBriefingMedia(std::chrono::nanoseconds dt);
    const std::vector<GoldCampaignEntry>& activeCampaignList() const;
    std::size_t activeCampaignIndex() const;
    std::size_t previewCampaignIndex(bool custom = false) const;
    std::string hitTestSinglePlayer(double sx, double sy) const;
    std::string hitTestCampaigns(double sx, double sy) const;
    std::string hitTestMissionBrowser(double sx, double sy) const;
    std::string hitTestBriefing(double sx, double sy) const;
    bool activateSinglePlayer(const std::string& id);
    bool handleSinglePlayerEditKey(SDL_Keycode key);
    void handleSinglePlayerKey(SDL_Keycode key);
    void handleCampaignsKey(SDL_Keycode key);
    void handleMissionBrowserKey(SDL_Keycode key);
    void handleBriefingKey(SDL_Keycode key);
    void handleSinglePlayerTextInput(std::string_view txt);
    void renderSinglePlayer(const RenderEnv& env);
    void renderCampaigns(const RenderEnv& env);
    void renderMissionBrowserScreen(const RenderEnv& env);
    void renderBriefing(const RenderEnv& env);

    // Multiplayer, Load Game, and Replay Browser (gold_frontend_multiplayer.cpp)
    std::string hitTestMultiplayerOrBrowser(double sx, double sy) const;
    bool activateMultiplayer(const std::string& id);
    void finishMultiplayerEdit();
    bool handleMultiplayerEditKey(SDL_Keycode key);
    void handleMultiplayerOrBrowserKey(SDL_Keycode key);
    void handleMultiplayerTextInput(std::string_view txt);
    void renderReplaysOrLoadGame(const RenderEnv& env);
    void renderMultiplayer(const RenderEnv& env);

    // Options, Ratings, and Credits (gold_frontend_dialogs.cpp)
    std::size_t ratingsRowCount() const;
    void scrollRatingsBy(int deltaRows);
    void adjustOption(const std::string& id, int delta);
    void beginOptionsEdit();
    void commitOptionsEdit();
    void cancelOptionsEdit();
    std::string hitTestOptions(double sx, double sy) const;
    std::string hitTestRatings(double sx, double sy) const;
    std::string hitTestCredits(double sx, double sy) const;
    void handleOptionsKey(SDL_Keycode key);
    void handleRatingsKey(SDL_Keycode key);
    void handleCreditsKey(SDL_Keycode key);
    void renderOptions(const RenderEnv& env);
    void renderRatings(const RenderEnv& env);
    void renderCredits(const RenderEnv& env);

    SDL_Window* window_{};
    std::filesystem::path gameDir_;
    FrontEnd fe_;
    std::vector<GoldMissionEntry> missions_;
    std::vector<GoldCampaignEntry> campaigns_;
    std::vector<GoldCampaignEntry> customCampaigns_;
    std::vector<FrontEnd::ReplayEntry> replays_;
    std::vector<FrontEnd::SaveEntry> saves_;
    std::vector<GoldMissionEntry> mpMissions_;
    enum class MpEditField { None, HostAddress, Port };
    MpEditField mpEditField_{MpEditField::None};
    std::string mpPortDraft_{"27015"};
    enum class ProfileEditMode { None, New, Rename };
    ProfileEditMode profileEditMode_{ProfileEditMode::None};
    std::string profileNameDraft_{"YourID"};
    int profileScroll_{0};
    std::optional<std::string> pendingDeleteSave_;
    UserSettings settings_{};
    UserSettings optionsDraft_{};
    Language optionsBackupLanguage_{Language::english};
    bool settingsSaved_{false};
    std::unique_ptr<Assets> assets_;

    std::string hoveredId_;
    std::string focusedId_{"single"};
    int scrollOffset_{0};
    int ratingsScroll_{0};
    double briefScroll_{0.0};
    std::string briefMediaPath_;
    int briefActivePart_{0};
    std::size_t briefLoopPart_{0};
    std::size_t briefFrameIndex_{0};
    double briefAccumMs_{0.0};
    double briefHoldMs_{0.0};
    bool briefPaused_{false};
    bool briefOverlayDirty_{false};
    double creditsScroll_{0.0};
    double creditsContentHeight_{0.0};
    bool creditsPaused_{false};
    bool draggingThumb_{false};
    double dragStartY_{0.0};
    int dragStartScroll_{0};
    bool dirty_{true};

    std::string noticeText_;
    std::chrono::nanoseconds noticeRemaining_{};

    int lastPixelW_{0};
    int lastPixelH_{0};
    std::vector<std::uint8_t> rgba_;
};

// Authentic Sudden Strike II Gold 2.2 Screen 5 Mission Loading screen (lng.sue\loadscr.dat).
// Decodes and caches the 640x480 wartime vignette background, 548x36 steel progress frame,
// 516x4 metallic progress bar fill, and 14x8 completion indicator lamp once per mission load,
// with an asset-free fallback when gameDir is empty or loadscr.dat is absent.
class GoldLoadingScreen {
public:
    GoldLoadingScreen(SDL_Window* window, const std::filesystem::path& gameDir,
                      std::optional<int> fixedBgIndex = std::nullopt);
    ~GoldLoadingScreen();

    GoldLoadingScreen(const GoldLoadingScreen&) = delete;
    GoldLoadingScreen& operator=(const GoldLoadingScreen&) = delete;

    bool hasRetailAssets() const;
    int backgroundIndex() const;
    std::size_t recordCount() const;

    void renderToBuffer(int pixelW, int pixelH, std::string_view missionTitle,
                        float progress01, std::vector<std::uint8_t>& outRgba);
    void drawAndPresent(VulkanWindow& renderer, std::string_view missionTitle, float progress01);
    void clear(VulkanWindow& renderer);

private:
    struct Assets;
    void loadAssets(std::optional<int> fixedBgIndex);

    SDL_Window* window_{};
    std::filesystem::path gameDir_;
    std::unique_ptr<Assets> assets_;
    std::vector<std::uint8_t> rgba_;
};

enum class GoldMedalFaction : std::uint8_t {
    Soviet = 0,  // ZVEZDY[0] / ZV_GOLD[0..4] / ZV_M[0] (Red Star)
    Axis = 1,    // ZVEZDY[1] / ZV_GOLD[5..9] / ZV_M[1] (Iron Cross)
    Allied = 2,  // ZVEZDY[2] / ZV_GOLD[10..14] / ZV_M[2] (Allied Star)
};

struct GoldPostMissionStats {
    bool victory{false};
    std::string missionTitle;
    std::string commanderName{"YourID"};
    GoldMedalFaction faction{GoldMedalFaction::Allied};
    bool isRussianCampaign{false};
    bool isJapaneseCampaign{false};

    unsigned elapsedSeconds{0};
    unsigned enemyInfantryKilled{0};
    unsigned enemyVehiclesDestroyed{0};
    unsigned enemyTanksDestroyed{0};
    unsigned enemyGunsDestroyed{0};
    unsigned enemyPlanesShotDown{0};
    unsigned buildingsDemolished{0};
    unsigned airSortiesFlown{0};
    unsigned friendlyCasualties{0};

    int battleScore{0};
    int bestScore{0};
    bool isNewBest{false};
    int starRating{0};  // 0..10 retail Gold star rating

    bool hasNextCampaign{false};
    std::string nextCampaignTitle;
    bool canReplay{true};
    bool canBrowseCampaigns{true};
    bool canBrowseMissions{true};
    float sfxVolume{1.0f};
};

struct GoldPostMissionResult {
    bool consumed{false};
    bool closed{false};
    std::string activated;
};

// Authentic Sudden Strike II Gold 2.2 Post-Mission Statistics, Medals & Victory/Defeat screen.
// Composites retail menu_common.sue artwork (mainbk*.jpg / win_rus.pck / win_jap.pck backdrops,
// stat_pic.pck unit silhouettes, mstat_bt.pck / rs_bt.pck button geometry, ZVEZDY.PCK /
// ZV_GOLD.PCK / ZV_M.PCK faction medals & gold star ratings, and winwav / loosewav fanfare audio)
// onto an aspect-fitted 800x600 stage inside OverlaySlot::panel, with an asset-free fallback.
class GoldPostMissionScreen {
public:
    GoldPostMissionScreen(SDL_Window* window, const std::filesystem::path& gameDir,
                          std::optional<int> fixedBgIndex = std::nullopt);
    ~GoldPostMissionScreen();

    GoldPostMissionScreen(const GoldPostMissionScreen&) = delete;
    GoldPostMissionScreen& operator=(const GoldPostMissionScreen&) = delete;

    void open(GoldPostMissionStats stats, bool playFanfare = true);
    void close();
    bool active() const { return active_; }

    bool hasRetailAssets() const;
    bool hasMenuCursor() const;
    bool hasFanfareAudio() const;
    bool fanfarePlaying() const;
    void stopFanfare();

    const GoldPostMissionStats& stats() const { return stats_; }
    const std::string& focusedId() const { return focusedId_; }
    const std::string& hoveredId() const { return hoveredId_; }
    std::vector<std::string> actionIds() const;
    std::optional<std::pair<double, double>> actionStageCenter(std::string_view id) const;
    void setFocusedId(std::string_view id);

    GoldPostMissionResult handleEvent(const SDL_Event& event);
    std::string handleKey(SDL_Keycode key, bool* outClosed = nullptr);
    void handleMouseMove(double stageX, double stageY);
    std::string handleMouseDown(double stageX, double stageY);
    void handleMouseUp();

    void renderToBuffer(int pixelW, int pixelH, std::vector<std::uint8_t>& outRgba);
    void draw(VulkanWindow& renderer);
    void clear(VulkanWindow& renderer);

private:
    struct Assets;
    struct ActionButton;

    void loadAssets(std::optional<int> fixedBgIndex);
    void startFanfare(bool victory, float volume);
    std::vector<ActionButton> buildActionButtons() const;
    std::string hitTestStage(double sx, double sy) const;
    void cycleFocus(int step);

    SDL_Window* window_{};
    std::filesystem::path gameDir_;
    std::unique_ptr<Assets> assets_;
    GoldPostMissionStats stats_{};
    bool active_{false};
    std::string hoveredId_;
    std::string focusedId_;
    bool dirty_{true};
    int lastPixelW_{0};
    int lastPixelH_{0};
    std::vector<std::uint8_t> rgba_;
};

}
