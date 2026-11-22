#include "ui/gold_frontend_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ss2 {
namespace {

using namespace gold_ui;

constexpr std::array<ButtonSpec, 7> kSinglePlayerButtons{{
    {"profile_new", "New", '\0', {305, 97, 56, 27}, true},
    {"profile_rename", "Rename", '\0', {69, 126, 98, 30}, true},
    {"campaigns", "Campaigns", 'C', {78, 179, 218, 35}, true},
    {"missions", "Single Missions", 'S', {40, 246, 293, 35}, true},
    {"load", "Load", 'L', {134, 312, 108, 35}, true},
    {"profile_delete", "Delete", '\0', {480, 462, 82, 27}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr std::array<ButtonSpec, 2> kCampaignFooterButtons{{
    {"custom", "Custom", 'C', {118, 512, 140, 35}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};
constexpr RectD kCustomCancelRect{542, 512, 118, 35};
constexpr RectD kProfileNameRect{66, 94, 238, 28};
constexpr RectD kProfileListRect{462, 95, 258, 362};
constexpr RectD kProfileScrollUpRect{720, 95, 18, 18};
constexpr RectD kProfileScrollDownRect{720, 439, 18, 18};

constexpr std::array<ButtonSpec, 2> kSingleMissionButtons{{
    {"start", "Start", 'S', {135, 512, 106, 35}, true},
    {"cancel", "Cancel", '\0', {542, 512, 118, 35}, true},
}};

constexpr RectD kDifficultyButtonRect{268, 515, 244, 30};

constexpr std::array<ButtonSpec, 4> kBriefingButtons{{
    {"brief_restart", "Restart", 'R', {577, 211, 186, 33}, false},
    {"brief_pause", "Pause", 'P', {627, 253, 136, 33}, false},
    {"start", "Start", 'S', {627, 484, 136, 33}, true},
    {"cancel", "Cancel", '\0', {627, 526, 136, 33}, true},
}};

RectD campaignRectFor(std::size_t idx, std::size_t count) {
    static constexpr std::array<double, 5> kGoldYs{{111.0, 178.0, 245.0, 312.0, 378.0}};
    if (count <= kGoldYs.size() && idx < kGoldYs.size()) {
        return RectD{78.0, kGoldYs[idx], 220.0, 35.0};
    }
    const double step = 338.0 / double(std::max<std::size_t>(1, count));
    const double h = std::min(34.0, std::max(22.0, step - 4.0));
    return RectD{78.0, 102.0 + double(idx) * step, 220.0, h};
}

struct CampaignButtonDisplay {
    std::string label;
    char shortcut;
};

CampaignButtonDisplay campaignButtonDisplay(const std::string& name) {
    std::string low = name;
    for (char& ch : low) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (low.find("german") != std::string::npos || low == "de") return {tr("Germany"), 'G'};
    if (low.find("japan") != std::string::npos) return {tr("Japan"), 'J'};
    if (low.find("brit") != std::string::npos || low == "gb" || low == "uk") return {tr("GB"), 'B'};
    if (low == "us" || low.starts_with("us ") || low.find("usa") != std::string::npos ||
        low.find("american") != std::string::npos || low.find("u.s.") != std::string::npos) {
        return {tr("USA"), 'U'};
    }
    if (low.find("russ") != std::string::npos || low.find("ussr") != std::string::npos ||
        low.find("soviet") != std::string::npos) {
        return {tr("USSR"), 'R'};
    }
    return {name, '\0'};
}

}  // namespace

void GoldFrontEnd::setMissions(std::vector<GoldMissionEntry> missions) {
    missions_ = std::move(missions);
    std::vector<FrontEnd::Entry> base;
    base.reserve(missions_.size());
    for (const auto& m : missions_) {
        base.push_back(FrontEnd::Entry{m.name, m.path, m.description, m.tag});
    }
    fe_.setMissions(std::move(base));
    assets_->overviews.clear();
    if (fe_.screen() == FrontEnd::Screen::SingleMissions) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
    } else if (fe_.screen() == FrontEnd::Screen::Ratings) {
        const int maxOff = std::max(0, int(ratingsRowCount()) - kVisibleRatingsRows);
        ratingsScroll_ = std::clamp(ratingsScroll_, 0, maxOff);
    }
    dirty_ = true;
}

void GoldFrontEnd::setCampaigns(std::vector<GoldCampaignEntry> campaigns) {
    campaigns_ = std::move(campaigns);
    std::vector<FrontEnd::CampaignEntry> base;
    base.reserve(campaigns_.size());
    for (const auto& c : campaigns_) {
        FrontEnd::CampaignEntry ce;
        ce.name = c.name;
        ce.missions.reserve(c.missions.size());
        for (const auto& m : c.missions) {
            ce.missions.push_back(FrontEnd::CampaignMissionEntry{
                m.code, m.name, m.path, m.easyPath, m.description, m.tag
            });
        }
        base.push_back(std::move(ce));
    }
    fe_.setCampaigns(std::move(base));
    assets_->overviews.clear();
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
    } else if (fe_.screen() == FrontEnd::Screen::Ratings) {
        const int maxOff = std::max(0, int(ratingsRowCount()) - kVisibleRatingsRows);
        ratingsScroll_ = std::clamp(ratingsScroll_, 0, maxOff);
    }
    if (fe_.screen() == FrontEnd::Screen::Campaigns) ensureFocusedValid();
    dirty_ = true;
}

void GoldFrontEnd::setCustomCampaigns(std::vector<GoldCampaignEntry> campaigns) {
    customCampaigns_ = std::move(campaigns);
    std::vector<FrontEnd::CampaignEntry> base;
    base.reserve(customCampaigns_.size());
    for (const auto& c : customCampaigns_) {
        FrontEnd::CampaignEntry ce;
        ce.name = c.name;
        ce.missions.reserve(c.missions.size());
        for (const auto& m : c.missions) {
            ce.missions.push_back(FrontEnd::CampaignMissionEntry{
                m.code, m.name, m.path, m.easyPath, m.description, m.tag
            });
        }
        base.push_back(std::move(ce));
    }
    fe_.setCustomCampaigns(std::move(base));
    assets_->overviews.clear();
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions && fe_.inCustomCampaign()) {
        scrollOffset_ = 0;
        ensureSelectionVisible();
    }
    if (fe_.screen() == FrontEnd::Screen::CustomCampaigns) ensureFocusedValid();
    dirty_ = true;
}

const std::vector<GoldCampaignEntry>& GoldFrontEnd::activeCampaignList() const {
    return fe_.inCustomCampaign() ? customCampaigns_ : campaigns_;
}

std::size_t GoldFrontEnd::activeCampaignIndex() const {
    return fe_.inCustomCampaign() ? fe_.selectedCustomCampaign() : fe_.selectedCampaign();
}

std::size_t GoldFrontEnd::previewCampaignIndex(bool custom) const {
    const auto& list = custom ? customCampaigns_ : campaigns_;
    if (list.empty()) return 0;
    const char* prefix = custom ? "ccampaign:" : "campaign:";
    std::size_t idx = 0;
    if (parseIndexAfter(hoveredId_, prefix, idx) && idx < list.size()) return idx;
    if (parseIndexAfter(focusedId_, prefix, idx) && idx < list.size()) return idx;
    const std::size_t sel = custom ? fe_.selectedCustomCampaign() : fe_.selectedCampaign();
    return sel < list.size() ? sel : 0;
}

bool GoldFrontEnd::hasBriefingMedia() const {
    return assets_ && (assets_->briefMedia.hasAnimation() || assets_->briefMedia.hasVoice());
}

std::size_t GoldFrontEnd::briefingPartCount() const {
    return assets_ ? assets_->briefMedia.parts.size() : 0u;
}

std::string GoldFrontEnd::briefingDisplayedText() const {
    if (assets_ && !assets_->briefMedia.paragraphs.empty()) {
        if (briefActivePart_ > 0 &&
            std::size_t(briefActivePart_) < assets_->briefMedia.paragraphs.size() &&
            !assets_->briefMedia.paragraphs[std::size_t(briefActivePart_)].empty()) {
            return assets_->briefMedia.paragraphs[std::size_t(briefActivePart_)];
        }
        return assets_->briefMedia.paragraphs[0];
    }
    const std::string raw = fe_.briefingText();
    const auto paras = assets::splitBriefingParagraphs(raw);
    return paras.empty() ? raw : paras[0];
}

void GoldFrontEnd::stopBriefingVoice() {
    if (assets_ && assets_->briefAudioStream) {
        SDL_ClearAudioStream(assets_->briefAudioStream);
    }
}

void GoldFrontEnd::setBriefingVoicePaused(bool paused) {
    if (!assets_ || !assets_->briefAudioDev) return;
    if (paused) SDL_PauseAudioDevice(assets_->briefAudioDev);
    else SDL_ResumeAudioDevice(assets_->briefAudioDev);
}

void GoldFrontEnd::startBriefingVoice(const std::vector<std::int16_t>& pcm) {
    stopBriefingVoice();
    if (!assets_ || !window_ || pcm.empty() || settings_.sfxVolume <= 0.001f) return;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = assets::kBriefingVoiceSampleRate;
    if (!assets_->briefAudioDev) {
        assets_->briefAudioDev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
        if (!assets_->briefAudioDev) return;
    }
    if (!assets_->briefAudioStream) {
        assets_->briefAudioStream = SDL_CreateAudioStream(&spec, &spec);
        if (!assets_->briefAudioStream ||
            !SDL_BindAudioStream(assets_->briefAudioDev, assets_->briefAudioStream)) {
            if (assets_->briefAudioStream) {
                SDL_DestroyAudioStream(assets_->briefAudioStream);
                assets_->briefAudioStream = nullptr;
            }
            return;
        }
    }
    SDL_SetAudioStreamGain(assets_->briefAudioStream, std::clamp(settings_.sfxVolume, 0.0f, 1.0f));
    SDL_ClearAudioStream(assets_->briefAudioStream);
    SDL_ResumeAudioDevice(assets_->briefAudioDev);
    SDL_PutAudioStreamData(assets_->briefAudioStream, pcm.data(),
                           static_cast<int>(pcm.size() * sizeof(std::int16_t)));
}

void GoldFrontEnd::clearBriefingMedia() {
    stopBriefingVoice();
    if (assets_) {
        assets_->briefMedia = {};
    }
    briefMediaPath_.clear();
    briefActivePart_ = 0;
    briefLoopPart_ = 0;
    briefFrameIndex_ = 0;
    briefAccumMs_ = 0.0;
    briefHoldMs_ = 0.0;
    briefPaused_ = false;
    briefOverlayDirty_ = false;
}

void GoldFrontEnd::loadBriefingMedia(const std::string& path) {
    clearBriefingMedia();
    briefScroll_ = 0.0;
    briefMediaPath_ = path;
    if (!path.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            try {
                assets_->briefMedia = assets::readMissionBriefingMedia(path);
            } catch (...) {
                assets_->briefMedia = {};
            }
        }
    }
    briefOverlayDirty_ = assets_ && assets_->briefMedia.hasAnimation();
    if (assets_ && !assets_->briefMedia.overviewVoicePcm.empty()) {
        startBriefingVoice(assets_->briefMedia.overviewVoicePcm);
    }
    dirty_ = true;
}

void GoldFrontEnd::selectBriefingPart(int part) {
    if (!assets_) return;
    const int clamped = (part <= 0 || std::size_t(part) > assets_->briefMedia.parts.size()) ? 0 : part;
    briefActivePart_ = clamped;
    briefLoopPart_ = (clamped == 0) ? 0u : static_cast<std::size_t>(clamped - 1);
    briefFrameIndex_ = 0u;
    briefAccumMs_ = 0.0;
    briefHoldMs_ = 0.0;
    briefPaused_ = false;
    briefScroll_ = 0.0;
    briefOverlayDirty_ = assets_->briefMedia.hasAnimation();
    dirty_ = true;
    if (clamped == 0) {
        startBriefingVoice(assets_->briefMedia.overviewVoicePcm);
    } else {
        startBriefingVoice(assets_->briefMedia.parts[static_cast<std::size_t>(clamped - 1)].voicePcm);
    }
}

void GoldFrontEnd::renderBriefingOverlayIfNeeded() {
    if (!assets_ || !assets_->briefMedia.hasAnimation()) return;
    const std::size_t curPart = (briefActivePart_ > 0)
        ? static_cast<std::size_t>(briefActivePart_ - 1)
        : briefLoopPart_;
    if (curPart >= assets_->briefMedia.parts.size() ||
        assets_->briefMedia.parts[curPart].frames.empty()) {
        return;
    }
    if (!assets_->briefOverlay) {
        assets_->briefOverlay = Surface(
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                       assets::kBriefingOverlayWidth,
                                       assets::kBriefingOverlayHeight),
            cairo_surface_destroy);
    }
    if (!assets_->briefOverlay ||
        cairo_surface_status(assets_->briefOverlay.get()) != CAIRO_STATUS_SUCCESS) {
        return;
    }
    if (!briefOverlayDirty_) return;
    cairo_surface_flush(assets_->briefOverlay.get());
    auto* dst = reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(assets_->briefOverlay.get()));
    const int stridePx = cairo_image_surface_get_stride(assets_->briefOverlay.get()) / static_cast<int>(sizeof(std::uint32_t));
    assets::renderBriefingFrameArgb32(assets_->briefMedia.parts[curPart], briefFrameIndex_, dst,
                                      assets::kBriefingOverlayWidth, assets::kBriefingOverlayHeight,
                                      stridePx);
    cairo_surface_mark_dirty(assets_->briefOverlay.get());
    briefOverlayDirty_ = false;
}

void GoldFrontEnd::advanceBriefingMedia(std::chrono::nanoseconds dt) {
    if (briefPaused_ || !assets_ || !assets_->briefMedia.hasAnimation()) return;
    constexpr double kFrameMs = 33.0;
    constexpr double kPartHoldMs = 1000.0;
    double dtMs = std::clamp(std::chrono::duration<double, std::milli>(dt).count(), 0.0, 2000.0);
    const std::size_t nParts = assets_->briefMedia.parts.size();
    if (nParts == 0u) return;

    std::size_t curPart = (briefActivePart_ > 0)
        ? static_cast<std::size_t>(briefActivePart_ - 1)
        : briefLoopPart_;
    if (curPart >= nParts) {
        curPart = 0u;
        briefLoopPart_ = 0u;
    }
    if (briefHoldMs_ > 0.0) {
        briefHoldMs_ -= dtMs;
        if (briefHoldMs_ <= 0.0) {
            dtMs = -briefHoldMs_;
            briefHoldMs_ = 0.0;
            briefFrameIndex_ = 0u;
            briefOverlayDirty_ = true;
            dirty_ = true;
        } else {
            dtMs = 0.0;
        }
    }
    briefAccumMs_ += dtMs;
    int steps = 0;
    while (briefAccumMs_ >= kFrameMs && steps < 64) {
        briefAccumMs_ -= kFrameMs;
        ++steps;
        const auto& frames = assets_->briefMedia.parts[curPart].frames;
        if (frames.empty()) break;
        if (briefFrameIndex_ + 1u < frames.size()) {
            ++briefFrameIndex_;
        } else if (briefActivePart_ == 0) {
            briefLoopPart_ = (briefLoopPart_ + 1u) % nParts;
            curPart = briefLoopPart_;
            briefFrameIndex_ = 0u;
        } else {
            briefHoldMs_ = kPartHoldMs;
            briefAccumMs_ = 0.0;
            briefOverlayDirty_ = true;
            dirty_ = true;
            break;
        }
        briefOverlayDirty_ = true;
        dirty_ = true;
    }
}

std::string GoldFrontEnd::hitTestSinglePlayer(double sx, double sy) const {
    if (const auto id = hitTestButtons(kSinglePlayerButtons, sx, sy); !id.empty()) return id;
    if (kProfileNameRect.contains(sx, sy)) return "profile_name_field";
    if (kProfileScrollUpRect.contains(sx, sy)) return "profile_scroll_up";
    if (kProfileScrollDownRect.contains(sx, sy)) return "profile_scroll_down";
    if (RectD{kProfileListRect.x + 4, kProfileListRect.y + 6, kProfileListRect.w - 8,
              kVisibleProfileRows * kRowH}.contains(sx, sy)) {
        const int row = int((sy - (kProfileListRect.y + 6)) / kRowH);
        const std::size_t idx = std::size_t(profileScroll_ + row);
        if (idx < settings_.profiles.size()) {
            return "profile_row:" + std::to_string(idx);
        }
    }
    return {};
}

std::string GoldFrontEnd::hitTestCampaigns(double sx, double sy) const {
    if (fe_.screen() == FrontEnd::Screen::CustomCampaigns) {
        for (std::size_t i = 0; i < customCampaigns_.size(); ++i) {
            if (campaignRectFor(i, customCampaigns_.size()).contains(sx, sy)) {
                return "ccampaign:" + std::to_string(i);
            }
        }
        if (kCustomCancelRect.contains(sx, sy)) return "cancel";
        return {};
    }
    for (std::size_t i = 0; i < campaigns_.size(); ++i) {
        if (campaignRectFor(i, campaigns_.size()).contains(sx, sy)) {
            return "campaign:" + std::to_string(i);
        }
    }
    return hitTestButtons(kCampaignFooterButtons, sx, sy);
}

std::string GoldFrontEnd::hitTestMissionBrowser(double sx, double sy) const {
    if (fe_.screen() == FrontEnd::Screen::CampaignMissions &&
        kDifficultyButtonRect.contains(sx, sy)) {
        return "difficulty";
    }
    if (const auto id = hitTestButtons(kSingleMissionButtons, sx, sy); !id.empty()) return id;
    return hitTestActiveListAndScrollbar(sx, sy);
}

std::string GoldFrontEnd::hitTestBriefing(double sx, double sy) const {
    for (const auto& b : kBriefingButtons) {
        const bool enabled = (std::string_view(b.id) == "brief_restart" ||
                              std::string_view(b.id) == "brief_pause")
            ? hasBriefingMedia()
            : b.enabled;
        if (enabled && b.rect.contains(sx, sy)) return b.id;
    }
    const std::size_t partCount = briefingPartCount();
    for (std::size_t s = 0; s < 4 && s < partCount; ++s) {
        const auto& slot = assets_->slotButtons[s * 3];
        if (slot.surface && RectD{double(slot.x), double(slot.y), double(slot.w), double(slot.h)}.contains(sx, sy)) {
            return "slot_" + std::to_string(s + 1);
        }
    }
    return {};
}

bool GoldFrontEnd::activateSinglePlayer(const std::string& id) {
    ensureProfilesInitialized(settings_);
    auto clampProfileScroll = [&] {
        const int maxOff = std::max(0, int(settings_.profiles.size()) - kVisibleProfileRows);
        profileScroll_ = std::clamp(profileScroll_, 0, maxOff);
        const std::size_t activeIdx = findProfileIndex(settings_, settings_.activeProfile);
        if (activeIdx < settings_.profiles.size()) {
            if (int(activeIdx) < profileScroll_) profileScroll_ = int(activeIdx);
            else if (int(activeIdx) >= profileScroll_ + kVisibleProfileRows) {
                profileScroll_ = std::min(maxOff, int(activeIdx) - kVisibleProfileRows + 1);
            }
        }
    };
    if (id == "profile_scroll_up") {
        profileScroll_ = std::max(0, profileScroll_ - 1);
        dirty_ = true;
        return true;
    }
    if (id == "profile_scroll_down") {
        const int maxOff = std::max(0, int(settings_.profiles.size()) - kVisibleProfileRows);
        profileScroll_ = std::min(maxOff, profileScroll_ + 1);
        dirty_ = true;
        return true;
    }
    if (std::size_t pIdx = 0; parseIndexAfter(id, "profile_row:", pIdx)) {
        if (profileEditMode_ != ProfileEditMode::None) {
            profileEditMode_ = ProfileEditMode::None;
            if (window_) SDL_StopTextInput(window_);
        }
        if (selectProfile(settings_, pIdx)) {
            profileNameDraft_ = settings_.activeProfile;
            settingsSaved_ = true;
            clampProfileScroll();
            dirty_ = true;
        }
        return true;
    }
    if (id == "profile_name_field") {
        profileEditMode_ = ProfileEditMode::Rename;
        profileNameDraft_ = settings_.activeProfile;
        focusedId_ = "profile_name_field";
        if (window_) SDL_StartTextInput(window_);
        dirty_ = true;
        return true;
    }
    if (id == "profile_new") {
        if (profileEditMode_ != ProfileEditMode::New) {
            profileEditMode_ = ProfileEditMode::New;
            profileNameDraft_.clear();
            focusedId_ = "profile_name_field";
            if (window_) SDL_StartTextInput(window_);
            dirty_ = true;
            return true;
        }
        std::string errKey;
        if (createProfile(settings_, profileNameDraft_, &errKey)) {
            profileEditMode_ = ProfileEditMode::None;
            profileNameDraft_ = settings_.activeProfile;
            if (window_) SDL_StopTextInput(window_);
            focusedId_ = "missions";
            settingsSaved_ = true;
            clampProfileScroll();
            showNotice(tr("Profile created."));
        } else {
            showNotice(tr(errKey));
        }
        return true;
    }
    if (id == "profile_rename") {
        if (profileEditMode_ != ProfileEditMode::Rename) {
            profileEditMode_ = ProfileEditMode::Rename;
            profileNameDraft_ = settings_.activeProfile;
            focusedId_ = "profile_name_field";
            if (window_) SDL_StartTextInput(window_);
            dirty_ = true;
            return true;
        }
        std::string errKey;
        if (renameActiveProfile(settings_, profileNameDraft_, &errKey)) {
            profileEditMode_ = ProfileEditMode::None;
            profileNameDraft_ = settings_.activeProfile;
            if (window_) SDL_StopTextInput(window_);
            focusedId_ = "missions";
            settingsSaved_ = true;
            showNotice(tr("Profile renamed."));
        } else {
            showNotice(tr(errKey));
        }
        return true;
    }
    if (id == "profile_delete") {
        if (profileEditMode_ != ProfileEditMode::None) {
            profileEditMode_ = ProfileEditMode::None;
            if (window_) SDL_StopTextInput(window_);
        }
        std::string errKey;
        if (deleteActiveProfile(settings_, &errKey)) {
            profileNameDraft_ = settings_.activeProfile;
            settingsSaved_ = true;
            clampProfileScroll();
            showNotice(tr("Profile deleted."));
        } else {
            showNotice(tr(errKey));
        }
        return true;
    }
    if (profileEditMode_ != ProfileEditMode::None) {
        profileEditMode_ = ProfileEditMode::None;
        profileNameDraft_ = settings_.activeProfile;
        if (window_) SDL_StopTextInput(window_);
        dirty_ = true;
    }
    return false;
}

bool GoldFrontEnd::handleSinglePlayerEditKey(SDL_Keycode key) {
    if (key == SDLK_BACKSPACE) {
        while (!profileNameDraft_.empty() &&
               (static_cast<unsigned char>(profileNameDraft_.back()) & 0xc0u) == 0x80u) {
            profileNameDraft_.pop_back();
        }
        if (!profileNameDraft_.empty()) profileNameDraft_.pop_back();
        dirty_ = true;
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        activateId(profileEditMode_ == ProfileEditMode::New ? "profile_new" : "profile_rename");
        return true;
    }
    if ((key >= SDLK_A && key <= SDLK_Z) || (key >= SDLK_0 && key <= SDLK_9) || key == SDLK_SPACE) {
        return true;
    }
    return false;
}

void GoldFrontEnd::handleSinglePlayerKey(SDL_Keycode key) {
    if (key == SDLK_UP || key == SDLK_LEFT) {
        cycleFocus({"campaigns", "missions", "load", "profile_new", "profile_rename", "profile_delete", "cancel"}, -1);
    } else if (key == SDLK_DOWN || key == SDLK_RIGHT || key == SDLK_TAB) {
        cycleFocus({"campaigns", "missions", "load", "profile_new", "profile_rename", "profile_delete", "cancel"}, 1);
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
        activateId(focusedId_);
    }
}

void GoldFrontEnd::handleCampaignsKey(SDL_Keycode key) {
    if (fe_.screen() == FrontEnd::Screen::CustomCampaigns) {
        std::vector<std::string> order;
        order.reserve(customCampaigns_.size() + 1);
        for (std::size_t i = 0; i < customCampaigns_.size(); ++i) {
            order.push_back("ccampaign:" + std::to_string(i));
        }
        order.push_back("cancel");
        if (key == SDLK_UP) cycleFocus(order, -1);
        else if (key == SDLK_DOWN) cycleFocus(order, 1);
        else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
            if (focusedId_ == "cancel" && !customCampaigns_.empty()) {
                focusedId_ = "ccampaign:" + std::to_string(fe_.selectedCustomCampaign());
            } else {
                focusedId_ = "cancel";
            }
            dirty_ = true;
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            activateId(focusedId_.empty() ? "cancel" : focusedId_);
        }
        return;
    }

    std::vector<std::string> order;
    order.reserve(campaigns_.size() + 2);
    for (std::size_t i = 0; i < campaigns_.size(); ++i) {
        order.push_back("campaign:" + std::to_string(i));
    }
    order.push_back("custom");
    order.push_back("cancel");
    if (key == SDLK_UP) cycleFocus(order, -1);
    else if (key == SDLK_DOWN) cycleFocus(order, 1);
    else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
        if (focusedId_ == "cancel" && !campaigns_.empty()) {
            focusedId_ = "campaign:" + std::to_string(fe_.selectedCampaign());
        } else {
            focusedId_ = "cancel";
        }
        dirty_ = true;
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
        activateId(focusedId_);
    }
}

void GoldFrontEnd::handleMissionBrowserKey(SDL_Keycode key) {
    handleActiveListNavigationKey(key);
    if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
        const int step = key == SDLK_LEFT ? -1 : 1;
        if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
            cycleFocus({"start", "difficulty", "cancel"}, step);
        } else {
            cycleFocus({"start", "cancel"}, step);
        }
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (focusedId_ == "cancel" ||
            (fe_.screen() == FrontEnd::Screen::CampaignMissions && focusedId_ == "difficulty")) {
            activateId(focusedId_);
        } else {
            activateId("start");
        }
    }
}

void GoldFrontEnd::handleBriefingKey(SDL_Keycode key) {
    if (key == SDLK_UP) {
        briefScroll_ = std::max(0.0, briefScroll_ - 28.0);
        dirty_ = true;
    } else if (key == SDLK_DOWN) {
        briefScroll_ += 28.0;
        dirty_ = true;
    } else if (key == SDLK_0) {
        if (hasBriefingMedia()) activateId("brief_restart");
    } else if (key >= SDLK_1 && key <= SDLK_4) {
        const std::size_t part = static_cast<std::size_t>(key - SDLK_1 + 1);
        if (part <= briefingPartCount()) {
            activateId("slot_" + std::to_string(part));
        }
    } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) {
        if (hasBriefingMedia()) {
            cycleFocus({"brief_restart", "brief_pause", "start", "cancel"},
                       key == SDLK_LEFT ? -1 : 1);
        } else {
            cycleFocus({"start", "cancel"}, 1);
        }
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
        activateId(focusedId_.empty() ? "start" : focusedId_);
    }
}

void GoldFrontEnd::handleSinglePlayerTextInput(std::string_view txt) {
    for (unsigned char uc : txt) {
        if (uc < 0x20u || uc == 0x7fu || uc == '=' || uc == ':' || uc == ',' ||
            uc == ';' || uc == '|' || uc == '/' || uc == '\\') {
            continue;
        }
        if (profileNameDraft_.size() < 24u) {
            profileNameDraft_.push_back(char(uc));
        }
    }
    dirty_ = true;
}

void GoldFrontEnd::renderSinglePlayer(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    ensureProfilesInitialized(settings_);
    drawHeading(cr, pangoCtx, tr("Single Player"), {310, 28, 180, 34}, 17.0);

    auto nameLbl = makeTextLayout(pangoCtx, tr("Player name:"), 14.0, false);
    cairo_move_to(cr, 122, 72);
    cairo_set_source_rgb(cr, 236.0 / 255.0, 232.0 / 255.0, 212.0 / 255.0);
    pango_cairo_show_layout(cr, nameLbl.get());
    const bool nameEditing = (profileEditMode_ != ProfileEditMode::None);
    const bool nameHov = (hoveredId_ == "profile_name_field");
    drawBronzeBox(cr, kProfileNameRect.x, kProfileNameRect.y, kProfileNameRect.w, kProfileNameRect.h,
                  nameEditing ? 0.96 : nameHov ? 0.91 : 0.86);
    if (nameEditing) {
        cairo_set_source_rgba(cr, 212.0 / 255.0, 198.0 / 255.0, 118.0 / 255.0, 0.92);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, kProfileNameRect.x + 1.0, kProfileNameRect.y + 1.0,
                        kProfileNameRect.w - 2.0, kProfileNameRect.h - 2.0);
        cairo_stroke(cr);
    }
    const bool draftEmpty = profileNameDraft_.empty();
    const std::string shownName = draftEmpty
        ? (nameEditing ? "_" : tr("Enter player name"))
        : (nameEditing ? (profileNameDraft_ + "_") : profileNameDraft_);
    auto prof = makeTextLayout(pangoCtx, shownName, 13.5, !draftEmpty);
    pango_layout_set_width(prof.get(), int((kProfileNameRect.w - 16.0) * PANGO_SCALE));
    pango_layout_set_ellipsize(prof.get(), PANGO_ELLIPSIZE_START);
    cairo_move_to(cr, kProfileNameRect.x + 8.0, kProfileNameRect.y + 6.0);
    if (draftEmpty && !nameEditing) cairo_set_source_rgb(cr, 165.0 / 255.0, 165.0 / 255.0, 148.0 / 255.0);
    else if (nameEditing) cairo_set_source_rgb(cr, 252.0 / 255.0, 240.0 / 255.0, 182.0 / 255.0);
    else cairo_set_source_rgb(cr, 240.0 / 255.0, 240.0 / 255.0, 230.0 / 255.0);
    pango_cairo_show_layout(cr, prof.get());

    auto listLbl = makeTextLayout(pangoCtx, tr("Player list:"), 14.0, false);
    cairo_move_to(cr, 540, 72);
    cairo_set_source_rgb(cr, 236.0 / 255.0, 232.0 / 255.0, 212.0 / 255.0);
    pango_cairo_show_layout(cr, listLbl.get());
    drawBronzeBox(cr, kProfileListRect.x, kProfileListRect.y, kProfileListRect.w, kProfileListRect.h, 0.86);
    drawGoldScrollbar(env, 720, 95, 18, 362, kVisibleProfileRows,
                      settings_.profiles.size() > std::size_t(kVisibleProfileRows),
                      settings_.profiles.size(), profileScroll_,
                      hoveredId_ == "profile_scroll_up", hoveredId_ == "profile_scroll_down", false);

    cairo_save(cr);
    cairo_rectangle(cr, kProfileListRect.x + 4, kProfileListRect.y + 4,
                    kProfileListRect.w - 8, kProfileListRect.h - 8);
    cairo_clip(cr);
    const int profCount = int(settings_.profiles.size());
    const int maxOff = std::max(0, profCount - kVisibleProfileRows);
    profileScroll_ = std::clamp(profileScroll_, 0, maxOff);
    const int endIdx = std::min(profCount, profileScroll_ + kVisibleProfileRows);
    for (int idx = profileScroll_; idx < endIdx; ++idx) {
        const int row = idx - profileScroll_;
        const double ry = kProfileListRect.y + 6.0 + row * kRowH;
        const auto& p = settings_.profiles[std::size_t(idx)];
        const bool isActive = profileNameEquals(p.name, settings_.activeProfile);
        const bool isHov = (hoveredId_ == "profile_row:" + std::to_string(idx));
        if (isActive) {
            cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.38);
            cairo_rectangle(cr, kProfileListRect.x + 5, ry, kProfileListRect.w - 10, kRowH - 1.0);
            cairo_fill(cr);
        } else if (isHov) {
            cairo_set_source_rgba(cr, 72.0 / 255.0, 96.0 / 255.0, 56.0 / 255.0, 0.22);
            cairo_rectangle(cr, kProfileListRect.x + 5, ry, kProfileListRect.w - 10, kRowH - 1.0);
            cairo_fill(cr);
        }
        std::string label = p.name;
        if (!p.completedMissions.empty()) {
            label += "  (" + std::to_string(p.completedMissions.size()) + " [\xE2\x9C\x93])";
        }
        auto item = makeTextLayout(pangoCtx, label, 13.0, isActive);
        pango_layout_set_width(item.get(), int((kProfileListRect.w - 20.0) * PANGO_SCALE));
        pango_layout_set_ellipsize(item.get(), PANGO_ELLIPSIZE_END);
        cairo_move_to(cr, kProfileListRect.x + 10.0, ry + 2.0);
        if (isActive) cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
        else if (isHov) cairo_set_source_rgb(cr, 128.0 / 255.0, 224.0 / 255.0, 138.0 / 255.0);
        else cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
        pango_cairo_show_layout(cr, item.get());
    }
    cairo_restore(cr);

    for (const auto& b : kSinglePlayerButtons) {
        const bool small = (std::string_view(b.id).rfind("profile_", 0) == 0);
        const bool activeButton =
            (profileEditMode_ == ProfileEditMode::New && std::string_view(b.id) == "profile_new") ||
            (profileEditMode_ == ProfileEditMode::Rename && std::string_view(b.id) == "profile_rename");
        drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                              hoveredId_ == b.id, focusedId_ == b.id || activeButton, b.enabled,
                              small ? 17.5 : 24.0, small);
    }
}

void GoldFrontEnd::renderCampaigns(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    const bool isCustom = (fe_.screen() == FrontEnd::Screen::CustomCampaigns);
    const auto& list = isCustom ? customCampaigns_ : campaigns_;
    const char* btnPrefix = isCustom ? "ccampaign:" : "campaign:";
    drawHeading(cr, pangoCtx, tr(isCustom ? "Custom Campaigns" : "Campaigns"), {240, 28, 320, 34}, 17.0);

    if (list.empty()) {
        drawBronzeBox(cr, 43, 134, 714, 148, 0.88);
        auto emptyLbl = makeTextLayout(pangoCtx,
                                       tr(isCustom ? "No custom campaigns installed." : "No campaigns installed."),
                                       14.5, true);
        cairo_move_to(cr, 64, 166);
        cairo_set_source_rgb(cr, 232.0 / 255.0, 224.0 / 255.0, 188.0 / 255.0);
        pango_cairo_show_layout(cr, emptyLbl.get());
        if (isCustom) {
            auto hintLbl = makeTextLayout(pangoCtx,
                "maps/custom_campaign/<CampaignName>/   |   ~/.local/share/ss2/custom_campaigns/",
                12.5, false, 670.0);
            cairo_move_to(cr, 64, 206);
            cairo_set_source_rgb(cr, 180.0 / 255.0, 188.0 / 255.0, 165.0 / 255.0);
            pango_cairo_show_layout(cr, hintLbl.get());
        }
    } else {
        const std::size_t pIdx = previewCampaignIndex(isCustom);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string cid = std::string(btnPrefix) + std::to_string(i);
            const RectD r = campaignRectFor(i, list.size());
            if (isCustom) {
                drawGoldStencilButton(cr, pangoCtx, r, list[i].name, '\0',
                                      hoveredId_ == cid, focusedId_ == cid || i == pIdx, true, 19.0, true);
            } else {
                const auto disp = campaignButtonDisplay(list[i].name);
                drawGoldStencilButton(cr, pangoCtx, r, disp.label, disp.shortcut,
                                      hoveredId_ == cid, focusedId_ == cid || i == pIdx, true, 24.0);
            }
        }

        const auto& camp = list[pIdx];
        drawHeading(cr, pangoCtx, camp.name, {420, 74, 348, 26}, 15.5);
        drawBronzeBox(cr, 420, 104, 348, 384, 0.86);

        std::size_t done = 0;
        std::size_t firstUncompleted = 0;
        bool foundUncompleted = false;
        for (std::size_t mi = 0; mi < camp.missions.size(); ++mi) {
            if (camp.missions[mi].completed) ++done;
            else if (!foundUncompleted) {
                firstUncompleted = mi;
                foundUncompleted = true;
            }
        }

        const std::string summaryStr =
            tr("Missions:") + " " + std::to_string(camp.missions.size()) + "   |   " +
            std::to_string(done) + "/" + std::to_string(camp.missions.size()) + " " + tr("Completed");
        auto sumLayout = makeTextLayout(pangoCtx, summaryStr, 12.5, true);
        cairo_move_to(cr, 434, 114);
        cairo_set_source_rgb(cr, 232.0 / 255.0, 224.0 / 255.0, 188.0 / 255.0);
        pango_cairo_show_layout(cr, sumLayout.get());

        const std::size_t maxPreviewRows = std::min<std::size_t>(camp.missions.size(), 8);
        for (std::size_t mi = 0; mi < maxPreviewRows; ++mi) {
            const double ry = 138.0 + double(mi) * 19.0;
            const bool highlightNext = (mi == firstUncompleted);
            if (highlightNext) {
                cairo_set_source_rgba(cr, 98.0 / 255.0, 114.0 / 255.0, 72.0 / 255.0, 0.34);
                cairo_rectangle(cr, 430, ry, 328, 18.0);
                cairo_fill(cr);
            }
            std::string mLabel = camp.missions[mi].name;
            if (camp.missions[mi].completed) mLabel += "  [\xE2\x9C\x93]";
            auto ml = makeTextLayout(pangoCtx, mLabel, 12.5, highlightNext);
            pango_layout_set_width(ml.get(), int(316.0 * PANGO_SCALE));
            pango_layout_set_ellipsize(ml.get(), PANGO_ELLIPSIZE_END);
            cairo_move_to(cr, 434, ry + 1.0);
            if (highlightNext) cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
            else cairo_set_source_rgb(cr, 49.0 / 255.0, 174.0 / 255.0, 74.0 / 255.0);
            pango_cairo_show_layout(cr, ml.get());
        }

        if (!camp.missions.empty()) {
            const auto& pm = camp.missions[firstUncompleted];
            ensureOverviewLoaded(pm.path);
            cairo_surface_t* overview = nullptr;
            if (const auto it = assets_->overviews.find(pm.path); it != assets_->overviews.end()) {
                overview = it->second.get();
            }
            drawDiamondOverview(cr, overview, 482.0, 304.0, 224.0, 117.0, true);

            std::string sizeStr = tr("Size:") + "  " + std::to_string(pm.width) + "x" + std::to_string(pm.height);
            if (pm.scheme == 2) sizeStr += "  (Beach)";
            else if (pm.width > 0) sizeStr += "  (Summer)";
            drawHeading(cr, pangoCtx, sizeStr, {434, 430, 320, 20}, 13.5);
        }

        const std::string diffStr = tr("Difficulty") + ": " + tr(difficultyKey(fe_.difficulty()));
        drawHeading(cr, pangoCtx, diffStr, {434, 456, 320, 18}, 12.5);
    }

    if (isCustom) {
        drawGoldStencilButton(cr, pangoCtx, kCustomCancelRect, tr("Cancel"), '\0',
                              hoveredId_ == "cancel", focusedId_ == "cancel", true, 24.0);
    } else {
        for (const auto& b : kCampaignFooterButtons) {
            drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                  hoveredId_ == b.id, focusedId_ == b.id, b.enabled, 24.0);
        }
    }
}

void GoldFrontEnd::renderMissionBrowserScreen(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    auto renderMissionBrowser = [&](const std::string& heading, auto rowAt, std::size_t rowCount,
                                    std::size_t selectedRow, const char* rowPrefix, bool showDifficulty) {
        drawHeading(cr, pangoCtx, heading, {200, 38, 400, 28}, 17.0);
        drawHeading(cr, pangoCtx, tr("Missions:"), {130, 110, 150, 20}, 15.0);
        drawHeading(cr, pangoCtx, tr("Description:"), {520, 110, 144, 20}, 15.0);

        drawBronzeBox(cr, kListX, kListY, kListW, kListH, 0.88);
        cairo_save(cr);
        cairo_rectangle(cr, kListX + 4, kListY + 4, kListW - 8, kListH - 8);
        cairo_clip(cr);

        if (rowCount == 0) {
            auto emptyLbl = makeTextLayout(pangoCtx, tr("No missions installed."), 14.0, false);
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

                const auto& m = rowAt(std::size_t(idx));
                std::string label = m.name;
                if (m.completed) label += "  [\xE2\x9C\x93]";
                auto rowLayout = makeTextLayout(pangoCtx, label, 13.0, isSelected);
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

        if (selectedRow < rowCount) {
            const auto& m = rowAt(selectedRow);
            cairo_save(cr);
            cairo_set_source_rgba(cr, 10.0 / 255.0, 14.0 / 255.0, 7.0 / 255.0, 0.42);
            cairo_rectangle(cr, 438, 136, 330, 186);
            cairo_fill(cr);
            cairo_restore(cr);
            {
                const auto paras = assets::splitBriefingParagraphs(m.description);
                const std::string& previewDesc = paras.empty() ? m.description : paras[0];
                auto desc = makeTextLayout(pangoCtx, previewDesc, 13.5, false, 316.0);
                pango_layout_set_height(desc.get(), int(172.0 * PANGO_SCALE));
                pango_layout_set_ellipsize(desc.get(), PANGO_ELLIPSIZE_END);
                cairo_move_to(cr, 445, 142);
                cairo_set_source_rgb(cr, 238.0 / 255.0, 240.0 / 255.0, 228.0 / 255.0);
                pango_cairo_show_layout(cr, desc.get());
            }

            ensureOverviewLoaded(m.path);
            cairo_surface_t* overview = nullptr;
            if (const auto it = assets_->overviews.find(m.path); it != assets_->overviews.end()) {
                overview = it->second.get();
            }
            drawDiamondOverview(cr, overview, 492.0, 338.0, 224.0, 117.0, true);

            std::string sizeStr = tr("Size:") + "  " + std::to_string(m.width) + "x" + std::to_string(m.height);
            if (m.scheme == 2) sizeStr += "  (Beach)";
            else if (m.width > 0) sizeStr += "  (Summer)";
            drawHeading(cr, pangoCtx, sizeStr, {464, 466, 280, 20}, 14.5);
            if (m.highScore > 0) {
                drawHeading(cr, pangoCtx, "Score: " + std::to_string(m.highScore), {464, 486, 280, 18}, 12.5);
            }
        }

        if (showDifficulty) {
            const bool hov = (hoveredId_ == "difficulty");
            const bool foc = (focusedId_ == "difficulty");
            drawBronzeBox(cr, kDifficultyButtonRect.x, kDifficultyButtonRect.y,
                          kDifficultyButtonRect.w, kDifficultyButtonRect.h,
                          (hov || foc) ? 0.92 : 0.84);
            const std::string diffLabel = tr("Difficulty") + ": " + tr(difficultyKey(fe_.difficulty()));
            drawGoldStencilButton(cr, pangoCtx, kDifficultyButtonRect, diffLabel, 'D',
                                  hov, foc, true, 15.5, true);
        }

        for (const auto& b : kSingleMissionButtons) {
            const bool enabled = std::string_view(b.id) == "start" ? (rowCount > 0) : b.enabled;
            drawGoldStencilButton(cr, pangoCtx, b.rect, tr(b.enLabel), b.shortcut,
                                  hoveredId_ == b.id, focusedId_ == b.id, enabled, 24.0);
        }
    };

    if (fe_.screen() == FrontEnd::Screen::CampaignMissions) {
        const auto& active = activeCampaignList();
        const std::size_t ci = activeCampaignIndex();
        static const std::vector<GoldCampaignMissionEntry> kEmptyCampaignMissions;
        const auto& campName = ci < active.size()
            ? active[ci].name
            : tr(fe_.inCustomCampaign() ? "Custom Campaigns" : "Campaigns");
        const auto& cMissions = ci < active.size() ? active[ci].missions : kEmptyCampaignMissions;
        renderMissionBrowser(campName, [&](std::size_t i) -> const auto& { return cMissions[i]; },
                             cMissions.size(), fe_.selectedCampaignMission(), "cmission:", true);
    } else {
        renderMissionBrowser(tr("Single Player"), [&](std::size_t i) -> const auto& { return missions_[i]; },
                             missions_.size(), fe_.selected(), "mission:", false);
    }
}

void GoldFrontEnd::renderBriefing(const RenderEnv& env) {
    cairo_t* cr = env.cr;
    PangoContext* pangoCtx = env.pangoCtx;

    if (assets_->briefingBg) drawSurfaceStretched(cr, assets_->briefingBg.get(), 0, 0, kStageW, kStageH);
    else drawFallbackBackdrop(cr, true);

    drawBronzeBox(cr, kBriefBoxX, kBriefBoxY, kBriefBoxW, kBriefBoxH, 0.82);
    {
        const double padX = 14.0, padY = 10.0;
        const double innerW = kBriefBoxW - 2.0 * padX;
        const double innerH = kBriefBoxH - 2.0 * padY;
        auto layout = makeTextLayout(pangoCtx, briefingDisplayedText(), 13.5, false, innerW);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(layout.get(), &tw, &th);
        const double maxScroll = std::max(0.0, th - innerH);
        briefScroll_ = std::clamp(briefScroll_, 0.0, maxScroll);

        cairo_save(cr);
        cairo_rectangle(cr, kBriefBoxX + padX, kBriefBoxY + padY, innerW, innerH);
        cairo_clip(cr);
        cairo_move_to(cr, kBriefBoxX + padX, kBriefBoxY + padY - briefScroll_);
        cairo_set_source_rgb(cr, 245.0 / 255.0, 248.0 / 255.0, 240.0 / 255.0);
        pango_cairo_show_layout(cr, layout.get());
        cairo_restore(cr);
    }

    const std::string overviewPath = fe_.launchPath();
    ensureOverviewLoaded(overviewPath);
    cairo_surface_t* overview = nullptr;
    if (const auto it = assets_->overviews.find(overviewPath); it != assets_->overviews.end()) {
        overview = it->second.get();
    }
    if (assets_->briefMedia.hasAnimation()) {
        renderBriefingOverlayIfNeeded();
        constexpr double kDx = 15.0, kDy = 187.0, kDw = 764.0, kDh = 380.0;
        const double cx = kDx + kDw * 0.5;
        const double cy = kDy + kDh * 0.5;
        const bool drawStroke = (assets_->nbramkaFrame.surface == nullptr);
        cairo_save(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, cx, kDy);
        cairo_line_to(cr, kDx + kDw, cy);
        cairo_line_to(cr, cx, kDy + kDh);
        cairo_line_to(cr, kDx, cy);
        cairo_close_path(cr);
        if (drawStroke) {
            cairo_save(cr);
            cairo_clip_preserve(cr);
        } else {
            cairo_clip(cr);
        }
        cairo_set_source_rgb(cr, 8.0 / 255.0, 10.0 / 255.0, 6.0 / 255.0);
        cairo_paint(cr);
        if (overview) {
            drawSurfaceStretched(cr, overview, 12.0, 184.0,
                                 double(assets::kBriefingOverlayWidth),
                                 double(assets::kBriefingOverlayHeight));
        }
        if (assets_->briefOverlay) {
            cairo_set_source_surface(cr, assets_->briefOverlay.get(), 12.0, 184.0);
            cairo_paint(cr);
        }
        if (drawStroke) {
            cairo_restore(cr);
            cairo_set_line_width(cr, 1.8);
            cairo_set_source_rgb(cr, 170.0 / 255.0, 162.0 / 255.0, 134.0 / 255.0);
            cairo_stroke(cr);
        } else {
            cairo_restore(cr);
        }
    } else {
        drawDiamondOverview(cr, overview, 15.0, 187.0, 764.0, 380.0,
                            assets_->nbramkaFrame.surface == nullptr);
    }

    if (assets_->nbramkaFrame.surface) {
        cairo_save(cr);
        cairo_set_source_surface(cr, assets_->nbramkaFrame.surface.get(),
                                 assets_->nbramkaFrame.x, assets_->nbramkaFrame.y);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    const std::size_t partCount = briefingPartCount();
    for (std::size_t s = 0; s < 4; ++s) {
        const std::string slotId = "slot_" + std::to_string(s + 1);
        const bool available = (s < partCount);
        const bool selected = available && (briefActivePart_ == int(s + 1));
        const bool hovered = available && (hoveredId_ == slotId);
        const std::size_t wantedOff = s * 3 + (selected ? 2u : (hovered ? 1u : 0u));
        const auto* slot = &assets_->slotButtons[wantedOff];
        if (!slot->surface) slot = &assets_->slotButtons[s * 3 + (hovered ? 1u : 0u)];
        if (!slot->surface) slot = &assets_->slotButtons[s * 3];
        if (slot->surface) {
            cairo_set_source_surface(cr, slot->surface.get(), slot->x, slot->y);
            if (available) cairo_paint(cr);
            else cairo_paint_with_alpha(cr, 0.35);
            if (selected) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, 250.0 / 255.0, 212.0 / 255.0, 112.0 / 255.0, 0.90);
                cairo_set_line_width(cr, 2.0);
                cairo_rectangle(cr, slot->x - 1.0, slot->y - 1.0, slot->w + 2.0, slot->h + 2.0);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
        }
    }

    drawHeading(cr, pangoCtx, tr("Mission Briefing"), {565, 161, 192, 26}, 16.0);
    const bool mediaAvail = hasBriefingMedia();
    for (const auto& b : kBriefingButtons) {
        const bool isPause = (std::string_view(b.id) == "brief_pause");
        const bool isRestart = (std::string_view(b.id) == "brief_restart");
        const bool enabled = (isPause || isRestart) ? mediaAvail : b.enabled;
        const char* labelKey = (isPause && briefPaused_) ? "Resume" : b.enLabel;
        drawGoldStencilButton(cr, pangoCtx, b.rect, tr(labelKey), b.shortcut,
                              hoveredId_ == b.id, focusedId_ == b.id, enabled, 22.5);
    }
}

}  // namespace ss2
