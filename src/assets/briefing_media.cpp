#include "assets/briefing_media.hpp"

#include "assets/mission.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

namespace ss2::assets {
namespace {

constexpr std::array<int, 16> kOkiIndexTable{{
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
}};

constexpr std::array<int, 49> kOkiStepTable{{
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
    279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
}};

std::string_view trimAsciiWs(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

std::string_view cstrSpanView(std::span<const std::uint8_t> bytes) {
    const auto it = std::ranges::find(bytes, 0);
    return std::string_view(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<std::size_t>(it - bytes.begin()));
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t pos) {
    return static_cast<std::uint16_t>(bytes[pos]) |
           (static_cast<std::uint16_t>(bytes[pos + 1]) << 8u);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t pos) {
    return static_cast<std::uint32_t>(bytes[pos]) |
           (static_cast<std::uint32_t>(bytes[pos + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[pos + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[pos + 3]) << 24u);
}

bool parseU32Token(std::string_view& line, std::uint32_t& out) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    if (line.empty()) return false;
    std::size_t len = 0;
    while (len < line.size() && line[len] != ' ' && line[len] != '\t') {
        ++len;
    }
    const auto res = std::from_chars(line.data(), line.data() + len, out);
    if (res.ec != std::errc{} || res.ptr != line.data() + len) return false;
    line.remove_prefix(len);
    return true;
}

std::uint32_t compositeOverArgb32(std::uint32_t dst, std::uint32_t src) {
    const std::uint32_t sa = src >> 24;
    if (sa == 0u) return dst;
    if (dst == 0u || sa == 255u) return src;
    const std::uint32_t inv = 255u - sa;
    const std::uint32_t da = (dst >> 24) & 0xFFu;
    const std::uint32_t dr = (dst >> 16) & 0xFFu;
    const std::uint32_t dg = (dst >> 8) & 0xFFu;
    const std::uint32_t db = dst & 0xFFu;
    const std::uint32_t sr = (src >> 16) & 0xFFu;
    const std::uint32_t sg = (src >> 8) & 0xFFu;
    const std::uint32_t sb = src & 0xFFu;
    const std::uint32_t oa = std::min(255u, sa + ((da * inv + 127u) / 255u));
    const std::uint32_t or_ = std::min(255u, sr + ((dr * inv + 127u) / 255u));
    const std::uint32_t og = std::min(255u, sg + ((dg * inv + 127u) / 255u));
    const std::uint32_t ob = std::min(255u, sb + ((db * inv + 127u) / 255u));
    return (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

} // namespace

std::vector<std::string> splitBriefingParagraphs(std::string_view description) {
    const std::string_view trimmedAll = trimAsciiWs(description);
    if (trimmedAll.empty()) return {};

    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= description.size()) {
        const std::size_t tilde = description.find('~', pos);
        const std::string_view seg = (tilde == std::string_view::npos)
            ? description.substr(pos)
            : description.substr(pos, tilde - pos);
        out.emplace_back(trimAsciiWs(seg));
        if (tilde == std::string_view::npos) break;
        pos = tilde + 1u;
    }
    return out;
}

std::vector<std::int16_t> decodeApmAudio(std::span<const std::uint8_t> apmBytes) {
    if (apmBytes.size() < 6u || apmBytes[0] != '2' || apmBytes[1] != 'D') {
        return {};
    }
    const std::uint32_t declaredBytes = readU32(apmBytes, 2u);
    const std::size_t payloadBytes = std::min<std::size_t>(declaredBytes, apmBytes.size() - 6u);
    if (payloadBytes == 0u) return {};

    std::vector<std::int16_t> pcm;
    pcm.reserve(payloadBytes * 2u);

    int predictor = 0;
    int stepIdx = 0;
    const auto decodeNibble = [&](unsigned nib) {
        int step = kOkiStepTable[static_cast<std::size_t>(stepIdx)];
        int diff = 0;
        if (nib & 4u) diff = step;
        step >>= 1;
        if (nib & 2u) diff += step;
        if (nib & 1u) {
            step >>= 1;
            diff += step;
        }
        if (nib & 8u) diff = -diff;
        predictor += diff;

        const int idxDelta = kOkiIndexTable[nib & 0x0Fu];
        if (idxDelta < 0) {
            if (stepIdx > 0) stepIdx += idxDelta;
            if (stepIdx < 0) stepIdx = 0;
        } else {
            stepIdx += idxDelta;
            if (stepIdx > 48) stepIdx = 48;
        }
        pcm.push_back(static_cast<std::int16_t>(std::clamp(predictor, -32768, 32767)));
    };

    const auto payload = apmBytes.subspan(6u, payloadBytes);
    for (const std::uint8_t byte : payload) {
        decodeNibble(static_cast<unsigned>(byte >> 4u) & 0x0Fu);
        decodeNibble(static_cast<unsigned>(byte) & 0x0Fu);
    }
    return pcm;
}

bool parseBriefingOff(std::span<const std::uint8_t> offBytes,
                      std::size_t datSize,
                      std::vector<BriefingLayer>& outLayers,
                      std::vector<BriefingFrame>& outFrames) {
    outLayers.clear();
    outFrames.clear();
    if (offBytes.empty() || datSize == 0u) return false;

    const std::string_view text(reinterpret_cast<const char*>(offBytes.data()), offBytes.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
            ? text.substr(pos)
            : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : (nl + 1u);
        line = trimAsciiWs(line);
        if (line.empty()) continue;

        std::uint32_t subIndex = 0;
        std::uint32_t dataOffset = 0;
        std::uint32_t colorRef = 0;
        std::uint32_t alphaRaw = 0;
        if (!parseU32Token(line, subIndex) ||
            !parseU32Token(line, dataOffset) ||
            !parseU32Token(line, colorRef) ||
            !parseU32Token(line, alphaRaw)) {
            continue;
        }
        if (dataOffset >= datSize) continue;

        BriefingLayer layer;
        layer.subIndex = subIndex;
        layer.dataOffset = dataOffset;
        layer.r = static_cast<std::uint8_t>(colorRef & 0xFFu);
        layer.g = static_cast<std::uint8_t>((colorRef >> 8u) & 0xFFu);
        layer.b = static_cast<std::uint8_t>((colorRef >> 16u) & 0xFFu);
        layer.alpha = static_cast<std::uint8_t>(std::min<std::uint32_t>(alphaRaw, 255u));

        const std::uint32_t layerIdx = static_cast<std::uint32_t>(outLayers.size());
        outLayers.push_back(layer);
        if (outFrames.empty() || layer.subIndex == 0u) {
            outFrames.push_back(BriefingFrame{layerIdx, 1u});
        } else {
            ++outFrames.back().layerCount;
        }
    }
    return !outFrames.empty();
}

void renderBriefingFrameArgb32(const BriefingPart& part,
                               std::size_t frameIndex,
                               std::uint32_t* argb32Dst,
                               int dstWidth,
                               int dstHeight,
                               int dstStridePixels) {
    if (!argb32Dst || dstWidth <= 0 || dstHeight <= 0 || dstStridePixels < dstWidth) return;
    for (int y = 0; y < dstHeight; ++y) {
        std::memset(argb32Dst + std::size_t(y) * std::size_t(dstStridePixels), 0,
                    std::size_t(dstWidth) * sizeof(std::uint32_t));
    }
    if (frameIndex >= part.frames.size() || part.dat.empty()) return;

    const auto& frame = part.frames[frameIndex];
    const std::span<const std::uint8_t> dat(part.dat);
    std::array<std::uint32_t, 256> lut{};

    for (std::uint32_t li = 0; li < frame.layerCount; ++li) {
        const std::size_t layerIdx = std::size_t(frame.firstLayer) + std::size_t(li);
        if (layerIdx >= part.layers.size()) break;
        const auto& layer = part.layers[layerIdx];
        if (layer.alpha == 0u || layer.dataOffset + 9u > dat.size()) continue;

        const std::size_t off = layer.dataOffset;
        const int left = static_cast<int>(static_cast<std::int16_t>(readU16(dat, off)));
        const int top = static_cast<int>(static_cast<std::int16_t>(readU16(dat, off + 2u)));
        const int w = static_cast<int>(readU16(dat, off + 4u));
        const int h = static_cast<int>(readU16(dat, off + 6u));
        const std::uint8_t fmt = dat[off + 8u];
        if (fmt != 0xA1u || w < 1 || h < 1 || w > 2048 || h > 2048) continue;

        lut[0] = 0u;
        for (std::uint32_t mask = 1u; mask < 256u; ++mask) {
            const std::uint32_t sa = (mask * static_cast<std::uint32_t>(layer.alpha) + 127u) / 255u;
            if (sa == 0u) {
                lut[mask] = 0u;
            } else {
                const std::uint32_t sr = (static_cast<std::uint32_t>(layer.r) * sa + 127u) / 255u;
                const std::uint32_t sg = (static_cast<std::uint32_t>(layer.g) * sa + 127u) / 255u;
                const std::uint32_t sb = (static_cast<std::uint32_t>(layer.b) * sa + 127u) / 255u;
                lut[mask] = (sa << 24) | (sr << 16) | (sg << 8) | sb;
            }
        }

        std::size_t pos = off + 9u;
        for (int y = 0; y < h; ++y) {
            if (pos + 2u > dat.size()) break;
            const std::size_t rowLen = readU16(dat, pos);
            pos += 2u;
            if (rowLen > dat.size() - pos) break;
            const std::size_t rowEnd = pos + rowLen;
            const int dy = top + y;
            if (dy >= 0 && dy < dstHeight) {
                auto* dstRow = argb32Dst + std::size_t(dy) * std::size_t(dstStridePixels);
                int x = 0;
                while (pos < rowEnd) {
                    const std::uint8_t op = dat[pos++];
                    const int count = static_cast<int>(op & 127u);
                    const bool repeat = (op & 128u) != 0u;
                    const std::size_t used = (count == 0) ? 0u : (repeat ? 1u : static_cast<std::size_t>(count));
                    if (used > rowEnd - pos || count > w - x) break;

                    if (repeat && count > 0) {
                        const std::uint8_t mask = dat[pos++];
                        const std::uint32_t src = lut[mask];
                        if (src != 0u) {
                            const int xStart = std::max(0, left + x);
                            const int xEnd = std::min(dstWidth, left + x + count);
                            for (int dx = xStart; dx < xEnd; ++dx) {
                                dstRow[dx] = compositeOverArgb32(dstRow[dx], src);
                            }
                        }
                    } else if (!repeat && count > 0) {
                        for (int j = 0; j < count; ++j) {
                            const std::uint8_t mask = dat[pos + static_cast<std::size_t>(j)];
                            if (mask == 0u) continue;
                            const std::uint32_t src = lut[mask];
                            if (src == 0u) continue;
                            const int dx = left + x + j;
                            if (dx >= 0 && dx < dstWidth) {
                                dstRow[dx] = compositeOverArgb32(dstRow[dx], src);
                            }
                        }
                        pos += static_cast<std::size_t>(count);
                    }
                    x += count;
                }
            }
            pos = rowEnd;
        }
    }
}

MissionBriefingMedia readMissionBriefingMedia(const MissionArchive& archive) {
    MissionBriefingMedia media;
    if (const auto desc = archive.findMissionRecord("I-DESC")) {
        const std::string decoded = decodeMissionText(cstrSpanView(*desc));
        media.paragraphs = splitBriefingParagraphs(decoded);
    }
    if (const auto ovSnd = archive.findMissionRecord("I-BRF-SND"); ovSnd && !ovSnd->empty()) {
        media.overviewVoicePcm = decodeApmAudio(*ovSnd);
    }

    for (unsigned p = 1u; p <= 4u; ++p) {
        const std::string prefix = "I-BRF" + std::to_string(p);
        const auto datRec = archive.findMissionRecord(prefix + "DAT");
        const auto offRec = archive.findMissionRecord(prefix + "OFF");
        if (!datRec || datRec->empty() || !offRec || offRec->empty()) continue;

        BriefingPart part;
        part.dat.assign(datRec->begin(), datRec->end());
        if (!parseBriefingOff(*offRec, part.dat.size(), part.layers, part.frames)) continue;

        if (const auto sndRec = archive.findMissionRecord(prefix + "SND"); sndRec && !sndRec->empty()) {
            part.voicePcm = decodeApmAudio(*sndRec);
        }
        media.parts.push_back(std::move(part));
    }
    return media;
}

MissionBriefingMedia readMissionBriefingMedia(const std::filesystem::path& missionPath) {
    return readMissionBriefingMedia(loadMissionArchive(missionPath));
}

} // namespace ss2::assets
