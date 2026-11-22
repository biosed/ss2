#pragma once

#include "ui/menu.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ss2 {

// The Gold frontend navigation graph, decoupled from SDL rendering and game assets so it
// can be regression-tested headless. The caller feeds the mission/campaign/replay/save entries,
// builds a Menu from items()/title(), shows briefingText() on the Briefing screen, and
// reacts to screen(): Launch means load launchPath(), LaunchReplay means replay launchReplayPath(),
// LaunchSave means launch saved game at launchSavePath().
class FrontEnd {
public:
    enum class Screen {
        MainMenu, SinglePlayer, SingleMissions, Campaigns, CustomCampaigns, CampaignMissions,
        Briefing, Multiplayer, Options, Ratings, Replays, LoadGame, Credits,
        Launch, LaunchReplay, LaunchSave, LaunchMultiplayer, Quit
    };
    enum class RatingsMode { Campaigns, SingleMissions };

    struct ReplayEntry {
        std::string name;
        std::string path;
        std::string mission;
        std::string hashHex;
        std::size_t commandCount{};
        std::uint32_t endTick{};
        int difficulty{2};
        bool autoEngage{true};
    };

    struct SaveEntry {
        std::string name;
        std::string path;
        std::string mission;
        std::string missionTitle;
        std::string slotLabel;
        std::string timestamp;
        std::uint32_t tick{};
        std::uint32_t playerSoldiers{};
        std::uint32_t playerVehicles{};
        int difficulty{2};
        std::uint64_t fileBytes{};
    };

    struct Entry {
        std::string name, path, description, tag;
    };

    struct CampaignMissionEntry {
        std::string code;
        std::string name;
        std::string path;
        std::string easyPath;
        std::string description;
        std::string tag;
    };

    struct CampaignEntry {
        std::string name;
        std::vector<CampaignMissionEntry> missions;
    };

    void setMissions(std::vector<Entry> missions);
    void setCampaigns(std::vector<CampaignEntry> campaigns);
    void setCustomCampaigns(std::vector<CampaignEntry> campaigns);
    void setReplays(std::vector<ReplayEntry> replays);
    void setSaves(std::vector<SaveEntry> saves);
    void setMultiplayerMissions(std::vector<Entry> missions);

    Screen screen() const { return screen_; }
    std::string title() const;
    std::vector<MenuItem> items() const;

    std::size_t selected() const { return selected_; }
    void select(std::size_t index);
    const Entry* selectedMission() const;

    const std::vector<CampaignEntry>& campaigns() const { return campaigns_; }
    std::size_t selectedCampaign() const { return selectedCampaign_; }
    void selectCampaign(std::size_t index);
    const std::vector<CampaignEntry>& customCampaigns() const { return customCampaigns_; }
    std::size_t selectedCustomCampaign() const { return selectedCustomCampaign_; }
    void selectCustomCampaign(std::size_t index);
    bool inCustomCampaign() const { return inCustomCampaign_; }
    std::size_t selectedCampaignMission() const { return selectedCampaignMission_; }
    void selectCampaignMission(std::size_t index);
    const CampaignEntry* currentCampaign() const;
    const CampaignMissionEntry* selectedCampaignMissionEntry() const;
    bool briefingFromCampaign() const { return briefingFromCampaign_; }

    int difficulty() const { return difficulty_; }
    void setDifficulty(int difficulty);
    void cycleDifficulty(int delta = 1);

    RatingsMode ratingsMode() const { return ratingsMode_; }
    void setRatingsMode(RatingsMode mode) { ratingsMode_ = mode; }
    void toggleRatingsMode() {
        ratingsMode_ = (ratingsMode_ == RatingsMode::Campaigns)
            ? RatingsMode::SingleMissions : RatingsMode::Campaigns;
    }

    std::string briefingText() const;
    std::string launchPath() const;

    const std::vector<ReplayEntry>& replays() const { return replays_; }
    std::size_t selectedReplay() const { return selectedReplay_; }
    void selectReplay(std::size_t index);
    const ReplayEntry* selectedReplayEntry() const;
    std::string launchReplayPath() const;

    const std::vector<SaveEntry>& saves() const { return saves_; }
    std::size_t selectedSave() const { return selectedSave_; }
    void selectSave(std::size_t index);
    const SaveEntry* selectedSaveEntry() const;
    std::string launchSavePath() const;

    const std::vector<Entry>& multiplayerMissions() const { return mpMissions_; }
    std::size_t selectedMultiplayerMission() const { return selectedMpMission_; }
    void selectMultiplayerMission(std::size_t index);
    const Entry* selectedMultiplayerMissionEntry() const;
    std::string launchMultiplayerPath() const;
    bool mpIsHost() const { return mpIsHost_; }
    void setMpIsHost(bool isHost) { mpIsHost_ = isHost; }
    void toggleMpMode() { mpIsHost_ = !mpIsHost_; }
    const std::string& mpHostAddress() const { return mpHostAddress_; }
    void setMpHostAddress(std::string addr) { mpHostAddress_ = std::move(addr); }
    std::uint16_t mpPort() const { return mpPort_; }
    void setMpPort(std::uint16_t port) { if (port > 0) mpPort_ = port; }
    const std::string& mpStatusText() const { return mpStatusText_; }
    void setMpStatusText(std::string text) { mpStatusText_ = std::move(text); }

    // A deferred branch was chosen; the message stays on the current screen. Cleared on read.
    std::string takeNotice();

    void activate(const std::string& id);
    void back();   // Escape / Cancel: walk one screen up the stack

    // The row id an advertised letter shortcut activates on the current screen, or empty.
    std::string shortcutId(char letter) const;

private:
    Screen screen_ = Screen::MainMenu;
    std::vector<Entry> missions_;
    std::vector<CampaignEntry> campaigns_;
    std::vector<CampaignEntry> customCampaigns_;
    std::vector<ReplayEntry> replays_;
    std::vector<SaveEntry> saves_;
    std::vector<Entry> mpMissions_;
    std::size_t selected_ = 0;
    std::size_t selectedCampaign_ = 0;
    std::size_t selectedCustomCampaign_ = 0;
    std::size_t selectedCampaignMission_ = 0;
    std::size_t selectedReplay_ = 0;
    std::size_t selectedSave_ = 0;
    std::size_t selectedMpMission_ = 0;
    bool inCustomCampaign_ = false;
    bool briefingFromCampaign_ = false;
    bool mpIsHost_ = true;
    std::string mpHostAddress_ = "127.0.0.1";
    std::uint16_t mpPort_ = 27015;
    std::string mpStatusText_;
    int difficulty_ = 2;
    RatingsMode ratingsMode_ = RatingsMode::Campaigns;
    std::string notice_;
};
}
