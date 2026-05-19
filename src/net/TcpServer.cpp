#include "TcpServer.hpp"

#include "Acceptor.hpp"
#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include "Log.hpp"
#include "TcpConnection.hpp"
#include <mutex>

namespace net {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr)
    : loop_(loop), acceptor_(loop, listenAddr) {
    acceptor_.setNewConnectionCallback([this](int fd, const InetAddress& peer) {
        (void) peer;  // 当前未使用对端地址，未来可用于日志
        this->newConnection(fd, peer);
    });
}

TcpServer::~TcpServer() {
    for (auto& item : connections_) {
        auto& conn = item.second;
        conn->forceClose();
    }
    connections_.clear();
}

void TcpServer::start() {
    thread_ = std::thread([this] { this->threadFunc(); });
    {
        std::unique_lock<std::mutex> lock(cond_mutex_);
        cond_.wait(lock,[this]{
            return eventloop_ != nullptr;
        });
    }
    // Start accepting on the provided loop (no extra thread by default)
    loop_->runInLoop([this]() { acceptor_.listen(); });
    LOG_INFO("TcpServer listening started");
}

void TcpServer::threadFunc(){
    EventLoop loop;

    {
        std::lock_guard<std::mutex> lock(cond_mutex_);
        eventloop_ = &loop;
    }
    cond_.notify_all();

    loop.loop(10000);
}

void TcpServer::newConnection(int sockfd, const InetAddress& peer) {
    (void) peer; 
    loop_->assertInLoopThread();
    // 设置 conn 的 ioloop
    auto conn = std::make_shared<TcpConnection>(loop_, sockfd);
    if (connectionCallback_) {
        conn->setConnectionCallback(connectionCallback_);
    }
    if (messageCallback_) {
        conn->setMessageCallback(messageCallback_);
    }
    if (writeCompleteCallback_) {
        conn->setWriteCompleteCallback(writeCompleteCallback_);
    }
    conn->setCloseCallback([this](const TcpConnectionPtr& c) {
        if (connectionCallback_) {
            connectionCallback_(c);  // reuse connection callback to report disconnect event
        }
        this->removeConnection(c);
    });
    connections_.emplace(sockfd, conn);
    loop_->runInLoop([conn](){
        conn->connectEstablished();  // 让channel绑定自己,并通知链接建立
    });
    LOG_INFO("new connection fd={} established (total={})", sockfd, connections_.size());
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    int  fd = conn->fd();
    auto it = connections_.find(fd);
    if (it != connections_.end()) {
        connections_.erase(it);
        LOG_INFO("connection fd={} removed (remain={})", fd, connections_.size());
    }
}

}  // namespace Server
