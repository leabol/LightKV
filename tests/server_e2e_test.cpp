// 服务端端到端测试（RESP 协议）
//
// 覆盖：
//   1. 命令语义：PING / SET / GET / DEL / GET 未命中 / 空 value
//   2. 大小写不敏感
//   3. 二进制安全（value 里含 \r\n 和 \0）
//   4. 错误路径：未知命令、参数个数错误 → -ERR 且不断连接
//   5. inline 命令（不带 * 前缀）
//   6. 半包（逐字节发送）与 pipeline（一次性写多条）
//   7. 持久化：SET 后重启进程，GET 仍能读到；DEL 后重启仍是 nil
//   8. QUIT：回 +OK 后服务端关闭连接

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint16_t kPort = 19090;

// ---------------------------------------------------------------- RESP 编码

std::string EncodeRequest(const std::vector<std::string>& argv) {
  std::string out = "*" + std::to_string(argv.size()) + "\r\n";
  for (const auto& arg : argv) {
    out += "$" + std::to_string(arg.size()) + "\r\n";
    out += arg;
    out += "\r\n";
  }
  return out;
}

bool SendAll(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t count = send(fd, data.data() + sent, data.size() - sent, 0);
    if (count <= 0) {
      return false;
    }
    sent += static_cast<size_t>(count);
  }
  return true;
}

bool ReceiveAll(int fd, void* buffer, size_t size) {
  auto* output = static_cast<uint8_t*>(buffer);
  size_t received = 0;
  while (received < size) {
    const ssize_t count = recv(fd, output + received, size - received, 0);
    if (count <= 0) {
      return false;
    }
    received += static_cast<size_t>(count);
  }
  return true;
}

// 读一行（含结尾 CRLF），返回去掉 CRLF 的内容
bool ReadLine(int fd, std::string* line) {
  line->clear();
  char c = '\0';
  while (true) {
    if (!ReceiveAll(fd, &c, 1)) {
      return false;
    }
    if (c == '\r') {
      if (!ReceiveAll(fd, &c, 1) || c != '\n') {
        return false;
      }
      return true;
    }
    line->push_back(c);
    if (line->size() > 64 * 1024) {
      return false;  // 防御：正常回复不该有这么长的行
    }
  }
}

struct Reply {
  char type = '\0';  // '+', '-', ':', '$'
  std::string text;  // 简单字符串 / 错误 / 整数 / bulk 内容
  bool nullBulk = false;

  bool IsError() const { return type == '-'; }
  bool IsNull() const { return type == '$' && nullBulk; }
};

// 只支持本测试用到的 4 种回复类型（简单字符串/错误/整数/bulk）
bool ReadReply(int fd, Reply* reply) {
  std::string line;
  if (!ReadLine(fd, &line) || line.empty()) {
    return false;
  }
  reply->type = line[0];
  reply->text = line.substr(1);
  reply->nullBulk = false;

  switch (reply->type) {
    case '+':
    case '-':
    case ':':
      return true;
    case '$': {
      const long long length = std::stoll(reply->text);
      if (length < 0) {
        reply->nullBulk = true;
        reply->text.clear();
        return true;
      }
      reply->text.assign(static_cast<size_t>(length), '\0');
      if (length > 0 && !ReceiveAll(fd, reply->text.data(), static_cast<size_t>(length))) {
        return false;
      }
      char crlf[2];
      return ReceiveAll(fd, crlf, sizeof(crlf)) && crlf[0] == '\r' && crlf[1] == '\n';
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------- 断言辅助

bool ExpectEqual(const std::string& what, const Reply& reply, char expected_type,
                 const std::string& expected_text) {
  const bool ok = reply.type == expected_type && reply.text == expected_text;
  if (!ok) {
    std::cerr << "  FAIL " << what << ": expected (" << expected_type << " "
              << expected_text << "), got (" << reply.type << " " << reply.text << ")\n";
  }
  return ok;
}

bool ExpectNullBulk(const std::string& what, const Reply& reply) {
  if (!reply.IsNull()) {
    std::cerr << "  FAIL " << what << ": expected nil bulk ($-1), got (" << reply.type << " "
              << reply.text << ")\n";
    return false;
  }
  return true;
}

// 发一条命令并读回回复
bool RoundTrip(int fd, const std::vector<std::string>& argv, Reply* reply) {
  return SendAll(fd, EncodeRequest(argv)) && ReadReply(fd, reply);
}

int ConnectToServer() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  timeval timeout{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPort);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool WaitForServer() {
  for (int attempt = 0; attempt < 100; ++attempt) {
    const int fd = ConnectToServer();
    if (fd >= 0) {
      close(fd);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

pid_t StartServer(const std::filesystem::path& executable,
                  const std::filesystem::path& working_directory) {
  const pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }

  if (chdir(working_directory.c_str()) != 0) {
    _exit(126);
  }
  // 参数：线程数、端口。测试期间把日志降到 error，避免刷屏影响时序
  setenv("SPDLOG_LEVEL", "err", 1);
  const std::string threads = "1";
  const std::string port = std::to_string(kPort);
  execl(executable.c_str(), executable.c_str(), threads.c_str(), port.c_str(), nullptr);
  _exit(127);
}

bool StopServer(pid_t pid) {
  if (pid <= 0) {
    return false;
  }
  kill(pid, SIGTERM);
  int status = 0;
  return waitpid(pid, &status, 0) == pid;
}

// ---------------------------------------------------------------- 测试用例

// 基础命令语义：PING / SET / GET / DEL / 未命中 / 二进制安全 / 空 value
bool TestCommands(int fd) {
  Reply reply;

  if (!RoundTrip(fd, {"PING"}, &reply) || !ExpectEqual("PING", reply, '+', "PONG")) {
    return false;
  }
  if (!RoundTrip(fd, {"PiNg"}, &reply) || !ExpectEqual("PING 大小写", reply, '+', "PONG")) {
    return false;
  }
  if (!RoundTrip(fd, {"ping", "hello"}, &reply) ||
      !ExpectEqual("PING 带参数", reply, '$', "hello")) {
    return false;
  }

  if (!RoundTrip(fd, {"SET", "k1", "v1"}, &reply) || !ExpectEqual("SET", reply, '+', "OK")) {
    return false;
  }
  if (!RoundTrip(fd, {"set", "k1", "v1-again"}, &reply) ||
      !ExpectEqual("SET 小写覆盖", reply, '+', "OK")) {
    return false;
  }
  if (!RoundTrip(fd, {"Get", "k1"}, &reply) ||
      !ExpectEqual("GET 覆盖后", reply, '$', "v1-again")) {
    return false;
  }
  if (!RoundTrip(fd, {"GET", "no-such-key"}, &reply) || !ExpectNullBulk("GET 未命中", reply)) {
    return false;
  }

  if (!RoundTrip(fd, {"DEL", "k1"}, &reply) || !ExpectEqual("DEL 已存在", reply, ':', "1")) {
    return false;
  }
  if (!RoundTrip(fd, {"DEL", "k1"}, &reply) || !ExpectEqual("DEL 再删", reply, ':', "0")) {
    return false;
  }
  if (!RoundTrip(fd, {"GET", "k1"}, &reply) || !ExpectNullBulk("DEL 后 GET", reply)) {
    return false;
  }

  // 二进制安全：value 里含 CRLF 和 NUL，必须按长度原样返回
  const std::string binary_value = std::string("a\r\nb\0c", 6);
  if (!RoundTrip(fd, {"SET", "bin", binary_value}, &reply) ||
      !ExpectEqual("SET 二进制 value", reply, '+', "OK")) {
    return false;
  }
  if (!RoundTrip(fd, {"GET", "bin"}, &reply) ||
      !ExpectEqual("GET 二进制 value", reply, '$', binary_value)) {
    return false;
  }

  // 空 value 必须回 $0（而不是 $-1），两者语义不同
  if (!RoundTrip(fd, {"SET", "empty", ""}, &reply) ||
      !ExpectEqual("SET 空 value", reply, '+', "OK")) {
    return false;
  }
  if (!RoundTrip(fd, {"GET", "empty"}, &reply) || !ExpectEqual("GET 空 value", reply, '$', "")) {
    return false;
  }

  return true;
}

// 错误路径：未知命令 / 参数个数错误必须回 -ERR，且连接必须继续可用
bool TestErrors(int fd) {
  Reply reply;

  if (!RoundTrip(fd, {"FOOBAR", "x"}, &reply) || reply.type != '-' ||
      reply.text.rfind("ERR unknown command", 0) != 0) {
    std::cerr << "  FAIL 未知命令应回 -ERR, got (" << reply.type << " " << reply.text << ")\n";
    return false;
  }
  if (!RoundTrip(fd, {"SET", "only-key"}, &reply) || reply.type != '-' ||
      reply.text.rfind("ERR wrong number of arguments", 0) != 0) {
    std::cerr << "  FAIL SET 参数不足应回 -ERR, got (" << reply.type << " " << reply.text
              << ")\n";
    return false;
  }
  if (!RoundTrip(fd, {"GET"}, &reply) || reply.type != '-') {
    std::cerr << "  FAIL GET 参数不足应回 -ERR\n";
    return false;
  }

  // 关键：报错之后连接仍然可用（redis 客户端/压测工具依赖这一点）
  if (!RoundTrip(fd, {"PING"}, &reply) || !ExpectEqual("错误后 PING", reply, '+', "PONG")) {
    std::cerr << "  FAIL 未知命令后连接不可用\n";
    return false;
  }
  return true;
}

// inline 命令：不带 * 前缀，直接 "SET k v\r\n"
bool TestInlineCommands(int fd) {
  Reply reply;
  if (!SendAll(fd, "SET inline-key inline-value\r\n") || !ReadReply(fd, &reply) ||
      !ExpectEqual("inline SET", reply, '+', "OK")) {
    return false;
  }
  if (!SendAll(fd, "GET inline-key\r\n") || !ReadReply(fd, &reply) ||
      !ExpectEqual("inline GET", reply, '$', "inline-value")) {
    return false;
  }
  if (!RoundTrip(fd, {"DEL", "inline-key"}, &reply) ||
      !ExpectEqual("清理 inline-key", reply, ':', "1")) {
    return false;
  }
  return true;
}

// 半包：把一条完整请求逐字节发送，解析器必须能拼接后正确解析
bool TestPartialFrames(int fd) {
  Reply reply;
  for (const char c : EncodeRequest({"SET", "chunk-key", "chunk-value"})) {
    if (!SendAll(fd, std::string(1, c))) {
      return false;
    }
  }
  if (!ReadReply(fd, &reply) || !ExpectEqual("逐字节 SET", reply, '+', "OK")) {
    return false;
  }

  for (const char c : EncodeRequest({"GET", "chunk-key"})) {
    if (!SendAll(fd, std::string(1, c))) {
      return false;
    }
  }
  if (!ReadReply(fd, &reply) || !ExpectEqual("逐字节 GET", reply, '$', "chunk-value")) {
    return false;
  }

  if (!RoundTrip(fd, {"DEL", "chunk-key"}, &reply) ||
      !ExpectEqual("清理 chunk-key", reply, ':', "1")) {
    return false;
  }
  return true;
}

// 瞬时断开：发出大量请求后不读回复直接 close。
// 服务端必须清理掉该连接的 pending 回复/缓冲，并且不能影响后续连接。
bool TestAbruptClose() {
  const int fd = ConnectToServer();
  if (fd < 0) {
    return false;
  }
  std::string payload;
  for (int i = 0; i < 200; ++i) {
    payload += EncodeRequest({"SET", "abrupt-" + std::to_string(i), std::string(1024, 'x')});
  }
  // 不关心是否全部写出，也不读回复
  SendAll(fd, payload);
  close(fd);
  return true;
}

// pipeline：一次写入多条命令，必须按顺序拿到同样数量的回复
bool TestPipeline(int fd) {
  std::string payload;
  payload += EncodeRequest({"PING"});
  payload += EncodeRequest({"SET", "pipe-a", "1"});
  payload += EncodeRequest({"SET", "pipe-b", "2"});
  payload += EncodeRequest({"GET", "pipe-a"});
  payload += EncodeRequest({"GET", "pipe-b"});
  payload += EncodeRequest({"PING"});
  if (!SendAll(fd, payload)) {
    return false;
  }

  Reply reply;
  if (!ReadReply(fd, &reply) || !ExpectEqual("pipeline[0] PING", reply, '+', "PONG")) {
    return false;
  }
  if (!ReadReply(fd, &reply) || !ExpectEqual("pipeline[1] SET a", reply, '+', "OK")) {
    return false;
  }
  if (!ReadReply(fd, &reply) || !ExpectEqual("pipeline[2] SET b", reply, '+', "OK")) {
    return false;
  }
  if (!ReadReply(fd, &reply) || !ExpectEqual("pipeline[3] GET a", reply, '$', "1")) {
    return false;
  }
  if (!ReadReply(fd, &reply) || !ExpectEqual("pipeline[4] GET b", reply, '$', "2")) {
    return false;
  }
  if (!ReadReply(fd, &reply) || !ExpectEqual("pipeline[5] PING", reply, '+', "PONG")) {
    return false;
  }

  if (!RoundTrip(fd, {"DEL", "pipe-a"}, &reply) || !ExpectEqual("清理 pipe-a", reply, ':', "1")) {
    return false;
  }
  if (!RoundTrip(fd, {"DEL", "pipe-b"}, &reply) || !ExpectEqual("清理 pipe-b", reply, ':', "1")) {
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 1) {
    return 1;
  }
  const auto executable =
      std::filesystem::absolute(argv[0]).parent_path() / "lightkv_kv_server";
  const auto working_directory = std::filesystem::temp_directory_path() /
                                 ("lightkv_server_e2e_" +
                                  std::to_string(static_cast<long long>(getpid())));
  std::filesystem::remove_all(working_directory);
  std::filesystem::create_directories(working_directory);

  bool passed = true;

  // ---- 第一轮：命令语义 + 写入持久化验证用的数据 ----
  pid_t server = StartServer(executable, working_directory);
  if (server <= 0 || !WaitForServer()) {
    std::cerr << "server failed to start\n";
    if (server > 0) {
      StopServer(server);
    }
    std::filesystem::remove_all(working_directory);
    return 1;
  }

  {
    const int fd = ConnectToServer();
    if (fd < 0) {
      std::cerr << "connect failed\n";
      passed = false;
    } else {
      passed = TestCommands(fd) && passed;
      passed = TestErrors(fd) && passed;
      passed = TestInlineCommands(fd) && passed;
      passed = TestPartialFrames(fd) && passed;
      passed = TestPipeline(fd) && passed;
      passed = TestAbruptClose() && passed;

      // 瞬时断开之后，新连接必须照常工作（验证 handleClose 里的清理没有破坏状态机）
      Reply reply;
      passed = RoundTrip(fd, {"PING"}, &reply) && ExpectEqual("断开后 PING", reply, '+', "PONG") &&
               passed;

      // SET 的 +OK 是在 WAL fdatasync 之后才回的，所以这里确认过就代表已持久化
      passed = RoundTrip(fd, {"SET", "persist-key", "persist-value"}, &reply) &&
               ExpectEqual("SET 持久化 key", reply, '+', "OK") && passed;
      passed = RoundTrip(fd, {"SET", "deleted-key", "gone"}, &reply) &&
               ExpectEqual("SET 待删除 key", reply, '+', "OK") && passed;
      passed = RoundTrip(fd, {"DEL", "deleted-key"}, &reply) &&
               ExpectEqual("DEL 待删除 key", reply, ':', "1") && passed;
      close(fd);
    }
  }
  StopServer(server);

  // ---- 第二轮：重启后 WAL 回放，数据必须还在 / 删除必须仍生效 ----
  server = StartServer(executable, working_directory);
  if (server <= 0 || !WaitForServer()) {
    std::cerr << "server failed to restart\n";
    if (server > 0) {
      StopServer(server);
    }
    std::filesystem::remove_all(working_directory);
    return 1;
  }

  {
    const int fd = ConnectToServer();
    if (fd < 0) {
      std::cerr << "connect after restart failed\n";
      passed = false;
    } else {
      Reply reply;
      passed = RoundTrip(fd, {"GET", "persist-key"}, &reply) &&
               ExpectEqual("重启后 GET", reply, '$', "persist-value") && passed;
      passed = RoundTrip(fd, {"GET", "deleted-key"}, &reply) &&
               ExpectNullBulk("重启后被删 key", reply) && passed;
      passed = RoundTrip(fd, {"DEL", "persist-key"}, &reply) &&
               ExpectEqual("清理 persist-key", reply, ':', "1") && passed;
      close(fd);
    }
  }
  StopServer(server);

  // ---- 第三轮：QUIT 回 +OK 后服务端主动关闭连接 ----
  server = StartServer(executable, working_directory);
  if (server <= 0 || !WaitForServer()) {
    std::cerr << "server failed to start (quit case)\n";
    if (server > 0) {
      StopServer(server);
    }
    std::filesystem::remove_all(working_directory);
    return 1;
  }
  {
    const int fd = ConnectToServer();
    Reply reply;
    // 故意把 QUIT 和后续命令放在同一个 TCP 包里：QUIT 之后的命令必须被丢弃
    bool quit_ok = fd >= 0 &&
                   SendAll(fd, EncodeRequest({"QUIT"}) + EncodeRequest({"SET", "after-quit", "x"})) &&
                   ReadReply(fd, &reply) && ExpectEqual("QUIT", reply, '+', "OK");
    if (quit_ok) {
      // QUIT 之后对端应关闭连接：再读会拿到 EOF，且不会先收到 SET 的回复
      char c = '\0';
      quit_ok = recv(fd, &c, 1, 0) == 0;
      if (!quit_ok) {
        std::cerr << "  FAIL QUIT 后连接未被关闭（或仍在回复后续命令）\n";
      }
    }
    if (fd >= 0) {
      close(fd);
    }
    passed = quit_ok && passed;
  }
  StopServer(server);

  // ---- 第四轮：QUIT 之后的命令确实没有执行（重启后确认）----
  server = StartServer(executable, working_directory);
  if (server <= 0 || !WaitForServer()) {
    std::cerr << "server failed to start (post-quit case)\n";
    if (server > 0) {
      StopServer(server);
    }
    std::filesystem::remove_all(working_directory);
    return 1;
  }
  {
    const int fd = ConnectToServer();
    Reply reply;
    bool ok = fd >= 0 && RoundTrip(fd, {"GET", "after-quit"}, &reply) &&
              ExpectNullBulk("QUIT 后的命令不应被执行", reply);
    if (fd >= 0) {
      close(fd);
    }
    passed = ok && passed;
  }
  StopServer(server);

  std::filesystem::remove_all(working_directory);
  if (!passed) {
    std::cerr << "server end-to-end test failed\n";
    return 1;
  }
  std::cout << "server end-to-end test passed\n";
  return 0;
}

