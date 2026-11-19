#include "assets/sound_bank.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace ss2::assets {
namespace {
std::uint32_t u32(std::span<const std::uint8_t> data, std::size_t pos) {
    if (pos > data.size() || data.size() - pos < 4)
        throw std::runtime_error("Truncated sound archive");
    return std::uint32_t(data[pos]) | (std::uint32_t(data[pos + 1]) << 8) |
           (std::uint32_t(data[pos + 2]) << 16) | (std::uint32_t(data[pos + 3]) << 24);
}

std::string normalizeName(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == 0) break;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open sound file: " + path.string());
    const auto size = file.tellg();
    if (size < 0) throw std::runtime_error("Cannot determine size of: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Cannot read sound file: " + path.string());
    return bytes;
}
} // namespace

SoundBank SoundBank::load(const std::filesystem::path& gameDir) {
    const auto hdr = readFileBytes(gameDir / "SOUNDS.HDR");
    const auto rus = readFileBytes(gameDir / "SOUNDS.RUS");
    return fromMemory(hdr, rus);
}

SoundBank SoundBank::fromMemory(std::span<const std::uint8_t> hdr, std::span<const std::uint8_t> rus) {
    SoundBank bank;
    const std::uint32_t numGroups = u32(hdr, 0);
    std::size_t pos = 4;
    for (std::uint32_t g = 0; g < numGroups; ++g) {
        if (pos + 68 > hdr.size()) throw std::runtime_error("Truncated SOUNDS.HDR group");
        const std::string name = normalizeName(
            std::string_view(reinterpret_cast<const char*>(hdr.data() + pos), 64));
        pos += 64;
        const std::uint32_t count = u32(hdr, pos);
        pos += 4;
        if (pos + std::size_t(count) * 4 > hdr.size())
            throw std::runtime_error("Truncated SOUNDS.HDR indices");
        std::vector<std::uint32_t> indices;
        indices.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            indices.push_back(u32(hdr, pos));
            pos += 4;
        }
        bank.groups_.emplace(name, std::move(indices));
    }

    const std::uint32_t clipCount = u32(rus, 0);
    if (4 + std::size_t(clipCount + 1) * 4 > rus.size())
        throw std::runtime_error("Truncated SOUNDS.RUS offset table");
    bank.clips_.reserve(clipCount);
    for (std::uint32_t i = 0; i < clipCount; ++i) {
        const std::uint32_t start = u32(rus, 4 + std::size_t(i) * 4);
        const std::uint32_t end = u32(rus, 4 + std::size_t(i + 1) * 4);
        if (end < start || end > rus.size() || end - start < 12)
            throw std::runtime_error("Invalid SOUNDS.RUS clip bounds");
        SoundClip clip;
        clip.sampleRate = u32(rus, start);
        clip.bitsPerSample = u32(rus, start + 4);
        clip.loop = u32(rus, start + 8) != 0;
        const std::size_t pcmBytes = end - start - 12;
        clip.pcm.assign(rus.begin() + start + 12, rus.begin() + start + 12 + pcmBytes);
        bank.clips_.push_back(std::move(clip));
    }
    return bank;
}

std::size_t SoundBank::variantCount(const std::string& group) const {
    const auto it = groups_.find(normalizeName(group));
    return it == groups_.end() ? 0 : it->second.size();
}

const SoundClip* SoundBank::clip(const std::string& group, std::size_t variant) const {
    const auto it = groups_.find(normalizeName(group));
    if (it == groups_.end() || it->second.empty()) return nullptr;
    const std::uint32_t idx = it->second[variant % it->second.size()];
    if (idx >= clips_.size()) return nullptr;
    return &clips_[idx];
}

} // namespace ss2::assets
