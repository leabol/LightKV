#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include "server/kv_server.hpp"
#include "Log.hpp"
#include <spdlog/common.h>

int main(int argc, char** argv) {
    Server::setLevel(spdlog::level::off);
	std::string port = "8990";
	if (argc > 1) {
		port = argv[1];
	}

	net::EventLoop loop;
	net::InetAddress listenAddr(port);

	server::KvServer srv(&loop, listenAddr);
	srv.setThreadNum(8);
	srv.start();

	loop.loop();
	return 0;
}
