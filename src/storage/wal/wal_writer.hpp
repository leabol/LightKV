#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "storage/wal/log_record.hpp"

namespace wal {
class WALWriter {
public:
  using CompletionCallback = std::function<void(bool)>;

  struct WALTask {
    std::string data;
    CompletionCallback completion;
  };

  explicit WALWriter(const std::filesystem::path& walPath);
  ~WALWriter();

  WALWriter(const WALWriter&) = delete;
  WALWriter& operator=(const WALWriter&) = delete;

  void append(const LogRecord& record);
  void appendBatch(std::string data, CompletionCallback callback);

private:
  void writeLoop();

  int fd_{-1};

  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<WALTask> tasks_;
  bool stop_{false};

  std::thread write_thread_;
};
}  // namespace wal
