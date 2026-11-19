#include "assets/hd_pack.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
    // 1. Round-trip SS2H encode/decode
    ss2::assets::MapImage src{4, 3, std::vector<std::uint8_t>(4 * 3 * 4, 0)};
    for (std::size_t i = 0; i < src.rgba.size(); i += 4) {
        src.rgba[i + 0] = static_cast<std::uint8_t>(i * 7);
        src.rgba[i + 1] = static_cast<std::uint8_t>(i * 13);
        src.rgba[i + 2] = static_cast<std::uint8_t>(i * 29);
        src.rgba[i + 3] = 255u;
    }
    auto packed = ss2::assets::encodeHdMap(src, 2);
    auto decoded = ss2::assets::decodeHdMap(packed);
    assert(decoded.scale == 2);
    assert(decoded.image.width == 4 && decoded.image.height == 3);
    assert(decoded.image.rgba == src.rgba);

    // 2. Reject corrupt magic & corrupt zlib
    auto badMagic = packed;
    badMagic[0] = 'X';
    bool threw = false;
    try { (void)ss2::assets::decodeHdMap(badMagic); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    auto badZlib = packed;
    badZlib.back() ^= 0xFFu;
    threw = false;
    try { (void)ss2::assets::decodeHdMap(badZlib); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    // 3. Verify EPX/Scale2x diagonal smoothing on a 3x3 corner pattern
    ss2::assets::MapImage diag{3, 3, std::vector<std::uint8_t>(3 * 3 * 4, 0)};
    auto setPx = [&](int x, int y, std::uint8_t r) {
        auto* p = diag.rgba.data() + (y * 3 + x) * 4;
        p[0] = r; p[3] = 255u;
    };
    // A(1,0) and C(0,1) are red=200; center(1,1), B(2,1), D(1,2) are black=0 -> top-left subpixel (2,2) becomes red=200!
    setPx(1, 0, 200);
    setPx(0, 1, 200);
    auto up = ss2::assets::upscalePatch2x(diag);
    assert(up.width == 6 && up.height == 6);
    assert(up.rgba[(2 * 6 + 2) * 4 + 0] == 200); // smoothed corner
    assert(up.rgba[(2 * 6 + 3) * 4 + 0] == 0);   // preserved interior

    std::cout << "hd_pack synthetic tests PASSED\n";
    return 0;
}
