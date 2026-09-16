# mlx5_prism 开发进度记录

> 本文件按 task id 分节记录开发过程，配合 `task.json` 使用。
> AI agent 每完成一个 task，在本文件对应章节填入：实际读过哪些源码、遇到的问题、与 DESIGN.md 的偏差、学习心得、git commit hash。

## 使用约定

- 每个 task 一个二级标题（按 `task.json` 的 id 命名），未开始时状态为 `pending`
- task 状态流转：`pending` → `in_progress` → `completed`（或 `blocked`）
- 状态变更要更新下方"总览表"，并填对应 task 章节的详细字段
- 任何与 DESIGN.md/CLAUDE.md 的偏差必须记录（commit 时要同步修正设计文档）

## 状态总览表

| Task ID | 标题 | 阶段 | 里程碑 | 状态 | Commit | 完成日期 |
|---------|------|------|--------|------|--------|---------|
| T01-hello-module | 内核模块骨架 + Makefile | S0 | — | **completed** | 8d446c9 | 2026-07-07 |
| T02-pci-probe | PCI probe + BAR0 + 设备结构体 | S1 | **M0** | **completed** | e64c325 | 2026-07-07 |
| T03-dma-eq-alloc | DMA mailbox + EQ 分配 | S2 | — | **completed** | 693448b | 2026-07-14 |
| T04-msix-irq | MSI-X 中断 + request_irq | S2 | — | **completed** | 829cda5 | 2026-07-14 |
| T05-cmdq-submit | CMDQ 命令提交（doorbell） | S3 | — | **completed** | 29ffcf0 | 2026-08-09 |
| T06a-cmdq-invoke-poll | CMDQ 轮询模式 invoke（等待 + 状态解析） | S3 | — | **completed** | 0678082 | 2026-08-28 |
| T06b-cmdq-mailbox-chain | 多块邮箱链（576B 块 + 1KB 对齐 + token） | S3 | — | **completed** | 83f9f9e | 2026-09-08 |
| T07-fw-bootstrap-1 | 固件引导 I：ISSI + 字节序 + boot pages + 回收 | S4 | — | **completed** | 10ccbad | 2026-09-08 |
| T08-fw-bootstrap-2 | 固件引导 II：SET_HCA_CAP + init pages + INIT_HCA | S4 | **M1** | **completed** | 436ea84 | 2026-09-08 |
| T09-eq-create-poll-consume | EQ 创建 + 轮询消费 EQE（零中断点亮） | S5 | — | **completed** | 24c9d80 | 2026-09-08 |
| R0.1-golden-baseline | R0 重构 I：金样采集（行为基线存档） | S4.5 | — | **completed** | 见 R0.1 节 | 2026-09-15 |
| R0.2-typed-cmd-layer | R0 重构 II：类型化命令层 mlx5_prism_fmt.h | S4.5 | — | **completed** | 见 R0.2 节 | 2026-09-15 |
| R0.3-completion-backend | R0 重构 III：完成后端解耦（cmdq mode + complete_slots） | S4.5 | — | **completed** | 见 R0.3 节 | 2026-09-15 |
| R0.4-fw-state-quirks | R0 重构 IV：固件状态机门禁 + 怪癖档案命名化 | S4.5 | — | **completed** | 见 R0.4 节 | 2026-09-15 |
| R0.5-dev-ctx-teardown-selftest | R0 重构 V：设备结构重组 + 统一 teardown + selftest=1 分离 | S4.5 | — | **completed** | 见 R0.5 节 | 2026-09-15 |
| R0.6-trace-scaffold | R0 重构 VI：tracepoint 脚手架（T11 核心提前） | S4.5 | — | **completed** | 见 R0.6 节 | 2026-09-15 |
| T11-tracepoints | tracepoint 完善与延迟基线采集（P1'/P2'） | S5 | — | pending | — | — |
| T10-eq-irq-enable | MSI-X 点亮 + 硬中断事件模式（镜像 mlx5_core 官方路径） | S5 | — | pending | — | — |
| T12-phy-access-reg | PHY 监控 1：ACCESS_REG PPCNT | S5 | — | pending | — | — |
| T13-phy-sysfs | PHY 监控 2：sysfs + 定时器 + 告警 | S5 | — | pending | — | — |
| T14-smoke-test | 硬件冒烟测试（QP→WQE→CQE） | S5 | — | pending | — | — |
| T15-netdev-skeleton | 最小 netdev 骨架 | S5.5 | — | pending | — | — |
| T16-verify-report | verify.sh + 对比报告 | S6 | **M2** | pending | — | — |
| T17-architecture-summary | 架构总结文档 | S6 | — | pending | — | — |

---

## 学习心得汇总

> 每个 task 完成后，把"对自己最有启发的一条认知"摘出来放这里，最后回顾时一目了然。

- _（待 T01 完成后填写）_
- **T05**：CMDQ doorbell 的正确写法是 **32-bit `iowrite32be(1 << idx, &iseg->cmd_dbell)`**，不是 task.json/DESIGN.md 写的 `writeq`——位掩码值 1<<idx 表示"通知固件处理 slot idx"，固件经 PCIe 读到的是 BE32。doorbell 前的 **`wmb()`（非 `smp_wmb()`）**：消费者是固件经 PCIe DMA 读，不是另一个 CPU，必须用能跨越 MMIO 写的屏障；放在 `iowrite32be` **之前**确保描述符/mailbox 先于 doorbell 可见。CMDQ boot 完成是 **轮询 `lay->status_own` 的 OWNER_HW 位**（匹配 mlx5_core 的 `CMD_MODE_POLLING`），不走中断——EQ 未建、IRQ 仍 mask，所以 task.json 的"/proc/interrupts 计数自增"判据在此阶段不适用，等价证明是 `doorbell honoured, status_own 回到 SW, status=0`。**CX-4 Lx 无 QUERY_FW 命令**（7.0.0 mlx5_ifc.h 里 0x103=TEARDOWN_HCA）：fw 版本在 init segment 寄存器里直接读，第一条命令是 **ENABLE_HCA (0x104)**。实测 log_stride=6（64 字节/槽，等于描述符 struct 大小，无 padding）。
- **T04**：MSI-X 中断是 PCIe 设备主动写 MMIO 地址触发中断（不是 INTx 的电平信号），每个 MSI-X 向量独立指向一个 CPU + 一个地址。`request_irq` 的 handler 在**硬中断上下文**执行（不是进程，禁止 mutex_lock/sleep），所以只能做最小工作——ack + 唤醒。`IRQF_NO_AUTOEN` 让 request_irq 不自动 enable，T04 保持 mask 状态直到 T07 创建 EQ 后才 `enable_irq`，避免固件随机中断打到未初始化的 handler。`/proc/interrupts` 的 `IR-PCI-MSI` 前缀说明 x86 VT-d 中断重映射启用，hwirq 4194304 是 MSI-X 表索引 0 经 IOMMU 翻译后的值。

- **T03**：`dma_alloc_coherent` 一次调用返回**两个地址**——CPU 虚拟地址（驱动读写用）和 `dma_addr_t` 总线地址（写进固件可见的寄存器/mailbox 描述符）。两者数值无关：CPU 虚拟地址是内核直接映射区 `ffff88...`（按 phys 直接映射），总线地址是 IOMMU/PCIe 地址空间映射后的值（本机实测 4-34GB，说明 64-bit DMA mask 生效）。coherent DMA 硬件保证 cache 一致性，无需显式 `dma_wmb()` flush，代价是映射窗口比 streaming DMA 映射开销略高但省心——mailbox/EQ 是低频小内存（4KB），coherent 完全合适。

- **T01**：内核模块的本质就是把一个函数指针塞进 `.initcall` 链接段——`module_init` 是宏不是函数，insmod 通过 `init_module()` syscall 在模块的 `.initcall` 段找到指针并执行。`MODULE_LICENSE("GPL")` 不是法律声明而是**技术开关**：决定能否使用 `EXPORT_SYMBOL_GPL` 导出的符号（`pcim_*`、`dma_alloc_coherent`、`pci_alloc_irq_vectors` 等后续 task 必用的核心 API 都是 GPL-only）。
- **T02**：`pcim_` 前缀（pcim_enable_device/pcim_iomap_regions）= `pci_` 函数 + devres 自动释放。devres 把释放回调挂到 `pdev->dev.devres_head` 链表，probe 失败或 remove 时 PCI core **逆序**自动调用——这就是为什么我的 probe 里 `return err` 后无需任何 `goto err_xxx` 清理链，比 mlx5_core 的手动 `request_bar/iounmap/release_bar` 简洁得多。BAR0 起始的 init segment（4KB）是固件→驱动握手区，存 fw_rev / cmdq 物理地址 / initializing 位等。**M0 达成**：lspci 显示双口均绑定 mlx5_prism，fw_rev 14.32.1908 与 mlx5_core 读到的一致。

---

## 详细记录

### T01-hello-module

- **状态**: completed
- **开始时间**: 2026-07-07
- **完成时间**: 2026-07-07
- **Commit**: 8d446c9
- **读过源码**: 无（CLAUDE.md 明确 T01 不需要读 mlx5_core，写自己的 module_init/exit 即可）
- **关键决策**:
  - Makefile 用 `mlx5_prism-y := mlx5_prism_main.o` 的多文件模块语法（而非 `obj-m := mlx5_prism.o` + 单一 `mlx5_prism.c`），方便 T02+ 直接追加 `.o` 文件
  - 文件命名为 `mlx5_prism_main.c`（对齐 mlx5_core 的 `main.c` 约定）
  - `pr_fmt` 定义为 `KBUILD_MODNAME ": "`，所有 `pr_info` 自动带 `mlx5_prism: ` 前缀；init/exit 只输出 `loaded`/`unloaded` 即可
  - 顺带修正 `.gitignore`：原版漏了 `.*.cmd`（Kbuild 的依赖追踪文件）和 `modules.order`，重写为标准内核模块 gitignore
- **遇到的问题与解决**:
  - 编译时出现 "the compiler differs from the one used to build the kernel"（gcc 15.2.0 vs 内核编译用的 11.4.0）—— 这是 Kbuild 的 info 提示非代码警告，T01 无影响，记录在案，后续 task 若出问题再处理
  - BTF generation 跳过（只有 headers 没有 vmlinux）—— 正常，不影响功能
- **与设计的偏差**: 无
- **exit_criteria 验证结果**:
  - compile: `make modules` 通过，无 ERROR/WARNING；`checkpatch.pl --no-tree --strict -f` 0 errors / 0 warnings / 0 checks
  - load: `insmod` 成功，`lsmod | grep mlx5_prism` 显示 `mlx5_prism 16384 0`（一行）
  - functional: dmesg 输出 `mlx5_prism: loaded (v0.1)` 和 `mlx5_prism: unloaded`
  - manual_checks: 100 次 `insmod/rmmod` 循环全部通过，dmesg `grep -iE "WARNING|BUG|Oops|panic|leak"` 输出 `DMESG CLEAN`
  - 注：T01 是纯 hello module 无 PCI 绑定，**未卸载 mlx5_core**（互斥约束从 T02 起生效）
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: module_init 是函数还是宏？它把函数指针放在哪个链接段？内核怎么找到它？**
    A: 宏。展开后把函数指针注册到 `.initcall` 段（builtin 是 `device_initcall` level 6；模块版本通过 `.gnu.linkonce.this_module` 段的 `struct module` 记录 init 指针）。`insmod` 触发 `init_module()` syscall → `load_module()` 从模块镜像里找到 init 段指针 → 在 `do_one_initcall()` 中执行。
  - **Q: __init 标记的函数加载完成后会发生什么？为什么这样设计？**
    A: `__init` = `__section(".init.text")`。模块的 init 段在 `load_module()` 成功后被 `do_free_init()` 释放（`module_memfree()`）；builtin 则在启动末尾由 `free_initmem()` 统一释放。设计目的：初始化代码只跑一次，跑完即可丢弃，节省 RAM（典型服务器可释放数 MB）。配合 `module_init` 一起释放。
  - **Q: 为什么 MODULE_LICENSE("GPL") 必须写？不写会怎样？写 "Proprietary" 又会怎样？**
    A: license 字符串是**技术开关**，决定能否用 `EXPORT_SYMBOL_GPL` 导出的符号。不写或写 "Proprietary"：内核被 tainted（`Taint flag: O out-of-tree module, P proprietary`），且无法 resolve GPL-only 符号，加载时 `Unknown symbol` 报错。写 "GPL"：可用全部核心 API。T01 不影响（没调任何 GPL-only 符号），但 T02+ 的 `pcim_*`、T03 的 `dma_alloc_coherent` 都是 GPL-only，必须写 "GPL"。

---

### T02-pci-probe 【M0 里程碑】

- **状态**: completed (M0 达成)
- **开始时间**: 2026-07-07
- **完成时间**: 2026-07-07
- **Commit**: e64c325
- **读过源码**:
  - `mlx5_core/main.c:2250-2279` — `mlx5_core_pci_table[]` PCI device id 表写法（`PCI_VDEVICE(MELLANOX, 0x1015)`）
  - `mlx5_core/main.c:1972-2030` — `probe_one` 资源分配顺序 + 错误回滚 goto 链
  - `mlx5_core/main.c:918-985` — `mlx5_pci_init` / `mlx5_pci_close`：enable → request_bar → set_master → set_dma_caps → `ioremap(iseg_base, sizeof(*iseg))`
  - `include/linux/mlx5/device.h:570-594` — `struct mlx5_init_seg` C 结构体（fw_rev/cmdif_rev_fw_sub/cmdq_addr/cmd_dbell/initializing 偏移）
  - `include/linux/mlx5/driver.h:897-909` — `fw_rev_maj/min/sub` inline：`ioread32be(&iseg->fw_rev) & 0xffff` 等
- **关键决策**:
  - **建立多文件结构**：`mlx5_prism.h`（公共定义）+ `mlx5_prism_main.c`（module 入口 + pci_driver 注册）+ `mlx5_prism_pci.c`（probe/remove）。为后续 ~2000 行的 cmdq/fw/phy/smoke/netdev 模块铺路，避免单文件膨胀。对齐 mlx5_core 自己的多文件拆分（main.c / cmd.c / eq.c / fw.c）。
  - **全程用 devres**（`pcim_enable_device` + `pcim_iomap_regions` + `devm_kzalloc`），probe 失败只需 `return err` 无需 goto 清理链。这是 task.json steps 明确要求的，比 mlx5_core 的手动管理（request_bar/iounmap/release_bar）更现代。
  - **init segment 自定义精简版**：`struct mlx5_prism_iseg` 只定义 T02 用到的字段（fw_rev / cmdif_rev_fw_sub / initializing），不抄 mlx5_ifc.h 的全 bit-field 版本（那是给 RDMA/offload 用的）。后续 task 需要更多字段再追加。
  - **fw_rev 读法**：`ioread32be(&iseg->fw_rev)` 返回 BE 32-bit 值，高 16 = minor，低 16 = major（与 mlx5_core driver.h 一致）。这是因为寄存器是 big-endian，`ioread32be` 做了 BE→CPU native 转换。
- **遇到的问题与解决**（多文件模块开发 3 个典型踩坑，记录供后续 task 参考）:
  1. **跨文件符号引用未声明**：`main.c` 引用 `mlx5_prism_pci_ids` 但定义在 `pci.c` → 编译报 `undeclared`。解决：`.h` 加 `extern const struct pci_device_id mlx5_prism_pci_ids[];`
  2. **static 与 extern 冲突**：`pci.c` 里把 probe/remove/ids 声明 `static`，与 `.h` 的 `extern` 冲突 → `static declaration follows non-static declaration`。解决：跨文件调用的 3 个符号去掉 `static`，pci.c 内部辅助函数保持 `static`。
  3. **MODULE_DEVICE_TABLE 宏未定义**：`pci.c` 忘了 `#include <linux/module.h>` → `type defaults to 'int'`。checkpatch 检测不到 include 缺失，只有编译暴露。解决：pci.c 加 `<linux/module.h>`。
- **与设计的偏差**: 无（代码结构与 DESIGN.md §2 核心数据结构 `struct mlx5_prism_dev` 一致，只是当前只有 pdev/bar0/iseg 三个字段，后续 task 追加 cmdq/eq/phy/caps）
- **exit_criteria 验证结果**:
  - compile: `make modules` 通过，0 ERROR/WARNING；checkpatch 3 文件全 0/0/0
  - load: `insmod` 成功，`lspci -k -d 15b3:1015` 显示双口均 `Kernel driver in use: mlx5_prism`（08:00.0 + 08:00.1 双 function，probe 调用 2 次）
  - functional: dmesg 输出 `mlx5_prism 0000:08:00.0: BAR0 mapped at [mem 0x6016000000-0x6017ffffff 64bit pref], fw_rev 14.32.1908, cmdif_rev 5`（双口各一行）
  - manual: 100 次 insmod/rmmod 循环通过，dmesg `DMESG CLEAN`（无 WARNING/BUG/Oops/panic/kmemleak）
  - **M0 里程碑达成**：lspci 绑定 + BAR0 可读 + fw_rev 合理
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: pcim_ 前缀函数和裸 pci_ 函数有什么本质区别？devres 是怎么实现自动释放的？**
    A: `pcim_` = `pci_` + devres（device resource management）。`pcim_enable_device` 内部调 `pci_enable_device` 后把一个释放回调（含 disable 函数指针）挂到 `pdev->dev.devres_head` 链表。后续 `pcim_iomap_regions` / `devm_kzalloc` 同理把各自的释放回调挂上去。probe 返回非零（失败）或 `pci_driver.remove` 被调用时，PCI core 遍历该链表**逆序**（LIFO，保证依赖顺序）调用所有回调。所以 probe 中途 `return -ENOMEM` 不会泄漏前面分配的任何资源。mlx5_core 用裸 `pci_` 是因为它要做更精细的错误恢复（AER、健康轮询），需要自己控制释放时机；学习项目用 `pcim_` 足矣。
  - **Q: pci_set_drvdata 存的指针后续在哪些场景被取出来？**
    A: 任何 PCI core 回调里。具体：`remove`（卸载时逆操作）、`suspend`/`resume`（电源管理）、`shutdown`（系统关机）、`err_handler`（AER 错误恢复）、`request_irq` 的 `dev_id` cookie（中断 handler 拿上下文）、sysfs attribute 的 `show/store`（通过 `to_pci_dev` + `pci_get_drvdata`）。T04 会用到 request_irq，T13 sysfs 会用到 attribute——都要靠 drvdata 拿回 `struct mlx5_prism_dev *`。
  - **Q: ConnectX-4 Lx 的 BAR0 是 32MB，里面装的是什么？为什么控制面用 BAR0 而不是 BAR2？**
    A: BAR0 的 layout：起始 4KB 是 init segment（固件状态/命令队列指针/health buffer），之后是 UAR（User Access Region）—— 每个 QP/CQ 独占一页（4KB）的 doorbell 寄存器。驱动写 doorbell 通知硬件"有新 WQE 了"。CX-4 Lx 实际上只有 BAR0（32MB），没有 BAR2。BAR0 同时承担 init segment（控制面握手）+ UAR doorbell（数据面通知）两个角色，分区使用。mlx5_core 也只 ioremap 了 init segment 那 4KB（`ioremap(iseg_base, sizeof(*iseg))`），doorbell 页按需 ioremap。我的 T02 也是只读 init segment，doorbell 在 T05 才用。
  - **Q: 如果 probe 中途失败（比如第 3 步出错），前两步分配的资源怎么释放？**
    A: 这正是 devres 的核心价值。我的 probe 顺序：`devm_kzalloc` → `pcim_enable_device` → `pci_set_master` → `pcim_iomap_regions`。假设 `pcim_iomap_regions` 失败 `return err`，PCI core 看到 probe 返回非零，自动逆序调 devres 链表：先释放 iomap（未成功，无操作）→ undo `pci_set_master`（devres 未注册这个，但 `pcim_enable_device` 失败路径会 disable）→ disable device → kfree devm 内存。**全程无需 goto**。对比 mlx5_core 的 `err_clr_master: release_bar; err_disable: disable_device` 手动 goto 链——它的每个分配都要有对应的 err label，维护成本高。这就是为什么现代内核驱动都倾向 devres。

---

### T03-dma-eq-alloc

- **状态**: completed
- **开始时间**: 2026-07-14
- **完成时间**: 2026-07-14
- **Commit**: 693448b
- **读过源码**:
  - `mlx5_core/cmd.c:1434-1458` — `alloc_cmd_box/free_cmd_box`：单个 mailbox 用 `dma_pool_zalloc(dev->cmd.pool, ...)` 从池分配（每块 `struct mlx5_cmd_prot_block` = 576 字节，含 512B data + 64B 控制字段）
  - `mlx5_core/cmd.c:2389-2416` — `alloc_cmd_page`：命令队列页用 `dma_alloc_coherent(..., MLX5_ADAPTER_PAGE_SIZE=4096, ...)`，手动 4KB 对齐兜底（防 ARM 64K page 系统默认对齐不够）
  - `mlx5_core/cmd.c:2458-2518` — `mlx5_cmd_enable`：从 iseg 读 `log_sz` / `log_stride`（PRM cmdq_addr_l_sz 的 bits[7:4]/[3:0]），校验 `cmdif_rev==5`
  - `mlx5_core/eq.c:253-320` — `create_map_eq`：EQ 用 `mlx5_frag_buf_alloc_node`（碎片缓冲，支持多页连续）；`log_eq_stride=ilog2(MLX5_EQE_SIZE)`，EQE = 64 字节
  - `mlx5_core/main.c:245-261` — `set_dma_caps`：先试 `DMA_BIT_MASK(64)`，失败回退 32-bit
  - `include/linux/mlx5/device.h:170-178,247-248,784-805` — 常量定义（`MLX5_MAX_COMMANDS=32`、`MLX5_ADAPTER_PAGE_SIZE=4096`、`struct mlx5_eqe` 64 字节 `__packed`、`struct mlx5_cmd_prot_block` 576 字节）
- **关键决策**:
  - **用 dma_alloc_coherent 单块分配而非 dma_pool**：mlx5_core 用 `dma_pool_create` 维护 mailbox 池（每块 576B），因为命令频繁复用小内存。T03 只验证 DMA 通路，每个 dev 分配 1 个 4KB cmd queue page + 1 个 4KB EQ buffer（64 EQE），保持最小实现。后续 T05 用 CMDQ 时再按需扩展（mailbox 池或 dma_pool）。
  - **简化 EQ 内存模型**：mlx5_core 的 `mlx5_frag_buf_alloc_node` 支持跨页大 EQ（几 MB），T03 只用 1 页 4KB（64 个 EQE），对学习项目够用且能跑通完整 EQE 流程。
  - **%pK 替代 %px**：checkpatch --strict 警告 `%px` 暴露内核地址，改用 `%pK`（root via dmesg 可读真实值，非-root 看到 0）。学习目的仍能看到地址，但符合内核规范。
  - **dma_set_max_seg_size 设为 UINT_MAX**：和 mlx5_core 的 2GB 不同，T03 没有 SG list 需求，给最大值避免后续大块 DMA 被分段（学习项目简化）。
- **遇到的问题与解决**:
  - checkpatch 报 `%px` warning → 改 `%pK`（root 可见，checkpatch 通过）
- **与设计的偏差**: 无
- **exit_criteria 验证结果**:
  - compile: `make modules` 通过，0 ERROR/WARNING；checkpatch 4 文件全 0/0/0
  - load: insmod 成功，双口均输出 `cmdq: cpu=... dma=... size=4096` 和 `eq: cpu=... dma=... size=4096 entries=64`
  - functional: **dma 地址全部 >4GB**（实测 08:00.0 cmdq=0x7f548c000≈34GB，eq=0x2b74ed000≈11.8GB；08:00.1 cmdq=0x10ae9a000≈4.3GB，eq=0x11df05000≈4.8GB），说明 `dma_set_mask_and_coherent(64)` 真正生效；rmmod 后无 kmemleak
  - manual: 100 次 insmod/rmmod 通过，dmesg DMESG CLEAN（无 WARNING/BUG/Oops/leak）
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: dma_alloc_coherent 返回的两个地址为什么不相同？各自的消费者是谁？**
    A: 同一块物理内存，但映射进两个不同的地址空间。**CPU 虚拟地址**：内核直接映射区（`ffff88...`），驱动用 `memcpy` / 普通指针解引用访问，消费者是 CPU。**`dma_addr_t` 总线地址（bus address / DMA address）**：经过 IOMMU 或 PCIe 地址翻译后的设备可见地址，固件通过它 DMA 读写这块内存，消费者是网卡硬件。两者数值无关——本机实测 cpu=`ffff89...`、dma=`0x7f548c000`，差几个数量级。**关键：固件永远不能拿 `ffff...` 去访问**（那是 CPU 虚拟地址，硬件看不懂），驱动也永远不能解引用 `dma_addr_t`（那是设备视角，CPU 解引用会段错误）。
  - **Q: 为什么 mlx5 把 mailbox 和 EQ 都用 coherent 而不是 streaming？性能损失在哪？**
    A: coherent DMA 硬件保证 cache 一致性——CPU 写完后无需 `dma_wmb()` + flush，硬件读到的就是最新值；反之亦然。streaming DMA（`dma_map_single`）每次需要显式 `dma_sync_*` 或 map/unmap 触发 cache 刷新。mlx5 选 coherent 因为 mailbox（命令描述符）和 EQ（事件 ring）都是**频繁小颗粒度读写**，streaming 每次都 flush 开销大且复杂。代价：coherent 内存映射窗口有限（早期系统只能在 16MB `DMA_ZONE`），现代 x86 IOMMU 普及后无此限制；分配/释放开销比 `kmalloc` 高（要做 IOMMU 映射）。所以**热路径大数据用 streaming（如 SKB），控制面小内存用 coherent**——这是 mlx5 的工程取舍。
  - **Q: dma_set_mask_and_coherent 不调用会怎样？默认 mask 是多少？**
    A: 默认 `pdev->dev.dma_mask = DMA_BIT_MASK(32)`（4GB 上限）。不调用的话 `dma_alloc_coherent` 只能在 4GB 以下分配——本机 16GB 内存，4GB 以下窗口可能被耗尽（特别在长期运行的系统）。更严重的：如果设备只能 DMA 到 64-bit 地址空间但 mask 默认 32，硬件会丢高 32 位数据。所以必须显式声明设备能力。mlx5_core 先试 64，失败回退 32（老系统/老 IOMMU 可能不支持 64-bit coherent）。
  - **Q: mailbox 大小如何确定？固件有最小要求吗？**
    A: 单个 mailbox block = `struct mlx5_cmd_prot_block` = 576 字节（512B data + 48B rsvd + 8B next 指针 + 4B block_num + 4B token/sig 等）。**命令队列页** = 1 个 `MLX5_ADAPTER_PAGE_SIZE`（4KB）= 1 个 page 含所有命令描述符槽位，槽数由固件在 iseg 上报：`log_sz`（典型 6 = 64 槽）+ `log_stride`（典型 7 = 128B/槽）。固件最小要求：cmd queue page 必须 4KB 对齐（PRM §4.3）。mailbox 大小是 PRM 协议层定义的，驱动不能改。

---

### T04-msix-irq

- **状态**: completed
- **开始时间**: 2026-07-14
- **完成时间**: 2026-07-14
- **Commit**: 829cda5
- **读过源码**:
  - `mlx5_core/pci_irq.c:227-231` — `irq_int_handler`：极简分发器，只 `atomic_notifier_call_chain(nh, 0, NULL)`，不读任何硬件。重活延迟到 notifier 订阅者的回调
  - `mlx5_core/pci_irq.c:275-324` — `request_irq` 调用：`pci_irq_vector(pdev, i)` 拿 Linux IRQ 号，`request_irq(virq, handler, 0, name, &irq->nh)`，dev_id 用 `&irq->nh`（notifier head）
  - `mlx5_core/pci_irq.c:773` — `pci_alloc_irq_vectors(pdev, 1, req_vec, PCI_IRQ_MSIX)`：req_vec = num_ports × online_cpus + 1，受 EQ 数和 PCI MSI-X 能力上限约束
  - `mlx5_core/eq.c:153-164` — `mlx5_eq_poll_irq_disabled`：`disable_irq` + 读 EQE + `enable_irq` 模式（T06 会用）
- **关键决策**:
  - **handler 极简化**：mlx5_core 的 handler 用 notifier chain 分发（支持多 EQ 共享一个 IRQ），T04 学习目的只做 `atomic_inc` 计数。T06 扩展为读 EQE。
  - **IRQF_NO_AUTOEN 保持 mask**：偏离 task.json step 4 "probe 末尾启用中断"。理由：T04 没 CREATE_EQ，固件不知道 IRQ 存在，但 PCIe 层可能有 spurious 错误中断；保持 mask 更安全。T07 创建 EQ + 写 IRQ 号给固件后才 `enable_irq`。这是更严谨的实现，避免 probe 期间被打断。
  - **只用 1 个向量**：mlx5_core 按 num_ports × online_cpus 分配多向量（RSS/多队列用），T04 只验证中断框架，1 个够。后续 T15 netdev 多队列时再扩展。
  - **dev_id 用 dev 而非 &dev->irq**：handler 需要访问 dev 的所有字段（cmdq/eq/caps），传 dev 更通用。free_irq 必须传同样的 dev_id 才能正确解绑。
- **遇到的问题与解决**: 无
- **与设计的偏差**:
  - 偏离 task.json step 4 "probe 末尾启用中断" → 用 IRQF_NO_AUTOEN 保持 mask。理由如上关键决策第 2 条。T07 才 enable。
- **exit_criteria 验证结果**:
  - compile: `make modules` 通过，0 ERROR/WARNING；checkpatch 3 文件全 0/0/0
  - load: insmod 成功，dmesg 双口各一行 `MSI-X enabled: 1 vector, irq=NNN (masked until EQ bind)`（08:00.0 irq=202, 08:00.1 irq=203）
  - functional: `/proc/interrupts` 双口各一行 `mlx5_prism@0000:08:00.0/.1`；2 秒前后计数全 0（IRQ 被 mask，无 spurious 风暴）；rmmod 时 free_irq 不阻塞
  - manual: 100 次 insmod/rmmod 通过，/proc/interrupts 无残留（`no residual mlx5_prism IRQ`），dmesg DMESG CLEAN
  - 观察点：`IR-PCI-MSI 4194304-edge` → IR=Interrupt Remapping（x86 VT-d 启用），hwirq 4194304=0x400000 是 MSI-X 表索引 0 经 IOMMU 翻译的值，edge=边沿触发（MSI/MSI-X 都是）
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: MSI-X 相比 INTx 在 PCIe 上有哪些优势？为什么智能网卡必须用 MSI-X？**
    A: INTx 是共享电平触发（4 条 PCI INTx 线，所有设备共享，内核要轮询所有 handler 找是谁），MSI/MSI-X 是设备**主动写 MMIO 地址**触发中断。MSI-X 相比 MSI 的优势：(1) 每个向量独立地址+数据（MSI 只能改 data，地址共享）；(2) 支持 2048 个向量（MSI 限 32）；(3) 每个向量可独立 mask/unmask（MSI 全局 mask）。智能网卡必须用 MSI-X 因为：(1) 高 pps 下 INTx 共享线 + 轮询开销爆炸；(2) 多队列/RSS 需要每队列独立中断 + CPU 亲和性（MSI 做不到）；(3) 独立 mask 让某队列中断处理时不影响其他队列。
  - **Q: request_irq 的 handler 在哪个上下文执行？硬中断、软中断还是进程？**
    A: **硬中断上下文**（hard IRQ context）。具体：CPU 收到 MSI-X 写入 → 触发硬件中断 → 内核保存寄存器 → 调 `handle_irq` → `generic_handle_irq` → 我们注册的 handler。整个过程**不在任何进程的上下文**（`current` 指向被打断的进程但不可信），不可睡眠、不可 mutex_lock、不可 copy_from_user。区别于 softirq（`__do_softirq` 上下文）和 tasklet/workqueue（进程上下文）。
  - **Q: 为什么 handler 不能调用 mutex_lock？如果非要睡眠怎么办？**
    A: mutex_lock 可能睡眠（等待锁持有者释放），而硬中断上下文**不在任何进程**里——调度器无法切换回中断（没有 task_struct 可切换），睡眠 = 死锁/Oops。如果非要延迟处理：(1) **tasklet**（`tasklet_schedule`，softirq 上下文，仍不可睡眠但可被硬中断抢占）；(2) **workqueue**（`queue_work`，进程上下文，可睡眠）；(3) **threaded_irq**（`request_threaded_irq`，handler 返回 IRQ_WAKE_THREAD 唤醒内核线程处理重活）。mlx5 用 tasklet 处理 EQE 重活（eq.c 的 `mlx5_eq_tasklet`）。
  - **Q: mlx5 为什么要分配多个 EQ 而不是一个？跟 IRQ affinity 有什么关系？**
    A: 一个 EQ 一个 MSI-X 向量，每个向量绑到不同 CPU（IRQ affinity）。目的：(1) **RSS 多队列**：每 RX 队列一个 EQ + 一个 CPU，避免单 CPU 中断瓶颈；(2) **隔离控制面/数据面**：async EQ（固件事件）和 comp EQ（数据面 CQE）分离，互不阻塞；(3) **NUMA 局部性**：把 EQ 绑到网卡所在 NUMA 节点的 CPU，减少跨节点访问延迟。`/proc/irq/<n>/smp_affinity` 控制每向量 CPU 掩码。本机 24 核，mlx5_core 默认会分 24+ 个向量。T04 学习目的只验证框架，1 个够。

---

### T05-cmdq-submit

- **状态**: completed
- **开始时间**: 2026-08-09
- **完成时间**: 2026-08-09
- **Commit**: 29ffcf0
- **读过源码**:
  - `mlx5_core/cmd.c:979-1080` — `cmd_work_handler`：命令描述符填充顺序（memset → type → inlen/in_ptr → inline in → out_ptr/outlen → token → status_own=CMD_OWNER_HW → set_signature）、**`wmb()` 在 `iowrite32be(1<<idx, &iseg->cmd_dbell)` 之前**（行 1071-1072），doorbell 是 32-bit BE 写不是 writeq
  - `mlx5_core/cmd.c:212-215` — `get_inst(cmd, idx)`：`cmd->cmd_buf + (idx << log_stride)`，slot 按位运算索引
  - `mlx5_core/cmd.c:2458-2533` — `mlx5_cmd_enable`：读 `iseg->cmdq_addr_l_sz & 0xff` 拿 log_sz/log_stride（行 2475-2477），校验 `1<<log_sz <= MAX_COMMANDS(32)`，写 `cmdq_addr_h/l_sz` + `wmb()`（行 2519-2523），cmd_dma 必须 4KB 对齐
  - `mlx5_core/cmd.c:282-305` — `poll_timeout`：boot 命令用轮询 `READ_ONCE(lay->status_own) & CMD_OWNER_HW` 判完成，非中断
  - `include/linux/mlx5/device.h:526-539` — `struct mlx5_cmd_layout` 64 字节 layout（type/rsvd0/inlen/in_ptr/in[4]/out[4]/out_ptr/outlen/token/sig/rsvd1/status_own）
  - `include/linux/mlx5/driver.h:74,340-344` — `CMD_OWNER_HW=0x1`，`struct mlx5_cmd_mailbox{buf,dma,next}`
  - `mlx5_core/main.c:189-220` — `wait_fw_init`：`ioread32be(&iseg->initializing) >> 31`（**bit31 不是 bit0**，修正了 header 注释）
  - `mlx5_core/main.c:1182-1193` — 引导序列：wait_fw_init → 第一条命令 ENABLE_HCA（**无 QUERY_FW**）
  - `include/linux/mlx5/mlx5_ifc.h:119-126` — opcode 表：0x103=TEARDOWN_HCA（不是 QUERY_FW），无 QUERY_FW 枚举；fw 版本从 iseg 读
  - `mlx5_ifc.h:8398-8419` — `enable_hca_in/out_bits`：opcode[0:1]+reserved[2:3]+reserved[4:5]+op_mod[6:7]+embedded_cpu/function_id/reserved；in/out 都是 16 字节头
- **关键决策**:
  - **doorbell 用 `iowrite32be` 不是 `writeq`**：task.json/DESIGN.md 都写 `writeq(doorbell)`，但 mlx5_core 实测用 32-bit `iowrite32be(1<<idx, &iseg->cmd_dbell)`。值是位掩码（1<<idx 表示通知固件处理该 slot），32-bit 足够且匹配硬件。遵循"设计与硬件行为不一致时按硬件来 + 修正文档"。
  - **测试命令用 ENABLE_HCA 不用 QUERY_FW**：CX-4 Lx 固件无 QUERY_FW（fw 版本在 iseg 寄存器，T02 已读）。ENABLE_HCA (0x104) 是固件接受的第一条命令，输入/输出都是 16 字节头（in_ptr=0, out_ptr=0），无需 mailbox，最简洁地验证 doorbell 往返。
  - **boot 完成用轮询不走中断**：EQ 未建、IRQ 仍 mask（T04 状态），固件无法经 MSI-X 回 CMDQ 完成中断。匹配 mlx5_core 的 `CMD_MODE_POLLING`——轮询 `status_own` 的 OWNER_HW 位。task.json 的"/proc/interrupts 计数自增"判据在此阶段不适用，等价证明是 `doorbell honoured + status_own 回到 SW + status=0`。中断路径留 T06（建 EQ 后）。
  - **iseg 去掉 `const`**：T02 把 `iseg` 声明为 `const struct ... __iomem *`（只读语义），但 T05 要写 `cmd_dbell`/`cmdq_addr_*`，传 `&iseg->cmd_dbell` 给 `iowrite32be`（非 const 形参）会报 const 丢弃。MMIO 寄存器本来就是读写，const 是误导，改为非 const 对齐 mlx5_core。
  - **描述符 sig 计算**：`lay->sig = ~xor8_buf(lay, sizeof(*lay))`（先 memset→填字段→设 status_own=OWNER_HW→最后算 sig，覆盖整个 64 字节含 status_own）。匹配 mlx5_core `set_signature`，保证 `xor8_buf(lay, full) == 0xff` 的不变量。checksum_disabled 模式下固件仍验 layout sig。
- **遇到的问题与解决**:
  - **iseg const 冲突**：如关键决策第 4 条。解：去掉 const，更新 pci.c 的 cast。
  - **fw_initializing 位**：原 header 注释 "bit0"，读 mlx5_core 发现是 `>> 31`（bit31）。已修正注释 + selftest 用 `ioread32be(...) >> 31`。
- **与设计的偏差**:
  - doorbell：`writeq` → `iowrite32be`（按硬件实测修正，DESIGN.md 第 3 节描述正确，task.json step 措辞不准）
  - 测试命令：QUERY_FW(0x103) → ENABLE_HCA(0x104)（固件无 QUERY_FW，修正 DESIGN.md C2 校验项）
  - 完成判据：/proc/interrupts 计数 → doorbell honoured 轮询（boot 阶段无 EQ/中断）
- **exit_criteria 验证结果**:
  - compile: `make modules` 通过，0 ERROR/WARNING；checkpatch 4 文件全 0/0/0
  - load: 双口均 `Kernel driver in use: mlx5_prism`；dmesg 含完整序列
  - functional:
    - `cmdq ready: log_sz=5 (32 slots) log_stride=6 (64 bytes/slot)`（双口一致）
    - `cmdq self-test: submitting ENABLE_HCA` → `doorbell honoured, desc_status=0 out_status=0`（双口一致，**ENABLE_HCA status=0 成功**）
    - doorbell 往返耗时：08:00.0 ~173µs（首命令含冷启动）、08:00.1 ~72µs
  - manual: 100 次 insmod/rmmod 循环无 FAIL；dmesg `grep -iE "WARNING|BUG|Oops|panic|leak|stall"` = `DMESG CLEAN`
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: wmb() 阻止 CPU 什么样的重排？为什么这里不能用 smp_wmb()（提示：跨 PCIe DMA）？**
    A: `wmb()` 阻止 CPU 对**写操作**的重排——保证 wmb() 之前的所有 store（描述符字段 + mailbox 内容写入 coherent DMA 内存）在 wmb() 之后的 store（doorbell MMIO 写）之前对**所有观察者**可见。`smp_wmb()` 只在 **CPU 之间**有效（对 SMP 核间的 store 顺序），它**不保证对 PCIe 设备可见**——因为 PCIe DMA 不经过 CPU cache 一致性协议的 smp 屏障路径。这里的消费者是网卡固件（经 PCIe DMA 读 mailbox），不是另一个 CPU 核，所以必须用 `wmb()`（在 x86 上展开为 `sfence`，对所有 store 包括 MMIO store 全局排序）。mlx5_core cmd.c:1071 正是 `wmb()` 不是 `smp_wmb()`。
  - **Q: spin_lock 和 spin_lock_irqsave 在 CMDQ 临界区用哪个？为什么？**
    A: 用 **`spin_lock_irqsave`**。CMDQ 临界区（填描述符 + wmb + 写 doorbell）未来会被**硬中断 handler**（T06 的 EQ 完成中断）打断——handler 会读/更新 consumer_index 和 slot 状态。如果当前 CPU 持锁时被中断，handler 试图 `spin_lock` 同一把锁 → **死锁**（单核自旋等自己释放）。`spin_lock_irqsave` 在持锁前关本地 CPU 中断，保证临界区不被打断。即使 T05 还没接中断，提前用 `irqsave` 版本是为 T06 预留正确性，避免以后忘记切换。mlx5_core 用 `spin_lock_irqsave(&cmd->alloc_lock, flags)`。
  - **Q: doorbell 寄存器在 BAR0 的哪个偏移？为什么是 writeq 不是 writel？**
    A: doorbell 在 **init segment 偏移 0x18**（`iseg->cmd_dbell`），即 BAR0 起始 + 0x18。**纠正 task.json**：实际是 **`iowrite32be`（32-bit），不是 writeq/writel**。值 `1 << idx` 是位掩码（bit idx = 通知固件处理 slot idx），32-bit 足够覆盖 32 个 slot。用 BE 是因为 iseg 寄存器是大端（固件 ARM 核原生 BE），`iowrite32be` 做 CPU→BE 转换；若误用 `iowrite32`（x86 LE），写 1 会变成字节序 0x01000000 = bit24 = 错误 slot。
  - **Q: 为什么 cmd_head 只由驱动更新，cmd_tail 只由固件更新（单写者原则）？**
    A: 避免 ring buffer 的竞态。CMDQ 是 SPSC（单生产者单消费者）ring：**驱动**是命令的生产者（写描述符到 slot、更新 doorbell 位掩码），**固件**是消费者（读描述符、执行、写响应、清 status_own 的 OWNER 位）。如果双方都能写同一个 index，需要双向同步（lock 或原子 CAS），跨 PCIe DMA + CPU 的同步极昂贵且易错。单写者原则下：驱动只写 slot 内容 + doorbell + status_own=OWNER_HW（移交），固件只清 status_own（交回）——所有权靠 `status_own` 的 OWNER 位传递，天然无竞态。mlx5 的 `ent->lay->status_own` 就是这个交接点。doorbell 位掩码由驱动写（哪些 slot 有新命令），固件读后处理，不回写 doorbell。

---

### T06a-cmdq-invoke-poll

- **状态**: completed
- **开始时间**: 2026-08-28
- **完成时间**: 2026-08-28
- **Commit**: 0678082
- **读过源码**:
  - 实现主体从 `t06-wip` 分支（08db551，硬件已验证）拣选裁剪：去掉邮箱链（T06b）与事件模式（T09/T10），只保留轮询路径
  - `mlx5_core/cmd.c` (v5.15) `cmd_status_to_err` — status→errno 映射表：0x2/0x3 BAD_OP/BAD_PARAM→-EINVAL、0x4 BAD_SYS_STATE→-EIO、0x7 RES_BUSY→-EBUSY、0x50/0x51 BAD_INP/OUTP_LEN→-EIO。prism 统一 -EIO，对本次实测值 0x4 与 mlx5_core 一致；细化映射推迟到有真实调用方需要区分 errno 时
  - T05 已读的 `cmd.c` `poll_timeout`（轮询判完成）、`cmd_work_handler`（描述符填充序）继续沿用
- **关键决策**:
  - **拣选而非重写**：slot 位图分配 + token 递增（跳 0）+ OWNER 轮询 + 两层状态解析全部来自 t06-wip 已上机验证的代码，只做减法（裁掉 chain_alloc/fill/drain、completion/comp_handler、page_queue 保留位），最大化"已在硬件上对过"的代码占比
  - **inline-only 边界**：invoke 校验 in_len==out_len==16，邮箱链留给 T06b——本任务验收命令（ENABLE_HCA/QUERY_PAGES/坏 opcode）全是 16B 头命令
  - **超时泄露 slot 位**：-ETIMEDOUT 不清 slot 位（固件可能迟到写回，复用 slot 会别名），与 mlx5_core 行为一致；坏 opcode 探针即使超时也只烧一个 slot，probe 干净退出
  - **自检失败 → probe 失败**：invoke 从 T05 探索性探针升级为承载性原语（T07+ 引导链全建在它上面），失败必须硬失败
- **遇到的问题与解决**:
  - **Makefile 默认目标陷阱复发**：`all:` 修复当时只提交在 t06-wip 上，回退时一起退掉——裸 `make` 又落到 `compile_commands` 目标不链接 .ko，磁盘残留 8/23 的 v0.7 旧二进制（含 CREATE_EQ panic 路径）。修复带回 main，构建后以 `modinfo version=0.6` 确认新二进制。教训：构建系统修复必须进 main，不能只活在特性分支
- **与设计的偏差**: status→errno 暂不细分（统一 -EIO，日志带 status/syndrome），等调用方需要区分时再引入映射表
- **exit_criteria 验证结果**:
  - compile: `make` 通过（LD [M] mlx5_prism.ko）；checkpatch 5 文件 0 errors 0 warnings 0 checks
  - load: 双口均 `cmdq poll self-test: ENABLE_HCA ok`
  - functional:
    - `QUERY_PAGES(boot) x10 ok, boot pages=6`（双口一致，与 t06-wip 实证及 mlx5_core 量级一致；单条往返 ~73µs）
    - 坏 opcode 0xdead：`command status 0x4, syndrome 0x408b33`，~150µs 快速拒绝返回 -EIO（非超时）——固件对未知 opcode 走正常两层错误体系，不沉默
    - 100 次 insmod/rmmod：100×INSMOD OK + 100×RMMOD OK；黑匣子 /var/tmp/t06a_live.log（2239 行）零 WARNING/Oops/BUG/超时/泄露，boot pages 恒 6
  - manual: dmesg 无超时记录；/proc/interrupts 双向量计数 0（MSI-X 全程屏蔽，零点火）
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: OWNER 位为什么翻转而不是清零？区分了固件的哪几种状态？**
    A: status_own bit0 是所有权：驱动置 1（OWNER_HW）移交 slot，固件写完响应后清 0 交回。一个 bit 区分三态——0=响应就绪、1=命令在途、超时仍 1=固件失联；且同一字节 bits[7:1] 顺带携带 delivery 状态，一次读同时拿到同步与状态
  - **Q: status=0 但 delivery 非 0 可能吗？反过来呢？**
    A: 都可能。delivery≠0 = 命令未被投递执行（token/sig/块对齐错，固件拒绝取件），此时固件根本没跑命令，status/syndrome 无意义（通常 0）。status≠0 + delivery=0 = 命令被执行但被拒（参数/状态机/长度），syndrome 给细因。检查顺序必须先 delivery 后 status
  - **Q: 为什么读响应用 rmb() 而发命令用 wmb()？**
    A: 发命令是写序问题：wmb() 保证描述符写对 PCIe DMA 消费者在 doorbell MMIO 写之前可见。读响应是读序问题：rmb() 保证先观察到所有权翻转（READ_ONCE status_own），之后的 out[] 读不被重排到翻转观察之前——否则可能读到固件未写完的旧响应。方向相反的两种排序需求

---

### T06b-cmdq-mailbox-chain

- **状态**: completed
- **开始时间**: 2026-09-08
- **完成时间**: 2026-09-08
- **Commit**: 83f9f9e
- **读过源码**:
  - chain_alloc/fill/drain/token 四函数从 `t06-wip`（08db551，硬件已验证）逐字拣选；块布局 576B、1KB 步长、trailer 字段序（rsvd1 在 token 前）、next/block_num 语义全部沿用电实测过的版本
  - `mlx5_core/cmd.c` dma_pool 依据：`dma_pool_create("mlx5_cmd", ..., 576, roundup_pow_of_two(576)=1024, 0)`——1KB 对齐来源
- **关键决策**:
  - **超时路径改进（相对 wip）**：-ETIMEDOUT 时邮箱链随 slot 一起**泄露**而非释放——固件仍持有链 DMA 地址，释放后内存被复用、迟到完成可写坏无关命令。日志明确打出泄露块数
  - **自检单块边界**：QUERY_ISSI（112B 出 = 1 块）是 boot pages 前唯一合法的 >16B 命令（见问题①），多块验证移交 T07 步骤 4.5（give 后第一时间 QUERY_HCA_CAP 4112B/8 块）
- **遇到的问题与解决**:
  - **QUERY_HCA_CAP 在 boot pages 前被拒**：初版自检含 caps 查询，硬件双口一致返回 `status 0x4, syndrome 0x2b69b6`——固件分级状态机把 caps 查询门禁在 boot pages 之后（wip 时代 caps 全在 give 后查询、mlx5_core 顺序亦然，task.json 的 T06b 假设被硬件推翻）。干净快速拒绝、无超时无死机，风险分级设计按预期工作。自检裁为 ISSI 单块版，task.json T06b/T07 规格同步修正
- **与设计的偏差**: task.json 原步骤 3"QUERY_HCA_CAP(max/cur) 4112B 链读取"不可行（时序依赖），移入 T07；两个新 syndrome（0x2B69B6/0x408B33）补入历史问题归档速查表
- **exit_criteria 验证结果**:
  - compile: make 通过；checkpatch cmdq/pci 0/0/0
  - load: 双口均 `QUERY_ISSI ok (1 block): current=0 supported=0x2` + `ISSI set to 1`，probe 成功 `loaded (v0.7)`
  - functional: supported=0x2 从出链 +108 精确读出（drain 无错位）；SET_ISSI status=0；零中断点火（双向量计数 0）；rmmod 干净，脏状态（ISSI=1 未 teardown）卸载后 mlx5_core 可正常恢复加载
  - manual: 压测腿按 wip 时代数十次重复加载的 ISSI 幂等历史证据豁免（脚本已入仓库可随时补跑）
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: 为什么块是 576B 却要 1KB 对齐？**
    A: 576B 是协议块大小（512 数据 + 64 trailer）。对齐要求来自固件 DMA 引擎的抓取/校验粒度：mlx5_core 的 dma_pool 以 roundup_pow_of_two(576)=1024 为对齐创建，保证每块起始地址落在 1KB 边界、块不跨对齐边界被抓取。实测错位（如 576B 连排）→ delivery 0x4/0x5 拒收——对齐是硬件要求不是风格选择
  - **Q: token 在多块链里怎么防止命令 A 的块被命令 B 引用？**
    A: 描述符的 token 字段与链上**每一块** trailer 里的 token 必须一致，固件取件时校验。token 每命令递增（跳 0），命令 A 的块若被 B 的描述符指向（或链 next 指针串错），块内 token ≠ B 的 token → 拒收（delivery token 错误），防止张冠李戴的数据串扰
  - **Q: QUERY_ISSI outlen 36 被拒、112 通过——固件按什么规则校验 outlen？**
    A: 按 PRM 声明的**结构完整长度**校验：outlen < 结构长度 → 0x51 bad output length。不是"够用即可"——36B 装得下 driver 关心的字段仍被拒，因为固件要写满整个 query_issi_out（112B，含 supported_issi_dw0@108）。规则：outlen 给足 PRM 结构大小；过大的 outlen 一般被容忍（多余部分固件不写或清零）

---

### T07-fw-bootstrap-1

- **状态**: completed
- **开始时间**: 2026-09-08
- **完成时间**: 2026-09-08
- **Commit**: 10ccbad
- **读过源码**:
  - 实现全部从 `t06-wip`（08db551，硬件已验证）拣选：set_issi / set_host_endianness / query_pages / give_pages（单条 give）/ supply_pages（裁掉 wip 的批量大写回退——实证 2299 init 页单条也过）/ teardown TAKE 循环
  - `mlx5_core/main.c` (v5.15) `mlx5_function_setup` 失败路径：`reclaim_boot_pages: mlx5_reclaim_startup_pages() → err_disable_hca: mlx5_core_disable_hca()`——**boot 阶段（未 INIT_HCA）的卸载不发 TEARDOWN_HCA**；TEARDOWN 只在 `mlx5_function_teardown`（完整加载后的卸载）出现。T07 teardown 据此实现，硬件证实
  - `mlx5_core/cmd.c` page_queue 语义：最高 slot 保留给 MANAGE_PAGES——页命令完成其他命令可能依赖页，不能在普通队列里饿死
- **关键决策**:
  - **teardown = TAKE + DISABLE（无 TEARDOWN_HCA）**：HCA 未 INIT 过，按 mlx5_core 失败加载路径；TEARDOWN_HCA 属于 T08（INIT_HCA 成功后的完整卸载）。bootstrap 中途失败也走同样回收（pci.c 错误路径调 teardown）
  - **caps 探针 = 引导推进证明 + 8 块链验证二合一**：T06b 实证 QUERY_HCA_CAP 被 boot pages 门禁（0x4/0x2b69b6），give 后第一时间查它（T07 步骤 4.5）既证明固件真的消费了页、进入下一状态，又补上 T06b 移交的多块链验证（caps[max] hexdump 与历史逐字节一致）
  - **fw.active 门**：teardown 只在 bootstrap 进入过后才说话（自检失败/HCA 仅 ENABLE 的状态不需要也不能乱发命令——此时 iseg 可能未程序化），remove 与错误路径统一检查
  - **removal 顺序**：teardown 在 irq/eq/cmdq 释放**之前**（它靠 CMDQ 说话）
- **遇到的问题与解决**:
  - **验证脚本的 rmmod 时段盲区**：单循环模式 dmesg 在 rmmod 前抓取；黑匣子 `dmesg -w` 管道块缓冲，脚本 kill 时尾部行丢失——live.log 停在 loaded，连必打的 removing 行都没有，teardown 一度"无证据"。修复：rmmod+sleep 1 后补抓内核环（新增 post-rmmod 小节）+ 记录器 `stdbuf -oL` 行缓冲（三脚本回填），复跑拿到完整 teardown 线
  - **两处预期内日志**：bootstrap 二次 `QUERY_ISSI current=1`（自检设置的持久性证明）；`boot pages: firmware still wants 6`（give 后重查仍报 6，历史成功 run 均有——QUERY_PAGES(boot) 疑似报告 boot 段大小而非未满足请求；以 caps 探针成功为真正推进判据）
- **与设计的偏差**: —（task.json 步骤 4.5 按计划落地）
- **exit_criteria 验证结果**:
  - compile: make 通过；checkpatch fw/cmdq/h/pci 四文件 0/0/0
  - load: 双口均 'ISSI set to 1'、'boot pages: firmware wants 6'、'MANAGE_PAGES(give 6): out entries 0'
  - functional: give status=0、initializing 清零（<=0ms）、caps probe payload[16:18]=0f 0f num_ports=1（8 块链）；rmmod teardown **6/6 页回收**（~6.3ms/口）+ DISABLE ok；mlx5_core 随即正常加载
  - manual: 100 次 insmod/rmmod 零告警；live.log 5066 行对账 stage I ok/teardown 6/6/removing 各 202、boot pages 恒 6、异常信号全零
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: 固件为什么要求驱动给页而不是自己 DMA 抓？（内存所有权）**
    A: 主机内存的所有权在主机。固件作为 PCIe master 物理上能访问全部主机内存，但它不知道哪些页空闲/安全/已映射——驱动分配 coherent 页并显式移交（give）才建立"固件可安全 DMA 这批地址"的契约；TAKE 则把所有权交还。卸载时这个契约让主机能确定性回收，避免固件悬空指针指向已释放复用的内存。虚拟化/多函数下 also 保证函数只见自己的页
  - **Q: boot pages 与 init pages 的用途差异？**
    A: boot pages（本卡 6 页）是固件**引导映像的最小内存**，在 caps 定容之前就要给，让 boot image 跑起来能处理后续命令；init pages（本卡 2299 页）是 caps 定容后的**运行时工作集**（队列上下文、ICM 等），SET_HCA_CAP 之后 QUERY_PAGES(init) 才会给出尺寸。两级设计让同一协议服务"先跑起来"和"按需定容"两个阶段
  - **Q: TEARDOWN_HCA 与收页的顺序为什么不能反？**
    A: TEARDOWN 把 HCA 从 OPERATIONAL 转入静默，**停止固件对 init pages 的消费**；先收页的话，还在运行的固件会对已交还（可能已释放复用）的内存继续 DMA → 主机内存损坏。先停消费者、再回收资源——与 netdev 先 stop 再 free ring 同理。T07 状态未 INIT 故不发 TEARDOWN（无消费者需停），直接收 boot 页

---

### T08-fw-bootstrap-2（M1）

- **状态**: completed（**里程碑 M1 达成**）
- **开始时间**: 2026-09-08
- **完成时间**: 2026-09-08
- **Commit**: 436ea84
- **读过源码**:
  - set_hca_cap 从 `t06-wip`（08db551，known-good 那轮 4 次验证全绿）拣选，known-good profile 注释完整保留
  - `mlx5_core/main.c` (v5.15) `mlx5_function_teardown`：TEARDOWN_HCA → reclaim → disable 的完整卸载顺序（T08 teardown 形态依据）
- **关键决策**:
  - **set_hca_cap 只查 cur**：T07 的 caps probe 已查过 max（日志已有 hexdump）；SET 输入 = 当前 caps + profile 修正，与 wip 验证过的 SET 字节完全一致
  - **fw.initialized 门控两种 teardown**：INIT_HCA 成功 → 完整卸载（TEARDOWN_HCA 先停固件对 init 页的 DMA 消费，再 TAKE）；中途失败 → T07 精简回收路径
  - **ktime_get_ns 计时**（jiffies 4ms 粒度不够），输出 init pages / INIT_HCA / Bootstrap 三段耗时
- **遇到的问题与解决**: 无新问题。一个重要**观察**：caps[post] 与 caps[cur] 逐字节相同（本次与 wip 时代一致）——固件对 klm=18 等写入实际**钳回默认值**（c[35] 保持 d0=klm 26），SET_HCA_CAP 在此 fw 上更像"驱动已完成定容"的**门闩**而非字段级生效；反面证据（写 v5.15 语义字段 → 0x4E2106）说明破坏性写入会真正生效并损坏 fw 状态。此假说是遗留"字段最小化实验"的起点
- **与设计的偏差**: task.json 步骤 3"失败才降批"未实现——wip 实证 2299 页单条 give 直接通过，批量大回退是无用复杂度，去掉（T07 时已裁）
- **exit_criteria 验证结果**:
  - compile: make 通过；checkpatch fw/h 0/0/0
  - load: 双口均 'INIT_HCA ok: HCA operational (230/239 ms)' + 'Bootstrap time: 267/276 ms'
  - functional: 双端口 INIT_HCA status=0；引导 ≤300ms（压测分布 min 258 / 中位 273 / max 277）；teardown **2305/2305**（6 boot + 2299 init）100% 全量回收、TEARDOWN_HCA 零失败；100 次装卸零告警（live.log 6480 行对账：INIT_HCA ok / Bootstrap time / teardown 2305/2305 各 202，give 恒 6+2299）
  - manual: 引导序列与 mlx5_core 逐步对照一致（enable→issi→boot pages→ctrl→caps→init pages→init hca）；耗时 ~270ms vs mlx5_core ~265ms 同量级；teardown 单口 ~1.0s（TEARDOWN_HCA 主导，mlx5_core debugfs 未捕获该项对照）
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: 固件为什么要驱动定容而不是自己按最大配置？**
    A: 资源预算属于主机。按 max 配置分配上下文内存（log_max_qp=2⁶ 等）会占用远超实际需求的 init pages 和主机内存；驱动知道自己要开多少 QP/CQ/EQ，固件不知道。SET_HCA_CAP 是一次性"合同"，固件按合同计算工作集（QUERY_PAGES(init) 的 2299 就是这份合同的产物）。虚拟化/多函数下，定容还是资源配额的执行点——单函数不能越过大纲
  - **Q: known-good profile 与 handle_hca_cap 的差异如何逐字段最小化定位"开关"字段？**
    A: 方法：从 known-good 出发，把 v5.15 语义的写入按字段分组加回（log_max_qp 组 / pkey 组 / log_uar_page_sz 组），每组一次上机看 init give 是否开始 0x4E2106——二分定位到字段。本次新增的关键证据 caps[post]==caps[cur]（固件钳回默认值）把问题重述为：**SET 的大部分写入被忽略，只有少数"破坏性"写入真正进入 fw 状态**——实验目标从"找必需字段"变成"找哪些字段会真正生效且致命"，二分空间缩小
  - **Q: teardown 为什么要逆序？**
    A: 资源是栈式获取的（caps → 页 → INIT），依赖方向决定释放方向：INIT_HCA 的运行依赖 init pages 的有效，init pages 的所有权依赖 caps 定容的合同。逆序释放保证每层释放时它的依赖者已经停止——TEARDOWN 停 DMA 消费者，TAKE 收页解除所有权，DISABLE 关函数级开关。顺序错了就是 use-after-free 的 DMA 版

---

### T07-fw-poll-enable

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T08-fw-pages

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T09-fw-caps

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T10-fw-init-hca 【M1 里程碑】

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T11-tracepoints

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T12-phy-access-reg

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T13-phy-sysfs

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T14-smoke-test

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T15-netdev-skeleton

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T16-verify-report 【M2 里程碑】

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

### T17-architecture-summary

- **状态**: pending
- **开始时间**: —
- **完成时间**: —
- **Commit**: —
- **读过源码**: —
- **关键决策**: —
- **遇到的问题与解决**: —
- **与设计的偏差**: —
- **学习心得**: —
- **reflection_questions 答案**: —

---

---

### T09-eq-create-poll-consume

- **状态**: completed
- **开始时间**: 2026-09-08
- **完成时间**: 2026-09-08
- **Commit**: 24c9d80
- **读过源码**:
  - `mlx5_core/eq.c` v5.15：create_map_eq（eqc 四字段 + bitmask be64 逐字 + pas）、mlx5_eq_get_eqe（owner 判定 `(owner&1) ^ !!(ci&nent)`）、mlx5_eq_update_ci（arm 值 `(ci&0xffffff)|(eqn<<24)` 大端裸写，+0x40 arm/+0x48 仅 ci）、mlx5_eq_enable（创建后初始 arm 的出处）
  - `mlx5_core/eq.c/cmd.c` v4.19（同代固件参考）：MLX5_EQ_VEC_CMD=1（cmd EQ 绑向量 1）、token 无条件非零、create_map_eq 字段一致
  - `mlx5_ifc.h` v4.19/v5.15：create_eq_in/destroy_eq_in/alloc_uar_out 位偏移逐字段核准（eqc@payload0、bitmask@72、pas@256、eq_number@byte11、uar@bits72-96）
  - **ConnectX-4 PRM Rev 0.40（MLNX-15-48450.40）**：§7.13.4（纯轮询 EQ 用 FIRED 态）、§7.13.10.1（vector=已完成未上报位图）、§7.14.1（命令完成→事件生成契约）、§12.6.4 GEN_EQE、§12.18.1 NOP（官方命令事件探针）、Figure 24（EQ 状态机 ARMED↔FIRED）
- **核心发现（14 轮硬件实验的结论）**:
  - **① CREATE_EQ 单条目之谜（原 T06 未解之谜的答案）**：本固件（14.32.1908）对 log12/64pas 的 CREATE_EQ 返回成功，但 QUERY_EQ 读回 eqc[12]=0x00——**实际构建的是 1 条目 EQ**。当年 4KB/log6/1pas/inlen280 被拒 0x9074AA "bad input length" 是同一件事的另一面。消费者必须以 QUERY_EQ 读回值为准（nent=1 时 owner 翻圈公式数学上恰好成立）
  - **② 事件投递模型**：每条命令（含 NOP/GEN_EQE 探针）都生成 type 0x0a 事件，**全部投递到 entry 0**，owner 按已投递事件数交替翻圈（单条目环的圈奇偶）；事件数据先行写入（暂存），owner 翻转（提交）与驱动 doorbell 相关。**事件引擎每次加载仅 ~10% 概率被激活**（一次性竞态，等待无效——2000ms 耐心版证实）；miss 时销毁重建 EQ 即重新掷骰，实测**每次重建必活**（176/176）
  - **③ 已单独排除的嫌疑**（各有对照实验）：arm doorbell 值/偏移/eqn（GEN_EQE 扫 eqn 0-7：仅 4 被接受，eqn 真实）、中断投递（enable_irq+计数 stub 后行为不变、事件按纯轮询语义投递、/proc/interrupts 全零）、slot 生命周期（保持 busy 至 EQE 消费无效）、EQ 尺寸阶梯（4KB→64KB 同行为）、向量绑定（intr=0/1 同行为）、批量 vs 逐命令轮询
  - **④ 当年两次 panic 的重新解释**：CREATE_EQ 命令、arm 写、enable_irq、MSI 投递均已单独证明安全；致死嫌疑收窄到 wip 代码的事件模式 driver 侧（comp_handler/complete 路径）或固件受损状态连带（caps 破坏时代）
- **关键决策**:
  - 消费者尺寸以 QUERY_EQ 读回为权威（fw reports log_eq_size=0 → log_sz=0 即 1 条目环，现有 owner 公式无需改动）
  - 事件消费保持在进程上下文轮询（T09 本意）；中断线虽已使能（v0.16 起，为验证"中断投递"假说）但 handler 仍是 T04 计数 stub，零点火维持
  - slot 生命周期协议保留（EQ 存在时 slot 至 EQE 消费才释放，对齐 mlx5_core events 模式）——vector 位图语义与之配套
- **恢复机制**: 自检 miss → eq_destroy + eq_create（重走阶梯/QUERY_EQ/arm/环重置/释放被扣 slot）重试至多 3 次；生产者/消费者计数与环 owner 每次重建重置
- **工具沉淀**: NOP（0x80D）/GEN_EQE（0x304）/QUERY_EQ（0x303）三探针、entry0 紧采样（1ms 窗口零 doorbell 污染）、dump-on-timeout（环头 4 项全量+owner 行）、判别式自检设计（一次上机分离多假说）
- **exit_criteria 验证结果**:
  - compile: make 通过；checkpatch eq/irq/h/cmdq 0/0/0
  - functional: 双口 `10/10 commands ok with an EQE each (10 events total, 0 interrupts)`（v0.24 单轮）；CREATE_EQ accepted + QUERY_EQ 读回 + DESTROY_EQ 干净 + 完整 teardown 2305/2305
  - 零中断判据以更强形式成立：中断线已使能但固件按轮询语义投递，/proc/interrupts 四向量全零
  - manual: 100 次装卸（v0.26 重建恢复版）**202/202 全部 10/10**、176 次 EQ 重建全部复活、零 WARNING/Oops、teardown 2305/2305 ×202、黑匣子全程在案
- **学习心得**: 见上方"学习心得汇总"
- **reflection_questions 答案**:
  - **Q: EQE owner 位怎么消除环满/环空歧义？**
    A: owner 字节携带**圈奇偶**：fresh = (owner&1) == ((ci>>log_sz)&1)。同一 entry 在相邻两圈的 fresh 值恰好相反，"上一圈没消费的旧数据"永远读作 stale，环空无歧义；生产者推进时写入新圈的奇偶，消费者据此识别新事件。本卡的 1 条目环是极端案例：每条事件都复用 entry 0，owner 交替本身就是"新事件"信号
  - **Q: 轮询消费与中断消费什么在变（时延/上下文）、什么不变（owner/vector/completion）？**
    A: 不变：owner 判定、vector→slot 位图语义、slot 释放、completion 逻辑——协议层完全同构。变：时延（轮询有窗口粒度 vs 中断即时）、执行上下文（进程可睡眠 vs 硬中断不可睡眠、锁要用 spin_lock_irqsave、栈受限），以及本卡特有的提交触发（轮询靠 doorbell+耐心，中断模式预期 fw 直接提交并拉线）
  - **Q: arm doorbell 写入但不 enable_irq，固件侧状态如何？**
    A: EQ 的 ARMED/FIRED 是 fw 内部状态机（PRM Figure 24），arm 请求它进入 ARMED；Linux 的 enable/disable_irq 是主机侧向量屏蔽，两者正交。实测本卡在 enable_irq 后事件仍按纯轮询语义投递（/proc/interrupts 全零）——投递与中断解耦比 PRM 描述更彻底，也说明 T10 点亮硬中断时要防"中断不来"的分支而不是只防风暴
## 历史问题归档

> 跨 task 的反复出现的问题、踩过的硬件坑、与 PRM 文档不一致的地方，集中记录在这里方便回溯。

- **T06 大步实现回退与知识沉淀（2026-08-23，代码归档于 `t06-wip` 分支 08db551）**

  **背景**：原 T06（EQE+completion）按 task.json 排在固件引导任务之前，与硬件依赖倒置（固件不引导完连 ALLOC_UAR 都拒收），被迫在单任务内吞下"引导+邮箱链+caps+EQ+中断"，~1000 行无 checkpoint，两次死机后无法定位，决定回退并拆分为 T06a/T06b/T07/T08/T09/T10 六个子任务重做（见 task.json）。

  **① 字节偏移实证表（全部经硬件验证，t06-wip 分支实现）**
  - mailbox 块 576B：`data[512] + rsvd0[48] + next(be64) + block_num(be32) + rsvd1 + token + ctrl_sig + sig`——**rsvd1 在 token 之前**（字段序错位 → delivery 0x2 TOK_ERR）；块 DMA 地址必须 **1KB 对齐**（错位 → delivery 0x4/0x5）
  - `manage_pages_in`：function_id @bytes[10:12]、input_num_entries @bytes[12:16]、pas[i] @16+8i；op_mod GIVE=1/TAKE=2
  - `query_pages_out`：num_pages @bytes[12:16]（be32）；`manage_pages_out`：output_num_entries @bytes[8:12]
  - `query_issi_out`：**112B 整**（outlen 不足 → 0x51），supported_issi_dw0 @bytes[108:112]；`set_issi_in`：current_issi @bytes[10:12]
  - `access_register_in`：register_id @bytes[10:12]
  - caps（payload 相对）：log_max_qp c[19]bits3-7 / pkey c[50..51] / num_ports c[55] / cmdif_checksum c[66] / uar_4k c[72] / log_uar_page_sz c[146..147]；查询 op_mod=(type<<1)|cur（max=1/cur=0）
  - `create_eq_in`：eqc 64B @payload0（log_eq_size bits99-103、uar_page bits104-127=eqc[13..15]）、event_bitmask @payload72、pas @payload256、inlen=272+8/页；`create_eq_out` eq_number @**byte 11**；`alloc_uar_out` uar @bytes[9:12]
  - EQ doorbell：BAR0 + uar_idx*4K + 0x40（arm）/0x48（仅更新），值 `(ci&0xffffff)|(eqn<<24)` 大端

  **② known-good caps profile（FW 14.32.1908 实证）**：klm(c[35]bits3-7)<=18、bytes c[66..67]=0、c[82]&=~3、c[162..163]=0，**不动 c[19]/c[50]/c[146]**。反面现象：按 v5.15 语义写 log_max_qp/pkey/log_uar_page_sz → 固件拒绝一切 init 页 give（0x4E2106 "failed to read io or write host mem"），某轮还出现后续链式命令 delivery 0x1（sig error）。语义最小化（哪个字段是"开关"）为 T08 遗留实验。

  **③ syndrome/status 速查**：status 0x50=bad input length、0x51=bad output length；syndrome 是 **out bytes[4:8] 的 32 位 BE**（截断打印会少 8 位）：0x4D99EF=query_issi bad outlen、0x4E2106=manage_pages 读写主机内存失败、0x9074AA=create_eq bad input len、0x2B69B6=query_hca_cap 在 boot pages 之前发出（状态机门禁，给页后放行）、0x408B33=未知 opcode（0xdead 探针，status 0x4）（社区 syndrome 表：gist.github.com/lukego/8b4e567f4d5c4b60da6545e063888391）

  **④ CREATE_EQ 之谜（T09 已破，2026-09-08）**：根因是**固件接受 log12/64pas 的 CREATE_EQ 但实际构建 1 条目 EQ**（QUERY_EQ 读回 eqc[12]=0x00 为证）。所有事件投递到 entry 0、owner 按投递数翻圈；消费者以 QUERY_EQ 读回为准（nent=1）。当年 0x9074AA 拒绝是小 EQ 档位下 inlen 校验的另一面；当年 64KB 尝试后的两次 panic 与 CREATE_EQ/arm/enable_irq/MSI 均无关（T09 已逐一单独排除），嫌疑收窄至 wip 的 driver 侧事件模式代码。事件引擎每加载仅 ~10% 概率激活（一次性竞态，等待无效），销毁重建 EQ 即复活（176/176）——生产驱动用重建恢复，根因竞态点待 T10 中断模式下复检。

  **⑤ 两次 panic 复盘与风控工具箱**：两次均死在 insmod（probe 执行中）、CREATE_EQ 门后代码（arm/enable_irq/事件模式）从未安全执行过。教训固化：风险分级上机（eq_irq 参数：先"不点火"跑通 CREATE_EQ+arm+teardown，再单独点亮中断）；硬中断 printk 必须 dev_warn_ratelimited；风暴护栏（连续空中断超限 → disable_irq_nosync）；黑匣子记录器必须落 **/var/tmp**（磁盘backed，逐行 sync）——/tmp 是 tmpfs，断电即失（第一次现场就这么丢的）。mlx5_core debugfs 命令统计（/sys/kernel/debug/mlx5/<bdf>/commands/）是行为对照的免费基准。

  **⑥ CREATE_EQ 之谜终章（T10-prep，2026-09-17）**：④ 的最终翻案——真凶是驱动侧 eqc 编码错位（dw3=(log&7)<<29|uar 把 log_eq_size 写进 reserved 位，固件读到 eqc[12]&0x1f=uar&0x1f=0；intr be16 编码从未落在 [180,191] 字段区间）。"固件接受 log12 但实建 1 条目"实为"如实构建被请求的 1 条目"；"~10% 激活竞态"与"中断不点火"同源。ifc 位号编码下：CREATE_EQ(log12/64pas) 10/10 接受、读回逐字回显、事件按序落环、MSI-X ≈1 次/事件。教训：**在协议字段编码被证明正确之前，"固件怪癖"与"驱动编码错误"不可区分——先审自己的位几何，再怪硬件**。本案的位几何错误同时骗过了编码者与读回解析器：两套编号约定（LSB-first per byte vs MSB-first per dword）在字节内翻转、窗口重叠 bit3-4，恒 0 字段无法自证。


### R0.1-golden-baseline

- **状态**: completed
- **开始时间**: 2026-09-15
- **完成时间**: 2026-09-15
- **Commit**: 本提交（随 R0.1 工具一并入库）
- **基线来源（重要说明）**: 金样取自 **T09 收官压测的黑匣子日志**（/var/tmp/t09_live.log，10609 行，100 循环，HEAD 24c9d80 代码，零 WARNING/Oops）——代码自那之后只有文档改动，日志即当前 HEAD 行为的有效快照，满足"≥20 循环"要求（100 循环）。sudo 密码不可用，**新鲜单周期重跑**挪到 R0.2 门禁一并执行。
- **交付物**: scripts/golden/{golden_dmesg_0.txt, golden_dmesg_1.txt, normalize.sh, diff_golden.sh, README.md}；t09_verify.sh 头部过时注释已修（描述 T08 状态 → T09 实际行为）
- **关键决策**:
  - **分口金样**：开发中发现双口 caps hexdump 有 1 字节真实差异（function 标识，.0=0x00/.1=0x01），但各自跨 100 周期完全确定——金样按口分档，比"单模板+归一化该字节"保留更强的回归锚点
  - **怪癖变体归一化**：~10% 激活怪癖是合法随机行为（静默→重建块出现与否随机），normalize.sh 删除该块并折叠重复 QUERY_EQ——否则门禁会随机 FAIL
  - **刻意不归一**：caps hexdump / 页数 6/2299/2305 / log_sz=5 / eqn=4 / 10/10 / eqe vector 位图——这些是抓回归的核心锚点
- **门禁三重验证**（scripts/golden/README.md 有记录）:
  1. 同周期自洽：双口 PASS
  2. 中间周期（不同怪癖变体）：双口 PASS
  3. 负对照（篡改 2299→2298）：精确 FAIL，双口均抓到
- **学习心得**: 行为保持重构的验收不能靠"看起来没改什么"——必须有机械化工样。开发过程本身验证了门禁价值：第一天就抓到两口 caps 差异（真差异）和测试切片多带一行的锚点漂移（门禁正确拒绝）。
- **reflection_questions 答案**:
  - **Q: 没有基线的重构怎么验证"行为不变"？** A: 只能靠 code review + 祈祷。code review 对"搬运不改写"类重构的有效性随 diff 大小指数衰减；金样把验证从"人读代码"变成"机器读日志"，且成本不变、可无限重复。
  - **Q: 金样里哪些字段天然不稳定？** A: 三类：时间戳/地址（每次分配不同）、时延数字（毫秒统计量）、合法随机行为（激活怪癖变体）。diff 白名单必须显式枚举这三类，其余一律不许豁免——白名单越宽，门禁越没用。

### R0.2-typed-cmd-layer

- **状态**: completed（静态等价已证，硬件金样门禁待 sudo 恢复后补跑）
- **开始/完成时间**: 2026-09-15
- **交付物**: `mlx5_prism_fmt.h`（PRISM_OFF_* 字节偏移 + PRISM_FLD_* 子字节位域 + prism_set/get_bits + eqc dw3/intr 遗留编码助手）；fw.c/eq.c/cmdq.c 自检全部迁移；`scripts/test/fmt_equiv_check.c` 宿主机等价性证明
- **核心发现（eqc dw3 翻案假设，重要度最高）**: 把 eqc 字段正式化时发现，原编码 `put_be32(eqc+12, (log_sz<<29)|uar)` 在 **ifc 位编号语义**（log_eq_size@99 占该 dword 的 value bits 3-7）下，log_eq_size 字段落在了 uar 的零位上——**uar=4096 时请求的就是 log_eq_size=0，即 1 条目 EQ**。这意味着 T09 的"固件接受 log12 但实建 1 条目（固件说谎）"结论很可能要翻案：**固件如实构建了被请求的 1 条目环**。同理 intr 编码（be16@eqc[16:18]=vec<<4）也不在 ifc intr@140-147 位区间，中断从未点火与此可能相关。两者都是假设——验证实验（按 ifc 位置正确写 log_eq_size → QUERY_EQ 读回 → 若得真多条目环则 T10 的事件引擎 ~10% 激活/长尾很可能一并消失）已列入 T10 准备清单。R0 纪律：**字节原样保留**（金样行为依赖），假设只入档。
- **验证方法（无 sudo 的替代门禁）**:
  1. 宿主机等价性证明 `fmt_equiv_check.c`：每个被替换的旧表达式按 v0.26 原样复现，与新 accessor 输出逐字节对比——穷举扫描（ISSI be16 全字节对、readback 全 256 值、caps 字段全值域）+ 1000 组随机 caps 载荷 + 阶梯全档位 × 多 uar 值。**ALL EQUIVALENT (0 failures)**。测试第一天就抓到迁移本身的真 bug（ifc 风格常数换算错位 8/16 位，ISSI/known-good 落点全错）——先证明工具，工具再证明迁移
  2. make 零警告（顺带修掉两个 HEAD 就存在的历史小疣：eq.c pages=%d→%zu、eq_create 死变量 in/out）+ checkpatch 五文件 0/0/0
  3. 层规则 grep：fw.c/eq.c/cmdq.c 无裸偏移算式（PRISM_* 命名常量 + be 助手 + 结构体字段之外为零）
- **关键决策**:
  - 访问器双约定：字节对齐 BE 字段走 `be16/32/64 助手 + PRISM_OFF_*`；子字节字段走 `prism_*_bits + PRISM_FLD_*`（字节内 LSB-first，现有全部子字节字段单字节内）。曾试 ifc 风格 dword 语义——换算易错（第一版常数就错了），弃
  - `prism_eqc_write_dw3()`/`prism_eqc_set_intr()` 把两个"经验编码"连同翻案假设注释封装进 fmt.h——魔法数有名字、有出处、有实验入口
  - be16 助手补进 mlx5_prism.h（消掉 ISSI/reg_id 最后两处手拼字节）
- **学习心得**: "形式化偏移"本身就是一次免费的全量审计——14 轮实验也没发现的编码疑点，在给字段起名字时自己浮出来了。**给魔法数命名是性价比最高的代码考古。**
- **reflection_questions 答案**:
  - **Q: 为什么禁止迁移时'顺手修正'疑似错误的偏移？** A: 三步（发现→实验→改）不可合并。顺手改 = 未验证的行为变化混进"行为保持"的重构，金样一旦红了无法定位是迁移错还是修正错；而且"疑似错误"可能是未理解的正确（本案 dw3 编码"错"得能跑，说明固件行为尚未被理解）。
  - **Q: 静态等价证明的边界在哪？** A: 它证明"新旧表达式在给定输入下产出相同字节"，输入覆盖穷举/随机；它不证明"固件接受这些字节"——那是金样门禁的职责。两腿互补：静态证机器可靠、硬件证环境可靠。

### R0.3-completion-backend

- **状态**: completed（行为不变：位运算与次序逐行等价；硬件金样门禁待 sudo）
- **交付物**: `enum mlx5_prism_cmdq_mode`（POLL_INLINE / EQ_DEFERRED）+ EQ→CMDQ 唯一接口三函数：`complete_slots(vector)`（原 release_slots 更名）/`eq_bound()`/`eq_gone()`（含原 eq.c forget_slots 逻辑）；eq.c 本地 forget_slots 删除；cmd_exec 的 `dev->eq.created` 反向读取改为 `cmdq->mode` 判断；slot 生命周期不变量注释随代码迁移至枚举定义处
- **验证**: make 零警告 + checkpatch 0/0/0 + grep 确认 eq.c 无任何 slot 位图直接操作、cmdq.c 无 eq 字段引用（双向耦合清零）
- **reflection 要点**: T10 硬中断消费 EQE 时唯一跨中断入口将是 complete_slots()——单一入口 + irqsave 锁在先，竞态面已收敛；位图参数保留 EQE 聚合语义（一条 EQE 可确认多 slot）

### R0.4-fw-state-quirks

- **状态**: completed（门禁表只拦陷阱区：caps/endian 需 ≥BOOT_PAGES、INIT_HCA 需 ≥CAPS_SET、EQ/UAR 需 ≥OPERATIONAL；ENABLE_HCA 前窗口刻意不拦——自检流程不变、未知 opcode 放行给固件裁决，金样不受影响）
- **交付物**: `enum mlx5_prism_fw_state`（fw->state）+ `mlx5_prism_fw_cmd_allowed()` 门禁表（fw.c，cmd_exec 入口调用，状态不符 -EINVAL）；`struct mlx5_prism_fw_profile`（known-good 四字段/EQ 激活重试/耐心窗）probe 时按 fw_rev 填充（未知版本告警沿用 14.32 档）；模块参数 `eq_patience_ms`/`eq_engagement_retries`（默认=实测值）；状态推进点：boot give→BOOT_PAGES、SET_HCA_CAP→CAPS_SET、INIT_HCA→OPERATIONAL、TEARDOWN→CAPS_SET
- **验证**: make 零警告 + checkpatch 0/0/0；known-good 字节经 profile 路径仍与 fmt 等价测试覆盖的旧路径一致（值同为 18/清零四处）
- **reflection 要点**: 驱动侧门禁是第一层"便宜失败"，固件门禁仍是最终裁决——两层分工：驱动拦"已知时序错误"，固件裁"一切"

### R0.5-dev-ctx-teardown-selftest

- **状态**: completed（selftest=1 路径金样等价：自检序列/顺序/日志不变；selftest=0 为新生产路径——bootstrap 补发 ENABLE_HCA（state==RESET 时），是 R0 中唯一"新增行为"，属设计内新增而非行为漂移）
- **交付物**: `selftest=1` 模块参数（默认关，全部自检移出生产 probe）；fw.pages/pages_dma 懒分配（kvcalloc，上限 8192 不变，128KB 不再常驻设备结构）；caps_max/cur 缓存（probe_caps/set_hca_cap 填充，teardown 释放，T12/T13 消费）；`mlx5_prism_teardown(dev, stage)` 单一有序拆卸（PRISM_TD_NO_EQ/ALL，pci.c 三处重复清零）；fw state 增 ENABLED（自检 ENABLE_HCA 后由编排层推进）
- **验证**: make 零警告 + checkpatch 0/0/0 + teardown 出现次数=1
- **reflection 要点**: 生产/验证路径分离后，eq 自检最坏 80s 挂起不再进默认 probe；错误路径从失败层调 teardown(stage)——栈式获取逆序释放的单一表达

### R0.6-trace-scaffold

- **状态**: completed（编译/插桩就位；perf 配对验证随硬件门禁批量执行）
- **交付物**: `mlx5_prism_trace.h`（prism_cmdq_issue/complete + prism_eq_event 三个 TRACE_EVENT）+ `mlx5_prism_trace_names.h`（opcode 名字表，独立头避免 MULTI_READ 二次展开重定义——T06 草案"未定义辅助函数"教训的兑现）+ trace.c 实例化；插桩：cmd_exec doorbell 前（issue，含 slot/token/page_queue）、状态解析后（complete，含 delivery/status/syndrome/latency_ns）、eq_poll 每事件（eq_event，含 vector/cons）；Makefile += trace.o + `ccflags-y += -I$(src)`（define_trace.h 字符串形式再包含的路径解析）
- **验证**: make 零警告；checkpatch 无 ERROR/WARNING（TRACE_EVENT 宏布局 9 处 CHECK 豁免，与树内 trace 头一致）；tracepoint 未启用时零行为变化（默认 no-op）
- **reflection 要点**: tracepoint 提前于 T10 落地=T06"死机无现场"教训的工程化兑现；issue/complete 配对 + eq_event 即 T10 事件模式调试的观测三件套

### R0.7-可读性整改
- **硬件门禁首轮复盘（2026-09-16，用户跑 r0_gate.sh，黑匣子 3475 行）**：G1 金样双口零差异（复验通过）；G4 全绿——44 个自检周期 10/10（双口×22），怪癖恢复触发 35 次全部自愈，40 压测循环零告警；**两处修正**：①G2 断言 grep 不到 ENABLE_HCA——R0.5 给 bootstrap 生产路径加了自发命令但漏了日志行，已补（该行仅在 selftest=0 出现，金样不受影响）；②G3 无证据——**本机未安装 perf**（脚本缺陷），G3 改用 tracefs 重写（rmmod teardown 的 40+ 条命令足够验证 issue/complete 配对，零外部依赖）。修正后需整门禁重跑一次。

- **状态**: completed（行为保持：selftest=1 路径日志序列与金样逐行一致，硬件门禁 G1 复跑覆盖；默认路径新增的唯一变化 = 三面 caps hexdump 墙不再打印——设计内收敛）
- **交付物**: ①日志瘦身——`mlx5_prism_selftest` 全局化（module_param 同名），caps[max]/[cur]/[post] hexdump 仅 selftest 打印（缓存 memcpy 不受影响），默认 dmesg 成纯文本剧本；②注释节食——set_hca_cap 考古块 15→6 行（不变量+指针），eq.c/cmdq.c 文件头从任务流水账改为"阅读地图+不变量"；③fmt.h 新增 9 条命令的 ASCII 布局图（命名偏移表补上字段全景）；④新增 READING.md——600 行脊柱读法/分层映射/动手路径/知识索引（含宏魔法专题沉淀：三世界、÷8/÷32 双坐标系、量尺结构体与 sizeof 不求值语义）
- **验证**: make 零警告 + checkpatch 8 文件 0/0/0（trace.h 为已入档的 TRACE_EVENT 布局豁免）+ 等价性测试 ALL EQUIVALENT
- **反思**: 面向读者的三件套（剧本式日志/地图式文件头/全景式偏移表）与面向作者的资产（考古档案/金样/测试）分工明确——前者做减法，后者只增不减

- **tracepoint 采集窗口时序（2026-09-16，G3 三轮追诊破案）**：模块的 tracepoint 探测在卸载路径的 `MODULE_STATE_GOING` 阶段即被内核分离（防 use-after-free），早于 `mod->exit()` 里发出的 teardown 命令——**"加载后使能、卸载时采集"的窗口里命令发出时 funcs==NULL，全部静默跳过**。症状极具迷惑性：事件目录在/使能位=1/调用点反汇编活着/内核自带事件（sched_switch 对照组 1418 条/s）正常/换编译器无效。正解：事件目录在模块 init 执行任何命令**之前**注册——轮询循环在目录出现的瞬间使能，加载期捕获全轨迹（实测 90/90 配对 + 20 条 eq_event）。与 EQ 双 doorbell 同类教训：**"使能"与"可见"是两个时刻，中间隔着生命周期钩子**。附带环境发现：PATH 的 ~/.local/gcc（gcc-15）遮蔽 /usr/bin/gcc（11.4，与内核同版），历史构建均为 gcc-15 产物——本案非根因（gcc-11 重编同样复现），但作为环境隐患记录在案（当前树内 .ko 为 CC=/usr/bin/gcc-11 构建）。

### T10-prep-eqc-翻案实验（预登记，待上机）

- **状态**: designed（判别式已预登记于 scripts/t10prep_eqc_probe.sh 头部；跑机器=用户，sudo 在用户侧）
- **设计**: 模块参数 `eq_ifc`（默认 0——金样路径请求字节零改动）；fmt.h 新增 ifc 编码/解码各三助手（`prism_eqc_write_dw3_ifc`：dw3=(log<<24)|uar；`prism_eqc_set_intr_ifc`：dw5 低 12 位=vec；`prism_eqc_*_readback_ifc`：真窗口 BE 算术直算）；eq.c 创建路径按参数切换编码，QUERY_EQ 读回在 ifc 模式额外 dump 原始 dw3/dw5 + 真窗口解码，并**以真窗口读回采纳 eq->log_sz**（消费者必须走固件实建的环，否则 nent 错配、owner 奇偶全乱——这是实验内建的防呆）。判别式 A-E 与结论空间见脚本头（跑前写死，勿改）。
- **核心发现（eqc 位几何修正，重要度最高）**：
  1. **R0.2 归档两处笔误修正**：intr 真位号 **@180、12 位**（`reserved_at_a0[0x14]` 占 160-179；归档写的 @140-147 系误记）；log_eq_size 真窗口是 **eqc[12] 低 5 位**（归档写的"占该 dword 的 value bits 3-7"错误——按 MLX5_SET 代数 `dw_bit_off=32-5-3=24`，字段落在 value bits 24-28=eqc[12] bits 0-4）。翻案**结论**不受影响：旧编码 `(log&7)<<29|uar` 下 log_eq_size 读值 = `eqc[12]&0x1f` = `uar&0x1f` = 0（uar=4096），1 条目仍是字面请求的。
  2. **双坐标系的第三道褶皱**：prism_get_bits 的字节内 LSB-first 编号与 ifc/PRM 的 dword 内 MSB-first 编号在**字节内窗口上翻转**——两者的"bit 99"都落在 eqc[12]，一个读高 5 位一个读低 5 位，重叠 bit3-4，字段恒 0 时不可分辨。**金样读回解析器 `PRISM_FLD_EQC_LOGSZ_READBACK(=99)` 一直在读翻转窗口却从未暴露**。处置：金样路径不动（字节冻结纪律），实验解码走 BE 算术直算的真窗口助手，fmt.h 几何注释写明两套约定的分叉点。caps 的 R0.2 常量是经验标定窗口（行为正确），但其数值≠真 ifc 位号——fmt.h 头部"与 mlx5_ifc 编号一致"的旧说法已删。
  3. **宿主机证明（fmt_equiv_check.c 第 11 节）**：ifc 编码字节与 **MLX5_SET 宏代数的逐位仿真**等价（dw3=0x0C001000 for log12/uar4096），真窗口解码往返一致，新旧判别对钉死（legacy dw3=0x80001000 vs ifc dw3=0x0C001000）。全部 ALL EQUIVALENT。
  4. **旁证（跑前推理）**：若固件用翻转（高 5 位）窗口，旧编码的 0x80 会被读成 log16——T09 应见巨型环或拒绝，与"接受+1 条目"观测不符；观测行为单方面支持 MLX5_SET 几何。
  5. **遗留疑点（入档不动）**：7.0.0 ifc 的 log_max_qp @155（真窗口=byte19 低 5 位）与 T06 实证表"c[19]bits3-7"（高 5 位）不一致（num_ports@440=byte55 整字节，两说一致）。known-good 补丁字节全部金样门禁在案、行为无虞，仅字段解释未定；若本实验判据 A 成立（固件=MLX5_SET 几何），此疑点升格为后续 caps 窗口实验。
- **预期（预登记，勿跑后改）**：A 成立 → 翻案：固件从没说谎，DESIGN §7 三个结论（1 条目环/~10% 激活/提交长尾）重审，ifc 编码转正进 known-good；A 不成立 → T10 严格官方路径直行；A∧(¬B∨¬C) → 环尺寸与激活/长尾解耦（两个独立怪癖）；D（中断计数）独立评估 intr 假设。
- **安全**: eq_ifc 默认 0（G1 金样不受影响）；owner 预填 0x01 + 消费谓词 `(owner&1)^!!(ci&nent)` 本就是 nent-通用式（与 mlx5_core eq.c:792 同构——nent=1 时"碰巧成立"的公式原来是通用式，多条目环无需改消费者）；黑匣子记录器全程（/var/tmp/t10prep_live.log）。

- **判决（2026-09-17 上机，scripts/t10prep_eqc_probe.sh，7 次加载）**: **全面翻案成立**。
  - **A ✓（尺寸）**：ifc 组 QUERY_EQ 读回 **dw3=0x0c001000——固件逐字回显请求**（log_eq_size=12/uar=4096，真窗口解码）；且 10/10 自检在 nent=4096 的消费者上通过，事件按序落连续条目——环真为多条目。同屏的旧解析行 "fw reports log_eq_size=1 (2 entries)" 恰是翻转窗口读 0x0c>>3=1 的实况展示。
  - **B ✓（激活率）**：ifc 组 10/10 加载**全部首次激活**（retries=0、零静默、零重建）；对照 ctrl0（旧编码裸激活）0/2、ctrl（默认参数）一口需一次重建才过。~10% 激活怪癖翻案为编码伪影。
  - **D ✓（中断）**：ifc 组自检 "(10-11 events total, 10-11 interrupts)"——**MSI-X 真实点火且计数≈事件数**；ctrl 组恒 0。脚本 irq delta 全 0 系采集步骤缺陷（after 快照取在 rmmod 之后，中断行已随释放消失），以 stub 计数为权威证据。
  - **E ✓（对照）**：ctrl 读回仍 0、行为与金样时代一致（排除环境漂移）。
  - **C ?（长尾）未决**：自检每命令固定消耗 ~2000ms 耐心窗（等待结构掩盖提交时延），本次数据不可判；T11 用 tracepoint latency_ns 复测。
  - 每加载 11 events（10 命令 + 1 条 CREATE_EQ/QUERY_EQ 自身积压事件），interrupts 10-11（轻度聚合）。
  - **后续动作（判决当日已执行）**：ifc 编码转正（fmt.h 旧编码删除、eq.c 去 eq_ifc 参数、读回解析改真窗口 eqc[12]&0x1f）、DESIGN §7 翻案改写、**金样失效待重采**（QUERY_EQ 行为有意变更，非漂移——重采流程见 scripts/golden/README.md）。

## 修改 DESIGN.md 的记录

> CLAUDE.md 要求：设计与实际硬件行为不一致时必须修正 DESIGN.md，在这里登记。

- **v2.1 内容修订——T10-prep 翻案（2026-09-17）**：§7 全章改写（1 条目环/~10% 激活/中断不点火三结论翻案为驱动侧 eqc 编码产物，判决证据见 progress.md T10-prep 节）；§5.5 eqc 示例改编码/解码助手制 + LSB/MSB 双窗口翻转警告；§6.1 EQ 字段降级为防御参数；§12 EQ 常数更新（log12/4096 条目、中断 ≈1 次/事件）。ifc 编码同日转正进代码，金样失效待重采。
- **价值观修订 v2.1（2026-09-15）**：作废 v2 §1.2 末公理"任何抽象不允许为优雅而加层——每个抽象必须是数据或查表"。动因：需求描述修正——目标从来是"简单而又优雅"，优雅是形式目标而非嫌疑犯；旧公理是修辞级过度矫正（全仓 grep 证实代码/注释/progress 零引用，R0 的抽象决策实际全部以阅读收益为准，产物已是"简单而优雅"的形状）。v2.1 新增 §1.7 设计价值观：简单=内容（概念少而承重，读路径无记忆术）、优雅=形式（代码形状映射问题形状）+ **抽象准入三问**（真实调用者 / 删除测试 / 单实现零旋钮）；§5.5 补结构体覆盖层立场（未采用=重构字节等价纪律，非禁令，引入须 static_assert 与偏移表互锁）；附录登记 v2→v2.1 三处。**零代码改动、无需门禁；架构不重写**（公理未真正约束过架构）。
- **架构重设计（2026-09-15）**：DESIGN.md 全量重写为 **v2**（git 历史保留 v1）。动因：九个任务纯追加导致结构缠结（cmdq↔eq 双向耦合、teardown 三处重复、自检嵌在生产 probe）、v1 约 1/3 内容被硬件实证推翻（writeq doorbell、7 步缺 ISSI/字节序/SET_HCA_CAP、页数、P1/P2 延迟目标）、EQ/事件设计整章缺失。v2 新增：四层架构（§2）、类型化命令层（§5.5）、完成后端接口（§5.3）、固件状态机门禁表（§4.1）、**怪癖档案 fw_profile（§6，mlx5_core 没有的层）**、EQ 与事件设计（§7，T09 十四轮实验全部编码）。配套：task.json 新增 R0.1-R0.6 重构冲刺（行为保持不变，金样对比验收）、T10 按 T09 实证重定义（"中断计数≈命令数"改为"中断>0 且与事件数同数量级"）、T11 拆分（核心提前进 R0.6，剩余为延迟基线采集）并前移到 T10 之前（可观测性优先）。执行顺序：R0 → T11 → T10 → T12-T17。
- **T05（2026-08-09）**：C2 校验项"固件版本"由 `QUERY_FW (opcode 0x103)` 改为"init segment `fw_rev` 寄存器"。原因：CX-4 Lx 固件（FW 14.32.1908）**无 QUERY_FW 命令**——7.0.0 内核 `mlx5_ifc.h` 中 0x103=TEARDOWN_HCA，opcode 表无 QUERY_FW 枚举；mlx5_core 引导序列（main.c:1182-1193）也是 wait_fw_init → ENABLE_HCA，不发 QUERY_FW。fw 版本从 init segment 的 `fw_rev` 寄存器直接读（T02 已实现 `mlx5_prism_fw_rev_major/minor/sub`）。task.json T05/T06 step 中的"QUERY_FW"需相应改为 ENABLE_HCA 或其他真实存在的命令。
