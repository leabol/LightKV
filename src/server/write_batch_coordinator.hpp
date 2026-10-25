#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>

#include "server/server_def.hpp"

namespace net {
class EventLoop;
}
namespace  wal {
class WALWriter;
}

namespace server {
// 协调写请求批处理和 WAL 提交
class WriteBatchCoordinator {

  using Clock = std::chrono::steady_clock;

  constexpr static size_t kMaxBatchSize = 64 * 1024;  // 最大批量写入大小为 64KB
  constexpr static auto kCommitWindow = std::chrono::milliseconds(1); // 提交窗口时间为 1 毫秒
public:
  WriteBatchCoordinator();
  ~WriteBatchCoordinator();

  void start();
  void stop();

  // 线程安全：可从任意线程调用
  void submitWriteReq(PendingRequest pending);

  void setWALWriter(wal::WALWriter* walWriter){
    walWriter_ = walWriter;
  }

private:
  void submitWriteLoop();

  // 状态标志
  bool stop_{false};
  bool ready_{false};

  size_t pendingWriteBytes_{0}; // 待处理的写操作的总字节数
  std::filesystem::path walPath_; // WAL 文件路径

  wal::WALWriter* walWriter_;
  std::mutex mtx_;
  std::condition_variable cvWAL_;
  std::queue<PendingRequest> writeRequests_;

  std::thread workerThread_; 
};

}  // namespace server
