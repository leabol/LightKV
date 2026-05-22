#include "Channel.hpp"

#include <sys/epoll.h>

#include "EventLoop.hpp"
#include "Log.hpp"

using namespace net;

void Channel::update() {
  loop_->updateChannel(this);
}

void Channel::handleEvent() {
  // 生命周期守卫：若绑定对象已析构，则不再分发事件
  if (tied_) {
    auto guard = tie_.lock();
    if (!guard) {
      return;
    }
  }
  const uint32_t rev = revents_;

  // 错误或挂断优先
  if (rev & (EPOLLERR | EPOLLHUP)) {
    LOG_WARN("fd:{}, channel handleEvent() EPOLLUP/EPOLLERR", fd_);
    if (closeCallback_) {
      closeCallback_();  // 多数场景下读到 0 即可判定关闭
    }
    return;
  }
  // 半关闭
  if (rev & EPOLLRDHUP) {
    LOG_WARN("fd:{}, channel handleEvent() EPOLLRDHUP", fd_);
    if (readCallback_) {
      readCallback_();  // 读到 0，按关闭处理
    }
  }
  // 可读
  if (rev & EPOLLIN) {
    if (readCallback_) {
      readCallback_();
    }
  }
  // 可写
  if (rev & EPOLLOUT) {
    if (writeCallback_) {
      writeCallback_();
    }
  }
}

void Channel::remove() {
  loop_->removeChannel(this);
}
