#include "net/EventLoop.hpp"
#include "server/request_processor.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

constexpr int kSubmitterCount = 8;
constexpr int kRequestsPerSubmitter = 128;
constexpr int kResponseTimeoutMs = 5000;

bool readFully(int fd, void* buffer, size_t size) {
  auto* bytes = static_cast<char*>(buffer);
  while (size > 0) {
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, kResponseTimeoutMs) <= 0) {
      return false;
    }
    const ssize_t count = ::recv(fd, bytes, size, 0);
    if (count <= 0) {
      return false;
    }
    bytes += count;
    size -= static_cast<size_t>(count);
  }
  return true;
}

uint32_t readU32(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

uint32_t readU24(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 16) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         static_cast<uint32_t>(bytes[2]);
}

void submitRequest(const std::shared_ptr<net::TcpConnection>& connection,
                   net::EventLoop* ioLoop,
                   server::RequestProcessor* processor,
                   protocol::Request request) {
  server::PendingRequest pending{std::move(request), connection, ioLoop};
  processor->handleRequest(pending);
}

}  // namespace

int main() {
  int sockets[2];
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  net::EventLoop ioLoop;
  auto connection = std::make_shared<net::TcpConnection>(&ioLoop, sockets[0]);
  connection->connectEstablished();

  server::RequestProcessor processor;
  processor.start();

  constexpr int kRequestCount = kSubmitterCount * kRequestsPerSubmitter;
  const int expectedResponseCount = kRequestCount * 2;
  std::thread submitters[kSubmitterCount];
  for (int submitter = 0; submitter < kSubmitterCount; ++submitter) {
    submitters[submitter] = std::thread([&, submitter] {
      for (int request = 0; request < kRequestsPerSubmitter; ++request) {
        const int id = submitter * kRequestsPerSubmitter + request;
        submitRequest(connection,
                      &ioLoop,
                      &processor,
                      {protocol::CommandType::SET,
                       "key-" + std::to_string(id),
                       "value-" + std::to_string(id)});
      }
    });
  }
  for (auto& submitter : submitters) {
    submitter.join();
  }

  for (int submitter = 0; submitter < kSubmitterCount; ++submitter) {
    submitters[submitter] = std::thread([&, submitter] {
      for (int request = 0; request < kRequestsPerSubmitter; ++request) {
        const int id = submitter * kRequestsPerSubmitter + request;
        submitRequest(connection,
                      &ioLoop,
                      &processor,
                      {protocol::CommandType::GET, "key-" + std::to_string(id), {}});
      }
    });
  }
  for (auto& submitter : submitters) {
    submitter.join();
  }
  processor.stop();

  bool responsesAreCorrect = true;
  std::unordered_map<std::string, int> values;
  int emptyResponses = 0;
  std::thread reader([&] {
    for (int response = 0; response < expectedResponseCount; ++response) {
      unsigned char header[8];
      if (!readFully(sockets[1], header, sizeof(header)) ||
          readU32(header) < sizeof(header) ||
          header[4] != 0xC8) {
        responsesAreCorrect = false;
        ioLoop.quit();
        return;
      }

      const uint32_t valueLength = readU24(header + 5);
      if (readU32(header) != valueLength + sizeof(header)) {
        responsesAreCorrect = false;
        ioLoop.quit();
        return;
      }

      std::string value(valueLength, '\0');
      if (!readFully(sockets[1], value.data(), value.size())) {
        responsesAreCorrect = false;
        ioLoop.quit();
        return;
      }
      if (value.empty()) {
        ++emptyResponses;
      } else {
        ++values[value];
      }
    }
    ioLoop.quit();
  });

  ioLoop.loop();
  reader.join();

  assert(responsesAreCorrect);
  assert(emptyResponses == kRequestCount);
  assert(values.size() == static_cast<size_t>(kRequestCount));
  for (int id = 0; id < kRequestCount; ++id) {
    assert(values["value-" + std::to_string(id)] == 1);
  }

  ::close(sockets[1]);
  return 0;
}
