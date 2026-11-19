#include "render/window.hpp"
#include "ui/briefing.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

bool hasColour(const ss2::Notice& n, int r, int g, int b) {
    const auto& p = n.rgba();
    for (std::size_t i = 0; i + 3 < p.size(); i += 4)
        if (p[i] == r && p[i + 1] == g && p[i + 2] == b) return true;
    return false;
}

void notices(SDL_Window* window, ss2::VulkanWindow& renderer) {
    using namespace std::chrono_literals;
    using ss2::NoticeKind;
    int pixelW{}, pixelH{};
    SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
    ss2::Notice notice(window);
    check(!notice.visible() && !notice.advance(1s), "starts hidden");
    notice.clear();

    notice.show("Move order accepted", NoticeKind::accepted, 3s);
    check(notice.visible() && notice.kind() == NoticeKind::accepted && notice.text() == "Move order accepted", "shown");
    check(notice.width() && notice.width() <= unsigned(pixelW) && notice.height() <= unsigned(pixelH), "notice fits");
    check(notice.rgba().size() == std::size_t(notice.width()) * notice.height() * 4, "notice pixel count");
    check(std::ranges::all_of(notice.rgba(), [n = 0](std::uint8_t v) mutable { return n++ % 4 != 3 || v == 255; }),
          "notice is opaque");
    check(hasColour(notice, 236, 236, 228), "notice text was rendered");
    check(hasColour(notice, 104, 178, 120) && !hasColour(notice, 212, 96, 88), "accepted accent");
    const auto accepted = notice.rgba();

    // Age comes only from the caller.
    check(!notice.advance(0ns) && !notice.advance(1s) && !notice.advance(1999ms) && notice.visible(), "still young");
    check(notice.advance(1ms) && !notice.visible(), "expires exactly at its lifetime");
    check(!notice.advance(10s), "expiry is reported once");

    // Showing again replaces the text and restarts the age.
    notice.show("Cannot move there", NoticeKind::blocked, 2s);
    check(hasColour(notice, 212, 96, 88) && !hasColour(notice, 104, 178, 120), "blocked accent");
    check(notice.rgba() != accepted, "kinds look different");
    check(!notice.advance(1500ms), "first notice ages");
    notice.show("That order is not available yet", NoticeKind::unsupported, 2s);
    check(hasColour(notice, 214, 170, 82), "unsupported accent");
    check(!notice.advance(1500ms) && notice.visible(), "replacement restarts the age");
    check(notice.advance(1h) && !notice.visible(), "a long stall expires it");

    // Zero lifetime stays until cleared; empty text clears.
    notice.show("Pinned", NoticeKind::accepted, 0s);
    check(!notice.advance(24h) && notice.visible(), "pinned notice stays");
    notice.show("", NoticeKind::accepted);
    check(!notice.visible(), "empty text clears");
    try { notice.advance(-1ns); check(false, "negative elapsed is rejected"); } catch (const std::invalid_argument&) {}
    try { notice.show("x", NoticeKind::accepted, -1s); check(false, "negative lifetime is rejected"); }
    catch (const std::invalid_argument&) {}

    // Long text is bounded to a few lines instead of covering the map.
    std::string longText;
    for (int i = 0; i < 300; ++i) longText += "order feedback ";
    notice.show("Short", NoticeKind::blocked);
    const auto oneLine = notice.height();
    notice.show(longText, NoticeKind::blocked);
    check(notice.height() <= unsigned(pixelH) && notice.height() < oneLine * 4 && notice.width() <= unsigned(pixelW),
          "long notice is bounded");

    // Not modal: input events change nothing.
    for (auto event : {key(SDLK_RETURN), key(SDLK_ESCAPE), button(SDL_EVENT_MOUSE_BUTTON_DOWN, 1, 1),
                       button(SDL_EVENT_MOUSE_BUTTON_UP, 1, 1)}) notice.handle(event);
    check(notice.visible(), "input does not dismiss a notice");

    // Presentation: alone, together with a panel, and while each is removed.
    notice.draw(renderer);
    present(renderer);
    ss2::Briefing briefing(window);
    briefing.open("Panel above the notice.");
    briefing.draw(renderer);
    present(renderer);
    check(notice.advance(10s), "expires while the panel is open");
    notice.draw(renderer);
    present(renderer);
    notice.show("Back again", NoticeKind::accepted);
    notice.draw(renderer);
    check(briefing.acknowledge(), "panel closes");
    briefing.draw(renderer);
    present(renderer);

    // Oversized and tiny strips are cropped or placed without leaving the window.
    renderer.setOverlay(1024, 768, std::vector<std::uint8_t>(1024 * 768 * 4, 255), ss2::OverlaySlot::notice);
    present(renderer);
    renderer.setOverlay(1, 1, std::vector<std::uint8_t>(4, 255), ss2::OverlaySlot::notice);
    present(renderer);
    renderer.clearOverlay(ss2::OverlaySlot::notice);
    present(renderer);
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
            SDL_CreateWindow("Briefing test", 640, 480, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        int w{}, h{}, pixelW{}, pixelH{};
        SDL_GetWindowSize(window.get(), &w, &h);
        SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
        const float midX = w / 2.f, midY = h / 2.f;

        ss2::Briefing briefing(window.get());
        check(!briefing.active() && !briefing.acknowledge(), "starts closed");
        check(!briefing.handle(key(SDLK_RETURN)), "closed panel ignores keys");

        briefing.open("iii");
        const auto narrow = briefing.width();
        briefing.open(std::string(20, 'W'));
        const auto wide = briefing.width();
        briefing.open(std::string(20, 'i'));
        check(briefing.width() < wide && narrow < wide, "panel follows proportional text width");
        briefing.open("Déplacez l’unité.\n" + std::string(300, 'W'));
        check(briefing.pageCount() > 1 && briefing.width() <= unsigned(pixelW), "Unicode and long words wrap");

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
        notices(window.get(), renderer);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
