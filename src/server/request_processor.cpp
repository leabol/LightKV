#include "server/request_processor.hpp"
#include <pthread.h>
#include "net/EventLoop.hpp"
#include "protocol/codec.hpp"
#include "server/server_def.hpp"

namespace server {

RequestProcessor::RequestProcessor() = default;

void RequestProcessor::start() {
  reqProcessTh_ = std::thread([this]() { processLoop(); });
  std::unique_lock lock(mtx_);
  cvProcessor_.wait(lock, [this]() { return ready_; });
}

void RequestProcessor::stop() {
  {
    std::lock_guard lock(mtx_);
    stop_ = true;
  }
  cvSubmitor_.notify_all();
  cvProcessor_.notify_all();
  if (reqProcessTh_.joinable()) {
    reqProcessTh_.join();
  }
}

void RequestProcessor::handleRequest(PendingRequest& request) {
  {
    std::unique_lock lock(mtx_);
    cvSubmitor_.wait(lock, [this] { return stop_ || requests_.size() < kMaxReqSize; });
    if (stop_) {
      return;
    }
    requests_.emplace(std::move(request));
  }
  cvProcessor_.notify_all();
}

void RequestProcessor::handleGroupRequest(std::deque<PendingRequest>& requests){
  {
    std::unique_lock lock(mtx_);
    cvSubmitor_.wait(lock, [this] { return stop_ || requests_.size() < kMaxReqSize; });
    if (stop_) {
      return;
    }
    while(!requests.empty()){
      requests_.emplace(std::move(requests.front()));
      requests.pop_front();
    }
  }
  cvProcessor_.notify_all();
}

void RequestProcessor::processLoop() {
pthread_setname_np(pthread_self(), "process-th"); 
  initDispatcher();
  {
    std::unique_lock lock(mtx_);
    ready_ = true;
  }
  cvProcessor_.notify_one();

  while (true) {
    std::queue<PendingRequest> reqs;
    {
      std::unique_lock lock(mtx_);
      cvProcessor_.wait(lock, [this] { return stop_ || !requests_.empty(); });
      std::swap(reqs, requests_);
    }
    if (stop_ && reqs.empty()) {
      break;
    }
    cvSubmitor_.notify_all();

    while (!reqs.empty()) {
      PendingRequest pending = std::move(reqs.front());
      Response response = dispatcher_->dispatch(pending.request);
      std::string responseData = encodeResponse(response);
      pending.ioLoop->queueInLoop(
          [conn = pending.connection, responseData] { conn->send(responseData); });
      reqs.pop();
    }
  }
}

void RequestProcessor::initDispatcher() {
  dispatcher_ = std::make_unique<Dispatcher>(&memtable_);
  dispatcher_->registerHandler(
      CommandType::GET, [this](const Request& request) { return memtable_.GET(request); });
  dispatcher_->registerHandler(
      CommandType::SET, [this](const Request& request) { return memtable_.SET(request); });
  dispatcher_->registerHandler(
      CommandType::DEL, [this](const Request& request) { return memtable_.DEL(request); });
}

}  // namespace server
