#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ss2::assets {
using Bytes = std::vector<std::uint8_t>;

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b);
std::filesystem::path resolveCaseInsensitivePath(const std::filesystem::path& base,
                                                 const std::filesystem::path& relative);

Bytes readSueEntry(const std::filesystem::path& archive, std::string_view entry);
Bytes readSueEntryFallback(const std::vector<std::filesystem::path>& archives,
                           std::string_view entry);

struct SpriteFrame {
    int x, y, width, height;
    // Original 16-bit pixel words serialized little-endian in top-to-bottom row order.
    Bytes pixels;
};
SpriteFrame decodePckFrame(std::span<const std::uint8_t> file, std::size_t index);
std::size_t pckFrameCount(std::span<const std::uint8_t> file);
SpriteFrame decodeUiPckFrame(std::span<const std::uint8_t> file, std::size_t index);
}
