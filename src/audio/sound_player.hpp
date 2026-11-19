#pragma once
#include "assets/sound_bank.hpp"
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
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

} // namespace audio_detail

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
    void setHdAudioDir(const std::filesystem::path& dir);
    bool playGroup(const std::string& group, float gain = 0.65f,
                   VoicePriority priority = VoicePriority::Normal);

    // Streaming background music (MUSIC/Track*.ogg or remastered tracks in audio-hd/music/).
    bool startMusic(const std::filesystem::path& musicDir, float gain = 0.28f);
    void updateMusic();
    bool toggleMusicMute();
    bool nextMusicTrack();
    bool musicPlaying() const;

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
    std::uint32_t variantCounter_{0};
    std::map<std::string, std::vector<HdClip>> hdClips_;
    std::unique_ptr<MusicState> music_;

    bool openMusicTrack(std::size_t index);
};

} // namespace ss2
