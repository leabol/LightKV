#include "protocol/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace protocol {
namespace {

// 单条命令最多的参数个数（防止恶意构造超大 argv）
constexpr int64_t kMaxArgc = 1024;
// inline 命令单行最大长度（超过后视为协议错误）
constexpr size_t kMaxInlineLength = 64UL * 1024;
// 错误信息里最多回显多少个命令名字节
constexpr size_t kMaxEchoCommandLength = 64;

enum class ScanResult { Ok, NeedMore, Error };

char toLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string toLowerAscii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(toLowerAscii(c));
  }
  return out;
}

bool equalsIgnoreCase(std::string_view s, std::string_view expect) {
  if (s.size() != expect.size()) {
    return false;
  }
  for (size_t i = 0; i < s.size(); ++i) {
    if (toLowerAscii(s[i]) != toLowerAscii(expect[i])) {
      return false;
    }
  }
  return true;
}

// 从 *pos 开始扫描一个以 CRLF 结尾的十进制整数。
// NeedMore 表示数据还没收全（此时 *pos 不会被修改）。
ScanResult scanInteger(const uint8_t *data, size_t avail, size_t *pos, int64_t *out) {
  size_t i = *pos;
  bool negative = false;
  if (i < avail && (data[i] == '-' || data[i] == '+')) {
    negative = (data[i] == '-');
    ++i;
  }

  size_t digits = 0;
  int64_t value = 0;
  while (i < avail && data[i] >= '0' && data[i] <= '9') {
    if (value > (INT64_MAX - 9) / 10) {
      return ScanResult::Error;  // 溢出
    }
    value = value * 10 + (data[i] - '0');
    ++i;
    ++digits;
  }

  if (i >= avail) {
    return ScanResult::NeedMore;  // 数字可能还没收全
  }
  if (i + 1 >= avail) {
    return ScanResult::NeedMore;  // 只收到了 '\r'
  }
  if (data[i] != '\r' || data[i + 1] != '\n' || digits == 0) {
    return ScanResult::Error;
  }

  *out = negative ? -value : value;
  *pos = i + 2;
  return ScanResult::Ok;
}

// 把 argv 映射成 Request。参数个数不对时置 wrongArity，交给上层回错误信息
ParseResult buildRequest(std::vector<std::string> argv, Request &req) {
  req = Request{};
  if (argv.empty()) {
    return ParseResult::Error;
  }

  std::string commandName = toLowerAscii(argv[0]);
  if (commandName.size() > kMaxEchoCommandLength) {
    commandName.resize(kMaxEchoCommandLength);
  }
  req.commandName = std::move(commandName);

  if (equalsIgnoreCase(argv[0], "get")) {
    if (argv.size() != 2) {
      req.wrongArity = true;
      return ParseResult::Ok;
    }
    req.cmd = CommandType::GET;
    req.key = std::move(argv[1]);
    return ParseResult::Ok;
  }

  if (equalsIgnoreCase(argv[0], "set")) {
    if (argv.size() != 3) {
      req.wrongArity = true;
      return ParseResult::Ok;
    }
    req.cmd = CommandType::SET;
    req.key = std::move(argv[1]);
    req.value = std::move(argv[2]);
    return ParseResult::Ok;
  }

  if (equalsIgnoreCase(argv[0], "del")) {
    if (argv.size() != 2) {
      req.wrongArity = true;
      return ParseResult::Ok;
    }
    req.cmd = CommandType::DEL;
    req.key = std::move(argv[1]);
    return ParseResult::Ok;
  }

  if (equalsIgnoreCase(argv[0], "ping")) {
    if (argv.size() > 2) {
      req.wrongArity = true;
      return ParseResult::Ok;
    }
    req.cmd = CommandType::PING;
    if (argv.size() == 2) {
      req.hasArgument = true;
      req.value = std::move(argv[1]);
    }
    return ParseResult::Ok;
  }

  if (equalsIgnoreCase(argv[0], "quit")) {
    if (argv.size() != 1) {
      req.wrongArity = true;
      return ParseResult::Ok;
    }
    req.cmd = CommandType::QUIT;
    return ParseResult::Ok;
  }

  req.cmd = CommandType::UNKNOWN;
  return ParseResult::Ok;
}

// inline 命令：GET foo\r\n
ParseResult parseInline(const uint8_t *data, size_t avail, net::Buffer &buffer, Request &req) {
  size_t lineEnd = std::string::npos;
  for (size_t i = 0; i + 1 < avail; ++i) {
    if (data[i] == '\r' && data[i + 1] == '\n') {
      lineEnd = i;
      break;
    }
  }
  if (lineEnd == std::string::npos) {
    // 还没有完整的一行；限制长度，避免对端一直发没有换行的数据把缓冲撑满
    return avail > kMaxInlineLength ? ParseResult::Error : ParseResult::NeedMore;
  }

  std::vector<std::string> argv;
  size_t i = 0;
  while (i < lineEnd && static_cast<int64_t>(argv.size()) < kMaxArgc) {
    while (i < lineEnd && (data[i] == ' ' || data[i] == '\t')) {
      ++i;
    }
    if (i >= lineEnd) {
      break;
    }
    const size_t start = i;
    while (i < lineEnd && data[i] != ' ' && data[i] != '\t') {
      ++i;
    }
    argv.emplace_back(reinterpret_cast<const char *>(data + start), i - start);
  }

  buffer.consume(lineEnd + 2);

  if (argv.empty()) {
    // 空行：不当作错误，返回一条 commandName 为空的 UNKNOWN，上层不回复
    req = Request{};
    return ParseResult::Ok;
  }
  return buildRequest(std::move(argv), req);
}

}  // namespace

ParseResult parserRequest(net::Buffer &buffer, Request &req) {
  const size_t avail = buffer.readableByte();
  if (avail == 0) {
    return ParseResult::NeedMore;
  }

  const uint8_t *data = buffer.peek(avail);
  if (data == nullptr) {
    return ParseResult::NeedMore;
  }

  // 非 '*' 开头按 inline 命令处理（redis-cli 等客户端一律用 RESP 数组）
  if (data[0] != '*') {
    return parseInline(data, avail, buffer, req);
  }

  size_t pos = 1;
  int64_t argc = 0;
  ScanResult scan = scanInteger(data, avail, &pos, &argc);
  if (scan == ScanResult::NeedMore) {
    return ParseResult::NeedMore;
  }
  if (scan == ScanResult::Error) {
    return ParseResult::Error;
  }
  if (argc <= 0 || argc > kMaxArgc) {
    return ParseResult::Error;
  }

  std::vector<std::string> argv;
  argv.reserve(static_cast<size_t>(argc));

  for (int64_t i = 0; i < argc; ++i) {
    if (pos >= avail) {
      return ParseResult::NeedMore;
    }
    if (data[pos] != '$') {
      return ParseResult::Error;
    }
    ++pos;

    int64_t length = 0;
    scan = scanInteger(data, avail, &pos, &length);
    if (scan == ScanResult::NeedMore) {
      return ParseResult::NeedMore;
    }
    if (scan == ScanResult::Error) {
      return ParseResult::Error;
    }
    if (length < 0) {
      return ParseResult::Error;
    }

    const size_t len = static_cast<size_t>(length);
    if (len > buffer.maxSize()) {
      return ParseResult::Error;  // 单参数不可能超过缓冲上限
    }
    if (avail - pos < len + 2) {
      return ParseResult::NeedMore;  // 参数还没收全
    }
    if (data[pos + len] != '\r' || data[pos + len + 1] != '\n') {
      return ParseResult::Error;  // 长度和实际内容不一致
    }

    argv.emplace_back(reinterpret_cast<const char *>(data + pos), len);
    pos += len + 2;
  }

  buffer.consume(pos);
  return buildRequest(std::move(argv), req);
}

}  // namespace protocol
