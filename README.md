# LightKV

一个用于学习 KV 存储原理的轻量级服务，用 C++17 实现，走 LSM-tree 路线，
**Redis 协议（RESP）**。

选 RESP 的原因很实际：`redis-cli` 能直接当客户端，`redis-benchmark` /
`memtier_benchmark` 能直接压测，各语言官方 Redis 客户端也能直接连，
不需要再维护一套自研协议和自研压测工具。

## 项目目的

- 亲手实现一条完整的存储写入链路：**写请求 → 攒批 → WAL 落盘 → MemTable → SSTable**
- 亲手实现一条完整的网络链路：**epoll → Reactor（one loop per thread）→ 跨线程投递 → 有序回包**
- 用真实工具（`redis-cli` / `redis-benchmark`）而不是自造轮子来验证正确性和性能
- 在可控规模下把并发、持久化、协议语义这些容易出问题的地方踩一遍

## 已实现的功能

### 网络层

- 模仿muduo库，epoll **LT** 模式 + **one loop per thread**，多 IO 线程 + 独立 acceptor
- 自动扩容的读写 Buffer，正确处理**半包 / 粘包**
- 输入缓冲上限保护（超限断开，避免被超大帧撑爆）
- 输出缓冲高水位保护（`Buffer::append` 空间不足时会静默截断，截断会产生残缺的
  RESP 帧，所以改为主动断开慢消费者）

### 协议层（RESP）

| 命令 | 回复 |
| --- | --- |
| `PING` / `PING msg` | `+PONG` / bulk 形式的 msg |
| `SET key value` | `+OK` |
| `GET key` | bulk value / `$-1`（未命中） |
| `DEL key` | `:1` / `:0` |
| `QUIT` | `+OK` 后关闭连接 |
| 其他 | `-ERR unknown command '<name>'`，**不断开连接** |

- 命令名**大小写不敏感**，value **二进制安全**（可含 `\r\n`、`\0`）
- 支持 **inline 命令**（不带 `*` 前缀），方便 `telnet` / `nc` 手测
- 同一连接严格满足「**一个请求恰好一个回复，且顺序与请求一致**」
  （见下方「回复保序」）

### 存储层

- **WAL**：追加写 + `fdatasync`，进程启动时回放恢复；`SET`/`DEL` 只有落盘成功才回复
- **MemTable**：`std::map` 有序内存表，记录数达到 64K 时切换为 immutable 并异步刷盘
- **SSTable**：数据块 + 稀疏索引 + footer，每个 block 带 CRC 校验；
  查询时先二分索引再读单块；支持删除标记（tombstone）
- 读路径按 **active → immutable → flushing → SSTable 由新到旧**逐层查找
- **组提交**：`WriteBatchCoordinator` 攒批（1ms 窗口 / 64KB），减少 fsync 次数
- **背压**：请求队列有界（64K 条 / 64MB），超限回 `-ERR` 并断开，避免深 pipeline 打爆内存

### 回复保序（设计中比较关键的一块）

写请求要经过 WAL 线程、读请求经过存储线程、`PING` 直接在 IO 线程回，
执行天然是乱序的。为了保证 RESP 的顺序语义：

- IO 线程每解析出一条请求就分配一个**连接级序号**
- 回复按序号重排后再输出：正常情况走快路径（不触碰 `std::map`），
  乱序时才进入重排缓冲
- 同时把 GET/SET/DEL 放进同一条 FIFO，保证**执行顺序 == 请求顺序**，
  这样 pipeline 里的 `SET k v` 紧跟 `GET k` 能读到新值

### 尚未实现

- **Compaction**：SSTable 只增不减，长期写入会累积文件数
- 过期时间、多 DB、事务、`MGET`/`SCAN`/`INCR` 等其他命令
- 存储分片并行（当前执行路径是单线程的，多 IO 线程只并行网络与解析）

## 快速开始

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

依赖：CMake ≥ 3.16、支持 C++17 的编译器、`spdlog`、`pthread`。

### 运行

```bash
# 用法: lightkv_kv_server [线程数=1] [端口=8990] [WAL 路径=wal]
./build/lightkv_kv_server 4
./build/lightkv_kv_server 4 8990 wal
```

> 压测或跑性能数据时务必降低日志级别，默认 `trace` 会把每条请求的十几行日志
> 都打出来，实测吞吐会差约 20 倍：
>
> ```bash
> SPDLOG_LEVEL=err ./build/lightkv_kv_server 4
> ```

### 用 redis-cli 操作

服务端监听 **8990**（不是 6379），记得带 `-p`：

```bash
$ redis-cli -p 8990 ping
PONG
$ redis-cli -p 8990 set foo bar
OK
$ redis-cli -p 8990 get foo
"bar"
$ redis-cli -p 8990 del foo
(integer) 1
$ redis-cli -p 8990 get foo
(nil)
$ redis-cli -p 8990 lpush nope 1
(error) ERR unknown command 'lpush'
```

没有 `redis-cli` 时也可以用 `nc` 手敲 inline 命令：

```bash
printf 'SET foo bar\r\nGET foo\r\n' | nc 127.0.0.1 8990
```

### 压测

```bash
redis-benchmark -h 127.0.0.1 -p 8990 -t ping,set,get -n 100000 -c 50 -P 16 -q
```

完整的压测流程、参数含义、结果解读和火焰图分析见
[`docs/load_test_guide.md`](docs/load_test_guide.md)。

> `redis-benchmark` 启动时会打印 `WARNING: Could not fetch server CONFIG`，
> 因为服务端没实现 `CONFIG` 命令，**不影响压测结果**。

## 测试

```bash
cd build && ctest --output-on-failure
```

`lightkv_server_e2e_test` 是主力端到端测试：自己 fork 服务端、自己实现 RESP
客户端，覆盖命令语义、大小写、二进制安全、未知命令不断连接、inline 命令、
半包、pipeline 顺序、`QUIT` 后同包命令不执行、瞬时断开、重启后 WAL 恢复等。

其他辅助程序：

| 程序 | 用途 |
| --- | --- |
| `lightkv_conn_stress_test` | 纯连接风暴（大量并发连接建立并保持），redis 工具覆盖不到 |
| `lightkv_client_demo` | 从 stdin 读一行发一行，配合 inline 命令手测 |
| `lightkv_server_demo` | 最小服务端示例（需 `-DLIGHTKV_BUILD_EXAMPLES=ON`） |

## 目录结构

```
src/
├── main.cpp                 程序入口（线程数 / 端口 / WAL 路径）
├── net/                     epoll、EventLoop、TcpServer、TcpConnection、Buffer
├── protocol/                RESP 解析（parser）与编码（codec）
├── server/                  KvServer、请求分发、组提交、请求处理线程
├── storage/
│   ├── wal/                 WAL 写入与回放
│   ├── memtable/            MemTable 与刷盘
│   └── sstable/             SSTable 构建与读取
└── util/                    日志、CRC32
```

## 日志

日志同时输出到终端和 `build/Log/server.log`（5MB 滚动，最多保留 3 个文件），
包含时间、级别、线程 ID 和源码位置，便于排查并发时序问题。

```bash
SPDLOG_LEVEL=debug ./build/lightkv_kv_server 4   # debug
SPDLOG_LEVEL=err   ./build/lightkv_kv_server 4   # 只留错误
tail -f build/Log/server.log
```

常用过滤：

```bash
grep -E 'Write request queued|Submitting WAL batch|WAL batch persisted|Request processed' \
  build/Log/server.log
```

Release 构建会裁剪 `TRACE`/`DEBUG` 调用。
