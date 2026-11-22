#pragma once

#include "assets/mission.hpp"
#include "sim/command.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ss2 {

class MissionRunner;

inline constexpr std::array<char, 8> kLockstepMagic{'S', 'S', '2', 'N', 'E', 'T', '1', '\0'};
inline constexpr std::uint32_t kLockstepProtocolVersion = 1u;
inline constexpr std::uint32_t kDefaultTurnTicks = 2u;
inline constexpr std::uint32_t kDefaultInputDelayTurns = 2u;
inline constexpr std::uint32_t kDefaultHashIntervalTurns = 5u;
inline constexpr std::size_t kMaxLockstepPacketBytes = 4u * 1024u * 1024u;

struct HelloPacket {
    std::uint32_t protocolVersion{kLockstepProtocolVersion};
    std::uint8_t preferredSeat{1};
    std::string playerName;
};

struct MatchSetupPacket {
    std::uint8_t assignedSeat{1};
    std::uint8_t playerCount{2};
    std::uint32_t turnTicks{kDefaultTurnTicks};
    std::uint32_t inputDelayTurns{kDefaultInputDelayTurns};
    std::uint32_t hashIntervalTurns{kDefaultHashIntervalTurns};
    std::uint64_t mapHash{0};
    std::string missionSpec;
    std::array<int, 4> playerTeams{0, 1, 2, 3};
    bool autoEngage{true};
};

struct TurnBundlePacket {
    std::uint32_t turn{0};
    std::uint8_t seat{0};
    std::vector<cmd::Command> commands;
    std::optional<std::uint64_t> stateHash;
    std::uint32_t hashTick{0};
};

struct DesyncPacket {
    std::uint32_t tick{0};
    std::uint64_t localHash{0};
    std::uint64_t remoteHash{0};
};

struct ByePacket {
    std::uint8_t seat{0};
    std::string reason;
};

using LockstepPacket = std::variant<HelloPacket, MatchSetupPacket, TurnBundlePacket, DesyncPacket, ByePacket>;

enum class LockstepDecodeStatus {
    Ok,
    NeedMore,
    Invalid,
};

std::vector<std::uint8_t> encodeLockstepPacket(const LockstepPacket& packet);
LockstepDecodeStatus tryDecodeLockstepPacket(std::vector<std::uint8_t>& rxBuffer,
                                             std::optional<LockstepPacket>& outPacket);

std::uint64_t computeMissionMapHash(const assets::MissionData& data);

struct LockstepConfig {
    std::uint8_t localSeat{0};
    std::uint8_t playerCount{2};
    std::uint32_t turnTicks{kDefaultTurnTicks};
    std::uint32_t inputDelayTurns{kDefaultInputDelayTurns};
    std::uint32_t hashIntervalTurns{kDefaultHashIntervalTurns};
};

// Pure deterministic lockstep turn coordinator. Buffers local player commands,
// assigns them to (currentTurn + inputDelayTurns), merges per-seat TurnBundles
// in canonical seat order (0..playerCount-1) prior to advancing MissionRunner ticks,
// and verifies periodic 64-bit stateHash() equality across peers at matched ticks.
class LockstepCoordinator {
public:
    enum class StepStatus {
        Stepped,
        WaitingForPeer,
        Desynced,
    };

    struct DesyncInfo {
        std::uint32_t tick{0};
        std::uint64_t localHash{0};
        std::uint64_t remoteHash{0};
        std::uint8_t remoteSeat{0};
    };

    explicit LockstepCoordinator(LockstepConfig config = {});

    const LockstepConfig& config() const noexcept { return config_; }

    void queueLocalCommand(cmd::Command command);
    std::vector<TurnBundlePacket> produceOutboundBundles();
    std::vector<TurnBundlePacket> produceOutboundBundles(std::uint32_t currentSimTick) {
        (void)currentSimTick;
        return produceOutboundBundles();
    }
    bool receiveTurnBundle(TurnBundlePacket bundle);
    bool isTurnReady(std::uint32_t turn) const;

    StepStatus stepTick(MissionRunner& runner, const std::function<void()>& afterTickHook = {});

    std::uint32_t lockstepTick() const noexcept { return lockstepTick_; }
    const std::optional<DesyncInfo>& desync() const noexcept { return desync_; }
    std::optional<std::uint32_t> waitingTurn() const noexcept { return waitingTurn_; }
    std::optional<std::uint32_t> lastVerifiedTick() const noexcept { return lastVerifiedTick_; }
    std::optional<std::uint64_t> lastVerifiedHash() const noexcept { return lastVerifiedHash_; }
    std::optional<std::uint64_t> recordedLocalHash(std::uint32_t tick) const;

private:
    LockstepConfig config_;
    std::uint32_t lockstepTick_{0};
    std::vector<cmd::Command> pendingLocalCommands_;
    std::uint32_t nextLocalSendTurn_{0};
    std::map<std::uint32_t, std::map<std::uint8_t, TurnBundlePacket>> receivedBundles_;
    std::map<std::uint32_t, bool> appliedTurns_;
    std::map<std::uint32_t, std::uint64_t> localHashHistory_;
    std::map<std::uint32_t, std::map<std::uint8_t, std::uint64_t>> pendingRemoteHashes_;
    std::optional<std::uint32_t> latestHashTick_;
    std::uint64_t latestHash_{0};
    std::optional<std::uint32_t> waitingTurn_;
    std::optional<std::uint32_t> lastVerifiedTick_;
    std::optional<std::uint64_t> lastVerifiedHash_;
    std::optional<DesyncInfo> desync_;

    void recordAndCheckRemoteHash(std::uint32_t hashTick, std::uint8_t seat, std::uint64_t remoteHash);
};

} // namespace ss2
