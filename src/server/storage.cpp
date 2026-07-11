#include "server/storage.hpp"

#include <filesystem>
#include <memory>

#include "net/EventLoop.hpp"
#include "net/TcpConnection.hpp"
#include "protocol/codec.hpp"
#include "protocol/request.hpp"
#include "storage/wal/log_record.hpp"
#include "storage/wal/wal_reader.hpp"
#include "storage/wal/wal_writer.hpp"
#include "util/Log.hpp"

namespace server {

Storage::Storage(const std::filesystem::path& walPath)
    : walPath_(walPath) {}

Storage::~Storage() {
  stop();
}

void Storage::start() {
  loop_ = loopThread_.startLoop();

  // 在存储线程中初始化
  loop_->runInLoop([this] {
    initWAL();

    dispatcher_ = std::make_unique<Dispatcher>(&memtable_);
    dispatcher_->registerHandler(
        CommandType::GET, [this](const Request &req) { return memtable_.GET(req); });
    dispatcher_->registerHandler(
        CommandType::SET, [this](const Request &req) { return memtable_.SET(req); });
    dispatcher_->registerHandler(
        CommandType::DEL, [this](const Request &req) { return memtable_.DEL(req); });
  });

  LOG_INFO("Storage thread started");
}

void Storage::stop() {
  if (loop_) {
    loop_->quit();
  }
}

void Storage::handleRequest(const Request& req,
                             const net::TcpServer::TcpConnectionPtr& conn,
                             net::EventLoop* ioLoop) {
  // 将请求投递到存储线程处理
  loop_->queueInLoop([this, req, conn, ioLoop] {
    LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);

    // WAL 写入（SET/DEL）
    if (req.cmd == CommandType::SET || req.cmd == CommandType::DEL) {
      wal::LogRecord record{wal::FromCommandType(req.cmd), req.key, req.value};
      walWriter_->append(record);
      LOG_DEBUG("WAL wrote: type={} key={}", static_cast<int>(record.type), req.key);
    }

    // 执行操作（无锁，因为这是存储线程）
    Response rsp = dispatcher_->dispatch(req);
    LOG_DEBUG("response: ok={} value={}", rsp.ok, rsp.value);

    // 编码响应
    std::string rspData = encodeResponse(rsp);

    // 将发送任务投递回 IO 线程
    ioLoop->queueInLoop([conn, rspData] {
      conn->send(rspData);
    });
  });
}

void Storage::initWAL() {
  loop_->assertInLoopThread();
  LOG_INFO("Recovering from WAL: {}", walPath_.string());

  // 恢复数据到 memtable
  wal::WALReader::Recover(walPath_, memtable_);

  // 初始化 WALWriter
  walWriter_ = std::make_unique<wal::WALWriter>(walPath_);

  LOG_INFO("WAL recovery done, storage ready");
}

}  // namespace server
