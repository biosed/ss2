#include "assets/infantry_scale.hpp"
#include "assets/hd_pack.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ss2::assets {
namespace {
constexpr std::uint32_t kMaxSpriteDim = 4096u;
constexpr std::uint64_t kMaxSpriteBytes = 64ull << 20;

bool samePixel(const std::uint8_t* a, const std::uint8_t* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

int unpremulChannel(std::uint8_t c, std::uint8_t a) {
    return a ? (int(c) * 255 + int(a) / 2) / int(a) : 0;
}

bool similarPremul(const std::uint8_t* p, const std::uint8_t* q) {
    if (samePixel(p, q)) return true;
    // Never treat transparent, semi-transparent shadow, and opaque pixels as similar.
    if (std::abs(int(p[3]) - int(q[3])) > 16) return false;
    if (p[3] == 0 || q[3] == 0) return false;

    const int dr = unpremulChannel(p[0], p[3]) - unpremulChannel(q[0], q[3]);
    const int dg = unpremulChannel(p[1], p[3]) - unpremulChannel(q[1], q[3]);
    const int db = unpremulChannel(p[2], p[3]) - unpremulChannel(q[2], q[3]);

    const int dy = (299 * dr + 587 * dg + 114 * db) / 1000;
    const int du = (-147 * dr - 289 * dg + 436 * db) / 1000;
    const int dv = (615 * dr - 515 * dg - 100 * db) / 1000;
    return std::abs(dy) <= 44 && std::abs(du) <= 18 && std::abs(dv) <= 18;
}

std::array<std::uint8_t, 4> avgPremul(const std::uint8_t* a, const std::uint8_t* b) {
    const std::uint8_t alpha = static_cast<std::uint8_t>((int(a[3]) + int(b[3]) + 1) / 2);
    if (alpha == 0) return {0, 0, 0, 0};
    return {
        static_cast<std::uint8_t>(std::min(int(alpha), (int(a[0]) + int(b[0]) + 1) / 2)),
        static_cast<std::uint8_t>(std::min(int(alpha), (int(a[1]) + int(b[1]) + 1) / 2)),
        static_cast<std::uint8_t>(std::min(int(alpha), (int(a[2]) + int(b[2]) + 1) / 2)),
        alpha,
    };
}

MapImage canonicalizePremul(const MapImage& src) {
    MapImage out{src.width, src.height, src.rgba};
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        const std::uint8_t a = out.rgba[i + 3];
        if (a == 0) {
            out.rgba[i + 0] = 0;
            out.rgba[i + 1] = 0;
            out.rgba[i + 2] = 0;
        } else {
            out.rgba[i + 0] = std::min(out.rgba[i + 0], a);
            out.rgba[i + 1] = std::min(out.rgba[i + 1], a);
            out.rgba[i + 2] = std::min(out.rgba[i + 2], a);
        }
    }
    return out;
}

MapImage upscaleSprite2x(const MapImage& src) {
    const std::uint32_t w = src.width;
    const std::uint32_t h = src.height;
    const std::uint32_t pw = w + 2u;
    const std::uint32_t ph = h + 2u;

    // Pad with a 1px transparent border so edge pixels smooth cleanly against transparency.
    MapImage padded{pw, ph, std::vector<std::uint8_t>(std::size_t(pw) * ph * 4u, 0u)};
    for (std::uint32_t y = 0; y < h; ++y) {
        std::memcpy(padded.rgba.data() + (std::size_t(y + 1u) * pw + 1u) * 4u,
                    src.rgba.data() + std::size_t(y) * w * 4u,
                    std::size_t(w) * 4u);
    }

    // Reuse existing exact EPX/Scale2x helper for all exact-match corners.
    const MapImage up = upscalePatch2x(padded);
    const std::uint32_t dw = w * 2u;
    const std::uint32_t dh = h * 2u;
    MapImage dst{dw, dh, std::vector<std::uint8_t>(std::size_t(dw) * dh * 4u, 0u)};
    for (std::uint32_t y = 0; y < dh; ++y) {
        std::memcpy(dst.rgba.data() + std::size_t(y) * dw * 4u,
                    up.rgba.data() + (std::size_t(y + 2u) * (pw * 2u) + 2u) * 4u,
                    std::size_t(dw) * 4u);
    }

    // Refine shaded diagonal silhouettes where adjacent diagonal pixels are perceptually close
    // (same alpha class and small YUV delta) without ever blending across transparency.
    const auto ppx = [&](std::uint32_t x, std::uint32_t y) -> const std::uint8_t* {
        return padded.rgba.data() + (std::size_t(y) * pw + x) * 4u;
    };
    const auto put = [&](std::uint32_t x, std::uint32_t y, const std::array<std::uint8_t, 4>& c) {
        std::memcpy(dst.rgba.data() + (std::size_t(y) * dw + x) * 4u, c.data(), 4u);
    };

    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto* A = ppx(x + 1u, y);
            const auto* B = ppx(x + 2u, y + 1u);
            const auto* C = ppx(x, y + 1u);
            const auto* D = ppx(x + 1u, y + 2u);
            if (!similarPremul(C, B) && !similarPremul(A, D)) {
                if (!samePixel(C, A) && similarPremul(C, A)) put(x * 2u, y * 2u, avgPremul(C, A));
                if (!samePixel(A, B) && similarPremul(A, B)) put(x * 2u + 1u, y * 2u, avgPremul(A, B));
                if (!samePixel(D, C) && similarPremul(D, C)) put(x * 2u, y * 2u + 1u, avgPremul(D, C));
                if (!samePixel(B, D) && similarPremul(B, D)) put(x * 2u + 1u, y * 2u + 1u, avgPremul(B, D));
            }
        }
    }
    return dst;
}

MapImage resampleAreaPremul(const MapImage& src, std::uint32_t dstW, std::uint32_t dstH) {
    const std::uint32_t srcW = src.width;
    const std::uint32_t srcH = src.height;

    // Separable non-negative box/area filter in premultiplied RGBA:
    // convex weights guarantee zero ringing and zero transparent-edge halos.
    std::vector<float> horiz(std::size_t(dstW) * srcH * 4u, 0.0f);
    const double scaleX = double(srcW) / double(dstW);
    for (std::uint32_t dx = 0; dx < dstW; ++dx) {
        const double x0 = double(dx) * scaleX;
        const double x1 = double(dx + 1u) * scaleX;
        const std::uint32_t sxBegin = static_cast<std::uint32_t>(std::floor(x0));
        const std::uint32_t sxEnd = std::min(srcW, static_cast<std::uint32_t>(std::ceil(x1)));
        const double invSpan = 1.0 / (x1 - x0);

        for (std::uint32_t y = 0; y < srcH; ++y) {
            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            for (std::uint32_t sx = sxBegin; sx < sxEnd; ++sx) {
                const double overlap = std::min(double(sx + 1u), x1) - std::max(double(sx), x0);
                if (overlap <= 0.0) continue;
                const double w = overlap * invSpan;
                const std::uint8_t* p = src.rgba.data() + (std::size_t(y) * srcW + sx) * 4u;
                acc[0] += double(p[0]) * w;
                acc[1] += double(p[1]) * w;
                acc[2] += double(p[2]) * w;
                acc[3] += double(p[3]) * w;
            }
            float* out = horiz.data() + (std::size_t(y) * dstW + dx) * 4u;
            out[0] = static_cast<float>(acc[0]);
            out[1] = static_cast<float>(acc[1]);
            out[2] = static_cast<float>(acc[2]);
            out[3] = static_cast<float>(acc[3]);
        }
    }

    MapImage dst{dstW, dstH, std::vector<std::uint8_t>(std::size_t(dstW) * dstH * 4u, 0u)};
    const double scaleY = double(srcH) / double(dstH);
    const auto toByte = [](double v) -> std::uint8_t {
        const long rounded = std::lround(v);
        return static_cast<std::uint8_t>(std::clamp(rounded, 0l, 255l));
    };

    for (std::uint32_t dy = 0; dy < dstH; ++dy) {
        const double y0 = double(dy) * scaleY;
        const double y1 = double(dy + 1u) * scaleY;
        const std::uint32_t syBegin = static_cast<std::uint32_t>(std::floor(y0));
        const std::uint32_t syEnd = std::min(srcH, static_cast<std::uint32_t>(std::ceil(y1)));
        const double invSpan = 1.0 / (y1 - y0);

        for (std::uint32_t dx = 0; dx < dstW; ++dx) {
            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            for (std::uint32_t sy = syBegin; sy < syEnd; ++sy) {
                const double overlap = std::min(double(sy + 1u), y1) - std::max(double(sy), y0);
                if (overlap <= 0.0) continue;
                const double w = overlap * invSpan;
                const float* p = horiz.data() + (std::size_t(sy) * dstW + dx) * 4u;
                acc[0] += double(p[0]) * w;
                acc[1] += double(p[1]) * w;
                acc[2] += double(p[2]) * w;
                acc[3] += double(p[3]) * w;
            }
            const std::uint8_t a = toByte(acc[3]);
            std::uint8_t* out = dst.rgba.data() + (std::size_t(dy) * dstW + dx) * 4u;
            if (a == 0) {
                out[0] = 0;
                out[1] = 0;
                out[2] = 0;
                out[3] = 0;
            } else {
                out[0] = std::min(toByte(acc[0]), a);
                out[1] = std::min(toByte(acc[1]), a);
                out[2] = std::min(toByte(acc[2]), a);
                out[3] = a;
            }
        }
    }
    return dst;
}
} // namespace

MapImage resampleInfantry(const MapImage& premultipliedRgba, unsigned targetWidth, unsigned targetHeight) {
    if (!premultipliedRgba.width || !premultipliedRgba.height || !targetWidth || !targetHeight ||
        premultipliedRgba.width > kMaxSpriteDim || premultipliedRgba.height > kMaxSpriteDim ||
        targetWidth > kMaxSpriteDim || targetHeight > kMaxSpriteDim) {
        throw std::runtime_error("Invalid infantry sprite dimensions");
    }
    const std::uint64_t srcBytes = std::uint64_t(premultipliedRgba.width) * premultipliedRgba.height * 4u;
    const std::uint64_t dstBytes = std::uint64_t(targetWidth) * targetHeight * 4u;
    if (srcBytes > kMaxSpriteBytes || dstBytes > kMaxSpriteBytes || premultipliedRgba.rgba.size() != srcBytes) {
        throw std::runtime_error("Invalid infantry sprite RGBA buffer");
    }

    MapImage cur = canonicalizePremul(premultipliedRgba);
    if (cur.width == targetWidth && cur.height == targetHeight) return cur;

    while ((cur.width < targetWidth || cur.height < targetHeight) &&
           cur.width <= kMaxSpriteDim / 2u && cur.height <= kMaxSpriteDim / 2u &&
           std::uint64_t(cur.width) * 2u * std::uint64_t(cur.height) * 2u * 4u <= kMaxSpriteBytes) {
        cur = upscaleSprite2x(cur);
    }

    if (cur.width == targetWidth && cur.height == targetHeight) return cur;
    return resampleAreaPremul(cur, targetWidth, targetHeight);
}
} // namespace ss2::assets
