// Gold frontend navigation graph: the verified Tutorial I path to Start, Back/Esc,
// cancel-briefing-starts-no-mission, switch-mission, quit and deferred branches. Asset-free.
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
        def.activate("single");
        def.activate("campaigns");
        check(def.screen() == FrontEnd::Screen::SinglePlayer, "Campaigns is deferred, stays on Single Player");
        check(!def.takeNotice().empty(), "Campaigns reports a notice");

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
        check(sc.shortcutId('s') == "missions" && sc.shortcutId('l') == "load", "Single Player letter shortcuts");
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