#include "protocol/codec.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace protocol {
namespace {

constexpr std::string_view kCrlf = "\r\n";

// 简单字符串 / 错误只能是一行，需要把内容里的换行替换掉，避免协议注入
void appendSingleLine(std::string& out, std::string_view s) {
  for (const char c : s) {
    out.push_back((c == '\r' || c == '\n') ? ' ' : c);
  }
}

}  // namespace

std::string encodeSimpleString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 3);
  out.push_back('+');
  appendSingleLine(out, s);
  out.append(kCrlf);
  return out;
}

std::string encodeError(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 3);
  out.push_back('-');
  appendSingleLine(out, s);
  out.append(kCrlf);
  return out;
}

std::string encodeInteger(int64_t v) {
  std::string out;
  out.reserve(24);
  out.push_back(':');
  out.append(std::to_string(v));
  out.append(kCrlf);
  return out;
}

std::string encodeBulkString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 16);
  out.push_back('$');
  out.append(std::to_string(s.size()));
  out.append(kCrlf);
  out.append(s.data(), s.size());
  out.append(kCrlf);
  return out;
}

std::string encodeNullBulkString() {
  return "$-1\r\n";
}

std::string encodeResponse(const Response& rep) {
  switch (rep.type) {
    case ReplyType::SimpleString:
      return encodeSimpleString(rep.value);
    case ReplyType::Error:
      return encodeError(rep.value);
    case ReplyType::Integer:
      return encodeInteger(rep.integer);
    case ReplyType::BulkString:
      return encodeBulkString(rep.value);
    case ReplyType::NullBulk:
      return encodeNullBulkString();
  }
  return encodeError("ERR internal error");
}

std::string encodeRequest(const std::vector<std::string>& argv) {
  std::string out;
  out.push_back('*');
  out.append(std::to_string(argv.size()));
  out.append(kCrlf);
  for (const auto& arg : argv) {
    out.push_back('$');
    out.append(std::to_string(arg.size()));
    out.append(kCrlf);
    out.append(arg.data(), arg.size());
    out.append(kCrlf);
  }
  return out;
}

}  // namespace protocol

