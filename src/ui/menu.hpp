#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct _PangoContext;

namespace ss2 {
class VulkanWindow;

// One row of a menu. A row that is not selectable is plain text: a heading, or a line
// of a reference list such as key bindings (label on the left, value on the right).
struct MenuItem {
    std::string id, label, value;
    bool selectable = true;
};

struct MenuResult {
    bool consumed = false;      // the event belonged to the menu; do not pass it on
    bool closed = false;        // Escape was pressed; the menu is now closed
    std::string activated;      // id of the row that was chosen, or empty
};

// A modal list panel for pause, settings and reference screens. The caller supplies the
// rows and reacts to the chosen id: it may close the menu, open another page, or call
// setItems() to show a changed value. While active it consumes keyboard and mouse input.
// It has its own overlay slot, drawn above the briefing panel, so a briefing that is
// open underneath is left intact and reappears when the menu closes.
class Menu {
public:
    explicit Menu(SDL_Window* parent);
    ~Menu();

    void loadRetailAssets(const std::filesystem::path& gameDir);
    bool hasRetailSkin() const;

    void open(std::string title, std::vector<MenuItem> items);
    void setItems(std::vector<MenuItem> items);   // keeps the highlighted row when its id still exists
    void close();
    bool active() const { return active_; }
    const std::string& highlighted() const;        // id of the highlighted row, or empty
    void highlight(const std::string& id);         // move the highlight to a row by id, if selectable

    // Up/Down move, Enter or Space choose, Escape closes. The mouse highlights the row
    // under it and chooses on a left click released over the same row. A list taller
    // than the panel scrolls with the wheel, PageUp/PageDown and Home/End, and with
    // Up/Down when there is no other row to move the highlight to.
    MenuResult handle(const SDL_Event&);

    // Call once per frame WHETHER OR NOT the menu is active: the call after close() is
    // what removes the panel from the screen. It uploads only after a change.
    void draw(VulkanWindow&);

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    const std::vector<std::uint8_t>& rgba() const { return rgba_; }
    // Row geometry in panel pixels, or an empty rectangle for a row that is scrolled out.
    struct Rect { int x = 0, y = 0, width = 0, height = 0; };
    Rect rowRect(const std::string& id) const;

private:
    void render();
    int rowAt(float windowX, float windowY) const;
    bool move(int step);      // false when there is no other selectable row
    void scroll(int rows);

    SDL_Window* parent_;
    std::shared_ptr<_PangoContext> fonts_;
    std::string title_;
    std::vector<MenuItem> items_;
    bool active_ = false, follow_ = false;   // follow_: bring the highlight into view on the next render
    int highlight_ = -1, armed_ = -1, first_ = 0, visible_ = 0, rowHeight_ = 0, rowsTop_ = 0;
    unsigned width_ = 0, height_ = 0;
    int left_ = 0, top_ = 0;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0, drawn_ = 0;
    struct Skin;
    std::unique_ptr<Skin> skin_;
};
}
