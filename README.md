# LightKV

学习练习用的轻量级内存 KV 存储服务，基于 LSM-tree 架构。目前实现了 Reactor 模式的多线程 TCP 服务器、自定义二进制协议（GET/SET/DEL）、WAL 持久化，后续会逐步实现 MemTable、SSTable、Compaction 等组件。

## 构建与运行

```bash
mkdir build && cd build
cmake -DLIGHTKV_BUILD_EXAMPLES=ON ..
make -j$(nproc)
./build/lightkv_kv_server [线程数]   # 默认 1，监听端口默认 8990
```

## 日志

服务端默认输出日志到终端，并写入 `build/Log/server.log`（达到 5 MB
后最多保留 3 个滚动文件）。日志中包含时间、级别、线程 ID 和源码位置，
便于排查并发时序。

通过 `SPDLOG_LEVEL` 调整运行时日志级别：

```bash
SPDLOG_LEVEL=debug ./build/lightkv_kv_server 4
SPDLOG_LEVEL=trace ./build/lightkv_kv_server 4
SPDLOG_LEVEL=warn ./build/lightkv_kv_server 4
```

常用查看方式：

```bash
tail -f build/Log/server.log
grep -E 'Write request queued|Submitting WAL batch|WAL batch persisted|Request processed' \
  build/Log/server.log
```

开发版会编译 `TRACE` 日志；发布版会裁剪 `TRACE/DEBUG` 调用。若只想看
错误日志，可使用 `SPDLOG_LEVEL=err`。

客户端示例：`lightkv_protocol_client`、`lightkv_client_demo`
