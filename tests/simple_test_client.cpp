#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/Socket.hpp"
#include "util/Log.hpp"

namespace {

constexpr uint8_t kOpGet = 0;
constexpr uint8_t kOpSet = 1;

struct Config {
  std::string host{"127.0.0.1"};
  std::string port{"8990"};
  std::string mode{"smoke"};
  size_t keys{1000};
  size_t requests{1000};
  size_t threads{4};
  size_t value_size{16};
};

void write_u32_be(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  data[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[3] = static_cast<uint8_t>(value & 0xFF);
}

void write_u24_be(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[2] = static_cast<uint8_t>(value & 0xFF);
}

void write_u16_be(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[1] = static_cast<uint8_t>(value & 0xFF);
}

uint32_t read_u32_be(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint32_t read_u24_be(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) |
         static_cast<uint32_t>(data[2]);
}

bool send_all(int fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const ssize_t n = ::send(fd, data + sent, len - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool recv_all(int fd, uint8_t* data, size_t len) {
  size_t recvd = 0;
  while (recvd < len) {
    const ssize_t n = ::recv(fd, data + recvd, len - recvd, 0);
    if (n <= 0) {
      return false;
    }
    recvd += static_cast<size_t>(n);
  }
  return true;
}

std::string build_request(uint8_t op, const std::string& key, const std::string& value) {
  const uint32_t key_len = static_cast<uint32_t>(key.size());
  const uint32_t value_len = static_cast<uint32_t>(value.size());
  const uint32_t total_len = 4 + 1 + 2 + key_len + 3 + value_len;

  std::string frame;
  frame.resize(total_len);
  auto* out = reinterpret_cast<uint8_t*>(&frame[0]);

  write_u32_be(out, total_len);
  out += 4;
  *out++ = op;
  write_u16_be(out, static_cast<uint16_t>(key_len));
  out += 2;
  if (key_len > 0) {
    std::memcpy(out, key.data(), key_len);
    out += key_len;
  }
  write_u24_be(out, value_len);
  out += 3;
  if (value_len > 0) {
    std::memcpy(out, value.data(), value_len);
  }
  return frame;
}

bool read_response(int fd, uint8_t& status, std::string& value) {
  uint8_t hdr[8];
  if (!recv_all(fd, hdr, sizeof(hdr))) {
    return false;
  }

  const uint32_t total_len = read_u32_be(hdr);
  status = hdr[4];
  const uint32_t value_len = read_u24_be(hdr + 5);
  if (total_len < 8 || value_len + 8 != total_len) {
    return false;
  }

  value.clear();
  value.resize(value_len);
  if (value_len > 0 && !recv_all(fd, reinterpret_cast<uint8_t*>(&value[0]), value_len)) {
    return false;
  }
  return true;
}

Config parse_args(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[++i];
    };

    if (arg == "--host") {
      cfg.host = next_value("--host");
    } else if (arg == "--port") {
      cfg.port = next_value("--port");
    } else if (arg == "--mode") {
      cfg.mode = next_value("--mode");
    } else if (arg == "--keys") {
      cfg.keys = static_cast<size_t>(std::stoul(next_value("--keys")));
    } else if (arg == "--requests") {
      cfg.requests = static_cast<size_t>(std::stoul(next_value("--requests")));
    } else if (arg == "--threads") {
      cfg.threads = static_cast<size_t>(std::stoul(next_value("--threads")));
    } else if (arg == "--value-size") {
      cfg.value_size = static_cast<size_t>(std::stoul(next_value("--value-size")));
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  return cfg;
}

void print_usage() {
  std::cout << "Usage: ./build/lightkv_simple_test_client --mode smoke|get|set "
               "[--host 127.0.0.1] [--port 8990] [--keys 1000] [--requests 1000] "
               "[--threads 4] [--value-size 16]\n";
}

bool run_smoke(const Config& cfg) {
  net::Socket sock;
  sock.connect(cfg.host, cfg.port);
  const int fd = sock.fd();

  const std::string set_req = build_request(kOpSet, "smoke_key", "smoke_value");
  if (!send_all(fd, reinterpret_cast<const uint8_t*>(set_req.data()), set_req.size())) {
    std::cerr << "smoke: SET send failed\n";
    return false;
  }

  uint8_t status = 0;
  std::string value;
  if (!read_response(fd, status, value)) {
    std::cerr << "smoke: SET response read failed\n";
    return false;
  }
  std::cout << "smoke SET status=0x" << std::hex << std::uppercase << static_cast<int>(status)
            << std::dec << " value=" << value << '\n';

  const std::string get_req = build_request(kOpGet, "smoke_key", "");
  if (!send_all(fd, reinterpret_cast<const uint8_t*>(get_req.data()), get_req.size())) {
    std::cerr << "smoke: GET send failed\n";
    return false;
  }
  if (!read_response(fd, status, value)) {
    std::cerr << "smoke: GET response read failed\n";
    return false;
  }
  std::cout << "smoke GET status=0x" << std::hex << std::uppercase << static_cast<int>(status)
            << std::dec << " value=" << value << '\n';

  return true;
}

bool run_set_load(const Config& cfg) {
  std::vector<std::string> keys;
  keys.reserve(cfg.keys);
  for (size_t i = 0; i < cfg.keys; ++i) {
    keys.push_back("k" + std::to_string(i));
  }

  std::atomic<uint64_t> ok{0};
  std::atomic<uint64_t> fail{0};

  auto worker = [&](size_t worker_id, size_t begin, size_t end) {
    try {
      net::Socket sock;
      sock.connect(cfg.host, cfg.port);
      const int fd = sock.fd();
      const std::string value(cfg.value_size, 'x');

      const size_t span = (end > begin) ? (end - begin) : 1;
      for (size_t seq = 0; seq < cfg.requests; ++seq) {
        const size_t key_idx = begin + (seq % span);
        const std::string& key = keys[key_idx];
        const std::string req = build_request(kOpSet, key, value);

        if (!send_all(fd, reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
          ++fail;
          return;
        }

        uint8_t status = 0;
        std::string resp_value;
        if (!read_response(fd, status, resp_value)) {
          ++fail;
          return;
        }

        if (status == 0xC8) {
          ++ok;
        } else {
          ++fail;
        }
      }
    } catch (const std::exception&) {
      ++fail;
    }
  };

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(cfg.threads);

  const size_t per_thread = cfg.keys == 0 ? 1 : std::max<size_t>(1, cfg.keys / cfg.threads);
  for (size_t i = 0; i < cfg.threads; ++i) {
    const size_t begin = std::min(cfg.keys, i * per_thread);
    const size_t end = (i + 1 == cfg.threads) ? cfg.keys : std::min(cfg.keys, begin + per_thread);
    threads.emplace_back(worker, i, begin, end);
  }

  for (auto& thread : threads) {
    thread.join();
  }

  const auto end = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(end - start).count();
  const uint64_t total = ok.load() + fail.load();
  const double qps = elapsed > 0.0 ? static_cast<double>(total) / elapsed : 0.0;

  std::cout << "done mode=set total=" << total << " ok=" << ok.load() << " fail=" << fail.load()
            << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed << " qps="
            << std::fixed << std::setprecision(2) << qps << '\n';
  return fail.load() == 0;
}

bool run_get_load(const Config& cfg) {
  std::vector<std::string> keys;
  keys.reserve(cfg.keys);
  for (size_t i = 0; i < cfg.keys; ++i) {
    keys.push_back("k" + std::to_string(i));
  }

  std::atomic<uint64_t> ok{0};
  std::atomic<uint64_t> fail{0};

  auto worker = [&](size_t worker_id, size_t begin, size_t end) {
    try {
      net::Socket sock;
      sock.connect(cfg.host, cfg.port);
      const int fd = sock.fd();

      const size_t span = (end > begin) ? (end - begin) : 1;
      std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                          (worker_id * 0x9e3779b97f4a7c15ULL));
      std::uniform_int_distribution<size_t> offset_dist(0, span - 1);

      for (size_t seq = 0; seq < cfg.requests; ++seq) {
        const size_t key_idx = begin + offset_dist(rng);
        const std::string& key = keys[key_idx];
        const std::string req = build_request(kOpGet, key, "");

        if (!send_all(fd, reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
          ++fail;
          return;
        }

        uint8_t status = 0;
        std::string resp_value;
        if (!read_response(fd, status, resp_value)) {
          ++fail;
          return;
        }

        if (status == 0xC8) {
          ++ok;
        } else {
          ++fail;
        }
      }
    } catch (const std::exception&) {
      ++fail;
    }
  };

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(cfg.threads);

  const size_t per_thread = cfg.keys == 0 ? 1 : std::max<size_t>(1, cfg.keys / cfg.threads);
  for (size_t i = 0; i < cfg.threads; ++i) {
    const size_t begin = std::min(cfg.keys, i * per_thread);
    const size_t end = (i + 1 == cfg.threads) ? cfg.keys : std::min(cfg.keys, begin + per_thread);
    threads.emplace_back(worker, i, begin, end);
  }

  for (auto& thread : threads) {
    thread.join();
  }

  const auto end = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(end - start).count();
  const uint64_t total = ok.load() + fail.load();
  const double qps = elapsed > 0.0 ? static_cast<double>(total) / elapsed : 0.0;

  std::cout << "done mode=get total=" << total << " ok=" << ok.load() << " fail=" << fail.load()
            << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed << " qps="
            << std::fixed << std::setprecision(2) << qps << '\n';
  return fail.load() == 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Config cfg = parse_args(argc, argv);

    Server::initLogger();
    Server::setLevel(spdlog::level::off);

    std::cout << "LightKV simple test client\n"
              << "  host=" << cfg.host << " port=" << cfg.port << " mode=" << cfg.mode
              << " keys=" << cfg.keys << " requests=" << cfg.requests
              << " threads=" << cfg.threads << " value_size=" << cfg.value_size << '\n';

    if (cfg.mode == "smoke") {
      return run_smoke(cfg) ? 0 : 1;
    }
    if (cfg.mode == "set") {
      return run_set_load(cfg) ? 0 : 1;
    }
    if (cfg.mode == "get") {
      return run_get_load(cfg) ? 0 : 1;
    }

    print_usage();
    std::cerr << "unknown mode: " << cfg.mode << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    print_usage();
    return 1;
  }
}