#include "storage/wal/wal_writer.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "util/Log.hpp"
#include "util/crc32.hpp"

namespace wal {
namespace {

bool writeAll(int fd, const char* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const ssize_t result = ::write(fd, data + written, size - written);
    if (result > 0) {
      written += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

std::string encodeRecord(const LogRecord& record) {
  RecordHeader header{};
  header.key_size = record.key.size();
  header.value_size = record.value.size();
  header.type = static_cast<uint8_t>(record.type);

  std::string buffer;
  buffer.reserve(sizeof(header) + record.key.size() + record.value.size());
  buffer.append(reinterpret_cast<const char*>(&header), sizeof(header));
  buffer.append(record.key.data(), record.key.size());
  buffer.append(record.value.data(), record.value.size());

  const uint32_t crc = util::crc32::Value(
      buffer.data() + sizeof(uint32_t), buffer.size() - sizeof(uint32_t));
  std::memcpy(buffer.data(), &crc, sizeof(crc));
  return buffer;
}

}  // namespace

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
  appendBatch(encodeRecord(record), [](bool) {});
}

void WALWriter::appendBatch(std::string data, CompletionCallback callback) {
  bool accepted = false;
  {
    std::lock_guard lock(mtx_);
    if (fd_ >= 0 && !stop_) {
      tasks_.push_back({std::move(data), std::move(callback)});
      accepted = true;
    }
  }

  if (!accepted) {
    callback(false);
    return;
  }
  cv_.notify_one();
}

void WALWriter::writeLoop() {
  while (true) {
    WALTask task;

    {
      std::unique_lock lock(mtx_);
      cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

      if (stop_ && tasks_.empty()) {
        break;
      }

      task = std::move(tasks_.front());
      tasks_.pop_front();
    }

    bool success = writeAll(fd_, task.data.data(), task.data.size());
    if (success) {
      success = ::fdatasync(fd_) == 0;
    }
    if (!success) {
      LOG_ERROR("Failed to persist WAL batch: {}", std::strerror(errno));
    }

    task.completion(success);
  }
  LOG_INFO("WAL writer thread stopped");
}

}  // namespace wal
