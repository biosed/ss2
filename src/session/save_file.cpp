#include "session/save_file.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <system_error>

namespace ss2 {
namespace {

constexpr std::array<std::uint8_t, 8> kSaveMagicV1{'S', 'S', '2', 'S', 'A', 'V', 'E', '1'};
constexpr std::uint32_t kMaxStringBytes = 4096u;

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void appendI32(std::vector<std::uint8_t>& out, std::int32_t v) {
    appendU32(out, static_cast<std::uint32_t>(v));
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xffu));
    }
}

void appendI64(std::vector<std::uint8_t>& out, std::int64_t v) {
    appendU64(out, static_cast<std::uint64_t>(v));
}

void appendString(std::vector<std::uint8_t>& out, std::string_view str) {
    const std::uint32_t len = static_cast<std::uint32_t>(std::min<std::size_t>(str.size(), kMaxStringBytes));
    appendU32(out, len);
    out.insert(out.end(), str.begin(), str.begin() + static_cast<std::ptrdiff_t>(len));
}

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos{0};

    bool readU32(std::uint32_t& out) {
        if (pos + 4 > bytes.size()) return false;
        out = static_cast<std::uint32_t>(bytes[pos]) |
              (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
        pos += 4;
        return true;
    }

    bool readI32(std::int32_t& out) {
        std::uint32_t raw = 0;
        if (!readU32(raw)) return false;
        out = static_cast<std::int32_t>(raw);
        return true;
    }

    bool readI64(std::int64_t& out) {
        if (pos + 8 > bytes.size()) return false;
        std::uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
            raw |= static_cast<std::uint64_t>(bytes[pos + static_cast<std::size_t>(i)]) << (i * 8);
        }
        pos += 8;
        out = static_cast<std::int64_t>(raw);
        return true;
    }

    bool readString(std::string& out) {
        std::uint32_t len = 0;
        if (!readU32(len) || len > kMaxStringBytes || len > bytes.size() - pos) return false;
        out.assign(reinterpret_cast<const char*>(bytes.data() + pos), len);
        pos += len;
        return true;
    }
};

void populateCounts(SaveFileMetadata& meta, const TutorialUnits::Snapshot& snap, std::uint64_t byteSize) {
    meta.tick = snap.tick;
    meta.totalSoldiers = static_cast<std::uint32_t>(snap.soldiers.size());
    meta.totalVehicles = static_cast<std::uint32_t>(snap.vehicles.size());
    std::uint32_t pInf = 0;
    for (const auto& s : snap.soldiers) {
        if (s.owner == 0 && s.hp > 0) ++pInf;
    }
    std::uint32_t pVeh = 0;
    for (const auto& v : snap.vehicles) {
        if (v.owner == 0 && v.alive && v.hp > 0) ++pVeh;
    }
    meta.playerSoldiers = pInf;
    meta.playerVehicles = pVeh;
    meta.fileBytes = byteSize;
}

char toLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool endsWithIgnoreCaseAscii(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    const std::size_t off = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (toLowerAscii(text[off + i]) != toLowerAscii(suffix[i])) return false;
    }
    return true;
}

std::optional<std::string_view> parseTrailingSlotDigits(std::string_view stem) {
    std::size_t digitPos = stem.size();
    while (digitPos > 0 && stem[digitPos - 1] >= '0' && stem[digitPos - 1] <= '9') {
        --digitPos;
    }
    if (digitPos == stem.size()) return std::nullopt;
    const std::string_view prefix = stem.substr(0, digitPos);
    if (!endsWithIgnoreCaseAscii(prefix, "_slot")) return std::nullopt;
    return stem.substr(digitPos);
}

} // namespace

std::vector<std::uint8_t> encodeSaveFile(const SaveFileMetadata& meta, const TutorialUnits::Snapshot& snap) {
    const std::vector<std::uint8_t> payload = TutorialUnits::serializeSnapshot(snap);
    std::vector<std::uint8_t> out;
    out.reserve(64 + meta.mission.size() + meta.missionTitle.size() + payload.size());
    out.insert(out.end(), kSaveMagicV1.begin(), kSaveMagicV1.end());
    appendString(out, meta.mission);
    appendString(out, meta.missionTitle);
    appendI32(out, static_cast<std::int32_t>(std::clamp(meta.difficulty, 1, 3)));
    appendI64(out, meta.savedEpochSec);
    appendU32(out, meta.enemyInfKilled);
    appendU32(out, meta.enemyVehDestroyed);
    appendU32(out, meta.enemyTanksDestroyed);
    appendU32(out, meta.enemyGunsDestroyed);
    appendU32(out, meta.enemyPlanesShotDown);
    appendU32(out, meta.buildingsDemolished);
    appendU32(out, meta.airSortiesFlown);
    appendI32(out, meta.enemyScoreEarned);
    appendI32(out, meta.friendlyScoreLost);
    appendU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<DecodedSaveFile> decodeSaveFile(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= kSaveMagicV1.size() &&
        std::equal(kSaveMagicV1.begin(), kSaveMagicV1.end(), bytes.begin())) {
        Reader r{bytes, kSaveMagicV1.size()};
        SaveFileMetadata meta{};
        std::int32_t diff = 2;
        std::uint32_t payloadLen = 0;
        if (!r.readString(meta.mission) ||
            !r.readString(meta.missionTitle) ||
            !r.readI32(diff) ||
            !r.readI64(meta.savedEpochSec) ||
            !r.readU32(meta.enemyInfKilled) ||
            !r.readU32(meta.enemyVehDestroyed) ||
            !r.readU32(meta.enemyTanksDestroyed) ||
            !r.readU32(meta.enemyGunsDestroyed) ||
            !r.readU32(meta.enemyPlanesShotDown) ||
            !r.readU32(meta.buildingsDemolished) ||
            !r.readU32(meta.airSortiesFlown) ||
            !r.readI32(meta.enemyScoreEarned) ||
            !r.readI32(meta.friendlyScoreLost) ||
            !r.readU32(payloadLen)) {
            return std::nullopt;
        }
        if (payloadLen > bytes.size() - r.pos || r.pos + payloadLen != bytes.size()) {
            return std::nullopt;
        }
        auto snap = TutorialUnits::deserializeSnapshot(bytes.subspan(r.pos, payloadLen));
        if (!snap) return std::nullopt;
        meta.hasMetadata = true;
        meta.difficulty = std::clamp(static_cast<int>(diff), 1, 3);
        populateCounts(meta, *snap, static_cast<std::uint64_t>(bytes.size()));
        return DecodedSaveFile{std::move(meta), std::move(*snap)};
    }

    auto snap = TutorialUnits::deserializeSnapshot(bytes);
    if (!snap) return std::nullopt;
    SaveFileMetadata meta{};
    meta.hasMetadata = false;
    meta.difficulty = 2;
    populateCounts(meta, *snap, static_cast<std::uint64_t>(bytes.size()));
    return DecodedSaveFile{std::move(meta), std::move(*snap)};
}

bool writeSaveFile(const std::filesystem::path& path, const SaveFileMetadata& meta,
                   const TutorialUnits::Snapshot& snap) {
    if (path.empty()) return false;
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
    }
    const std::vector<std::uint8_t> bytes = encodeSaveFile(meta, snap);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
}

std::optional<DecodedSaveFile> readSaveFile(const std::filesystem::path& path) {
    if (path.empty()) return std::nullopt;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const auto endPos = in.tellg();
    if (endPos <= 0) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(endPos));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    auto decoded = decodeSaveFile(bytes);
    if (!decoded) return std::nullopt;
    if (decoded->meta.savedEpochSec <= 0) {
        const auto ftime = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
            decoded->meta.savedEpochSec =
                std::chrono::duration_cast<std::chrono::seconds>(sysTime.time_since_epoch()).count();
        }
    }
    return decoded;
}

std::string inferSaveSlotLabel(std::string_view fileStem) {
    if (endsWithIgnoreCaseAscii(fileStem, "_autosave")) {
        return "Auto-Save";
    }
    if (const auto digits = parseTrailingSlotDigits(fileStem)) {
        return "Slot " + std::string(*digits);
    }
    return "Quick Save";
}

std::string stripSaveSlotSuffix(std::string_view fileStem) {
    if (endsWithIgnoreCaseAscii(fileStem, "_autosave")) {
        fileStem.remove_suffix(9);
        return std::string(fileStem);
    }
    if (const auto digits = parseTrailingSlotDigits(fileStem)) {
        fileStem.remove_suffix(5 + digits->size());
        return std::string(fileStem);
    }
    return std::string(fileStem);
}

std::string formatSaveTimestamp(std::int64_t epochSec) {
    if (epochSec <= 0) return {};
    const std::time_t tt = static_cast<std::time_t>(epochSec);
    std::tm tm{};
    if (!localtime_r(&tt, &tm)) return {};
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm) == 0) return {};
    return std::string(buf);
}

} // namespace ss2
