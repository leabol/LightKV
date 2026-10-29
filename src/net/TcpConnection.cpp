#include "net/TcpConnection.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "net/Channel.hpp"
#include "net/EventLoop.hpp"
#include "util/Log.hpp"
#include "net/Socket.hpp"
#include "net/buffer.hpp"

namespace net {

TcpConnection::TcpConnection(EventLoop* loop, Socket sock) :
    loop_(loop), socket_(std::move(sock)), channel_(loop_, socket_.fd()),
    outputBuffer_(net::kBufferSize, net::kMaxSize),
    // 输入缓冲要比「单帧上限」多留一个 recv chunk 的余量：否则当缓冲里还剩半包数据时，
    // beginWrite(kRecvChunk) 会因为 writerIndex_ + len > maxSize_ 而返回 nullptr，
    // 即使这一帧本身完全合法也读不进来。
    inputBuffer_(net::kBufferSize, net::kMaxSize + net::kRecvChunk) {
  // 绑定事件回调
  channel_.setReadCallback([this] { handleRead(); });
  channel_.setWriteCallback([this] { handleWrite(); });
  channel_.setCloseCallback([this] { handleClose(); });
}

TcpConnection::TcpConnection(EventLoop* loop, int fd) :
    TcpConnection(loop, Socket(fd)) {}

TcpConnection::~TcpConnection() = default;

int TcpConnection::fd() const {
  return socket_.fd();
}

void TcpConnection::connectEstablished() {
  loop_->assertInLoopThread();
  assert(state_ == kConnecting);
  setState(kConnected);
  LOG_DEBUG("TcpConnection fd={} established", fd());
  auto self = shared_from_this();
  channel_.tie(self);  // 生命周期守卫
  channel_.enableReading();
  if (connectionCallback_) {
    connectionCallback_(self);  // 通知连接建立
  }
}

void TcpConnection::shutdown() {
  if (state_ == kConnected) {
    setState(kDisconnecting);
    if (!channel_.isWriting()) {
      ::shutdown(fd(), SHUT_WR);
      LOG_INFO("TcpConnection fd={} shutdown (write closed)", fd());
    } else {
      LOG_DEBUG("TcpConnection fd={} shutdown pending (has data to write)", fd());
    }
  }
}

void TcpConnection::forceClose() {
  if (state_ == kConnected || state_ == kDisconnecting) {
    LOG_WARN("TcpConnection fd={} force close", fd());
    handleClose();
  }
}

void TcpConnection::send(const std::string& data) {
  // kDisconnecting 仍允许写：QUIT 的 +OK 发出后进入优雅关闭，handleWrite 会在
  // outputBuffer 排空后才真正 SHUT_WR，这期间还可能有回复要追加。
  if (state_ == kConnecting || state_ == kDisconnected) {
    LOG_WARN("TcpConnection fd={} send failed: not connected", fd());
    return;
  }
  // 慢消费者保护：Buffer::append 在空间不足时是静默截断的，会发出残缺的 RESP 帧。
  // 与其让客户端解析错乱，不如主动断开。
  // 注意判定的是「累计积压」，不能把本条回复的 size 也算进去 —— 否则单条大 value
  // （例如 20MB 的 GET）会被误判成溢出而永远发不出去。
  const size_t buffered = outputBuffer_.readableByte();
  if (buffered > kOutputHighWaterMark || buffered + data.size() > outputBuffer_.maxSize()) {
    LOG_ERROR("TcpConnection fd={} output buffer overflow: buffered={} pending={} limit={}, closing",
              fd(),
              buffered,
              data.size(),
              kOutputHighWaterMark);
    handleError();
    return;
  }
  LOG_TRACE("TcpConnection fd={} sending {} bytes", fd(), data.size());
  if (!channel_.isWriting() && outputBuffer_.empty()) {
    // 尝试直接写
    ssize_t n = ::send(fd(), data.data(), data.size(), 0);
    if (n >= 0) {
      size_t sent = static_cast<size_t>(n);
      if (sent < data.size()) {
        LOG_TRACE("TcpConnection fd={} partial send: {}/{} bytes, buffering remaining",
                  fd(),
                  sent,
                  data.size());
        outputBuffer_.append(data.data() + sent, data.size() - sent);
        channel_.enableWriting();
      } else {
        LOG_TRACE("TcpConnection fd={} sent all {} bytes directly", fd(), sent);
        if (writeCompleteCallback_) {
          auto self = shared_from_this();
          writeCompleteCallback_(self);
        }
      }
    } else {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        outputBuffer_.append(data);
        channel_.enableWriting();
      } else {
        LOG_ERROR("send error fd={} errno={} msg={}", fd(), errno, strerror(errno));
        handleError();
      }
    }
  } else {
    outputBuffer_.append(data);
    if (!channel_.isWriting()) {
      channel_.enableWriting();
    }
  }
}

uint64_t TcpConnection::nextRequestSeq() {
  loop_->assertInLoopThread();
  return issuedRequestSeq_++;
}

void TcpConnection::sendOrderedReply(uint64_t seq, std::string data, bool closeAfter) {
  // 快路径：已经在所属 IO 线程（PING / 未知命令 / 协议错误都走这里），
  // 直接处理，避免一次 std::function 堆分配 + 一次事件循环往返。
  if (loop_->isInLoopThread()) {
    pushOrderedReply(seq, std::move(data), closeAfter);
    return;
  }
  // 慢路径：可能从存储线程 / WAL 线程调用，统一切回 IO 线程
  auto self = shared_from_this();
  loop_->queueInLoop([self, seq, data = std::move(data), closeAfter]() mutable {
    self->pushOrderedReply(seq, std::move(data), closeAfter);
  });
}

void TcpConnection::pushOrderedReply(uint64_t seq, std::string data, bool closeAfter) {
  loop_->assertInLoopThread();
  if (state_ == kDisconnected) {
    return;
  }

  // 快路径：正是下一个该输出的序号，且没有积压 —— 稳态下完全不碰 map。
  if (seq == nextReplySeq_ && earlyReplies_.empty()) {
    ++nextReplySeq_;
    if (!data.empty()) {
      send(data);
    }
    if (closeAfter) {
      closeAfterReplySeq_ = seq;
      hasCloseAfterReply_ = true;
    }
    maybeCloseAfterReply();
    return;
  }

  // 乱序到达：先进重排缓冲，等前序到齐
  earlyReplyBytes_ += data.size();
  earlyReplies_.emplace(seq, std::move(data));
  if (closeAfter) {
    closeAfterReplySeq_ = seq;
    hasCloseAfterReply_ = true;
  }

  // 兜底：某个请求永远不产生回复时，连接会一直卡在等待上并无限吃内存
  if (earlyReplies_.size() > kMaxEarlyReplies || earlyReplyBytes_ > kMaxEarlyReplyBytes) {
    LOG_ERROR("TcpConnection fd={} too many pending replies (count={} bytes={} next={}), closing",
              fd(),
              earlyReplies_.size(),
              earlyReplyBytes_,
              nextReplySeq_);
    earlyReplies_.clear();
    earlyReplyBytes_ = 0;
    handleError();
    return;
  }

  flushOrderedReplies();
}

void TcpConnection::flushOrderedReplies() {
  loop_->assertInLoopThread();

  std::string batch;
  while (true) {
    auto it = earlyReplies_.find(nextReplySeq_);
    if (it == earlyReplies_.end()) {
      break;
    }
    earlyReplyBytes_ -= it->second.size();
    batch += it->second;
    earlyReplies_.erase(it);
    ++nextReplySeq_;
  }

  if (!batch.empty()) {
    send(batch);
  }

  // 空回复（例如空行）也要推进序号，所以关闭判断必须放在 batch 检查之外
  maybeCloseAfterReply();
}

void TcpConnection::maybeCloseAfterReply() {
  loop_->assertInLoopThread();
  // QUIT：等它前面所有回复都输出完，再关闭写端（shutdown 会等 outputBuffer 排空）
  if (hasCloseAfterReply_ && nextReplySeq_ > closeAfterReplySeq_) {
    hasCloseAfterReply_ = false;
    shutdown();
  }
}

void TcpConnection::handleRead() {
  for (;;) {
    uint8_t* pWrite = inputBuffer_.beginWrite(net::kRecvChunk);
    if (pWrite == nullptr) {
      // 输入缓冲已达上限：要么对端在发超过 maxSize 的超大帧，要么残留的半包数据一直
      // 无法解析、把缓冲撑满了。两种都是这个连接上无法继续服务的状态。
      //
      // 注意：这里不能 break —— epoll 是 LT 模式，socket 里还有未读数据，下一轮
      // epoll_wait 会立刻再次触发 handleRead，形成 100% CPU 的空转；也不能 continue ——
      // 会在同一次回调里死循环，把 IO 线程彻底卡死。
      LOG_ERROR("TcpConnection fd={} input buffer full: readable={} max={}, closing",
                fd(),
                inputBuffer_.readableByte(),
                inputBuffer_.maxSize());
      handleError(); 
      return;
    }
    ssize_t n = ::recv(fd(), pWrite, net::kRecvChunk, 0);
    if (n > 0) {
      inputBuffer_.updateWrite(n);
      LOG_TRACE("TcpConnection fd={} received {} bytes", fd(), n);
      if (messageCallback_) {
        auto self = shared_from_this();
        messageCallback_(self, inputBuffer_);
      }
      // 回调里可能已经因为协议错误 / 背压拒绝把连接关掉了，此时不能再 recv
      if (state_ == kDisconnected) {
        break;
      }
      continue;
    }

    if (n == 0) {  // 对端关闭
      LOG_INFO("TcpConnection fd={} peer closed", fd());
      handleClose();
      break;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      break;  // 已无数据
    }
    if (errno == EINTR) {
      continue;  // 重试
    }
    // Do not use 'else' after 'break'
    LOG_ERROR("recv error fd={} errno={} msg={}", fd(), errno, strerror(errno));
    handleError();
    break;
  }
}

void TcpConnection::handleWrite() {
  if (!channel_.isWriting()) {
    return;
  }
  LOG_TRACE(
      "TcpConnection fd={} writing buffered data ({} bytes)", fd(), outputBuffer_.readableByte());
  while (!outputBuffer_.empty()) {
    void* pRead = static_cast<void*>(outputBuffer_.readPoint());
    ssize_t n = ::send(fd(), pRead, outputBuffer_.readableByte(), 0);
    if (n > 0) {
      outputBuffer_.consume(static_cast<size_t>(n));
      if (outputBuffer_.empty()) {
        LOG_TRACE("TcpConnection fd={} write buffer emptied", fd());
        channel_.disableWriting();
        if (writeCompleteCallback_) {
          auto self = shared_from_this();
          writeCompleteCallback_(self);
        }
        if (state_ == kDisconnecting) {
          ::shutdown(fd(), SHUT_WR);
        }
        break;
      }
    } else {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        // 下次再写
        break;
      }
      if (errno == EINTR) {
        continue;  // 重试
      }
      LOG_ERROR("send(write) error fd={} errno={} msg={}", fd(), errno, strerror(errno));
      handleError();
      break;
    }
  }
}

void TcpConnection::handleClose() {
  if (state_ == kDisconnected) {
    return;
  }
  LOG_INFO("TcpConnection fd={} closing (pending_replies={} buffered_out={} buffered_in={})",
           fd(),
           earlyReplies_.size(),
           outputBuffer_.readableByte(),
           inputBuffer_.readableByte());
  setState(kDisconnected);
  channel_.remove();

  // 主动清空：TcpConnection 会被「在途 PendingRequest 里的 shared_ptr」延长生命周期，
  // 如果这里不释放，等待队列排空之前这些缓冲会一直占着内存
  // （earlyReplies_ 里可能是几十条 MB 级的 GET 回复，outputBuffer_ 上限 16MB）。
  // 连接已经断了，这些数据也没有任何发送意义。
  earlyReplies_.clear();
  earlyReplyBytes_ = 0;
  hasCloseAfterReply_ = false;
  closeAfterReplySeq_ = 0;
  if (inputBuffer_.readableByte() > 0) {
    inputBuffer_.consume(inputBuffer_.readableByte());
  }
  if (outputBuffer_.readableByte() > 0) {
    outputBuffer_.consume(outputBuffer_.readableByte());
  }

  if (closeCallback_) {
    // 这里会清楚tcpserver的所有权，防止tcpconnection被释放
    auto self = shared_from_this();
    //调用tcpserver::removeConnection()
    closeCallback_(self);
  } else if (connectionCallback_) {
    auto self = shared_from_this();
    connectionCallback_(self);  // 也可分离为 onConnected/onDisconnected
  }
}

void TcpConnection::handleError() {
  // 发生严重错误时直接关闭
  LOG_ERROR("TcpConnection fd={} encountered error, force closing", fd());
  forceClose();
}

}  // namespace net
