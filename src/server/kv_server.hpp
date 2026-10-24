#pragma once
#include <netinet/tcp.h>
#include <filesystem>

#include "net/TcpConnection.hpp"
#include "net/TcpServer.hpp"
#include "net/buffer.hpp"
#include "server/WriteBatchCoordinator.hpp"
#include "server/request_processor.hpp"
#include "storage/wal/wal_writer.hpp"

namespace server {

class KvServer {
public:
  explicit KvServer(net::EventLoop* loop, const net::InetAddress& listenAddr, const std::filesystem::path& walPath = "wal");

  // Start listening
  void start();
  void initWAL();
  void onMessage(const net::TcpServer::TcpConnectionPtr& conn, net::Buffer& inputBuffer);
  void setThreadNum(int numThreads);
   std::unique_ptr<wal::WALWriter>& getWALWriter(){
    return walWriter_;
   }
private:
  std::unique_ptr<wal::WALWriter> walWriter_;
  WriteBatchCoordinator writeCoordinator_;
  RequestProcessor reqProcessor_;
  net::TcpServer server_;

  std::filesystem::path walPath_; // WAL 文件路径
};
}  // namespace server
