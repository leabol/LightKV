#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "Acceptor.hpp"
#include "EventLoopThread.hpp"
#include "EventLoopThreadPool.hpp"
#include "TcpConnection.hpp"

namespace net {

class EventLoop;
class InetAddress;
class EventLoopThread;

// 单线程 TcpServer：接受新连接并创建 TcpConnection，转发回调。
class TcpServer {
  public:
    using TcpConnectionPtr      = std::shared_ptr<TcpConnection>;
    using ConnectionCallback    = TcpConnection::ConnectionCallback;     // 建立/关闭
    using MessageCallback       = TcpConnection::MessageCallback;        // 收到数据
    using WriteCompleteCallback = TcpConnection::WriteCompleteCallback;  // 发送完成
    using ThreadInitCallback    = std::function<void(EventLoop*)>;

    TcpServer(EventLoop* loop, const InetAddress& listenAddr);
    ~TcpServer();

    TcpServer(const TcpServer&)            = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }
    void setWriteCompleteCallback(WriteCompleteCallback cb) {
        writeCompleteCallback_ = std::move(cb);
    }

    // 开始监听
    void start();
    void threadFunc();
  private:
    void newConnection(int sockfd, const InetAddress& peer);  // Acceptor 回调
    void removeConnection(const TcpConnectionPtr& conn);      // 连接关闭回调

    EventLoop* loop_{nullptr};
    Acceptor   acceptor_;  // 监听 + 接收
    std::shared_ptr<EventLoopThreadPool> threadPools_;
    EventLoop* eventloop_{nullptr};
    
    std::unique_ptr<EventLoopThread> thread_;

    std::unordered_map<int, TcpConnectionPtr> connections_;  // fd -> 连接

    ConnectionCallback    connectionCallback_;  // 用户设置（可能为空）
    MessageCallback       messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback    ThreadInitCallback_;
};
}  // namespace Server
