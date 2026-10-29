#include "net/Channel.hpp"

#include <sys/epoll.h>

#include "net/EventLoop.hpp"
#include "util/Log.hpp"

using namespace net;

Channel::~Channel() {
  // index_ != kNew 说明还登记在 poller 的表里（含 disableAll 后的 kDeleted）：
  // 那是一条悬垂指针，fd 被复用时会被误当成有效 Channel，必须在 remove() 之后才析构。
  // 注意：这里只读自己的成员并打日志，绝不回调 loop_ —— loop_ 可能已经销毁，
  // 而且析构未必发生在 loop 线程上。
  if (index_ != IndexE::kNew) {
    LOG_ERROR("Channel fd={} destroyed without remove()", fd_);
  }
}

void Channel::update() {
  // 状态不由这里维护：ADD / MOD / DEL 三种结果由 EpollPoller 判定并回写 index_。
  loop_->updateChannel(this);
}

void Channel::remove() {
  // 幂等由 poller 保证：不在 fd->Channel 表里就直接返回。
  loop_->removeChannel(this);
}

void Channel::disableAll() {
  events_ = 0;
  update();
}

// 生命周期守卫：确保在执行handleEvent期间，绑定对象还存活
void Channel::handleEvent() {
  if (tied_) {
    auto guard = tie_.lock();
    if (guard) {
      handleEventWithGuard();
    }
  } else {
    handleEventWithGuard();
  }
}

void Channel::handleEventWithGuard() {
  const uint32_t rev = revents_;

  // 错误或挂断优先：这两种状态下 fd 基本已不可用，直接走关闭路径。
  if (rev & (EPOLLERR | EPOLLHUP)) {
    LOG_WARN("fd:{}, channel handleEvent() EPOLLUP/EPOLLERR", fd_);
    if (closeCallback_) {
      closeCallback_();  // 多数场景下读到 0 即可判定关闭
    }
    return;
  }

  // 可读 / 半关闭合并成一次调用：
  //   EPOLLIN   —— 有数据可读；对端关闭时也会置位（EOF 使 socket 可读）。
  //   EPOLLRDHUP —— 对端半关闭,需要提前注册才能收到。
  // Channel 不做关闭决定 —— RDHUP 只代表对端不再写，我们仍可能要回数据。
  if (rev & (EPOLLIN | EPOLLRDHUP)) {
    if (readCallback_) {
      readCallback_();
    }
  }

  // 可写。readCallback_ 里可能已经把连接关掉了（对端关闭 -> handleClose -> remove），
  // 此时 index_ 已回到 kNew，不能再用一个已注销的 Channel 派发写事件。
  if ((rev & EPOLLOUT) && index_ != IndexE::kNew) {
    if (writeCallback_) {
      writeCallback_();
    }
  }
}