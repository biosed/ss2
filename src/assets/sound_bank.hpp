#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ss2::assets {

struct SoundClip {
    std::uint32_t sampleRate{22050};
    std::uint32_t bitsPerSample{16};
    bool loop{false};
    std::vector<std::uint8_t> pcm;
};

// Decodes Gold 2.2 SOUNDS.HDR + SOUNDS.RUS uncompressed mono PCM archives.
class SoundBank {
public:
    static SoundBank load(const std::filesystem::path& gameDir);
    static SoundBank fromMemory(std::span<const std::uint8_t> hdr, std::span<const std::uint8_t> rus);

    const SoundClip* clip(const std::string& group, std::size_t variant = 0) const;
    std::size_t variantCount(const std::string& group) const;
    std::size_t groupCount() const { return groups_.size(); }
    std::size_t clipCount() const { return clips_.size(); }

private:
    std::map<std::string, std::vector<std::uint32_t>> groups_;
    std::vector<SoundClip> clips_;
};

} // namespace ss2::assets
