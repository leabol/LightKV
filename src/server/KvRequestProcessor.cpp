#include "server/KvRequestProcessor.hpp"

#include <cstring>
#include <memory>
#include <utility>
#include <pthread.h>

#include "net/EventLoop.hpp"
#include "net/TcpConnection.hpp"
#include "protocol/request.hpp"
#include "storage/wal/log_record.hpp"
#include "storage/wal/wal_writer.hpp"
#include "util/Log.hpp"

namespace server {

KvRequestProcessor::KvRequestProcessor(){}

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

void KvRequestProcessor::handleRequest(const protocol::Request& req,
                                       const net::TcpServer::TcpConnectionPtr& conn,
                                       net::EventLoop* ioLoop) {
  enqueueTask([this, req, conn, ioLoop] {
    // 如果是写请求，加入待处理队列
    pendingRequests_.push_back({req, conn, ioLoop});
    pendingWriteBytes_ += sizeof(wal::RecordHeader) + req.key.size() + req.value.size();
    processPendingRequests();
  });
}

void KvRequestProcessor::submitWriteReq(PendingRequest& pending){
  handleRequest(pending.request, pending.connection, pending.ioLoop);
}

void KvRequestProcessor::workerLoop() {
  // initWAL();
  // initDispatcher();
  pthread_setname_np(pthread_self(), "submitwrite"); 
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


void KvRequestProcessor::submitWriteBatch() {
  auto pendingRequestsPtr = std::make_shared<std::deque<PendingRequest>>(std::move(pendingRequests_));
  pendingRequests_.clear();
  writeInFlight_ = true;
  pendingWriteBytes_ = 0;

  auto callback = [](bool) mutable {
  };

  walWriter_->appendRequests(pendingRequestsPtr, std::move(callback));
  writeInFlight_ = false;
  
  // walWriter_->handleWriteRequest(pendingRequestsPtr);
}

}  // namespace server
