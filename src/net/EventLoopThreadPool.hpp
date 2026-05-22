#pragma once
#include <functional>
#include <memory>
#include <vector>
namespace net {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool {
public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  EventLoopThreadPool(EventLoop* baseLoop);
  ~EventLoopThreadPool();

  void setThreadNum(int numThreads) { numThreads_ = numThreads; }
  void start(const ThreadInitCallback& cb = {});

  // 轮询的方式，获得下一个loop
  EventLoop* getNextLoop();

  bool started() const { return start_; }

private:
  EventLoop* baseLoop_{nullptr};
  bool start_{false};
  int numThreads_{0};
  size_t next_{0};
  std::vector<std::unique_ptr<EventLoopThread>> threads_;
  std::vector<EventLoop*> loops_;
};

}  // namespace net