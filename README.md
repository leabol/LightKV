# LightKV

学习练习用的轻量级内存 KV 存储服务，基于 LSM-tree 架构。目前实现了 Reactor 模式的多线程 TCP 服务器、自定义二进制协议（GET/SET/DEL）、WAL 持久化，后续会逐步实现 MemTable、SSTable、Compaction 等组件。

## 构建与运行

```bash
mkdir build && cd build
cmake -DLIGHTKV_BUILD_EXAMPLES=ON ..
make -j$(nproc)
./build/lightkv_kv_server [port]   # 默认 8990
```

客户端示例：`lightkv_protocol_client`、`lightkv_client_demo`
