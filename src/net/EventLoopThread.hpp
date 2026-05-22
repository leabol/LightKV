#pragma once
#include <condition_variable>
#include <functional>
#include <thread>

namespace net {

class EventLoop;

class EventLoopThread {
public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  EventLoopThread(const ThreadInitCallback& cb = {});
  ~EventLoopThread();

  EventLoop* startLoop();

private:
  void threadFunc();

  EventLoop* loop_{nullptr};
  bool exiting_{false};
  std::thread thread_;

  std::mutex mutex_;
  std::condition_variable cond_;

  ThreadInitCallback tInitCallback_;
};
}  // namespace net