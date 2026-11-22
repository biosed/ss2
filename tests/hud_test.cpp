// Command bar: layout, drawing, hit-testing, and its place in a rendered frame.
#include "render/window.hpp"
#include "ui/hud.hpp"
#include "ui/strings.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
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
        check(hud.rgba()[(std::size_t(3) * hud.width() + 3) * 4 + 3] == 255, "left console is opaque");
        check(hud.rgba()[(std::size_t(hud.height() - 3) * hud.width() + hud.width() - 4) * 4 + 3] == 255, "bottom strip is opaque");
        check(hud.rgba()[(std::size_t(3) * hud.width() + hud.width() - 4) * 4 + 3] == 0, "upper-right L-cutout is transparent");

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

        // Minimap takes the shape of the map it is given (clipped inside an isometric diamond).
        hud.setMinimap(solid(200, 100, 40, 90, 50));
        const auto wide = hud.minimapRect();
        check(std::abs(double(wide.width) / wide.height - 2.0) < 0.1, "minimap keeps the map's shape");
        check(countColour(hud, wide, 40, 90, 50, 3) > wide.width * wide.height / 3, "overview is drawn in the diamond minimap");

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
        // With nothing selected, unit-order slots (e.g. Stop) are blank and do not fire actions.
        {
            const auto stopEmpty = hud.buttonRect(HudAction::stop);
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(stopEmpty), centreY(stopEmpty))).action == HudAction::none,
                  "blank unit slot press does not fire when nothing is selected");
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(stopEmpty), centreY(stopEmpty))).action == HudAction::none,
                  "blank unit slot release does not fire when nothing is selected");
        }
        state.selection = {{"rifle", 12, 0.9f, 0.5f}};
        hud.update(state);
        const auto rifleOnly = hud.rgba();
        check(rifleOnly != before, "selection changes the readout and lights up unit orders");
        state.selection = {{"rifle", 12, 0.9f, 0.5f}, {"officer", 1, 0.3f, 1.f}};
        state.losses = 3;
        hud.update(state);
        check(hud.rgba() != rifleOnly, "adding a specialist (officer) updates the context-sensitive grid and readout");
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

        // EX / L / M / A / R bottom meter lip renders proportional fills when hasSelection is true.
        const auto beforeMeters = hud.rgba();
        state.hasSelection = true;
        state.meters = {0.8f, 0.9f, 0.7f, 0.6f, 1.0f};
        hud.update(state);
        check(hud.rgba() != beforeMeters, "EX/L/M/A/R meters visibly change the HUD pixels");

        // Buttons: the action is reported on release over the same button.
        for (auto a : actions) {
            const auto r = hud.buttonRect(a);
            auto down = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(r), centreY(r)));
            check(down.consumed && down.action == HudAction::none, "press is consumed and reports nothing yet");
            auto up = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(r), centreY(r)));
            check(up.consumed && up.action == a, "release reports the button's action");
        }
        {
            // Lowercase "officer" in state.selection activates Recon hit-testing and buttonRect().
            const auto reconR = hud.buttonRect(HudAction::recon);
            check(reconR.width >= 20 && reconR.height >= 20, "recon buttonRect active for lowercase officer selection");
            auto down = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(reconR), centreY(reconR)));
            check(down.consumed && down.action == HudAction::none, "recon press consumed");
            auto up = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(reconR), centreY(reconR)));
            check(up.consumed && up.action == HudAction::recon, "recon release reports HudAction::recon");
        }
        {
            // Phase 4H interactive row-3 tactical grid: AttackMove, Patrol, Unload, Mine, Bombard.
            ss2::HudState rState = state;
            rState.selection = {{"rifle", 6, 1.f, 1.f}};
            rState.binoculars = false;
            hud.update(rState);
            const auto amR = hud.buttonRect(HudAction::attackMove);
            const auto patR = hud.buttonRect(HudAction::patrol);
            check(amR.width >= 20 && patR.width >= 20, "standard selection exposes AttackMove and Patrol");
            hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(amR), centreY(amR)));
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(amR), centreY(amR))).action == HudAction::attackMove,
                  "clicking row-3 col-0 fires HudAction::attackMove");
            hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(patR), centreY(patR)));
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(patR), centreY(patR))).action == HudAction::patrol,
                  "clicking row-3 col-1 fires HudAction::patrol");
            const auto unarmedPixels = hud.rgba();
            rState.attackMoveArmed = true;
            hud.update(rState);
            check(hud.rgba() != unarmedPixels, "attackMoveArmed visibly highlights row-3 cell");

            ss2::HudState sapState = rState;
            sapState.attackMoveArmed = false;
            sapState.hasSapper = true;
            hud.update(sapState);
            const auto mineR = hud.buttonRect(HudAction::mine);
            const auto patR3 = hud.buttonRect(HudAction::patrol);
            check(mineR.width >= 20 && patR3.width >= 20 && mineR.x != patR3.x, "sapper exposes Mine and shifts Patrol to col 2");
            hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(mineR), centreY(mineR)));
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(mineR), centreY(mineR))).action == HudAction::mine,
                  "clicking Mine button fires HudAction::mine");

            ss2::HudState vehState = rState;
            vehState.attackMoveArmed = false;
            vehState.hasCargo = true;
            vehState.hasBombard = true;
            hud.update(vehState);
            const auto unlR = hud.buttonRect(HudAction::unload);
            const auto bmbR = hud.buttonRect(HudAction::attackGround);
            check(unlR.width >= 20 && bmbR.width >= 20, "cargo+armed vehicle exposes Unload and Bombard");
            hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(unlR), centreY(unlR)));
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(unlR), centreY(unlR))).action == HudAction::unload,
                  "clicking Unload button fires HudAction::unload");
            hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(bmbR), centreY(bmbR)));
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(bmbR), centreY(bmbR))).action == HudAction::attackGround,
                  "clicking Bombard button fires HudAction::attackGround");
            const auto bmbBefore = hud.rgba();
            vehState.attackGroundArmed = true;
            hud.update(vehState);
            check(hud.rgba() != bmbBefore, "attackGroundArmed visibly highlights Bombard button");
            hud.update(state);
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
        check(!hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, float(hud.width()) - 20, 4)).consumed,
              "click in transparent upper-right L-cutout passes through to the map");
        {
            const auto cutoutPt = mouse.at(float(hud.width()) - 20.f, 4.f);
            check(!hud.opaqueAtWindow(cutoutPt.x, cutoutPt.y), "opaqueAtWindow false in transparent upper-right L-cutout");
            const auto trayPt = mouse.at(float(hud.width()) - 20.f, float(hud.height()) - 10.f);
            check(hud.opaqueAtWindow(trayPt.x, trayPt.y), "opaqueAtWindow true in bottom-right status tray");
        }
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
        // Lower-left power emblem opens the pause menu; extreme bounding corner outside diamond does not move camera.
        auto corner = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, float(m.x + 1), float(m.y + 1)));
        check(corner.consumed && corner.action == HudAction::none, "corner outside minimap diamond does not move camera");
        hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, float(m.x + 1), float(m.y + 1)));
        const float powerX = float(m.x) + 11.f, powerY = float(m.y + m.height) - 22.f;
        check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, powerX, powerY)).consumed, "power emblem press consumed");
        check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, powerX, powerY)).action == HudAction::menu, "power emblem opens menu");

        // Bad input is rejected.
        try { hud.setMinimap({4, 4, std::vector<std::uint8_t>(8)}); check(false, "short minimap accepted"); } catch (const std::invalid_argument&) {}
        try { ss2::HudState bad; bad.dots = {{std::nanf(""), 0, false}}; hud.update(bad); check(false, "NaN dot accepted"); }
        catch (const std::invalid_argument&) {}

        // In a frame: the bar is flush with the bottom, the notice sits above it, the panel is on top.
        const bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        renderer.setMap(solid(960, 600, 90, 90, 90));
        hud.draw(renderer);
        auto frame = renderer.captureFrame();
        const auto framePixel = [&](int x, int y) { return &frame.rgba[(std::size_t(y) * frame.width + std::size_t(x)) * 4]; };
        const int barTop = int(frame.height) - int(hud.height());
        for (auto [x, y] : {std::pair{3, 3}, {int(hud.width()) / 2, int(hud.height()) - 12}, {int(hud.width()) - 4, int(hud.height()) - 2}}) {
            const auto* want = &hud.rgba()[(std::size_t(y) * hud.width() + std::size_t(x)) * 4];
            const auto* got = framePixel(x, barTop + y);
            check(std::abs(int(got[0]) - int(want[0])) <= 1 && std::abs(int(got[1]) - int(want[1])) <= 1 &&
                  std::abs(int(got[2]) - int(want[2])) <= 1, "opaque HUD pixels reach the screen unchanged");
        }
        // The transparent upper-right L-cutout reveals the underlying map (90, 90, 90).
        {
            const auto* cutout = framePixel(int(hud.width()) * 3 / 4, barTop + 4);
            check(cutout[0] == 90 && cutout[1] == 90 && cutout[2] == 90, "L-cutout shows the live map beneath");
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
        check(countColour(hud, {0, 0, int(hud.width()), int(hud.height())}, 250, 10, 250, 3) > 16, "portrait stays drawn at 640x400");

        // Changing the interface language redraws the bar on the next update, at the same size.
        {
            hud.update(state);
            const auto english = hud.rgba();
            const auto wide = hud.width(), tall = hud.height();
            ss2::setLanguage(ss2::Language::french);
            hud.update(state);
            const auto french = hud.rgba();
            check(french != english && hud.width() == wide && hud.height() == tall, "French redraws the bar at the same size");
            ss2::setLanguage(ss2::Language::spanish);
            hud.update(state);
            check(hud.rgba() != french && hud.rgba() != english, "Spanish differs from both");
            ss2::setLanguage(ss2::Language::english);
            hud.update(state);
            check(hud.rgba() == english, "back to English restores the picture");
        }

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
        {
            int w{}, h{}, pixelW{}, pixelH{};
            SDL_GetWindowSize(window.get(), &w, &h);
            SDL_GetWindowSizeInPixels(window.get(), &pixelW, &pixelH);
            const auto beforePings = hud.rgba();
            const auto m = hud.minimapRect();
            const int barTop = pixelH - int(hud.height());
            SDL_Event rclick{};
            rclick.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            rclick.button.button = SDL_BUTTON_RIGHT;
            rclick.button.x = float(m.x + m.width / 2) * w / pixelW;
            rclick.button.y = float(barTop + m.y + m.height / 2) * h / pixelH;
            const auto rres = hud.handle(rclick);
            check(rres.consumed && rres.action == ss2::HudAction::orderMinimap,
                  "right-click inside minimap issues orderMinimap action");
            check(std::abs(rres.u - 0.5f) < 0.08f && std::abs(rres.v - 0.5f) < 0.08f,
                  "minimap right-click reports normalized map coordinates");

            auto pingState = state;
            pingState.pings = {{0.5f, 0.5f, true, 0.75f}};
            pingState.dots.push_back({0.4f, 0.5f, false, true, false});
            pingState.dots.push_back({0.6f, 0.5f, false, false, true});
            hud.update(pingState);
            check(hud.rgba() != beforePings, "minimap tactical ping and allied/neutral dots redraw the HUD");

            const auto beforeAir = hud.rgba();
            auto airState = pingState;
            airState.airReady = {2, 1, 0, 1, 0};
            airState.armedAir = 0;
            airState.planes = {{0.35f, 0.45f, false}, {0.65f, 0.55f, true}};
            hud.update(airState);
            check(hud.rgba() != beforeAir, "air support pills and minimap aircraft blips redraw the HUD");
            const auto r0 = hud.airButtonRect(0);
            const auto r1 = hud.airButtonRect(1);
            const auto r3 = hud.airButtonRect(3);
            check(r0.width > 0 && r0.height > 0 && r1.width > 0 && r1.height > 0 && r3.width > 0 && r3.height > 0,
                  "available air support slots have positive hit rects");
            check(hud.airButtonRect(2).width == 0 && hud.airButtonRect(4).width == 0,
                  "zero-count air support slots have empty hit rects");
            const auto downAir = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(r1), centreY(r1)));
            check(downAir.consumed, "air support pill press is consumed");
            const auto upAir = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(r1), centreY(r1)));
            check(upAir.consumed && upAir.action == ss2::HudAction::airSupport && upAir.airSlot == 1,
                  "clicking air support pill reports HudAction::airSupport and slot index");
            hud.draw(renderer);
            frame = renderer.captureFrame();
            const int curBarTop = int(frame.height) - int(hud.height());
            const auto* cutoutAfter = framePixel(int(hud.width()) * 3 / 4, curBarTop + 4);
            check(cutoutAfter[0] == 90 && cutoutAfter[1] == 90 && cutoutAfter[2] == 90,
                  "air support strip stays strictly below the transparent upper-right L-cutout");
        }
        {
            // Phase 4J: minimap objective marker star, clickable group chips & class filter cards, Scatter button.
            if (!SDL_SetWindowSize(window.get(), 1100, 700)) throw std::runtime_error(SDL_GetError());
            SDL_SyncWindow(window.get());
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
            ss2::HudState p4jState;
            p4jState.hasSelection = true;
            p4jState.hasInfantry = true;
            p4jState.selection = {{"rifle", 12, 0.9f, 0.8f}, {"officer", 1, 1.0f, 1.0f}};
            p4jState.groups = {{1, 8}, {4, 3}};
            p4jState.hasObjectiveMarker = true;
            p4jState.objectiveU = 0.5f;
            p4jState.objectiveV = 0.5f;
            hud.update(p4jState);

            check(countColour(hud, hud.minimapRect(), 246, 216, 84, 15) > 4,
                  "Phase 4J minimap objective marker star is rendered");

            const auto g1 = hud.groupChipRect(1);
            const auto g4 = hud.groupChipRect(4);
            check(g1.width > 0 && g1.height > 0 && g4.width > 0 && g4.height > 0,
                  "Phase 4J control group chips have valid rects");
            check(g1.x + g1.width <= g4.x, "Phase 4J group chips do not overlap");
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(g1), centreY(g1))).consumed,
                  "group chip 1 press consumed");
            const auto g1Up = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(g1), centreY(g1)));
            check(g1Up.consumed && g1Up.action == HudAction::recallGroup && g1Up.groupDigit == 1,
                  "clicking group chip 1 emits HudAction::recallGroup digit 1");
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(g4), centreY(g4))).consumed,
                  "group chip 4 press consumed");
            const auto g4Up = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(g4), centreY(g4)));
            check(g4Up.consumed && g4Up.action == HudAction::recallGroup && g4Up.groupDigit == 4,
                  "clicking group chip 4 emits HudAction::recallGroup digit 4");

            const auto c0 = hud.classCardRect(0);
            const auto c1 = hud.classCardRect(1);
            check(c0.width > 0 && c0.height > 0 && c1.width > 0 && c1.height > 0,
                  "Phase 4J class cards have valid rects");
            check(c0.x + c0.width <= c1.x, "Phase 4J class cards do not overlap");
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(c0), centreY(c0))).consumed,
                  "class card 0 press consumed");
            const auto c0Up = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(c0), centreY(c0)));
            check(c0Up.consumed && c0Up.action == HudAction::selectClass && c0Up.classLabel == "rifle",
                  "clicking class card 0 emits HudAction::selectClass for rifle");
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(c1), centreY(c1))).consumed,
                  "class card 1 press consumed");
            const auto c1Up = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(c1), centreY(c1)));
            check(c1Up.consumed && c1Up.action == HudAction::selectClass && c1Up.classLabel == "officer",
                  "clicking class card 1 emits HudAction::selectClass for officer");

            const auto scR = hud.buttonRect(HudAction::scatter);
            check(scR.width >= 20 && scR.height >= 20, "Phase 4J infantry selection exposes Scatter button");
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(scR), centreY(scR))).consumed,
                  "Scatter button press consumed");
            check(hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(scR), centreY(scR))).action == HudAction::scatter,
                  "clicking Scatter button emits HudAction::scatter");

            auto crowded4j = p4jState;
            crowded4j.selection.clear();
            for (int i = 0; i < 40; ++i) crowded4j.selection.push_back({"class" + std::to_string(i), 1u, 1.f, 1.f});
            hud.update(crowded4j);
            std::size_t lastClickable = 0;
            while (lastClickable < crowded4j.selection.size() && hud.classCardRect(lastClickable).width > 0) {
                ++lastClickable;
            }
            check(lastClickable > 0 && lastClickable < crowded4j.selection.size(),
                  "crowded overflow card has empty classCardRect");
            const auto prevCard = hud.classCardRect(lastClickable - 1);
            const auto stepX = prevCard.width + 4;
            const auto overResDown = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(prevCard) + float(stepX), centreY(prevCard)));
            const auto overResUp = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(prevCard) + float(stepX), centreY(prevCard)));
            check(overResDown.consumed && overResUp.action == HudAction::none,
                  "clicking +N more overflow card does not emit selectClass");
            std::cout << "Phase 4J interactive HUD cards, group chips, minimap star & scatter PASSED\n";

            // Phase 4S: Interactive HUD command-bar tooltips & cancelMinimapDrag.
            auto p4sState = p4jState;
            p4sState.airReady = {2, 1, 1, 1, 1};
            hud.update(p4sState);
            const auto tipAtRect = [&](ss2::Hud::Rect r) {
                const auto pt = mouse.at(centreX(r), centreY(r));
                return hud.tooltipAtWindow(pt.x, pt.y);
            };
            const auto stopTip = tipAtRect(hud.buttonRect(HudAction::stop));
            check(stopTip && stopTip->title == "Stop (S)" && !stopTip->lines.empty(),
                  "Phase 4S stop button tooltip mismatch");
            const auto scTip = tipAtRect(hud.buttonRect(HudAction::scatter));
            check(scTip && scTip->title == "Scatter (D)", "Phase 4S scatter button tooltip mismatch");
            const auto airTip0 = tipAtRect(hud.airButtonRect(0));
            check(airTip0 && airTip0->title.find("Bomber Strike") != std::string::npos &&
                  !airTip0->lines.empty() && airTip0->lines[0].find("2") != std::string::npos,
                  "Phase 4S air support slot 0 tooltip matches Bomber Strike");
            const auto airTip1 = tipAtRect(hud.airButtonRect(1));
            check(airTip1 && airTip1->title.find("Recon Plane") != std::string::npos,
                  "Phase 4S air support slot 1 tooltip matches Recon Plane");
            const auto airTip2 = tipAtRect(hud.airButtonRect(2));
            check(airTip2 && airTip2->title.find("Paratroopers") != std::string::npos,
                  "Phase 4S air support slot 2 tooltip matches Paratroopers");
            const auto airTip3 = tipAtRect(hud.airButtonRect(3));
            check(airTip3 && airTip3->title.find("Supply Drop") != std::string::npos,
                  "Phase 4S air support slot 3 tooltip matches Supply Drop");
            const auto airTip4 = tipAtRect(hud.airButtonRect(4));
            check(airTip4 && airTip4->title.find("Fighter Strafe") != std::string::npos,
                  "Phase 4S air support slot 4 tooltip matches Fighter Strafe");
            const auto grpTip = tipAtRect(hud.groupChipRect(1));
            check(grpTip && grpTip->title == "Control Group 1" &&
                  !grpTip->lines.empty() && grpTip->lines[0].find("8") != std::string::npos,
                  "Phase 4S group chip tooltip mismatch");
            const auto clsTip = tipAtRect(hud.classCardRect(0));
            check(clsTip && clsTip->title == "rifle" && !clsTip->lines.empty(),
                  "Phase 4S class card tooltip mismatch");
            check(!tipAtRect(hud.minimapRect()).has_value(),
                  "Phase 4S minimap should not emit button tooltip");

            const auto mmR = hud.minimapRect();
            const auto mmDown = hud.handle(mouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(mmR), centreY(mmR)));
            check(mmDown.consumed && mmDown.action == HudAction::moveCamera,
                  "Phase 4S minimap click did not start camera move");
            hud.cancelMinimapDrag();
            const auto mmDrag = hud.handle(mouse.motion(centreX(mmR) + 4.f, centreY(mmR) + 2.f, true));
            check(mmDrag.action == HudAction::none, "Phase 4S cancelMinimapDrag did not suppress drag");
            std::cout << "Phase 4S HUD command-bar tooltips & cancelMinimapDrag PASSED\n";

            // Phase 4W: Right-click class card emits HudAction::selectClass with shiftHeld=true (exclude).
            const auto cls0 = hud.classCardRect(0);
            SDL_Event rDown{};
            rDown.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            rDown.button.windowID = SDL_GetWindowID(window.get());
            rDown.button.button = SDL_BUTTON_RIGHT;
            const auto rPt = mouse.at(centreX(cls0), centreY(cls0));
            rDown.button.x = rPt.x;
            rDown.button.y = rPt.y;
            const auto rRes = hud.handle(rDown);
            check(rRes.consumed && rRes.action == HudAction::selectClass &&
                  rRes.classLabel == "rifle" && rRes.shiftHeld,
                  "Phase 4W right-click class card did not emit exclude selectClass");
            std::cout << "Phase 4W HUD class card right-click exclude PASSED\n";

            // Retail Gold 2.2 skin fallback, Weather overlay (Rain/Snow/Fog), opaqueAtWindow invariance,
            // and damaged-aircraft smoke/fire trails.
            {
                if (!SDL_SetWindowSize(window.get(), 800, 600)) throw std::runtime_error(SDL_GetError());
                SDL_SyncWindow(window.get());
                SDL_Event resizeEv;
                while (SDL_PollEvent(&resizeEv)) {
                    renderer.handleEvent(resizeEv);
                }
                int wLog{}, hLog{}, pw{}, ph{};
                SDL_GetWindowSize(window.get(), &wLog, &hLog);
                SDL_GetWindowSizeInPixels(window.get(), &pw, &ph);

                ss2::Hud weatherHud(window.get());
                check(!weatherHud.hasRetailAssets() && weatherHud.panelRecordCount() == 0,
                      "fresh Hud starts in asset-free procedural mode");
                check(!weatherHud.loadRetailAssets("/nonexistent/ss2/gold/install"),
                      "loadRetailAssets returns false when retail archives are absent");
                check(!weatherHud.hasRetailAssets(), "failed loadRetailAssets preserves procedural fallback");

                // Synthetic damaged-aircraft trail lifecycle.
                weatherHud.recordAirSortieTrail(101u, 200, 200, 8u, 100, 100, true, true, false);
                check(!weatherHud.hasActiveAirTrails(), "undamaged aircraft emits no smoke/fire trail");
                weatherHud.recordAirSortieTrail(101u, 220, 200, 8u, 40, 100, true, true, true);
                weatherHud.recordAirSortieTrail(101u, 240, 200, 8u, 35, 100, true, true, false);
                check(weatherHud.hasActiveAirTrails(), "AA-damaged aircraft emits trailing smoke/fire puffs");
                std::vector<ss2::MapBeam> trailBeams;
                weatherHud.appendAirTrailBeams(1, 16u, trailBeams);
                check(trailBeams.size() >= 2u, "damaged aircraft trail populates smoke/fire beams");
                for (int step = 0; step < 12; ++step) (void)weatherHud.stepAirTrails(0.20f);
                check(!weatherHud.hasActiveAirTrails(), "aircraft smoke/fire trails expire cleanly over time");

                // Weather overlay: Clear vs Rain vs Snow vs Fog in real Vulkan frames + opaqueAtWindow invariance.
                renderer.setMap(solid(unsigned(pw), unsigned(ph), 72, 88, 64));
                ss2::HudState ws;
                ws.hasSelection = true;
                ws.selection = {{"rifle", 6, 1.0f, 1.0f}};
                weatherHud.update(ws);
                weatherHud.setWeather(ss2::assets::WeatherKind::Clear, 0u);
                weatherHud.draw(renderer);
                const auto clearFrame = renderer.captureFrame();
                const int wBarTop = int(clearFrame.height) - int(weatherHud.height());

                const auto checkViewportPassThrough = [&](const char* label) {
                    for (int gy = 16; gy < wBarTop - 8; gy += 48) {
                        for (int gx = 24; gx < pw - 24; gx += 96) {
                            const float lx = float(gx) * float(wLog) / float(pw);
                            const float ly = float(gy) * float(hLog) / float(ph);
                            check(!weatherHud.opaqueAtWindow(lx, ly), label);
                        }
                    }
                    // Transparent upper-right L-cutout must also stay non-opaque in all weather modes.
                    const float cutX = float(pw - 24) * float(wLog) / float(pw);
                    const float cutY = float(wBarTop + 4) * float(hLog) / float(ph);
                    check(!weatherHud.opaqueAtWindow(cutX, cutY), label);
                    // Opaque console plates must remain opaque.
                    const float barX = 16.0f * float(wLog) / float(pw);
                    const float barY = float(ph - 12) * float(hLog) / float(ph);
                    check(weatherHud.opaqueAtWindow(barX, barY), "HUD console stays opaque under weather");
                };
                checkViewportPassThrough("opaqueAtWindow false over Clear viewport");

                const auto countViewportDiffs = [&](const MapImage& a, const MapImage& b) {
                    int diffs = 0;
                    for (int y = 8; y < wBarTop - 8; ++y) {
                        for (int x = 8; x < int(a.width) - 8; ++x) {
                            const std::size_t idx = (std::size_t(y) * a.width + std::size_t(x)) * 4u;
                            if (a.rgba[idx] != b.rgba[idx] || a.rgba[idx + 1] != b.rgba[idx + 1] ||
                                a.rgba[idx + 2] != b.rgba[idx + 2]) {
                                ++diffs;
                            }
                        }
                    }
                    return diffs;
                };

                weatherHud.setWeather(ss2::assets::WeatherKind::Rain, 4u);
                weatherHud.draw(renderer);
                const auto rainFrameA = renderer.captureFrame();
                check(countViewportDiffs(clearFrame, rainFrameA) > 500, "WeatherKind::Rain visibly alters viewport pixels");
                checkViewportPassThrough("opaqueAtWindow stays false over Rain viewport");

                weatherHud.setWeather(ss2::assets::WeatherKind::Rain, 11u);
                weatherHud.draw(renderer);
                const auto rainFrameB = renderer.captureFrame();
                check(countViewportDiffs(rainFrameA, rainFrameB) > 200, "WeatherKind::Rain streaks animate across ticks");

                weatherHud.setWeather(ss2::assets::WeatherKind::Rain, 4u);
                weatherHud.draw(renderer);
                const auto rainRepeat = renderer.captureFrame();
                check(rainRepeat.rgba == rainFrameA.rgba, "WeatherKind::Rain rendering is deterministic for a given tick");

                weatherHud.setWeather(ss2::assets::WeatherKind::Snow, 6u);
                weatherHud.draw(renderer);
                const auto snowFrame = renderer.captureFrame();
                check(countViewportDiffs(clearFrame, snowFrame) > 500, "WeatherKind::Snow visibly alters viewport pixels");
                check(countViewportDiffs(rainFrameA, snowFrame) > 500, "WeatherKind::Snow differs from Rain");
                checkViewportPassThrough("opaqueAtWindow stays false over Snow viewport");

                weatherHud.setWeather(ss2::assets::WeatherKind::Fog, 8u);
                weatherHud.draw(renderer);
                const auto fogFrame = renderer.captureFrame();
                check(countViewportDiffs(clearFrame, fogFrame) > 1000, "WeatherKind::Fog mist alters viewport pixels");
                check(countViewportDiffs(snowFrame, fogFrame) > 500, "WeatherKind::Fog differs from Snow");
                checkViewportPassThrough("opaqueAtWindow stays false over Fog viewport");

                weatherHud.setWeather(ss2::assets::WeatherKind::Clear, 0u);
                weatherHud.draw(renderer);
                const auto backToClear = renderer.captureFrame();
                check(backToClear.rgba == clearFrame.rgba, "returning to WeatherKind::Clear restores exact viewport");
                std::cout << "HUD live Weather (Clear/Rain/Snow/Fog) & opaqueAtWindow invariance PASSED\n";

                // Optional retail Gold 2.2 HUD skin + AIRFIRE.ANI/AIRSMOKE.ANI verification when present.
                std::filesystem::path retailDir;
                if (const char* env = std::getenv("SS2_PRIVATE_GAME"); env && *env) {
                    retailDir = env;
                } else if (const char* home = std::getenv("HOME"); home && *home) {
                    const auto candidate = std::filesystem::path(home) / "ss2-private/game/app";
                    if (std::filesystem::exists(candidate / "lng.sue")) retailDir = candidate;
                }
                if (!retailDir.empty() && std::filesystem::exists(retailDir)) {
                    ss2::Hud retailHud(window.get());
                    retailHud.setMinimap(solid(200, 100, 40, 90, 50));
                    ss2::Hud procHud(window.get());
                    procHud.setMinimap(solid(200, 100, 40, 90, 50));

                    ss2::HudState rs;
                    rs.hasSelection = true;
                    rs.hasInfantry = true;
                    rs.hasBombard = true;
                    rs.selection = {{"rifle", 8, 0.9f, 0.8f}, {"officer", 1, 1.0f, 1.0f}};
                    rs.airReady = {1, 1, 2, 1, 1};
                    rs.armedAir = 2;
                    procHud.update(rs);

                    check(retailHud.loadRetailAssets(retailDir), "loadRetailAssets succeeds on Gold 2.2 gameDir");
                    check(retailHud.hasRetailAssets(), "hasRetailAssets true after loading lng.sue");
                    check(retailHud.panelRecordCount() == 184u, "panel.dat decodes all 184 records");
                    check(retailHud.pdropRecordCount() == 9u, "pdrop.dat decodes all 9 records");
                    check(retailHud.retailTextCount() >= 60u, "game.lng decodes localized tactical strings");
                    check(retailHud.retailText("ITXT_STOP").find("STOP") != std::string::npos,
                          "game.lng ITXT_STOP decoded");
                    check(retailHud.retailText("ITXT_AMOVE").find("ASSAULT") != std::string::npos,
                          "game.lng ITXT_AMOVE decoded");
                    check(retailHud.retailText("ITXT_PLANESTR1", 2u).find("PARATROOPS") != std::string::npos,
                          "game.lng indexed ITXT_PLANESTR1[2] decoded");
                    check(retailHud.hasAirVfxAtlas() &&
                          retailHud.airFireFrameCount() >= 20u &&
                          retailHud.airSmokeFrameCount() >= 45u,
                          "game_common.sue AIRFIRE.ANI and AIRSMOKE.ANI decoded into sprite sheet");

                    // Blank unit-order slot must stay clean recessed dark steel (never stamped with checkmark recs[35]).
                    ss2::HudState emptyRs;
                    retailHud.update(emptyRs);
                    const auto emptyStop = retailHud.buttonRect(HudAction::stop);
                    std::uint8_t emptyMaxLuma = 0u;
                    for (int y = emptyStop.y + 3; y < emptyStop.y + emptyStop.height - 3; ++y) {
                        for (int x = emptyStop.x + 3; x < emptyStop.x + emptyStop.width - 3; ++x) {
                            const auto* p = &retailHud.rgba()[(std::size_t(y) * retailHud.width() + std::size_t(x)) * 4u];
                            emptyMaxLuma = std::max({emptyMaxLuma, p[0], p[1], p[2]});
                        }
                    }
                    check(emptyMaxLuma < 110u, "empty unit-order slot has clean recessed backing (no checkmark bleed)");

                    // Retail Medic/Special icons in row 3 visibly differ from procedural fallback.
                    ss2::HudState specRs;
                    specRs.hasSelection = true;
                    specRs.hasInfantry = true;
                    specRs.selection = {{"medic", 1, 1.f, 1.f}, {"flamer", 1, 1.f, 1.f}};
                    procHud.update(specRs);
                    retailHud.update(specRs);
                    const auto atkR = retailHud.buttonRect(HudAction::attackMove);
                    const int slotY = atkR.y;
                    const int col1X = atkR.x + atkR.width + 2;
                    const int col2X = col1X + atkR.width + 2;
                    auto sliceDiffers = [&](int rx) {
                        for (int y = slotY + 2; y < slotY + atkR.height - 2; ++y) {
                            for (int x = rx + 2; x < rx + atkR.width - 2; ++x) {
                                const std::size_t idx = (std::size_t(y) * retailHud.width() + std::size_t(x)) * 4u;
                                if (retailHud.rgba()[idx] != procHud.rgba()[idx] ||
                                    retailHud.rgba()[idx + 1] != procHud.rgba()[idx + 1] ||
                                    retailHud.rgba()[idx + 2] != procHud.rgba()[idx + 2]) {
                                    return true;
                                }
                            }
                        }
                        return false;
                    };
                    check(sliceDiffers(col1X) && sliceDiffers(col2X),
                          "retail Medic (recs[111]) and Special (recs[67]) icons render in row 3");

                    procHud.update(rs);
                    retailHud.update(rs);
                    check(retailHud.rgba() != procHud.rgba(),
                          "retail panel.dat + pdrop.dat skin visibly skins the HUD vs procedural fallback");
                    check(retailHud.rgba()[(std::size_t(3) * retailHud.width() + 3) * 4 + 3] == 255,
                          "retail left console stays opaque");
                    check(retailHud.rgba()[(std::size_t(retailHud.height() - 3) * retailHud.width() + retailHud.width() - 4) * 4 + 3] == 255,
                          "retail bottom tray stays opaque");
                    check(retailHud.rgba()[(std::size_t(3) * retailHud.width() + retailHud.width() - 4) * 4 + 3] == 0,
                          "retail upper-right L-cutout stays transparent");

                    const Mouse rMouse{window.get(), retailHud};
                    const auto stopR = retailHud.buttonRect(HudAction::stop);
                    retailHud.handle(rMouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(stopR), centreY(stopR)));
                    check(retailHud.handle(rMouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(stopR), centreY(stopR))).action == HudAction::stop,
                          "retail HUD preserves Stop button hitbox");
                    const auto paraR = retailHud.airButtonRect(2);
                    check(paraR.width > 0 && paraR.height > 0, "retail paratrooper air pill has valid rect");
                    retailHud.handle(rMouse.button(SDL_EVENT_MOUSE_BUTTON_DOWN, centreX(paraR), centreY(paraR)));
                    const auto paraUp = retailHud.handle(rMouse.button(SDL_EVENT_MOUSE_BUTTON_UP, centreX(paraR), centreY(paraR)));
                    check(paraUp.action == HudAction::airSupport && paraUp.airSlot == 2,
                          "retail paratrooper air pill dispatches slot 2");
                    const auto stopPt = rMouse.at(centreX(stopR), centreY(stopR));
                    const auto rStopTip = retailHud.tooltipAtWindow(stopPt.x, stopPt.y);
                    check(rStopTip && rStopTip->lines.size() >= 2u,
                          "retail button tooltip includes game.lng ITXT subtitle");

                    // Validate Vulkan sprite rendering of decoded AIRFIRE.ANI / AIRSMOKE.ANI trail sprites.
                    renderer.setSpriteAtlas(5u, retailHud.airVfxAtlas());
                    retailHud.bindAirVfxAtlasSlot(5u);
                    retailHud.recordAirSortieTrail(77u, 320, 220, 4u, 30, 100, true, true, true);
                    retailHud.recordAirCrashTrail(340, 230);
                    std::vector<ss2::Sprite> trailSprites;
                    retailHud.appendAirTrailSprites(1, trailSprites);
                    check(!trailSprites.empty(), "retail AIRFIRE/AIRSMOKE trail emits GPU sprites");
                    renderer.setSprites(trailSprites);
                    retailHud.draw(renderer);
                    const auto vfxFrame = renderer.captureFrame();
                    check(vfxFrame.width == unsigned(pw) && vfxFrame.height == unsigned(ph),
                          "Vulkan frame captured with retail HUD skin and AIRFIRE/AIRSMOKE sprites");
                    renderer.setSprites({});
                    std::cout << "Retail Gold 2.2 panel.dat + pdrop.dat + game.lng + AIRFIRE/AIRSMOKE PASSED\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    if (report.errors.load()) return 1;
    return status;
}
