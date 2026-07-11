#include "server/kv_server.hpp"
#include "protocol/parser.hpp"
#include "protocol/request.hpp"
#include "util/Log.hpp"

namespace server {

KvServer::KvServer(net::EventLoop *loop, const net::InetAddress &listenAddr, const std::filesystem::path& walPath) :
  storage_(walPath), server_(loop, listenAddr) {
  // 设置消息回调，IO 线程收到请求后转发到存储线程
  server_.setMessageCallback([this](const net::TcpServer::TcpConnectionPtr &conn,
                                    net::Buffer &data) { this->onMessage(conn, data); });
}

void KvServer::setThreadNum(int numThreads) {
  server_.setThreadNum(numThreads);
}

void KvServer::start() {
  storage_.start();
  server_.start();
}

void KvServer::onMessage(const net::TcpServer::TcpConnectionPtr &conn, net::Buffer &inputBuffer) {
  Request req;
  if (!parserRequest(inputBuffer, req)) {
    return;
  }
  LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);

  // 将请求投递到存储线程处理（IO 线程不直接操作 memtable_）
  storage_.handleRequest(req, conn, conn->getLoop());
}

}  // namespace server
