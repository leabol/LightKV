#pragma once
#include <netinet/tcp.h>
#include <filesystem>

#include "net/TcpConnection.hpp"
#include "net/TcpServer.hpp"
#include "net/buffer.hpp"
#include "server/storage.hpp"

namespace server {

class KvServer {
public:
  explicit KvServer(net::EventLoop* loop, const net::InetAddress& listenAddr, const std::filesystem::path& walPath = "wal");

  // Start listening
  void start();

  void onMessage(const net::TcpServer::TcpConnectionPtr& conn, net::Buffer& inputBuffer);
  void setThreadNum(int numThreads);

private:
  Storage storage_;
  net::TcpServer server_;
};
}  // namespace server
