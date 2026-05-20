#pragma once

#include <condition_variable>
#include <functional>
#include <thread>

namespace net{

class EventLoop;

class EventLoopThread{
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThread(const ThreadInitCallback& cb = {});
    ~EventLoopThread();

    EventLoop* startLoop();

private:
    void threadFunc();

    std::mutex mutex_;
    std::condition_variable cond_;
    EventLoop* loop_;

    bool exiting_{false};
    std::thread thread_;
    ThreadInitCallback tInitCallback_;
};
}