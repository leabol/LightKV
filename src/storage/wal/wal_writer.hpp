#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "server/request_processor.hpp"
#include "storage/wal/log_record.hpp"
#include "server/KvRequestProcessor.hpp"

namespace wal {
class WALWriter {
public:
  using CompletionCallback = std::function<void(bool)>;

  struct WALTask {
    std::shared_ptr<std::deque<server::PendingRequest>> pendingRequestsPtr;
    CompletionCallback completion;
  };

  explicit WALWriter(const std::filesystem::path& walPath);
  ~WALWriter();

  WALWriter(const WALWriter&) = delete;
  WALWriter& operator=(const WALWriter&) = delete;

  void setprocessor(server::RequestProcessor* pprocessor){
    processor_ = pprocessor;
  }
  void append(const LogRecord& record);
  void appendBatch(std::string data, CompletionCallback callback);
  void appendRequests(std::shared_ptr<std::deque<server::PendingRequest>> pendingRequestsPtr, CompletionCallback cb);
  void handleWriteRequest(std::shared_ptr<std::deque<server::PendingRequest>> pendingRequestsPtr);
private:
  void writeLoop();

  int fd_{-1};

  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<WALTask> tasks_;
  bool stop_{false};

  server::RequestProcessor* processor_{nullptr};
  std::thread write_thread_;
};
}  // namespace wal
