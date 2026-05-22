#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "Log.hpp"
#include "Socket.hpp"

namespace {

constexpr uint8_t kOpGet = 0;
constexpr uint8_t kOpSet = 1;

struct Config {
  std::string host{"127.0.0.1"};
  std::string port{"8990"};
  size_t warmup_keys{4096};
  size_t threads{4};
  size_t requests_per_thread{1000};
  size_t value_size{16};
  size_t failure_samples{16};
};

struct FailureSample {
  std::string key;
  uint8_t status{0xFF};
  std::string message;
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
    ssize_t n = ::send(fd, data + sent, len - sent, 0);
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
    ssize_t n = ::recv(fd, data + recvd, len - recvd, 0);
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
    std::string arg = argv[i];
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
    } else if (arg == "--warmup-keys") {
      cfg.warmup_keys = static_cast<size_t>(std::stoul(next_value("--warmup-keys")));
    } else if (arg == "--threads") {
      cfg.threads = static_cast<size_t>(std::stoul(next_value("--threads")));
    } else if (arg == "--requests") {
      cfg.requests_per_thread = static_cast<size_t>(std::stoul(next_value("--requests")));
    } else if (arg == "--value-size") {
      cfg.value_size = static_cast<size_t>(std::stoul(next_value("--value-size")));
    } else if (arg == "--samples") {
      cfg.failure_samples = static_cast<size_t>(std::stoul(next_value("--samples")));
    } else {
      throw std::runtime_error("unknown arg: " + arg);
    }
  }
  return cfg;
}

void warmup_phase(const Config& cfg,
                  std::atomic<uint64_t>& ok_count,
                  std::atomic<uint64_t>& fail_count) {
  try {
    net::Socket sock;
    sock.connect(cfg.host, cfg.port);
    const int fd = sock.fd();

    for (size_t i = 0; i < cfg.warmup_keys; ++i) {
      const std::string key = "k" + std::to_string(i);
      const std::string value = "warmup_" + std::to_string(i);
      const std::string req = build_request(kOpSet, key, value);
      if (!send_all(fd, reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
        ++fail_count;
        return;
      }

      uint8_t status = 0;
      std::string resp_value;
      if (!read_response(fd, status, resp_value)) {
        ++fail_count;
        return;
      }

      if (status == 0xC8) {
        ++ok_count;
      } else {
        ++fail_count;
      }
    }
  } catch (const std::exception&) { ++fail_count; }
}

void get_worker(const Config& cfg,
                size_t thread_index,
                std::atomic<uint64_t>& ok_count,
                std::atomic<uint64_t>& fail_count,
                std::vector<uint64_t>& latencies_us,
                std::mutex& latencies_mutex,
                std::vector<FailureSample>& failures,
                std::mutex& failures_mutex) {
  try {
    net::Socket sock;
    sock.connect(cfg.host, cfg.port);
    const int fd = sock.fd();

    std::mt19937_64 rng(
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
        (static_cast<uint64_t>(thread_index) * 0x9e3779b97f4a7c15ULL));
    std::uniform_int_distribution<uint64_t> key_dist(
        0, cfg.warmup_keys == 0 ? 0 : cfg.warmup_keys - 1);

    std::vector<uint64_t> local_latencies;
    local_latencies.reserve(cfg.requests_per_thread);
    std::vector<FailureSample> local_failures;
    local_failures.reserve(std::min(cfg.failure_samples, cfg.requests_per_thread));

    for (size_t seq = 0; seq < cfg.requests_per_thread; ++seq) {
      const uint64_t key_id = key_dist(rng);
      const std::string key = "k" + std::to_string(key_id);
      const std::string req = build_request(kOpGet, key, "");

      const auto begin = std::chrono::steady_clock::now();
      if (!send_all(fd, reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
        ++fail_count;
        std::lock_guard<std::mutex> guard(failures_mutex);
        if (failures.size() < cfg.failure_samples) {
          failures.push_back({key, 0xFF, "send failed"});
        }
        break;
      }

      uint8_t status = 0;
      std::string resp_value;
      if (!read_response(fd, status, resp_value)) {
        ++fail_count;
        std::lock_guard<std::mutex> guard(failures_mutex);
        if (failures.size() < cfg.failure_samples) {
          failures.push_back({key, 0xFF, "recv failed"});
        }
        break;
      }

      const auto end = std::chrono::steady_clock::now();
      const uint64_t latency_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
      local_latencies.push_back(latency_us);

      if (status == 0xC8) {
        ++ok_count;
      } else {
        ++fail_count;
        local_failures.push_back({key, status, resp_value});
      }
    }

    {
      std::lock_guard<std::mutex> guard(latencies_mutex);
      latencies_us.insert(latencies_us.end(), local_latencies.begin(), local_latencies.end());
    }
    if (!local_failures.empty()) {
      std::lock_guard<std::mutex> guard(failures_mutex);
      for (const auto& sample : local_failures) {
        if (failures.size() >= cfg.failure_samples) {
          break;
        }
        failures.push_back(sample);
      }
    }
  } catch (const std::exception&) { ++fail_count; }
}

uint64_t percentile_us(std::vector<uint64_t> values, double p) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const size_t index = std::min(
      values.size() - 1, static_cast<size_t>(std::floor(p * static_cast<double>(values.size() - 1))));
  return values[index];
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Config cfg = parse_args(argc, argv);

    Server::initLogger();
    Server::setLevel(spdlog::level::off);

    std::cout << "load test config:"
              << " host=" << cfg.host << " port=" << cfg.port << " warmup_keys=" << cfg.warmup_keys
              << " threads=" << cfg.threads << " requests/thread=" << cfg.requests_per_thread
              << " value_size=" << cfg.value_size << '\n';

    std::atomic<uint64_t> warmup_ok{0};
    std::atomic<uint64_t> warmup_fail{0};
    warmup_phase(cfg, warmup_ok, warmup_fail);
    if (warmup_fail.load() != 0) {
      std::cout << "warmup finished with failures: ok=" << warmup_ok.load()
                << " fail=" << warmup_fail.load() << '\n';
    } else {
      std::cout << "warmup ok=" << warmup_ok.load() << '\n';
    }

    std::atomic<uint64_t> get_ok{0};
    std::atomic<uint64_t> get_fail{0};

    std::vector<uint64_t> latencies_us;
    latencies_us.reserve(cfg.threads * cfg.requests_per_thread);
    std::mutex latencies_mutex;

    std::vector<FailureSample> failures;
    failures.reserve(cfg.failure_samples);
    std::mutex failures_mutex;

    const auto begin = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(cfg.threads);
    for (size_t i = 0; i < cfg.threads; ++i) {
      threads.emplace_back(get_worker,
                           std::cref(cfg),
                           i,
                           std::ref(get_ok),
                           std::ref(get_fail),
                           std::ref(latencies_us),
                           std::ref(latencies_mutex),
                           std::ref(failures),
                           std::ref(failures_mutex));
    }

    for (auto& thread : threads) {
      thread.join();
    }

    const auto end = std::chrono::steady_clock::now();
    const double elapsed_s = std::chrono::duration<double>(end - begin).count();
    const uint64_t total = get_ok.load() + get_fail.load();
    const double qps = elapsed_s > 0.0 ? static_cast<double>(total) / elapsed_s : 0.0;

    const uint64_t p50 = percentile_us(latencies_us, 0.50);
    const uint64_t p95 = percentile_us(latencies_us, 0.95);
    const uint64_t p99 = percentile_us(latencies_us, 0.99);

    std::cout << "done: total=" << total << " ok=" << get_ok.load() << " fail=" << get_fail.load()
              << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed_s
              << " qps=" << std::fixed << std::setprecision(2) << qps << " p50_us=" << p50
              << " p95_us=" << p95 << " p99_us=" << p99 << '\n';

    if (!failures.empty()) {
      std::cout << "failure samples:" << '\n';
      for (const auto& sample : failures) {
        std::cout << "  key=" << sample.key << " status=0x" << std::hex << std::uppercase
                  << static_cast<int>(sample.status) << std::dec << " message=" << sample.message
                  << '\n';
      }
    }

    return (warmup_fail.load() == 0 && get_fail.load() == 0) ? 0 : 2;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}