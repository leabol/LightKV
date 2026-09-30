#include "server/KvRequestProcessor.hpp"

#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>

#include "net/EventLoop.hpp"
#include "net/TcpConnection.hpp"
#include "protocol/codec.hpp"
#include "protocol/request.hpp"
#include "protocol/response.hpp"
#include "storage/wal/log_record.hpp"
#include "storage/wal/wal_reader.hpp"
#include "storage/wal/wal_writer.hpp"
#include "util/Log.hpp"

namespace server {

KvRequestProcessor::KvRequestProcessor(const std::filesystem::path& walPath) : walPath_(walPath) {}

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
  cv_.notify_all();
}

void KvRequestProcessor::handleRequest(const Request& req,
                                       const net::TcpServer::TcpConnectionPtr& conn,
                                       net::EventLoop* ioLoop) {
  enqueueTask([this, req, conn, ioLoop] {
    // 如果是读请求，直接执行并回复
    if (!isWrite(req)) {
      executeAndReply({req, conn, ioLoop});
      return;
    }
    // 如果是写请求，加入待处理队列
    pendingRequests_.push_back({req, conn, ioLoop});
    pendingWriteBytes_ += sizeof(wal::RecordHeader) + req.key.size() + req.value.size();
    processPendingRequests();
  });
}

void KvRequestProcessor::initWAL() {
  LOG_INFO("Recovering from WAL: {}", walPath_.string());

  wal::WALReader::Recover(walPath_, memtable_);

  walWriter_ = std::make_unique<wal::WALWriter>(walPath_);

  LOG_INFO("WAL recovery done, storage ready");
}

void KvRequestProcessor::initDispatcher() {
  dispatcher_ = std::make_unique<Dispatcher>(&memtable_);
  dispatcher_->registerHandler(CommandType::GET,
                               [this](const Request& req) { return memtable_.GET(req); });
  dispatcher_->registerHandler(CommandType::SET,
                               [this](const Request& req) { return memtable_.SET(req); });
  dispatcher_->registerHandler(CommandType::DEL,
                               [this](const Request& req) { return memtable_.DEL(req); });
}

void KvRequestProcessor::workerLoop() {
  initWAL();
  initDispatcher();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = true;
  }
  cv_.notify_one();

  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (collectingWrites_ && !writeInFlight_) {
        const bool awakenedByTask =
            cv_.wait_until(lock, commitDeadline_, [this] { return stop_ || !tasks_.empty(); });

        // 如果定时器到期，则提交批量写入
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
  if (!pendingRequests_.empty()) {
    // 首次进入收集写操作的状态
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
  auto pendingRequestsPtr = std::make_shared<std::deque<PendingRequest>>(std::move(pendingRequests_));
  pendingRequests_.clear();
  writeInFlight_ = true;
  
  auto callback = [this, ptr = pendingRequestsPtr](bool success) mutable {
    enqueueTask([this, ptr = std::move(ptr), success]() mutable {
      completeWriteBatch(ptr, success);
    });
  };

  walWriter_->appendRequests(pendingRequestsPtr, std::move(callback));
}

void KvRequestProcessor::completeWriteBatch(const std::shared_ptr<std::deque<server::PendingRequest>> &pendingRequestsPtr, bool success) {
  for (auto& pending : *pendingRequestsPtr) {
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


bool KvRequestProcessor::isWrite(const Request& request) const {
  return request.cmd == CommandType::SET || request.cmd == CommandType::DEL;
}
}  // namespace server
