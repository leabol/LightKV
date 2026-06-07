#pragma  once

#include <unordered_map>
#include <string>
#include <mutex>

#include "protocol/response.hpp"
#include "protocol/request.hpp"

using namespace protocol;
using Storage = std::unordered_map<std::string, std::string>;

namespace storage {
class Memtable {
public:
  Response GET(const Request &req) {
    std::lock_guard lock(storageMutex_);
    auto it = storage_.find(req.key);
    if (it == storage_.end()) {
      return {false, "not found key"};
    }
    return {true, it->second};
  }

  Response SET(const Request &req) {
    std::lock_guard lock(storageMutex_);
    storage_[req.key] = req.value;
    return {true, "ok"};
  }

  Response DEL(const Request &req) {
    std::lock_guard lock(storageMutex_);
    auto it = storage_.find(req.key);
    if (it == storage_.end()) {
      return {false, "not found key"};
    }
    storage_.erase(it);
    return {true, "ok"};
  }

private:
  std::mutex storageMutex_;
  Storage storage_;
};
}// namespace storage