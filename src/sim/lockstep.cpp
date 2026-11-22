#include "sim/lockstep.hpp"
#include "sim/mission_runner.hpp"
#include "sim/state_hash.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace ss2 {
namespace {

enum class PacketTag : std::uint8_t {
    Hello = 1,
    MatchSetup = 2,
    TurnBundle = 3,
    Desync = 4,
    Bye = 5,
    Chat = 6,
    Ping = 7,
    Pong = 8,
    Beacon = 9,
};

void writeU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

void writeU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
}

void writeU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xffu));
    }
}

void writeString(std::vector<std::uint8_t>& out, std::string_view s) {
    writeU32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

struct WireReader {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    std::size_t pos{0};

    bool readU8(std::uint8_t& out) {
        if (pos + 1 > size) return false;
        out = data[pos++];
        return true;
    }

    bool readU32(std::uint32_t& out) {
        if (pos + 4 > size) return false;
        out = (static_cast<std::uint32_t>(data[pos]) << 24) |
              (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
              (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
              static_cast<std::uint32_t>(data[pos + 3]);
        pos += 4;
        return true;
    }

    bool readU64(std::uint64_t& out) {
        if (pos + 8 > size) return false;
        std::uint64_t acc = 0;
        for (int i = 0; i < 8; ++i) {
            acc = (acc << 8) | static_cast<std::uint64_t>(data[pos++]);
        }
        out = acc;
        return true;
    }

    bool readString(std::string& out, std::size_t maxLen = 65536u) {
        std::uint32_t len = 0;
        if (!readU32(len) || len > maxLen || pos + len > size) return false;
        out.assign(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return true;
    }

    bool atEnd() const noexcept { return pos == size; }
};

void patchFrameLength(std::vector<std::uint8_t>& frame) {
    const std::uint32_t payloadLen = static_cast<std::uint32_t>(frame.size() - 4u);
    frame[0] = static_cast<std::uint8_t>((payloadLen >> 24) & 0xffu);
    frame[1] = static_cast<std::uint8_t>((payloadLen >> 16) & 0xffu);
    frame[2] = static_cast<std::uint8_t>((payloadLen >> 8) & 0xffu);
    frame[3] = static_cast<std::uint8_t>(payloadLen & 0xffu);
}

} // namespace

std::vector<std::uint8_t> encodeLockstepPacket(const LockstepPacket& packet) {
    std::vector<std::uint8_t> out(4, 0u);
    std::visit(
        [&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, HelloPacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::Hello));
                out.insert(out.end(), kLockstepMagic.begin(), kLockstepMagic.begin() + 7);
                writeU32(out, p.protocolVersion);
                writeU8(out, p.preferredSeat);
                writeString(out, p.playerName);
                writeU8(out, p.nation);
            } else if constexpr (std::is_same_v<T, MatchSetupPacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::MatchSetup));
                out.insert(out.end(), kLockstepMagic.begin(), kLockstepMagic.begin() + 7);
                writeU8(out, p.assignedSeat);
                writeU8(out, p.playerCount);
                writeU32(out, p.turnTicks);
                writeU32(out, p.inputDelayTurns);
                writeU32(out, p.hashIntervalTurns);
                writeU64(out, p.mapHash);
                writeString(out, p.missionSpec);
                for (int team : p.playerTeams) {
                    writeU32(out, static_cast<std::uint32_t>(team));
                }
                writeU8(out, p.autoEngage ? 1u : 0u);
                for (std::uint8_t nat : p.playerNations) {
                    writeU8(out, nat);
                }
                for (const std::string& nm : p.playerNames) {
                    writeString(out, nm);
                }
            } else if constexpr (std::is_same_v<T, TurnBundlePacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::TurnBundle));
                writeU32(out, p.turn);
                writeU8(out, p.seat);
                writeU8(out, p.stateHash.has_value() ? 1u : 0u);
                if (p.stateHash) {
                    writeU32(out, p.hashTick);
                    writeU64(out, *p.stateHash);
                }
                std::vector<std::string> encoded;
                encoded.reserve(p.commands.size());
                for (const auto& c : p.commands) {
                    std::string line = cmd::toLine({0u, c});
                    if (!line.empty()) encoded.push_back(std::move(line));
                }
                writeU32(out, static_cast<std::uint32_t>(encoded.size()));
                for (const auto& line : encoded) {
                    writeString(out, line);
                }
            } else if constexpr (std::is_same_v<T, DesyncPacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::Desync));
                writeU32(out, p.tick);
                writeU64(out, p.localHash);
                writeU64(out, p.remoteHash);
            } else if constexpr (std::is_same_v<T, ByePacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::Bye));
                writeU8(out, p.seat);
                writeString(out, p.reason);
            } else if constexpr (std::is_same_v<T, ChatPacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::Chat));
                writeU8(out, p.seat);
                writeString(out, p.sender);
                writeString(out, p.text);
                writeU8(out, p.teamOnly ? 1u : 0u);
            } else if constexpr (std::is_same_v<T, PingPacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::Ping));
                writeU32(out, p.seq);
                writeU64(out, p.sentTimeMs);
            } else if constexpr (std::is_same_v<T, PongPacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::Pong));
                writeU32(out, p.seq);
                writeU64(out, p.echoedTimeMs);
            } else if constexpr (std::is_same_v<T, BeaconPacket>) {
                writeU8(out, static_cast<std::uint8_t>(PacketTag::Beacon));
                writeU8(out, p.seat);
                writeU32(out, static_cast<std::uint32_t>(p.x));
                writeU32(out, static_cast<std::uint32_t>(p.y));
                writeU8(out, p.teamOnly ? 1u : 0u);
            }
        },
        packet);
    patchFrameLength(out);
    return out;
}

LockstepDecodeStatus tryDecodeLockstepPacket(std::vector<std::uint8_t>& rxBuffer,
                                             std::optional<LockstepPacket>& outPacket) {
    outPacket.reset();
    if (rxBuffer.size() < 4u) return LockstepDecodeStatus::NeedMore;
    const std::uint32_t payloadLen =
        (static_cast<std::uint32_t>(rxBuffer[0]) << 24) |
        (static_cast<std::uint32_t>(rxBuffer[1]) << 16) |
        (static_cast<std::uint32_t>(rxBuffer[2]) << 8) |
        static_cast<std::uint32_t>(rxBuffer[3]);
    if (payloadLen == 0u || payloadLen > kMaxLockstepPacketBytes) {
        return LockstepDecodeStatus::Invalid;
    }
    const std::size_t frameBytes = 4u + static_cast<std::size_t>(payloadLen);
    if (rxBuffer.size() < frameBytes) return LockstepDecodeStatus::NeedMore;

    WireReader r{rxBuffer.data() + 4u, static_cast<std::size_t>(payloadLen), 0u};
    std::uint8_t rawTag = 0;
    if (!r.readU8(rawTag)) return LockstepDecodeStatus::Invalid;

    const auto consumeAndReturn = [&]() {
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + static_cast<std::ptrdiff_t>(frameBytes));
        return LockstepDecodeStatus::Ok;
    };

    const auto checkMagic = [&](WireReader& rd) -> bool {
        if (rd.pos + 7u > rd.size) return false;
        if (std::memcmp(rd.data + rd.pos, kLockstepMagic.data(), 7u) != 0) return false;
        rd.pos += 7u;
        return true;
    };

    switch (static_cast<PacketTag>(rawTag)) {
    case PacketTag::Hello: {
        if (!checkMagic(r)) return LockstepDecodeStatus::Invalid;
        HelloPacket pkt;
        if (!r.readU32(pkt.protocolVersion) || !r.readU8(pkt.preferredSeat) ||
            !r.readString(pkt.playerName, 256u) || !r.readU8(pkt.nation) ||
            pkt.nation > 4u || !r.atEnd()) {
            return LockstepDecodeStatus::Invalid;
        }
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::MatchSetup: {
        if (!checkMagic(r)) return LockstepDecodeStatus::Invalid;
        MatchSetupPacket pkt;
        std::uint8_t autoEngageByte = 1u;
        if (!r.readU8(pkt.assignedSeat) || !r.readU8(pkt.playerCount) ||
            !r.readU32(pkt.turnTicks) || !r.readU32(pkt.inputDelayTurns) ||
            !r.readU32(pkt.hashIntervalTurns) || !r.readU64(pkt.mapHash) ||
            !r.readString(pkt.missionSpec, 4096u)) {
            return LockstepDecodeStatus::Invalid;
        }
        for (int& team : pkt.playerTeams) {
            std::uint32_t rawTeam = 0;
            if (!r.readU32(rawTeam)) return LockstepDecodeStatus::Invalid;
            team = static_cast<int>(rawTeam);
        }
        if (!r.readU8(autoEngageByte)) return LockstepDecodeStatus::Invalid;
        pkt.autoEngage = (autoEngageByte != 0u);
        for (std::uint8_t& nat : pkt.playerNations) {
            if (!r.readU8(nat) || nat > 4u) return LockstepDecodeStatus::Invalid;
        }
        for (std::string& nm : pkt.playerNames) {
            if (!r.readString(nm, 64u)) return LockstepDecodeStatus::Invalid;
        }
        if (!r.atEnd()) return LockstepDecodeStatus::Invalid;
        if (pkt.playerCount < 2u || pkt.playerCount > 4u || pkt.assignedSeat >= pkt.playerCount ||
            pkt.turnTicks == 0u || pkt.turnTicks > 64u || pkt.hashIntervalTurns == 0u) {
            return LockstepDecodeStatus::Invalid;
        }
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::TurnBundle: {
        TurnBundlePacket pkt;
        std::uint8_t hasHash = 0;
        std::uint32_t cmdCount = 0;
        if (!r.readU32(pkt.turn) || !r.readU8(pkt.seat) || !r.readU8(hasHash)) {
            return LockstepDecodeStatus::Invalid;
        }
        if (hasHash != 0u) {
            std::uint64_t h = 0;
            if (!r.readU32(pkt.hashTick) || !r.readU64(h)) return LockstepDecodeStatus::Invalid;
            pkt.stateHash = h;
        }
        if (!r.readU32(cmdCount) || cmdCount > 1024u) return LockstepDecodeStatus::Invalid;
        pkt.commands.reserve(cmdCount);
        for (std::uint32_t i = 0; i < cmdCount; ++i) {
            std::string line;
            if (!r.readString(line, 1024u * 1024u)) return LockstepDecodeStatus::Invalid;
            auto parsed = cmd::fromLine(line);
            if (!parsed) return LockstepDecodeStatus::Invalid;
            pkt.commands.push_back(std::move(parsed->command));
        }
        if (!r.atEnd()) return LockstepDecodeStatus::Invalid;
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::Desync: {
        DesyncPacket pkt;
        if (!r.readU32(pkt.tick) || !r.readU64(pkt.localHash) || !r.readU64(pkt.remoteHash) || !r.atEnd()) {
            return LockstepDecodeStatus::Invalid;
        }
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::Bye: {
        ByePacket pkt;
        if (!r.readU8(pkt.seat) || pkt.seat >= 4u || !r.readString(pkt.reason, 1024u) || !r.atEnd()) {
            return LockstepDecodeStatus::Invalid;
        }
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::Chat: {
        ChatPacket pkt;
        std::uint8_t teamByte = 0u;
        if (!r.readU8(pkt.seat) || pkt.seat >= 4u || !r.readString(pkt.sender, 64u) ||
            !r.readString(pkt.text, 256u) || !r.readU8(teamByte) || !r.atEnd()) {
            return LockstepDecodeStatus::Invalid;
        }
        pkt.teamOnly = (teamByte != 0u);
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::Ping: {
        PingPacket pkt;
        if (!r.readU32(pkt.seq) || !r.readU64(pkt.sentTimeMs) || !r.atEnd()) {
            return LockstepDecodeStatus::Invalid;
        }
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::Pong: {
        PongPacket pkt;
        if (!r.readU32(pkt.seq) || !r.readU64(pkt.echoedTimeMs) || !r.atEnd()) {
            return LockstepDecodeStatus::Invalid;
        }
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    case PacketTag::Beacon: {
        BeaconPacket pkt;
        std::uint32_t rawX = 0u;
        std::uint32_t rawY = 0u;
        std::uint8_t teamByte = 0u;
        if (!r.readU8(pkt.seat) || pkt.seat >= 4u || !r.readU32(rawX) || !r.readU32(rawY) ||
            !r.readU8(teamByte) || !r.atEnd()) {
            return LockstepDecodeStatus::Invalid;
        }
        pkt.x = static_cast<std::int32_t>(rawX);
        pkt.y = static_cast<std::int32_t>(rawY);
        pkt.teamOnly = (teamByte != 0u);
        outPacket = std::move(pkt);
        return consumeAndReturn();
    }
    default:
        return LockstepDecodeStatus::Invalid;
    }
}

std::uint64_t computeMissionMapHash(const assets::MissionData& data) {
    StateHasher h;
    h.all(data.width, data.height, data.weather, data.playerTeams, data.flags, data.obstacles, data.zones, data.mines);
    h.each(data.scripts, [&](const std::vector<std::uint32_t>& sc) { h.add(sc); });
    h.each(data.reinforcements, [&](const std::vector<assets::ReinforcementUnit>& wave) {
        h.add(wave.size());
        for (const auto& u : wave) {
            h.all(u.id, u.group, u.hp, u.ammo, u.expa, u.passengers.size());
        }
    });
    return h.value();
}

LockstepCoordinator::LockstepCoordinator(LockstepConfig config) : config_(config) {
    config_.playerCount = static_cast<std::uint8_t>(std::clamp<int>(config_.playerCount, 2, 4));
    config_.localSeat = static_cast<std::uint8_t>(
        std::clamp<int>(config_.localSeat, 0, static_cast<int>(config_.playerCount) - 1));
    config_.turnTicks = std::clamp(config_.turnTicks, 1u, 32u);
    config_.inputDelayTurns = std::min(config_.inputDelayTurns, 16u);
    config_.hashIntervalTurns = std::clamp(config_.hashIntervalTurns, 1u, 100u);

    for (std::uint32_t turn = 0; turn < config_.inputDelayTurns; ++turn) {
        for (std::uint8_t seat = 0; seat < config_.playerCount; ++seat) {
            receivedBundles_[turn][seat] = TurnBundlePacket{turn, seat, {}, std::nullopt, 0u};
        }
    }
    nextLocalSendTurn_ = config_.inputDelayTurns;
}

void LockstepCoordinator::queueLocalCommand(cmd::Command command) {
    pendingLocalCommands_.push_back(std::move(command));
}

std::vector<TurnBundlePacket> LockstepCoordinator::produceOutboundBundles() {
    const std::uint32_t currentTurn = lockstepTick_ / config_.turnTicks;
    const std::uint32_t targetTurn = currentTurn + config_.inputDelayTurns;
    std::vector<TurnBundlePacket> outbound;
    while (nextLocalSendTurn_ <= targetTurn) {
        TurnBundlePacket pkt;
        pkt.turn = nextLocalSendTurn_;
        pkt.seat = config_.localSeat;
        if (nextLocalSendTurn_ == targetTurn && !pendingLocalCommands_.empty()) {
            pkt.commands = std::move(pendingLocalCommands_);
            pendingLocalCommands_.clear();
        }
        if ((pkt.turn % config_.hashIntervalTurns) == 0u && latestHashTick_.has_value()) {
            pkt.hashTick = *latestHashTick_;
            pkt.stateHash = latestHash_;
        }
        receivedBundles_[pkt.turn][pkt.seat] = pkt;
        outbound.push_back(pkt);
        ++nextLocalSendTurn_;
    }
    return outbound;
}

void LockstepCoordinator::recordAndCheckRemoteHash(std::uint32_t hashTick,
                                                   std::uint8_t seat,
                                                   std::uint64_t remoteHash) {
    if (desync_.has_value() || seat == config_.localSeat) return;
    if (const auto localIt = localHashHistory_.find(hashTick); localIt != localHashHistory_.end()) {
        if (localIt->second != remoteHash) {
            desync_ = DesyncInfo{hashTick, localIt->second, remoteHash, seat};
        } else {
            lastVerifiedTick_ = hashTick;
            lastVerifiedHash_ = localIt->second;
        }
        return;
    }
    pendingRemoteHashes_[hashTick][seat] = remoteHash;
}

bool LockstepCoordinator::receiveTurnBundle(TurnBundlePacket bundle) {
    if (bundle.seat >= config_.playerCount) return false;
    auto& turnMap = receivedBundles_[bundle.turn];
    if (turnMap.contains(bundle.seat)) return false;
    if (bundle.stateHash.has_value()) {
        recordAndCheckRemoteHash(bundle.hashTick, bundle.seat, *bundle.stateHash);
    }
    turnMap.emplace(bundle.seat, std::move(bundle));
    return true;
}

bool LockstepCoordinator::isTurnReady(std::uint32_t turn) const {
    const auto it = receivedBundles_.find(turn);
    if (it == receivedBundles_.end()) return false;
    for (std::uint8_t seat = 0; seat < config_.playerCount; ++seat) {
        if (!it->second.contains(seat)) return false;
    }
    return true;
}

std::optional<std::uint64_t> LockstepCoordinator::recordedLocalHash(std::uint32_t tick) const {
    if (const auto it = localHashHistory_.find(tick); it != localHashHistory_.end()) {
        return it->second;
    }
    return std::nullopt;
}

LockstepCoordinator::StepStatus LockstepCoordinator::stepTick(MissionRunner& runner,
                                                              const std::function<void()>& afterTickHook) {
    if (desync_.has_value()) return StepStatus::Desynced;

    const std::uint32_t turn = lockstepTick_ / config_.turnTicks;
    const std::uint32_t subTick = lockstepTick_ % config_.turnTicks;

    if (subTick == 0u && !appliedTurns_.contains(turn)) {
        if (!isTurnReady(turn)) {
            waitingTurn_ = turn;
            return StepStatus::WaitingForPeer;
        }
        waitingTurn_.reset();
        const auto& turnMap = receivedBundles_.at(turn);
        for (std::uint8_t seat = 0; seat < config_.playerCount; ++seat) {
            const auto& bundle = turnMap.at(seat);
            for (const auto& cmdItem : bundle.commands) {
                runner.applyAs(static_cast<int>(seat), cmdItem);
            }
        }
        appliedTurns_[turn] = true;
    }

    runner.advanceTicks(1u);
    if (afterTickHook) afterTickHook();

    ++lockstepTick_;
    const std::uint32_t postTick = lockstepTick_;
    const std::uint64_t postHash = runner.stateHash();
    localHashHistory_[postTick] = postHash;
    latestHashTick_ = postTick;
    latestHash_ = postHash;

    if (const auto rIt = pendingRemoteHashes_.find(postTick); rIt != pendingRemoteHashes_.end()) {
        for (const auto& [remoteSeat, remoteHash] : rIt->second) {
            if (remoteHash != postHash) {
                desync_ = DesyncInfo{postTick, postHash, remoteHash, remoteSeat};
                pendingRemoteHashes_.erase(rIt);
                return StepStatus::Desynced;
            }
            lastVerifiedTick_ = postTick;
            lastVerifiedHash_ = postHash;
        }
        pendingRemoteHashes_.erase(rIt);
    }

    if (subTick + 1u == config_.turnTicks) {
        receivedBundles_.erase(turn);
        appliedTurns_.erase(turn);
    }
    while (localHashHistory_.size() > 256u) {
        localHashHistory_.erase(localHashHistory_.begin());
    }
    while (!pendingRemoteHashes_.empty() &&
           pendingRemoteHashes_.begin()->first + 256u < postTick) {
        pendingRemoteHashes_.erase(pendingRemoteHashes_.begin());
    }
    return StepStatus::Stepped;
}

} // namespace ss2
