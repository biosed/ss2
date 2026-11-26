#include "ui/gold_frontend_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ss2 {
namespace {

using namespace gold_ui;

constexpr RectD kRatingsCampaignsTabRect{43, 94, 168, 28};
constexpr RectD kRatingsMissionsTabRect{220, 94, 210, 28};
constexpr RectD kRatingsCancelRect{542, 512, 118, 35};
constexpr RectD kCreditsFrameRect{96, 62, 608, 434};
constexpr RectD kCreditsCancelRect{542, 512, 118, 35};

constexpr RectD kOptionsPanelRect{86, 82, 628, 434};
constexpr double kOptionsRowY0 = 94.0;
constexpr double kOptionsRowStep = 42.0;
constexpr double kOptionsRowH = 36.0;
constexpr RectD kOptionsOkRect{132, 524, 112, 35};
constexpr RectD kOptionsEnhancedGfxRect{264, 524, 258, 35};
constexpr RectD kOptionsCancelRect{542, 524, 118, 35};
constexpr RectD kOptionsDoctrineRect{526, 26, 188, 30};

RectD optionRowRect(std::size_t idx) {
    return {96.0, kOptionsRowY0 + double(idx) * kOptionsRowStep, 608.0, kOptionsRowH};
}
RectD optionDecRect(std::size_t idx) {
    return {412.0, kOptionsRowY0 + double(idx) * kOptionsRowStep + 4.0, 28.0, 28.0};
}
RectD optionValueRect(std::size_t idx) {
    return {444.0, kOptionsRowY0 + double(idx) * kOptionsRowStep + 4.0, 212.0, 28.0};
}
RectD optionIncRect(std::size_t idx) {
    return {660.0, kOptionsRowY0 + double(idx) * kOptionsRowStep + 4.0, 28.0, 28.0};
}

bool isCreditsRoleHeading(std::string_view line) {
    bool hasAlpha = false;
    for (unsigned char c : line) {
        if (c >= 'a' && c <= 'z') return false;
        if (c >= 'A' && c <= 'Z') hasAlpha = true;
    }
    return hasAlpha && line.size() >= 3;
}

struct RatingsRow {
    bool isHeader{false};
    std::string name;
    bool completed{false};
    int score{0};
    std::string status;
};

std::vector<RatingsRow> buildRatingsRows(FrontEnd::RatingsMode mode,
                                         const std::vector<GoldCampaignEntry>& campaigns,
                                         const std::vector<GoldMissionEntry>& missions) {
    std::vector<RatingsRow> rows;
    if (mode == FrontEnd::RatingsMode::Campaigns) {
        for (const auto& c : campaigns) {
            int done = 0;
            int campScore = 0;
            for (const auto& m : c.missions) {
                if (m.completed) ++done;
                campScore += std::max(0, m.highScore);
            }
            rows.push_back(RatingsRow{
                true,
                c.name,
                !c.missions.empty() && done == int(c.missions.size()),
                campScore,
                std::to_string(done) + "/" + std::to_string(c.missions.size()) + " " + tr("Completed"),
            });
            for (const auto& m : c.missions) {
                rows.push_back(RatingsRow{
                    false,
                    "  " + m.name,
                    m.completed,
                    std::max(0, m.highScore),
                    m.completed ? tr("Completed") : std::string{},
                });
            }
        }
        return rows;
    }

    std::vector<std::size_t> order(missions.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (missions[a].highScore != missions[b].highScore) {
            return missions[a].highScore > missions[b].highScore;
        }
        if (missions[a].completed != missions[b].completed) {
            return missions[a].completed && !missions[b].completed;
        }
        return a < b;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const auto& m = missions[order[rank]];
        rows.push_back(RatingsRow{
            false,
            std::to_string(rank + 1) + ". " + m.name,
            m.completed,
            std::max(0, m.highScore),
            m.completed ? tr("Completed") : std::string{},
        });
    }
    return rows;
}

}  // namespace

void GoldFrontEnd::setDifficulty(int difficulty) {
    fe_.setDifficulty(difficulty);
    settings_.difficulty = fe_.difficulty();
    optionsDraft_.difficulty = fe_.difficulty();
    dirty_ = true;
}

void GoldFrontEnd::setSettings(const UserSettings& settings) {
    settings_ = settings;
    ensureProfilesInitialized(settings_);
    optionsDraft_ = settings_;
    optionsBackupLanguage_ = ss2::language();
    fe_.setDifficulty(settings_.difficulty);
    profileEditMode_ = ProfileEditMode::None;
    profileNameDraft_ = settings_.activeProfile;
    const std::size_t activeIdx = findProfileIndex(settings_, settings_.activeProfile);
    const int maxOff = std::max(0, int(settings_.profiles.size()) - kVisibleProfileRows);
    profileScroll_ = std::clamp(profileScroll_, 0, maxOff);
    if (activeIdx < settings_.profiles.size()) {
        if (int(activeIdx) < profileScroll_) profileScroll_ = int(activeIdx);
        else if (int(activeIdx) >= profileScroll_ + kVisibleProfileRows) {
            profileScroll_ = std::min(maxOff, int(activeIdx) - kVisibleProfileRows + 1);
        }
    }
    dirty_ = true;
    syncMenuMusic();
}

bool GoldFrontEnd::takeSettingsSaved() {
    return std::exchange(settingsSaved_, false);
}

void GoldFrontEnd::beginOptionsEdit() {
    availableMods_ = gameDir_.empty()
        ? std::vector<assets::ModInfo>{}
        : assets::listInstalledMods(gameDir_, defaultModsDir());
    optionsDraft_ = settings_;
    optionsDraft_.difficulty = fe_.difficulty();
    optionsBackupLanguage_ = ss2::language();
}

void GoldFrontEnd::commitOptionsEdit() {
    const bool modChanged = (settings_.activeMod != optionsDraft_.activeMod);
    settings_ = optionsDraft_;
    fe_.setDifficulty(settings_.difficulty);
    if (modChanged) {
        assets::setActiveMod(settings_.activeMod);
        reloadModAssets();
    }
    settingsSaved_ = true;
    syncMenuMusic();
}

void GoldFrontEnd::cancelOptionsEdit() {
    optionsDraft_ = settings_;
    ss2::setLanguage(optionsBackupLanguage_);
    syncMenuMusic();
}

void GoldFrontEnd::adjustOption(const std::string& id, int delta) {
    const int step = delta >= 0 ? 1 : -1;
    bool openCue = (step >= 0);
    if (id == "opt_difficulty") {
        optionsDraft_.difficulty = 1 + ((optionsDraft_.difficulty - 1 + step + 3) % 3);
    } else if (id == "opt_language") {
        static constexpr std::array<Language, 3> kLangs{{
            Language::english, Language::french, Language::spanish
        }};
        const auto cur = ss2::language();
        std::size_t idx = 0;
        for (std::size_t i = 0; i < kLangs.size(); ++i) {
            if (kLangs[i] == cur) { idx = i; break; }
        }
        idx = (idx + std::size_t(step + 3)) % kLangs.size();
        optionsDraft_.language = kLangs[idx];
        ss2::setLanguage(kLangs[idx]);
    } else if (id == "opt_sfx") {
        static constexpr std::array<float, 5> kSfx{{0.0f, 0.25f, 0.50f, 0.75f, 1.0f}};
        std::size_t best = 0;
        float bestDiff = 10.0f;
        for (std::size_t i = 0; i < kSfx.size(); ++i) {
            if (const float d = std::abs(optionsDraft_.sfxVolume - kSfx[i]); d < bestDiff) {
                bestDiff = d;
                best = i;
            }
        }
        optionsDraft_.sfxVolume = kSfx[(best + std::size_t(step + int(kSfx.size()))) % kSfx.size()];
    } else if (id == "opt_music_vol") {
        static constexpr std::array<float, 4> kMus{{0.09f, 0.18f, 0.28f, 0.40f}};
        std::size_t best = 0;
        float bestDiff = 10.0f;
        for (std::size_t i = 0; i < kMus.size(); ++i) {
            if (const float d = std::abs(optionsDraft_.musicVolume - kMus[i]); d < bestDiff) {
                bestDiff = d;
                best = i;
            }
        }
        optionsDraft_.musicVolume = kMus[(best + std::size_t(step + int(kMus.size()))) % kMus.size()];
    } else if (id == "opt_music_mute") {
        optionsDraft_.musicMuted = !optionsDraft_.musicMuted;
        openCue = !optionsDraft_.musicMuted;
    } else if (id == "opt_speed") {
        static constexpr std::array<int, 4> kSpeeds{{50, 100, 200, 300}};
        std::size_t idx = 1;
        for (std::size_t i = 0; i < kSpeeds.size(); ++i) {
            if (kSpeeds[i] == optionsDraft_.speedPercent) { idx = i; break; }
        }
        optionsDraft_.speedPercent = kSpeeds[(idx + std::size_t(step + int(kSpeeds.size()))) % kSpeeds.size()];
    } else if (id == "opt_smooth") {
        optionsDraft_.smoothTerrain = !optionsDraft_.smoothTerrain;
        openCue = optionsDraft_.smoothTerrain;
    } else if (id == "opt_enhanced_gfx") {
        optionsDraft_.enhancedGraphics = !optionsDraft_.enhancedGraphics;
        openCue = optionsDraft_.enhancedGraphics;
    } else if (id == "opt_doctrine") {
        optionsDraft_.realismDoctrine = !optionsDraft_.realismDoctrine;
        openCue = optionsDraft_.realismDoctrine;
    } else if (id == "opt_effects") {
        optionsDraft_.combatEffects = !optionsDraft_.combatEffects;
        openCue = optionsDraft_.combatEffects;
    } else if (id == "opt_skip_intro") {
        optionsDraft_.skipIntro = !optionsDraft_.skipIntro;
        openCue = optionsDraft_.skipIntro;
    } else if (id == "opt_mod") {
        std::vector<std::string> choices{""};
        choices.reserve(1 + availableMods_.size());
        for (const auto& m : availableMods_) choices.push_back(m.id);
        std::size_t idx = 0;
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (assets::equalsIgnoreCaseAscii(choices[i], optionsDraft_.activeMod)) {
                idx = i;
                break;
            }
        }
        idx = (idx + std::size_t(step + int(choices.size()))) % choices.size();
        optionsDraft_.activeMod = choices[idx];
    }
    dirty_ = true;
    syncMenuMusic();
    playUiSfx(openCue ? "checkbox_open" : "checkbox_close", 0.55f);
}

std::size_t GoldFrontEnd::ratingsRowCount() const {
    if (fe_.ratingsMode() == FrontEnd::RatingsMode::Campaigns) {
        std::size_t total = 0;
        for (const auto& c : campaigns_) {
            total += 1 + c.missions.size();
        }
        return total;
    }
    return missions_.size();
}

void GoldFrontEnd::scrollRatingsBy(int deltaRows) {
    const std::size_t count = ratingsRowCount();
    const int maxOff = std::max(0, int(count) - kVisibleRatingsRows);
    const int next = std::clamp(ratingsScroll_ + deltaRows, 0, maxOff);
    if (next != ratingsScroll_) {
        ratingsScroll_ = next;
        dirty_ = true;
    }
}

std::string GoldFrontEnd::hitTestOptions(double sx, double sy) const {
    if (kOptionsOkRect.contains(sx, sy)) return "ok";
    if (kOptionsEnhancedGfxRect.contains(sx, sy)) return "opt_enhanced_gfx";
    if (kOptionsDoctrineRect.contains(sx, sy)) return "opt_doctrine";
    if (kOptionsCancelRect.contains(sx, sy)) return "cancel";
    for (std::size_t i = 0; i < kOptionRowIds.size(); ++i) {
        if (optionDecRect(i).contains(sx, sy)) return std::string("opt_dec:") + kOptionRowIds[i];
        if (optionIncRect(i).contains(sx, sy) || optionValueRect(i).contains(sx, sy) ||
            optionRowRect(i).contains(sx, sy)) {
            return std::string("opt_inc:") + kOptionRowIds[i];
        }
    }
    return {};
}

std::string GoldFrontEnd::hitTestRatings(double sx, double sy) const {
    if (kRatingsCampaignsTabRect.contains(sx, sy)) return "ratings_campaigns";
    if (kRatingsMissionsTabRect.contains(sx, sy)) return "ratings_missions";
    if (kRatingsCancelRect.contains(sx, sy)) return "cancel";
    const std::size_t count = ratingsRowCount();
    if (RectD{kRatingsScrollX, kRatingsScrollY, kRatingsScrollW, kScrollArrowH}.contains(sx, sy)) {
        return "ratings_scroll_up";
    }
    if (RectD{kRatingsScrollX, kRatingsScrollY + kRatingsScrollH - kScrollArrowH, kRatingsScrollW, kScrollArrowH}.contains(sx, sy)) {
        return "ratings_scroll_down";
    }
    if (RectD{kRatingsScrollX, kRatingsTrackY, kRatingsScrollW, kRatingsTrackH}.contains(sx, sy) &&
        int(count) > kVisibleRatingsRows) {
        const double thumbY = thumbTopFor(ratingsScroll_, count, kVisibleRatingsRows, kRatingsTrackY, kRatingsTrackH);
        if (sy >= thumbY && sy < thumbY + kThumbH) return "ratings_scroll_thumb";
        return sy < thumbY ? "ratings_scroll_page_up" : "ratings_scroll_page_down";
    }
    return {};
}

std::string GoldFrontEnd::hitTestCredits(double sx, double sy) const {
    if (kCreditsCancelRect.contains(sx, sy)) return "cancel";
    if (kCreditsFrameRect.contains(sx, sy)) return "credits_toggle_pause";
    return {};
}

void GoldFrontEnd::handleOptionsKey(SDL_Keycode key) {
    static const std::vector<std::string> kOrder{
        "opt_difficulty", "opt_language", "opt_sfx", "opt_music_vol",
        "opt_music_mute", "opt_speed", "opt_smooth", "opt_effects",
        "opt_skip_intro", "opt_mod", "opt_doctrine", "opt_enhanced_gfx", "ok", "cancel"
    };
    if (key == SDLK_UP) cycleFocus(kOrder, -1);
    else if (key == SDLK_DOWN || key == SDLK_TAB) cycleFocus(kOrder, 1);
    else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
        const int step = (key == SDLK_LEFT) ? -1 : 1;
        if (focusedId_.rfind("opt_", 0) == 0) adjustOption(focusedId_, step);
        else { focusedId_ = (focusedId_ == "ok") ? "cancel" : "ok"; dirty_ = true; }
    } else if (key == SDLK_SPACE) {
        if (focusedId_.rfind("opt_", 0) == 0) adjustOption(focusedId_, 1);
        else activateId(focusedId_);
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        activateId(focusedId_ == "cancel" ? "cancel" : "ok");
    }
}

void GoldFrontEnd::handleRatingsKey(SDL_Keycode key) {
    const std::size_t count = ratingsRowCount();
    const int maxOff = std::max(0, int(count) - kVisibleRatingsRows);
    if (key == SDLK_UP) scrollRatingsBy(-1);
    else if (key == SDLK_DOWN) scrollRatingsBy(1);
    else if (key == SDLK_PAGEUP) scrollRatingsBy(-kVisibleRatingsRows);
    else if (key == SDLK_PAGEDOWN) scrollRatingsBy(kVisibleRatingsRows);
    else if (key == SDLK_HOME) {
        if (ratingsScroll_ != 0) { ratingsScroll_ = 0; dirty_ = true; }
    } else if (key == SDLK_END) {
        if (ratingsScroll_ != maxOff) { ratingsScroll_ = maxOff; dirty_ = true; }
    } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
        cycleFocus({"ratings_campaigns", "ratings_missions", "cancel"}, key == SDLK_LEFT ? -1 : 1);
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
        activateId(focusedId_.empty() ? "cancel" : focusedId_);
    }
}

void GoldFrontEnd::handleCreditsKey(SDL_Keycode key) {
    const double minScroll = -kCreditsClipH + 40.0;
    const double maxScroll = creditsContentHeight_ > 0.0 ? creditsContentHeight_ : 1.0e6;
    if (key == SDLK_SPACE) {
        creditsPaused_ = !creditsPaused_;
        dirty_ = true;
    } else if (key == SDLK_UP) {
        creditsScroll_ = std::max(minScroll, creditsScroll_ - 28.0);
        dirty_ = true;
    } else if (key == SDLK_DOWN) {
        creditsScroll_ = std::min(maxScroll, creditsScroll_ + 28.0);
        dirty_ = true;
    } else if (key == SDLK_PAGEUP) {
        creditsScroll_ = std::max(minScroll, creditsScroll_ - 160.0);
        dirty_ = true;
    } else if (key == SDLK_PAGEDOWN) {
        creditsScroll_ = std::min(maxScroll, creditsScroll_ + 160.0);
        dirty_ = true;
    } else if (key == SDLK_HOME) {
        creditsScroll_ = 0.0;
        dirty_ = true;
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        activateId("cancel");
    }
}

void GoldFrontEnd::renderOptions(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    drawHeading(cr, pangoCtx, tr("Options"), {200, 28, 400, 32}, 18.0);
    {
        const bool docHov = (hoveredId_ == "opt_doctrine");
        const bool docFoc = (focusedId_ == "opt_doctrine");
        drawBronzeBox(cr, kOptionsDoctrineRect.x, kOptionsDoctrineRect.y,
                      kOptionsDoctrineRect.w, kOptionsDoctrineRect.h, (docHov || docFoc) ? 0.94 : 0.85);
        const std::string docLabel = tr("Doctrine") + ": " +
            tr(optionsDraft_.realismDoctrine ? "Realism" : "Classic");
        drawGoldStencilButton(cr, pangoCtx, kOptionsDoctrineRect, docLabel, '\0',
                              docHov, docFoc, true, 13.5, true);
    }
    drawBronzeBox(cr, kOptionsPanelRect.x, kOptionsPanelRect.y,
                  kOptionsPanelRect.w, kOptionsPanelRect.h, 0.88);

    const auto langVal = ss2::language();
    const std::string langLabel = (langVal == Language::french) ? "Français"
        : (langVal == Language::spanish ? "Español" : "English");
    const long sfxPct = std::lround(std::clamp(optionsDraft_.sfxVolume, 0.0f, 1.0f) * 100.0f);
    const std::string sfxStr = sfxPct <= 0 ? tr("Muted") : (std::to_string(sfxPct) + "%");
    const float mv = optionsDraft_.musicVolume;
    const std::string musVolStr = mv >= 0.34f ? "100%" : mv >= 0.23f ? "75%" : mv >= 0.13f ? "50%" : "25%";
    const std::string musStateStr = tr(optionsDraft_.musicMuted ? "Muted" : "On");
    const std::string spdStr = optionsDraft_.speedPercent == 50 ? "0.5x"
        : optionsDraft_.speedPercent == 200 ? "2x" : optionsDraft_.speedPercent == 300 ? "3x" : "1x";

    std::string modStr = tr("Original");
    if (!optionsDraft_.activeMod.empty()) {
        modStr = optionsDraft_.activeMod;
        for (const auto& m : availableMods_) {
            if (assets::equalsIgnoreCaseAscii(m.id, optionsDraft_.activeMod)) {
                modStr = m.name;
                break;
            }
        }
        if (modStr == optionsDraft_.activeMod) {
            const auto stem = std::filesystem::path(optionsDraft_.activeMod).filename().string();
            if (!stem.empty()) modStr = stem;
        }
    }

    struct OptDisplay {
        std::string label;
        std::string value;
    };
    const std::array<OptDisplay, 10> rows{{
        {tr("Game difficulty:"), tr(difficultyKey(optionsDraft_.difficulty))},
        {tr("Language"), langLabel},
        {tr("SFX volume"), sfxStr},
        {tr("Music volume"), musVolStr},
        {tr("Music"), musStateStr},
        {tr("Game speed"), spdStr},
        {tr("Smooth terrain"), tr(optionsDraft_.smoothTerrain ? "On" : "Off")},
        {tr("Tracers and flashes"), tr(optionsDraft_.combatEffects ? "On" : "Off")},
        {tr("Skip Intro"), tr(optionsDraft_.skipIntro ? "On" : "Off")},
        {tr("Mod"), modStr},
    }};

    for (std::size_t i = 0; i < kOptionRowIds.size(); ++i) {
        const RectD rr = optionRowRect(i);
        const RectD dr = optionDecRect(i);
        const RectD vr = optionValueRect(i);
        const RectD ir = optionIncRect(i);
        const std::string id = kOptionRowIds[i];
        const std::string decId = "opt_dec:" + id;
        const std::string incId = "opt_inc:" + id;
        const bool rowHov = (hoveredId_ == decId || hoveredId_ == incId);
        const bool rowFoc = (focusedId_ == id);

        if (rowFoc) {
            cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.36);
            cairo_rectangle(cr, rr.x, rr.y, rr.w, rr.h);
            cairo_fill(cr);
        } else if (rowHov) {
            cairo_set_source_rgba(cr, 72.0 / 255.0, 96.0 / 255.0, 56.0 / 255.0, 0.22);
            cairo_rectangle(cr, rr.x, rr.y, rr.w, rr.h);
            cairo_fill(cr);
        } else if ((i % 2) == 1) {
            cairo_set_source_rgba(cr, 36.0 / 255.0, 42.0 / 255.0, 28.0 / 255.0, 0.22);
            cairo_rectangle(cr, rr.x, rr.y, rr.w, rr.h);
            cairo_fill(cr);
        }

        auto lbl = makeTextLayout(pangoCtx, rows[i].label, 14.5, rowFoc);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(lbl.get(), &tw, &th);
        cairo_move_to(cr, rr.x + 12.0, rr.y + (rr.h - th) * 0.5);
        cairo_set_source_rgb(cr, rowFoc ? 248.0 / 255.0 : 232.0 / 255.0,
                             rowFoc ? 236.0 / 255.0 : 228.0 / 255.0,
                             rowFoc ? 186.0 / 255.0 : 206.0 / 255.0);
        pango_cairo_show_layout(cr, lbl.get());

        auto drawArrowBox = [&](const RectD& br, const char* sym, bool hov) {
            drawBronzeBox(cr, br.x, br.y, br.w, br.h, hov ? 0.96 : 0.86);
            drawGoldStencilButton(cr, pangoCtx, br, sym, '\0', hov, false, true, 16.0, true);
        };
        drawArrowBox(dr, "<", hoveredId_ == decId);
        drawBronzeBox(cr, vr.x, vr.y, vr.w, vr.h, (rowFoc || rowHov) ? 0.92 : 0.84);
        // Retail menu_common.sue widgets: speedbar.pck/speed_bt.pck sliders for the
        // continuous rows, rombik.pck emerald gems for difficulty.
        std::optional<double> sliderFrac;
        if (i == 2) sliderFrac = std::clamp(double(optionsDraft_.sfxVolume), 0.0, 1.0);
        else if (i == 3) sliderFrac = mv >= 0.34f ? 1.0 : mv >= 0.23f ? 2.0 / 3.0 : mv >= 0.13f ? 1.0 / 3.0 : 0.0;
        else if (i == 5) {
            const int sp = optionsDraft_.speedPercent;
            sliderFrac = sp == 50 ? 0.0 : sp == 200 ? 2.0 / 3.0 : sp == 300 ? 1.0 : 1.0 / 3.0;
        }
        const auto& bar = assets_->speedBar;
        const auto& thumb = assets_->speedThumb[(rowHov || rowFoc) ? 1 : 0];
        if (sliderFrac && bar.surface && thumb.surface) {
            const double bx = vr.x + 8.0;
            const double by = vr.y + std::floor((vr.h - bar.h) * 0.5);
            drawPckAt(cr, bar, bx, by);
            const double tx = bx + std::round(*sliderFrac * double(bar.w - thumb.w));
            drawPckAt(cr, thumb, tx, vr.y + std::floor((vr.h - thumb.h) * 0.5));
            const double textX = bx + bar.w + 4.0;
            const RectD textR{textX, vr.y, vr.x + vr.w - 4.0 - textX, vr.h};
            drawGoldStencilButton(cr, pangoCtx, textR, rows[i].value, '\0', rowHov, rowFoc, true, 13.5, true);
        } else if (i == 0 && assets_->rombik[0].surface && assets_->rombik[1].surface) {
            const int diff = std::clamp(optionsDraft_.difficulty, 1, 3);
            const auto& gem = assets_->rombik[0];
            const double gy = vr.y + std::floor((vr.h - gem.h) * 0.5);
            for (int g = 0; g < 3; ++g) {
                drawPckAt(cr, assets_->rombik[g < diff ? 1 : 0], vr.x + 6.0 + g * (gem.w + 1.0), gy);
            }
            const double textX = vr.x + 6.0 + 3.0 * (gem.w + 1.0) + 2.0;
            const RectD textR{textX, vr.y, vr.x + vr.w - 4.0 - textX, vr.h};
            drawGoldStencilButton(cr, pangoCtx, textR, rows[i].value, '\0', rowHov, rowFoc, true, 14.5, true);
        } else {
            drawGoldStencilButton(cr, pangoCtx, vr, rows[i].value, '\0', rowHov, rowFoc, true, 14.5, true);
        }
        drawArrowBox(ir, ">", hoveredId_ == incId);
    }

    drawGoldStencilButton(cr, pangoCtx, kOptionsOkRect, tr("OK"), '\0',
                          hoveredId_ == "ok", focusedId_ == "ok", true, 24.0);
    {
        const bool egHov = (hoveredId_ == "opt_enhanced_gfx");
        const bool egFoc = (focusedId_ == "opt_enhanced_gfx");
        drawBronzeBox(cr, kOptionsEnhancedGfxRect.x, kOptionsEnhancedGfxRect.y,
                      kOptionsEnhancedGfxRect.w, kOptionsEnhancedGfxRect.h, (egHov || egFoc) ? 0.94 : 0.85);
        const std::string egLabel = tr("Enhanced graphics") + ": " +
            tr(optionsDraft_.enhancedGraphics ? "On" : "Off");
        drawGoldStencilButton(cr, pangoCtx, kOptionsEnhancedGfxRect, egLabel, '\0',
                              egHov, egFoc, true, 14.5, true);
    }
    drawGoldStencilButton(cr, pangoCtx, kOptionsCancelRect, tr("Cancel"), '\0',
                          hoveredId_ == "cancel", focusedId_ == "cancel", true, 24.0);
}

void GoldFrontEnd::renderRatings(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    drawHeading(cr, pangoCtx, tr("Ratings"), {200, 28, 400, 32}, 18.0);

    auto modeLbl = makeTextLayout(pangoCtx, tr("Rating:"), 14.0, false);
    cairo_move_to(cr, kRatingsTableX, 71.0);
    cairo_set_source_rgb(cr, 236.0 / 255.0, 232.0 / 255.0, 212.0 / 255.0);
    pango_cairo_show_layout(cr, modeLbl.get());

    const auto mode = fe_.ratingsMode();
    const bool campActive = (mode == FrontEnd::RatingsMode::Campaigns);
    const bool missActive = (mode == FrontEnd::RatingsMode::SingleMissions);

    auto drawModeTab = [&](const RectD& r, const char* id, const std::string& label,
                           char shortcut, bool active) {
        const bool hov = (hoveredId_ == id);
        const bool foc = (focusedId_ == id);
        drawBronzeBox(cr, r.x, r.y, r.w, r.h, active ? 0.94 : (hov ? 0.88 : 0.78));
        if (active) {
            cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.34);
            cairo_rectangle(cr, r.x + 3.0, r.y + 3.0, r.w - 6.0, r.h - 6.0);
            cairo_fill(cr);
        }
        drawGoldStencilButton(cr, pangoCtx, r, label, shortcut, hov, foc || active, true, 15.0, true);
    };
    drawModeTab(kRatingsCampaignsTabRect, "ratings_campaigns", tr("Campaigns"), 'C', campActive);
    drawModeTab(kRatingsMissionsTabRect, "ratings_missions", tr("Single Missions"), 'S', missActive);

    int totalScore = 0;
    std::size_t completedCount = 0;
    std::size_t totalMissions = 0;
    if (campActive) {
        for (const auto& c : campaigns_) {
            for (const auto& m : c.missions) {
                ++totalMissions;
                if (m.completed) ++completedCount;
                totalScore += std::max(0, m.highScore);
            }
        }
    } else {
        totalMissions = missions_.size();
        for (const auto& m : missions_) {
            if (m.completed) ++completedCount;
            totalScore += std::max(0, m.highScore);
        }
    }

    drawBronzeBox(cr, 438, 94, 319, 28, 0.86);
    const std::string totalStr = tr("Total Score") + ": " + std::to_string(totalScore) +
        "   (" + std::to_string(completedCount) + "/" + std::to_string(totalMissions) + ")";
    drawHeading(cr, pangoCtx, totalStr, {442, 94, 311, 28}, 13.5);

    drawBronzeBox(cr, kRatingsTableX, kRatingsTableY, kRatingsTableW, kRatingsTableH, 0.88);
    {
        auto colMission = makeTextLayout(pangoCtx, tr("Missions:"), 13.5, true);
        cairo_move_to(cr, kRatingsTableX + 14.0, kRatingsTableY + 8.0);
        cairo_set_source_rgb(cr, 232.0 / 255.0, 224.0 / 255.0, 188.0 / 255.0);
        pango_cairo_show_layout(cr, colMission.get());

        auto colStatus = makeTextLayout(pangoCtx, tr("Completed"), 13.5, true, 130.0, PANGO_ALIGN_CENTER);
        cairo_move_to(cr, 480.0, kRatingsTableY + 8.0);
        pango_cairo_show_layout(cr, colStatus.get());

        auto colScore = makeTextLayout(pangoCtx, tr("Score"), 13.5, true, 110.0, PANGO_ALIGN_RIGHT);
        cairo_move_to(cr, 614.0, kRatingsTableY + 8.0);
        pango_cairo_show_layout(cr, colScore.get());

        cairo_set_source_rgba(cr, 182.0 / 255.0, 174.0 / 255.0, 146.0 / 255.0, 0.55);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, kRatingsTableX + 8.0, kRatingsTableY + 30.0);
        cairo_line_to(cr, kRatingsTableX + kRatingsTableW - 8.0, kRatingsTableY + 30.0);
        cairo_stroke(cr);
    }

    const auto rows = buildRatingsRows(mode, campaigns_, missions_);
    const int maxOff = std::max(0, int(rows.size()) - kVisibleRatingsRows);
    ratingsScroll_ = std::clamp(ratingsScroll_, 0, maxOff);

    cairo_save(cr);
    cairo_rectangle(cr, kRatingsTableX + 6.0, kRatingsTableY + 32.0, kRatingsTableW - 12.0, kRatingsTableH - 38.0);
    cairo_clip(cr);

    if (rows.empty()) {
        const std::string emptyMsg = campActive ? tr("No campaigns installed.") : tr("No missions installed.");
        auto emptyLbl = makeTextLayout(pangoCtx, emptyMsg, 14.0, false);
        cairo_move_to(cr, kRatingsTableX + 18.0, kRatingsTableY + 48.0);
        cairo_set_source_rgb(cr, 180.0 / 255.0, 180.0 / 255.0, 165.0 / 255.0);
        pango_cairo_show_layout(cr, emptyLbl.get());
    } else {
        const int endIdx = std::min(int(rows.size()), ratingsScroll_ + kVisibleRatingsRows);
        for (int idx = ratingsScroll_; idx < endIdx; ++idx) {
            const int visRow = idx - ratingsScroll_;
            const double ry = kRatingsTableY + 36.0 + visRow * kRatingsRowH;
            const auto& row = rows[std::size_t(idx)];
            if (row.isHeader) {
                cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.34);
                cairo_rectangle(cr, kRatingsTableX + 8.0, ry, kRatingsTableW - 16.0, kRatingsRowH - 1.0);
                cairo_fill(cr);
            } else if ((visRow % 2) == 1) {
                cairo_set_source_rgba(cr, 40.0 / 255.0, 48.0 / 255.0, 32.0 / 255.0, 0.24);
                cairo_rectangle(cr, kRatingsTableX + 8.0, ry, kRatingsTableW - 16.0, kRatingsRowH - 1.0);
                cairo_fill(cr);
            }

            auto nameLayout = makeTextLayout(pangoCtx, row.name, 13.0, row.isHeader);
            pango_layout_set_width(nameLayout.get(), int(412.0 * PANGO_SCALE));
            pango_layout_set_ellipsize(nameLayout.get(), PANGO_ELLIPSIZE_END);
            cairo_move_to(cr, kRatingsTableX + 14.0, ry + 2.0);
            if (row.isHeader) {
                cairo_set_source_rgb(cr, 245.0 / 255.0, 224.0 / 255.0, 145.0 / 255.0);
            } else if (row.completed) {
                cairo_set_source_rgb(cr, 242.0 / 255.0, 244.0 / 255.0, 236.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
            }
            pango_cairo_show_layout(cr, nameLayout.get());

            const bool gemBadge = !row.isHeader && row.completed && assets_->rombik[1].surface != nullptr;
            const std::string statusShown = row.isHeader
                ? row.status
                : (row.completed ? (gemBadge ? tr("Completed") : (tr("Completed") + " [\xE2\x9C\x93]"))
                                 : std::string("-"));
            const double statX = gemBadge ? 480.0 + double(assets_->rombik[1].w) : 480.0;
            const double statW = gemBadge ? 130.0 - double(assets_->rombik[1].w) : 130.0;
            if (gemBadge) drawPckAt(cr, assets_->rombik[1], 482.0, ry + 2.0);
            auto statLayout = makeTextLayout(pangoCtx, statusShown, 12.5, row.isHeader, statW, PANGO_ALIGN_CENTER);
            cairo_move_to(cr, statX, ry + 2.0);
            if (row.completed || row.isHeader) {
                cairo_set_source_rgb(cr, 212.0 / 255.0, 224.0 / 255.0, 178.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 132.0 / 255.0, 136.0 / 255.0, 122.0 / 255.0);
            }
            pango_cairo_show_layout(cr, statLayout.get());

            const std::string scoreShown = (row.score > 0 || row.isHeader)
                ? std::to_string(row.score)
                : std::string("-");
            auto scoreLayout = makeTextLayout(pangoCtx, scoreShown, 13.0, row.isHeader || row.score > 0, 110.0, PANGO_ALIGN_RIGHT);
            cairo_move_to(cr, 614.0, ry + 2.0);
            if (row.score > 0) {
                cairo_set_source_rgb(cr, 246.0 / 255.0, 216.0 / 255.0, 124.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 140.0 / 255.0, 142.0 / 255.0, 130.0 / 255.0);
            }
            pango_cairo_show_layout(cr, scoreLayout.get());
        }
    }
    cairo_restore(cr);

    drawGoldScrollbar(env, kRatingsScrollX, kRatingsScrollY, kRatingsScrollW, kRatingsScrollH,
                      kVisibleRatingsRows, true, rows.size(), ratingsScroll_,
                      hoveredId_ == "ratings_scroll_up", hoveredId_ == "ratings_scroll_down",
                      hoveredId_ == "ratings_scroll_thumb" || draggingThumb_);

    if (totalScore == 0 && !rows.empty()) {
        auto noScore = makeTextLayout(pangoCtx, tr("No scores recorded yet."), 13.0, false);
        cairo_move_to(cr, kRatingsTableX, 520.0);
        cairo_set_source_rgba(cr, 220.0 / 255.0, 216.0 / 255.0, 190.0 / 255.0, 0.85);
        pango_cairo_show_layout(cr, noScore.get());
    }

    drawGoldStencilButton(cr, pangoCtx, kRatingsCancelRect, tr("Cancel"), '\0',
                          hoveredId_ == "cancel", focusedId_ == "cancel", true, 24.0);
}

void GoldFrontEnd::renderCredits(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    if (assets_->creditsBg.surface) {
        drawSurfaceStretched(cr, assets_->creditsBg.surface.get(), 0, 0, kStageW, kStageH);
    } else if (assets_->mainBg) {
        drawSurfaceStretched(cr, assets_->mainBg.get(), 0, 0, kStageW, kStageH);
    } else {
        drawFallbackBackdrop(cr, false);
    }

    drawHeading(cr, pangoCtx, tr("Credits"), {200, 22, 400, 32}, 18.0);
    drawBronzeBox(cr, kCreditsFrameRect.x, kCreditsFrameRect.y,
                  kCreditsFrameRect.w, kCreditsFrameRect.h, 0.72);

    cairo_save(cr);
    cairo_rectangle(cr, kCreditsClipX, kCreditsClipY, kCreditsClipW, kCreditsClipH);
    cairo_clip(cr);

    double lineY = 0.0;
    for (const std::string& line : assets_->creditsLines) {
        if (line.empty()) {
            lineY += 14.0;
            continue;
        }
        const bool isBanner = line.size() >= 3 && line.front() == '-' && line.back() == '-';
        const bool isRole = !isBanner && isCreditsRoleHeading(line);
        const double fontPx = isBanner ? 19.0 : (isRole ? 14.5 : 15.5);
        const bool bold = isBanner || isRole;

        auto layout = makeTextLayout(pangoCtx, line, fontPx, bold, kCreditsClipW - 16.0, PANGO_ALIGN_CENTER);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(layout.get(), &tw, &th);
        const double drawY = kCreditsClipY + 32.0 + lineY - creditsScroll_;
        if (drawY + th >= kCreditsClipY - 8.0 && drawY <= kCreditsClipY + kCreditsClipH + 8.0) {
            cairo_move_to(cr, kCreditsClipX + 8.0 + 1.5, drawY + 1.5);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.92);
            pango_cairo_show_layout(cr, layout.get());

            cairo_move_to(cr, kCreditsClipX + 8.0, drawY);
            if (isBanner) {
                cairo_set_source_rgb(cr, 248.0 / 255.0, 218.0 / 255.0, 128.0 / 255.0);
            } else if (isRole) {
                cairo_set_source_rgb(cr, 220.0 / 255.0, 208.0 / 255.0, 160.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 244.0 / 255.0, 246.0 / 255.0, 238.0 / 255.0);
            }
            pango_cairo_show_layout(cr, layout.get());
        }
        lineY += double(th) + (isBanner ? 8.0 : 5.0);
    }
    creditsContentHeight_ = lineY;
    cairo_restore(cr);

    const std::string hintText = creditsPaused_
        ? tr("Paused — Space to resume")
        : tr("Wheel / Up / Down to scroll • Space to pause");
    auto hintLayout = makeTextLayout(pangoCtx, hintText, 12.5, false);
    cairo_move_to(cr, kCreditsFrameRect.x, 520.0);
    cairo_set_source_rgba(cr, 220.0 / 255.0, 216.0 / 255.0, 190.0 / 255.0, 0.88);
    pango_cairo_show_layout(cr, hintLayout.get());

    drawGoldStencilButton(cr, pangoCtx, kCreditsCancelRect, tr("Cancel"), '\0',
                          hoveredId_ == "cancel", focusedId_ == "cancel", true, 24.0);
}

}  // namespace ss2
