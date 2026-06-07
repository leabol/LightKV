#pragma once
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>

#include "storage/wal/log_record.hpp"
#include "util/crc32.hpp"
#include "util/Log.hpp"

namespace wal {
class WALWriter {
public:
  WALWriter(const std::filesystem::path& walPath) {
    fd_ = ::open(walPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd_ < 0) {
      throw std::runtime_error("Failed to open WAL file: " + walPath.string());
    }
    LOG_INFO("WAL opened for writing: {}", walPath.string());
  }

  ~WALWriter() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void append(const LogRecord& record) {
    wal::RecordHeader header{};
    header.crc = 0;
    header.key_size = record.key.size();
    header.value_size = record.value.size();
    header.type = static_cast<uint8_t>(record.type);

    size_t total_size = sizeof(header) + record.key.size() + record.value.size();
    std::string buffer;
    buffer.reserve(total_size);
    buffer.append(reinterpret_cast<const char*>(&header), sizeof(header));
    buffer.append(record.key.data(), record.key.size());
    buffer.append(record.value.data(), record.value.size());

    uint32_t crc = util::crc32::Value(buffer.data() + sizeof(uint32_t), buffer.size() - sizeof(uint32_t));

    *reinterpret_cast<uint32_t*>(buffer.data()) = crc;

    ::write(fd_, buffer.data(), buffer.size());
    ::fsync(fd_);
    LOG_DEBUG("WAL append: type={} key={} size={}", static_cast<int>(record.type), record.key, total_size);
  }

private:
  int fd_{-1};
};
}
