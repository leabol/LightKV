# LightKV 压测指南

> 服务端已实现 **RESP（Redis 协议）**，因此压测统一使用 Redis 生态工具，
> 不再维护自研压测客户端。
>
> 本文是一份**可复现的压测流程**：环境准备 → 各场景指令 → 结果解读 → 火焰图分析。
> 每次对系统做重要改动后按此流程跑一遍，对比数据即可判断性能是提升还是回退。

---

## 目录

1. [环境准备](#1-环境准备)
2. [服务端与协议](#2-服务端与协议)
3. [工具说明](#3-工具说明)
4. [流程概览](#4-流程概览)
5. [测试 1：纯 GET 吞吐 & 延迟](#5-测试-1纯-get-吞吐--延迟)
6. [测试 2：纯 SET 吞吐 & 延迟](#6-测试-2纯-set-吞吐--延迟)
7. [测试 3：混合读写吞吐](#7-测试-3混合读写吞吐)
8. [测试 4：连接风暴](#8-测试-4连接风暴)
9. [火焰图采样与分析](#9-火焰图采样与分析)
10. [结果对比模板](#10-结果对比模板)
11. [常见问题](#11-常见问题)

---

## 1. 环境准备

### 1.1 编译

```bash
# 追求 QPS 数字：Release
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 追求火焰图可读性：Debug（含 -g，且不省略 frame pointer）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

| 目的 | 构建类型 | 原因 |
|------|---------|------|
| 最终 QPS / 延迟验收 | **Release** | `-O2`，数字才接近真实水平 |
| 火焰图热点分析 | **Debug** | 有完整符号，`perf` 才能解析函数名 |
| 功能回归 | Debug | 断言全开 |

> 仓库默认是 Debug。用同一台机器、同一构建类型做前后对比，否则数字没有可比性。

### 1.2 安装 redis-cli / redis-benchmark

Debian / Ubuntu：

```bash
sudo apt install redis-tools
redis-benchmark --version   # 需要 6.x 以上
```

**没有 root 权限时**可以就地解包（用于 Debian 系）：

```bash
mkdir -p ~/.local/redis-tools && cd /tmp
apt-get download redis-tools liblzf1
for deb in redis-tools_*.deb liblzf1_*.deb; do dpkg-deb -x "$deb" ~/.local/redis-tools; done

# 做个 wrapper，自动带上 liblzf 的路径
mkdir -p ~/.local/bin
for t in redis-cli redis-benchmark; do
  printf '#!/bin/sh\nexport LD_LIBRARY_PATH="$HOME/.local/redis-tools/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"\nexec "$HOME/.local/redis-tools/usr/bin/%s" "$@"\n' "$t" \
    > ~/.local/bin/$t
  chmod +x ~/.local/bin/$t
done
```

### 1.3 安装 memtier_benchmark（可选，但强烈推荐）

`redis-benchmark` 是**单进程客户端**，且只支持固定几种 workload。
`memtier_benchmark` 支持多线程、读写比例、key 分布、value 大小分布、数据校验，压高 QPS 时也更不容易成为瓶颈。

Debian 源里没有，需要源码编译（要 root 装依赖）：

```bash
sudo apt install build-essential autoconf automake libtool pkg-config libevent-dev libpcre2-dev
git clone https://github.com/RedisLabs/memtier_benchmark.git
cd memtier_benchmark
autoreconf -ivf && ./configure && make -j$(nproc)
sudo make install
```

### 1.4 关键：关掉服务端日志

`src/util/Log.cpp` 默认级别是 **trace**，每条请求会打十几行日志。实测这会把吞吐压到 **1/20**：

| 配置 | PING | SET | GET |
|------|:----:|:---:|:---:|
| 默认（trace） | 2.3k | 514 | 545 |
| `SPDLOG_LEVEL=err` | **18.2k** | **17.6k** | **18.2k** |

日志级别由 `spdlog::cfg::load_env_levels()` 从环境变量读取，所以启动时这样写：

```bash
SPDLOG_LEVEL=err ./build/lightkv_kv_server 1
```

### 1.5 检查 perf 权限（只在做火焰图时需要）

```bash
cat /proc/sys/kernel/perf_event_paranoid
```

| 值 | 含义 | 能否采样 |
|----|------|---------|
| -1 / 0 | 无限制 | ✅ |
| 1 | 禁止 CPU 事件计数，但 process-level 采样可用 | ⚠️ 够用 |
| ≥ 2 | 禁止内核 profiling | ❌ |

```bash
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid          # 临时
echo "kernel.perf_event_paranoid = 1" | sudo tee /etc/sysctl.d/99-perf.conf
sudo sysctl -p                                                   # 永久
```

### 1.6 安装 FlameGraph

```bash
git clone --depth=1 https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
```

用到的两个脚本：`stackcollapse-perf.pl`（折叠栈）、`flamegraph.pl`（渲染 SVG）。

---

## 2. 服务端与协议

### 2.1 启动参数

```
lightkv_kv_server [线程数=1] [端口=8990] [WAL 路径=wal]
```

```bash
# 单 IO 线程，默认端口
SPDLOG_LEVEL=err ./build/lightkv_kv_server 1

# 4 个 IO 线程（one-loop-per-thread），端口 8990
SPDLOG_LEVEL=err ./build/lightkv_kv_server 4 8990
```

### 2.2 支持的命令

| 命令 | 回复 | 说明 |
|------|------|------|
| `PING` | `+PONG` | |
| `PING msg` | `$n msg` | 带参数时回 bulk |
| `SET key value` | `+OK` | 落 WAL 并 `fdatasync` 后才回复 |
| `GET key` | `$n value` / `$-1` | 未命中回 `$-1` |
| `DEL key` | `:1` / `:0` | 删除成功 / key 不存在 |
| `QUIT` | `+OK` | 回复后关闭连接 |
| 其他 | `-ERR unknown command '<name>'` | **不断开连接** |

细节：

- 命令名**大小写不敏感**（`set` / `Set` / `SET` 都行）。
- 支持 **inline 命令**（`GET foo\r\n`），方便 `telnet` / `nc` 手测。
- value 是**二进制安全**的，可以包含 `\r\n` 和 `\0`。
- 空 value 回 `$0`，未命中回 `$-1`，两者不同。
- 单帧上限 64MB（`net::kMaxSize`），超出会回 `-ERR Protocol error` 并关闭连接。

### 2.3 与真实 Redis 的差异（压测时的注意事项）

| 项 | LightKV 行为 | 影响 |
|----|-------------|------|
| `CONFIG GET` / `INFO` / `SELECT` / `AUTH` / `HELLO` / `CLIENT` | 一律回 `-ERR unknown command` | `redis-benchmark` 启动时会打印 `WARNING: Could not fetch server CONFIG`，**无害** |
| `DEL` 返回值 | 基于"这次操作是否生效"，不完全等价于 Redis 的"实际删除个数" | 压测无影响；不要用 `DEL` 返回值做精确计数 |
| 只支持 GET/SET/DEL | `INCR`/`LPUSH`/`MSET`/`EXPIRE` 等回错误 | 压测务必带 `-t set,get`，不要跑默认全量命令 |
| 没有数据库/事务/订阅 | — | — |

---

## 3. 工具说明

| 工具 | 用途 | 何时用 |
|------|------|--------|
| `redis-cli` | 手工验证、`--pipe` 批量灌数据 | 压测前后做冒烟检查 |
| `redis-benchmark` | 官方标准压测，内置 PING/SET/GET 等 workload、pipeline、延迟分位 | **所有基础吞吐/延迟测试** |
| `memtier_benchmark` | 多线程、读写比例、key 分布、`--verify` 校验 | 混合负载、多线程压满、正确性验证 |
| `lightkv_conn_stress_test` | **纯连接风暴**：建立大量连接并保持，不发业务请求 | 找连接数上限 / 查 fd 泄漏 |
| `lightkv_server_e2e_test` | 端到端功能回归（RESP 语义、半包、pipeline、重启恢复、QUIT） | 改协议/路由后先跑它 |

`redis-benchmark` 常用参数：

| 参数 | 含义 |
|------|------|
| `-h` / `-p` | 服务端地址 / 端口（**本机默认 8990，不是 6379**） |
| `-n` | 总请求数 |
| `-c` | 并发连接数（注意：**不是线程数**） |
| `-P` | pipeline 深度（一次发多少条不等回复） |
| `-d` | value 字节数 |
| `-t` | 要测的命令类型，如 `ping,set,get` |
| `-r` | key 空间大小，key 形如 `key:000000012345` |
| `--threads` | 客户端线程数，多核压满时必须加 |
| `-q` | 只打印汇总 |

> `redis-benchmark` 是单进程的，`-c 200` 也可能压不满多核服务端。
> 需要更高压力时用 `--threads 4`，或换 `memtier_benchmark`。

---

## 4. 流程概览

```
① 启动服务端（SPDLOG_LEVEL=err）
    │
② 冒烟：redis-cli ping / set / get / del
    │
③ warmup：灌入 baseline key 空间
    │
④ 测试 1 纯 GET（P=1 测延迟，P=16 测吞吐）
⑤ 测试 2 纯 SET
⑥ 测试 3 混合读写
⑦ 测试 4 连接风暴
    │
⑧ 火焰图采样（读 / 写分别采）
    │
⑨ 整理数据 → 写入报告
```

> `memtable` 的阈值是 64K 条记录（`Memtable::kMaxMemtableSize`），超过会切 immutable 并 flush 成 SSTable。
> 所以压到几十万 key 时会经历多次 flush/compaction，**这是设计内的行为**，不是异常。

冒烟检查：

```bash
redis-cli -p 8990 ping                     # PONG
redis-cli -p 8990 set foo bar              # OK
redis-cli -p 8990 get foo                  # "bar"
redis-cli -p 8990 del foo                  # (integer) 1
redis-cli -p 8990 get foo                  # (nil)
redis-cli -p 8990 lpush nope 1             # (error) ERR unknown command 'lpush'
```

---

## 5. 测试 1：纯 GET 吞吐 & 延迟

### 5.1 目的

- 测量**只读**场景的极限 QPS
- 拿到 P50/P95/P99 延迟分布，判断长尾
- 验证多 IO 线程能否吃满多核

### 5.2 指令

```bash
# 1) warmup：灌 20 万条 key（-r 让 key 变成随机，避免每次都命中同一个）
redis-benchmark -h 127.0.0.1 -p 8990 -t set -n 200000 -r 200000 -d 128 -q

# 2) 延迟测试：无 pipeline，单条往返，最能反映真实延迟
redis-benchmark -h 127.0.0.1 -p 8990 -t get -n 20000 -c 20 -r 200000 -P 1 -q

# 3) 吞吐测试：pipeline 16
redis-benchmark -h 127.0.0.1 -p 8990 -t get -n 1000000 -c 50 -r 200000 -P 16 -q
```

### 5.3 示例输出

```
GET: 17917.94 requests per second, p50=1.439 msec
```

`-P 1` 时更能看出单次往返延迟：

```
GET: 41322.31 requests per second, p50=0.423 msec
```

### 5.4 关注重点

| 指标 | 关注点 |
|------|--------|
| `requests per second` | 绝对值越高越好。改动前后对比时**下降超过 5% 需警惕** |
| `p50` | 大多数请求的体验 |
| `p99` | 长尾。突然飙升通常意味着锁竞争、GC 式抖动或队列积压 |
| 是否出现 error | 出现 `-ERR` 说明请求被打错，检查 `-t` 是否只用了支持的命令 |

### 5.5 调参

```bash
# 单连接单线程：测纯延迟下限
redis-benchmark -p 8990 -t get -n 10000 -c 1 -P 1 -q

# 大 value：测带宽与内存分配
redis-benchmark -p 8990 -t set,get -n 50000 -c 20 -d 4096 -q

# 多线程客户端压多核服务端（先启动 lightkv_kv_server 4）
redis-benchmark -p 8990 -t get -n 1000000 -c 100 -P 16 --threads 4 -q
```

---

## 6. 测试 2：纯 SET 吞吐 & 延迟

### 6.1 目的

- 测量**写入**场景的极限 QPS
- SET 路径最长：`IO 线程 → 批量协调线程 → WAL 线程（write + fdatasync）→ 存储线程`
- 验证批量提交（`kCommitWindow = 1ms`、`kMaxBatchSize = 64KB`）是否有效

### 6.2 指令

```bash
# 写满 50 万不同 key
redis-benchmark -h 127.0.0.1 -p 8990 -t set -n 500000 -c 50 -r 500000 -d 128 -P 16 -q

# 单条往返，看写延迟
redis-benchmark -h 127.0.0.1 -p 8990 -t set -n 5000 -c 20 -d 128 -P 1 -q
```

### 6.3 示例输出

```
SET: 17771.46 requests per second, p50=2.135 msec      # -P 16
SET: 10869.57 requests per second, p50=1.655 msec      # -P 1（无水位参考，仅示意）
```

### 6.4 关注重点

| 指标 | 关注点 |
|------|--------|
| `p50` | SET 一定比 GET 高，因为**必须等 `fdatasync` 完成才回复**（这是持久化语义，不是 bug） |
| QPS 随并发增长 | 如果 `-c 50` 比 `-c 1` 没什么提升，说明批量提交没生效 |
| 磁盘状态 | SET 受磁盘 fsync 能力限制。机械盘/云盘 IOPS 低时 QPS 上限很低 |
| SSTable 刷盘 | key 数超过 64K 后会触发 flush，QPS 会出现阶段性下降 |

### 6.5 交叉验证磁盘是否瓶颈

```bash
iostat -x 1        # 观察 %util 和 await
pidstat -d -p $(pgrep lightkv_kv_serv) 1
```

如果 `%util` 接近 100%，说明瓶颈在磁盘 fsync，优化方向是批量提交/组提交，而不是改 CPU 代码。

---

## 7. 测试 3：混合读写吞吐

### 7.1 目的

- 模拟真实 workload
- 观察读操作是否被写路径拖慢（现在 GET 和 SET 走同一条有序队列）
- 验证 `DEL` 路径

### 7.2 指令

用 `redis-benchmark`（读写各测一遍，交替看）：

```bash
redis-benchmark -p 8990 -t set,get -n 200000 -c 50 -r 100000 -d 128 -P 16 -q
```

**更推荐 memtier**，因为它能在**同一批连接上按比例交错发送**，更接近真实服务：

```bash
# 读写 1:1，4 线程 × 25 连接
memtier_benchmark -s 127.0.0.1 -p 8990 --protocol=redis \
  -t 4 -c 25 -n 20000 --ratio=1:1 -d 128 --key-pattern=R:R

# 读多写少 9:1
memtier_benchmark -s 127.0.0.1 -p 8990 --protocol=redis \
  -t 4 -c 25 -n 20000 --ratio=9:1 -d 128

# 打开数据校验：确认 GET 拿到的就是 SET 写进去的值
memtier_benchmark -s 127.0.0.1 -p 8990 --protocol=redis \
  -t 2 -c 10 -n 5000 --ratio=1:1 --verify
```

### 7.3 示例输出（memtier）

```
Type        Ops/sec     Hits/sec   Misses/sec    Latency    KB/sec
Sets       8231.60          ---           ---      5.884     619.66
Gets       8231.60      8121.30       110.30      5.744     604.28
Totals    16463.20      8121.30       110.30      5.814    1223.94
```

### 7.4 关注重点

| 指标 | 关注点 |
|------|--------|
| `Misses/sec` | 有 miss 是正常的（随机 key 不一定都被 SET 过）。但如果 miss 率接近 100%，说明 warmup 或 `--key-pattern` 配错了 |
| `Latency` | 混合负载下 GET 延迟会靠近 SET 延迟。这是**当前架构的必然结果**（GET 排在写批次之后以保持一致顺序） |
| `--verify` | **必须 0 校验失败**。失败说明读到了旧值，即顺序/可见性被破坏 |
| Sets/Gets 比例 | 应接近 `--ratio` |

### 7.5 与历史数据对比时的提醒

旧版的 `lightkv_rbench_client` 把"GET 未命中"算作 failure，所以历史报告里的 `fail` 数字**不能**直接和现在的输出对比。现在用 `Misses/sec` 表达同一件事。

---

## 8. 测试 4：连接风暴

### 8.1 目的

- 测服务器能同时保持多少 TCP 连接
- 验证连接建立/关闭没有 fd 或内存泄漏
- 检查 listen backlog 是否够用

`redis-benchmark` 只能"建连接 + 发请求"，测不了**空闲连接保持**，所以这个场景保留自研工具。

### 8.2 指令

```bash
# 5K 空闲连接，50 并发建立，保持 5 秒后释放
./build/lightkv_conn_stress_test -n 5000 -c 50 --idle

# 1 万连接
./build/lightkv_conn_stress_test -n 10000 -c 50 --idle

# 带 PING（走 RESP 路径，验证连接可用）
./build/lightkv_conn_stress_test -n 5000 -c 50 --ping

# 探索上限
./build/lightkv_conn_stress_test -n 20000 -c 100 --idle
```

### 8.3 示例输出

```
success=10000 failed=0 elapsed_s=0.553 conn/s=18086
fds_held=10000
holding 10000 connections for 5 seconds...
releasing connections.
```

### 8.4 关注重点

| 指标 | 关注点 |
|------|--------|
| `failed` | 必须 0。非 0 见 8.5 |
| `conn/s` | 受 `somaxconn`、CPU、backlog 限制 |
| `fds_held` | 应等于 `success`。保持期间持续增长说明 fd 泄漏 |
| 保持期稳定性 | 期间不应 crash，CPU 占用应接近 0（idle 连接不应有轮询） |

### 8.5 失败排查

```bash
cat /proc/sys/net/core/somaxconn                      # backlog 上限
cat /proc/$(pgrep lightkv_kv_serv)/limits | grep "open files"
ss -tn | grep 8990 | wc -l                            # 当前连接数
ss -tn state time-wait | wc -l                        # TIME_WAIT 堆积
```

| 原因 | 现象 | 解决 |
|------|------|------|
| `somaxconn` 太小 | 大量 connect 立即被拒 | `sudo sysctl -w net.core.somaxconn=65535` |
| `ulimit -n` 太小 | 到上限后 fail | `ulimit -n 100000` 或 systemd 配 `LimitNOFILE` |
| 临时端口耗尽 | 客户端侧报 `Cannot assign requested address` | `sysctl -w net.ipv4.ip_local_port_range="10000 65000"` |
| 服务端崩溃 | 全部被拒 | `ps aux | grep lightkv`、看日志 |

---

## 9. 火焰图采样与分析

### 9.1 目的

火焰图回答一个问题：**CPU 时间花在哪了？**

- 宽条 = 热点 = 优化机会
- 从下往上读 = 调用链
- 鼠标悬停可看函数名与占比

### 9.2 采样原则

| 原则 | 说明 |
|------|------|
| 必须在负载下采样 | 空载只有 `epoll_wait`，没有意义 |
| `-F 99` | Brendan Gregg 推荐频率，样本量够且干扰小 |
| 采 10–15 秒 | 太短样本不足，太长会跨过负载阶段 |
| 读、写分别采 | 纯 GET 和纯 SET 的热点完全不同 |

### 9.3 纯 GET 采样

```bash
# 1. 取服务端 PID
SERVER_PID=$(pgrep lightkv_kv_serv)
echo "Server PID: $SERVER_PID"

# 2. 后台采样 15 秒
perf record -F 99 -p $SERVER_PID -g -o /tmp/perf-get.data -- sleep 15 &

# 3. 在采样窗口内跑压测（另开一个终端也行）
redis-benchmark -h 127.0.0.1 -p 8990 -t get -n 2000000 -c 50 -r 200000 -P 16 -q

# 4. 等采样结束
wait

# 5. 生成火焰图
perf script -i /tmp/perf-get.data \
  | ~/FlameGraph/stackcollapse-perf.pl \
  | ~/FlameGraph/flamegraph.pl > docs/flamegraph_get.svg
```

**预期的调用链**：

```
EventLoop::loop()
├─ EpollPoller::poll()                 ← epoll_wait（空闲时最宽）
└─ Channel::handleEvent()
   └─ TcpConnection::handleRead()
      ├─ ::recv()                      ← 内核网络栈
      └─ KvServer::onMessage()
         ├─ protocol::parserRequest()  ← RESP 解析
         └─ WriteBatchCoordinator::submitWriteReq()   ← 投递到有序队列
            ...
   └─ TcpConnection::flushOrderedReplies()
      └─ ::send()                      ← 回包
```

存储线程侧：

```
RequestProcessor::processLoop()
├─ Dispatcher::dispatch()
│  └─ Memtable::GET()
│     ├─ std::map::find()
│     └─ sstable::Reader::Lookup()
└─ encodeResponse()
```

**关注点**：

- `parserRequest` 的宽度 → RESP 解析有没有多余拷贝
- `::recv` / `::send` 的宽度 → 系统调用次数是否过多（chunk 太小 / 每条回复单独 send）
- `Memtable::GET` 的宽度 → map 查找 + SSTable 探测是否成为瓶颈
- `std::mutex` 的宽度 → 存储层内部锁竞争

### 9.4 混合负载采样

```bash
perf record -F 99 -p $SERVER_PID -g -o /tmp/perf-mixed.data -- sleep 15 &

memtier_benchmark -s 127.0.0.1 -p 8990 --protocol=redis -t 4 -c 25 -n 20000 --ratio=1:1 -d 128

wait
perf script -i /tmp/perf-mixed.data \
  | ~/FlameGraph/stackcollapse-perf.pl \
  | ~/FlameGraph/flamegraph.pl > docs/flamegraph_mixed.svg
```

混合负载会额外出现：

- `Memtable::SET` / `Memtable::DEL`
- `wal::WALWriter::writeLoop` → `::write` + `::fdatasync`（**fsync 很常见的一条宽条，说明写受磁盘限制**）
- `WriteBatchCoordinator::submitWriteLoop`

### 9.5 对比火焰图

```bash
perf script -i /tmp/perf-before.data | ~/FlameGraph/stackcollapse-perf.pl > /tmp/before.folded
perf script -i /tmp/perf-after.data  | ~/FlameGraph/stackcollapse-perf.pl > /tmp/after.folded

~/FlameGraph/difffolded.pl /tmp/before.folded /tmp/after.folded \
  | ~/FlameGraph/flamegraph.pl > docs/flamegraph_diff.svg
```

红色 = 变多，蓝色 = 变少。

### 9.6 注意事项

- `perf` 仅 Linux 可用。
- Release 下内联严重、栈可能不完整；做火焰图优先用 Debug。
- `-F 99 -- sleep 15` 约 1500 个样本。图太稀说明采样不足。
- 采样期间别同时跑其他重负载程序。

---

## 10. 结果对比模板

```markdown
## 改动说明

<!-- 一句话，例如："GET/SET 统一走有序队列" -->

## 环境

- 日期: 2026-10-25
- 编译: Release / Debug
- 服务端 IO 线程数: 1
- 日志级别: SPDLOG_LEVEL=err
- 内核: `uname -r`
- 客户端: redis-benchmark 8.0.2 / memtier_benchmark x.y

## 结果

| 测试 | 命令 | QPS | p50 | p99 | 对比基线 |
|------|------|:---:|:---:|:---:|:--------:|
| 纯 GET (P=1) | `-t get -n 20000 -c 20 -P 1` | 41,322 | 0.42ms | — | — |
| 纯 GET (P=16) | `-t get -n 1000000 -c 50 -P 16` | 17,918 | 1.44ms | — | — |
| 纯 SET (P=16) | `-t set -n 500000 -c 50 -P 16` | 17,771 | 2.14ms | — | — |
| 混合 1:1 (memtier) | `--ratio=1:1 -t 4 -c 25` | 16,463 | 5.81ms | — | — |
| 连接风暴 10K | `conn_stress_test -n 10000 --idle` | 18,086 conn/s | — | — | — |

## 火焰图观察

<!-- 新图和旧图相比，热点函数有什么变化 -->

## 结论

<!-- 提升还是回退？是否达到预期？ -->
```

---

## 11. 常见问题

### Q: 为什么 `GET` 也要经过"写队列"？

为了让**执行顺序等于请求顺序**。否则 pipeline 里的 `SET k v` 紧跟 `GET k` 时，GET 会先于 SET 执行、读到旧值（真实的 Redis 客户端会这样 pipeline）。
现在的做法是：GET 也进同一条 FIFO 队列，但**不写 WAL**；纯读批次不等待提交窗口，直接放行，所以不引入额外延迟。

### Q: `SET` 的延迟为什么比 `GET` 高这么多？

因为 `SET` 的 `+OK` 是在 **`write` + `fdatasync` 完成之后**才回复的，这是持久化语义的代价。想降低写延迟，方向是组提交（已有 `kCommitWindow` / `kMaxBatchSize`）、SSD、或改成分段 fsync。

### Q: 为什么 `-P` 加大后延迟反而变高？

`-P 16` 时客户端会一次发 16 条不等回复，测的是**吞吐**；单条延迟自然被排队放大。要测真实延迟请用 `-P 1`。

### Q: `redis-benchmark` 报 `WARNING: Could not fetch server CONFIG`

服务端未实现 `CONFIG` 命令，回的是 `-ERR unknown command`。**无害**，不影响压测结果。想消掉可以给 `CONFIG` 加个空数组回复（`*0\r\n`）。

### Q: 压测时 QPS 上不去，怎么定位？

按顺序排查：

1. **日志级别**：是不是没设 `SPDLOG_LEVEL=err`？（影响可达 20 倍）
2. **构建类型**：是不是 Debug？
3. **客户端是否瓶颈**：`redis-benchmark` 单线程。换 `--threads 4` 或 memtier，同时看客户端 CPU 是否 100%。
4. **服务端是否单线程**：`lightkv_kv_server 1` 只有一个 IO 线程，多核用不上。
5. **磁盘**：纯 SET 场景看 `iostat -x 1` 的 `%util`。
6. **火焰图**：看最宽的条落在哪。

### Q: 结果每次都不一样怎么办？

正常。影响因素：CPU 频率缩放、其他进程争用、NUMA、磁盘缓存状态。
**每个配置跑 3 次取中位数**，并且对比时保证构建类型、线程数、日志级别完全一致。

### Q: 功能改动后先跑什么？

```bash
./build/lightkv_server_e2e_test      # 或 ctest -R server_e2e
```

它覆盖 RESP 语义、大小写、二进制安全、未知命令不断连接、inline、半包、pipeline 顺序、重启后 WAL 恢复、QUIT 关闭连接。跑通再压测，能省掉大量误判。
