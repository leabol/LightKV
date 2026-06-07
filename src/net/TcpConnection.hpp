#pragma once
#include <functional>
#include <memory>
#include <string>

#include "net/buffer.hpp"


namespace net {

class Buffer;
class Socket;
class Channel;
class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
  using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
  using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;        // 建立 / 关闭
  using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer&)>;  // 可读数据
  using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;  // 发送缓冲清空

  TcpConnection(EventLoop* loop, std::unique_ptr<Socket> sock);
  // 便捷重载：从现有 fd 构造（内部包装为 Socket）
  TcpConnection(EventLoop* loop, int fd);
  ~TcpConnection();

  // 禁止拷贝，允许移动（通常不移动，连接由智能指针管理）
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  // 用户/框架设置回调
  void setConnectionCallback(ConnectionCallback cb) { connectionCallback_ = std::move(cb); }
  void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
  void setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
  }
  void setCloseCallback(ConnectionCallback cb) { closeCallback_ = std::move(cb); }

  // 由外部（Acceptor 或 Connector 完成后）调用，触发 "已建立" 逻辑
  void connectEstablished();
  // 关闭流程（优雅关闭）
  void shutdown();
  // 强制立即关闭
  void forceClose();

  // 发送数据（追加到发送缓冲，注册写事件）
  void send(const std::string& data);

  EventLoop* getLoop() const { return loop_; }
  int fd() const;  // 便捷

private:
  enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };
  void setState(StateE s) { state_ = s; }

  // Channel 回调入口
  void handleRead();
  void handleWrite();
  void handleClose();
  void handleError();

  EventLoop* loop_{nullptr};
  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;

  StateE state_{kConnecting};

  net::Buffer outputBuffer_;
  net::Buffer inputBuffer_;

  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  WriteCompleteCallback writeCompleteCallback_;
  ConnectionCallback closeCallback_;
};
}  // namespace net
