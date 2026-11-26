// Menu: navigation, activation, scrolling, input capture and its place in a frame.
#include "render/window.hpp"
#include "ui/menu.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
SDL_Event key(SDL_Keycode code, bool repeat = false) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.repeat = repeat;
    return e;
}
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    ss2::ValidationReport report;
    int status = 0;
    try {
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow("Menu test", 800, 600, SDL_WINDOW_VULKAN), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        int w{}, h{}, pixelW{}, pixelH{};
        SDL_GetWindowSize(window.get(), &w, &h);
        SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);

        ss2::Menu menu(window.get());
        check(!menu.active() && menu.highlighted().empty(), "starts closed");
        check(!menu.handle(key(SDLK_RETURN)).consumed, "a closed menu ignores input");

        menu.open("Paused", {{"heading", "Game", "", false}, {"resume", "Resume", "", true},
                             {"smooth", "Smooth terrain", "On", true}, {"note", "Stop", "S", false},
                             {"quit", "Quit", "", true}});
        check(menu.active() && menu.highlighted() == "resume", "opens on the first selectable row");
        check(menu.width() <= unsigned(pixelW) && menu.height() <= unsigned(pixelH), "fits the window");
        for (std::size_t i = 3; i < menu.rgba().size(); i += 4) check(menu.rgba()[i] == 255, "panel is opaque");
        check(menu.handle(key(SDLK_RIGHT)).step == 1 && menu.handle(key(SDLK_LEFT)).step == -1,
              "Left/Right arrows emit step -1/+1 without changing existing activation");

        // Section header ("sec:*") renders distinctly from a plain info row.
        {
            ss2::Menu secMenu(window.get()), plainMenu(window.get());
            secMenu.open("Options", {{"sec:video", "Video", "", false}, {"ok", "OK", "", true}});
            plainMenu.open("Options", {{"", "Video", "", false}, {"ok", "OK", "", true}});
            check(secMenu.rgba() != plainMenu.rgba(), "section header draws distinct accent/rule");
        }

        // Keyboard: Down and Up skip plain rows and wrap; Enter and Space choose; Escape closes.
        auto shot = menu.rgba();
        check(menu.handle(key(SDLK_DOWN)).consumed && menu.highlighted() == "smooth", "down");
        check(menu.rgba() != shot, "highlight is drawn");
        menu.handle(key(SDLK_DOWN));
        check(menu.highlighted() == "quit", "down skips a plain row");
        menu.handle(key(SDLK_DOWN));
        check(menu.highlighted() == "resume", "down wraps past the heading");
        menu.handle(key(SDLK_UP));
        check(menu.highlighted() == "quit", "up wraps");
        auto chosen = menu.handle(key(SDLK_RETURN));
        check(chosen.consumed && chosen.activated == "quit" && menu.active(), "Enter chooses and leaves the menu open");
        check(menu.handle(key(SDLK_RETURN, true)).activated.empty(), "key repeat does not choose");
        check(menu.handle(key(SDLK_SPACE)).activated == "quit", "Space chooses");
        check(menu.handle(key(SDLK_S)).consumed, "other keys are swallowed while open");

        // Values can change while the highlight stays put.
        menu.handle(key(SDLK_UP));
        check(menu.highlighted() == "smooth", "back to the toggle");
        shot = menu.rgba();
        menu.setItems({{"heading", "Game", "", false}, {"resume", "Resume", "", true},
                       {"smooth", "Smooth terrain", "Off", true}, {"quit", "Quit", "", true}});
        check(menu.highlighted() == "smooth" && menu.rgba() != shot, "setItems keeps the highlight and redraws");

        // Mouse: hover highlights; a click must press and release on the same row.
        const auto point = [&](const std::string& id) {
            const auto r = menu.rowRect(id);
            check(r.height > 0, "row is visible");
            return SDL_FPoint{((pixelW - int(menu.width())) / 2 + r.width / 2.f) * w / pixelW,
                              ((pixelH - int(menu.height())) / 2 + r.y + r.height / 2.f) * h / pixelH};
        };
        const auto mouse = [&](SDL_EventType type, SDL_FPoint p, Uint8 button = SDL_BUTTON_LEFT) {
            SDL_Event e{};
            e.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) { e.motion.x = p.x; e.motion.y = p.y; }
            else { e.button.button = button; e.button.x = p.x; e.button.y = p.y; }
            return menu.handle(e);
        };
        check(mouse(SDL_EVENT_MOUSE_MOTION, point("resume")).consumed && menu.highlighted() == "resume", "hover highlights");
        mouse(SDL_EVENT_MOUSE_MOTION, point("heading"));
        check(menu.highlighted() == "resume", "hovering a plain row changes nothing");
        check(mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, point("quit")).activated.empty(), "press alone does not choose");
        check(mouse(SDL_EVENT_MOUSE_BUTTON_UP, point("quit")).activated == "quit", "click chooses");
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, point("quit"));
        check(mouse(SDL_EVENT_MOUSE_BUTTON_UP, point("resume")).activated.empty(), "release on another row cancels");
        check(mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, {1, 1}).consumed, "clicks outside are swallowed");
        check(mouse(SDL_EVENT_MOUSE_BUTTON_UP, {1, 1}).activated.empty(), "and choose nothing");
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, point("quit"), SDL_BUTTON_RIGHT);
        check(mouse(SDL_EVENT_MOUSE_BUTTON_UP, point("quit"), SDL_BUTTON_RIGHT).activated.empty(), "right click does not choose");

        auto closed = menu.handle(key(SDLK_ESCAPE));
        check(closed.consumed && closed.closed && !menu.active(), "Escape closes");

        // A long list scrolls to keep the highlight visible.
        std::vector<ss2::MenuItem> many;
        for (int i = 0; i < 80; ++i) many.push_back({"row" + std::to_string(i), "Row " + std::to_string(i), std::to_string(i), true});
        menu.open("Long list", many);
        check(menu.height() <= unsigned(pixelH) && menu.rowRect("row79").height == 0, "long list is clipped to the window");
        for (int i = 0; i < 79; ++i) menu.handle(key(SDLK_DOWN));
        check(menu.highlighted() == "row79" && menu.rowRect("row79").height > 0 && menu.rowRect("row0").height == 0, "list scrolls down");
        menu.handle(key(SDLK_DOWN));
        check(menu.highlighted() == "row0" && menu.rowRect("row0").height > 0, "wraps back to the top");
        // Clicking the rendered scrollbar gutter pages down cleanly.
        {
            const SDL_FPoint sbPt{
                ((pixelW - int(menu.width())) / 2 + int(menu.width()) - 14.f) * w / pixelW,
                ((pixelH - int(menu.height())) / 2 + int(menu.height()) * 0.60f) * h / pixelH
            };
            check(mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, sbPt).consumed, "scrollbar click is consumed");
            check(menu.rowRect("row0").height == 0, "clicking scrollbar track below thumb pages down");
        }
        // A label carrying line breaks still draws as one row: nothing spills into the next row.
        {
            menu.open("Rows", {{"a", "One", "", true}, {"b", "plain", "", false}});
            const auto clean = menu.rgba();
            menu.open("Rows", {{"a", "One\rTwo\nThree", "", true}, {"b", "plain", "", false}});
            const auto r = menu.rowRect("b");
            for (int y = r.y; y < r.y + r.height; ++y)
                for (int x = 0; x < int(menu.width()) * 4; ++x)
                    check(menu.rgba()[std::size_t(y) * menu.width() * 4 + x] == clean[std::size_t(y) * menu.width() * 4 + x],
                          "a label with line breaks spilled into the next row");
        }
        // A log: one choice on top and many plain rows. Everything must be reachable.
        {
            std::vector<ss2::MenuItem> log{{"close", "Close", "", true}};
            for (int i = 0; i < 60; ++i) log.push_back({"line" + std::to_string(i), "Line " + std::to_string(i), "", false});
            menu.open("Log", log);
            const auto seen = [&](const char* id) { return menu.rowRect(id).height > 0; };
            check(seen("close") && !seen("line59"), "log opens at the top");
            menu.handle(key(SDLK_DOWN));
            check(!seen("close") && menu.highlighted() == "close", "Down scrolls when there is no other choice");
            menu.handle(key(SDLK_UP));
            check(seen("close"), "Up scrolls back");
            menu.handle(key(SDLK_END));
            check(seen("line59") && !seen("line0"), "End shows the newest rows");
            check(menu.handle(key(SDLK_RETURN)).activated == "close", "the choice still works while scrolled away");
            menu.handle(key(SDLK_HOME));
            check(seen("close") && seen("line0"), "Home returns to the top");
            menu.handle(key(SDLK_PAGEDOWN));
            check(!seen("line0") && !seen("line59"), "PageDown moves a page");
            menu.handle(key(SDLK_PAGEUP));
            check(seen("line0"), "PageUp moves back");
            SDL_Event wheel{};
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.y = -1;
            check(menu.handle(wheel).consumed && !seen("close"), "wheel down scrolls");
            wheel.wheel.y = 1;
            for (int i = 0; i < 40; ++i) menu.handle(wheel);
            check(seen("close"), "wheel up stops at the top");
            wheel.wheel.y = -1;
            for (int i = 0; i < 40; ++i) menu.handle(wheel);
            check(seen("line59"), "wheel down stops at the bottom");
            // A click lands on the row that is drawn there, not on the unscrolled one.
            menu.open("Long", many);
            menu.handle(key(SDLK_END));
            check(mouse(SDL_EVENT_MOUSE_MOTION, point("row79")).consumed && menu.highlighted() == "row79", "hover follows scrolling");
            menu.handle(key(SDLK_UP));
            check(menu.highlighted() == "row78" && menu.rowRect("row78").height > 0, "moving the highlight keeps it in view");
        }
        menu.open("Reference only", {{"a", "Stop", "S", false}, {"b", "Pause", "P", false}});
        check(menu.highlighted().empty() && menu.handle(key(SDLK_RETURN)).activated.empty(), "a list with no choices chooses nothing");
        menu.handle(key(SDLK_DOWN));
        menu.open("Empty", {});
        menu.handle(key(SDLK_DOWN));
        menu.close();
        menu.close();

        // In a frame: the menu is drawn centred and removed when closed.
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        renderer.setMap({64, 48, std::vector<std::uint8_t>(64 * 48 * 4, 200)});
        menu.open("Paused", {{"resume", "Resume", "", true}});
        menu.draw(renderer);
        auto frame = renderer.captureFrame();
        const auto* centre = &frame.rgba[(std::size_t(frame.height / 2) * frame.width + frame.width / 2) * 4];
        const auto* want = &menu.rgba()[(std::size_t(menu.height() / 2) * menu.width() + menu.width() / 2) * 4];
        check(centre[0] == want[0] && centre[1] == want[1] && centre[2] == want[2], "menu pixels reach the screen");
        menu.close();
        menu.draw(renderer);
        frame = renderer.captureFrame();
        centre = &frame.rgba[(std::size_t(frame.height / 2) * frame.width + frame.width / 2) * 4];
        check(centre[0] == 200 && centre[1] == 200, "closed menu is removed");

        // The menu has its own slot: a panel underneath survives the menu opening and closing.
        renderer.setOverlay(40, 30, std::vector<std::uint8_t>(40 * 30 * 4, 77));
        menu.open("Paused", {{"resume", "Resume", "", true}});
        menu.draw(renderer);
        frame = renderer.captureFrame();
        centre = &frame.rgba[(std::size_t(frame.height / 2) * frame.width + frame.width / 2) * 4];
        check(centre[0] == want[0] && centre[1] == want[1], "menu is drawn above the panel");
        menu.handle(key(SDLK_ESCAPE));
        menu.draw(renderer);
        frame = renderer.captureFrame();
        centre = &frame.rgba[(std::size_t(frame.height / 2) * frame.width + frame.width / 2) * 4];
        check(centre[0] == 77 && centre[1] == 77, "the panel underneath reappears when the menu closes");

        if (const char* priv = std::getenv("SS2_PRIVATE_GAME"); priv && *priv && std::filesystem::is_directory(priv)) {
            ss2::Menu skinned(window.get());
            check(!skinned.hasRetailSkin(), "fresh Menu starts unskinned");
            skinned.loadRetailAssets(priv);
            check(skinned.hasRetailSkin(), "Menu loads retail lng.sue\\msgb.dat steel skin");
            skinned.open("Paused", {{"resume", "Resume", "", true}, {"quit", "Quit", "", true}});
            const auto unskinnedW = [&] {
                ss2::Menu plain(window.get());
                plain.open("Paused", {{"resume", "Resume", "", true}, {"quit", "Quit", "", true}});
                return plain.rgba();
            }();
            check(skinned.width() > 0u && skinned.height() > 0u && skinned.rgba() != unskinnedW,
                  "retail msgb.dat skin renders distinct frame over fallback");
            for (std::size_t i = 3; i < skinned.rgba().size(); i += 4) {
                check(skinned.rgba()[i] == 255, "skinned menu panel stays opaque");
            }
            check(skinned.rowRect("quit").height > 0 && skinned.handle(key(SDLK_DOWN)).consumed &&
                  skinned.highlighted() == "quit",
                  "skinned menu preserves navigation and row geometry");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
