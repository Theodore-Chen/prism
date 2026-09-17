# mlx5_prism — ConnectX-4 Lx 控制面驱动（从零实现）

从零实现的 Mellanox ConnectX-4 Lx 控制面内核驱动 + 固件行为档案：PCIe 探测、CMDQ 命令引擎、9 步固件引导、EQ 事件与 MSI-X 中断、PHY 诊断（进行中）。定位是 mlx5_core 底三层的**最小可读重写**——每个结论都经硬件实证、与 mlx5_core 对照验证，固件的真实行为（而非文档宣称的行为）作为一等公民记录在代码里。

状态：T01-T09 与 R0 重构冲刺完成；EQ 事件与中断已点亮（eqc 编码翻案，见 progress.md T10-prep）。路线图与任务库见 `task.json`。

## 快速开始

```bash
make                                    # 构建（根 Makefile 转发 src/，默认钉死内核同款 gcc-11）
sudo modprobe -r mlx5_ib mlx5_core      # 与生产驱动互斥
sudo insmod src/mlx5_prism.ko selftest=1
dmesg | grep mlx5_prism                 # 一步一行的引导剧本
sudo rmmod mlx5_prism
sudo modprobe mlx5_core                 # 恢复生产驱动

sudo bash scripts/gate/r0_gate.sh       # 硬件门禁一键跑（G0-G4）
```

## 目录地图

| 位置 | 内容 |
|------|------|
| `src/` | 模块源码（7 .c + 4 .h）与 obj-m 清单；构建产物也只落在这里 |
| `docs/` | `DESIGN.md`（设计 v2.1：四层架构/状态机/怪癖档案/EQ 设计）、`READING.md`（~600 行协议脊柱深读指南） |
| `scripts/gate/` | 活跃硬件门禁（r0_gate / rebaseline / collect_latency / t09_verify / r0_trace_check） |
| `scripts/baseline/` | 行为基线：dmesg 模板 + 归一化 + 对比门禁 |
| `scripts/test/`、`scripts/archive/` | 宿主机测试；已完成任务的一次性脚本（见 `scripts/README.md`） |
| `progress.md`、`task.json` | 每任务实证记录 + 历史问题归档；任务库 |

## 深入阅读

- **第一次读这个仓库** → `docs/READING.md`：沿 ~600 行协议脊柱读完就能复述主机如何命令一块 ConnectX-4 Lx。
- **设计与架构决策** → `docs/DESIGN.md`。
- **踩坑史与翻案记录**（eqc 编码翻案、两次死机复盘、基线重采事故……）→ `progress.md` 历史问题归档。

硬件环境：ConnectX-4 Lx `15b3:1015` 双口 25GbE，FW 14.32.1908，内核 5.15.0-191-generic。
