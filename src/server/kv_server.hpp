#pragma once
#include <netinet/tcp.h>

#include <mutex>
#include <string>
#include <unordered_map>

#include "TcpConnection.hpp"
#include "TcpServer.hpp"
#include "buffer.hpp"
#include "dispatcher.hpp"
#include "request.hpp"
#include "response.hpp"

namespace server {
using Storage = std::unordered_map<std::string, std::string>;
using namespace protocol;

class KvServer {
public:
  explicit KvServer(net::EventLoop* loop, const net::InetAddress& listenAddr);

  // Start listening
  void start();

  void onMessage(const net::TcpServer::TcpConnectionPtr& conn, net::Buffer& inputBuffer);
  void setThreadNum(int numThreads);

private:
  Response handleGET(const Request& req, Storage& storage);
  Response handleSET(const Request& req, Storage& storage);
  Response handleDEL(const Request& req, Storage& storage);

  std::mutex storageMutex_;
  Storage storage_;
  net::TcpServer server_;
  Dispatcher dispatcher_;
};
}  // namespace server