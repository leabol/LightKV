#pragma once
#include <sys/epoll.h>
#include <unistd.h>

#include <unordered_map>
#include <vector>

namespace net {

class Channel;
// epoll实现的核心, 管理epollfd的所有权, 这个类不拥有channel和fd,
// 这个类通过channels_这个哈希表来记录所有已经添加的channel
class EpollPoller {
  public:
    EpollPoller();
    ~EpollPoller();

    // 返回活跃的Channel
    std::vector<Channel*> poll(int timeout);

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

  private:
    static const int kInitEventListSize = 16;
    int              epollfd_{-1};
    std::vector<epoll_event> events_;  // 内核返回的原始事件,经过处理可以得到返回的channel
    std::unordered_map<int, Channel*> channels_;  // 记录所有已注册的channel
};
}  // namespace Server
