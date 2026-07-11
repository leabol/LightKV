#pragma once

#include <filesystem>
#include <memory>

#include "net/EventLoop.hpp"
#include "net/EventLoopThread.hpp"
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

  net::EventLoop* loop_{nullptr};

  storage::Memtable memtable_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<wal::WALWriter> walWriter_;
  std::filesystem::path walPath_;
  net::EventLoopThread loopThread_;
};

}  // namespace server
