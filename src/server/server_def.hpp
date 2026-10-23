#pragma once

#include "net/TcpServer.hpp"
#include "protocol/request.hpp"

namespace server{
//  PendingRequest 结构体用于封装待处理的请求信息，包括请求本身、连接对象和事件循环器指针
struct PendingRequest {
  protocol::Request request;
  net::TcpServer::TcpConnectionPtr connection;
  net::EventLoop* ioLoop;
};
};