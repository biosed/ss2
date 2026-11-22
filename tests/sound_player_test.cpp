#include "audio/sound_player.hpp"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
}

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
}

std::int16_t readS16(const std::vector<std::uint8_t>& pcm, std::size_t byteOffset) {
    const std::uint16_t u = static_cast<std::uint16_t>(
        pcm[byteOffset] | (static_cast<std::uint16_t>(pcm[byteOffset + 1]) << 8u));
    return static_cast<std::int16_t>(u);
}

// Builds a RIFF/WAVE file with 16- or 24-bit PCM samples.
std::vector<std::uint8_t> makeWav(std::uint16_t channels, std::uint16_t bits,
                                   const std::vector<std::uint32_t>& samples) {
    const std::uint32_t sampleRate = 48000;
    const std::uint16_t bytesPerSample = static_cast<std::uint16_t>(bits / 8);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * bytesPerSample);
    std::vector<std::uint8_t> data;
    for (std::uint32_t value : samples) {
        for (std::uint16_t c = 0; c < channels; ++c) {
            if (bits == 16) {
                data.push_back(static_cast<std::uint8_t>(value & 0xFFu));
                data.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
            } else {
                data.push_back(static_cast<std::uint8_t>(value & 0xFFu));
                data.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
                data.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
            }
        }
    }

    std::vector<std::uint8_t> wav;
    const char riff[4] = {'R', 'I', 'F', 'F'};
    const char wave[4] = {'W', 'A', 'V', 'E'};
    const char fmt[4] = {'f', 'm', 't', ' '};
    const char dataId[4] = {'d', 'a', 't', 'a'};
    wav.insert(wav.end(), riff, riff + 4);
    putU32(wav, static_cast<std::uint32_t>(36 + data.size()));
    wav.insert(wav.end(), wave, wave + 4);
    wav.insert(wav.end(), fmt, fmt + 4);
    putU32(wav, 16);
    putU16(wav, 1); // PCM
    putU16(wav, channels);
    putU32(wav, sampleRate);
    putU32(wav, sampleRate * blockAlign);
    putU16(wav, blockAlign);
    putU16(wav, bits);
    wav.insert(wav.end(), dataId, dataId + 4);
    putU32(wav, static_cast<std::uint32_t>(data.size()));
    wav.insert(wav.end(), data.begin(), data.end());
    return wav;
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path scratchDir() {
    const auto dir = std::filesystem::temp_directory_path() / "ss2-sound-player-test";
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

int main() {
    using ss2::VoicePriority;

    // --- voice allocation ------------------------------------------------
    {
        std::vector<ss2::VoiceSlot> slots;
        if (ss2::chooseVoice(slots, VoicePriority::Normal) != -1)
            throw std::runtime_error("an empty pool must not hand out a voice");
    }
    {
        std::vector<ss2::VoiceSlot> slots(4);
        slots[2].priority = VoicePriority::High;
        slots[2].active = true;
        // An idle voice is reused even for the least important cue.
        if (ss2::chooseVoice(slots, VoicePriority::Low) != 0)
            throw std::runtime_error("an idle voice must be reused");
    }
    {
        std::vector<ss2::VoiceSlot> slots(4);
        slots[0].priority = VoicePriority::High;
        slots[0].active = true;
        slots[0].started = 1;
        slots[1].priority = VoicePriority::Normal;
        slots[1].active = true;
        slots[1].started = 2;
        slots[2].priority = VoicePriority::High;
        slots[2].active = true;
        slots[2].started = 3;
        slots[3].priority = VoicePriority::High;
        slots[3].active = true;
        slots[3].started = 4;
        // The quiet Normal cue takes the Normal voice, not a High one.
        if (ss2::chooseVoice(slots, VoicePriority::Normal) != 1)
            throw std::runtime_error("the lowest priority voice must be stolen");
    }
    {
        std::vector<ss2::VoiceSlot> slots(3);
        for (auto& s : slots) {
            s.priority = VoicePriority::High;
            s.active = true;
            s.started = 1;
        }
        // A cue that matters less than every running voice is dropped.
        if (ss2::chooseVoice(slots, VoicePriority::Low) != -1)
            throw std::runtime_error("a cue below every active voice must be dropped");
        if (ss2::chooseVoice(slots, VoicePriority::Normal) != -1)
            throw std::runtime_error("a cue below every active voice must be dropped");
        if (ss2::chooseVoice(slots, VoicePriority::High) != 0)
            throw std::runtime_error("an equal priority voice must be stolen");
    }
    {
        std::vector<ss2::VoiceSlot> slots(3);
        for (auto& s : slots) {
            s.priority = VoicePriority::Normal;
            s.active = true;
        }
        slots[0].started = 7;
        slots[1].started = 3; // oldest of the equal priorities
        slots[2].started = 5;
        if (ss2::chooseVoice(slots, VoicePriority::Normal) != 1)
            throw std::runtime_error("the oldest voice of equal priority must be stolen");
    }

    // --- non-repeating multi-variant cycling -----------------------------
    {
        ss2::VariantSelector selector;
        for (int i = 0; i < 8; ++i) {
            if (selector.next("accept", 0) != 0u || selector.next("accept", 1) != 0u)
                throw std::runtime_error("single-variant group must always pick index 0");
        }
        for (std::size_t count : {std::size_t{2}, std::size_t{3}, std::size_t{5}, std::size_t{6}}) {
            const std::string group = "d_select_" + std::to_string(count);
            std::size_t prev = count;
            for (int bag = 0; bag < 6; ++bag) {
                std::set<std::size_t> seenInBag;
                for (std::size_t step = 0; step < count; ++step) {
                    // Interleave unrelated SFX calls to verify per-group isolation.
                    (void)selector.next("rifle", 3);
                    const std::size_t pick = selector.next(group, count);
                    if (pick >= count)
                        throw std::runtime_error("VariantSelector returned out-of-range variant");
                    if (pick == prev)
                        throw std::runtime_error("VariantSelector repeated same take back-to-back");
                    seenInBag.insert(pick);
                    prev = pick;
                }
                if (seenInBag.size() != count)
                    throw std::runtime_error("VariantSelector failed to cover all takes in cycle");
            }
        }
    }

    // --- stereo pan clamping & S16 mixing --------------------------------
    {
        if (ss2::clampPan(-3.0f) != -1.0f || ss2::clampPan(2.5f) != 1.0f || ss2::clampPan(0.0f) != 0.0f)
            throw std::runtime_error("clampPan must clamp to [-1.0, +1.0]");
        const auto [cL, cR] = ss2::stereoPanGains(0.0f);
        if (std::abs(cL - 1.0f) > 1e-5f || std::abs(cR - 1.0f) > 1e-5f)
            throw std::runtime_error("center pan must yield unity stereo gains");
        const auto [hL, hLRight] = ss2::stereoPanGains(-4.0f);
        if (std::abs(hL - 1.0f) > 1e-5f || std::abs(hLRight - 0.0f) > 1e-5f)
            throw std::runtime_error("hard-left clamped pan must silence right channel");
        const auto [hRLeft, hR] = ss2::stereoPanGains(4.0f);
        if (std::abs(hRLeft - 0.0f) > 1e-5f || std::abs(hR - 1.0f) > 1e-5f)
            throw std::runtime_error("hard-right clamped pan must silence left channel");

        if (ss2::viewportStereoPan(0.0, 640.0) != 0.0f)
            throw std::runtime_error("centered world sound must have zero pan");
        const float leftEdge = ss2::viewportStereoPan(-640.0, 640.0);
        const float rightEdge = ss2::viewportStereoPan(640.0, 640.0);
        if (std::abs(leftEdge + 0.45f) > 1e-4f || std::abs(rightEdge - 0.45f) > 1e-4f)
            throw std::runtime_error("viewport edge pan must be symmetric +/-0.45");
        if (std::abs(ss2::viewportStereoPan(-5000.0, 640.0, 0.65f) + 0.65f) > 1e-4f ||
            std::abs(ss2::viewportStereoPan(5000.0, 640.0, 0.65f) - 0.65f) > 1e-4f)
            throw std::runtime_error("off-screen viewportStereoPan must clamp to maxPan");

        // Mono S16 sample (+10000, -10000) panned +0.5 -> L=5000, R=10000; -5000, -10000.
        std::vector<std::uint8_t> monoSrc;
        putU16(monoSrc, static_cast<std::uint16_t>(std::int16_t{10000}));
        putU16(monoSrc, static_cast<std::uint16_t>(std::int16_t{-10000}));
        std::vector<std::uint8_t> stereoOut;
        ss2::applyStereoPanS16(monoSrc, 1, 0.5f, stereoOut);
        if (stereoOut.size() != 8u)
            throw std::runtime_error("applyStereoPanS16 mono->stereo frame byte count mismatch");
        if (readS16(stereoOut, 0) != 5000 || readS16(stereoOut, 2) != 10000 ||
            readS16(stereoOut, 4) != -5000 || readS16(stereoOut, 6) != -10000) {
            throw std::runtime_error("applyStereoPanS16 +0.5 right-pan sample mismatch");
        }
        ss2::applyStereoPanS16(monoSrc, 1, -1.5f, stereoOut);
        if (readS16(stereoOut, 0) != 10000 || readS16(stereoOut, 2) != 0 ||
            readS16(stereoOut, 4) != -10000 || readS16(stereoOut, 6) != 0) {
            throw std::runtime_error("applyStereoPanS16 clamped hard-left sample mismatch");
        }
    }

    // --- Ogg container magic ---------------------------------------------
    const auto dir = scratchDir();
    {
        const auto track = dir / "_Track11.og_"; // retail MUSIC naming
        writeFile(track, {'O', 'g', 'g', 'S', 0x00, 0x02, 0x00, 0x00});
        if (!ss2::hasOggMagic(track))
            throw std::runtime_error("an OggS file with a wrong extension must be detected");
        if (ss2::hasOggMagic(dir / "missing.ogg"))
            throw std::runtime_error("a missing file must not report an Ogg magic");
        const auto wave = dir / "notmusic.ogg";
        writeFile(wave, {'R', 'I', 'F', 'F', 0x24, 0x08, 0x00, 0x00});
        if (ss2::hasOggMagic(wave))
            throw std::runtime_error("a non-Ogg file must not report an Ogg magic");
    }

    // --- WAV loading ------------------------------------------------------
    {
        const auto path = dir / "smg_burst_0.wav";
        writeFile(path, makeWav(1, 16, {0x1234u, 0xABCDu}));
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::vector<std::uint8_t> pcm;
        if (!ss2::loadWavPcm(path, sampleRate, channels, pcm))
            throw std::runtime_error("a 16-bit WAV must load");
        if (sampleRate != 48000 || channels != 1 || pcm.size() != 4)
            throw std::runtime_error("a 16-bit WAV must keep its rate, channels and length");
        if (pcm[0] != 0x34 || pcm[1] != 0x12 || pcm[2] != 0xCD || pcm[3] != 0xAB)
            throw std::runtime_error("a 16-bit WAV must keep its samples");
    }
    {
        const auto path = dir / "fire_0.wav";
        writeFile(path, makeWav(2, 24, {0x123456u, 0x00ABCDEFu}));
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::vector<std::uint8_t> pcm;
        if (!ss2::loadWavPcm(path, sampleRate, channels, pcm))
            throw std::runtime_error("a 24-bit WAV must load");
        if (sampleRate != 48000 || channels != 2)
            throw std::runtime_error("a 24-bit WAV must keep its rate and channels");
        if (pcm.size() != 8)
            throw std::runtime_error("a 24-bit WAV must be converted to 16-bit PCM");
        // The top 16 bits of every 24-bit sample survive the conversion.
        if (pcm[0] != 0x34 || pcm[1] != 0x12 || pcm[2] != 0x34 || pcm[3] != 0x12 ||
            pcm[4] != 0xCD || pcm[5] != 0xAB || pcm[6] != 0xCD || pcm[7] != 0xAB)
            throw std::runtime_error("a 24-bit WAV must keep the top 16 bits of each sample");
    }
    {
        const auto path = dir / "old_0.wav";
        writeFile(path, makeWav(1, 8, {0x12u, 0x34u}));
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::vector<std::uint8_t> pcm;
        if (ss2::loadWavPcm(path, sampleRate, channels, pcm))
            throw std::runtime_error("an 8-bit WAV must be rejected");
    }
    {
        const auto path = dir / "broken.wav";
        writeFile(path, {'N', 'O', 'P', 'E', 0, 0, 0, 0});
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::vector<std::uint8_t> pcm;
        if (ss2::loadWavPcm(path, sampleRate, channels, pcm))
            throw std::runtime_error("a file without a RIFF header must be rejected");
    }

    {
        if (ss2::clampVolume(-0.5f) != 0.0f) throw std::runtime_error("clampVolume negative did not clamp to 0");
        if (ss2::clampVolume(2.5f) != 1.0f) throw std::runtime_error("clampVolume overflow did not clamp to 1");
        if (std::abs(ss2::clampVolume(0.4f) - 0.4f) > 0.001f) {
            throw std::runtime_error("clampVolume 0.4 mismatch");
        }
        if (ss2::scaledSfxGain(0.8f, 0.0f) != 0.0f || ss2::scaledSfxGain(0.8f, 0.0005f) != 0.0f) {
            throw std::runtime_error("scaledSfxGain muted master must yield 0");
        }
        if (std::abs(ss2::scaledSfxGain(0.65f, 0.5f) - 0.325f) > 0.001f) {
            throw std::runtime_error("scaledSfxGain 0.65 * 0.5 mismatch");
        }
        if (std::abs(ss2::scaledSfxGain(0.01f, 0.5f) - 0.02f) > 0.001f) {
            throw std::runtime_error("scaledSfxGain minimum clamp 0.02 mismatch");
        }
        if (ss2::scaledSfxGain(2.0f, 2.0f) != 1.0f) {
            throw std::runtime_error("scaledSfxGain maximum clamp 1.0 mismatch");
        }
    }

    {
        if (ss2::spatialAttenuation(0.0, 0.0, 600.0) != 1.0f ||
            ss2::spatialAttenuation(250.0, 0.0, 600.0) != 1.0f) {
            throw std::runtime_error("spatialAttenuation inside viewport must be 1.0");
        }
        const float mid = ss2::spatialAttenuation(1110.0, 0.0, 600.0);
        if (mid <= 0.20f || mid >= 0.30f) {
            throw std::runtime_error("spatialAttenuation mid-distance quadratic rolloff mismatch");
        }
        if (ss2::spatialAttenuation(5000.0, 0.0, 600.0) != 0.0f) {
            throw std::runtime_error("spatialAttenuation far off-screen must be 0.0");
        }
        if (ss2::spatialGain(0.5f, 5000.0, 0.0, 600.0) != 0.0f ||
            ss2::spatialGain(0.5f, 1450.0, 0.0, 600.0) != 0.0f) {
            throw std::runtime_error("spatialGain off-screen / sub-threshold culling mismatch");
        }
        if (std::abs(ss2::spatialGain(0.5f, 100.0, 100.0, 600.0) - 0.5f) > 0.001f) {
            throw std::runtime_error("spatialGain in-view gain mismatch");
        }
    }

    std::cout << "sound_player_test: ok\n";
    return 0;
}
