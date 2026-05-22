#include "buffer.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace net {

Buffer::Buffer(size_t bufferSize, size_t maxSize) :
    buffer_(bufferSize), readerIndex_(0), writerIndex_(0), maxSize_(maxSize) {}

//  buffer中可读的数据长度
size_t Buffer::readableByte() const {
  return writerIndex_ - readerIndex_;
}
//  到buffer尾部的可写入长度
size_t Buffer::writableByte() const {
  return buffer_.size() - writerIndex_;
}

// 向buffer申请写入len长度的数据，返回起始的指针，不改变writeIndex_
// 会自动扩容和先前移动数据
uint8_t* Buffer::beginWrite(size_t len) {
  if (len <= writableByte()) {
    return writePonint();
  }

  if (len <= freeSize()) {
    compact();
  } else {
    const size_t requiredSize = writerIndex_ + len;
    if (requiredSize > maxSize_) {
      return nullptr;
    }

    size_t newSize = buffer_.empty() ? kBufferSize : buffer_.size();
    while (newSize < requiredSize && newSize < maxSize_) {
      newSize = std::min(newSize * 2, maxSize_);
    }
    if (newSize < requiredSize) {
      newSize = requiredSize;
    }
    buffer_.resize(newSize);
  }

  return writePonint();
}

//  真实写入n字节，更新writerIndex_
void Buffer::updateWrite(size_t n) {
  assert(writerIndex_ + n <= buffer_.size());
  writerIndex_ += n;
}

//  尝试读取len字节的数据，如果不足则返回nullptr, 不改变readerIndex_
uint8_t* Buffer::peek(size_t len) {
  if (readableByte() >= len) {
    return readPoint();
  }
  return nullptr;
}


void Buffer::consume(size_t len) {
  if (len >= readableByte()) {
    // consume all
    readerIndex_ = 0;
    writerIndex_ = 0;
    return;
  }
  readerIndex_ += len;
}

void Buffer::append(const uint8_t* data, size_t len) {
  if (len == 0)
    return;
  uint8_t* dst = beginWrite(len);
  if (dst) {
    std::memcpy(dst, data, len);
    updateWrite(len);
    return;
  }
  // not enough space even after compact/grow; append as much as possible
  size_t avail = writableByte();
  if (avail == 0)
    return;
  size_t toCopy = std::min(avail, len);
  std::memcpy(writePonint(), data, toCopy);
  updateWrite(toCopy);
}


//  读取有效载荷，返回有效数据的起始地址，更改readerIndex_
uint8_t* Buffer::readPayload(size_t len) {
  if (readableByte() < len) {
    return nullptr;
  }
  uint8_t* data = readPoint() + 4;
  readerIndex_ += len;
  return data;
}

//  将数据移动到buffer的开头
void Buffer::compact() {
  assert(readerIndex_ <= writerIndex_ && writerIndex_ <= buffer_.size());
  const size_t readable = readableByte();
  if (readable > 0 && readerIndex_ > 0) {
    std::memmove(buffer_.data(), buffer_.data() + readerIndex_, readable);
  }
  readerIndex_ = 0;
  writerIndex_ = readable;
}

//  返回当前可以写入位置的指针
uint8_t* Buffer::writePonint() {
  return buffer_.data() + writerIndex_;
}
//  返回当前可以读取数据的指针
uint8_t* Buffer::readPoint() {
  return buffer_.data() + readerIndex_;
}

//  buffer中所有的空闲大小，包括readerIndex前面的空余
size_t Buffer::freeSize() const {
  return buffer_.size() - readableByte();
}

}  // namespace net
