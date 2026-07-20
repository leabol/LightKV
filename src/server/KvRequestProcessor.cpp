#include "server/KvRequestProcessor.hpp"

#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>
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

KvRequestProcessor::KvRequestProcessor(const std::filesystem::path& walPath)
    : walPath_(walPath) {}

KvRequestProcessor::~KvRequestProcessor() {
  stop();
}

void KvRequestProcessor::start() {
  workerThread_ = std::thread([this] { workerLoop(); });

  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return ready_; });

  LOG_INFO("Storage coordinator started");
}

void KvRequestProcessor::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (workerThread_.joinable()) {
    workerThread_.join();
  }
}

void KvRequestProcessor::enqueueTask(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
}

void KvRequestProcessor::handleRequest(const Request& req,
                                       const net::TcpServer::TcpConnectionPtr& conn,
                                       net::EventLoop* ioLoop) {
  enqueueTask([this, req, conn, ioLoop] {
    pendingRequests_.push_back({req, conn, ioLoop});

    if (isWrite(req)) {
      pendingWriteBytes_ += sizeof(wal::RecordHeader) + req.key.size() + req.value.size();
    }

    processPendingRequests();
  });
}

void KvRequestProcessor::initWAL() {
  LOG_INFO("Recovering from WAL: {}", walPath_.string());

  wal::WALReader::Recover(walPath_, memtable_);

  walWriter_ = std::make_unique<wal::WALWriter>(walPath_);

  LOG_INFO("WAL recovery done, storage ready");
}

void KvRequestProcessor::workerLoop() {
  initWAL();

  dispatcher_ = std::make_unique<Dispatcher>(&memtable_);
  dispatcher_->registerHandler(CommandType::GET,
                               [this](const Request& req) { return memtable_.GET(req); });
  dispatcher_->registerHandler(CommandType::SET,
                               [this](const Request& req) { return memtable_.SET(req); });
  dispatcher_->registerHandler(CommandType::DEL,
                               [this](const Request& req) { return memtable_.DEL(req); });

  {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = true;
  }
  cv_.notify_all();

  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (collectingWrites_ && !writeInFlight_) {
        const bool awakenedByTask =
            cv_.wait_until(lock, commitDeadline_, [this] { return stop_ || !tasks_.empty(); });

        if (!awakenedByTask && collectingWrites_) {
          collectingWrites_ = false;
          lock.unlock();
          submitWriteBatch();
          continue;
        }
      } else {
        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
      }

      if (stop_ && tasks_.empty()) {
        break;
      }
      if (!tasks_.empty()) {
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
    }

    if (task) {
      task();
    }
  }
  LOG_INFO("Storage coordinator stopped");
}

void KvRequestProcessor::processPendingRequests() {
  if (writeInFlight_) {
    return;
  }
  while (!pendingRequests_.empty()) {
    if (pendingRequests_.front().request.cmd == CommandType::GET) {
      auto pending = std::move(pendingRequests_.front());
      pendingRequests_.pop_front();
      executeAndReply(std::move(pending));
      continue;
    }

    if (!collectingWrites_) {
      if (pendingWriteBytes_ >= kMaxBatchSize) {
        submitWriteBatch();
        return;
      }

      collectingWrites_ = true;
      commitDeadline_ = Clock::now() + kCommitWindow;
      return;
    }

    if (pendingWriteBytes_ >= kMaxBatchSize) {
      collectingWrites_ = false;
      submitWriteBatch();
      return;
    }

    if (Clock::now() >= commitDeadline_) {
      collectingWrites_ = false;
      submitWriteBatch();
    }
    return;
  }
}

void KvRequestProcessor::executeAndReply(PendingRequest pending) {
  auto req = pending.request;
  LOG_DEBUG("cmd={} key={}", static_cast<int>(req.cmd), req.key);

  Response rsp = dispatcher_->dispatch(req);
  LOG_DEBUG("response: ok={} value={}", rsp.ok, rsp.value);

  std::string rspData = encodeResponse(rsp);

  pending.ioLoop->queueInLoop([conn = pending.connection, rspData] { conn->send(rspData); });
}

void KvRequestProcessor::reply(PendingRequest& pending, const Response& response) {
  std::string data = encodeResponse(response);

  pending.ioLoop->queueInLoop(
      [conn = pending.connection, data = std::move(data)]() mutable { conn->send(data); });
}

void KvRequestProcessor::submitWriteBatch() {
  WriteBatch batch;

  while (!pendingRequests_.empty() && isWrite(pendingRequests_.front().request) &&
         batch.encodedData.size() < kMaxBatchSize) {
          const size_t recordSize = sizeof(wal::RecordHeader) +
                  pendingRequests_.front().request.key.size() +
                  pendingRequests_.front().request.value.size();
    auto pending = std::move(pendingRequests_.front());
    pendingRequests_.pop_front();

    batch.encodedData += encodeLogRecord(pending.request);
    batch.requests.push_back(std::move(pending));

          pendingWriteBytes_ -= recordSize;
  }

  writeInFlight_ = true;
  walWriter_->appendBatch(std::move(batch.encodedData),
                          [this, requests = std::move(batch.requests)](bool success) mutable {
                            enqueueTask([this, requests = std::move(requests), success]() mutable {
                              completeWriteBatch(std::move(requests), success);
                            });
                          });
}

void KvRequestProcessor::completeWriteBatch(std::vector<PendingRequest> requests,
                                            bool success) {
  for (auto& pending : requests) {
    Response response;

    if (success) {
      response = dispatcher_->dispatch((pending.request));
    } else {
      response = {false, "WAL persistence failed"};
    }

    reply(pending, response);
  }

  writeInFlight_ = false;
  processPendingRequests();
}

std::string KvRequestProcessor::encodeLogRecord(const protocol::Request& request) {
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

  uint32_t crc = util::crc32::Value(buffer.data() + sizeof(uint32_t),
                                    buffer.size() - sizeof(uint32_t));
  std::memcpy(buffer.data(), &crc, sizeof(crc));

  return buffer;
}

bool KvRequestProcessor::isWrite(const Request& request) const {
  return request.cmd == CommandType::SET || request.cmd == CommandType::DEL;
}
}  // namespace server
