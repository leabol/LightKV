#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <thread>

namespace net {

class Channel;
class EpollPoller;

// 对epoll的分装, 对外提供更多的接口, 用来执行channel, 这个类也不拥有channel
class EventLoop {
  public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    int createEventFd();  //  创建一个wakeup事件返回对应的fd
    void loop(int timeout);

    void quit();

    void addChannel(Channel* channel);  // 只能有channel类中调用, 外部不可直接使用
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    void assertInLoopThread();
    bool isInLoopThread() const;

    void wakeup();

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);
  private:
    void doPendingFunctors();
    void handelWakeup();

    bool looping_{false};
    std::atomic<bool> quit_{false};
    bool eventHandling_{false};
    bool callingPendingFunctors_{false};

    const std::thread::id threadId_{0};

    std::unique_ptr<EpollPoller> poller_;
    // std::unordered_map<int, std::shared_ptr<Channel>> channels_;
    std::vector<Channel*> activeChannels_;

    int wakeupFd_{0};
    std::unique_ptr<Channel> wakeupChannel_;

    mutable std::mutex mutex_;
    std::vector<Functor> pendingFunctors_; 
};
}  // namespace Server
