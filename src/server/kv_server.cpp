#include "server/kv_server.hpp"
#include "protocol/parser.hpp"
#include "protocol/request.hpp"
#include "server/request_processor.hpp"
#include "server/server_def.hpp"
#include "storage/wal/wal_reader.hpp"
#include "util/Log.hpp"

namespace server {

KvServer::KvServer(net::EventLoop *loop, const net::InetAddress &listenAddr, const std::filesystem::path& walPath) :
  server_(loop, listenAddr), walPath_(walPath){
  // 设置消息回调，IO 线程收到请求后转发到存储线程
  server_.setMessageCallback([this](const net::TcpServer::TcpConnectionPtr &conn,
                                    net::Buffer &data) { this->onMessage(conn, data); });
}

void KvServer::setThreadNum(int numThreads) {
  server_.setThreadNum(numThreads);
}

void KvServer::start() {
  initWAL();
  writeCoordinator_.setWALWriter(walWriter_.get());
  writeCoordinator_.start();
  walWriter_->setprocessor(&this->reqProcessor_);
  reqProcessor_.start();
  
  server_.start();
}

void KvServer::onMessage(const net::TcpServer::TcpConnectionPtr &conn, net::Buffer &inputBuffer) {
  Request req;
  while (parserRequest(inputBuffer, req)) {
    LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);
    // 将请求投递到存储线程处理（IO 线程不直接操作 memtable_）
    PendingRequest pending = {req,conn, conn->getLoop()};
    if (req.cmd == CommandType::GET){
      reqProcessor_.handleRequest(pending);
    }else {
      writeCoordinator_.submitWriteReq(std::move(pending));
    }
  }
}
void KvServer::initWAL() {
  LOG_INFO("Recovering from WAL: {}", walPath_.string());

  wal::WALReader::Recover(walPath_, reqProcessor_.getMemtable());

  walWriter_ = std::make_unique<wal::WALWriter>(walPath_);
  LOG_INFO("WAL recovery done, storage ready");
}
}  // namespace server
