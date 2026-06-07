#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "util/Log.hpp"
#include "net/Socket.hpp"

using namespace std;
using namespace net;

struct Args {
  string host = "127.0.0.1";
  string port = "8990";
  uint64_t num_connections = 1000;
  size_t concurrency = 100;
  bool idle = true;   // just hold connections
  bool ping = false;  // send a simple ping/pong
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    string s = argv[i];
    if (s == "-n") {
      a.num_connections = stoull(argv[++i]);
    } else if (s == "-c") {
      a.concurrency = stoul(argv[++i]);
    } else if (s == "--host") {
      a.host = argv[++i];
    } else if (s == "--port") {
      a.port = argv[++i];
    } else if (s == "--idle") {
      a.idle = true;
      a.ping = false;
    } else if (s == "--ping") {
      a.ping = true;
      a.idle = false;
    }
  }
  return a;
}

int main(int argc, char** argv) {
  try {
    Args a = parse(argc, argv);
    Server::initLogger();
    Server::setLevel(spdlog::level::off);

    cout << "TCP connection stress test\n";
    cout << "  host=" << a.host << " port=" << a.port << " num_conn=" << a.num_connections
         << " concurrency=" << a.concurrency << " mode=" << (a.ping ? "ping" : "idle") << "\n";

    atomic<uint64_t> next_idx{0};
    atomic<uint64_t> success{0}, failed{0};
    vector<net::Socket> sockets;
    sockets.reserve(a.num_connections);
    mutex sock_m;

    auto start_time = chrono::steady_clock::now();

    auto worker = [&](size_t worker_id) {
      while (true) {
        uint64_t idx = next_idx.fetch_add(1);
        if (idx >= a.num_connections)
          break;

        try {
          net::Socket sock;
          sock.connect(a.host, a.port);

          if (a.ping) {
            // Send a simple 8-byte ping frame
            uint8_t ping_frame[8] = {0, 0, 0, 8, 0, 0, 0, 0};
            ssize_t sent = ::send(sock.fd(), ping_frame, 8, 0);
            if (sent <= 0) {
              failed.fetch_add(1);
              continue;
            }
          }

          {
            lock_guard<mutex> g(sock_m);
            sockets.push_back(std::move(sock));
          }
          success.fetch_add(1);
        } catch (const exception& e) {
          failed.fetch_add(1);
          if (idx < 10) {  // Only print first few errors
            lock_guard<mutex> g(sock_m);
            cerr << "worker-" << worker_id << " connect failed at idx=" << idx << ": " << e.what()
                 << '\n';
          }
        }
      }
    };

    vector<thread> threads;
    threads.reserve(a.concurrency);
    for (size_t i = 0; i < a.concurrency; i++) {
      threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
      t.join();
    }

    auto end_time = chrono::steady_clock::now();
    double elapsed = chrono::duration<double>(end_time - start_time).count();

    cout << "done\n";
    cout << "  success=" << success.load() << " failed=" << failed.load()
         << " elapsed_s=" << elapsed << " conn/s=" << (success.load() / elapsed) << "\n";
    cout << "  fds_held=" << sockets.size() << "\n";

    if (a.idle) {
      cout << "holding " << sockets.size() << " connections for 5 seconds...\n";
      this_thread::sleep_for(chrono::seconds(5));
      cout << "releasing connections.\n";
    }

    return 0;
  } catch (const exception& e) {
    cerr << e.what() << "\n";
    cerr << "Usage: -n num_connections -c concurrency --host --port [--idle|--ping]\n";
    return 1;
  }
}
