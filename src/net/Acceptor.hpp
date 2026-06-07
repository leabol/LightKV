#pragma once
#include <sys/socket.h>

#include <functional>

#include "net/Channel.hpp"
#include "net/Socket.hpp"

namespace net {

class EventLoop;
class InetAddress;

class Acceptor {
public:
  using NewConnectionCallback = std::function<void(int, const InetAddress&)>;

  Acceptor(EventLoop* loop, const InetAddress& addr);
  ~Acceptor();

  Acceptor(const Acceptor&) = delete;
  Acceptor& operator=(const Acceptor&) = delete;
  Acceptor(Acceptor&&) = delete;
  Acceptor& operator=(Acceptor&&) = delete;

  void setNewConnectionCallback(NewConnectionCallback cb) { newConnectionCb_ = std::move(cb); }

  void listen(int backlog = SOMAXCONN);
  bool listening() const { return listening_; }

private:
  void handleRead();

  EventLoop* loop_{nullptr};
  bool listening_{false};
  int idleFd_{-1};
  Socket acceptSocket_;                    // 监听socket
  Channel acceptChannel_;                  // 用来接收socket, 通过使用handleRead回调
  NewConnectionCallback newConnectionCb_;  // 具体处理连接的socket
};
}  // namespace net
