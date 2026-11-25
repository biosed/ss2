#include "net/tcp_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

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
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ss2 {
namespace {

using SocketHandle = TcpConnection::SocketHandle;
constexpr SocketHandle kInvalidSocket = TcpConnection::kInvalidSocket;

void ensureWinsockInitialized() {
#ifdef _WIN32
    static const bool initialized = [] {
        WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)initialized;
#endif
}

void closeSocket(SocketHandle fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

int lastSockError() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

std::string sockErrorString(int err) {
#ifdef _WIN32
    return "Winsock error " + std::to_string(err);
#else
    return std::strerror(err);
#endif
}

bool isWouldBlockErr(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

bool isConnectInProgressErr(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EINPROGRESS;
#endif
}

bool isIntrErr(int err) {
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

bool setNonBlockingAndNoDelay(SocketHandle fd) {
#ifdef _WIN32
    u_long nonBlock = 1;
    if (::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nonBlock) != 0) return false;
    const char one = 1;
    (void)::setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
#else
    const int flags = ::fcntl(static_cast<int>(fd), F_GETFL, 0);
    if (flags < 0 || ::fcntl(static_cast<int>(fd), F_SETFL, flags | O_NONBLOCK) < 0) return false;
    const int one = 1;
    (void)::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
#endif
}

std::string formatSockAddr(const struct sockaddr* sa, socklen_t len) {
    char host[NI_MAXHOST]{};
    char serv[NI_MAXSERV]{};
    if (::getnameinfo(sa, len, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        return std::string(host) + ":" + std::string(serv);
    }
    return {};
}

} // namespace

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : fd_(std::exchange(other.fd_, kInvalidSocket)),
      connected_(std::exchange(other.connected_, false)),
      connecting_(std::exchange(other.connecting_, false)),
      peerAddress_(std::move(other.peerAddress_)),
      lastError_(std::move(other.lastError_)),
      txBuf_(std::move(other.txBuf_)),
      rxBuf_(std::move(other.rxBuf_)) {}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, kInvalidSocket);
        connected_ = std::exchange(other.connected_, false);
        connecting_ = std::exchange(other.connecting_, false);
        peerAddress_ = std::move(other.peerAddress_);
        lastError_ = std::move(other.lastError_);
        txBuf_ = std::move(other.txBuf_);
        rxBuf_ = std::move(other.rxBuf_);
    }
    return *this;
}

TcpConnection TcpConnection::adoptConnectedSocket(SocketHandle fd, std::string peerAddr) {
    TcpConnection c;
    c.fd_ = fd;
    c.connected_ = (fd != kInvalidSocket);
    c.connecting_ = false;
    c.peerAddress_ = std::move(peerAddr);
    if (fd != kInvalidSocket && !setNonBlockingAndNoDelay(fd)) {
        c.lastError_ = sockErrorString(lastSockError());
        c.close();
    }
    return c;
}

TcpConnection TcpConnection::connectNonBlocking(const std::string& host, std::uint16_t port) {
    ensureWinsockInitialized();
    TcpConnection conn;
    if (host.empty() || port == 0) {
        conn.lastError_ = "Invalid host or port";
        return conn;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const std::string portStr = std::to_string(port);
    struct addrinfo* res = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        conn.lastError_ = gai != 0 ? ::gai_strerror(gai) : "Failed to resolve host";
        return conn;
    }

    int lastErr = 0;
    for (struct addrinfo* it = res; it != nullptr; it = it->ai_next) {
#ifdef _WIN32
        const SOCKET rawSock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        const SocketHandle fd = (rawSock == INVALID_SOCKET) ? kInvalidSocket : static_cast<SocketHandle>(rawSock);
#else
        const SocketHandle fd = static_cast<SocketHandle>(::socket(it->ai_family, it->ai_socktype, it->ai_protocol));
#endif
        if (fd == kInvalidSocket) {
            lastErr = lastSockError();
            continue;
        }
        if (!setNonBlockingAndNoDelay(fd)) {
            lastErr = lastSockError();
            closeSocket(fd);
            continue;
        }
        const int rc = ::connect(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
        if (rc == 0) {
            conn.fd_ = fd;
            conn.connected_ = true;
            conn.connecting_ = false;
            conn.peerAddress_ = formatSockAddr(it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
            ::freeaddrinfo(res);
            return conn;
        }
        const int err = lastSockError();
        if (isConnectInProgressErr(err)) {
            conn.fd_ = fd;
            conn.connected_ = false;
            conn.connecting_ = true;
            conn.peerAddress_ = formatSockAddr(it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
            ::freeaddrinfo(res);
            return conn;
        }
        lastErr = err;
        closeSocket(fd);
    }
    ::freeaddrinfo(res);
    conn.lastError_ = lastErr != 0 ? sockErrorString(lastErr) : "Connection refused";
    return conn;
}

bool TcpConnection::pollConnected() {
    if (fd_ == kInvalidSocket) return false;
    if (connected_) return true;
    if (!connecting_) return false;

#ifdef _WIN32
    fd_set wfds;
    fd_set efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(static_cast<SOCKET>(fd_), &wfds);
    FD_SET(static_cast<SOCKET>(fd_), &efds);
    struct timeval tv{0, 0};
    const int rc = ::select(0, nullptr, &wfds, &efds, &tv);
    if (rc < 0) {
        const int err = lastSockError();
        if (isIntrErr(err)) return false;
        lastError_ = sockErrorString(err);
        close();
        return false;
    }
    if (rc == 0) return false;
#else
    struct pollfd pfd{};
    pfd.fd = static_cast<int>(fd_);
    pfd.events = POLLOUT;
    const int rc = ::poll(&pfd, 1, 0);
    if (rc < 0) {
        if (errno == EINTR) return false;
        lastError_ = std::strerror(errno);
        close();
        return false;
    }
    if (rc == 0) return false;
#endif

    int err = 0;
    socklen_t errLen = sizeof(err);
#ifdef _WIN32
    if (::getsockopt(static_cast<SOCKET>(fd_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errLen) < 0) {
#else
    if (::getsockopt(static_cast<int>(fd_), SOL_SOCKET, SO_ERROR, &err, &errLen) < 0) {
#endif
        lastError_ = sockErrorString(lastSockError());
        close();
        return false;
    }
    if (err != 0) {
        lastError_ = sockErrorString(err);
        close();
        return false;
    }
    connected_ = true;
    connecting_ = false;
    return true;
}

bool TcpConnection::sendPacket(const LockstepPacket& packet) {
    if (fd_ == kInvalidSocket) return false;
    const auto frame = encodeLockstepPacket(packet);
    txBuf_.insert(txBuf_.end(), frame.begin(), frame.end());
    return flush();
}

bool TcpConnection::flush() {
    if (fd_ == kInvalidSocket) return false;
    if (!connected_ && !pollConnected()) return !connecting_ ? false : true;
    std::size_t sentOffset = 0;
    while (sentOffset < txBuf_.size()) {
        const auto remaining = txBuf_.size() - sentOffset;
#ifdef _WIN32
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1u << 20));
        const int n = ::send(static_cast<SOCKET>(fd_),
                             reinterpret_cast<const char*>(txBuf_.data() + sentOffset), chunk, 0);
#else
        const ssize_t n = ::send(static_cast<int>(fd_), txBuf_.data() + sentOffset, remaining, MSG_NOSIGNAL);
#endif
        if (n > 0) {
            sentOffset += static_cast<std::size_t>(n);
            continue;
        }
        const int err = lastSockError();
        if (n < 0 && isIntrErr(err)) continue;
        if (n < 0 && isWouldBlockErr(err)) break;
        lastError_ = n == 0 ? "Connection closed during send" : sockErrorString(err);
        close();
        return false;
    }
    if (sentOffset > 0) {
        txBuf_.erase(txBuf_.begin(), txBuf_.begin() + static_cast<std::ptrdiff_t>(sentOffset));
    }
    return true;
}

bool TcpConnection::pollPackets(std::vector<LockstepPacket>& outPackets) {
    if (fd_ == kInvalidSocket) return false;
    if (!connected_ && !pollConnected()) return connecting_;
    (void)flush();
    if (fd_ == kInvalidSocket) return false;

    std::array<std::uint8_t, 16384> tmp{};
    for (;;) {
#ifdef _WIN32
        const int n = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(tmp.data()),
                             static_cast<int>(tmp.size()), 0);
#else
        const ssize_t n = ::recv(static_cast<int>(fd_), tmp.data(), tmp.size(), 0);
#endif
        if (n > 0) {
            rxBuf_.insert(rxBuf_.end(), tmp.begin(), tmp.begin() + n);
            continue;
        }
        if (n == 0) {
            lastError_ = "Peer closed connection";
            close();
            break;
        }
        const int err = lastSockError();
        if (isIntrErr(err)) continue;
        if (isWouldBlockErr(err)) break;
        lastError_ = sockErrorString(err);
        close();
        return false;
    }

    for (;;) {
        std::optional<LockstepPacket> pkt;
        const auto status = tryDecodeLockstepPacket(rxBuf_, pkt);
        if (status == LockstepDecodeStatus::Ok) {
            if (pkt) outPackets.push_back(std::move(*pkt));
            continue;
        }
        if (status == LockstepDecodeStatus::Invalid) {
            lastError_ = "Malformed lockstep packet";
            close();
            return false;
        }
        break;
    }
    return fd_ != kInvalidSocket;
}

void TcpConnection::close() {
    if (fd_ != kInvalidSocket) {
        closeSocket(fd_);
        fd_ = kInvalidSocket;
    }
    connected_ = false;
    connecting_ = false;
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : fd_(std::exchange(other.fd_, TcpConnection::kInvalidSocket)), lastError_(std::move(other.lastError_)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, TcpConnection::kInvalidSocket);
        lastError_ = std::move(other.lastError_);
    }
    return *this;
}

bool TcpListener::listen(std::uint16_t port, const std::string& bindAddr) {
    ensureWinsockInitialized();
    close();
#ifdef _WIN32
    const SOCKET rawSock = ::socket(AF_INET, SOCK_STREAM, 0);
    const SocketHandle fd = (rawSock == INVALID_SOCKET) ? kInvalidSocket : static_cast<SocketHandle>(rawSock);
#else
    const SocketHandle fd = static_cast<SocketHandle>(::socket(AF_INET, SOCK_STREAM, 0));
    const int one = 1;
    if (fd != kInvalidSocket) (void)::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    if (fd == kInvalidSocket) {
        lastError_ = sockErrorString(lastSockError());
        return false;
    }
    if (!setNonBlockingAndNoDelay(fd)) {
        lastError_ = sockErrorString(lastSockError());
        closeSocket(fd);
        return false;
    }
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bindAddr.empty() || bindAddr == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) {
        lastError_ = "Invalid bind address: " + bindAddr;
        closeSocket(fd);
        return false;
    }
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 8) < 0) {
        lastError_ = sockErrorString(lastSockError());
        closeSocket(fd);
        return false;
    }
    fd_ = fd;
    lastError_.clear();
    return true;
}

std::uint16_t TcpListener::boundPort() const {
    if (fd_ == kInvalidSocket) return 0;
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

std::optional<TcpConnection> TcpListener::pollAccept() {
    if (fd_ == kInvalidSocket) return std::nullopt;
    struct sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
#ifdef _WIN32
    const SOCKET clientRaw = ::accept(static_cast<SOCKET>(fd_), reinterpret_cast<struct sockaddr*>(&ss), &len);
    const SocketHandle clientFd = (clientRaw == INVALID_SOCKET) ? kInvalidSocket : static_cast<SocketHandle>(clientRaw);
#else
    const SocketHandle clientFd = static_cast<SocketHandle>(::accept(static_cast<int>(fd_), reinterpret_cast<struct sockaddr*>(&ss), &len));
#endif
    if (clientFd == kInvalidSocket) {
        const int err = lastSockError();
        if (!isWouldBlockErr(err) && !isIntrErr(err)) {
            lastError_ = sockErrorString(err);
        }
        return std::nullopt;
    }
    auto conn = TcpConnection::adoptConnectedSocket(
        clientFd, formatSockAddr(reinterpret_cast<const struct sockaddr*>(&ss), len));
    if (!conn.isOpen()) return std::nullopt;
    return conn;
}

void TcpListener::close() {
    if (fd_ != kInvalidSocket) {
        closeSocket(fd_);
        fd_ = kInvalidSocket;
    }
}

bool MultiplayerHandshake::startHost(std::uint16_t port,
                                     MatchSetupPacket matchTemplate,
                                     std::string bindAddr) {
    cancel();
    isHost_ = true;
    setup_ = std::move(matchTemplate);
    if (setup_.playerCount < 2u || setup_.playerCount > 4u) setup_.playerCount = 2u;
    for (int s = 0; s < static_cast<int>(setup_.playerCount); ++s) {
        if (setup_.playerNames[s].empty()) {
            setup_.playerNames[s] = "Player " + std::to_string(s + 1);
        }
    }
    if (!listener_.listen(port, bindAddr)) {
        state_ = State::Failed;
        errorText_ = listener_.lastError();
        statusText_ = "Listen failed: " + errorText_;
        return false;
    }
    state_ = State::Listening;
    (void)lanBeacon_.start(kDefaultLanDiscoveryPort, listener_.boundPort(), 1u, setup_.playerCount,
                           setup_.mapHash, setup_.playerNames[0], setup_.missionSpec);
    statusText_ = setup_.playerCount > 2u
        ? ("Waiting for players (1/" + std::to_string(setup_.playerCount) + ")...")
        : ("Listening on port " + std::to_string(listener_.boundPort()) + "...");
    return true;
}

bool MultiplayerHandshake::startJoin(const std::string& host,
                                     std::uint16_t port,
                                     std::string playerName,
                                     std::uint64_t expectedMapHash,
                                     std::uint8_t nation) {
    cancel();
    isHost_ = false;
    playerName_ = std::move(playerName);
    expectedMapHash_ = expectedMapHash;
    nation_ = nation <= 4u ? nation : 0u;
    helloSent_ = false;
    auto c = TcpConnection::connectNonBlocking(host, port);
    if (!c.isOpen()) {
        state_ = State::Failed;
        errorText_ = c.lastError();
        statusText_ = "Connect failed: " + errorText_;
        return false;
    }
    conn_ = std::make_shared<TcpConnection>(std::move(c));
    state_ = State::Connecting;
    statusText_ = "Connecting to " + host + ":" + std::to_string(port) + "...";
    return true;
}

MultiplayerHandshake::State MultiplayerHandshake::poll() {
    if (state_ == State::Idle || state_ == State::Ready || state_ == State::Failed) return state_;

    if (isHost_) {
        const std::size_t expectedClients = static_cast<std::size_t>(setup_.playerCount - 1u);
        while (pendingPeers_.size() < expectedClients) {
            auto accepted = listener_.pollAccept();
            if (!accepted) break;
            std::uint8_t nextSeat = 1u;
            for (; nextSeat < setup_.playerCount; ++nextSeat) {
                const bool used = std::any_of(pendingPeers_.begin(), pendingPeers_.end(),
                    [nextSeat](const PendingPeer& p) { return p.seat == nextSeat; });
                if (!used) break;
            }
            if (nextSeat >= setup_.playerCount) break;
            PendingPeer peer;
            peer.conn = std::make_shared<TcpConnection>(std::move(*accepted));
            peer.helloDone = false;
            peer.seat = nextSeat;
            pendingPeers_.push_back(std::move(peer));
        }

        if (!pendingPeers_.empty() && state_ == State::Listening) {
            state_ = State::Exchanging;
        }

        for (std::size_t i = 0; i < pendingPeers_.size();) {
            auto& peer = pendingPeers_[i];
            std::vector<LockstepPacket> packets;
            const bool pollOk = peer.conn && peer.conn->pollPackets(packets);
            bool dropPeer = !pollOk;
            for (const auto& pkt : packets) {
                if (const auto* hello = std::get_if<HelloPacket>(&pkt)) {
                    if (hello->protocolVersion != kLockstepProtocolVersion) {
                        if (peer.conn) {
                            (void)peer.conn->sendPacket(ByePacket{0u, "Protocol version mismatch"});
                            (void)peer.conn->flush();
                        }
                        dropPeer = true;
                        break;
                    }
                    setup_.playerNations[peer.seat] = hello->nation <= 4u ? hello->nation : 0u;
                    if (!hello->playerName.empty()) {
                        setup_.playerNames[peer.seat] = hello->playerName;
                    }
                    peer.helloDone = true;
                } else if (std::get_if<ByePacket>(&pkt)) {
                    dropPeer = true;
                    break;
                }
            }
            if (dropPeer) {
                pendingPeers_.erase(pendingPeers_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }

        std::size_t readyCount = 1u;
        for (const auto& p : pendingPeers_) {
            if (p.helloDone) ++readyCount;
        }
        lanBeacon_.updatePlayerCount(static_cast<std::uint8_t>(std::min<std::size_t>(255u, readyCount)));
        lanBeacon_.tick(kDefaultLanDiscoveryPort);
        statusText_ = "Waiting for players (" + std::to_string(readyCount) + "/" +
                      std::to_string(setup_.playerCount) + ")...";

        if (pendingPeers_.size() == expectedClients && readyCount == setup_.playerCount) {
            lanBeacon_.stop();
            listener_.close();
            std::sort(pendingPeers_.begin(), pendingPeers_.end(),
                      [](const PendingPeer& a, const PendingPeer& b) { return a.seat < b.seat; });
            readyPeers_.clear();
            readyPeers_.reserve(expectedClients);
            for (const auto& p : pendingPeers_) {
                MatchSetupPacket perPeer = setup_;
                perPeer.assignedSeat = p.seat;
                if (!p.conn || !p.conn->sendPacket(perPeer)) {
                    const std::string err = p.conn ? p.conn->lastError() : "Lost peer";
                    pendingPeers_.clear();
                    readyPeers_.clear();
                    state_ = State::Failed;
                    errorText_ = err;
                    statusText_ = "Failed sending MatchSetup: " + errorText_;
                    return state_;
                }
                (void)p.conn->flush();
                readyPeers_.push_back(p.conn);
            }
            pendingPeers_.clear();
            setup_.assignedSeat = 0u;
            state_ = State::Ready;
            statusText_ = setup_.playerCount > 2u
                ? ("Connected (Seat 1/" + std::to_string(setup_.playerCount) + " Host)")
                : "Connected (Seat 0 Host)";
            return state_;
        }
        return state_;
    }

    if (state_ == State::Connecting) {
        if (!conn_ || (!conn_->pollConnected() && !conn_->isOpen())) {
            state_ = State::Failed;
            errorText_ = conn_ ? conn_->lastError() : "Connect failed";
            statusText_ = "Connect failed: " + errorText_;
            return state_;
        }
        if (conn_->isConnected()) {
            state_ = State::Exchanging;
            statusText_ = "Connected; exchanging setup...";
        } else {
            return state_;
        }
    }

    if (state_ == State::Exchanging) {
        if (!helloSent_) {
            if (!conn_ || !conn_->sendPacket(HelloPacket{kLockstepProtocolVersion, 1u, playerName_, nation_})) {
                state_ = State::Failed;
                errorText_ = conn_ ? conn_->lastError() : "Send failed";
                statusText_ = "Handshake failed: " + errorText_;
                return state_;
            }
            helloSent_ = true;
        }
        std::vector<LockstepPacket> packets;
        if (!conn_ || !conn_->pollPackets(packets)) {
            state_ = State::Failed;
            errorText_ = conn_ ? conn_->lastError() : "Connection closed";
            statusText_ = "Handshake failed: " + errorText_;
            return state_;
        }
        for (const auto& pkt : packets) {
            if (const auto* setup = std::get_if<MatchSetupPacket>(&pkt)) {
                if (expectedMapHash_ != 0u && setup->mapHash != expectedMapHash_) {
                    (void)conn_->sendPacket(ByePacket{setup->assignedSeat, "Map hash mismatch"});
                    state_ = State::Failed;
                    errorText_ = "Map hash mismatch between host and client";
                    statusText_ = errorText_;
                    return state_;
                }
                setup_ = *setup;
                state_ = State::Ready;
                statusText_ = setup_.playerCount > 2u
                    ? ("Connected (Seat " + std::to_string(setup_.assignedSeat + 1u) + "/" +
                       std::to_string(setup_.playerCount) + ")")
                    : ("Connected (Seat " + std::to_string(setup_.assignedSeat) + ")");
                return state_;
            }
            if (const auto* bye = std::get_if<ByePacket>(&pkt)) {
                state_ = State::Failed;
                errorText_ = bye->reason.empty() ? "Host rejected handshake" : bye->reason;
                statusText_ = errorText_;
                return state_;
            }
        }
    }
    return state_;
}

void MultiplayerHandshake::cancel() {
    lanBeacon_.stop();
    listener_.close();
    if (conn_) {
        if (conn_->isConnected()) {
            (void)conn_->sendPacket(ByePacket{isHost_ ? std::uint8_t{0} : setup_.assignedSeat, "Cancelled"});
        }
        conn_->close();
        conn_.reset();
    }
    for (auto& p : pendingPeers_) {
        if (p.conn) {
            if (p.conn->isConnected()) (void)p.conn->sendPacket(ByePacket{0u, "Cancelled"});
            p.conn->close();
        }
    }
    pendingPeers_.clear();
    for (auto& c : readyPeers_) {
        if (c) {
            if (c->isConnected()) (void)c->sendPacket(ByePacket{0u, "Cancelled"});
            c->close();
        }
    }
    readyPeers_.clear();
    helloSent_ = false;
    state_ = State::Idle;
    errorText_.clear();
}

std::uint16_t MultiplayerHandshake::listeningPort() const {
    return listener_.boundPort();
}

std::shared_ptr<TcpConnection> MultiplayerHandshake::takeConnection() {
    if (isHost_) {
        if (readyPeers_.empty()) return {};
        return std::move(readyPeers_.front());
    }
    return std::move(conn_);
}

std::vector<std::shared_ptr<TcpConnection>> MultiplayerHandshake::takeConnections() {
    if (isHost_) {
        return std::exchange(readyPeers_, {});
    }
    std::vector<std::shared_ptr<TcpConnection>> out;
    if (conn_) out.push_back(std::move(conn_));
    return out;
}

StarRelayTransport::StarRelayTransport(bool isHost,
                                       std::uint8_t localSeat,
                                       std::uint8_t playerCount,
                                       std::array<int, 4> playerTeams,
                                       std::vector<std::shared_ptr<TcpConnection>> peers)
    : isHost_(isHost),
      localSeat_(localSeat < 4u ? localSeat : 0u),
      playerCount_(static_cast<std::uint8_t>(std::clamp<int>(playerCount, 2, 4))),
      playerTeams_(playerTeams),
      peers_(std::move(peers)),
      peerDisconnected_(peers_.size(), false),
      peerRttMs_(peers_.size(), -1) {}

bool StarRelayTransport::isActive() const noexcept {
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        if (!peerDisconnected_[i] && peers_[i] && peers_[i]->isConnected()) return true;
    }
    return false;
}

bool StarRelayTransport::allied(std::uint8_t a, std::uint8_t b) const noexcept {
    if (a >= 4u || b >= 4u) return false;
    return playerTeams_[a] == playerTeams_[b];
}

bool StarRelayTransport::sendBroadcast(const LockstepPacket& packet) {
    bool any = false;
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        if (peerDisconnected_[i] || !peers_[i] || !peers_[i]->isConnected()) continue;
        if (peers_[i]->sendPacket(packet)) any = true;
    }
    return any;
}

bool StarRelayTransport::sendTurnBundle(const TurnBundlePacket& bundle) {
    return sendBroadcast(LockstepPacket{bundle});
}

bool StarRelayTransport::sendChat(const ChatPacket& chat) {
    if (!isHost_) return sendBroadcast(LockstepPacket{chat});
    bool any = false;
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        if (peerDisconnected_[i] || !peers_[i] || !peers_[i]->isConnected()) continue;
        const auto targetSeat = static_cast<std::uint8_t>(i + 1u);
        if (chat.teamOnly && !allied(localSeat_, targetSeat)) continue;
        if (peers_[i]->sendPacket(LockstepPacket{chat})) any = true;
    }
    return any;
}

bool StarRelayTransport::sendBeacon(const BeaconPacket& beacon) {
    if (!isHost_) return sendBroadcast(LockstepPacket{beacon});
    bool any = false;
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        if (peerDisconnected_[i] || !peers_[i] || !peers_[i]->isConnected()) continue;
        const auto targetSeat = static_cast<std::uint8_t>(i + 1u);
        if (beacon.teamOnly && !allied(localSeat_, targetSeat)) continue;
        if (peers_[i]->sendPacket(LockstepPacket{beacon})) any = true;
    }
    return any;
}

bool StarRelayTransport::sendDesync(const DesyncPacket& desync) {
    return sendBroadcast(LockstepPacket{desync});
}

bool StarRelayTransport::sendBye(const ByePacket& bye) {
    const bool ok = sendBroadcast(LockstepPacket{bye});
    flushAll();
    return ok;
}

void StarRelayTransport::sendPingsIfDue(std::uint64_t nowMs, std::uint32_t& pingSeq) {
    if (!isActive()) return;
    if (!pingSentYet_ || (nowMs >= lastPingSentMs_ && nowMs - lastPingSentMs_ >= 2000u)) {
        pingSentYet_ = true;
        lastPingSentMs_ = nowMs;
        (void)sendBroadcast(LockstepPacket{PingPacket{++pingSeq, nowMs}});
    }
}

void StarRelayTransport::flushAll() {
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        if (!peerDisconnected_[i] && peers_[i]) (void)peers_[i]->flush();
    }
}

StarRelayTransport::PollResult StarRelayTransport::poll(std::uint64_t nowMs) {
    PollResult res;
    if (!isHost_) {
        if (!peers_.empty() && !peerDisconnected_[0] && peers_[0]) {
            std::vector<LockstepPacket> rx;
            const bool ok = peers_[0]->pollPackets(rx);
            for (auto& pkt : rx) {
                if (const auto* ping = std::get_if<PingPacket>(&pkt)) {
                    (void)peers_[0]->sendPacket(LockstepPacket{PongPacket{ping->seq, ping->sentTimeMs}});
                } else if (const auto* pong = std::get_if<PongPacket>(&pkt)) {
                    if (nowMs >= pong->echoedTimeMs) {
                        peerRttMs_[0] = static_cast<int>(std::min<std::uint64_t>(nowMs - pong->echoedTimeMs, 9999u));
                    }
                } else {
                    res.localPackets.push_back(std::move(pkt));
                }
            }
            if (!ok) {
                peerDisconnected_[0] = true;
                res.disconnectedSeats.push_back(0u);
            }
        }
        if (!peerRttMs_.empty()) res.maxRttMs = peerRttMs_[0];
        return res;
    }

    for (std::size_t i = 0; i < peers_.size(); ++i) {
        if (peerDisconnected_[i] || !peers_[i]) continue;
        const auto seat = static_cast<std::uint8_t>(i + 1u);
        std::vector<LockstepPacket> rx;
        const bool ok = peers_[i]->pollPackets(rx);
        bool sawBye = false;
        std::string byeReason = "Peer disconnected";

        for (auto& pkt : rx) {
            if (const auto* bundle = std::get_if<TurnBundlePacket>(&pkt)) {
                if (bundle->seat != seat) continue;
                for (std::size_t j = 0; j < peers_.size(); ++j) {
                    if (j != i && !peerDisconnected_[j] && peers_[j] && peers_[j]->isConnected()) {
                        (void)peers_[j]->sendPacket(LockstepPacket{*bundle});
                    }
                }
                res.localPackets.push_back(LockstepPacket{std::move(*bundle)});
            } else if (auto* chat = std::get_if<ChatPacket>(&pkt)) {
                chat->seat = seat;
                for (std::size_t j = 0; j < peers_.size(); ++j) {
                    if (j == i || peerDisconnected_[j] || !peers_[j] || !peers_[j]->isConnected()) continue;
                    const auto peerSeat = static_cast<std::uint8_t>(j + 1u);
                    if (chat->teamOnly && !allied(seat, peerSeat)) continue;
                    (void)peers_[j]->sendPacket(LockstepPacket{*chat});
                }
                if (!chat->teamOnly || allied(localSeat_, seat)) {
                    res.localPackets.push_back(LockstepPacket{std::move(*chat)});
                }
            } else if (auto* beacon = std::get_if<BeaconPacket>(&pkt)) {
                beacon->seat = seat;
                for (std::size_t j = 0; j < peers_.size(); ++j) {
                    if (j == i || peerDisconnected_[j] || !peers_[j] || !peers_[j]->isConnected()) continue;
                    const auto peerSeat = static_cast<std::uint8_t>(j + 1u);
                    if (beacon->teamOnly && !allied(seat, peerSeat)) continue;
                    (void)peers_[j]->sendPacket(LockstepPacket{*beacon});
                }
                if (!beacon->teamOnly || allied(localSeat_, seat)) {
                    res.localPackets.push_back(LockstepPacket{std::move(*beacon)});
                }
            } else if (const auto* desync = std::get_if<DesyncPacket>(&pkt)) {
                for (std::size_t j = 0; j < peers_.size(); ++j) {
                    if (j != i && !peerDisconnected_[j] && peers_[j] && peers_[j]->isConnected()) {
                        (void)peers_[j]->sendPacket(LockstepPacket{*desync});
                    }
                }
                res.localPackets.push_back(LockstepPacket{*desync});
            } else if (const auto* bye = std::get_if<ByePacket>(&pkt)) {
                sawBye = true;
                if (!bye->reason.empty()) byeReason = bye->reason;
            } else if (const auto* ping = std::get_if<PingPacket>(&pkt)) {
                if (peers_[i]->isConnected()) {
                    (void)peers_[i]->sendPacket(LockstepPacket{PongPacket{ping->seq, ping->sentTimeMs}});
                }
            } else if (const auto* pong = std::get_if<PongPacket>(&pkt)) {
                if (nowMs >= pong->echoedTimeMs) {
                    peerRttMs_[i] = static_cast<int>(std::min<std::uint64_t>(nowMs - pong->echoedTimeMs, 9999u));
                }
            }
        }

        if (sawBye || !ok) {
            peerDisconnected_[i] = true;
            if (peers_[i]) peers_[i]->close();
            res.disconnectedSeats.push_back(seat);
            const ByePacket relayBye{seat, byeReason};
            for (std::size_t j = 0; j < peers_.size(); ++j) {
                if (j != i && !peerDisconnected_[j] && peers_[j] && peers_[j]->isConnected()) {
                    (void)peers_[j]->sendPacket(LockstepPacket{relayBye});
                }
            }
            res.localPackets.push_back(LockstepPacket{relayBye});
        }
    }

    int maxRtt = -1;
    for (std::size_t i = 0; i < peerRttMs_.size(); ++i) {
        if (!peerDisconnected_[i] && peerRttMs_[i] > maxRtt) maxRtt = peerRttMs_[i];
    }
    res.maxRttMs = maxRtt;
    return res;
}

} // namespace ss2
