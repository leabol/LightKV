#pragma once

// 编译期日志裁剪级别：Debug 构建=TRACE，Release 构建=INFO
#ifndef SPDLOG_ACTIVE_LEVEL
#  ifdef NDEBUG
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#  else
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#  endif
#endif

#include <memory>
#include <spdlog/spdlog.h>

namespace Server {

// 线程安全初始化（可多次调用）
void initLogger();

// 优雅关闭（可选，程序退出前调用）
void shutdownLogger();

// 获取全局 logger（如未初始化将自动初始化）
std::shared_ptr<spdlog::logger> getLogger();

// 运行时调整级别（便于测试或交互切换）
void setLevel(spdlog::level::level_enum level);

} // namespace Server

// 使用 spdlog 的宏以获得源码位置（文件/行/函数）
#include <spdlog/common.h>

#define LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(::Server::getLogger().get(), __VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(::Server::getLogger().get(), __VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_LOGGER_INFO (::Server::getLogger().get(), __VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_LOGGER_WARN (::Server::getLogger().get(), __VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(::Server::getLogger().get(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(::Server::getLogger().get(), __VA_ARGS__)