#pragma once
#include <sys/epoll.h>

#include <functional>
#include <memory>

namespace net {
class EventLoop;

class Channel {
public:
  using EventCallback = std::function<void()>;

  Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

  // 禁止拷贝/移动：Channel 会把自己（this）注册进 epoll，
  // 一旦对象地址变化或出现副本，epoll 里的注册项就会指向错误的对象。
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(Channel&&) = delete;

  void setReadCallback(EventCallback func) { readCallback_ = std::move(func); }
  void setWriteCallback(EventCallback func) { writeCallback_ = std::move(func); }
  void setCloseCallback(EventCallback func) { closeCallback_ = std::move(func); }
  void enableReading() {
    events_ |= EPOLLIN;
    update();
  }
  void disableReading() {
    events_ &= ~EPOLLIN;
    update();
  }
  void enableWriting() {
    events_ |= EPOLLOUT;
    update();
  }
  void disableWriting() {
    events_ &= ~EPOLLOUT;
    update();
  }
  void disableAll() {
    events_ = 0;  // 清空所有事件
    update();
  }
  [[nodiscard]] bool isWriting() const { return (events_ & EPOLLOUT) != 0; }
  [[nodiscard]] bool isReading() const { return (events_ & EPOLLIN) != 0; }

  [[nodiscard]] uint32_t getInterestedEvents() const { return events_; }

  void setReadyEvents(uint32_t revents) { revents_ = revents; }

  [[nodiscard]] int getFd() const { return fd_; }

  void update();

  void handleEvent();

  void remove();

  // 绑定生命周期守卫：避免回调时对象已析构
  void tie(const std::shared_ptr<void>& obj) {
    tie_ = obj;
    tied_ = true;
  }

private:
  EventLoop* loop_{nullptr};
  int fd_{-1};
  uint32_t events_{0};
  uint32_t revents_{0};

  // 生命周期守卫
  std::weak_ptr<void> tie_;
  bool tied_{false};

  EventCallback readCallback_;
  EventCallback writeCallback_;
  EventCallback closeCallback_;
};
}  // namespace net
