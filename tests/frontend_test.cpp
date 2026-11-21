// Gold frontend navigation graph: the verified Tutorial I path to Start, Back/Esc,
// cancel-briefing-starts-no-mission, switch-mission, Campaigns + Difficulty routing,
// quit and deferred branches. Asset-free.
#include "ui/frontend.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
bool hasId(const std::vector<ss2::MenuItem>& items, const std::string& id) {
    for (const auto& it : items) if (it.id == id) return true;
    return false;
}
std::vector<ss2::FrontEnd::Entry> missions() {
    return {
        {"Tutorial I", "/game/maps/single/--- TUTORIAL I ---.SSM", "Tutorial mission #1.", "128x128 Summer"},
        {"V2", "/game/maps/single/V2.ssm", "V2 description.", "384x384 Summer"},
    };
}
std::vector<ss2::FrontEnd::CampaignEntry> campaigns() {
    return {
        {
            "German Campaign",
            {
                {"023000", "1. Starting Position", "/game/maps/campaign/023000.ssc",
                 "/game/maps/campaign/EASY/023000.ssc", "Kharkiv advance.", "256x256 Summer"},
                {"023001", "2. The Approach to Tchutovo", "/game/maps/campaign/023001.ssc",
                 "/game/maps/campaign/EASY/023001.ssc", "6th Panzer support.", "256x256 Summer"},
            },
        },
        {
            "Japanese Campaign",
            {
                {"002000", "1. Omen of Victory", "/game/maps/campaign/002000.ssc",
                 "/game/maps/campaign/EASY/002000.ssc", "Rescue pilots.", "256x256 Beach"},
            },
        },
        {
            "British Campaign",
            {
                {"030000", "1. Leopard - Unexpected Circumstances", "/game/maps/campaign/030000.ssc",
                 "/game/maps/campaign/EASY/030000.ssc", "Counter-offensive.", "256x256 Summer"},
            },
        },
        {
            "US Campaign",
            {
                {"016000", "1. The Assault", "/game/maps/campaign/016000.ssc",
                 "/game/maps/campaign/EASY/016000.ssc", "Cross the Rhine.", "128x128 Summer"},
            },
        },
        {
            "Russian Campaign",
            {
                {"000000", "1. Revenge", "/game/maps/campaign/000000.ssc",
                 "/game/maps/campaign/EASY/000000.ssc", "Capture Berlin.", "128x128 Summer"},
            },
        },
    };
}
}

int main() {
    using namespace ss2;
    try {
        FrontEnd fe;
        check(fe.screen() == FrontEnd::Screen::MainMenu, "ordinary launch opens the Main Menu");
        check(hasId(fe.items(), "single") && hasId(fe.items(), "quit"), "Main Menu offers Single Player and Quit");

        // Verified path: Main Menu -> Single Player -> Single Missions -> Tutorial I -> Briefing -> Start.
        fe.activate("single");
        check(fe.screen() == FrontEnd::Screen::SinglePlayer, "Single Player opens Screen 2");
        check(hasId(fe.items(), "missions"), "Single Player offers Single Missions");
        fe.activate("missions");
        check(fe.screen() == FrontEnd::Screen::SingleMissions, "Single Missions opens Screen 3");

        fe.setMissions(missions());
        check(hasId(fe.items(), "mission:0") && hasId(fe.items(), "mission:1"), "mission browser rows are listed");
        check(hasId(fe.items(), "start") && hasId(fe.items(), "cancel"), "Screen 3 offers Start and Cancel");

        fe.activate("mission:0");
        check(fe.screen() == FrontEnd::Screen::Briefing, "choosing Tutorial I opens the Briefing");
        check(fe.selected() == 0, "Tutorial I is selected");
        check(fe.briefingText() == "Tutorial mission #1.", "briefing shows the mission description");
        check(hasId(fe.items(), "start") && hasId(fe.items(), "cancel"), "Briefing offers Start and Cancel");

        fe.activate("start");
        check(fe.screen() == FrontEnd::Screen::Launch, "Briefing Start launches");
        check(fe.launchPath() == "/game/maps/single/--- TUTORIAL I ---.SSM", "Start loads Tutorial I");

        // Cancelling the briefing starts no mission and returns to Single Missions.
        FrontEnd cancel;
        cancel.setMissions(missions());
        cancel.activate("single");
        cancel.activate("missions");
        cancel.activate("mission:0");
        check(cancel.screen() == FrontEnd::Screen::Briefing, "reached briefing before cancel");
        cancel.activate("cancel");
        check(cancel.screen() == FrontEnd::Screen::SingleMissions, "Cancel returns to Single Missions");
        check(cancel.launchPath().empty() || cancel.screen() != FrontEnd::Screen::Launch,
              "cancelling the briefing starts no mission");

        // Escape from the briefing also starts no mission.
        FrontEnd esc;
        esc.setMissions(missions());
        esc.activate("single");
        esc.activate("missions");
        esc.activate("mission:0");
        esc.back();
        check(esc.screen() == FrontEnd::Screen::SingleMissions, "Esc from briefing returns to Single Missions");
        check(esc.screen() != FrontEnd::Screen::Launch, "Esc from briefing starts no mission");

        // Selecting a different mission loads that mission.
        FrontEnd sw;
        sw.setMissions(missions());
        sw.activate("single");
        sw.activate("missions");
        sw.activate("mission:1");
        check(sw.selected() == 1 && sw.briefingText() == "V2 description.", "V2 is selected with its own briefing");
        sw.activate("start");
        check(sw.screen() == FrontEnd::Screen::Launch, "V2 briefing Start launches");
        check(sw.launchPath() == "/game/maps/single/V2.ssm", "switching mission loads V2, not Tutorial I");

        // The Start row uses the highlighted mission (keyboard navigation).
        FrontEnd kb;
        kb.setMissions(missions());
        kb.activate("single");
        kb.activate("missions");
        kb.select(1);
        kb.activate("start");
        check(kb.screen() == FrontEnd::Screen::Briefing && kb.selected() == 1,
              "Start opens the briefing for the highlighted mission");
        kb.activate("start");
        check(kb.launchPath() == "/game/maps/single/V2.ssm", "highlighted Start loads V2");

        // Back/Esc walks up the stack; Main Menu Esc/Quit exits.
        FrontEnd nav;
        nav.activate("single");
        nav.activate("missions");
        nav.back();
        check(nav.screen() == FrontEnd::Screen::SinglePlayer, "Esc from Single Missions -> Single Player");
        nav.back();
        check(nav.screen() == FrontEnd::Screen::MainMenu, "Esc from Single Player -> Main Menu");
        nav.back();
        check(nav.screen() == FrontEnd::Screen::Quit, "Esc from Main Menu quits");
        FrontEnd quit;
        quit.activate("quit");
        check(quit.screen() == FrontEnd::Screen::Quit, "Quit row exits");

        // Deferred branches stay put and report a notice.
        FrontEnd def;
        def.activate("multi");
        check(def.screen() == FrontEnd::Screen::MainMenu, "Multiplayer is deferred, stays on Main Menu");
        check(!def.takeNotice().empty(), "deferred branch reports a notice");
        check(def.takeNotice().empty(), "notice is cleared after reading");
        def.activate("options");
        check(def.screen() == FrontEnd::Screen::MainMenu && !def.takeNotice().empty(),
              "Options is deferred and reports a notice");
        def.activate("replay");
        check(def.screen() == FrontEnd::Screen::MainMenu && !def.takeNotice().empty(),
              "Replay is deferred and reports a notice");
        def.activate("single");
        def.activate("load");
        check(def.screen() == FrontEnd::Screen::SinglePlayer, "Load is deferred, stays on Single Player");
        check(!def.takeNotice().empty(), "Load reports a notice");

        // Ratings (R) and Credits (C) screens, tab toggling, and Cancel/Esc return.
        {
            FrontEnd rc;
            rc.setMissions(missions());
            rc.setCampaigns(campaigns());
            check(rc.shortcutId('r') == "ratings" && rc.shortcutId('c') == "credits",
                  "Main Menu advertises R for Ratings and C for Credits");

            rc.activate(rc.shortcutId('r'));
            check(rc.screen() == FrontEnd::Screen::Ratings && rc.title() == "Ratings",
                  "R opens Ratings screen");
            check(rc.ratingsMode() == FrontEnd::RatingsMode::Campaigns,
                  "Ratings opens in Campaigns mode by default");
            check(rc.shortcutId('s') == "ratings_missions" && rc.shortcutId('c') == "ratings_campaigns",
                  "Ratings advertises C and S shortcuts");
            rc.activate(rc.shortcutId('s'));
            check(rc.ratingsMode() == FrontEnd::RatingsMode::SingleMissions,
                  "S switches Ratings to Single Missions");
            rc.activate("ratings_toggle");
            check(rc.ratingsMode() == FrontEnd::RatingsMode::Campaigns,
                  "ratings_toggle toggles back to Campaigns");
            rc.activate("cancel");
            check(rc.screen() == FrontEnd::Screen::MainMenu, "Cancel returns from Ratings to Main Menu");

            rc.activate("ratings");
            check(rc.screen() == FrontEnd::Screen::Ratings, "Ratings reopened");
            rc.back();
            check(rc.screen() == FrontEnd::Screen::MainMenu, "Esc returns from Ratings to Main Menu");

            rc.activate(rc.shortcutId('c'));
            check(rc.screen() == FrontEnd::Screen::Credits && rc.title() == "Credits",
                  "C opens Credits screen");
            check(hasId(rc.items(), "cancel"), "Credits offers Cancel");
            rc.activate("cancel");
            check(rc.screen() == FrontEnd::Screen::MainMenu, "Cancel returns from Credits to Main Menu");
            rc.activate("credits");
            rc.back();
            check(rc.screen() == FrontEnd::Screen::MainMenu, "Esc returns from Credits to Main Menu");
        }

        // Campaigns path, shortcuts (G/J/B/U/R/C/D/S), Easy/Normal/Hard routing, and stack back.
        FrontEnd camp;
        camp.setMissions(missions());
        camp.setCampaigns(campaigns());
        camp.activate(camp.shortcutId('s'));
        check(camp.screen() == FrontEnd::Screen::SinglePlayer, "Main Menu S -> Single Player");
        camp.activate(camp.shortcutId('c'));
        check(camp.screen() == FrontEnd::Screen::Campaigns, "Single Player C -> Campaigns");
        check(camp.shortcutId('g') == "campaign:0" && camp.shortcutId('j') == "campaign:1" &&
              camp.shortcutId('b') == "campaign:2" && camp.shortcutId('u') == "campaign:3" &&
              camp.shortcutId('r') == "campaign:4" && camp.shortcutId('c') == "custom",
              "Campaigns nation + Custom shortcuts resolve accurately without US/Russian collision");
        camp.activate(camp.shortcutId('c'));
        check(camp.screen() == FrontEnd::Screen::Campaigns && !camp.takeNotice().empty(),
              "Custom stays on Campaigns and emits a notice");
        camp.activate(camp.shortcutId('g'));
        check(camp.screen() == FrontEnd::Screen::CampaignMissions && camp.selectedCampaign() == 0,
              "G opens German Campaign missions");
        camp.selectCampaignMission(1);
        check(camp.difficulty() == 2, "Default difficulty is Normal (2)");
        check(camp.launchPath() == "/game/maps/campaign/023001.ssc", "Normal difficulty uses standard .ssc");

        // Cycle difficulty Normal -> Hard -> Easy via 'd' shortcut.
        camp.activate(camp.shortcutId('d'));
        check(camp.difficulty() == 3 && camp.launchPath() == "/game/maps/campaign/023001.ssc",
              "Hard difficulty uses standard .ssc");
        camp.activate(camp.shortcutId('d'));
        check(camp.difficulty() == 1 && camp.launchPath() == "/game/maps/campaign/EASY/023001.ssc",
              "Easy difficulty routes to EASY/*.ssc");

        camp.activate(camp.shortcutId('s'));
        check(camp.screen() == FrontEnd::Screen::Briefing && camp.briefingFromCampaign(),
              "Start on CampaignMissions enters campaign Briefing");
        check(camp.briefingText() == "6th Panzer support.", "Briefing displays campaign I-DESC");

        // Cancel / Esc from campaign Briefing returns to CampaignMissions and preserves mission index.
        camp.activate("cancel");
        check(camp.screen() == FrontEnd::Screen::CampaignMissions && camp.selectedCampaignMission() == 1,
              "Cancel from campaign Briefing returns to CampaignMissions keeping selection");
        camp.activate("cmission:1");
        check(camp.screen() == FrontEnd::Screen::Briefing, "cmission:1 reopens Briefing");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::CampaignMissions && camp.selectedCampaignMission() == 1,
              "Esc from campaign Briefing returns to CampaignMissions");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::Campaigns, "Esc from CampaignMissions returns to Campaigns");
        // Re-entering same campaign preserves mission index; switching to another resets to 0.
        camp.activate("campaign:0");
        check(camp.selectedCampaignMission() == 1, "Re-entering same campaign keeps selected mission");
        camp.back();
        camp.activate("campaign:1");
        check(camp.selectedCampaign() == 1 && camp.selectedCampaignMission() == 0,
              "Switching to another campaign resets mission selection to 0");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::Campaigns, "Back to Campaigns");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::SinglePlayer, "Esc from Campaigns returns to SinglePlayer");

        // Switching back to SingleMissions clears briefingFromCampaign_ flag.
        camp.activate("missions");
        camp.activate("mission:0");
        check(camp.screen() == FrontEnd::Screen::Briefing && !camp.briefingFromCampaign(),
              "Single Missions resets briefingFromCampaign()");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::SingleMissions, "Cancel returns to SingleMissions");

        // Empty campaigns check.
        FrontEnd emptyCamp;
        emptyCamp.activate("single");
        emptyCamp.activate("campaigns");
        check(emptyCamp.screen() == FrontEnd::Screen::Campaigns, "Empty campaigns opens Campaigns screen safely");
        bool hasNoCampMsg = false;
        for (const auto& it : emptyCamp.items()) {
            if (!it.selectable && it.label == "No campaigns installed.") hasNoCampMsg = true;
        }
        check(hasNoCampMsg, "Empty campaigns advertises non-selectable notice row");

        // No missions installed: Start is inert and reports why.
        FrontEnd empty;
        empty.activate("single");
        empty.activate("missions");
        check(empty.selectedMission() == nullptr, "no mission selected when none installed");
        empty.activate("start");
        check(empty.screen() == FrontEnd::Screen::SingleMissions, "Start with no missions does not open a briefing");
        check(!empty.takeNotice().empty(), "empty Start reports a notice");
        bool inertStart = false;
        for (const auto& it : empty.items()) if (it.id == "start") inertStart = !it.selectable;
        check(inertStart, "Start is not selectable when no missions are installed");

        // Cancelling the briefing preserves the selected mission (no reset to the first row).
        FrontEnd keep;
        keep.setMissions(missions());
        keep.activate("single");
        keep.activate("missions");
        keep.activate("mission:1");
        keep.activate("cancel");
        check(keep.screen() == FrontEnd::Screen::SingleMissions && keep.selected() == 1,
              "cancel keeps the cancelled mission selected");
        keep.activate("start");
        check(keep.selected() == 1 && keep.launchPath() == "/game/maps/single/V2.ssm",
              "Start after cancel reopens the same mission, not the first");

        // Advertised letter shortcuts map to row ids, case-insensitively.
        FrontEnd sc;
        sc.setMissions(missions());
        check(sc.shortcutId('s') == "single" && sc.shortcutId('S') == "single", "Main Menu S -> Single Player");
        check(sc.shortcutId('m') == "multi" && sc.shortcutId('c') == "credits", "Main Menu letter shortcuts");
        check(sc.shortcutId('x').empty(), "unadvertised letter has no shortcut");
        sc.activate(sc.shortcutId('s'));
        check(sc.screen() == FrontEnd::Screen::SinglePlayer, "shortcut S opens Single Player");
        check(sc.shortcutId('s') == "missions" && sc.shortcutId('c') == "campaigns" &&
              sc.shortcutId('l') == "load", "Single Player letter shortcuts");
        sc.activate(sc.shortcutId('s'));
        check(sc.screen() == FrontEnd::Screen::SingleMissions, "shortcut S opens Single Missions");
        sc.activate("mission:0");
        check(sc.screen() == FrontEnd::Screen::Briefing && sc.shortcutId('s') == "start",
              "Briefing advertises S and maps it to Start");
        sc.activate(sc.shortcutId('s'));
        check(sc.screen() == FrontEnd::Screen::Launch, "Briefing shortcut S launches the mission");

        std::cout << "frontend_test: all checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "frontend_test: " << e.what() << '\n';
        return 1;
    }
}
