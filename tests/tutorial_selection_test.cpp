#include "ui/tutorial_selection.hpp"
#include <memory>
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
        SDL_Event next{};
        next.type = SDL_EVENT_KEY_DOWN;
        next.key.key = SDLK_V;
        select.handle(next);
        expect("vehicle   HP 100   Ammo 75");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    return status || report.errors.load() != 0;
}
