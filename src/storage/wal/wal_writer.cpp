#include "storage/wal/wal_writer.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#include "util/crc32.hpp"
#include "util/Log.hpp"

namespace wal {

WALWriter::WALWriter(const std::filesystem::path& walPath) {
  fd_ = ::open(walPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd_ < 0) {
    LOG_ERROR("Failed to open WAL file: {}", walPath.string());
    return;
  }
  LOG_INFO("WAL opened for writing: {}", walPath.string());
  write_thread_ = std::thread(&WALWriter::writeLoop, this);
}

WALWriter::~WALWriter() {
  {
    std::lock_guard lock(mtx_);
    stop_ = true;
  }
  cv_.notify_one();
  if (write_thread_.joinable()) {
    write_thread_.join();
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void WALWriter::append(const LogRecord& record) {
  if (fd_ < 0) return;

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
  std::memcpy(buffer.data(), &crc, sizeof(crc));

  {
    std::unique_lock lock(mtx_);
    if (queue_.size() >= max_queue_size_) {
      LOG_WARN("WAL queue full ({}), dropping record for key={}", queue_.size(), record.key);
      return;
    }
    queue_.push(std::move(buffer));
  }
  cv_.notify_one();
}

void WALWriter::writeLoop() {
  while (true) {
    std::queue<std::string> batch;
    {
      std::unique_lock lock(mtx_);
      cv_.wait_for(lock, flush_interval_, [this] { return !queue_.empty() || stop_; });

      if (queue_.empty() && stop_) break;

      batch.swap(queue_);
    }
    cv_.notify_all();

    if (batch.empty()) continue;

    std::string write_buf;
    while (!batch.empty()) {
      write_buf.append(batch.front());
      batch.pop();
    }

    ::write(fd_, write_buf.data(), write_buf.size());
    ::fsync(fd_);
  }
}

}  // namespace wal
