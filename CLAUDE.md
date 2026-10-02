# CLAUDE.md — mlx5_prism 项目指引

## 项目

实现 **mlx5_prism.ko**——ConnectX-4 Lx 智能网卡的控制面内核驱动。从零完成 PCIe 探测、CMDQ 引擎、7 步固件引导、PHY 诊断、硬件冒烟测试和**最小 netdev 骨架**（register_netdev + NAPI + ndo_start_xmit，覆盖 JD 关键词）。

完整技术设计见 **DESIGN.md**。先读 DESIGN.md，然后拆任务、写代码、逐个验证。

## 硬件环境

### 网卡

- **型号**: Mellanox ConnectX-4 Lx, `15b3:1015`, PCI `08:00.0`, 双口 25GbE
- **BAR0**: 32MB, UAR doorbell 寄存器

### GPU（分析 CUDA/RDMA 交互用）

- **型号**: NVIDIA RTX 3080 Ti (12GB)
- **驱动**: 580.126.09, CUDA 13.0

### RDMA 栈

- **libibverbs**: 39.0
- **rdma-core**: 已安装
- **mlx5_ib**: 已加载，RoCEv2 已激活
- **nvidia-peermem**: 模块已编译但加载失败（CX-4 Lx + GeForce 消费卡组合上 GPUDirect RDMA 不支持）

### 内核

- **运行内核**: `5.15.0-174-generic` (Ubuntu 22.04)
- **构建用 headers**: `/lib/modules/$(uname -r)/build` (5.15.0-174-generic)
- **内核源码**: `/home/zqc/code/linux` (自编译 7.0.0, mlx5_core 驱动源码 ~8000 行可供阅读)
- **内核配置**: `CONFIG_PCI_P2PDMA` 未启用，IOMMU 未在启动参数中启用

### GPUDirect RDMA 硬件限制（已知）

ConnectX-4 Lx 硬件不支持 GPUDirect RDMA——NVIDIA 官方要求 ConnectX-5+。
GPUDirect RDMA 当前做不了，但不影响 mlx5_prism（纯控制面，不需要 GPU）。
RoCEv2 + cudaHostRegister pinned memory 路径在当前硬件上完全可行。

## 构建方式

```bash
# 当前运行内核（根 Makefile 薄转发到 src/，KDIR 咒语只在根 Makefile 一处）
make

# 切换 7.0.0 内核后
make KDIR=/home/zqc/code/linux
```

## 学习参考: 每做一个模块，先去读对应的 mlx5_core 源码

mlx5_prism 不是闭门造车。**实现每个模块之前，先去 `/home/zqc/code/linux/drivers/net/ethernet/mellanox/mlx5/core/` 读对应的 mlx5_core 源文件**，理解 Mellanox 自己的实现做了什么，然后写精简版。实现完成后用 dmesg/ftrace 对照行为。

### 参考源文件总览 (共 8277 行)

| 文件 | 行数 | 对应 mlx5_prism 模块 | 要学什么 |
|------|------|---------------------|---------|
| `cmd.c` | 2575 | CMDQ 引擎 | 命令描述符构造、mailbox 写入、doorbell 通知、响应轮询、completion |
| `main.c` | 2409 | PCI probe, 模块生命周期 | `pci_register_driver`, probe/remove 函数, `module_init/exit` 模式 |
| `eq.c` | 1249 | Event Queue + MSI-X 中断 | EQ 初始化、MSI-X handler、中断响应处理、EQE 解析 |
| `fw.c` | 884 | 固件引导序列 | ENABLE_HCA, QUERY_HCA_CAP, INIT_HCA, QUERY_PAGES, MANAGE_PAGES |
| `pci_irq.c` | 833 | MSI-X 中断向量分配 | `pci_alloc_irq_vectors`, `request_irq`, IRQ affinity |
| `uar.c` | 327 | BAR0 UAR 管理 | doorbell 页映射、UAR 偏移计算 |

### 按任务读源码的顺序

```
T01 hello.ko + Makefile
    → 不需要读 mlx5_core，写自己的 module_init/exit

T02 PCI probe + BAR0 映射
    → 先读 main.c (mlx5_init_one / mlx5_remove_one)
    → 实现 mlx5_prism_probe: pcim_enable_device → pcim_iomap → pci_set_drvdata

T03 DMA mailbox + Event Queue 分配
    → 先读 cmd.c 前 ~200 行 (mailbox 分配), eq.c 前 ~200 行 (EQ 内存分配)
    → 理解 dma_alloc_coherent 的参数 (size, alignment, GFP flags)
    → 注意: mlx5_core 分配了大得多的结构体，mlx5_prism 只需最小版本

T04 MSI-X 中断 + request_irq
    → 先读 pci_irq.c (pci_alloc_irq_vectors) + eq.c (request_irq + handler)
    → 理解 MSI-X 向量数量如何决定、中断 handler 的最小框架

T05 CMDQ 命令提交 (doorbell)
    → 先读 cmd.c 中段 doorbell 写入逻辑
    → 理解 cmd_head/cmd_tail 维护、wmb() 位置、doorbell MMIO 地址计算
    → 关键: wmb() 在 writeq 之前，不是之后

T06 CMDQ 响应处理 (completion)
    → 先读 cmd.c 后半段中断完成处理 + 响应读取
    → 理解 EQE 格式、status 字段、wait_for_completion 的唤醒链

T07 固件引导 (7 步)
    → 先读 fw.c 全部 + main.c 中引导序列部分
    → 逐步骤对照 DESIGN.md 第四部分状态机
    → 每步完成都 dmesg 打印，出错回滚

T08 PHY 监控 (ACCESS_REG + sysfs)
    → 先读 cmd.c 中 ACCESS_REG 的调用方式
    → sysfs 部分参考 内核 Documentation/filesystems/sysfs.rst

T09 硬件冒烟测试
    → 先读 cmd.c 中 CREATE_QP 的调用方式
    → 理解 QP 创建 → RTS → WQE → doorbell → CQE 这条链

T09.5 最小 netdev 骨架 (register_netdev + ndo_start_xmit + NAPI poll)
    → 先读 en/main.c 中 alloc_etherdev_mq / register_netdev / net_device_ops 的注册方式
    → ndo_start_xmit: skb → DMA map → WQE → doorbell → 等 CQE → dev_kfree_skb
    → NAPI poll: CQE → skb 构造 → napi_gro_receive → napi_complete
    → 不做 offload，单包验证即可

T10 ftrace tracepoint
    → 参考内核 samples/trace_events/ 示例
    → 插桩位置: cmdq_issue (doorbell 前) 和 cmdq_complete (响应回来)

T11 verify.sh + 对比报告
    → 采集用 tracefs 直采（kprobe/kretprobe 经 /sys/kernel/tracing——
      本机无 perf/bpftrace，T11 裁决，勿引入外部依赖）
    → 延迟口径与判据见 DESIGN.md §10（P1' bp 档 / P2'ev 反向配对）
```

### 可以忽略的 mlx5_core 代码

- RDMA 相关 (SRIOV, eswitch, flow steering, TLS)
- 以太网数据面 offload (en_tx.c 的 GSO/TSO, en_rx.c 的 GRO, RSS, multiqueue)——但 en/main.c 的 netdev 注册部分需参考（S5.5）
- 调试功能 (debugfs, health, dpll, devlink)

### E810/冰湖驱动对比学习 (只读，不需要 E810 硬件)

E810 驱动位于 `/home/zqc/code/linux/drivers/net/ethernet/intel/ice/`。

| mlx5 概念 | E810 对应 | 文件 |
|-----------|----------|------|
| CMDQ | Admin Queue (AQ) | `ice_controlq.c` |
| mailbox + doorbell | 同模式: 共享内存 + 写寄存器 | 同上 |
| MSI-X + EQ | 中断 + completion queue | `ice_main.c` |
| UAR (硬件隔离) | **无**——E810 必须用 VFIO | — |

E810 的核心差异：没有 UAR 硬件隔离，控制面和数据面都在 VFIO 用户态。这是理解"为什么 Mellanox 不需要 VFIO"的最佳对比材料。

## 关键约束

1. **只做最小 netdev 骨架**——注册 netdev + `ndo_start_xmit`（SQ 单包）+ NAPI poll（RQ 单包），但不做 GRO/GSO/TSO/multiqueue/RSS/ethtool offload（那是 mlx5_core 的成熟功能，重写无 ROI）
2. **不依赖 mlx5_core**——不 `#include` mlx5_core 的任何头文件，独立实现 CMDQ
3. **不导出用户态接口**——不创建 `/dev/` 设备，不使用 `mmap`
4. **与 mlx5_core 互斥**——加载前必须先 `rmmod mlx5_ib && rmmod mlx5_core`
5. **PHY 诊断 sysfs 挂在 `/sys/class/misc/`**——独立于 netdev 接口（netdev 是网络栈入口，PHY 监控是诊断功能，两者职责不同）
6. **代码风格**: Linux 内核 coding style (`scripts/checkpatch.pl --no-tree --strict`)

## 验证命令

```bash
# 卸载 mlx5_core
sudo modprobe -r mlx5_ib mlx5_core

# 编译
make clean
make

# 加载
sudo insmod src/mlx5_prism.ko

# 检查
lspci -k -d 15b3:1015          # 应显示: Kernel driver in use: mlx5_prism
dmesg | tail -20               # 引导日志
cat /sys/class/misc/mlx5_prism0/status  # PHY 状态

# 恢复 mlx5_core
sudo rmmod mlx5_prism
sudo modprobe mlx5_core

# M2 一键验收（T16）：18 项判据 + 双层对比报告（docs/report.md 由
# scripts/gate/compare_report.sh 生成；--soak N 为稳定性快通道）
sudo ./verify.sh
```

## 工作方式

1. 读 DESIGN.md 了解完整技术设计
2. 自行拆分任务（从小到大，每步可验证）
3. **每做一个模块，先去读对应的 mlx5_core 源文件**（见上方对照表）
4. 每完成一个任务，编译验证、加载测试、git commit
5. 遇到设计文档与实际硬件行为不一致时，修正 DESIGN.md（commit 时注明原因）
6. 全部完成后，写一篇 **mlx5 架构总结** 文档（CMDQ 协议 / 固件引导 / QP-CQ 数据结构 / DMA 一致性 / doorbell-UAR 机制），作为知识沉淀和面试弹药
