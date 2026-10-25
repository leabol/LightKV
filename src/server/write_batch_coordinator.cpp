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


void WriteBatchCoordinator::submitWriteReq(PendingRequest pending){
  const auto key = pending.request.key;
  const auto command = static_cast<int>(pending.request.cmd);
  std::size_t queueSize = 0;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stop_) {
      LOG_WARN("Dropping write request because storage coordinator is stopping: cmd={} key={}",
               command, key);
      return;
    }
    writeRequests_.push(std::move(pending));
    queueSize = writeRequests_.size();
  }
  LOG_DEBUG("Write request queued: cmd={} key={} queue_size={}", command, key, queueSize);
  cvWAL_.notify_all();
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
  batch.reserve(kMaxBatchSize/100);
  std::optional<Clock::time_point> batch_start;

  while (true) {
      std::unique_lock<std::mutex> lock(mtx_);

      if (stop_ && writeRequests_.empty() && batch.empty()) break;

      if (writeRequests_.empty()){
        if (batch.empty()){
          cvWAL_.wait(lock, [&]{ return stop_ || !writeRequests_.empty();});
        } else {
          cvWAL_.wait_until(lock, *batch_start + kCommitWindow,[&]{
            return stop_ || !writeRequests_.empty(); });
        }
      }

      while (!writeRequests_.empty() && pendingWriteBytes_ < kMaxBatchSize){
        if (batch.empty()){
          batch_start = Clock::now();
        }
        PendingRequest pending = writeRequests_.front();
        pendingWriteBytes_ += pending.request.key.size() + pending.request.value.size();
        batch.push_back(std::move(pending));
        writeRequests_.pop();
      }

      bool isfull = pendingWriteBytes_ >= kMaxBatchSize;
      bool timeout = batch_start && Clock::now() >= *batch_start + kCommitWindow;

      if (batch.empty() || !(isfull || timeout || stop_)){
        continue;
      }

      std::vector<PendingRequest> to_submit;
      to_submit.swap(batch);
      const auto submittedBytes = pendingWriteBytes_;
      const auto submittedCount = to_submit.size();
      const auto age = batch_start ? Clock::now() - *batch_start : Clock::duration::zero();
      pendingWriteBytes_ = 0;
      batch_start.reset();
      lock.unlock();

      LOG_DEBUG("Submitting WAL batch: requests={} bytes={} age_us={}",
                submittedCount, submittedBytes,
                std::chrono::duration_cast<std::chrono::microseconds>(age).count());
      walWriter_->appendBatch(std::move(to_submit));
      batch.reserve(kMaxBatchSize/100);
  }
  LOG_INFO("Write batching thread stopped");
}

}  // namespace server
