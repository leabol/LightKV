#include "server/write_batch_coordinator.hpp"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>
#include <pthread.h>
#include <optional>
#include <vector>

#include "protocol/request.hpp"
#include "server/server_def.hpp"
#include "storage/wal/wal_writer.hpp"
#include "util/Log.hpp"

namespace server {

WriteBatchCoordinator::WriteBatchCoordinator(){}

WriteBatchCoordinator::~WriteBatchCoordinator() {
  stop();
}

void WriteBatchCoordinator::start() {
  workerThread_ = std::thread([this] { submitWriteLoop(); });

  std::unique_lock<std::mutex> lock(mtx_);
  cvWAL_.wait(lock, [this] { return ready_; });

  LOG_INFO("Storage coordinator started");
}

void WriteBatchCoordinator::stop() {
  {
    std::lock_guard<std::mutex> lock(mtx_);
    stop_ = true;
  }
  cvWAL_.notify_all();
  if (workerThread_.joinable()) {
    workerThread_.join();
  }
  LOG_INFO("Storage coordinator stopped");
}


bool WriteBatchCoordinator::submitWriteReq(PendingRequest pending){
  const auto key = pending.request.key;
  const auto command = static_cast<int>(pending.request.cmd);
  const size_t reqBytes = pending.request.key.size() + pending.request.value.size();
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stop_) {
      LOG_WARN("Dropping write request because storage coordinator is stopping: cmd={} key={}",
               command, key);
      return false;
    }
    // 背压：不设上限的话深 pipeline 客户端可以把队列堆到 OOM
    if (writeRequests_.size() >= kMaxQueuedRequests ||
        queuedBytes_ + reqBytes > kMaxQueuedBytes) {
      LOG_WARN("Write queue full, rejecting: cmd={} key={} queued={} bytes={}",
               command, key, writeRequests_.size(), queuedBytes_);
      return false;
    }
    queuedBytes_ += reqBytes;
    writeRequests_.push(std::move(pending));
  }
  LOG_DEBUG("Write request queued: cmd={} key={} queue_size={}", command, key);
  cvWAL_.notify_all();
  return true;
}

void WriteBatchCoordinator::submitWriteLoop(){
  pthread_setname_np(pthread_self(), "submitwrite");
  {
    std::lock_guard<std::mutex> lock(mtx_);
    ready_ = true;
  }
  cvWAL_.notify_one();
  LOG_INFO("Write batching thread started");

  std::vector<PendingRequest> batch;
  batch.reserve(kInitialBatchCapacity);
  std::optional<Clock::time_point> batch_start;
  bool batchHasWrite = false;

  while (true) {
      std::unique_lock<std::mutex> lock(mtx_);

      if (stop_ && writeRequests_.empty() && batch.empty()) break;

      if (writeRequests_.empty()){
        if (batch.empty()){
          cvWAL_.wait(lock, [&]{ return stop_ || !writeRequests_.empty();});
        } else if (batchHasWrite) {
          // 只有包含写请求时才需要等提交窗口来攒批；纯读批次直接放行，避免引入 1ms 延迟
          cvWAL_.wait_until(lock, *batch_start + kCommitWindow,[&]{
            return stop_ || !writeRequests_.empty(); });
        }
      }

      while (!writeRequests_.empty() && pendingWriteBytes_ < kMaxQueuedBytes){
        if (batch.empty()){
          batch_start = Clock::now();
        }
        PendingRequest pending = std::move(writeRequests_.front());
        writeRequests_.pop();
        // 出队即从背压统计里扣掉（批内的量由 pendingWriteBytes_ 独立限制）
        queuedBytes_ -= pending.request.key.size() + pending.request.value.size();
        pendingWriteBytes_ += pending.request.key.size() + pending.request.value.size();
        if (pending.request.cmd != protocol::CommandType::GET) {
          batchHasWrite = true;
        }
        batch.push_back(std::move(pending));
      }

      bool isfull = pendingWriteBytes_ >= kMaxQueuedBytes;
      bool timeout = batch_start && Clock::now() >= *batch_start + kCommitWindow;

      if (batch.empty() || !(isfull || timeout || stop_ || !batchHasWrite)){
        continue;
      }

      std::vector<PendingRequest> to_submit;
      to_submit.swap(batch);
      const auto submittedBytes = pendingWriteBytes_;
      const auto submittedCount = to_submit.size();
      const auto age = batch_start ? Clock::now() - *batch_start : Clock::duration::zero();
      pendingWriteBytes_ = 0;
      batch_start.reset();
      batchHasWrite = false;
      lock.unlock();

      LOG_DEBUG("Submitting WAL batch: requests={} bytes={} age_us={}",
                submittedCount, submittedBytes,
                std::chrono::duration_cast<std::chrono::microseconds>(age).count());
      walWriter_->appendBatch(std::move(to_submit));
      batch.reserve(kInitialBatchCapacity);
  }
  LOG_INFO("Write batching thread stopped");
}

}  // namespace server
