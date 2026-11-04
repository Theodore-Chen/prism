# scripts/ — 按生命周期分层

| 目录 | 职责 | 内容 |
|------|------|------|
| `gate/` | **活跃**的硬件门禁与周期生成器 | `r0_gate.sh`（G0-G4 一键门禁）、`rebaseline.sh`（基线重采一条龙）、`verify.sh`（T16/M2 18 项一键验收 + `--soak N` 稳定性；根部 `./verify.sh` 为其薄壳）、`collect_mlx5_baseline.sh`（T16 core 侧基准一站式采集）、`compare_report.sh`（T16 生成 docs/report.md）、`collect_datapath.sh`（T18 四臂数据面基准：prism 限速梯 / core 钉同几何 / 线速表 / RX 时延分布，配套 `datapath_aggregate.py` 配对聚合）、`collect_latency.sh`（T11 延迟基线三阶段采集，配套 `latency_aggregate.py` 聚合/裁决）、`t09_verify.sh`（fresh selftest=1 周期生成器）、`r0_trace_check.sh`（tracepoint 独立诊断） |
| `baseline/` | 行为基线：数据 + 归一化 + 对比 | `baseline_dmesg_{0,1}.txt`、`normalize.sh`、`diff_baseline.sh`（详见其 README） |
| `test/` | 宿主机测试（无需 root/硬件） | `fmt_equiv_check.c`（编码字节等价证明）、`latency_aggr_test.py`（延迟聚合器合成 trace 单测） |
| `archive/` | **已完成任务**的一次性脚本 | t06a/t06b/t07/t08/t10_verify、t10prep_eqc_probe、t18_autopsy_burst——内部路径按当时仓库布局书写，不随重组更新，不可直接重跑 |

约定：脚本入口一律从仓库根调用（`sudo bash scripts/gate/<name>.sh`），黑匣子日志落 `/var/tmp`；新增上机门禁进 `gate/`，任务完结后的验证脚本移入 `archive/` 并加归档横幅。
