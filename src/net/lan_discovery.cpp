#include "net/lan_discovery.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ss2 {
namespace {

using UdpHandle = std::intptr_t;
constexpr UdpHandle kInvalidUdp = -1;
constexpr std::array<std::uint8_t, 8> kMagic{'S', 'S', '2', 'L', 'A', 'N', '1', '\0'};

void ensureWinsock() {
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)ok;
#endif
}

void closeUdp(UdpHandle& fd) {
    if (fd == kInvalidUdp) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
    fd = kInvalidUdp;
}

bool setNonBlocking(UdpHandle fd) {
#ifdef _WIN32
    u_long nb = 1;
    return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nb) == 0;
#else
    const int flags = ::fcntl(static_cast<int>(fd), F_GETFL, 0);
    return flags >= 0 && ::fcntl(static_cast<int>(fd), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void writeU16(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
}

void writeU64(std::vector<std::uint8_t>& buf, std::uint64_t v) {
    for (unsigned i = 0; i < 8u; ++i) {
        buf.push_back(static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu));
    }
}

std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8u);
}

std::uint64_t readU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (i * 8u);
    }
    return v;
}

} // namespace

std::vector<std::uint8_t> encodeLanBeaconPacket(std::uint16_t tcpPort,
                                                std::uint8_t currentPlayers,
                                                std::uint8_t maxPlayers,
                                                std::uint64_t mapHash,
                                                const std::string& hostName,
                                                const std::string& missionSpec) {
    const std::string hn = hostName.size() > 48u ? hostName.substr(0, 48u) : hostName;
    const std::string ms = missionSpec.size() > 64u ? missionSpec.substr(0, 64u) : missionSpec;
    std::vector<std::uint8_t> buf;
    buf.reserve(22u + hn.size() + ms.size());
    buf.insert(buf.end(), kMagic.begin(), kMagic.end());
    writeU16(buf, tcpPort);
    buf.push_back(currentPlayers);
    buf.push_back(maxPlayers);
    writeU64(buf, mapHash);
    buf.push_back(static_cast<std::uint8_t>(hn.size()));
    buf.push_back(static_cast<std::uint8_t>(ms.size()));
    buf.insert(buf.end(), hn.begin(), hn.end());
    buf.insert(buf.end(), ms.begin(), ms.end());
    return buf;
}

bool decodeLanBeaconPacket(const std::uint8_t* data, std::size_t size, LanGameInfo& out) {
    if (!data || size < 22u || size > 256u) return false;
    if (!std::equal(kMagic.begin(), kMagic.end(), data)) return false;
    const std::uint16_t tcpPort = readU16(data + 8u);
    if (tcpPort == 0u) return false;
    const std::uint8_t curP = data[10];
    const std::uint8_t maxP = data[11];
    const std::uint64_t hash = readU64(data + 12u);
    const std::size_t hnLen = data[20];
    const std::size_t msLen = data[21];
    if (22u + hnLen + msLen > size) return false;
    out.tcpPort = tcpPort;
    out.currentPlayers = curP;
    out.maxPlayers = maxP;
    out.mapHash = hash;
    out.hostName.assign(reinterpret_cast<const char*>(data + 22u), hnLen);
    out.missionSpec.assign(reinterpret_cast<const char*>(data + 22u + hnLen), msLen);
    return true;
}

LanBeacon::~LanBeacon() { stop(); }

bool LanBeacon::start(std::uint16_t discoveryPort,
                      std::uint16_t tcpPort,
                      std::uint8_t currentPlayers,
                      std::uint8_t maxPlayers,
                      std::uint64_t mapHash,
                      std::string hostName,
                      std::string missionSpec) {
    stop();
    ensureWinsock();
    const UdpHandle fd = static_cast<UdpHandle>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd == kInvalidUdp) return false;
    int bcast = 1;
#ifdef _WIN32
    const char cbcast = 1;
    (void)::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_BROADCAST, &cbcast, sizeof(cbcast));
#else
    (void)::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
#endif
    (void)setNonBlocking(fd);
    fd_ = fd;
    discoveryPort_ = discoveryPort;
    tcpPort_ = tcpPort;
    currentPlayers_ = currentPlayers;
    maxPlayers_ = maxPlayers;
    mapHash_ = mapHash;
    hostName_ = std::move(hostName);
    missionSpec_ = std::move(missionSpec);
    sentOnce_ = false;
    tick(discoveryPort_);
    return true;
}

void LanBeacon::updatePlayerCount(std::uint8_t currentPlayers) {
    currentPlayers_ = currentPlayers;
}

void LanBeacon::tick(std::uint16_t discoveryPort) {
    if (fd_ == kInvalidUdp) return;
    const auto now = std::chrono::steady_clock::now();
    if (sentOnce_ && now - lastSent_ < std::chrono::milliseconds(900)) return;
    lastSent_ = now;
    sentOnce_ = true;
    const std::uint16_t port = discoveryPort ? discoveryPort : discoveryPort_;
    const auto pkt = encodeLanBeaconPacket(
        tcpPort_, currentPlayers_, maxPlayers_, mapHash_, hostName_, missionSpec_);
    auto sendToAddr = [&](std::uint32_t addrHostOrder) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(addrHostOrder);
#ifdef _WIN32
        (void)::sendto(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(pkt.data()),
                       static_cast<int>(pkt.size()), 0,
                       reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#else
        (void)::sendto(static_cast<int>(fd_), pkt.data(), pkt.size(), 0,
                       reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#endif
    };
    sendToAddr(INADDR_BROADCAST);
    sendToAddr(INADDR_LOOPBACK);
}

void LanBeacon::stop() {
    closeUdp(fd_);
    sentOnce_ = false;
}

LanScanner::~LanScanner() { stop(); }

bool LanScanner::start(std::uint16_t discoveryPort) {
    stop();
    ensureWinsock();
    UdpHandle fd = static_cast<UdpHandle>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd == kInvalidUdp) return false;
#ifdef _WIN32
    const char one = 1;
    (void)::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#else
    int one = 1;
    (void)::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(discoveryPort);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef _WIN32
    if (::bind(static_cast<SOCKET>(fd), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        closeUdp(fd);
        return false;
    }
#else
    if (::bind(static_cast<int>(fd), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        closeUdp(fd);
        return false;
    }
#endif
    (void)setNonBlocking(fd);
    fd_ = fd;
    return true;
}

void LanScanner::poll() {
    if (fd_ == kInvalidUdp) return;
    const auto now = std::chrono::steady_clock::now();
    std::array<std::uint8_t, 512> buf{};
    for (int iter = 0; iter < 32; ++iter) {
        sockaddr_in src{};
#ifdef _WIN32
        int srcLen = sizeof(src);
        const int n = ::recvfrom(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(buf.data()),
                                 static_cast<int>(buf.size()), 0,
                                 reinterpret_cast<sockaddr*>(&src), &srcLen);
#else
        socklen_t srcLen = sizeof(src);
        const ssize_t n = ::recvfrom(static_cast<int>(fd_), buf.data(), buf.size(), 0,
                                     reinterpret_cast<sockaddr*>(&src), &srcLen);
#endif
        if (n <= 0) break;
        LanGameInfo info;
        if (!decodeLanBeaconPacket(buf.data(), static_cast<std::size_t>(n), info)) continue;
        char ipStr[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &src.sin_addr, ipStr, sizeof(ipStr))) {
            info.hostAddress = ipStr;
        } else {
            info.hostAddress = "127.0.0.1";
        }
        info.lastSeen = now;
        auto it = std::find_if(games_.begin(), games_.end(), [&](const LanGameInfo& g) {
            return g.hostAddress == info.hostAddress && g.tcpPort == info.tcpPort;
        });
        if (it != games_.end()) *it = std::move(info);
        else games_.push_back(std::move(info));
    }
    std::erase_if(games_, [&](const LanGameInfo& g) {
        return now - g.lastSeen >= std::chrono::milliseconds(4500);
    });
}

void LanScanner::stop() {
    closeUdp(fd_);
    games_.clear();
}

} // namespace ss2
