#pragma once
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#include "storage/wal/log_record.hpp"
#include "storage/memtable/memtable.hpp"
#include "util/crc32.hpp"
#include "util/Log.hpp"

namespace wal {
class WALReader {
public:
  explicit WALReader(const std::filesystem::path& walPath = "wal") {
    fd_ = ::open(walPath.c_str(), O_RDONLY);
    if (fd_ < 0) {
      valid_ = false;
      LOG_WARN("WAL file not found: {}", walPath.string());
      return;
    }
    valid_ = true;
    LOG_DEBUG("WAL opened: {}", walPath.string());
  }

  ~WALReader() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  // 读取下一条记录，失败或EOF返回false
  bool next(LogRecord& record) {
    if (!valid_) return false;

    // 读取header
    RecordHeader header{};
    if (!readExact(&header, sizeof(header))) {
      return false;
    }

    uint32_t key_size = header.key_size;
    uint32_t value_size = header.value_size;

    // 读取key和value
    size_t data_size = key_size + value_size;
    std::string data(data_size, '\0');
    if (data_size > 0 && !readExact(&data[0], data_size)) {
      LOG_WARN("WAL truncated record: key_size={} value_size={}", key_size, value_size);
      return false;
    }

    // 验证CRC：重建完整buffer，CRC字段置零后计算
    size_t total_size = sizeof(header) + data_size;
    std::string buffer(total_size, '\0');
    std::memcpy(buffer.data(), &header, sizeof(header));
    *reinterpret_cast<uint32_t*>(buffer.data()) = 0;
    if (data_size > 0) {
      std::memcpy(buffer.data() + sizeof(header), data.data(), data_size);
    }

    uint32_t expected_crc = util::crc32::Value(
        buffer.data() + sizeof(uint32_t), buffer.size() - sizeof(uint32_t));

    if (header.crc != expected_crc) {
      LOG_WARN("WAL CRC mismatch: stored={:#x} computed={:#x}", header.crc, expected_crc);
      return false;
    }

    record.type = static_cast<RecordType>(header.type);
    record.key = data.substr(0, key_size);
    record.value = data.substr(key_size, value_size);
    return true;
  }

  // 从WAL文件恢复Memtable
  static void Recover(const std::filesystem::path& walPath, storage::Memtable& memtable) {
    WALReader reader(walPath);
    if (!reader.valid_) return;

    size_t count = 0;
    LogRecord record;
    while (reader.next(record)) {
      if (record.type == RecordType::SET) {
        protocol::Request req{protocol::CommandType::SET, record.key, record.value};
        memtable.SET(req);
        LOG_DEBUG("Recover SET: key={}", req.key);
      } else if (record.type == RecordType::DELETE) {
        protocol::Request req{protocol::CommandType::DEL, record.key, ""};
        memtable.DEL(req);
        LOG_DEBUG("Recover DEL: key={}", req.key);
      }
      ++count;
    }
    LOG_INFO("WAL recovery complete: {} records applied", count);
  }

private:
  bool readExact(void* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
      ssize_t n = ::read(fd_, static_cast<char*>(buf) + received, len - received);
      if (n <= 0) return false;
      received += n;
    }
    return true;
  }

  int fd_{-1};
  bool valid_{false};
};
}  // namespace wal
