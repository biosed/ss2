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
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
