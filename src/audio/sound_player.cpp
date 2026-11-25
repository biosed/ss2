#include "audio/sound_player.hpp"
#include <SDL3/SDL_loadso.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <iostream>

namespace ss2 {
namespace {
constexpr std::size_t kVoicePool = 32;

struct VorbisInfo {
    int version;
    int channels;
    long rate;
};

struct OvCallbacks {
    std::size_t (*read_func)(void* ptr, std::size_t size, std::size_t nmemb, void* datasource);
    int (*seek_func)(void* datasource, std::int64_t offset, int whence);
    int (*close_func)(void* datasource);
    long (*tell_func)(void* datasource);
};

using OvFopenFn = int (*)(const char*, void*);
using OvOpenCallbacksFn = int (*)(void*, void*, const char*, long, OvCallbacks);
using OvInfoFn = VorbisInfo* (*)(void*, int);
using OvReadFn = long (*)(void*, char*, int, int, int, int, int*);
using OvClearFn = int (*)(void*);

#ifdef _WIN32
std::size_t vfRead(void* ptr, std::size_t size, std::size_t nmemb, void* ds) {
    return std::fread(ptr, size, nmemb, static_cast<std::FILE*>(ds));
}
int vfSeek(void* ds, std::int64_t offset, int whence) {
    return _fseeki64(static_cast<std::FILE*>(ds), offset, whence);
}
int vfClose(void* ds) {
    return std::fclose(static_cast<std::FILE*>(ds));
}
long vfTell(void* ds) {
    const __int64 pos = _ftelli64(static_cast<std::FILE*>(ds));
    return (pos < 0 || pos > LONG_MAX) ? -1L : static_cast<long>(pos);
}
#endif
} // namespace

struct SoundPlayer::MusicState {
    SDL_SharedObject* lib{nullptr};
    OvFopenFn ov_fopen{nullptr};
    OvOpenCallbacksFn ov_open_callbacks{nullptr};
    OvInfoFn ov_info{nullptr};
    OvReadFn ov_read{nullptr};
    OvClearFn ov_clear{nullptr};

    SDL_AudioStream* stream{nullptr};
    std::vector<std::filesystem::path> playlist;
    std::size_t trackIndex{0};
    float gain{0.28f};
    bool muted{false};

    // Opaque 2048-byte aligned storage for OggVorbis_File (~840 bytes on 64-bit).
    alignas(16) std::array<std::uint64_t, 256> vfStorage{};
    bool vfOpen{false};

    ~MusicState() {
        if (vfOpen && ov_clear) {
            ov_clear(vfStorage.data());
            vfOpen = false;
        }
        if (stream) SDL_DestroyAudioStream(stream);
        if (lib) SDL_UnloadObject(lib);
    }

    bool openTrack(const std::filesystem::path& path) {
#ifdef _WIN32
        if (ov_open_callbacks) {
            std::FILE* f = _wfopen(path.c_str(), L"rb");
            if (!f) return false;
            const OvCallbacks cb{vfRead, vfSeek, vfClose, vfTell};
            if (ov_open_callbacks(f, vfStorage.data(), nullptr, 0, cb) != 0) {
                std::fclose(f);
                return false;
            }
            return true;
        }
#endif
        if (!ov_fopen) return false;
        const std::string pathStr = path.string();
        return ov_fopen(pathStr.c_str(), vfStorage.data()) == 0;
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
        std::string group = audio_detail::lowerAscii(stem);
        if (const auto us = group.rfind('_'); us != std::string::npos && us + 1 < group.size()) {
            bool allDigits = true;
            for (std::size_t i = us + 1; i < group.size(); ++i) {
                if (group[i] < '0' || group[i] > '9') { allDigits = false; break; }
            }
            if (allDigits) group = group.substr(0, us);
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

bool SoundPlayer::playGroup(const std::string& group, float gain, VoicePriority priority, float pan) {
    const float effectiveGain = scaledSfxGain(gain, sfxVolume_);
    if (!device_ || voices_.empty() || effectiveGain <= 0.0f) return false;

    const std::string normGroup = audio_detail::lowerAscii(group);
    const std::uint8_t* pcmData = nullptr;
    int pcmBytes = 0;
    int fullClipBytes = 0;
    int baseFreq = 22050;
    int channels = 1;

    const bool isVoiceover = (priority == VoicePriority::High) ||
                             (!normGroup.empty() && normGroup[0] == '$') ||
                             (normGroup.size() > 2 && normGroup[1] == '_' &&
                              (normGroup[0] == 'a' || normGroup[0] == 'b' || normGroup[0] == 'd' ||
                               normGroup[0] == 'f' || normGroup[0] == 'i' || normGroup[0] == 'j' ||
                               normGroup[0] == 'r'));

    if (const auto it = hdClips_.find(normGroup); it != hdClips_.end() && !it->second.empty()) {
        const std::size_t variant = variantSelector_.next(normGroup, it->second.size());
        const auto& hc = it->second[variant];
        pcmData = hc.pcm.data();
        pcmBytes = static_cast<int>(hc.pcm.size());
        fullClipBytes = pcmBytes;
        baseFreq = static_cast<int>(hc.sampleRate);
        channels = static_cast<int>(hc.channels);
    } else {
        const std::size_t count = bank_.variantCount(normGroup);
        if (count == 0u) return false;
        std::size_t variant = variantSelector_.next(normGroup, count);
        const auto* clip = bank_.clip(normGroup, variant);
        if (!clip || clip->pcm.empty() || clip->bitsPerSample != 16) return false;
        auto isSilentStub = [](const assets::SoundClip* c) {
            if (!c || c->pcm.empty() || c->bitsPerSample != 16) return false;
            const int freq = static_cast<int>(c->sampleRate ? c->sampleRate : 22050u);
            if (static_cast<int>(c->pcm.size()) >= freq * 2 / 10) return false;
            for (std::size_t i = 0; i + 1 < c->pcm.size(); i += 2) {
                if (c->pcm[i] != 0u || c->pcm[i + 1] != 0u) return false;
            }
            return true;
        };
        if (count > 1u && isSilentStub(clip)) {
            variant = variantSelector_.next(normGroup, count);
            clip = bank_.clip(normGroup, variant);
            if (!clip || clip->pcm.empty() || clip->bitsPerSample != 16) return false;
        }
        pcmData = clip->pcm.data();
        pcmBytes = static_cast<int>(clip->pcm.size());
        fullClipBytes = pcmBytes;
        baseFreq = static_cast<int>(clip->sampleRate ? clip->sampleRate : 22050u);
        channels = 1;
        if (!isVoiceover) {
            // Cap long retail ambient/death tails (e.g., 16s fall_small) so voices return cleanly to pool.
            const int maxSeconds = (priority == VoicePriority::Ambient || clip->loop) ? 9 : 3;
            const int maxBytes = baseFreq * 2 * maxSeconds;
            if (pcmBytes > maxBytes) pcmBytes = maxBytes;
        }
    }

    std::vector<std::uint8_t> fadeScratch;
    if (pcmBytes < fullClipBytes && pcmBytes >= channels * 2) {
        fadeScratch.assign(pcmData, pcmData + pcmBytes);
        const int frameBytes = channels * 2;
        const int totalFrames = pcmBytes / frameBytes;
        const int fadeSamples = std::min(totalFrames, std::max(1, baseFreq * 35 / 1000));
        for (int i = 0; i < fadeSamples; ++i) {
            const float fadeGain = (fadeSamples > 1)
                ? static_cast<float>(fadeSamples - 1 - i) / static_cast<float>(fadeSamples - 1)
                : 0.0f;
            const int frameIdx = totalFrames - fadeSamples + i;
            for (int c = 0; c < channels; ++c) {
                const std::size_t off =
                    (static_cast<std::size_t>(frameIdx) * static_cast<std::size_t>(channels) +
                     static_cast<std::size_t>(c)) * 2u;
                const auto sample = static_cast<std::int16_t>(audio_detail::readU16(fadeScratch.data() + off));
                const int faded = std::clamp(
                    static_cast<int>(std::lround(static_cast<float>(sample) * fadeGain)),
                    -32768, 32767);
                const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(faded));
                fadeScratch[off] = static_cast<std::uint8_t>(u & 0xFFu);
                fadeScratch[off + 1] = static_cast<std::uint8_t>((u >> 8u) & 0xFFu);
            }
        }
        pcmData = fadeScratch.data();
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

    const float clampedPan = clampPan(pan);
    const std::uint8_t* streamData = pcmData;
    int streamBytes = pcmBytes;
    int streamChannels = channels;
    if (std::abs(clampedPan) > 0.005f) {
        applyStereoPanS16(
            std::span<const std::uint8_t>(pcmData, static_cast<std::size_t>(pcmBytes)),
            channels, clampedPan, pannedScratch_);
        if (!pannedScratch_.empty()) {
            streamData = pannedScratch_.data();
            streamBytes = static_cast<int>(pannedScratch_.size());
            streamChannels = 2;
        }
    }

    SDL_AudioSpec src{};
    src.format = SDL_AUDIO_S16;
    src.channels = streamChannels;
    const std::uint32_t pitchStep = pitchCounter_++;
    const float pitchMul = isVoiceover ? 1.0f : (0.97f + 0.015f * static_cast<float>(pitchStep % 5u));
    src.freq = std::max(8000, static_cast<int>(static_cast<float>(baseFreq) * pitchMul));
    SDL_SetAudioStreamFormat(stream, &src, nullptr);
    SDL_SetAudioStreamGain(stream, effectiveGain);
    SDL_ClearAudioStream(stream);
    if (!SDL_PutAudioStreamData(stream, streamData, streamBytes)) {
        voices_[voiceIndex].active = false;
        return false;
    }
    voices_[voiceIndex].priority = priority;
    voices_[voiceIndex].active = true;
    voices_[voiceIndex].started = ++voiceSequence_;
    const std::uint64_t frameBytes = static_cast<std::uint64_t>(streamChannels) * 2u;
    const std::uint64_t durationMs =
        frameBytes > 0 && src.freq > 0
            ? (static_cast<std::uint64_t>(streamBytes) * 1000u) /
                  (frameBytes * static_cast<std::uint64_t>(src.freq))
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
    if (!music_->openTrack(path)) return false;
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
#ifdef _WIN32
    if (const char* base = SDL_GetBasePath(); base && *base) {
        const auto u8 = (std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(base))) /
                         "libvorbisfile-3.dll").u8string();
        const std::string baseDll(reinterpret_cast<const char*>(u8.data()), u8.size());
        st->lib = SDL_LoadObject(baseDll.c_str());
    }
    constexpr const char* kVorbisLibs[] = {"libvorbisfile-3.dll", "vorbisfile.dll", "libvorbisfile.dll"};
#else
    constexpr const char* kVorbisLibs[] = {"libvorbisfile.so.3", "libvorbisfile.so"};
#endif
    for (const char* libName : kVorbisLibs) {
        if (st->lib) break;
        st->lib = SDL_LoadObject(libName);
    }
    if (!st->lib) return false;

    st->ov_fopen = reinterpret_cast<OvFopenFn>(SDL_LoadFunction(st->lib, "ov_fopen"));
    st->ov_open_callbacks = reinterpret_cast<OvOpenCallbacksFn>(SDL_LoadFunction(st->lib, "ov_open_callbacks"));
    st->ov_info = reinterpret_cast<OvInfoFn>(SDL_LoadFunction(st->lib, "ov_info"));
    st->ov_read = reinterpret_cast<OvReadFn>(SDL_LoadFunction(st->lib, "ov_read"));
    st->ov_clear = reinterpret_cast<OvClearFn>(SDL_LoadFunction(st->lib, "ov_clear"));
    if ((!st->ov_fopen && !st->ov_open_callbacks) || !st->ov_info || !st->ov_read || !st->ov_clear) return false;

    // Keep only the tracks the Ogg Vorbis decoder can open, so a next-track
    // press never lands on a file the decoder rejects.
    std::vector<std::filesystem::path> playable;
    for (const auto& path : tracks) {
        if (!st->openTrack(path)) continue;
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
    musicGain_ = clampVolume(gain);
    st->gain = musicGain_;
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

void SoundPlayer::setSfxVolume(float volume) {
    sfxVolume_ = clampVolume(volume);
}

void SoundPlayer::setMusicVolume(float volume) {
    musicGain_ = clampVolume(volume);
    if (music_) {
        music_->gain = musicGain_;
        if (music_->stream) {
            SDL_SetAudioStreamGain(music_->stream, music_->muted ? 0.0f : music_->gain);
        }
    }
}

} // namespace ss2
