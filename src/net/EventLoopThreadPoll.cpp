#include <assert.h>

#include <memory>

#include "EventLoop.hpp"
#include "EventLoopThread.hpp"
#include "EventLoopThreadPool.hpp"

namespace net {
EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop) :
    baseLoop_(baseLoop), start_(false), numThreads_(0), next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() {}

void EventLoopThreadPool::start(const ThreadInitCallback& cb) {
  assert(!start_);
  baseLoop_->assertInLoopThread();

  start_ = true;

  for (int i = 0; i < numThreads_; i++) {
    auto t = std::make_unique<EventLoopThread>(cb);
    loops_.push_back(t->startLoop());
    threads_.push_back(std::move(t));
  }
  if (numThreads_ == 0 && cb) {
    cb(baseLoop_);
  }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
  assert(start_);
  baseLoop_->assertInLoopThread();
  EventLoop* loop = baseLoop_;

  if (!loops_.empty()) {
    if (next_ >= loops_.size()) {
      next_ = 0;
    }
    loop = loops_[next_++];
  }
  return loop;
}

}  // namespace net