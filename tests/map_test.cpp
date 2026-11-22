#include "assets/indexed.hpp"
#include "assets/terrain.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

int main() try {
    std::array<std::uint8_t, 16> info{};
    info[8] = info[12] = 1;
    std::array<std::uint8_t, 8> cells{};
    std::array<std::uint8_t, 1024> tile{};
    tile.fill(1);
    std::array<std::uint8_t, 512> palette{};
    palette[3] = 0xf8; // palette[1] = 0xf800, red
    auto map = ss2::assets::decodeTerrain(info, cells, tile, palette);
    if (map.width != 64 || map.height != 32) return 1;
    size_t reds = 0;
    for (size_t i = 0; i < map.rgba.size(); i += 4)
        if (map.rgba[i] == 255 && map.rgba[i+3] == 255) ++reds;
    if (reds != 1024) return 1;
    for (unsigned y = 0; y < 32; ++y) {
        unsigned row = 0;
        for (unsigned x = 0; x < 64; ++x) row += map.rgba[(64 * y + x) * 4 + 3] != 0;
        if (row != (y < 16 ? 4 * y + 3 : 61 - 4 * (y - 16))) return 1;
    }
    cells[6] = 1;
    try { ss2::assets::decodeTerrain(info, cells, tile, palette); return 1; }
    catch (const std::runtime_error&) {}

    // Three pixels, skip one, repeat two green. Exercise A2, negative origins, and clipping.
    palette[4] = 0xe0; palette[5] = 0x07;
    std::array<std::uint8_t, 14> frame{255,255,0,0,3,0,1,0,0xa2,3,0,0xc1,0x82,2};
    ss2::assets::drawIndexedFrame(map, 0, 0, frame, palette);
    if (map.rgba[1] != 255 || map.rgba[5] != 255 || map.rgba[9] != 0) return 1;
    frame[12] = 0x83;
    try { ss2::assets::drawIndexedFrame(map, 0, 0, frame, palette); return 1; }
    catch (const std::runtime_error&) {}

    // Synthetic 0xa3 shadow frame: negative left clipping, skip run, omitted row tail, alpha==0 guard,
    // and shadowMask deduplication (no double-darkening).
    ss2::assets::MapImage shMap{6, 2, std::vector<std::uint8_t>(6u * 2u * 4u, 255u)};
    for (std::size_t i = 0; i < shMap.rgba.size(); i += 4u) {
        shMap.rgba[i + 0u] = 200u;
        shMap.rgba[i + 1u] = 100u;
        shMap.rgba[i + 2u] = 40u;
        shMap.rgba[i + 3u] = 255u;
    }
    // Leave out-of-diamond corner pixel (5, 1) transparent.
    shMap.rgba[(1u * 6u + 5u) * 4u + 0u] = 0u;
    shMap.rgba[(1u * 6u + 5u) * 4u + 1u] = 0u;
    shMap.rgba[(1u * 6u + 5u) * 4u + 2u] = 0u;
    shMap.rgba[(1u * 6u + 5u) * 4u + 3u] = 0u;

    const std::vector<std::uint8_t> shSprite{
        0xff, 0xff, 0, 0, 7, 0, 2, 0, 0xa3, // left=-1, top=0, w=7, h=2
        3, 0, 0x02, 0x81, 0x03,             // row 0: shadow 2 (-1,0), skip 1 (1), shadow 3 (2,3,4), omit x=6
        2, 0, 0x85, 0x02                    // row 1: skip 5 (-1..3), shadow 2 (4,5)
    };
    std::vector<std::uint8_t> shMask(6u * 2u, 0u);
    ss2::assets::drawShadowFrame(shMap, 0, 0, shSprite, &shMask);

    const auto expectPx = [&](unsigned x, unsigned y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        const std::size_t p = (std::size_t(y) * 6u + x) * 4u;
        return shMap.rgba[p] == r && shMap.rgba[p + 1u] == g &&
               shMap.rgba[p + 2u] == b && shMap.rgba[p + 3u] == a;
    };
    if (!expectPx(0, 0, 100, 66, 61, 255) || !expectPx(1, 0, 200, 100, 40, 255) ||
        !expectPx(2, 0, 100, 66, 61, 255) || !expectPx(3, 0, 100, 66, 61, 255) ||
        !expectPx(4, 0, 100, 66, 61, 255) || !expectPx(5, 0, 200, 100, 40, 255) ||
        !expectPx(0, 1, 200, 100, 40, 255) || !expectPx(4, 1, 100, 66, 61, 255) ||
        !expectPx(5, 1, 0, 0, 0, 0)) {
        return 1;
    }
    if (shMask[0] != 1u || shMask[1] != 0u || shMask[10] != 1u || shMask[11] != 0u) return 1;

    // Second pass with same mask must not double-darken.
    ss2::assets::drawShadowFrame(shMap, 0, 0, shSprite, &shMask);
    if (!expectPx(0, 0, 100, 66, 61, 255) || !expectPx(4, 1, 100, 66, 61, 255)) return 1;

    // Invalid tag and overflowing run length must throw.
    auto badTag = shSprite;
    badTag[8] = 0xa2;
    try { ss2::assets::drawShadowFrame(shMap, 0, 0, badTag); return 1; }
    catch (const std::runtime_error&) {}
    auto badRun = shSprite;
    badRun.back() = 0x05;
    try { ss2::assets::drawShadowFrame(shMap, 0, 0, badRun); return 1; }
    catch (const std::runtime_error&) {}
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
