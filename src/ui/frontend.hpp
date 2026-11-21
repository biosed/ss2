#pragma once

#include "ui/menu.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ss2 {

// The Gold frontend navigation graph, decoupled from SDL rendering and game assets so it
// can be regression-tested headless. The caller feeds the mission browser's entries, builds
// a Menu from items()/title(), shows briefingText() in a Briefing panel on the Briefing
// screen, and reacts to screen(): Launch means load launchPath(), Quit means exit.
class FrontEnd {
public:
    enum class Screen { MainMenu, SinglePlayer, SingleMissions, Briefing, Launch, Quit };

    struct Entry {
        std::string name, path, description, tag;
    };

    void setMissions(std::vector<Entry> missions);

    Screen screen() const { return screen_; }
    std::string title() const;
    std::vector<MenuItem> items() const;

    std::size_t selected() const { return selected_; }
    void select(std::size_t index);
    const Entry* selectedMission() const;
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
    std::size_t selected_ = 0;
    std::string notice_;
};
}