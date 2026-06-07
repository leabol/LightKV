#include "server/kv_server.hpp"
#include "protocol/codec.hpp"
#include "protocol/parser.hpp"

namespace server {

KvServer::KvServer(net::EventLoop *loop, const net::InetAddress &listenAddr) :
    server_(loop, listenAddr), dispatcher_(&memtable_) {
  server_.setMessageCallback([this](const net::TcpServer::TcpConnectionPtr &conn,
                                    net::Buffer &data) { this->onMessage(conn, data); });
  dispatcher_.registerHandler(
      CommandType::GET, [this](const Request &req) { return memtable_.GET(req); });
  dispatcher_.registerHandler(
      CommandType::SET, [this](const Request &req) { return memtable_.SET(req); });
  dispatcher_.registerHandler(
      CommandType::DEL, [this](const Request &req) { return memtable_.DEL(req); });
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

}  // namespace server
