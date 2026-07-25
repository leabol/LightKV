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
  //get操作先查询activeStorage_，如果找不到，再查询immutableStorage_，之后在查询sstable_，如果都找不到，则返回not found。
  Response GET(const Request &req);
  Response SET(const Request &req);
  Response DEL(const Request &req);

private:
  // flush线程的loop函数
  void flushLoop() {
  }
  //当storage_的大小超过kMaxMemtableSize时，可以将其转为immutableStorage_，并创建一个新的storage_，以便继续处理新的写请求。

  // flush线程被唤醒后，进行flush操作，将immutableStorage_中的数据写入磁盘。
  void flushToDisk() {
  } 
  bool stopFlushThread_{false}; // 用于控制flush线程的停止

  std::mutex mutex_;
  Storage storage_;
 // 需要一个immutableStorage_队列，用来存储待flush的数据块。每当storage_达到一定大小时，将其转为immutableStorage_，并创建一个新的storage_。;
  std::mutex flush_mutex_;
  std::condition_variable flush_cv_;
  std::thread flushThread_;
};
}// namespace storage