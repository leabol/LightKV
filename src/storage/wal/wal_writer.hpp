#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "server/request_processor.hpp"
#include "server/server_def.hpp"
#include "server/WriteBatchCoordinator.hpp"

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
  void appendBatch(std::vector<server::PendingRequest> batch);
private:
  void writeLoop();

  int fd_{-1};

  std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<std::vector<server::PendingRequest>> batch_que_;
  bool stop_{false};

  server::RequestProcessor* processor_{nullptr};
  std::thread write_thread_;
};
}  // namespace wal
