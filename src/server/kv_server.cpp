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
  // QUIT 之后仍在同一连接上收到的字节必须全部丢弃（RESP 语义：QUIT 后的命令不执行）。
  // 客户端可能不会立刻关连接，甚至继续 pipeline，这些数据都不能进解析器。
  if (conn->quitReceived()) {
    inputBuffer.consume(inputBuffer.readableByte());
    return;
  }

  Request req;
  for (;;) {
    const ParseResult result = parserRequest(inputBuffer, req);
    if (result == ParseResult::NeedMore) {
      return;  // 半包，等下一次可读事件
    }
    if (result == ParseResult::Error) {
      LOG_WARN("Protocol error from fd={}, closing connection", conn->fd());
      // 走有序通道：前面 pipeline 里可能还有未输出的回复，这条错误必须排在它们之后。
      const uint64_t seq = conn->nextRequestSeq();
      conn->sendOrderedReply(seq, protocol::encodeError("ERR Protocol error: invalid request"),
                             /*closeAfter=*/true);
      // 剩下的字节全是垃圾，直接丢弃，避免再次解析
      inputBuffer.consume(inputBuffer.readableByte());
      return;
    }

    // 这里的 PING / QUIT / UNKNOWN 不碰存储，直接在 IO 线程回复，
    // 但回复仍必须按请求顺序输出，所以统一走 sendOrderedReply。
    const uint64_t seq = conn->nextRequestSeq();
    switch (req.cmd) {
      case CommandType::PING:
        conn->sendOrderedReply(seq, req.hasArgument ? protocol::encodeBulkString(req.value)
                                                    : protocol::encodeSimpleString("PONG"));
        continue;
      case CommandType::QUIT:
        LOG_DEBUG("Client fd={} sent QUIT, closing after pending replies", conn->fd());
        conn->markQuitReceived();
        conn->sendOrderedReply(seq, protocol::encodeSimpleString("OK"), /*closeAfter=*/true);
        // 同一个 buffer 里 QUIT 之后可能还有命令，直接丢掉
        inputBuffer.consume(inputBuffer.readableByte());
        return;
      case CommandType::UNKNOWN:
        if (req.wrongArity) {
          conn->sendOrderedReply(
              seq,
              protocol::encodeError("ERR wrong number of arguments for '" + req.commandName +
                                    "' command"));
        } else if (!req.commandName.empty()) {
          // 未知命令只回错误，绝不断开连接，否则压测工具会整体失败
          conn->sendOrderedReply(
              seq, protocol::encodeError("ERR unknown command '" + req.commandName + "'"));
        } else {
          // 空行：不回复，但要占用序号，否则后续回复会永远等不到这个序号
          conn->sendOrderedReply(seq, std::string());
        }
        continue;
      default:
        break;
    }

    LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);
    req.seq = seq;
    // GET/SET/DEL 全部走同一条 FIFO 队列：写请求在这里落 WAL，读请求随批次直接放行、
    // 不写日志。这样「执行顺序 == 请求顺序」，pipeline 里 `SET k v` 紧跟 `GET k`
    // 也能读到新值；同时回复天然按序发出。
    PendingRequest pending = {req, conn, conn->getLoop()};
    if (!writeCoordinator_.submitWriteReq(std::move(pending))) {
      // 队列已满/正在停止：必须自己回复，否则客户端会永远等这一条回复。
      // 这里不能阻塞等待（onMessage 跑在 IO 线程上，会卡死该 EventLoop 上的所有连接）。
      LOG_WARN("Rejecting request from fd={} due to backpressure, closing", conn->fd());
      conn->sendOrderedReply(seq,
                             protocol::encodeError("ERR server overloaded, closing connection"),
                             /*closeAfter=*/true);
      inputBuffer.consume(inputBuffer.readableByte());
      return;
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
