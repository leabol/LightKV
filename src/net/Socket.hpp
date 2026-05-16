#pragma once

#include <string_view>

namespace net {

class InetAddress;  // 前置声明，避免不必要的编译依赖

// 通过socket()只能得到一个半成品的socket, 还需要bind()才具有使用功能,
//  因此不把地址作为socket的成员变量
class Socket {
  public:
    explicit Socket(int socketfd);
    explicit Socket();
    ~Socket();

    // Non-copyable
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;

    // Movable
    Socket(Socket&& other) noexcept : socketfd_(other.socketfd_) {
        other.socketfd_ = -1;
    }
    Socket& operator=(Socket&& other) noexcept;

    void bindAddr(const InetAddress& addr) const;

    void listen(int backlog) const;

    // 接受新连接，返回新连接 fd；通过 peeraddr 返回对端地址（数字形式）。
    int accept(InetAddress& peeraddr) const;

    // 将套接字设为非阻塞（O_NONBLOCK）
    void setNonblock(bool on = true) const;

    // 开关 TCP_NODELAY（禁用/启用 Nagle 算法）。开启通常降低延迟但可能增加小包数量。
    void setTcpNoDelay(bool on = true) const;

    // 开关 SO_REUSEADDR：允许 TIME_WAIT 状态下快速复用本地地址端口（常用于服务端重启）。
    void setReuseAddr(bool on = true) const;

    // 开关 SO_REUSEPORT：允许多个进程/线程绑定同一 <ip,port>（用于多核负载均衡）。
    // 某些平台/内核版本不支持，可能返回 ENOPROTOOPT/EINVAL。
    void setReusePort(bool on = true) const;

    // 开关 SO_KEEPALIVE：周期性探测对端是否存活，检测断链。
    // 注意：探测间隔/重试次数通常需要通过 TCP 层 sysctl 或 TCP_KEEP* 选项进一步配置。
    void setKeepAlive(bool on = true) const;

    // for client
    void connect(const InetAddress& serveraddr) const;
    // 便捷重载：内部构造 InetAddress
    void connect(std::string_view host, std::string_view port) const;

    // 便捷重载：服务端绑定（host,port）或仅端口（绑定到任意地址）
    void bindAddr(std::string_view host, std::string_view port) const;
    void bindAddr(std::string_view port) const;  // 仅端口表示任意地址

    [[nodiscard]] int fd() const noexcept {
        return socketfd_;
    }

  private:
    int socketfd_{-1};
};
// 常见 errno 含义：
// - EAGAIN/EWOULDBLOCK：非阻塞调用当前无法完成，稍后重试
// - EINTR：系统调用被信号打断，应根据语义选择重试
// - ENOPROTOOPT：请求的套接字选项不被支持
// - EINVAL：无效参数或状态（例如不支持的选项组合）
// - ECONNREFUSED：连接被拒绝
// - ETIMEDOUT：操作超时
// - ENETUNREACH/EHOSTUNREACH：网络/主机不可达
}  // namespace Server
