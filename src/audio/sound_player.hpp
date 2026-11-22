#pragma once
#include "assets/sound_bank.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ss2 {

// Mixing priority of a cue, independent of its loudness. Gain only scales the
// output level; the priority decides whether a cue is played when the voice
// pool is busy, so a quiet cue is no longer dropped for being quiet.
enum class VoicePriority { Ambient = 0, Low = 1, Normal = 2, High = 3 };

// One pool slot as seen by the voice allocator.
struct VoiceSlot {
    VoicePriority priority{VoicePriority::Ambient};
    bool active{false};
    std::uint64_t started{0}; // sequence number, an older voice has a lower one
};

// Picks the pool slot that should play a cue of `priority`.
//  1. an idle slot is always reused, in pool order;
//  2. otherwise the oldest active slot with the lowest priority is stolen;
//  3. a cue that matters less than every active slot is dropped (-1), so a
//     quiet cue never cuts off a more important one.
inline int chooseVoice(const std::vector<VoiceSlot>& slots, VoicePriority priority) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].active) return static_cast<int>(i);
    }
    std::size_t victim = slots.size();
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].active) continue;
        if (victim == slots.size() || slots[i].priority < slots[victim].priority ||
            (slots[i].priority == slots[victim].priority && slots[i].started < slots[victim].started)) {
            victim = i;
        }
    }
    if (victim == slots.size()) return -1;
    if (slots[victim].priority > priority) return -1; // every active voice matters more
    return static_cast<int>(victim);
}

namespace audio_detail {

inline std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8u));
}

inline std::uint32_t readU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

inline std::string lowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (c == 0) break;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline std::uint32_t hashGroupSeed(std::string_view key) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : key) {
        h ^= static_cast<std::uint32_t>(std::tolower(c));
        h *= 16777619u;
    }
    return h ? h : 0x9E3779B9u;
}

inline std::uint32_t xorshift32(std::uint32_t& state) {
    if (state == 0u) state = 0x9E3779B9u;
    std::uint32_t x = state;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    state = x;
    return x;
}

} // namespace audio_detail

// Per-group non-repeating shuffle-bag selector across `0 .. count - 1`.
// Visits every take in a group once per bag and guarantees no immediate back-to-back
// repeat across bag boundaries when `count >= 2`, while keeping each sound group's
// progression isolated from unrelated SFX activity.
class VariantSelector {
public:
    std::size_t next(std::string_view group, std::size_t count) {
        if (count <= 1u) return 0u;
        const std::string key = audio_detail::lowerAscii(group);
        auto [it, inserted] = states_.try_emplace(key);
        auto& st = it->second;
        if (inserted) {
            st.rng = audio_detail::hashGroupSeed(key);
        }
        if (st.bag.size() != count || st.cursor >= count) {
            refillBag(st, count);
        }
        const std::size_t chosen = st.bag[st.cursor++];
        st.lastPicked = chosen;
        st.hasLast = true;
        return chosen;
    }

    void reset() { states_.clear(); }

private:
    struct GroupState {
        std::vector<std::size_t> bag;
        std::size_t cursor{0};
        std::size_t lastPicked{0};
        bool hasLast{false};
        std::uint32_t rng{0x9E3779B9u};
    };

    static void refillBag(GroupState& st, std::size_t count) {
        st.bag.resize(count);
        for (std::size_t i = 0; i < count; ++i) st.bag[i] = i;
        for (std::size_t i = count - 1; i > 0; --i) {
            const std::size_t j = audio_detail::xorshift32(st.rng) % (i + 1u);
            std::swap(st.bag[i], st.bag[j]);
        }
        if (st.hasLast && count >= 2u && st.bag[0] == st.lastPicked) {
            const std::size_t swapIdx = 1u + (audio_detail::xorshift32(st.rng) % (count - 1u));
            std::swap(st.bag[0], st.bag[swapIdx]);
        }
        st.cursor = 0;
    }

    std::unordered_map<std::string, GroupState> states_;
};

// True when the file starts with the OggS container magic, whatever its
// extension is. Retail MUSIC ships one track as "_Track11.og_", so matching
// the extension alone skips a playable track.
inline bool hasOggMagic(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    char magic[4] = {};
    if (!f.read(magic, 4)) return false;
    return std::memcmp(magic, "OggS", 4) == 0;
}

// Reads a RIFF/WAVE file into 16-bit PCM. 24-bit samples are shifted down by
// 8 bits, which is what the SDL S16 stream format expects; 8- and 32-bit
// files stay unsupported.
inline bool loadWavPcm(const std::filesystem::path& path, std::uint32_t& sampleRate,
                       std::uint16_t& channels, std::vector<std::uint8_t>& pcm) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz < 44 || sz > 32 * 1024 * 1024) return false;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) return false;
    if (std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) return false;

    bool haveFmt = false;
    std::uint16_t bitsPerSample = 16;
    std::size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const std::uint32_t chunkSize = audio_detail::readU32(buf.data() + pos + 4);
        const std::size_t dataPos = pos + 8;
        if (dataPos + chunkSize > buf.size()) break;
        if (std::memcmp(buf.data() + pos, "fmt ", 4) == 0 && chunkSize >= 16) {
            const std::uint16_t fmt = audio_detail::readU16(buf.data() + dataPos);
            const std::uint16_t ch = audio_detail::readU16(buf.data() + dataPos + 2);
            const std::uint32_t sr = audio_detail::readU32(buf.data() + dataPos + 4);
            bitsPerSample = audio_detail::readU16(buf.data() + dataPos + 14);
            if (fmt != 1 || (bitsPerSample != 16 && bitsPerSample != 24) || (ch != 1 && ch != 2) ||
                sr < 8000 || sr > 192000) {
                return false;
            }
            channels = ch;
            sampleRate = sr;
            haveFmt = true;
        } else if (std::memcmp(buf.data() + pos, "data", 4) == 0 && haveFmt && chunkSize > 0) {
            const std::uint8_t* src = buf.data() + dataPos;
            if (bitsPerSample == 16) {
                pcm.assign(src, src + chunkSize);
                return true;
            }
            const std::size_t frameBytes = static_cast<std::size_t>(channels) * 3u;
            if (frameBytes == 0) return false;
            const std::size_t frames = chunkSize / frameBytes;
            pcm.clear();
            pcm.reserve(frames * static_cast<std::size_t>(channels) * 2u);
            for (std::size_t f = 0; f < frames; ++f) {
                for (std::size_t c = 0; c < channels; ++c) {
                    const std::uint8_t* sample = src + (f * frameBytes) + c * 3u;
                    pcm.push_back(sample[1]); // keep the top 16 bits of the 24-bit sample
                    pcm.push_back(sample[2]);
                }
            }
            return !pcm.empty();
        }
        pos = dataPos + chunkSize + (chunkSize & 1u);
    }
    return false;
}

inline float clampVolume(float volume) {
    return std::clamp(volume, 0.0f, 1.0f);
}

inline float clampPan(float pan) {
    if (!std::isfinite(pan)) return 0.0f;
    return std::clamp(pan, -1.0f, 1.0f);
}

struct StereoGains {
    float left{1.0f};
    float right{1.0f};
};

// Stereo left/right balance gains from normalized pan in [-1.0, +1.0].
// Center (0.0) yields (1.0, 1.0) so unpanned cues keep exact unity level without clipping;
// panning attenuates the opposite channel linearly toward 0.0 at full hard-pan.
inline StereoGains stereoPanGains(float pan) {
    const float p = clampPan(pan);
    const float left = std::clamp(1.0f - std::max(0.0f, p), 0.0f, 1.0f);
    const float right = std::clamp(1.0f + std::min(0.0f, p), 0.0f, 1.0f);
    return {left, right};
}

// Subtle horizontal stereo pan derived from world offset `dx` relative to `viewportHalfWidth`.
// Viewport edges map to +/-0.45 and off-screen cues clamp cleanly to +/-maxPan.
inline float viewportStereoPan(double dx, double viewportHalfWidth, float maxPan = 0.65f) {
    const float cap = clampVolume(maxPan);
    if (viewportHalfWidth <= 1.0 || cap <= 0.0f) return 0.0f;
    const double normX = dx / viewportHalfWidth;
    return std::clamp(static_cast<float>(normX * 0.45), -cap, cap);
}

// Converts mono or stereo 16-bit little-endian PCM into stereo 16-bit little-endian PCM
// with left/right balance applied from `pan` in [-1.0, +1.0].
inline void applyStereoPanS16(std::span<const std::uint8_t> srcPcm, int srcChannels,
                              float pan, std::vector<std::uint8_t>& dstStereoPcm) {
    dstStereoPcm.clear();
    if (srcChannels != 1 && srcChannels != 2) return;
    const std::size_t frameBytes = static_cast<std::size_t>(srcChannels) * 2u;
    const std::size_t frames = srcPcm.size() / frameBytes;
    if (frames == 0u) return;

    const auto [gainL, gainR] = stereoPanGains(pan);
    dstStereoPcm.resize(frames * 4u);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::uint8_t* frame = srcPcm.data() + i * frameBytes;
        const auto sL = static_cast<std::int16_t>(audio_detail::readU16(frame));
        const auto sR = (srcChannels == 2)
                            ? static_cast<std::int16_t>(audio_detail::readU16(frame + 2))
                            : sL;
        const int outL = std::clamp(static_cast<int>(std::lround(static_cast<float>(sL) * gainL)),
                                    -32768, 32767);
        const int outR = std::clamp(static_cast<int>(std::lround(static_cast<float>(sR) * gainR)),
                                    -32768, 32767);
        const auto uL = static_cast<std::uint16_t>(static_cast<std::int16_t>(outL));
        const auto uR = static_cast<std::uint16_t>(static_cast<std::int16_t>(outR));
        std::uint8_t* dst = dstStereoPcm.data() + i * 4u;
        dst[0] = static_cast<std::uint8_t>(uL & 0xFFu);
        dst[1] = static_cast<std::uint8_t>((uL >> 8u) & 0xFFu);
        dst[2] = static_cast<std::uint8_t>(uR & 0xFFu);
        dst[3] = static_cast<std::uint8_t>((uR >> 8u) & 0xFFu);
    }
}

inline float scaledSfxGain(float cueGain, float masterVolume) {
    const float master = clampVolume(masterVolume);
    if (master <= 0.001f) return 0.0f;
    return std::clamp(cueGain * master, 0.02f, 1.0f);
}

inline float spatialAttenuation(double dx, double dy, double viewportHalfDiag) {
    const double inner = std::max(280.0, viewportHalfDiag * 0.90);
    const double outer = std::max(1400.0, viewportHalfDiag * 2.80);
    const double dist = std::hypot(dx, dy);
    if (dist <= inner) return 1.0f;
    if (dist >= outer || outer <= inner) return 0.0f;
    const double t = (dist - inner) / (outer - inner);
    return static_cast<float>((1.0 - t) * (1.0 - t));
}

inline float spatialGain(float baseGain, double dx, double dy, double viewportHalfDiag) {
    if (baseGain <= 0.0f) return 0.0f;
    const float g = baseGain * spatialAttenuation(dx, dy, viewportHalfDiag);
    return g < 0.03f ? 0.0f : g;
}

// 48 kHz stereo SDL3 audio player for SOUNDS.RUS 16-bit PCM cues, optional 48 kHz
// HD WAV overrides (hd-pack/audio-hd/), and streaming Ogg Vorbis background music.
// Safe no-op when running headless or when no audio device is available.
class SoundPlayer {
public:
    explicit SoundPlayer(assets::SoundBank bank);
    ~SoundPlayer();
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

    bool available() const { return device_ != 0; }
    const assets::SoundBank& bank() const noexcept { return bank_; }
    void setHdAudioDir(const std::filesystem::path& dir);
    bool playGroup(const std::string& group, float gain = 0.65f,
                   VoicePriority priority = VoicePriority::Normal,
                   float pan = 0.0f);
    void setSfxVolume(float volume);
    float sfxVolume() const { return sfxVolume_; }

    // Streaming background music (MUSIC/Track*.ogg or remastered tracks in audio-hd/music/).
    bool startMusic(const std::filesystem::path& musicDir, float gain = 0.28f);
    void updateMusic();
    bool toggleMusicMute();
    bool nextMusicTrack();
    bool musicPlaying() const;
    void setMusicVolume(float volume);
    float musicVolume() const { return musicGain_; }

private:
    struct Voice {
        SDL_AudioStream* stream{nullptr};
        VoicePriority priority{VoicePriority::Ambient};
        bool active{false};
        std::uint64_t started{0};
        std::uint64_t expiresAt{0}; // SDL_GetTicks() deadline of the clip
    };
    struct HdClip {
        std::uint32_t sampleRate{48000};
        std::uint16_t channels{1};
        std::vector<std::uint8_t> pcm;
    };
    struct MusicState;

    assets::SoundBank bank_;
    SDL_AudioDeviceID device_{0};
    std::vector<Voice> voices_;
    std::size_t nextVoice_{0};
    std::uint64_t voiceSequence_{0};
    std::uint32_t pitchCounter_{0};
    VariantSelector variantSelector_;
    std::vector<std::uint8_t> pannedScratch_;
    std::map<std::string, std::vector<HdClip>> hdClips_;
    float sfxVolume_{1.0f};
    float musicGain_{0.28f};
    std::unique_ptr<MusicState> music_;

    bool openMusicTrack(std::size_t index);
};

} // namespace ss2
