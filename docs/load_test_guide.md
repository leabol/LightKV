# LightKV 压测指南

> 本文档是一份**可复现的压测流程**，涵盖测试目的、指令、结果解读和火焰图分析。
> 每次对系统做了重要改动后，按此流程跑一遍，对比数据即可判断性能是提升还是回退。

---

## 目录

1. [环境准备](#1-环境准备)
2. [测试工具说明](#2-测试工具说明)
3. [流程概览](#3-流程概览)
4. [测试 1：纯 GET 吞吐 & 延迟](#4-测试-1纯-get-吞吐--延迟)
5. [测试 2：纯 SET 吞吐 & 延迟](#5-测试-2纯-set-吞吐--延迟)
6. [测试 3：混合读写吞吐](#6-测试-3混合读写吞吐)
7. [测试 4：连接风暴压力](#7-测试-4连接风暴压力)
8. [火焰图采样与分析](#8-火焰图采样与分析)
9. [结果对比模板](#9-结果对比模板)
10. [常见问题](#10-常见问题)

---

## 1. 环境准备

### 1.1 确保 Debug 编译

```bash
# 配置 Debug 模式（已包含 -g 调试符号）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# 编译
cmake --build build -j4
```

> **为什么用 Debug？** Debug 模式包含完整的调试符号，`perf` 火焰图能正确解析函数名。Release 模式开启 `-O2`/`-O3` 后内联严重，火焰图难以阅读。

### 1.2 检查 perf 权限

```bash
cat /proc/sys/kernel/perf_event_paranoid
```

返回值含义：

| 值 | 含义 | 能否采样 |
|----|------|---------|
| -1 | 无限制 | ✅ |
| 0 | 允许几乎所有事件 | ✅ |
| 1 | 禁止 CPU 事件访问（`perf stat -e cycles` 不可用），但 **process-level 采样可用** | ⚠️ 够用 |
| 2 | 禁止内核 profiling | ❌ |
| 3 | 禁止所有非 root 性能事件 | ❌ |

**压测需要至少 paranoid ≤ 1**。如果当前值 > 1，需要 root 权限调低：

```bash
# 临时生效（重启后恢复）
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# 永久生效
echo "kernel.perf_event_paranoid = 1" | sudo tee /etc/sysctl.d/99-perf.conf
sudo sysctl -p
```

### 1.3 安装 FlameGraph 工具

火焰图由 Brendan Gregg 的工具生成，需要从 GitHub 克隆：

```bash
git clone --depth=1 https://github.com/brendangregg/FlameGraph.git /path/to/FlameGraph
```

后续会用到其中的两个脚本：
- `stackcollapse-perf.pl` — 将 `perf script` 的输出折叠成单行栈轨迹
- `flamegraph.pl` — 将折叠后的数据渲染成 SVG

### 1.4 启动服务器

```bash
# 启动，端口默认 8990
./build/lightkv_kv_server 8990

# 验证是否在监听
ss -tlnp | grep 8990
```

> 服务器日志级别已在 `main.cpp` 中设为 `spdlog::level::off`，压测期间日志无干扰。

---

## 2. 测试工具说明

项目中有三个测试可执行文件，编译后在 `build/` 下：

| 工具 | 文件 | 用途 | 适合场景 |
|------|------|------|---------|
| `lightkv_load_test_client` | `tests/load_test_client.cpp` | **预热 + GET 压测**，带延迟百分位统计 | 只读 QPS 基准 |
| `lightkv_rbench_client` | `tests/rbench_client.cpp` | **多命令混合压测**（GET/SET/DEL），类似 redis-benchmark | 读写混合基准 |
| `lightkv_conn_stress_test` | `tests/conn_stress_test.cpp` | **连接风暴测试**，大量并发连接建立 + 保持 | 连接数上限验证 |

---

## 3. 流程概览

一次完整的压测按以下顺序执行，每个测试依赖前一个的准备数据：

```
① 启动服务器
    │
② 测试 1：纯 GET 压测
    │  ├─ warmup (SET 20 万条 key)
    │  └─ GET 40 万次 (8 线程 × 5 万)
    │
③ 测试 2：纯 SET 压测
    │  └─ SET 20 万次 (16 并发)
    │
④ 测试 3：混合读写压测
    │  └─ 随机 GET/SET/DEL 20 万次 (16 并发)
    │
⑤ 测试 4：连接风暴测试
    │  ├─ 5K 连接 → 保持 5s → 释放
    │  └─ 10K 连接 → 保持 5s → 释放
    │
⑥ 火焰图采样
    │  ├─ perf record + GET 压测 (纯读热点)
    │  └─ perf record + 混合压测 (读写热点 + 锁开销)
    │
⑦ 整理数据 → 写入报告
```

> **注意**：测试 ② 的 warmup 会清空并重新写入 20 万条 key，后续测试无需重复 warmup。
> 如果服务器重启过，需要在测试 ③④ 之前重新做一次 warmup。

---

## 4. 测试 1：纯 GET 吞吐 & 延迟

### 4.1 目的

- 测量服务器**只读**场景下的极限 QPS
- 获取延迟分布（P50/P95/P99），判断是否存在长尾
- 验证多线程 epoll 是否能充分利用多核

### 4.2 指令

```bash
# 含义：
#   --warmup-keys 200000   先 SET 20 万条 key（单线程串行写入）
#   --threads 8            用 8 个并发线程发请求
#   --requests 50000       每线程发 5 万个请求（总计 40 万）
#   --value-size 128       value 长度 128 字节

./build/lightkv_load_test_client \
  --warmup-keys 200000 \
  --threads 8 \
  --requests 50000 \
  --value-size 128
```

### 4.3 示例输出

```
warmup ok=200000
done: total=400000 ok=400000 fail=0
      elapsed_s=1.352
      qps=295884.20
      p50_us=25  p95_us=28  p99_us=34
```

### 4.4 关注重点

| 指标 | 关注点 |
|------|--------|
| **`fail`** | 必须为 0。任何失败都说明有 bug（协议解析错误、连接断开等） |
| **`qps`** | 绝对值越高越好。改动前后对比时，**下降超过 5% 需要警惕** |
| **`p50_us`** | 基准延迟，代表大多数请求的体验。目标：< 50μs |
| **`p95_us`** | 尾部延迟，代表系统在负载下的表现。目标：< 100μs |
| **`p99_us`** | 长尾延迟，可能暴露锁竞争或调度抖动。目标：< 500μs |
| **`elapsed_s`** | 总耗时，用来复核 QPS 计算是否合理（QPS ≈ total / elapsed） |

### 4.5 可调参数

```bash
# 单线程压测（测基础延迟，无并发竞争）
./build/lightkv_load_test_client --threads 1 --requests 10000

# 高并发 + 小 value（测最大 QPS）
./build/lightkv_load_test_client --threads 16 --requests 100000 --value-size 16

# 大 value（测带宽和内存分配）
./build/lightkv_load_test_client --threads 8 --requests 10000 --value-size 4096
```

---

## 5. 测试 2：纯 SET 吞吐 & 延迟

### 5.1 目的

- 测量服务器**写入**场景的极限 QPS
- 验证锁（`std::mutex`）是否正常工作，有无死锁或竞争
- 写操作涉及哈希表插入/扩容，压力与读完全不同

### 5.2 指令

```bash
# 含义：
#   -t set                只执行 SET 命令
#   -n 200000             总请求量 20 万
#   -c 16                 16 个并发连接
#   -r 500000             key 空间 50 万（SET 会写新 key）
#   -d 128                value 长度 128 字节
#   --sequential          顺序 key（k0, k1, k2...），避免哈希表频繁 rehash

./build/lightkv_rbench_client \
  -t set -n 200000 -c 16 -r 500000 -d 128 --sequential
```

### 5.3 示例输出

```
done total=200000 ok=200000 fail=0
      connect_fail=0 io_fail=0 parse_fail=0
      elapsed_s=0.685  qps=291951
      p50_us=44  p95_us=94  p99_us=127
```

### 5.4 关注重点

| 指标 | 关注点 |
|------|--------|
| **`fail=0`** | 必须 0 失败。SET 不应该有任何 "key not found" 类错误 |
| **`connect_fail` / `io_fail` / `parse_fail`** | 任何非零值都表示系统级问题（服务器 crash、连接断开、协议错乱） |
| **qps** | 通常比 GET 略低（因为锁开销）。如果明显低于 GET 的一半，说明锁竞争严重 |
| **延迟** | SET 延迟通常比 GET 高（哈希表插入可能需要 rehash）。关注 P99 是否在可接受范围 |

### 5.5 为什么一定要测 SET？

多线程写入 `unordered_map` 是一个**经典的数据竞争场景**。如果没有锁保护，SET 压测必然导致：
- **Segmentation fault**（哈希表内部指针被并发修改）
- 内存损坏（丢失 key/value 数据）
- 进程无响应

所以 **SET 压测本质上是并发正确性的验证**——如果 SET 能跑通且 0 失败，说明锁加对了。

---

## 6. 测试 3：混合读写吞吐

### 6.1 目的

- 模拟真实 workload（读多写少或读写均衡）
- 观察读操作是否被写锁拖累（`std::mutex` 下读不能与写并发）
- 测试 DEL 操作的错误处理路径

### 6.2 指令

```bash
# 含义：
#   -t get,set,del        随机三种命令
#   -n 200000             总请求量 20 万
#   -c 16                 16 个并发连接
#   -r 500000             key 空间 50 万（GET/DEL 约有 60% 命中率）
#   -d 128                value 长度 128 字节

./build/lightkv_rbench_client \
  -t get,set,del -n 200000 -c 16 -r 500000 -d 128
```

### 6.3 示例输出

```
done total=200000 ok=121366 fail=78634
      connect_fail=0 io_fail=0 parse_fail=0
      elapsed_s=0.481  qps=415737
      p50_us=31  p95_us=63  p99_us=113
```

### 6.4 关注重点

| 指标 | 关注点 |
|------|--------|
| **`fail` ≠ 0** | 这里的 fail 大多是 **语义错误**（GET 不存在的 key、DEL 不存在的 key），不是系统错误。因为 `-r 500000` 的 key 空间远大于 warmup 的 20 万，约 60% 的随机 key 不存在 |
| **如何区分语义 fail 和系统 fail？** | 看 `connect_fail` / `io_fail` / `parse_fail`。这三个是 **系统级错误**，必须为 0。而 `fail - io_fail - parse_fail - connect_fail` 得到的是语义错误 |
| **qps** | 混合负载 QPS 通常高于纯读，因为读多写少时锁争用不大 + 更小的请求处理路径。但如果写比例高，QPS 会下降 |
| **p99 是否恶化** | 混合负载的 P99 通常比纯 GET 高。如果 P99 飙升（> 500μs），说明锁竞争严重 |

### 6.5 调参技巧

```bash
# 读多写少 (80% GET, 20% SET)
./build/lightkv_rbench_client -t get,get,get,get,set -n 200000 -c 16

# 写多读少 (70% SET, 30% GET)
./build/lightkv_rbench_client -t set,set,set,set,set,set,set,get,get,get -n 200000 -c 16
```

---

## 7. 测试 4：连接风暴压力

### 7.1 目的

- 测试服务器能同时处理多少 TCP 连接
- 验证连接建立/关闭流程没有资源泄漏（fd 未关闭、内存未释放）
- 检测 listen backlog 是否够用

### 7.2 指令

```bash
# 基础：5K 连接，50 并发建立，保持 5 秒后释放
./build/lightkv_conn_stress_test -n 5000 -c 50 --idle

# 加大：1 万连接
./build/lightkv_conn_stress_test -n 10000 -c 50 --idle

# 探索上限：如果失败，可能是 somaxconn 或 ulimit -n 限制
./build/lightkv_conn_stress_test -n 20000 -c 100 --idle
```

### 7.3 示例输出

```
success=10000 failed=0 elapsed_s=0.553 conn/s=18086
fds_held=10000
holding 10000 connections for 5 seconds...
releasing connections.
```

### 7.4 关注重点

| 指标 | 关注点 |
|------|--------|
| **`success` / `failed`** | failed > 0 说明服务器拒绝了连接。常见原因见下方 |
| **`conn/s`** | 连接建立速率，受到 `somaxconn` 和 CPU 限制 |
| **`fds_held`** | 保持连接期间服务器持有的 fd 数，应与 success 相等。如果服务器 fd 泄漏，这个数会持续增长 |
| **保持期间服务器是否稳定** | 服务器在 5 秒保持期内不应 crash，也不应消耗异常 CPU |

### 7.5 连接失败原因排查

```bash
# 查看 listen backlog 上限
cat /proc/sys/net/core/somaxconn

# 查看进程可打开文件数
cat /proc/$(pgrep lightkv_kv_serv)/limits | grep "open files"

# 查看当前端口上的连接数
ss -tn | grep 8990 | wc -l

# 查看 TIME_WAIT 堆积
ss -tn state time-wait | wc -l
```

如果 `Connection refused`，三个可能原因：

| 原因 | 现象 | 解决 |
|------|------|------|
| `somaxconn` 太小 | 大量 connect 立即被拒 | `sysctl -w net.core.somaxconn=65535` |
| `ulimit -n` 太小 | 连接数到上限后 fail | `ulimit -n 100000` 或在 systemd 里配 `LimitNOFILE` |
| 服务器进程崩了 | 所有连接都被拒 | 检查服务器日志；用 `ps aux` 确认进程存活 |

---

## 8. 火焰图采样与分析

### 8.1 目的

火焰图回答一个问题：**CPU 时间花在哪里了？**

- 宽条 = 热点函数 = 优化机会
- 从上往下读 = 调用链
- 鼠标悬停在 SVG 的方块上可查看函数名和占比

### 8.2 采样原则

| 原则 | 说明 |
|------|------|
| **在负载下采样** | 空载服务器的火焰图只有 `epoll_wait`，无意义。火焰图必须配合压测客户端一起跑 |
| **采样频率 99 Hz** | `-F 99` 是 Brendan Gregg 推荐的标准频率，既够采样数又不过度干扰 |
| **采 10-15 秒** | 太短样本不够，太长可能采集到 idle 阶段 |
| **分别采读和写** | 纯 GET 和混合负载的火焰图可能完全不同，分开采才能定位各自瓶颈 |

### 8.3 纯 GET 负载采样

**步骤**：

```bash
# 1. 确保服务器已启动，warmup 已完成
#    服务器 PID 用以下命令获取：
SERVER_PID=$(pgrep lightkv_kv_serv)
echo "Server PID: $SERVER_PID"

# 2. 启动 perf 采样（后台运行 15 秒）
perf record -F 99 -p $SERVER_PID -g -o /tmp/perf-get.data -- sleep 15 &

# 3. 立刻运行压测客户端（在 perf 采样的 15 秒窗口内跑完）
./build/lightkv_load_test_client \
  --warmup-keys 0 --threads 8 --requests 50000 --value-size 128

# 4. 等待 perf 采样结束
wait

# 5. 生成火焰图
perf script -i /tmp/perf-get.data \
  | /path/to/FlameGraph/stackcollapse-perf.pl \
  | /path/to/FlameGraph/flamegraph.pl \
  > docs/flamegraph_get.svg
```

**预期火焰图结构**（自上而下）：

```
┌─────────────────────────────────────────────┐
│  EventLoop::loop()                           │  ← 事件循环入口
│  ├─ EpollPoller::poll()                      │  ← epoll_wait (idle 时为最宽条)
│  └─ Channel::handleEvent()                   │  ← 事件分发
│     └─ TcpConnection::handleRead()           │  ← TCP 读事件
│        ├─ ::recv()                           │  ← 内核网络栈
│        └─ KvServer::onMessage()              │  ← 应用层处理
│           ├─ parserRequest()                 │  ← 协议解析
│           ├─ Dispatcher::dispatch()          │  ← 路由到 handler
│           │  └─ handleGET()                  │  ← 只读 map find()
│           ├─ encodeResponse()                │  ← 序列化
│           └─ conn->send()                    │  ← 写回客户端
└─────────────────────────────────────────────┘
```

**关注点**：

- `handleGET()` 的宽度 → 只读 QPS 瓶颈是否在 map lookup？
- `::recv()` / `::send()` 的宽度 → 是否受网络栈限制？
- `parserRequest()` 的宽度 → 协议解析有没有不必要的拷贝？

### 8.4 混合负载采样

```bash
# 1. 启动 perf 采样
perf record -F 99 -p $SERVER_PID -g -o /tmp/perf-mixed.data -- sleep 15 &

# 2. 运行混合压测
./build/lightkv_rbench_client \
  -t get,set -n 200000 -c 16 -r 500000 -d 128

# 3. 等待，生成火焰图
wait
perf script -i /tmp/perf-mixed.data \
  | /path/to/FlameGraph/stackcollapse-perf.pl \
  | /path/to/FlameGraph/flamegraph.pl \
  > docs/flamegraph_mixed.svg
```

**混合负载火焰图与纯 GET 的差异**：

- ✅ 会出现 `handleSET()` 的调用栈
- ✅ 可能出现 `__pthread_mutex_lock()` 或 `std::mutex::lock()` 的宽条——**锁竞争越宽，性能越差**
- ✅ 如果锁竞争严重，`handleGET()` 的条会变窄（因为被锁阻塞了）

### 8.5 火焰图对比方法

**方法 1：肉眼对比**——打开两张 SVG 并列看，热点函数的宽窄变化一目了然。

**方法 2：diff 火焰图**——用 FlameGraph 自带的 `difffolded.pl` 生成差异图：

```bash
# 生成两份折叠数据
perf script -i /tmp/perf-get.data | /path/to/FlameGraph/stackcollapse-perf.pl > /tmp/get.folded
perf script -i /tmp/perf-mixed.data | /path/to/FlameGraph/stackcollapse-perf.pl > /tmp/mixed.folded

# 生成 diff 火焰图（红色 = 增加，蓝色 = 减少）
/path/to/FlameGraph/difffolded.pl /tmp/get.folded /tmp/mixed.folded \
  | /path/to/FlameGraph/flamegraph.pl \
  > docs/flamegraph_diff.svg
```

### 8.6 注意事项

- **perf 只能在 Linux 上用**，macOS 不行
- **`-fno-omit-frame-pointer`**：如果编译时加了 `-fomit-frame-pointer`（常见于 Release 模式），perf 采到的栈是不完整的。Debug 模式默认不省略 frame pointer
- **采样数**：`-F 99 -- sleep 10` 大约采 990 个样本。如果火焰图太稀疏（看到的条很少），可以加长时间或提高频率
- **短函数可能被吞**：火焰图的每个方块宽到至少 1 像素，采样数太少时短函数不显示

---

## 9. 结果对比模板

每次改动后，按下面模板记录结果：

```markdown
## 改动说明

<!-- 一句话描述改了啥，例如："给 storage_ 加了 std::mutex" -->

## 环境

- 日期: 2026-05-21
- 编译: Debug / Release
- 服务器线程数: 8
- 内核: `uname -r`

## 结果

| 测试 | QPS | P50(μs) | P95(μs) | P99(μs) | fail | 对比基线 |
|------|:---:|:-------:|:-------:|:-------:|:----:|:--------:|
| 纯 GET 40万 | 295,962 | 24 | 29 | 39 | 0 | +74% |
| 纯 SET 20万 | 291,951 | 44 | 94 | 127 | 0 | 新增 |
| 混合 20万 | 415,737 | 31 | 63 | 113 | 0(系统) | 新增 |
| 5K 连接 | 15,600/s | — | — | — | 0 | — |
| 10K 连接 | 18,086/s | — | — | — | 0 | — |

## 火焰图观察

<!-- 新火焰图和旧图比，热点函数有什么变化？ -->

## 结论

<!-- 改动带来了提升还是回退？是否达到预期？ -->
```

**基线数据**（第一次压测的结果）保存在 `docs/load_test_report.md` 中。后续每次对比时，把最新结果填入模板即可。

---

## 10. 常见问题

### Q: 为什么纯 GET 不需要加锁？

C++ 标准规定：**多个线程同时读取 `std::unordered_map` 的 const 成员函数（如 `find()`）是安全的**，只要没有其他线程同时写入非 const 成员函数（如 `operator[]`、`insert()`、`erase()`）。所以纯 GET 场景不需要锁。

但如果有混合读写，GET 也必须加锁（或使用读写锁），否则 `find()` 可能读到被写入操作破坏的中间状态。

### Q: 为什么 warmup 要用 SET 20 万条？

- 让哈希表 reach 到一个合理的大小，避免压测时频繁 rehash 影响结果
- 确保后续 GET 压测有足够多的 key 可以命中（如果全 miss，测的是错误路径而不是正常路径）

### Q: 服务器在高压下 crash 了怎么办？

1. 先检查是不是 `storage_` 没有加锁（最常见的 crash 原因）
2. 运行 `dmesg | tail -20` 看内核日志（segfault 会有记录）
3. 用 gdb 复现：`gdb ./build/lightkv_kv_server core`（如果有 core dump）
4. 减少并发数逐步加压，找到 crash 的阈值

### Q: 测试结果每次不一样怎么办？

正常。性能测试受以下因素影响：

- 系统其他进程的 CPU/IO 争用
- CPU 频率缩放（turbo boost / thermal throttling）
- 内存 NUMA 亲和性

**建议**：每个配置跑 **3 次取中位数**，而不是只跑一次。

### Q: 可以用 Release 模式压测吗？

可以，但要清楚代价：

- Release 编译的服务器性能更高（`-O2`/`-O3`）
- 但火焰图的函数名可能不全（内联和尾调用优化导致调用栈丢失）
- 如果只是对比 QPS 数字（不做火焰图分析），Release 模式更真实

建议策略：**Debug 模式做火焰图分析**，**Release 模式做最终 QPS 数字验收**。
