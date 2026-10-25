#pragma once
#include <stdint.h>
#include <string>

namespace protocol {

enum class CommandType {
  GET,
  SET,
  DEL,
  PING,
  QUIT,
  // 未支持的命令；具体命令名见 Request::commandName
  UNKNOWN,
};

struct Request {
  CommandType cmd{CommandType::UNKNOWN};
  std::string key;
  std::string value;

  // 小写化后的命令名，仅用于拼错误信息（未知命令 / 参数个数不匹配）
  std::string commandName;
  // PING 是否带 message 参数：无参回 +PONG，带参回 bulk 形式的 message
  bool hasArgument{false};
  // 参数个数不匹配（此时 cmd 为 UNKNOWN，commandName 为对应命令）
  bool wrongArity{false};

  // 连接级的请求序号，由 IO 线程在解析出请求后分配，用于保证回复顺序。
  // 协议层不关心它的值，只在 PendingRequest 里传递。
  uint64_t seq{0};
};

}  // namespace protocol