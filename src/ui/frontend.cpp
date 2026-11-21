#include "ui/frontend.hpp"

#include "ui/strings.hpp"

#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

namespace ss2 {

void FrontEnd::setMissions(std::vector<Entry> missions) {
    missions_ = std::move(missions);
    if (selected_ >= missions_.size()) selected_ = 0;
}

void FrontEnd::select(std::size_t index) {
    selected_ = missions_.empty() ? 0 : (index < missions_.size() ? index : missions_.size() - 1);
}

const FrontEnd::Entry* FrontEnd::selectedMission() const {
    return selected_ < missions_.size() ? &missions_[selected_] : nullptr;
}

std::string FrontEnd::briefingText() const {
    const Entry* m = selectedMission();
    return m ? m->description : std::string{};
}

std::string FrontEnd::launchPath() const {
    const Entry* m = selectedMission();
    return m ? m->path : std::string{};
}

std::string FrontEnd::takeNotice() {
    std::string n = std::move(notice_);
    notice_.clear();
    return n;
}

std::string FrontEnd::title() const {
    switch (screen_) {
        case Screen::MainMenu: return tr("Main Menu");
        case Screen::SinglePlayer: return tr("Single Player");
        case Screen::SingleMissions: return tr("Single Missions");
        case Screen::Briefing: return tr("Mission Briefing");
        case Screen::Launch:
        case Screen::Quit: break;
    }
    return {};
}

std::vector<MenuItem> FrontEnd::items() const {
    std::vector<MenuItem> items;
    switch (screen_) {
        case Screen::MainMenu:
            items.push_back({"single", tr("Single Player"), "S", true});
            items.push_back({"multi", tr("Multiplayer"), "M", true});
            items.push_back({"options", tr("Options"), "O", true});
            items.push_back({"ratings", tr("Ratings"), "R", true});
            items.push_back({"replay", tr("Replay"), "P", true});
            items.push_back({"credits", tr("Credits"), "C", true});
            items.push_back({"quit", tr("Quit"), "Esc", true});
            break;
        case Screen::SinglePlayer:
            items.push_back({"missions", tr("Single Missions"), "S", true});
            items.push_back({"campaigns", tr("Campaigns"), "C", true});
            items.push_back({"load", tr("Load"), "L", true});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::SingleMissions:
            for (std::size_t i = 0; i < missions_.size(); ++i) {
                items.push_back({"mission:" + std::to_string(i), missions_[i].name, missions_[i].tag, true});
            }
            if (missions_.empty()) {
                items.push_back({"", tr("No missions installed."), "", false});
            }
            items.push_back({"start", tr("Start"), "Enter", !missions_.empty()});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Briefing:
            items.push_back({"start", tr("Start"), "S", true});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Launch:
        case Screen::Quit:
            break;
    }
    return items;
}

void FrontEnd::activate(const std::string& id) {
    if (id.rfind("mission:", 0) == 0) {
        std::size_t index = 0;
        const std::string_view digits(id.data() + 8, id.size() - 8);
        const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (res.ec == std::errc{} && index < missions_.size()) {
            select(index);
            screen_ = Screen::Briefing;
        }
        return;
    }
    switch (screen_) {
        case Screen::MainMenu:
            if (id == "single") screen_ = Screen::SinglePlayer;
            else if (id == "quit") screen_ = Screen::Quit;
            else if (id == "multi" || id == "options" || id == "ratings" ||
                     id == "replay" || id == "credits") notice_ = tr("Not implemented yet.");
            break;
        case Screen::SinglePlayer:
            if (id == "missions") { screen_ = Screen::SingleMissions; }
            else if (id == "cancel" || id == "back") screen_ = Screen::MainMenu;
            else if (id == "campaigns" || id == "load") notice_ = tr("Not implemented yet.");
            break;
        case Screen::SingleMissions:
            if (id == "start") {
                if (selectedMission()) screen_ = Screen::Briefing;
                else notice_ = tr("No missions installed.");
            } else if (id == "cancel" || id == "back") screen_ = Screen::SinglePlayer;
            break;
        case Screen::Briefing:
            if (id == "start") {
                if (selectedMission()) screen_ = Screen::Launch;
            } else if (id == "cancel" || id == "back") screen_ = Screen::SingleMissions;
            break;
        case Screen::Launch:
        case Screen::Quit:
            break;
    }
}

void FrontEnd::back() {
    switch (screen_) {
        case Screen::MainMenu: screen_ = Screen::Quit; break;
        case Screen::SinglePlayer: screen_ = Screen::MainMenu; break;
        case Screen::SingleMissions: screen_ = Screen::SinglePlayer; break;
        case Screen::Briefing: screen_ = Screen::SingleMissions; break;
        case Screen::Launch:
        case Screen::Quit: break;
    }
}

std::string FrontEnd::shortcutId(char letter) const {
    const char c = (letter >= 'A' && letter <= 'Z') ? char(letter - 'A' + 'a') : letter;
    switch (screen_) {
        case Screen::MainMenu:
            switch (c) {
                case 's': return "single";
                case 'm': return "multi";
                case 'o': return "options";
                case 'r': return "ratings";
                case 'p': return "replay";
                case 'c': return "credits";
                default: break;
            }
            break;
        case Screen::SinglePlayer:
            switch (c) {
                case 's': return "missions";
                case 'c': return "campaigns";
                case 'l': return "load";
                default: break;
            }
            break;
        case Screen::SingleMissions:
            break;
        case Screen::Briefing:
            if (c == 's') return "start";
            break;
        case Screen::Launch:
        case Screen::Quit:
            break;
    }
    return {};
}
}