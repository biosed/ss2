#include "assets/infantry_scale.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
using ss2::assets::MapImage;
using ss2::assets::resampleInfantry;
using Px = std::array<std::uint8_t, 4>;

void setPx(MapImage& img, unsigned x, unsigned y, Px c) {
    auto* p = img.rgba.data() + (std::size_t(y) * img.width + x) * 4u;
    p[0] = c[0];
    p[1] = c[1];
    p[2] = c[2];
    p[3] = c[3];
}

Px getPx(const MapImage& img, unsigned x, unsigned y) {
    const auto* p = img.rgba.data() + (std::size_t(y) * img.width + x) * 4u;
    return {p[0], p[1], p[2], p[3]};
}

void assertPremulInvariant(const MapImage& img) {
    assert(img.width > 0 && img.height > 0);
    assert(img.rgba.size() == std::size_t(img.width) * img.height * 4u);
    for (std::size_t i = 0; i < img.rgba.size(); i += 4u) {
        const std::uint8_t r = img.rgba[i + 0];
        const std::uint8_t g = img.rgba[i + 1];
        const std::uint8_t b = img.rgba[i + 2];
        const std::uint8_t a = img.rgba[i + 3];
        if (a == 0) {
            assert(r == 0 && g == 0 && b == 0);
        } else {
            assert(r <= a && g <= a && b <= a);
        }
    }
}

double totalAlphaFraction(const MapImage& img) {
    double sum = 0.0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4u) sum += double(img.rgba[i]) / 255.0;
    return sum / (double(img.width) * double(img.height));
}

MapImage makeStandingSoldier() {
    // Representative 16x24 standing infantryman: helmet, olive uniform, horizontal rifle, boots, semi-transparent shadow.
    MapImage img{16, 24, std::vector<std::uint8_t>(16u * 24u * 4u, 0u)};
    const Px helmet{112, 124, 92, 255};
    const Px helmetHi{152, 164, 128, 255};
    const Px face{216, 176, 140, 255};
    const Px uniform1{84, 96, 60, 255};
    const Px uniform2{76, 88, 54, 255};
    const Px rifle{68, 56, 44, 255};
    const Px boot{44, 36, 28, 255};
    const Px shadow{16, 20, 12, 128}; // premultiplied 50% shadow

    // Shadow ellipse at feet
    for (unsigned x = 4; x <= 12; ++x) setPx(img, x, 21, shadow);
    for (unsigned x = 3; x <= 13; ++x) setPx(img, x, 22, shadow);
    for (unsigned x = 5; x <= 11; ++x) setPx(img, x, 23, shadow);

    // Helmet + 1px highlight
    for (unsigned x = 6; x <= 9; ++x) setPx(img, x, 2, helmet);
    for (unsigned x = 5; x <= 10; ++x) setPx(img, x, 3, helmet);
    setPx(img, 7, 2, helmetHi);

    // Face
    for (unsigned x = 6; x <= 9; ++x) setPx(img, x, 4, face);
    for (unsigned x = 7; x <= 9; ++x) setPx(img, x, 5, face);

    // Torso (shaded olive)
    for (unsigned y = 6; y <= 13; ++y) {
        for (unsigned x = 5; x <= 10; ++x) {
            setPx(img, x, y, (x + y) % 2 == 0 ? uniform1 : uniform2);
        }
    }

    // 1px horizontal rifle barrel protruding right
    for (unsigned x = 9; x <= 14; ++x) setPx(img, x, 9, rifle);

    // Standing legs + boots
    for (unsigned y = 14; y <= 19; ++y) {
        setPx(img, 6, y, uniform1);
        setPx(img, 7, y, uniform2);
        setPx(img, 9, y, uniform1);
        setPx(img, 10, y, uniform2);
    }
    setPx(img, 6, 20, boot);
    setPx(img, 7, 20, boot);
    setPx(img, 9, 20, boot);
    setPx(img, 10, 20, boot);
    return img;
}

MapImage makeRunningSoldier() {
    // Representative 18x24 running infantryman with diagonal torso lean, diagonal legs, angled rifle, and shadow.
    MapImage img{18, 24, std::vector<std::uint8_t>(18u * 24u * 4u, 0u)};
    const Px helmet{108, 120, 88, 255};
    const Px face{216, 176, 140, 255};
    const Px uniform1{88, 100, 64, 255};
    const Px uniform2{80, 92, 58, 255};
    const Px rifle{72, 60, 48, 255};
    const Px boot{48, 40, 32, 255};
    const Px shadow{16, 20, 12, 128};

    // Diagonal shadow under stride
    for (unsigned x = 2; x <= 15; ++x) setPx(img, x, 21, shadow);
    for (unsigned x = 3; x <= 14; ++x) setPx(img, x, 22, shadow);

    // Forward-leaning head
    for (unsigned x = 9; x <= 12; ++x) setPx(img, x, 3, helmet);
    for (unsigned x = 8; x <= 13; ++x) setPx(img, x, 4, helmet);
    for (unsigned x = 10; x <= 12; ++x) setPx(img, x, 5, face);

    // Leaning torso
    for (unsigned y = 6; y <= 13; ++y) {
        const unsigned shift = (y < 10) ? 1u : 0u;
        for (unsigned x = 7 + shift; x <= 11 + shift; ++x) {
            setPx(img, x, y, (x < 9 + shift) ? uniform1 : uniform2);
        }
    }

    // Diagonal angled rifle across body
    for (unsigned i = 0; i < 8; ++i) {
        setPx(img, 6 + i, 12 - i / 2, rifle);
    }

    // Diagonal striding legs
    for (unsigned i = 0; i < 6; ++i) {
        // Back leg going down-left
        setPx(img, 7 - i / 2, 14 + i, uniform1);
        setPx(img, 8 - i / 2, 14 + i, uniform2);
        // Front leg going down-right
        setPx(img, 10 + i / 2, 14 + i, uniform1);
        setPx(img, 11 + i / 2, 14 + i, uniform2);
    }
    setPx(img, 4, 20, boot);
    setPx(img, 5, 20, boot);
    setPx(img, 13, 20, boot);
    setPx(img, 14, 20, boot);
    return img;
}
} // namespace

int main() {
    // 1. Validation checks
    {
        MapImage empty{0, 4, {}};
        bool threw = false;
        try { (void)resampleInfantry(empty, 8, 8); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);

        MapImage badBuf{4, 4, std::vector<std::uint8_t>(10, 0)};
        threw = false;
        try { (void)resampleInfantry(badBuf, 8, 8); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);

        MapImage ok{4, 4, std::vector<std::uint8_t>(4 * 4 * 4, 0)};
        threw = false;
        try { (void)resampleInfantry(ok, 0, 8); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);

        threw = false;
        try { (void)resampleInfantry(ok, 8192, 8); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);
    }

    // 2. 1x identity + premultiplied canonicalization
    {
        MapImage src{2, 2, std::vector<std::uint8_t>(2 * 2 * 4, 0)};
        setPx(src, 0, 0, {200, 150, 100, 0});   // stray RGB on transparent pixel
        setPx(src, 1, 0, {180, 120, 60, 128});  // RGB > alpha
        setPx(src, 0, 1, {90, 100, 70, 255});
        auto out = resampleInfantry(src, 2, 2);
        assertPremulInvariant(out);
        assert(getPx(out, 0, 0) == (Px{0, 0, 0, 0}));
        assert(getPx(out, 1, 0) == (Px{128, 120, 60, 128}));
        assert(getPx(out, 0, 1) == (Px{90, 100, 70, 255}));
    }

    // 3. Crisp 45-degree diagonal silhouette + exact palette + zero transparent-edge halos
    {
        MapImage diag{4, 4, std::vector<std::uint8_t>(4 * 4 * 4, 0)};
        const Px bright{200, 180, 120, 255};
        // 45-degree diagonal stripe
        setPx(diag, 2, 0, bright);
        setPx(diag, 1, 1, bright);
        setPx(diag, 0, 2, bright);

        auto up2 = resampleInfantry(diag, 8, 8);
        assertPremulInvariant(up2);
        // At source (2,1) transparent, A=(2,0) and C=(1,1) are both `bright` -> top-left subpixel (4,2) smoothed!
        assert(getPx(up2, 4, 2) == bright);
        // Opposite corner (5,3) remains transparent
        assert(getPx(up2, 5, 3) == (Px{0, 0, 0, 0}));
        // Every pixel is either exact `bright` or exact transparent (zero halos, exact palette)
        for (unsigned y = 0; y < up2.height; ++y) {
            for (unsigned x = 0; x < up2.width; ++x) {
                const Px p = getPx(up2, x, y);
                assert(p == bright || p == (Px{0, 0, 0, 0}));
            }
        }

        // Non-integer 1.5x and 3x: verify ZERO dark halo on every semi-transparent edge pixel!
        for (const auto& dims : std::array<std::pair<unsigned, unsigned>, 3>{{{6, 6}, {10, 10}, {12, 12}}}) {
            auto res = resampleInfantry(diag, dims.first, dims.second);
            assertPremulInvariant(res);
            unsigned nonZero = 0;
            for (unsigned y = 0; y < res.height; ++y) {
                for (unsigned x = 0; x < res.width; ++x) {
                    const Px p = getPx(res, x, y);
                    if (p[3] == 0) continue;
                    ++nonZero;
                    // Unpremultiplied color must match `bright` within rounding tolerance (no black halo!)
                    const int ur = (int(p[0]) * 255 + int(p[3]) / 2) / int(p[3]);
                    const int ug = (int(p[1]) * 255 + int(p[3]) / 2) / int(p[3]);
                    const int ub = (int(p[2]) * 255 + int(p[3]) / 2) / int(p[3]);
                    const int tol = p[3] < 32 ? 12 : 3;
                    assert(std::abs(ur - int(bright[0])) <= tol);
                    assert(std::abs(ug - int(bright[1])) <= tol);
                    assert(std::abs(ub - int(bright[2])) <= tol);
                }
            }
            assert(nonZero > 0);
        }
    }

    // 4. Shaded diagonal smoothing (two close uniform shades against transparent)
    {
        MapImage shaded{3, 3, std::vector<std::uint8_t>(3 * 3 * 4, 0)};
        const Px oliveA{92, 104, 68, 255};
        const Px oliveC{84, 96, 62, 255};
        setPx(shaded, 1, 0, oliveA);
        setPx(shaded, 0, 1, oliveC);

        auto up2 = resampleInfantry(shaded, 6, 6);
        assertPremulInvariant(up2);
        // Center (1,1) top-left subpixel (2,2) should be smoothed opaque olive midpoint, not transparent!
        const Px smoothed = getPx(up2, 2, 2);
        assert(smoothed[3] == 255);
        assert(smoothed[0] == 88 && smoothed[1] == 100 && smoothed[2] == 65);
        assert(getPx(up2, 3, 3) == (Px{0, 0, 0, 0}));
    }

    // 5. Semi-transparent shadow adjacent to opaque body: no cross-alpha bleed
    {
        MapImage sh{3, 3, std::vector<std::uint8_t>(3 * 3 * 4, 0)};
        const Px body{180, 160, 120, 255};
        const Px shadow{16, 16, 16, 128};
        setPx(sh, 1, 0, body);
        setPx(sh, 0, 1, shadow);
        auto up2 = resampleInfantry(sh, 6, 6);
        assertPremulInvariant(up2);
        // Body (a=255) and shadow (a=128) must NOT be treated as similar -> (2,2) stays transparent
        assert(getPx(up2, 2, 2) == (Px{0, 0, 0, 0}));
        // Both body and shadow are preserved at full fidelity
        assert(getPx(up2, 2, 0) == body);
        assert(getPx(up2, 0, 2) == shadow);
    }

    // 6. Thin 1px horizontal rifle & isolated 1px highlight preserved
    {
        MapImage thin{5, 5, std::vector<std::uint8_t>(5 * 5 * 4, 0)};
        const Px rifle{70, 58, 46, 255};
        const Px dot{240, 240, 200, 255};
        setPx(thin, 1, 2, rifle);
        setPx(thin, 2, 2, rifle);
        setPx(thin, 3, 2, rifle);
        setPx(thin, 2, 0, dot);

        auto up2 = resampleInfantry(thin, 10, 10);
        assertPremulInvariant(up2);
        for (unsigned x = 2; x <= 7; ++x) {
            assert(getPx(up2, x, 4) == rifle);
            assert(getPx(up2, x, 5) == rifle);
        }
        assert(getPx(up2, 4, 0) == dot && getPx(up2, 5, 1) == dot);
    }

    // 7. Representative standing & running infantry frames across 2x, 3x, 1.5x, and +/-25% target bounds
    {
        const MapImage standing = makeStandingSoldier();
        const MapImage running = makeRunningSoldier();
        const double standAlpha = totalAlphaFraction(standing);
        const double runAlpha = totalAlphaFraction(running);

        const std::array<std::pair<unsigned, unsigned>, 5> standTargets{{
            {32, 48}, {48, 72}, {24, 36}, {20, 30}, {12, 18}
        }};
        for (const auto& [tw, th] : standTargets) {
            auto out = resampleInfantry(standing, tw, th);
            assert(out.width == tw && out.height == th);
            assertPremulInvariant(out);
            assert(std::abs(totalAlphaFraction(out) - standAlpha) < 0.05);
            // Verify opaque and semi-transparent shadow alpha classes both exist
            bool hasOpaque = false, hasShadow = false, hasTrans = false;
            for (std::size_t i = 3; i < out.rgba.size(); i += 4u) {
                if (out.rgba[i] == 255) hasOpaque = true;
                else if (out.rgba[i] >= 96 && out.rgba[i] <= 160) hasShadow = true;
                else if (out.rgba[i] == 0) hasTrans = true;
            }
            assert(hasOpaque && hasShadow && hasTrans);
        }

        const std::array<std::pair<unsigned, unsigned>, 5> runTargets{{
            {36, 48}, {54, 72}, {27, 36}, {40, 54}, {14, 18}
        }};
        for (const auto& [tw, th] : runTargets) {
            auto out = resampleInfantry(running, tw, th);
            assert(out.width == tw && out.height == th);
            assertPremulInvariant(out);
            assert(std::abs(totalAlphaFraction(out) - runAlpha) < 0.05);
        }
    }

    std::cout << "infantry_scale tests PASSED\n";
    return 0;
}
