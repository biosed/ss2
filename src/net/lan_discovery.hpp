#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ss2 {

inline constexpr std::uint16_t kDefaultLanDiscoveryPort = 27016;

struct LanGameInfo {
    std::string hostAddress;
    std::uint16_t tcpPort{27015};
    std::uint8_t currentPlayers{1};
    std::uint8_t maxPlayers{2};
    std::uint64_t mapHash{0};
    std::string hostName;
    std::string missionSpec;
    std::chrono::steady_clock::time_point lastSeen{};
};

// Non-blocking UDP broadcaster announcing an active host lobby on the local network.
class LanBeacon {
public:
    LanBeacon() = default;
    ~LanBeacon();

    LanBeacon(const LanBeacon&) = delete;
    LanBeacon& operator=(const LanBeacon&) = delete;

    bool start(std::uint16_t discoveryPort,
               std::uint16_t tcpPort,
               std::uint8_t currentPlayers,
               std::uint8_t maxPlayers,
               std::uint64_t mapHash,
               std::string hostName,
               std::string missionSpec);
    void updatePlayerCount(std::uint8_t currentPlayers);
    void tick(std::uint16_t discoveryPort = kDefaultLanDiscoveryPort);
    void stop();
    bool active() const noexcept { return fd_ != -1; }

private:
    std::intptr_t fd_{-1};
    std::uint16_t discoveryPort_{kDefaultLanDiscoveryPort};
    std::uint16_t tcpPort_{27015};
    std::uint8_t currentPlayers_{1};
    std::uint8_t maxPlayers_{2};
    std::uint64_t mapHash_{0};
    std::string hostName_;
    std::string missionSpec_;
    std::chrono::steady_clock::time_point lastSent_{};
    bool sentOnce_{false};
};

// Non-blocking UDP listener discovering hosted games on loopback / LAN subnets.
class LanScanner {
public:
    LanScanner() = default;
    ~LanScanner();

    LanScanner(const LanScanner&) = delete;
    LanScanner& operator=(const LanScanner&) = delete;

    bool start(std::uint16_t discoveryPort = kDefaultLanDiscoveryPort);
    void poll();
    void stop();
    bool active() const noexcept { return fd_ != -1; }
    const std::vector<LanGameInfo>& games() const noexcept { return games_; }

private:
    std::intptr_t fd_{-1};
    std::vector<LanGameInfo> games_;
};

// Serialize / parse compact SS2LAN1 UDP announcement datagrams.
std::vector<std::uint8_t> encodeLanBeaconPacket(std::uint16_t tcpPort,
                                                std::uint8_t currentPlayers,
                                                std::uint8_t maxPlayers,
                                                std::uint64_t mapHash,
                                                const std::string& hostName,
                                                const std::string& missionSpec);
bool decodeLanBeaconPacket(const std::uint8_t* data,
                           std::size_t size,
                           LanGameInfo& out);

} // namespace ss2
