#include <spdlog/common.h>
#include <sys/socket.h>

#include <iostream>
#include <string>

#include "Log.hpp"
#include "Socket.hpp"

int main() {
  Server::setLevel(spdlog::level::off);
  net::Socket socket;
  socket.connect("127.0.0.1", "8990");

  std::string message;
  while (true) {
    std::cin >> message;
    if (message == "quit") {
      break;
    }

    send(socket.fd(), message.data(), message.size(), 0);

    char buffer[1000];
    ssize_t n = recv(socket.fd(), buffer, sizeof(buffer), 0);
    if (n > 0) {
      std::string response(buffer, buffer + n);
      std::cout << response << std::endl;
    } else {
      std::cout << "failed to recv" << std::endl;
    }
  }

  return 0;
}