#pragma  once

#include <map>
#include <string>
#include <mutex>
#include <memory>
#include <thread>
#include <condition_variable>

#include "protocol/response.hpp"
#include "protocol/request.hpp"

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
  Response GET(const Request &req);
  Response SET(const Request &req);
  Response DEL(const Request &req);

private:
  // flush线程的loop函数
  void flushLoop() {
    std::unique_lock<std::mutex> lock(flush_mutex_);
    while (true) {
      flush_cv_.wait(lock, [this] { return stopFlushThread_ || !immutableStorage_->empty(); });
      if (stopFlushThread_) {
        break;
      }
      // 执行flush操作，将immutableStorage_中的数据写入磁盘
      flushToDisk();
      // 清空immutableStorage_，释放内存空间
      immutableStorage_->clear();
    }
  }
  //当storage_的大小超过kMaxMemtableSize时，可以将其转为immutableStorage_，并创建一个新的storage_，以便继续处理新的写请求。
  // void switchToImmutable(){
  //   std::lock_guard<std::mutex> lock(mutex_);
  //   if (storage_->size() >= kMaxMemtableSize) {
  //     immutableStorage_ = std::move(storage_);
  //     storage_ = std::make_unique<Storage>();
  //     flush_cv_.notify_one(); // 通知flush线程进行flush操作
  //   }
  // }
  // flush线程被唤醒后，进行flush操作，将immutableStorage_中的数据写入磁盘，并在写入完成后清空immutableStorage_，以释放内存空间。
  void flushToDisk() {
    // 这里可以实现将immutableStorage_中的数据写入磁盘的逻辑
    // 例如，将数据序列化为二进制格式，写入文件等
    // 具体实现根据实际需求进行调整
  } 
  bool stopFlushThread_{false}; // 用于控制flush线程的停止

  std::mutex mutex_;
  Storage storage_;
  std::unique_ptr<Storage> immutableStorage_;
  std::mutex flush_mutex_;
  std::condition_variable flush_cv_;
  std::thread flushThread_;
};
}// namespace storage