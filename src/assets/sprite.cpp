#include "assets/sprite.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <zlib.h>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;
constexpr size_t tableLimit = 16u << 20;
constexpr size_t assetLimit = 64u << 20;

constexpr unsigned char asciiLower(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

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

struct SueIndex {
    std::filesystem::path key;
    std::uintmax_t fileSize{};
    std::filesystem::file_time_type mtime{};
    size_t indexStart{};
    std::uint64_t indexHash{};
    Bytes items;
    Bytes offsets;
    std::unordered_map<std::string, size_t> exactPos;
    std::unordered_map<std::string, size_t> lowerPos;
};

std::uint64_t hashIndexPrefix(std::ifstream& file, size_t indexStart, std::uintmax_t fileSize) {
    if (fileSize == static_cast<std::uintmax_t>(-1) || indexStart >= fileSize) return 0;
    const size_t n = static_cast<size_t>(std::min<std::uintmax_t>(fileSize - indexStart, 256));
    Bytes buf(n);
    file.seekg(static_cast<std::streamoff>(indexStart));
    if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n))) return 0;
    std::uint64_t h = 14695981039346656037ull;
    for (std::uint8_t b : buf) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::shared_ptr<const SueIndex> loadSueIndex(const std::filesystem::path& archive, std::ifstream& file, size_t indexStart) {
    static std::mutex cacheMutex;
    static std::vector<std::shared_ptr<const SueIndex>> cache;

    std::error_code ec;
    const auto normKey = archive.lexically_normal();
    const auto fsize = std::filesystem::file_size(archive, ec);
    const auto fileSize = ec ? static_cast<std::uintmax_t>(-1) : fsize;
    ec.clear();
    const auto mtime = std::filesystem::last_write_time(archive, ec);
    const bool hasStat = !ec && fileSize != static_cast<std::uintmax_t>(-1);
    const std::uint64_t prefixHash = hasStat ? hashIndexPrefix(file, indexStart, fileSize) : 0;

    if (hasStat) {
        std::lock_guard lock(cacheMutex);
        for (const auto& entry : cache) {
            if (entry->key == normKey && entry->fileSize == fileSize &&
                entry->mtime == mtime && entry->indexStart == indexStart &&
                entry->indexHash == prefixHash) {
                return entry;
            }
        }
    }

    file.clear();
    file.seekg(static_cast<std::streamoff>(indexStart));
    Bytes items = table(file, 20);
    const Bytes names = table(file, 0);
    Bytes offsets = table(file, 4);
    if (items.size() % 20 || offsets.size() % 4) throw std::runtime_error("Invalid SUE index layout");

    auto idx = std::make_shared<SueIndex>();
    idx->key = normKey;
    idx->fileSize = fileSize;
    idx->mtime = mtime;
    idx->indexStart = indexStart;
    idx->indexHash = prefixHash;
    idx->items = std::move(items);
    idx->offsets = std::move(offsets);
    idx->exactPos.reserve(idx->items.size() / 20 + 1);
    idx->lowerPos.reserve(idx->items.size() / 20 + 1);

    for (size_t pos = 0; pos < idx->items.size(); pos += 20) {
        const auto info = slice(idx->items, pos, 20);
        const size_t n = u32(info, 0);
        if (n >= names.size()) throw std::runtime_error("Invalid SUE name offset");
        const auto* startPtr = reinterpret_cast<const char*>(names.data() + n);
        const size_t maxLen = names.size() - n;
        const auto* nul = static_cast<const char*>(std::memchr(startPtr, '\0', maxLen));
        if (!nul) continue;
        std::string name(startPtr, std::size_t(nul - startPtr));
        idx->exactPos.try_emplace(name, pos);
        for (char& ch : name) ch = static_cast<char>(asciiLower(static_cast<unsigned char>(ch)));
        idx->lowerPos.try_emplace(std::move(name), pos);
    }

    if (hasStat) {
        std::lock_guard lock(cacheMutex);
        for (auto& existing : cache) {
            if (existing->key == normKey) {
                existing = idx;
                return idx;
            }
        }
        if (cache.size() >= 16) cache.erase(cache.begin());
        cache.push_back(idx);
    }
    return idx;
}

std::optional<Bytes> tryReadSueEntry(const std::filesystem::path& archive, std::string_view entry) {
    std::ifstream file(archive, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open SUE archive");
    const auto header = read(file, 8);
    if (u32(header, 0) != 0x53465a46) throw std::runtime_error("Missing FZFS signature");
    const size_t indexStart = 8ull + u32(header, 4);
    const auto idx = loadSueIndex(archive, file, indexStart);

    std::optional<size_t> matchedPos;
    const std::string key(entry);
    if (const auto it = idx->exactPos.find(key); it != idx->exactPos.end()) {
        matchedPos = it->second;
    } else {
        std::string lower = key;
        for (char& ch : lower) ch = static_cast<char>(asciiLower(static_cast<unsigned char>(ch)));
        if (const auto lit = idx->lowerPos.find(lower); lit != idx->lowerPos.end()) {
            matchedPos = lit->second;
        }
    }
    if (!matchedPos) return std::nullopt;

    const auto info = slice(idx->items, *matchedPos, 20);
    const size_t kind = u32(info, 4), first = u32(info, 8), size = u32(info, 12);
    if (size > assetLimit) throw std::runtime_error("SUE entry exceeds reader limit");
    if (kind == 2) return payload(file, first, size, indexStart);
    if (kind != 8) throw std::runtime_error("Unsupported SUE storage type");
    const size_t chunk = u32(info, 16);
    if (!chunk) throw std::runtime_error("Zero SUE chunk size");
    const size_t count = (size + chunk - 1) / chunk;
    if (first >= idx->offsets.size() / 4 || count >= idx->offsets.size() / 4 - first)
        throw std::runtime_error("Invalid SUE chunk range");
    Bytes result;
    result.reserve(size);
    for (size_t i = 0; i < count; ++i) {
        const size_t start = u32(idx->offsets, 4 * (first + i));
        const size_t end = u32(idx->offsets, 4 * (first + i + 1));
        const size_t expected = std::min(chunk, size - result.size());
        if (end < start || end - start > compressBound(expected))
            throw std::runtime_error("Invalid SUE chunk length");
        const auto decoded = inflateExact(payload(file, start, end - start, indexStart), expected);
        result.insert(result.end(), decoded.begin(), decoded.end());
    }
    return result;
}
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(static_cast<unsigned char>(a[i])) !=
            asciiLower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::filesystem::path resolveCaseInsensitivePath(const std::filesystem::path& base,
                                                 const std::filesystem::path& relative) {
    const auto direct = base / relative;
    std::error_code ec;
    if (relative.empty() || std::filesystem::exists(direct, ec)) return direct;

    std::filesystem::path current = base;
    for (const auto& part : relative) {
        if (part.empty() || part == ".") continue;
        if (part == ".." || part == relative.root_name() || part == relative.root_directory()) {
            current /= part;
            continue;
        }
        const auto exact = current / part;
        ec.clear();
        if (std::filesystem::exists(exact, ec)) {
            current = exact;
            continue;
        }
        const auto dir = current.empty() ? std::filesystem::path(".") : current;
        ec.clear();
        if (!std::filesystem::is_directory(dir, ec) || ec) return direct;

        const auto target = part.string();
        bool matched = false;
        ec.clear();
        std::filesystem::directory_iterator it(
            dir, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) return direct;
        for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename();
            if (equalsIgnoreCaseAscii(name.string(), target)) {
                current /= name;
                matched = true;
                break;
            }
        }
        if (!matched) return direct;
    }
    return current;
}

Bytes readSueEntry(const std::filesystem::path& archive, std::string_view entry) {
    if (auto bytes = tryReadSueEntry(archive, entry)) return *bytes;
    throw std::runtime_error("SUE entry not found");
}

Bytes readSueEntryFallback(const std::vector<std::filesystem::path>& archives,
                           std::string_view entry) {
    for (const auto& archive : archives) {
        const auto resolved = resolveCaseInsensitivePath(archive.root_path(), archive.relative_path());
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec)) continue;
        if (auto bytes = tryReadSueEntry(resolved, entry)) return *bytes;
    }
    throw std::runtime_error("SUE entry not found in fallback archives: " + std::string(entry));
}

namespace {
SpriteFrame decodePckFrameImpl(View file, size_t index, bool allowTruncatedRows) {
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
        if (remaining) {
            if (!allowTruncatedRows) throw std::runtime_error("Incomplete PCK row");
            // UI PCKs omit trailing 0xF81F transparent key words at the right edge of rows.
            for (int j = 0; j < remaining; ++j) {
                image.pixels.push_back(0x1f);
                image.pixels.push_back(0xf8);
            }
        }
    }
    return image;
}
}

SpriteFrame decodePckFrame(View file, size_t index) {
    return decodePckFrameImpl(file, index, false);
}

std::size_t pckFrameCount(View file) {
    if (file.size() < 4) return 0;
    const size_t first = u32(file, 0);
    if (!first || first % 4 || first > file.size()) return 0;
    return first / 4;
}

SpriteFrame decodeUiPckFrame(View file, size_t index) {
    return decodePckFrameImpl(file, index, true);
}
}
