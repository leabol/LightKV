#include "kv_server.hpp"

#include <mutex>

#include "codec.hpp"
#include "parser.hpp"

namespace server {

KvServer::KvServer(net::EventLoop *loop, const net::InetAddress &listenAddr) :
    server_(loop, listenAddr), dispatcher_(&storage_) {
  server_.setMessageCallback([this](const net::TcpServer::TcpConnectionPtr &conn,
                                    net::Buffer &data) { this->onMessage(conn, data); });
  dispatcher_.registerHandler(
      CommandType::GET, [this](const Request &req, Storage &s) { return this->handleGET(req, s); });
  dispatcher_.registerHandler(
      CommandType::SET, [this](const Request &req, Storage &s) { return this->handleSET(req, s); });
  dispatcher_.registerHandler(
      CommandType::DEL, [this](const Request &req, Storage &s) { return this->handleDEL(req, s); });
}

void KvServer::setThreadNum(int numThreads) {
  server_.setThreadNum(numThreads);
}
void KvServer::start() {
  server_.start();
}

void KvServer::onMessage(const net::TcpServer::TcpConnectionPtr &conn, net::Buffer &inputBuffer) {
  Request req;
  // 直接从接收buffer上解析请求
  if (!parserRequest(inputBuffer, req)) {
    return;
  }
  Response rsp = dispatcher_.dispatch(req);
  std::string rspData = encodeResponse(rsp);
  conn->send(rspData);
}

Response KvServer::handleGET(const Request &req, Storage &storage) {
  std::lock_guard lock(storageMutex_);
  auto it = storage.find(req.key);
  if (it == storage.end()) {
    return {false, "not found key"};
  }
  return {true, it->second};
}

Response KvServer::handleSET(const Request &req, Storage &storage) {
  std::lock_guard lock(storageMutex_);
  storage[req.key] = req.value;
  return {true, "ok"};
}

Response KvServer::handleDEL(const Request &req, Storage &storage) {
  std::lock_guard lock(storageMutex_);
  auto it = storage.find(req.key);
  if (it == storage.end()) {
    return {false, "not found key"};
  }
  storage.erase(it);
  return {true, "ok"};
}

}  // namespace server
