#include "ui/hud.hpp"
#include "ui/strings.hpp"
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include "render/window.hpp"

#include <pango/pangocairo.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ss2 {
namespace {
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;

void colour(cairo_t* cr, int r, int g, int b, double a = 1) {
    cairo_set_source_rgba(cr, r / 255., g / 255., b / 255., a);
}

// Draws one line of text; returns its pixel width. Text wider than maxWidth is ellipsized.
// With centre set, the line is centred within maxWidth.
int text(cairo_t* cr, PangoContext* fonts, const std::string& value, int size, bool bold, double x, double y, int maxWidth,
         bool centre = false) {
    Layout layout(pango_layout_new(fonts), g_object_unref);
    std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
        pango_font_description_from_string("Inter, sans-serif"), pango_font_description_free);
    pango_font_description_set_weight(font.get(), bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(font.get(), size * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_text(layout.get(), value.c_str(), -1);
    if (maxWidth > 0) {
        pango_layout_set_width(layout.get(), maxWidth * PANGO_SCALE);
        pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
        if (centre) pango_layout_set_alignment(layout.get(), PANGO_ALIGN_CENTER);
    }
    int w{}, h{};
    pango_layout_get_pixel_size(layout.get(), &w, &h);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout.get());
    return w;
}

// The largest size from `size` down to about 70% of it at which the text fits the width
// on one line; longer words in other languages should shrink before they are cut off.
int fittedSize(PangoContext* fonts, const std::string& value, int size, bool bold, int maxWidth) {
    for (int candidate = size; candidate > std::max(8, size * 7 / 10); --candidate) {
        Layout layout(pango_layout_new(fonts), g_object_unref);
        std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
            pango_font_description_from_string("Inter, sans-serif"), pango_font_description_free);
        pango_font_description_set_weight(font.get(), bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
        pango_font_description_set_absolute_size(font.get(), candidate * PANGO_SCALE);
        pango_layout_set_font_description(layout.get(), font.get());
        pango_layout_set_text(layout.get(), value.c_str(), -1);
        int w{}, h{};
        pango_layout_get_pixel_size(layout.get(), &w, &h);
        if (w <= maxWidth) return candidate;
    }
    return std::max(8, size * 7 / 10);
}

// Wraps RGBA8 pixels as a cairo image (premultiplied, native-endian ARGB).
Surface image(const assets::MapImage& source) {
    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(source.width), int(source.height)),
                    cairo_surface_destroy);
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error("Could not create a HUD image");
    auto* out = cairo_image_surface_get_data(surface.get());
    const int stride = cairo_image_surface_get_stride(surface.get());
    for (unsigned y = 0; y < source.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(out + std::size_t(y) * stride);
        const auto* in = &source.rgba[std::size_t(y) * source.width * 4];
        for (unsigned x = 0; x < source.width; ++x, in += 4) {
            const unsigned a = in[3];
            const auto pre = [a](unsigned c) { return (c * a + 127) / 255; };
            row[x] = (a << 24) | (pre(in[0]) << 16) | (pre(in[1]) << 8) | pre(in[2]);
        }
    }
    cairo_surface_mark_dirty(surface.get());
    return surface;
}

bool inside(Hud::Rect r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

bool hasRole(const HudState& state, std::initializer_list<const char*> needles) {
    for (const auto& c : state.selection) {
        std::string low = c.label;
        for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (const char* n : needles) {
            if (low.find(n) != std::string::npos) return true;
        }
    }
    return false;
}

bool hasOfficerSelection(const HudState& state) {
    return (state.hasSelection || !state.selection.empty()) && hasRole(state, {"officer"});
}

Surface makeNullSurface() {
    return Surface(nullptr, cairo_surface_destroy);
}

std::uint16_t readU16LE(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (std::uint16_t(bytes[at + 1]) << 8));
}

std::int16_t readI16LE(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::int16_t>(readU16LE(bytes, at));
}

std::uint32_t readU32LE(std::span<const std::uint8_t> bytes, std::size_t at) {
    return std::uint32_t(readU16LE(bytes, at)) | (std::uint32_t(readU16LE(bytes, at + 2)) << 16);
}

bool parseOffsetRecords(std::span<const std::uint8_t> raw, std::uint32_t minRecords, std::uint32_t maxRecords,
                        std::vector<std::span<const std::uint8_t>>& outRecords) {
    outRecords.clear();
    if (raw.size() < 8) return false;
    const std::uint32_t count = readU32LE(raw, 0);
    if (count < minRecords || count > maxRecords) return false;
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

Surface decodeIndexedSurface(std::span<const std::uint8_t> sprite, std::span<const std::uint8_t> palette) {
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
        return image(img);
    } catch (...) {
        return makeNullSurface();
    }
}

void drawSurfaceCrisp(cairo_t* cr, cairo_surface_t* surf, double x, double y, double w, double h, double alpha = 1.0) {
    if (!surf || w <= 0.0 || h <= 0.0) return;
    const int sw = cairo_image_surface_get_width(surf);
    const int sh = cairo_image_surface_get_height(surf);
    if (sw <= 0 || sh <= 0) return;
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, w / double(sw), h / double(sh));
    cairo_set_source_surface(cr, surf, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    if (alpha >= 0.999) cairo_paint(cr);
    else cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

void tileSurfaceRegion(cairo_t* cr, cairo_surface_t* surf, int rx, int ry, int rw, int rh, double alpha) {
    if (!surf || rw <= 0 || rh <= 0) return;
    const int sw = cairo_image_surface_get_width(surf);
    const int sh = cairo_image_surface_get_height(surf);
    if (sw <= 0 || sh <= 0) return;
    cairo_save(cr);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_clip(cr);
    for (int y = ry; y < ry + rh; y += sh) {
        for (int x = rx; x < rx + rw; x += sw) {
            cairo_set_source_surface(cr, surf, x, y);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
            cairo_paint_with_alpha(cr, alpha);
        }
    }
    cairo_restore(cr);
}

std::string stripRetailKeyTag(std::string s) {
    for (;;) {
        const auto open = s.find("(~KEY_");
        if (open == std::string::npos) break;
        const auto close = s.find("~)", open);
        if (close == std::string::npos) break;
        std::size_t start = open;
        while (start > 0 && s[start - 1] == ' ') --start;
        s.erase(start, (close + 2) - start);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

void parseGameLng(std::string_view rawText, std::map<std::string, std::vector<std::string>>& out) {
    std::size_t pos = 0;
    while (pos < rawText.size()) {
        std::size_t eol = rawText.find('\n', pos);
        if (eol == std::string_view::npos) eol = rawText.size();
        std::string_view line = rawText.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        std::size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        line.remove_prefix(lead);
        if (line.empty() || line.front() == '[' || line.front() == ';' || line.front() == '/') continue;

        std::size_t sep = 0;
        while (sep < line.size() && line[sep] != ' ' && line[sep] != '\t') ++sep;
        std::string_view rawKey = line.substr(0, sep);
        std::string_view rawVal = line.substr(sep);
        while (!rawVal.empty() && (rawVal.front() == ' ' || rawVal.front() == '\t')) rawVal.remove_prefix(1);
        if (rawKey.empty()) continue;

        std::size_t idx = 0;
        const auto brOpen = rawKey.find('[');
        const auto brClose = rawKey.find(']');
        if (brOpen != std::string_view::npos && brClose != std::string_view::npos && brClose > brOpen + 1) {
            std::size_t parsed = 0;
            bool ok = true;
            for (std::size_t k = brOpen + 1; k < brClose; ++k) {
                if (rawKey[k] < '0' || rawKey[k] > '9') { ok = false; break; }
                parsed = parsed * 10u + std::size_t(rawKey[k] - '0');
            }
            if (ok && parsed < 32u) idx = parsed;
            rawKey = rawKey.substr(0, brOpen);
        }
        auto& vec = out[std::string(rawKey)];
        if (vec.size() <= idx) vec.resize(idx + 1u);
        vec[idx] = assets::decodeMissionText(rawVal);
    }
}

// Decode a 0xCA000002 .ANI file paired with its 1024-byte (RGB565 + bgTrans254) .COL palette,
// stamping each non-empty frame centered into a cellSize x cellSize cell of `sheet`.
std::size_t decodeAniFramesIntoSheet(std::span<const std::uint8_t> ani,
                                     std::span<const std::uint8_t> col,
                                     assets::MapImage& sheet,
                                     unsigned sheetCols,
                                     unsigned cellSize,
                                     std::size_t startCell,
                                     std::size_t maxCells) {
    if (ani.size() < 12 || col.size() < 1024) return 0;
    if (readU32LE(ani, 0) != 0xCA000002u) return 0;
    const std::uint32_t frameCount = readU32LE(ani, 4);
    if (frameCount == 0 || frameCount > 256u) return 0;
    const std::size_t hdrBytes = 8u + std::size_t(frameCount + 1u) * 4u;
    if (ani.size() < hdrBytes) return 0;

    std::vector<std::uint32_t> offs(frameCount + 1u);
    for (std::uint32_t i = 0; i <= frameCount; ++i) {
        offs[i] = readU32LE(ani, 8u + std::size_t(i) * 4u);
    }
    if (offs[0] < hdrBytes || offs[frameCount] > ani.size()) return 0;

    // Precompute straight RGBA8 palette from 256 x (u16 premul_rgb565, u16 bg_trans_254).
    std::array<std::array<std::uint8_t, 4>, 256> rgbaPal{};
    for (std::size_t idx = 0; idx < 256u; ++idx) {
        const std::uint16_t c565 = readU16LE(col, idx * 4u);
        const std::uint16_t bgTrans = readU16LE(col, idx * 4u + 2u);
        const unsigned r5 = (c565 >> 11u) & 31u;
        const unsigned g6 = (c565 >> 5u) & 63u;
        const unsigned b5 = c565 & 31u;
        const unsigned pr = (r5 << 3u) | (r5 >> 2u);
        const unsigned pg = (g6 << 2u) | (g6 >> 4u);
        const unsigned pb = (b5 << 3u) | (b5 >> 2u);
        const int alpha254 = std::clamp(254 - int(bgTrans & 0xFFu), 0, 254);
        const int a8 = std::max((alpha254 * 255 + 127) / 254,
                                c565 != 0u ? int(std::max({pr, pg, pb})) : 0);
        if (a8 <= 0) {
            rgbaPal[idx] = {0, 0, 0, 0};
        } else {
            rgbaPal[idx] = {
                static_cast<std::uint8_t>(std::min(255, (int(pr) * 255 + a8 / 2) / a8)),
                static_cast<std::uint8_t>(std::min(255, (int(pg) * 255 + a8 / 2) / a8)),
                static_cast<std::uint8_t>(std::min(255, (int(pb) * 255 + a8 / 2) / a8)),
                static_cast<std::uint8_t>(std::clamp(a8, 0, 255)),
            };
        }
    }

    std::size_t written = 0;
    const int half = int(cellSize) / 2;
    for (std::uint32_t fi = 0; fi < frameCount && written < maxCells; ++fi) {
        if (offs[fi] > offs[fi + 1u]) break;
        const auto spr = ani.subspan(offs[fi], offs[fi + 1u] - offs[fi]);
        if (spr.size() < 9) continue;
        const int ox = readI16LE(spr, 0);
        const int oy = readI16LE(spr, 2);
        const int fw = readI16LE(spr, 4);
        const int fh = readI16LE(spr, 6);
        const std::uint8_t tag = spr[8];
        if ((tag != 0xA1u && tag != 0xA2u) || fw <= 1 || fh <= 1 || fw > 256 || fh > 256) continue;

        const std::size_t cellIdx = startCell + written;
        const int cellLeft = int(cellIdx % sheetCols) * int(cellSize);
        const int cellTop = int(cellIdx / sheetCols) * int(cellSize);
        const bool compact = (tag == 0xA1u);
        std::size_t pos = 9u;
        bool ok = true;
        for (int y = 0; y < fh; ++y) {
            if (pos + 2u > spr.size()) { ok = false; break; }
            const unsigned rlen = readU16LE(spr, pos);
            pos += 2u;
            if (rlen > spr.size() - pos) { ok = false; break; }
            const auto row = spr.subspan(pos, rlen);
            pos += rlen;
            int x = 0;
            for (std::size_t i = 0; i < row.size();) {
                const std::uint8_t op = row[i++];
                const int cnt = op & (compact ? 127 : 63);
                if (cnt > fw - x) { ok = false; break; }
                const bool skip = !compact && (op & 192u) == 192u;
                const bool rep = (op & 128u) != 0u && !skip;
                const std::size_t used = (skip || cnt == 0) ? 0u : (rep ? 1u : std::size_t(cnt));
                if (used > row.size() - i) { ok = false; break; }
                for (int j = 0; !skip && j < cnt; ++j) {
                    const std::uint8_t palIdx = row[i + (rep ? 0u : std::size_t(j))];
                    if (compact && rep && palIdx == 0u) continue;
                    const auto c = rgbaPal[palIdx];
                    if (c[3] == 0u) continue;
                    const int lx = half + ox + x + j;
                    const int ly = half + oy + y;
                    if (lx < 1 || ly < 1 || lx >= int(cellSize) - 1 || ly >= int(cellSize) - 1) continue;
                    const int dx = cellLeft + lx;
                    const int dy = cellTop + ly;
                    if (dx < 0 || dy < 0 || dx >= int(sheet.width) || dy >= int(sheet.height)) continue;
                    auto* dst = &sheet.rgba[(std::size_t(dy) * sheet.width + std::size_t(dx)) * 4u];
                    dst[0] = c[0];
                    dst[1] = c[1];
                    dst[2] = c[2];
                    dst[3] = c[3];
                }
                i += used;
                x += cnt;
            }
            if (!ok) break;
        }
        if (ok) ++written;
    }
    return written;
}

std::uint32_t hashWeather(std::uint32_t x) noexcept {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
}

struct Hud::RetailAssets {
    static constexpr unsigned kAirSheetCols = 10u;
    static constexpr unsigned kAirSheetRows = 8u;
    static constexpr unsigned kAirCellSize = 40u;

    bool loaded = false;
    std::size_t panelRecordCount = 0;
    std::size_t pdropRecordCount = 0;

    Surface panelLeftPlate = makeNullSurface();
    Surface panelRightTray = makeNullSurface();
    Surface pdropPlate = makeNullSurface();
    Surface pdropChuteIcon = makeNullSurface();
    std::array<Surface, 4> pdropButtons{
        makeNullSurface(), makeNullSurface(), makeNullSurface(), makeNullSurface()
    };
    Surface menuBtnNormal = makeNullSurface();
    Surface menuBtnPressed = makeNullSurface();
    Surface pauseBtnNormal = makeNullSurface();
    Surface pauseBtnPressed = makeNullSurface();
    Surface blankBtnBevel = makeNullSurface();
    Surface medicIcon = makeNullSurface();
    Surface specialIcon = makeNullSurface();
    std::map<HudAction, std::pair<Surface, Surface>> cmdButtons;
    std::array<std::pair<Surface, Surface>, 5> airIcons{{
        {makeNullSurface(), makeNullSurface()},
        {makeNullSurface(), makeNullSurface()},
        {makeNullSurface(), makeNullSurface()},
        {makeNullSurface(), makeNullSurface()},
        {makeNullSurface(), makeNullSurface()},
    }};
    std::map<std::string, std::vector<std::string>> texts;

    assets::MapImage airVfxSheet{};
    std::size_t airFireFrames = 0;
    std::size_t airSmokeFrames = 0;
    std::optional<unsigned> airVfxSlot;

    struct PlaneTrailState {
        int lastX = 0;
        int lastY = 0;
        float distAccum = 0.0f;
        bool initialized = false;
    };
    struct AirPuff {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float age = 0.0f;
        float duration = 1.0f;
        float scale = 1.0f;
        bool isFire = false;
    };
    std::map<std::uint32_t, PlaneTrailState> planeTrails;
    std::vector<AirPuff> puffs;
};

Hud::Hud(SDL_Window* parent) : parent_(parent), retail_(std::make_unique<RetailAssets>()) {
    if (!parent_) throw std::invalid_argument("HUD needs a parent window");
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> map(pango_cairo_font_map_new(), g_object_unref);
    fonts_.reset(pango_font_map_create_context(map.get()), g_object_unref);
    buttons_ = {{HudAction::stop, "Stop", "S", {}}, {HudAction::holdFire, "Hold fire", "H", {}},
                {HudAction::crouch, "Crouch", "C", {}}, {HudAction::mission, "Mission", "M", {}},
                {HudAction::pause, "Pause", "P", {}}, {HudAction::log, "Log", "L", {}}};
    layout();
    render();
}

Hud::~Hud() = default;

bool Hud::loadRetailAssets(const std::filesystem::path& gameDir) {
    if (!retail_) retail_ = std::make_unique<RetailAssets>();
    if (gameDir.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(gameDir, ec)) return false;

    bool panelOk = false;
    bool pdropOk = false;

    const auto lngSue = assets::resolveCaseInsensitivePath(gameDir, "lng.sue");
    if (std::filesystem::exists(lngSue, ec)) {
        try {
            const auto panelBytes = assets::readSueEntry(lngSue, "panel.dat");
            std::vector<std::span<const std::uint8_t>> recs;
            if (parseOffsetRecords(panelBytes, 176u, 256u, recs)) {
                retail_->panelRecordCount = recs.size();
                const auto pal = recs[0];
                retail_->panelLeftPlate = decodeIndexedSurface(recs[2], pal);
                retail_->panelRightTray = decodeIndexedSurface(recs[4], pal);
                retail_->menuBtnNormal = decodeIndexedSurface(recs[9], pal);
                retail_->menuBtnPressed = decodeIndexedSurface(recs[10], pal);
                retail_->pauseBtnNormal = decodeIndexedSurface(recs[15], pal);
                retail_->pauseBtnPressed = decodeIndexedSurface(recs[16], pal);
                retail_->blankBtnBevel = decodeIndexedSurface(recs[35], pal);
                retail_->medicIcon = decodeIndexedSurface(recs[111], pal);
                retail_->specialIcon = decodeIndexedSurface(recs[67], pal);

                auto loadPair = [&](HudAction act, std::size_t nIdx, std::size_t pIdx) {
                    if (nIdx < recs.size() && pIdx < recs.size()) {
                        auto a = decodeIndexedSurface(recs[nIdx], pal);
                        auto b = decodeIndexedSurface(recs[pIdx], pal);
                        if (a && b) retail_->cmdButtons.insert_or_assign(act, std::pair{std::move(a), std::move(b)});
                    }
                };
                // Exact Gold 2.2 n2Game_Dll.dll (0x100fc74c / 0x100fc83c) command button mappings:
                loadPair(HudAction::stop, 61u, 62u);          // ITXT_STOP
                loadPair(HudAction::holdFire, 32u, 33u);      // ITXT_HOLDFIRE
                loadPair(HudAction::crouch, 167u, 168u);      // ITXT_FOOTCRAWL
                loadPair(HudAction::mission, 63u, 64u);       // ITXT_COVER
                loadPair(HudAction::pause, 15u, 16u);         // ITXT_PAUSE
                loadPair(HudAction::log, 13u, 14u);           // ITXT_LOAD / dispatch
                loadPair(HudAction::recon, 85u, 86u);         // ITXT_BINOCULAR
                loadPair(HudAction::attackMove, 55u, 56u);    // ITXT_AMOVE
                loadPair(HudAction::patrol, 101u, 102u);      // ITXT_PLANE_PATROL
                loadPair(HudAction::attackGround, 57u, 58u);  // ITXT_GAUBFIRE
                loadPair(HudAction::unload, 59u, 60u);        // ITXT_UNLOAD
                loadPair(HudAction::mine, 77u, 78u);          // ITXT_LAYMINE
                loadPair(HudAction::scatter, 65u, 66u);       // ITXT_SCATTER

                // Air mission icons: Bomber, Recon, Paratrooper, Supply, Fighter
                retail_->airIcons[0] = {decodeIndexedSurface(recs[95], pal), decodeIndexedSurface(recs[96], pal)};
                retail_->airIcons[1] = {decodeIndexedSurface(recs[99], pal), decodeIndexedSurface(recs[100], pal)};
                retail_->airIcons[2] = {decodeIndexedSurface(recs[97], pal), decodeIndexedSurface(recs[98], pal)};
                retail_->airIcons[3] = {decodeIndexedSurface(recs[97], pal), decodeIndexedSurface(recs[98], pal)};
                retail_->airIcons[4] = {decodeIndexedSurface(recs[103], pal), decodeIndexedSurface(recs[104], pal)};

                panelOk = retail_->panelLeftPlate != nullptr &&
                          retail_->panelRightTray != nullptr &&
                          !retail_->cmdButtons.empty();
            }
        } catch (...) {
        }

        try {
            const auto pdropBytes = assets::readSueEntry(lngSue, "pdrop.dat");
            std::vector<std::span<const std::uint8_t>> precs;
            if (parseOffsetRecords(pdropBytes, 9u, 32u, precs)) {
                retail_->pdropRecordCount = precs.size();
                const auto ppal = precs[0];
                retail_->pdropPlate = decodeIndexedSurface(precs[3], ppal);
                retail_->pdropChuteIcon = decodeIndexedSurface(precs[4], ppal);
                for (std::size_t k = 0; k < 4u; ++k) {
                    retail_->pdropButtons[k] = decodeIndexedSurface(precs[5u + k], ppal);
                }
                if (retail_->pdropButtons[2] && retail_->pdropButtons[3]) {
                    retail_->airIcons[2] = {
                        decodeIndexedSurface(precs[7], ppal),
                        decodeIndexedSurface(precs[8], ppal)
                    };
                }
                pdropOk = retail_->pdropPlate != nullptr && retail_->pdropChuteIcon != nullptr;
            }
        } catch (...) {
        }

        try {
            const auto glng = assets::readSueEntry(lngSue, "game.lng");
            parseGameLng(std::string_view(reinterpret_cast<const char*>(glng.data()), glng.size()), retail_->texts);
        } catch (...) {
        }
    }

    if (retail_->texts.empty()) {
        const auto langSue = assets::resolveCaseInsensitivePath(gameDir, "lang.sue");
        if (std::filesystem::exists(langSue, ec)) {
            try {
                const auto glng = assets::readSueEntry(langSue, "game.lng");
                parseGameLng(std::string_view(reinterpret_cast<const char*>(glng.data()), glng.size()), retail_->texts);
            } catch (...) {
            }
        }
    }

    const auto commonSue = assets::resolveCaseInsensitivePath(gameDir, "game_common.sue");
    if (std::filesystem::exists(commonSue, ec)) {
        try {
            const auto fireAni = assets::readSueEntry(commonSue, "AIRFIRE.ANI");
            const auto fireCol = assets::readSueEntry(commonSue, "AIRFIRE.COL");
            const auto smokeAni = assets::readSueEntry(commonSue, "AIRSMOKE.ANI");
            const auto smokeCol = assets::readSueEntry(commonSue, "AIRSMOKE.COL");

            const unsigned sheetW = RetailAssets::kAirSheetCols * RetailAssets::kAirCellSize;
            const unsigned sheetH = RetailAssets::kAirSheetRows * RetailAssets::kAirCellSize;
            assets::MapImage sheet{
                sheetW, sheetH,
                std::vector<std::uint8_t>(std::size_t(sheetW) * sheetH * 4u, 0u)
            };
            const std::size_t maxTotal = std::size_t(RetailAssets::kAirSheetCols) * RetailAssets::kAirSheetRows;
            const std::size_t nFire = decodeAniFramesIntoSheet(
                fireAni, fireCol, sheet, RetailAssets::kAirSheetCols, RetailAssets::kAirCellSize, 0u, 28u);
            const std::size_t nSmoke = decodeAniFramesIntoSheet(
                smokeAni, smokeCol, sheet, RetailAssets::kAirSheetCols, RetailAssets::kAirCellSize,
                nFire, maxTotal - nFire);
            if (nFire > 0 && nSmoke > 0) {
                retail_->airVfxSheet = std::move(sheet);
                retail_->airFireFrames = nFire;
                retail_->airSmokeFrames = nSmoke;
            }
        } catch (...) {
        }
    }

    retail_->loaded = panelOk && pdropOk;
    if (retail_->loaded) render();
    return retail_->loaded;
}

bool Hud::hasRetailAssets() const noexcept {
    return retail_ && retail_->loaded;
}

std::size_t Hud::panelRecordCount() const noexcept {
    return retail_ ? retail_->panelRecordCount : 0u;
}

std::size_t Hud::pdropRecordCount() const noexcept {
    return retail_ ? retail_->pdropRecordCount : 0u;
}

std::size_t Hud::retailTextCount() const noexcept {
    return retail_ ? retail_->texts.size() : 0u;
}

std::string Hud::retailText(std::string_view key, std::size_t index) const {
    if (!retail_) return {};
    const auto it = retail_->texts.find(std::string(key));
    if (it == retail_->texts.end() || index >= it->second.size()) return {};
    return it->second[index];
}

void Hud::setWeather(assets::WeatherKind kind, std::uint32_t phaseTicks) {
    if (kind == assets::WeatherKind::Clear) phaseTicks = 0;
    if (weather_ == kind && weatherPhase_ == phaseTicks) return;
    weather_ = kind;
    weatherPhase_ = phaseTicks;
    ++weatherRevision_;
}

bool Hud::hasAirVfxAtlas() const noexcept {
    return retail_ && retail_->airFireFrames > 0 && retail_->airSmokeFrames > 0 &&
           retail_->airVfxSheet.width > 0 && retail_->airVfxSheet.height > 0;
}

const assets::MapImage& Hud::airVfxAtlas() const noexcept {
    static const assets::MapImage kEmpty{};
    return retail_ ? retail_->airVfxSheet : kEmpty;
}

std::size_t Hud::airFireFrameCount() const noexcept {
    return retail_ ? retail_->airFireFrames : 0u;
}

std::size_t Hud::airSmokeFrameCount() const noexcept {
    return retail_ ? retail_->airSmokeFrames : 0u;
}

void Hud::bindAirVfxAtlasSlot(unsigned slot) noexcept {
    if (retail_) retail_->airVfxSlot = slot;
}

std::optional<unsigned> Hud::airVfxAtlasSlot() const noexcept {
    return retail_ ? retail_->airVfxSlot : std::nullopt;
}

void Hud::recordAirSortieTrail(std::uint32_t sortieId, int mapX, int mapY, unsigned dir32,
                               int hp, int maxHp, bool alive, bool active, bool tookHit) {
    if (!retail_) return;
    if (!active) {
        retail_->planeTrails.erase(sortieId);
        return;
    }
    auto& st = retail_->planeTrails[sortieId];
    const float prevX = st.initialized ? float(st.lastX) : float(mapX);
    const float prevY = st.initialized ? float(st.lastY) : float(mapY);
    const float dx = float(mapX) - prevX;
    const float dy = float(mapY) - prevY;
    const float stepDist = std::hypot(dx, dy);
    st.lastX = mapX;
    st.lastY = mapY;
    st.initialized = true;

    const bool damaged = maxHp > 0 && (hp < maxHp || !alive);
    if (!damaged && !tookHit) {
        st.distAccum = 0.0f;
        return;
    }

    const float hpRatio = maxHp > 0 ? std::clamp(float(hp) / float(maxHp), 0.0f, 1.0f) : 0.0f;
    const bool severe = !alive || hpRatio <= 0.55f;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double headingRad = (double(dir32 & 31u) / 32.0) * kTwoPi;
    // Offset slightly aft of the aircraft center along the inverse flight heading.
    const float tailX = float(mapX) - float(std::sin(headingRad)) * 10.0f;
    const float tailY = float(mapY) + float(std::cos(headingRad)) * 7.0f;

    auto pushPuff = [&](float px, float py, float vx, float vy, float dur, float sc, bool fire) {
        if (retail_->puffs.size() >= 96u) retail_->puffs.erase(retail_->puffs.begin());
        retail_->puffs.push_back({px, py, vx, vy, 0.0f, dur, sc, fire});
    };

    if (tookHit) {
        pushPuff(tailX, tailY, -dx * 0.25f, -8.0f, 0.55f, 1.25f, true);
        pushPuff(tailX + 3.0f, tailY - 2.0f, 4.0f, -12.0f, 1.15f, 1.30f, false);
    }

    st.distAccum += stepDist;
    const float spacing = severe ? 11.0f : 18.0f;
    if (st.distAccum >= spacing || (stepDist == 0.0f && tookHit)) {
        st.distAccum = std::fmod(st.distAccum, spacing);
        const std::uint32_t jitter = hashWeather(sortieId * 2654435761u ^ std::uint32_t(mapX * 73856093 ^ mapY * 19349663));
        const float jx = (float(jitter & 0xFFu) / 255.0f - 0.5f) * 6.0f;
        const float jy = (float((jitter >> 8u) & 0xFFu) / 255.0f - 0.5f) * 4.0f;
        pushPuff(tailX + jx, tailY + jy, jx * 0.8f, -10.0f - (severe ? 4.0f : 0.0f),
                 severe ? 1.25f : 0.95f, severe ? 1.25f : 0.95f, false);
        if (severe) {
            pushPuff(tailX + jx * 0.4f, tailY + jy * 0.4f, jx * 0.4f, -6.0f, 0.48f, 1.15f, true);
        }
    }
}

void Hud::recordAirCrashTrail(int mapX, int mapY) {
    if (!retail_) return;
    for (int k = 0; k < 6; ++k) {
        const std::uint32_t h = hashWeather(std::uint32_t(mapX * 31 + mapY * 17 + k * 97));
        const float ox = (float(h & 0xFFu) / 255.0f - 0.5f) * 22.0f;
        const float oy = (float((h >> 8u) & 0xFFu) / 255.0f - 0.5f) * 14.0f;
        if (retail_->puffs.size() >= 96u) retail_->puffs.erase(retail_->puffs.begin());
        retail_->puffs.push_back({
            float(mapX) + ox, float(mapY) + oy,
            ox * 0.5f, -14.0f - float(k) * 2.0f,
            0.0f, 1.35f + 0.12f * float(k), 1.45f, (k % 2) == 0
        });
    }
}

bool Hud::stepAirTrails(float dtSeconds) {
    if (!retail_ || retail_->puffs.empty()) return false;
    const float dt = std::clamp(dtSeconds, 0.0f, 0.25f);
    if (dt <= 0.0f) return false;
    for (auto& p : retail_->puffs) {
        p.age += dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
    }
    std::erase_if(retail_->puffs, [](const RetailAssets::AirPuff& p) {
        return p.age >= p.duration;
    });
    return true;
}

bool Hud::hasActiveAirTrails() const noexcept {
    return retail_ && !retail_->puffs.empty();
}

void Hud::appendAirTrailSprites(int mapScale, std::vector<Sprite>& outSprites) const {
    if (!retail_ || !retail_->airVfxSlot || retail_->puffs.empty()) return;
    if (retail_->airFireFrames == 0 || retail_->airSmokeFrames == 0) return;

    const float s = float(std::max(1, mapScale));
    const unsigned slot = *retail_->airVfxSlot;
    const unsigned cols = RetailAssets::kAirSheetCols;
    const float cell = float(RetailAssets::kAirCellSize);

    // Draw trailing smoke puffs first, then bright fire tongues on top.
    for (bool firePass : {false, true}) {
        for (const auto& p : retail_->puffs) {
            if (p.isFire != firePass) continue;
            const float t = std::clamp(p.age / std::max(0.05f, p.duration), 0.0f, 0.999f);
            const std::size_t frameCount = firePass ? retail_->airFireFrames : retail_->airSmokeFrames;
            const std::size_t baseCell = firePass ? 0u : retail_->airFireFrames;
            const std::size_t cellIdx = baseCell + std::min(frameCount - 1u, std::size_t(t * float(frameCount)));
            const float u0 = float(cellIdx % cols) * cell + 1.0f;
            const float v0 = float(cellIdx / cols) * cell + 1.0f;
            const float u1 = u0 + cell - 2.0f;
            const float v1 = v0 + cell - 2.0f;
            const float growth = firePass ? (1.0f + 0.15f * t) : (0.90f + 0.55f * t);
            const float dst = (cell - 2.0f) * p.scale * growth * s;
            const float alpha = firePass ? std::clamp(1.0f - 0.65f * t, 0.15f, 1.0f)
                                         : std::clamp(0.88f * (1.0f - t), 0.08f, 0.88f);
            outSprites.push_back({
                p.x * s - dst * 0.5f,
                p.y * s - dst * 0.5f,
                dst,
                dst,
                u0, v0, u1, v1,
                slot,
                1.0f,
                alpha
            });
        }
    }
}

void Hud::appendAirTrailBeams(int mapScale, std::size_t maxCount, std::vector<MapBeam>& outBeams) const {
    if (!retail_ || retail_->puffs.empty() || maxCount == 0u) return;
    const float s = float(std::max(1, mapScale));
    std::size_t added = 0;
    const std::size_t stride = std::max<std::size_t>(1u, (retail_->puffs.size() + maxCount - 1u) / maxCount);
    for (std::size_t i = 0; i < retail_->puffs.size() && added < maxCount; i += stride) {
        const auto& p = retail_->puffs[i];
        const float t = std::clamp(p.age / std::max(0.05f, p.duration), 0.0f, 1.0f);
        const float wx = p.x * s;
        const float wy = p.y * s;
        if (p.isFire) {
            const float fade = (1.0f - t) * 0.82f;
            outBeams.push_back({wx, wy, wx, wy - 4.0f * s, 7.5f * p.scale * s, 255, 145, 40, fade, fade * 0.35f});
        } else {
            const float fade = (1.0f - t) * 0.46f;
            outBeams.push_back({wx, wy, wx, wy - (8.0f + 10.0f * t) * s,
                                (8.0f + 7.0f * t) * p.scale * s, 60, 55, 50, fade, fade * 0.20f});
        }
        ++added;
    }
}

void Hud::setMinimap(const assets::MapImage& overview) {
    if (!overview.width || !overview.height || std::uint64_t(overview.width) * overview.height * 4 != overview.rgba.size())
        throw std::invalid_argument("Unsupported minimap dimensions or pixel count");
    overview_ = overview;
    layout();
    render();
}

void Hud::setPortrait(const std::string& label, const assets::MapImage& portrait) {
    if (!portrait.width || !portrait.height || std::uint64_t(portrait.width) * portrait.height * 4 != portrait.rgba.size())
        throw std::invalid_argument("Unsupported portrait dimensions or pixel count");
    portraits_[label] = portrait;
    render();
}

void Hud::log(std::string message) {
    if (message.empty()) return;
    // One line per entry; a few hundred entries are plenty for a session's review.
    // Line breaks of either kind become spaces; a CR LF pair becomes one space.
    std::string flat;
    for (char c : message) {
        if (c == '\r' || c == '\n' || c == '\t') {
            if (!flat.empty() && flat.back() != ' ') flat += ' ';
        } else flat += c;
    }
    while (!flat.empty() && flat.back() == ' ') flat.pop_back();
    if (flat.empty()) return;
    message = std::move(flat);
    messages_.push_back(std::move(message));
    if (messages_.size() > 400) messages_.erase(messages_.begin(), messages_.begin() + 100);
    render();
}

std::string Hud::logText(std::size_t newest) const {
    std::string out;
    const std::size_t first = messages_.size() > newest ? messages_.size() - newest : 0;
    for (std::size_t i = first; i < messages_.size(); ++i) {
        if (!out.empty()) out += '\n';
        out += messages_[i];
    }
    return out;
}

// The bar must not rely on being handed the resize event: a caller that routes events
// elsewhere while a dialog is open would leave it laid out for the old window, drawn at
// the wrong size and swallowing clicks in the wrong place.
void Hud::refit() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) return;
    if (unsigned(std::max(pixelW, 1)) == width_ && std::max(pixelH, 1) == top_ + int(height_)) {
        // Same size: only a change of interface language needs a new picture.
        if (int(language()) != language_) render();
        return;
    }
    pressed_.reset();
    hovered_.reset();
    pressedAir_.reset();
    hoveredAir_.reset();
    pressedGroup_.reset();
    hoveredGroup_.reset();
    pressedClass_.reset();
    hoveredClass_.reset();
    draggingMinimap_ = false;
    layout();
    render();
    ++weatherRevision_;
}

void Hud::update(const HudState& state) {
    refit();
    for (const auto& dot : state.dots)
        if (!std::isfinite(dot.u) || !std::isfinite(dot.v)) throw std::invalid_argument("HUD dot has a non-finite position");
    for (const auto& plane : state.planes)
        if (!std::isfinite(plane.u) || !std::isfinite(plane.v)) throw std::invalid_argument("HUD plane has a non-finite position");
    for (float v : {state.viewU, state.viewV, state.viewW, state.viewH})
        if (!std::isfinite(v)) throw std::invalid_argument("HUD view rectangle has a non-finite value");
    if (state.hasObjectiveMarker && (!std::isfinite(state.objectiveU) || !std::isfinite(state.objectiveV)))
        throw std::invalid_argument("HUD objective marker has a non-finite position");
    if (state == state_) return;
    state_ = state;
    render();
}

Hud::Rect Hud::row3Rect(int col) const {
    if (buttons_.empty() || col < 0 || col > 2) return {};
    const int bw = buttons_[0].rect.width, bh = buttons_[0].rect.height;
    return {buttons_[0].rect.x + col * (bw + 2), buttons_[0].rect.y + 2 * (bh + 2), bw, bh};
}

Hud::Rect Hud::reconButtonRect() const {
    return row3Rect(0);
}

std::optional<HudAction> Hud::row3Action(int col) const {
    const bool hasUnits = state_.hasSelection || !state_.selection.empty();
    if (!hasUnits) return std::nullopt;
    if (col == 0) return hasOfficerSelection(state_) ? HudAction::recon : HudAction::attackMove;
    if (col == 1) {
        if (state_.hasCargo) return HudAction::unload;
        if (state_.hasSapper) return HudAction::mine;
        if (hasRole(state_, {"medic"})) return std::nullopt;
        return HudAction::patrol;
    }
    if (col == 2) {
        if (state_.hasBombard) return HudAction::attackGround;
        if (row3Action(1) != HudAction::patrol) return HudAction::patrol;
        if (state_.hasInfantry) return HudAction::scatter;
    }
    return std::nullopt;
}

Hud::Rect Hud::buttonRect(HudAction action) const {
    for (const auto& b : buttons_) if (b.action == action) return b.rect;
    for (int col = 0; col < 3; ++col) {
        if (row3Action(col) == action) return row3Rect(col);
    }
    return {};
}

Hud::Rect Hud::airButtonRect(int slot) const {
    if (slot < 0 || slot >= 5 || state_.airReady[std::size_t(slot)] <= 0 || readout_.width <= 40) return {};
    const int pad = 6;
    const int leftBlockW = std::clamp(readout_.x - 4, minimap_.x + minimap_.width + pad, int(width_));
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, int(height_));
    const int sx = leftBlockW + 2, sw = std::max(1, int(width_) - sx - 2);
    const int size = std::clamp(strip_ * 3 / 5, 9, 14);
    const int pillH = std::max(12, strip_ - 7);
    const int pillY = trayTop + 4;
    static constexpr const char* kLetters[5] = {"B", "R", "P", "S", "F"};
    int rightEdge = sx + sw - 4;
    for (int k = 4; k >= 0; --k) {
        const int ready = state_.airReady[std::size_t(k)];
        if (ready <= 0) continue;
        const std::string label = std::string(kLetters[k]) + ":" + std::to_string(ready);
        const int pillW = std::max(34, int(label.size()) * size * 2 / 3 + size);
        const int pillX = rightEdge - pillW;
        if (pillX < sx + sw / 2) break;
        if (k == slot) return {pillX, pillY, pillW, pillH};
        rightEdge = pillX - 4;
    }
    return {};
}

Hud::Rect Hud::groupChipRect(unsigned digit) const {
    if (readout_.width <= 40 || state_.groups.empty()) return {};
    const int pad = 6;
    const int leftBlockW = std::clamp(readout_.x - 4, minimap_.x + minimap_.width + pad, int(width_));
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, int(height_));
    const int sx = leftBlockW + 2, sw = std::max(1, int(width_) - sx - 2);
    const int size = std::clamp(strip_ * 3 / 5, 9, 14);
    const int half = sw / 2;
    int chipX = sx + half + 4;
    const int chipH = std::max(8, strip_ - 7);
    for (const auto& group : state_.groups) {
        const std::string label = std::to_string(group.digit) + ":" + std::to_string(group.count);
        const int chipW = int(label.size()) * size * 2 / 3 + size;
        if (chipX + chipW > sx + sw * 3 / 4) break;
        if (group.digit == digit) return {chipX, trayTop + 5, chipW, chipH};
        chipX += chipW + 4;
    }
    return {};
}

Hud::Rect Hud::classCardRect(std::size_t index) const {
    if (readout_.width <= 40 || index >= state_.selection.size()) return {};
    const int full = int(height_);
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, full);
    const int trayH = full - trayTop;
    const int small = std::clamp(trayH / 7, 9, 15);
    const int inX = readout_.x + 8;
    const int inW = std::max(1, readout_.width - 16);
    const int cardsY = readout_.y + small + 3;
    const int cardsH = readout_.y + readout_.height - cardsY - 1;
    if (cardsH < 14) return {};
    const int portrait = cardsH - 4, gap = 4;
    const int widest = std::max(portrait + small * 8, 112);
    const int narrowest = std::max(portrait + small * 5, 82);
    const int classes = int(state_.selection.size());
    const int cardW = std::clamp((inW - (classes - 1) * gap) / classes, narrowest, widest);
    const int fit = std::max(1, (inW + gap) / (cardW + gap));
    if (int(index) >= fit) return {};
    if (int(index) == fit - 1 && classes > fit) return {};
    return {inX + int(index) * (cardW + gap), cardsY, cardW, cardsH};
}

void Hud::layout() {
    int pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH)) throw std::runtime_error(SDL_GetError());
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);
    width_ = unsigned(pixelW);
    // Classic Sudden Strike II bottom-left L-shaped console height (~22% of screen, bounded <= H/3).
    const int full = std::clamp(int(std::lround(pixelH * 0.22)), std::min(110, pixelH), std::min(260, std::max(110, pixelH / 3)));
    height_ = unsigned(full);
    strip_ = std::clamp(full / 7, 16, 22);
    top_ = pixelH - full;
    const int pad = 6;

    // 1. Far-left: Isometric diamond minimap in armored steel housing (2:1 aspect, <= 26% of width).
    const int mapH = std::max(24, full - 2 * pad);
    const double aspect = overview_.width && overview_.height ? double(overview_.width) / overview_.height : 2.0;
    int mapW = int(std::lround(mapH * aspect)), fittedH = mapH;
    const int widest = std::max(24, pixelW * 26 / 100);
    if (mapW > widest) {
        mapW = widest;
        fittedH = std::max(16, int(std::lround(mapW / aspect)));
    }
    minimap_ = {pad, pad + (mapH - fittedH) / 2, mapW, fittedH};

    // 2. Immediately right of minimap: classic 3x3 square metallic command grid.
    const int count = int(buttons_.size());
    const int rows = 3;
    const int perRow = 3;
    const int gap = 2;
    const int gridH = full - 2 * pad;
    const int buttonH = std::max(20, (gridH - (rows - 1) * gap) / rows);
    const int maxGridW = std::max(perRow * 22, pixelW * 18 / 100);
    const int maxCellW = std::max(20, (maxGridW - (perRow - 1) * gap) / perRow);
    const int buttonW = std::clamp(buttonH, 20, maxCellW);
    const int blockW = perRow * buttonW + (perRow - 1) * gap;
    const int startX = minimap_.x + minimap_.width + pad;
    for (int i = 0; i < count; ++i) {
        const int row = i / perRow, column = i % perRow;
        buttons_[i].rect = {startX + column * (buttonW + gap), pad + row * (buttonH + gap), buttonW, buttonH};
    }

    // 3. Right of the tall left cluster: slim horizontal unit/status tray flush with bottom edge.
    //    Everything above trayTop on the right is transparent (alpha=0) so live battlefield shows through.
    const int leftBlockW = startX + blockW + pad;
    const int trayH = std::clamp(int(std::lround(full * 0.54)), std::min(72, full), std::max(72, std::min(126, full - 24)));
    const int trayTop = full - trayH;
    const int meterH = 10;
    readout_ = {leftBlockW + 4, trayTop + strip_ + 1, std::max(0, pixelW - leftBlockW - 8),
                std::max(18, trayH - strip_ - meterH - 3)};
}

namespace {
void bevelFrame(cairo_t* cr, double x, double y, double w, double h, bool inset) {
    if (w <= 2 || h <= 2) return;
    // Crisp metallic highlight on top/left (raised) or bottom/right (inset)
    cairo_set_line_width(cr, 1.5);
    if (!inset) colour(cr, 186, 194, 182); else colour(cr, 16, 20, 18);
    cairo_move_to(cr, x + 0.75, y + h - 0.75);
    cairo_line_to(cr, x + 0.75, y + 0.75);
    cairo_line_to(cr, x + w - 0.75, y + 0.75);
    cairo_stroke(cr);
    if (!inset) colour(cr, 16, 20, 18); else colour(cr, 162, 170, 158);
    cairo_move_to(cr, x + w - 0.75, y + 0.75);
    cairo_line_to(cr, x + w - 0.75, y + h - 0.75);
    cairo_line_to(cr, x + 0.75, y + h - 0.75);
    cairo_stroke(cr);
}

void drawRivet(cairo_t* cr, double cx, double cy, double r) {
    colour(cr, 18, 22, 20);
    cairo_arc(cr, cx + 0.6, cy + 0.6, r, 0, 2 * G_PI);
    cairo_fill(cr);
    colour(cr, 128, 136, 124);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill(cr);
    colour(cr, 204, 210, 198);
    cairo_arc(cr, cx - r * 0.3, cy - r * 0.3, r * 0.4, 0, 2 * G_PI);
    cairo_fill(cr);
}

void drawCompassRose(cairo_t* cr, double cx, double cy, double r) {
    colour(cr, 16, 20, 16);
    cairo_arc(cr, cx, cy, r * 0.32, 0, 2 * G_PI);
    cairo_fill(cr);
    colour(cr, 214, 198, 130);
    cairo_set_line_width(cr, 1.3);
    cairo_move_to(cr, cx, cy - r); cairo_line_to(cr, cx, cy + r);
    cairo_move_to(cr, cx - r, cy); cairo_line_to(cr, cx + r, cy);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r);
    cairo_line_to(cr, cx - r * 0.22, cy - r * 0.35);
    cairo_line_to(cr, cx + r * 0.22, cy - r * 0.35);
    cairo_close_path(cr);
    cairo_fill(cr);
}
void drawDiamondPath(cairo_t* cr, double x, double y, double w, double h) {
    const double cx = x + w * 0.5, cy = y + h * 0.5;
    cairo_new_sub_path(cr);
    cairo_move_to(cr, cx, y);
    cairo_line_to(cr, x + w, cy);
    cairo_line_to(cr, cx, y + h);
    cairo_line_to(cr, x, cy);
    cairo_close_path(cr);
}

void drawPowerSymbol(cairo_t* cr, double cx, double cy, double r) {
    colour(cr, 188, 196, 184);
    cairo_set_line_width(cr, 1.4);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, -G_PI * 0.25, G_PI * 1.25);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r * 1.25);
    cairo_line_to(cr, cx, cy - r * 0.15);
    cairo_stroke(cr);
}

void drawCommandIcon(cairo_t* cr, HudAction action, double cx, double cy, double r, bool active) {
    if (r <= 1.0) return;
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const double lw = std::clamp(r * 0.22, 1.25, 2.6);
    const auto strokeShadow = [&](auto&& drawPath) {
        cairo_save(cr);
        cairo_translate(cr, 0.7, 0.7);
        colour(cr, 10, 13, 11, 0.8);
        cairo_set_line_width(cr, lw);
        drawPath();
        cairo_stroke(cr);
        cairo_restore(cr);
    };
    const auto mainTone = [&]() {
        if (active) colour(cr, 238, 244, 174);
        else colour(cr, 218, 224, 210);
    };

    switch (action) {
    case HudAction::stop: {
        // Octagonal halt emblem with crisp inner halt square
        const double rad = r * 0.88;
        auto octagon = [&](double ox, double oy) {
            cairo_new_sub_path(cr);
            for (int k = 0; k < 8; ++k) {
                const double a = G_PI / 8.0 + k * (G_PI / 4.0);
                const double px = ox + std::cos(a) * rad, py = oy + std::sin(a) * rad;
                if (k == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
            }
            cairo_close_path(cr);
        };
        colour(cr, 10, 13, 11, 0.75);
        octagon(cx + 0.7, cy + 0.7);
        cairo_fill(cr);
        if (active) colour(cr, 198, 72, 60); else colour(cr, 146, 50, 44);
        octagon(cx, cy);
        cairo_fill_preserve(cr);
        mainTone();
        cairo_set_line_width(cr, std::max(1.0, lw * 0.7));
        cairo_stroke(cr);
        const double sq = r * 0.36;
        colour(cr, 236, 238, 226);
        cairo_rectangle(cr, cx - sq, cy - sq, sq * 2.0, sq * 2.0);
        cairo_fill(cr);
        break;
    }
    case HudAction::holdFire: {
        // Crosshair reticle with bold diagonal slash
        const double ringR = r * 0.68, outerR = r * 0.94, innerR = r * 0.34;
        auto crosshair = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, ringR, 0, 2 * G_PI);
            cairo_move_to(cr, cx - outerR, cy); cairo_line_to(cr, cx - innerR, cy);
            cairo_move_to(cr, cx + innerR, cy); cairo_line_to(cr, cx + outerR, cy);
            cairo_move_to(cr, cx, cy - outerR); cairo_line_to(cr, cx, cy - innerR);
            cairo_move_to(cr, cx, cy + innerR); cairo_line_to(cr, cx, cy + outerR);
        };
        strokeShadow(crosshair);
        mainTone();
        cairo_set_line_width(cr, std::max(1.1, lw * 0.85));
        crosshair();
        cairo_stroke(cr);
        const double s = r * 0.78;
        colour(cr, 12, 15, 13, 0.85);
        cairo_set_line_width(cr, lw * 1.55);
        cairo_move_to(cr, cx - s, cy + s); cairo_line_to(cr, cx + s, cy - s);
        cairo_stroke(cr);
        if (active) colour(cr, 240, 96, 74); else colour(cr, 212, 86, 72);
        cairo_set_line_width(cr, lw * 1.05);
        cairo_move_to(cr, cx - s, cy + s); cairo_line_to(cr, cx + s, cy - s);
        cairo_stroke(cr);
        break;
    }
    case HudAction::crouch: {
        // Downward kneeling/shield chevron over ground baseline
        auto chevron = [&]() {
            cairo_move_to(cr, cx - r * 0.74, cy - r * 0.56);
            cairo_line_to(cr, cx, cy + r * 0.08);
            cairo_line_to(cr, cx + r * 0.74, cy - r * 0.56);
            cairo_move_to(cr, cx - r * 0.58, cy - r * 0.04);
            cairo_line_to(cr, cx, cy + r * 0.52);
            cairo_line_to(cr, cx + r * 0.58, cy - r * 0.04);
            cairo_move_to(cr, cx - r * 0.68, cy + r * 0.78);
            cairo_line_to(cr, cx + r * 0.68, cy + r * 0.78);
        };
        strokeShadow(chevron);
        mainTone();
        cairo_set_line_width(cr, lw);
        chevron();
        cairo_stroke(cr);
        break;
    }
    case HudAction::mission: {
        // Tactical target crosshair ring with central military star
        const double ringR = r * 0.82, tickIn = r * 0.64, tickOut = r * 0.98;
        auto reticle = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, ringR, 0, 2 * G_PI);
            cairo_move_to(cr, cx, cy - tickOut); cairo_line_to(cr, cx, cy - tickIn);
            cairo_move_to(cr, cx, cy + tickIn);  cairo_line_to(cr, cx, cy + tickOut);
            cairo_move_to(cr, cx - tickOut, cy); cairo_line_to(cr, cx - tickIn, cy);
            cairo_move_to(cr, cx + tickIn, cy);  cairo_line_to(cr, cx + tickOut, cy);
        };
        strokeShadow(reticle);
        colour(cr, 186, 194, 178);
        cairo_set_line_width(cr, std::max(1.0, lw * 0.72));
        reticle();
        cairo_stroke(cr);
        const double outer = r * 0.54, inner = r * 0.22;
        cairo_new_sub_path(cr);
        for (int k = 0; k < 10; ++k) {
            const double rad = (k % 2 == 0) ? outer : inner;
            const double a = -G_PI * 0.5 + k * (G_PI / 5.0);
            const double px = cx + std::cos(a) * rad, py = cy + std::sin(a) * rad;
            if (k == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
        }
        cairo_close_path(cr);
        if (active) colour(cr, 244, 224, 138); else colour(cr, 218, 194, 114);
        cairo_fill_preserve(cr);
        colour(cr, 18, 20, 16, 0.7);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);
        break;
    }
    case HudAction::pause: {
        // Dual vertical machined bars
        const double barW = std::max(2.0, r * 0.34), barH = r * 1.44, gap = std::max(2.0, r * 0.32);
        const double y0 = cy - barH * 0.5;
        const double lx = cx - gap * 0.5 - barW, rx = cx + gap * 0.5;
        colour(cr, 10, 13, 11, 0.75);
        cairo_rectangle(cr, lx + 0.8, y0 + 0.8, barW, barH);
        cairo_rectangle(cr, rx + 0.8, y0 + 0.8, barW, barH);
        cairo_fill(cr);
        mainTone();
        cairo_rectangle(cr, lx, y0, barW, barH);
        cairo_rectangle(cr, rx, y0, barW, barH);
        cairo_fill(cr);
        break;
    }
    case HudAction::log: {
        // Horizontal dispatch / field-radio lines with signal bullets
        const double x0 = cx - r * 0.82, xBar = cx - r * 0.38, xEnd = cx + r * 0.82;
        const double ys[3] = {cy - r * 0.48, cy, cy + r * 0.48};
        const double ends[3] = {xEnd, xEnd - r * 0.14, xEnd - r * 0.30};
        const double dot = std::max(1.4, lw * 0.85);
        auto lines = [&]() {
            for (int k = 0; k < 3; ++k) {
                cairo_move_to(cr, xBar, ys[k]);
                cairo_line_to(cr, ends[k], ys[k]);
            }
        };
        strokeShadow(lines);
        for (int k = 0; k < 3; ++k) {
            if (k == 0) colour(cr, 218, 198, 126); else mainTone();
            cairo_rectangle(cr, x0, ys[k] - dot * 0.5, dot, dot);
            cairo_fill(cr);
            cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
            cairo_move_to(cr, xBar, ys[k]);
            cairo_line_to(cr, ends[k], ys[k]);
            cairo_stroke(cr);
        }
        break;
    }
    case HudAction::recon: {
        auto bino = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx - r * 0.48, cy, r * 0.44, 0, 2 * G_PI);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx + r * 0.48, cy, r * 0.44, 0, 2 * G_PI);
            cairo_move_to(cr, cx - r * 0.14, cy - r * 0.22);
            cairo_line_to(cr, cx + r * 0.14, cy - r * 0.22);
        };
        strokeShadow(bino);
        mainTone();
        cairo_set_line_width(cr, std::max(1.2, lw * 0.9));
        bino();
        cairo_stroke(cr);
        break;
    }
    case HudAction::attackMove: {
        // Tactical thrust arrow toward forward combat pip
        auto thrust = [&]() {
            cairo_move_to(cr, cx - r * 0.82, cy + r * 0.48);
            cairo_line_to(cr, cx + r * 0.62, cy - r * 0.48);
            cairo_move_to(cr, cx + r * 0.12, cy - r * 0.56);
            cairo_line_to(cr, cx + r * 0.68, cy - r * 0.52);
            cairo_line_to(cr, cx + r * 0.46, cy - r * 0.02);
        };
        strokeShadow(thrust);
        colour(cr, active ? 242 : 226, active ? 132 : 112, active ? 92 : 84);
        cairo_set_line_width(cr, lw);
        thrust();
        cairo_stroke(cr);
        break;
    }
    case HudAction::patrol: {
        // Bidirectional alternating patrol sweep arrows
        auto loop = [&]() {
            cairo_move_to(cr, cx - r * 0.78, cy - r * 0.34);
            cairo_line_to(cr, cx + r * 0.68, cy - r * 0.34);
            cairo_move_to(cr, cx + r * 0.34, cy - r * 0.64);
            cairo_line_to(cr, cx + r * 0.74, cy - r * 0.34);
            cairo_line_to(cr, cx + r * 0.34, cy - r * 0.04);
            cairo_move_to(cr, cx + r * 0.78, cy + r * 0.34);
            cairo_line_to(cr, cx - r * 0.68, cy + r * 0.34);
            cairo_move_to(cr, cx - r * 0.34, cy + r * 0.04);
            cairo_line_to(cr, cx - r * 0.74, cy + r * 0.34);
            cairo_line_to(cr, cx - r * 0.34, cy + r * 0.64);
        };
        strokeShadow(loop);
        mainTone();
        cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
        loop();
        cairo_stroke(cr);
        break;
    }
    case HudAction::attackGround: {
        // Ballistic arc & starburst ground impact reticle
        auto shell = [&]() {
            cairo_move_to(cr, cx - r * 0.78, cy + r * 0.25);
            cairo_curve_to(cr, cx - r * 0.35, cy - r * 0.85, cx + r * 0.30, cy - r * 0.75, cx + r * 0.56, cy + r * 0.16);
            cairo_move_to(cr, cx + r * 0.22, cy + r * 0.42);
            cairo_line_to(cr, cx + r * 0.86, cy + r * 0.42);
            cairo_move_to(cr, cx + r * 0.54, cy + r * 0.10);
            cairo_line_to(cr, cx + r * 0.54, cy + r * 0.74);
        };
        strokeShadow(shell);
        colour(cr, active ? 244 : 230, active ? 186 : 164, active ? 96 : 86);
        cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
        shell();
        cairo_stroke(cr);
        break;
    }
    case HudAction::unload: {
        // Open transport hull + disembark arrow
        auto hatch = [&]() {
            cairo_move_to(cr, cx - r * 0.68, cy - r * 0.56);
            cairo_line_to(cr, cx - r * 0.68, cy + r * 0.56);
            cairo_line_to(cr, cx - r * 0.08, cy + r * 0.56);
            cairo_move_to(cr, cx - r * 0.68, cy - r * 0.56);
            cairo_line_to(cr, cx - r * 0.08, cy - r * 0.56);
            cairo_move_to(cr, cx - r * 0.28, cy);
            cairo_line_to(cr, cx + r * 0.76, cy);
            cairo_move_to(cr, cx + r * 0.40, cy - r * 0.34);
            cairo_line_to(cr, cx + r * 0.78, cy);
            cairo_line_to(cr, cx + r * 0.40, cy + r * 0.34);
        };
        strokeShadow(hatch);
        mainTone();
        cairo_set_line_width(cr, std::max(1.1, lw * 0.88));
        hatch();
        cairo_stroke(cr);
        break;
    }
    case HudAction::mine: {
        // Anti-tank circular mine housing with central detonator pressure cap
        auto mine = [&]() {
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, r * 0.68, 0, 2 * G_PI);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, r * 0.26, 0, 2 * G_PI);
        };
        strokeShadow(mine);
        colour(cr, active ? 244 : 216, active ? 222 : 196, active ? 132 : 114);
        cairo_set_line_width(cr, std::max(1.15, lw * 0.9));
        mine();
        cairo_stroke(cr);
        break;
    }
    case HudAction::scatter: {
        // Four outward-pointing arrows representing emergency disperse
        auto arrows = [&]() {
            static constexpr double dirs[4][2] = {{1, -1}, {-1, -1}, {1, 1}, {-1, 1}};
            for (const auto& d : dirs) {
                const double sx0 = cx + d[0] * r * 0.16, sy0 = cy + d[1] * r * 0.16;
                const double ex = cx + d[0] * r * 0.72, ey = cy + d[1] * r * 0.72;
                cairo_move_to(cr, sx0, sy0);
                cairo_line_to(cr, ex, ey);
                cairo_move_to(cr, ex - d[0] * r * 0.34, ey);
                cairo_line_to(cr, ex, ey);
                cairo_line_to(cr, ex, ey - d[1] * r * 0.34);
            }
        };
        strokeShadow(arrows);
        mainTone();
        cairo_set_line_width(cr, std::max(1.05, lw * 0.85));
        arrows();
        cairo_stroke(cr);
        break;
    }
    default:
        break;
    }
    cairo_restore(cr);
}
} // namespace

void Hud::render() {
    language_ = int(language());
    const int w = int(width_), full = int(height_);
    Surface target(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, full), cairo_surface_destroy);
    Context graphics(cairo_create(target.get()), cairo_destroy);
    auto* cr = graphics.get();

    // Start fully transparent so the upper-right L-cutout reveals the live battlefield.
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const int pad = 6;
    const int leftBlockW = std::clamp(readout_.x - 4, minimap_.x + minimap_.width + pad, w);
    const int trayTop = std::clamp(readout_.y - strip_ - 2, 0, full);
    const int trayH = full - trayTop;
    const int small = std::clamp(trayH / 7, 9, 15), body = std::clamp(trayH / 6, 10, 18);

    // L-shaped weathered gunmetal-steel console plate with machined horizontal grain & steel bevels.
    {
        auto fillSteelPlate = [&](int rx, int ry, int rw, int rh) {
            if (rw <= 0 || rh <= 0) return;
            cairo_save(cr);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_clip(cr);
            auto* gradient = cairo_pattern_create_linear(0, ry, 0, ry + rh);
            cairo_pattern_add_color_stop_rgb(gradient, 0, 56 / 255., 62 / 255., 54 / 255.);
            cairo_pattern_add_color_stop_rgb(gradient, 0.35, 36 / 255., 41 / 255., 36 / 255.);
            cairo_pattern_add_color_stop_rgb(gradient, 1, 20 / 255., 24 / 255., 21 / 255.);
            cairo_set_source(cr, gradient);
            cairo_paint(cr);
            cairo_pattern_destroy(gradient);
            cairo_set_line_width(cr, 1.0);
            for (int gy = ry + 2; gy < ry + rh; gy += 3) {
                if ((gy % 6) == 0) colour(cr, 66, 72, 64);
                else colour(cr, 26, 30, 27);
                cairo_move_to(cr, rx, gy + 0.5);
                cairo_line_to(cr, rx + rw, gy + 0.5);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
        };
        // Tall left armored cluster (minimap + 3x3 command grid)
        fillSteelPlate(0, 0, leftBlockW, full);
        // Low right horizontal unit/status tray
        fillSteelPlate(leftBlockW, trayTop, w - leftBlockW, trayH);

        if (retail_ && retail_->loaded) {
            if (retail_->panelRightTray) {
                tileSurfaceRegion(cr, retail_->panelRightTray.get(), 0, 0, leftBlockW, full, 0.55);
                tileSurfaceRegion(cr, retail_->panelRightTray.get(), leftBlockW, trayTop, w - leftBlockW, trayH, 0.62);
            }
            if (retail_->panelLeftPlate) {
                drawSurfaceCrisp(cr, retail_->panelLeftPlate.get(), 0.0, 0.0, double(leftBlockW), double(full), 0.48);
            }
        }

        // Crisp 2px bright steel outer rim along the entire L-shaped top/step perimeter
        colour(cr, 198, 206, 192);
        cairo_rectangle(cr, 0, 0, leftBlockW, 2);
        cairo_rectangle(cr, leftBlockW - 2, 0, 2, trayTop + 2);
        cairo_rectangle(cr, leftBlockW - 2, trayTop, w - leftBlockW + 2, 2);
        cairo_fill(cr);
        colour(cr, 16, 20, 18);
        cairo_rectangle(cr, 0, 2, leftBlockW - 2, 1);
        cairo_rectangle(cr, leftBlockW, trayTop + 2, w - leftBlockW, 1);
        cairo_fill(cr);
    }

    // 1. Isometric diamond minimap in dark riveted steel housing + brass compass rose + corner buttons.
    {
        const auto m = minimap_;
        colour(cr, 16, 20, 17);
        cairo_rectangle(cr, m.x - 2, m.y - 2, m.width + 4, m.height + 4);
        cairo_fill(cr);
        bevelFrame(cr, m.x - 3, m.y - 3, m.width + 6, m.height + 6, true);

        // Dark charcoal diamond well
        colour(cr, 10, 13, 12);
        drawDiamondPath(cr, m.x, m.y, m.width, m.height);
        cairo_fill(cr);

        if (overview_.width) {
            const auto picture = image(overview_);
            cairo_save(cr);
            drawDiamondPath(cr, m.x, m.y, m.width, m.height);
            cairo_clip(cr);
            cairo_translate(cr, m.x, m.y);
            cairo_scale(cr, double(m.width) / overview_.width, double(m.height) / overview_.height);
            cairo_set_source_surface(cr, picture.get(), 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        // Dots and camera view rectangle clipped inside the diamond
        cairo_save(cr);
        drawDiamondPath(cr, m.x, m.y, m.width, m.height);
        cairo_clip(cr);
        const double radius = std::max(1.5, full / 75.0);
        for (bool hostile : {false, true})
            for (const auto& dot : state_.dots) {
                if (dot.hostile != hostile) continue;
                if (hostile) colour(cr, 232, 72, 60);
                else if (dot.allied) colour(cr, 240, 215, 75);
                else if (dot.neutral) colour(cr, 130, 165, 225);
                else colour(cr, 110, 224, 120);
                cairo_arc(cr, m.x + dot.u * m.width, m.y + dot.v * m.height, radius, 0, 2 * G_PI);
                cairo_fill(cr);
            }
        for (const auto& plane : state_.planes) {
            const double px = m.x + double(plane.u) * m.width;
            const double py = m.y + double(plane.v) * m.height;
            const double span = std::clamp(full / 38.0, 3.0, 5.5);
            auto drawCross = [&]() {
                cairo_move_to(cr, px - span, py);
                cairo_line_to(cr, px + span, py);
                cairo_move_to(cr, px, py - span);
                cairo_line_to(cr, px, py + span);
            };
            colour(cr, 12, 16, 14, 0.9);
            cairo_set_line_width(cr, 2.8);
            drawCross();
            cairo_stroke(cr);
            if (plane.hostile) colour(cr, 255, 118, 66);
            else colour(cr, 135, 240, 255);
            cairo_set_line_width(cr, 1.5);
            drawCross();
            cairo_stroke(cr);
        }
        for (const auto& ping : state_.pings) {
            const float inten = std::clamp(ping.intensity, 0.0f, 1.0f);
            if (inten <= 0.0f) continue;
            const double px = m.x + std::clamp(ping.u, 0.0f, 1.0f) * m.width;
            const double py = m.y + std::clamp(ping.v, 0.0f, 1.0f) * m.height;
            const double ringR = 4.0 + 10.0 * (1.0 - double(inten));
            if (ping.hostile) colour(cr, 245, 75, 65, 0.9 * double(inten));
            else colour(cr, 90, 225, 255, 0.9 * double(inten));
            cairo_set_line_width(cr, 1.8);
            cairo_arc(cr, px, py, ringR, 0, 2 * G_PI);
            cairo_stroke(cr);
        }
        if (state_.hasObjectiveMarker) {
            const double ox = m.x + std::clamp(state_.objectiveU, 0.0f, 1.0f) * m.width;
            const double oy = m.y + std::clamp(state_.objectiveV, 0.0f, 1.0f) * m.height;
            const double ro = std::clamp(full / 24.0, 4.5, 7.5);
            const double ri = ro * 0.36;
            auto starPath = [&]() {
                cairo_new_sub_path(cr);
                for (int k = 0; k < 8; ++k) {
                    const double a = -G_PI * 0.5 + k * (G_PI * 0.25);
                    const double rad = (k % 2 == 0) ? ro : ri;
                    const double sx = ox + std::cos(a) * rad;
                    const double sy = oy + std::sin(a) * rad;
                    if (k == 0) cairo_move_to(cr, sx, sy); else cairo_line_to(cr, sx, sy);
                }
                cairo_close_path(cr);
            };
            starPath();
            colour(cr, 14, 18, 15, 0.92);
            cairo_set_line_width(cr, 2.2);
            cairo_stroke_preserve(cr);
            colour(cr, 246, 216, 84);
            cairo_fill(cr);
        }
        colour(cr, 245, 245, 240);
        cairo_set_line_width(cr, std::max(1.0, std::round(full / 120.0)));
        cairo_rectangle(cr, std::floor(m.x + state_.viewU * m.width) + .5, std::floor(m.y + state_.viewV * m.height) + .5,
                        std::max(2.0, std::round(double(state_.viewW) * m.width)),
                        std::max(2.0, std::round(double(state_.viewH) * m.height)));
        cairo_stroke(cr);
        cairo_restore(cr);

        // Sharp light-steel diamond border matching the GOG SS2 minimap frame
        colour(cr, 178, 186, 172);
        cairo_set_line_width(cr, 1.5);
        drawDiamondPath(cr, m.x, m.y, m.width, m.height);
        cairo_stroke(cr);

        // Brass compass rose on upper-right steel corner outside the diamond
        if (m.width >= 60 && m.height >= 40) {
            drawCompassRose(cr, m.x + m.width - 13.0, m.y + 13.0, std::clamp(m.height * 0.095, 5.5, 10.0));
            // Classic power symbol tucked in lower-left corner outside the diamond (opens Pause Menu)
            const bool menuHover = hovered_ && *hovered_ == HudAction::menu;
            const bool menuDown = pressed_ && *pressed_ == HudAction::menu;
            if (retail_ && retail_->loaded && retail_->menuBtnNormal) {
                auto* mSurf = ((menuHover || menuDown) && retail_->menuBtnPressed)
                    ? retail_->menuBtnPressed.get() : retail_->menuBtnNormal.get();
                drawSurfaceCrisp(cr, mSurf, m.x + 3.0, m.y + m.height - 30.0, 15.0, 17.0);
            } else {
                if (menuHover) {
                    colour(cr, 72, 82, 62);
                    cairo_arc(cr, m.x + 11.0, m.y + m.height - 22.0, 8.0, 0, 2 * G_PI);
                    cairo_fill(cr);
                }
                drawPowerSymbol(cr, m.x + 11.0, m.y + m.height - 22.0, 5.0);
            }
            // 3 tiny square utility pips along the bottom-left lip (down, up, pause)
            const int pip = 10, py = m.y + m.height - pip - 1;
            for (int k = 0; k < 3; ++k) {
                const int px = m.x + 2 + k * (pip + 2);
                if (k == 2 && retail_ && retail_->loaded && retail_->pauseBtnNormal) {
                    auto* pSurf = (state_.paused && retail_->pauseBtnPressed)
                        ? retail_->pauseBtnPressed.get() : retail_->pauseBtnNormal.get();
                    drawSurfaceCrisp(cr, pSurf, double(px), double(py), double(pip), double(pip));
                    bevelFrame(cr, px, py, pip, pip, state_.paused);
                    continue;
                }
                colour(cr, k == 2 && state_.paused ? 74 : 32, k == 2 && state_.paused ? 88 : 37, k == 2 && state_.paused ? 56 : 33);
                cairo_rectangle(cr, px, py, pip, pip);
                cairo_fill(cr);
                bevelFrame(cr, px, py, pip, pip, false);
                colour(cr, 198, 204, 192);
                cairo_set_line_width(cr, 1.1);
                if (k == 0) {
                    cairo_move_to(cr, px + 5.0, py + 2.5); cairo_line_to(cr, px + 5.0, py + 7.5);
                    cairo_move_to(cr, px + 3.0, py + 5.5); cairo_line_to(cr, px + 5.0, py + 7.5); cairo_line_to(cr, px + 7.0, py + 5.5);
                    cairo_stroke(cr);
                } else if (k == 1) {
                    cairo_move_to(cr, px + 5.0, py + 7.5); cairo_line_to(cr, px + 5.0, py + 2.5);
                    cairo_move_to(cr, px + 3.0, py + 4.5); cairo_line_to(cr, px + 5.0, py + 2.5); cairo_line_to(cr, px + 7.0, py + 4.5);
                    cairo_stroke(cr);
                } else {
                    cairo_rectangle(cr, px + 3.0, py + 2.5, 1.5, 5.0);
                    cairo_rectangle(cr, px + 5.5, py + 2.5, 1.5, 5.0);
                    cairo_fill(cr);
                }
            }
        }
    }

    // 2. Classic 3x3 square metallic command grid immediately right of minimap (context-sensitive).
    const bool hasUnits = state_.hasSelection || !state_.selection.empty();

    const auto isActionActive = [&](HudAction act) {
        return (act == HudAction::holdFire && state_.holdFire) ||
               (act == HudAction::crouch && state_.crouched) ||
               (act == HudAction::pause && state_.paused) ||
               (act == HudAction::recon && state_.binoculars) ||
               (act == HudAction::attackMove && state_.attackMoveArmed) ||
               (act == HudAction::patrol && state_.patrolArmed) ||
               (act == HudAction::attackGround && state_.attackGroundArmed) ||
               (act == HudAction::mine && state_.mineArmed);
    };
    const auto drawCommandCell = [&](const Rect& r, HudAction act, const char* label, const char* key) {
        const bool on = isActionActive(act);
        const bool down = pressed_ && *pressed_ == act;
        const bool hover = hovered_ && *hovered_ == act;
        auto* bg = cairo_pattern_create_linear(r.x, r.y, r.x, r.y + r.height);
        if (down) {
            cairo_pattern_add_color_stop_rgb(bg, 0, 86 / 255., 98 / 255., 66 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 56 / 255., 64 / 255., 46 / 255.);
        } else if (on) {
            cairo_pattern_add_color_stop_rgb(bg, 0, 74 / 255., 88 / 255., 56 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 46 / 255., 56 / 255., 38 / 255.);
        } else if (hover) {
            cairo_pattern_add_color_stop_rgb(bg, 0, 62 / 255., 69 / 255., 60 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 38 / 255., 43 / 255., 38 / 255.);
        } else {
            cairo_pattern_add_color_stop_rgb(bg, 0, 38 / 255., 43 / 255., 39 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 22 / 255., 26 / 255., 23 / 255.);
        }
        cairo_set_source(cr, bg);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_fill(cr);
        cairo_pattern_destroy(bg);
        bevelFrame(cr, r.x, r.y, r.width, r.height, down);
        if (on) {
            colour(cr, 206, 222, 130);
            cairo_set_line_width(cr, 1.5);
            cairo_rectangle(cr, r.x + 1.5, r.y + 1.5, r.width - 3, r.height - 3);
            cairo_stroke(cr);
        }
        const int labelSize = std::clamp(r.height / 5, 7, 13), keySize = std::clamp(r.height / 5, 7, 11);
        const std::string lbl = tr(label);
        const int capW = std::max(8, r.width - 4);
        const int capSize = fittedSize(fonts_.get(), lbl, labelSize, false, capW);
        const double captionY = r.y + r.height - labelSize * 1.35 - 2;
        const double iconTop = r.y + 4.0;
        const double iconBottom = captionY - 1.0;
        const double iconR = std::clamp(std::min(double(r.width) - 8.0, iconBottom - iconTop) * 0.31, 4.5, 16.0);
        bool drewRetailBtn = false;
        if (retail_ && retail_->loaded) {
            const auto bit = retail_->cmdButtons.find(act);
            if (bit != retail_->cmdButtons.end() && bit->second.first) {
                auto* bSurf = ((on || down) && bit->second.second) ? bit->second.second.get() : bit->second.first.get();
                if (retail_->blankBtnBevel && (act == HudAction::holdFire || act == HudAction::crouch ||
                                               act == HudAction::pause || act == HudAction::log)) {
                    drawSurfaceCrisp(cr, retail_->blankBtnBevel.get(), r.x + 1.0, r.y + 1.0,
                                     std::max(2.0, r.width - 2.0), std::max(2.0, r.height - 2.0), 0.55);
                }
                const double boxSide = std::clamp(std::min(double(r.width) - 6.0, iconBottom - iconTop + 2.0), 10.0, 28.0);
                const double bx0 = r.x + (r.width - boxSide) * 0.52;
                const double by0 = (iconTop + iconBottom - boxSide) * 0.5;
                drawSurfaceCrisp(cr, bSurf, bx0, by0, boxSide, boxSide, 0.96);
                drewRetailBtn = true;
            }
        }
        if (!drewRetailBtn) {
            drawCommandIcon(cr, act, r.x + r.width * 0.53, (iconTop + iconBottom) * 0.53, iconR, on || down);
        }
        if (key && *key) {
            const int badgeW = std::max(9, keySize + 2), badgeH = std::max(9, keySize + 2);
            const double bx = r.x + 2.0, by = r.y + 2.0;
            colour(cr, 14, 18, 15, 0.72);
            cairo_rectangle(cr, bx, by, badgeW, badgeH);
            cairo_fill(cr);
            colour(cr, on ? 168 : 96, on ? 182 : 104, on ? 104 : 92, 0.65);
            cairo_set_line_width(cr, 0.8);
            cairo_rectangle(cr, bx + 0.4, by + 0.4, badgeW - 0.8, badgeH - 0.8);
            cairo_stroke(cr);
            colour(cr, on ? 236 : 196, on ? 242 : 202, on ? 186 : 188);
            text(cr, fonts_.get(), key, keySize, true, bx, by + (badgeH - keySize * 1.2) * 0.5, badgeW, true);
        }
        colour(cr, on ? 228 : 186, on ? 234 : 192, on ? 196 : 180);
        text(cr, fonts_.get(), lbl, capSize, false, r.x + 2, captionY, capW, true);
    };

    if (!buttons_.empty()) {
        const int gx0 = buttons_[0].rect.x;
        const int gy0 = buttons_[0].rect.y;
        const int bw = buttons_[0].rect.width;
        const int bh = buttons_[0].rect.height;
        const int gap = 2;
        const int gridW = 3 * bw + 2 * gap;
        const int gridH = 3 * bh + 2 * gap;

        // Recessed dark charcoal backing for the full 3x3 grid
        colour(cr, 14, 17, 15);
        cairo_rectangle(cr, gx0 - 2, gy0 - 2, gridW + 4, gridH + 4);
        cairo_fill(cr);
        bevelFrame(cr, gx0 - 3, gy0 - 3, gridW + 6, gridH + 6, true);

        static constexpr std::pair<HudAction, std::pair<const char*, const char*>> kRow3Meta[] = {
            {HudAction::recon, {"Recon", "B"}},
            {HudAction::attackMove, {"Attack", "A"}},
            {HudAction::patrol, {"Patrol", "Q"}},
            {HudAction::attackGround, {"Bombard", "G"}},
            {HudAction::unload, {"Unload", "U"}},
            {HudAction::mine, {"Mine", "E"}},
            {HudAction::scatter, {"Scatter", "D"}},
        };
        for (int col = 0; col < 3; ++col) {
            const Rect r = row3Rect(col);
            if (const auto act = row3Action(col)) {
                const char* lbl = "";
                const char* key = "";
                for (const auto& [k, mk] : kRow3Meta) {
                    if (k == *act) { lbl = mk.first; key = mk.second; break; }
                }
                drawCommandCell(r, *act, lbl, key);
                continue;
            }
            const bool showMedic = (col == 1 && hasUnits && hasRole(state_, {"medic"}));
            const bool showSpecial = (col == 2 && hasUnits && hasRole(state_, {"flam", "sniper", "sapper", "miner", "at "}));
            if (!showMedic && !showSpecial) {
                colour(cr, 22, 26, 23);
                cairo_rectangle(cr, r.x, r.y, r.width, r.height);
                cairo_fill(cr);
                if (retail_ && retail_->loaded && retail_->blankBtnBevel) {
                    drawSurfaceCrisp(cr, retail_->blankBtnBevel.get(), r.x + 1.0, r.y + 1.0,
                                     std::max(2.0, r.width - 2.0), std::max(2.0, r.height - 2.0), 0.65);
                }
                bevelFrame(cr, r.x, r.y, r.width, r.height, true);
                continue;
            }
            const int rx = r.x, ry = r.y;
            auto* bg = cairo_pattern_create_linear(rx, ry, rx, ry + bh);
            cairo_pattern_add_color_stop_rgb(bg, 0, 46 / 255., 54 / 255., 44 / 255.);
            cairo_pattern_add_color_stop_rgb(bg, 1, 26 / 255., 31 / 255., 26 / 255.);
            cairo_set_source(cr, bg);
            cairo_rectangle(cr, rx, ry, bw, bh);
            cairo_fill(cr);
            cairo_pattern_destroy(bg);
            bevelFrame(cr, rx, ry, bw, bh, false);

            const int labelSize = std::clamp(bh / 5, 7, 13);
            const double captionY = ry + bh - labelSize * 1.35 - 2;
            const double cx = rx + bw * 0.5, cy = (ry + 4.0 + captionY) * 0.5;
            const double ir = std::clamp(std::min(double(bw) - 8.0, captionY - ry - 5.0) * 0.30, 4.5, 14.0);
            if (showMedic) {
                colour(cr, 226, 72, 62);
                cairo_rectangle(cr, cx - ir * 0.25, cy - ir * 0.75, ir * 0.5, ir * 1.5);
                cairo_rectangle(cr, cx - ir * 0.75, cy - ir * 0.25, ir * 1.5, ir * 0.5);
                cairo_fill(cr);
                colour(cr, 186, 194, 178);
                text(cr, fonts_.get(), "Heal", fittedSize(fonts_.get(), "Heal", labelSize, false, std::max(8, bw - 4)),
                     false, rx + 2, captionY, std::max(8, bw - 4), true);
            } else {
                colour(cr, 236, 156, 62);
                cairo_move_to(cr, cx, cy - ir * 0.85);
                cairo_line_to(cr, cx + ir * 0.55, cy + ir * 0.25);
                cairo_line_to(cr, cx, cy + ir * 0.75);
                cairo_line_to(cr, cx - ir * 0.55, cy + ir * 0.25);
                cairo_close_path(cr);
                cairo_fill(cr);
                colour(cr, 186, 194, 178);
                text(cr, fonts_.get(), "Special", fittedSize(fonts_.get(), "Special", labelSize, false, std::max(8, bw - 4)),
                     false, rx + 2, captionY, std::max(8, bw - 4), true);
            }
        }
    }
    for (const auto& b : buttons_) {
        const bool unitOrder = (b.action == HudAction::stop || b.action == HudAction::holdFire || b.action == HudAction::crouch);
        const auto r = b.rect;
        if (unitOrder && !hasUnits) {
            // Match original Sudden Strike II: unit-order slots are blank recessed steel when nothing is selected.
            colour(cr, 22, 26, 23);
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
            cairo_fill(cr);
            if (retail_ && retail_->loaded && retail_->blankBtnBevel) {
                drawSurfaceCrisp(cr, retail_->blankBtnBevel.get(), r.x + 1.0, r.y + 1.0,
                                 std::max(2.0, r.width - 2.0), std::max(2.0, r.height - 2.0), 0.65);
            }
            bevelFrame(cr, r.x, r.y, r.width, r.height, true);
            continue;
        }
        drawCommandCell(r, b.action, b.label, b.key);
    }

    // 3. Slim bottom-right tray: top status strip, unit portrait bay, and classic EX/L/M/A/R stat lip.
    if (readout_.width > 40) {
        // 3a. Top status strip inside the right tray
        {
            const int sx = leftBlockW + 2, sw = std::max(1, w - sx - 2);
            const int size = std::clamp(strip_ * 3 / 5, 9, 14), y = trayTop + (strip_ - size) / 2 + 1;
            colour(cr, 16, 20, 17);
            cairo_rectangle(cr, sx, trayTop + 3, sw, strip_ - 3);
            cairo_fill(cr);
            colour(cr, 130, 138, 126);
            cairo_rectangle(cr, sx, trayTop + strip_, sw, 1);
            cairo_fill(cr);

            int x = sx + 6;
            const int half = sw / 2;
            if (!state_.objective.empty()) {
                colour(cr, 214, 198, 130);
                x += text(cr, fonts_.get(), tr("Objective"), size, true, x, y, 0) + size / 2;
                colour(cr, 234, 236, 228);
                text(cr, fonts_.get(), state_.objective, size, false, x, y, std::max(1, sx + half - x));
            }
            int chipX = sx + half + 4;
            for (const auto& group : state_.groups) {
                const auto gr = groupChipRect(group.digit);
                if (gr.width <= 0 || gr.height <= 0) break;
                const std::string label = std::to_string(group.digit) + ":" + std::to_string(group.count);
                const bool down = pressedGroup_ && *pressedGroup_ == group.digit;
                const bool hov = hoveredGroup_ && *hoveredGroup_ == group.digit;
                if (down) colour(cr, 86, 98, 66);
                else if (hov) colour(cr, 64, 74, 58);
                else colour(cr, 48, 56, 44);
                cairo_rectangle(cr, gr.x, gr.y, gr.width, gr.height);
                cairo_fill(cr);
                bevelFrame(cr, gr.x, gr.y, gr.width, gr.height, down);
                colour(cr, 236, 238, 230);
                text(cr, fonts_.get(), label, size, true, gr.x, y, gr.width, true);
                chipX = std::max(chipX, gr.x + gr.width + 4);
            }
            int airLeft = sx + sw - 4;
            static constexpr const char* kAirLetters[5] = {"B", "R", "P", "S", "F"};
            for (int k = 0; k < 5; ++k) {
                const auto ar = airButtonRect(k);
                if (ar.width <= 0 || ar.height <= 0) continue;
                airLeft = std::min(airLeft, ar.x);
                const bool armed = (state_.armedAir == k);
                const bool down = (pressedAir_ && *pressedAir_ == k);
                const bool hov = (hoveredAir_ && *hoveredAir_ == k);
                if (armed) colour(cr, 96, 110, 56);
                else if (down) colour(cr, 86, 98, 66);
                else if (hov) colour(cr, 64, 74, 58);
                else colour(cr, 42, 50, 44);
                cairo_rectangle(cr, ar.x, ar.y, ar.width, ar.height);
                cairo_fill(cr);
                if (retail_ && retail_->loaded && retail_->pdropPlate) {
                    drawSurfaceCrisp(cr, retail_->pdropPlate.get(), ar.x, ar.y, ar.width, ar.height,
                                     armed ? 0.42 : 0.58);
                }
                bevelFrame(cr, ar.x, ar.y, ar.width, ar.height, down);
                if (armed) {
                    colour(cr, 235, 212, 118);
                    cairo_set_line_width(cr, 1.2);
                    cairo_rectangle(cr, ar.x + 1, ar.y + 1, ar.width - 2, ar.height - 2);
                    cairo_stroke(cr);
                }
                int textX = ar.x;
                int textW = ar.width;
                if (retail_ && retail_->loaded) {
                    const auto& pair = retail_->airIcons[std::size_t(k)];
                    auto* iconSurf = ((armed || down) && pair.second) ? pair.second.get() : pair.first.get();
                    const int iconSz = std::clamp(ar.height - 2, 8, 14);
                    if (iconSurf && ar.width >= iconSz + 18) {
                        drawSurfaceCrisp(cr, iconSurf, ar.x + 2.0, ar.y + (ar.height - iconSz) * 0.5,
                                         double(iconSz), double(iconSz), 0.95);
                        if (k == 2 && retail_->pdropChuteIcon) {
                            const int pipSz = std::max(6, iconSz - 3);
                            drawSurfaceCrisp(cr, retail_->pdropChuteIcon.get(),
                                             ar.x + 3.0, ar.y + (ar.height - pipSz) * 0.5,
                                             double(pipSz), double(pipSz), 0.95);
                        }
                        textX = ar.x + iconSz + 2;
                        textW = std::max(8, ar.width - iconSz - 3);
                    }
                }
                colour(cr, armed ? 245 : 230, armed ? 236 : 220, armed ? 180 : 172);
                const std::string lbl = std::string(kAirLetters[k]) + ":" + std::to_string(state_.airReady[std::size_t(k)]);
                text(cr, fonts_.get(), lbl, size, true, textX, y - 1, textW, true);
            }
            static const std::pair<HudAction, const char*> hints[] = {
                {HudAction::stop, "Stop: cancel movement and attacks (S)"},
                {HudAction::holdFire, "Hold fire: toggle firing at will (H)"},
                {HudAction::crouch, "Crouch: toggle crouching, slower but harder to hit (C)"},
                {HudAction::mission, "Mission: jump to the mission marker (M)"},
                {HudAction::pause, "Pause: stop or resume the mission clock (P)"},
                {HudAction::log, "Log: review recent messages (L)"},
                {HudAction::recon, "Recon: Officer binoculars for extended vision (B)"},
                {HudAction::attackMove, "Attack-Move: advance and engage hostiles (A)"},
                {HudAction::patrol, "Patrol: loop between position and target (Q)"},
                {HudAction::attackGround, "Bombard: fire at ground target (G)"},
                {HudAction::unload, "Unload: disembark passengers (U)"},
                {HudAction::mine, "Mine: lay mine or disarm detected mines (E)"},
                {HudAction::scatter, "Scatter: disperse infantry outward from blasts (D)"}};
            std::string right;
            bool hint = false;
            if (hovered_) for (const auto& [action, line] : hints) if (action == *hovered_) { right = tr(line); hint = true; }
            if (!hint && hoveredAir_ && *hoveredAir_ >= 0 && *hoveredAir_ < 5) {
                static constexpr const char* kAirHints[5] = {
                    "Bomber: click map to carpet-bomb (Alt+1)",
                    "Recon: click map for scout overflight (Alt+2)",
                    "Paratrooper: click map to drop airborne squad (Alt+3)",
                    "Supply: click map for supply drop (Alt+4)",
                    "Fighter: click map for strafing run (Alt+5)",
                };
                right = tr(kAirHints[*hoveredAir_]);
                hint = true;
            }
            if (!hint && hoveredGroup_) {
                right = trf("Group {0}: click to recall (click again to focus, Shift+click to add)",
                            {std::to_string(*hoveredGroup_)});
                hint = true;
            }
            if (!hint && hoveredClass_) {
                right = trf("{0}: click to select only this class, Shift+click to remove",
                            {tr(*hoveredClass_)});
                hint = true;
            }
            if (!hint && !messages_.empty()) right = messages_.back();
            if (!right.empty()) {
                const int rx = std::max(chipX + 4, sx + sw * 3 / 5);
                const int maxRight = (airLeft < sx + sw - 4) ? (airLeft - 6) : (sx + sw - 4);
                const int rw = maxRight - rx;
                if (rw > 8) {
                    if (hint) colour(cr, 206, 222, 136); else colour(cr, 178, 184, 174);
                    cairo_save(cr);
                    cairo_rectangle(cr, rx, trayTop + 3, rw, strip_ - 3);
                    cairo_clip(cr);
                    text(cr, fonts_.get(), right, size, false, rx, y, rw);
                    cairo_restore(cr);
                }
            }
        }

        // 3b. Recessed steel unit & squad bay
        const auto r = readout_;
        colour(cr, 18, 22, 19);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_fill(cr);
        bevelFrame(cr, r.x - 1, r.y - 1, r.width + 2, r.height + 2, true);
        if (r.width > 120 && r.height > 28) {
            drawRivet(cr, r.x + 4, r.y + 4, 1.8);
            drawRivet(cr, r.x + r.width - 4, r.y + 4, 1.8);
        }
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_clip(cr);
        const int inX = r.x + 8;
        const int inW = std::max(1, r.width - 16);
        unsigned total = 0;
        for (const auto& c : state_.selection) total += c.count;
        const std::string title = total == 1 ? tr("1 soldier selected")
                                : total ? trf("{0} soldiers selected", {std::to_string(total)}) : tr("No units selected");
        colour(cr, 214, 198, 130);
        const int titleW = text(cr, fonts_.get(), title, small, true, inX, r.y + 2, inW / 2);

        // Stance + losses inline on the right of the header line to maximize portrait height
        std::string status = tr(state_.crouched ? "Crouched" : "Standing");
        status += "  " + tr(state_.holdFire ? "Hold fire" : "Fire at will");
        if (state_.paused) status += "  " + tr("Paused");
        colour(cr, 174, 182, 170);
        const int statusX = inX + titleW + 12;
        const int statusW = text(cr, fonts_.get(), status, small, false, statusX, r.y + 2, std::max(1, inW - titleW - 12));
        if (state_.losses) {
            colour(cr, 232, 112, 96);
            text(cr, fonts_.get(), trf("Losses {0}", {std::to_string(state_.losses)}), small, true,
                 statusX + statusW + 10, r.y + 2, std::max(1, r.x + r.width - (statusX + statusW + 14)));
        }

        // Compact metallic portrait slots for selected classes
        const int cardsY = r.y + small + 3, cardsH = r.y + r.height - cardsY - 1;
        if (cardsH >= 14 && !state_.selection.empty()) {
            const int portrait = cardsH - 4, gap = 4;
            const int widest = std::max(portrait + small * 8, 112), narrowest = std::max(portrait + small * 5, 82);
            const int classes = int(state_.selection.size());
            const int cardW = std::clamp((inW - (classes - 1) * gap) / classes, narrowest, widest);
            const int fit = std::max(1, (inW + gap) / (cardW + gap));
            int x = inX;
            for (int i = 0; i < int(state_.selection.size()) && i < fit; ++i, x += cardW + gap) {
                const auto& c = state_.selection[i];
                const bool last = i == fit - 1 && int(state_.selection.size()) > fit;
                const bool cardDown = !last && pressedClass_ && *pressedClass_ == c.label;
                const bool cardHov = !last && hoveredClass_ && *hoveredClass_ == c.label;
                if (cardDown) colour(cr, 56, 68, 52);
                else if (cardHov) colour(cr, 42, 50, 42);
                else colour(cr, 28, 33, 29);
                cairo_rectangle(cr, x, cardsY, cardW, cardsH);
                cairo_fill(cr);
                bevelFrame(cr, x, cardsY, cardW, cardsH, cardDown);
                if (last) {
                    colour(cr, 178, 184, 174);
                    text(cr, fonts_.get(), "+" + std::to_string(state_.selection.size() - fit + 1) + " more", body, false,
                         x + 4, cardsY + (cardsH - body) / 2.0, cardW - 8);
                    break;
                }
                const auto found = portraits_.find(c.label);
                if (found != portraits_.end()) {
                    const auto picture = image(found->second);
                    cairo_save(cr);
                    cairo_rectangle(cr, x + 2, cardsY + 2, portrait, portrait);
                    cairo_clip(cr);
                    cairo_translate(cr, x + 2, cardsY + 2);
                    cairo_scale(cr, double(portrait) / found->second.width, double(portrait) / found->second.height);
                    cairo_set_source_surface(cr, picture.get(), 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else {
                    colour(cr, 48, 56, 44);
                    cairo_rectangle(cr, x + 2, cardsY + 2, portrait, portrait);
                    cairo_fill(cr);
                    colour(cr, 214, 198, 130);
                    const std::string initial = c.label.empty() ? std::string("?") : c.label.substr(0, 1);
                    const int letter = std::max(9, portrait / 2);
                    text(cr, fonts_.get(), initial, letter, true, x + 2, cardsY + 2 + (portrait - letter * 1.25) / 2.0, portrait, true);
                }
                bevelFrame(cr, x + 2, cardsY + 2, portrait, portrait, true);
                const int tx = x + portrait + 5, tw = std::max(8, cardW - portrait - 8);
                colour(cr, 236, 238, 230);
                {
                    const std::string name = tr(c.label) + " x" + std::to_string(c.count);
                    text(cr, fonts_.get(), name, fittedSize(fonts_.get(), name, small, true, tw), true, tx, cardsY + 2, tw);
                }
                const auto bar = [&](int row, float value, int red, int green, int blue) {
                    const double barH = std::max(2.5, cardsH / 10.0), by = cardsY + cardsH - (2 - row) * (barH + 2) - 2;
                    colour(cr, 14, 17, 15);
                    cairo_rectangle(cr, tx, by, tw, barH);
                    cairo_fill(cr);
                    colour(cr, red, green, blue);
                    cairo_rectangle(cr, tx, by, tw * std::clamp(value, 0.f, 1.f), barH);
                    cairo_fill(cr);
                };
                const float hp = std::clamp(c.health, 0.f, 1.f);
                bar(0, c.health, int(220 - 110 * hp), int(80 + 144 * hp), 90);
                bar(1, c.ammo, 214, 170, 82);
            }
        }
        cairo_restore(cr);

        // 3c. Classic Sudden Strike II bottom metallic EX / L / M / A / R stat tick bar
        {
            const int my = full - 10, mh = 9, mw = std::max(1, w - leftBlockW - 6);
            colour(cr, 16, 19, 17);
            cairo_rectangle(cr, leftBlockW + 3, my, mw, mh);
            cairo_fill(cr);
            bevelFrame(cr, leftBlockW + 2, my - 1, mw + 2, mh + 2, true);
            static const char* labels[5] = {"EX", "L", "M", "A", "R"};
            struct Rgb { int r, g, b; };
            const auto meterColour = [](int index, float v) -> Rgb {
                switch (index) {
                case 0: return {230, 186, 66};   // EX: gold / amber
                case 1: {                        // L: green -> amber -> red by value
                    if (v >= 0.5f) {
                        const float t = (v - 0.5f) * 2.f;
                        return {int(std::lround(228 - 144 * t)), int(std::lround(174 + 34 * t)), int(std::lround(58 + 38 * t))};
                    }
                    const float t = v * 2.f;
                    return {int(std::lround(220 + 8 * t)), int(std::lround(62 + 112 * t)), int(std::lround(54 + 4 * t))};
                }
                case 2: return {118, 174, 188};  // M: sky / olive-slate
                case 3: return {214, 152, 68};   // A: warm brass
                default: return {68, 196, 148};  // R: crisp green / teal
                }
            };
            const int segW = mw / 5;
            for (int i = 0; i < 5; ++i) {
                const int lx = leftBlockW + 5 + i * segW;
                const int tx = lx + 16, ty = my + 2, tw = std::max(4, segW - 22), th = mh - 4;
                if (state_.hasSelection) colour(cr, 214, 220, 202); else colour(cr, 186, 192, 176);
                text(cr, fonts_.get(), labels[i], 7, true, lx, my, 16);
                colour(cr, 36, 42, 36);
                cairo_rectangle(cr, tx, ty, tw, th);
                cairo_fill(cr);
                if (state_.hasSelection) {
                    const float raw = state_.meters[std::size_t(i)];
                    const float v = std::isfinite(raw) ? std::clamp(raw, 0.f, 1.f) : 0.f;
                    const double fillW = tw * double(v);
                    if (fillW > 0.0) {
                        const auto c = meterColour(i, v);
                        colour(cr, c.r, c.g, c.b);
                        cairo_rectangle(cr, tx, ty, fillW, th);
                        cairo_fill(cr);
                        colour(cr, std::min(255, c.r + 36), std::min(255, c.g + 36), std::min(255, c.b + 36), 0.7);
                        cairo_rectangle(cr, tx, ty, fillW, 1.5);
                        cairo_fill(cr);
                    }
                }
            }
        }
    }
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr)));

    rgba_.resize(std::size_t(w) * full * 4);
    cairo_surface_flush(target.get());
    if (!SDL_ConvertPixels(w, full, SDL_PIXELFORMAT_ARGB8888, cairo_image_surface_get_data(target.get()),
                           cairo_image_surface_get_stride(target.get()), SDL_PIXELFORMAT_RGBA32, rgba_.data(), w * 4))
        throw std::runtime_error(SDL_GetError());
    // Cairo ARGB32 is premultiplied; un-premultiply into straight RGBA8 while keeping transparent cutout alpha=0.
    for (std::size_t i = 0; i < rgba_.size(); i += 4) {
        const std::uint8_t a = rgba_[i + 3];
        if (a == 0) {
            rgba_[i] = rgba_[i + 1] = rgba_[i + 2] = 0;
        } else if (a < 255) {
            rgba_[i] = std::uint8_t(std::min<unsigned>(255, (unsigned(rgba_[i]) * 255 + a / 2) / a));
            rgba_[i + 1] = std::uint8_t(std::min<unsigned>(255, (unsigned(rgba_[i + 1]) * 255 + a / 2) / a));
            rgba_[i + 2] = std::uint8_t(std::min<unsigned>(255, (unsigned(rgba_[i + 2]) * 255 + a / 2) / a));
        }
    }
    ++revision_;
}

std::optional<SDL_FPoint> Hud::barPoint(float windowX, float windowY) const {
    int w = 0, h = 0, pixelW = 0, pixelH = 0;
    if (!SDL_GetWindowSize(parent_, &w, &h) || !SDL_GetWindowSizeInPixels(parent_, &pixelW, &pixelH) || w <= 0 || h <= 0)
        return std::nullopt;
    return SDL_FPoint{windowX * pixelW / w, windowY * pixelH / h - top_};
}

std::optional<HudAction> Hud::buttonAt(float x, float y) const {
    const bool hasUnits = state_.hasSelection || !state_.selection.empty();
    for (const auto& b : buttons_) {
        if (!inside(b.rect, x, y)) continue;
        const bool unitOrder = (b.action == HudAction::stop || b.action == HudAction::holdFire || b.action == HudAction::crouch);
        if (unitOrder && !hasUnits) return std::nullopt;
        return b.action;
    }
    for (int col = 0; col < 3; ++col) {
        if (const auto act = row3Action(col)) {
            if (inside(row3Rect(col), x, y)) return *act;
        }
    }
    if (minimap_.width >= 60 && minimap_.height >= 40) {
        const Rect powerRect{minimap_.x + 2, minimap_.y + minimap_.height - 30, 18, 16};
        if (inside(powerRect, x, y)) return HudAction::menu;
        const Rect pausePip{minimap_.x + 26, minimap_.y + minimap_.height - 12, 12, 12};
        if (inside(pausePip, x, y)) return HudAction::pause;
    }
    return std::nullopt;
}

std::optional<int> Hud::airAt(float x, float y) const {
    for (int k = 0; k < 5; ++k) {
        const auto r = airButtonRect(k);
        if (r.width > 0 && r.height > 0 && inside(r, x, y)) return k;
    }
    return std::nullopt;
}

std::optional<unsigned> Hud::groupAt(float x, float y) const {
    for (const auto& g : state_.groups) {
        const auto r = groupChipRect(g.digit);
        if (r.width > 0 && r.height > 0 && inside(r, x, y)) return g.digit;
    }
    return std::nullopt;
}

std::optional<std::string> Hud::classAt(float x, float y) const {
    for (std::size_t i = 0; i < state_.selection.size(); ++i) {
        const auto r = classCardRect(i);
        if (r.width > 0 && r.height > 0 && inside(r, x, y)) return state_.selection[i].label;
    }
    return std::nullopt;
}

bool Hud::minimapPoint(float x, float y, float& u, float& v, bool clampOutside) const {
    if (!clampOutside) {
        if (!inside(minimap_, x, y)) return false;
        const float rx = std::max(1.0f, minimap_.width * 0.5f);
        const float ry = std::max(1.0f, minimap_.height * 0.5f);
        const float nx = std::abs(x - (minimap_.x + rx)) / rx;
        const float ny = std::abs(y - (minimap_.y + ry)) / ry;
        if (nx + ny > 1.01f) return false;
    }
    u = std::clamp((x - minimap_.x) / std::max(1, minimap_.width), 0.f, 1.f);
    v = std::clamp((y - minimap_.y) / std::max(1, minimap_.height), 0.f, 1.f);
    return true;
}

bool Hud::opaqueAtBarPoint(const std::optional<SDL_FPoint>& p) const {
    if (!p) return false;
    const int ix = int(std::floor(p->x)), iy = int(std::floor(p->y));
    if (ix < 0 || iy < 0 || ix >= int(width_) || iy >= int(height_)) return false;
    const std::size_t idx = (std::size_t(iy) * width_ + std::size_t(ix)) * 4 + 3;
    return idx < rgba_.size() && rgba_[idx] > 0;
}

bool Hud::opaqueAtWindow(float windowX, float windowY) const {
    return opaqueAtBarPoint(barPoint(windowX, windowY));
}

std::optional<HudTip> Hud::tooltipAtWindow(float windowX, float windowY) const {
    const auto p = barPoint(windowX, windowY);
    if (!opaqueAtBarPoint(p)) return std::nullopt;
    if (const auto act = buttonAt(p->x, p->y)) {
        auto withRetailHint = [&](HudTip tip, std::string_view itxtKey) {
            if (retail_ && retail_->loaded) {
                const std::string raw = stripRetailKeyTag(retailText(itxtKey));
                if (!raw.empty()) tip.lines.push_back(raw);
            }
            return tip;
        };
        switch (*act) {
        case HudAction::stop:
            return withRetailHint(HudTip{tr("Stop (S)"), {tr("Halt movement and clear current orders")}}, "ITXT_STOP");
        case HudAction::holdFire:
            return withRetailHint(HudTip{tr("Hold Fire (H)"), {tr("Toggle weapons hold / fire at will")}}, "ITXT_HOLDFIRE");
        case HudAction::crouch:
            return withRetailHint(HudTip{tr("Crouch / Prone (C)"), {tr("Take cover and reduce visibility")}}, "ITXT_FOOTCRAWL");
        case HudAction::mission:
            return withRetailHint(HudTip{tr("Mission Objective (M)"), {tr("Focus camera on mission marker")}}, "ITXT_COVER");
        case HudAction::log:
            return HudTip{tr("Battle Log (L)"), {tr("Open mission event log")}};
        case HudAction::menu:
            return withRetailHint(HudTip{tr("Menu (Esc)"), {tr("Open pause menu, save/load & controls")}}, "ITXT_MENU");
        case HudAction::pause:
            return withRetailHint(HudTip{tr("Pause (Ctrl+P)"), {tr("Pause or resume battle clock")}}, "ITXT_PAUSE");
        case HudAction::recon:
            return withRetailHint(HudTip{tr("Binoculars (B)"), {tr("Extend officer sight range while stationary")}}, "ITXT_BINOCULAR");
        case HudAction::attackMove:
            return withRetailHint(HudTip{tr("Attack-Move (A)"), {tr("Advance and engage hostiles along the way")}}, "ITXT_AMOVE");
        case HudAction::patrol:
            return withRetailHint(HudTip{tr("Patrol (Q)"), {tr("Patrol continuously between current position and target")}}, "ITXT_PLANE_PATROL");
        case HudAction::attackGround:
            return withRetailHint(HudTip{tr("Attack Ground (G)"), {tr("Bombard terrain or buildings with primary ordnance")}}, "ITXT_GAUBFIRE");
        case HudAction::unload:
            return withRetailHint(HudTip{tr("Unload / Unhitch (U)"), {tr("Disembark passengers, unhitch gun, or evacuate building")}}, "ITXT_UNLOAD");
        case HudAction::mine:
            return withRetailHint(HudTip{tr("Lay AT Mine (E)"), {tr("Sapper plants an anti-tank mine at current tile")}}, "ITXT_LAYMINE");
        case HudAction::scatter:
            return withRetailHint(HudTip{tr("Scatter (D)"), {tr("Disperse infantry immediately away from explosions")}}, "ITXT_SCATTER");
        default:
            return std::nullopt;
        }
    }
    if (const auto slot = airAt(p->x, p->y)) {
        static constexpr std::array<const char*, 5> kAirNames{{
            "Recon Plane", "Fighter Strafe", "Bomber Strike", "Paratroopers", "Supply Drop"
        }};
        const int k = *slot;
        if (k >= 0 && k < 5) {
            return HudTip{
                tr(kAirNames[static_cast<std::size_t>(k)]) + " (Alt+" + std::to_string(k + 1) + ")",
                {trf("Ready sorties: {0}", {std::to_string(state_.airReady[static_cast<std::size_t>(k)])}),
                 tr("Click or press F, then click map or minimap")}
            };
        }
    }
    if (const auto grp = groupAt(p->x, p->y)) {
        unsigned cnt = 0;
        for (const auto& g : state_.groups) {
            if (g.digit == *grp) { cnt = g.count; break; }
        }
        return HudTip{
            trf("Control Group {0}", {std::to_string(*grp)}),
            {trf("{0} units — click to select, click again to focus", {std::to_string(cnt)})}
        };
    }
    if (const auto cls = classAt(p->x, p->y)) {
        return HudTip{tr(*cls), {tr("Click to select class • Shift/R-click to exclude")}};
    }
    return std::nullopt;
}

HudResult Hud::handle(const SDL_Event& event) {
    refit();
    const auto id = SDL_GetWindowID(parent_);
    const auto ours = [id](SDL_WindowID other) { return !other || other == id; };
    const auto opaqueAt = [this](const std::optional<SDL_FPoint>& p) -> bool {
        return opaqueAtBarPoint(p);
    };
    HudResult result;
    switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event.window.windowID == id) {
            layout();
            render();
        }
        return result;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        draggingMinimap_ = draggingMinimap_ && event.type != SDL_EVENT_WINDOW_FOCUS_LOST;
        if (pressed_ || hovered_ || pressedAir_ || hoveredAir_ ||
            pressedGroup_ || hoveredGroup_ || pressedClass_ || hoveredClass_) {
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                pressed_.reset();
                pressedAir_.reset();
                pressedGroup_.reset();
                pressedClass_.reset();
            }
            hovered_.reset();
            hoveredAir_.reset();
            hoveredGroup_.reset();
            hoveredClass_.reset();
            render();
        }
        return result;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (!ours(event.button.windowID)) return result;
        const auto p = barPoint(event.button.x, event.button.y);
        if (!opaqueAt(p)) return result;
        result.consumed = true;
        if (event.button.button == SDL_BUTTON_RIGHT && minimapPoint(p->x, p->y, result.u, result.v, false)) {
            result.action = HudAction::orderMinimap;
            return result;
        }
        if (event.button.button == SDL_BUTTON_RIGHT) {
            if (const auto hitClass = classAt(p->x, p->y)) {
                result.action = HudAction::selectClass;
                result.classLabel = *hitClass;
                result.shiftHeld = true;
                return result;
            }
        }
        if (event.button.button != SDL_BUTTON_LEFT) return result;
        if (const auto hitAir = airAt(p->x, p->y)) {
            pressedAir_ = hitAir;
            pressed_.reset();
            pressedGroup_.reset();
            pressedClass_.reset();
            render();
        } else if (const auto hitGroup = groupAt(p->x, p->y)) {
            pressedGroup_ = hitGroup;
            pressed_.reset();
            pressedAir_.reset();
            pressedClass_.reset();
            render();
        } else if (const auto hitClass = classAt(p->x, p->y)) {
            pressedClass_ = hitClass;
            pressed_.reset();
            pressedAir_.reset();
            pressedGroup_.reset();
            render();
        } else if (const auto hit = buttonAt(p->x, p->y)) {
            pressed_ = hit;
            pressedAir_.reset();
            pressedGroup_.reset();
            pressedClass_.reset();
            render();
        } else if (minimapPoint(p->x, p->y, result.u, result.v, false)) {
            draggingMinimap_ = true;
            result.action = HudAction::moveCamera;
        }
        return result;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!ours(event.motion.windowID)) return result;
        const auto p = barPoint(event.motion.x, event.motion.y);
        if (!p) return result;
        // A drag that began on the minimap keeps steering the camera even outside it.
        if (draggingMinimap_ && (event.motion.state & SDL_BUTTON_LMASK)) {
            result.consumed = true;
            result.action = HudAction::moveCamera;
            minimapPoint(p->x, p->y, result.u, result.v, true);
            return result;
        }
        result.consumed = pressed_.has_value() || pressedAir_.has_value() ||
                          pressedGroup_.has_value() || pressedClass_.has_value() || opaqueAt(p);
        const auto over = buttonAt(p->x, p->y);
        const auto overAir = airAt(p->x, p->y);
        const auto overGroup = groupAt(p->x, p->y);
        const auto overClass = classAt(p->x, p->y);
        if (over != hovered_ || overAir != hoveredAir_ || overGroup != hoveredGroup_ || overClass != hoveredClass_) {
            hovered_ = over;
            hoveredAir_ = overAir;
            hoveredGroup_ = overGroup;
            hoveredClass_ = overClass;
            render();
        }
        return result;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!ours(event.button.windowID)) return result;
        const auto p = barPoint(event.button.x, event.button.y);
        const bool overBar = opaqueAt(p);
        if (event.button.button == SDL_BUTTON_LEFT &&
            (pressed_ || pressedAir_ || pressedGroup_ || pressedClass_ || draggingMinimap_)) {
            // The release ends a gesture that began on the bar, wherever it lands.
            result.consumed = true;
            if (pressedAir_) {
                if (p && airAt(p->x, p->y) == pressedAir_) {
                    result.action = HudAction::airSupport;
                    result.airSlot = *pressedAir_;
                }
                pressedAir_.reset();
                render();
            } else if (pressedGroup_) {
                if (p && groupAt(p->x, p->y) == pressedGroup_) {
                    result.action = HudAction::recallGroup;
                    result.groupDigit = int(*pressedGroup_);
                    result.shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                }
                pressedGroup_.reset();
                render();
            } else if (pressedClass_) {
                if (p && classAt(p->x, p->y) == pressedClass_) {
                    result.action = HudAction::selectClass;
                    result.classLabel = *pressedClass_;
                    result.shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                }
                pressedClass_.reset();
                render();
            } else if (pressed_) {
                if (p && buttonAt(p->x, p->y) == pressed_) result.action = *pressed_;
                pressed_.reset();
                render();
            }
            draggingMinimap_ = false;
            return result;
        }
        result.consumed = overBar;
        return result;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!ours(event.wheel.windowID)) return result;
        const auto p = barPoint(event.wheel.mouse_x, event.wheel.mouse_y);
        result.consumed = opaqueAt(p);
        return result;
    }
    default:
        return result;
    }
}

void Hud::composeWeatherOverlay() {
    const int winW = std::max(1, int(width_));
    const int winH = std::max(int(height_), top_ + int(height_));
    composedRgba_.assign(std::size_t(winW) * std::size_t(winH) * 4u, 0u);

    const auto blendWeatherPixel = [&](int px, int py, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        if (px < 0 || py < 0 || px >= winW || py >= winH || a == 0u) return;
        if (py >= top_) {
            const int by = py - top_;
            if (by < int(height_)) {
                const std::size_t bi = (std::size_t(by) * width_ + std::size_t(px)) * 4u;
                if (bi + 3u < rgba_.size() && rgba_[bi + 3u] > 0u) return;
            }
        }
        auto* dst = &composedRgba_[(std::size_t(py) * std::size_t(winW) + std::size_t(px)) * 4u];
        if (dst[3] == 0u) {
            dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
            return;
        }
        // Straight-alpha 'over' blend: C_out = (C_src * a_src + C_dst * a_dst * (1 - a_src)) / a_out
        const unsigned sa = a;
        const unsigned da = dst[3];
        const unsigned invSa = 255u - sa;
        const unsigned outA = sa + (da * invSa + 127u) / 255u;
        if (outA == 0u) return;
        const auto mix = [&](unsigned sc, unsigned dc) -> std::uint8_t {
            const unsigned num = sc * sa * 255u + dc * da * invSa;
            return static_cast<std::uint8_t>(std::min(255u, (num + (outA * 255u) / 2u) / (outA * 255u)));
        };
        dst[0] = mix(r, dst[0]);
        dst[1] = mix(g, dst[1]);
        dst[2] = mix(b, dst[2]);
        dst[3] = static_cast<std::uint8_t>(std::min(255u, outA));
    };

    // 1. Copy opaque/semi-opaque HUD bar rows into the bottom of the full-window overlay.
    for (int by = 0; by < int(height_); ++by) {
        const int wy = top_ + by;
        if (wy < 0 || wy >= winH) continue;
        const auto* srcRow = &rgba_[std::size_t(by) * width_ * 4u];
        auto* dstRow = &composedRgba_[std::size_t(wy) * std::size_t(winW) * 4u];
        std::copy_n(srcRow, std::size_t(winW) * 4u, dstRow);
    }

    // 2. Render deterministic atmospheric weather across all open viewport pixels (including L-cutout).
    const int viewBottom = winH;
    if (weather_ == assets::WeatherKind::Fog) {
        const float cx = float(winW) * 0.5f;
        const float cy = float(std::max(1, top_)) * 0.5f;
        const float invRx = 1.0f / std::max(1.0f, cx);
        const float invRy = 1.0f / std::max(1.0f, float(winH) * 0.5f);
        const float drift = float(weatherPhase_) * 0.035f;
        for (int y = 0; y < viewBottom; ++y) {
            const float ny = (float(y) - cy) * invRy;
            const float waveY = std::sin(float(y) * 0.022f + drift * 0.6f);
            for (int x = 0; x < winW; ++x) {
                if (y >= top_) {
                    const std::size_t bi = (std::size_t(y - top_) * width_ + std::size_t(x)) * 4u;
                    if (bi + 3u < rgba_.size() && rgba_[bi + 3u] > 0u) continue;
                }
                const float nx = (float(x) - cx) * invRx;
                const float r2 = std::min(1.8f, nx * nx + ny * ny);
                const float bank = 0.5f + 0.5f * std::sin(float(x) * 0.014f + drift + waveY * 1.4f);
                const float mist = std::clamp(0.05f + 0.19f * r2 + 0.05f * bank, 0.04f, 0.30f);
                auto* dst = &composedRgba_[(std::size_t(y) * std::size_t(winW) + std::size_t(x)) * 4u];
                dst[0] = 186u;
                dst[1] = 200u;
                dst[2] = 212u;
                dst[3] = static_cast<std::uint8_t>(std::lround(mist * 255.0f));
            }
        }
    } else if (weather_ == assets::WeatherKind::Rain) {
        // Subtle wet-slate atmospheric overcast wash
        for (int y = 0; y < viewBottom; ++y) {
            for (int x = 0; x < winW; ++x) {
                if (y >= top_) {
                    const std::size_t bi = (std::size_t(y - top_) * width_ + std::size_t(x)) * 4u;
                    if (bi + 3u < rgba_.size() && rgba_[bi + 3u] > 0u) continue;
                }
                auto* dst = &composedRgba_[(std::size_t(y) * std::size_t(winW) + std::size_t(x)) * 4u];
                dst[0] = 94u;
                dst[1] = 112u;
                dst[2] = 132u;
                dst[3] = 18u;
            }
        }
        // Deterministic angled rain streaks + ground splash ripples
        const int streakCount = std::clamp((winW * viewBottom) / 2600, 96, 420);
        const int spanW = winW + 96;
        const int spanH = viewBottom + 64;
        for (int i = 0; i < streakCount; ++i) {
            const std::uint32_t h0 = hashWeather(std::uint32_t(i) * 0x9e3779b9u ^ 0x51a7c3d1u);
            const int speedY = 18 + int(h0 & 7u) * 2;
            const int speedX = -(speedY * 2) / 5;
            const int len = 14 + int((h0 >> 4u) & 7u) * 2;
            const std::int64_t t = std::int64_t(weatherPhase_);
            int headX = int(((std::int64_t(h0 >> 8u) % spanW) + t * speedX) % spanW);
            if (headX < 0) headX += spanW;
            headX -= 32;
            int headY = int(((std::int64_t(h0 >> 18u) % spanH) + t * speedY) % spanH) - 24;
            const std::uint8_t baseA = static_cast<std::uint8_t>(92u + ((h0 >> 12u) & 63u));
            for (int step = 0; step < len; ++step) {
                const int px = headX + (step * 2) / 5;
                const int py = headY - step;
                const unsigned fadeA = (unsigned(baseA) * unsigned(len - step)) / unsigned(len);
                blendWeatherPixel(px, py, 198u, 218u, 240u, static_cast<std::uint8_t>(fadeA));
            }
            if (((h0 + weatherPhase_ * 5u) & 15u) == 0u) {
                blendWeatherPixel(headX - 1, headY, 210u, 226u, 244u, 84u);
                blendWeatherPixel(headX + 1, headY, 210u, 226u, 244u, 84u);
            }
        }
    } else if (weather_ == assets::WeatherKind::Snow) {
        // Subtle crisp winter frost wash
        for (int y = 0; y < viewBottom; ++y) {
            for (int x = 0; x < winW; ++x) {
                if (y >= top_) {
                    const std::size_t bi = (std::size_t(y - top_) * width_ + std::size_t(x)) * 4u;
                    if (bi + 3u < rgba_.size() && rgba_[bi + 3u] > 0u) continue;
                }
                auto* dst = &composedRgba_[(std::size_t(y) * std::size_t(winW) + std::size_t(x)) * 4u];
                dst[0] = 212u;
                dst[1] = 224u;
                dst[2] = 238u;
                dst[3] = 14u;
            }
        }
        // Deterministic multi-depth drifting snowflakes
        const int flakeCount = std::clamp((winW * viewBottom) / 2800, 90, 380);
        const int spanW = winW + 48;
        const int spanH = viewBottom + 48;
        for (int i = 0; i < flakeCount; ++i) {
            const std::uint32_t h0 = hashWeather(std::uint32_t(i) * 0x85ebca6bu ^ 0xc2b2ae35u);
            const bool foreground = (h0 & 3u) == 0u;
            const int speedY = foreground ? 4 : (2 + int(h0 & 1u));
            const int driftX = 1 + int((h0 >> 3u) & 1u);
            const float sway = std::sin(float(weatherPhase_) * 0.08f + float(h0 & 0xFFu) * 0.05f) * (foreground ? 7.0f : 4.0f);
            const std::int64_t t = std::int64_t(weatherPhase_);
            int fx = int(((std::int64_t(h0 >> 8u) % spanW) + t * driftX + std::llround(sway)) % spanW);
            if (fx < 0) fx += spanW;
            fx -= 24;
            const int fy = int(((std::int64_t(h0 >> 17u) % spanH) + t * speedY) % spanH) - 24;
            const std::uint8_t coreA = foreground ? 204u : 148u;
            blendWeatherPixel(fx, fy, 244u, 248u, 255u, coreA);
            blendWeatherPixel(fx + 1, fy, 236u, 242u, 252u, static_cast<std::uint8_t>(coreA * 3u / 4u));
            blendWeatherPixel(fx, fy + 1, 236u, 242u, 252u, static_cast<std::uint8_t>(coreA * 3u / 4u));
            if (foreground) {
                blendWeatherPixel(fx - 1, fy, 232u, 240u, 250u, static_cast<std::uint8_t>(coreA / 2u));
                blendWeatherPixel(fx, fy - 1, 232u, 240u, 250u, static_cast<std::uint8_t>(coreA / 2u));
            }
        }
    }
}

void Hud::draw(VulkanWindow& renderer) {
    refit();
    if (weather_ == assets::WeatherKind::Clear) {
        if (!overlayWasWeather_ && drawn_ == revision_) return;
        renderer.setOverlay(width_, height_, rgba_, OverlaySlot::hud);
        drawn_ = revision_;
        weatherDrawn_ = weatherRevision_;
        overlayWasWeather_ = false;
        return;
    }
    if (overlayWasWeather_ && drawn_ == revision_ && weatherDrawn_ == weatherRevision_) return;
    composeWeatherOverlay();
    const unsigned fullWinH = static_cast<unsigned>(std::max(int(height_), top_ + int(height_)));
    renderer.setOverlay(width_, fullWinH, composedRgba_, OverlaySlot::hud);
    drawn_ = revision_;
    weatherDrawn_ = weatherRevision_;
    overlayWasWeather_ = true;
}
}
