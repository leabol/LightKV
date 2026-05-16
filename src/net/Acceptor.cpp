#include "Acceptor.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "Channel.hpp"
#include "InetAddress.hpp"
#include "Log.hpp"
#include "Socket.hpp"

namespace net {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr)
    : loop_(loop), acceptChannel_(loop, acceptSocket_.fd()) {
    acceptSocket_.setNonblock();
    acceptSocket_.setReuseAddr();
    acceptSocket_.setReusePort();
    acceptSocket_.bindAddr(listenAddr);
    acceptChannel_.setReadCallback([this]() { this->handleRead(); });
}

Acceptor::~Acceptor() {
    acceptChannel_.disableAll();
    acceptChannel_.remove();
};

void Acceptor::listen(int backlog) {
    LOG_INFO("Acceptor starting to listen (backlog={})", backlog);
    acceptSocket_.listen(backlog);
    acceptChannel_.enableReading();
    LOG_INFO("Acceptor listening on fd={}", acceptSocket_.fd());
}

void Acceptor::handleRead() {
    for (;;) {
        InetAddress peeraddr;
        const int   connectFd = acceptSocket_.accept(peeraddr);
        if (connectFd >= 0) {
            LOG_DEBUG("Acceptor accepted new connection: fd={}", connectFd);
            if (newConnectionCb_) {
                newConnectionCb_(connectFd, peeraddr);
            } else {
                LOG_WARN("connectionCallback_ is not set, closing connectFd{}", connectFd);
                ::close(connectFd);
            }
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;  // 本轮无更多连接
        }
        if (errno == EINTR) {
            continue;  // 继续重试
        }
        if (errno == ECONNABORTED) {
            // 三次握手已完成但连接在 accept 前被对端复位，跳过继续
            LOG_WARN("accept ECONNABORTED, continue");
            continue;
        }
        LOG_ERROR("accept failed errno={} msg={}", errno, strerror(errno));
        break;
    }
}
}  // namespace Server
