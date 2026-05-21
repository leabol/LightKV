# Load Test 分析报告

日期：2026-05-20

摘要：本次在本机环境对 `lightkv_kv_server` 做了短时高并发压测（使用 `lightkv_load_test_client` 与 `lightkv_conn_stress_test`），并与之前的 `rbench` 测试结果做了对比。此次测试基于 **one-loop-per-thread** 实现（每个线程绑定独立 EventLoop）。总体发现：在当前代码（one-loop-per-thread 改造）下，最大短时吞吐可达 ~169k qps，且延迟显著降低。

测试环境与方法：
- 服务端：`./build/lightkv_kv_server`（端口 8990，运行在测试机）
- 压测：`./build/lightkv_load_test_client`（本机客户端）与 `./build/lightkv_conn_stress_test`
- 本次重点采样参数：`--value-size 32`，`requests/thread=5000`，并在 threads=[1,2,4,8] 下跑短时基准；另用 conn_stress_test 做 1k/5k/10k 连接建立测试。

本次关键结果（摘要）：

- load_test_client（requests/thread=5000, value_size=32）
  - threads=1:  qps=67,024.82  p50=13us  p95=14us  p99=22us
  - threads=2:  qps=118,732.89 p50=18us  p95=19us  p99=21us
  - threads=4:  qps=156,331.54 p50=15us  p95=33us  p99=51us
  - threads=8:  qps=169,250.69 p50=36us  p95=77us  p99=99us

- 连接压力测试（idle）
  - 1000 conn (c=100):  success=1000 failed=0 elapsed_s=0.159864 conn/s=6,255.33
  - 5000 conn (c=200):  success=5000 failed=0 elapsed_s=0.269199 conn/s=18,573.6
  - 10000 conn (c=500): success=10000 failed=0 elapsed_s=0.464168 conn/s=21,543.9

与之前测试的对比（选取两个参考基线）：

基线 A — 之前 `rbench-like` 正式 `GET`（2026-05-16，文档中记录）：
- qps=124,312  p50=155us  p95=171us  p99=190us

基线 B — 之前顺序 key 的短时 `GET`（2026-05-16，顺序测试）：
- qps=162,224  p50=117us  p95=144us  p99=167us

以本次最佳观测（threads=8）与基线对比：
- 与基线 A 比较（124,312 → 169,251）：
  - 吞吐提升约 +36.2%（169,251 / 124,312 - 1）
  - p50 从 155us 降到 36us，降低约 76.8%
  - p95 从 171us 降到 77us，降低约 55.0%
  - p99 从 190us 降到 99us，降低约 47.9%

- 与基线 B 比较（162,224 → 169,251）：
  - 吞吐提升约 +4.3%
  - p50 从 117us 降到 36us，降低约 69.2%
  - p95 从 144us 降到 77us，降低約 46.5%
  - p99 从 167us 降到 99us，降低約 40.7%

分析与可能原因：
- 吞吐提升：相对于较旧的 `rbench` 正式基线（124k qps），本次观测到约 36% 的短时吞吐增长；与顺序 key 的高并发短测（162k qps）相比，提升较小（~4%）。说明总体实现对短时并发和请求调度有明显优化，但部分提升受限于测试硬件与内核网络栈。
- 延迟大幅下降：p50/p95/p99 均显著降低，表明单次请求的处理路径更短或者竞争/排队显著减少（可能因每线程單獨 `EventLoop` 减少锁竞争或队列切换开销）。
- 随并发增长延迟分布变宽：threads=8 时 p95/p99 明显上升，说明当客户端并发继续增加时，系统进入更高的内核调度与资源竞争阶段，导致尾延迟抬升。

局限性与建议：
- 当前测试为“单机”模式（客户端与服务端在同一台机器），可能高估峰值吞吐。建议在独立机器间做跨机压测以得到更接近真实生产的数字。
- 为了更稳定的比较，建议做多轮取中值/均值并导出 CSV；同时增加单轮请求数与运行时间以减少短时抖动。
- 已生成服务端火焰图（docs/lightkv_server_perf.svg），下一步可结合火焰图定位仍存的热点并针对性优化（例如减少系统调用、合并小写 send、优化解析路径等）。

结论：本次改造（one-loop-per-thread）带来了可观的延迟下降和吞吐提升，短时最大观测吞吐接近 170k qps；但要验证在真实部署下的稳定性与可扩展性，还需跨机、长时和多参数扫描测试。

文件：此分析保存在 `docs/load_test_analysis.md`（本文件）。如需我将关键数据导出为 CSV 或绘制吞吐/延迟图表，我可以继续处理。

## 原始输出 (Raw outputs)

load_test_client threads=1:

```text
done: total=5000 ok=5000 fail=0 elapsed_s=0.075 qps=67024.82 p50_us=13 p95_us=14 p99_us=22
```

load_test_client threads=2:

```text
done: total=10000 ok=10000 fail=0 elapsed_s=0.084 qps=118732.89 p50_us=18 p95_us=19 p99_us=21
```

load_test_client threads=4:

```text
done: total=20000 ok=20000 fail=0 elapsed_s=0.128 qps=156331.54 p50_us=15 p95_us=33 p99_us=51
```

load_test_client threads=8:

```text
done: total=40000 ok=40000 fail=0 elapsed_s=0.236 qps=169250.69 p50_us=36 p95_us=77 p99_us=99
```

连接压力测试 1k:

```text
success=1000 failed=0 elapsed_s=0.159864 conn/s=6255.33 fds_held=1000
```

连接压力测试 5k:

```text
success=5000 failed=0 elapsed_s=0.269199 conn/s=18573.6 fds_held=5000
```

连接压力测试 10k:

```text
success=10000 failed=0 elapsed_s=0.464168 conn/s=21543.9 fds_held=10000
```

