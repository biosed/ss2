#include "ui/frontend.hpp"

#include "ui/strings.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

namespace ss2 {
namespace {

std::string toLowerAscii(std::string_view sv) {
    std::string out(sv);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool parseTailIndex(std::string_view id, std::string_view prefix, std::size_t& out) {
    if (!id.starts_with(prefix)) return false;
    const std::string_view digits = id.substr(prefix.size());
    if (digits.empty()) return false;
    const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), out);
    return res.ec == std::errc{} && res.ptr == digits.data() + digits.size();
}

char campaignShortcutChar(std::string_view name) {
    const std::string low = toLowerAscii(name);
    if (low.find("german") != std::string::npos || low == "de") return 'G';
    if (low.find("japan") != std::string::npos) return 'J';
    if (low.find("brit") != std::string::npos || low == "gb" || low == "uk") return 'B';
    if (low == "us" || low.starts_with("us ") || low.find("usa") != std::string::npos ||
        low.find("american") != std::string::npos || low.find("u.s.") != std::string::npos) return 'U';
    if (low.find("russ") != std::string::npos || low.find("ussr") != std::string::npos ||
        low.find("soviet") != std::string::npos) return 'R';
    return '\0';
}

const char* difficultyKey(int difficulty) {
    return difficulty == 1 ? "Easy" : difficulty == 3 ? "Hard" : "Normal";
}

}  // namespace

void FrontEnd::setMissions(std::vector<Entry> missions) {
    missions_ = std::move(missions);
    if (selected_ >= missions_.size()) selected_ = 0;
}

void FrontEnd::setCampaigns(std::vector<CampaignEntry> campaigns) {
    campaigns_ = std::move(campaigns);
    if (selectedCampaign_ >= campaigns_.size()) selectedCampaign_ = 0;
    if (const auto* c = currentCampaign(); !c || selectedCampaignMission_ >= c->missions.size()) {
        selectedCampaignMission_ = 0;
    }
}

void FrontEnd::setReplays(std::vector<ReplayEntry> replays) {
    replays_ = std::move(replays);
    if (selectedReplay_ >= replays_.size()) selectedReplay_ = 0;
}

void FrontEnd::selectReplay(std::size_t index) {
    selectedReplay_ = replays_.empty() ? 0 : (index < replays_.size() ? index : replays_.size() - 1);
}

const FrontEnd::ReplayEntry* FrontEnd::selectedReplayEntry() const {
    return selectedReplay_ < replays_.size() ? &replays_[selectedReplay_] : nullptr;
}

std::string FrontEnd::launchReplayPath() const {
    const auto* r = selectedReplayEntry();
    return r ? r->path : std::string{};
}

void FrontEnd::setSaves(std::vector<SaveEntry> saves) {
    saves_ = std::move(saves);
    if (saves_.empty()) selectedSave_ = 0;
    else if (selectedSave_ >= saves_.size()) selectedSave_ = saves_.size() - 1;
}

void FrontEnd::selectSave(std::size_t index) {
    selectedSave_ = saves_.empty() ? 0 : (index < saves_.size() ? index : saves_.size() - 1);
}

const FrontEnd::SaveEntry* FrontEnd::selectedSaveEntry() const {
    return selectedSave_ < saves_.size() ? &saves_[selectedSave_] : nullptr;
}

std::string FrontEnd::launchSavePath() const {
    const auto* s = selectedSaveEntry();
    return s ? s->path : std::string{};
}

void FrontEnd::setMultiplayerMissions(std::vector<Entry> missions) {
    mpMissions_ = std::move(missions);
    if (selectedMpMission_ >= mpMissions_.size()) selectedMpMission_ = 0;
}

void FrontEnd::selectMultiplayerMission(std::size_t index) {
    if (index < mpMissions_.size()) selectedMpMission_ = index;
}

const FrontEnd::Entry* FrontEnd::selectedMultiplayerMissionEntry() const {
    return selectedMpMission_ < mpMissions_.size() ? &mpMissions_[selectedMpMission_] : nullptr;
}

std::string FrontEnd::launchMultiplayerPath() const {
    const auto* m = selectedMultiplayerMissionEntry();
    return m ? m->path : std::string{};
}

void FrontEnd::select(std::size_t index) {
    selected_ = missions_.empty() ? 0 : (index < missions_.size() ? index : missions_.size() - 1);
}

const FrontEnd::Entry* FrontEnd::selectedMission() const {
    return selected_ < missions_.size() ? &missions_[selected_] : nullptr;
}

void FrontEnd::selectCampaign(std::size_t index) {
    if (campaigns_.empty()) {
        selectedCampaign_ = 0;
        selectedCampaignMission_ = 0;
        return;
    }
    const std::size_t clamped = index < campaigns_.size() ? index : campaigns_.size() - 1;
    if (clamped != selectedCampaign_) {
        selectedCampaign_ = clamped;
        selectedCampaignMission_ = 0;
    } else if (const auto* c = currentCampaign(); !c || selectedCampaignMission_ >= c->missions.size()) {
        selectedCampaignMission_ = 0;
    }
}

void FrontEnd::selectCampaignMission(std::size_t index) {
    const auto* c = currentCampaign();
    if (!c || c->missions.empty()) {
        selectedCampaignMission_ = 0;
        return;
    }
    selectedCampaignMission_ = index < c->missions.size() ? index : c->missions.size() - 1;
}

const FrontEnd::CampaignEntry* FrontEnd::currentCampaign() const {
    return selectedCampaign_ < campaigns_.size() ? &campaigns_[selectedCampaign_] : nullptr;
}

const FrontEnd::CampaignMissionEntry* FrontEnd::selectedCampaignMissionEntry() const {
    const auto* c = currentCampaign();
    return (c && selectedCampaignMission_ < c->missions.size()) ? &c->missions[selectedCampaignMission_] : nullptr;
}

void FrontEnd::setDifficulty(int difficulty) {
    difficulty_ = std::clamp(difficulty, 1, 3);
}

void FrontEnd::cycleDifficulty(int delta) {
    const int offset = ((difficulty_ - 1) + (delta % 3) + 3) % 3;
    difficulty_ = 1 + offset;
}

std::string FrontEnd::briefingText() const {
    const bool useCampaign = (screen_ == Screen::Campaigns || screen_ == Screen::CampaignMissions || briefingFromCampaign_);
    if (useCampaign) {
        const auto* m = selectedCampaignMissionEntry();
        return m ? m->description : std::string{};
    }
    const Entry* m = selectedMission();
    return m ? m->description : std::string{};
}

std::string FrontEnd::launchPath() const {
    const bool useCampaign = (screen_ == Screen::Campaigns || screen_ == Screen::CampaignMissions || briefingFromCampaign_);
    if (useCampaign) {
        const auto* m = selectedCampaignMissionEntry();
        if (!m) return {};
        return (difficulty_ == 1 && !m->easyPath.empty()) ? m->easyPath : m->path;
    }
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
        case Screen::Campaigns: return tr("Campaigns");
        case Screen::CampaignMissions:
            return currentCampaign() ? currentCampaign()->name : tr("Campaigns");
        case Screen::Briefing: return tr("Mission Briefing");
        case Screen::Options: return tr("Options");
        case Screen::Ratings: return tr("Ratings");
        case Screen::Replays: return tr("Replay Game");
        case Screen::LoadGame: return tr("Load Game");
        case Screen::Multiplayer: return tr("Multiplayer");
        case Screen::Credits: return tr("Credits");
        case Screen::Launch:
        case Screen::LaunchReplay:
        case Screen::LaunchSave:
        case Screen::LaunchMultiplayer:
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
        case Screen::Campaigns:
            for (std::size_t i = 0; i < campaigns_.size(); ++i) {
                const char sc = campaignShortcutChar(campaigns_[i].name);
                const std::string tag = sc ? std::string(1, sc) : std::string{};
                items.push_back({"campaign:" + std::to_string(i), campaigns_[i].name, tag, true});
            }
            if (campaigns_.empty()) {
                items.push_back({"", tr("No campaigns installed."), "", false});
            }
            items.push_back({"custom", tr("Custom"), "C", true});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::CampaignMissions: {
            const auto* camp = currentCampaign();
            const bool hasMissions = camp && !camp->missions.empty();
            if (hasMissions) {
                for (std::size_t i = 0; i < camp->missions.size(); ++i) {
                    items.push_back({"cmission:" + std::to_string(i), camp->missions[i].name, camp->missions[i].tag, true});
                }
            } else {
                items.push_back({"", tr("No missions installed."), "", false});
            }
            items.push_back({"difficulty", tr("Difficulty"), tr(difficultyKey(difficulty_)), true});
            items.push_back({"start", tr("Start"), "S", hasMissions});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        }
        case Screen::Briefing:
            items.push_back({"start", tr("Start"), "S", true});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Options:
            items.push_back({"ok", tr("OK"), "Enter", true});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Ratings:
            items.push_back({"ratings_campaigns", tr("Campaigns"), "C", true});
            items.push_back({"ratings_missions", tr("Single Missions"), "S", true});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Replays:
            for (std::size_t i = 0; i < replays_.size(); ++i) {
                items.push_back({"replay:" + std::to_string(i), replays_[i].name,
                                 std::to_string(replays_[i].commandCount) + " cmd", true});
            }
            if (replays_.empty()) {
                items.push_back({"", tr("No replays recorded yet."), "", false});
            }
            items.push_back({"replay_start", tr("Replay"), "R", !replays_.empty()});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::LoadGame:
            for (std::size_t i = 0; i < saves_.size(); ++i) {
                items.push_back({"save:" + std::to_string(i), saves_[i].name,
                                 saves_[i].slotLabel, true});
            }
            if (saves_.empty()) {
                items.push_back({"", tr("No saved games yet."), "", false});
            }
            items.push_back({"load_start", tr("Load"), "L", !saves_.empty()});
            items.push_back({"load_delete", tr("Delete"), "D", !saves_.empty()});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Multiplayer:
            items.push_back({"mp_mode", tr(mpIsHost_ ? "Host Match" : "Join Match"), "M", true});
            if (mpMissions_.empty()) {
                items.push_back({"mp_none", tr("No multiplayer maps installed."), "", false});
            } else {
                for (std::size_t i = 0; i < mpMissions_.size(); ++i) {
                    items.push_back({"mpmap:" + std::to_string(i), mpMissions_[i].name, "", true});
                }
            }
            items.push_back({"mp_start", tr("Start"), "S", !mpIsHost_ || !mpMissions_.empty()});
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Credits:
            items.push_back({"cancel", tr("Cancel"), "Esc", true});
            break;
        case Screen::Launch:
        case Screen::LaunchReplay:
        case Screen::LaunchSave:
        case Screen::LaunchMultiplayer:
        case Screen::Quit:
            break;
    }
    return items;
}

void FrontEnd::activate(const std::string& id) {
    if (std::size_t index = 0; parseTailIndex(id, "mission:", index)) {
        if (index < missions_.size()) {
            select(index);
            briefingFromCampaign_ = false;
            screen_ = Screen::Briefing;
        }
        return;
    }
    if (std::size_t index = 0; parseTailIndex(id, "campaign:", index)) {
        if (index < campaigns_.size()) {
            selectCampaign(index);
            screen_ = Screen::CampaignMissions;
        }
        return;
    }
    if (std::size_t index = 0; parseTailIndex(id, "cmission:", index)) {
        if (const auto* camp = currentCampaign(); camp && index < camp->missions.size()) {
            selectCampaignMission(index);
            briefingFromCampaign_ = true;
            screen_ = Screen::Briefing;
        }
        return;
    }
    if (std::size_t index = 0; parseTailIndex(id, "replay:", index)) {
        if (index < replays_.size()) {
            selectReplay(index);
            screen_ = Screen::LaunchReplay;
        }
        return;
    }
    if (std::size_t index = 0; parseTailIndex(id, "save:", index)) {
        if (index < saves_.size()) {
            selectSave(index);
            screen_ = Screen::LaunchSave;
        }
        return;
    }
    if (std::size_t index = 0; screen_ == Screen::Multiplayer && parseTailIndex(id, "mpmap:", index)) {
        if (index < mpMissions_.size()) {
            selectMultiplayerMission(index);
        }
        return;
    }

    switch (screen_) {
        case Screen::MainMenu:
            if (id == "single") screen_ = Screen::SinglePlayer;
            else if (id == "options") screen_ = Screen::Options;
            else if (id == "ratings") screen_ = Screen::Ratings;
            else if (id == "replay") screen_ = Screen::Replays;
            else if (id == "credits") screen_ = Screen::Credits;
            else if (id == "quit") screen_ = Screen::Quit;
            else if (id == "multi") screen_ = Screen::Multiplayer;
            break;
        case Screen::SinglePlayer:
            if (id == "missions") {
                briefingFromCampaign_ = false;
                screen_ = Screen::SingleMissions;
            } else if (id == "campaigns") {
                screen_ = Screen::Campaigns;
            } else if (id == "cancel" || id == "back") {
                screen_ = Screen::MainMenu;
            } else if (id == "load") {
                screen_ = Screen::LoadGame;
            }
            break;
        case Screen::SingleMissions:
            if (id == "start") {
                if (selectedMission()) {
                    briefingFromCampaign_ = false;
                    screen_ = Screen::Briefing;
                } else {
                    notice_ = tr("No missions installed.");
                }
            } else if (id == "cancel" || id == "back") {
                screen_ = Screen::SinglePlayer;
            }
            break;
        case Screen::Campaigns:
            if (id == "custom") notice_ = tr("Not implemented yet.");
            else if (id == "cancel" || id == "back") screen_ = Screen::SinglePlayer;
            break;
        case Screen::CampaignMissions:
            if (id == "difficulty") {
                cycleDifficulty(1);
            } else if (id == "start") {
                if (selectedCampaignMissionEntry()) {
                    briefingFromCampaign_ = true;
                    screen_ = Screen::Briefing;
                } else {
                    notice_ = tr("No missions installed.");
                }
            } else if (id == "cancel" || id == "back") {
                screen_ = Screen::Campaigns;
            }
            break;
        case Screen::Briefing:
            if (id == "start") {
                const bool valid = briefingFromCampaign_ ? (selectedCampaignMissionEntry() != nullptr)
                                                         : (selectedMission() != nullptr);
                if (valid) screen_ = Screen::Launch;
            } else if (id == "cancel" || id == "back") {
                screen_ = briefingFromCampaign_ ? Screen::CampaignMissions : Screen::SingleMissions;
            }
            break;
        case Screen::Options:
            if (id == "ok" || id == "cancel" || id == "back") screen_ = Screen::MainMenu;
            break;
        case Screen::Ratings:
            if (id == "ratings_campaigns") ratingsMode_ = RatingsMode::Campaigns;
            else if (id == "ratings_missions") ratingsMode_ = RatingsMode::SingleMissions;
            else if (id == "ratings_toggle") toggleRatingsMode();
            else if (id == "cancel" || id == "back") screen_ = Screen::MainMenu;
            break;
        case Screen::Replays:
            if (id == "replay_start") {
                if (selectedReplayEntry()) screen_ = Screen::LaunchReplay;
                else notice_ = tr("No replays recorded yet.");
            } else if (id == "cancel" || id == "back") {
                screen_ = Screen::MainMenu;
            }
            break;
        case Screen::LoadGame:
            if (id == "load_start") {
                if (selectedSaveEntry()) screen_ = Screen::LaunchSave;
                else notice_ = tr("No saved games yet.");
            } else if (id == "load_delete") {
                if (!selectedSaveEntry()) notice_ = tr("No saved games yet.");
            } else if (id == "cancel" || id == "back") {
                screen_ = Screen::SinglePlayer;
            }
            break;
        case Screen::Credits:
            if (id == "cancel" || id == "back") screen_ = Screen::MainMenu;
            break;
        case Screen::Multiplayer:
            if (id == "mp_mode") {
                toggleMpMode();
            } else if (id == "mp_start") {
                if (mpIsHost_ && mpMissions_.empty()) {
                    notice_ = tr("No multiplayer maps installed.");
                } else if (!mpIsHost_ && mpHostAddress_.empty()) {
                    notice_ = tr("Enter address");
                } else {
                    screen_ = Screen::LaunchMultiplayer;
                }
            } else if (id == "cancel" || id == "back") {
                screen_ = Screen::MainMenu;
            }
            break;
        case Screen::Launch:
        case Screen::LaunchReplay:
        case Screen::LaunchSave:
        case Screen::LaunchMultiplayer:
        case Screen::Quit:
            break;
    }
}

void FrontEnd::back() {
    switch (screen_) {
        case Screen::MainMenu: screen_ = Screen::Quit; break;
        case Screen::SinglePlayer: screen_ = Screen::MainMenu; break;
        case Screen::SingleMissions: screen_ = Screen::SinglePlayer; break;
        case Screen::Campaigns: screen_ = Screen::SinglePlayer; break;
        case Screen::CampaignMissions: screen_ = Screen::Campaigns; break;
        case Screen::Briefing:
            screen_ = briefingFromCampaign_ ? Screen::CampaignMissions : Screen::SingleMissions;
            break;
        case Screen::LoadGame:
            screen_ = Screen::SinglePlayer;
            break;
        case Screen::LaunchSave:
            screen_ = Screen::LoadGame;
            break;
        case Screen::Options:
        case Screen::Ratings:
        case Screen::Replays:
        case Screen::Multiplayer:
        case Screen::Credits:
            screen_ = Screen::MainMenu;
            break;
        case Screen::LaunchMultiplayer:
            screen_ = Screen::Multiplayer;
            break;
        case Screen::Launch:
        case Screen::LaunchReplay:
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
        case Screen::Campaigns: {
            if (c == 'c') return "custom";
            const char target = char(c - 'a' + 'A');
            for (std::size_t i = 0; i < campaigns_.size(); ++i) {
                if (campaignShortcutChar(campaigns_[i].name) == target) {
                    return "campaign:" + std::to_string(i);
                }
            }
            break;
        }
        case Screen::CampaignMissions:
            if (c == 'd') return "difficulty";
            if (c == 's') return "start";
            break;
        case Screen::Briefing:
            if (c == 's') return "start";
            break;
        case Screen::Ratings:
            if (c == 'c') return "ratings_campaigns";
            if (c == 's') return "ratings_missions";
            break;
        case Screen::Replays:
            if (c == 'r') return "replay_start";
            break;
        case Screen::LoadGame:
            if (c == 'l') return "load_start";
            if (c == 'd') return "load_delete";
            break;
        case Screen::Multiplayer:
            if (c == 'm') return "mp_mode";
            if (c == 's') return "mp_start";
            break;
        case Screen::Options:
        case Screen::Credits:
        case Screen::Launch:
        case Screen::LaunchReplay:
        case Screen::LaunchSave:
        case Screen::LaunchMultiplayer:
        case Screen::Quit:
            break;
    }
    return {};
}
}
