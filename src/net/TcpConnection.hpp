#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "net/buffer.hpp"


namespace net {

class Buffer;
class Socket;
class Channel;
class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
  using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
  using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;        // 建立 / 关闭
  using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer&)>;  // 可读数据
  using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;  // 发送缓冲清空

  TcpConnection(EventLoop* loop, std::unique_ptr<Socket> sock);
  // 便捷重载：从现有 fd 构造（内部包装为 Socket）
  TcpConnection(EventLoop* loop, int fd);
  ~TcpConnection();

  // 禁止拷贝，允许移动（通常不移动，连接由智能指针管理）
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

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
  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;

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
