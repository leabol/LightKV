#include <spdlog/common.h>

#include "net/EventLoop.hpp"
#include "net/InetAddress.hpp"
#include "server/kv_server.hpp"

int main(int argc, char** argv) {
  std::string port = "8990";

  int numThreads = 1;
  if (argc > 1) {
    numThreads = std::stoi(argv[1]);
  }
  if (argc > 2) {
    port = argv[2];
  }
  const char* walPath = argc > 3 ? argv[3] : "wal";

  net::EventLoop loop;
  net::InetAddress listenAddr(port);

  server::KvServer srv(&loop, listenAddr, walPath);
  srv.setThreadNum(numThreads);
  srv.start();

  loop.loop();
  return 0;
}
