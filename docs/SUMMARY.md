# SUMMARY.md —— mlx5 控制面架构总结（T17，知识沉淀）

> 本文是 17 个 task（~7200 行手写驱动、六十余趟上机判决）的学习成果总纲，定位是**知识沉淀 + 面试弹药**。
> 它不重复 READING.md 的"怎么读代码"，回答的是"学到了什么"：五大机制各自解决什么问题、
> 关键不变量是什么、硬件上实证到了什么。每节末附面试话术。
> 读者验收标准：读完 §6 的自检清单，不看正文能答出十之八九。

所有 `file:line` 引用均对当前 `src/` 树逐一核对（2026-10-02）。

---

## §0 三十秒电梯演讲 + 全景图

**三十秒版**：ConnectX-4 Lx 上有一颗 ARM 核在跑固件，主机看不见它的内存，只能通过 BAR0
里的一块共享寄存器区（init segment）握手。主机在 DMA 内存里摆一个 64 字节描述符
（命令放不下就挂 576 字节的邮箱块链），然后往 doorbell 寄存器写一下自己的门牌号，
固件经 PCIe DMA 把命令读走、执行、把响应写回同一个描述符，并翻转所有权位交还；
有 Event Queue 之后，这次完成还会以中断形式通知主机。开机时这套机制先把 2305 页主机
内存的所有权交给固件（引导），之后数据面用同样的"描述符 + doorbell"模式发送报文
（WQE/CQE）。doorbell 寄存器被 UAR 硬件按页隔离，所以用户态进程可以直接敲自己的
doorbell 而不需要 VFIO。

```
 insmod
   │
   ▼
 PCI probe (pci.c:80) ──── BAR0 32MB ────► init segment：fw_rev / initializing / cmdq_addr
   │                                        cmd_dbell@+0x18
   ▼
 CMDQ 引擎 (cmdq.c:215)                    ────── §1 CMDQ 协议
   │   64B 描述符 + 576B 邮箱链 + doorbell + OWNER 位交接
   ▼
 固件引导 9 步 (fw.c:509)                   ────── §2 引导状态机
   │   ENABLE_HCA→ISSI→字节序→boot 页×6→caps→SET_HCA_CAP→init 页×2299→INIT_HCA
   ▼
 EQ + MSI-X (eq.c:244, irq.c)              ────── §1 之后半 + §5 doorbell 几何
   │   EQE 环 + owner 圈奇偶 + arm/publish 双 doorbell
   ▼
 数据面对象链 (smoke.c / net.c)             ────── §3 数据面
   │   PD→MKEY→CQ→TD→TIS→SQ/RQ→FT/FTE→netdev(NAPI)
   ▼
 ping 回环字节比对全过 (T15) / verify.sh 18/18 (T16)

 DMA 一致性横切全部层次                     ────── §4
 doorbell 与 UAR 隔离贯穿始终               ────── §5
```

一句话定位：prism ≈ mlx5_core 底部三层的最小可读重写（~7300 行 vs mlx5_core 数万行），
外加一个 mlx5_core 没有的层——怪癖档案（§6）。

---

## §1 CMDQ 协议：主机如何对固件下命令

### 1.1 概念模型

CMDQ 是一个 **SPSC ring**：驱动是唯一生产者，固件是唯一消费者。两侧唯一的同步原语
是描述符里的一个 bit（所有权位）和一对 MMIO 写（doorbell）。

```
 主机（驱动）                                     固件（ARM 核）
 ─────────────                                   ─────────────
 ① 填 64B 描述符（coherent DMA）
    + 邮箱块链（>16B 时，576B/块）
 ② status_own = OWNER_HW  ──────────交出所有权
 ③ wmb()                                        （PCIe DMA 读走描述符+邮箱）
 ④ iowrite32be(1<<idx, cmd_dbell) ──────────────► "slot idx 有新货"
                                                 执行命令，写响应
                                                 清 status_own 的 OWNER 位
 ⑤ 轮询见 owner 归零 / 等 EQE 中断 ◄──────────── ⑥ 投递 EQE（type 0x0a，vector 位图）
 ⑦ dma_rmb 后读 out[] / 邮箱                    （bit i = slot i 完成）
```

### 1.2 64 字节描述符（`src/mlx5_prism.h:134`）

```
偏移  字段            说明
0     type            传输类型（PCI）
4     inlen           输入总长（头 + 邮箱）
8     in_ptr          首个输入邮箱块的 DMA 地址；0 = 纯内联
16    in[4]           内联输入：opcode/op_mod + 前 8B 参数
32    out[4]          内联输出：status / syndrome
40    out_ptr         首个输出邮箱块的 DMA 地址
48    outlen          输出总长
56    token           每命令递增（跳 0），邮箱链每块 trailer 必须一致
57    sig             xor8 校验和，覆盖整个 64B（含 status_own）
63    status_own      bit0 = OWNER_HW；bits[7:1] = 命令状态
```

超过 16B 内联容量的数据走**邮箱块链**（`src/mlx5_prism.h:151`）：512B 数据 + 64B trailer
（next 指针 / block_num / token / sig）= 576B，块间用 DMA 地址串链。块必须 **1KB 对齐**
（`roundup_pow_of_two(576)`，固件 DMA 引擎的抓取粒度——错位直接 delivery 0x4/0x5 拒收，
对齐是硬件要求不是风格选择）。

### 1.3 关键不变量

- **单写者原则**：驱动只写 slot 内容 + doorbell + 置 OWNER_HW；固件只写响应 + 清 OWNER 位。
  所有权靠一个 bit 传递，天然无竞态——不需要任何跨 PCIe 的锁。
- **OWNER 位三态**（一个 bit 区分三件事）：0 = 响应就绪；1 = 命令在途；超时仍 1 = 固件失联。
  同一字节的 bits[7:1] 顺带携带 delivery 状态，一次读同时拿到同步与状态。
- **token 防串扰**：描述符 token 与链上每块 trailer 的 token 必须一致，命令 A 的块被
  命令 B 引用时固件拒收（delivery token 错）。
- **page_queue**：最高槽（slot 31）保留给 MANAGE_PAGES——页命令的完成被其他命令依赖，
  不能在普通队列里饿死（`src/mlx5_prism_cmdq.c:238` 的 force_poll 判定）。
- **超时故意泄露**：-ETIMEDOUT 不清 slot 位、不释放邮箱链（`src/mlx5_prism_cmdq.c:365,401`）。
  固件可能迟到写回；复用 slot 或释放内存后，迟到完成会写坏无关命令的内存。宁可泄漏
  一小块 coherent 内存换确定性。
- **两层错误分类学**：先查 delivery（固件有没有取件：token/sig/对齐错），再查 status
  （命令被执行后的裁决，syndrome 给细因）。delivery≠0 时 status 无意义——检查顺序不可反。

### 1.4 屏障语义（本协议最容易被问的一题）

- **写侧 `wmb()` 不是 `smp_wmb()`**（`src/mlx5_prism_cmdq.c:338`）：消费者是经 PCIe DMA
  读内存的固件，不是另一个 CPU 核。`smp_wmb()` 只约束 SMP 核间 store 顺序，不保证对
  PCIe 设备可见；`wmb()` 在 x86 上展开为 `sfence`，对所有 store（含后续 MMIO 写）全局排序。
  位置在 doorbell 之前——保证描述符/邮箱先于"通知"可见。
- **doorbell 是 `iowrite32be(1 << idx)`**（`src/mlx5_prism_cmdq.c:339`）：32-bit 足够
  （32 个 slot 的位掩码）；BE 是因为固件寄存器大端（ARM 核原生 BE）。设计文档 v1 写的
  `writeq` 是错的——硬件实证推翻，这是本项目"按硬件来 + 修正文档"的第一案。
- **读侧对偶**：轮询者先 `READ_ONCE(status_own)` 见到 owner 翻转，再读 `out[]`——中间需要
  读屏障防止后面的读被提前。EQ 消费路径用 `dma_rmb()`（`src/mlx5_prism_eq.c:193,260`）：
  保证先观察到 owner 翻转（来自设备的写），之后的数据读才可信。

### 1.5 完成路径：轮询与事件两种形态

- **轮询**（引导期 / `eq_mode=poll` 回退）：invoke 自旋轮询 owner 位。引导期 EQ 未建、
  IRQ 仍 mask，这是唯一可行形态。
- **事件**（默认，T10 转正）：命令完成生成 EQE（type 0x0a），payload 里的 `cmd_vector`
  位图 bit i 即 slot i 完成。MSI-X 硬中断进 `complete_slots()`（`src/mlx5_prism_cmdq.c:490`）
  认领 pending 位并 `complete()` 唤醒睡眠中的 invoke。实测 events ≈ hardirq ≈ 1 次/事件。
- **owner 门卫**（T10 竞态修复，`src/mlx5_prism_cmdq.c:517` 附近）：固件先写输出、翻 owner、
  后投 EQE（posted-write 有序保证可见序）。因此"EQE 已到而 slot owner 仍为 HW"在构造上
  必是前任占用者的迟到 EQE——跳过不唤醒。这条门卫消灭了一类与调度时序无关的别名竞态。

### 1.6 实测延迟（T16 终值，双口径并陈）

| 量 | 口径 | 数值 |
|----|------|------|
| P1' | doorbell → 唤醒等待者（事件模式） | n=1512，p50=255µs，p99=12.2ms，max=229.6ms |
| P2'ev | complete − 自身 EQE（事件唤醒延迟） | n=216，p50=23µs，p99=69µs，max=1.49ms |
| mlx5_core | kretprobe `mlx5_cmd_invoke` 函数驻留 | n=20700，p50=5.0µs，p99=7.07ms |

解读：P1' 的长尾是**固件真实处理时间**（INIT_HCA ~230ms 主导，轮询量子只有 10-20µs，
造不出这种尾部）；P2'ev 的 23µs 证明 prism 的事件路径本身没有额外惩罚。与 core 的差距
主要是口径差（prism 量到"唤醒等待者"，core 的调用方自己就是等待者）。

### 1.7 面试话术

**30 秒版**："mlx5 的控制面是一个共享内存命令队列：驱动在 coherent DMA 内存里填 64 字节
描述符、大命令挂 576 字节邮箱块链，置所有权位后往 BAR0 的 doorbell 寄存器写一个 32 位
位掩码通知固件；固件 DMA 读走执行，响应写回原描述符并清所有权位；建好 Event Queue 之后
完成还会以 MSI-X 中断通知，事件里带 slot 位图。错误分两层：投递层（token/校验和/对齐）
和执行层（status + syndrome）。超时的 slot 故意泄漏不复用，防固件迟到写回。"

**预判追问**：
- *"为什么 wmb 不用 smp_wmb？"*——消费者是 PCIe 设备不是 CPU，见 §1.4。
- *"为什么 doorbell 是 32 位 BE？"*——32 slot 位掩码；固件寄存器大端，写错字节序会通知
  错误的 slot（写 1 变 bit24）。
- *"邮箱块为什么 576B 却要 1KB 对齐？"*——固件 DMA 引擎按 2 的幂对齐抓取，对齐是硬件要求。

---

## §2 固件引导状态机：从 PCIe 枚举到 HCA OPERATIONAL

### 2.1 九步引导（`src/mlx5_prism_fw.c:509`）

```
 wait_fw_init          轮询 iseg->initializing bit31 清零（注意是 bit31 不是 bit0）
    │
 ENABLE_HCA (0x104)   固件接受的第一条命令——CX-4 Lx 无 QUERY_FW，
    │                 固件版本在 iseg 寄存器直接读（设计 v1 被 hardware 推翻的第二案）
 QUERY/SET_ISSI       接口版本协商（本卡 current=0 → set 1）
    │
 QUERY_PAGES(boot)    固件要 6 页引导内存 ──► MANAGE_PAGES(give 6)
    │                 之后等 iseg->initializing 清零（boot image 从刚拿到的页里跑起来）
 SET_HOST_ENDIANNESS  告诉固件主机字节序（ACCESS_REG reg 0x7004）
    │                 （caps/endian 都被门禁在 boot 页之后——提前发 = status 0x4/0x2b69b6）
 QUERY_HCA_CAP        max / cur 两面（8 块邮箱链，4112B）
    │
 SET_HCA_CAP          按 known-good profile 定容（§2.3）
    │
 QUERY_PAGES(init)    固件按合同要 2299 页 ──► MANAGE_PAGES(give 2299)
    │
 INIT_HCA             HCA → OPERATIONAL（~230ms，引导耗时主导项）
```

每步都有门禁：`fw_cmd_allowed()`（`src/mlx5_prism_fw.c:85`）按 `fw_state`
（RESET→ENABLED→BOOT_PAGES→CAPS_SET→OPERATIONAL）拦"已知时序错误"，固件状态机是
最终裁决——两层分工，驱动拦便宜的，固件裁一切。

### 2.2 页所有权经济学：为什么要驱动给页

主机内存的所有权在主机。固件作为 PCIe master 物理上够得着全部主机内存，但它不知道
哪些页空闲/安全/已映射——驱动分配 coherent 页并显式 GIVE，才建立"固件可安全 DMA 这批
地址"的契约；TAKE 交还。卸载时这个契约让主机确定性回收，避免固件悬空指针指向已复用
的内存。

两级页设计服务两个阶段：

| | boot 页 ×6 | init 页 ×2299 |
|---|---|---|
| 用途 | 引导映像的最小内存 | 运行时工作集（队列上下文、ICM） |
| 时点 | caps 定容**之前**就要给 | SET_HCA_CAP **之后**才知尺寸 |
| 语义 | "先跑起来" | "按合同定容" |

### 2.3 SET_HCA_CAP：定容合同，且在此固件上是门闩

资源预算属于主机：按 max 配置（log_max_qp=2⁶ 等）会占用远超需求的内存，驱动知道自己
要开多少对象，固件不知道。SET_HCA_CAP 是一次性合同，固件按合同计算工作集——
QUERY_PAGES(init) 的 2299 页就是这份合同的产物。虚拟化下它还是资源配额的执行点。

本卡（FW 14.32.1908）的实证怪癖：caps[post] 与 caps[cur] 逐字节相同——固件对多数写入
**钳回默认值**，SET 在此更像"驱动已完成定容"的门闩而非字段级生效；但破坏性写入
（按新内核语义改字段）会真正生效并损坏固件状态（0x4E2106 连环拒页）。known-good
profile（`fw_profile`）因此作为数据管理而非字段语义（§6 怪癖档案）。

### 2.4 两形态 teardown（`src/mlx5_prism_fw.c:616`）

资源是栈式获取的（caps → 页 → INIT），依赖方向决定释放方向：

- **INIT_HCA 成功过**（完整卸载）：TEARDOWN_HCA → TAKE 收页 → DISABLE_HCA。
  TEARDOWN 必须最先——先停固件对 init 页的 DMA 消费，再回收；顺序反了就是 DMA 版
  use-after-free。
- **引导中途失败**：HCA 未 INIT，无消费者需停——直接 TAKE boot 页 + DISABLE
  （镜像 mlx5_core 失败加载路径）。

实测：teardown **2305/2305**（6 boot + 2299 init）100% 全量回收，100 次装卸循环零告警。

### 2.5 关键数字

引导全程 ~270ms（INIT_HCA ~230ms 主导，与 mlx5_core ~265ms 同量级）；每口完整引导
约 42 条命令；卸载单口 ~1s（TEARDOWN_HCA 主导）。

### 2.6 面试话术

**30 秒版**："引导是把 HCA 从 RESET 推到 OPERATIONAL 的九步状态机：使能函数、协商接口
版本和字节序、交 6 页 boot 内存让引导映像先跑起来、查 caps、按需定容签合同、按合同交
2299 页运行时内存、最后 INIT_HCA。每步都被固件状态机门禁——顺序错会被干净地拒绝。
卸载逆序：先 TEARDOWN 停固件的 DMA 消费、再收页、再关函数开关。所有页都是驱动 GIVE
固件 TAKE 的显式所有权交接，所以卸载能确定性回收。"

**预判追问**：
- *"固件为什么不自己抓内存？"*——见 §2.2，所有权 + 安全 + 配额三层理由。
- *"SET_HCA_CAP 改了什么？"*——定容合同；本卡实证它更像门闩（§2.3）。
- *"卸载顺序错了会怎样？"*——先收页 = 运行中的固件对已复用内存继续 DMA。

---

## §3 数据面对象链：SQ-CQ-WQE-CQE（QP 为什么退场）

### 3.1 胜者对象链（T14 十六趟判决收敛的最终形态）

```
 ALLOC_PD ──► CREATE_MKEY(PA) ──► CREATE_CQ ──► ALLOC_TD ──► CREATE_TIS
 (保护域)     (物理地址 mkey)      (完成队列)     (传输域)     (传输实例)
                                                          │
                                                          ▼
 CREATE_SQ(tisn) ──► MODIFY_SQ RST→RDY ──► NOP/SEND WQE ──► CQE ──► 销毁链逆序
 (发送队列, WQ+dbr+UAR)   (bitmask=0)
```

原设计的 QP 路线被硬件封死：CREATE_QP(RC) 能过，但 INIT2RTR 被本卡 vport roce_en=0
**范畴级拒绝**（内容无关、富集梯 12 变体同码），且运行时启用 RoCE 对 prism 拒绝对
mlx5_core 放行（前置状态不明）。路线 A 的洞察：**SQ/RQ 对象模型天然绕开整个连接状态机**
——对象的"状态"藏在命令序列（CREATE→MODIFY RDY）而非 qpc 状态机里。这是 mlx5e 以太网
数据面的原生形态。

### 3.2 WQE / CQE：数据面的"描述符 + doorbell"同构

```
 WQE（工作队列条目，WQEBB = 64B 步长，DS = 16B 单元）
 ┌─────────────┐
 │ ctrl (16B)  │ WQE 编号 / DS 数 / 签名 / QP/SQ 号 —— 前 8B 就是 send doorbell 的值
 ├─────────────┤
 │ eseg (16B)  │ 以太网段：inline_hdr@+12（2B 头内嵌，余头溢入后续 DS）
 ├─────────────┤
 │ dseg (16B+) │ 30B 计数|inline 标志 + lkey + 地址；或 inline 载荷直嵌
 └─────────────┘

 CQE（完成队列条目，64B）
 byte_cnt@0x2c / wqe_counter@0x3c / opcode+owner@0x3f
 错误形态：vendor@0x36 / syndrome@0x37 / s_wqe_opcode_qpn@0x38
 （偏移表见 src/mlx5_prism_fmt.h:530）
```

- **CQE owner 位消歧环复用**：owner 字节携带圈奇偶，fresh 判定
  `(op_own & 1) == ((ci >> log_sz) & 1)`。同一 entry 相邻两圈 fresh 值相反——上一圈的
  旧 CQE 永远读作 stale，环空/环满无歧义。与 CMDQ/EQ 的 owner 是同一个思想的三次实例化。
- **dbr（doorbell record）**：coherent 内存里的 64B 小结构（`src/mlx5_prism_smoke.c:108`），
  SQ 侧 [4] 是 be32 的 WQEBB 计数——生产者游标的内存镜像，硬件异步跟随；CQ 侧 [0] set_ci
  [4] arm。MMIO doorbell 是"打铃"，dbr 是"挂牌"。
- **三因子**（本卡 L2 发送的硬约束，缺一不可）：① TIS——无传输域的 SEND 判
  LOCAL_QP_OPERATION_ERROR；② L2-inline——vport min_wqe_inline_mode=1 地板，以太网头
  必须放 eseg.inline_hdr；③ byte_cnt=0——本固件 TX CQE 不报字节数（判据按实测口径修正）。

### 3.3 发送的完整动作序列（`src/mlx5_prism_smoke.c:523,564-570`）

```
 构造 WQE（coherent WQ 里，可能跨两个 WQEBB）
   │
   ▼
 dma_wmb()                     ← WQE 对设备可见（写序对 DMA 消费者）
   │
   ▼
 SQ dbr 写 be32 BB 计数        ← 先挂牌（游标内存镜像）
   │
   ▼
 wmb()
   │
   ▼
 __raw_writeq(bf)              ← MMIO 打铃：ctrl 段前 8B 原样直写（本身 BE），
   │                             BAR0 + uar×4K + 0x800
   ▼
 固件 DMA 读 WQE → 组帧出队 → DMA 写 CQE + 翻 owner
   │
   ▼
 驱动谓词判定 fresh → 解析 byte_cnt/wqe_counter → 推进 ci → 写 CQ dbr
```

这套屏障序与 mlx5e_notify_hw 逐字一致。全链路打通的含义：固件经 PAS 找到 WQE 页、
DMA 读 WQE、解析执行、DMA 写回 CQE、owner 语义正确、销毁链零泄漏——数据面整条电路闭环。

### 3.4 T15 netdev：把对象链接进内核网络栈

RX 侧引导链比 TX 多三件：CREATE_RQ → CREATE_TIR（direct dispatch，inline_rqn）→
CREATE_FLOW_TABLE/GROUP/FTE/ROOT（把该口流量指到 RQ）。形态（`src/mlx5_prism_net.c`）：

- **单 CQ 共享 SQ/RQ** + 专用 comp EQ（vector 0、零掩码——事件模型实证最稳的拓扑）；
- TX 主档 hdr-inline + 指针 dseg（`net.c:882` ndo_start_xmit，`:977` dma_map_single）；
- RX 用 coherent 大缓冲预投递、CQE 后 `napi_alloc_skb` 拷贝构造 skb（`net.c:793`），
  重投递只需 dbr-only 发布新 producer index（`net.c:455`）；
- NAPI：事件唤醒 + 10ms delayed_work 看门狗双路调度（`net.c:1026`）——本固件事件模型
  的怪癖是"完成只在 arm 后 µs 级窗口内通知"（µs 三元组），看门狗把兜底时延钉死；
- 验收：双口互 ping exit 0 + AF_PACKET 双向逐字节比对 + PPCNT 线缆级对账，全绿。

### 3.5 面试话术

**30 秒版**："数据面和控制面是同一个模式的两套实例：WQE 是给硬件的工作描述符（控制段
+ 以太网段 + 数据段，64 字节步长），CQE 是硬件的完成回执——两者都在 coherent DMA 环上，
用 owner 位的圈奇偶消歧环复用，游标用内存里的 doorbell record 挂牌、MMIO doorbell 打铃。
以太网发送走 SQ 对象链：PD、mkey、CQ、传输域 TIS、发送队列，MODIFY 到 RDY 后 post WQE。
接收侧再挂 RQ/TIR/流表把流量引到队列，CQE 进 NAPI。和 RDMA 的 QP 相比，SQ/RQ 把连接
状态机换成了命令序列——没有连接语义的以太网场景更简单直接。"

**预判追问**：
- *"CQE owner 位解决什么问题？"*——环复用歧义，圈奇偶签名，见 §3.2。
- *"为什么 WQE 放 coherent 内存？"*——WQE 分段构造、post 后软件不再触碰，关键是写序
  对设备可见（dma_wmb → dbr → wmb → MMIO）；coherent 免去 cache 维护。
- *"QP 和 SQ 什么关系？"*——QP 是 RDMA 的连接状态机对象；本卡 roce_en=0 封死了 RTR
  之后的路，SQ/RQ 对象模型绕开状态机（§3.1）。

---

## §4 DMA 一致性：coherent 与 streaming 的分工

### 4.1 两个地址，两个世界

`dma_alloc_coherent` 一次调用返回**两个地址**（`src/mlx5_prism_cmdq.c:44`）：CPU 虚拟
地址（内核直接映射区，驱动读写用）和 `dma_addr_t` 总线地址（写进固件可见的寄存器/描述符）。
两者数值无关——本机实测总线地址落在 4-34GB 区间，是 64-bit DMA mask 生效 + IOMMU 翻译
后的值。coherent 映射硬件保证 cache 一致性，无需显式 flush。

```
        CPU 世界                      PCIe 世界（固件视角）
   ffff88... 虚拟地址   ──IOMMU 翻译──►   4-34GB 总线地址
        cpu_addr                            dma_addr
        （驱动读写）                     （写进描述符/寄存器给固件）
```

### 4.2 分工清单（本驱动的每一次选择）

| 内存 | 映射 | 位置 | 为什么 |
|------|------|------|--------|
| CMDQ 描述符环 | coherent | `cmdq.c:44` | 低频小内存（4KB 级），硬件保证一致，省心 |
| 邮箱块链 | coherent | `cmdq.c:150` | 同上，576B/块 |
| EQ 环（EQE） | coherent | `eq.c` | 同上 |
| WQ / CQ 环 | coherent | `smoke.c` / `net.c` | 环小且双向频繁读写 |
| dbr（doorbell record） | coherent | `smoke.c:156` | 64B，硬件异步轮读的游标镜像 |
| RX 预投递缓冲 | coherent | `net.c:1140` | 骨架简化：收帧后拷贝构造 skb |
| **TX 载荷** | **streaming** | `net.c:977` | 数据面大流量：`dma_map_single` 按包映射，完成时 unmap（`net.c:758`），避免常驻 coherent 大池 |

判据：**低频小结构走 coherent**（映射开销摊薄、免 cache 维护）；**高频大数据走
streaming**（按需映射/解映射，cache bounce 由 dma API 管）。引导期的页（GIVE/TAKE）
本质也是 DMA 所有权契约——驱动分配的页，固件按合同 DMA。

### 4.3 屏障选型表（谁在哪管什么序）

| 屏障 | 出场点 | 管什么 |
|------|--------|--------|
| `wmb()` | doorbell MMIO 前（`cmdq.c:338`） | 描述符写序对 PCIe 设备可见（跨 MMIO 的全局 store 序） |
| `dma_wmb()` | WQE 后、dbr 前（`smoke.c:564`） | DMA 内存内的写序：WQE 先于游标挂牌对设备可见 |
| `dma_rmb()` | EQE owner 翻转后（`eq.c:193,260`） | 设备写的读序：先见 owner 翻转再读事件数据 |
| 读屏障 | 轮询 owner 归零后读 out[] | 同上思想，命令响应路径 |

选型口诀：写侧看消费者（CPU → CPU 用 smp 系；CPU → 设备用 wmb/dma_wmb 系），
读侧看生产者（设备写的，观察同步标志后用 dma_rmb 再读数据）。

### 4.4 面试话术

**30 秒版**："mlx5 里 coherent 用在所有低频控制结构上——命令队列、事件队列、邮箱、
WQE/CQ 环、doorbell record，都是 4KB 级小内存，硬件保证 cache 一致，一次映射常驻；
streaming 用在数据面载荷上——发送的 skb 走 dma_map_single 按包映射、完成时解映射，
避免为吞吐量常驻 coherent 池。两类之外的第三个体现是页所有权：引导时 GIVE 固件的
2305 页，本质是 DMA 访问权的显式契约。屏障上，写侧 doorbell 前必须 wmb 而不是
smp_wmb，因为消费者是 PCIe 设备；读侧在 EQE owner 翻转后 dma_rmb 再读数据。"

**预判追问**：
- *"coherent 的代价？"*——映射窗口开销略高、可能挤占一致性域；低频小内存无所谓，
  高吞吐必须 streaming。
- *"流式映射忘了 unmap 会怎样？"*——泄漏 DMA 映射资源 + 悬空总线地址；prism 的 TX
  在 CQE 回收路径统一 unmap（`net.c:758`）。

---

## §5 doorbell 与 UAR：为什么 Mellanox 不需要 VFIO

### 5.1 BAR0 上的三种 doorbell

doorbell 是 mlx5 的统一通知原语：一个对固定 MMIO 地址的写入，硬件按**地址路由**到
对应对象。本驱动敲过三种，几何各不相同：

```
 BAR0 (32 MB)
 ├─ 0x0000   init segment（4KB 握手区：fw_rev / initializing / cmdq_addr / cmd_dbell）
 │              └─ cmd_dbell @ +0x18
 └─ +uar×4KB  UAR 页空间（页号由 ALLOC_UAR 分配）
    ├─ +0x40   EQ arm doorbell      ┐ 值 = (ci & 0xffffff) | (eqn << 24)，大端
    ├─ +0x48   EQ publish doorbell  ┘ +0x40 附带重新武装中断，+0x48 只发布游标
    └─ +0x800  BF：send doorbell    值 = WQE ctrl 段前 8 字节原样直写（本身 BE）
```

| doorbell | 地址 | 写法 | 值语义 |
|----------|------|------|--------|
| CMDQ | iseg + 0x18 | `iowrite32be(1 << idx)`（`cmdq.c:339`） | 位掩码：slot idx 有新命令 |
| EQ | uar×4K + 0x40 / 0x48 | 大端 `(ci)\|(eqn<<24)`（`eq.c:163`） | 消费者索引（arm / 仅发布） |
| send | uar×4K + 0x800 | `__raw_writeq`（`smoke.c:570`） | WQE 描述符头 + 24 位 WQE 序号 |

细节两则：EQ 的双 doorbell 分工是本卡的实证重点——事件**数据**先行暂存入环，owner
（提交位）只在非 arm doorbell（+0x48）发布消费者索引后才翻转；arm doorbell（+0x40）
只门控中断生成，本身不释放已暂存的事件（`eq.c:234` 注释）。send doorbell 的 v1 设计
曾写 `writeq(doorbell)`——对 send 恰好成立（8B 裸写），但 CMDQ 那次是 32 位位掩码：
两个"写 doorbell"字面相同、几何不同，都以硬件实测定案。

### 5.2 UAR 隔离原理

UAR（User Access Region）是 BAR0 里按 4KB 划页的门铃区：每个上下文（EQ、每个 QP/SQ）
经 ALLOC_UAR 拿到独立页号，硬件按写入地址路由到对应对象。关键性质：

1. **写入即通知**——一个 8B 写、无读副作用、无状态泄漏，天然适合非特权映射；
2. **页级隔离**——映射自己那页的进程只能敲自己的门铃，碰不到别人的对象和寄存器。

于是 Mellanox 的形态是：**内核态跑控制面（CMDQ/引导/EQ），用户态进程 mmap 自己的
UAR 页直接敲 send doorbell + 直接轮询 CQ**，零拷贝数据面与内核控制面共存，互不越权。

### 5.3 与 Intel E810 对比（CLAUDE.md 对照表）

| | ConnectX（mlx5） | E810（ice） |
|---|---|---|
| 控制面 | 内核驱动经 CMDQ（共享内存 + doorbell） | 用户态经 Admin Queue（同模式） |
| 数据面 doorbell | UAR 页硬件隔离，可非特权 mmap | **无等价物**——MMIO 需特权 |
| 用户态数据面 | mmap UAR + 轮询 CQ 即可 | 必须 **VFIO** 整卡接管 |
| 代价 | 硬件多一层页路由 | 用户态扛全部（含控制面），失去内核栈共存 |

E810 没有 UAR 这层硬件隔离，控制面和数据面都被迫挤进 VFIO 用户态——这是理解
"为什么 Mellanox 不需要 VFIO"的最佳对照：**doorbell 的可委托性 = 数据面可用户态化的
全部前提**。

### 5.4 面试话术

**30 秒版**："mlx5 所有异步通知都是 doorbell：往固定 MMIO 地址写一下，硬件按地址路由。
三种几何——命令队列是 init segment 里的 32 位位掩码；事件队列是 UAR 页上的消费者索引，
还分 arm 和仅发布两个地址；发送队列是把 WQE 头 8 字节直接写到 UAR 页的 BF 区。UAR 把
BAR0 划成每上下文一页的门铃区，硬件隔离且无读副作用，所以用户态进程 mmap 自己那页
就能直接发数据包，内核继续管控制面——这就是 Mellanox 不需要 VFIO 的原因；Intel E810
没有这层，用户态数据面只能整卡 VFIO。"

**预判追问**：
- *"为什么 doorbell 可以非特权映射而寄存器不行？"*——写入即路由、无副作用、页级隔离，
  映射者能做的只有"敲自己的铃"。
- *"EQ 为什么要两个 doorbell？"*——arm 门控中断生成，publish 提交事件数据；本卡实证
  数据先行、owner 后提交（§5.1）。

---

## §6 尾章：怪癖档案方法论 + 复述自检

### 6.1 第六层：把固件实测行为当数据管理

prism 在四层架构（硬件访问/传输/固件/服务）之外多了一个 mlx5_core 没有的层：**怪癖
档案**（DESIGN.md §6，`fw_profile`）。设计动机：固件的文档（PRM）与行为之间存在真实
落差，而"这颗固件实际怎么表现"是昂贵的上机实验换来的——应当作为数据沉淀、按版本管理，
而不是散落在注释和记忆里。三个代表案例：

1. **能力位谎报**：PCAM 位说 CX4 Lx 没有 0x16 计数器组，固件照常维护并放行（T12 实测
   rx_bits 按线率精确累积）；MCAM mtmp 位不可达而 MTMP 寄存器照常服务（T13 双口同读
   94°C）。**能力位谎报缺失，固件行为先行**。
2. **eqc 编码翻案**：T09 时代三个"固件怪癖"（1 条目环 / ~10% 激活 / 中断不点火）全部
   是驱动侧位号编码错误的产物（T10-prep ifc 编码后 10/10 首次激活、MSI-X ≈1 次/事件）。
   教训：**在协议字段编码被证明正确之前，"固件怪癖"与"驱动 bug"不可区分——先审自己
   的位几何，再怪硬件**。
3. **caps 门闩**：SET_HCA_CAP 的多数写入被钳回默认值，但破坏性写入真会损坏固件状态
   （§2.3）——known-good profile 因此按字节管理。

方法论层面还有一条贯穿全程的纪律：**判别式阶梯 + 黑匣子**——每次上机预登记判据、
一次实验只杀一个假设；同一判据连红两次就停下做四层尸检（T16 的 P2'ev 口径重定义即
此产物），而不是在错误假设上连环小修。

### 6.2 读者复述自检清单

不看正文，能答出十之八九即通过本文验收：

1. 从 insmod 到 ping 通，按顺序说出经过的六层（§0 全景图）。
2. CMDQ 的两侧各自只写什么？所有权靠什么传递？（§1.3 单写者原则 + OWNER 位）
3. doorbell 前为什么要 `wmb()` 而不是 `smp_wmb()`？（§1.4）
4. >16B 的命令数据走哪里？块多大、为何 1KB 对齐、token 防什么？（§1.2/§1.3）
5. 命令超时为什么故意泄漏 slot？（§1.3）
6. 固件为什么要驱动给页而不是自己抓？boot 页和 init 页的区别？（§2.2）
7. SET_HCA_CAP 扮演什么角色？卸载时 TEARDOWN 为什么必须先于收页？（§2.3/§2.4）
8. CQE 的 owner 位怎么消除环复用歧义？（§3.2 圈奇偶谓词）
9. 说出一个以太网帧的完整发送序列，含三个屏障的位置。（§3.3）
10. coherent 和 streaming 各用在本驱动哪些地方？选择依据？（§4.2）
11. 三种 doorbell 的地址、写法、值语义分别是什么？（§5.1 表）
12. UAR 为什么让 Mellanox 不需要 VFIO？E810 为什么需要？（§5.2/§5.3）

### 6.3 深读指针

- **READING.md**——按 ~600 行协议脊柱读源码的顺序与不变量（怎么读代码）；
- **DESIGN.md v2.6**——完整技术设计：四层架构 §2、命令引擎 §5（错误分类学 §5.4）、
  引导状态机 §4、EQ 与事件 §7、怪癖档案 §6、netdev 数据面 §5.8；
- **progress.md**——每任务的实证记录 + 历史问题归档（字节偏移实证表、syndrome 速查、
  翻案全程）；
- **docs/report.md**——T16 双层对比报告（prism vs mlx5_core，18 项判据 + 双口径延迟）。

---

*SUMMARY.md · T17 · 2026-10-02 · 依据 DESIGN v2.6 / progress T01-T16 / READING ·
全部数字为上机实测终值，全部行号对当前 src/ 树核对。*
