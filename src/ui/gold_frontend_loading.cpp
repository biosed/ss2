#include "ui/gold_frontend_internal.hpp"

#include "assets/indexed.hpp"
#include "render/window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ss2 {
namespace {

using namespace gold_ui;

struct IntRect {
    int left{}, top{}, right{}, bottom{};
    int width() const { return std::max(0, right - left + 1); }
    int height() const { return std::max(0, bottom - top + 1); }
};

std::uint16_t readU16LE(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (std::uint16_t(bytes[at + 1]) << 8));
}

std::int16_t readI16LE(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::int16_t>(readU16LE(bytes, at));
}

std::uint32_t readU32LE(std::span<const std::uint8_t> bytes, std::size_t at) {
    return std::uint32_t(readU16LE(bytes, at)) |
           (std::uint32_t(readU16LE(bytes, at + 2)) << 16);
}

std::int32_t readI32LE(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::int32_t>(readU32LE(bytes, at));
}

Surface decodeIndexedSpriteSurface(std::span<const std::uint8_t> sprite,
                                   std::span<const std::uint8_t> palette) {
    if (sprite.size() < 9 || palette.size() < 512) return makeNullSurface();
    const int originX = readI16LE(sprite, 0);
    const int originY = readI16LE(sprite, 2);
    const int width = readI16LE(sprite, 4);
    const int height = readI16LE(sprite, 6);
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) return makeNullSurface();

    try {
        assets::MapImage img{
            static_cast<unsigned>(width),
            static_cast<unsigned>(height),
            std::vector<std::uint8_t>(std::size_t(width) * height * 4u, 0u),
        };
        assets::drawIndexedFrame(img, -originX, -originY, sprite, palette);
        return mapImageToSurface(img);
    } catch (...) {
        return makeNullSurface();
    }
}

bool parseLoadScrRecords(std::span<const std::uint8_t> raw,
                         std::vector<std::span<const std::uint8_t>>& outRecords) {
    outRecords.clear();
    if (raw.size() < 8) return false;
    const std::uint32_t count = readU32LE(raw, 0);
    if (count < 17u || count > 256u) return false;
    const std::size_t headerBytes = 4u + std::size_t(count + 1u) * 4u;
    if (raw.size() < headerBytes) return false;

    std::vector<std::uint32_t> offsets(count + 1u);
    for (std::uint32_t i = 0; i <= count; ++i) {
        offsets[i] = readU32LE(raw, 4u + std::size_t(i) * 4u);
    }
    if (offsets[0] < headerBytes || offsets[count] > raw.size()) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offsets[i] > offsets[i + 1u]) return false;
    }

    outRecords.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        outRecords.push_back(raw.subspan(offsets[i], offsets[i + 1u] - offsets[i]));
    }
    return true;
}

IntRect readLayoutRect(std::span<const std::uint8_t> rectTable, std::size_t index, IntRect fallback) {
    const std::size_t off = index * 16u;
    if (off + 16u > rectTable.size()) return fallback;
    const IntRect r{
        readI32LE(rectTable, off + 0u),
        readI32LE(rectTable, off + 4u),
        readI32LE(rectTable, off + 8u),
        readI32LE(rectTable, off + 12u),
    };
    if (r.right < r.left || r.bottom < r.top) return fallback;
    return r;
}

}  // namespace

struct GoldLoadingScreen::Assets {
    FontMap fontMap{pango_cairo_font_map_new(), g_object_unref};
    FontCtx fontCtx{pango_font_map_create_context(fontMap.get()), g_object_unref};

    Surface bgSurface = makeNullSurface();
    Surface fallbackMenuBg = makeNullSurface();
    Surface frameSurface = makeNullSurface();
    Surface barSurface = makeNullSurface();
    Surface lampSurface = makeNullSurface();

    // Retail loadscr.dat record[1] layout rectangles relative to (kBarX, kBarY)
    IntRect plaqueRect{156, 0, 390, 23};
    IntRect textRect{178, 9, 368, 19};
    IntRect barRect{10, 28, 525, 31};
    IntRect lampSocketRect{531, 24, 547, 35};

    int bgIndex{0};
    std::size_t recordCount{0};
    bool loadedRetail{false};
};

GoldLoadingScreen::GoldLoadingScreen(SDL_Window* window, const std::filesystem::path& gameDir,
                                     std::optional<int> fixedBgIndex)
    : window_(window), gameDir_(gameDir), assets_(std::make_unique<Assets>()) {
    loadAssets(fixedBgIndex);
}

GoldLoadingScreen::~GoldLoadingScreen() = default;

bool GoldLoadingScreen::hasRetailAssets() const {
    return assets_ && assets_->loadedRetail;
}

int GoldLoadingScreen::backgroundIndex() const {
    return assets_ ? assets_->bgIndex : 0;
}

std::size_t GoldLoadingScreen::recordCount() const {
    return assets_ ? assets_->recordCount : 0u;
}

void GoldLoadingScreen::loadAssets(std::optional<int> fixedBgIndex) {
    int chosen = fixedBgIndex.value_or(-1);
    if (chosen < 0 || chosen > 2) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 2);
        chosen = dist(rng);
    }
    assets_->bgIndex = chosen;

    if (gameDir_.empty()) return;

    // Retail Sudden Strike II Gold 2.2 stores Screen 5 loading art in lng.sue\loadscr.dat:
    //   record[0]      : 512-byte RGB565 UI palette
    //   record[1]      : 1040-byte RECT layout table (plaque, text, 516x4 bar, lamp socket)
    //   record[3]      : 548x36 steel loading frame + raised nameplate tab
    //   record[6]      : 516x4 metallic progress bar fill
    //   record[8]      : 14x8 completion indicator lamp
    //   record[11..13] : 512-byte RGB565 palettes for the 3 wartime vignette backgrounds
    //   record[14..16] : 640x480 stippled wartime vignette backgrounds
    for (const char* sueName : {"lng.sue", "lang.sue", "menu_common.sue"}) {
        const auto suePath = assets::resolveCaseInsensitivePath(gameDir_, sueName);
        std::error_code ec;
        if (!std::filesystem::exists(suePath, ec)) continue;
        try {
            const auto raw = assets::readSueEntry(suePath, "loadscr.dat");
            std::vector<std::span<const std::uint8_t>> recs;
            if (!parseLoadScrRecords(raw, recs)) continue;

            assets_->recordCount = recs.size();
            const auto uiPal = recs[0];
            assets_->plaqueRect = readLayoutRect(recs[1], 1u, assets_->plaqueRect);
            assets_->textRect = readLayoutRect(recs[1], 2u, assets_->textRect);
            assets_->barRect = readLayoutRect(recs[1], 4u, assets_->barRect);
            assets_->lampSocketRect = readLayoutRect(recs[1], 5u, assets_->lampSocketRect);

            assets_->frameSurface = decodeIndexedSpriteSurface(recs[3], uiPal);
            assets_->barSurface = decodeIndexedSpriteSurface(recs[6], uiPal);
            assets_->lampSurface = decodeIndexedSpriteSurface(recs[8], uiPal);

            const std::size_t bgPalIdx = 11u + static_cast<std::size_t>(chosen);
            const std::size_t bgSprIdx = 14u + static_cast<std::size_t>(chosen);
            if (bgSprIdx < recs.size() && bgPalIdx < recs.size()) {
                assets_->bgSurface = decodeIndexedSpriteSurface(recs[bgSprIdx], recs[bgPalIdx]);
            }
            if (!assets_->bgSurface) {
                assets_->bgSurface = decodeIndexedSpriteSurface(recs[2], uiPal);
            }

            assets_->loadedRetail =
                assets_->bgSurface != nullptr &&
                assets_->frameSurface != nullptr &&
                assets_->barSurface != nullptr;
            if (assets_->loadedRetail) return;
        } catch (...) {
        }
    }

    // Secondary fallback if loadscr.dat is absent but menu_common.sue is present.
    const auto menuSue = assets::resolveCaseInsensitivePath(gameDir_, "menu_common.sue");
    std::error_code ec;
    if (std::filesystem::exists(menuSue, ec)) {
        try {
            const auto bytes = assets::readSueEntry(menuSue, "mainbk000.jpg");
            assets_->fallbackMenuBg = mapImageToSurface(assets::decodeJpeg(bytes));
        } catch (...) {
        }
    }
}

void GoldLoadingScreen::renderToBuffer(int pixelW, int pixelH, std::string_view missionTitle,
                                       float progress01, std::vector<std::uint8_t>& outRgba) {
    const int pw = std::max(1, pixelW);
    const int ph = std::max(1, pixelH);
    const float clamped = std::clamp(progress01, 0.0f, 1.0f);

    Surface canvas(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph), cairo_surface_destroy);
    Context crGuard(cairo_create(canvas.get()), cairo_destroy);
    cairo_t* cr = crGuard.get();

    // Pure black surround matches n2Game_Dll.dll screen clear so the 640x480 stippled
    // vignette border blends seamlessly on both 4:3 and widescreen swapchains.
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    const GoldStageRect stage = GoldFrontEnd::computeStage(pw, ph);
    cairo_save(cr);
    cairo_translate(cr, stage.x, stage.y);
    cairo_scale(cr, stage.scale, stage.scale);
    cairo_rectangle(cr, 0, 0, kStageW, kStageH);
    cairo_clip(cr);

    if (assets_->bgSurface) {
        // Centered 640x480 wartime vignette on the 800x600 stage: ((800-640)/2, (600-480)/2) = (80, 60).
        drawSurfaceStretched(cr, assets_->bgSurface.get(), 80.0, 60.0, 640.0, 480.0);
    } else if (assets_->fallbackMenuBg) {
        drawSurfaceStretched(cr, assets_->fallbackMenuBg.get(), 0.0, 0.0, kStageW, kStageH);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.52);
        cairo_rectangle(cr, 0.0, 0.0, kStageW, kStageH);
        cairo_fill(cr);
    } else {
        drawFallbackBackdrop(cr, false);
    }

    // Authentic Gold 2.2 single-player loading bar placement from n2Game_Dll.dll (0x100c21ab..0x100c21c2):
    //   barX = (800 - 544) / 2 = 128, barY = (600 - 32) / 2 = 284.
    constexpr double kBarX = 128.0;
    constexpr double kBarY = 284.0;
    constexpr double kFrameW = 548.0;
    constexpr double kFrameH = 36.0;

    if (assets_->frameSurface) {
        drawSurfaceStretched(cr, assets_->frameSurface.get(), kBarX, kBarY, kFrameW, kFrameH);
    } else {
        drawBronzeBox(cr, kBarX + assets_->plaqueRect.left, kBarY + assets_->plaqueRect.top,
                      assets_->plaqueRect.width(), assets_->plaqueRect.height(), 0.90);
        drawBronzeBox(cr, kBarX, kBarY + 24.0, kFrameW, 12.0, 0.92);
    }

    // Progress bar fill inside record[1] rect[4] = (10, 28, 525, 31) -> 516x4 px.
    const double fillX = kBarX + double(assets_->barRect.left);
    const double fillY = kBarY + double(assets_->barRect.top);
    const double fullW = double(assets_->barRect.width());
    const double fillH = double(assets_->barRect.height());
    const double curW = fullW * double(clamped);
    if (curW >= 0.5 && fillH > 0.0) {
        cairo_save(cr);
        cairo_rectangle(cr, fillX, fillY, curW, fillH);
        cairo_clip(cr);
        if (assets_->barSurface) {
            drawSurfaceStretched(cr, assets_->barSurface.get(), fillX, fillY, fullW, fillH);
        } else {
            cairo_pattern_t* barGrad = cairo_pattern_create_linear(0.0, fillY, 0.0, fillY + fillH);
            cairo_pattern_add_color_stop_rgb(barGrad, 0.0, 242.0 / 255.0, 244.0 / 255.0, 232.0 / 255.0);
            cairo_pattern_add_color_stop_rgb(barGrad, 0.6, 178.0 / 255.0, 184.0 / 255.0, 168.0 / 255.0);
            cairo_pattern_add_color_stop_rgb(barGrad, 1.0, 92.0 / 255.0, 98.0 / 255.0, 86.0 / 255.0);
            cairo_set_source(cr, barGrad);
            cairo_rectangle(cr, fillX, fillY, curW, fillH);
            cairo_fill(cr);
            cairo_pattern_destroy(barGrad);
        }
        cairo_restore(cr);
    }

    // Right-hand completion lamp inside record[1] rect[5] = (531, 24, 547, 35) -> drawn at (+2, +2) = 14x8 px.
    if (clamped >= 0.999f) {
        const double lampX = kBarX + double(assets_->lampSocketRect.left) + 2.0;
        const double lampY = kBarY + double(assets_->lampSocketRect.top) + 2.0;
        if (assets_->lampSurface) {
            drawSurfaceStretched(cr, assets_->lampSurface.get(), lampX, lampY, 14.0, 8.0);
        } else {
            cairo_save(cr);
            cairo_set_source_rgb(cr, 250.0 / 255.0, 206.0 / 255.0, 68.0 / 255.0);
            cairo_rectangle(cr, lampX, lampY, 14.0, 8.0);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    auto* pangoCtx = assets_->fontCtx.get();

    // "LOADING" label centered inside the raised nameplate tab (n2Game_Dll.dll 0x100c257b..0x100c27b0).
    {
        const double plaqueX = kBarX + double(assets_->plaqueRect.left);
        const double plaqueY = kBarY + double(assets_->plaqueRect.top);
        const double plaqueW = double(assets_->plaqueRect.width());
        const double plaqueH = double(assets_->plaqueRect.height());
        const std::string loadingLabel = toUpperAscii(tr("LOADING"));
        auto lbl = makeTextLayout(pangoCtx, loadingLabel, 12.5, true);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(lbl.get(), &tw, &th);
        const double tx = plaqueX + (plaqueW - tw) * 0.5;
        const double ty = plaqueY + (plaqueH - th) * 0.5 + 1.0;

        cairo_save(cr);
        cairo_move_to(cr, tx + 1.2, ty + 1.2);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.92);
        pango_cairo_show_layout(cr, lbl.get());

        cairo_move_to(cr, tx, ty);
        cairo_set_source_rgb(cr, 214.0 / 255.0, 198.0 / 255.0, 152.0 / 255.0);
        pango_cairo_show_layout(cr, lbl.get());
        cairo_restore(cr);
    }

    if (!missionTitle.empty()) {
        const std::string cleanTitle = sanitizeUtf8(missionTitle);
        drawHeading(cr, pangoCtx, cleanTitle, {100.0, kBarY - 34.0, 600.0, 24.0}, 15.5);
    }

    cairo_restore(cr);

    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));
    }
    cairo_surface_flush(canvas.get());
    outRgba.resize(std::size_t(pw) * ph * 4u);
    if (!SDL_ConvertPixels(pw, ph, SDL_PIXELFORMAT_ARGB8888,
                           cairo_image_surface_get_data(canvas.get()),
                           cairo_image_surface_get_stride(canvas.get()),
                           SDL_PIXELFORMAT_RGBA32, outRgba.data(), pw * 4)) {
        throw std::runtime_error(SDL_GetError());
    }
}

void GoldLoadingScreen::drawAndPresent(VulkanWindow& renderer, std::string_view missionTitle,
                                       float progress01) {
    int pw = 0, ph = 0;
    if (window_ && SDL_GetWindowSizeInPixels(window_, &pw, &ph) && pw > 0 && ph > 0) {
    } else {
        const auto [extW, extH] = renderer.extent();
        pw = int(extW);
        ph = int(extH);
    }
    pw = std::max(1, pw);
    ph = std::max(1, ph);

    renderToBuffer(pw, ph, missionTitle, progress01, rgba_);
    renderer.setOverlay(static_cast<unsigned>(pw), static_cast<unsigned>(ph), rgba_, OverlaySlot::panel);
    const Snapshot empty{};
    (void)renderer.present(empty);
}

void GoldLoadingScreen::clear(VulkanWindow& renderer) {
    renderer.clearOverlay(OverlaySlot::panel);
}

}  // namespace ss2
