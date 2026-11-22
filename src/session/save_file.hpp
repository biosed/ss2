#pragma once

#include "sim/tutorial_units.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ss2 {

struct SaveFileMetadata {
    bool hasMetadata{false};
    std::string mission;
    std::string missionTitle;
    int difficulty{2};
    std::int64_t savedEpochSec{0};
    std::uint32_t tick{0};
    std::uint32_t playerSoldiers{0};
    std::uint32_t playerVehicles{0};
    std::uint32_t totalSoldiers{0};
    std::uint32_t totalVehicles{0};
    std::uint64_t fileBytes{0};
    std::uint32_t enemyInfKilled{0};
    std::uint32_t enemyVehDestroyed{0};
    std::uint32_t enemyTanksDestroyed{0};
    std::uint32_t enemyGunsDestroyed{0};
    std::uint32_t enemyPlanesShotDown{0};
    std::uint32_t buildingsDemolished{0};
    std::uint32_t airSortiesFlown{0};
    std::int32_t enemyScoreEarned{0};
    std::int32_t friendlyScoreLost{0};

    bool operator==(const SaveFileMetadata&) const = default;
};

struct DecodedSaveFile {
    SaveFileMetadata meta;
    TutorialUnits::Snapshot snapshot;
};

std::vector<std::uint8_t> encodeSaveFile(const SaveFileMetadata& meta, const TutorialUnits::Snapshot& snap);
std::optional<DecodedSaveFile> decodeSaveFile(std::span<const std::uint8_t> bytes);
bool writeSaveFile(const std::filesystem::path& path, const SaveFileMetadata& meta, const TutorialUnits::Snapshot& snap);
std::optional<DecodedSaveFile> readSaveFile(const std::filesystem::path& path);

std::string inferSaveSlotLabel(std::string_view fileStem);
std::string stripSaveSlotSuffix(std::string_view fileStem);
std::string formatSaveTimestamp(std::int64_t epochSec);

} // namespace ss2
