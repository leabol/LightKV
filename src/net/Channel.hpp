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

  // 本 Channel 在 poller 中的注册状态。
  enum class IndexE { 
    kNew,    // 从未 update() 过，poller 里没有任何记录
    kAdded,  // 已在 epoll 中关注事件
    kDeleted // 已从 epoll 摘除（disableAll），但仍留在 poller 的 fd->Channel 表里
  };

  [[nodiscard]] IndexE index() const { return index_; }
  // 仅供 EpollPoller 调用：让状态只有一份真实来源，Channel 不自己维护副本
  void setIndex(IndexE idx) { index_ = idx; }

  // 析构里会检查是否漏了 remove()。
  ~Channel();

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
    // EPOLLRDHUP 必须显式注册才会出现在 revents 里（EPOLLERR/EPOLLHUP 则总是上报）。
    // 它表示对端半关闭（shutdown(SHUT_WR)），注册后才能显式拿到这个信号。
    events_ |= (EPOLLIN | EPOLLRDHUP);
    update();
  }
  void disableReading() {
    events_ &= ~(EPOLLIN | EPOLLRDHUP);
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
  // 注意这只是「不再关心事件」，Channel 仍登记在 poller 的 fd->Channel 表里，要彻底注销仍需 remove()。
  void disableAll();
  [[nodiscard]] bool isWriting() const { return (events_ & EPOLLOUT) != 0; }
  [[nodiscard]] bool isReading() const { return (events_ & (EPOLLIN | EPOLLRDHUP)) != 0; }

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
  void handleEventWithGuard();

  EventLoop* loop_{nullptr};
  int fd_{-1};
  IndexE index_{IndexE::kNew};
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
