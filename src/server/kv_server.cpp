#include "server/kv_server.hpp"
#include "protocol/codec.hpp"
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
  for (;;) {
    const ParseResult result = parserRequest(inputBuffer, req);
    if (result == ParseResult::NeedMore) {
      return;  // 半包，等下一次可读事件
    }
    if (result == ParseResult::Error) {
      LOG_WARN("Protocol error from fd={}, closing connection", conn->fd());
      conn->send(protocol::encodeError("ERR Protocol error: invalid request"));
      conn->shutdown();  // 写缓冲排空后关闭，不截断这条错误信息
      return;
    }

    // 这里的 PING / QUIT / UNKNOWN 不碰存储，直接在 IO 线程回复，
    // 避免无谓地跨线程投递，也避免 SET/DEL 的 WAL 排队拖慢探活
    switch (req.cmd) {
      case CommandType::PING:
        conn->send(req.hasArgument ? protocol::encodeBulkString(req.value)
                                   : protocol::encodeSimpleString("PONG"));
        continue;
      case CommandType::QUIT:
        conn->send(protocol::encodeSimpleString("OK"));
        conn->shutdown();
        LOG_DEBUG("Client fd={} sent QUIT, closing", conn->fd());
        return;
      case CommandType::UNKNOWN:
        if (req.wrongArity) {
          conn->send(protocol::encodeError("ERR wrong number of arguments for '" +
                                           req.commandName + "' command"));
        } else if (!req.commandName.empty()) {
          // 未知命令只回错误，绝不断开连接，否则压测工具会整体失败
          conn->send(protocol::encodeError("ERR unknown command '" + req.commandName + "'"));
        }
        continue;  // 空行（commandName 为空）直接忽略，不回复
      default:
        break;
    }

    LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);
    // 写请求必须落 WAL，读请求直接进存储线程
    PendingRequest pending = {req, conn, conn->getLoop()};
    if (req.cmd == CommandType::GET) {
      reqProcessor_.handleRequest(pending);
    } else {
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
