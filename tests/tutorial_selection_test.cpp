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

        // Phase 4J: recallControlGroup, setSelection, selectionCentroid & abandoned owner!=0 auto-deselect.
        {
            ss2::TutorialSelection sel4j(
                window.get(), renderer,
                std::vector<ss2::assets::Infantry>{
                    {"rifle", 120, 100, 100, 80, 1, 0, 115, 90, 10, 20},
                    {"dofficer", 160, 100, 100, 80, 1, 0, 155, 90, 10, 20},
                },
                std::vector<ss2::assets::VehicleUnit>{
                    {"tiger", 220, 120, 100, 100, 2, 0, 0, 0, 196, 96, 48, 48, true, true, true},
                    {"kubel", 280, 120, 100, 100, 2, 0, 0, 0, 256, 96, 48, 48, true, true, true},
                });
            sel4j.setSelection({0}, {0});
            if (sel4j.selected() != std::set<size_t>{0} || sel4j.selectedVehicles() != std::set<size_t>{0}) {
                throw std::runtime_error("Phase 4J setSelection failed");
            }
            SDL_Event saveGrp{};
            saveGrp.type = SDL_EVENT_KEY_DOWN;
            saveGrp.key.key = SDLK_1;
            saveGrp.key.mod = SDL_KMOD_CTRL;
            sel4j.handle(saveGrp);
            sel4j.setSelection({1}, {});
            const auto ctr = sel4j.selectionCentroid();
            if (!ctr || ctr->first != 160 || ctr->second != 100) {
                throw std::runtime_error("Phase 4J selectionCentroid mismatch");
            }
            sel4j.recallControlGroup(1);
            if (sel4j.selected() != std::set<size_t>{0} || sel4j.selectedVehicles() != std::set<size_t>{0}) {
                throw std::runtime_error("Phase 4J recallControlGroup(1) mismatch");
            }
            ss2::assets::VehicleUnit abandoned{"tiger", 220, 120, 100, 100, 2, 3, 0, 0, 196, 96, 48, 48, true, true, false};
            sel4j.updateVehicle(0, abandoned);
            if (sel4j.selectedVehicles().contains(0) || sel4j.playerLosses() != 0) {
                throw std::runtime_error("Phase 4J abandoned vehicle was not deselected cleanly");
            }
            std::cout << "Phase 4J TutorialSelection helpers & abandoned vehicle auto-deselect PASSED\n";
        }

        // Phase 4Q: Double-tap 0..9 group camera jump, player-only V cycling & selectAllPlayerUnits.
        {
            renderer.focusMap(160, 160, true);
            if (!renderer.present({})) throw std::runtime_error("Phase 4Q fixture failed to present");
            ss2::TutorialSelection sel4q(
                window.get(), renderer,
                std::vector<ss2::assets::Infantry>{
                    {"rifle", 120, 120, 100, 80, 1, 0, 115, 110, 10, 20},    // 0: player on-screen
                    {"dofficer", 340, 340, 100, 80, 1, 0, 335, 330, 10, 20}, // 1: player off-screen (unclamped centerable)
                    {"rifle", 130, 120, 100, 80, 2, 1, 125, 110, 10, 20},    // 2: enemy on-screen
                },
                std::vector<ss2::assets::VehicleUnit>{
                    {"tiger", 140, 140, 100, 100, 2, 0, 0, 0, 116, 116, 48, 48, true, true, true}, // 0: player on-screen
                    {"t34", 160, 140, 100, 100, 3, 1, 0, 0, 136, 116, 48, 48, true, true, true},   // 1: enemy vehicle
                    {"kubel", 350, 350, 100, 100, 2, 0, 0, 0, 326, 326, 48, 48, true, true, true}, // 2: player off-screen
                });

            renderer.focusMap(160, 160, true);
            if (!renderer.present({})) throw std::runtime_error("Phase 4Q refocus failed to present");
            if (sel4q.selectAllPlayerUnits(true) != 2u ||
                sel4q.selected() != std::set<size_t>{0} ||
                sel4q.selectedVehicles() != std::set<size_t>{0}) {
                throw std::runtime_error("Phase 4Q selectAllPlayerUnits(true) screen filter mismatch");
            }
            if (sel4q.selectAllPlayerUnits(false) != 4u ||
                sel4q.selected() != (std::set<size_t>{0, 1}) ||
                sel4q.selectedVehicles() != (std::set<size_t>{0, 2})) {
                throw std::runtime_error("Phase 4Q selectAllPlayerUnits(false) all-map mismatch");
            }

            // Double-tap control group 3 jumps camera from (160,160) to group 3 at (340,340).
            sel4q.setSelection({1}, {});
            SDL_Event grp3{};
            grp3.type = SDL_EVENT_KEY_DOWN;
            grp3.key.key = SDLK_3;
            grp3.key.mod = SDL_KMOD_CTRL;
            sel4q.handle(grp3);
            sel4q.setSelection({0}, {});
            renderer.focusMap(160, 160, true);
            if (!renderer.present({})) throw std::runtime_error("Phase 4Q pre-jump present failed");

            grp3.key.mod = SDL_KMOD_NONE;
            sel4q.handle(grp3);
            if (sel4q.selected() != std::set<size_t>{1} || !sel4q.selectedVehicles().empty()) {
                throw std::runtime_error("Phase 4Q first press of group 3 did not recall selection");
            }
            if (!renderer.present({})) throw std::runtime_error("Phase 4Q mid-jump present failed");
            const auto [cx1, cy1] = renderer.mapPosition(w / 2.0f, h / 2.0f);
            if (std::abs(cx1 - 160.0) > 6.0 || std::abs(cy1 - 160.0) > 6.0) {
                throw std::runtime_error("Phase 4Q single group press unexpectedly jumped camera");
            }

            sel4q.handle(grp3);
            if (!renderer.present({})) throw std::runtime_error("Phase 4Q post-jump present failed");
            const auto [cx2, cy2] = renderer.mapPosition(w / 2.0f, h / 2.0f);
            if (std::abs(cx2 - 340.0) > 4.0 || std::abs(cy2 - 340.0) > 4.0) {
                throw std::runtime_error("Phase 4Q double-tap group 3 did not jump camera to centroid");
            }

            // Player-only V cycling skips enemy vehicle 1 and selects player vehicles {0} and {2}.
            SDL_Event vKey{};
            vKey.type = SDL_EVENT_KEY_DOWN;
            vKey.key.key = SDLK_V;
            std::set<size_t> seenVeh;
            for (int step = 0; step < 4; ++step) {
                sel4q.handle(vKey);
                if (!sel4q.selected().empty() || sel4q.selectedVehicles().size() != 1u ||
                    sel4q.selectedVehicles().contains(1u)) {
                    throw std::runtime_error("Phase 4Q SDLK_V selected invalid/enemy vehicle");
                }
                seenVeh.insert(*sel4q.selectedVehicles().begin());
            }
            if (seenVeh != (std::set<size_t>{0, 2})) {
                throw std::runtime_error("Phase 4Q SDLK_V did not cycle all player vehicles");
            }
            std::cout << "Phase 4Q double-tap group jump, player V vehicle cycle & Ctrl+A select-all PASSED\n";
        }

        // Phase 4U: Double-click & triple-click (clicks >= 2) same-type selection excludes hidden units.
        {
            renderer.focusMap(160, 160, true);
            if (!renderer.present({})) throw std::runtime_error("Phase 4U fixture failed to present");
            const auto [ux0, uy0] = renderer.mapPosition(0, 0);
            const auto [ux1, uy1] = renderer.mapPosition(float(w), float(h));
            ss2::TutorialSelection sel4u(
                window.get(), renderer,
                std::vector<ss2::assets::Infantry>{
                    {"drifle", 120, 120, 100, 80, 1, 0, 115, 110, 10, 20, 0u, 0u, false, true},  // 0: visible
                    {"drifle", 145, 120, 100, 80, 1, 0, 140, 110, 10, 20, 0u, 0u, false, true},  // 1: visible
                    {"drifle", 170, 120, 100, 80, 1, 0, 165, 110, 10, 20, 0u, 0u, false, false}, // 2: garrisoned/hidden
                });
            const auto clickAt = [&](int mapX, int mapY, int clicks) {
                SDL_Event e{};
                e.button.button = SDL_BUTTON_LEFT;
                e.button.clicks = static_cast<Uint8>(clicks);
                e.button.x = float((mapX - ux0) * w / (ux1 - ux0));
                e.button.y = float((mapY - uy0) * h / (uy1 - uy0));
                e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                sel4u.handle(e);
                e.type = SDL_EVENT_MOUSE_BUTTON_UP;
                sel4u.handle(e);
            };
            clickAt(120, 120, 2);
            if (sel4u.selected() != (std::set<size_t>{0, 1})) {
                throw std::runtime_error("Phase 4U double-click included hidden unit or missed visible match");
            }
            clickAt(120, 120, 3);
            if (sel4u.selected() != (std::set<size_t>{0, 1})) {
                throw std::runtime_error("Phase 4U clicks >= 2 failed on triple-click");
            }
            std::cout << "Phase 4U double-click hidden-unit exclusion & clicks>=2 selection PASSED\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    return status || report.errors.load() != 0;
}
