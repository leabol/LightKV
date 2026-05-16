
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

namespace net {
    static const size_t kBufferSize = 4*1024;
    static const size_t kMaxSize = 64 * 1024;
class Buffer {
public:
    explicit Buffer(size_t bufferSize = kBufferSize, size_t maxSize = kMaxSize);

    size_t readableByte() const;

    size_t writableByte() const;

    uint8_t* readPoint();
    // 返回指向可写区域的指针，若需要扩容且超过 `maxSize_` 则返回 nullptr
    // 约定：调用者必须在收到 nullptr 时处理失败（例如丢弃消息或关闭连接）。
    uint8_t* beginWrite(size_t len);

    void updateWrite(size_t n);

    // len 表示整帧长度，且包含头部自身 4 字节
    uint8_t* peek(size_t len);

    uint8_t* readPayload(size_t len);

    void consume(size_t len);

    // append data to buffer (will try to grow/compact). If not enough room,
    // appends as much as possible.
    void append(const uint8_t* data, size_t len);
    void append(const char* data, size_t len) { append(reinterpret_cast<const uint8_t*>(data), len); }
    void append(const std::string& s) { append(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

    bool empty() const {
        return (readerIndex_ == writerIndex_) ? true : false;
    }
    size_t maxSize() const { return maxSize_; }
private:
    void compact();

    uint8_t* writePonint();


    size_t freeSize() const;
private:
    std::vector<uint8_t> buffer_;
    size_t readerIndex_{0};
    size_t writerIndex_{0};
    size_t maxSize_{0};
};

}//namespace net