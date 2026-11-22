#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ss2::assets {

struct MissionArchive;

inline constexpr int kBriefingOverlayWidth = 768;
inline constexpr int kBriefingOverlayHeight = 384;
inline constexpr int kBriefingVoiceSampleRate = 22050;

/// Single animated layer entry unpacked from a mission's `I-BRF{1..4}OFF` table.
struct BriefingLayer {
    std::uint32_t subIndex{0};
    std::uint32_t dataOffset{0};
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t alpha{255};
};

/// Composite animation frame referencing a contiguous slice of `BriefingLayer`s.
struct BriefingFrame {
    std::uint32_t firstLayer{0};
    std::uint32_t layerCount{0};
};

/// One tactical map briefing part (`I-BRF{1..4}DAT`, `I-BRF{1..4}OFF`, `I-BRF{1..4}SND`).
struct BriefingPart {
    std::vector<std::uint8_t> dat;
    std::vector<BriefingLayer> layers;
    std::vector<BriefingFrame> frames;
    std::vector<std::int16_t> voicePcm;
};

/// Decoded retail briefing animations, 22050 Hz mono voiceovers, and `~`-split paragraphs.
struct MissionBriefingMedia {
    std::vector<std::string> paragraphs;
    std::vector<std::int16_t> overviewVoicePcm;
    std::vector<BriefingPart> parts;

    bool hasAnimation() const {
        for (const auto& p : parts) {
            if (!p.frames.empty()) return true;
        }
        return false;
    }

    bool hasVoice() const {
        if (!overviewVoicePcm.empty()) return true;
        for (const auto& p : parts) {
            if (!p.voicePcm.empty()) return true;
        }
        return false;
    }

    bool empty() const {
        return !hasAnimation() && !hasVoice() && paragraphs.size() <= 1u;
    }
};

/// Split `I-DESC` text on retail `~` delimiters into trimmed overview + part paragraphs.
std::vector<std::string> splitBriefingParagraphs(std::string_view description);

/// Decode a retail `"2D"` Dialogic OKI 4-bit ADPCM `.apm` stream into 22050 Hz mono PCM16.
std::vector<std::int16_t> decodeApmAudio(std::span<const std::uint8_t> apmBytes);

/// Parse an `I-BRF{1..4}OFF` text directory against an `I-BRF{1..4}DAT` payload size.
bool parseBriefingOff(std::span<const std::uint8_t> offBytes,
                      std::size_t datSize,
                      std::vector<BriefingLayer>& outLayers,
                      std::vector<BriefingFrame>& outFrames);

/// Rasterize one animated briefing frame into a premultiplied `CAIRO_FORMAT_ARGB32` buffer.
void renderBriefingFrameArgb32(const BriefingPart& part,
                               std::size_t frameIndex,
                               std::uint32_t* argb32Dst,
                               int dstWidth,
                               int dstHeight,
                               int dstStridePixels);

/// Load all briefing animation parts, ADPCM voiceovers, and paragraphs from an archive.
MissionBriefingMedia readMissionBriefingMedia(const MissionArchive& archive);

/// Load all briefing animation parts, ADPCM voiceovers, and paragraphs from a mission file.
MissionBriefingMedia readMissionBriefingMedia(const std::filesystem::path& missionPath);

} // namespace ss2::assets
