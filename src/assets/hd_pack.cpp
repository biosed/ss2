#include "assets/hd_pack.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <zlib.h>

namespace ss2::assets {
namespace {
constexpr std::uint32_t kHdMagic = 0x48325353u; // "SS2H" LE
constexpr std::uint32_t kMaxHdDim = 16384u;
constexpr std::uint64_t kMaxHdRgbaBytes = 512ull << 20; // 16384 * 8192 * 4 = 512 MiB
constexpr std::size_t kMaxPackedBytes = 256u << 20;

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    if (at > b.size() || b.size() - at < 4)
        throw std::runtime_error("Truncated SS2H header");
    return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) |
           (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
}

void pushU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

bool samePixel(const std::uint8_t* a, const std::uint8_t* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}
}

std::vector<std::uint8_t> encodeHdMap(const MapImage& image, std::uint32_t scale) {
    if (!image.width || !image.height || image.width > kMaxHdDim || image.height > kMaxHdDim ||
        scale < 1 || scale > 4)
        throw std::runtime_error("Invalid SS2H dimensions or scale");
    const std::uint64_t expected = std::uint64_t(image.width) * image.height * 4u;
    if (expected > kMaxHdRgbaBytes || image.rgba.size() != expected)
        throw std::runtime_error("Invalid SS2H RGBA buffer size");

    uLongf bound = compressBound(static_cast<uLong>(image.rgba.size()));
    std::vector<std::uint8_t> out;
    out.reserve(24 + bound);
    pushU32(out, kHdMagic);
    pushU32(out, 1u); // version
    pushU32(out, scale);
    pushU32(out, image.width);
    pushU32(out, image.height);
    pushU32(out, 0u); // placeholder for compressedSize
    out.resize(24 + bound);

    uLongf packedLen = bound;
    if (compress2(out.data() + 24, &packedLen, image.rgba.data(), static_cast<uLong>(image.rgba.size()), 1) != Z_OK)
        throw std::runtime_error("Failed to compress SS2H payload");
    out.resize(24 + packedLen);
    const std::uint32_t plen = static_cast<std::uint32_t>(packedLen);
    out[20] = static_cast<std::uint8_t>(plen & 0xFFu);
    out[21] = static_cast<std::uint8_t>((plen >> 8) & 0xFFu);
    out[22] = static_cast<std::uint8_t>((plen >> 16) & 0xFFu);
    out[23] = static_cast<std::uint8_t>((plen >> 24) & 0xFFu);
    return out;
}

HdMap decodeHdMap(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 24) throw std::runtime_error("Truncated SS2H file");
    if (u32(bytes, 0) != kHdMagic) throw std::runtime_error("Invalid SS2H magic");
    if (u32(bytes, 4) != 1u) throw std::runtime_error("Unsupported SS2H version");
    const std::uint32_t scale = u32(bytes, 8);
    const std::uint32_t width = u32(bytes, 12);
    const std::uint32_t height = u32(bytes, 16);
    const std::uint32_t packedLen = u32(bytes, 20);
    if (scale < 1 || scale > 4 || !width || !height || width > kMaxHdDim || height > kMaxHdDim)
        throw std::runtime_error("SS2H dimensions or scale out of range");
    const std::uint64_t expected = std::uint64_t(width) * height * 4u;
    if (expected > kMaxHdRgbaBytes)
        throw std::runtime_error("SS2H output exceeds memory limit");
    if (packedLen != bytes.size() - 24)
        throw std::runtime_error("SS2H compressed size mismatch");

    MapImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(expected))};
    uLongf outSize = static_cast<uLongf>(image.rgba.size());
    uLong inSize = static_cast<uLong>(packedLen);
    if (uncompress2(image.rgba.data(), &outSize, bytes.data() + 24, &inSize) != Z_OK ||
        outSize != image.rgba.size() || inSize != packedLen)
        throw std::runtime_error("Corrupt SS2H zlib stream");
    return {scale, std::move(image)};
}

HdMap loadHdPackMap(const std::filesystem::path& hdPackDir) {
    const auto path = std::filesystem::is_directory(hdPackDir) ? (hdPackDir / "tutorial1_2x.ss2hd") : hdPackDir;
    const auto size = std::filesystem::file_size(path);
    if (size < 24 || size > kMaxPackedBytes)
        throw std::runtime_error("HD pack file size out of range");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open HD pack file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Truncated HD pack file");
    return decodeHdMap(bytes);
}

MapImage upscalePatch2x(const MapImage& src) {
    if (!src.width || !src.height || src.width > kMaxHdDim / 2 || src.height > kMaxHdDim / 2)
        throw std::runtime_error("Invalid patch dimensions for 2x upscale");
    const std::uint64_t srcBytes = std::uint64_t(src.width) * src.height * 4u;
    if (src.rgba.size() != srcBytes || srcBytes * 4u > kMaxHdRgbaBytes)
        throw std::runtime_error("Invalid patch RGBA buffer for 2x upscale");

    const std::uint32_t w = src.width, h = src.height;
    const std::uint32_t dw = w * 2u, dh = h * 2u;
    MapImage dst{dw, dh, std::vector<std::uint8_t>(std::size_t(dw) * dh * 4u)};

    const auto px = [&](std::uint32_t x, std::uint32_t y) -> const std::uint8_t* {
        return src.rgba.data() + (std::size_t(y) * w + x) * 4u;
    };
    const auto put = [&](std::uint32_t x, std::uint32_t y, const std::uint8_t* c) {
        std::uint8_t* o = dst.rgba.data() + (std::size_t(y) * dw + x) * 4u;
        std::memcpy(o, c, 4);
    };

    // Classic EPX / Scale2x: preserves sharp diagonal silhouettes with zero haloing.
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t ym = y > 0 ? y - 1 : 0;
        const std::uint32_t yp = y + 1 < h ? y + 1 : h - 1;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t xm = x > 0 ? x - 1 : 0;
            const std::uint32_t xp = x + 1 < w ? x + 1 : w - 1;
            const auto* P = px(x, y);
            const auto* A = px(x, ym);
            const auto* B = px(xp, y);
            const auto* C = px(xm, y);
            const auto* D = px(x, yp);

            const auto* o00 = P;
            const auto* o10 = P;
            const auto* o01 = P;
            const auto* o11 = P;
            if (!samePixel(C, B) && !samePixel(A, D)) {
                if (samePixel(C, A)) o00 = A;
                if (samePixel(A, B)) o10 = B;
                if (samePixel(D, C)) o01 = C;
                if (samePixel(B, D)) o11 = D;
            }
            put(x * 2u, y * 2u, o00);
            put(x * 2u + 1u, y * 2u, o10);
            put(x * 2u, y * 2u + 1u, o01);
            put(x * 2u + 1u, y * 2u + 1u, o11);
        }
    }
    return dst;
}
}
