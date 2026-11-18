/*
Copyright (c) 2024 - 2025 Americus Maximus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "assets/terrain.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;
constexpr std::size_t maxGridSide = 128;
constexpr std::size_t maxOutputBytes = 128u << 20;
constexpr std::size_t rhombBytes = 1024;
constexpr std::size_t paletteBytes = 512;
constexpr std::size_t maxInfoBytes = 64;
constexpr std::size_t maxRhombsBytes = maxGridSide * maxGridSide * 8;
constexpr std::size_t maxResBytes = 64u << 20;
constexpr std::size_t maxPlBytes = 1u << 20;

std::uint16_t u16(View bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2)
        throw std::runtime_error("Truncated terrain record");
    return static_cast<std::uint16_t>(bytes[at] | (unsigned(bytes[at + 1]) << 8));
}

std::uint32_t u32(View bytes, std::size_t at) {
    return u16(bytes, at) | (std::uint32_t(u16(bytes, at + 2)) << 16);
}

Bytes readBoundedFile(const std::filesystem::path& path, std::size_t limit) {
    const auto size = std::filesystem::file_size(path);
    if (size > limit) throw std::runtime_error("Terrain file exceeds reader limit");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open terrain file");
    Bytes bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Truncated terrain file");
    return bytes;
}

std::array<std::uint8_t, 4> rgb565ToRgba(std::uint16_t word) {
    const auto r5 = static_cast<std::uint8_t>((word >> 11) & 31u);
    const auto g6 = static_cast<std::uint8_t>((word >> 5) & 63u);
    const auto b5 = static_cast<std::uint8_t>(word & 31u);
    return {
        static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)),
        static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
        static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)),
        255u,
    };
}
}

MapImage decodeTerrain(View info, View rhombs, View rhombsRes, View rhombsPl) {
    if (info.size() < 16 || info.size() > maxInfoBytes)
        throw std::runtime_error("Invalid terrain info record");
    const std::uint32_t gridW = u32(info, 8);
    const std::uint32_t gridH = u32(info, 12);
    if (!gridW || !gridH || gridW > maxGridSide || gridH > maxGridSide)
        throw std::runtime_error("Terrain grid dimensions out of range");

    const std::size_t cellCount = std::size_t(gridW) * gridH;
    if (rhombs.size() != cellCount * 8)
        throw std::runtime_error("Terrain rhomb map size mismatch");
    if (rhombsRes.empty() || rhombsRes.size() > maxResBytes || (rhombsRes.size() % rhombBytes) != 0)
        throw std::runtime_error("Invalid rhombs.res archive size");
    if (rhombsPl.empty() || rhombsPl.size() > maxPlBytes || (rhombsPl.size() % paletteBytes) != 0)
        throw std::runtime_error("Invalid rhombs.pl palette size");

    const std::uint32_t width = (gridW + gridH) * 32u;
    const std::uint32_t height = (gridW + gridH) * 16u;
    const std::uint64_t outBytes = std::uint64_t(width) * height * 4u;
    if (outBytes > maxOutputBytes)
        throw std::runtime_error("Terrain RGBA output exceeds limit");

    const std::size_t tileCount = rhombsRes.size() / rhombBytes;
    const std::size_t paletteCount = rhombsPl.size() / paletteBytes;
    const bool useDithered64 = paletteCount >= 64;
    const std::size_t maxLightLevel = std::min<std::size_t>(32, paletteCount);

    std::vector<std::array<std::uint8_t, 4>> palettes(paletteCount * 256);
    for (std::size_t i = 0; i < palettes.size(); ++i)
        palettes[i] = rgb565ToRgba(u16(rhombsPl, i * 2));

    std::vector<std::uint8_t> lights(cellCount);
    std::vector<std::uint16_t> tiles(cellCount);
    for (std::size_t i = 0; i < cellCount; ++i) {
        const std::size_t lightLevel = u16(rhombs, i * 8 + 4) >> 4;
        const std::uint16_t tileIndex = u16(rhombs, i * 8 + 6);
        if (lightLevel >= maxLightLevel || tileIndex >= tileCount)
            throw std::runtime_error("Terrain cell index out of range");
        lights[i] = static_cast<std::uint8_t>(lightLevel);
        tiles[i] = tileIndex;
    }

    const auto lightAt = [&](std::uint32_t cx, std::uint32_t cy) -> int {
        const auto x = std::min(cx, gridW - 1u);
        const auto y = std::min(cy, gridH - 1u);
        return int(lights[std::size_t(y) * gridW + x]);
    };

    MapImage map{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(outBytes), 0)};

    for (std::uint32_t y = 0; y < gridH; ++y) {
        for (std::uint32_t x = 0; x < gridW; ++x) {
            const auto* tile = rhombsRes.data() + std::size_t(tiles[std::size_t(y) * gridW + x]) * rhombBytes;
            const std::size_t originX = std::size_t(x + (gridH - 1u) - y) * 32u;
            const std::size_t originY = std::size_t(x + y) * 16u;

            // WarStorm N2CAD800/Renderer.cxx interpolates corner light values and alternates palette banks.
            const int angle2 = lightAt(x, y);         // top
            const int angle0 = lightAt(x, y + 1u);    // left
            const int angle1 = lightAt(x + 1u, y);    // right
            const int angle3 = lightAt(x + 1u, y + 1u); // bottom
            const int diff = (angle1 - angle0) << 2;
            const unsigned rowToggle = useDithered64 ? 0x20u : 0u;
            const unsigned pxToggle = useDithered64 ? 0x2000u : 0u;

            std::size_t src = 0;
            unsigned rowParity = 0;

            const auto drawRow = [&](std::size_t screenY, std::size_t screenX, std::size_t span, int txDelta) {
                std::uint8_t* dst = map.rgba.data() + (screenY * width + screenX) * 4u;
                std::uint16_t shade = static_cast<std::uint16_t>(
                    ((static_cast<unsigned>(txDelta >> 8) ^ rowParity) << 8) | (static_cast<unsigned>(txDelta) & 0xFFu));
                rowParity ^= rowToggle;
                for (std::size_t dx = 0; dx < span; ++dx) {
                    const std::size_t palIdx = useDithered64
                        ? std::size_t((shade >> 8) & 63u)
                        : std::min<std::size_t>((shade >> 8) & 31u, paletteCount - 1u);
                    const auto& color = palettes[palIdx * 256u + tile[src++]];
                    std::copy(color.begin(), color.end(), dst);
                    dst += 4;
                    shade = static_cast<std::uint16_t>((shade + static_cast<std::uint16_t>(diff)) ^ pxToggle);
                }
            };

            // Upper 16 scanlines: widths 3, 7, 11, ..., 63 (528 pixels).
            int vertDiff = (angle0 - angle2) << 4;
            int txDelta = (angle2 << 8) + vertDiff + diff;
            for (std::size_t dy = 0; dy < 16; ++dy) {
                const std::size_t span = dy * 4u + 3u;
                const std::size_t sx = originX + 31u - dy * 2u;
                drawRow(originY + dy, sx, span, txDelta);
                txDelta += vertDiff;
            }

            // Lower 16 scanlines: widths 61, 57, 53, ..., 1 (496 pixels).
            rowParity ^= rowToggle;
            vertDiff = (angle3 - angle0) << 4;
            txDelta = (angle0 << 8) + vertDiff;
            for (std::size_t k = 0; k < 16; ++k) {
                const std::size_t span = 61u - k * 4u;
                const std::size_t sx = originX + 2u + k * 2u;
                drawRow(originY + 16u + k, sx, span, txDelta);
                txDelta += vertDiff;
            }
        }
    }
    return map;
}

MapImage loadTutorialTerrain(const std::filesystem::path& gameDir) {
    const auto mapDir = gameDir / "maps.src/map.020";
    const auto info = readBoundedFile(mapDir / "info", maxInfoBytes);
    const auto rhombs = readBoundedFile(mapDir / "rhombs", maxRhombsBytes);
    const auto archive = gameDir / "game_scheme1.sue";
    const auto rhombsRes = readSueEntry(archive, "rhombs.res");
    const auto rhombsPl = readSueEntry(archive, "rhombs.pl");
    return decodeTerrain(info, rhombs, rhombsRes, rhombsPl);
}
}
