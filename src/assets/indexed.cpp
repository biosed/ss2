#include "assets/indexed.hpp"
#include <algorithm>
#include <stdexcept>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;

unsigned word(View v, std::size_t i) {
    if (i > v.size() || v.size() - i < 2) throw std::runtime_error("Truncated indexed sprite");
    return v[i] | unsigned(v[i + 1]) << 8;
}
}

SpriteCoverage readShadowCoverage(int originX, int originY, View sprite) {
    if (sprite.size() < 9 || sprite[8] != 0xa3) throw std::runtime_error("Invalid stand shadow");
    const int width = word(sprite,4), height = word(sprite,6);
    if (width < 1 || height < 1 || width > 2048 || height > 2048)
        throw std::runtime_error("Invalid stand shadow dimensions");
    SpriteCoverage result{originX+std::int16_t(word(sprite,0)), originY+std::int16_t(word(sprite,2)), width,
                          height, std::vector<std::uint8_t>(std::size_t(width)*height)};
    std::size_t pos = 9;
    for (int y = 0; y < height; ++y) {
        const auto size = word(sprite,pos);
        pos += 2;
        if (size > sprite.size()-pos) throw std::runtime_error("Truncated stand shadow row");
        int x = 0;
        for (const auto op : sprite.subspan(pos,size)) {
            const int count = op & 127;
            if (count > width-x) throw std::runtime_error("Stand shadow run outside sprite");
            if (!(op & 128)) std::fill_n(result.pixels.begin()+std::size_t(y)*width+x, count, 1);
            x += count;
        }
        pos += size;
    }
    return result;
}

void drawShadowFrame(MapImage& map, int originX, int originY, View sprite, std::vector<std::uint8_t>* shadowMask) {
    if (sprite.size() < 9 || sprite[8] != 0xa3) throw std::runtime_error("Invalid shadow sprite");
    if (!map.width || !map.height || (std::uint64_t(map.width) * map.height > map.rgba.size() / 4))
        throw std::runtime_error("Invalid shadow destination");
    const int width = word(sprite, 4), height = word(sprite, 6);
    if (width < 1 || height < 1 || width > 2048 || height > 2048)
        throw std::runtime_error("Invalid shadow sprite dimensions");
    if (shadowMask && shadowMask->size() != std::size_t(map.width) * map.height)
        throw std::runtime_error("Invalid shadow mask size");
    const auto left = std::int64_t(originX) + std::int16_t(word(sprite, 0));
    const auto top = std::int64_t(originY) + std::int16_t(word(sprite, 2));
    std::size_t pos = 9;
    for (int y = 0; y < height; ++y) {
        const auto size = word(sprite, pos);
        pos += 2;
        if (size > sprite.size() - pos) throw std::runtime_error("Truncated shadow row");
        const auto dy = top + y;
        const bool rowVisible = dy >= 0 && dy < std::int64_t(map.height);
        int x = 0;
        for (const auto op : sprite.subspan(pos, size)) {
            const int count = op & 127;
            if (count > width - x) throw std::runtime_error("Shadow run outside sprite");
            if (!(op & 128) && rowVisible && count > 0) {
                const auto dx0 = std::max<std::int64_t>(0, left + x);
                const auto dx1 = std::min<std::int64_t>(map.width, left + x + count);
                for (std::int64_t dx = dx0; dx < dx1; ++dx) {
                    const std::size_t idx = std::size_t(dy) * map.width + std::size_t(dx);
                    if (shadowMask && (*shadowMask)[idx]) continue;
                    auto* pixel = map.rgba.data() + idx * 4u;
                    if (pixel[3] == 0u) continue;
                    if (shadowMask) (*shadowMask)[idx] = 1u;
                    // Retail n2Cad800.dll 0xa3 formula: RGB565 ((c & 0xF7DE) >> 1) + 0x0085.
                    pixel[0] = static_cast<std::uint8_t>(pixel[0] >> 1);
                    pixel[1] = static_cast<std::uint8_t>((pixel[1] >> 1) + 16u);
                    pixel[2] = static_cast<std::uint8_t>((pixel[2] >> 1) + 41u);
                }
            }
            x += count;
        }
        pos += size;
    }
}

void drawIndexedFrame(MapImage& map, int originX, int originY, View sprite, View palette, SpriteCoverage* coverage) {
    if (sprite.size() < 9 || palette.size() < 512 || !map.width || !map.height ||
        (std::uint64_t(map.width) * map.height > map.rgba.size() / 4))
        throw std::runtime_error("Invalid indexed sprite or destination");
    const auto left = std::int64_t(originX) + std::int16_t(word(sprite, 0));
    const auto top = std::int64_t(originY) + std::int16_t(word(sprite, 2));
    const int width = std::int16_t(word(sprite, 4)), height = std::int16_t(word(sprite, 6));
    const bool compact = sprite[8] == 0xa1;
    if ((!compact && sprite[8] != 0xa2) || width < 1 || height < 1 || width > 2048 || height > 2048)
        throw std::runtime_error("Unsupported indexed sprite header");
    if (coverage) *coverage = {int(left), int(top), width, height, std::vector<std::uint8_t>(std::size_t(width)*height)};
    std::size_t pos = 9;
    for (int y = 0; y < height; ++y) {
        const auto length = word(sprite, pos);
        pos += 2;
        if (length > sprite.size() - pos) throw std::runtime_error("Short indexed sprite row");
        const auto row = sprite.subspan(pos, length);
        pos += length;
        int x = 0;
        for (std::size_t i = 0; i < row.size();) {
            const auto op = row[i++];
            const int count = op & (compact ? 127 : 63);
            if (count > width - x) throw std::runtime_error("Indexed run outside sprite");
            const bool skip = !compact && (op & 192) == 192;
            const bool repeat = (op & 128) && !skip;
            const auto used = std::size_t(skip || !count ? 0 : repeat ? 1 : count);
            if (used > row.size() - i) throw std::runtime_error("Short indexed sprite run");
            for (int j = 0; !skip && j < count; ++j) {
                const auto dx = left + x + j, dy = top + y;
                if (dx < 0 || dy < 0 || dx >= map.width || dy >= map.height) continue;
                const auto index = row[i + (repeat ? 0 : j)];
                if (compact && repeat && !index) continue;
                if (coverage) coverage->pixels[std::size_t(y)*width+x+j] = 1;
                const auto color = word(palette, index * 2);
                const unsigned r = color >> 11, g = color >> 5 & 63, b = color & 31;
                auto* pixel = map.rgba.data() + 4 * (dy * map.width + dx);
                const bool blend = !compact && (op & 192) == 64;
                const auto channel = [&](int value, int old) { return blend ? (value + old) / 2 : value; };
                pixel[0] = channel((r << 3) | (r >> 2), pixel[0]);
                pixel[1] = channel((g << 2) | (g >> 4), pixel[1]);
                pixel[2] = channel((b << 3) | (b >> 2), pixel[2]);
                pixel[3] = channel(255, pixel[3]);
            }
            i += used;
            x += count;
        }
        // Transparent tails can be omitted from a row.
    }
}

void drawAlphaIndexedFrame(MapImage& map, int originX, int originY, View sprite, View palette1024) {
    if (sprite.size() < 9u || sprite[8] != 0xa9u || palette1024.size() < 1024u ||
        !map.width || !map.height || (std::uint64_t(map.width) * map.height > map.rgba.size() / 4u)) {
        throw std::runtime_error("Invalid 0xa9 alpha sprite or destination");
    }
    const auto left = std::int64_t(originX) + std::int16_t(word(sprite, 0));
    const auto top = std::int64_t(originY) + std::int16_t(word(sprite, 2));
    const int width = std::int16_t(word(sprite, 4));
    const int height = std::int16_t(word(sprite, 6));
    if (width < 1 || height < 1 || width > 2048 || height > 2048) {
        throw std::runtime_error("Invalid 0xa9 alpha sprite dimensions");
    }
    std::size_t pos = 9u;
    for (int y = 0; y < height; ++y) {
        const auto length = word(sprite, pos);
        pos += 2u;
        if (length > sprite.size() - pos) throw std::runtime_error("Short 0xa9 alpha sprite row");
        const auto row = sprite.subspan(pos, length);
        pos += length;
        int x = 0;
        for (std::size_t i = 0; i < row.size();) {
            const auto op = row[i++];
            const int count = op & 127;
            if (count > width - x) throw std::runtime_error("0xa9 alpha run outside sprite");
            const bool repeat = (op & 128u) != 0u;
            const auto used = std::size_t(!count ? 0 : repeat ? 1 : count);
            if (used > row.size() - i) throw std::runtime_error("Short 0xa9 alpha sprite run");
            for (int j = 0; j < count; ++j) {
                const auto index = row[i + (repeat ? 0u : std::size_t(j))];
                if (repeat && index == 0u) continue;
                const auto dx = left + x + j;
                const auto dy = top + y;
                if (dx < 0 || dy < 0 || dx >= std::int64_t(map.width) || dy >= std::int64_t(map.height)) continue;
                const std::size_t pOff = std::size_t(index) * 4u;
                const unsigned premulWord = word(palette1024, pOff);
                const unsigned bgWeight5 = (palette1024[pOff + 2u] >> 3u) & 0x1fu;
                // Retail n2Cad800.dll 0x10008e8e/0x10008ef3: bgWeight5 == 31 leaves pixel unchanged.
                if (bgWeight5 == 31u) continue;
                const unsigned pr = (premulWord >> 11u) & 31u;
                const unsigned pg = (premulWord >> 5u) & 63u;
                const unsigned pb = premulWord & 31u;
                const unsigned fgR = (pr << 3u) | (pr >> 2u);
                const unsigned fgG = (pg << 2u) | (pg >> 4u);
                const unsigned fgB = (pb << 3u) | (pb >> 2u);
                auto* pixel = map.rgba.data() + 4u * (std::size_t(dy) * map.width + std::size_t(dx));
                if (pixel[3] == 0u) {
                    const unsigned fgWeight = 32u - bgWeight5;
                    pixel[0] = static_cast<std::uint8_t>(std::min(255u, (fgR * 32u + fgWeight / 2u) / fgWeight));
                    pixel[1] = static_cast<std::uint8_t>(std::min(255u, (fgG * 32u + fgWeight / 2u) / fgWeight));
                    pixel[2] = static_cast<std::uint8_t>(std::min(255u, (fgB * 32u + fgWeight / 2u) / fgWeight));
                    pixel[3] = static_cast<std::uint8_t>((fgWeight * 255u + 16u) / 32u);
                } else {
                    // Retail n2Cad800.dll 0x10008e92..0x10008ea5: dst' = (dst * bgWeight5) / 32 + premulFg.
                    pixel[0] = static_cast<std::uint8_t>(std::min(255u, (unsigned(pixel[0]) * bgWeight5) / 32u + fgR));
                    pixel[1] = static_cast<std::uint8_t>(std::min(255u, (unsigned(pixel[1]) * bgWeight5) / 32u + fgG));
                    pixel[2] = static_cast<std::uint8_t>(std::min(255u, (unsigned(pixel[2]) * bgWeight5) / 32u + fgB));
                    pixel[3] = static_cast<std::uint8_t>(255u - ((255u - unsigned(pixel[3])) * bgWeight5) / 32u);
                }
            }
            i += used;
            x += count;
        }
    }
}

namespace {
std::uint32_t dword(View v, std::size_t i) {
    return word(v, i) | (std::uint32_t(word(v, i + 2u)) << 16u);
}

std::pair<std::uint32_t, std::size_t> validateAniHeader(View ani) {
    if (ani.size() < 16u) throw std::runtime_error("Truncated ANI header");
    const std::uint32_t magic = dword(ani, 0u);
    if (magic < 0xca000001u || magic > 0xca000004u) {
        throw std::runtime_error("Unsupported ANI container magic");
    }
    const std::uint32_t count = dword(ani, 4u);
    if (count == 0u || count > 4096u) throw std::runtime_error("Invalid ANI frame count");
    const std::size_t tableEnd = 8u + (std::size_t(count) + 1u) * 4u;
    if (tableEnd > ani.size()) throw std::runtime_error("Truncated ANI offset table");
    if (dword(ani, 8u) != tableEnd) throw std::runtime_error("Invalid ANI first frame offset");
    std::uint32_t prev = dword(ani, 8u);
    for (std::size_t k = 1u; k <= count; ++k) {
        const std::uint32_t cur = dword(ani, 8u + k * 4u);
        if (cur < prev || cur > ani.size()) throw std::runtime_error("Invalid ANI frame offset range");
        prev = cur;
    }
    return {magic, std::size_t(count)};
}
} // namespace

std::size_t aniFrameCount(View ani) {
    return validateAniHeader(ani).second;
}

View aniFrameSlice(View ani, std::size_t index) {
    const auto [magic, count] = validateAniHeader(ani);
    (void)magic;
    if (index >= count) throw std::runtime_error("ANI frame index out of range");
    const std::size_t start = dword(ani, 8u + index * 4u);
    const std::size_t end = dword(ani, 8u + (index + 1u) * 4u);
    return ani.subspan(start, end - start);
}

AniSequence decodeAniSequence(View ani, View palette) {
    const auto [magic, count] = validateAniHeader(ani);
    int minX = 0, minY = 0, maxX = 1, maxY = 1;
    for (std::size_t idx = 0u; idx < count; ++idx) {
        const View fr = aniFrameSlice(ani, idx);
        if (fr.size() < 9u) continue;
        const int dx = std::int16_t(word(fr, 0u));
        const int dy = std::int16_t(word(fr, 2u));
        const int w = std::int16_t(word(fr, 4u));
        const int h = std::int16_t(word(fr, 6u));
        if (w <= 0 || h <= 0 || w > 2048 || h > 2048) continue;
        minX = std::min(minX, dx);
        minY = std::min(minY, dy);
        maxX = std::max(maxX, dx + w);
        maxY = std::max(maxY, dy + h);
    }
    const unsigned canvasW = static_cast<unsigned>(std::max(1, maxX - minX));
    const unsigned canvasH = static_cast<unsigned>(std::max(1, maxY - minY));
    const int hotX = -minX;
    const int hotY = -minY;

    std::array<std::uint8_t, 512> pal512{};
    bool hasPal512 = false;
    if (palette.size() >= 1024u) {
        for (std::size_t e = 0u; e < 256u; ++e) {
            pal512[e * 2u] = palette[e * 4u];
            pal512[e * 2u + 1u] = palette[e * 4u + 1u];
        }
        hasPal512 = true;
    } else if (palette.size() >= 512u) {
        std::copy_n(palette.data(), 512u, pal512.data());
        hasPal512 = true;
    }

    AniSequence seq{magic, hotX, hotY, canvasW, canvasH, {}};
    seq.frames.reserve(count);
    for (std::size_t idx = 0u; idx < count; ++idx) {
        MapImage img{canvasW, canvasH, std::vector<std::uint8_t>(std::size_t(canvasW) * canvasH * 4u, 0u)};
        const View fr = aniFrameSlice(ani, idx);
        if (fr.size() >= 9u) {
            const int w = std::int16_t(word(fr, 4u));
            const int h = std::int16_t(word(fr, 6u));
            if (w >= 1 && h >= 1 && w <= 2048 && h <= 2048) {
                const std::uint8_t tag = fr[8];
                if (tag == 0xa9u && palette.size() >= 1024u) {
                    drawAlphaIndexedFrame(img, hotX, hotY, fr, palette);
                } else if ((tag == 0xa1u || tag == 0xa2u) && hasPal512) {
                    drawIndexedFrame(img, hotX, hotY, fr, pal512);
                } else if (tag == 0xa3u) {
                    const auto cov = readShadowCoverage(hotX, hotY, fr);
                    for (int py = 0; py < cov.height; ++py) {
                        const int dy = cov.top + py;
                        if (dy < 0 || dy >= int(canvasH)) continue;
                        for (int px = 0; px < cov.width; ++px) {
                            const int dx = cov.left + px;
                            if (dx < 0 || dx >= int(canvasW)) continue;
                            if (cov.pixels[std::size_t(py) * std::size_t(cov.width) + std::size_t(px)]) {
                                auto* p = img.rgba.data() + 4u * (std::size_t(dy) * canvasW + std::size_t(dx));
                                p[0] = 0u;
                                p[1] = 16u;
                                p[2] = 41u;
                                p[3] = 128u;
                            }
                        }
                    }
                }
            }
        }
        seq.frames.push_back(std::move(img));
    }
    return seq;
}
}
