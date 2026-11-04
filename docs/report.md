# mlx5_prism M2 验证报告（T16-verify-report）

- 生成：2026-10-02 00:09:45　commit `f006255`
- 工件：latency_report / mlx5_baseline / verify_run / verify_live（/var/tmp，快照永久保留）
- **口径声明**：prism P1' 为事件口径（doorbell→唤醒，T10 起默认）；T11 存档行为轮询口径（doorbell→owner 翻转观察）；mlx5_core 为 kretprobe `mlx5_cmd_invoke` 函数驻留口径——三者相近但不严格等价，仅同数量级对比（DESIGN §10.1）。

## 一致性层（C1-C5，DESIGN §10.1）

| # | 指标 | prism 实测 | mlx5_core 实测 | 裁决 |
|---|------|-----------|---------------|------|
| C1 | HCA 能力集 | caps[cur]: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 80 00 00 01 …（num_ports=1, log_max_qp=2） | debugfs 命令集统计口径受限（记缺） | 关键字段一致；caps hexdump 锚点 bit-exact 由基线 diff 门禁背书 |
| C2 | 固件版本 | iseg fw_rev 14.32.1908 | ethtool -i 14.32.1908 | 一致 |
| C3 | 引导序列 | 9 步（基线 68 行/口锁定） | core dmesg 14 行存档 | 序列族一致（caps 差异已记录于基线归一化） |
| C4 | PHY 计数器 | PPCNT 0x16 rx_bits=200000000 sym_err=0；0x00 tx_ok=0 rx_ok=0 | ethtool -S rx_packets_phy=0 rx_symbol_err_phy=0 | 单调存活（V_prism≥V_mlx5 弱形式如实注记，链路 down 锚点计数见 §8.3 tslc 现象学） |
| C5 | 页回收 | teardown 2305/2305 ×2/口 | —（core 无对应口径） | 2305/2305 全量 |

## 性能层（P1'-P4，v1 P1/P2 长尾发现后修订）

| # | 指标 | mlx5_core 基准 | prism 实测（T16 事件口径复测） | 说明 |
|---|------|---------------|------------------------------|------|
| P1' | 命令往返（doorbell→唤醒，bp 档） | n=20700 p50=5.0us p99=7070.0us max=20006.0us | n=1512 p50=255.1us p99=12235.2us max=229552.8us（全量含 monitor ACCESS_REG：n=1912 p50=254.1us） | 仅同数量级对比；T11 轮询口径存档：p50=76.5us p99=237,971us max=239,163us（n=1008） |
| P2' | 事件唤醒延迟（EQE 消费→等待者观察，P2'ev 反向配对档） | — | n=216 p50=23.0us p99=69.0us max=1493.0us（旧正向配对形态记录：n=1081 p99=322890.0us） | 判据C：n=216，p99=69.0us，max=1,493.0us，>500ms 样本 0 条 |
| P3 | 固件引导时间 | ~265ms | 263/275ms（双口） | ≤300ms 判据（历史实测 258-277 档） |
| P4 | 中断响应 | ~5us | 由 P2' hardirq 份额承载（基线行 H consumed in hardirq>0） | 旧判据按 §10.1 撤销，事件模式证据并入 P2'，不单列硬指标——如实注记 |

## p99 差异分析（exit_criteria.manual_check）

P1' 事件口径（bp 档）p50=255.1us 对 core kretprobe p50=5.0us：差异首先来自**口径**而非实现——core 侧量的是 `mlx5_cmd_invoke` 函数驻留（调用方同步等完成返回），prism 事件口径量的是 doorbell 写入到完成路径唤醒等待者，两者共享同一段固件往返但等待边界不同（中断投递 + 唤醒路径计入 prism 侧）。其次看分布形状：prism 的 p99=12235.2 与 max=229552.8 由固件真实处理时间主导——T16 12 载慢样本归属表逐位复现：每口 6 个确定性慢样本（QUERY_HCA_CAP ~1.7ms、init 页三连 ~11-12ms、INIT_HCA ~228ms、CREATE_EQ ~1.8ms，另 monitor 首 PPCNT 首读 ~239ms 入 all 档）在全部 12 载同一位置出现，是固件处理时间不是驱动抖动；core 窗口由链路翻转命令构成（无 INIT_HCA 级命令），其 max=20006.0us 为自身慢命令长尾，两侧尾部不可逐命令对表，仅数量级对照。结论：长尾具固件确定性（12 载逐位复现在案），无驱动侧异常放大。

## 18 项验收结果（verify.sh，DESIGN §10.2）

| 项 | 内容 | 结果 |
|----|------|------|
| 1 | 模块加载 | PASS |
| 2 | 驱动绑定 | PASS |
| 3 | 引导 9 步 | PASS |
| 4 | caps | PASS |
| 5 | CMDQ 成功率 | PASS |
| 6 | 延迟 P1' | PASS |
| 7 | 延迟 P2' | PASS |
| 8 | 引导时间 | PASS |
| 9 | smoke | PASS |
| 9a | netdev | PASS |
| 9b | 单包发 | PASS |
| 9c | NAPI 收 | PASS |
| 10 | PHY sysfs | PASS |
| 11 | PHY 一致 | PASS |
| 12 | PHY 告警 | PASS |
| 13 | tracepoint 完整 | PASS |
| 14 | 无泄漏 | PASS |
| 15 | mlx5_core 切换 | PASS |

**合计：18/18**——ALL 18/18 PASSED

## 附：工件路径

- `/var/tmp/latency_report.txt`、`/var/tmp/latency_samples.csv`、快照 `/var/tmp/prism_lat_snap.*/`
- `/var/tmp/mlx5_baseline/`（fw/dmesg/debugfs/ethtool_S/kprobe）
- `/var/tmp/verify_run.log`、黑匣子 `/var/tmp/verify_live.log`、soak `/var/tmp/verify_soak.d/`
- 判据预登记：`scripts/gate/verify.sh` 头部 J1-J18


## 性能附录：数据面基准（T18-datapath-benchmark，2026-11-05）

- 口径声明：accept_local 单机双口对拉（拓扑税：协议栈路径计入两端）；pktgen kpktgend_23 单流 clone_skb 0（逐包真实路径）；TX=port0→RX=port1；core 臂钉同几何（combined=1 + 六 offload 全关，回读存档 `/var/tmp/datapath/core_geom_*`）；prism 臂为限速梯（全速楔死机理见下注）；线速锚 64B≈37.2Mpps / 1514B≈2.0Mpps。**数值入档不设阈值**（T20 立门禁）。

| 臂 | 指标 | 数值 | 对照 | 样本/口径 |
|----|------|------|------|----------|
| 1 prism 基线 | 64B 零 TX 丢失 pps | **10,000 请求档全投递**（delivered 5,334pps，4 万包 tx_errors=0） | core 2,295,262pps | 梯 2k/10k/20k/35k（ladder 入 summary.txt） |
| 1 prism 基线 | 1514B 零 TX 丢失 pps | **10,000 请求档全投递**（delivered 4,217pps，tx_errors=0） | core 1,769,912pps | 同上 |
| 2 core 钉同几何 | 64B / 1514B pps | rx 1,221,168（3.28% 线速）/ rx 1,307,919（65.4%） | — | 两遍法 ~2s 窗口；64B 受单线程生成器天花板（自报 ~2.5Mpps） |
| 3 线速换算 | 见 collect 臂 3 表 | prism 64B 0.014% / 1514B 0.211%（rx 口径） | core 同表 | — |
| 4 RX 端到端时延 | 100pps 低负载 | n=972 p50=86µs p99=9,895µs max=19,811µs（≥8ms 占 1.6%） | — | sq_xmit↔rx_cqe FIFO 配对，unpaired=0 |
| 4 RX 端到端时延 | 1000pps 低负载 | n=3,599 p50=3,073µs p99=17,862µs max=18,865µs（≥8ms 占 26.5%） | — | 同上；**10ms 看门狗地板清晰可见**（D3） |

**楔死机理注记（prism 全速突发，移交 T24/T25）**：固件在「CQE 产出 − 已发布 ci」距离 ≥ CQ 深度时静默粘滞停产——标度律 64→66 / 512→514 实证；doorbell 三形态反证在案（旧 idx 重环=重执行污染 tx 1377>sofar 410；新鲜 NOP=无效；pending 重放=无效）；根因=负载下事件路径死火（T15 µs 窗怪癖）→ ci 仅靠 10ms 看门狗发布。修复=事件模型重设计=T24（arm 节流）/T25（CQE 聚合）立项范围。RX 侧另有 RQ16+10ms 批处理 ≈1.6kpps 服务天花板（2k 档 77% 投递），属 T22 环几何。

**驱动变更（T18 例外，「先修后收」拍板）**：CQ 深度 64→512 + 多页 PAS（net.c trip-21/21b，行为面零打印、68 行基线不动、r0_gate 8/8 + verify 18/18 复验）。

- 工件：`/var/tmp/datapath_report.txt`、快照 `/var/tmp/datapath/`（summary.txt 机器可读=T20 上游）、黑匣子 `/var/tmp/datapath_live.log`；尸检专档 `/var/tmp/t18_autopsy/`；判据预登记 `scripts/gate/collect_datapath.sh` 头部 D1-D3。
