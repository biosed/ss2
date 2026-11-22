#include "net/tcp_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace ss2 {
namespace {

bool setNonBlockingAndNoDelay(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
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
    : fd_(std::exchange(other.fd_, -1)),
      connected_(std::exchange(other.connected_, false)),
      connecting_(std::exchange(other.connecting_, false)),
      peerAddress_(std::move(other.peerAddress_)),
      lastError_(std::move(other.lastError_)),
      txBuf_(std::move(other.txBuf_)),
      rxBuf_(std::move(other.rxBuf_)) {}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        connected_ = std::exchange(other.connected_, false);
        connecting_ = std::exchange(other.connecting_, false);
        peerAddress_ = std::move(other.peerAddress_);
        lastError_ = std::move(other.lastError_);
        txBuf_ = std::move(other.txBuf_);
        rxBuf_ = std::move(other.rxBuf_);
    }
    return *this;
}

TcpConnection TcpConnection::adoptConnectedSocket(int fd, std::string peerAddr) {
    TcpConnection c;
    c.fd_ = fd;
    c.connected_ = (fd >= 0);
    c.connecting_ = false;
    c.peerAddress_ = std::move(peerAddr);
    if (fd >= 0 && !setNonBlockingAndNoDelay(fd)) {
        c.lastError_ = std::strerror(errno);
        c.close();
    }
    return c;
}

TcpConnection TcpConnection::connectNonBlocking(const std::string& host, std::uint16_t port) {
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

    int lastErr = ECONNREFUSED;
    for (struct addrinfo* it = res; it != nullptr; it = it->ai_next) {
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            lastErr = errno;
            continue;
        }
        if (!setNonBlockingAndNoDelay(fd)) {
            lastErr = errno;
            ::close(fd);
            continue;
        }
        const int rc = ::connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc == 0) {
            conn.fd_ = fd;
            conn.connected_ = true;
            conn.connecting_ = false;
            conn.peerAddress_ = formatSockAddr(it->ai_addr, it->ai_addrlen);
            ::freeaddrinfo(res);
            return conn;
        }
        if (errno == EINPROGRESS) {
            conn.fd_ = fd;
            conn.connected_ = false;
            conn.connecting_ = true;
            conn.peerAddress_ = formatSockAddr(it->ai_addr, it->ai_addrlen);
            ::freeaddrinfo(res);
            return conn;
        }
        lastErr = errno;
        ::close(fd);
    }
    ::freeaddrinfo(res);
    conn.lastError_ = std::strerror(lastErr);
    return conn;
}

bool TcpConnection::pollConnected() {
    if (fd_ < 0) return false;
    if (connected_) return true;
    if (!connecting_) return false;

    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLOUT;
    const int rc = ::poll(&pfd, 1, 0);
    if (rc < 0) {
        if (errno == EINTR) return false;
        lastError_ = std::strerror(errno);
        close();
        return false;
    }
    if (rc == 0) return false;

    int err = 0;
    socklen_t errLen = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &errLen) < 0) {
        lastError_ = std::strerror(errno);
        close();
        return false;
    }
    if (err != 0) {
        lastError_ = std::strerror(err);
        close();
        return false;
    }
    connected_ = true;
    connecting_ = false;
    return true;
}

bool TcpConnection::sendPacket(const LockstepPacket& packet) {
    if (fd_ < 0) return false;
    const auto frame = encodeLockstepPacket(packet);
    txBuf_.insert(txBuf_.end(), frame.begin(), frame.end());
    return flush();
}

bool TcpConnection::flush() {
    if (fd_ < 0) return false;
    if (!connected_ && !pollConnected()) return !connecting_ ? false : true;
    std::size_t sentOffset = 0;
    while (sentOffset < txBuf_.size()) {
        const ssize_t n = ::send(fd_, txBuf_.data() + sentOffset, txBuf_.size() - sentOffset, MSG_NOSIGNAL);
        if (n > 0) {
            sentOffset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR)) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        lastError_ = n == 0 ? "Connection closed during send" : std::strerror(errno);
        close();
        return false;
    }
    if (sentOffset > 0) {
        txBuf_.erase(txBuf_.begin(), txBuf_.begin() + static_cast<std::ptrdiff_t>(sentOffset));
    }
    return true;
}

bool TcpConnection::pollPackets(std::vector<LockstepPacket>& outPackets) {
    if (fd_ < 0) return false;
    if (!connected_ && !pollConnected()) return connecting_;
    (void)flush();
    if (fd_ < 0) return false;

    std::array<std::uint8_t, 16384> tmp{};
    for (;;) {
        const ssize_t n = ::recv(fd_, tmp.data(), tmp.size(), 0);
        if (n > 0) {
            rxBuf_.insert(rxBuf_.end(), tmp.begin(), tmp.begin() + n);
            continue;
        }
        if (n == 0) {
            lastError_ = "Peer closed connection";
            close();
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        lastError_ = std::strerror(errno);
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
    return fd_ >= 0;
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    connected_ = false;
    connecting_ = false;
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), lastError_(std::move(other.lastError_)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        lastError_ = std::move(other.lastError_);
    }
    return *this;
}

bool TcpListener::listen(std::uint16_t port, const std::string& bindAddr) {
    close();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        lastError_ = std::strerror(errno);
        return false;
    }
    const int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (!setNonBlockingAndNoDelay(fd)) {
        lastError_ = std::strerror(errno);
        ::close(fd);
        return false;
    }
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bindAddr.empty() || bindAddr == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) {
        lastError_ = "Invalid bind address: " + bindAddr;
        ::close(fd);
        return false;
    }
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 8) < 0) {
        lastError_ = std::strerror(errno);
        ::close(fd);
        return false;
    }
    fd_ = fd;
    lastError_.clear();
    return true;
}

std::uint16_t TcpListener::boundPort() const {
    if (fd_ < 0) return 0;
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

std::optional<TcpConnection> TcpListener::pollAccept() {
    if (fd_ < 0) return std::nullopt;
    struct sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    const int clientFd = ::accept(fd_, reinterpret_cast<struct sockaddr*>(&ss), &len);
    if (clientFd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            lastError_ = std::strerror(errno);
        }
        return std::nullopt;
    }
    auto conn = TcpConnection::adoptConnectedSocket(
        clientFd, formatSockAddr(reinterpret_cast<const struct sockaddr*>(&ss), len));
    if (!conn.isOpen()) return std::nullopt;
    return conn;
}

void TcpListener::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool MultiplayerHandshake::startHost(std::uint16_t port,
                                     MatchSetupPacket matchTemplate,
                                     std::string bindAddr) {
    cancel();
    isHost_ = true;
    setup_ = std::move(matchTemplate);
    if (setup_.playerCount < 2u || setup_.playerCount > 4u) setup_.playerCount = 2u;
    if (!listener_.listen(port, bindAddr)) {
        state_ = State::Failed;
        errorText_ = listener_.lastError();
        statusText_ = "Listen failed: " + errorText_;
        return false;
    }
    state_ = State::Listening;
    statusText_ = "Listening on port " + std::to_string(listener_.boundPort()) + "...";
    return true;
}

bool MultiplayerHandshake::startJoin(const std::string& host,
                                     std::uint16_t port,
                                     std::string playerName,
                                     std::uint64_t expectedMapHash) {
    cancel();
    isHost_ = false;
    playerName_ = std::move(playerName);
    expectedMapHash_ = expectedMapHash;
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
        if (state_ == State::Listening) {
            if (auto accepted = listener_.pollAccept()) {
                conn_ = std::make_shared<TcpConnection>(std::move(*accepted));
                listener_.close();
                state_ = State::Exchanging;
                statusText_ = "Peer connected from " + conn_->peerAddress() + "; waiting handshake...";
            }
            return state_;
        }
        if (state_ == State::Exchanging) {
            std::vector<LockstepPacket> packets;
            if (!conn_ || !conn_->pollPackets(packets)) {
                state_ = State::Failed;
                errorText_ = conn_ ? conn_->lastError() : "Lost peer connection";
                statusText_ = "Handshake failed: " + errorText_;
                return state_;
            }
            for (const auto& pkt : packets) {
                if (const auto* hello = std::get_if<HelloPacket>(&pkt)) {
                    if (hello->protocolVersion != kLockstepProtocolVersion) {
                        (void)conn_->sendPacket(ByePacket{0u, "Protocol version mismatch"});
                        state_ = State::Failed;
                        errorText_ = "Protocol version mismatch";
                        statusText_ = errorText_;
                        return state_;
                    }
                    setup_.assignedSeat = 1u;
                    if (!conn_->sendPacket(setup_)) {
                        state_ = State::Failed;
                        errorText_ = conn_->lastError();
                        statusText_ = "Failed sending MatchSetup: " + errorText_;
                        return state_;
                    }
                    state_ = State::Ready;
                    statusText_ = "Connected (Seat 0 Host)";
                    return state_;
                }
                if (const auto* bye = std::get_if<ByePacket>(&pkt)) {
                    state_ = State::Failed;
                    errorText_ = bye->reason.empty() ? "Peer cancelled handshake" : bye->reason;
                    statusText_ = errorText_;
                    return state_;
                }
            }
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
            if (!conn_ || !conn_->sendPacket(HelloPacket{kLockstepProtocolVersion, 1u, playerName_})) {
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
                statusText_ = "Connected (Seat " + std::to_string(setup_.assignedSeat) + ")";
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
    listener_.close();
    if (conn_) {
        if (conn_->isConnected()) {
            (void)conn_->sendPacket(ByePacket{isHost_ ? std::uint8_t{0} : setup_.assignedSeat, "Cancelled"});
        }
        conn_->close();
        conn_.reset();
    }
    helloSent_ = false;
    state_ = State::Idle;
    errorText_.clear();
}

std::uint16_t MultiplayerHandshake::listeningPort() const {
    return listener_.boundPort();
}

std::shared_ptr<TcpConnection> MultiplayerHandshake::takeConnection() {
    return std::move(conn_);
}

} // namespace ss2
