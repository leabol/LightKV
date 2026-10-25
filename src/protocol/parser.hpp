#pragma once

#include "net/buffer.hpp"
#include "protocol/request.hpp"

// RESP (REdis Serialization Protocol) 请求解析。
//
// 客户端发来的请求是一个 bulk string 数组：
//   *<参数个数>\r\n
//   $<字节数>\r\n<参数>\r\n
//   ...
// 例如 SET foo bar：
//   *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
//
// 同时兼容 inline 命令（方便 telnet 手工测试）：
//   GET foo\r\n

namespace protocol {

enum class ParseResult {
  Ok,        // 解析出一条完整请求，req 已填充，buffer 已消费
  NeedMore,  // 数据不完整（半包），buffer 未被消费，等下次可读事件
  Error,     // 协议错误，调用方应回复错误并关闭连接
};

// 增量解析：只有完整解析出一条请求后才会消费 buffer，因此可直接在循环里调用
ParseResult parserRequest(net::Buffer &buffer, Request &req);

}  // namespace protocol