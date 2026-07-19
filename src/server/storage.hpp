#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/TcpConnection.hpp"
#include "net/TcpServer.hpp"
#include "protocol/request.hpp"
#include "protocol/response.hpp"
#include "server/dispatcher.hpp"
#include "storage/memtable/memtable.hpp"
#include "storage/wal/wal_writer.hpp"

namespace net {
class EventLoop;
}

namespace server {
// KV 请求处理器：串联 I/O、WAL 和 memtable
class KvRequestProcessor {
  struct PendingRequest {
    protocol::Request request;
    net::TcpServer::TcpConnectionPtr connection;
    net::EventLoop* ioLoop;
  };

  struct WriteBatch {
    std::string encodedData;
    std::vector<PendingRequest> requests;
  };

  using Clock = std::chrono::steady_clock;
  constexpr static size_t kMaxBatchSize = 64 * 1024;
  constexpr static auto kCommitWindow = std::chrono::milliseconds(1);
public:
  explicit KvRequestProcessor(const std::filesystem::path& walPath);
  ~KvRequestProcessor();

  void start();
  void stop();

  // 线程安全：可从任意线程调用
  void handleRequest(const Request& req,
                     const net::TcpServer::TcpConnectionPtr& conn,
                     net::EventLoop* ioLoop);

private:
  void initWAL();
  void workerLoop();
  void enqueueTask(std::function<void()> task);
  void processPendingRequests();
  void executeAndReply(PendingRequest pending);
  void submitWriteBatch();
  void completeWriteBatch(std::vector<PendingRequest> requests, bool success);
  void reply(PendingRequest& pending, const Response& response);
  std::string encodeLogRecord(const protocol::Request& request);
  bool isWrite(const Request& request) const;

  bool collectingWrites_{false};
  Clock::time_point commitDeadline_;
  size_t pendingWriteBytes_{0};
  storage::Memtable memtable_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<wal::WALWriter> walWriter_;
  std::filesystem::path walPath_;
  std::thread workerThread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stop_{false};
  bool ready_{false};
  std::deque<PendingRequest> pendingRequests_;
  bool writeInFlight_{false};
};

}  // namespace server
