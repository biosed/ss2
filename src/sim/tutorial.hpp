#pragma once
#include "assets/mission.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ss2 {
struct MissionEvent {
    enum Kind {
        Dialog, Message, Camera, Marker, Victory, Defeat, Reinforcement, GroupOrder, FogReveal, PlayerTeam,
        AirSupportAddPlanes, AirSupportAddSorties, AirStrike, GroupTransferOwner, GroupReassignInZone, GroupBombard,
        MultiplayerWave, MultiplayerZeppelinWave, MultiplayerZeppelinPlanes, MultiplayerZeppelinSorties,
        AirStrikeZone, DeployReserve, UnitPoolClear, UnitPoolSaveGroup, UnitPoolSavePlayer, UnitPoolToWave,
        GroupSetStat, GroupEvacuate, ZoneEvacuate, GroupAirfieldOrder
    } kind;
    unsigned index{0};
    unsigned owner{0}, gate{0}, destZone{0}, delaySec{0};
};

struct MissionQueries {
    std::function<unsigned(unsigned zone, unsigned player)> countInZone{};
    std::function<unsigned(unsigned group)> countAliveByGroup{};
    std::function<unsigned(unsigned player)> countAliveByPlayer{};
    std::function<unsigned(unsigned zone, unsigned group)> countGroupInZone{};
    std::function<std::optional<unsigned>(unsigned player)> lastHitTick{};
    std::function<unsigned(unsigned group, unsigned mask)> countByGroupMask{};
    std::function<unsigned(int owner, unsigned mask)> countByPlayerMask{};
    std::function<bool(unsigned marker)> isMarkerObjectDestroyed{};
    std::function<unsigned(unsigned group, int team, unsigned mask)> countByGroupTeamMask{};
    std::function<unsigned(unsigned owner, unsigned slot)> countAirPlanes{};
    std::function<unsigned(unsigned poolIdx)> countUnitPool{};
    std::function<unsigned(unsigned group, unsigned slot)> groupAmmoPercent{};
    std::function<bool(unsigned waveIdx)> hasDynamicWave{};
};

// Tutorial I mission trigger VM operating on mission ticks.
class Tutorial {
public:
    explicit Tutorial(assets::TutorialData, unsigned initialZoneCount, unsigned initialEnemyGroup3 = 5);
    Tutorial(assets::TutorialData, MissionQueries queries);
    void step(unsigned playerUnitsInZone2, unsigned enemyGroup3Alive = 5);
    void step(const MissionQueries& queries);
    const assets::TutorialData& data() const { return data_; }
    uint32_t tick() const { return tick_; }
    std::vector<MissionEvent> takeEvents();
    // 64-bit hash of the script VM state (mission_state_hash.cpp).
    std::uint64_t stateHash() const;
private:
    struct GroupAiState {
        uint32_t behavior{0};
        uint32_t zone1{0};
        uint32_t zone2{0};
        uint32_t aiFlag0{0};
        uint32_t aiFlag1{0};
        // OP(79) appends patrol waypoints (Gold group AI queue at +0xd5, capped at 16);
        // OP(80) cycles the cursor (+0xf5) and copies the entry into zone1/zone2 (+8/+9).
        std::vector<std::pair<uint32_t, uint32_t>> waypoints{};
        uint32_t waypointIndex{0};
    };
    assets::TutorialData data_;
    uint32_t tick_{};
    int32_t countdown_{-1};
    std::vector<bool> active_;
    std::array<uint32_t, 100> timers_{}, timerPeriods_{}, variables_{};
    std::array<GroupAiState, 100> groupAi_{};
    std::vector<MissionEvent> events_;
    std::vector<std::pair<uint32_t, uint32_t>> airRouteWaypoints_;
    void initActive(bool fullMission);
    void scan(const MissionQueries& queries);
    void evaluate(unsigned script, const MissionQueries& queries, uint64_t& dynamicWaveMask);
};
unsigned tutorialTurnDelay(const std::filesystem::path& gameDir);

// Packs Russian/German/British/US (low 4 bytes of `packed4`) + Japanese (`jp`) wave indices for OP(52)/OP(53).
constexpr std::uint32_t packMpFourWaves(unsigned ru, unsigned de, unsigned gb, unsigned us) noexcept {
    return (ru & 0xffu) | ((de & 0xffu) << 8) | ((gb & 0xffu) << 16) | ((us & 0xffu) << 24);
}
constexpr unsigned unpackMpNationWave(std::uint32_t packed4, std::uint32_t jpWave, unsigned nation) noexcept {
    if (nation < 4u) return (packed4 >> (nation * 8u)) & 0xffu;
    if (nation == 4u) return jpWave & 0xffu;
    return 0xffu;
}
}
