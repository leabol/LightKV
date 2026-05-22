#include "TcpServer.hpp"
#include <unistd.h>

#include <memory>

#include "Acceptor.hpp"
#include "EventLoop.hpp"
#include "EventLoopThread.hpp"
#include "EventLoopThreadPool.hpp"
#include "InetAddress.hpp"
#include "Log.hpp"
#include "TcpConnection.hpp"

namespace net {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr) :
    loop_(loop), acceptor_(loop, listenAddr),
    threadPools_(loop) {
  acceptor_.setNewConnectionCallback([this](int fd, const InetAddress& peer) {
    (void)peer;  // 当前未使用对端地址，未来可用于日志
    this->newConnection(fd, peer);
  });
}

TcpServer::~TcpServer() {
  loop_->assertInLoopThread();
  LOG_TRACE("~TcpServer");

  for (auto& item : connections_) {
    auto& conn = item.second;
    conn->getLoop()->runInLoop([conn] { conn->forceClose(); });
  }
  connections_.clear();
}

void TcpServer::start() {
  threadPools_.start(ThreadInitCallback_);

  assert(!acceptor_.listening());
  loop_->runInLoop([this] { acceptor_.listen(); });
  LOG_INFO("TcpServer listening started");
}

void TcpServer::setThreadNum(int numThreads) {
  assert(numThreads >= 0);
  threadPools_.setThreadNum(numThreads);
}

void TcpServer::newConnection(int sockfd, const InetAddress& peer) {
  (void)peer;
  loop_->assertInLoopThread();
  EventLoop* ioLoop = threadPools_.getNextLoop();
  // 设置 conn 的 ioloop
  auto conn = std::make_shared<TcpConnection>(ioLoop, sockfd);
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
    loop_->runInLoop([this, c]{
      this->removeConnection(c);
    });
  });
  connections_.emplace(sockfd, conn);
  // 添加到ioloop的pending队列中
  ioLoop->runInLoop([conn]() {
    conn->connectEstablished();  // 让channel绑定自己,并通知链接建立
  });
  LOG_INFO("new connection fd={} established (total={})", sockfd, connections_.size());
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
  int fd = conn->fd();
  auto it = connections_.find(fd);
  if (it != connections_.end()) {
    connections_.erase(it);
    LOG_INFO("connection fd={} removed (remain={})", fd, connections_.size());
  }
}

}  // namespace net
