#include "memtable.hpp"

using namespace protocol;

namespace storage {

Response Memtable::GET(const Request &req) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = storage_.find(req.key);
  if (it == storage_.end() || it->second.deleted) {
    return {false, ""};
  }
  return {true, it->second.value};
}

Response Memtable::SET(const Request &req) {
  std::lock_guard<std::mutex> lock(mutex_);
  storage_[req.key] = {req.value, false};
  return {true, ""};
}

Response Memtable::DEL(const Request &req) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = storage_.find(req.key);
  if (it == storage_.end() || it->second.deleted) {
    return {false, ""};
  }
  it->second.deleted = true;
  return {true, ""};
}

}  // namespace storage
