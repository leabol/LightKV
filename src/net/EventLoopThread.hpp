#pragma once
#include <condition_variable>
#include <functional>
#include <thread>

namespace net {

class EventLoop;

class EventLoopThread {
public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  EventLoopThread(const ThreadInitCallback& cb = {}); //回调函数默认为空
  ~EventLoopThread();

  EventLoop* startLoop();

private:
  void threadFunc();

  EventLoop* loop_{nullptr};
  bool exiting_{false};
  std::thread thread_;

  std::mutex mutex_;
  std::condition_variable cond_;

  ThreadInitCallback tInitCallback_;  // 新线程中的初始化回调，EventLoop 创建后调用，用于线程内资源或状态初始化
};
}  // namespace net