#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "net/Channel.hpp"
#include "net/EpollPoller.hpp"

namespace net {

// 对epoll的分装, 对外提供更多的接口, 用来执行channel, 这个类也不拥有channel
class EventLoop {
public:
  using Functor = std::function<void()>;

  EventLoop();
  ~EventLoop();

  static int createEventFd();  //  创建一个wakeup事件返回对应的fd
  void loop();
  void quit();

  void addChannel(Channel* channel);  // 只能有channel类中调用, 外部不可直接使用
  void updateChannel(Channel* channel);
  void removeChannel(Channel* channel);

  void assertInLoopThread() const;
  bool isInLoopThread() const;

  void wakeup() const;
  void runInLoop(Functor cb);
  void queueInLoop(Functor cb);

private:
  void doPendingFunctors();
  void handelWakeup() const;

  const std::thread::id threadId_{0};
  bool looping_{false};
  std::atomic<bool> quit_{false};
  bool eventHandling_{false};
  bool callingPendingFunctors_{false};

  EpollPoller poller_;
  std::vector<Channel*> activeChannels_;

  int wakeupFd_{0};
  Channel wakeupChannel_;  // 依赖 wakeupFd_, 故必须声明在其之后

  mutable std::mutex mutex_;
  std::vector<Functor> pendingFunctors_;
};
}  // namespace net
