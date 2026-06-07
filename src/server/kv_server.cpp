#include "server/kv_server.hpp"
#include "protocol/codec.hpp"
#include "protocol/parser.hpp"
#include "protocol/request.hpp"
#include "storage/wal/log_record.hpp"
#include "storage/wal/wal_writer.hpp"
#include "storage/wal/wal_reader.hpp"
#include "util/Log.hpp"

namespace server {

KvServer::KvServer(net::EventLoop *loop, const net::InetAddress &listenAddr, const std::filesystem::path& walPath) :
    server_(loop, listenAddr), dispatcher_(&memtable_), walWriter_(walPath) {
  // 启动时从WAL恢复数据
  LOG_INFO("Recovering from WAL: {}", walPath.string());
  wal::WALReader::Recover(walPath, memtable_);
  LOG_INFO("WAL recovery done");

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
  if (!parserRequest(inputBuffer, req)) {
    return;
  }
  LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);
  if (req.cmd == CommandType::SET || req.cmd == CommandType::DEL) {
    wal::LogRecord record{wal::FromCommandType(req.cmd), req.key, req.value};
    walWriter_.append(record);
    LOG_DEBUG("WAL wrote: type={} key={}", static_cast<int>(record.type), req.key);
  }
  Response rsp = dispatcher_.dispatch(req);
  LOG_DEBUG("response: ok={} value={}", rsp.ok, rsp.value);
  std::string rspData = encodeResponse(rsp);
  conn->send(rspData);
}

}  // namespace server
