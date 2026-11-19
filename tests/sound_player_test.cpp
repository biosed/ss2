#include "audio/sound_player.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

    std::cout << "sound_player_test: ok\n";
    return 0;
}
