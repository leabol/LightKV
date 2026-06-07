#pragma once
#include <cstdint>

#include "net/buffer.hpp"
#include "protocol/request.hpp"

// Protocol frame layout:
//
//  +------------+--------+--------------+-----------+----------------+-----------+
//  | 4-byte len | 1-byte | 2-byte key   | key bytes | 3-byte value   | value     |
//  |            | op     | length       |           | length         | bytes     |
//  +------------+--------+--------------+-----------+----------------+-----------+
//  | total frame size includes all fields above, followed by variable-length key/value payloads |

namespace protocol {

// 从 uint8_t 指针读取大端 uint32_t
inline uint32_t read_u32(const uint8_t *data) {
  // 最高位字节(data[0])左移24位，依此类推
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

inline uint32_t read_u24(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) | data[2];
}

inline uint16_t read_u16(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 8) | data[1];
}


bool parserRequest(net::Buffer &buffer, Request &req);

}  // namespace protocol