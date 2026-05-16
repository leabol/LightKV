#include "EpollPoller.hpp"

#include <cerrno>
#include <cstring>

#include "Channel.hpp"
#include "Log.hpp"

using namespace net;

EpollPoller::EpollPoller() : epollfd_(epoll_create1(EPOLL_CLOEXEC)), events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        LOG_CRITICAL("epoll_create1 failed: {}", strerror(errno));
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
    epoll_event event;
    event.data.ptr = channel;
    event.events   = channel->getInterestedEvents();

    int  fd = channel->getFd();
    auto it = channels_.find(fd);

    // 禁用：events==0 → DEL
    if (event.events == 0) {
        if (channel->isAdded()) {
            if (epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, nullptr) == -1 && errno != ENOENT) {
                LOG_ERROR("epoll_ctl DEL fd={} failed: {}", fd, strerror(errno));
            }
            channel->setAdded(false);
        }
        if (it != channels_.end()) {
            channels_.erase(it);
        }
        return;
    }

    // 优先 MOD：若不存在则回退 ADD
    if (epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        if (errno == ENOENT || !channel->isAdded() || it == channels_.end()) {
            if (epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &event) == -1) {
                if (errno == EEXIST) {
                    // 内核已存在：再尝试 MOD 一次
                    if (epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &event) == -1) {
                        LOG_ERROR(
                            "epoll_ctl MOD after EEXIST fd={} failed: {}", fd, strerror(errno));
                        return;
                    }
                } else {
                    LOG_ERROR("epoll_ctl ADD fd={} failed: {}", fd, strerror(errno));
                    return;
                }
            }
            channel->setAdded(true);
            channels_[fd] = channel;
        } else {
            LOG_ERROR("epoll_ctl MOD fd={} failed: {}", fd, strerror(errno));
            return;
        }
    } else {
        // MOD 成功，确保映射存在
        if (it == channels_.end()) {
            channels_[fd] = channel;
        }
        channel->setAdded(true);
    }
}

void EpollPoller::removeChannel(Channel* channel) {
    if (channel == nullptr) {
        return;
    }
    int fd = channel->getFd();
    if (epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, nullptr) == -1 && errno != ENOENT) {
        LOG_ERROR("epoll_ctl DEL fd={} failed: {}", fd, strerror(errno));
    }
    channel->setAdded(false);
    channels_.erase(fd);
}
