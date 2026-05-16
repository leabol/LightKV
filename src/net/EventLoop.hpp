#pragma once
#include <memory>
#include <vector>

namespace net {

class Channel;
class EpollPoller;

// 对epoll的分装, 对外提供更多的接口, 用来执行channel, 这个类也不拥有channel
class EventLoop {
  public:
    EventLoop();
    ~EventLoop();

    void loop(int timeout);

    void addChannel(Channel* channel);  // 只能有channel类中调用, 外部不可直接使用
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

  private:
    std::unique_ptr<EpollPoller> poller_;
    // std::unordered_map<int, std::shared_ptr<Channel>> channels_;
    std::vector<Channel*> activeChannels_;
};
}  // namespace Server
