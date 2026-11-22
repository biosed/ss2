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

    // Synthetic 0xa9 alpha-indexed frame + 0xCA000002 .ANI sequence (n2Cad800.dll 0x10008c38..0x10008f1b).
    std::array<std::uint8_t, 1024> pal1024{};
    for (std::size_t e = 0u; e < 256u; ++e) pal1024[e * 4u + 2u] = 0xfeu; // default bgWeight5=31 transparent
    // [1]: 100% opaque pure red (0xf800, bgWeight5=0).
    pal1024[1u * 4u + 0u] = 0x00u;
    pal1024[1u * 4u + 1u] = 0xf8u;
    pal1024[1u * 4u + 2u] = 0x00u;
    // [2]: 50% black drop shadow (premul=0x0000, byte2=0x80 -> bgWeight5=16).
    pal1024[2u * 4u + 0u] = 0x00u;
    pal1024[2u * 4u + 1u] = 0x00u;
    pal1024[2u * 4u + 2u] = 0x80u;
    // [3]: 100% transparent fringe (bgWeight5=31).
    pal1024[3u * 4u + 2u] = 0xf8u;

    const std::vector<std::uint8_t> a9Frame{
        0xff, 0xff, 0xfe, 0xff, 3, 0, 1, 0, 0xa9, // dx=-1, dy=-2, w=3, h=1
        4, 0, 0x03, 1, 2, 3                        // literal [1, 2, 3]
    };
    ss2::assets::MapImage a9Bg{3, 1, {100, 120, 140, 255, 100, 120, 140, 255, 100, 120, 140, 255}};
    ss2::assets::drawAlphaIndexedFrame(a9Bg, 1, 2, a9Frame, pal1024);
    if (a9Bg.rgba[0] != 255 || a9Bg.rgba[1] != 0 || a9Bg.rgba[2] != 0 || a9Bg.rgba[3] != 255) return 1;
    if (a9Bg.rgba[4] != 50 || a9Bg.rgba[5] != 60 || a9Bg.rgba[6] != 70 || a9Bg.rgba[7] != 255) return 1;
    if (a9Bg.rgba[8] != 100 || a9Bg.rgba[9] != 120 || a9Bg.rgba[10] != 140 || a9Bg.rgba[11] != 255) return 1;

    // Pack two 0xa9 frames into a 0xCA000002 .ANI container and verify union canvas & hotspot.
    const std::vector<std::uint8_t> a9Frame2{
        0x00, 0x00, 0x00, 0x00, 2, 0, 2, 0, 0xa9, // dx=0, dy=0, w=2, h=2
        2, 0, 0x82, 1,                             // row 0: repeat 2x [1]
        2, 0, 0x82, 0                              // row 1: skip 2
    };
    std::vector<std::uint8_t> ani;
    const auto pushU32 = [&](std::uint32_t v) {
        ani.push_back(std::uint8_t(v & 0xffu));
        ani.push_back(std::uint8_t((v >> 8u) & 0xffu));
        ani.push_back(std::uint8_t((v >> 16u) & 0xffu));
        ani.push_back(std::uint8_t((v >> 24u) & 0xffu));
    };
    pushU32(0xca000002u);
    pushU32(2u);
    const std::uint32_t off0 = 8u + 3u * 4u;
    const std::uint32_t off1 = off0 + std::uint32_t(a9Frame.size());
    const std::uint32_t off2 = off1 + std::uint32_t(a9Frame2.size());
    pushU32(off0);
    pushU32(off1);
    pushU32(off2);
    ani.insert(ani.end(), a9Frame.begin(), a9Frame.end());
    ani.insert(ani.end(), a9Frame2.begin(), a9Frame2.end());

    if (ss2::assets::aniFrameCount(ani) != 2u) return 1;
    const auto seq = ss2::assets::decodeAniSequence(ani, pal1024);
    // Union of [-1..2)x[-2..0) and [0..2)x[0..2) -> [-1..2)x[-2..2) => 3x4, hot=(1, 2).
    if (seq.hotX != 1 || seq.hotY != 2 || seq.width != 3u || seq.height != 4u || seq.frames.size() != 2u) return 1;
    // Frame 0 pixel (0,0) = index [1] opaque red; (1,0) = index [2] 50% black shadow (alpha 128); (2,0) = transparent.
    if (seq.frames[0].rgba[0] != 255 || seq.frames[0].rgba[3] != 255) return 1;
    if (seq.frames[0].rgba[4] != 0 || seq.frames[0].rgba[7] != 128) return 1;
    if (seq.frames[0].rgba[11] != 0) return 1;
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
