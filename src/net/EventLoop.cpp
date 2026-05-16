#include "EventLoop.hpp"

#include "Channel.hpp"
#include "EpollPoller.hpp"

using namespace net;

EventLoop::EventLoop() : poller_(std::make_unique<EpollPoller>()) {}

EventLoop::~EventLoop() = default;

void EventLoop::loop(int timeout) {
    while (true) {
        activeChannels_ = poller_->poll(timeout);
        for (auto* channel : activeChannels_) {
            channel->handleEvent();
        }
    }
}

void EventLoop::updateChannel(Channel* channel) {
    if (channel == nullptr) {
        return;
    }
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    poller_->removeChannel(channel);
}
