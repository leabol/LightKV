#include "server/storage.hpp"

#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include "net/EventLoop.hpp"
#include "net/TcpConnection.hpp"
#include "protocol/codec.hpp"
#include "protocol/request.hpp"
#include "protocol/response.hpp"
#include "storage/wal/log_record.hpp"
#include "storage/wal/wal_reader.hpp"
#include "storage/wal/wal_writer.hpp"
#include "util/Log.hpp"
#include "util/crc32.hpp"

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
  if (loop_ != nullptr) {
    loop_->quit();
  }
}

void Storage::handleRequest(const Request& req,
                             const net::TcpServer::TcpConnectionPtr& conn,
                             net::EventLoop* ioLoop) {
  loop_->queueInLoop([this, req, conn, ioLoop]{
    pendingRequests_.push_back({req, conn, ioLoop});
    processPendingRequests();
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

void Storage::processPendingRequests() {
  if (writeInFlight_){
    return;
  }
  while (!pendingRequests_.empty()){
    if (pendingRequests_.front().request.cmd == CommandType::GET){
      auto pending = std::move(pendingRequests_.front());
      pendingRequests_.pop_front(); 
      executeAndReply(std::move(pending));
      continue;
    }

    submitWriteBatch();
    return;
  }
}

void Storage::executeAndReply(PendingRequest pending) {
  auto req = pending.request;
  LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);

  // 执行get操作
  Response rsp = dispatcher_->dispatch(req);
  LOG_DEBUG("response: ok={} value={}", rsp.ok, rsp.value);

  // 编码响应
  std::string rspData = encodeResponse(rsp);

  // 将发送任务投递回 IO 线程

  pending.ioLoop->queueInLoop([conn = pending.connection, rspData] { conn->send(rspData); });
}

void Storage::reply(PendingRequest& pending, const Response& response) {
  std::string data = encodeResponse(response);

  pending.ioLoop->queueInLoop(
      [conn = pending.connection, data = std::move(data)]() mutable {
        conn->send(data);
      });
}

void Storage::submitWriteBatch() {
  WriteBatch batch;

  while (!pendingRequests_.empty() &&
     (pendingRequests_.front().request.cmd == CommandType::SET ||
      pendingRequests_.front().request.cmd == CommandType::DEL) &&
         batch.encodedData.size() < 64 * 1024) {
    auto pending = std::move(pendingRequests_.front());
    pendingRequests_.pop_front();

    batch.encodedData += encodeLogRecord(pending.request);
    batch.requests.push_back(std::move(pending));
  }

  writeInFlight_ = true;
  walWriter_->appendBatch(std::move(batch.encodedData),
      [this, requests = std::move(batch.requests)](bool success) mutable {
        loop_->queueInLoop([this, requests = std::move(requests), success]() mutable {
          completeWriteBatch(std::move(requests), success);
        });
      });
}

void Storage::completeWriteBatch(std::vector<PendingRequest> requests, bool success){
  for(auto& pending : requests){
    Response response;

    if (success){
      response = dispatcher_->dispatch((pending.request));
    }else {
      response = {false, "WAL persistence failed"};
    }

    reply(pending, response);
  }

  writeInFlight_ = false;
  processPendingRequests();
}

std::string Storage::encodeLogRecord(const protocol::Request& request){
  wal::RecordHeader header{};
  header.crc = 0;
  header.key_size = request.key.size();
  header.value_size = request.value.size();
  header.type = static_cast<uint8_t>(wal::FromCommandType(request.cmd));

  size_t total_size = sizeof(header) + request.key.size() + request.value.size();
  std::string buffer;
  buffer.reserve(total_size);
  buffer.append(reinterpret_cast<const char*>(&header), sizeof(header));
  buffer.append(request.key.data(), request.key.size());
  buffer.append(request.value.data(), request.value.size());

  uint32_t crc = util::crc32::Value(buffer.data() + sizeof(uint32_t), buffer.size() - sizeof(uint32_t));
  std::memcpy(buffer.data(), &crc, sizeof(crc));
  
  return buffer;
}

}  // namespace server
