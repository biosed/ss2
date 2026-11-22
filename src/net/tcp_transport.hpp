#pragma once

#include "sim/lockstep.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ss2 {

// Non-blocking POSIX TCP peer connection with length-prefixed LockstepPacket framing.
class TcpConnection {
public:
    TcpConnection() = default;
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&& other) noexcept;
    TcpConnection& operator=(TcpConnection&& other) noexcept;

    static TcpConnection adoptConnectedSocket(int fd, std::string peerAddr = {});
    static TcpConnection connectNonBlocking(const std::string& host, std::uint16_t port);

    bool pollConnected();
    bool sendPacket(const LockstepPacket& packet);
    bool flush();
    bool pollPackets(std::vector<LockstepPacket>& outPackets);
    void close();

    bool isOpen() const noexcept { return fd_ >= 0; }
    bool isConnected() const noexcept { return fd_ >= 0 && connected_; }
    const std::string& lastError() const noexcept { return lastError_; }
    const std::string& peerAddress() const noexcept { return peerAddress_; }
    std::size_t bufferedTxBytes() const noexcept { return txBuf_.size(); }

private:
    int fd_{-1};
    bool connected_{false};
    bool connecting_{false};
    std::string peerAddress_;
    std::string lastError_;
    std::vector<std::uint8_t> txBuf_;
    std::vector<std::uint8_t> rxBuf_;
};

// Non-blocking POSIX TCP listener supporting ephemeral port 0 in tests and standard ports live.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    bool listen(std::uint16_t port, const std::string& bindAddr = "0.0.0.0");
    std::uint16_t boundPort() const;
    std::optional<TcpConnection> pollAccept();
    void close();

    bool isOpen() const noexcept { return fd_ >= 0; }
    const std::string& lastError() const noexcept { return lastError_; }

private:
    int fd_{-1};
    std::string lastError_;
};

// Non-blocking Host/Client lockstep handshake coordinator shared by CLI, lobby UI, and CTests.
class MultiplayerHandshake {
public:
    enum class State {
        Idle,
        Listening,
        Connecting,
        Exchanging,
        Ready,
        Failed,
    };

    bool startHost(std::uint16_t port,
                   MatchSetupPacket matchTemplate,
                   std::string bindAddr = "0.0.0.0");
    bool startJoin(const std::string& host,
                   std::uint16_t port,
                   std::string playerName = "Player 2",
                   std::uint64_t expectedMapHash = 0,
                   std::uint8_t nation = 0);

    State poll();
    void cancel();

    State state() const noexcept { return state_; }
    bool isHost() const noexcept { return isHost_; }
    std::uint16_t listeningPort() const;
    const std::string& statusText() const noexcept { return statusText_; }
    const std::string& errorText() const noexcept { return errorText_; }
    const MatchSetupPacket& matchSetup() const noexcept { return setup_; }
    std::shared_ptr<TcpConnection> takeConnection();
    std::vector<std::shared_ptr<TcpConnection>> takeConnections();

private:
    struct PendingPeer {
        std::shared_ptr<TcpConnection> conn;
        bool helloDone{false};
        std::uint8_t seat{1};
    };

    bool isHost_{false};
    State state_{State::Idle};
    TcpListener listener_;
    std::shared_ptr<TcpConnection> conn_;
    std::vector<PendingPeer> pendingPeers_;
    std::vector<std::shared_ptr<TcpConnection>> readyPeers_;
    MatchSetupPacket setup_{};
    std::string playerName_{"Player 2"};
    std::uint64_t expectedMapHash_{0};
    std::uint8_t nation_{0};
    bool helloSent_{false};
    std::string statusText_;
    std::string errorText_;
};

// Star Host-Relay lockstep transport supporting 2-4 players: the Host (seat 0) relays
// TurnBundles, global/team ChatPackets, BeaconPackets, DesyncPackets, and ByePackets
// across seats 1..playerCount-1 and answers/tracks per-peer RTT pings.
class StarRelayTransport {
public:
    struct PollResult {
        std::vector<LockstepPacket> localPackets;
        std::vector<std::uint8_t> disconnectedSeats;
        int maxRttMs{-1};
    };

    StarRelayTransport() = default;
    StarRelayTransport(bool isHost,
                       std::uint8_t localSeat,
                       std::uint8_t playerCount,
                       std::array<int, 4> playerTeams,
                       std::vector<std::shared_ptr<TcpConnection>> peers);

    bool isActive() const noexcept;
    bool isHost() const noexcept { return isHost_; }
    bool sendTurnBundle(const TurnBundlePacket& bundle);
    bool sendChat(const ChatPacket& chat);
    bool sendBeacon(const BeaconPacket& beacon);
    bool sendDesync(const DesyncPacket& desync);
    bool sendBye(const ByePacket& bye);
    void sendPingsIfDue(std::uint64_t nowMs, std::uint32_t& pingSeq);
    void flushAll();
    PollResult poll(std::uint64_t nowMs);

private:
    bool allied(std::uint8_t a, std::uint8_t b) const noexcept;
    bool sendBroadcast(const LockstepPacket& packet);

    bool isHost_{false};
    std::uint8_t localSeat_{0};
    std::uint8_t playerCount_{2};
    std::array<int, 4> playerTeams_{0, 1, 2, 3};
    std::vector<std::shared_ptr<TcpConnection>> peers_;
    std::vector<bool> peerDisconnected_;
    std::vector<int> peerRttMs_;
    std::uint64_t lastPingSentMs_{0};
    bool pingSentYet_{false};
};

} // namespace ss2
