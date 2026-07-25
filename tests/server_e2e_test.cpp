#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr uint8_t kGet = 0;
constexpr uint8_t kSet = 1;
constexpr uint8_t kDel = 2;
constexpr uint8_t kOk = 0xc8;
constexpr uint16_t kPort = 19090;

void WriteU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value);
}

void WriteU24(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 16);
  output[1] = static_cast<uint8_t>(value >> 8);
  output[2] = static_cast<uint8_t>(value);
}

uint32_t ReadU24(const uint8_t* input) {
  return (static_cast<uint32_t>(input[0]) << 16) |
         (static_cast<uint32_t>(input[1]) << 8) | input[2];
}

bool SendAll(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t count = send(fd, data.data() + sent, data.size() - sent, 0);
    if (count <= 0) return false;
    sent += static_cast<size_t>(count);
  }
  return true;
}

bool ReceiveAll(int fd, void* buffer, size_t size) {
  auto* output = static_cast<uint8_t*>(buffer);
  size_t received = 0;
  while (received < size) {
    const ssize_t count = recv(fd, output + received, size - received, 0);
    if (count <= 0) return false;
    received += static_cast<size_t>(count);
  }
  return true;
}

std::string MakeRequest(uint8_t command, const std::string& key,
                        const std::string& value) {
  const uint32_t total_size = 4 + 1 + 2 + key.size() + 3 + value.size();
  std::string request(total_size, '\0');
  auto* output = reinterpret_cast<uint8_t*>(request.data());
  output[0] = static_cast<uint8_t>(total_size >> 24);
  output[1] = static_cast<uint8_t>(total_size >> 16);
  output[2] = static_cast<uint8_t>(total_size >> 8);
  output[3] = static_cast<uint8_t>(total_size);
  output[4] = command;
  WriteU16(output + 5, static_cast<uint16_t>(key.size()));
  std::memcpy(output + 7, key.data(), key.size());
  WriteU24(output + 7 + key.size(), static_cast<uint32_t>(value.size()));
  std::memcpy(output + 10 + key.size(), value.data(), value.size());
  return request;
}

bool ReadResponse(int fd, uint8_t* status, std::string* value) {
  uint8_t header[8];
  if (!ReceiveAll(fd, header, sizeof(header))) return false;
  const uint32_t total_size = (static_cast<uint32_t>(header[0]) << 24) |
                              (static_cast<uint32_t>(header[1]) << 16) |
                              (static_cast<uint32_t>(header[2]) << 8) |
                              header[3];
  const uint32_t value_size = ReadU24(header + 5);
  if (total_size != value_size + sizeof(header)) return false;
  *status = header[4];
  value->assign(value_size, '\0');
  return value_size == 0 || ReceiveAll(fd, value->data(), value_size);
}

int ConnectToServer() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

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
  if (pid != 0) return pid;

  if (chdir(working_directory.c_str()) != 0) _exit(126);
  const std::string port = std::to_string(kPort);
  execl(executable.c_str(), executable.c_str(), port.c_str(), nullptr);
  _exit(127);
}

bool StopServer(pid_t pid) {
  if (pid <= 0) return false;
  kill(pid, SIGTERM);
  int status = 0;
  return waitpid(pid, &status, 0) == pid;
}

bool CheckRequest(int fd, uint8_t command, const std::string& key,
                  const std::string& value, uint8_t expected_status,
                  const std::string& expected_value) {
  if (!SendAll(fd, MakeRequest(command, key, value))) return false;
  uint8_t status = 0;
  std::string response;
  return ReadResponse(fd, &status, &response) &&
         status == expected_status && response == expected_value;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 1) return 1;
  const auto executable = std::filesystem::absolute(argv[0]).parent_path() /
                          "lightkv_kv_server";
  const auto working_directory = std::filesystem::temp_directory_path() /
                                 ("lightkv_server_e2e_" +
                                  std::to_string(static_cast<long long>(getpid())));
  std::filesystem::remove_all(working_directory);
  std::filesystem::create_directories(working_directory);

  pid_t server = StartServer(executable, working_directory);
  bool passed = server > 0 && WaitForServer();
  if (passed) {
    const int fd = ConnectToServer();
    passed = fd >= 0 &&
             CheckRequest(fd, kSet, "network-key", "network-value", kOk, "") &&
             CheckRequest(fd, kGet, "network-key", "", kOk, "network-value") &&
             CheckRequest(fd, kDel, "network-key", "", kOk, "") &&
             CheckRequest(fd, kGet, "network-key", "", 0, "");
    if (fd >= 0) close(fd);
  }
  if (server > 0) StopServer(server);

  server = StartServer(executable, working_directory);
  passed = passed && server > 0 && WaitForServer();
  if (passed) {
    const int fd = ConnectToServer();
    passed = fd >= 0 &&
             CheckRequest(fd, kGet, "network-key", "", 0, "");
    if (fd >= 0) close(fd);
  }
  if (server > 0) StopServer(server);

  std::filesystem::remove_all(working_directory);
  if (!passed) {
    std::cerr << "server end-to-end test failed\n";
  }
  return passed ? 0 : 1;
}
