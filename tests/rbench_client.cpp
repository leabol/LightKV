#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Log.hpp"
#include "Socket.hpp"

using namespace std;
using namespace net;

// Simple redis-benchmark like tool for the custom binary protocol.
// Supports: -t cmd1,cmd2  -n total_requests  -c concurrency  -r keyspace  -d value_size
//           --sequential (use deterministic ordered keys)

static void write_u32_be(uint8_t* d, uint32_t v) {
  d[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
  d[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
  d[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
  d[3] = static_cast<uint8_t>(v & 0xFF);
}
static void write_u24_be(uint8_t* d, uint32_t v) {
  d[0] = static_cast<uint8_t>((v >> 16) & 0xFF);
  d[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  d[2] = static_cast<uint8_t>(v & 0xFF);
}
static void write_u16_be(uint8_t* d, uint16_t v) {
  d[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
  d[1] = static_cast<uint8_t>(v & 0xFF);
}
static uint32_t read_u32_be(const uint8_t* d) {
  return (static_cast<uint32_t>(d[0]) << 24) | (static_cast<uint32_t>(d[1]) << 16) |
         (static_cast<uint32_t>(d[2]) << 8) | static_cast<uint32_t>(d[3]);
}
static uint32_t read_u24_be(const uint8_t* d) {
  return (static_cast<uint32_t>(d[0]) << 16) | (static_cast<uint32_t>(d[1]) << 8) |
         static_cast<uint32_t>(d[2]);
}

static bool send_all(int fd, const uint8_t* buf, size_t len) {
  size_t s = 0;
  while (s < len) {
    ssize_t r = ::send(fd, buf + s, len - s, 0);
    if (r <= 0)
      return false;
    s += static_cast<size_t>(r);
  }
  return true;
}
static bool recv_all(int fd, uint8_t* buf, size_t len) {
  size_t rcv = 0;
  while (rcv < len) {
    ssize_t r = ::recv(fd, buf + rcv, len - rcv, 0);
    if (r <= 0)
      return false;
    rcv += static_cast<size_t>(r);
  }
  return true;
}

string build_frame(uint8_t op, const string& key, const string& val) {
  uint32_t k = static_cast<uint32_t>(key.size());
  uint32_t v = static_cast<uint32_t>(val.size());
  uint32_t total = 4 + 1 + 2 + k + 3 + v;
  string s;
  s.resize(total);
  uint8_t* p = reinterpret_cast<uint8_t*>(&s[0]);
  write_u32_be(p, total);
  p += 4;
  *p++ = op;
  write_u16_be(p, static_cast<uint16_t>(k));
  p += 2;
  if (k) {
    memcpy(p, key.data(), k);
    p += k;
  }
  write_u24_be(p, v);
  p += 3;
  if (v)
    memcpy(p, val.data(), v);
  return s;
}

bool read_response(int fd, uint8_t& status, string& value) {
  uint8_t hdr[8];
  if (!recv_all(fd, hdr, 8))
    return false;
  uint32_t total = read_u32_be(hdr);
  status = hdr[4];
  uint32_t vlen = read_u24_be(hdr + 5);
  if (total < 8 || vlen + 8 != total)
    return false;
  if (vlen) {
    value.resize(vlen);
    if (!recv_all(fd, reinterpret_cast<uint8_t*>(&value[0]), vlen))
      return false;
  } else
    value.clear();
  return true;
}

struct Args {
  string host = "127.0.0.1";
  string port = "8990";
  vector<string> cmds;
  uint64_t total = 100000;
  size_t concurrency = 50;
  uint64_t keyspace = 100000;
  size_t value_size = 16;
  bool sequential = false;
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    string s = argv[i];
    if (s == "-h" || s == "--help") {
      throw runtime_error("help");
    }
    if (s == "-t") {
      if (i + 1 >= argc)
        throw runtime_error("-t needs arg");
      string list = argv[++i];
      string tok;
      stringstream ss(list);
      while (getline(ss, tok, ','))
        if (!tok.empty())
          a.cmds.push_back(tok);
    } else if (s == "-n") {
      a.total = stoull(argv[++i]);
    } else if (s == "-c") {
      a.concurrency = stoul(argv[++i]);
    } else if (s == "-r") {
      a.keyspace = stoull(argv[++i]);
    } else if (s == "-d") {
      a.value_size = stoul(argv[++i]);
    } else if (s == "--host") {
      a.host = argv[++i];
    } else if (s == "--port") {
      a.port = argv[++i];
    } else if (s == "--sequential") {
      a.sequential = true;
    }
  }
  if (a.cmds.empty())
    a.cmds.push_back("get");
  return a;
}

int main(int argc, char** argv) {
  try {
    Args a = parse(argc, argv);
    Server::initLogger();
    Server::setLevel(spdlog::level::off);
    cout << "rbench-like: host=" << a.host << " port=" << a.port << " total=" << a.total
         << " concurrency=" << a.concurrency << " keyspace=" << a.keyspace
         << " value_size=" << a.value_size << " sequential=" << (a.sequential ? "true" : "false")
         << " cmds=";
    for (auto& c : a.cmds)
      cout << c << ",";
    cout << "\n";

    atomic<uint64_t> next_request{0};
    atomic<uint64_t> ok{0}, fail{0};
    atomic<uint64_t> connect_fail{0}, io_fail{0}, parse_fail{0};
    vector<uint64_t> latencies;
    latencies.reserve((size_t)min<uint64_t>(a.total, 200000));
    mutex lat_m;

    auto worker = [&](size_t id) {
      try {
        net::Socket sock;
        sock.connect(a.host, a.port);
        int fd = sock.fd();
        mt19937_64 rng((uint64_t)chrono::steady_clock::now().time_since_epoch().count() ^
                       (id * 0x9e3779b97f4a7c15ULL));
        uniform_int_distribution<uint64_t> keyd(0, a.keyspace ? a.keyspace - 1 : 0);
        uniform_int_distribution<int> cmdidx(0, (int)a.cmds.size() - 1);
        while (true) {
          uint64_t index = next_request.fetch_add(1);
          if (index >= a.total)
            break;

          uint64_t key_index = a.sequential ? (index % (a.keyspace ? a.keyspace : 1)) : keyd(rng);
          string key = string("k") + to_string(key_index);
          string val(a.value_size, 'x');
          string cmd = a.cmds[cmdidx(rng)];
          uint8_t op = 0;
          if (cmd == "get")
            op = 0;
          else if (cmd == "set")
            op = 1;
          else if (cmd == "del")
            op = 2;

          string frame = build_frame(op, key, (op == 1) ? val : string());
          auto t0 = chrono::steady_clock::now();
          if (!send_all(fd, reinterpret_cast<const uint8_t*>(frame.data()), frame.size())) {
            io_fail.fetch_add(1);
            fail.fetch_add(1);
            break;
          }

          uint8_t status = 0;
          string body;
          if (!read_response(fd, status, body)) {
            parse_fail.fetch_add(1);
            fail.fetch_add(1);
            break;
          }

          auto t1 = chrono::steady_clock::now();
          uint64_t us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
          {
            lock_guard<mutex> g(lat_m);
            if (latencies.size() < latencies.capacity())
              latencies.push_back(us);
          }
          if (status == 0xC8)
            ok.fetch_add(1);
          else
            fail.fetch_add(1);
        }
      } catch (const exception& e) {
        connect_fail.fetch_add(1);
        lock_guard<mutex> g(lat_m);
        cerr << "worker-" << id << " error: " << e.what() << '\n';
      }
    };

    vector<thread> th;
    th.reserve(a.concurrency);
    auto start = chrono::steady_clock::now();
    for (size_t i = 0; i < a.concurrency; i++)
      th.emplace_back(worker, i);
    for (auto& t : th)
      t.join();
    auto end = chrono::steady_clock::now();
    double elapsed = chrono::duration<double>(end - start).count();
    uint64_t total_done = ok.load() + fail.load();
    double qps = elapsed > 0 ? static_cast<double>(total_done) / elapsed : 0;
    sort(latencies.begin(), latencies.end());
    auto pct = [&](double p) -> uint64_t {
      if (latencies.empty())
        return 0;
      size_t idx = min(latencies.size() - 1, (size_t)floor(p * (latencies.size() - 1)));
      return latencies[idx];
    };
    cout << "done total=" << total_done << " ok=" << ok.load() << " fail=" << fail.load()
         << " connect_fail=" << connect_fail.load() << " io_fail=" << io_fail.load()
         << " parse_fail=" << parse_fail.load() << " elapsed_s=" << elapsed << " qps=" << qps
         << " p50_us=" << pct(0.5) << " p95_us=" << pct(0.95) << " p99_us=" << pct(0.99) << "\n";
    return 0;
  } catch (const exception& e) {
    cerr << e.what() << "\n";
    cerr << "Usage: -t get,set -n total -c concurrency -r keyspace -d value_size [--sequential] "
            "--host --port\n";
    return 1;
  }
}
