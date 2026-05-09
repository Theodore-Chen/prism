# CLAUDE.md — mlx5_prism 项目指引

## 项目

实现 **mlx5_prism.ko**——ConnectX-4 Lx 智能网卡的控制面内核驱动。从零完成 PCIe 探测、CMDQ 引擎、7 步固件引导、PHY 诊断和硬件冒烟测试。

完整技术设计见 **DESIGN.md**。先读 DESIGN.md，然后拆任务、写代码、逐个验证。

## 硬件环境

- **网卡**: Mellanox ConnectX-4 Lx, `15b3:1015`, PCI `08:00.0`, 双口 25GbE
- **BAR0**: 32MB, UAR doorbell 寄存器
- **内核源码**: `/home/zqc/code/linux` (自编译 7.0.0, 驱动源码 ~8000 行可供阅读)
- **构建用 headers**: `/lib/modules/$(uname -r)/build` (5.15.0-174-generic)
- **运行内核**: `5.15.0-174-generic`，headers 已安装

## 构建方式

```bash
# 当前运行内核
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# 切换 7.0.0 内核后
make -C /home/zqc/code/linux M=$(pwd) modules
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

T10 ftrace tracepoint
    → 参考内核 samples/trace_events/ 示例
    → 插桩位置: cmdq_issue (doorbell 前) 和 cmdq_complete (响应回来)

T11 verify.sh + 对比报告
    → 用 bpftrace 采集 mlx5_core 运行时数据
    → 脚本在 DESIGN.md 第七部分
```

### 可以忽略的 mlx5_core 代码

- RDMA 相关 (SRIOV, eswitch, flow steering, TLS)
- 以太网数据面 (en_*.c, ethtool, netdev)
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

1. **不注册 netdev**——不实现 `ndo_start_xmit` / `napi_gro_receive` / ethtool 回调
2. **不依赖 mlx5_core**——不 `#include` mlx5_core 的任何头文件，独立实现 CMDQ
3. **不导出用户态接口**——不创建 `/dev/` 设备，不使用 `mmap`
4. **与 mlx5_core 互斥**——加载前必须先 `rmmod mlx5_ib && rmmod mlx5_core`
5. **sysfs 挂在 `/sys/class/misc/`**——不注册 `struct net_device`
6. **代码风格**: Linux 内核 coding style (`scripts/checkpatch.pl --no-tree --strict`)

## 验证命令

```bash
# 卸载 mlx5_core
sudo modprobe -r mlx5_ib mlx5_core

# 编译
make -C /lib/modules/$(uname -r)/build M=$(pwd) clean
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# 加载
sudo insmod mlx5_prism.ko

# 检查
lspci -k -d 15b3:1015          # 应显示: Kernel driver in use: mlx5_prism
dmesg | tail -20               # 引导日志
cat /sys/class/misc/mlx5_prism0/status  # PHY 状态

# 恢复 mlx5_core
sudo rmmod mlx5_prism
sudo modprobe mlx5_core
```

## 工作方式

1. 读 DESIGN.md 了解完整技术设计
2. 自行拆分任务（从小到大，每步可验证）
3. **每做一个模块，先去读对应的 mlx5_core 源文件**（见上方对照表）
4. 每完成一个任务，编译验证、加载测试、git commit
5. 遇到设计文档与实际硬件行为不一致时，修正 DESIGN.md（commit 时注明原因）
