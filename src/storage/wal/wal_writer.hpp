#pragma once
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "storage/wal/log_record.hpp"

namespace wal {
class WALWriter {
public:
  explicit WALWriter(const std::filesystem::path& walPath);
  ~WALWriter();

  WALWriter(const WALWriter&) = delete;
  WALWriter& operator=(const WALWriter&) = delete;

  void append(const LogRecord& record);

private:
  void writeLoop();

  int fd_{-1};

  std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<std::string> queue_;
  bool stop_{false};

  std::thread write_thread_;
  static constexpr size_t max_queue_size_{100000}; // 队列的最大容量
  static constexpr std::chrono::milliseconds flush_interval_{50}; //50ms检测一次
};
}  // namespace wal
