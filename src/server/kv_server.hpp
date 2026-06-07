#pragma once
#include <netinet/tcp.h>

#include "net/TcpConnection.hpp"
#include "net/TcpServer.hpp"
#include "net/buffer.hpp"
#include "server/dispatcher.hpp"

namespace server {

class KvServer {
public:
  explicit KvServer(net::EventLoop* loop, const net::InetAddress& listenAddr);

  // Start listening
  void start();

  void onMessage(const net::TcpServer::TcpConnectionPtr& conn, net::Buffer& inputBuffer);
  void setThreadNum(int numThreads);

private:
  storage::Memtable memtable_;
  net::TcpServer server_;
  Dispatcher dispatcher_;
};
}  // namespace server
