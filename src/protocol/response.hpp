#pragma once
#include <string>

namespace protocol {

// RESP 回复类型，决定 encodeResponse 使用哪种前缀
enum class ReplyType {
  SimpleString,  // +OK\r\n
  Error,         // -ERR xxx\r\n
  Integer,       // :1\r\n
  BulkString,    // $3\r\nbar\r\n
  NullBulk,      // $-1\r\n
};

struct Response {
  bool ok = false;
  std::string value;
  ReplyType type = ReplyType::BulkString;
  int64_t integer = 0;
};

}  // namespace protocol