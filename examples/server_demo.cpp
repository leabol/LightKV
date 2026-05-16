#include "TcpServer.hpp"
#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include "Log.hpp"
#include <spdlog/common.h>

int main() {
    Server::setLevel(spdlog::level::off);

    net::EventLoop loop;
    net::InetAddress listenAddr("8990");

    net::TcpServer server(&loop, listenAddr);
    server.setMessageCallback([](const net::TcpServer::TcpConnectionPtr& conn,
                                 net::Buffer& data) {
        size_t n = data.readableByte();
        if (n > 0) {
            std::string s(reinterpret_cast<char*>(data.readPoint()), n);
            conn->send(s);
            data.consume(n);
        }
    });

    server.start();
    loop.loop(1000);
    return 0;
}