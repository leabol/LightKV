#include "memtable.hpp"

#include <utility>

#include "storage/sstable/sstable_builder.hpp"

using namespace protocol;

namespace storage {

Memtable::Memtable(std::filesystem::path sstable_dir)
    : sstable_dir_(std::move(sstable_dir)),
      flushThread_(&Memtable::flushLoop, this) {}

Memtable::~Memtable() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopFlushThread_ = true;
  }
  flush_cv_.notify_one();
  if (flushThread_.joinable()) {
    flushThread_.join();
  }
}

Response Memtable::GET(const Request &req) {
  std::lock_guard<std::mutex> lock(mutex_);
  Response response;
  if (lookupMemoryLocked(req.key, &response)) {
    return response;
  }

  for (auto it = immutable_storages_.rbegin();
       it != immutable_storages_.rend(); ++it) {
    const auto found = (*it)->find(req.key);
    if (found != (*it)->end()) {
      if (found->second.deleted) {
        return {false, ""};
      }
      return {true, found->second.value};
    }
  }
  if (flushing_storage_ != nullptr) {
    const auto found = flushing_storage_->find(req.key);
    if (found != flushing_storage_->end()) {
      if (found->second.deleted) {
        return {false, ""};
      }
      return {true, found->second.value};
    }
  }

  for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
    std::string error;
    const auto result = (*it)->Lookup(req.key, &error);
    if (result.found) {
      if (result.deleted) {
        return {false, ""};
      }
      return {true, result.value};
    }
  }
  return {false, ""};
}

Response Memtable::SET(const Request &req) {
  std::lock_guard<std::mutex> lock(mutex_);
  storage_[req.key] = {req.value, false};
  if (storage_.size() >= kMaxMemtableSize) {
    switchToImmutableLocked();
  }
  return {true, ""};
}

Response Memtable::DEL(const Request &req) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = storage_.find(req.key);
  if (it != storage_.end() && !it->second.deleted) {
    it->second.deleted = true;
  } else if (it == storage_.end()) {
    storage_[req.key] = {"", true};
  } else {
    return {false, ""};
  }
  if (storage_.size() >= kMaxMemtableSize) {
    switchToImmutableLocked();
  }
  return {true, ""};
}

bool Memtable::lookupMemoryLocked(const std::string& key, Response* response) const {
  const auto it = storage_.find(key);
  if (it == storage_.end()) {
    return false;
  }
  if (it->second.deleted) {
    *response = {false, ""};
  } else {
    *response = {true, it->second.value};
  }
  return true;
}

// 必须在持有mutex_锁的情况下调用
void Memtable::switchToImmutableLocked() {
  auto immutable = std::make_shared<Storage>(std::move(storage_));
  storage_.clear();
  immutable_storages_.push_back(std::move(immutable));
  flush_cv_.notify_one();
}

bool Memtable::flushToDisk(const std::shared_ptr<Storage>& storage) {
  std::map<std::string, sstable::data_entry> entries;
  for (const auto& [key, entry] : *storage) {
    entries.emplace(key, sstable::data_entry{
                           key, entry.deleted ? "" : entry.value,
                           entry.deleted ? sstable::value_type::deletion
                                         : sstable::value_type::value});
  }

  const auto path = sstable_dir_ /
                    (std::to_string(next_sstable_id_++) + ".sst");
  sstable::Builder builder(path);
  std::string error;
  if (!builder.Build(entries, &error)) {
    return false;
  }

  auto reader = std::make_unique<sstable::Reader>(path);
  if (!reader->Open(&error)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sstables_.push_back(std::move(reader));
  }
  return true;
}

void Memtable::flushLoop() {
  while (true) {
    std::shared_ptr<Storage> immutable;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      flush_cv_.wait(lock, [this] {
        return stopFlushThread_ || !immutable_storages_.empty();
      });
      if (immutable_storages_.empty() && stopFlushThread_) {
        return;
      }
      immutable = immutable_storages_.front();
      immutable_storages_.pop_front();
      flushing_storage_ = immutable;
    }

    const bool success = flushToDisk(immutable);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!success) {
        immutable_storages_.push_front(std::move(immutable));
        if (stopFlushThread_) {
          return;// to-do: 如果队列不空，应该继续尝试刷新，直到队列为空
        }
      }
      flushing_storage_.reset();
      if (stopFlushThread_ && immutable_storages_.empty()) {
        return;
      }
    }
  }
}

}  // namespace storage
