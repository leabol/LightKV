#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include "server/kv_server.hpp"
#include <iostream>

int main(int argc, char** argv) {
	std::string port = "8990";
	if (argc > 1) {
		port = argv[1];
	}

	net::EventLoop loop;
	net::InetAddress listenAddr(port);

	server::KvServer srv(&loop, listenAddr);
	srv.start();

	loop.loop(1000);
	return 0;
}
