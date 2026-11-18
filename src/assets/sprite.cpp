#include "assets/sprite.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <zlib.h>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;
constexpr size_t tableLimit = 16u << 20;
constexpr size_t assetLimit = 64u << 20;

View slice(View bytes, size_t offset, size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset)
        throw std::runtime_error("Truncated asset record");
    return bytes.subspan(offset, count);
}

unsigned u16(View bytes, size_t at) {
    const auto v = slice(bytes, at, 2);
    return v[0] | (unsigned(v[1]) << 8);
}
std::uint32_t u32(View bytes, size_t at) {
    return u16(bytes, at) | (std::uint32_t(u16(bytes, at + 2)) << 16);
}

Bytes read(std::ifstream& file, size_t length) {
    Bytes bytes(length);
    if (length && !file.read(reinterpret_cast<char*>(bytes.data()), length))
        throw std::runtime_error("Truncated SUE file");
    return bytes;
}

Bytes inflateExact(View input, size_t expected) {
    Bytes output(std::max<size_t>(expected, 1));
    uLongf outSize = output.size();
    uLong inSize = input.size();
    if (uncompress2(output.data(), &outSize, input.data(), &inSize) != Z_OK ||
        outSize != expected || inSize != input.size())
        throw std::runtime_error("Invalid SUE zlib stream or declared length");
    output.resize(expected);
    return output;
}

Bytes table(std::ifstream& file, unsigned recordWidth) {
    const auto header = read(file, 12);
    if (recordWidth && u32(header, 8) != recordWidth)
        throw std::runtime_error("Unexpected SUE table record width");
    const auto packed = u32(header, 0);
    const std::uint64_t length = std::uint64_t(u32(header, 4)) * u32(header, 8);
    if (length > tableLimit || packed > compressBound(tableLimit))
        throw std::runtime_error("SUE index exceeds reader limit");
    return inflateExact(read(file, packed), length);
}

Bytes payload(std::ifstream& file, size_t start, size_t length, size_t indexStart) {
    if (start < 8 || start > indexStart || length > indexStart - start)
        throw std::runtime_error("SUE payload outside data section");
    file.seekg(start);
    return read(file, length);
}
}

Bytes readSueEntry(const std::filesystem::path& archive, std::string_view name) {
    std::ifstream file(archive, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open SUE archive");
    const auto header = read(file, 8);
    if (u32(header, 0) != 0x53465a46) throw std::runtime_error("Missing FZFS signature");
    const size_t indexStart = 8ull + u32(header, 4);
    file.seekg(indexStart);
    const auto items = table(file, 20);
    const auto names = table(file, 0);
    const auto offsets = table(file, 4);
    if (items.size() % 20 || offsets.size() % 4) throw std::runtime_error("Invalid SUE index layout");
    for (size_t pos = 0; pos < items.size(); pos += 20) {
        const auto info = slice(items, pos, 20);
        const size_t n = u32(info, 0);
        if (n >= names.size()) throw std::runtime_error("Invalid SUE name offset");
        // Exact stored names avoid case folding or path interpretation.
        if (name.size() >= names.size() - n || names[n + name.size()] != 0 ||
            std::string_view(reinterpret_cast<const char*>(names.data() + n), name.size()) != name) continue;
        const size_t kind = u32(info, 4), first = u32(info, 8), size = u32(info, 12);
        if (size > assetLimit) throw std::runtime_error("SUE entry exceeds reader limit");
        if (kind == 2) return payload(file, first, size, indexStart);
        if (kind != 8) throw std::runtime_error("Unsupported SUE storage type");
        const size_t chunk = u32(info, 16);
        if (!chunk) throw std::runtime_error("Zero SUE chunk size");
        const size_t count = (size + chunk - 1) / chunk;
        if (first >= offsets.size() / 4 || count >= offsets.size() / 4 - first)
            throw std::runtime_error("Invalid SUE chunk range");
        Bytes result;
        result.reserve(size);
        for (size_t i = 0; i < count; ++i) {
            const size_t start = u32(offsets, 4 * (first + i));
            const size_t end = u32(offsets, 4 * (first + i + 1));
            const size_t expected = std::min(chunk, size - result.size());
            if (end < start || end - start > compressBound(expected))
                throw std::runtime_error("Invalid SUE chunk length");
            const auto decoded = inflateExact(payload(file, start, end - start, indexStart), expected);
            result.insert(result.end(), decoded.begin(), decoded.end());
        }
        return result;
    }
    throw std::runtime_error("SUE entry not found");
}

SpriteFrame decodePckFrame(View file, size_t index) {
    const size_t first = u32(file, 0);
    if (!first || first % 4 || first > file.size() || index >= first / 4)
        throw std::runtime_error("Invalid PCK frame table or index");
    const size_t start = u32(file, index * 4);
    const size_t end = index + 1 < first / 4 ? u32(file, (index + 1) * 4) : file.size();
    if (start < first || end < start) throw std::runtime_error("Invalid PCK frame range");
    const auto frame = slice(file, start, end - start);
    const auto header = slice(frame, 0, 9);
    SpriteFrame image{std::int16_t(u16(header, 0)), std::int16_t(u16(header, 2)),
                      std::int16_t(u16(header, 4)), std::int16_t(u16(header, 6)), {}};
    if (header[8] != 0xa5) throw std::runtime_error("Unsupported PCK frame tag");
    if (image.width <= 0 || image.height <= 0 || 2ull * image.width * image.height > assetLimit)
        throw std::runtime_error("Invalid PCK dimensions");
    image.pixels.reserve(2ull * image.width * image.height);
    size_t pos = 9;
    for (int y = 0; y < image.height; ++y) {
        const auto row = slice(frame, pos + 2, u16(frame, pos));
        pos += 2 + row.size();
        int remaining = image.width;
        for (size_t i = 0; i < row.size();) {
            const auto opcode = row[i++];
            const int count = opcode & 127;
            if (!count || count > remaining) throw std::runtime_error("Invalid PCK run width");
            const bool repeat = (opcode & 128) != 0;
            const auto words = slice(row, i, repeat ? 2 : 2 * count);
            i += words.size();
            if (repeat) {
                for (int j = 0; j < count; ++j) image.pixels.insert(image.pixels.end(), words.begin(), words.end());
            } else image.pixels.insert(image.pixels.end(), words.begin(), words.end());
            remaining -= count;
        }
        if (remaining) throw std::runtime_error("Incomplete PCK row");
    }
    return image;
}
}
