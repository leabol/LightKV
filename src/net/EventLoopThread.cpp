#include "net/EventLoopThread.hpp"

#include <assert.h>

#include "net/EventLoop.hpp"

namespace net {

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb) :
    loop_(nullptr), exiting_(false), tInitCallback_(cb) {}

EventLoopThread::~EventLoopThread() {
  exiting_ = true;
  if (loop_) {
    loop_->quit();
    thread_.join();
  }
}

EventLoop* EventLoopThread::startLoop() {
  thread_ = std::thread([this] { this->threadFunc(); });
  EventLoop* loop = nullptr;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return loop_ != nullptr; });
  }
  loop = loop_;
  assert(loop);
  return loop;
}

void EventLoopThread::threadFunc() {
  EventLoop loop;

  if (tInitCallback_) {
    tInitCallback_(&loop);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = &loop;
    cond_.notify_all();
  }

  loop.loop();

  std::lock_guard<std::mutex> lock(mutex_);
  loop_ = nullptr;
}

}  // namespace net