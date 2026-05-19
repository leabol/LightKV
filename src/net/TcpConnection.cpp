#include "TcpConnection.hpp"

#include <cassert>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Log.hpp"
#include "Socket.hpp"
#include "buffer.hpp"

namespace net {

TcpConnection::TcpConnection(EventLoop* loop, std::unique_ptr<Socket> sock)
    : loop_(loop)
    , socket_(std::move(sock))
    , channel_(std::make_unique<Channel>(loop_, socket_->fd()))
    , outputBuffer_(net::kBufferSize, net::kMaxSize) 
    , inputBuffer_(net::kBufferSize, net::kMaxSize) {
    // 绑定事件回调
    channel_->setReadCallback([this] { handleRead(); });
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setCloseCallback([this] { handleClose(); });
}

TcpConnection::TcpConnection(EventLoop* loop, int fd)
    : TcpConnection(loop, std::make_unique<Socket>(fd)) {}

TcpConnection::~TcpConnection() = default;

int TcpConnection::fd() const {
    return socket_ ? socket_->fd() : -1;
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    assert(state_ == kConnecting);
    setState(kConnected);
    LOG_DEBUG("TcpConnection fd={} established", fd());
    auto self = shared_from_this();
    channel_->tie(self);  // 生命周期守卫
    channel_->enableReading();
    if (connectionCallback_) {
        connectionCallback_(self);  // 通知连接建立
    }
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        setState(kDisconnecting);
        if (!channel_->isWriting()) {
            ::shutdown(fd(), SHUT_WR);
            LOG_INFO("TcpConnection fd={} shutdown (write closed)", fd());
        } else {
            LOG_DEBUG("TcpConnection fd={} shutdown pending (has data to write)", fd());
        }
    }
}

void TcpConnection::forceClose() {
    if (state_ == kConnected || state_ == kDisconnecting) {
        LOG_WARN("TcpConnection fd={} force close", fd());
        handleClose();
    }
}

void TcpConnection::send(const std::string& data) {
    if (state_ != kConnected) {
        LOG_WARN("TcpConnection fd={} send failed: not connected", fd());
        return;
    }
    LOG_TRACE("TcpConnection fd={} sending {} bytes", fd(), data.size());
    if (!channel_->isWriting() && outputBuffer_.empty()) {
        // 尝试直接写
        ssize_t n = ::send(fd(), data.data(), data.size(), 0);
        if (n >= 0) {
            size_t sent = static_cast<size_t>(n);
            if (sent < data.size()) {
                LOG_TRACE("TcpConnection fd={} partial send: {}/{} bytes, buffering remaining",
                          fd(),
                          sent,
                          data.size());
                outputBuffer_.append(data.data() + sent, data.size() - sent);
                channel_->enableWriting();
            } else {
                LOG_TRACE("TcpConnection fd={} sent all {} bytes directly", fd(), sent);
                if (writeCompleteCallback_) {
                    auto self = shared_from_this();
                    writeCompleteCallback_(self);
                }
            }
        } else {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                outputBuffer_.append(data);
                channel_->enableWriting();
            } else {
                LOG_ERROR("send error fd={} errno={} msg={}", fd(), errno, strerror(errno));
                handleError();
            }
        }
    } else {
        outputBuffer_.append(data);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::handleRead() {
    static constexpr size_t kPerRecvSize = 64*1024;
    for (;;) {
        void* pWrite = static_cast<void*>(inputBuffer_.beginWrite(kPerRecvSize));
        ssize_t n = ::recv(fd(), pWrite, kPerRecvSize, 0);
        if (n > 0) {
            inputBuffer_.updateWrite(n);
            LOG_TRACE("TcpConnection fd={} received {} bytes", fd(), n);
            if (messageCallback_) {
              auto self = shared_from_this();
              messageCallback_(self, inputBuffer_);
            }
            continue;
        }

        if (n == 0) {  // 对端关闭
            LOG_INFO("TcpConnection fd={} peer closed", fd());
            handleClose();
            break;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            break;  // 已无数据
        }
        if (errno == EINTR) {
            continue;  // 重试
        }
        // Do not use 'else' after 'break'
        LOG_ERROR("recv error fd={} errno={} msg={}", fd(), errno, strerror(errno));
        handleError();
        break;
    }
}

void TcpConnection::handleWrite() {
    if (!channel_->isWriting()) {
        return;
    }
    LOG_TRACE("TcpConnection fd={} writing buffered data ({} bytes)", fd(), outputBuffer_.readableByte());
    while (!outputBuffer_.empty()) {
        void *pRead = static_cast<void*>(outputBuffer_.readPoint());
        ssize_t n = ::send(fd(), pRead, outputBuffer_.readableByte(), 0);
        if (n > 0) {
            outputBuffer_.consume(static_cast<size_t>(n));
            if (outputBuffer_.empty()) {
                LOG_TRACE("TcpConnection fd={} write buffer emptied", fd());
                channel_->disableWriting();
                if (writeCompleteCallback_) {
                    auto self = shared_from_this();
                    writeCompleteCallback_(self);
                }
                if (state_ == kDisconnecting) {
                    ::shutdown(fd(), SHUT_WR);
                }
                break;
            }
        } else {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // 下次再写
                break;
            }
            if (errno == EINTR) {
                continue;  // 重试
            }
            LOG_ERROR("send(write) error fd={} errno={} msg={}", fd(), errno, strerror(errno));
            handleError();
            break;
        }
    }
}

void TcpConnection::handleClose() {
    if (state_ == kDisconnected) {
        return;
    }
    LOG_INFO("TcpConnection fd={} closing", fd());
    setState(kDisconnected);
    channel_->disableAll();
    // channel_->remove(); // 可选：若需要主动从 Loop 移除
    if (closeCallback_) {
        auto self = shared_from_this();
        closeCallback_(self);
    } else if (connectionCallback_) {
        auto self = shared_from_this();
        connectionCallback_(self);  // 也可分离为 onConnected/onDisconnected
    }
}

void TcpConnection::handleError() {
    // 发生严重错误时直接关闭
    LOG_ERROR("TcpConnection fd={} encountered error, force closing", fd());
    forceClose();
}

}  // namespace Server
