# mlx5_prism 设计文档 v2.2 —— ConnectX-4 Lx 控制面驱动

> 内核模块名：`mlx5_prism.ko`
> 从零实现 ConnectX-4 Lx 的固件-驱动控制面协议：CMDQ 引擎 + 9 步固件引导 + EQ 事件 + PHY 诊断 + 最小 netdev 骨架 + ftrace 全链路追踪。全部指标与 mlx5_core 对照验证。
>
> **v2 修订说明（2026-09-15）**：v1（2026-07 初稿）约三分之一内容已被硬件实证推翻或缺口。v2 是全量重写：修正 doorbell 写法（`iowrite32be(1<<idx)`，非 writeq）、补全引导序列（ISSI/字节序/SET_HCA_CAP）、新增此前完全缺失的 EQ 与事件设计章节、新增怪癖档案与固件状态机两个 mlx5_core 没有的层、修订被"事件提交长尾"发现推翻的延迟目标。所有标〔实证〕的数据来自 task.json/progress.md 记录的上机实验。
>
> **v2.1 修订说明（2026-09-15）**：§1.2 旧公理"任何抽象不允许为优雅而加层"作废，代之以 §1.7 设计价值观（简单而优雅 + 抽象准入三问）；§5.5 澄清结构体覆盖层"未采用是纪律、非禁令"。零代码影响——该公理在代码中零引用。
>
> **v2.1 内容修订（2026-09-17，T10-prep 判决）**：§7 全章翻案改写——T09 的"1 条目 EQ / ~10% 激活 / 中断不点火"三结论均系驱动侧 eqc 编码错位（固件忠实按 ifc 语义解读）；ifc 编码转正（读回改真窗口解析），§5.5/§6/§12 同步。

---

## 1. 项目定位

### 1.1 一句话定位

**mlx5_prism 是一个轻量级控制面驱动 + 固件行为档案**：它实现"主机怎么命令智能网卡"这一层（PCIe 探测、CMDQ、固件引导、事件、PHY 诊断），并把这过程中硬件**真实行为**（而非文档宣称的行为）作为一等公民记录在代码里。

### 1.2 与 mlx5_core 的五个本质区别（不是简单重复）

| # | 维度 | mlx5_core | mlx5_prism |
|---|------|-----------|-----------|
| 1 | **怪癖档案层** | 无——假设固件符合 PRM 文档 | `fw_profile` 结构体：known-good caps、提交长尾（T11 复测中）等实验发现命名化、集中化；已翻案项（1 条目 EQ/激活率，T10-prep 证明系自身编码产物）随判决撤档 |
| 2 | **固件接口状态机** | 隐式（散在各命令的错误处理里） | 显式 `fw_state` 枚举 + 每命令门禁查表——固件自己有状态机（QUERY_HCA_CAP 在 boot pages 前被拒 0x2B69B6〔实证〕），驱动侧镜像它 |
| 3 | **命令编码** | mlx5_ifc.h 生成头（数万行） | ~300 行微型 bit 级 accessor + 只覆盖 prism 用到的命令的偏移表 |
| 4 | **可观测性** | debugfs 命令统计（事后查看） | tracepoint 为主调试接口，先于事件模式落地（T06 两次死机无现场的直接教训） |
| 5 | **资源管理** | 手动 request/release + goto 链 | devres 全程（pcim_*/devm_*），统一有序 teardown 函数 |

规模预算：**总量 ≤ 4500 行**（当前 v0.26 约 2100 行 + 剩余任务约 1770 行 + 重构净增 ~400 行 + 余量）。预算防的是 mlx5_core 式的框架税，不防封装——行数以读者的总负担计，不以全局行数计（一个让三处调用各短十行的辅助函数，总行数持平，读者负担减半）。判据见 §1.7。

### 1.3 mlx5 内核栈中的位置

```
用户态程序 (DPDK / RDMA 应用)
───────
ib_uverbs / mlx5_ib            ← RDMA 栈（prism 不碰）
───────
mlx5_core                      ← 生产驱动（prism 的对照基准，运行时互斥）
───────
mlx5_prism.ko  ← 替换 mlx5_core 的底部：PCIe 探测、CMDQ、固件引导、事件、PHY 诊断
```

prism 是终点模块——上游只有内核基础设施（PCI 框架、sysfs、ftrace、netdev 框架），下游是 PCIe 总线上的固件。不提供 verbs、不做完整数据面转发（GRO/GSO/TSO/multiqueue/RSS/ethtool offload 是 mlx5_core 的成熟代码，重写无 ROI）；但做**最小 netdev 骨架**（register_netdev + ndo_start_xmit 单包 + NAPI poll 单包，~350 行）以覆盖 Linux 网络子系统的完整 API 链路。

### 1.4 上游/下游接口全景

```
mlx5_prism 的上游                     mlx5_prism 的下游
──────────────────                   ──────────────────
Linux PCI 子系统                       PCIe 总线
  pci_register_driver                   │ BAR0 MMIO（iseg 寄存器 / doorbell）
  → 探测 15b3:1015（双口）              │ dma_alloc_coherent 共享内存
                                        │ （cmdq 页 / EQ 环 / 邮箱链）
sysfs                                   │ MSI-X 中断（固件→驱动）
  /sys/class/misc/mlx5_prism0/          ▼
                                       固件（ARM 核，FW 14.32.1908〔实证〕）
ftrace                                    │
  prism:cmdq_issue / cmdq_complete        ▼
  prism:eq_event / sq_xmit / rq_poll    硬件（HCA / PHY / 交换矩阵）

netdev 框架（最小骨架：prism0）
printk / dmesg（分级：生产路径 info，自检 verbose）
```

### 1.5 运行时互斥

mlx5_prism 与 mlx5_core 不能同时加载（同一 PCI 设备只能绑定一个驱动）：

```bash
sudo modprobe -r mlx5_ib mlx5_core   # 逆序卸载 Mellanox 栈
sudo insmod src/mlx5_prism.ko [selftest=1]
# ... 验证 ...
sudo rmmod mlx5_prism
sudo modprobe mlx5_core              # 回生产驱动
```

### 1.6 明确不做

| 不做 | 原因 |
|------|------|
| 完整数据面（offload/RSS/多队列） | mlx5_core 成熟功能，重写无 ROI |
| RDMA / verbs / libibverbs 对接 | 独立用户态项目；prism 不暴露 /dev 接口、不做 mmap |
| SR-IOV / VF / eswitch / flow steering | 超出控制面学习目标 |
| 固件镜像加载 | CX-4 固件在 Flash 上电自举，prism 只做引导协议 |
| VFIO | 永不需要——UAR 硬件隔离是 Mellanox 不依赖 VFIO 的根本原因（见 §5.6） |

### 1.7 设计价值观：简单而优雅（v2.1）

> v2 原公理"任何抽象不允许为优雅而加层——每个抽象必须是数据或查表"作废：它把优雅当成了嫌疑犯，而优雅从来是目标的一半。本节是修正后的表述。

**简单是内容，优雅是形式，两者同为目标：**

- **简单**：概念少而承重。一条读路径从 probe 到 doorbell 中途不需要记忆术——每个概念（slot/token/邮箱链/owner/双 doorbell）只在唯一一处定义、有唯一含义。
- **优雅**：代码形状映射问题形状。9 步引导读起来就是 9 步；固件状态门禁就是一张表；EQ 消费协议就是 §7.3 那段伪码的直译。读者从代码结构直接看到协议结构，不需要先解码代码结构。

**抽象准入三问**——一个抽象获准进入代码，只需通过三问：

1. **它有真实调用者吗？** 没有调用者的抽象是框架税，无论多优雅。
2. **删掉它，调用处会更难读吗？** 不会 → 删。简单的度量尺是调用处的阅读成本，不是全局行数。
3. **它只有一个实现、零配置旋钮吗？** 第二个实现出现之前，配置项是猜测出来的复杂度。

数据与查表仍是首选形态——不是因为别的形态被禁止，而是它们天生满足三问（有消费者、删了变难读、无旋钮）。小函数层同样欢迎：`complete_slots()`、`teardown(stage)`、`prism_set_bits()` 都是"为优雅而加的层"，也都是好层。被三问拦在外面的从来是"为对称而加的层"——空转的接口、单实现的基类、预防性的配置项。

---

## 2. 分层架构

### 2.1 四层模型

```
┌────────────────────────────────────────────────────────────┐
│ L3 服务层                                                   │
│   phy 监控(T12/T13)   冒烟测试(T14)   netdev 骨架(T15)      │
│   只调用 L2 的类型化命令 API，禁止触碰任何线格式字节          │
├────────────────────────────────────────────────────────────┤
│ L2 固件层                                                   │
│   fw 状态机（门禁查表）   caps 缓存   怪癖档案 fw_profile    │
│   引导/teardown 编排；对 L1 以下只提"语义命令"               │
├────────────────────────────────────────────────────────────┤
│ L1 传输层                                                   │
│   cmdq（命令发送+完成后端）   eq（事件接收+消费）             │
│   irq（MSI-X 向量）   页池（DMA 内存移交/回收）               │
│   全部线格式知识（字节偏移/大端/屏障）封闭在本层              │
├────────────────────────────────────────────────────────────┤
│ L0 硬件访问层                                                │
│   BAR0 映射   iseg 寄存器读写（BE）   doorbell MMIO          │
└────────────────────────────────────────────────────────────┘
```

**依赖规则（grep 可检查）**：
1. 依赖严格向下：L(n) 只能调用 L(<n)。L1 不得引用任何固件语义（不知道 ENABLE_HCA 是什么）；L2 不知道 sysfs/netdev 的存在。
2. **跨模块触碰 slot 只能走声明 API**：eq.c 消费事件后释放命令槽，必须调用 `mlx5_prism_cmdq_complete_slots()`，禁止直接操作 cmdq 内部位图（R0.3 落地）。
3. 线格式只允许出现在 L1（及 `mlx5_prism_fmt.h` 的偏移表）——`grep -rn "put_be\|prism_set_bits" src/mlx5_prism_fw.c` 等在 R0.2 后应只命中 accessor 调用。
4. L0/L1 的不变量注释是承重墙：搬运代码必须连同注释一起搬。

### 2.2 文件到层的映射

> 仓库重组（2026-09-17）起：全部源码在 `src/`（下表文件名均在 src/ 下），设计/阅读文档在 `docs/`，脚本按生命周期分 `scripts/{gate,baseline,test,archive}/`。

| 文件 | 层 | 职责 | 行数（v0.26） |
|------|---|------|------|
| `mlx5_prism_main.c` | — | module_init/exit | 49 |
| `mlx5_prism_pci.c` | L0 | probe/remove、BAR0、DMA caps、**启动编排**（调用 L2/L1 的公开序列） | 184 |
| `mlx5_prism.h` | 全层 | 共享定义：设备结构、iseg 内联访问、BE 助手、原型 | 298 |
| `mlx5_prism_fmt.h`（新，R0.2） | L1 | bit 级 accessor + 全部命令/结构偏移表 | ~300 |
| `mlx5_prism_cmdq.c` | L1 | 命令引擎：slot/token/邮箱链/doorbell/完成后端 | 527 |
| `mlx5_prism_eq.c` | L1 | EQ 生命周期 + 事件消费（轮询今天/中断 T10） | 436 |
| `mlx5_prism_irq.c` | L1 | MSI-X 向量分配 + handler | 86 |
| `mlx5_prism_fw.c` | L2 | 引导状态机、caps、页池、teardown、fw_profile | 492 |
| `mlx5_prism_trace.{h,c}`（新，R0.6） | 横切 | tracepoint 定义与实例化 | ~150 |
| `mlx5_prism_phy.c`（T12/T13） | L3 | ACCESS_REG PPCNT + sysfs + 定时器 | ~450 |
| `mlx5_prism_smoke.c`（T14） | L3 | CREATE_QP→RTS→WQE→CQE | ~300 |
| `mlx5_prism_netdev.c`（T15） | L3 | 最小 netdev + NAPI | ~350 |

横切关注点：tracepoint（R0.6 起插桩 L1 关键路径）、日志规范（`pr_fmt` 前缀；生产路径 pci_info，自检路径额外 verbose）、黑匣子记录器（验证脚本内建，落 /var/tmp 逐行 sync）。

### 2.3 probe 时序（自检默认关闭，R0.5 起）

```
probe
 ├─ pcim_enable_device + BAR0 iomap + iseg 合法性检查          [L0]
 ├─ cmdq_alloc（4KB 描述符环）→ eq_alloc（64 页环）→ irq_alloc（2 向量，mask）
 ├─ cmdq_init（读 log_sz/stride，发布环 DMA 地址到 iseg）       [L1]
 ├─ fw_bootstrap（§4 状态机）                                   [L2]
 ├─ eq_setup（ALLOC_UAR → CREATE_EQ → QUERY_EQ 读回 → arm）     [L1]
 └─ [selftest=1] cmdq/mailbox/EQ 自检（生产路径不含）            [横切]
remove / 错误回滚
 └─ prism_teardown(stage)：§4.4 单一有序函数
```

---

## 3. 核心数据结构

### 3.1 设备结构（R0.5 重组后）

```c
struct mlx5_prism_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;
    struct mlx5_prism_iseg __iomem *iseg;   /* L0 */

    struct mlx5_prism_cmdq cmdq;    /* L1：slot 位图 + token + mode + 一把 spinlock */
    struct mlx5_prism_eq   eq;      /* L1：cons_index + eqn/uar + events 计数 */
    struct mlx5_prism_irq  irq;     /* L1：vector[2] + 原子计数 */

    struct mlx5_prism_fw fw;        /* L2：state + func_id + 页池 + caps 缓存 */
    struct mlx5_prism_fw_profile profile;  /* L2：怪癖档案（§6） */
};
```

与 v1/v0.26 的差异：
- **`fw.pages[8192]` 改动态分配**——v0.26 把 128KB 指针数组内嵌在设备结构里；R0.5 起首次 supply_pages 时按需分配（上限仍 `MLX5_PRISM_MAX_FW_PAGES=8192`，防固件失控）。
- **新增 caps 缓存**——v0.26 QUERY_HCA_CAP 结果打印后即丢弃，T12/T13 每次都要重查；R0.5 起引导时缓存 max/cur 两份。
- **`fw.state`**——显式固件接口状态（§4.1）。
- **`profile`**——怪癖档案（§6）。

### 3.2 锁地图（为 T10/T13/T15 预设计）

| 资源 | 保护者 | 上下文 | 说明 |
|------|--------|--------|------|
| cmdq slot 位图 + token | `cmdq->lock`（spin_lock_irqsave） | probe / 自检 / **T10 硬中断** | 唯一一扇跨中断锁；eq.c 只经 `cmdq_complete_slots()` 进入 |
| eq.cons_index | 单写者 | 今天进程上下文轮询；T10 起硬中断 | T10 后轮询路径仅作恢复手段（模块参数），两写者不同时存在 |
| fw.state / 页池 | probe 上下文单线程 | — | T13 定时器只读 profile + caps 缓存，不改 fw 状态 |
| phy 状态（T13） | `phy->lock`（自增） | 定时器 vs sysfs 读 | 拷贝后释放锁再发命令 |
| NAPI vs xmit（T15） | netif_tx_lock / napi 禁调度 | — | 沿用内核惯例，不发明新锁 |

---

## 4. 固件引导状态机

### 4.1 接口状态与门禁表

固件自身有分级状态机：命令在错误的状态下发出会被干净拒绝〔实证：QUERY_HCA_CAP 在 boot pages 前发出 → status 0x4 / syndrome 0x2B69B6〕。驱动侧镜像这一状态机，每条命令声明所需状态，`cmd_exec` 查表把关（R0.4 落地）：

```
PRISM_FW_RESET ──ENABLE_HCA──→ PRISM_FW_ENABLED
      ──QUERY_PAGES(boot)+give──→ PRISM_FW_BOOT_PAGES
      ──SET_HCA_CAP──→ PRISM_FW_CAPS_SET
      ──QUERY_PAGES(init)+give+INIT_HCA──→ PRISM_FW_OPERATIONAL
      ──TEARDOWN_HCA──→ PRISM_FW_CAPS_SET（收页后回 RESET）
```

| 命令 | 所需状态 | 实证依据 |
|------|---------|---------|
| ENABLE_HCA (0x104) | RESET | 首条命令，T05 |
| QUERY_ISSI (0x10A) / SET_ISSI | ENABLED | T06b：boot pages 前唯一合法 >16B 命令 |
| QUERY_PAGES (0x106) boot=1 | ENABLED | T06a |
| MANAGE_PAGES (0x107) give boot | ENABLED → BOOT_PAGES | T07（6 页） |
| QUERY_HCA_CAP (0x100) | **BOOT_PAGES** | T06b 实证门禁 0x2B69B6 |
| ACCESS_REG HOST_ENDIANNESS (0x805/0x7004) | BOOT_PAGES | T07 |
| SET_HCA_CAP (0x101) | BOOT_PAGES → CAPS_SET | T08 |
| QUERY_PAGES init / MANAGE_PAGES give init | CAPS_SET | T08（2299 页） |
| INIT_HCA (0x102) | CAPS_SET → OPERATIONAL | T08（230/239ms） |
| ALLOC_UAR / CREATE_EQ / QUERY_EQ / DESTROY_EQ / DEALLOC_UAR / NOP / GEN_EQE | OPERATIONAL | T09 |
| TEARDOWN_HCA (0x103) | OPERATIONAL | T08 |
| MANAGE_PAGES take | BOOT_PAGES..OPERATIONAL | T07/T08 回收 |

门禁不匹配 → 驱动侧直接 -EINVAL 并打印"state=X 不接受 opcode=0x…（需 Y）"，不发上机命令。这把"哪个命令哪个阶段能发"从注释/踩坑记忆变成一张可检查的表。

### 4.2 引导序列（9 步，取代 v1 的 7 步）

v1 缺 ISSI、字节序、SET_HCA_CAP 三步，页数与实测不符。实证序列：

```
 1. wait_fw_init      轮询 iseg->initializing bit31 → 固件自举完成（≤2s）
 2. cmdq_init         读 log_sz=5/log_stride=6〔实证〕，发布描述符环 DMA 地址
 3. ENABLE_HCA        function 级开关
 4. SET_ISSI          接口版本协商（QUERY 出 112B 整，outlen 不足 → 0x51〔实证〕）
 5. boot pages        QUERY_PAGES(boot)=6 页〔实证〕→ MANAGE_PAGES give 6
 6. 字节序 + caps 探针  ACCESS_REG HOST_ENDIANNESS；QUERY_HCA_CAP(max/cur)
                       8 块邮箱链 4112B 读回（多块链验证点）
 7. SET_HCA_CAP       known-good profile 补丁（§6.2，字节见 fw.c）
 8. init pages        QUERY_PAGES(init)=2299 页〔实证〕→ 单条 give 2299
 9. INIT_HCA          → OPERATIONAL（实测 230-277ms，mlx5_core 对照 ~265ms）
```

### 4.3 两形态 teardown（T07/T08 实证）

- **完整卸载**（INIT_HCA 成功后）：TEARDOWN_HCA 停止固件对 init 页的 DMA 消费 → TAKE 回收（≤63 页/条，实测 2305/2305 = 6 boot + 2299 init）→ DISABLE_HCA。
- **仅回收**（引导中途失败，HCA 未 INIT）：直接收 boot 页 + DISABLE，不发 TEARDOWN_HCA（对齐 mlx5_core 失败路径）。
- 顺序不可逆：先停消费者（TEARDOWN）、再回收所有权（TAKE）——反序是 DMA 版 use-after-free。

### 4.4 统一 teardown 函数（R0.5）

v0.26 的拆卸顺序在 pci.c 手写了三遍（probe 错误路径两处 + remove 一处）。R0.5 收敛为：

```c
void mlx5_prism_teardown(struct mlx5_prism_dev *dev, enum prism_teardown_stage stage);
/* stage ∈ {BELOW_EQ, BELOW_FW, FULL}，按启动逆序：eq_teardown → fw_teardown
 * → irq_free → eq_free → cmdq_free。错误路径从失败层调用，remove 调 FULL。 */
```

---

## 5. CMDQ 命令引擎

### 5.1 概念模型

CMDQ 是驱动与固件之间的共享内存命令协议（硬件定义，非软件选择）。主机侧一个 4KB 描述符环（32 槽 × 64B〔实证 log_sz=5/log_stride=6〕）+ 大端寄存器握手：

```
驱动（生产者）                         固件（消费者，经 PCIe DMA）
─────                                ─────
填 64B 描述符到 slot idx
  type/inlen/in_ptr/out_ptr/outlen
  token / sig=~xor8
  status_own = OWNER_HW（移交）
wmb()
iowrite32be(1<<idx, iseg->cmd_dbell)  ← doorbell：位掩码，32-bit BE
                                      取命令 → 执行 → 写响应回描述符/mailbox
                                      status_own 清 OWNER（交回）
轮询 status_own 或等 EQE 唤醒
rmb() → 读 out[] → 两层状态解析
```

**v1 勘误**：doorbell 不是 `writeq`——是 32-bit `iowrite32be(1 << idx)`（T05 实证 + mlx5_core cmd.c:1072 同款）；`wmb()`（非 `smp_wmb()`）在 doorbell 之前，因为消费者是经 PCIe DMA 的固件，不是另一个 CPU。

### 5.2 邮箱链（>16B 命令）

- 块 = 576B（512B data + 64B trailer）；trailer 字段序〔实证，错序 → delivery 0x2 TOK_ERR〕：`rsvd0[48] + next(be64) + block_num(be32) + rsvd1 + token + ctrl_sig + sig`。
- **块 DMA 地址必须 1KB 对齐**（错位 → delivery 0x4/0x5 拒收）〔实证〕——分配按 n×1024 步长。
- token 每命令递增（跳 0），链上每块 trailer 与描述符一致，防跨命令串扰。

### 5.3 完成后端（R0.3 接口化）

命令完成有两种推进方式，统一为一个接口：

```
阶段 A（现状，T09 语义）：
  invoke 轮询描述符 status_own 完成自身；
  slot 生命周期与 EQ 绑定——EQ 存活时 slot 保持 busy，
  由 EQE 的 cmd_vector 位图经 mlx5_prism_cmdq_complete_slots() 释放；
  EQ 死亡 → forget_slots 一次清空（合并进 complete_slots 的全置位语义）。

阶段 B（T10，2026-09-17 落地，镜像 mlx5_core 事件模式）：
  invoke 睡在完成量上；硬中断消费 EQE → vector → complete_slots()
  → test_and_clear(PENDING) 抢占者 complete() 唤醒。
  超时路径与真实完成的竞态由 PENDING 位 claim/revoke 裁决
  （对齐 mlx5_core cmd.c 的 MLX5_CMD_ENT_STATE_PENDING_COMP）。

  落地偏差（有意）：抢占者（硬中断）只 claim + complete，**不收尾**——
  读描述符输出、释放槽位、回收邮箱都由被唤醒的 invoke 执行。同步-only
  驱动的简化：硬中断永不触碰 issuer 的邮箱与栈生命周期；claim/revoke
  语义不变。另两条强制轮询例外（对齐 mlx5_core force_polling）：页命令
  （fw 投页完成到 pages 向量，本驱动无 pages EQ）与 CREATE/DESTROY/
  QUERY_EQ（自身 EQE 时序未定义）。`eq_mode=poll` 模块参数可整体退回
  阶段 A 行为（T09 语义逐字保留）。
```

`cmdq->mode` 由 eq 模块在创建/销毁时设置，cmdq 不再反向读取 `eq.created`（R0.3 解耦）。

### 5.4 两层错误分类学

| 层 | 位置 | 含义 | 已知值〔实证〕 |
|----|------|------|---------------|
| delivery status | `status_own >> 1` | 命令**未被投递执行**（格式/对齐/token 错，固件没跑它） | 0x2 TOK_ERR（trailer 字段序错）、0x4/0x5（块未 1KB 对齐）、0x1 sig err |
| command status | out[0] 高 8 位 + syndrome=out[1..4] BE24 | 命令**被执行但被拒**（参数/状态机/长度） | 0x4+0x2B69B6 状态机门禁、0x50/0x51 长度、0x4E2106 页 give 失败、0x408B33 未知 opcode |

检查顺序必须先 delivery 后 status；syndrome 是 out bytes[4:8] 的 32 位 BE（截断打印会少 8 位）。超时：slot 与邮箱链**故意泄露**——固件可能迟到写回，复用会别名（对齐 mlx5_core）。

### 5.5 类型化命令层（R0.2，`mlx5_prism_fmt.h`）

消灭散落在 fw.c/eq.c 的手工偏移拼装。API：

```c
/* bit 级 BE accessor：first_bit 从缓冲区 bit0（LSB-first）计数，
 * 与 mlx5_ifc 编号一致（如 eqc.log_eq_size = bit99 起 5 位）。
 * 字段 ≤32 位，可跨字节。仅控制面路径使用，不做热路径优化。 */
static inline void prism_set_bits(u8 *buf, u16 first_bit, u8 nbits, u32 val);
static inline u32  prism_get_bits(const u8 *buf, u16 first_bit, u8 nbits);
```

配套**命名偏移表**（唯一允许出现裸偏移的地方，每项注释实证来源）：

```c
/* create_eq_in（T09 实证；eqc 字段经 T10-prep 翻案后按 ifc 位号编码，
 * 见 §7.1——dw3=(log_eq_size<<24)|uar_page，dw5 低 12 位=intr） */
/* eqc 编码/解码走 prism_eqc_write_dw3()/set_intr()/_readback() 助手，
 * 真窗口 BE 算术直算；prism_get_bits 的 LSB-first 窗口与 ifc/PRM 的
 * MSB-first 窗口在字节内翻转，eqc 字段不得经它读写 */
#define PRISM_OFF_EQ_EVENT_BITMASK  72   /* payload 相对字节 */
#define PRISM_OFF_EQ_PAS            256
/* query_issi_out：112B 整（outlen<112 → 0x51）；supported_issi_dw0 @108 */
/* caps（T08）：log_max_qp c[19]bits3-7 / num_ports c[55] / cmdif_checksum c[66] ... */
/* manage_pages_in：func_id @10 / num_entries @12 / pas @16+8i；GIVE=1/TAKE=2 */
```

迁移纪律（R0.2）：fw.c → eq.c → cmdq 自检逐文件替换；**产出的命令缓冲区必须字节级不变**（基线 hexdump 对比验证）。禁止顺手"修正"任何偏移——发现疑似错误先上机实验再改。

结构体覆盖层（mlx5_ifc `_bits` 风格，字段名即文档）未被采用是重构期字节等价纪律所致，**非价值观禁令**（§1.7）；未来若引入，须以 static_assert 与偏移表互锁——偏移表仍是单一事实源，结构体只是它的编译期镜像。

### 5.6 UAR 与 doorbell

- EQ doorbell 地址 = BAR0 + uar_idx×4KB + 0x40（arm）/ 0x48（仅更新 ci）〔实证〕；值为 `(ci & 0xffffff) | (eqn << 24)` 大端裸写。
- 命令 doorbell = iseg 内 `cmd_dbell` 寄存器（§5.1）。
- QP doorbell（T14/T15）：BF/UAR 页每 QP 独立——这是 Mellanox 不需要 VFIO 的硬件根基：硬件在硅片层面校验 doorbell 页归属，非法主人写不进去。prism 学的控制面（CMDQ 创建 QP/分配 UAR）正是该机制的前提。

---

## 6. 怪癖档案（mlx5_core 没有的层）

### 6.1 设计动机

prism 与固件博弈的全部实验发现（14 轮 EQ 实验、多轮 caps 实验）目前一半活在代码魔法数、一半活在注释里。怪癖档案把它们变成**命名的数据**：boot 时按固件版本填充一次，消费者查表，不各自硬编码。换一张卡/一个固件版本，只改这一处。

```c
struct mlx5_prism_fw_profile {
    /* caps known-good 补丁（FW 14.32.1908 实证，字节见 §6.2） */
    bool caps_patch_apply;          /* SET_HCA_CAP 前应用 known-good 补丁 */
    u8   caps_log_max_klm;          /* 18：c[35] bits3-7 钳到 18 */
    bool caps_zero_cmdif_checksum;  /* c[66..67] = 0 */
    bool caps_clear_c82_bits01;     /* c[82] &= ~3（字段未识别，效果实证） */
    bool caps_zero_c162_163;        /* c[162..163] = 0（字段未识别） */

    /* EQ 行为（T10-prep 翻案后：1 条目环/~10% 激活/中断不点火均系旧
     * eqc 编码产物，字段降级为防御性恢复参数） */
    u8   eq_engagement_retries;     /* 3：静默时 destroy/recreate（编码修正后 10/10 首次激活，防御保留） */
    u32  eq_poll_patience_ms;       /* 2000：每事件提交等待窗（长尾在 1 条目环上实测，T11 复测） */

    /* 协议细节 */
    u16  query_issi_outlen;         /* 112 整，不足 → 0x51 */
    u32  cmd_timeout_ms;            /* 2000 */
};
```

**R0.4 落地纪律：只搬家，不改字节。** known-good 四处补丁、EQ 阶梯、双 doorbell 序列、耐心窗全部原样搬运并命名化；上机基线对比证明行为不变。

### 6.2 known-good caps 补丁（FW 14.32.1908）

SET_HCA_CAP 的语义在此固件上是**门闩而非字段写入**〔实证：SET 后 caps[post]==caps[cur] 逐字节相同，固件把 klm=18 以外的大部分写入钳回默认值〕。反面证据：按 v5.15 语义写 log_max_qp/pkey/log_uar_page_sz → 固件拒绝一切 init 页 give（0x4E2106），某轮还污染固件状态致后续命令 delivery 0x1。因此：

- 唯一必须的数值补丁：`log_max_klm ≤ 18`（c[35] bits3-7）。
- 清零补丁：c[66..67]（cmdif_checksum 区）、c[82] 低 2 位、c[162..163]——考古见 progress.md T08，两处字段含义未识别但效果实证。
- **禁止**写入 v5.15 语义字段（log_max_qp/pkey/log_uar_page_sz）。
- 字段最小化实验（哪个补丁可省）是独立上机实验课题，不在重构路径上。

---

## 7. EQ 与事件设计（v2 新增章节；v2.1 经 T10-prep 翻案改写）

> v1 完全没有本章。T09 的 14 轮硬件实验编码于此；2026-09-17 的 T10-prep
> 判别实验推翻了其中三个结论——**T09 记录的"固件怪癖"实为驱动侧 eqc
> 编码错位**（dw3 把 log_eq_size 写进 reserved 位、intr 从未落在字段内），
> 固件全程忠实。本章为翻案后版本；被推翻的原文见 progress.md 归档④⑥。

### 7.1 创建：ifc 位号编码 + 权威读回

- **编码（T10-prep 实证转正）**：eqc 按 mlx5_ifc/PRM 位号编码——dw3 = (log_eq_size<<24)|uar_page（字段 [99,103]/[104,127]），dw5 低 12 位 = intr（[180,191]），字节等价于 MLX5_SET 宏代数（宿主机证明）。**旧编码** dw3=((log&7)<<29)|uar 下，固件读到 log_eq_size = eqc[12]&0x1f = uar&0x1f = 0——**T09 的"1 条目环"是被字面请求的**；旧 intr 编码（be16 vec<<4 @eqc[16:18]）从未落在字段区间。
- **QUERY_EQ 读回为权威**（真窗口 eqc[12]&0x1f）：正确编码下读回 dw3=0x0c001000 逐字回显请求；消费者采纳读回尺寸（该纪律曾在编码错误时代默默救场）。
- 创建阶梯（log12/64pas → …）保留为防御性回退：rung0 自编码修正后 10/10 加载直接接受。
- owner 谓词 `(owner&1) ^ !!(ci&nent)` 是 nent-通用式（与 mlx5_core eq.c 同构）——T09 在 nent=1 上"碰巧成立"，T10-prep 已在 nent=4096 上验证。

### 7.2 事件投递模型（T10-prep 修订；T10 事件模式下复验成立）

- 每条命令（含 NOP 0x80D / GEN_EQE 0x304 探针）生成 type 0x0a（命令完成）事件，**按序投递到连续条目**——T09 的"全部投递到 entry 0"是 1 条目环的当然结果，不是独立怪癖。
- **数据先行、owner 后提交**（双 doorbell 序列）仍实证有效（T09）。提交时延分布待 T11 tracepoint 复测——T09 的 >500ms 长尾是在 1 条目环上测得的，是否随编码修正消失未决。
- **~10% 激活率：翻案为编码伪影**。正确编码下 10/10 加载首次激活（对照：旧编码裸激活当次抽样 0/2，历史 ~10%）；destroy/recreate 恢复机制降级为防御性保留。
- **中断真实点火**：正确 intr 绑定 + arm 后，MSI-X 计数 ≈ 事件数（10-11 : 10-11，stub handler 实测）；T09 的"中断不点火"系 intr 编码错位所致。
- **T10 事件模式复验（2026-09-17，真硬件）**：以上各项在硬中断消费下全部保持——双口 10/10 自检、events==hardirq 消费数（11/加载，聚合时 13）、中断 10-12 ≈ 事件数、stage1+stage2+100 压测零风暴零投诉、拆除全量回收。每 EQE 的 dmesg 行被移除（硬中断热路径零 printk，tracepoint 承担观测）；积压聚合（11↔13）为正常形态，归一化覆盖。

### 7.3 消费协议（事件模式为默认；轮询为 eq_mode=poll 回退）

```
事件模式（T10 起为默认）：
  create 末尾（中断仍掩蔽）：best-effort 排空积压 EQE（eq_poll(dev,0)
    单次 walk——观测用，不承重；EQE owner 何时翻转不受驱动控制）。
  invoke: doorbell → 置 pending → wait_for_completion_timeout
  硬中断 handler: 计数 → EQE 循环（owner 谓词 + dma_rmb + budget 128）
    → type 0x0a → complete_slots(vector): **owner 门卫**（固件先翻转
    slot owner 后投递 EQE，posted-write 有序 + dma_rmb 保证此可见序；
    EQE 到而 owner 仍 HW = 前任占用者的迟到 EQE 在冒名——跳过不唤醒，
    真完成的 EQE 还在路上。2026-09-18 两轮熔断实证的别名竞态，
    门卫与调度时序无关地关闭它）→ claim pending → complete()
    → 退出前一次 arm doorbell（+0x40）
  被唤醒的 invoke: 重读描述符输出 → 释放槽位（收尾在进程侧）
  防护：风暴护栏（连续 1000 空中断 → disable_irq_nosync 自保）
        超时竞态：pending 位 claim/revoke（抢到=超时泄漏，输=补收）
        位图别名：owner 门卫（force_polled 命令内联释放槽位但 EQE
        在途，迟到 EQE 的 vector 位会指向复用后的槽——门卫拦之）

轮询回退（T09 语义保留）：
  eq_poll():
    循环直至 idle 或超时（patience=2000ms）：
      walk ring 至 owner 失效：
        dma_rmb()（先见 owner 翻转，再读事件内容）
        type 0x0a → vector 位图 → cmdq_complete_slots(vector)
      写 +0x48（commit ci）→ 写 +0x40（arm），每轮都要，wmb 前置
    静默 → dump 环头 4 项 + owner 行（超时取证）
```

slot 协议不变量：EQ 存活期间，非页命令 slot 从 doorbell 起保持 busy，直到对应 EQE 的 vector 位到来；EQ 死亡 → 全部清除（页命令 slot 走内联生命周期，页命令固定占最高槽，防饿死——对齐 mlx5_core page_queue 语义）。owner 谓词与双 doorbell 序列在 nent=4096 环上验证不变（T10-prep）。

### 7.4 T10 事件模式（已落地，2026-09-17 硬件验证通过）

按"known-good first"纪律复刻 mlx5_core 官方形态（7.3 事件模式块为现行协议），验证记录：

- **stage1**（双口单载）：S1 事件模式 10/10 自检 ✓、S2 events==hardirq（11/加载，聚合 13）✓、S3 中断 10-12 ~ 事件 11-13 ✓、S5 零风暴零投诉 ✓（S4 拆除证据在黑匣子：DESTROY_EQ ok + 2305/2305 双口全量）。
- **stage2**：5/5 事件模式循环 + `eq_mode=poll` 回退对照通过（T09 轮询语义作为恢复手段保留）。
- **stress**：100/100 insmod/rmmod，零失败零投诉。
- **时序红利**：自检从轮询时代每命令烧满 2s 耐心窗（~18-20s/口）降至 **~6ms/口**——T11 遗留的"耐心窗掩盖提交时延"观察就此消解；P1' 事件口径（doorbell→唤醒）由现有 tracepoint 自动承载，T16 复测。
- **行为变更**：每 EQE 的 dmesg 行移除（热路径零 printk）；基线随转正重采（预期族白名单已扩）。

**验收（对照重定义）**：中断计数 >0 且与事件数可对账 ✓；事件模式往返成功率 ≥99%（实测 106/106）✓；轮询回退参数可用 ✓。

---

## 8. PHY 诊断子系统（T12/T13，沿用 v1 设计）

### 8.1 技术关键：PHY 寄存器不能 MMIO 直读

PHY 计数器（PPCNT, register ID 0x5008）不在 BAR 空间，必须经 `ACCESS_REG`（opcode 0x805）由固件代读——CMDQ 引擎的第一个 L3 服务场景。access_register_in：register_id @bytes[10:12]、argument @bytes[12:16]、op_mod 0=写/1=读〔实证 + mlx5_core port.c〕。每 dev 实例查 local_port=1（本卡双 PF，一 PF 一口；mlx5_core 同样硬编码）。

**字段语义勘误（v2.2，2026-09-26 T12 功课）**：`pre_fec_ber` 不是硬件字段——内核驱动无此名，BER 是 grp 0x16 的 `phy_symbol_errors/phy_received_bits` 之商，而 grp 0x16 需 PCAM 能力位（CX5+ 特性）。CX4 Lx 是 pre-PCAM 芯片，可用组为 0x0（IEEE 802.3，ethtool `rx/tx_packets_phy`/`rx_symbol_err_phy` 锚点）、0x1/0x2/0x5/0x10、0x12（物理层：symbol/sync_hdr/edpl_bip、fc_fec 每 lane、rs_fec 总量、link_down_events）。

**0x16 上机判决（2026-09-26，T12 双口实测，推翻"预期拒绝"）**：FW 14.32.1908 **接受** grp 0x16 且计数器活跃——`rx_bits=200e6/225e6`、`sym_err=0`、`corr_bits=0`。即 PCAM 能力位为 0（mlx5_core 因此从不读它）但固件照常维护并放行该组——**能力位谎报缺失，固件行为先行**（怪癖档案级事实）。T13 可直接用真分母推导 pre-FEC BER，线速×时间估算法降为 fallback。

**tslc/rx_bits 现象学（同次实测）**：两组计数器的 time_since_last_clear 在 PPCNT 读取时仅 7/8 ms，且 0x16 的 rx_bits 与 tslc 严格成 25.78125 Gbps 标称线率之比（200e6/7.76ms、225e6/8.73ms，双口独立吻合）——①计数器时钟在 prism 引导期间（触发点未定，候选 INIT_HCA/端口 PHY 重建）重启过，"跨驱动单调存活"未被证明，交叉对账以预登记弱形式（链路 down 全零锚点）通过；②即便链路 down，rx_bits 也按标称线率精确累积——BER 分母在无链路时依然有效。

```
定时器（默认 100ms，monitor_interval_ms RW）
  → access_reg(PPCNT, port=1, counter_group∈{0x0, 0x12})
  → 解析 symbol_errors / sync_headers_errors / fc_fec_corrected(4 lanes) /
    rs_fec_corrected / rs_fec_uncorrectable / link_down_events
  → caps 缓存辅助判断 + sysfs 导出 + 阈值状态机（T13 按 8.3 输入源推导 BER）
```

clr 恒为 0：prism 的读不得清零固件计数器（ethtool 交叉对账与 T13 采样都建立在计数器单调存活上）。

### 8.2 sysfs 接口（/sys/class/misc/mlx5_prism0/）

```
pre_fec_ber  symbol_error_rate  fec_corrected  fec_uncorrectable   (RO)
temperature  link_quality       status                              (RO)
monitor_interval_ms                                                 (RW)
```

misc 设备与 netdev 职责分离：netdev 是网络栈入口（T15），PHY 监控是诊断功能。

### 8.3 阈值状态机

```
ok ──BER≥1e-10──→ degraded ──BER≥1e-7──→ critical   （带滞回，防抖）
      dmesg WARNING              dmesg ERR
```

输入源（v2.2 勘误 + 上机判决）：BER 为推导量——**优先 grp 0x16 真分母（本卡固件实测可用，见 §8.1 判决）**，fallback 为 25G 标称线速 × 采样间隔估算分母、grp 0x12 symbol_errors 做分子（链路 up 时 PHY 持续发送，idle 符号计入，口径成立；链路 down 时状态机冻结在 ok——但注意 rx_bits 分母在链路 down 时仍有效累积）。sysfs 同时导出原始计数器，读者可自行换算口径。

交叉验证：mlx5_core 加载时 `ethtool -S` 同名计数器（`tx/rx_packets_phy`、`rx_symbol_err_phy`、`link_down_events_phy`）对账，V_prism ≥ V_mlx5（计数器单调存活）或同数量级（<5% 偏差仅适用于同时间窗双读，跨驱动切换用单调性）。链路 down 的实测注记：锚点全零时该判据为弱形式（§8.1 tslc 现象学——计数器时钟随引导重启）。

---

## 9. 可观测性

### 9.1 tracepoint 目录（R0.6 起插桩，先于 T10）

| 事件 | 插桩点 | 字段 |
|------|--------|------|
| `prism:cmdq_issue` | cmd_exec doorbell 前 | opcode / op_mod / slot / token |
| `prism:cmdq_complete` | 完成解析后 | opcode / delivery / status / syndrome / latency_ns |
| `prism:eq_event` | eq_poll 每事件 | eqn / type / vector / cons_index |
| `prism:sq_xmit` / `prism:rq_poll` | T15 | qpn/sqn、cqn、len |

v1 勘误：v1 草案里 TP_printk 引用了不存在的 `mlx5_prism_cmdq_opcode_name()`——v2 规定配套 ~30 行 `prism_opcode_name()` switch 表（只覆盖 prism 用到的 opcode），不引入未定义符号。

### 9.2 黑匣子记录器（标准组件）

任何新中断/MMIO 路径上机必配：`dmesg -w | stdbuf -oL tee /var/tmp/xxx.log`（/tmp 是 tmpfs 断电即失——T06 第一次现场就这么丢的），逐行落盘。验证脚本内建（scripts/gate/t09_verify.sh 已实现，头部注释 R0.1 修正）。

---

## 10. 验证框架

### 10.1 双层对比（结构沿用 v1，判据修订）

**一致性（必须 bit-exact 或同数量级）**：

| # | 指标 | prism 采集 | mlx5_core 采集 | 判据 |
|---|------|-----------|---------------|------|
| C1 | HCA 能力集 | QUERY_HCA_CAP（caps 缓存） | debugfs/mlx5_core dmesg | 关键字段 bit-exact |
| C2 | 固件版本 | iseg fw_rev（**无 QUERY_FW 命令**〔实证〕） | ethtool -i | 一致（14.32.1908） |
| C3 | 引导序列 | dmesg 逐步 | mlx5_core dmesg 对照 | 9 步一致（caps 差异记录） |
| C4 | PHY 计数器 | ACCESS_REG PPCNT（grp 0x12/0x0） | ethtool -S 同名计数器 | V_prism ≥ V_mlx5（单调存活；BER 为 T13 推导量，见 §8.1 勘误） |
| C5 | 页回收 | teardown 计数 | — | 2305/2305 全量 |

**性能（v1 的 P1/P2 按长尾发现修订）**：

| # | 指标 | mlx5_core 基准 | prism 实测（T11，n=1008/264） | 说明 |
|---|------|---------------|-----------|------|
| P1' | 命令描述符往返（doorbell→status_own） | p50 ~5µs（kretprobe 实测，n=20764） | 轮询口径：p50=76.5µs，p90=1.76ms，p99=238ms，max=239ms | 轮询 vs 事件口径，仅同数量级对比。轮询口径长尾确定性：每口 6 个慢样本（QUERY_HCA_CAP ~1.7ms、init 页三连 ~9-12ms、**INIT_HCA ~230-240ms**、CREATE_EQ ~1.8ms），系固件真实处理时间（轮询量子仅 10-20µs）。**T10 起事件模式为默认**：latency_ns 自动变为 doorbell→唤醒 口径，T16 复测 |
| P2' | 事件提交延迟（owner 翻转→EQE 消费） | — | p50=1.0µs，p90=105µs，p99=159µs，max=162µs | **判据C 裁决（预登记）：长尾消失**——1 条目环时代的 >500ms 长尾随 4096 深环消失，n=264 全配对零孤儿。典型 1µs=EQE 在 owner 翻转时已 staged；~105-160µs 为突发首条（QUERY_EQ 积压事件） |
| P3 | 固件引导时间 | ~265ms | ≤ 300ms（实测 258-277ms） | 不变（注：INIT_HCA 的 ~240ms 占大头，与 P1' 长尾同源） |
| P4 | 中断响应 | ~5µs | T10 后测 | 轮询阶段无意义 |

> v1 的"CMDQ avg ≤10µs / p99 ≤30µs"目标作废：该口径假设事件模式 + 无长尾，均与实证不符。诚实记录差异原因比强凑数字更符合项目原则。

### 10.2 验收项（18 项，v1 §9 修订）

v1 表沿用，修订三处：项 6/7 换成 P1'/P2' 口径；项 3 引导流程改为 9 步含 ISSI/SET_HCA_CAP；项 13 tracepoint 事件按 §9.1 目录核对。冒烟测试与 netdev 项（9/9a/9b/9c）不变。

### 10.3 verify.sh 与基线

- `verify.sh`（T16）：18 项一键验收，`selftest=1` 传参驱动。
- **基线机制（R0.1 起）**：`scripts/baseline/` 存档基线 dmesg（每口 cmdq/ISSI/页数/EQ/teardown 关键行）。重构每步与基线 diff 为空（时延/地址除外）才算通过——行为保持不变的机械化判据。

---

## 11. 执行计划

### 11.1 阶段与里程碑

| 阶段 | 内容 | 产出 |
|------|------|------|
| S0-S4（已完成，T01-T09） | 模块骨架→PCI probe→DMA/中断→CMDQ→引导→EQ 轮询 | v0.26，M0/M1 达成 |
| **S4.5 R0 重构冲刺**（本次） | 基线→类型化命令层→完成后端解耦→状态机+怪癖档案→设备结构/teardown/自检分离→tracepoint | v0.27-v0.3x，**行为不变** |
| S5 事件与服务 | T11'(trace 完善) → T10'(事件模式) → T12/T13(PHY) → T14(冒烟) → T15(netdev) | M2 前置全部就绪 |
| S6 收尾 | T16(verify.sh+报告) → T17(架构总结) | **M2** |

**顺序决策：tracepoint（R0.6/T11'）先于事件模式（T10'）**——T06 两次死机的直接教训是"无观测就上新路径"；T10 调试必须先有观测手段。

### 11.2 R0 任务清单（task.json 同步）

| 任务 | 内容 | 验收 |
|------|------|------|
| R0.1 | 基线采集（≥20 循环）+ 修 t09_verify.sh 头注释 | scripts/baseline/ 存档 |
| R0.2 | `mlx5_prism_fmt.h` + fw.c/eq.c/cmdq 自检迁移 | 命令缓冲字节不变（hexdump 对比）；偏移只存在于偏移表 |
| R0.3 | `cmdq->mode` + `complete_slots()` 接口，eq↔cmdq 解耦 | eq.c 无 cmdq 内部触碰；基线不变；100× 装卸 |
| R0.4 | fw_state 门禁表 + fw_profile 命名化 | 魔法字节全部命名；基线不变 |
| R0.5 | pages 动态分配 + caps 缓存 + 统一 teardown + `selftest=1` | probe 默认路径无自检；teardown 单点；100× 装卸（带/不带 selftest） |
| R0.6 | tracepoint 脚手架 + 插桩 | perf list 可见；issue/complete 配对；零行为变化 |

R0 安全规则：基线 diff 必须为空；绝不原地改写协议字节（known-good 补丁/阶梯/双 doorbell/邮箱字段序原样搬+注释随迁）；任何行为偏差 → 停止、二分、上机重测。

---

## 12. 硬件环境（沿用 v1，更新实证数据）

| 组件 | 型号/版本 | 备注 |
|------|----------|------|
| 网卡 | Mellanox ConnectX-4 Lx `15b3:1015`，双口 25GbE，PCI `08:00.0/.1` | BAR0 32MB；FW 14.32.1908，cmdif_rev 5 |
| 内核 | 运行 5.15.0-174-generic（Ubuntu 22.04）；源码树 7.0.0（/home/zqc/code/linux） | mlx5_core 源 ~8300 行作对照 |
| GPU | RTX 3080 Ti（分析 CUDA/RDMA 交互用） | GPUDirect RDMA 需 CX-5+，本卡不支持（不影响 prism，纯控制面） |
| RDMA 栈 | libibverbs 39.0 / rdma-core / mlx5_ib | 与 prism 运行时互斥 |
| MFT 工具 | 未安装 | syndrome 考古依赖社区表 + 自制探针 |

关键实测常数速查：boot 页 6 / init 页 2299 / teardown 2305 全回收；引导 230-277ms；命令往返 72-173µs；EQ log12/4096 条目（ifc 编码，10/10 接受，读回逐字回显）；事件耐心窗 2000ms（长尾 T11 复测）；MSI-X 2 向量（索引 1 = cmd EQ，v4.19 MLX5_EQ_VEC_CMD 约定，中断 ≈1 次/事件已实测）。

---

## 附：v1 → v2 修订登记

| v1 位置 | v1 内容 | v2 处置 | 依据 |
|---------|--------|---------|------|
| 文档头 | "不注册 netdev" | 改为最小 netdev 骨架（与 §1/验收 9a-9c 一致） | v1 自相矛盾（§1 说做、头部说不做） |
| §3 | `writeq(cmd_head, doorbell)` | `iowrite32be(1<<idx, &iseg->cmd_dbell)` | T05 实证 + mlx5_core cmd.c |
| §4 | 7 步、boot=1 页、init=4 页、无 ISSI/字节序/SET_HCA_CAP | 9 步、6/2299 页、补全三步 | T06b/T07/T08 实证 |
| §2 | 数据结构（cmd_head/resp_head 等不存在字段） | 按 v0.26 实际 + R0.5 重组重写 | 代码现状 |
| 无 | EQ/事件设计 | 新增 §7 | T09 十四轮实验 |
| 无 | 怪癖档案/状态机 | 新增 §6/§4.1 | 架构重设计 |
| §7 P1/P2 | CMDQ ≤10µs/30µs | P1'/P2' 口径修订 | 事件提交长尾实证 |
| §3 tracepoint | 未定义的 name() 辅助函数 | 规定 prism_opcode_name() switch 表 | 编译可行性 |
| §5 | 冒烟测试 writeq(BF doorbell) | T14 实施时按 mlx5_core BF 语义重新核准 | 预防性勘误 |

---

## 附：v2 → v2.1 修订登记（2026-09-15）

| v2 位置 | v2 内容 | v2.1 处置 | 依据 |
|---------|--------|-----------|------|
| §1.2 末 | "任何抽象不允许为优雅而加层——每个抽象必须是数据或查表，不是框架" | 公理删除；规模预算注脚改为"防框架税不防封装，行数以读者总负担计" | 需求澄清：优雅是目标的一半而非嫌疑犯；公理全仓零代码引用，属修辞级过度矫正 |
| §1.7（新增） | — | 设计价值观：简单（内容）× 优雅（形式）+ 抽象准入三问；数据/查表为首选形态的理由重述（天生过三问，非唯一合法形态）；正例引 `complete_slots()`/`teardown(stage)`/`prism_set_bits()` | 取代旧公理的判据 |
| §5.5 | （无覆盖层立场） | 补：结构体覆盖层未采用是重构字节等价纪律，非禁令；引入须与偏移表 static_assert 互锁 | 消除旧公理的隐性外延（"硬编码 vs 结构体提示"讨论的结论） |

**v2.1 内容修订（2026-09-17，T10-prep 判决）**：

| 位置 | 原内容 | 处置 | 依据 |
|------|--------|------|------|
| §7 全章 | 1 条目环（"固件说谎"）/ ~10% 激活 / 中断不点火 | 翻案：均为旧 eqc 编码产物，固件忠实；ifc 编码转正（dw3=(log<<24)\|uar、dw5=intr），事件按序落环、10/10 首次激活、MSI-X ≈1 次/事件 | T10-prep 判别实验：判据 A/B/D 成立、E 对照通过、C（长尾）未决 T11 复测（progress.md T10-prep） |
| §5.5 | eqc 偏移示例（含 intr@92 旧笔误） | 改为编码/解码助手描述 + LSB/MSB 双窗口翻转警告 | mlx5_ifc.h 原文核准 + 硬件实证 |
| §6.1 / §12 | profile EQ 字段、常数速查 | 激活重试降级为防御参数；EQ 常数更新（log12/4096 条目，10/10 接受） | 同上 |

---

## 附：v2.1 → v2.2 修订登记（2026-09-26，T12）

| 位置 | 原内容 | 处置 | 依据 |
|------|--------|------|------|
| §8.1 | "解析 pre_fec_ber / symbol_error_rate ..." | 勘误：pre_fec_ber 非硬件字段（内核无此名）；BER 由 grp 0x16 推导且 0x16 需 PCAM（CX5+）；CX4 Lx 可用组 0x0/0x12 等；BER 推导移交 T13（真分母或线速×时间估算）；补 clr=0 不变量与 op_mod/argument 布局 | task.json 判据冲突发现：mlx5_ifc.h v5.15 全量核查 + en_stats.c PCAM 门卫（v5.15 :754）|
| §8.1/8.3（上机回填） | "0x16 探测预期拒绝" | 判决：FW 接受 0x16 且计数器活跃（能力位谎报缺失、固件行为先行）——T13 用真分母；另记 tslc/rx_bits 现象学（引导期计数器时钟重启 + 链路 down 时 rx_bits 仍按线率累积） | T12 上机 2026-09-26，/var/tmp/t12_run.log |
| §8.3 | "差值 <5% 通过" | 交叉验证改单调性口径（跨驱动切换计数器存活）+ BER 输入源标注 | 同上 |
| §10.1 C4 | "PHY BER / 同数量级" | 改 PHY 计数器 + V_prism ≥ V_mlx5 | 同上 |
