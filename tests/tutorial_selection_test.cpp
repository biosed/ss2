#include "ui/tutorial_selection.hpp"
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>
#include <iostream>

int main(int argc, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    int status = 0;
    ss2::ValidationReport report;
    try {
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow("Selection test", 320, 240, SDL_WINDOW_VULKAN), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        ss2::VulkanWindow renderer(window.get(), argc > 1 ? &report : nullptr);
        renderer.setMap({512, 512, std::vector<std::uint8_t>(512 * 512 * 4, 100)});
        ss2::TutorialSelection select(window.get(), renderer, {
            {"dofficer", 256, 256, 100, 90, 1, 0, 252, 240, 9, 20},
            {"rifle", 270, 256, 80, 70, 1, 0, 266, 240, 9, 20},
            {"rifle", 280, 256, 90, 80, 2, 0, 276, 240, 9, 20},
            {"rifle", 260, 256, 90, 80, 2, 1, 259, 240, 5, 20},
            {"rifle", 511, 256, 100, 100, 1, 0, 510, 240, 2, 20}},
            {{"vehicle", 128, 128, 0, 100, 75, 1, 0}});
        if (select.infantryAt(260,248) != 3 || select.infantryAt(256,248) != 0 || select.infantryAt(0,0))
            throw std::runtime_error("Context target did not match the visible infantry");
        if (!renderer.present({})) throw std::runtime_error("Selection fixture failed to present");
        int w{}, h{};
        SDL_GetWindowSize(window.get(), &w, &h);
        const auto [x0,y0] = renderer.mapPosition(0,0);
        const auto [x1,y1] = renderer.mapPosition(float(w),float(h));
        const auto mouse = [&](SDL_EventType kind, int mx, int my, int clicks = 1) {
            SDL_Event e{};
            e.type = kind;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.clicks = clicks;
            e.button.x = float((mx-x0) * w / (x1-x0));
            e.button.y = float((my-y0) * h / (y1-y0));
            select.handle(e);
        };
        const auto expect = [&](std::string_view part) {
            if (std::string_view(SDL_GetWindowTitle(window.get())).find(part) == std::string_view::npos)
                throw std::runtime_error(std::string("Unexpected selection: ") + SDL_GetWindowTitle(window.get()));
        };
        const auto click = [&](int x, int y, int clicks) {
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, clicks);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y, clicks);
        };
        expect("dofficer");
        click(269, 250, 1);
        expect("Ammo 70");
        // Neither foot is inside this small drag. Gold treats it as a midpoint click.
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 267, 245);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, 272, 251);
        expect("Ammo 70");
        click(269, 250, 2);
        expect("2 infantry");
        SDL_SetModState(SDL_KMOD_CTRL);
        click(269, 250, 2);
        expect("3 infantry");
        SDL_SetModState(SDL_KMOD_NONE);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 248, 232);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, 288, 269);
        expect("3 infantry");
        SDL_SetModState(SDL_KMOD_SHIFT);
        click(269, 250, 1);
        expect("2 infantry");
        SDL_SetModState(SDL_KMOD_NONE);
        if (!renderer.present({})) throw std::runtime_error("Selection highlight failed to present");
        SDL_Event group{};
        group.type = SDL_EVENT_KEY_DOWN;
        group.key.key = SDLK_1;
        group.key.mod = SDL_KMOD_CTRL;
        select.handle(group);
        click(251, 239, 1);
        group.key.mod = SDL_KMOD_NONE;
        select.handle(group);
        expect("2 infantry");
        SDL_Event next{};
        next.type = SDL_EVENT_KEY_DOWN;
        next.key.key = SDLK_TAB;
        const auto selectedGroup = select.selected();
        select.handle(next);
        if (select.selected() != selectedGroup) throw std::runtime_error("Tab discarded a squad");
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 248, 232);
        select.cancelDrag();
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, 288, 269);
        if (select.selected() != selectedGroup) throw std::runtime_error("Canceled drag changed the selection");
        next.key.key = SDLK_V;
        select.handle(next);
        expect("vehicle   HP 100   Ammo 75");
        next.key.key = SDLK_TAB;
        for (int i = 0; i < 8; ++i) {
            select.handle(next);
            expect("Owner 0");
        }
        {
        // Deaths: a unit reported with no health leaves the selection, saved groups,
        // picking, box selection and Tab cycling, and the title follows.
        // Earlier keys moved the camera, so map-to-window positions are taken afresh.
        renderer.focusMap(256, 256);
        if (!renderer.present({})) throw std::runtime_error("Death fixture failed to present");
        const auto [dx0,dy0] = renderer.mapPosition(0,0);
        const auto [dx1,dy1] = renderer.mapPosition(float(w),float(h));
        const auto mouse = [&](SDL_EventType kind, int mx, int my, int clicks = 1) {
            SDL_Event e{};
            e.type = kind;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.clicks = clicks;
            e.button.x = float((mx-dx0) * w / (dx1-dx0));
            e.button.y = float((my-dy0) * h / (dy1-dy0));
            select.handle(e);
        };
        const auto click = [&](int x, int y, int clicks) {
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, clicks);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y, clicks);
        };
        const auto fixture = [](const char* id, int x, int hp, int group, int owner, int left, int width) {
            return ss2::assets::Infantry{id, x, 256, hp, 80, group, owner, left, 240, width, 20};
        };
        click(256, 250, 1);
        expect("dofficer");
        SDL_SetModState(SDL_KMOD_SHIFT);
        click(269, 250, 1);
        SDL_SetModState(SDL_KMOD_NONE);
        expect("2 infantry");
        const auto pair = select.selected();
        if (pair != std::set<size_t>{0, 1}) throw std::runtime_error("Death fixture did not select units 0 and 1");
        group.key.key = SDLK_2;
        group.key.mod = SDL_KMOD_CTRL;
        select.handle(group);
        if (select.playerLosses() != 0) throw std::runtime_error("Losses counted before any death");
        select.update(1, fixture("rifle", 270, 0, 1, 0, 266, 9));
        if (select.selected() != std::set<size_t>{0}) throw std::runtime_error("Dead unit stayed selected");
        expect("dofficer");
        if (select.playerLosses() != 1) throw std::runtime_error("Player loss was not counted");
        if (select.infantryAt(270, 248) == 1) throw std::runtime_error("Dead unit can still be picked");
        group.key.mod = SDL_KMOD_NONE;
        select.handle(group);
        if (select.selected() != std::set<size_t>{0}) throw std::runtime_error("Saved group kept a dead unit");
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 240, 232);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, 300, 269);
        if (select.selected().contains(1)) throw std::runtime_error("Box selection included a dead unit");
        // A death during a drag must not disturb the box in progress.
        click(256, 250, 1);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 240, 232);
        select.update(0, fixture("dofficer", 256, 0, 1, 0, 252, 9));
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, 300, 269);
        if (select.selected().contains(0) || select.selected().contains(1))
            throw std::runtime_error("Unit that died during a drag was selected");
        if (select.playerLosses() != 2) throw std::runtime_error("Second player loss was not counted");
        next.key.key = SDLK_TAB;
        for (int i = 0; i < 8; ++i) {
            select.handle(next);
            if (select.selected().contains(0) || select.selected().contains(1))
                throw std::runtime_error("Tab focused a dead unit");
            expect("Owner 0");
        }
        // An enemy death is not a player loss and never enters the selection.
        select.update(3, fixture("rifle", 260, 0, 2, 1, 259, 5));
        if (select.playerLosses() != 2) throw std::runtime_error("Enemy death counted as a player loss");
        // A wounded unit stays selectable and its health shows in the title.
        select.update(2, fixture("rifle", 280, 37, 2, 0, 276, 9));
        click(279, 250, 1);
        expect("HP 37");
        if (!renderer.present({})) throw std::runtime_error("Selection after deaths failed to present");
        }
        next.key.key = SDLK_HOME;
        renderer.handleEvent(next);
        if (!renderer.present({})) throw std::runtime_error("Overview did not render");
        select.mark(240,200);
        next.key.key = SDLK_M;
        select.handle(next);
        if (!renderer.present({})) throw std::runtime_error("Marker focus did not render");
        const auto [cx,cy] = renderer.mapPosition(w/2.0f,h/2.0f);
        if (std::abs(cx-240) > 1 || std::abs(cy-200) > 1)
            throw std::runtime_error("Marker shortcut left the camera at the whole-map overview");
        for (const int scale : {1, 2}) {
            // Deliberately dissimilar sprite rectangles. Only the feet and radius affect the hit.
            ss2::TutorialSelection targets(window.get(), renderer, {
                {"rifle",100*scale,100*scale,100,100,0,0,0,0,200*scale,200*scale},
                {"rifle",200*scale,100*scale,100,100,0,0,200*scale,100*scale,1,1},
                {"rifle",201*scale,200*scale,100,100,0,0,201*scale,200*scale,1,1}}, {}, scale);
            if (targets.infantryAt(100*scale,117*scale) != 0 ||
                targets.infantryAt(100*scale,118*scale) ||
                targets.infantryAt(200*scale,117*scale) != 1 ||
                targets.infantryAt(201*scale,217*scale) != 2 ||
                targets.infantryAt(201*scale,218*scale))
                throw std::runtime_error("Infantry picking differs at the original foot-distance cutoff");
        }
        ss2::assets::Infantry earlier{"rifle",100,110,100,100,0,0,95,95,10,18};
        ss2::assets::Infantry later{"rifle",100,90,100,100,0,0,95,75,10,18};
        ss2::TutorialSelection overlap(window.get(), renderer, {earlier,later});
        if (overlap.infantryAt(100,100) != 0)
            throw std::runtime_error("Equal foot distances did not follow original live-list order");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    return status || report.errors.load() != 0;
}
