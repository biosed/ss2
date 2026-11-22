#pragma once
#include "assets/map_image.hpp"
#include "assets/placement.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ss2::assets {
/// Decode raw mission text (preserving valid UTF-8, falling back per-byte to Windows-1252).
std::string decodeMissionText(std::string_view raw);

/// Loaded mission container supporting single-file `.ssm` (MS2S),
/// multiplayer `.smm`/`.ssm` (MM2S), and paired campaign `.ssc`
/// (`NNN.ssc` P2SS + `NNNMMM.ssc` I2SS with `I-CINFO` crop).
struct MissionArchive {
    std::vector<std::uint8_t> mapBytes;
    std::vector<std::uint8_t> missionBytes;
    unsigned scheme{0};
    unsigned baseWidth{128}, baseHeight{128};
    unsigned offsetX{0}, offsetY{0};
    unsigned width{128}, height{128};
    bool isCampaign{false};
    bool isMultiplayer{false};

    std::optional<std::span<const std::uint8_t>> findMapRecord(std::string_view wanted) const;
    std::optional<std::span<const std::uint8_t>> findMissionRecord(std::string_view wanted) const;
    std::span<const std::uint8_t> requireMapRecord(std::string_view wanted) const;
    std::span<const std::uint8_t> requireMissionRecord(std::string_view wanted) const;
};

MissionArchive loadMissionArchive(const std::filesystem::path& path);

struct ReinforcementUnit {
    std::string id;
    int group{0}, hp{100}, ammo{100}, expa{0};
    std::vector<PassengerPlacement> passengers{};
};

enum class AirMissionKind : std::uint8_t {
    Bomber = 0,
    Recon = 1,
    Paratrooper = 2,
    Supply = 3,
    Fighter = 4
};

struct AirSupportSlot {
    std::string planeId;
    int total{0};
    int ready{0};
};

struct ParatrooperEntry {
    std::string unitId;
    int count{0};
};

struct AmbientWoofer {
    std::string sound;
    int x{0}, y{0};
    int radiusTiles{32};
    float volume{0.2f};
    unsigned minPeriodMs{0};
    unsigned maxPeriodMs{0};
};

/// Resolve a retail I-WOOFERS sound name (e.g. "@wind_bunker", "@misc_birds") to its SOUNDS.HDR group.
std::string resolveAmbientWooferSound(std::string_view raw);

enum class WeatherKind : std::uint8_t {
    Clear = 0,
    Fog = 1,
    Rain = 2,
    Snow = 3
};

struct MissionData {
    unsigned scheme{0};
    unsigned width{128}, height{128};
    std::string description;
    std::vector<std::uint32_t> flags;
    std::vector<std::uint16_t> obstacles;
    std::vector<std::string> phrases;
    std::vector<std::string> phraseSounds;
    std::vector<std::vector<std::uint32_t>> scripts;
    std::vector<std::uint8_t> zones;
    std::array<std::pair<int, int>, 64> locations{};
    std::array<std::pair<int, int>, 100> markers{};
    std::array<std::pair<int, int>, 16> gates{};
    std::array<std::pair<int, int>, 16> gateTargets{};
    std::array<std::vector<ReinforcementUnit>, 64> reinforcements{};
    std::array<int, 4> playerTeams{0, 1, 2, 3};
    std::array<std::array<AirSupportSlot, 5>, 4> airSupport{};
    std::array<int, 4> paratrooperGroup{};
    std::array<std::vector<ParatrooperEntry>, 4> paratrooperRoster{};
    std::vector<std::uint8_t> mines;
    std::vector<AmbientWoofer> woofers;
    WeatherKind weather{WeatherKind::Clear};
};

using TutorialData = MissionData;

struct MissionSummary {
    std::filesystem::path path;
    std::string name;
    std::string description;
    unsigned width{};
    unsigned height{};
    unsigned scheme{};
};

struct CampaignMissionSummary {
    std::string code;
    std::string title;
    std::filesystem::path path;
    std::filesystem::path easyPath;
    unsigned width{};
    unsigned height{};
    unsigned scheme{};
    std::string description{};
};

struct CampaignSummary {
    std::string name;
    std::vector<CampaignMissionSummary> missions;
};

MissionData readMissionData(const std::filesystem::path& mission);
inline MissionData readTutorialData(const std::filesystem::path& mission) { return readMissionData(mission); }
MapImage decodeJpeg(std::span<const std::uint8_t> data);
MapImage readMissionOverview(const std::filesystem::path& mission);
std::vector<MissionSummary> listSinglePlayerMissions(const std::filesystem::path& gameDir);
std::optional<MissionSummary> findSinglePlayerMission(const std::filesystem::path& gameDir, std::string_view query);
std::vector<MissionSummary> listMultiplayerMissions(const std::filesystem::path& gameDir);
std::optional<MissionSummary> findMultiplayerMission(const std::filesystem::path& gameDir, std::string_view query);
std::vector<CampaignSummary> listCampaigns(const std::filesystem::path& gameDir);
std::optional<CampaignMissionSummary> findCampaignMission(const std::filesystem::path& gameDir,
                                                          std::string_view query,
                                                          bool easy = false);
std::vector<CampaignSummary> listCustomCampaigns(const std::filesystem::path& gameDir,
                                                 const std::filesystem::path& userDir = {});
std::optional<CampaignMissionSummary> findCustomCampaignMission(const std::filesystem::path& gameDir,
                                                                std::string_view query,
                                                                bool easy = false,
                                                                const std::filesystem::path& userDir = {});
}
