#include "render/window.hpp"
#include "ui/briefing.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

void wrapping() {
    using Lines = std::vector<std::string>;
    check(ss2::wrapText("", 10) == Lines{""}, "empty text is one blank line");
    check(ss2::wrapText("one two three", 7) == Lines{"one two", "three"}, "greedy wrap");
    check(ss2::wrapText("a  b\t c", 80) == Lines{"a b c"}, "runs of blanks collapse");
    check(ss2::wrapText("first\n\nsecond", 80) == Lines{"first", "", "second"}, "newlines are kept");
    check(ss2::wrapText("abcdefghij", 4) == Lines{"abcd", "efgh", "ij"}, "long words are split");
    check(ss2::wrapText("x abcdefgh", 4) == Lines{"x", "abcd", "efgh"}, "long word after a short one");
    // Two-byte code points count as one column each and are never cut in half.
    check(ss2::wrapText("\xC3\xA9\xC3\xA9\xC3\xA9", 2) == Lines{"\xC3\xA9\xC3\xA9", "\xC3\xA9"}, "UTF-8 split");
    check(ss2::wrapText("word", 0) == Lines{"w", "o", "r", "d"}, "zero columns behaves as one");
}

SDL_Event key(SDL_Keycode code, bool repeat = false) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.repeat = repeat;
    return e;
}

SDL_Event button(SDL_EventType type, float x, float y, Uint8 which = SDL_BUTTON_LEFT) {
    SDL_Event e{};
    e.type = type;
    e.button.button = which;
    e.button.x = x;
    e.button.y = y;
    return e;
}

// Colours other than the border and body prove that glyphs were drawn.
bool hasText(const ss2::Briefing& b) {
    const auto& p = b.rgba();
    for (std::size_t i = 0; i + 3 < p.size(); i += 4)
        if (p[i] == 236 && p[i + 1] == 236 && p[i + 2] == 228) return true;
    return false;
}

void present(ss2::VulkanWindow& renderer) {
    for (int i = 0; i < 200; ++i) {
        SDL_PumpEvents();
        if (renderer.present({})) return;
        SDL_Delay(5);
    }
    throw std::runtime_error("Timed out waiting for presentation");
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
        wrapping();
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow("Briefing test", 640, 480, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        int w{}, h{}, pixelW{}, pixelH{};
        SDL_GetWindowSize(window.get(), &w, &h);
        SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
        const float midX = w / 2.f, midY = h / 2.f;

        ss2::Briefing briefing(window.get());
        check(!briefing.active() && !briefing.acknowledge(), "starts closed");
        check(!briefing.handle(key(SDLK_RETURN)), "closed panel ignores keys");

        briefing.open("Move the selected squad to the marked position.\nPress Enter when ready.");
        check(briefing.active() && briefing.pageCount() == 1, "opens on a single page");
        check(briefing.width() <= unsigned(pixelW) && briefing.height() <= unsigned(pixelH), "fits the window");
        check(briefing.rgba().size() == std::size_t(briefing.width()) * briefing.height() * 4, "pixel count");
        check(hasText(briefing), "text was rendered");
        check(std::ranges::all_of(briefing.rgba(), [n = 0](std::uint8_t v) mutable { return n++ % 4 != 3 || v == 255; }),
              "panel is opaque");

        // Keys: unrelated and auto-repeated keys do nothing.
        check(!briefing.handle(key(SDLK_A)) && briefing.active(), "other keys are ignored");
        check(!briefing.handle(key(SDLK_RETURN, true)) && briefing.active(), "key repeat is ignored");
        check(briefing.handle(key(SDLK_RETURN)) && !briefing.active(), "Enter closes");
        briefing.open("again");
        check(briefing.handle(key(SDLK_SPACE)) && !briefing.active(), "Space closes");

        // Mouse: a click must both start and end inside the panel.
        briefing.open("again");
        check(!briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_DOWN, 1, 1)), "press outside");
        check(!briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_UP, midX, midY)) && briefing.active(), "release without press");
        check(!briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_DOWN, midX, midY)), "press alone does not close");
        check(!briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_UP, 1, 1)) && briefing.active(), "drag out cancels");
        check(!briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_DOWN, midX, midY, SDL_BUTTON_RIGHT)), "right press");
        check(!briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_UP, midX, midY, SDL_BUTTON_RIGHT)) && briefing.active(),
              "right click is ignored");
        check(!briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_DOWN, midX, midY)), "press inside");
        check(briefing.handle(button(SDL_EVENT_MOUSE_BUTTON_UP, midX, midY)) && !briefing.active(), "click closes");

        // Paging: text taller than the window needs several acknowledgements.
        std::string longText;
        for (int i = 0; i < 200; ++i) longText += "line " + std::to_string(i) + "\n";
        briefing.open(longText);
        const auto pages = briefing.pageCount();
        check(pages > 1 && briefing.height() <= unsigned(pixelH), "long text is paged");
        for (std::size_t i = 0; i + 1 < pages; ++i) check(!briefing.acknowledge() && briefing.page() == i + 1, "next page");
        check(briefing.acknowledge() && !briefing.active(), "last page closes");
        briefing.open(longText);
        check(briefing.handle(key(SDLK_ESCAPE)) && !briefing.active(), "Escape closes from any page");

        // Presentation, with and without a map underneath, and after a resize.
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        briefing.open("Shown over an empty window.");
        briefing.draw(renderer);
        present(renderer);
        renderer.setMap({96, 64, std::vector<std::uint8_t>(96 * 64 * 4, 128)});
        briefing.open(longText);
        briefing.draw(renderer);
        present(renderer);
        check(!briefing.acknowledge(), "second page");
        briefing.draw(renderer);
        present(renderer);

        if (!SDL_SetWindowSize(window.get(), 320, 200) || !SDL_SyncWindow(window.get()))
            throw std::runtime_error(SDL_GetError());
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            renderer.handleEvent(event);
            briefing.handle(event);
        }
        SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
        briefing.open(longText); // Lays out again for the current size on every platform.
        check(briefing.height() <= unsigned(pixelH), "fits after resize");
        briefing.draw(renderer);
        present(renderer);

        // A panel larger than the window is cropped rather than rejected.
        renderer.setOverlay(1024, 768, std::vector<std::uint8_t>(1024 * 768 * 4, 255));
        present(renderer);
        renderer.setOverlay(1, 1, std::vector<std::uint8_t>(4, 255));
        present(renderer);
        try {
            renderer.setOverlay(2, 2, std::vector<std::uint8_t>(4, 255));
            check(false, "mismatched overlay size is rejected");
        } catch (const std::invalid_argument&) {}

        briefing.open("closing");
        briefing.draw(renderer);
        check(briefing.acknowledge(), "closes");
        briefing.draw(renderer);
        present(renderer);
        renderer.clearOverlay();
        present(renderer);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
