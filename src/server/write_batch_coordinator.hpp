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
  // 队列背压上限。submitWriteReq 跑在 IO 线程上，不能阻塞等待（会卡死该 EventLoop
  // 上的所有连接），所以满了直接拒绝，由调用方回复错误并断开连接。
  constexpr static size_t kMaxQueuedRequests = 64 * 1024;
  constexpr static size_t kMaxQueuedBytes = 64UL * 1024 * 1024;
public:
  WriteBatchCoordinator();
  ~WriteBatchCoordinator();

  void start();
  void stop();

  // 线程安全：可从任意线程调用。
  // 返回 false 表示队列已满或正在停止，请求未被受理（调用方必须自己回复并断开连接，
  // 否则客户端会永远等这一条回复）。
  bool submitWriteReq(PendingRequest pending);

  void setWALWriter(wal::WALWriter* walWriter){
    walWriter_ = walWriter;
  }

private:
  void submitWriteLoop();

  // 状态标志
  bool stop_{false};
  bool ready_{false};

  size_t pendingWriteBytes_{0}; // 待处理的写操作的总字节数
  size_t queuedBytes_{0};       // 队列中尚未取走的请求字节数（用于背压）
  std::filesystem::path walPath_; // WAL 文件路径

  wal::WALWriter* walWriter_;
  std::mutex mtx_;
  std::condition_variable cvWAL_;
  std::queue<PendingRequest> writeRequests_;

  std::thread workerThread_; 
};

}  // namespace server
