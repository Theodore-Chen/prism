# mlx5_prism: ConnectX-4 Lx Control-Plane Driver

> 内核模块名：`mlx5_prism.ko`
> 从零实现 ConnectX-4 Lx 的固件-驱动控制面协议：CMDQ 引擎 + 7 步固件引导 + PHY 层诊断 + ftrace 全链路追踪。不注册 netdev，不跑流量——专注"主机怎么命令智能网卡"这一层。全部指标与 Mellanox 自带的 mlx5_core 做对照验证。

---

## 1. 项目概述

### 为什么叫 mlx5_prism

- `mlx5`——明确是 Mellanox ConnectX-4/5 系列硬件，和 mlx5_core 对应
- `prism`——棱镜，让控制面协议（CMDQ / 固件引导 / PHY 寄存器 / 中断响应）变得透明可读
- E810 上的对应项目将命名为 `e810_prism`（用户态 VFIO 程序，技术栈不同但项目平行）

### mlx5 内核栈全景：mlx5_prism 在哪里

当前环境的 Mellanox 内核栈（`lsmod | grep mlx`）：

```
用户态程序 (DPDK / RDMA 应用)
───────
ib_uverbs          ← /dev/infiniband/uverbs0 后端，用户态 verbs 入口
mlx5_ib            ← RDMA 驱动，verbs → CMDQ 翻译
───────
mlx5_core          ← 基础网卡驱动：PCIe 探测、固件引导、CMDQ 引擎、以太网收发
mlxfw              ← 固件升级工具驱动
───────
硬件
```

**mlx5_prism.ko 替换的是 mlx5_core 的底部两层**——PCIe 探测、固件引导、CMDQ 引擎。但不实现 verbs 接口、以太网收发、任何上层协议。

```
mlx5_prism 的上游                   mlx5_prism 的下游
───────────────                    ───────────────
Linux PCI 子系统                     PCIe 总线
  pci_register_driver                 │
  → 探测 15b3:1015                    ▼
                                   CMDQ 协议（硬件 spec 定义，非软件选择）
sysfs                                 │ dma_alloc_coherent (mailbox)
  /sys/class/misc/mlx5_prism0/        │ writeq (doorbell)
                                      │ request_irq (MSI-X)
ftrace                                ▼
  mlx5_prism:cmdq_issue              固件 (ARM RTOS)
  mlx5_prism:cmdq_complete            │
                                      ▼
printk / dmesg                       硬件
```

**上游不是其他内核模块——没有其他模块依赖 mlx5_prism。** 它是一个终点模块，只和硬件对话。上游是内核基础设施（PCI 框架、sysfs、ftrace）。

### 控制面与数据面的分工

```
控制面                                  数据面
──────────────                        ──────────────
主机怎么"命令"网卡                      网卡怎么"搬"数据

PCIe probe + BAR mapping              DPDK Raw Packet Engine
CMDQ 引擎 (mailbox+doorbell+EQ)        RDMA QP 状态机
7 步固件引导 (DISABLED→OPERATIONAL)     RoCEv2 包构造 (BTH/RETH/AETH)
MSI-X 中断 + Event Queue               DCQCN 拥塞控制
ACCESS_REG PHY 计数器                  RDMA one-sided 操作
ftrace CMDQ 命令追踪                   rte_rdtsc 用户态 profiling
硬件冒烟测试 (CREATE_QP→WQE→CQE)

交付物：mlx5_prism.ko ~2000 行
```

### 为什么只做最小 netdev（不做完整转发）

**不做完整数据面转发**（GRO/GSO/TSO/multiqueue/RSS/ethtool offload）——那是 mlx5_core 的成熟代码，重写没有工程价值。

**但做最小 netdev 骨架**（~300 行）——覆盖 JD 硬性要求"Linux 网络子系统（register_netdev/NAPI/ndo_start_xmit）"，证明你懂 API 链路。

| 做什么 | 不做什么 | 理由 |
|--------|---------|------|
| `register_netdev` + `net_device_ops` | GRO/GSO/TSO offload | mlx5_core 成熟功能，重写无 ROI |
| `ndo_start_xmit`（SQ 单包路径） | multiqueue / RSS / ethtool | 生产级功能，非面试考点 |
| NAPI poll（RQ 单包回收 + `napi_gro_receive`） | 线速转发 / 中断自适应 | 目的是 JD 关键词覆盖 |
| `ndo_open/stop`（CMDQ 创建/销毁 RQ/SQ） | XDP / tc offload | 超出项目范围 |

**面试话术**："我没有重写 mlx5_core 的 netdev——它有 GRO/GSO/TSO/multiqueue 全套 offload，重写没有工程价值。但我注册了最小 netdev 骨架（NAPI + ndo_start_xmit），用 CMDQ 创建 RQ/SQ 实现单包收发——证明我理解 Linux 网络子系统的完整 API 链路。差异化在控制面和可观测性。"

### netdev 在 Prism 中的角色：最小骨架，不是转发引擎

netdev 在 Prism 中**只做最小验证**：注册 netdev + 实现 `ndo_start_xmit`（单包 SQ 路径）+ NAPI poll（单包 RQ 回收）。目的不是让 ping/iperf 跑线速，而是覆盖 JD 关键词"Linux 网络子系统"。用户态数据采集仍通过 sysfs / ftrace / dmesg。

### 为什么不需要对接 libibverbs

libibverbs 是用户态 RDMA 接口，依赖 `ib_uverbs.ko` → `mlx5_ib.ko` → `mlx5_core.ko` 这条调用链。mlx5_prism 不提供 verbs 兼容层——它的 CMDQ 引擎只在内核模块内部使用。冒烟测试在内核态直接构造 WQE、写 doorbell、轮询 CQE——不把硬件资源 mmap 给用户态，不暴露 `/dev/infiniband/uverbsX` 设备文件。

### UAR 与为什么 Mellanox 不需要 VFIO

**VFIO 是"内核不配合时"的通用方案**——把整张 PCI 卡从内核驱动抢过来，用户态通过 VFIO 映射整个 BAR、通过 IOMMU 重映射 DMA 地址。Intel E810/Broadcom 等网卡的驱动不支持数据面 doorbell 按页 mmap 给用户态，所以 DPDK 只能用 VFIO 独占整张卡。

**Mellanox 不需要 VFIO 的根本原因是硬件层面的 UAR (User Access Region) 隔离**：

- BAR0 里给每个 QP 分配一个独立的 doorbell page
- 内核通过 CMDQ 创建 QP 时，硬件硅片层面记录"这个 QP 属于哪个保护域"
- 内核通过 mmap 把 QP 的 doorbell page 只暴露给对应的用户态进程
- 用户态 `writeq(doorbell, mmap_addr)` 时，硬件自己做访问控制——不是合法的 QP 主人，写不进去
- 结果：内核保有 CMDQ（控制面），用户态只拿到 QP 数据面 doorbell

**这和 mlx5_prism 的关系**：mlx5_prism 学的是 CMDQ 控制面（创建 QP、管理硬件资源），这些是 UAR 机制的前提——没有 CMDQ 创建 QP，UAR 就映射不出 doorbell page。

### CMDQ 不是软件架构，是硬件协议

CMDQ 不是 Mellanox 写的一个软件框架。它是 ConnectX-4 固件对外的唯一通信接口——固件只认 mailbox + doorbell 格式的消息。就像 UART 驱动必须按 UART 寄存器格式写数据一样，没有"不用 CMDQ"的选项。

| 你想做的事 | 必须通过什么命令 |
|-----------|----------------|
| 读 PHY 寄存器 | ACCESS_REG (opcode 0x805) |
| 创建 QP | CREATE_QP (opcode 0x400) |
| 查询设备能力 | QUERY_HCA_CAP (opcode 0x100) |
| 引导固件 | ENABLE_HCA (0x104) / INIT_HCA (0x102) |

---

### 跨平台 DMA 记忆：ION/DMA-BUF 到 CMDQ

CMDQ 需要的 DMA 内存分配和其他场景中的 DMA 管理是同一套原理。理解这种统一性有助于降低学习曲线的陡峭度。

#### ION 与 CMDQ 的 DMA 同构

| 场景 | 分配接口 | 消费者 | 共享方式 | 零拷贝思想 | 内存屏障 |
|------|---------|--------|---------|----------|---------|
| Android ION (NPU) | `ion_alloc`（用户态） | NPU DMA 引擎 | fd → Binder → mmap（进程间） | 两个进程共享物理页 | `wmb()` → NPU 可见 |
| 嵌入式 ZYNQ | `dma_alloc_coherent`（内核态） | 片内外设 DMA | 内核内部直接访问 | CPU↔外设直接 DMA | `wmb()` → 外设可见 |
| mlx5 CMDQ | `dma_alloc_coherent`（内核态） | 网卡固件 DMA（PCIe） | 内核内部 + MMIO doorbell | 主机↔固件共享 mailbox | `wmb()` → doorbell 前固件可见 |

**关键认知**：不同硬件平台上的 DMA 内存管理本质相同——分配物理连续内存 → 通知消费者 → 内存屏障保证可见性 → 消费者 DMA 读写。区别只在作用域（单机进程间 / 片内 / PCIe 跨总线）。

#### ION 零拷贝与 RDMA MR 的同构性

ION 是 Android 的物理连续内存分配器，解决 NPU/GPU/摄像头需要大块物理连续内存做 DMA 的问题。其核心机制和 mlx5 的 RDMA Memory Region 注册完全同构：

```
ION 分配 -> struct file -> fd -> Binder 传递 -> mmap 零拷贝（单机进程间）
                                                      |
                                             同一套思想，不同作用域
                                                      v
ibv_reg_mr -> rkey 交换 -> RDMA WRITE/READ -> 零拷贝（跨机器 PCIe）
```

**ION 进程间零拷贝链路**：
1. 进程 A `ion_alloc` -> 内核分配物理连续页 -> 创建 `struct file` -> 返回 fd
2. 进程 A 通过 Binder 把 fd 传给进程 B（传的是文件描述符，不是数据，20MB 数据从未被拷贝）
3. 内核把同一个 `struct file` 安装到进程 B 的 fd 表
4. 进程 B `mmap(fd)` -> 虚拟地址指向同一块物理内存
5. 之后两个进程直接读写 —— CPU MMU 查页表翻译，不经过内核，零拷贝

#### system cache 热冷分离的迁移

ION 场景中的做法：`mmap MAP_FIXED` 将 8MB system cache + 12MB 普通内存拼接成连续 20MB 虚拟地址空间。上层看到连续数组，底层是两块不同物理内存池——热数据访问延迟 ~几 ns（SRAM），冷数据 ~几百 ns（DRAM）。

在 mlx5 驱动中可以应用的思想：
- CMDQ descriptor ring（热，每毫秒读写） -> `dma_alloc_coherent`，coherent 内存
- 固件镜像区（冷，初始化后只读） -> streaming DMA，`dma_map_single` 按需映射

`dma_alloc_coherent` 比 streaming DMA 更消耗系统资源（绕过 cache、需要 UC/WC 映射），不该把冷数据也放进 coherent 区。mlx5_prism 的实现中，CMDQ mailbox 用 coherent，固件镜像区用 streaming——这是热冷分离思想的直接迁移。

#### CMDQ 与 ION 共享内存的对位

| 概念 | ION（单机进程间） | CMDQ（主机<->固件） |
|------|-----------------|-----------------|
| 共享内存 | ION heap 分配的物理页 | `dma_alloc_coherent` 分配的 mailbox |
| 生产者通知 | `wmb()` + `eventfd` / Binder 消息 | `wmb()` + doorbell MMIO write |
| 消费者通知 | `eventfd` / 原子变量轮询 | MSI-X 中断 |
| 访问控制 | fd 权限（谁能 mmap） | BAR 空间 MMIO 映射（内核独占） |
| 同步原语 | 原子变量 / futex | `spin_lock`（CMDQ 临界区 < 10μs） |


## 2. 技术架构

### 模块边界

```
┌─────────────────────────────────────────────────┐
│                用户态                             │
│                                                  │
│  cat /sys/class/misc/mlx5_prism0/pre_fec_ber    │  ← PHY 诊断数据
│  cat /sys/class/misc/mlx5_prism0/status         │
│                                                  │
│  perf record -a -e mlx5_prism:cmdq_issue,\      │  ← CMDQ 命令追踪
│                      mlx5_prism:cmdq_complete    │
│  perf script | awk ... → avg/p99 latency        │
│                                                  │
│  bpftrace -e 'kprobe:mlx5_cmd_exec {...}'       │  ← 采集 mlx5_core 基准
│                                                  │
│  sudo ./verify.sh                               │  ← 一键验收
└──────────────────┬──────────────────────────────┘
                   │ sysfs / ftrace / perf_event
┌──────────────────▼──────────────────────────────┐
│              mlx5_prism.ko (~2000 行)            │
│                                                  │
│  ┌─ mlx5_prism_probe()     PCIe 设备探测        │
│  │  pcim_iomap()           映射 BAR0 (UAR)       │
│  │  pci_set_master()       使能 DMA              │
│  │                                               │
│  ├─ mlx5_prism_cmdq_init() CMDQ 引擎初始化       │
│  │  dma_alloc_coherent()   mailbox 内存          │
│  │  初始化 command/response 队列                  │
│  │  trace_mlx5_prism_cmdq_issue()               │
│  │  trace_mlx5_prism_cmdq_complete()            │
│  │                                               │
│  ├─ mlx5_prism_fw_bootstrap() 固件引导序列       │
│  │  poll iseg.initializing → 就绪               │
│  │  ENABLE_HCA (opcode 0x104)                  │
│  │  QUERY_PAGES → MANAGE_PAGES (boot)          │
│  │  QUERY_HCA_CAP → 解析能力集                   │
│  │  QUERY_PAGES → MANAGE_PAGES (init)          │
│  │  INIT_HCA (opcode 0x102)                    │
│  │  → HCA 进入 OPERATIONAL 状态                 │
│  │                                               │
│  ├─ mlx5_prism_eq_init()   Event Queue + MSI-X │
│  │  pci_alloc_irq_vectors()                     │
│  │  request_irq() → CMDQ completion handler     │
│  │                                               │
│  ├─ mlx5_prism_phy_monitor PHY 信号质量监控      │
│  │  定时器(100ms) → ACCESS_REG PPCNT            │
│  │  pre-FEC BER / symbol error rate             │
│  │  sysfs 导出 + 阈值告警                         │
│  │  → 与 mlx5_core ethtool -S 交叉验证           │
│  │                                               │
│  ├─ mlx5_prism_smoke()    硬件冒烟测试            │
│  │  CREATE_QP → 修改 QP → RTS                     │
│  │  构造 Send WQE → 写 doorbell                   │
│  │  轮询 CQE → 验证 opcode/status                 │
│  │  → DESTROY_QP                                 │
│  │                                               │
│  └─ mlx5_prism_cmdq_bench  CMDQ 性能测量        │
│     自行测量 avg/p99 延迟                         │
│      vs bpftrace 采集的 mlx5_core 基准           │
│                                                  │
└──────────────────┬──────────────────────────────┘
                   │ PCIe (MMIO + DMA)
┌──────────────────▼──────────────────────────────┐
│           Mellanox ConnectX-4 Lx                │
│                                                  │
│  BAR0: UAR (doorbell 寄存器，每 QP 独立页)         │
│  CMDQ mailbox (DMA 一致性内存，主机↔固件共享)      │
│  Event Queue (固件 → 主机 MSI-X 中断)            │
│  固件 (ARM RTOS, 处理 CMDQ 命令)                  │
│  PHY: PPCNT 寄存器 (0x5008, ACCESS_REG 访问)     │
└─────────────────────────────────────────────────┘
```

### 核心数据结构

```c
struct mlx5_prism_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;            // UAR (doorbell)

    // CMDQ
    struct mlx5_prism_cmdq {
        void *mailbox;             // dma_alloc_coherent 分配
        dma_addr_t mailbox_dma;
        spinlock_t lock;           // 保护 CMDQ 并发访问

        u16 cmd_head;              // 下一个命令写入位置
        u16 cmd_tail;              // 固件已消费位置
        u16 resp_head;             // 下一个响应读取位置

        u64 doorbell_offset;       // BAR0 内 doorbell 偏移
    } cmdq;

    // Event Queue
    struct mlx5_prism_eq {
        void *buf;                 // dma_alloc_coherent 分配
        dma_addr_t buf_dma;
        int irq;                   // MSI-X 向量编号
        u32 consumer_index;
    } eq;

    // PHY 监控
    struct mlx5_prism_phy {
        struct timer_list timer;
        u32 pre_fec_ber;           // FEC 前误比特率
        u32 symbol_error_rate;
        u8  link_quality;          // 0-100
        u8  status;                // 0=ok, 1=degraded, 2=critical
        u32 prev_ber;              // 用于趋势判断
    } phy;

    // 固件能力集
    struct mlx5_hca_cap caps;      // QUERY_HCA_CAP 返回的结构体

    // 冒烟测试
    u32 smoke_qpn;                 // 测试用的 QP number
    u32 smoke_cqn;                 // 测试用的 CQ number
};
```

### 不做的事

| 不做 | 原因 |
|------|------|
| 数据面 (TX/RX/NAPI/netdev) | DPDK/用户态数据面，不重复造 |
| ethtool 回调 | 需要 netdev，砍掉 |
| 多队列 / RSS | 控制面不需要 |
| RDMA / verbs / libibverbs | 独立的用户态项目 |
| SR-IOV / VF | 远期扩展 |
| 固件镜像加载 | ConnectX-4 固件在 Flash 中，上电自动加载到 ARM 核。mlx5_prism 不做固件分发——只做引导协议 |
| VFIO | mlx5_prism 永远不需要 VFIO——它是内核模块，不把硬件交给用户态 |

### 运行时独占关系

**mlx5_prism.ko 和 mlx5_core 不能同时加载**——同一张 PCI 卡只能有一个驱动。加载 mlx5_prism 前必须先卸载整个 Mellanox 栈：

```bash
sudo modprobe -r mlx5_ib mlx5_core   # 逆序卸载
sudo insmod mlx5_prism.ko            # 只留下 mlx5_prism
lsmod | grep mlx                     # 一行：mlx5_prism
```

mlx5_prism 做完验证后卸载，mlx5_core 回来：

```bash
sudo rmmod mlx5_prism
sudo modprobe mlx5_core              # 回到生产驱动
```

---

## 3. CMDQ 协议详解

### 概念模型

CMDQ（Command Queue）是主机驱动和网卡固件之间的**共享内存消息队列**。

```
驱动                         固件（网卡内置 ARM 核）
───                          ────
写命令到 mailbox             读取 mailbox
写 doorbell 寄存器            收到通知，执行命令
                             写响应到 mailbox
                             发 MSI-X 中断
收到中断，读响应
```

### 关键操作序列

```
时间轴 ──────────────────────────────────────────────────────→

1. 驱动：构造命令描述符 (opcode + 参数 + 输出缓冲区地址)
2. 驱动：写入 CMDQ mailbox 的下一个 slot
3. 驱动：更新 cmd_head
4. 驱动：wmb() 内存屏障（确保固件看到完整命令）
5. 驱动：writeq(cmd_head, BAR0 + doorbell_offset)   ← tracepoint: cmdq_issue
         ║
6. 固件：门铃中断触发，固件 DMA 读取 mailbox 中的命令
7. 固件：执行命令（如读取 PHY 寄存器）
8. 固件：将响应写入 mailbox，更新响应队列
9. 固件：触发 MSI-X 中断
         ║
10. 主机：MSI-X handler → EQ 处理 → 读响应  ← tracepoint: cmdq_complete
11. 驱动：解析状态码，返回结果给调用者
```

### 并发保护

- 内核模块内部对 CMDQ 的访问用 `spin_lock` 保护
- `spin_lock`（而非 `mutex`）——因为 CMDQ 操作在进程上下文中耗时 < 10μs，不需要睡眠
- 驱动侧更新 cmd_head、固件侧更新 cmd_tail——**单写者模式**，不需要锁保护这两个字段

### ftrace tracepoint 定义

```c
// mlx5_prism_trace.h
TRACE_EVENT(mlx5_prism_cmdq_issue,
    TP_PROTO(u16 opcode, u16 opcode_mod, u32 reg_id, u64 cycle),
    TP_ARGS(opcode, opcode_mod, reg_id, cycle),
    TP_STRUCT__entry(
        __field(u16, opcode)
        __field(u16, opcode_mod)
        __field(u32, reg_id)
        __field(u64, cycle)
    ),
    TP_fast_assign(
        __entry->opcode = opcode;
        __entry->opcode_mod = opcode_mod;
        __entry->reg_id = reg_id;
        __entry->cycle = cycle;
    ),
    TP_printk("opcode=0x%x(%s) reg_id=0x%x(%s) cycle=%llu",
              __entry->opcode,
              mlx5_prism_cmdq_opcode_name(__entry->opcode),
              __entry->reg_id,
              mlx5_prism_reg_name(__entry->reg_id),
              __entry->cycle)
);

TRACE_EVENT(mlx5_prism_cmdq_complete,
    TP_PROTO(u16 opcode, u8 status, u64 latency_ns, u64 cycle),
    TP_ARGS(opcode, status, latency_ns, cycle),
    TP_STRUCT__entry(
        __field(u16, opcode)
        __field(u8,  status)
        __field(u64, latency_ns)
        __field(u64, cycle)
    ),
    TP_fast_assign(
        __entry->opcode = opcode;
        __entry->status = status;
        __entry->latency_ns = latency_ns;
        __entry->cycle = cycle;
    ),
    TP_printk("opcode=0x%x status=%u latency_ns=%llu cycle=%llu",
              __entry->opcode, __entry->status,
              __entry->latency_ns, __entry->cycle)
);
```

---

## 4. 固件引导序列

### 状态机

```
          ┌─────────────────────────────┐
          │ NIC_IFC_DISABLED            │ ← rmmod mlx5_core 后
          │ 固件就绪，HCA 不可操作         │   硬件上电初始状态
          └─────────────┬───────────────┘
                        │ poll iseg.initializing → 固件启动完成
                        │ (最多 100 次轮询，每次 10ms)
                        ▼
          ┌─────────────────────────────┐
          │ ENABLE_HCA (opcode 0x104)   │ ← 告诉固件："驱动来了，准备初始化"
          │ 固件为 HCA 分配内部资源        │
          └─────────────┬───────────────┘
                        │ 状态码 = 0 (OK)
                        ▼
          ┌─────────────────────────────┐
          │ QUERY_PAGES(boot)           │ ← 固件告知需要多少启动页 (1 页)
          │ MANAGE_PAGES(give boot)      │ ← 驱动分配并交给固件
          └─────────────┬───────────────┘
                        │
                        ▼
          ┌─────────────────────────────┐
          │ QUERY_HCA_CAP               │ ← 读 HCA 能力集
          │ 返回: max_qp, max_cq,       │   ~3000 字节结构体
          │       num_ports, max_speed  │   Mellanox PRM 有完整位定义
          │       等数百个字段           │
          └─────────────┬───────────────┘
                        │
                        ▼
          ┌─────────────────────────────┐
          │ QUERY_PAGES(init)           │ ← 固件告知需要多少初始化页 (4 页)
          │ MANAGE_PAGES(give init)      │
          └─────────────┬───────────────┘
                        │
                        ▼
          ┌─────────────────────────────┐
          │ INIT_HCA (opcode 0x102)     │ ← 最终初始化
          │ 状态码 = 0 → OPERATIONAL     │
          └─────────────────────────────┘
                        │
                        ▼
          ┌─────────────────────────────┐
          │ NIC_IFC_OPERATIONAL         │ ← CMDQ 完全可用
          │ 可以创建 QP/CQ，读写寄存器     │   驱动正常工作
          └─────────────────────────────┘
```

### 关键命令说明

| 命令 | Opcode | 输入 | 输出 |
|------|--------|------|------|
| ENABLE_HCA | 0x104 | - | 状态码 |
| QUERY_HCA_CAP | 0x100 | op_mod=0x1 | HCA 能力集（~3KB） |
| QUERY_PAGES | 0x106 | op_mod=0x1 (boot) / 0x2 (init) | 所需页数 |
| MANAGE_PAGES | 0x107 | 页的物理地址列表 | 状态码 |
| INIT_HCA | 0x102 | HCA 能力集（写回） | 状态码 |
| ACCESS_REG | 0x805 | reg_id (如 0x5008=PPCNT) | 寄存器内容 |

### 与 mlx5_core 引导一致性验证

固件引导完成后，`dmesg` 输出 mlx5_prism 读取的 HCA 能力集摘要，与 `mlx5_core` 加载时的 dmesg 对比：

```
# mlx5_core 加载 (modprobe mlx5_core)
$ dmesg | grep mlx5
mlx5_core 0000:01:00.0: firmware version: 14.32.1010
mlx5_core 0000:01:00.0: 252.048 Gb/s available PCIe bandwidth
mlx5_core 0000:01:00.0: max_qp=262144, max_cq=16777216, num_ports=2
mlx5_core 0000:01:00.0: port 1 speed=25G, port 2 speed=25G

# mlx5_prism 加载
$ sudo rmmod mlx5_core && sudo insmod mlx5_prism.ko && dmesg | grep mlx5_prism
[mlx5_prism] PCIe device 15b3:1015 found, BAR0 mapped
[mlx5_prism] FW initializing... poll iseg->initializing: ready (attempt 3/100)
[mlx5_prism] ENABLE_HCA complete, status=0
[mlx5_prism] QUERY_PAGES: boot=1 page
[mlx5_prism] MANAGE_PAGES boot: accepted
[mlx5_prism] QUERY_HCA_CAP: max_qp=262144, max_cq=16777216, num_ports=2 ← 完全一致 ✓
[mlx5_prism] QUERY_HCA_CAP: port 1 speed=25G, port 2 speed=25G           ← 完全一致 ✓
[mlx5_prism] QUERY_PAGES: init=4 pages
[mlx5_prism] MANAGE_PAGES init: accepted
[mlx5_prism] INIT_HCA complete, status=0
[mlx5_prism] HCA operational. Bootstrap time: 203ms
[mlx5_prism] CMDQ latency: 8.5μs avg (n=20)
[mlx5_prism] PHY monitor started
```

HCA 能力集 2976 字节 bit-exact 一致 = 固件引导协议实现完全正确，不需要 ping 来证明。

---

## 5. 验证三层次——从 CMDQ 读到硬件自检

### 层次 1：CMDQ 读能力验证（QUERY_HCA_CAP）

验证 CMDQ mailbox DMA（双向）、doorbell 通知机制、MSI-X 中断投递、固件引导协议全部正确。2976 字节 HCA 能力集与 mlx5_core 逐字段匹配。

### 层次 2：CMDQ 读寄存器验证（ACCESS_REG PPCNT）

验证固件能代为读取内部寄存器（PHY 计数器不在 BAR 空间，只能通过固件访问）。与 mlx5_core `ethtool -S` 交叉验证数值。

### 层次 3：CMDQ 对象生命周期 + 硬件数据路径验证（冒烟测试）

QUERY_HCA_CAP 不验证以下几点——因为数据路径和寄存器读取走的是不同的硬件通路：

| 没验证的 | 需要什么才能验证 |
|---------|----------------|
| WQE 格式能否被硬件正确解析 | 创建一个 QP、发一个 Send WQE |
| QP doorbell 能否被硬件接受 | 写 QP doorbell、等硬件执行 |
| 硬件 DMA 能否从主机内存读 WQE | CQE 返回表示硬件已读 WQE |
| CQE 能否写回主机内存 | 轮询 CQE 直到 owner bit 翻转 |
| QP/CQ 对象能否被 CMDQ 创建和销毁 | 完整的对象生命周期 |

**这三层缺一不可。层次 1+2 通过了，层次 3 失败的话，dmesg 里什么都看不到——硬件不报错，固件不报错，就是没动静。必须跑一个包来区分"一切正常只是没流量"和"硬件在静默丢弃"。**

#### 冒烟测试代码

```c
// mlx5_prism_smoke.c —— 最小硬件数据路径验证，~150 行

int mlx5_prism_smoke_test(struct mlx5_prism_dev *dev)
{
    // 1. CMDQ 创建 QP, CQ
    qpn = mlx5_prism_cmdq_create_qp(dev, cqn);
    // → 固件返回 qpn=5

    // 2. CMDQ 修改 QP 状态 → RTS (Ready To Send)
    mlx5_prism_cmdq_modify_qp(dev, qpn, MLX5_QP_STATE_RTS);

    // 3. 分配一块 DMA 内存，写一个最小以太网帧
    frame = dma_alloc_coherent(dev, 64, &dma_handle, GFP_KERNEL);
    memcpy(frame, dummy_eth_frame, 64);

    // 4. 构造一个 Send WQE
    wqe = kmalloc(MLX5_WQE_SIZE, GFP_KERNEL);
    wqe->ctrl.opcode = MLX5_OPCODE_SEND;
    wqe->dseg.addr = cpu_to_be64(dma_handle);
    wqe->dseg.byte_count = cpu_to_be32(64);

    // 5. 写 WQE 到 QP 的 send queue
    dev->sq[qpn]->wqe[wqe_idx] = wqe;

    // 6. 写 doorbell —— 硬件开始工作
    wmb();
    writeq(wqe_counter, BAR0 + UAR_OFFSET(qpn));

    // 7. 轮询 CQE，最多等 1ms
    for (retry = 0; retry < 100; retry++) {
        cqe = dev->cq[cqn]->cqe[cqe_idx];
        if (cqe->owner == current_owner) {
            if (cqe->opcode == MLX5_CQE_RESP_SEND &&
                cqe->status == 0)
                goto pass;
        }
        udelay(10);
    }
    return -ETIMEDOUT;  // 硬件没响应 ← 层次 3 没通过

pass:
    // 8. 销毁对象
    mlx5_prism_cmdq_destroy_qp(dev, qpn);
    mlx5_prism_cmdq_destroy_cq(dev, cqn);
    return 0;
}
```

冒烟测试不是"数据面功能"——它是 CMDQ 对象生命周期验证 + 硬件数据路径电路测试。不注册 netdev，不经过 Linux 协议栈，三个层次一起构成了完整的自我验证。

---

## 6. PHY 诊断子系统

### 技术关键：PHY 寄存器不能直接 MMIO 读

ConnectX-4 的 PHY 诊断计数器（PPCNT, register ID 0x5008）**不能通过 BAR 直接 MMIO 读取**。必须通过 `ACCESS_REG` 固件命令（opcode 0x805）访问。这恰好证明了 CMDQ 的价值——PHY 监控是 CMDQ 引擎的第一个应用场景。

```
定时器触发 (每 100ms)
    │
    ▼
mlx5_prism_cmdq_access_reg(dev, MLX5_REG_PPCNT, &buf, sizeof(buf))
    │
    ├── 构造 ACCESS_REG 命令描述符
    ├── 写入 CMDQ mailbox
    ├── wmb()
    ├── writeq(doorbell)           ← tracepoint: cmdq_issue
    ├── wait_for_completion()       ← 等待 MSI-X EQ 响应
    │                                  tracepoint: cmdq_complete
    └── 解析 PPCNT 内容
        ├── pre_fec_ber
        ├── symbol_error_rate
        ├── fec_corrected_blocks
        └── fec_uncorrectable_blocks
            │
            ▼
        sysfs 导出 + 阈值判断
```

### sysfs 接口

```
/sys/class/misc/mlx5_prism0/
├── pre_fec_ber          # RO: FEC 前向纠错前的误比特率
├── symbol_error_rate    # RO: 符号错误率
├── fec_corrected        # RO: FEC 已纠正块数
├── fec_uncorrectable    # RO: FEC 无法纠正块数
├── temperature          # RO: 光模块温度 (°C)
├── link_quality         # RO: 链路质量评分 (0-100)
├── status               # RO: ok / degraded / critical
└── monitor_interval_ms  # RW: 采样间隔 (默认 100ms)
```

注意：因为不注册 netdev，sysfs 挂在 `/sys/class/misc/` 下——干净，不需要 `struct net_device`。

### 状态机与告警

```
ok ──────────→ degraded ──────────→ critical
   pre-FEC BER       pre-FEC BER        pre-FEC BER
   < 1e-10           1e-10 ~ 1e-7       > 1e-7
   正常范围           链路劣化            即将断链
                     dmesg WARNING      dmesg ERR
```

**验证方式**：用 mlx5_core 的 `ethtool -S` 交叉验证 PHY 计数器的数值。

```bash
# 采集 mlx5_core 基准
modprobe mlx5_core
ethtool -S eth0 | grep -E "ber|fec|error" > baseline_mlx5.txt

# 采集 mlx5_prism 数据
rmmod mlx5_core && insmod mlx5_prism.ko
cat /sys/class/misc/mlx5_prism0/pre_fec_ber
cat /sys/class/misc/mlx5_prism0/symbol_error_rate

# 对比：同数量级即为通过
```

---

## 7. 双层对比验证框架

### 第一层：一致性校验（必须 bit-exact 或同数量级）

| # | 指标 | mlx5_prism 采集方式 | mlx5_core 采集方式 | 判据 |
|---|------|-------------------|-------------------|------|
| C1 | HCA 能力集 | `QUERY_HCA_CAP` (opcode 0x100) | `cat /sys/class/net/eth0/device/caps` | 逐字段 bit-exact |
| C2 | 固件版本 | init segment `fw_rev`（无 QUERY_FW 命令） | `ethtool -i eth0` | 完全一致 |
| C3 | Port 数量/速率 | `QUERY_HCA_CAP` ports 字段 | `ethtool eth0` | 完全一致 |
| C4 | pre-FEC BER | `ACCESS_REG PPCNT` → sysfs | `ethtool -S eth0 \| grep ber` | 同数量级（差值 < 5%） |
| C5 | FEC 纠错计数 | 同上 | `ethtool -S eth0 \| grep fec` | 趋势一致 |

### 第二层：性能对比（允许差异，需要合理解释）

| # | 指标 | mlx5_core 基准 | mlx5_prism 目标 | mlx5 采集方式 |
|---|------|---------------|----------------|--------------|
| P1 | CMDQ avg 延迟 | ~8μs | ≤ 10μs | `bpftrace kprobe:mlx5_cmd_exec` |
| P2 | CMDQ p99 延迟 | ~15μs | ≤ 30μs | 同上 |
| P3 | 固件引导时间 | ~200ms | ≤ 300ms | `dmesg` 时间戳差值 |
| P4 | MSI-X 中断响应 | ~5μs | ≤ 10μs | `ftrace irq_handler_entry` |

### bpftrace 采集 mlx5_core 基准脚本

```bash
#!/bin/bash
# collect_mlx5_baseline.sh

echo "=== 采集 mlx5_core CMDQ 延迟基准 (30s) ==="
sudo bpftrace -e '
kprobe:mlx5_cmd_exec {
    @start[tid] = nsecs;
    @opcode[tid] = arg1;
}
kretprobe:mlx5_cmd_exec /@start[tid]/ {
    $lat_ns = nsecs - @start[tid];
    @lat_us = hist($lat_ns / 1000);
    @lat_stats = stats($lat_ns / 1000);
    delete(@start[tid]);
}
END {
    printf("\nCMDQ 延迟分布 (us):\n");
    print(@lat_us);
    printf("\nCMDQ 延迟统计 (us):\n");
    print(@lat_stats);
}
interval:s:1 { printf("."); }
' &

sleep 30
kill %1
```

### 预期对比报告格式

```markdown
## mlx5_prism vs mlx5_core 对比报告

### 一致性校验
| 指标 | mlx5_core | mlx5_prism | 结果 |
|------|-----------|------------|------|
| max_qp | 262144 | 262144 | ✓ |
| num_ports | 2 | 2 | ✓ |
| port 1 speed | 25G | 25G | ✓ |
| pre-FEC BER | 3.2e-12 | 3.3e-12 | ✓ (同数量级) |
| FEC corrected | 0 | 0 | ✓ |

### 性能对比
| 指标 | mlx5_core | mlx5_prism | 差异 |
|------|-----------|------------|------|
| CMDQ avg | 8.2μs | 8.5μs | +3.7% |
| CMDQ p99 | 14.1μs | 12.3μs | -12.8% |
| 引导时间 | 187ms | 203ms | +8.6% |

### p99 差异分析
p99 比 mlx5_core 低 12.8%——因为 mlx5_core 有 20+ 个并发 CMDQ 消费者
(RDMA/E-Switch/Flow Steering 等)，存在锁竞争。mlx5_prism 只有一个 PHY 监控消费者，
单生产者无锁竞争。差异的方向（mlx5_prism 更低）印证了 CMDQ 的并发特性。
```

---

## 8. 执行计划

### 按阶段分解

| 阶段 | 内容 | 产出物 |
|------|------|--------|
| S0 内核模块基础 | `hello.ko` → `mlx5_prism_init/exit` | 模块可加载/卸载 |
| S1 PCIe 探测 | `pci_device_id` → `mlx5_prism_probe()` → `pcim_iomap BAR0` | `lspci -k` 显示 mlx5_prism |
| S2 DMA + 中断 | `dma_alloc_coherent` (CMDQ mailbox + EQ) → `pci_alloc_irq_vectors` → `request_irq` | `/proc/interrupts` 能看到 mlx5_prism 中断计数 |
| S3 CMDQ 引擎 | mailbox 数据结构 → doorbell 写 → spin_lock → EQ handler 读响应 → completion | 完整 CMDQ 往返：issue → doorbell → MSI-X → complete |
| S4 固件引导 | poll iseg → ENABLE_HCA → QUERY_PAGES → MANAGE_PAGES → QUERY_HCA_CAP → INIT_HCA | **M1 里程碑：HCA OPERATIONAL** |
| S5 PHY 诊断 + 冒烟测试 | ACCESS_REG PPCNT → sysfs → CREATE_QP → WQE → doorbell → CQE 验证 | sysfs PHY 可读 + 硬件自检通过 |
| S5.5 最小 netdev 骨架 | `register_netdev` + `ndo_start_xmit`（SQ 单包）+ NAPI poll（RQ 单包 `napi_gro_receive`）+ `ndo_open/stop`（CMDQ 创建/销毁 RQ/SQ） | `ip link` 能看到 prism0 + 单包收发验证通过 |
| S6 收尾 | ftrace tracepoint 完善 → verify.sh → 双层对比报告 → bpftrace 基准采集 → **mlx5 架构总结文档** | **M2 里程碑：完整交付** |

### 关键里程碑

```
M0 (初始)              M1 (固件引导完成)      M2 (完整交付)
mlx5_prism.ko 可加载     HCA OPERATIONAL        18 项验收通过
BAR0 可读                QUERY_HCA_CAP 完成      双层对比报告
                         dmesg 完整引导日志
                         冒烟测试通过
```

**M1 验证**：
```bash
$ sudo rmmod mlx5_core && sudo insmod mlx5_prism.ko && dmesg | tail -15
[mlx5_prism] INIT_HCA complete, status=0
[mlx5_prism] HCA operational. max_qp=262144, num_ports=2, speed=25G
[mlx5_prism] Hardware datapath smoke test: PASSED
[mlx5_prism] CMDQ latency: 8.5μs avg (n=20)
```

**M2 验证**：
```bash
$ sudo ./verify.sh
ALL 18/18 PASSED
```

---

## 9. 验收标准（18 项）

### 必须全部通过

| # | 验收项 | 测试方法 | 通过条件 |
|---|--------|---------|---------|
| 1 | **模块加载/卸载零错误** | `insmod mlx5_prism.ko && rmmod mlx5_prism.ko` × 100 | `dmesg` 无 BUG/WARNING/Oops/panic |
| 2 | **驱动绑定** | `lspci -k -d 15b3:1015` | 显示 `Kernel driver in use: mlx5_prism` |
| 3 | **固件引导成功** | 加载后 `dmesg \| grep mlx5_prism` | 输出完整 7 步引导流程：`poll iseg` → `ENABLE_HCA` → `QUERY_PAGES` → `MANAGE_PAGES` → `QUERY_HCA_CAP` → `MANAGE_PAGES(init)` → `INIT_HCA` → `OPERATIONAL` |
| 4 | **HCA 能力集一致性** | mlx5_prism `QUERY_HCA_CAP` vs mlx5_core sysfs caps | 关键字段 (max_qp, num_ports, speed) bit-exact 一致 |
| 5 | **CMDQ 命令成功率** | `QUERY_HCA_CAP` × 1000 次 | 100% status=0 |
| 6 | **CMDQ avg 延迟** | ftrace/perf 采集 | ≤ 10μs（mlx5_core 基准 ~8μs） |
| 7 | **CMDQ p99 延迟** | ftrace/perf 采集 | ≤ 30μs（mlx5_core 基准 ~15μs） |
| 8 | **引导时间** | dmesg 第一条 poll 到 `INIT_HCA complete` 的时间戳差值 | ≤ 300ms（mlx5_core 基准 ~200ms） |
| 9 | **硬件冒烟测试** | 内置 `mlx5_prism_smoke()` 自动执行 | `dmesg` 显示 `Hardware datapath smoke test: PASSED`，CQE.opcode=MLX5_CQE_RESP_SEND, status=0 |
| 9a | **netdev 注册** | `ip link show prism0` | 显示 prism0 接口，state=DOWN |
| 9b | **单包发送（SQ 路径）** | ftrace `mlx5_prism:sq_xmit` | skb → DMA map → WQE → doorbell → CQE 回收 → `dev_kfree_skb` |
| 9c | **NAPI 收包（RQ 路径）** | ftrace `mlx5_prism:rq_poll` | CQE → skb 构造 → `napi_gro_receive` → `napi_complete` |
| 10 | **PHY sysfs 可读** | `cat /sys/class/misc/mlx5_prism0/pre_fec_ber` | 输出有效数值（量级 1e-15 ~ 1e-5） |
| 11 | **PHY 数据一致性** | `cat /sys/class/misc/mlx5_prism0/pre_fec_ber` vs `ethtool -S eth0` (mlx5_core 加载时) | 同数量级 |
| 12 | **PHY 告警触发** | 拔松光纤或衰减器 | `cat /sys/class/misc/mlx5_prism0/status` 从 `ok` → `degraded`，dmesg 有 WARNING |
| 13 | **ftrace 事件完整性** | `perf record -a -e mlx5_prism:* sleep 10 && perf script` | cmdq_issue 与 cmdq_complete 成对出现，零丢失 |
| 14 | **零内存泄漏** | 100 次 load/unload 前后对比 `/proc/meminfo \| grep Slab` | Slab 增量 = 0 |
| 15 | **mlx5_core 切换** | `rmmod mlx5_prism && modprobe mlx5_core` 往返 20 次 | 双方每次都能正常加载、正常工作 |

### 验证脚本

```bash
#!/bin/bash
# verify.sh —— mlx5_prism 一键验收

set -e
DEV="15b3:1015"  # ConnectX-4 Lx
PASS=0; FAIL=0

check() {
    local name="$1"; shift
    echo -n "[....] $name"
    if "$@"; then
        echo -e "\r[PASS] $name"
        ((PASS++))
    else
        echo -e "\r[FAIL] $name"
        ((FAIL++))
    fi
}

setup_mlx5() { modprobe mlx5_core 2>/dev/null; sleep 2; }

# 1: 加载/卸载 100 次
check "load/unload 100x" bash -c '
for i in $(seq 100); do
    insmod mlx5_prism.ko 2>/dev/null && rmmod mlx5_prism 2>/dev/null
done
dmesg | tail -100 | grep -q -v "WARNING\|BUG\|Oops\|panic"
'

# 2: 驱动绑定
setup_mlx5; rmmod mlx5_core; insmod mlx5_prism.ko
check "lspci binding" bash -c 'lspci -k -d '"$DEV"' | grep -q mlx5_prism'

# 3: 固件引导
check "FW bootstrap" bash -c 'dmesg | grep mlx5_prism | grep -q "INIT_HCA complete"'

# 4: HCA 能力集一致性
check "HCA caps match" bash -c '
grep -q "$(cat /tmp/mlx5_caps_max_qp)" /tmp/mlx5_prism_caps.txt
'

# 9: 硬件冒烟测试
check "HW smoke test" bash -c 'dmesg | grep mlx5_prism | grep -q "smoke test: PASSED"'

# ... (其余验收项类似)

echo "=== $((PASS+FAIL)) tests: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
```

---

## 10. 交付物清单

| # | 交付物 | 形式 |
|---|--------|------|
| 1 | **mlx5_prism.ko** | GitHub 源码，~2000 行 C |
| 2 | **dmesg 引导日志** | README 中截图/文本 |
| 3 | **15 项验收 checklist** | verify.sh + 验收表格 |
| 4 | **双层对比报告** | markdown 表格，含 bpftrace 采集的 mlx5 基准数据 |
| 5 | **CMDQ 延迟分布图** | ftrace 数据 → histogram |
| 6 | **固件引导状态机图** | ASCII 图 / draw.io |

---

## 11. 硬件环境

### 11.1 开发平台（sunrise-studio）

| 组件 | 型号/版本 | 状态 |
|------|----------|------|
| GPU | NVIDIA RTX 3080 Ti (12GB) | 驱动 580.126.09, CUDA 13.0 |
| RDMA NIC | Mellanox ConnectX-4 Lx [15b3:1015] 双口 | RoCEv2 已激活, 端口 ACTIVE |
| RDMA 栈 | libibverbs 39.0, rdma-core, mlx5_ib | 全部正常工作 |
| 内核 | 5.15.0-174-generic (Ubuntu 22.04) | — |
| 内核源码 | /home/zqc/code/linux (自编译 7.0.0) | mlx5 驱动源码 ~8000 行可参考 |
| nvidia-peermem | 模块已编译 (.ko 存在) | 加载失败 (Operation not permitted) |
| IOMMU | 未在启动参数中启用 | 缺少 `intel_iommu=on` |
| MFT 工具 | 未安装 | 无法查询/配置网卡固件 |

### 11.2 项目硬件需求匹配

| 项目 | 需要的硬件能力 | 当前环境 | 结论 |
|------|-------------|---------|------|
| mlx5_prism：CMDQ + 固件引导 + PHY 监控 | ConnectX-4/5 任意，PCIe 访问 | CX-4 Lx，RoCEv2 已激活 | ✅ 完全满足 |
| RoCEv2 协议栈（用户态） | RoCEv2 网卡 + DPDK | CX-4 Lx，libibverbs 正常 | ✅ 完全满足 |
| cudaHostRegister + RDMA | CUDA 卡 + RDMA 网卡 | RTX 3080 Ti + CX-4 Lx | ✅ pinned memory 路径满足 |
| GPUDirect RDMA（GPU 显存直通网卡） | CX-5+ + Tesla/Quadro | CX-4 Lx + RTX 3080 Ti | ❌ 三个硬性障碍 |

### 11.3 GPUDirect RDMA 硬件门槛

GPUDirect RDMA 允许网卡直接 DMA 到 GPU 显存，绕过 CPU 内存：

```
GPU 显存 --PCIe P2P--> NIC --RDMA--> 远端
        （不经过 CPU 和系统内存）
```

**硬件要求**（NVIDIA 官方文档）：
1. **网卡**：Mellanox ConnectX-5 及以上（ConnectX-4 Lx 不支持）
2. **GPU**：数据中心 GPU（Tesla/A100/H100），`nvidia_p2p_get_pages()` API 官方仅对数据中心 GPU 开放
3. **内核**：`CONFIG_PCI_P2PDMA` 已启用，IOMMU 已配置
4. **驱动**：`nvidia-peermem.ko` 已加载

**pinned memory 替代路径**（当前硬件支持）：
```
GPU 显存 -> cudaMemcpy -> CPU pinned memory -> ibv_post_send RDMA -> 远端
```
虽然没有绕过 CPU，但 pinned memory 避免了内核的 `copy_from_user`，且 `cudaMemcpy` 走 PCIe 带宽（~16 GB/s），对绝大多数场景够用。

### 11.4 环境验证命令

```bash
# 确认 RDMA 栈正常
ibv_devinfo | grep -E "hca_id|link_layer|state"

# 确认 RoCEv2 端口激活
rdma link show

# 确认 nvidia-peermem 状态（CX-4 Lx 消费卡上预计加载失败）
lsmod | grep nvidia_peermem
dmesg | grep peermem

# 确认 PCI P2P 支持
cat /boot/config-$(uname -r) | grep PCI_P2PDMA
```

### 11.5 远期硬件（暂不购入）

| 设备 | 用途 | 触发条件 |
|------|------|---------|
| Mellanox ConnectX-5 | GPUDirect RDMA 真机验证 | RoCEv2 协议栈完成后，需要验证 GPU 显存直通时再买 |
| Intel E810-XXVDA2 | e810_prism（VFIO 用户态 Admin Queue 引擎） | 完成 mlx5_prism 后，需要证明跨厂商控制面理解时再买 |

不需要对端设备。不需要光纤连接。mlx5_prism 是纯控制面——固件引导、PHY 寄存器读取和硬件冒烟测试都不需要 link up。


## 12. 与原始方案对比

| 维度 | 原方案 | 新方案 |
|------|--------|--------|
| 模块名 | `prism.ko` | **`mlx5_prism.ko`** |
| 定位 | 精简以太网驱动 | 智能网卡控制面协议实现 + 可观测性框架 |
| 数据面 | TX/RX/NAPI/netdev/ethtool | **无**（内核态冒烟测试除外，验证后即销毁） |
| 固件引导 | "假设 mlx5_core 已初始化固件" ✗ | 从零实现 7 步引导 ✓ |
| CMDQ | "不做"（列为排除项） | **核心卖点** |
| PHY 监控 | 假设直接 MMIO 读 | ACCESS_REG 固件命令 |
| 验证方式 | ping/iperf | **三层验证**：能力集 bit-exact + PHY 交叉验证 + 硬件冒烟测试 |
| 对标 | 自说自话 | **mlx5_core 双层对比框架**（bpftrace 采集基准） |
| libibverbs 对接 | 不涉及 | 不需要（mlx5_prism 不暴露用户态接口） |
| VFIO | 不涉及 | 不需要（内核模块不走 VFIO；Mellanox UAR 天然不需要 VFIO） |
| 代码量 | ~1600-1900 行 | ~1800-2200 行 |
| 验收 | 15 项（含 ping/iperf） | 15 项（含硬件冒烟测试 + mlx5 对比验证） |
| E810 扩展 | "同一套 CMDQ 框架适配" ✗ | 独立项目 **`e810_prism`**（VFIO 用户态程序，技术栈不同但定位平行） |
