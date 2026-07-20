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
  //  PendingRequest 结构体用于封装待处理的请求信息，包括请求本身、连接对象和事件循环器指针
  struct PendingRequest {
    protocol::Request request;
    net::TcpServer::TcpConnectionPtr connection;
    net::EventLoop* ioLoop;
  };
  // WriteBatch 结构体用于封装一批待写入 WAL 的请求信息，包括编码后的数据和对应的请求列表
  struct WriteBatch {
    std::string encodedData;
    std::vector<PendingRequest> requests;
  };
  using Clock = std::chrono::steady_clock;

  constexpr static size_t kMaxBatchSize = 64 * 1024;  // 最大批量写入大小为 64KB
  constexpr static auto kCommitWindow = std::chrono::milliseconds(1); // 提交窗口时间为 1 毫秒
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
  void initDispatcher();
  void workerLoop();
  void enqueueTask(std::function<void()> task);
  void processPendingRequests();
  void executeAndReply(PendingRequest pending);
  void submitWriteBatch();
  void completeWriteBatch(std::vector<PendingRequest> requests, bool success);
  void reply(PendingRequest& pending, const Response& response);
  std::string encodeLogRecord(const protocol::Request& request);
  bool isWrite(const Request& request) const;

  // 状态标志
  bool stop_{false};
  bool ready_{false};
  bool collectingWrites_{false};  // 是否正在收集写操作
  bool writeInFlight_{false}; // 是否有写操作正在进行

  Clock::time_point commitDeadline_;  // 收集的写操作的提交截止时间
  size_t pendingWriteBytes_{0}; // 待处理的写操作的总字节数
  std::filesystem::path walPath_; // WAL 文件路径

  storage::Memtable memtable_; 
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<wal::WALWriter> walWriter_;

  std::thread workerThread_; 
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_; // 待处理的任务队列
  std::deque<PendingRequest> pendingRequests_; // 待处理的请求队列
};

}  // namespace server
