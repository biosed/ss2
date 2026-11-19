#include "audio/sound_player.hpp"
#include <algorithm>
#include <array>
#include <dlfcn.h>
#include <iostream>

namespace ss2 {
namespace {
constexpr std::size_t kVoicePool = 32;

struct VorbisInfo {
    int version;
    int channels;
    long rate;
};

using OvFopenFn = int (*)(const char*, void*);
using OvInfoFn = VorbisInfo* (*)(void*, int);
using OvReadFn = long (*)(void*, char*, int, int, int, int, int*);
using OvClearFn = int (*)(void*);
} // namespace

struct SoundPlayer::MusicState {
    void* lib{nullptr};
    OvFopenFn ov_fopen{nullptr};
    OvInfoFn ov_info{nullptr};
    OvReadFn ov_read{nullptr};
    OvClearFn ov_clear{nullptr};

    SDL_AudioStream* stream{nullptr};
    std::vector<std::filesystem::path> playlist;
    std::size_t trackIndex{0};
    float gain{0.28f};
    bool muted{false};

    // Opaque 2048-byte aligned storage for OggVorbis_File (~840 bytes on 64-bit Linux).
    alignas(16) std::array<std::uint64_t, 256> vfStorage{};
    bool vfOpen{false};

    ~MusicState() {
        if (vfOpen && ov_clear) {
            ov_clear(vfStorage.data());
            vfOpen = false;
        }
        if (stream) SDL_DestroyAudioStream(stream);
        if (lib) dlclose(lib);
    }
};

SoundPlayer::SoundPlayer(assets::SoundBank bank) : bank_(std::move(bank)) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 48000;
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!device_) return;

    voices_.resize(kVoicePool);
    for (auto& v : voices_) {
        v.stream = SDL_CreateAudioStream(&spec, &spec);
        if (v.stream) {
            SDL_BindAudioStream(device_, v.stream);
        }
    }
    SDL_ResumeAudioDevice(device_);
}

SoundPlayer::~SoundPlayer() {
    music_.reset();
    for (auto& v : voices_) {
        if (v.stream) SDL_DestroyAudioStream(v.stream);
    }
    if (device_) SDL_CloseAudioDevice(device_);
}

void SoundPlayer::setHdAudioDir(const std::filesystem::path& dir) {
    hdClips_.clear();
    if (dir.empty() || !std::filesystem::is_directory(dir)) return;
    std::vector<std::filesystem::path> wavs;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wav") {
            wavs.push_back(entry.path());
        }
    }
    std::sort(wavs.begin(), wavs.end());
    std::size_t loaded = 0;
    for (const auto& path : wavs) {
        const std::string stem = path.stem().string();
        // Naming convention: <group>_<variant>.wav (e.g., smg_burst_0.wav) or <group>.wav
        std::string group = stem;
        if (const auto us = stem.rfind('_'); us != std::string::npos && us + 1 < stem.size()) {
            bool allDigits = true;
            for (std::size_t i = us + 1; i < stem.size(); ++i) {
                if (stem[i] < '0' || stem[i] > '9') { allDigits = false; break; }
            }
            if (allDigits) group = stem.substr(0, us);
        }
        HdClip clip;
        if (loadWavPcm(path, clip.sampleRate, clip.channels, clip.pcm)) {
            hdClips_[group].push_back(std::move(clip));
            ++loaded;
        }
    }
    if (loaded > 0) {
        std::cout << "HD 48 kHz audio overrides: " << loaded << " clips across " << hdClips_.size() << " groups\n";
    }
}

bool SoundPlayer::playGroup(const std::string& group, float gain, VoicePriority priority) {
    if (!device_ || voices_.empty()) return false;
    const std::uint32_t variant = variantCounter_++;

    const std::uint8_t* pcmData = nullptr;
    int pcmBytes = 0;
    int baseFreq = 22050;
    int channels = 1;

    if (const auto it = hdClips_.find(group); it != hdClips_.end() && !it->second.empty()) {
        const auto& hc = it->second[variant % it->second.size()];
        pcmData = hc.pcm.data();
        pcmBytes = static_cast<int>(hc.pcm.size());
        baseFreq = static_cast<int>(hc.sampleRate);
        channels = static_cast<int>(hc.channels);
    } else {
        const auto* clip = bank_.clip(group, variant);
        if (!clip || clip->pcm.empty() || clip->bitsPerSample != 16) return false;
        pcmData = clip->pcm.data();
        pcmBytes = static_cast<int>(clip->pcm.size());
        baseFreq = static_cast<int>(clip->sampleRate ? clip->sampleRate : 22050u);
        channels = 1;
        // Cap long retail ambient/death tails (e.g., 16s fall_small) so voices return cleanly to pool.
        const int maxBytes = baseFreq * 2 * 3; // 3 seconds max for SFX
        if (pcmBytes > maxBytes) pcmBytes = maxBytes;
    }

    // Pick a pool slot: an idle voice first, otherwise steal the oldest voice
    // with the lowest priority. A cue that matters less than every running
    // voice is dropped instead of cutting one off. The loudness of a cue never
    // decides whether it is heard.
    const std::uint64_t now = SDL_GetTicks();
    std::vector<VoiceSlot> slots;
    slots.reserve(voices_.size());
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        const std::size_t idx = (nextVoice_ + i) % voices_.size();
        VoiceSlot slot;
        slot.priority = voices_[idx].priority;
        // A voice stays busy until its clip has played out. The stream alone
        // reports no data as soon as the device has pulled it into the output
        // buffer, which is before the sound is over.
        slot.active = voices_[idx].stream != nullptr &&
                      (SDL_GetAudioStreamAvailable(voices_[idx].stream) > 0 || now < voices_[idx].expiresAt);
        slot.started = voices_[idx].started;
        slots.push_back(slot);
    }
    const int chosen = chooseVoice(slots, priority);
    if (chosen < 0) return false;
    const std::size_t voiceIndex = (nextVoice_ + static_cast<std::size_t>(chosen)) % voices_.size();
    nextVoice_ = (voiceIndex + 1) % voices_.size();

    SDL_AudioStream* stream = voices_[voiceIndex].stream;
    if (!stream) return false;

    SDL_AudioSpec src{};
    src.format = SDL_AUDIO_S16;
    src.channels = channels;
    const float pitchMul = 0.97f + 0.015f * static_cast<float>(variant % 5u);
    src.freq = std::max(8000, static_cast<int>(static_cast<float>(baseFreq) * pitchMul));
    SDL_SetAudioStreamFormat(stream, &src, nullptr);
    SDL_SetAudioStreamGain(stream, std::clamp(gain, 0.05f, 1.0f));
    SDL_ClearAudioStream(stream);
    if (!SDL_PutAudioStreamData(stream, pcmData, pcmBytes)) {
        voices_[voiceIndex].active = false;
        return false;
    }
    voices_[voiceIndex].priority = priority;
    voices_[voiceIndex].active = true;
    voices_[voiceIndex].started = ++voiceSequence_;
    const std::uint64_t frameBytes = static_cast<std::uint64_t>(channels) * 2u;
    const std::uint64_t durationMs = frameBytes > 0 && src.freq > 0
                                         ? (static_cast<std::uint64_t>(pcmBytes) * 1000u) / (frameBytes * static_cast<std::uint64_t>(src.freq))
                                         : 0u;
    voices_[voiceIndex].expiresAt = SDL_GetTicks() + durationMs + 50u; // small margin
    return true;
}

bool SoundPlayer::openMusicTrack(std::size_t index) {
    if (!music_ || music_->playlist.empty()) return false;
    if (music_->vfOpen && music_->ov_clear) {
        music_->ov_clear(music_->vfStorage.data());
        music_->vfOpen = false;
    }
    music_->trackIndex = index % music_->playlist.size();
    const auto& path = music_->playlist[music_->trackIndex];
    if (music_->ov_fopen(path.c_str(), music_->vfStorage.data()) != 0) return false;
    music_->vfOpen = true;
    const auto* info = music_->ov_info(music_->vfStorage.data(), -1);
    if (!info || info->channels < 1 || info->channels > 2 || info->rate < 8000) {
        music_->ov_clear(music_->vfStorage.data());
        music_->vfOpen = false;
        return false;
    }
    SDL_AudioSpec src{};
    src.format = SDL_AUDIO_S16;
    src.channels = info->channels;
    src.freq = static_cast<int>(info->rate);
    SDL_SetAudioStreamFormat(music_->stream, &src, nullptr);
    SDL_SetAudioStreamGain(music_->stream, music_->muted ? 0.0f : music_->gain);
    std::cout << "Music track: " << path.filename().string() << " (" << info->rate << " Hz, "
              << info->channels << "ch)\n";
    return true;
}

bool SoundPlayer::startMusic(const std::filesystem::path& musicDir, float gain) {
    if (!device_ || musicDir.empty() || !std::filesystem::is_directory(musicDir)) return false;
    std::vector<std::filesystem::path> tracks;
    for (const auto& entry : std::filesystem::directory_iterator(musicDir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // The container magic decides, not the extension: retail MUSIC keeps one
        // track as "_Track11.og_", which an extension-only match skips.
        if (hasOggMagic(entry.path()) || ext == ".ogg") tracks.push_back(entry.path());
    }
    if (tracks.empty()) return false;
    std::sort(tracks.begin(), tracks.end());

    auto st = std::make_unique<MusicState>();
    st->lib = dlopen("libvorbisfile.so.3", RTLD_LAZY);
    if (!st->lib) st->lib = dlopen("libvorbisfile.so", RTLD_LAZY);
    if (!st->lib) return false;

    st->ov_fopen = reinterpret_cast<OvFopenFn>(dlsym(st->lib, "ov_fopen"));
    st->ov_info = reinterpret_cast<OvInfoFn>(dlsym(st->lib, "ov_info"));
    st->ov_read = reinterpret_cast<OvReadFn>(dlsym(st->lib, "ov_read"));
    st->ov_clear = reinterpret_cast<OvClearFn>(dlsym(st->lib, "ov_clear"));
    if (!st->ov_fopen || !st->ov_info || !st->ov_read || !st->ov_clear) return false;

    // Keep only the tracks the Ogg Vorbis decoder can open, so a next-track
    // press never lands on a file the decoder rejects.
    std::vector<std::filesystem::path> playable;
    for (const auto& path : tracks) {
        if (st->ov_fopen(path.c_str(), st->vfStorage.data()) != 0) continue;
        const auto* info = st->ov_info(st->vfStorage.data(), -1);
        const bool ok = info && info->channels >= 1 && info->channels <= 2 && info->rate >= 8000;
        st->ov_clear(st->vfStorage.data());
        if (ok) playable.push_back(path);
    }
    if (playable.empty()) return false;

    SDL_AudioSpec dst{};
    dst.format = SDL_AUDIO_S16;
    dst.channels = 2;
    dst.freq = 48000;
    st->stream = SDL_CreateAudioStream(&dst, &dst);
    if (!st->stream || !SDL_BindAudioStream(device_, st->stream)) return false;

    st->playlist = std::move(playable);
    st->gain = std::clamp(gain, 0.0f, 1.0f);
    music_ = std::move(st);
    if (!openMusicTrack(0)) {
        music_.reset();
        return false;
    }
    updateMusic();
    return true;
}

void SoundPlayer::updateMusic() {
    if (!music_ || !music_->stream || !music_->vfOpen) return;
    // Keep ~1.5 seconds of stereo 48kHz audio queued (~288 KB) so playback never starves.
    constexpr int kTargetQueuedBytes = 288 * 1024;
    std::array<char, 16384> chunk{};
    int bitstream = 0;
    while (SDL_GetAudioStreamQueued(music_->stream) < kTargetQueuedBytes) {
        const long n = music_->ov_read(music_->vfStorage.data(), chunk.data(),
                                       static_cast<int>(chunk.size()), 0, 2, 1, &bitstream);
        if (n > 0) {
            SDL_PutAudioStreamData(music_->stream, chunk.data(), static_cast<int>(n));
        } else if (n == 0) {
            if (!openMusicTrack(music_->trackIndex + 1)) break;
        } else {
            break;
        }
    }
}

bool SoundPlayer::toggleMusicMute() {
    if (!music_ || !music_->stream) return false;
    music_->muted = !music_->muted;
    SDL_SetAudioStreamGain(music_->stream, music_->muted ? 0.0f : music_->gain);
    return music_->muted;
}

bool SoundPlayer::nextMusicTrack() {
    if (!music_ || !music_->stream) return false;
    SDL_ClearAudioStream(music_->stream);
    if (!openMusicTrack(music_->trackIndex + 1)) return false;
    updateMusic();
    return true;
}

bool SoundPlayer::musicPlaying() const {
    return music_ && music_->vfOpen && !music_->muted;
}

} // namespace ss2
