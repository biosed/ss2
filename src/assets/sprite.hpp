#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ss2::assets {
using Bytes = std::vector<std::uint8_t>;

Bytes readSueEntry(const std::filesystem::path& archive, std::string_view name);

struct SpriteFrame {
    int x, y, width, height;
    // Original 16-bit pixel words serialized little-endian in top-to-bottom row order.
    Bytes pixels;
};
SpriteFrame decodePckFrame(std::span<const std::uint8_t> file, std::size_t index);
}
