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

        // Multiplayer navigation, mode toggle, map selection, launch & validation notices; Load opens LoadGame browser.
        FrontEnd def;
        check(def.shortcutId('m') == "multi", "Main Menu M -> Multiplayer");
        def.activate("multi");
        check(def.screen() == FrontEnd::Screen::Multiplayer && def.title() == "Multiplayer",
              "Multiplayer opens Screen::Multiplayer");
        def.activate("mp_start");
        check(def.screen() == FrontEnd::Screen::Multiplayer && !def.takeNotice().empty(),
              "Host Start with no multiplayer maps stays put and emits notice");
        check(def.takeNotice().empty(), "notice is cleared after reading");
        check(def.shortcutId('m') == "mp_mode" && def.shortcutId('n') == "mp_nation" &&
              def.shortcutId('s') == "mp_start",
              "Multiplayer advertises M (mode), N (Nation), and S (Start) shortcuts");
        check(def.mpNation() == 1u, "Multiplayer defaults nation to German (1)");
        def.activate("mp_nation");
        check(def.mpNation() == 2u, "mp_nation cycles nation to British (2)");
        check(def.mpIsHost(), "Multiplayer defaults to Host Match");
        def.activate("mp_mode");
        check(!def.mpIsHost(), "mp_mode toggles to Join Match");
        def.setMpHostAddress("");
        def.activate("mp_start");
        check(def.screen() == FrontEnd::Screen::Multiplayer && !def.takeNotice().empty(),
              "Join Start with empty host address emits notice");
        def.setMpHostAddress("127.0.0.1");
        def.activate("mp_mode");
        check(def.mpIsHost(), "mp_mode toggles back to Host Match");
        def.setMultiplayerMissions({
            {"Skirmish A", "/game/maps/mplay/1x1(102-0)_.smm", "Skirmish A", "128x128 Summer"},
            {"Skirmish B", "/game/maps/mplay/2x2(101-0)_.smm", "Skirmish B", "256x256 Summer"},
        });
        check(hasId(def.items(), "mpmap:0") && hasId(def.items(), "mpmap:1"),
              "Multiplayer lists installed map items");
        def.activate("mpmap:1");
        check(def.selectedMultiplayerMission() == 1 &&
              def.launchMultiplayerPath() == "/game/maps/mplay/2x2(101-0)_.smm",
              "mpmap:1 selects multiplayer map");
        def.activate("mp_start");
        check(def.screen() == FrontEnd::Screen::LaunchMultiplayer,
              "mp_start enters Screen::LaunchMultiplayer");
        def.back();
        check(def.screen() == FrontEnd::Screen::Multiplayer, "Back from LaunchMultiplayer returns to Multiplayer");
        def.back();
        check(def.screen() == FrontEnd::Screen::MainMenu, "Back from Multiplayer returns to Main Menu");
        def.activate("single");
        def.activate("load");
        check(def.screen() == FrontEnd::Screen::LoadGame && def.title() == "Load Game",
              "Load opens Load Game screen");
        def.activate("load_start");
        check(def.screen() == FrontEnd::Screen::LoadGame && !def.takeNotice().empty(),
              "Empty Load Start stays on Load Game and emits a notice");
        def.activate("load_delete");
        check(def.screen() == FrontEnd::Screen::LoadGame && !def.takeNotice().empty(),
              "Empty Load Delete stays on Load Game and emits a notice");
        def.setSaves({
            {"Tutorial I — Quick Save", "/home/user/.local/share/ss2/saves/tutorial1.ss2save",
             "maps/single/--- TUTORIAL I ---.SSM", "Tutorial I", "Quick Save",
             "2026-11-22 01:00", 1200u, 29u, 0u, 2, 4096u},
            {"1. Starting Position — Slot 1", "/home/user/.local/share/ss2/saves/023000_slot1.ss2save",
             "maps/campaign/023000.ssc", "1. Starting Position", "Slot 1",
             "2026-11-22 01:15", 2400u, 18u, 4u, 3, 8192u},
        });
        check(def.shortcutId('l') == "load_start" && def.shortcutId('d') == "load_delete",
              "Load Game advertises L and D shortcuts");
        check(hasId(def.items(), "save:0") && hasId(def.items(), "save:1"),
              "Load Game lists saved game items");
        def.selectSave(1);
        def.activate(def.shortcutId('l'));
        check(def.screen() == FrontEnd::Screen::LaunchSave &&
              def.launchSavePath() == "/home/user/.local/share/ss2/saves/023000_slot1.ss2save",
              "L launches selected saved game");
        def.back();
        check(def.screen() == FrontEnd::Screen::LoadGame, "Back from LaunchSave returns to Load Game");
        def.activate("save:0");
        check(def.screen() == FrontEnd::Screen::LaunchSave &&
              def.selectedSave() == 0 &&
              def.launchSavePath() == "/home/user/.local/share/ss2/saves/tutorial1.ss2save",
              "save:0 selects and launches first save");
        def.back();
        def.activate("cancel");
        check(def.screen() == FrontEnd::Screen::SinglePlayer, "Cancel from Load Game returns to Single Player");
        def.activate("load");
        def.back();
        check(def.screen() == FrontEnd::Screen::SinglePlayer, "Esc from Load Game returns to Single Player");

        // Options (O) and Replays (P) navigation, shortcuts, and launch/cancel flows.
        {
            FrontEnd opt;
            opt.activate(opt.shortcutId('o'));
            check(opt.screen() == FrontEnd::Screen::Options && opt.title() == "Options",
                  "O opens Options screen");
            check(hasId(opt.items(), "ok") && hasId(opt.items(), "cancel"),
                  "Options offers OK and Cancel");
            opt.activate("ok");
            check(opt.screen() == FrontEnd::Screen::MainMenu, "OK returns from Options to Main Menu");
            opt.activate("options");
            opt.activate("cancel");
            check(opt.screen() == FrontEnd::Screen::MainMenu, "Cancel returns from Options to Main Menu");
            opt.activate("options");
            opt.back();
            check(opt.screen() == FrontEnd::Screen::MainMenu, "Esc returns from Options to Main Menu");

            FrontEnd rep;
            rep.activate(rep.shortcutId('p'));
            check(rep.screen() == FrontEnd::Screen::Replays && rep.title() == "Replay Game",
                  "P opens Replay Game screen");
            rep.activate("replay_start");
            check(rep.screen() == FrontEnd::Screen::Replays && !rep.takeNotice().empty(),
                  "Empty Replay Start stays on Replays and emits a notice");
            rep.setReplays({
                {"tut1_run", "/home/user/.local/share/ss2/replays/tut1_run.ss2rep",
                 "maps/single/--- TUTORIAL I ---.SSM", "0xdeadbeef", 12, 3734, 2, true},
                {"russian1", "/home/user/.local/share/ss2/replays/russian1.ss2rep",
                 "maps/campaign/000000.ssc", "0xcafebabe", 4, 120, 3, false},
            });
            check(rep.shortcutId('r') == "replay_start", "Replays advertises R for Replay");
            rep.selectReplay(1);
            rep.activate(rep.shortcutId('r'));
            check(rep.screen() == FrontEnd::Screen::LaunchReplay &&
                  rep.launchReplayPath() == "/home/user/.local/share/ss2/replays/russian1.ss2rep",
                  "R on Replays launches selected replay path");
            rep.back();
            rep.activate("options");
            rep.back();
            rep.activate("replay");
            rep.activate("replay:0");
            check(rep.screen() == FrontEnd::Screen::LaunchReplay &&
                  rep.selectedReplay() == 0 &&
                  rep.launchReplayPath() == "/home/user/.local/share/ss2/replays/tut1_run.ss2rep",
                  "replay:0 selects and launches first replay");
            FrontEnd repEsc;
            repEsc.activate("replay");
            repEsc.back();
            check(repEsc.screen() == FrontEnd::Screen::MainMenu, "Esc returns from Replays to Main Menu");
        }

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
        check(camp.screen() == FrontEnd::Screen::CustomCampaigns,
              "Custom opens CustomCampaigns screen");
        check(!camp.items().empty() && !camp.items()[0].selectable &&
              camp.items()[0].label == "No custom campaigns installed.",
              "Empty CustomCampaigns advertises non-selectable empty message");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::Campaigns, "Esc from empty CustomCampaigns returns to Campaigns");

        camp.setCustomCampaigns({
            {
                "Operation SeaLion",
                {
                    {"01_dover", "1. Dover Landing", "/custom/sealion/01_dover.ssc",
                     "/custom/sealion/EASY/01_dover.ssc", "Channel crossing.", "256x256 Summer"},
                },
            },
        });
        camp.activate("custom");
        check(camp.screen() == FrontEnd::Screen::CustomCampaigns && hasId(camp.items(), "ccampaign:0"),
              "Populated CustomCampaigns lists ccampaign:0");
        camp.activate("ccampaign:0");
        check(camp.screen() == FrontEnd::Screen::CampaignMissions && camp.inCustomCampaign() &&
              camp.selectedCustomCampaign() == 0,
              "Choosing ccampaign:0 opens CampaignMissions in custom mode");
        check(camp.launchPath() == "/custom/sealion/01_dover.ssc", "Custom campaign Normal launchPath matches");
        camp.activate(camp.shortcutId('s'));
        check(camp.screen() == FrontEnd::Screen::Briefing && camp.briefingFromCampaign(),
              "Custom campaign Start enters Briefing");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::CampaignMissions && camp.inCustomCampaign(),
              "Back from custom campaign Briefing keeps custom CampaignMissions");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::CustomCampaigns,
              "Back from custom CampaignMissions returns to CustomCampaigns");
        camp.back();
        check(camp.screen() == FrontEnd::Screen::Campaigns,
              "Back from CustomCampaigns returns to Campaigns");
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
