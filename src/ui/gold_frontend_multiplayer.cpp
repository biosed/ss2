#include "ui/gold_frontend_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ss2 {
namespace {

using namespace gold_ui;

constexpr std::array<ButtonSpec, 2> kReplayButtons{{
    {"replay_start", "Replay", 'R', {118, 512, 140, 35}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr std::array<ButtonSpec, 3> kLoadGameButtons{{
    {"load_start", "Load", 'L', {118, 512, 120, 35}, true},
    {"load_delete", "Delete", 'D', {324, 512, 130, 35}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr std::array<ButtonSpec, 2> kMultiplayerButtons{{
    {"mp_start", "Start", 'S', {135, 512, 106, 35}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr RectD kMpModeRect{438, 136, 330, 34};
constexpr RectD kMpLanPillRect{556, 178, 212, 22};
constexpr RectD kMpHostFieldRect{438, 204, 330, 32};
constexpr RectD kMpPortFieldRect{438, 268, 160, 32};
constexpr RectD kMpNationRect{608, 268, 160, 32};
constexpr RectD kMpPlayersRect{438, 308, 160, 28};
constexpr RectD kMpTeamsRect{608, 308, 160, 28};

const char* mpNationLabel(std::uint8_t nation) {
    switch (nation) {
        case 0: return "Russian";
        case 1: return "German";
        case 2: return "British";
        case 3: return "US";
        case 4: return "Japanese";
        default: return "German";
    }
}

std::optional<std::uint16_t> parsePortText(std::string_view s) {
    if (s.empty()) return std::nullopt;
    unsigned v = 0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size() || v == 0 || v > 65535u) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(v);
}

}  // namespace

void GoldFrontEnd::setReplays(std::vector<FrontEnd::ReplayEntry> replays) {
    replays_ = replays;
    fe_.setReplays(std::move(replays));
    if (fe_.screen() == FrontEnd::Screen::Replays) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
        ensureFocusedValid();
    }
    dirty_ = true;
}

void GoldFrontEnd::setSaves(std::vector<FrontEnd::SaveEntry> saves) {
    saves_ = saves;
    fe_.setSaves(std::move(saves));
    if (fe_.screen() == FrontEnd::Screen::LoadGame) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
        ensureFocusedValid();
    }
    dirty_ = true;
}

void GoldFrontEnd::setMultiplayerMissions(std::vector<GoldMissionEntry> missions) {
    mpMissions_ = std::move(missions);
    std::vector<FrontEnd::Entry> base;
    base.reserve(mpMissions_.size());
    for (const auto& m : mpMissions_) {
        base.push_back(FrontEnd::Entry{m.name, m.path, m.description, m.tag});
    }
    fe_.setMultiplayerMissions(std::move(base));
    assets_->overviews.clear();
    if (fe_.screen() == FrontEnd::Screen::Multiplayer) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
        ensureFocusedValid();
    }
    dirty_ = true;
}

std::optional<std::string> GoldFrontEnd::takeDeleteSaveRequest() {
    auto req = std::move(pendingDeleteSave_);
    pendingDeleteSave_.reset();
    return req;
}

std::string GoldFrontEnd::hitTestMultiplayerOrBrowser(double sx, double sy) const {
    if (fe_.screen() == FrontEnd::Screen::Replays) {
        if (const auto id = hitTestButtons(kReplayButtons, sx, sy); !id.empty()) return id;
    } else if (fe_.screen() == FrontEnd::Screen::LoadGame) {
        if (const auto id = hitTestButtons(kLoadGameButtons, sx, sy); !id.empty()) return id;
    } else if (fe_.screen() == FrontEnd::Screen::Multiplayer) {
        if (kMpModeRect.contains(sx, sy)) return "mp_mode";
        if (assets_ && !fe_.mpIsHost() && !assets_->lanScanner.games().empty() &&
            kMpLanPillRect.contains(sx, sy)) {
            return "mp_lan_pick";
        }
        if (kMpHostFieldRect.contains(sx, sy)) return "mp_host_field";
        if (kMpPortFieldRect.contains(sx, sy)) return "mp_port_field";
        if (kMpNationRect.contains(sx, sy)) return "mp_nation";
        if (fe_.mpIsHost() && kMpPlayersRect.contains(sx, sy)) return "mp_players";
        if (fe_.mpIsHost() && fe_.mpPlayerCount() == 4 && kMpTeamsRect.contains(sx, sy)) return "mp_teams";
        if (const auto id = hitTestButtons(kMultiplayerButtons, sx, sy); !id.empty()) return id;
    }
    return hitTestActiveListAndScrollbar(sx, sy);
}

void GoldFrontEnd::finishMultiplayerEdit() {
    if (const auto p = parsePortText(mpPortDraft_)) fe_.setMpPort(*p);
    mpPortDraft_ = std::to_string(fe_.mpPort());
    if (mpEditField_ != MpEditField::None) {
        mpEditField_ = MpEditField::None;
        if (window_) SDL_StopTextInput(window_);
    }
}

bool GoldFrontEnd::activateMultiplayer(const std::string& id) {
    if (const auto p = parsePortText(mpPortDraft_)) fe_.setMpPort(*p);
    mpPortDraft_ = std::to_string(fe_.mpPort());
    if (id == "mp_lan_pick") {
        if (assets_ && !assets_->lanScanner.games().empty()) {
            const auto& g = assets_->lanScanner.games().front();
            fe_.setMpHostAddress(g.hostAddress);
            fe_.setMpPort(g.tcpPort);
            mpPortDraft_ = std::to_string(g.tcpPort);
            if (!g.missionSpec.empty()) {
                for (std::size_t mi = 0; mi < mpMissions_.size(); ++mi) {
                    const std::string stem = std::filesystem::path(mpMissions_[mi].path).stem().string();
                    if (assets::equalsIgnoreCaseAscii(stem, g.missionSpec)) {
                        fe_.selectMultiplayerMission(mi);
                        ensureOverviewLoaded(mpMissions_[mi].path);
                        break;
                    }
                }
            }
            if (mpEditField_ != MpEditField::None) {
                mpEditField_ = MpEditField::None;
                if (window_) SDL_StopTextInput(window_);
            }
            showNotice(trf("Joined LAN lobby: {0}:{1}", {g.hostAddress, std::to_string(g.tcpPort)}));
            dirty_ = true;
        }
        return true;
    }
    if (id == "mp_host_field") {
        mpEditField_ = MpEditField::HostAddress;
        focusedId_ = "mp_host_field";
        if (window_) SDL_StartTextInput(window_);
        dirty_ = true;
        return true;
    }
    if (id == "mp_port_field") {
        mpEditField_ = MpEditField::Port;
        focusedId_ = "mp_port_field";
        if (window_) SDL_StartTextInput(window_);
        dirty_ = true;
        return true;
    }
    if (mpEditField_ != MpEditField::None) {
        mpEditField_ = MpEditField::None;
        if (window_) SDL_StopTextInput(window_);
        dirty_ = true;
    }
    return false;
}

bool GoldFrontEnd::handleMultiplayerEditKey(SDL_Keycode key) {
    if (key == SDLK_BACKSPACE) {
        if (mpEditField_ == MpEditField::HostAddress) {
            std::string addr = fe_.mpHostAddress();
            if (!addr.empty()) addr.pop_back();
            fe_.setMpHostAddress(std::move(addr));
        } else if (mpEditField_ == MpEditField::Port) {
            if (!mpPortDraft_.empty()) mpPortDraft_.pop_back();
            if (const auto p = parsePortText(mpPortDraft_)) fe_.setMpPort(*p);
        }
        dirty_ = true;
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (const auto p = parsePortText(mpPortDraft_)) fe_.setMpPort(*p);
        mpPortDraft_ = std::to_string(fe_.mpPort());
        mpEditField_ = MpEditField::None;
        if (window_) SDL_StopTextInput(window_);
        focusedId_ = "mp_start";
        dirty_ = true;
        return true;
    }
    if (key == SDLK_TAB) {
        if (const auto p = parsePortText(mpPortDraft_)) fe_.setMpPort(*p);
        mpPortDraft_ = std::to_string(fe_.mpPort());
        if (mpEditField_ == MpEditField::HostAddress) {
            mpEditField_ = MpEditField::Port;
            focusedId_ = "mp_port_field";
        } else {
            mpEditField_ = MpEditField::None;
            focusedId_ = "mp_start";
            if (window_) SDL_StopTextInput(window_);
        }
        dirty_ = true;
        return true;
    }
    if ((key >= SDLK_A && key <= SDLK_Z) || (key >= SDLK_0 && key <= SDLK_9)) {
        return true;
    }
    return false;
}

void GoldFrontEnd::handleMultiplayerOrBrowserKey(SDL_Keycode key) {
    if (fe_.screen() == FrontEnd::Screen::LoadGame && key == SDLK_DELETE) {
        activateId("load_delete");
        return;
    }
    handleActiveListNavigationKey(key);
    if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
        const int step = key == SDLK_LEFT ? -1 : 1;
        if (fe_.screen() == FrontEnd::Screen::Replays) {
            cycleFocus({"replay_start", "cancel"}, step);
        } else if (fe_.screen() == FrontEnd::Screen::LoadGame) {
            cycleFocus({"load_start", "load_delete", "cancel"}, step);
        } else {
            cycleFocus({"mp_mode", "mp_host_field", "mp_port_field", "mp_nation",
                        "mp_players", "mp_teams", "mp_start", "cancel"}, step);
        }
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (fe_.screen() == FrontEnd::Screen::Replays) {
            activateId(focusedId_ == "cancel" ? "cancel" : "replay_start");
        } else if (fe_.screen() == FrontEnd::Screen::LoadGame) {
            if (focusedId_ == "cancel" || focusedId_ == "load_delete") activateId(focusedId_);
            else activateId("load_start");
        } else {
            if (focusedId_ == "cancel" || focusedId_ == "mp_mode" ||
                focusedId_ == "mp_nation" || focusedId_ == "mp_players" ||
                focusedId_ == "mp_teams" ||
                focusedId_ == "mp_host_field" || focusedId_ == "mp_port_field") {
                activateId(focusedId_);
            } else {
                activateId("mp_start");
            }
        }
    }
}

void GoldFrontEnd::handleMultiplayerTextInput(std::string_view txt) {
    if (mpEditField_ == MpEditField::HostAddress) {
        std::string addr = fe_.mpHostAddress();
        for (unsigned char uc : txt) {
            if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
                (uc >= '0' && uc <= '9') || uc == '.' || uc == '-' || uc == ':' || uc == '_') {
                if (addr.size() < 64) addr.push_back(char(uc));
            }
        }
        fe_.setMpHostAddress(std::move(addr));
        dirty_ = true;
    } else if (mpEditField_ == MpEditField::Port) {
        for (unsigned char uc : txt) {
            if (uc >= '0' && uc <= '9' && mpPortDraft_.size() < 5) {
                mpPortDraft_.push_back(char(uc));
            }
        }
        if (const auto p = parsePortText(mpPortDraft_)) fe_.setMpPort(*p);
        dirty_ = true;
    }
}

void GoldFrontEnd::renderReplaysOrLoadGame(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    struct DetailRow {
        std::string k;
        std::string v;
    };
    auto renderDetailBrowser = [&](const std::string& heading, const std::string& leftHeading,
                                   const char* rowPrefix, std::size_t rowCount, std::size_t selectedRow,
                                   const std::string& emptyMsg, const std::string& emptyHint,
                                   auto rowNameFn, auto detailsFn, auto captionFn,
                                   const auto& buttons, auto buttonEnabledFn) {
        drawHeading(cr, pangoCtx, heading, {200, 38, 400, 28}, 17.0);
        drawHeading(cr, pangoCtx, leftHeading, {130, 110, 150, 20}, 15.0);
        drawHeading(cr, pangoCtx, tr("Details:"), {520, 110, 144, 20}, 15.0);

        drawBronzeBox(cr, kListX, kListY, kListW, kListH, 0.88);
        cairo_save(cr);
        cairo_rectangle(cr, kListX + 4, kListY + 4, kListW - 8, kListH - 8);
        cairo_clip(cr);

        if (rowCount == 0) {
            auto emptyLbl = makeTextLayout(pangoCtx, emptyMsg, 14.0, false);
            cairo_move_to(cr, kListX + 16, kListY + 18);
            cairo_set_source_rgb(cr, 180.0 / 255.0, 180.0 / 255.0, 165.0 / 255.0);
            pango_cairo_show_layout(cr, emptyLbl.get());
        } else {
            const int endIdx = std::min(int(rowCount), scrollOffset_ + kVisibleMissionRows);
            for (int idx = scrollOffset_; idx < endIdx; ++idx) {
                const int row = idx - scrollOffset_;
                const double ry = kListY + 6.0 + row * kRowH;
                const bool isSelected = (std::size_t(idx) == selectedRow);
                const bool isHovered = (hoveredId_ == std::string(rowPrefix) + std::to_string(idx));
                if (isSelected) {
                    cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.38);
                    cairo_rectangle(cr, kListX + 5, ry, kListW - 10, kRowH - 1.0);
                    cairo_fill(cr);
                } else if (isHovered) {
                    cairo_set_source_rgba(cr, 72.0 / 255.0, 96.0 / 255.0, 56.0 / 255.0, 0.22);
                    cairo_rectangle(cr, kListX + 5, ry, kListW - 10, kRowH - 1.0);
                    cairo_fill(cr);
                }

                auto rowLayout = makeTextLayout(pangoCtx, rowNameFn(std::size_t(idx)), 13.0, isSelected);
                pango_layout_set_width(rowLayout.get(), int((kListW - 22.0) * PANGO_SCALE));
                pango_layout_set_ellipsize(rowLayout.get(), PANGO_ELLIPSIZE_END);
                cairo_move_to(cr, kListX + 12, ry + 2.0);
                if (isSelected) cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
                else if (isHovered) cairo_set_source_rgb(cr, 128.0 / 255.0, 224.0 / 255.0, 138.0 / 255.0);
                else cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
                pango_cairo_show_layout(cr, rowLayout.get());
            }
        }
        cairo_restore(cr);

        drawGoldScrollbar(env, kScrollX, kScrollY, kScrollW, kScrollH, kVisibleMissionRows,
                          true, rowCount, scrollOffset_,
                          hoveredId_ == "scroll_up", hoveredId_ == "scroll_down",
                          hoveredId_ == "scroll_thumb" || draggingThumb_);

        drawBronzeBox(cr, 438, 134, 330, 352, 0.86);
        if (selectedRow < rowCount) {
            drawHeading(cr, pangoCtx, rowNameFn(selectedRow), {450, 146, 306, 26}, 15.5);
            cairo_set_source_rgba(cr, 182.0 / 255.0, 174.0 / 255.0, 146.0 / 255.0, 0.55);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 452, 180);
            cairo_line_to(cr, 754, 180);
            cairo_stroke(cr);

            const auto details = detailsFn(selectedRow);
            for (std::size_t i = 0; i < details.size(); ++i) {
                const double dy = 196.0 + double(i) * 38.0;
                auto kl = makeTextLayout(pangoCtx, details[i].k + ":", 13.0, false);
                cairo_move_to(cr, 454, dy);
                cairo_set_source_rgb(cr, 200.0 / 255.0, 194.0 / 255.0, 166.0 / 255.0);
                pango_cairo_show_layout(cr, kl.get());

                auto vl = makeTextLayout(pangoCtx, details[i].v, 13.0, true, 184.0, PANGO_ALIGN_RIGHT);
                pango_layout_set_ellipsize(vl.get(), PANGO_ELLIPSIZE_END);
                cairo_move_to(cr, 570, dy);
                cairo_set_source_rgb(cr, 245.0 / 255.0, 224.0 / 255.0, 145.0 / 255.0);
                pango_cairo_show_layout(cr, vl.get());
            }
            const std::string caption = captionFn(selectedRow);
            if (!caption.empty()) {
                auto pathLbl = makeTextLayout(pangoCtx, caption, 11.0, false, 302.0);
                pango_layout_set_ellipsize(pathLbl.get(), PANGO_ELLIPSIZE_START);
                cairo_move_to(cr, 452, 446.0);
                cairo_set_source_rgba(cr, 188.0 / 255.0, 192.0 / 255.0, 174.0 / 255.0, 0.85);
                pango_cairo_show_layout(cr, pathLbl.get());
            }
        } else {
            drawHeading(cr, pangoCtx, emptyMsg, {452, 240, 302, 32}, 14.5);
            auto hint = makeTextLayout(pangoCtx, emptyHint, 12.5, false, 302.0, PANGO_ALIGN_CENTER);
            cairo_move_to(cr, 452, 286);
            cairo_set_source_rgb(cr, 188.0 / 255.0, 192.0 / 255.0, 174.0 / 255.0);
            pango_cairo_show_layout(cr, hint.get());
        }

        for (const auto& b : buttons) {
            const bool enabled = buttonEnabledFn(std::string_view(b.id), b.enabled);
            drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                  hoveredId_ == b.id, focusedId_ == b.id, enabled, 24.0);
        }
    };

    auto formatTickTime = [](std::uint32_t ticks) {
        const unsigned secs = ticks / 25u;
        const unsigned mins = secs / 60u;
        const unsigned remS = secs % 60u;
        return (mins < 10u ? "0" : "") + std::to_string(mins) + ":" +
               (remS < 10u ? "0" : "") + std::to_string(remS) +
               " (" + std::to_string(ticks) + "t)";
    };

    if (fe_.screen() == FrontEnd::Screen::Replays) {
        const std::size_t rowCount = replays_.size();
        renderDetailBrowser(
            tr("Replay Game"), tr("Replays:"), "replay:", rowCount, fe_.selectedReplay(),
            tr("No replays recorded yet."), "--record FILE\n~/.local/share/ss2/replays/",
            [&](std::size_t idx) { return replays_[idx].name; },
            [&](std::size_t idx) -> std::array<DetailRow, 6> {
                const auto& r = replays_[idx];
                const std::string missionStem = r.mission.empty()
                    ? std::string("-")
                    : std::filesystem::path(r.mission).stem().string();
                return {{
                    {tr("Mission"), missionStem},
                    {tr("Duration"), formatTickTime(r.endTick)},
                    {tr("Commands"), std::to_string(r.commandCount)},
                    {tr("Difficulty"), tr(difficultyKey(r.difficulty))},
                    {tr("Auto-engage"), tr(r.autoEngage ? "On" : "Off")},
                    {tr("State hash"), r.hashHex.empty() ? std::string("-") : r.hashHex},
                }};
            },
            [&](std::size_t idx) { return replays_[idx].mission; },
            kReplayButtons,
            [rowCount](std::string_view id, bool def) {
                return id == "replay_start" ? (rowCount > 0) : def;
            });
    } else {
        const std::size_t rowCount = saves_.size();
        renderDetailBrowser(
            tr("Load Game"), tr("Saved Games:"), "save:", rowCount, fe_.selectedSave(),
            tr("No saved games yet."), "F9 / Save / Load\n~/.local/share/ss2/saves/",
            [&](std::size_t idx) { return saves_[idx].name; },
            [&](std::size_t idx) -> std::array<DetailRow, 6> {
                const auto& s = saves_[idx];
                const std::string missionName = !s.missionTitle.empty()
                    ? s.missionTitle
                    : (s.mission.empty() ? std::string("-")
                                         : std::filesystem::path(s.mission).stem().string());
                const std::string slotDisplay = s.slotLabel.empty()
                    ? std::string("-")
                    : ((s.slotLabel == "Auto-Save" || s.slotLabel == "Quick Save")
                           ? tr(s.slotLabel)
                           : s.slotLabel);
                return {{
                    {tr("Mission"), missionName},
                    {tr("Slot"), slotDisplay},
                    {tr("Battle time"), formatTickTime(s.tick)},
                    {tr("Forces"), std::to_string(s.playerSoldiers) + " / " + std::to_string(s.playerVehicles)},
                    {tr("Difficulty"), tr(difficultyKey(s.difficulty))},
                    {tr("Saved"), s.timestamp.empty() ? std::string("-") : s.timestamp},
                }};
            },
            [&](std::size_t idx) {
                const auto& s = saves_[idx];
                std::string caption = s.mission;
                if (s.fileBytes > 0) {
                    const auto kb = std::max<std::uint64_t>(1u, s.fileBytes / 1024u);
                    if (!caption.empty()) caption += "  |  ";
                    caption += std::to_string(kb) + " KB";
                }
                return caption;
            },
            kLoadGameButtons,
            [rowCount](std::string_view id, bool def) {
                return (id == "load_start" || id == "load_delete") ? (rowCount > 0) : def;
            });
    }
}

void GoldFrontEnd::renderMultiplayer(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    drawHeading(cr, pangoCtx, tr("Multiplayer"), {200, 38, 400, 28}, 17.0);
    drawHeading(cr, pangoCtx, tr("Missions:"), {130, 110, 150, 20}, 15.0);
    drawHeading(cr, pangoCtx, tr(fe_.mpIsHost() ? "Host Match" : "Join Match"), {492, 110, 220, 20}, 15.0);

    drawBronzeBox(cr, kListX, kListY, kListW, kListH, 0.88);
    cairo_save(cr);
    cairo_rectangle(cr, kListX + 4, kListY + 4, kListW - 8, kListH - 8);
    cairo_clip(cr);

    const std::size_t rowCount = mpMissions_.size();
    const std::size_t selectedRow = fe_.selectedMultiplayerMission();
    if (rowCount == 0) {
        auto emptyLbl = makeTextLayout(pangoCtx, tr("No multiplayer maps installed."), 14.0, false);
        cairo_move_to(cr, kListX + 16, kListY + 18);
        cairo_set_source_rgb(cr, 180.0 / 255.0, 180.0 / 255.0, 165.0 / 255.0);
        pango_cairo_show_layout(cr, emptyLbl.get());
    } else {
        const int endIdx = std::min(int(rowCount), scrollOffset_ + kVisibleMissionRows);
        for (int idx = scrollOffset_; idx < endIdx; ++idx) {
            const int row = idx - scrollOffset_;
            const double ry = kListY + 6.0 + row * kRowH;
            const bool isSelected = (std::size_t(idx) == selectedRow);
            const bool isHovered = (hoveredId_ == "mpmap:" + std::to_string(idx));
            if (isSelected) {
                cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.38);
                cairo_rectangle(cr, kListX + 5, ry, kListW - 10, kRowH - 1.0);
                cairo_fill(cr);
            } else if (isHovered) {
                cairo_set_source_rgba(cr, 72.0 / 255.0, 96.0 / 255.0, 56.0 / 255.0, 0.22);
                cairo_rectangle(cr, kListX + 5, ry, kListW - 10, kRowH - 1.0);
                cairo_fill(cr);
            }
            auto rowLayout = makeTextLayout(pangoCtx, mpMissions_[std::size_t(idx)].name, 13.0, isSelected);
            pango_layout_set_width(rowLayout.get(), int((kListW - 22.0) * PANGO_SCALE));
            pango_layout_set_ellipsize(rowLayout.get(), PANGO_ELLIPSIZE_END);
            cairo_move_to(cr, kListX + 12, ry + 2.0);
            if (isSelected) cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
            else if (isHovered) cairo_set_source_rgb(cr, 128.0 / 255.0, 224.0 / 255.0, 138.0 / 255.0);
            else cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
            pango_cairo_show_layout(cr, rowLayout.get());
        }
    }
    cairo_restore(cr);

    drawGoldScrollbar(env, kScrollX, kScrollY, kScrollW, kScrollH, kVisibleMissionRows,
                      true, rowCount, scrollOffset_,
                      hoveredId_ == "scroll_up", hoveredId_ == "scroll_down",
                      hoveredId_ == "scroll_thumb" || draggingThumb_);

    const bool modeHov = (hoveredId_ == "mp_mode");
    const bool modeFoc = (focusedId_ == "mp_mode");
    drawBronzeBox(cr, kMpModeRect.x, kMpModeRect.y, kMpModeRect.w, kMpModeRect.h, (modeHov || modeFoc) ? 0.94 : 0.85);
    drawGoldStencilButton(cr, pangoCtx, kMpModeRect,
                          tr(fe_.mpIsHost() ? "Host Match" : "Join Match"), 'M',
                          modeHov, modeFoc, true, 16.5, true);

    auto drawFieldLabel = [&](const std::string& label, double y) {
        auto lbl = makeTextLayout(pangoCtx, label, 13.5, true);
        cairo_move_to(cr, 438.0, y);
        cairo_set_source_rgb(cr, 232.0 / 255.0, 224.0 / 255.0, 188.0 / 255.0);
        pango_cairo_show_layout(cr, lbl.get());
    };
    auto drawInputField = [&](const RectD& r, const std::string& val, const std::string& placeholder,
                              bool editing, bool hovered, bool focused) {
        drawBronzeBox(cr, r.x, r.y, r.w, r.h, editing ? 0.96 : (hovered || focused) ? 0.90 : 0.82);
        if (editing) {
            cairo_set_source_rgba(cr, 212.0 / 255.0, 198.0 / 255.0, 118.0 / 255.0, 0.90);
            cairo_set_line_width(cr, 1.5);
            cairo_rectangle(cr, r.x + 1.0, r.y + 1.0, r.w - 2.0, r.h - 2.0);
            cairo_stroke(cr);
        }
        const bool empty = val.empty();
        std::string shown = empty ? (editing ? "_" : placeholder) : (editing ? (val + "_") : val);
        auto tl = makeTextLayout(pangoCtx, shown, 14.0, !empty);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(tl.get(), &tw, &th);
        pango_layout_set_width(tl.get(), int((r.w - 16.0) * PANGO_SCALE));
        pango_layout_set_ellipsize(tl.get(), PANGO_ELLIPSIZE_START);
        cairo_move_to(cr, r.x + 8.0, r.y + (r.h - th) * 0.5);
        if (empty && !editing) cairo_set_source_rgb(cr, 165.0 / 255.0, 165.0 / 255.0, 148.0 / 255.0);
        else if (editing) cairo_set_source_rgb(cr, 252.0 / 255.0, 240.0 / 255.0, 182.0 / 255.0);
        else cairo_set_source_rgb(cr, 238.0 / 255.0, 242.0 / 255.0, 224.0 / 255.0);
        pango_cairo_show_layout(cr, tl.get());
    };

    drawFieldLabel(tr("Host Address") + ":", 182.0);
    if (assets_ && !fe_.mpIsHost() && !assets_->lanScanner.games().empty()) {
        const auto& g = assets_->lanScanner.games().front();
        const bool lanHov = (hoveredId_ == "mp_lan_pick");
        drawBronzeBox(cr, kMpLanPillRect.x, kMpLanPillRect.y, kMpLanPillRect.w, kMpLanPillRect.h,
                      lanHov ? 0.95 : 0.85);
        const std::string hostLabel = g.hostName.empty() ? g.hostAddress : g.hostName;
        const std::string pillText = "LAN: " + hostLabel + ":" + std::to_string(g.tcpPort);
        drawGoldStencilButton(cr, pangoCtx, kMpLanPillRect, pillText, '\0', lanHov, false, true, 11.5, true);
    }
    drawInputField(kMpHostFieldRect, fe_.mpHostAddress(), tr("Enter address"),
                   mpEditField_ == MpEditField::HostAddress,
                   hoveredId_ == "mp_host_field", focusedId_ == "mp_host_field");

    drawFieldLabel(tr("Port") + ":", 246.0);
    drawInputField(kMpPortFieldRect, mpPortDraft_, "27015",
                   mpEditField_ == MpEditField::Port,
                   hoveredId_ == "mp_port_field", focusedId_ == "mp_port_field");

    {
        auto nLbl = makeTextLayout(pangoCtx, tr("Nation") + ":", 13.5, true);
        cairo_move_to(cr, 608.0, 246.0);
        cairo_set_source_rgb(cr, 232.0 / 255.0, 224.0 / 255.0, 188.0 / 255.0);
        pango_cairo_show_layout(cr, nLbl.get());
        const bool natHov = (hoveredId_ == "mp_nation");
        const bool natFoc = (focusedId_ == "mp_nation");
        drawBronzeBox(cr, kMpNationRect.x, kMpNationRect.y, kMpNationRect.w, kMpNationRect.h,
                      (natHov || natFoc) ? 0.94 : 0.85);
        // colors_bt.pck: 12 colours x [normal, lit, plate]; nation -> colour base frame.
        static constexpr std::array<std::size_t, 5> kNationSwatchBase{0, 21, 9, 3, 6};
        const int nat = fe_.mpNation();
        const std::size_t swIdx = (nat >= 0 && nat < int(kNationSwatchBase.size()))
            ? kNationSwatchBase[std::size_t(nat)] + ((natHov || natFoc) ? 1u : 0u) : 0u;
        const auto& sw = assets_->colorSwatch[swIdx];
        if (sw.surface) {
            drawPckAt(cr, sw, kMpNationRect.x + 10.0, kMpNationRect.y + std::floor((kMpNationRect.h - sw.h) * 0.5));
            const RectD textR{kMpNationRect.x + 14.0 + sw.w, kMpNationRect.y,
                              kMpNationRect.w - 18.0 - sw.w, kMpNationRect.h};
            drawGoldStencilButton(cr, pangoCtx, textR, tr(mpNationLabel(fe_.mpNation())), 'N',
                                  natHov, natFoc, true, 15.0, true);
        } else {
            drawGoldStencilButton(cr, pangoCtx, kMpNationRect,
                                  tr(mpNationLabel(fe_.mpNation())), 'N',
                                  natHov, natFoc, true, 15.0, true);
        }
    }

    {
        const bool plEnabled = fe_.mpIsHost();
        const bool plHov = plEnabled && (hoveredId_ == "mp_players");
        const bool plFoc = plEnabled && (focusedId_ == "mp_players");
        drawBronzeBox(cr, kMpPlayersRect.x, kMpPlayersRect.y, kMpPlayersRect.w, kMpPlayersRect.h,
                      (plHov || plFoc) ? 0.94 : 0.85);
        drawGoldStencilButton(cr, pangoCtx, kMpPlayersRect,
                              tr("Players") + ": " + std::to_string(fe_.mpPlayerCount()), 'P',
                              plHov, plFoc, plEnabled, 13.5, true);

        const bool tmEnabled = fe_.mpIsHost() && fe_.mpPlayerCount() == 4;
        const bool tmHov = tmEnabled && (hoveredId_ == "mp_teams");
        const bool tmFoc = tmEnabled && (focusedId_ == "mp_teams");
        drawBronzeBox(cr, kMpTeamsRect.x, kMpTeamsRect.y, kMpTeamsRect.w, kMpTeamsRect.h,
                      (tmHov || tmFoc) ? 0.94 : 0.85);
        const std::string tmVal = (fe_.mpPlayerCount() == 4 && fe_.mpTeam2v2()) ? tr("2v2") : tr("FFA");
        drawGoldStencilButton(cr, pangoCtx, kMpTeamsRect,
                              tr("Teams") + ": " + tmVal, 'T',
                              tmHov, tmFoc, tmEnabled, 13.5, true);
    }

    if (selectedRow < rowCount) {
        const auto& m = mpMissions_[selectedRow];
        ensureOverviewLoaded(m.path);
        cairo_surface_t* overview = nullptr;
        if (const auto it = assets_->overviews.find(m.path); it != assets_->overviews.end()) {
            overview = it->second.get();
        }
        drawDiamondOverview(cr, assets_->overviewOrPlate(overview), 504.0, 344.0, 200.0, 104.0, true);
        std::string sizeStr = tr("Size:") + "  " + std::to_string(m.width) + "x" + std::to_string(m.height);
        if (m.scheme == 2) sizeStr += "  (Beach)";
        else if (m.width > 0) sizeStr += "  (Summer)";
        drawHeading(cr, pangoCtx, sizeStr, {464, 452, 280, 18}, 13.5);
        if (!m.description.empty()) {
            auto desc = makeTextLayout(pangoCtx, m.description, 12.0, false, 324.0, PANGO_ALIGN_CENTER);
            pango_layout_set_height(desc.get(), int(22.0 * PANGO_SCALE));
            pango_layout_set_ellipsize(desc.get(), PANGO_ELLIPSIZE_END);
            cairo_move_to(cr, 442.0, 472.0);
            cairo_set_source_rgb(cr, 220.0 / 255.0, 224.0 / 255.0, 206.0 / 255.0);
            pango_cairo_show_layout(cr, desc.get());
        }
    }

    for (const auto& b : kMultiplayerButtons) {
        const bool enabled = std::string_view(b.id) == "mp_start"
            ? (!fe_.mpIsHost() || rowCount > 0) : b.enabled;
        drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                              hoveredId_ == b.id, focusedId_ == b.id, enabled, 24.0);
    }
}

}  // namespace ss2
