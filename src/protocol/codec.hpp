#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "protocol/response.hpp"

// RESP (REdis Serialization Protocol) 回复编码。
//
//   +OK\r\n            简单字符串
//   -ERR xxx\r\n       错误
//   :1\r\n             整数
//   $3\r\nbar\r\n      bulk string（GET 命中）
//   $-1\r\n            bulk 空值（GET 未命中）

namespace protocol {

// 简单字符串 / 错误行里不能出现 CRLF，内部会把 \r\n 替换成空格
std::string encodeSimpleString(std::string_view s);
std::string encodeError(std::string_view s);
std::string encodeInteger(int64_t v);
std::string encodeBulkString(std::string_view s);
std::string encodeNullBulkString();

// 按 Response::type 分派到上面的具体编码函数
std::string encodeResponse(const Response& rep);

// 把命令编码成 RESP 请求（客户端 / 测试用）
std::string encodeRequest(const std::vector<std::string>& argv);

}  // namespace protocol

