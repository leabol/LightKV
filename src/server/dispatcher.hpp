#pragma once
#include <functional>
#include <unordered_map>

#include "protocol/request.hpp"
#include "protocol/response.hpp"
#include "storage/memtable/memtable.hpp"

using namespace protocol;
using Handler = std::function<Response(const Request &)>;

namespace server {
class Dispatcher {
public:
  Dispatcher(storage::Memtable *pStorage) { storage_ = pStorage; };

  void registerHandler(const CommandType cmd, Handler handler) {
    handlers_[static_cast<int>(cmd)] = std::move(handler);
  }

  Response dispatch(const Request &req) {
    auto it = handlers_.find(static_cast<int>(req.cmd));
    if (it == handlers_.end()) {
      return {false, "unsupported"};
    }
    return it->second(req);
  }

private:
  storage::Memtable *storage_{nullptr};
  std::unordered_map<int, Handler> handlers_;
};
}  // namespace server
