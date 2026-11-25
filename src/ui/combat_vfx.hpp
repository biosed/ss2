#pragma once
#include "render/window.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ss2 {

enum class CombatVfxClip : std::size_t {
    ExplosionTiny = 0,   // a_v50.ani: grenade / mortar / AP mine / light shell
    ExplosionSmall,      // a_v36.ani: light cannon / rocket / strafe explosive
    ExplosionMedium,     // a_v30.ani: medium tank shell / howitzer
    ExplosionLarge,      // a_v24.ani: AT mine / tank cook-off / heavy shell
    ExplosionHuge,       // a_v16.ani (fallback a_v14.ani): aerial bomb / building demolition
    CrateBlast1,         // BOOMBOX1.ANI: crate / ammo fireball 1
    CrateBlast2,         // BOOMBOX2.ANI: crate / ammo fireball 2
    CrateBlast3,         // BOOMBOX3.ANI: crate / ammo fireball 3
    BoxDust,             // BOXDUST.ANI: parachute / crate landing & shockwave dust
    GroundDust,          // a_pil.ani: bullet / shell ground dust puff
    MuzzleRifle,         // a_vistrel.ani (32 dirs): infantry rifle / SMG / MG muzzle flash
    MuzzleCannon,        // a_vistr0.ani (32 dirs): tank / AT gun / AA cannon muzzle blast
    MuzzleHowitzer,      // a_gaub0.ani (32 dirs): artillery / howitzer muzzle blast & smoke
    MuzzleKatyusha,      // a_katushka.ani (16 dirs): rocket artillery launch blast & backblast
    MuzzleMortar,        // MORTFIRE.ANI: mortar tube flash & smoke
    RocketPlume,         // a_raket.ani: rocket exhaust plume
    FlamePlume,          // FIREBOT.ANI: flamethrower / Molotov / burning wreck flame
    FireGlow,            // a_fireglow.ani: burning ember & smoke glow
    WaterSplash1,        // a_fontanchik.ani: bullet / light water splash
    WaterSplash2,        // a_fontanchik2.ani: shell water splash
    WaterSplash3,        // a_fontanchik3.ani: heavy shell / bomb water column
    TrackTank,           // a_sledtank.ani: tracked vehicle tread mark / amphibious wake
    TrackWheel1,         // a_sled1.ani: wheeled vehicle track / dust 1
    TrackWheel2,         // a_sled2.ani: wheeled vehicle track / dust 2
    TrackWheel3,         // a_sled3.ani: wheeled vehicle track / dust 3
    WakeShallows,        // a_slednavode.ani: shallow water wading ripple
    WakeBoat,            // a_sledkater.ani: naval boat / landing craft wake
    ParachuteCanopy,     // PARA.PCK + PARA.COL: 21 frames (0..7 open, 8 full, 9..20 collapse)
    ParachuteShadow,     // PARA.PCK shadow frames (0..20, tag 0xa3)
    AmmoCrate,           // RArt.rs2 ani 9 + SOLD.PL (American SoldClr): UNITS\ammo.box
    PartsCrate,          // RArt.rs2 ani 9 + SOLD.PL (German SoldClr): UNITS\spare.parts
    Custom,              // Synthetic / caller-supplied .ANI test slot
    Count
};

namespace combat_vfx_detail {

using ByteView = std::span<const std::uint8_t>;

constexpr unsigned char toLowerAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

inline bool iequalsAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (toLowerAscii(static_cast<unsigned char>(a[i])) !=
            toLowerAscii(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline std::uint16_t readU16(ByteView v, std::size_t pos) {
    if (pos > v.size() || v.size() - pos < 2u) {
        throw std::runtime_error("Truncated VFX binary word");
    }
    return static_cast<std::uint16_t>(unsigned(v[pos]) | (unsigned(v[pos + 1u]) << 8u));
}

inline std::int16_t readI16(ByteView v, std::size_t pos) {
    return static_cast<std::int16_t>(readU16(v, pos));
}

inline std::uint32_t readU32(ByteView v, std::size_t pos) {
    if (pos > v.size() || v.size() - pos < 4u) {
        throw std::runtime_error("Truncated VFX binary dword");
    }
    return std::uint32_t(v[pos]) | (std::uint32_t(v[pos + 1u]) << 8u) |
           (std::uint32_t(v[pos + 2u]) << 16u) | (std::uint32_t(v[pos + 3u]) << 24u);
}

inline std::filesystem::path resolveCaseInsensitive(const std::filesystem::path& base,
                                                    std::string_view leaf) {
    const auto direct = base / leaf;
    std::error_code ec;
    if (std::filesystem::exists(direct, ec)) return direct;
    ec.clear();
    if (!std::filesystem::is_directory(base, ec) || ec) return direct;
    std::filesystem::directory_iterator it(
        base, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (iequalsAscii(name, leaf)) return it->path();
    }
    return direct;
}

// Self-contained RFC-1950 / RFC-1951 zlib inflater so combat_vfx.hpp remains 100%
// header-only without requiring extra link libraries in lightweight render tests.
struct HuffTable {
    static constexpr unsigned kMaxBits = 15u;
    std::array<unsigned, kMaxBits + 1u> count{};
    std::array<unsigned, kMaxBits + 1u> firstCode{};
    std::array<unsigned, kMaxBits + 1u> firstSym{};
    std::array<std::uint16_t, 320u> symbols{};

    bool build(std::span<const std::uint8_t> lengths) noexcept {
        count.fill(0u);
        for (const std::uint8_t len : lengths) {
            if (len > kMaxBits) return false;
            if (len > 0u) ++count[len];
        }
        unsigned code = 0u;
        unsigned symOff = 0u;
        for (unsigned bits = 1u; bits <= kMaxBits; ++bits) {
            code = (code + count[bits - 1u]) << 1u;
            if (code + count[bits] > (1u << bits)) return false;
            firstCode[bits] = code;
            firstSym[bits] = symOff;
            symOff += count[bits];
        }
        if (symOff > symbols.size()) return false;
        auto nextIdx = firstSym;
        for (std::size_t sym = 0u; sym < lengths.size(); ++sym) {
            const unsigned len = lengths[sym];
            if (len > 0u) {
                symbols[nextIdx[len]++] = static_cast<std::uint16_t>(sym);
            }
        }
        return true;
    }
};

inline bool inflateDeflateBlocks(ByteView src, std::size_t startPos, std::size_t payloadEnd,
                                 std::size_t maxOut, bool exactSize,
                                 std::vector<std::uint8_t>& out) {
    out.clear();
    if (startPos > payloadEnd || payloadEnd > src.size()) return false;
    if (exactSize) {
        if (maxOut == 0u) return true;
        out.reserve(maxOut);
    }

    std::size_t bytePos = startPos;
    std::uint64_t bitBuf = 0u;
    unsigned bitCount = 0u;

    const auto ensureBits = [&](unsigned need) noexcept -> bool {
        while (bitCount < need) {
            if (bytePos >= payloadEnd) return false;
            bitBuf |= std::uint64_t(src[bytePos++]) << bitCount;
            bitCount += 8u;
        }
        return true;
    };
    const auto readBits = [&](unsigned n, unsigned& dst) noexcept -> bool {
        if (n == 0u) {
            dst = 0u;
            return true;
        }
        if (!ensureBits(n)) return false;
        dst = static_cast<unsigned>(bitBuf & ((1ull << n) - 1ull));
        bitBuf >>= n;
        bitCount -= n;
        return true;
    };
    const auto decodeSymbol = [&](const HuffTable& ht, unsigned& sym) noexcept -> bool {
        unsigned code = 0u;
        for (unsigned bits = 1u; bits <= HuffTable::kMaxBits; ++bits) {
            if (!ensureBits(1u)) return false;
            code = (code << 1u) | static_cast<unsigned>(bitBuf & 1ull);
            bitBuf >>= 1u;
            --bitCount;
            const unsigned cnt = ht.count[bits];
            const unsigned first = ht.firstCode[bits];
            if (code >= first && code - first < cnt) {
                sym = ht.symbols[ht.firstSym[bits] + (code - first)];
                return true;
            }
        }
        return false;
    };

    static constexpr std::array<unsigned, 29> kLenBase{
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static constexpr std::array<unsigned, 29> kLenExtra{
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static constexpr std::array<unsigned, 30> kDistBase{
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static constexpr std::array<unsigned, 30> kDistExtra{
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static constexpr std::array<std::uint8_t, 19> kCodeLenOrder{
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    HuffTable litTable{};
    HuffTable distTable{};
    unsigned bfinal = 0u;
    while (bfinal == 0u) {
        unsigned btype = 0u;
        if (!readBits(1u, bfinal) || !readBits(2u, btype)) return false;
        if (btype == 0u) {
            const unsigned rem = bitCount & 7u;
            bitBuf >>= rem;
            bitCount -= rem;
            unsigned len = 0u;
            unsigned nlen = 0u;
            if (!readBits(16u, len) || !readBits(16u, nlen)) return false;
            if ((len ^ 0xFFFFu) != nlen) return false;
            if (out.size() + len > maxOut) return false;
            for (unsigned i = 0u; i < len; ++i) {
                unsigned b = 0u;
                if (!readBits(8u, b)) return false;
                out.push_back(static_cast<std::uint8_t>(b));
            }
            continue;
        }
        if (btype == 1u) {
            std::array<std::uint8_t, 288> litLens{};
            for (std::size_t i = 0u; i < 144u; ++i) litLens[i] = 8u;
            for (std::size_t i = 144u; i < 256u; ++i) litLens[i] = 9u;
            for (std::size_t i = 256u; i < 280u; ++i) litLens[i] = 7u;
            for (std::size_t i = 280u; i < 288u; ++i) litLens[i] = 8u;
            std::array<std::uint8_t, 32> distLens{};
            distLens.fill(5u);
            if (!litTable.build(litLens) || !distTable.build(distLens)) return false;
        } else if (btype == 2u) {
            unsigned hlit = 0u, hdist = 0u, hclen = 0u;
            if (!readBits(5u, hlit) || !readBits(5u, hdist) || !readBits(4u, hclen)) return false;
            hlit += 257u;
            hdist += 1u;
            hclen += 4u;
            if (hlit > 286u || hdist > 30u) return false;
            std::array<std::uint8_t, 19> clLens{};
            for (unsigned i = 0u; i < hclen; ++i) {
                unsigned v = 0u;
                if (!readBits(3u, v)) return false;
                clLens[kCodeLenOrder[i]] = static_cast<std::uint8_t>(v);
            }
            HuffTable clTable{};
            if (!clTable.build(clLens)) return false;
            std::array<std::uint8_t, 320> combined{};
            const unsigned total = hlit + hdist;
            unsigned idx = 0u;
            while (idx < total) {
                unsigned sym = 0u;
                if (!decodeSymbol(clTable, sym)) return false;
                if (sym < 16u) {
                    combined[idx++] = static_cast<std::uint8_t>(sym);
                } else if (sym == 16u) {
                    if (idx == 0u) return false;
                    unsigned rep = 0u;
                    if (!readBits(2u, rep)) return false;
                    rep += 3u;
                    if (idx + rep > total) return false;
                    const std::uint8_t prev = combined[idx - 1u];
                    for (unsigned r = 0u; r < rep; ++r) combined[idx++] = prev;
                } else if (sym == 17u || sym == 18u) {
                    unsigned rep = 0u;
                    if (!readBits(sym == 17u ? 3u : 7u, rep)) return false;
                    rep += (sym == 17u ? 3u : 11u);
                    if (idx + rep > total) return false;
                    for (unsigned r = 0u; r < rep; ++r) combined[idx++] = 0u;
                } else {
                    return false;
                }
            }
            if (!litTable.build(std::span<const std::uint8_t>(combined.data(), hlit)) ||
                !distTable.build(std::span<const std::uint8_t>(combined.data() + hlit, hdist))) {
                return false;
            }
        } else {
            return false;
        }

        for (;;) {
            unsigned sym = 0u;
            if (!decodeSymbol(litTable, sym)) return false;
            if (sym < 256u) {
                if (out.size() >= maxOut) return false;
                out.push_back(static_cast<std::uint8_t>(sym));
            } else if (sym == 256u) {
                break;
            } else {
                const unsigned li = sym - 257u;
                if (li >= kLenBase.size()) return false;
                unsigned extraL = 0u;
                if (!readBits(kLenExtra[li], extraL)) return false;
                const unsigned matchLen = kLenBase[li] + extraL;

                unsigned dsym = 0u;
                if (!decodeSymbol(distTable, dsym) || dsym >= kDistBase.size()) return false;
                unsigned extraD = 0u;
                if (!readBits(kDistExtra[dsym], extraD)) return false;
                const std::size_t dist = kDistBase[dsym] + extraD;
                if (dist == 0u || dist > out.size() || out.size() + matchLen > maxOut) {
                    return false;
                }
                for (unsigned k = 0u; k < matchLen; ++k) {
                    out.push_back(out[out.size() - dist]);
                }
            }
        }
    }
    return exactSize ? (out.size() == maxOut) : true;
}

inline bool inflateZlibExact(ByteView src, std::size_t expectedSize,
                             std::vector<std::uint8_t>& out) {
    out.clear();
    if (expectedSize == 0u) return true;
    if (src.size() < 6u) return false;
    const unsigned cmf = src[0];
    const unsigned flg = src[1];
    if ((cmf & 0x0Fu) != 8u || ((cmf >> 4u) > 7u) || (((cmf << 8u) | flg) % 31u) != 0u ||
        (flg & 0x20u) != 0u) {
        return false;
    }
    return inflateDeflateBlocks(src, 2u, src.size() - 4u, expectedSize, true, out);
}

inline bool inflateGzipStream(ByteView src, std::vector<std::uint8_t>& out) {
    out.clear();
    if (src.size() < 18u || src[0] != 0x1Fu || src[1] != 0x8Bu || src[2] != 8u) return false;
    const unsigned flg = src[3];
    if ((flg & 0xE0u) != 0u) return false;
    std::size_t pos = 10u;
    const std::size_t payloadEnd = src.size() - 8u;
    if (flg & 0x04u) { // FEXTRA
        if (pos + 2u > payloadEnd) return false;
        const std::size_t xlen = std::size_t(src[pos]) | (std::size_t(src[pos + 1u]) << 8u);
        pos += 2u + xlen;
        if (pos > payloadEnd) return false;
    }
    auto skipCStr = [&]() -> bool {
        while (pos < payloadEnd && src[pos] != 0u) ++pos;
        if (pos >= payloadEnd) return false;
        ++pos;
        return true;
    };
    if ((flg & 0x08u) && !skipCStr()) return false; // FNAME
    if ((flg & 0x10u) && !skipCStr()) return false; // FCOMMENT
    if (flg & 0x02u) { // FHCRC
        if (pos + 2u > payloadEnd) return false;
        pos += 2u;
    }
    constexpr std::size_t kMaxAsset = 32u << 20u;
    return inflateDeflateBlocks(src, pos, payloadEnd, kMaxAsset, false, out);
}

class SueArchiveReader {
public:
    bool open(const std::filesystem::path& archivePath) {
        std::error_code ec;
        if (std::filesystem::is_directory(archivePath, ec) && !ec) {
            dirRoot_ = archivePath;
            isDir_ = true;
            valid_ = true;
            return true;
        }
        file_.open(archivePath, std::ios::binary);
        if (!file_) return false;
        std::array<std::uint8_t, 8> hdr{};
        if (!readBytes(hdr.data(), hdr.size())) return false;
        if (readU32(hdr, 0u) != 0x53465A46u) return false;
        indexStart_ = 8ull + std::size_t(readU32(hdr, 4u));
        file_.seekg(static_cast<std::streamoff>(indexStart_));
        if (!file_) return false;
        std::vector<std::uint8_t> names;
        if (!readTable(20u, items_) || !readTable(0u, names) || !readTable(4u, offsets_)) {
            return false;
        }
        if ((items_.size() % 20u) != 0u || (offsets_.size() % 4u) != 0u) return false;
        lowerPos_.reserve(items_.size() / 20u + 1u);
        for (std::size_t pos = 0u; pos < items_.size(); pos += 20u) {
            const std::size_t nOff = readU32(ByteView(items_).subspan(pos, 20u), 0u);
            if (nOff >= names.size()) return false;
            const auto* startPtr = reinterpret_cast<const char*>(names.data() + nOff);
            const auto* nul = static_cast<const char*>(std::memchr(startPtr, '\0', names.size() - nOff));
            if (!nul) continue;
            std::string key(startPtr, static_cast<std::size_t>(nul - startPtr));
            for (char& ch : key) ch = static_cast<char>(toLowerAscii(static_cast<unsigned char>(ch)));
            lowerPos_.try_emplace(std::move(key), pos);
        }
        valid_ = true;
        return true;
    }

    bool valid() const noexcept { return valid_; }

    std::optional<std::vector<std::uint8_t>> readEntry(std::string_view entry) {
        if (!valid_) return std::nullopt;
        if (isDir_) {
            return readDirEntry(entry);
        }
        std::string key(entry);
        for (char& ch : key) ch = static_cast<char>(toLowerAscii(static_cast<unsigned char>(ch)));
        const auto it = lowerPos_.find(key);
        if (it == lowerPos_.end()) return std::nullopt;
        const ByteView info = ByteView(items_).subspan(it->second, 20u);
        const std::size_t kind = readU32(info, 4u);
        const std::size_t first = readU32(info, 8u);
        const std::size_t size = readU32(info, 12u);
        constexpr std::size_t kMaxAsset = 32u << 20u;
        if (size > kMaxAsset) return std::nullopt;
        if (kind == 2u) {
            return readPayload(first, size);
        }
        if (kind != 8u) return std::nullopt;
        const std::size_t chunk = readU32(info, 16u);
        if (chunk == 0u) return std::nullopt;
        const std::size_t count = (size + chunk - 1u) / chunk;
        if (first >= offsets_.size() / 4u || count >= offsets_.size() / 4u - first) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> out;
        out.reserve(size);
        std::vector<std::uint8_t> piece;
        for (std::size_t i = 0u; i < count; ++i) {
            const std::size_t s = readU32(offsets_, 4u * (first + i));
            const std::size_t e = readU32(offsets_, 4u * (first + i + 1u));
            const std::size_t expected = std::min(chunk, size - out.size());
            if (e < s) return std::nullopt;
            const auto comp = readPayload(s, e - s);
            if (!comp || !inflateZlibExact(*comp, expected, piece)) return std::nullopt;
            out.insert(out.end(), piece.begin(), piece.end());
        }
        return out;
    }

private:
    bool readBytes(std::uint8_t* dst, std::size_t len) {
        if (len == 0u) return true;
        return static_cast<bool>(file_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len)));
    }

    std::optional<std::vector<std::uint8_t>> readPayload(std::size_t start, std::size_t len) {
        if (start < 8u || start > indexStart_ || len > indexStart_ - start) return std::nullopt;
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(start));
        std::vector<std::uint8_t> buf(len);
        if (!readBytes(buf.data(), len)) return std::nullopt;
        return buf;
    }

    bool readTable(unsigned expectedWidth, std::vector<std::uint8_t>& out) {
        std::array<std::uint8_t, 12> hdr{};
        if (!readBytes(hdr.data(), hdr.size())) return false;
        const std::size_t packed = readU32(hdr, 0u);
        const std::uint64_t count = readU32(hdr, 4u);
        const std::uint64_t width = readU32(hdr, 8u);
        if (expectedWidth != 0u && width != expectedWidth) return false;
        const std::uint64_t total = count * width;
        constexpr std::uint64_t kMaxTable = 16ull << 20u;
        if (total > kMaxTable || packed > kMaxTable) return false;
        std::vector<std::uint8_t> comp(packed);
        if (!readBytes(comp.data(), packed)) return false;
        return inflateZlibExact(comp, static_cast<std::size_t>(total), out);
    }

    std::optional<std::vector<std::uint8_t>> readDirEntry(std::string_view entry) const {
        std::filesystem::path cur = dirRoot_;
        std::size_t pos = 0u;
        bool hasSeg = false;
        while (pos <= entry.size()) {
            const std::size_t sep = entry.find_first_of("\\/", pos);
            const std::string_view seg = (sep == std::string_view::npos)
                ? entry.substr(pos) : entry.substr(pos, sep - pos);
            if (seg == "..") return std::nullopt;
            if (!seg.empty() && seg != ".") {
                cur = resolveCaseInsensitive(cur, seg);
                hasSeg = true;
            }
            if (sep == std::string_view::npos) break;
            pos = sep + 1u;
        }
        if (!hasSeg) return std::nullopt;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(cur, ec) || ec) return std::nullopt;
        constexpr std::size_t kMaxAsset = 32u << 20u;
        const auto sz = std::filesystem::file_size(cur, ec);
        if (ec || sz > kMaxAsset) return std::nullopt;
        std::ifstream in(cur, std::ios::binary);
        if (!in) return std::nullopt;
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(sz));
        if (!raw.empty() && !in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
            return std::nullopt;
        }
        if (raw.size() >= 2u && raw[0] == 0x1Fu && raw[1] == 0x8Bu) {
            std::vector<std::uint8_t> out;
            if (!inflateGzipStream(raw, out)) return std::nullopt;
            return out;
        }
        return raw;
    }

    std::filesystem::path dirRoot_;
    bool isDir_{false};
    std::ifstream file_;
    std::size_t indexStart_{0u};
    bool valid_{false};
    std::vector<std::uint8_t> items_;
    std::vector<std::uint8_t> offsets_;
    std::unordered_map<std::string, std::size_t> lowerPos_;
};

class ArchiveChainReader {
public:
    bool openArchives(std::span<const std::filesystem::path> archives) {
        readers_.clear();
        for (const auto& path : archives) {
            if (path.empty()) continue;
            auto reader = std::make_unique<SueArchiveReader>();
            if (reader->open(path)) {
                readers_.push_back(std::move(reader));
            }
        }
        return !readers_.empty();
    }

    std::optional<std::vector<std::uint8_t>> readEntry(std::string_view entry) {
        for (auto& r : readers_) {
            if (auto data = r->readEntry(entry)) return data;
        }
        return std::nullopt;
    }

private:
    std::vector<std::unique_ptr<SueArchiveReader>> readers_;
};

inline std::vector<std::filesystem::path> parseIniGameArchiveChain(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    const auto iniPath = resolveCaseInsensitive(gameDir, "sudtest.ini");
    std::error_code ec;
    if (std::filesystem::is_regular_file(iniPath, ec)) {
        std::ifstream in(iniPath, std::ios::binary);
        bool inData = false;
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            std::size_t first = 0u;
            while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
            const std::string_view sv(line.data() + first, line.size() - first);
            if (sv.empty() || sv.front() == ';' || sv.front() == '#') continue;
            if (sv.front() == '[') {
                const auto rb = sv.find(']');
                if (rb != std::string_view::npos) {
                    inData = iequalsAscii(sv.substr(1u, rb - 1u), "Data");
                }
                continue;
            }
            if (!inData) continue;
            const auto eq = sv.find('=');
            if (eq == std::string_view::npos) continue;
            auto key = sv.substr(0u, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1u);
            if (!iequalsAscii(key, "Game")) continue;
            std::string_view val = sv.substr(eq + 1u);
            std::size_t pos = 0u;
            while (pos <= val.size()) {
                const auto semi = val.find(';', pos);
                auto tok = (semi == std::string_view::npos) ? val.substr(pos) : val.substr(pos, semi - pos);
                while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1u);
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.remove_suffix(1u);
                if (!tok.empty()) {
                    std::filesystem::path cur = gameDir;
                    std::string seg;
                    for (char ch : tok) {
                        if (ch == '\\' || ch == '/') {
                            if (!seg.empty()) {
                                cur = resolveCaseInsensitive(cur, seg);
                                seg.clear();
                            }
                        } else {
                            seg.push_back(ch);
                        }
                    }
                    if (!seg.empty()) cur = resolveCaseInsensitive(cur, seg);
                    if (std::filesystem::exists(cur, ec)) out.push_back(std::move(cur));
                }
                if (semi == std::string_view::npos) break;
                pos = semi + 1u;
            }
            break;
        }
    }
    const auto fallback = resolveCaseInsensitive(gameDir, "game_common.sue");
    if (std::filesystem::exists(fallback, ec) &&
        std::ranges::find(out, fallback) == out.end()) {
        out.push_back(fallback);
    }
    return out;
}

struct DecodedTrimmedSprite {
    int offsetX{0};
    int offsetY{0};
    unsigned width{0u};
    unsigned height{0u};
    std::vector<std::uint8_t> rgba;
};

// Decodes a single 0xa9 / 0xa1 / 0xa2 / 0xa3 frame slice into a tightly-trimmed
// straight-RGBA8 sprite relative to world anchor (0, 0).
inline std::optional<DecodedTrimmedSprite> decodeSingleSprite(ByteView spr, ByteView pal) {
    if (spr.size() < 9u) return std::nullopt;
    const int dx = readI16(spr, 0u);
    const int dy = readI16(spr, 2u);
    const int w = readI16(spr, 4u);
    const int h = readI16(spr, 6u);
    const std::uint8_t tag = spr[8u];
    if (w < 1 || h < 1 || w > 1024 || h > 1024) return std::nullopt;
    if (tag != 0xA9u && tag != 0xA1u && tag != 0xA2u && tag != 0xA3u) return std::nullopt;

    const bool alpha1024 = (pal.size() >= 1024u) &&
                           (tag == 0xA9u || (tag == 0xA1u && pal[2u] == 0xFEu));
    if (tag == 0xA9u && pal.size() < 1024u) return std::nullopt;
    if ((tag == 0xA1u || tag == 0xA2u) && !alpha1024 && pal.size() < 512u) return std::nullopt;

    std::vector<std::uint8_t> canvas(std::size_t(w) * std::size_t(h) * 4u, 0u);
    int minX = w, minY = h, maxX = -1, maxY = -1;
    std::size_t pos = 9u;

    if (tag == 0xA3u) {
        for (int y = 0; y < h; ++y) {
            if (pos + 2u > spr.size()) return std::nullopt;
            const unsigned rowLen = readU16(spr, pos);
            pos += 2u;
            if (rowLen > spr.size() - pos) return std::nullopt;
            int x = 0;
            for (const std::uint8_t op : spr.subspan(pos, rowLen)) {
                const int cnt = op & 127;
                if (cnt > w - x) return std::nullopt;
                if ((op & 128u) == 0u && cnt > 0) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x + cnt - 1);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                    for (int j = 0; j < cnt; ++j) {
                        auto* p = &canvas[(std::size_t(y) * std::size_t(w) + std::size_t(x + j)) * 4u];
                        p[0] = 0u;
                        p[1] = 16u;
                        p[2] = 41u;
                        p[3] = 120u;
                    }
                }
                x += cnt;
            }
            pos += rowLen;
        }
    } else {
        const bool compact = (tag == 0xA1u || tag == 0xA9u);
        const std::size_t palStride = (pal.size() >= 1024u) ? 4u : 2u;
        for (int y = 0; y < h; ++y) {
            if (pos + 2u > spr.size()) return std::nullopt;
            const unsigned rowLen = readU16(spr, pos);
            pos += 2u;
            if (rowLen > spr.size() - pos) return std::nullopt;
            const ByteView row = spr.subspan(pos, rowLen);
            pos += rowLen;
            int x = 0;
            for (std::size_t i = 0u; i < row.size();) {
                const std::uint8_t op = row[i++];
                const int cnt = op & (compact ? 127 : 63);
                if (cnt > w - x) return std::nullopt;
                const bool skip = !compact && (op & 192u) == 192u;
                const bool repeat = (op & 128u) != 0u && !skip;
                const std::size_t used = (skip || cnt == 0) ? 0u : (repeat ? 1u : std::size_t(cnt));
                if (used > row.size() - i) return std::nullopt;
                for (int j = 0; !skip && j < cnt; ++j) {
                    const std::uint8_t idx = row[i + (repeat ? 0u : std::size_t(j))];
                    if (compact && repeat && idx == 0u) continue;
                    const int px = x + j;
                    auto* p = &canvas[(std::size_t(y) * std::size_t(w) + std::size_t(px)) * 4u];
                    if (alpha1024) {
                        const std::size_t pOff = std::size_t(idx) * 4u;
                        const unsigned bgWeight5 = (pal[pOff + 2u] >> 3u) & 0x1Fu;
                        if (bgWeight5 == 31u) continue;
                        const unsigned premulWord = readU16(pal, pOff);
                        const unsigned pr = (premulWord >> 11u) & 31u;
                        const unsigned pg = (premulWord >> 5u) & 63u;
                        const unsigned pb = premulWord & 31u;
                        const unsigned fgR = (pr << 3u) | (pr >> 2u);
                        const unsigned fgG = (pg << 2u) | (pg >> 4u);
                        const unsigned fgB = (pb << 3u) | (pb >> 2u);
                        const unsigned fgWeight = 32u - bgWeight5;
                        p[0] = static_cast<std::uint8_t>(std::min(255u, (fgR * 32u + fgWeight / 2u) / fgWeight));
                        p[1] = static_cast<std::uint8_t>(std::min(255u, (fgG * 32u + fgWeight / 2u) / fgWeight));
                        p[2] = static_cast<std::uint8_t>(std::min(255u, (fgB * 32u + fgWeight / 2u) / fgWeight));
                        p[3] = static_cast<std::uint8_t>((fgWeight * 255u + 16u) / 32u);
                    } else {
                        const unsigned color = readU16(pal, std::size_t(idx) * palStride);
                        const unsigned r = (color >> 11u) & 31u;
                        const unsigned g = (color >> 5u) & 63u;
                        const unsigned b = color & 31u;
                        const bool blend = !compact && (op & 192u) == 64u;
                        p[0] = static_cast<std::uint8_t>((r << 3u) | (r >> 2u));
                        p[1] = static_cast<std::uint8_t>((g << 2u) | (g >> 4u));
                        p[2] = static_cast<std::uint8_t>((b << 3u) | (b >> 2u));
                        p[3] = blend ? 140u : 255u;
                    }
                    if (p[3] > 0u) {
                        minX = std::min(minX, px);
                        maxX = std::max(maxX, px);
                        minY = std::min(minY, y);
                        maxY = std::max(maxY, y);
                    }
                }
                i += used;
                x += cnt;
            }
        }
    }

    if (maxX < minX || maxY < minY) return std::nullopt;
    const unsigned tw = static_cast<unsigned>(maxX - minX + 1);
    const unsigned th = static_cast<unsigned>(maxY - minY + 1);
    DecodedTrimmedSprite out{
        dx + minX,
        dy + minY,
        tw,
        th,
        std::vector<std::uint8_t>(std::size_t(tw) * std::size_t(th) * 4u, 0u)
    };
    for (unsigned ry = 0u; ry < th; ++ry) {
        const auto* srcRow = &canvas[(std::size_t(minY + int(ry)) * std::size_t(w) + std::size_t(minX)) * 4u];
        auto* dstRow = &out.rgba[std::size_t(ry) * std::size_t(tw) * 4u];
        std::memcpy(dstRow, srcRow, std::size_t(tw) * 4u);
    }
    return out;
}

inline std::vector<std::uint8_t> tintSoldPalette(ByteView baseSoldPl, int scaleR, int scaleG, int scaleB) {
    std::vector<std::uint8_t> out(baseSoldPl.begin(), baseSoldPl.begin() + std::min<std::size_t>(512u, baseSoldPl.size()));
    out.resize(512u, 0u);
    if (scaleR < 0) return out;
    for (std::size_t i = 0u; i < 256u; ++i) {
        const unsigned w = unsigned(out[2u * i]) | (unsigned(out[2u * i + 1u]) << 8u);
        const int r = static_cast<int>((w >> 8u) & 0xF8u);
        const int g = static_cast<int>((w >> 3u) & 0xFCu);
        const int b = static_cast<int>((w & 0x1Fu) << 3u);
        const int maxDiff = std::max({std::abs(r - b), std::abs(b - g), std::abs(r - g)});
        const int avg = (r + g + b) / 3;
        if (maxDiff > avg / 5) continue;
        const int nr = std::clamp((avg * scaleR) / 250, 0, 255);
        const int ng = std::clamp((avg * scaleG) / 250, 0, 255);
        const int nb = std::clamp((avg * scaleB) / 250, 0, 255);
        const auto nw = static_cast<std::uint16_t>(
            ((nr & 0xF8) << 8) | ((ng & 0xFC) << 3) | ((nb >> 3) & 0x1F));
        out[2u * i] = static_cast<std::uint8_t>(nw & 0xFFu);
        out[2u * i + 1u] = static_cast<std::uint8_t>((nw >> 8u) & 0xFFu);
    }
    return out;
}

} // namespace combat_vfx_detail

class CombatVfxBank {
public:
    static constexpr unsigned kMaxAtlasDim = 2048u;

    struct PackedFrame {
        std::uint16_t u0{0}, v0{0}, u1{0}, v1{0};
        std::int16_t offsetX{0}, offsetY{0};
    };

    CombatVfxBank() {
        buildProceduralFallback();
    }

    bool loadFromGameDir(const std::filesystem::path& gameDir) {
        const auto archives = combat_vfx_detail::parseIniGameArchiveChain(gameDir);
        return loadFromArchives(archives);
    }

    bool loadFromArchives(std::span<const std::filesystem::path> archives) {
        clearUnpacked();
        retailLoaded_ = false;
        combat_vfx_detail::ArchiveChainReader chain;
        if (!archives.empty() && chain.openArchives(archives)) {
            loadRetailClips(chain);
        }
        fillMissingClipsWithProcedural();
        return finalizeAtlas() && retailLoaded_;
    }

    void buildProceduralFallback() {
        clearUnpacked();
        retailLoaded_ = false;
        fillMissingClipsWithProcedural();
        (void)finalizeAtlas();
    }

    bool packSyntheticAni(CombatVfxClip clip,
                          std::span<const std::uint8_t> ani,
                          std::span<const std::uint8_t> col,
                          unsigned directions = 1u) {
        const auto ci = static_cast<std::size_t>(clip);
        if (ci >= clips_.size() || directions == 0u) return false;
        RawClip decoded;
        if (!decodeAniIntoRawClip(ani, col, directions, 1u, 0u, 1u, decoded)) {
            return false;
        }
        rawClips_[ci] = std::move(decoded);
        return finalizeAtlas();
    }

    bool hasRetailAssets() const noexcept { return retailLoaded_; }
    bool hasAtlas() const noexcept { return atlas_.width > 0u && atlas_.height > 0u; }
    const assets::MapImage& atlas() const noexcept { return atlas_; }
    std::size_t totalPackedFrames() const noexcept { return totalFrames_; }

    std::size_t clipDirectionCount(CombatVfxClip clip) const noexcept {
        const auto ci = static_cast<std::size_t>(clip);
        return ci < clips_.size() ? clips_[ci].directions.size() : 0u;
    }

    std::size_t clipFrameCount(CombatVfxClip clip, unsigned dir = 0u) const noexcept {
        const auto ci = static_cast<std::size_t>(clip);
        if (ci >= clips_.size() || clips_[ci].directions.empty()) return 0u;
        const auto& dirs = clips_[ci].directions;
        const std::size_t di = dir % dirs.size();
        return dirs[di].size();
    }

    const PackedFrame* sampleFrame(CombatVfxClip clip, unsigned dir32, float phase01) const noexcept {
        const auto ci = static_cast<std::size_t>(clip);
        if (ci >= clips_.size()) return nullptr;
        const auto& dirs = clips_[ci].directions;
        if (dirs.empty()) return nullptr;
        const std::size_t dirCount = dirs.size();
        const std::size_t di = (dirCount == 32u)
            ? (dir32 & 31u)
            : (dirCount == 16u ? (((dir32 + 1u) & 31u) >> 1u)
                               : (dirCount == 8u ? (((dir32 + 2u) & 31u) >> 2u) : (dir32 % dirCount)));
        const auto& frames = dirs[di];
        if (frames.empty()) return nullptr;
        const float clamped = std::clamp(phase01, 0.0f, 0.9999f);
        const auto fi = static_cast<std::size_t>(clamped * static_cast<float>(frames.size()));
        return &frames[std::min(fi, frames.size() - 1u)];
    }

    const PackedFrame* frameAt(CombatVfxClip clip, unsigned dir, std::size_t frameIdx) const noexcept {
        const auto ci = static_cast<std::size_t>(clip);
        if (ci >= clips_.size()) return nullptr;
        const auto& dirs = clips_[ci].directions;
        if (dirs.empty()) return nullptr;
        const auto& frames = dirs[dir % dirs.size()];
        if (frames.empty()) return nullptr;
        return &frames[std::min(frameIdx, frames.size() - 1u)];
    }

private:
    struct RawClip {
        std::vector<std::vector<combat_vfx_detail::DecodedTrimmedSprite>> directions;
    };
    struct PackedClip {
        std::vector<std::vector<PackedFrame>> directions;
    };

    void clearUnpacked() {
        for (auto& rc : rawClips_) rc.directions.clear();
        for (auto& pc : clips_) pc.directions.clear();
        totalFrames_ = 0u;
    }

    static bool decodeAniIntoRawClip(combat_vfx_detail::ByteView ani,
                                     combat_vfx_detail::ByteView col,
                                     unsigned totalDirs,
                                     unsigned dirStep,
                                     unsigned maxFramesPerDir,
                                     unsigned frameStride,
                                     RawClip& out) {
        using namespace combat_vfx_detail;
        if (ani.size() < 16u || totalDirs == 0u || dirStep == 0u || frameStride == 0u) return false;
        const std::uint32_t magic = readU32(ani, 0u);
        if (magic < 0xCA000001u || magic > 0xCA000004u) return false;
        const std::uint32_t count = readU32(ani, 4u);
        if (count == 0u || count > 4096u || count < totalDirs) return false;
        const std::size_t tableEnd = 8u + (std::size_t(count) + 1u) * 4u;
        if (tableEnd > ani.size() || readU32(ani, 8u) != tableEnd) return false;
        for (std::size_t k = 0u; k < count; ++k) {
            const std::uint32_t a = readU32(ani, 8u + k * 4u);
            const std::uint32_t b = readU32(ani, 8u + (k + 1u) * 4u);
            if (b < a || b > ani.size()) return false;
        }

        const unsigned fpd = count / totalDirs;
        if (fpd == 0u) return false;
        const unsigned limitF = (maxFramesPerDir > 0u && maxFramesPerDir < fpd) ? maxFramesPerDir : fpd;
        out.directions.clear();
        std::size_t anyFrames = 0u;
        for (unsigned d = 0u; d < totalDirs; d += dirStep) {
            std::vector<DecodedTrimmedSprite> dirFrames;
            for (unsigned f = 0u; f < limitF; f += frameStride) {
                const std::size_t idx = std::size_t(d) * std::size_t(fpd) + std::size_t(f);
                const std::size_t start = readU32(ani, 8u + idx * 4u);
                const std::size_t end = readU32(ani, 8u + (idx + 1u) * 4u);
                if (auto spr = decodeSingleSprite(ani.subspan(start, end - start), col)) {
                    dirFrames.push_back(std::move(*spr));
                    ++anyFrames;
                }
            }
            out.directions.push_back(std::move(dirFrames));
        }
        return anyFrames > 0u;
    }

    template <class Reader>
    void loadRetailClips(Reader& sue) {
        using namespace combat_vfx_detail;
        struct AniEntrySpec {
            CombatVfxClip clip;
            const char* stem;
            const char* fallbackStem;
            unsigned totalDirs;
            unsigned dirStep;
            unsigned maxFramesPerDir;
            unsigned frameStride;
        };
        static constexpr std::array<AniEntrySpec, 26> kSpecs{{
            {CombatVfxClip::ExplosionTiny,   "a_v50",         nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::ExplosionSmall,  "a_v36",         nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::ExplosionMedium, "a_v30",         nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::ExplosionLarge,  "a_v24",         "a_v18", 1u,  1u, 0u,  3u},
            {CombatVfxClip::ExplosionHuge,   "a_v16",         "a_v14", 1u,  1u, 0u,  3u},
            {CombatVfxClip::CrateBlast1,     "BOOMBOX1",      nullptr, 1u,  1u, 0u,  4u},
            {CombatVfxClip::CrateBlast2,     "BOOMBOX2",      nullptr, 1u,  1u, 0u,  4u},
            {CombatVfxClip::CrateBlast3,     "BOOMBOX3",      nullptr, 1u,  1u, 0u,  4u},
            {CombatVfxClip::BoxDust,         "BOXDUST",       nullptr, 1u,  1u, 0u,  3u},
            {CombatVfxClip::GroundDust,      "a_pil",         nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::MuzzleRifle,     "a_vistrel",     nullptr, 32u, 1u, 0u,  1u},
            {CombatVfxClip::MuzzleCannon,    "a_vistr0",      nullptr, 32u, 1u, 0u,  1u},
            {CombatVfxClip::MuzzleHowitzer,  "a_gaub0",       "a_gaub45", 32u, 1u, 24u, 4u},
            {CombatVfxClip::MuzzleKatyusha,  "a_katushka",    nullptr, 32u, 2u, 0u,  1u},
            {CombatVfxClip::MuzzleMortar,    "MORTFIRE",      nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::RocketPlume,     "a_raket",       nullptr, 1u,  1u, 0u,  4u},
            {CombatVfxClip::FlamePlume,      "FIREBOT",       nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::FireGlow,        "a_fireglow",    nullptr, 1u,  1u, 0u,  3u},
            {CombatVfxClip::WaterSplash1,    "a_fontanchik",  nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::WaterSplash2,    "a_fontanchik2", nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::WaterSplash3,    "a_fontanchik3", nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::TrackTank,       "a_sledtank",    nullptr, 1u,  1u, 0u,  2u},
            {CombatVfxClip::TrackWheel1,     "a_sled1",       nullptr, 1u,  1u, 0u,  4u},
            {CombatVfxClip::TrackWheel2,     "a_sled2",       nullptr, 1u,  1u, 0u,  4u},
            {CombatVfxClip::TrackWheel3,     "a_sled3",       nullptr, 1u,  1u, 0u,  4u},
            {CombatVfxClip::WakeShallows,    "a_slednavode",  nullptr, 1u,  1u, 0u,  1u},
        }};

        std::size_t loadedCount = 0u;
        const auto tryLoadAniSpec = [&](CombatVfxClip clip, const char* stem, unsigned dirs,
                                        unsigned dirStep, unsigned maxFpd, unsigned stride) -> bool {
            if (!stem) return false;
            const auto ani = sue.readEntry(std::string(stem) + ".ani");
            const auto col = sue.readEntry(std::string(stem) + ".col");
            if (!ani || !col) return false;
            RawClip rc;
            if (!decodeAniIntoRawClip(*ani, *col, dirs, dirStep, maxFpd, stride, rc)) return false;
            rawClips_[static_cast<std::size_t>(clip)] = std::move(rc);
            return true;
        };

        for (const auto& s : kSpecs) {
            if (tryLoadAniSpec(s.clip, s.stem, s.totalDirs, s.dirStep, s.maxFramesPerDir, s.frameStride) ||
                tryLoadAniSpec(s.clip, s.fallbackStem, s.totalDirs, s.dirStep, s.maxFramesPerDir, s.frameStride)) {
                ++loadedCount;
            }
        }
        if (tryLoadAniSpec(CombatVfxClip::WakeBoat, "a_sledkater", 1u, 1u, 0u, 2u)) {
            ++loadedCount;
        }

        // Retail PARA.PCK + PARA.COL: u32[0] = 21 canopy frames, u32[1..21] = 0xa2 canopy offsets,
        // u32[22..42] = 0xa3 shadow offsets, u32[43] = end offset.
        if (const auto paraPck = sue.readEntry("PARA.PCK")) {
            if (const auto paraCol = sue.readEntry("PARA.COL");
                paraCol && paraCol->size() >= 512u && paraPck->size() >= 44u * 4u) {
                const std::uint32_t canopyCount = readU32(*paraPck, 0u);
                if (canopyCount >= 9u && canopyCount <= 32u &&
                    paraPck->size() >= (std::size_t(canopyCount) * 2u + 2u) * 4u) {
                    std::vector<DecodedTrimmedSprite> canopies;
                    std::vector<DecodedTrimmedSprite> shadows;
                    for (std::uint32_t i = 0u; i < canopyCount; ++i) {
                        const std::size_t cs = readU32(*paraPck, (1u + i) * 4u);
                        const std::size_t ce = readU32(*paraPck, (2u + i) * 4u);
                        if (cs <= ce && ce <= paraPck->size()) {
                            if (auto sp = decodeSingleSprite(ByteView(*paraPck).subspan(cs, ce - cs), *paraCol)) {
                                canopies.push_back(std::move(*sp));
                            }
                        }
                        const std::size_t ss = readU32(*paraPck, (1u + canopyCount + i) * 4u);
                        const std::size_t se = readU32(*paraPck, (2u + canopyCount + i) * 4u);
                        if (ss <= se && se <= paraPck->size()) {
                            if (auto sh = decodeSingleSprite(ByteView(*paraPck).subspan(ss, se - ss), *paraCol)) {
                                shadows.push_back(std::move(*sh));
                            }
                        }
                    }
                    if (!canopies.empty()) {
                        rawClips_[static_cast<std::size_t>(CombatVfxClip::ParachuteCanopy)].directions = {std::move(canopies)};
                        ++loadedCount;
                    }
                    if (!shadows.empty()) {
                        rawClips_[static_cast<std::size_t>(CombatVfxClip::ParachuteShadow)].directions = {std::move(shadows)};
                    }
                }
            }
        }

        // Retail UNITS\ammo.box & UNITS\spare.parts -> art art (UNITS\art -> RArt.rs2), ani 9 (groups 72..79) + SOLD.PL.
        if (const auto rart = sue.readEntry("RArt.rs2")) {
            if (const auto soldPl = sue.readEntry("SOLD.PL"); soldPl && soldPl->size() >= 512u) {
                const auto usPal = tintSoldPalette(*soldPl, 210, 225, 165);   // American olive drab crate
                const auto gerPal = tintSoldPalette(*soldPl, -1, -1, -1);     // German field-grey crate
                std::vector<std::vector<DecodedTrimmedSprite>> ammoDirs;
                std::vector<std::vector<DecodedTrimmedSprite>> partsDirs;
                ByteView rest(*rart);
                bool ok = true;
                for (unsigned grp = 0u; grp < 80u; ++grp) {
                    if (rest.size() < 8u) { ok = false; break; }
                    const std::uint32_t cnt = readU32(rest, 0u);
                    const std::uint32_t sz = readU32(rest, 4u);
                    rest = rest.subspan(8u);
                    if (cnt > 256u || sz > rest.size() || (cnt > 0u && sz < cnt * 4u)) { ok = false; break; }
                    const ByteView payload = rest.first(sz);
                    rest = rest.subspan(sz);
                    if (grp >= 72u && grp < 80u && cnt > 0u) {
                        const std::size_t begin = readU32(payload, 0u);
                        const std::size_t end = (cnt == 1u) ? sz : readU32(payload, 4u);
                        if (begin <= end && end <= sz) {
                            const ByteView spr = payload.subspan(begin, end - begin);
                            if (auto aSpr = decodeSingleSprite(spr, usPal)) {
                                ammoDirs.push_back({std::move(*aSpr)});
                            }
                            if (auto pSpr = decodeSingleSprite(spr, gerPal)) {
                                partsDirs.push_back({std::move(*pSpr)});
                            }
                        }
                    }
                }
                if (ok && ammoDirs.size() == 8u) {
                    rawClips_[static_cast<std::size_t>(CombatVfxClip::AmmoCrate)].directions = std::move(ammoDirs);
                    ++loadedCount;
                }
                if (ok && partsDirs.size() == 8u) {
                    rawClips_[static_cast<std::size_t>(CombatVfxClip::PartsCrate)].directions = std::move(partsDirs);
                }
            }
        }
        retailLoaded_ = (loadedCount >= 8u);
    }

    static combat_vfx_detail::DecodedTrimmedSprite makeProceduralDisc(
        int radius, std::uint8_t rCore, std::uint8_t gCore, std::uint8_t bCore,
        std::uint8_t rRim, std::uint8_t gRim, std::uint8_t bRim, std::uint8_t maxAlpha,
        int shiftX = 0, int shiftY = 0) {
        const int side = std::max(4, radius * 2);
        combat_vfx_detail::DecodedTrimmedSprite sp{
            -radius + shiftX, -radius + shiftY,
            static_cast<unsigned>(side), static_cast<unsigned>(side),
            std::vector<std::uint8_t>(std::size_t(side) * std::size_t(side) * 4u, 0u)
        };
        const float cx = static_cast<float>(side - 1) * 0.5f;
        const float invR = 1.0f / std::max(1.0f, static_cast<float>(radius));
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const float d = std::hypot(static_cast<float>(x) - cx, static_cast<float>(y) - cx) * invR;
                if (d >= 1.0f) continue;
                const float w = 1.0f - d;
                auto* p = &sp.rgba[(std::size_t(y) * std::size_t(side) + std::size_t(x)) * 4u];
                p[0] = static_cast<std::uint8_t>(std::lround(rRim + (rCore - rRim) * w));
                p[1] = static_cast<std::uint8_t>(std::lround(gRim + (gCore - gRim) * w));
                p[2] = static_cast<std::uint8_t>(std::lround(bRim + (bCore - bRim) * w));
                p[3] = static_cast<std::uint8_t>(std::lround(static_cast<float>(maxAlpha) * w * w));
            }
        }
        return sp;
    }

    static combat_vfx_detail::DecodedTrimmedSprite makeProceduralBox(
        unsigned w, unsigned h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        combat_vfx_detail::DecodedTrimmedSprite sp{
            -static_cast<int>(w / 2u), -static_cast<int>(h / 2u), w, h,
            std::vector<std::uint8_t>(std::size_t(w) * std::size_t(h) * 4u, 0u)
        };
        for (unsigned y = 0u; y < h; ++y) {
            for (unsigned x = 0u; x < w; ++x) {
                const bool border = (x == 0u || y == 0u || x + 1u == w || y + 1u == h || y == h / 2u);
                auto* p = &sp.rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4u];
                p[0] = border ? static_cast<std::uint8_t>(r / 2u) : r;
                p[1] = border ? static_cast<std::uint8_t>(g / 2u) : g;
                p[2] = border ? static_cast<std::uint8_t>(b / 2u) : b;
                p[3] = 255u;
            }
        }
        return sp;
    }

    static combat_vfx_detail::DecodedTrimmedSprite makeProceduralCanopy(int radius, bool shadow) {
        const unsigned w = static_cast<unsigned>(std::max(6, radius * 2));
        const unsigned h = static_cast<unsigned>(std::max(4, radius + 4));
        combat_vfx_detail::DecodedTrimmedSprite sp{
            -static_cast<int>(w / 2u), shadow ? -static_cast<int>(h / 2u) : -static_cast<int>(h),
            w, h, std::vector<std::uint8_t>(std::size_t(w) * std::size_t(h) * 4u, 0u)
        };
        const float cx = static_cast<float>(w - 1u) * 0.5f;
        const float cy = static_cast<float>(h - 1u);
        for (unsigned y = 0u; y < h; ++y) {
            for (unsigned x = 0u; x < w; ++x) {
                const float dx = (static_cast<float>(x) - cx) / std::max(1.0f, static_cast<float>(radius));
                const float dy = (static_cast<float>(y) - cy * (shadow ? 0.5f : 0.85f)) /
                                 std::max(1.0f, static_cast<float>(radius) * (shadow ? 0.45f : 0.75f));
                const float d2 = dx * dx + dy * dy;
                if (d2 <= 1.0f && (shadow || static_cast<float>(y) <= cy * 0.85f)) {
                    auto* p = &sp.rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4u];
                    if (shadow) {
                        p[0] = 0u; p[1] = 16u; p[2] = 41u; p[3] = 110u;
                    } else {
                        const std::uint8_t shade = static_cast<std::uint8_t>(235 - int(d2 * 45.0f));
                        p[0] = shade; p[1] = shade; p[2] = static_cast<std::uint8_t>(shade - 15u); p[3] = 235u;
                    }
                }
            }
        }
        return sp;
    }

    void fillMissingClipsWithProcedural() {
        const auto ensure = [&](CombatVfxClip clip, auto&& builder) {
            auto& rc = rawClips_[static_cast<std::size_t>(clip)];
            if (rc.directions.empty() || rc.directions.front().empty()) {
                rc.directions = builder();
            }
        };
        const auto makeBurst = [](int r0, int r1, std::uint8_t rc, std::uint8_t gc, std::uint8_t bc,
                                  std::uint8_t rr, std::uint8_t gr, std::uint8_t br) {
            std::vector<combat_vfx_detail::DecodedTrimmedSprite> seq;
            for (int f = 0; f < 6; ++f) {
                const float t = static_cast<float>(f) / 5.0f;
                const int rad = r0 + static_cast<int>(std::lround(static_cast<float>(r1 - r0) * t));
                const auto alpha = static_cast<std::uint8_t>(std::lround(245.0f * (1.0f - 0.72f * t)));
                seq.push_back(makeProceduralDisc(rad, rc, gc, bc, rr, gr, br, alpha, 0, -int(t * 6.0f)));
            }
            return std::vector<std::vector<combat_vfx_detail::DecodedTrimmedSprite>>{std::move(seq)};
        };

        ensure(CombatVfxClip::ExplosionTiny,   [&] { return makeBurst(4,  10, 255, 240, 170, 220, 95,  30); });
        ensure(CombatVfxClip::ExplosionSmall,  [&] { return makeBurst(6,  14, 255, 235, 160, 225, 90,  28); });
        ensure(CombatVfxClip::ExplosionMedium, [&] { return makeBurst(8,  18, 255, 235, 150, 210, 80,  25); });
        ensure(CombatVfxClip::ExplosionLarge,  [&] { return makeBurst(10, 24, 255, 240, 160, 200, 75,  22); });
        ensure(CombatVfxClip::ExplosionHuge,   [&] { return makeBurst(12, 30, 255, 245, 180, 195, 70,  20); });
        ensure(CombatVfxClip::CrateBlast1,     [&] { return makeBurst(9,  22, 255, 225, 130, 215, 85,  25); });
        ensure(CombatVfxClip::CrateBlast2,     [&] { return makeBurst(9,  22, 255, 215, 120, 205, 80,  25); });
        ensure(CombatVfxClip::CrateBlast3,     [&] { return makeBurst(9,  22, 255, 230, 140, 220, 90,  30); });
        ensure(CombatVfxClip::BoxDust,         [&] { return makeBurst(6,  16, 190, 175, 145, 120, 110, 90); });
        ensure(CombatVfxClip::GroundDust,      [&] { return makeBurst(3,  9,  180, 165, 135, 115, 105, 85); });
        ensure(CombatVfxClip::MuzzleMortar,    [&] { return makeBurst(4,  10, 255, 235, 170, 160, 150, 140); });
        ensure(CombatVfxClip::RocketPlume,     [&] { return makeBurst(4,  9,  240, 210, 160, 150, 145, 140); });
        ensure(CombatVfxClip::FlamePlume,      [&] { return makeBurst(5,  11, 255, 220, 110, 225, 75,  18); });
        ensure(CombatVfxClip::FireGlow,        [&] { return makeBurst(5,  9,  255, 160, 55,  180, 50,  15); });
        ensure(CombatVfxClip::WaterSplash1,    [&] { return makeBurst(3,  8,  235, 248, 255, 120, 175, 215); });
        ensure(CombatVfxClip::WaterSplash2,    [&] { return makeBurst(5,  11, 240, 250, 255, 115, 170, 215); });
        ensure(CombatVfxClip::WaterSplash3,    [&] { return makeBurst(6,  15, 245, 252, 255, 110, 165, 210); });
        ensure(CombatVfxClip::TrackTank,       [&] { return makeBurst(4,  5,  58,  50,  40,  45,  38,  30); });
        ensure(CombatVfxClip::TrackWheel1,     [&] { return makeBurst(3,  5,  95,  84,  68,  72,  62,  48); });
        ensure(CombatVfxClip::TrackWheel2,     [&] { return makeBurst(3,  5,  92,  82,  66,  70,  60,  46); });
        ensure(CombatVfxClip::TrackWheel3,     [&] { return makeBurst(3,  5,  98,  86,  70,  74,  64,  50); });
        ensure(CombatVfxClip::WakeShallows,    [&] { return makeBurst(4,  8,  215, 235, 248, 130, 175, 205); });
        ensure(CombatVfxClip::WakeBoat,        [&] { return makeBurst(5,  11, 225, 242, 252, 135, 180, 210); });

        const auto makeDirMuzzle = [](unsigned dirs, int rad, int reach) {
            std::vector<std::vector<combat_vfx_detail::DecodedTrimmedSprite>> out(dirs);
            constexpr float kTwoPi = 6.28318530717958647692f;
            for (unsigned d = 0u; d < dirs; ++d) {
                const float a = (static_cast<float>(d) / static_cast<float>(dirs)) * kTwoPi;
                const float du = std::cos(a);
                const float dv = std::sin(a);
                const int sx = static_cast<int>(std::lround((du - dv) * static_cast<float>(reach)));
                const int sy = static_cast<int>(std::lround((du + dv) * 0.5f * static_cast<float>(reach)));
                for (int f = 0; f < 4; ++f) {
                    const int r = std::max(2, rad + (f == 1 ? 2 : 0) - f / 2);
                    const auto al = static_cast<std::uint8_t>(245 - f * 45);
                    out[d].push_back(makeProceduralDisc(r, 255, 245, 185, 240, 125, 35, al, sx, sy));
                }
            }
            return out;
        };
        ensure(CombatVfxClip::MuzzleRifle,    [&] { return makeDirMuzzle(8u,  3, 4); });
        ensure(CombatVfxClip::MuzzleCannon,   [&] { return makeDirMuzzle(16u, 5, 7); });
        ensure(CombatVfxClip::MuzzleHowitzer, [&] { return makeDirMuzzle(16u, 7, 9); });
        ensure(CombatVfxClip::MuzzleKatyusha, [&] { return makeDirMuzzle(8u,  8, 10); });

        ensure(CombatVfxClip::ParachuteCanopy, [&] {
            std::vector<combat_vfx_detail::DecodedTrimmedSprite> seq;
            static constexpr std::array<int, 21> kRadii{
                3, 5, 7, 9, 12, 14, 16, 18, 20,
                18, 16, 15, 13, 12, 10, 9, 8, 7, 6, 5, 4
            };
            for (const int r : kRadii) seq.push_back(makeProceduralCanopy(r, false));
            return std::vector<std::vector<combat_vfx_detail::DecodedTrimmedSprite>>{std::move(seq)};
        });
        ensure(CombatVfxClip::ParachuteShadow, [&] {
            std::vector<combat_vfx_detail::DecodedTrimmedSprite> seq;
            for (int i = 0; i < 21; ++i) seq.push_back(makeProceduralCanopy(8 + (i <= 8 ? i : (20 - i)), true));
            return std::vector<std::vector<combat_vfx_detail::DecodedTrimmedSprite>>{std::move(seq)};
        });
        ensure(CombatVfxClip::AmmoCrate, [&] {
            return std::vector<std::vector<combat_vfx_detail::DecodedTrimmedSprite>>{
                {makeProceduralBox(18u, 13u, 118u, 132u, 82u)}
            };
        });
        ensure(CombatVfxClip::PartsCrate, [&] {
            return std::vector<std::vector<combat_vfx_detail::DecodedTrimmedSprite>>{
                {makeProceduralBox(18u, 13u, 112u, 118u, 122u)}
            };
        });
        ensure(CombatVfxClip::Custom, [&] { return makeBurst(4, 8, 255, 220, 120, 210, 90, 30); });
    }

    bool finalizeAtlas() {
        struct Ref {
            std::size_t clipIdx;
            std::size_t dirIdx;
            std::size_t frameIdx;
            unsigned paddedW;
            unsigned paddedH;
        };
        std::vector<Ref> refs;
        for (std::size_t ci = 0u; ci < rawClips_.size(); ++ci) {
            const auto& dirs = rawClips_[ci].directions;
            clips_[ci].directions.assign(dirs.size(), {});
            for (std::size_t di = 0u; di < dirs.size(); ++di) {
                clips_[ci].directions[di].resize(dirs[di].size());
                for (std::size_t fi = 0u; fi < dirs[di].size(); ++fi) {
                    const auto& sp = dirs[di][fi];
                    if (sp.width == 0u || sp.height == 0u) continue;
                    refs.push_back({ci, di, fi, sp.width + 2u, sp.height + 2u});
                }
            }
        }
        if (refs.empty()) return false;

        std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
            return a.paddedH != b.paddedH ? a.paddedH > b.paddedH : a.paddedW > b.paddedW;
        });

        unsigned atlasW = retailLoaded_ ? kMaxAtlasDim : 512u;
        struct Placement {
            Ref ref;
            unsigned x;
            unsigned y;
        };
        std::vector<Placement> placed;
        placed.reserve(refs.size());

        unsigned curX = 0u, curY = 0u, rowH = 0u;
        for (const auto& r : refs) {
            if (r.paddedW > atlasW) atlasW = kMaxAtlasDim;
            if (curX + r.paddedW > atlasW) {
                curY += rowH;
                curX = 0u;
                rowH = 0u;
            }
            if (curY + r.paddedH > kMaxAtlasDim) {
                if (atlasW < kMaxAtlasDim) {
                    atlasW = kMaxAtlasDim;
                    placed.clear();
                    curX = 0u;
                    curY = 0u;
                    rowH = 0u;
                    for (const auto& r2 : refs) {
                        if (curX + r2.paddedW > atlasW) {
                            curY += rowH;
                            curX = 0u;
                            rowH = 0u;
                        }
                        if (curY + r2.paddedH > kMaxAtlasDim) break;
                        placed.push_back({r2, curX + 1u, curY + 1u});
                        curX += r2.paddedW;
                        rowH = std::max(rowH, r2.paddedH);
                    }
                    break;
                }
                continue;
            }
            placed.push_back({r, curX + 1u, curY + 1u});
            curX += r.paddedW;
            rowH = std::max(rowH, r.paddedH);
        }

        const unsigned usedH = std::clamp(((curY + rowH + 15u) & ~15u), 16u, kMaxAtlasDim);
        atlas_ = assets::MapImage{
            atlasW,
            usedH,
            std::vector<std::uint8_t>(std::size_t(atlasW) * std::size_t(usedH) * 4u, 0u)
        };
        totalFrames_ = placed.size();

        for (const auto& pl : placed) {
            const auto& sp = rawClips_[pl.ref.clipIdx].directions[pl.ref.dirIdx][pl.ref.frameIdx];
            for (unsigned ry = 0u; ry < sp.height; ++ry) {
                const auto* srcRow = &sp.rgba[std::size_t(ry) * std::size_t(sp.width) * 4u];
                auto* dstRow = &atlas_.rgba[(std::size_t(pl.y + ry) * std::size_t(atlasW) + std::size_t(pl.x)) * 4u];
                std::memcpy(dstRow, srcRow, std::size_t(sp.width) * 4u);
            }
            clips_[pl.ref.clipIdx].directions[pl.ref.dirIdx][pl.ref.frameIdx] = PackedFrame{
                static_cast<std::uint16_t>(pl.x),
                static_cast<std::uint16_t>(pl.y),
                static_cast<std::uint16_t>(pl.x + sp.width),
                static_cast<std::uint16_t>(pl.y + sp.height),
                static_cast<std::int16_t>(sp.offsetX),
                static_cast<std::int16_t>(sp.offsetY)
            };
        }
        return true;
    }

    bool retailLoaded_{false};
    assets::MapImage atlas_{};
    std::size_t totalFrames_{0u};
    std::array<RawClip, static_cast<std::size_t>(CombatVfxClip::Count)> rawClips_{};
    std::array<PackedClip, static_cast<std::size_t>(CombatVfxClip::Count)> clips_{};
};

class CombatVfxSystem {
public:
    struct RankedSprite {
        Sprite sprite;
        int sortY{0};
    };

    using FogBrightnessFn = std::function<float(int simX, int simY)>;

    static constexpr std::size_t kMaxActiveAnims = 160u;
    static constexpr std::size_t kMaxDecals = 192u;

    CombatVfxSystem() = default;

    bool load(const std::filesystem::path& gameDir) {
        clearActive();
        return bank_.loadFromGameDir(gameDir);
    }

    bool loadFromArchives(std::span<const std::filesystem::path> archives) {
        clearActive();
        return bank_.loadFromArchives(archives);
    }

    void buildProceduralFallback() {
        clearActive();
        bank_.buildProceduralFallback();
    }

    bool packSyntheticAni(CombatVfxClip clip,
                          std::span<const std::uint8_t> ani,
                          std::span<const std::uint8_t> col,
                          unsigned directions = 1u) {
        return bank_.packSyntheticAni(clip, ani, col, directions);
    }

    const CombatVfxBank& bank() const noexcept { return bank_; }
    bool hasRetailAssets() const noexcept { return bank_.hasRetailAssets(); }
    bool hasAtlas() const noexcept { return bank_.hasAtlas(); }
    const assets::MapImage& atlas() const noexcept { return bank_.atlas(); }

    void bindAtlasSlot(unsigned slot) noexcept { atlasSlot_ = slot; }
    std::optional<unsigned> atlasSlot() const noexcept { return atlasSlot_; }

    bool hasActiveSprites() const noexcept {
        return !anims_.empty() || !decals_.empty() || !drops_.empty();
    }
    std::size_t activeAnimCount() const noexcept { return anims_.size(); }
    std::size_t activeDecalCount() const noexcept { return decals_.size(); }
    std::size_t activeDropCount() const noexcept { return drops_.size(); }

    void clearActive() {
        anims_.clear();
        decals_.clear();
        drops_.clear();
        trackStates_.clear();
        wreckEmitAccum_ = 0.0f;
    }

    // Matches ss2::det::detDir32(dy + 0.5*dx, dy - 0.5*dx) without touching src/sim.
    static unsigned dir32FromMapDelta(double dx, double dy) noexcept {
        const double du = dy + dx * 0.5;
        const double dv = dy - dx * 0.5;
        if (du == 0.0 && dv == 0.0) return 0u;
        constexpr double kTan1 = 0.09849140335716425;
        constexpr double kTan3 = 0.3033466836073424;
        constexpr double kTan5 = 0.5345111359507916;
        constexpr double kTan7 = 0.8206787908286602;
        const double au = std::abs(du);
        const double av = std::abs(dv);
        const bool swap = av > au;
        const double x = swap ? av : au;
        const double y = swap ? au : av;
        unsigned s = 4u;
        if (y < x * kTan1) s = 0u;
        else if (y < x * kTan3) s = 1u;
        else if (y < x * kTan5) s = 2u;
        else if (y < x * kTan7) s = 3u;
        if (swap) s = 8u - s;
        if (du < 0.0) s = (dv < 0.0) ? (16u + s) : (16u - s);
        else if (dv < 0.0) s = 32u - s;
        return s & 31u;
    }

    void spawnClip(CombatVfxClip clip, float simX, float simY, unsigned dir32 = 0u,
                   float durationSec = 0.55f, float scale = 1.0f, float alpha = 1.0f,
                   int sortBias = 200000) {
        if (anims_.size() >= kMaxActiveAnims) anims_.erase(anims_.begin());
        anims_.push_back(ActiveAnim{
            clip, simX, simY, dir32 & 31u, 0.0f, std::max(0.05f, durationSec), scale, alpha, sortBias
        });
    }

    void spawnExplosion(float simX, float simY, int tier, bool onWater = false) {
        if (onWater) {
            spawnWaterSplash(simX, simY, std::clamp(tier + 1, 1, 3));
            if (tier >= 3) {
                spawnWaterSplash(simX - 8.0f, simY + 3.0f, 2);
                spawnWaterSplash(simX + 8.0f, simY - 2.0f, 2);
            }
            return;
        }
        CombatVfxClip clip = CombatVfxClip::ExplosionSmall;
        float dur = 0.62f;
        switch (std::clamp(tier, 0, 4)) {
        case 0: clip = CombatVfxClip::ExplosionTiny;   dur = 0.48f; break;
        case 1: clip = CombatVfxClip::ExplosionSmall;  dur = 0.58f; break;
        case 2: clip = CombatVfxClip::ExplosionMedium; dur = 0.68f; break;
        case 3: clip = CombatVfxClip::ExplosionLarge;  dur = 0.82f; break;
        default: clip = CombatVfxClip::ExplosionHuge;  dur = 0.96f; break;
        }
        spawnClip(clip, simX, simY, 0u, dur, 1.0f, 1.0f, 200000);
        if (tier >= 3) {
            spawnDust(simX, simY + 2.0f, true);
        } else if (tier >= 1) {
            spawnDust(simX, simY + 2.0f, false);
        }
    }

    void spawnCrateBlast(float simX, float simY) {
        const unsigned v = nextVariation() % 3u;
        const CombatVfxClip clip = (v == 0u)
            ? CombatVfxClip::CrateBlast1
            : (v == 1u ? CombatVfxClip::CrateBlast2 : CombatVfxClip::CrateBlast3);
        spawnClip(clip, simX, simY, 0u, 0.85f, 1.0f, 1.0f, 200010);
        spawnDust(simX, simY + 3.0f, true);
    }

    void spawnDust(float simX, float simY, bool heavyBoxDust = false) {
        spawnClip(heavyBoxDust ? CombatVfxClip::BoxDust : CombatVfxClip::GroundDust,
                  simX, simY, 0u, heavyBoxDust ? 0.80f : 0.46f, 1.0f,
                  heavyBoxDust ? 0.90f : 0.82f, 199990);
    }

    void spawnMuzzleFlash(CombatVfxClip muzzleClip, float simX, float simY,
                          unsigned dir32, float barrelOffsetPx = 0.0f) {
        float ax = simX;
        float ay = simY;
        if (barrelOffsetPx != 0.0f) {
            constexpr float kTwoPi = 6.28318530717958647692f;
            const float ang = (static_cast<float>(dir32 & 31u) / 32.0f) * kTwoPi;
            const float du = std::cos(ang);
            const float dv = std::sin(ang);
            ax += (du - dv) * barrelOffsetPx * 0.70710678f;
            ay += (du + dv) * barrelOffsetPx * 0.35355339f;
        }
        float dur = 0.20f;
        if (muzzleClip == CombatVfxClip::MuzzleHowitzer) dur = 0.42f;
        else if (muzzleClip == CombatVfxClip::MuzzleKatyusha) dur = 0.34f;
        else if (muzzleClip == CombatVfxClip::MuzzleMortar) dur = 0.38f;
        else if (muzzleClip == CombatVfxClip::MuzzleRifle) dur = 0.14f;
        spawnClip(muzzleClip, ax, ay, dir32, dur, 1.0f, 1.0f, 200020);
    }

    void spawnRocketPlume(float simX, float simY) {
        spawnClip(CombatVfxClip::RocketPlume, simX, simY, 0u, 0.52f, 0.85f, 0.88f, 200005);
    }

    void spawnFlameJet(float simX0, float simY0, float simX1, float simY1) {
        spawnClip(CombatVfxClip::FlamePlume,
                  simX0 + (simX1 - simX0) * 0.45f,
                  simY0 + (simY1 - simY0) * 0.45f,
                  0u, 0.44f, 0.72f, 0.92f, 200015);
        spawnClip(CombatVfxClip::FlamePlume, simX1, simY1, 0u, 0.58f, 0.95f, 0.96f, 200016);
        spawnClip(CombatVfxClip::FireGlow, simX1, simY1 + 2.0f, 0u, 0.64f, 0.85f, 0.80f, 200014);
    }

    void spawnWaterSplash(float simX, float simY, int intensity = 2) {
        CombatVfxClip clip = CombatVfxClip::WaterSplash2;
        float dur = 0.52f;
        if (intensity <= 1) {
            const unsigned v = nextVariation() & 1u;
            clip = v == 0u ? CombatVfxClip::WaterSplash1 : CombatVfxClip::WaterSplash2;
            dur = 0.42f;
        } else if (intensity >= 3) {
            clip = CombatVfxClip::WaterSplash3;
            dur = 0.66f;
        }
        spawnClip(clip, simX, simY, 0u, dur, 1.0f, 0.96f, 200008);
    }

    void recordVehicleMotion(std::size_t vehId, int simX, int simY,
                             bool alive, bool moving, bool visible,
                             bool isNaval, bool isTracked, bool onWater, bool onShallows) {
        auto& st = trackStates_[vehId];
        if (!alive || !visible) {
            st.initialized = false;
            return;
        }
        if (!st.initialized) {
            st.lastX = simX;
            st.lastY = simY;
            st.distAccum = 0.0f;
            st.initialized = true;
            return;
        }
        const float dx = static_cast<float>(simX - st.lastX);
        const float dy = static_cast<float>(simY - st.lastY);
        const float dist = std::hypot(dx, dy);
        st.lastX = simX;
        st.lastY = simY;
        if (!moving || dist <= 0.1f || dist > 96.0f) return;

        st.distAccum += dist;
        const float spacing = (isNaval || onWater) ? 13.0f : 15.0f;
        if (st.distAccum < spacing) return;
        st.distAccum = std::fmod(st.distAccum, spacing);

        CombatVfxClip clip = CombatVfxClip::TrackTank;
        float life = 5.0f;
        float alpha = 0.48f;
        if (isNaval || onWater) {
            clip = CombatVfxClip::WakeBoat;
            life = 2.8f;
            alpha = 0.78f;
        } else if (onShallows) {
            clip = CombatVfxClip::WakeShallows;
            life = 2.2f;
            alpha = 0.72f;
        } else if (isTracked) {
            clip = CombatVfxClip::TrackTank;
            life = 5.5f;
            alpha = 0.46f;
        } else {
            const unsigned v = nextVariation() % 3u;
            clip = (v == 0u) ? CombatVfxClip::TrackWheel1
                             : (v == 1u ? CombatVfxClip::TrackWheel2 : CombatVfxClip::TrackWheel3);
            life = 4.2f;
            alpha = 0.44f;
        }
        if (decals_.size() >= kMaxDecals) decals_.erase(decals_.begin());
        decals_.push_back(ActiveDecal{
            clip, static_cast<float>(simX), static_cast<float>(simY), 0.0f, life, alpha
        });
    }

    void spawnParachuteDrop(float simX, float simY, bool withCrate, bool partsCrate = false,
                            float dropHeightPx = 72.0f, float descentSec = 1.15f) {
        if (drops_.size() >= 32u) drops_.erase(drops_.begin());
        const unsigned dir8 = nextVariation() & 7u;
        drops_.push_back(ActiveDrop{
            simX,
            simY,
            std::max(16.0f, dropHeightPx),
            0.0f,
            std::max(0.25f, descentSec),
            0.85f, // canopy collapse tail duration after landing
            withCrate ? 4.5f : 0.0f,
            dir8,
            withCrate,
            partsCrate,
            false
        });
    }

    // Stateless immediate-mode parachute canopy + shadow emitter for live descending airborne units/crates.
    void appendParachuteSprites(float simX, float simY, float altitudePx, float deployPhase01,
                                int mapScale, float brightness,
                                std::vector<RankedSprite>& out) const {
        if (!atlasSlot_) return;
        const std::size_t canopyCount = bank_.clipFrameCount(CombatVfxClip::ParachuteCanopy, 0u);
        if (canopyCount == 0u) return;
        const std::size_t openFrame = std::min<std::size_t>(8u, canopyCount - 1u);
        std::size_t frameIdx = openFrame;
        if (altitudePx > 0.5f) {
            const float openT = std::clamp(deployPhase01 * 2.5f, 0.0f, 1.0f);
            frameIdx = static_cast<std::size_t>(std::lround(openT * static_cast<float>(openFrame)));
        } else if (deployPhase01 > 0.0f && openFrame + 1u < canopyCount) {
            const std::size_t collapseFrames = canopyCount - (openFrame + 1u);
            const std::size_t step = std::min<std::size_t>(
                collapseFrames - 1u,
                static_cast<std::size_t>(std::clamp(deployPhase01, 0.0f, 0.9999f) * static_cast<float>(collapseFrames)));
            frameIdx = openFrame + 1u + step;
        }
        const float fade = (altitudePx <= 0.5f) ? std::clamp(1.0f - deployPhase01 * 0.65f, 0.15f, 1.0f) : 1.0f;
        if (const auto* sh = bank_.frameAt(CombatVfxClip::ParachuteShadow, 0u, frameIdx)) {
            const float shShift = std::max(0.0f, altitudePx) * 0.35f;
            out.push_back({
                makeWorldSprite(*sh, simX + shShift, simY + shShift * 0.5f, mapScale, 1.0f, 0.0f, 0.42f * fade),
                -15000 + static_cast<int>(simY)
            });
        }
        if (const auto* cp = bank_.frameAt(CombatVfxClip::ParachuteCanopy, 0u, frameIdx)) {
            const int sortLayer = (altitudePx > 0.5f ? 850000 : 199980) + static_cast<int>(simY);
            out.push_back({
                makeWorldSprite(*cp, simX, simY - std::max(0.0f, altitudePx) - 10.0f,
                                mapScale, 1.0f, brightness, fade),
                sortLayer
            });
        }
    }

    void appendParachuteSprites(float simX, float simY, float altitudePx, float deployPhase01,
                                int mapScale, float brightness,
                                std::vector<Sprite>& out) const {
        std::vector<RankedSprite> tmp;
        appendParachuteSprites(simX, simY, altitudePx, deployPhase01, mapScale, brightness, tmp);
        for (const auto& rs : tmp) out.push_back(rs.sprite);
    }

    // Stateless immediate-mode ammo.box / spare.parts crate emitter at world coordinates.
    void appendCrateSprite(float simX, float simY, unsigned dir8, bool spareParts,
                           int mapScale, float brightness, float alpha,
                           std::vector<RankedSprite>& out) const {
        if (!atlasSlot_ || alpha <= 0.01f) return;
        const CombatVfxClip clip = spareParts ? CombatVfxClip::PartsCrate : CombatVfxClip::AmmoCrate;
        if (const auto* fr = bank_.frameAt(clip, dir8 & 7u, 0u)) {
            out.push_back({
                makeWorldSprite(*fr, simX, simY, mapScale, 1.0f, brightness, alpha),
                static_cast<int>(simY)
            });
        }
    }

    void appendCrateSprite(float simX, float simY, unsigned dir8, bool spareParts,
                           int mapScale, float brightness, float alpha,
                           std::vector<Sprite>& out) const {
        std::vector<RankedSprite> tmp;
        appendCrateSprite(simX, simY, dir8, spareParts, mapScale, brightness, alpha, tmp);
        for (const auto& rs : tmp) out.push_back(rs.sprite);
    }

    bool step(float dtSeconds, std::span<const std::pair<float, float>> visibleBurningWrecksSim = {}) {
        if (dtSeconds <= 0.0f) return hasActiveSprites();
        const bool hadAny = hasActiveSprites() || !visibleBurningWrecksSim.empty();

        for (auto& a : anims_) a.ageSec += dtSeconds;
        std::erase_if(anims_, [](const ActiveAnim& a) { return a.ageSec >= a.durationSec; });

        for (auto& d : decals_) d.ageSec += dtSeconds;
        std::erase_if(decals_, [](const ActiveDecal& d) { return d.ageSec >= d.durationSec; });

        for (auto& dr : drops_) {
            dr.ageSec += dtSeconds;
            if (!dr.landedDustSpawned && dr.ageSec >= dr.descentSec) {
                dr.landedDustSpawned = true;
                spawnDust(dr.simX, dr.simY + 2.0f, true);
            }
        }
        std::erase_if(drops_, [](const ActiveDrop& dr) {
            return dr.ageSec >= dr.descentSec + std::max(dr.collapseSec, dr.crateLingerSec);
        });

        if (!visibleBurningWrecksSim.empty()) {
            wreckEmitAccum_ += dtSeconds;
            if (wreckEmitAccum_ >= 0.22f) {
                wreckEmitAccum_ = std::fmod(wreckEmitAccum_, 0.22f);
                for (const auto& [wx, wy] : visibleBurningWrecksSim) {
                    const unsigned j = nextVariation();
                    const float jx = static_cast<float>(int(j & 7u) - 3);
                    const float jy = static_cast<float>(int((j >> 3u) & 3u) - 2);
                    spawnClip(CombatVfxClip::FlamePlume, wx + jx, wy + jy, 0u,
                              0.68f, 0.90f, 0.92f, 200012);
                    if ((j & 1u) == 0u) {
                        spawnClip(CombatVfxClip::FireGlow, wx, wy + 2.0f, 0u,
                                  0.75f, 0.95f, 0.72f, 200010);
                    }
                }
            }
        }
        return hadAny || hasActiveSprites();
    }

    void appendSprites(int mapScale, const FogBrightnessFn& fogFn,
                       std::vector<RankedSprite>& out) const {
        if (!atlasSlot_) return;
        const auto queryFog = [&](float sx, float sy) noexcept -> float {
            if (!fogFn) return 1.0f;
            return fogFn(static_cast<int>(std::lround(sx)), static_cast<int>(std::lround(sy)));
        };

        for (const auto& d : decals_) {
            const float bright = queryFog(d.simX, d.simY);
            if (bright < 0.55f) continue; // never reveal decals in unexplored fog
            const float phase = std::clamp(d.ageSec / std::max(0.05f, d.durationSec), 0.0f, 0.9999f);
            // Play wake/track animation over the first 45% of life, hold final settled frame while fading out.
            const float animPhase = std::min(0.9999f, phase / 0.45f);
            const float fade = d.alpha * (1.0f - phase * phase);
            if (fade <= 0.02f) continue;
            if (const auto* fr = bank_.sampleFrame(d.clip, 0u, animPhase)) {
                out.push_back({
                    makeWorldSprite(*fr, d.simX, d.simY, mapScale, 1.0f, bright, fade),
                    -20000 + static_cast<int>(d.simY)
                });
            }
        }

        for (const auto& dr : drops_) {
            const float bright = queryFog(dr.simX, dr.simY);
            if (bright < 0.55f) continue;
            if (dr.ageSec < dr.descentSec) {
                const float t = std::clamp(dr.ageSec / dr.descentSec, 0.0f, 1.0f);
                const float alt = dr.dropHeightPx * (1.0f - t);
                if (dr.withCrate) {
                    appendCrateSprite(dr.simX, dr.simY - alt, dr.dir8, dr.partsCrate,
                                      mapScale, bright, 1.0f, out);
                }
                appendParachuteSprites(dr.simX, dr.simY, alt, t, mapScale, bright, out);
            } else {
                const float after = dr.ageSec - dr.descentSec;
                if (after < dr.collapseSec) {
                    appendParachuteSprites(dr.simX, dr.simY, 0.0f, after / dr.collapseSec,
                                           mapScale, bright, out);
                }
                if (dr.withCrate && after < dr.crateLingerSec) {
                    const float fade = after > (dr.crateLingerSec - 0.8f)
                        ? std::clamp((dr.crateLingerSec - after) / 0.8f, 0.0f, 1.0f)
                        : 1.0f;
                    appendCrateSprite(dr.simX, dr.simY, dr.dir8, dr.partsCrate,
                                      mapScale, bright, fade, out);
                }
            }
        }

        for (const auto& a : anims_) {
            const float bright = queryFog(a.simX, a.simY);
            if (bright < 0.55f) continue;
            const float phase = std::clamp(a.ageSec / std::max(0.05f, a.durationSec), 0.0f, 0.9999f);
            const float tailFade = (phase > 0.78f) ? ((1.0f - phase) / 0.22f) : 1.0f;
            const float alpha = std::clamp(a.alpha * tailFade, 0.0f, 1.0f);
            if (alpha <= 0.02f) continue;
            if (const auto* fr = bank_.sampleFrame(a.clip, a.dir32, phase)) {
                const bool emissive = (a.clip <= CombatVfxClip::CrateBlast3 ||
                                       a.clip == CombatVfxClip::MuzzleRifle ||
                                       a.clip == CombatVfxClip::MuzzleCannon ||
                                       a.clip == CombatVfxClip::MuzzleHowitzer ||
                                       a.clip == CombatVfxClip::MuzzleKatyusha ||
                                       a.clip == CombatVfxClip::MuzzleMortar ||
                                       a.clip == CombatVfxClip::FlamePlume ||
                                       a.clip == CombatVfxClip::FireGlow);
                out.push_back({
                    makeWorldSprite(*fr, a.simX, a.simY, mapScale, a.scale,
                                    emissive ? 1.0f : bright, alpha),
                    a.sortBias + static_cast<int>(a.simY)
                });
            }
        }
    }

private:
    struct ActiveAnim {
        CombatVfxClip clip{CombatVfxClip::ExplosionSmall};
        float simX{0.0f};
        float simY{0.0f};
        unsigned dir32{0u};
        float ageSec{0.0f};
        float durationSec{0.5f};
        float scale{1.0f};
        float alpha{1.0f};
        int sortBias{200000};
    };

    struct ActiveDecal {
        CombatVfxClip clip{CombatVfxClip::TrackTank};
        float simX{0.0f};
        float simY{0.0f};
        float ageSec{0.0f};
        float durationSec{4.0f};
        float alpha{0.5f};
    };

    struct ActiveDrop {
        float simX{0.0f};
        float simY{0.0f};
        float dropHeightPx{72.0f};
        float ageSec{0.0f};
        float descentSec{1.15f};
        float collapseSec{0.85f};
        float crateLingerSec{0.0f};
        unsigned dir8{0u};
        bool withCrate{false};
        bool partsCrate{false};
        bool landedDustSpawned{false};
    };

    struct VehicleTrackState {
        int lastX{0};
        int lastY{0};
        float distAccum{0.0f};
        bool initialized{false};
    };

    unsigned nextVariation() noexcept {
        std::uint32_t x = ++visualCounter_;
        x ^= x >> 16u;
        x *= 0x7FEB352Du;
        x ^= x >> 15u;
        return x;
    }

    Sprite makeWorldSprite(const CombatVfxBank::PackedFrame& fr,
                           float simX, float simY, int mapScale, float extraScale,
                           float brightness, float alpha) const noexcept {
        const float s = static_cast<float>(std::max(1, mapScale));
        const float fw = static_cast<float>(fr.u1 - fr.u0) * extraScale * s;
        const float fh = static_cast<float>(fr.v1 - fr.v0) * extraScale * s;
        const float ox = static_cast<float>(fr.offsetX) * extraScale * s;
        const float oy = static_cast<float>(fr.offsetY) * extraScale * s;
        return Sprite{
            simX * s + ox,
            simY * s + oy,
            fw,
            fh,
            static_cast<float>(fr.u0),
            static_cast<float>(fr.v0),
            static_cast<float>(fr.u1),
            static_cast<float>(fr.v1),
            *atlasSlot_,
            brightness,
            alpha
        };
    }

    CombatVfxBank bank_{};
    std::optional<unsigned> atlasSlot_{};
    std::vector<ActiveAnim> anims_{};
    std::vector<ActiveDecal> decals_{};
    std::vector<ActiveDrop> drops_{};
    std::map<std::size_t, VehicleTrackState> trackStates_{};
    float wreckEmitAccum_{0.0f};
    std::uint32_t visualCounter_{1u};
};

} // namespace ss2
