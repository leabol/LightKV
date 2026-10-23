#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "server/dispatcher.hpp"
#include "server/server_def.hpp"
#include "storage/memtable/memtable.hpp"

namespace server {

class RequestProcessor {
  constexpr static size_t kMaxReqSize = 3000;

public:
  RequestProcessor();
  void start();
  void stop();
  void handleRequest(PendingRequest& request);

private:
  void processLoop();
  void initDispatcher();

  bool ready_{false};
  bool stop_{false};
  storage::Memtable memtable_;
  std::unique_ptr<Dispatcher> dispatcher_;

  std::thread reqProcessTh_;
  std::queue<PendingRequest> requests_;
  std::mutex mtx_;
  std::condition_variable cvProcessor_;
  std::condition_variable cvSubmitor_;
};

}  // namespace server
