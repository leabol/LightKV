#pragma  once

#include <map>
#include <string>
#include <mutex>
#include <memory>
#include <thread>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <vector>

#include "protocol/response.hpp"
#include "protocol/request.hpp"
#include "storage/sstable/sstable_reader.hpp"

using namespace protocol;

namespace storage {
class Memtable {

struct Entry {
  std::string value;
  bool deleted;
};
using Storage = std::map<std::string, Entry>;

constexpr static size_t kMaxMemtableSize = 64 * 1024; // 最大内存表大小为 64KB

public:
  explicit Memtable(std::filesystem::path sstable_dir = "data/sstable");
  ~Memtable();

  Memtable(const Memtable&) = delete;
  Memtable& operator=(const Memtable&) = delete;

  //get操作先查询activeStorage_，如果找不到，再查询immutableStorage_，之后在查询sstable_，如果都找不到，则返回not found。
  Response GET(const Request &req);
  Response SET(const Request &req);
  Response DEL(const Request &req);

private:
  void flushLoop();
  bool flushToDisk(const std::shared_ptr<Storage>& storage);
  void switchToImmutableLocked();
  bool lookupMemoryLocked(const std::string& key, Response* response) const;

  mutable std::mutex mutex_;
  Storage storage_;
  std::deque<std::shared_ptr<Storage>> immutable_storages_;
  std::shared_ptr<Storage> flushing_storage_;

  std::filesystem::path sstable_dir_;
  uint64_t next_sstable_id_{0};
  std::vector<std::unique_ptr<sstable::Reader>> sstables_;

  bool stopFlushThread_{false};
  std::condition_variable flush_cv_;
  std::thread flushThread_;
};
}  // namespace storage