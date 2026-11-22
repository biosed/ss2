#include "ui/gold_frontend_internal.hpp"

#include "audio/sound_player.hpp"
#include "render/window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ss2 {
namespace {

using namespace gold_ui;

// Retail n2Menu_dll.dll (0x10008568..0x1000857b) streams menu_common.sue\winwav and
// loosewav as raw headerless 22,050 Hz 16-bit signed stereo PCM.
constexpr int kRetailFanfareSampleRate = 22050;
constexpr int kRetailFanfareChannels = 2;

std::string formatDurationMmSs(unsigned totalSeconds) {
    const unsigned mins = totalSeconds / 60u;
    const unsigned secs = totalSeconds % 60u;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u", mins, secs);
    return std::string(buf);
}

void drawSurfaceNearest(cairo_t* cr, cairo_surface_t* surf, double x, double y, double w, double h) {
    if (!surf) return;
    const int sw = cairo_image_surface_get_width(surf);
    const int sh = cairo_image_surface_get_height(surf);
    if (sw <= 0 || sh <= 0) return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, w / double(sw), h / double(sh));
    cairo_set_source_surface(cr, surf, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
}

void drawStarPolygon(cairo_t* cr, double cx, double cy, double outerR, double innerR,
                     bool earned, bool goldTier) {
    cairo_save(cr);
    cairo_new_path(cr);
    constexpr double kPi = std::numbers::pi;
    for (int i = 0; i < 10; ++i) {
        const double angle = -kPi * 0.5 + double(i) * (kPi / 5.0);
        const double r = (i % 2 == 0) ? outerR : innerR;
        const double px = cx + std::cos(angle) * r;
        const double py = cy + std::sin(angle) * r;
        if (i == 0) cairo_move_to(cr, px, py);
        else cairo_line_to(cr, px, py);
    }
    cairo_close_path(cr);

    if (earned) {
        if (goldTier) {
            cairo_pattern_t* glow = cairo_pattern_create_radial(cx, cy, 1.5, cx, cy, outerR * 1.45);
            cairo_pattern_add_color_stop_rgba(glow, 0.0, 1.0, 0.92, 0.42, 0.55);
            cairo_pattern_add_color_stop_rgba(glow, 1.0, 1.0, 0.80, 0.18, 0.0);
            cairo_save(cr);
            cairo_set_source(cr, glow);
            cairo_arc(cr, cx, cy, outerR * 1.45, 0.0, 2.0 * kPi);
            cairo_fill(cr);
            cairo_restore(cr);
            cairo_pattern_destroy(glow);
        }
        cairo_pattern_t* grad = cairo_pattern_create_linear(cx, cy - outerR, cx, cy + outerR);
        if (goldTier) {
            cairo_pattern_add_color_stop_rgb(grad, 0.0, 1.00, 0.96, 0.62);
            cairo_pattern_add_color_stop_rgb(grad, 0.5, 0.96, 0.78, 0.24);
            cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.72, 0.48, 0.10);
        } else {
            cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.94, 0.84, 0.56);
            cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.66, 0.52, 0.24);
        }
        cairo_set_source(cr, grad);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(grad);
        cairo_set_line_width(cr, 1.1);
        cairo_set_source_rgba(cr, 0.16, 0.12, 0.04, 0.92);
        cairo_stroke(cr);
    } else {
        cairo_set_source_rgba(cr, 0.20, 0.22, 0.18, 0.55);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, 0.46, 0.44, 0.36, 0.55);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void drawFallbackCategoryGlyph(cairo_t* cr, int categoryIndex, const RectD& box) {
    const double cx = box.x + box.w * 0.5;
    const double cy = box.y + box.h * 0.5;
    cairo_save(cr);
    cairo_set_source_rgba(cr, 194.0 / 255.0, 186.0 / 255.0, 146.0 / 255.0, 0.86);
    cairo_set_line_width(cr, 1.8);
    switch (categoryIndex) {
        case 0:  // Infantry
            cairo_arc(cr, cx, cy - 8.0, 4.0, 0.0, 2.0 * std::numbers::pi);
            cairo_fill(cr);
            cairo_rectangle(cr, cx - 4.5, cy - 3.0, 9.0, 12.0);
            cairo_fill(cr);
            break;
        case 1:  // Tanks
            cairo_rectangle(cr, cx - 16.0, cy + 1.0, 32.0, 8.0);
            cairo_fill(cr);
            cairo_rectangle(cr, cx - 9.0, cy - 6.0, 16.0, 7.0);
            cairo_fill(cr);
            cairo_move_to(cr, cx + 7.0, cy - 3.0);
            cairo_line_to(cr, cx + 19.0, cy - 3.0);
            cairo_stroke(cr);
            break;
        case 2:  // Vehicles
            cairo_rectangle(cr, cx - 15.0, cy - 4.0, 20.0, 10.0);
            cairo_fill(cr);
            cairo_rectangle(cr, cx + 5.0, cy - 1.0, 9.0, 7.0);
            cairo_fill(cr);
            break;
        case 3:  // Artillery
            cairo_arc(cr, cx - 4.0, cy + 4.0, 5.0, 0.0, 2.0 * std::numbers::pi);
            cairo_stroke(cr);
            cairo_move_to(cr, cx - 14.0, cy + 7.0);
            cairo_line_to(cr, cx - 4.0, cy + 4.0);
            cairo_line_to(cr, cx + 16.0, cy - 5.0);
            cairo_stroke(cr);
            break;
        case 4:  // Aircraft
            cairo_move_to(cr, cx - 16.0, cy);
            cairo_line_to(cr, cx + 16.0, cy);
            cairo_move_to(cr, cx - 2.0, cy - 10.0);
            cairo_line_to(cr, cx + 4.0, cy + 10.0);
            cairo_stroke(cr);
            break;
        default:  // Buildings
            cairo_rectangle(cr, cx - 11.0, cy - 3.0, 22.0, 12.0);
            cairo_stroke(cr);
            cairo_move_to(cr, cx - 13.0, cy - 3.0);
            cairo_line_to(cr, cx, cy - 11.0);
            cairo_line_to(cr, cx + 13.0, cy - 3.0);
            cairo_close_path(cr);
            cairo_stroke(cr);
            break;
    }
    cairo_restore(cr);
}

bool parseDecodedFanfare(std::span<const std::uint8_t> raw, int& outFreq, int& outChannels,
                         std::vector<std::uint8_t>& outPcm) {
    outPcm.clear();
    if (raw.size() < 64u) return false;

    // Check RIFF/WAVE header if a modded archive replaces raw PCM with a WAV container.
    if (raw.size() >= 44u && std::memcmp(raw.data(), "RIFF", 4) == 0 &&
        std::memcmp(raw.data() + 8, "WAVE", 4) == 0) {
        bool haveFmt = false;
        std::uint16_t ch = 2;
        std::uint32_t sr = 22050;
        std::uint16_t bits = 16;
        std::size_t pos = 12u;
        while (pos + 8u <= raw.size()) {
            const std::uint32_t csz = audio_detail::readU32(raw.data() + pos + 4u);
            const std::size_t dpos = pos + 8u;
            if (dpos + csz > raw.size()) break;
            if (std::memcmp(raw.data() + pos, "fmt ", 4) == 0 && csz >= 16u) {
                const std::uint16_t fmt = audio_detail::readU16(raw.data() + dpos);
                ch = audio_detail::readU16(raw.data() + dpos + 2u);
                sr = audio_detail::readU32(raw.data() + dpos + 4u);
                bits = audio_detail::readU16(raw.data() + dpos + 14u);
                if (fmt != 1u || bits != 16u || (ch != 1u && ch != 2u) || sr < 8000u || sr > 192000u) {
                    return false;
                }
                haveFmt = true;
            } else if (std::memcmp(raw.data() + pos, "data", 4) == 0 && haveFmt && csz > 0u) {
                outFreq = int(sr);
                outChannels = int(ch);
                outPcm.assign(raw.begin() + std::ptrdiff_t(dpos),
                              raw.begin() + std::ptrdiff_t(dpos + (csz & ~1u)));
                return !outPcm.empty();
            }
            pos = dpos + csz + (csz & 1u);
        }
        return false;
    }

    // Check "2D" OKI ADPCM header if present.
    if (raw[0] == '2' && raw[1] == 'D') {
        const auto samples = assets::decodeApmAudio(raw);
        if (samples.empty()) return false;
        outFreq = assets::kBriefingVoiceSampleRate;
        outChannels = 1;
        outPcm.resize(samples.size() * sizeof(std::int16_t));
        std::memcpy(outPcm.data(), samples.data(), outPcm.size());
        return true;
    }

    // Retail Gold 2.2 winwav / loosewav: raw headerless 22,050 Hz S16LE stereo PCM.
    const std::size_t alignedBytes = raw.size() & ~std::size_t(3u);
    if (alignedBytes == 0u) return false;
    outFreq = kRetailFanfareSampleRate;
    outChannels = kRetailFanfareChannels;
    outPcm.assign(raw.begin(), raw.begin() + std::ptrdiff_t(alignedBytes));
    return true;
}

}  // namespace

struct GoldPostMissionScreen::ActionButton {
    std::string id;
    std::string label;
    char shortcut{'\0'};
    RectD rect{};
};

struct GoldPostMissionScreen::Assets {
    FontMap fontMap{pango_cairo_font_map_new(), g_object_unref};
    FontCtx fontCtx{pango_font_map_create_context(fontMap.get()), g_object_unref};

    std::filesystem::path menuSuePath;
    Surface mainBg = makeNullSurface();
    PckSurface winRusBg;
    PckSurface winJapBg;
    std::array<PckSurface, 9> statPics;       // stat_pic.pck[0..8]
    std::array<RectD, 3> mstatButtonRects{};  // mstat_bt.pck[0,2,4] hit geometry
    std::array<RectD, 3> rsButtonRects{};     // rs_bt.pck[0..2] hit geometry
    std::array<PckSurface, 3> starsRegular;   // ZVEZDY.PCK[0..2]
    std::array<PckSurface, 15> starsGold;     // ZV_GOLD.PCK[0..14]
    std::array<PckSurface, 3> starsSmall;     // ZV_M.PCK[0..2]

    bool loadedRetail{false};
    bool checkedWinWav{false};
    bool hasWinWav{false};
    bool checkedLooseWav{false};
    bool hasLooseWav{false};

    std::vector<std::uint8_t> cachedWinPcm;
    int cachedWinFreq{kRetailFanfareSampleRate};
    int cachedWinChannels{kRetailFanfareChannels};
    std::vector<std::uint8_t> cachedLoosePcm;
    int cachedLooseFreq{kRetailFanfareSampleRate};
    int cachedLooseChannels{kRetailFanfareChannels};

    SDL_AudioDeviceID fanfareDev{0};
    SDL_AudioStream* fanfareStream{nullptr};

    bool ensureFanfareLoaded(bool victory) {
        if (menuSuePath.empty()) return false;
        if (victory) {
            if (!checkedWinWav) {
                checkedWinWav = true;
                try {
                    const auto winBytes = assets::readSueEntry(menuSuePath, "winwav");
                    hasWinWav = parseDecodedFanfare(
                        winBytes, cachedWinFreq, cachedWinChannels, cachedWinPcm);
                } catch (...) {
                    hasWinWav = false;
                }
            }
            return hasWinWav;
        }
        if (!checkedLooseWav) {
            checkedLooseWav = true;
            try {
                const auto looseBytes = assets::readSueEntry(menuSuePath, "loosewav");
                hasLooseWav = parseDecodedFanfare(
                    looseBytes, cachedLooseFreq, cachedLooseChannels, cachedLoosePcm);
            } catch (...) {
                hasLooseWav = false;
            }
        }
        return hasLooseWav;
    }

    ~Assets() {
        if (fanfareStream) {
            SDL_DestroyAudioStream(fanfareStream);
            fanfareStream = nullptr;
        }
        if (fanfareDev) {
            SDL_CloseAudioDevice(fanfareDev);
            fanfareDev = 0;
        }
    }
    MenuCursorAnimator menuCursor;
};

GoldPostMissionScreen::GoldPostMissionScreen(SDL_Window* window,
                                             const std::filesystem::path& gameDir,
                                             std::optional<int> fixedBgIndex)
    : window_(window), gameDir_(gameDir), assets_(std::make_unique<Assets>()) {
    loadAssets(fixedBgIndex);
}

GoldPostMissionScreen::~GoldPostMissionScreen() {
    stopFanfare();
    if (assets_) {
        assets_->menuCursor.restore();
    }
}

bool GoldPostMissionScreen::hasRetailAssets() const {
    return assets_ && assets_->loadedRetail;
}

bool GoldPostMissionScreen::hasMenuCursor() const {
    return assets_ && assets_->menuCursor.loaded();
}

bool GoldPostMissionScreen::hasFanfareAudio() const {
    if (!assets_) return false;
    return assets_->ensureFanfareLoaded(true) && assets_->ensureFanfareLoaded(false);
}

bool GoldPostMissionScreen::fanfarePlaying() const {
    if (!assets_ || !assets_->fanfareDev || !assets_->fanfareStream) return false;
    return SDL_GetAudioStreamAvailable(assets_->fanfareStream) > 0;
}

void GoldPostMissionScreen::loadAssets(std::optional<int> fixedBgIndex) {
    if (gameDir_.empty()) return;
    const auto menuSue = assets::resolveCaseInsensitivePath(gameDir_, "menu_common.sue");
    std::error_code ec;
    if (!std::filesystem::exists(menuSue, ec)) return;
    assets_->menuSuePath = menuSue;

    int chosen = fixedBgIndex.value_or(-1);
    if (chosen < 0 || chosen > 68) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 68);
        chosen = dist(rng);
    }

    auto loadJpegBg = [&](const std::string& entry) -> Surface {
        try {
            return mapImageToSurface(assets::decodeJpeg(assets::readSueEntry(menuSue, entry)));
        } catch (...) {
            return makeNullSurface();
        }
    };

    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "mainbk%03d.jpg", chosen);
    assets_->mainBg = loadJpegBg(nameBuf);
    if (!assets_->mainBg && chosen != 0) {
        assets_->mainBg = loadJpegBg("mainbk000.jpg");
    }

    tryLoadPckSlot(menuSue, "win_rus.pck", 0u, false, assets_->winRusBg);
    tryLoadPckSlot(menuSue, "win_jap.pck", 0u, false, assets_->winJapBg);

    bool statPicsOk = false;
    try {
        const auto raw = assets::readSueEntry(menuSue, "stat_pic.pck");
        const std::size_t count = assets::pckFrameCount(raw);
        if (count >= 9u) {
            statPicsOk = true;
            for (std::size_t i = 0; i < 9u; ++i) {
                assets_->statPics[i] = pckFrameToSurface(assets::decodeUiPckFrame(raw, i), true);
                if (!assets_->statPics[i].surface) statPicsOk = false;
            }
        }
    } catch (...) {
        statPicsOk = false;
    }

    bool mstatBtOk = false;
    try {
        const auto raw = assets::readSueEntry(menuSue, "mstat_bt.pck");
        if (assets::pckFrameCount(raw) >= 6u) {
            for (std::size_t i = 0; i < 3u; ++i) {
                const auto f = assets::decodeUiPckFrame(raw, i * 2u);
                assets_->mstatButtonRects[i] = RectD{double(f.x), double(f.y), double(f.width), double(f.height)};
            }
            mstatBtOk = true;
        }
    } catch (...) {
    }

    try {
        const auto raw = assets::readSueEntry(menuSue, "rs_bt.pck");
        if (assets::pckFrameCount(raw) >= 3u) {
            for (std::size_t i = 0; i < 3u; ++i) {
                const auto f = assets::decodeUiPckFrame(raw, i);
                assets_->rsButtonRects[i] = RectD{double(f.x), double(f.y), double(f.width), double(f.height)};
            }
        }
    } catch (...) {
    }

    bool starsOk = false;
    try {
        const auto rawReg = assets::readSueEntry(menuSue, "ZVEZDY.PCK");
        const auto rawGold = assets::readSueEntry(menuSue, "ZV_GOLD.PCK");
        const auto rawSmall = assets::readSueEntry(menuSue, "ZV_M.PCK");
        if (assets::pckFrameCount(rawReg) >= 3u &&
            assets::pckFrameCount(rawGold) >= 15u &&
            assets::pckFrameCount(rawSmall) >= 3u) {
            starsOk = true;
            for (std::size_t i = 0; i < 3u; ++i) {
                assets_->starsRegular[i] = pckFrameToSurface(assets::decodeUiPckFrame(rawReg, i), true);
                assets_->starsSmall[i] = pckFrameToSurface(assets::decodeUiPckFrame(rawSmall, i), true);
                if (!assets_->starsRegular[i].surface || !assets_->starsSmall[i].surface) starsOk = false;
            }
            for (std::size_t i = 0; i < 15u; ++i) {
                assets_->starsGold[i] = pckFrameToSurface(assets::decodeUiPckFrame(rawGold, i), true);
                if (!assets_->starsGold[i].surface) starsOk = false;
            }
        }
    } catch (...) {
        starsOk = false;
    }

    assets_->loadedRetail =
        (assets_->mainBg != nullptr || assets_->winRusBg.surface != nullptr) &&
        statPicsOk && mstatBtOk && starsOk;
    if (window_ != nullptr) {
        assets_->menuCursor.load(gameDir_);
    }
}

void GoldPostMissionScreen::stopFanfare() {
    if (assets_ && assets_->fanfareStream) {
        SDL_ClearAudioStream(assets_->fanfareStream);
    }
}

void GoldPostMissionScreen::startFanfare(bool victory, float volume) {
    stopFanfare();
    if (!assets_ || !window_) return;
    const float gain = std::clamp(volume, 0.0f, 1.0f);
    if (gain <= 0.001f) return;
    if (!assets_->ensureFanfareLoaded(victory)) return;

    const auto& pcm = victory ? assets_->cachedWinPcm : assets_->cachedLoosePcm;
    const int freq = victory ? assets_->cachedWinFreq : assets_->cachedLooseFreq;
    const int channels = victory ? assets_->cachedWinChannels : assets_->cachedLooseChannels;
    if (pcm.empty() || freq < 8000 || channels < 1) return;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    }
    SDL_AudioSpec dstSpec{};
    dstSpec.format = SDL_AUDIO_S16;
    dstSpec.channels = 2;
    dstSpec.freq = 48000;
    if (!assets_->fanfareDev) {
        assets_->fanfareDev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dstSpec);
        if (!assets_->fanfareDev) return;
    }
    if (!assets_->fanfareStream) {
        assets_->fanfareStream = SDL_CreateAudioStream(&dstSpec, &dstSpec);
        if (!assets_->fanfareStream ||
            !SDL_BindAudioStream(assets_->fanfareDev, assets_->fanfareStream)) {
            if (assets_->fanfareStream) {
                SDL_DestroyAudioStream(assets_->fanfareStream);
                assets_->fanfareStream = nullptr;
            }
            return;
        }
    }
    SDL_AudioSpec srcSpec{};
    srcSpec.format = SDL_AUDIO_S16;
    srcSpec.channels = channels;
    srcSpec.freq = freq;
    SDL_SetAudioStreamFormat(assets_->fanfareStream, &srcSpec, nullptr);
    SDL_SetAudioStreamGain(assets_->fanfareStream, gain * 0.72f);
    SDL_ClearAudioStream(assets_->fanfareStream);
    SDL_ResumeAudioDevice(assets_->fanfareDev);
    (void)SDL_PutAudioStreamData(assets_->fanfareStream, pcm.data(), static_cast<int>(pcm.size()));
}

void GoldPostMissionScreen::open(GoldPostMissionStats stats, bool playFanfare) {
    stats_ = std::move(stats);
    stats_.starRating = std::clamp(stats_.starRating, 0, 10);
    active_ = true;
    hoveredId_.clear();
    const auto ids = actionIds();
    focusedId_ = ids.empty() ? std::string{} : ids.front();
    dirty_ = true;
    if (playFanfare) {
        startFanfare(stats_.victory, stats_.sfxVolume);
    } else {
        stopFanfare();
    }
    if (window_ && assets_) {
        assets_->menuCursor.apply();
    }
}

void GoldPostMissionScreen::close() {
    active_ = false;
    hoveredId_.clear();
    stopFanfare();
    if (assets_) {
        assets_->menuCursor.restore();
    }
    dirty_ = true;
}

std::vector<GoldPostMissionScreen::ActionButton> GoldPostMissionScreen::buildActionButtons() const {
    struct RawAction {
        const char* id;
        std::string label;
        char shortcut;
    };
    std::vector<RawAction> raw;
    raw.reserve(6);
    if (stats_.victory && stats_.hasNextCampaign) {
        raw.push_back({"next_campaign", tr("Next mission"), 'N'});
    }
    if (stats_.canReplay) {
        raw.push_back({"replay", tr("Replay mission"), 'R'});
    }
    if (stats_.canBrowseCampaigns) {
        raw.push_back({"campaigns", tr("Campaigns"), '\0'});
    }
    if (stats_.canBrowseMissions) {
        raw.push_back({"missions", tr("Missions"), 'M'});
    }
    raw.push_back({"resume", tr("Continue"), 'C'});
    raw.push_back({"quit", tr("Quit to desktop"), 'Q'});

    std::vector<ActionButton> out;
    out.reserve(raw.size());
    const std::size_t n = raw.size();
    if (n <= 3u) {
        // Single-row layout aligned with retail mstat_bt.pck button band (y=476..522).
        const double totalW = 712.0;
        const double gap = 18.0;
        const double btnW = (totalW - gap * double(std::max<std::size_t>(1u, n) - 1u)) / double(std::max<std::size_t>(1u, n));
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(ActionButton{
                raw[i].id,
                raw[i].label,
                raw[i].shortcut,
                RectD{44.0 + double(i) * (btnW + gap), 478.0, btnW, 44.0},
            });
        }
        return out;
    }

    const std::size_t row0Count = (n + 1u) / 2u;
    const std::size_t row1Count = n - row0Count;
    auto appendRow = [&](std::size_t startIdx, std::size_t count, double y) {
        constexpr double kStageMarginX = 44.0;
        constexpr double kUsableW = 712.0;
        constexpr double kGap = 14.0;
        const double btnW = (count == 2u)
            ? 248.0
            : (kUsableW - kGap * double(count - 1u)) / double(count);
        const double rowSpan = double(count) * btnW + double(count - 1u) * kGap;
        const double x0 = kStageMarginX + (kUsableW - rowSpan) * 0.5;
        for (std::size_t c = 0; c < count; ++c) {
            const auto& item = raw[startIdx + c];
            out.push_back(ActionButton{
                item.id,
                item.label,
                item.shortcut,
                RectD{x0 + double(c) * (btnW + kGap), y, btnW, 40.0},
            });
        }
    };
    appendRow(0u, row0Count, 468.0);
    appendRow(row0Count, row1Count, 516.0);
    return out;
}

std::vector<std::string> GoldPostMissionScreen::actionIds() const {
    const auto btns = buildActionButtons();
    std::vector<std::string> ids;
    ids.reserve(btns.size());
    for (const auto& b : btns) ids.push_back(b.id);
    return ids;
}

std::optional<std::pair<double, double>> GoldPostMissionScreen::actionStageCenter(std::string_view id) const {
    for (const auto& b : buildActionButtons()) {
        if (b.id == id) {
            return std::pair<double, double>{b.rect.x + b.rect.w * 0.5, b.rect.y + b.rect.h * 0.5};
        }
    }
    return std::nullopt;
}

void GoldPostMissionScreen::setFocusedId(std::string_view id) {
    for (const auto& b : buildActionButtons()) {
        if (b.id == id) {
            if (focusedId_ != b.id) {
                focusedId_ = b.id;
                dirty_ = true;
            }
            return;
        }
    }
}

std::string GoldPostMissionScreen::hitTestStage(double sx, double sy) const {
    for (const auto& b : buildActionButtons()) {
        if (b.rect.contains(sx, sy)) return b.id;
    }
    return {};
}

void GoldPostMissionScreen::cycleFocus(int step) {
    const auto ids = actionIds();
    if (ids.empty()) return;
    const auto it = std::ranges::find(ids, focusedId_);
    const int cur = (it == ids.end()) ? 0 : int(it - ids.begin());
    const int n = int(ids.size());
    const int next = ((cur + step) % n + n) % n;
    if (focusedId_ != ids[std::size_t(next)]) {
        focusedId_ = ids[std::size_t(next)];
        dirty_ = true;
    }
}

std::string GoldPostMissionScreen::handleKey(SDL_Keycode key, bool* outClosed) {
    if (outClosed) *outClosed = false;
    if (!active_) return {};

    if (key == SDLK_ESCAPE) {
        close();
        if (outClosed) *outClosed = true;
        return {};
    }
    if (key == SDLK_UP || key == SDLK_LEFT) {
        cycleFocus(-1);
        return {};
    }
    if (key == SDLK_DOWN || key == SDLK_RIGHT || key == SDLK_TAB) {
        cycleFocus(1);
        return {};
    }
    if (key == SDLK_HOME) {
        const auto ids = actionIds();
        if (!ids.empty() && focusedId_ != ids.front()) {
            focusedId_ = ids.front();
            dirty_ = true;
        }
        return {};
    }
    if (key == SDLK_END) {
        const auto ids = actionIds();
        if (!ids.empty() && focusedId_ != ids.back()) {
            focusedId_ = ids.back();
            dirty_ = true;
        }
        return {};
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
        if (focusedId_.empty()) {
            const auto ids = actionIds();
            if (!ids.empty()) focusedId_ = ids.front();
        }
        return focusedId_;
    }

    for (const auto& b : buildActionButtons()) {
        if (b.shortcut == '\0') continue;
        const auto lower = static_cast<SDL_Keycode>(std::tolower(static_cast<unsigned char>(b.shortcut)));
        if (key == lower) {
            focusedId_ = b.id;
            dirty_ = true;
            return b.id;
        }
    }
    return {};
}

void GoldPostMissionScreen::handleMouseMove(double stageX, double stageY) {
    if (!active_) return;
    const std::string hit = hitTestStage(stageX, stageY);
    if (hit != hoveredId_) {
        hoveredId_ = hit;
        dirty_ = true;
    }
    if (!hit.empty() && hit != focusedId_) {
        focusedId_ = hit;
        dirty_ = true;
    }
}

std::string GoldPostMissionScreen::handleMouseDown(double stageX, double stageY) {
    if (!active_) return {};
    const std::string hit = hitTestStage(stageX, stageY);
    if (hit.empty()) return {};
    hoveredId_ = hit;
    focusedId_ = hit;
    dirty_ = true;
    return hit;
}

void GoldPostMissionScreen::handleMouseUp() {}

GoldPostMissionResult GoldPostMissionScreen::handleEvent(const SDL_Event& event) {
    GoldPostMissionResult res{};
    if (!active_) return res;
    if (window_ && assets_) {
        assets_->menuCursor.apply();
    }

    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        window_ && event.window.windowID == SDL_GetWindowID(window_)) {
        dirty_ = true;
        return res;
    }

    auto toStage = [&](float wx, float wy) -> GoldPoint {
        if (!window_) return GoldPoint{wx, wy, true};
        int winW = 0, winH = 0;
        if (!SDL_GetWindowSize(window_, &winW, &winH)) return {};
        int pw = 0, ph = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &pw, &ph) || pw <= 0 || ph <= 0) {
            pw = lastPixelW_ > 0 ? lastPixelW_ : winW;
            ph = lastPixelH_ > 0 ? lastPixelH_ : winH;
        }
        return GoldFrontEnd::windowToStage(wx, wy, winW, winH, pw, ph);
    };

    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            res.consumed = true;
            if (!event.key.repeat) {
                bool closed = false;
                res.activated = handleKey(event.key.key, &closed);
                res.closed = closed;
            }
            return res;
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_WHEEL:
            res.consumed = true;
            return res;
        case SDL_EVENT_MOUSE_MOTION: {
            res.consumed = true;
            const auto pt = toStage(event.motion.x, event.motion.y);
            if (pt.inside) {
                handleMouseMove(pt.x, pt.y);
            } else if (!hoveredId_.empty()) {
                hoveredId_.clear();
                dirty_ = true;
            }
            return res;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            res.consumed = true;
            if (event.button.button == SDL_BUTTON_LEFT) {
                const auto pt = toStage(event.button.x, event.button.y);
                if (pt.inside) res.activated = handleMouseDown(pt.x, pt.y);
            }
            return res;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            res.consumed = true;
            if (event.button.button == SDL_BUTTON_LEFT) handleMouseUp();
            return res;
        default:
            return res;
    }
}

void GoldPostMissionScreen::renderToBuffer(int pixelW, int pixelH, std::vector<std::uint8_t>& outRgba) {
    const int pw = std::max(1, pixelW);
    const int ph = std::max(1, pixelH);

    Surface canvas(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph), cairo_surface_destroy);
    Context crGuard(cairo_create(canvas.get()), cairo_destroy);
    cairo_t* cr = crGuard.get();
    PangoContext* pangoCtx = assets_->fontCtx.get();

    // Pure black letterbox surround on widescreen swapchains.
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    const GoldStageRect stage = GoldFrontEnd::computeStage(pw, ph);
    cairo_save(cr);
    cairo_translate(cr, stage.x, stage.y);
    cairo_scale(cr, stage.scale, stage.scale);
    cairo_rectangle(cr, 0.0, 0.0, kStageW, kStageH);
    cairo_clip(cr);

    // 1. 800x600 Backdrop: campaign-specific win_rus.pck / win_jap.pck on victory, else mainbk*.jpg.
    if (stats_.victory && stats_.isRussianCampaign && assets_->winRusBg.surface) {
        drawSurfaceStretched(cr, assets_->winRusBg.surface.get(), 0.0, 0.0, kStageW, kStageH);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.30);
        cairo_rectangle(cr, 0.0, 0.0, kStageW, kStageH);
        cairo_fill(cr);
    } else if (stats_.victory && stats_.isJapaneseCampaign && assets_->winJapBg.surface) {
        drawSurfaceStretched(cr, assets_->winJapBg.surface.get(), 0.0, 0.0, kStageW, kStageH);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.30);
        cairo_rectangle(cr, 0.0, 0.0, kStageW, kStageH);
        cairo_fill(cr);
    } else if (assets_->mainBg) {
        drawSurfaceStretched(cr, assets_->mainBg.get(), 0.0, 0.0, kStageW, kStageH);
        if (!stats_.victory) {
            // Subtle cool dusk wash on Defeat to distinguish from Victory at a glance.
            cairo_set_source_rgba(cr, 18.0 / 255.0, 10.0 / 255.0, 10.0 / 255.0, 0.34);
            cairo_rectangle(cr, 0.0, 0.0, kStageW, kStageH);
            cairo_fill(cr);
        }
    } else {
        drawFallbackBackdrop(cr, !stats_.victory);
    }

    // 2. Victory / Defeat Banner Plaque (y = 14..52).
    {
        drawBronzeBox(cr, 150.0, 12.0, 500.0, 38.0, 0.90);
        const std::string banner = toUpperAscii(
            tr(stats_.victory ? "Mission Accomplished!" : "Mission Failed."));
        auto bannerLayout = makeTextLayout(pangoCtx, banner, 22.0, true);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(bannerLayout.get(), &tw, &th);
        const double bx = 400.0 - double(tw) * 0.5;
        const double by = 12.0 + (38.0 - double(th)) * 0.5;

        cairo_save(cr);
        cairo_pattern_t* glow = cairo_pattern_create_radial(400.0, 31.0, 8.0, 400.0, 31.0, 220.0);
        if (stats_.victory) {
            cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.95, 0.78, 0.25, 0.28);
            cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.95, 0.78, 0.25, 0.0);
        } else {
            cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.85, 0.22, 0.16, 0.30);
            cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.85, 0.22, 0.16, 0.0);
        }
        cairo_set_source(cr, glow);
        cairo_rectangle(cr, 152.0, 14.0, 496.0, 34.0);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);

        for (const auto [ox, oy] : std::array<std::pair<double, double>, 5>{
                 {{2.0, 2.0}, {-1.0, 0.0}, {1.0, 0.0}, {0.0, -1.0}, {0.0, 1.0}}}) {
            cairo_move_to(cr, bx + ox, by + oy);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.94);
            pango_cairo_show_layout(cr, bannerLayout.get());
        }

        cairo_pattern_t* grad = cairo_pattern_create_linear(0.0, by, 0.0, by + double(th));
        if (stats_.victory) {
            cairo_pattern_add_color_stop_rgb(grad, 0.0, 1.00, 0.95, 0.68);
            cairo_pattern_add_color_stop_rgb(grad, 0.55, 0.95, 0.78, 0.32);
            cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.72, 0.50, 0.16);
        } else {
            cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.98, 0.62, 0.54);
            cairo_pattern_add_color_stop_rgb(grad, 0.55, 0.86, 0.30, 0.24);
            cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.54, 0.14, 0.12);
        }
        cairo_move_to(cr, bx, by);
        cairo_set_source(cr, grad);
        pango_cairo_show_layout(cr, bannerLayout.get());
        cairo_pattern_destroy(grad);
        cairo_restore(cr);
    }

    // 3. Mission Title & Battle Clock Strip (y = 56..84).
    const std::string timeFormatted = formatDurationMmSs(stats_.elapsedSeconds);
    const std::size_t factionIdx = std::min<std::size_t>(2u, static_cast<std::size_t>(stats_.faction));
    {
        drawBronzeBox(cr, 44.0, 56.0, 712.0, 28.0, 0.88);
        const std::string titleShown = sanitizeUtf8(
            stats_.missionTitle.empty() ? std::string("Tutorial I") : stats_.missionTitle);

        double textLeft = 56.0;
        if (assets_->starsSmall[factionIdx].surface) {
            drawSurfaceNearest(cr, assets_->starsSmall[factionIdx].surface.get(), 55.0, 64.0, 11.0, 11.0);
            textLeft = 72.0;
        }
        auto titleLayout = makeTextLayout(pangoCtx, titleShown, 13.5, true);
        pango_layout_set_width(titleLayout.get(), int(440.0 * PANGO_SCALE));
        pango_layout_set_ellipsize(titleLayout.get(), PANGO_ELLIPSIZE_END);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(titleLayout.get(), &tw, &th);
        cairo_move_to(cr, textLeft, 56.0 + (28.0 - double(th)) * 0.5);
        cairo_set_source_rgb(cr, 242.0 / 255.0, 232.0 / 255.0, 188.0 / 255.0);
        pango_cairo_show_layout(cr, titleLayout.get());

        const std::string clockStr = tr("Battle time") + ": " + timeFormatted;
        auto clockLayout = makeTextLayout(pangoCtx, clockStr, 13.0, true, 210.0, PANGO_ALIGN_RIGHT);
        int cw = 0, ch = 0;
        pango_layout_get_pixel_size(clockLayout.get(), &cw, &ch);
        cairo_move_to(cr, 744.0 - 210.0, 56.0 + (28.0 - double(ch)) * 0.5);
        cairo_set_source_rgb(cr, 214.0 / 255.0, 224.0 / 255.0, 182.0 / 255.0);
        pango_cairo_show_layout(cr, clockLayout.get());
    }

    // 4. Retail stat_pic.pck Silhouette Showcase Strip (y = 90..192).
    {
        drawBronzeBox(cr, 44.0, 90.0, 712.0, 102.0, 0.88);

        // Left Commander Portrait (stat_pic.pck[0]: 69x74 crosshair commander).
        drawBronzeBox(cr, 52.0, 97.0, 96.0, 88.0, 0.82);
        if (assets_->statPics[0].surface) {
            drawSurfaceStretched(cr, assets_->statPics[0].surface.get(), 70.0, 100.0, 60.0, 64.0);
        } else {
            drawStarPolygon(cr, 100.0, 130.0, 18.0, 7.5, stats_.victory, stats_.victory);
        }
        const std::string cmdr = sanitizeUtf8(
            stats_.commanderName.empty() ? std::string("Commander") : stats_.commanderName);
        auto cmdrLayout = makeTextLayout(pangoCtx, cmdr, 11.0, true, 88.0, PANGO_ALIGN_CENTER);
        pango_layout_set_ellipsize(cmdrLayout.get(), PANGO_ELLIPSIZE_END);
        cairo_move_to(cr, 56.0, 166.0);
        cairo_set_source_rgb(cr, 228.0 / 255.0, 220.0 / 255.0, 186.0 / 255.0);
        pango_cairo_show_layout(cr, cmdrLayout.get());

        struct SilhouetteCol {
            std::size_t statPicIndex;
            std::string label;
            unsigned count;
        };
        const std::array<SilhouetteCol, 6> cols{{
            {1u, tr("Infantry"), stats_.enemyInfantryKilled},
            {2u, tr("Tanks"), stats_.enemyTanksDestroyed},
            {3u, tr("Vehicles"), stats_.enemyVehiclesDestroyed},
            {6u, tr("Artillery"), stats_.enemyGunsDestroyed},
            {4u, tr("Aircraft"), stats_.enemyPlanesShotDown},
            {8u, tr("Buildings"), stats_.buildingsDemolished},
        }};

        constexpr double kGridX0 = 156.0;
        constexpr double kColStep = 99.0;
        constexpr double kColW = 93.0;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            const double cx = kGridX0 + double(i) * kColStep;
            drawBronzeBox(cr, cx, 97.0, kColW, 88.0, 0.76);

            const auto& pic = assets_->statPics[cols[i].statPicIndex];
            if (pic.surface && pic.w > 0 && pic.h > 0) {
                const double scale = std::min({1.0, 78.0 / double(pic.w), 40.0 / double(pic.h)});
                const double dw = double(pic.w) * scale;
                const double dh = double(pic.h) * scale;
                const double dx = cx + (kColW - dw) * 0.5;
                const double dy = 102.0 + (42.0 - dh) * 0.5;
                drawSurfaceStretched(cr, pic.surface.get(), dx, dy, dw, dh);
            } else {
                drawFallbackCategoryGlyph(cr, int(i), RectD{cx, 102.0, kColW, 42.0});
            }

            auto valLayout = makeTextLayout(
                pangoCtx, std::to_string(cols[i].count), 15.5, true, kColW - 6.0, PANGO_ALIGN_CENTER);
            cairo_move_to(cr, cx + 3.0, 145.0);
            if (cols[i].count > 0u) {
                cairo_set_source_rgb(cr, 248.0 / 255.0, 220.0 / 255.0, 128.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 162.0 / 255.0, 166.0 / 255.0, 150.0 / 255.0);
            }
            pango_cairo_show_layout(cr, valLayout.get());

            auto lblLayout = makeTextLayout(pangoCtx, cols[i].label, 10.5, false, kColW - 4.0, PANGO_ALIGN_CENTER);
            pango_layout_set_ellipsize(lblLayout.get(), PANGO_ELLIPSIZE_END);
            cairo_move_to(cr, cx + 2.0, 166.0);
            cairo_set_source_rgb(cr, 212.0 / 255.0, 208.0 / 255.0, 184.0 / 255.0);
            pango_cairo_show_layout(cr, lblLayout.get());
        }
    }

    // 5. Left Detailed Combat Statistics Dossier (x=44, y=198, w=402, h=256).
    {
        constexpr double kLeftX = 44.0;
        constexpr double kLeftY = 198.0;
        constexpr double kLeftW = 402.0;
        constexpr double kLeftH = 256.0;
        drawBronzeBox(cr, kLeftX, kLeftY, kLeftW, kLeftH, 0.90);
        drawHeading(cr, pangoCtx, tr("Game Statistics"), {kLeftX + 12.0, kLeftY + 5.0, kLeftW - 24.0, 24.0}, 14.5);

        cairo_set_source_rgba(cr, 182.0 / 255.0, 174.0 / 255.0, 146.0 / 255.0, 0.50);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, kLeftX + 10.0, kLeftY + 31.0);
        cairo_line_to(cr, kLeftX + kLeftW - 10.0, kLeftY + 31.0);
        cairo_stroke(cr);

        struct StatRow {
            std::string label;
            std::string value;
            bool lossHighlight{false};
        };
        const std::array<StatRow, 8> rows{{
            {tr("Enemy infantry eliminated"), std::to_string(stats_.enemyInfantryKilled), false},
            {tr("Enemy vehicles destroyed"), std::to_string(stats_.enemyVehiclesDestroyed), false},
            {tr("Enemy armor / guns"),
             std::to_string(stats_.enemyTanksDestroyed) + " / " + std::to_string(stats_.enemyGunsDestroyed),
             false},
            {tr("Enemy aircraft shot down"), std::to_string(stats_.enemyPlanesShotDown), false},
            {tr("Buildings demolished"), std::to_string(stats_.buildingsDemolished), false},
            {tr("Air sorties flown"), std::to_string(stats_.airSortiesFlown), false},
            {tr("Friendly casualties"), std::to_string(stats_.friendlyCasualties), true},
            {tr("Battle time"), timeFormatted, false},
        }};

        for (std::size_t i = 0; i < rows.size(); ++i) {
            const double ry = kLeftY + 36.0 + double(i) * 26.5;
            if (rows[i].lossHighlight) {
                cairo_set_source_rgba(cr, 112.0 / 255.0, 48.0 / 255.0, 38.0 / 255.0, 0.28);
                cairo_rectangle(cr, kLeftX + 8.0, ry, kLeftW - 16.0, 24.0);
                cairo_fill(cr);
            } else if ((i % 2u) == 1u) {
                cairo_set_source_rgba(cr, 44.0 / 255.0, 52.0 / 255.0, 36.0 / 255.0, 0.28);
                cairo_rectangle(cr, kLeftX + 8.0, ry, kLeftW - 16.0, 24.0);
                cairo_fill(cr);
            }

            auto lLayout = makeTextLayout(pangoCtx, rows[i].label, 12.5, rows[i].lossHighlight);
            pango_layout_set_width(lLayout.get(), int(278.0 * PANGO_SCALE));
            pango_layout_set_ellipsize(lLayout.get(), PANGO_ELLIPSIZE_END);
            int lw = 0, lh = 0;
            pango_layout_get_pixel_size(lLayout.get(), &lw, &lh);
            cairo_move_to(cr, kLeftX + 14.0, ry + (24.0 - double(lh)) * 0.5);
            if (rows[i].lossHighlight) {
                cairo_set_source_rgb(cr, 246.0 / 255.0, 196.0 / 255.0, 178.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 232.0 / 255.0, 228.0 / 255.0, 208.0 / 255.0);
            }
            pango_cairo_show_layout(cr, lLayout.get());

            auto rLayout = makeTextLayout(pangoCtx, rows[i].value, 13.0, true, 96.0, PANGO_ALIGN_RIGHT);
            int rw = 0, rh = 0;
            pango_layout_get_pixel_size(rLayout.get(), &rw, &rh);
            cairo_move_to(cr, kLeftX + kLeftW - 14.0 - 96.0, ry + (24.0 - double(rh)) * 0.5);
            if (rows[i].lossHighlight) {
                cairo_set_source_rgb(cr, 248.0 / 255.0, 152.0 / 255.0, 128.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 246.0 / 255.0, 218.0 / 255.0, 132.0 / 255.0);
            }
            pango_cairo_show_layout(cr, rLayout.get());
        }
    }

    // 6. Right Score, Best Record & 1..10 Retail Star Medals Panel (x=456, y=198, w=300, h=256).
    {
        constexpr double kRightX = 456.0;
        constexpr double kRightY = 198.0;
        constexpr double kRightW = 300.0;
        constexpr double kRightH = 256.0;
        drawBronzeBox(cr, kRightX, kRightY, kRightW, kRightH, 0.90);
        drawHeading(cr, pangoCtx, tr("Mission rating"), {kRightX + 12.0, kRightY + 5.0, kRightW - 24.0, 24.0}, 14.5);

        cairo_set_source_rgba(cr, 182.0 / 255.0, 174.0 / 255.0, 146.0 / 255.0, 0.50);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, kRightX + 10.0, kRightY + 31.0);
        cairo_line_to(cr, kRightX + kRightW - 10.0, kRightY + 31.0);
        cairo_stroke(cr);

        // Battle score box (retail military green/gold accent from n2Menu_dll.dll 0x10020504).
        drawBronzeBox(cr, kRightX + 14.0, kRightY + 38.0, kRightW - 28.0, 44.0, 0.82);
        {
            auto bsLbl = makeTextLayout(pangoCtx, tr("Battle score"), 12.5, true);
            cairo_move_to(cr, kRightX + 24.0, kRightY + 51.0);
            cairo_set_source_rgb(cr, 224.0 / 255.0, 220.0 / 255.0, 194.0 / 255.0);
            pango_cairo_show_layout(cr, bsLbl.get());

            auto bsVal = makeTextLayout(
                pangoCtx, std::to_string(stats_.battleScore), 19.0, true, 136.0, PANGO_ALIGN_RIGHT);
            cairo_move_to(cr, kRightX + kRightW - 24.0 - 136.0, kRightY + 47.0);
            cairo_set_source_rgb(cr, 92.0 / 255.0, 214.0 / 255.0, 112.0 / 255.0);
            pango_cairo_show_layout(cr, bsVal.get());
        }

        // Best score box + New Best badge.
        drawBronzeBox(cr, kRightX + 14.0, kRightY + 88.0, kRightW - 28.0, 42.0, 0.82);
        {
            auto bestLbl = makeTextLayout(pangoCtx, tr("Best score"), 12.5, false);
            cairo_move_to(cr, kRightX + 24.0, kRightY + 95.0);
            cairo_set_source_rgb(cr, 220.0 / 255.0, 216.0 / 255.0, 190.0 / 255.0);
            pango_cairo_show_layout(cr, bestLbl.get());

            if (stats_.isNewBest) {
                auto badge = makeTextLayout(pangoCtx, tr("New Best!"), 10.5, true);
                cairo_move_to(cr, kRightX + 24.0, kRightY + 112.0);
                cairo_set_source_rgb(cr, 252.0 / 255.0, 214.0 / 255.0, 92.0 / 255.0);
                pango_cairo_show_layout(cr, badge.get());
            }

            const std::string bestValStr = stats_.bestScore > 0 ? std::to_string(stats_.bestScore) : std::string("-");
            auto bestVal = makeTextLayout(pangoCtx, bestValStr, 15.5, true, 132.0, PANGO_ALIGN_RIGHT);
            cairo_move_to(cr, kRightX + kRightW - 24.0 - 132.0, kRightY + 99.0);
            cairo_set_source_rgb(cr, 246.0 / 255.0, 216.0 / 255.0, 124.0 / 255.0);
            pango_cairo_show_layout(cr, bestVal.get());
        }

        // 1..10 Star Rating & Retail Medal Rack (ZVEZDY.PCK / ZV_GOLD.PCK / ZV_M.PCK).
        drawBronzeBox(cr, kRightX + 14.0, kRightY + 136.0, kRightW - 28.0, 108.0, 0.84);
        {
            const int stars = std::clamp(stats_.starRating, 0, 10);
            const std::string medalHdr = tr("Medals") + ": " + std::to_string(stars) + " / 10";
            auto hdrLayout = makeTextLayout(pangoCtx, medalHdr, 12.0, true, kRightW - 44.0, PANGO_ALIGN_CENTER);
            cairo_move_to(cr, kRightX + 22.0, kRightY + 142.0);
            cairo_set_source_rgb(cr, 238.0 / 255.0, 224.0 / 255.0, 172.0 / 255.0);
            pango_cairo_show_layout(cr, hdrLayout.get());

            // Two crisp rows of 5 medal sockets at 2x integer pixel scale (26x26 from 13x13 retail PCKs).
            constexpr double kSlotSize = 30.0;
            constexpr double kSpriteSize = 26.0;
            constexpr double kColGap = 16.0;
            const double rowSpan = 5.0 * kSlotSize + 4.0 * kColGap;
            const double rackX0 = kRightX + (kRightW - rowSpan) * 0.5;

            for (int idx = 0; idx < 10; ++idx) {
                const int r = idx / 5;
                const int c = idx % 5;
                const double sx = rackX0 + double(c) * (kSlotSize + kColGap);
                const double sy = kRightY + 163.0 + double(r) * 38.0;
                const bool earned = idx < stars;
                const bool goldAura = earned && (stars > 5 ? (idx < (stars - 5) || r == 1) : (idx == stars - 1));

                cairo_save(cr);
                cairo_rectangle(cr, sx, sy, kSlotSize, kSlotSize);
                if (earned) {
                    cairo_set_source_rgba(cr, 56.0 / 255.0, 50.0 / 255.0, 24.0 / 255.0, 0.78);
                } else {
                    cairo_set_source_rgba(cr, 18.0 / 255.0, 22.0 / 255.0, 16.0 / 255.0, 0.72);
                }
                cairo_fill_preserve(cr);
                cairo_set_line_width(cr, 1.0);
                if (earned) {
                    cairo_set_source_rgba(cr, 224.0 / 255.0, 192.0 / 255.0, 96.0 / 255.0, 0.85);
                } else {
                    cairo_set_source_rgba(cr, 96.0 / 255.0, 94.0 / 255.0, 78.0 / 255.0, 0.45);
                }
                cairo_stroke(cr);
                cairo_restore(cr);

                const std::size_t goldIdx = factionIdx * 5u + (goldAura ? 4u : 2u);
                if (earned && assets_->starsGold[goldIdx].surface && assets_->starsRegular[factionIdx].surface) {
                    auto* surf = (goldAura || idx >= 3)
                        ? assets_->starsGold[goldIdx].surface.get()
                        : assets_->starsRegular[factionIdx].surface.get();
                    drawSurfaceNearest(cr, surf, sx + 2.0, sy + 2.0, kSpriteSize, kSpriteSize);
                } else if (!earned && assets_->starsSmall[factionIdx].surface) {
                    cairo_save(cr);
                    drawSurfaceNearest(cr, assets_->starsSmall[factionIdx].surface.get(),
                                       sx + 4.0, sy + 4.0, 22.0, 22.0);
                    cairo_rectangle(cr, sx + 1.0, sy + 1.0, kSlotSize - 2.0, kSlotSize - 2.0);
                    cairo_set_source_rgba(cr, 12.0 / 255.0, 15.0 / 255.0, 10.0 / 255.0, 0.72);
                    cairo_fill(cr);
                    cairo_restore(cr);
                } else {
                    drawStarPolygon(cr, sx + kSlotSize * 0.5, sy + kSlotSize * 0.5,
                                    11.5, 4.8, earned, goldAura);
                }
            }
        }
    }

    // 7. Action Buttons & Next Campaign Operation Subtitle (y = 466..588).
    {
        const auto buttons = buildActionButtons();
        for (const auto& b : buttons) {
            const bool hov = (hoveredId_ == b.id);
            const bool foc = (focusedId_ == b.id);
            drawBronzeBox(cr, b.rect.x, b.rect.y, b.rect.w, b.rect.h, (hov || foc) ? 0.94 : 0.84);
            if (foc) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, 118.0 / 255.0, 134.0 / 255.0, 76.0 / 255.0, 0.30);
                cairo_rectangle(cr, b.rect.x + 3.0, b.rect.y + 3.0, b.rect.w - 6.0, b.rect.h - 6.0);
                cairo_fill(cr);
                cairo_restore(cr);
            }
            drawGoldStencilButton(cr, pangoCtx, b.rect, b.label, b.shortcut, hov, foc, true, 16.5);
        }

        if (stats_.victory && stats_.hasNextCampaign && !stats_.nextCampaignTitle.empty()) {
            const std::string nextBanner =
                tr("Next mission") + ": " + sanitizeUtf8(stats_.nextCampaignTitle);
            drawBronzeBox(cr, 140.0, 562.0, 520.0, 26.0, 0.82);
            drawHeading(cr, pangoCtx, nextBanner, {148.0, 562.0, 504.0, 26.0}, 12.5);
        }
    }

    cairo_restore(cr);

    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));
    }
    cairo_surface_flush(canvas.get());
    outRgba.resize(std::size_t(pw) * std::size_t(ph) * 4u);
    if (!SDL_ConvertPixels(pw, ph, SDL_PIXELFORMAT_ARGB8888,
                           cairo_image_surface_get_data(canvas.get()),
                           cairo_image_surface_get_stride(canvas.get()),
                           SDL_PIXELFORMAT_RGBA32, outRgba.data(), pw * 4)) {
        throw std::runtime_error(SDL_GetError());
    }
    for (std::size_t i = 3u; i < outRgba.size(); i += 4u) {
        outRgba[i] = 255u;
    }
}

void GoldPostMissionScreen::draw(VulkanWindow& renderer) {
    if (!active_) return;
    if (window_ && assets_) {
        assets_->menuCursor.apply();
    }
    int pw = 0, ph = 0;
    if (window_ && SDL_GetWindowSizeInPixels(window_, &pw, &ph) && pw > 0 && ph > 0) {
    } else {
        const auto [extW, extH] = renderer.extent();
        pw = int(extW);
        ph = int(extH);
    }
    pw = std::max(1, pw);
    ph = std::max(1, ph);

    if (!dirty_ && pw == lastPixelW_ && ph == lastPixelH_ &&
        rgba_.size() == std::size_t(pw) * std::size_t(ph) * 4u) {
        return;
    }
    renderToBuffer(pw, ph, rgba_);
    lastPixelW_ = pw;
    lastPixelH_ = ph;
    dirty_ = false;
    renderer.setOverlay(static_cast<unsigned>(pw), static_cast<unsigned>(ph), rgba_, OverlaySlot::panel);
}

void GoldPostMissionScreen::clear(VulkanWindow& renderer) {
    close();
    lastPixelW_ = 0;
    lastPixelH_ = 0;
    renderer.clearOverlay(OverlaySlot::panel);
}

}  // namespace ss2
