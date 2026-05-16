# rbench-like 正式再测记录（顺序 key）

日期：2026-05-16

## 测试环境

- 平台：Linux
- 仓库：`/home/lea/develop/LightKV`
- 服务端：`./build/lightkv_kv_server`
- 压测客户端：`./build/lightkv_rbench_client`

## 正式测试方式

这次使用顺序 key 模式，避免随机 key 命中率波动：

- 先执行顺序 `SET` 预热，确保 `keyspace` 覆盖完整；
- 再执行顺序 `GET` 主测，得到稳定命中率和延迟。

客户端参数新增了 `--sequential`，表示使用 `k0, k1, ...` 的固定顺序 key。

## 结果

### 1) 顺序 `SET` 预热

命令：

```bash
./build/lightkv_rbench_client -t set -n 20000 -c 20 -r 20000 -d 32 --sequential --host 127.0.0.1 --port 8990
```

结果：

```text
done total=20000 ok=20000 fail=0 connect_fail=0 io_fail=0 parse_fail=0 elapsed_s=0.121872 qps=164107 p50_us=115 p95_us=138 p99_us=180
```

### 2) 顺序 `GET` 主测

命令：

```bash
./build/lightkv_rbench_client -t get -n 20000 -c 20 -r 20000 -d 32 --sequential --host 127.0.0.1 --port 8990
```

结果：

```text
done total=20000 ok=20000 fail=0 connect_fail=0 io_fail=0 parse_fail=0 elapsed_s=0.123287 qps=162224 p50_us=117 p95_us=144 p99_us=167
```

## 结论

- 顺序 key 模式下，`SET` 和 `GET` 都达到了 `100%` 命中。
- 这次结果更适合作为“正式”压测记录，因为不存在随机未命中干扰。
- 读吞吐约 `162k qps`，延迟大致为 `p50=117us / p95=144us / p99=167us`。

## 备注

- 这组数据在当前单线程 `EventLoop` + 非阻塞网络模型下运行，适合做功能链路和轻量性能基准。
- 如果要进一步提高吞吐，可以考虑多 `EventLoop` / 多线程服务端模型。

---

# TCP 连接压力测试记录

日期：2026-05-16

## 测试环境

- 平台：Linux
- 工具：`./build/lightkv_conn_stress_test`（新增连接层压测工具）
- 服务端：`./build/lightkv_kv_server`

## 测试目标

验证网络层对大量并发连接的处理能力，独立于应用协议层。

## 测试结果

### 1) 1000 连接

命令：

```bash
./build/lightkv_conn_stress_test -n 1000 -c 100 --host 127.0.0.1 --port 8990 --idle
```

结果：

```text
done
  success=1000 failed=0 elapsed_s=0.0918957 conn/s=10881.9
  fds_held=1000
```

### 2) 5000 连接

命令：

```bash
./build/lightkv_conn_stress_test -n 5000 -c 200 --host 127.0.0.1 --port 8990 --idle
```

结果：

```text
done
  success=5000 failed=0 elapsed_s=0.281449 conn/s=17765.2
  fds_held=5000
```

### 3) 10000 连接

命令：

```bash
./build/lightkv_conn_stress_test -n 10000 -c 500 --host 127.0.0.1 --port 8990 --idle
```

结果：

```text
done
  success=10000 failed=0 elapsed_s=0.386342 conn/s=25883.8
  fds_held=10000
```

## 结论

- 网络层能稳定处理 **10000 并发 idle 连接**，无 failed。
- 连接建立速度随并发增加而提升：1k@10.8k conn/s → 10k@25.8k conn/s。
- 文件描述符管理正常，能按预期持有 10k 连接 5 秒。
- 当前单线程 `EventLoop` 的 `accept` 和 `select/epoll` 处理能力可应对 10k+ 连接量。

## 说明

- 这些连接在 `--idle` 模式下只是建立并持久化，不发送业务请求。
- 如果后续要加上业务流量，网络层仍有进一步优化空间（比如连接复用、批量处理、多线程分流等）。

---

# rbench-like 正式再测记录

日期：2026-05-16

## 测试环境

- 平台：Linux
- 仓库：`/home/lea/develop/LightKV`
- 服务端：`./build/lightkv_kv_server`
- 压测客户端：`./build/lightkv_rbench_client`

## 测试方法

按“先预热、再读测”的方式重测：

1. 先用 `SET` 预热 keyspace；
2. 再用 `GET` 测读性能；
3. 最后再补一轮更充分的 `SET` 预热，得到更稳定的 `GET` 结果。

## 结果

### 1) `SET` 预热

命令：

```bash
./build/lightkv_rbench_client -t set -n 20000 -c 20 -r 20000 -d 32 --host 127.0.0.1 --port 8990
```

结果：

```text
done total=20000 ok=20000 fail=0 connect_fail=0 io_fail=0 parse_fail=0 elapsed_s=0.19897 qps=100518 p50_us=172 p95_us=266 p99_us=311
```

### 2) 预热后的 `GET`

命令：

```bash
./build/lightkv_rbench_client -t get -n 20000 -c 20 -r 20000 -d 32 --host 127.0.0.1 --port 8990
```

结果：

```text
done total=20000 ok=12615 fail=7385 connect_fail=0 io_fail=0 parse_fail=0 elapsed_s=0.175716 qps=113820 p50_us=180 p95_us=188 p99_us=205
```

说明：这里的 `fail` 主要是随机 key 未命中，不是连接或协议错误。

### 3) 更充分的 `SET` 预热

命令：

```bash
./build/lightkv_rbench_client -t set -n 100000 -c 20 -r 20000 -d 32 --host 127.0.0.1 --port 8990
```

结果：

```text
done total=100000 ok=100000 fail=0 connect_fail=0 io_fail=0 parse_fail=0 elapsed_s=0.828152 qps=120751 p50_us=161 p95_us=180 p99_us=201
```

### 4) 正式 `GET`

命令：

```bash
./build/lightkv_rbench_client -t get -n 20000 -c 20 -r 20000 -d 32 --host 127.0.0.1 --port 8990
```

结果：

```text
done total=20000 ok=19960 fail=40 connect_fail=0 io_fail=0 parse_fail=0 elapsed_s=0.160885 qps=124312 p50_us=155 p95_us=171 p99_us=190
```

## 结论

- `rbench` 工具已修复并可稳定输出统计结果。
- 服务端协议链路正常，`SET/GET` 都能稳定执行。
- 在充分预热后，`GET` 的大多数请求命中，吞吐约为 `124k qps`，延迟大致在 `p50=155us / p95=171us / p99=190us`。

## 说明

- 如果要继续追求更高命中率，可以把 `SET` 预热次数继续加大，或改成顺序 key 生成。
- 当前 `GET` 里的少量 `fail` 属于随机 key 未命中的业务结果，不代表网络或编码错误。

---

# Server Flamegraph 记录

日期：2026-05-16

## 采集对象

这次火焰图采集的是 **server 端进程** `lightkv_kv_server`，不是客户端。

## 结果文件

- SVG：`docs/lightkv_server_perf.svg`

## 采集方式

```bash
./build/lightkv_kv_server > /tmp/lightkv_server.log 2>&1 &
sudo perf record -p <server_pid> -F 99 -g -o /tmp/server_perf.data -- sleep 4
sudo perf script -i /tmp/server_perf.data | /tmp/FlameGraph/stackcollapse-perf.pl > /tmp/server_perf_folded.txt
/tmp/FlameGraph/flamegraph.pl /tmp/server_perf_folded.txt > /tmp/server_perf.svg
cp /tmp/server_perf.svg docs/lightkv_server_perf.svg
```

## 关键观察

- 栈顶是 `lightkv_kv_server`，说明火焰图已经落在服务端进程上。
- 主要热点集中在 `net::EventLoop::loop`、`net::Channel::handleEvent`、`net::TcpConnection::handleRead`、`server::KvServer::onMessage`。
- 内核侧仍然占很大比例，尤其是 `epoll_wait`、`recvfrom`、`sendto`、`tcp_sendmsg`、`tcp_recvmsg`、`ip_output`、`net_rx_action` 等。

## 结论

- 这次采的是 **server-side flamegraph**。
- 当前服务端的热点仍以网络 I/O 和 TCP/IP 内核栈为主。
- 应用层处理（协议解析、路由、存储访问）并不是主要 CPU 瓶颈。

