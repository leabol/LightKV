#include "net/EpollPoller.hpp"

#include <cerrno>
#include <cstring>

#include "net/Channel.hpp"
#include "util/Log.hpp"

using namespace net;

EpollPoller::EpollPoller() : epollfd_(epoll_create1(EPOLL_CLOEXEC)), events_(kInitEventListSize) {
  if (epollfd_ < 0) {
    LOG_CRITICAL("epoll_create1 failed: {}", strerror(errno));
    abort();
  }
}

EpollPoller::~EpollPoller() {
  if (epollfd_ >= 0) {
    close(epollfd_);
  }
}

std::vector<Channel*> EpollPoller::poll(int timeout) {
  int numEvents = epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeout);

  if (numEvents < 0) {
    if (errno == EINTR) {
      return {};
    }
    LOG_ERROR("epoll_wait() is failed to call");
    return {};
  }

  std::vector<Channel*> activeChannel;
  activeChannel.reserve(static_cast<size_t>(numEvents));
  for (int i = 0; i < numEvents; ++i) {
    auto* channel = static_cast<Channel*>(events_[i].data.ptr);
    channel->setReadyEvents(events_[i].events);
    activeChannel.push_back(channel);
  }

  return activeChannel;
}

void EpollPoller::updateChannel(Channel* channel) {
  if (channel == nullptr) {
    return;
  }
  const int fd = channel->getFd();
  const Channel::IndexE index = channel->index();

  epoll_event event;
  event.data.ptr = channel;
  event.events = channel->getInterestedEvents();

  // kNew / kDeleted 都表示「当前不在 epoll 中」，必须 ADD。
  // kDeleted 时 fd 仍留在 channels_ 表里，所以不需要重新插入。
  if (index == Channel::IndexE::kNew || index == Channel::IndexE::kDeleted) {
    if (epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &event) == -1) {
      LOG_ERROR("epoll_ctl ADD fd={} failed: {}", fd, strerror(errno));
      return;
    }
    if (index == Channel::IndexE::kNew) {
      // 正常路径下表里不该有这个 fd：出现说明上一个 Channel 漏了 remove()，
      // 这里覆盖会掩盖一次泄漏（旧 Channel 从此没人注销）。留个线索。
      if (channels_.find(fd) != channels_.end()) {
        LOG_WARN("epoll_ctl ADD fd={} was still registered, previous channel leaked remove()", fd);
      }
      channels_[fd] = channel;
    }
    channel->setIndex(Channel::IndexE::kAdded);
    return;
  }

  // kAdded：已在 epoll 中
  if (event.events == 0) {
    // 不再关心任何事件 -> 从 epoll 摘除，但保留 channels_ 里的登记（kDeleted）。
    if (epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, nullptr) == -1 && errno != ENOENT) {
      LOG_ERROR("epoll_ctl DEL fd={} failed: {}", fd, strerror(errno));
      return;
    }
    channel->setIndex(Channel::IndexE::kDeleted);
    return;
  }

  if (epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    LOG_ERROR("epoll_ctl MOD fd={} failed: {}", fd, strerror(errno));
  }
}

void EpollPoller::removeChannel(Channel* channel) {
  if (channel == nullptr) {
    return;
  }
  const int fd = channel->getFd();
  auto it = channels_.find(fd);
  if (it == channels_.end()) {
    return;  // 从未登记或已注销：幂等，重复调用是空操作
  }
  if (it->second != channel) {
    LOG_WARN("removeChannel fd={} is registered by another channel, skip", fd);
    return;
  }
  // 只有真正在 epoll 里时才需要 DEL（kDeleted 的 fd 早就摘除了）
  if (channel->index() == Channel::IndexE::kAdded) {
    if (epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, nullptr) == -1 && errno != ENOENT) {
      LOG_ERROR("epoll_ctl DEL fd={} failed: {}", fd, strerror(errno));
    }
  }
  channels_.erase(it);
  channel->setIndex(Channel::IndexE::kNew);
}
