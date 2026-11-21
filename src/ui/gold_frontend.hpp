#pragma once

#include "ui/frontend.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
// plus the Gold Campaigns browser and campaign mission selector. Composites real
// menu_common.sue artwork + mission I-JPG1024 overviews onto an aspect-fitted 800x600
// stage inside OverlaySlot::panel so Briefing and Menu never overlap.
class GoldFrontEnd {
public:
    GoldFrontEnd(SDL_Window* window, const std::filesystem::path& gameDir,
                 std::optional<int> fixedBgIndex = std::nullopt);
    ~GoldFrontEnd();

    GoldFrontEnd(const GoldFrontEnd&) = delete;
    GoldFrontEnd& operator=(const GoldFrontEnd&) = delete;

    void setMissions(std::vector<GoldMissionEntry> missions);
    void setCampaigns(std::vector<GoldCampaignEntry> campaigns);
    void setDifficulty(int difficulty);
    int difficulty() const { return fe_.difficulty(); }

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
    std::string hoveredId() const { return hoveredId_; }
    std::string focusedId() const { return focusedId_; }
    bool hasMenuAssets() const;
    std::string activeNotice() const { return noticeText_; }

private:
    struct Assets;
    void loadAssets(std::optional<int> fixedBgIndex);
    void ensureFocusedValid();
    void syncNoticeFromState();
    void onScreenChanged(FrontEnd::Screen previous);
    std::size_t activeListCount() const;
    std::size_t activeListSelected() const;
    void selectActiveRow(std::size_t index);
    const char* activeRowPrefix() const;
    std::size_t previewCampaignIndex() const;
    void ensureSelectionVisible();
    void scrollBy(int deltaRows);
    std::string hitTestStage(double sx, double sy) const;
    void activateId(const std::string& id);
    void showNotice(std::string text);
    void ensureOverviewLoaded(const std::string& path);

    SDL_Window* window_{};
    std::filesystem::path gameDir_;
    FrontEnd fe_;
    std::vector<GoldMissionEntry> missions_;
    std::vector<GoldCampaignEntry> campaigns_;
    std::unique_ptr<Assets> assets_;

    FrontEnd::Screen lastSyncedScreen_{FrontEnd::Screen::MainMenu};
    std::string hoveredId_;
    std::string focusedId_{"single"};
    int scrollOffset_{0};
    double briefScroll_{0.0};
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

}
