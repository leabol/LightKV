#include "server/request_processor.hpp"
#include <pthread.h>
#include <vector>
#include "net/EventLoop.hpp"
#include "protocol/codec.hpp"
#include "protocol/response.hpp"
#include "server/server_def.hpp"
#include "util/Log.hpp"

namespace server {
namespace {

// 存储层只返回 ok/value，这里按命令翻译成 RESP 的回复类型
Response toRespReply(const Request& req, const Response& raw) {
  Response reply;
  reply.ok = raw.ok;
  switch (req.cmd) {
    case CommandType::GET:
      if (raw.ok) {
        reply.type = ReplyType::BulkString;
        reply.value = raw.value;
      } else {
        reply.type = ReplyType::NullBulk;  // 未命中必须是 $-1
      }
      break;
    case CommandType::SET:
      if (raw.ok) {
        reply.type = ReplyType::SimpleString;
        reply.value = "OK";
      } else {
        reply.type = ReplyType::Error;
        reply.value = "ERR write failed";
      }
      break;
    case CommandType::DEL:
      reply.type = ReplyType::Integer;
      reply.integer = raw.ok ? 1 : 0;
      break;
    default:
      reply.type = ReplyType::Error;
      reply.value = "ERR unsupported command";
      break;
  }
  return reply;
}

}  // namespace

RequestProcessor::RequestProcessor() = default;

void RequestProcessor::start() {
  reqProcessTh_ = std::thread([this]() { processLoop(); });
  std::unique_lock lock(mtx_);
  cvProcessor_.wait(lock, [this]() { return ready_; });
  LOG_INFO("Request processor started");
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
  LOG_INFO("Request processor stopped");
}

void RequestProcessor::handleRequest(PendingRequest& request) {
  {
    std::unique_lock lock(mtx_);
    cvSubmitor_.wait(lock, [this] { return stop_ || requests_.size() < kMaxReqSize; });
    if (stop_) {
      LOG_WARN("Rejecting request because request processor is stopping");
      return;
    }
    requests_.emplace(std::move(request));
    LOG_DEBUG("Read request queued: queue_size={}", requests_.size());
  }
  cvProcessor_.notify_all();
}

void RequestProcessor::handleGroupRequest(std::vector<PendingRequest> requests, bool success){
  {
    std::unique_lock lock(mtx_);
    cvSubmitor_.wait(lock, [this] { return stop_ || requests_.size() < kMaxReqSize; });
    if (stop_) {
      LOG_WARN("Rejecting completed WAL batch because request processor is stopping");
      return;
    }
    if (success){
      for (auto& req : requests){
        requests_.push(std::move(req));
      }
      LOG_DEBUG("WAL batch completed successfully: requests={} queue_size={}",
                requests.size(), requests_.size());
    } else {
      failed_request_.swap(requests);
      LOG_ERROR("WAL batch failed: requests={}", failed_request_.size());
    }
  }
  cvProcessor_.notify_all();
}

void RequestProcessor::processLoop() {
pthread_setname_np(pthread_self(), "process-th"); 
LOG_INFO("Request processing thread started");
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
    LOG_DEBUG("Processing request batch: requests={}", reqs.size());
    if (stop_ && reqs.empty()) {
      break;
    }
    cvSubmitor_.notify_all();

    while (!reqs.empty()) {
      PendingRequest pending = std::move(reqs.front());
      Response response = toRespReply(pending.request, dispatcher_->dispatch(pending.request));
      LOG_DEBUG("Request processed: cmd={} key={} success={}",
                static_cast<int>(pending.request.cmd), pending.request.key, response.ok);
      std::string responseData = encodeResponse(response);
      pending.ioLoop->queueInLoop(
          [conn = pending.connection, responseData] { conn->send(responseData); });
      reqs.pop();
    }

    if (!failed_request_.empty()){
      LOG_WARN("Sending failure responses: requests={}", failed_request_.size());
      // 每类命令的失败回复不同（SET 是错误行、DEL 是整数），所以逐条编码
      for (auto& pending : failed_request_) {
        Response failed{false, "ERR WAL failed to persist", ReplyType::Error, 0};
        std::string responseData = encodeResponse(failed);
        pending.ioLoop->queueInLoop(
          [conn = pending.connection, responseData] { conn->send(responseData); });
      }
      failed_request_.clear();
    }
  }
  LOG_INFO("Request processing thread stopped");
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
