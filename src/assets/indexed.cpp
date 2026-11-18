#include "assets/indexed.hpp"
#include <stdexcept>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;

unsigned word(View v, std::size_t i) {
    if (i > v.size() || v.size() - i < 2) throw std::runtime_error("Truncated indexed sprite");
    return v[i] | unsigned(v[i + 1]) << 8;
}
}

void drawIndexedFrame(MapImage& map, int originX, int originY, View sprite, View palette) {
    if (sprite.size() < 9 || palette.size() < 512 || !map.width || !map.height ||
        (std::uint64_t(map.width) * map.height > map.rgba.size() / 4))
        throw std::runtime_error("Invalid indexed sprite or destination");
    const auto left = std::int64_t(originX) + std::int16_t(word(sprite, 0));
    const auto top = std::int64_t(originY) + std::int16_t(word(sprite, 2));
    const int width = std::int16_t(word(sprite, 4)), height = std::int16_t(word(sprite, 6));
    const bool compact = sprite[8] == 0xa1;
    if ((!compact && sprite[8] != 0xa2) || width < 1 || height < 1 || width > 2048 || height > 2048)
        throw std::runtime_error("Unsupported indexed sprite header");
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
                const auto color = word(palette, index * 2);
                const unsigned r = color >> 11, g = color >> 5 & 63, b = color & 31;
                auto* pixel = map.rgba.data() + 4 * (dy * map.width + dx);
                const bool blend = !compact && (op & 192) == 64;
                const auto channel = [&](int value, int old) { return blend ? (value + old) / 2 : value; };
                pixel[0] = channel((r << 3) | (r >> 2), pixel[0]);
                pixel[1] = channel((g << 2) | (g >> 4), pixel[1]);
                pixel[2] = channel((b << 3) | (b >> 2), pixel[2]);
                pixel[3] = 255;
            }
            i += used;
            x += count;
        }
        // Transparent tails can be omitted from a row.
    }
}
}
