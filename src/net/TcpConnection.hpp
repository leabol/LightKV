#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "net/Channel.hpp"
#include "net/Socket.hpp"
#include "net/buffer.hpp"


namespace net {

class Buffer;
class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
  using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
  using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;        // 建立 / 关闭
  using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer&)>;  // 可读数据
  using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;  // 发送缓冲清空

  TcpConnection(EventLoop* loop, Socket sock);
  // 便捷重载：从现有 fd 构造（内部包装为 Socket）
  explicit TcpConnection(EventLoop* loop, int fd);
  ~TcpConnection();

  // 禁止拷贝、也不移动：连接始终由 shared_ptr 持有。
  // 注意不能移动的关键原因：channel_ 注册到 epoll 的是 this 指针，
  // 对象地址一变，epoll 里的 data.ptr 就成了悬空指针。
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&&) = delete;
  TcpConnection& operator=(TcpConnection&&) = delete;

  // 用户/框架设置回调
  void setConnectionCallback(ConnectionCallback cb) { connectionCallback_ = std::move(cb); }
  void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
  void setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
  }
  void setCloseCallback(ConnectionCallback cb) { closeCallback_ = std::move(cb); }

  // 由外部（Acceptor 或 Connector 完成后）调用，触发 "已建立" 逻辑
  void connectEstablished();
  // 关闭流程（优雅关闭）
  void shutdown();
  // 强制立即关闭
  void forceClose();

  // 发送数据（追加到发送缓冲，注册写事件）。线程安全：可从任意线程调用，内部会切回所属 EventLoop。
  void send(const std::string& data);

  // 回复保序: RESP 要求「一个请求恰好一个回复，且顺序与请求一致」。
  uint64_t nextRequestSeq();  // 只能在所属 EventLoop 线程调用

  // data 按值传入：调用方传临时量时可直接 move，省掉一次拷贝
  void sendOrderedReply(uint64_t seq, std::string data, bool closeAfter = false);

  // QUIT 之后收到的字节必须全部丢弃（RESP 语义：QUIT 后的命令不执行）
  void markQuitReceived() { quitReceived_ = true; }
  bool quitReceived() const { return quitReceived_; }

  EventLoop* getLoop() const { return loop_; }
  int fd() const;  // 便捷

private:
  enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };
  void setState(StateE s) { state_ = s; }

  // Channel 回调入口
  void handleRead();
  void handleWrite();
  void handleClose();
  void handleError();

  // 输出缓冲高水位。超过就断开连接：Buffer::append 在空间不足时是静默截断的，
  // 一旦截断客户端会收到残缺的 RESP 帧（长度头和内容对不上），比断开更糟。
  static constexpr size_t kOutputHighWaterMark = 16UL * 1024 * 1024;
  // 提前到达、等待前序回复的缓存上限（条数 + 字节数）
  static constexpr size_t kMaxEarlyReplies = 64UL * 1024;
  static constexpr size_t kMaxEarlyReplyBytes = 16UL * 1024 * 1024;

  // 以下只能在所属 EventLoop 线程调用
  void pushOrderedReply(uint64_t seq, std::string data, bool closeAfter);
  void flushOrderedReplies();
  void maybeCloseAfterReply();

  EventLoop* loop_{nullptr};
  // Socket / Channel 用值成员：两者都是「只属于本连接、随连接一起销毁」的资源，
  // 不需要共享所有权，也没有多态，用 unique_ptr 只是多两次堆分配 + 一层间接寻址。
  //
  // 两个前提保证这样做是安全的：
  //  1. TcpConnection 不可移动（拷贝已删除，因此也不会隐式生成移动构造），
  //     成员地址稳定 —— Channel 注册到 epoll 的是 this 指针，地址不能变。
  //  2. 声明顺序 Socket 在 Channel 之前：Channel 初始化需要 socket_.fd()；
  //     析构顺序相反，Channel 先销毁，保证注销 epoll 时 fd 仍然有效。
  Socket socket_;
  Channel channel_;

  StateE state_{kConnecting};

  net::Buffer outputBuffer_;
  net::Buffer inputBuffer_;

  // 回复保序状态
  uint64_t issuedRequestSeq_{0};               // 已分配的最大请求序号
  uint64_t nextReplySeq_{0};                   // 下一个该输出的回复序号
  std::map<uint64_t, std::string> earlyReplies_;  // 提前到达、等待前序的回复
  size_t earlyReplyBytes_{0};
  uint64_t closeAfterReplySeq_{0};             // 输出到该序号后关闭写端（QUIT）
  bool hasCloseAfterReply_{false};
  bool quitReceived_{false};                   // QUIT 已处理，后续字节一律丢弃

  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  WriteCompleteCallback writeCompleteCallback_;
  ConnectionCallback closeCallback_;
};
}  // namespace net
