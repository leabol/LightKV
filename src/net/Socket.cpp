#include "Socket.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "InetAddress.hpp"
#include "Log.hpp"

using namespace net;

Socket::Socket(int socketfd) : socketfd_(socketfd) {}

Socket::Socket() {
    socketfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
}

Socket::~Socket() {
    if (socketfd_ != -1) {
        ::close(socketfd_);
    }
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (socketfd_ != -1) {
            ::close(socketfd_);
        }
        socketfd_       = other.socketfd_;
        other.socketfd_ = -1;
    }
    return *this;
}

void Socket::bindAddr(const InetAddress& addr) const {
    const struct addrinfo* addrlist = addr.getAddrinfoList();
    const struct addrinfo* rp;

    LOG_INFO("Trying binding");
    for (rp = addrlist; rp != nullptr; rp = rp->ai_next) {
        if (bind(socketfd_, rp->ai_addr, rp->ai_addrlen) == 0) {
            LOG_INFO("Successfully bound (family={}, len={})",
                     rp->ai_family,
                     static_cast<int>(rp->ai_addrlen));
            break;
        }
    }
    if (rp == nullptr) {
        LOG_ERROR("Server failed bind");
        throw std::runtime_error("Failed bind");
    }
}

// backlog表示完成三次握手后的请求队列的最大长度
void Socket::listen(int backlog) const {
    if (::listen(socketfd_, backlog) == -1) {
        throw std::runtime_error("listen failed: " + std::string(strerror(errno)));
    }
}

int Socket::accept(InetAddress& peeraddr) const {
    struct sockaddr_storage ss;
    socklen_t               len = sizeof(ss);

    int connfd =
        ::accept4(socketfd_, reinterpret_cast<sockaddr*>(&ss), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (connfd == -1) {
        // 将常见的可恢复错误返回给调用方，由上层循环处理
        // - EAGAIN/EWOULDBLOCK：非阻塞监听套接字暂无新连接
        // - EINTR：被信号中断，可重试
        // 其他错误也交由上层决定是否继续或记录日志
        return -1;
    }

    // 直接用对端 sockaddr 构造，避免二次解析
    InetAddress tmp{reinterpret_cast<sockaddr*>(&ss), len};
    peeraddr = std::move(tmp);

    return connfd;
}

void Socket::setNonblock(bool on) const {
    int flags = ::fcntl(socketfd_, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error("fcntl(F_GETFL) failed: " + std::string(strerror(errno)));
    }
    int newFlags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(socketfd_, F_SETFL, newFlags) == -1) {
        throw std::runtime_error("fcntl(F_SETFL) O_NONBLOCK failed: " +
                                 std::string(strerror(errno)));
    }
}

void Socket::setTcpNoDelay(bool on) const {
    int val = on ? 1 : 0;
    if (::setsockopt(socketfd_, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1) {
        throw std::runtime_error("setsockopt(TCP_NODELAY) failed: " + std::string(strerror(errno)));
    }
}

void Socket::setReuseAddr(bool on) const {
    int val = on ? 1 : 0;
    if (::setsockopt(socketfd_, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == -1) {
        throw std::runtime_error("setsockopt(SO_REUSEADDR) failed: " +
                                 std::string(strerror(errno)));
    }
}

void Socket::setReusePort(bool on) const {
    int val = on ? 1 : 0;
    if (::setsockopt(socketfd_, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)) == -1) {
        if (errno == ENOPROTOOPT || errno == EINVAL) {
            if (on) {
                LOG_WARN("SO_REUSEPORT unsupported at runtime");
            }
            return;
        }
        throw std::runtime_error("setsockopt(SO_REUSEPORT) failed: " +
                                 std::string(strerror(errno)));
    }
}

void Socket::setKeepAlive(bool on) const {
    int val = on ? 1 : 0;
    if (::setsockopt(socketfd_, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
        throw std::runtime_error("setsockopt(SO_KEEPALIVE) failed: " +
                                 std::string(strerror(errno)));
    }
}

void Socket::connect(const InetAddress& serveraddr) const {
    const struct addrinfo* list = serveraddr.getAddrinfoList();
    const struct addrinfo* rp   = nullptr;

    int last_errno = 0;
    for (rp = list; rp != nullptr; rp = rp->ai_next) {
        if (::connect(socketfd_, rp->ai_addr, rp->ai_addrlen) == 0) {
            LOG_INFO(
                "Connected to server (family={}, len={})", rp->ai_family, (int) rp->ai_addrlen);
            return;
        }
        last_errno = errno;
    }
    throw std::runtime_error(std::string("connect failed: ") + ::strerror(last_errno));
}

void Socket::connect(std::string_view host, std::string_view port) const {
    InetAddress addr{std::string(host), std::string(port)};
    Socket::connect(static_cast<const InetAddress&>(addr));
}

void Socket::bindAddr(std::string_view host, std::string_view port) const {
    InetAddress addr{std::string(host), std::string(port)};
    bindAddr(static_cast<const InetAddress&>(addr));
}

void Socket::bindAddr(std::string_view port) const {
    InetAddress any{std::string(port)};
    bindAddr(static_cast<const InetAddress&>(any));
}
