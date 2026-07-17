#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "net/EventLoop.hpp"
#include "net/EventLoopThread.hpp"
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
// 存储线程：独占 memtable_，其他线程通过 queueInLoop 与之通信
class Storage {
  struct PendingRequest {
    protocol::Request request;
    net::TcpServer::TcpConnectionPtr connection;
    net::EventLoop* ioLoop;
  };

  struct WriteBatch {
    std::string encodedData;
    std::vector<PendingRequest> requests;
  };

public:
  explicit Storage(const std::filesystem::path& walPath);
  ~Storage();

  void start();
  void stop();

  // 线程安全：可从任意线程调用
  void handleRequest(const Request& req,
                     const net::TcpServer::TcpConnectionPtr& conn,
                     net::EventLoop* ioLoop);

private:
  void initWAL();
  void processPendingRequests();
  void executeAndReply(PendingRequest pending);
  void submitWriteBatch();
  void completeWriteBatch(std::vector<PendingRequest> requests, bool success);
  void reply(PendingRequest& pending, const Response& response);
  std::string encodeLogRecord(const protocol::Request& request);

  net::EventLoop* loop_{nullptr};

  storage::Memtable memtable_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<wal::WALWriter> walWriter_;
  std::filesystem::path walPath_;
  net::EventLoopThread loopThread_;
  std::deque<PendingRequest> pendingRequests_;
  bool writeInFlight_{false};
};

}  // namespace server
