// Command bar: layout, drawing, hit-testing, and its place in a rendered frame.
#include "render/window.hpp"
#include "ui/hud.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using ss2::HudAction;
using ss2::assets::MapImage;

void check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

MapImage solid(unsigned w, unsigned h, int r, int g, int b) {
    MapImage image{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4)};
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = std::uint8_t(r); image.rgba[i + 1] = std::uint8_t(g); image.rgba[i + 2] = std::uint8_t(b); image.rgba[i + 3] = 255;
    }
    return image;
}

struct Mouse {
    SDL_Window* window;
    const ss2::Hud& hud;
    // Bar pixels to window coordinates.
    SDL_FPoint at(float x, float y) const {
        int w{}, h{}, pw{}, ph{};
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        return {x * w / pw, (y + float(ph) - float(hud.height())) * h / ph};
    }
    SDL_Event button(SDL_EventType type, float x, float y, Uint8 which = SDL_BUTTON_LEFT) const {
        SDL_Event e{};
        e.type = type;
        e.button.button = which;
        const auto p = at(x, y);
        e.button.x = p.x;
        e.button.y = p.y;
        return e;
    }
    SDL_Event motion(float x, float y, bool held) const {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        const auto p = at(x, y);
        e.motion.x = p.x;
        e.motion.y = p.y;
        e.motion.state = held ? SDL_BUTTON_LMASK : 0;
        return e;
    }
};

int countColour(const ss2::Hud& hud, ss2::Hud::Rect area, int r, int g, int b, int tolerance) {
    int n = 0;
    for (int y = area.y; y < area.y + area.height; ++y)
        for (int x = area.x; x < area.x + area.width; ++x) {
            const auto* p = &hud.rgba()[(std::size_t(y) * hud.width() + std::size_t(x)) * 4];
            if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance) ++n;
        }
    return n;
}
float centreX(ss2::Hud::Rect r) { return r.x + r.width / 2.f; }
float centreY(ss2::Hud::Rect r) { return r.y + r.height / 2.f; }
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
            SDL_CreateWindow("HUD test", 960, 600, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        int pixelW{}, pixelH{};
        SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);

        ss2::Hud hud(window.get());
        const Mouse mouse{window.get(), hud};
        check(hud.width() == unsigned(pixelW), "bar spans the window");
        check(hud.height() >= 72 && hud.height() <= unsigned(pixelH) / 3, "bar height is bounded");
        check(hud.rgba().size() == std::size_t(hud.width()) * hud.height() * 4, "pixel count");
        for (std::size_t i = 3; i < hud.rgba().size(); i += 4) check(hud.rgba()[i] == 255, "bar is opaque");

        // Geometry: everything inside the bar, nothing overlapping.
        const auto minimap = hud.minimapRect();
        check(minimap.width > 0 && minimap.height > 0 && minimap.x >= 0 && minimap.y >= 0 &&
              minimap.x + minimap.width <= pixelW && minimap.y + minimap.height <= int(hud.height()), "minimap inside the bar");
        const HudAction actions[] = {HudAction::stop, HudAction::holdFire, HudAction::crouch, HudAction::mission, HudAction::pause};
        for (auto a : actions) {
            const auto r = hud.buttonRect(a);
            check(r.width >= 20 && r.height >= 20 && r.x > minimap.x + minimap.width && r.x + r.width <= pixelW &&
                  r.y >= 0 && r.y + r.height <= int(hud.height()), "button inside the bar, right of the minimap");
            for (auto other : actions) {
                if (other == a) continue;
                const auto o = hud.buttonRect(other);
                check(r.x + r.width <= o.x || o.x + o.width <= r.x || r.y + r.height <= o.y || o.y + o.height <= r.y, "buttons do not overlap");
            }
        }
        check(hud.buttonRect(HudAction::moveCamera).width == 0, "moveCamera has no button");

        // Minimap takes the shape of the map it is given.
        hud.setMinimap(solid(200, 100, 40, 90, 50));
        const auto wide = hud.minimapRect();
        check(std::abs(double(wide.width) / wide.height - 2.0) < 0.1, "minimap keeps the map's shape");
        check(countColour(hud, wide, 40, 90, 50, 3) > wide.width * wide.height / 2, "overview is drawn in the minimap");

        // Dots and the view rectangle.
        ss2::HudState state;
        state.dots = {{0.25f, 0.5f, false}, {0.75f, 0.5f, true}};
        state.viewU = 0.4f; state.viewV = 0.3f; state.viewW = 0.2f; state.viewH = 0.4f;
        hud.update(state);
        const auto half = [&](bool right) {
            auto r = hud.minimapRect();
            r.width /= 2;
            if (right) r.x += r.width;
            return r;
        };
        check(countColour(hud, half(false), 110, 224, 120, 12) > 0, "friendly dot on the left");
        check(countColour(hud, half(true), 232, 72, 60, 12) > 0, "hostile dot on the right");
        check(countColour(hud, half(true), 110, 224, 120, 12) == 0, "no friendly dot on the right");
        check(countColour(hud, hud.minimapRect(), 245, 245, 240, 6) > 8, "view rectangle is drawn");

        // Drawing follows the state; an unchanged state does not redraw.
        const auto before = hud.rgba();
        hud.update(state);
        check(hud.rgba() == before, "same state, same pixels");
        state.selection = {{"rifle", 12, 0.9f, 0.5f}, {"officer", 1, 0.3f, 1.f}};
        state.losses = 3;
        hud.update(state);
        check(hud.rgba() != before, "selection changes the readout");
        const auto withSelection = hud.rgba();
        state.holdFire = true;
        hud.update(state);
        check(hud.rgba() != withSelection, "a toggle changes its button");
        state.crouched = state.paused = true;
        hud.update(state);
        hud.setPortrait("rifle", solid(8, 8, 250, 10, 250));
        check(countColour(hud, {0, 0, int(hud.width()), int(hud.height())}, 250, 10, 250, 3) > 16, "portrait is drawn");
        ss2::HudState crowded = state;
        for (int i = 0; i < 40; ++i) crowded.selection.push_back({"class" + std::to_string(i), unsigned(i), 0.5f, 0.5f});
        hud.update(crowded);   // More classes than fit must not overflow or throw.
        hud.update(state);

        // Buttons: the action is reported on release over the same button.
        for (auto a : actions) {
            const auto r = hud.buttonRect(a);
            auto down = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(r), centreY(r)));
            check(down.consumed && down.action == HudAction::none, "press is consumed and reports nothing yet");
            auto up = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(r), centreY(r)));
            check(up.consumed && up.action == a, "release reports the button's action");
        }
        const auto stop = hud.buttonRect(HudAction::stop), crouch = hud.buttonRect(HudAction::crouch);
        hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(stop), centreY(stop)));
        auto slid = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(crouch), centreY(crouch)));
        check(slid.consumed && slid.action == HudAction::none, "release on another button cancels");
        hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(stop), centreY(stop)));
        auto outside = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(stop), -200));
        check(outside.consumed && outside.action == HudAction::none, "release above the bar cancels but is still consumed");
        auto right = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(stop), centreY(stop), SDL_BUTTON_RIGHT));
        check(right.consumed && right.action == HudAction::none, "right press on the bar is swallowed");
        check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(stop), centreY(stop), SDL_BUTTON_RIGHT)).consumed, "right release too");

        // Strip: objective, control groups, message log and hover hints all change the picture.
        {
            auto shot = hud.rgba();
            const auto changed = [&](const char* what) {
                check(hud.rgba() != shot, what);
                shot = hud.rgba();
            };
            state.objective = "Reach the crossroads";
            hud.update(state);
            changed("objective is drawn");
            state.groups = {{1, 12}, {4, 3}};
            hud.update(state);
            changed("control groups are drawn");
            check(hud.logSize() == 0 && hud.logText().empty(), "log starts empty");
            hud.log("First message");
            changed("latest message is drawn");
            hud.log("Second\nmessage");
            hud.log("");
            check(hud.logSize() == 2, "empty messages are ignored");
            check(hud.logText() == "First message\nSecond message", "log keeps order and stays one line per entry");
            check(hud.logText(1) == "Second message", "log can return only the newest");
            hud.log("Third\r\nline\rwith\tbreaks\r\n");
            check(hud.logText(1) == "Third line with breaks", "CR, LF and tabs collapse to single spaces");
            hud.log("\r\n");
            check(hud.logSize() == 3, "a message of only line breaks is ignored");
            for (int i = 0; i < 1000; ++i) hud.log("m" + std::to_string(i));
            check(hud.logSize() <= 400 && hud.logText(1) == "m999", "log is bounded and keeps the newest");
            shot = hud.rgba();
            const auto log = hud.buttonRect(HudAction::log);
            check(log.width >= 20, "log button exists");
            check(hud.handle(mouse.motion(centreX(log), centreY(log), false)).consumed, "hover is consumed");
            changed("hovering a button highlights it and shows its hint");
            hud.handle(mouse.motion(centreX(log), -100, false));
            changed("leaving the bar removes the hover state");
            hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(log), centreY(log)));
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(log), centreY(log))).action == HudAction::log, "log button reports its action");
        }

        // The map area above the bar is left alone.
        check(!hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, 100, -50)).consumed, "press above the bar passes through");
        check(!hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, 100, -50)).consumed, "release above the bar passes through");
        check(!hud.handle(mouse.motion(100, -50, false)).consumed, "motion above the bar passes through");
        check(hud.handle(mouse.motion(100, 10, false)).consumed, "motion over the bar is consumed");
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.key = SDLK_S;
        check(!hud.handle(key).consumed, "keys are never consumed");

        // Minimap: press reports the map fraction; a drag keeps reporting, clamped at the edges.
        const auto m = hud.minimapRect();
        auto click = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, m.x + m.width * 0.25f, m.y + m.height * 0.75f));
        check(click.consumed && click.action == HudAction::moveCamera, "minimap press moves the camera");
        check(std::abs(click.u - 0.25f) < 0.02f && std::abs(click.v - 0.75f) < 0.03f, "minimap fraction");
        auto drag = hud.handle(mouse.motion(m.x + m.width * 0.6f, m.y + m.height * 0.2f, true));
        check(drag.consumed && drag.action == HudAction::moveCamera && std::abs(drag.u - 0.6f) < 0.02f && std::abs(drag.v - 0.2f) < 0.03f,
              "minimap drag");
        drag = hud.handle(mouse.motion(m.x + m.width * 3.f, -300, true));
        check(drag.consumed && drag.action == HudAction::moveCamera && drag.u == 1.f && drag.v == 0.f, "drag outside the minimap clamps");
        check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, m.x + m.width * 3.f, -300)).consumed, "release ends the drag");
        check(!hud.handle(mouse.motion(m.x + m.width * 3.f, -300, true)).consumed, "no drag after release");
        auto noButton = hud.handle(mouse.motion(m.x + 4, m.y + 4, false));
        check(noButton.consumed && noButton.action == HudAction::none, "hover over the minimap does not move the camera");

        // Bad input is rejected.
        try { hud.setMinimap({4, 4, std::vector<std::uint8_t>(8)}); check(false, "short minimap accepted"); } catch (const std::invalid_argument&) {}
        try { ss2::HudState bad; bad.dots = {{std::nanf(""), 0, false}}; hud.update(bad); check(false, "NaN dot accepted"); }
        catch (const std::invalid_argument&) {}

        // In a frame: the bar is flush with the bottom, the notice sits above it, the panel is on top.
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        renderer.setMap(solid(64, 48, 90, 90, 90));
        hud.draw(renderer);
        auto frame = renderer.captureFrame();
        const auto framePixel = [&](int x, int y) { return &frame.rgba[(std::size_t(y) * frame.width + std::size_t(x)) * 4]; };
        const int barTop = int(frame.height) - int(hud.height());
        for (auto [x, y] : {std::pair{3, 3}, {int(hud.width()) / 2, int(hud.height()) / 2}, {int(hud.width()) - 4, int(hud.height()) - 2}}) {
            const auto* want = &hud.rgba()[(std::size_t(y) * hud.width() + std::size_t(x)) * 4];
            const auto* got = framePixel(x, barTop + y);
            check(got[0] == want[0] && got[1] == want[1] && got[2] == want[2], "bar pixels reach the screen unchanged");
        }
        renderer.setOverlay(60, 20, solid(60, 20, 1, 2, 3).rgba, ss2::OverlaySlot::notice);
        frame = renderer.captureFrame();
        int noticeBottom = -1;
        for (int y = 0; y < int(frame.height); ++y) {
            const auto* p = framePixel(int(frame.width) / 2, y);
            if (p[0] == 1 && p[1] == 2 && p[2] == 3) noticeBottom = y;
        }
        check(noticeBottom >= 0 && noticeBottom < barTop, "notice sits above the bar");
        renderer.setOverlay(40, int(frame.height), solid(40, frame.height, 7, 8, 9).rgba, ss2::OverlaySlot::panel);
        frame = renderer.captureFrame();
        const auto* over = framePixel(int(frame.width) / 2, barTop + 5);
        check(over[0] == 7 && over[1] == 8 && over[2] == 9, "panel is drawn over the bar");
        renderer.clearOverlay(ss2::OverlaySlot::panel);
        renderer.clearOverlay(ss2::OverlaySlot::notice);

        // Resize: the bar follows the window.
        if (!SDL_SetWindowSize(window.get(), 640, 400) || !SDL_SyncWindow(window.get())) throw std::runtime_error(SDL_GetError());
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            renderer.handleEvent(event);
            hud.handle(event);
        }
        SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
        SDL_Event resized{};
        resized.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
        resized.window.windowID = SDL_GetWindowID(window.get());
        hud.handle(resized);
        check(hud.width() == unsigned(pixelW) && hud.height() <= unsigned(pixelH) / 3 + 1, "bar follows a resize");
        hud.draw(renderer);
        frame = renderer.captureFrame();
        check(frame.width == unsigned(pixelW), "frame follows a resize");

        // The window changes size and nobody passes the bar the event (a caller may route
        // events elsewhere while a dialog is open). It must still fit the new window, and
        // must not keep swallowing clicks where the old bar was.
        {
            int oldW{}, oldH{};
            SDL_GetWindowSizeInPixels(window.get(), &oldW, &oldH);
            const int oldBarTop = oldH - int(hud.height());
            if (!SDL_SetWindowSize(window.get(), 1100, 900)) throw std::runtime_error(SDL_GetError());
            SDL_SyncWindow(window.get());
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
            int w{}, h{}, pixelW{}, pixelH{};
            SDL_GetWindowSize(window.get(), &w, &h);
            SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
            check(pixelW != oldW && pixelH != oldH, "test window was resized");
            SDL_Event press{};
            press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            press.button.button = SDL_BUTTON_RIGHT;
            press.button.x = 200.f * w / pixelW;
            press.button.y = (oldBarTop + 20.f) * h / pixelH;   // inside the old bar, now open map
            check(!hud.handle(press).consumed, "a right click where the old bar was passes through");
            check(hud.width() == unsigned(pixelW), "bar width follows the window without an event");
            check(int(hud.height()) < pixelH / 2 && hud.rgba().size() == std::size_t(hud.width()) * hud.height() * 4, "bar picture matches its new size");
            press.button.y = (pixelH - 5.f) * h / pixelH;
            check(hud.handle(press).consumed, "a click on the new bar is consumed");
            press.button.y = (pixelH - float(hud.height()) - 5.f) * h / pixelH;
            check(!hud.handle(press).consumed, "a click just above the new bar passes through");
            // update() and draw() fit it too.
            if (!SDL_SetWindowSize(window.get(), 700, 500)) throw std::runtime_error(SDL_GetError());
            SDL_SyncWindow(window.get());
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
            SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
            hud.update(state);
            check(hud.width() == unsigned(pixelW), "update fits the bar to the window");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
