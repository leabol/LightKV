#include "storage/wal/wal_writer.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "protocol/request.hpp"
#include "server/server_def.hpp"
#include "storage/wal/log_record.hpp"
#include "util/Log.hpp"
#include "util/crc32.hpp"

namespace wal {
namespace {

std::string encodeRecord(const protocol::Request& req) {
  LogRecord record{wal::RecordType(req.cmd), req.key, req.value};
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

bool writeAll(int fd, const std::vector<server::PendingRequest>& batch, bool* wrote_any) {
  *wrote_any = false;
  std::string writeBatch;
  for (const auto & pending : batch){
    // 读命令也走这条队列（为了保持执行顺序），但不能写进 WAL
    if (pending.request.cmd == protocol::CommandType::GET) {
      continue;
    }
    writeBatch += encodeRecord(pending.request);
  }
  if (writeBatch.empty()) {
    return true;  // 整批都是读：无需落盘
  }
  size_t written = 0;
  size_t total_size = writeBatch.size();
  while (written < total_size) {
    const ssize_t result = ::write(fd, writeBatch.data() + written, total_size - written);
    if (result > 0) {
      written += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  *wrote_any = true;
  return true;
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

void WALWriter::appendBatch(std::vector<server::PendingRequest> batch){
  bool accepted = false;
  const auto batchSize = batch.size();
  std::size_t queueSize = 0;
  bool stopped = false;
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (fd_ >= 0 && !stop_) {
      batch_que_.push(std::move(batch));
      accepted = true;
      queueSize = batch_que_.size();
    }
    stopped = stop_;
    fd = fd_;
  }
  if (!accepted) {
    LOG_ERROR("Rejecting WAL batch: requests={} writer_stopped={} fd={}",
              batchSize, stopped, fd);
    processor_->handleGroupRequest(std::move(batch), false);
    return;
  }
  LOG_DEBUG("WAL batch queued: requests={} queue_size={}", batchSize, queueSize);
  cv_.notify_one();
}


void WALWriter::writeLoop() {
  pthread_setname_np(pthread_self(), "wal-thread"); 
  LOG_INFO("WAL writer thread started");
  while (true) {
    std::vector<server::PendingRequest> pendings;
    {
      std::unique_lock lock(mtx_);
      cv_.wait(lock, [this] { return stop_ || !batch_que_.empty(); });

      if (stop_ && batch_que_.empty()) {
        break;
      }

      pendings = std::move(batch_que_.front());
      batch_que_.pop();
    }

    bool wroteAny = false;
    bool success = writeAll(fd_, pendings, &wroteAny);
    const auto batchSize = pendings.size();
    if (success && wroteAny) {
      success = ::fdatasync(fd_) == 0;
    }
    if (!success) {
      LOG_ERROR("Failed to persist WAL batch: requests={} error={}",
                batchSize, std::strerror(errno));
    } else {
      LOG_DEBUG("WAL batch persisted: requests={}", batchSize);
    }

    processor_->handleGroupRequest(std::move(pendings), success);
  }
  LOG_INFO("WAL writer thread stopped");
}

}  // namespace wal
