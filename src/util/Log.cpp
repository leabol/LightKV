// 现代、健壮的日志实现：异步、彩色控制台 + 滚动文件、目录自动创建、环境变量控制

#include "util/Log.hpp"

#include <spdlog/async.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <filesystem>
#include <mutex>

namespace Server {

static std::shared_ptr<spdlog::logger> g_logger;
static std::once_flag g_once;

static std::filesystem::path resolveLogDir() {
#ifdef SERVER_LOG_BASE
  return std::filesystem::path(SERVER_LOG_BASE);
#else
  return std::filesystem::current_path() / "build" / "Log";
#endif
}

static void init_once() {
  // 1) 确保目录存在（静默失败不抛异常）
  std::error_code ec;
  const auto logDir = resolveLogDir();
  std::filesystem::create_directories(logDir, ec);

  // 2) Sinks：彩色控制台 + 旋转文件（5MB x 3 份）
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      (logDir / "server.log").string(), 5 * 1024 * 1024, 3, true);

  // 3) 异步日志线程池（8K 队列，1 个后台线程）
  constexpr std::size_t queue_size = 8192;
  constexpr std::size_t worker_threads = 1;
  spdlog::init_thread_pool(queue_size, worker_threads);

  // 4) 创建异步 logger 并注册
  g_logger = std::make_shared<spdlog::async_logger>("Server",
                                                    spdlog::sinks_init_list{console_sink, file_sink},
                                                    spdlog::thread_pool(),
                                                    spdlog::async_overflow_policy::block);

  spdlog::register_logger(g_logger);
  spdlog::set_default_logger(g_logger);  // 便于直接用 spdlog::info 等

  // 5) 分别设置格式：控制台着色，文件无色；包含源码位置信息
  console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [tid %t] [%s:%# %!()] %v");
  file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [tid %t] [%s:%# %!()] %v");

  // 6) 默认级别（可被环境变量覆盖）
  g_logger->set_level(spdlog::level::trace);
  // 支持：SPDLOG_LEVEL=info 或 SPDLOG_LEVEL="Server=debug,other=warn"
  spdlog::cfg::load_env_levels();

  // 7) 刷新策略：≥warn 立刻刷；后台每 2s 刷一次
  g_logger->flush_on(spdlog::level::warn);
  spdlog::flush_every(std::chrono::seconds(2));
}

void initLogger() {
  std::call_once(g_once, init_once);
}

std::shared_ptr<spdlog::logger> getLogger() {
  initLogger();
  return g_logger;
}

void setLevel(spdlog::level::level_enum level) {
  initLogger();
  g_logger->set_level(level);
}

void shutdownLogger() {
  if (g_logger) {
    g_logger->flush();
  }
  spdlog::shutdown();
}

}  // namespace Server
