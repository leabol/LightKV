#include <spdlog/common.h>

#include "net/EventLoop.hpp"
#include "net/InetAddress.hpp"
#include "util/Log.hpp"
#include "server/kv_server.hpp"

int main(int argc, char** argv) {
  Server::setLevel(spdlog::level::off);
  std::string port = "8990";
  
  int numThreads = 1;
  if (argc > 1) {
    numThreads = std::stoi(argv[1]);
  }

  net::EventLoop loop;
  net::InetAddress listenAddr(port);

  server::KvServer srv(&loop, listenAddr,"wal");
  srv.setThreadNum(numThreads);
  srv.start();

  loop.loop();
  return 0;
}
