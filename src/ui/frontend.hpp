#pragma once

#include "ui/menu.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ss2 {

// The Gold frontend navigation graph, decoupled from SDL rendering and game assets so it
// can be regression-tested headless. The caller feeds the mission/campaign browser entries,
// builds a Menu from items()/title(), shows briefingText() on the Briefing screen, and
// reacts to screen(): Launch means load launchPath(), Quit means exit.
class FrontEnd {
public:
    enum class Screen {
        MainMenu, SinglePlayer, SingleMissions, Campaigns, CampaignMissions,
        Briefing, Ratings, Credits, Launch, Quit
    };
    enum class RatingsMode { Campaigns, SingleMissions };

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

    Screen screen() const { return screen_; }
    std::string title() const;
    std::vector<MenuItem> items() const;

    std::size_t selected() const { return selected_; }
    void select(std::size_t index);
    const Entry* selectedMission() const;

    const std::vector<CampaignEntry>& campaigns() const { return campaigns_; }
    std::size_t selectedCampaign() const { return selectedCampaign_; }
    void selectCampaign(std::size_t index);
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
    std::size_t selected_ = 0;
    std::size_t selectedCampaign_ = 0;
    std::size_t selectedCampaignMission_ = 0;
    bool briefingFromCampaign_ = false;
    int difficulty_ = 2;
    RatingsMode ratingsMode_ = RatingsMode::Campaigns;
    std::string notice_;
};
}
