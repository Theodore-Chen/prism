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
| T06b-cmdq-mailbox-chain | 多块邮箱链（576B 块 + 1KB 对齐 + token） | S3 | — | pending | — | — |
| T07-fw-bootstrap-1 | 固件引导 I：ISSI + 字节序 + boot pages + 回收 | S4 | — | pending | — | — |
| T08-fw-bootstrap-2 | 固件引导 II：SET_HCA_CAP + init pages + INIT_HCA | S4 | **M1** | pending | — | — |
| T09-eq-create-poll-consume | EQ 创建 + 轮询消费 EQE（零中断点亮） | S5 | — | pending | — | — |
| T10-eq-irq-enable | MSI-X 点亮 + 硬中断消费 EQE（分级放量） | S5 | — | pending | — | — |
| T11-tracepoints | ftrace tracepoint 插桩 | S6 | — | pending | — | — |
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

  **③ syndrome/status 速查**：status 0x50=bad input length、0x51=bad output length；syndrome 是 **out bytes[4:8] 的 32 位 BE**（截断打印会少 8 位）：0x4D99EF=query_issi bad outlen、0x4E2106=manage_pages 读写主机内存失败、0x9074AA=create_eq bad input len（社区 syndrome 表：gist.github.com/lukego/8b4e567f4d5c4b60da6545e063888391）

  **④ CREATE_EQ 未解之谜**：4KB/log6/1pas/inlen280 被拒 0x9074AA；inlen 梯度 288/336/528 全同拒（pas 数不变）；64KB/log12/64pas 尝试后死机（是否被接受未知）。mlx5_core 在本机从不发单页 EQ（其 EQ 均 64 页起）——最小 pas 覆盖要求待 T09 阶梯测定。

  **⑤ 两次 panic 复盘与风控工具箱**：两次均死在 insmod（probe 执行中）、CREATE_EQ 门后代码（arm/enable_irq/事件模式）从未安全执行过。教训固化：风险分级上机（eq_irq 参数：先"不点火"跑通 CREATE_EQ+arm+teardown，再单独点亮中断）；硬中断 printk 必须 dev_warn_ratelimited；风暴护栏（连续空中断超限 → disable_irq_nosync）；黑匣子记录器必须落 **/var/tmp**（磁盘backed，逐行 sync）——/tmp 是 tmpfs，断电即失（第一次现场就这么丢的）。mlx5_core debugfs 命令统计（/sys/kernel/debug/mlx5/<bdf>/commands/）是行为对照的免费基准。


## 修改 DESIGN.md 的记录

> CLAUDE.md 要求：设计与实际硬件行为不一致时必须修正 DESIGN.md，在这里登记。

- **T05（2026-08-09）**：C2 校验项"固件版本"由 `QUERY_FW (opcode 0x103)` 改为"init segment `fw_rev` 寄存器"。原因：CX-4 Lx 固件（FW 14.32.1908）**无 QUERY_FW 命令**——7.0.0 内核 `mlx5_ifc.h` 中 0x103=TEARDOWN_HCA，opcode 表无 QUERY_FW 枚举；mlx5_core 引导序列（main.c:1182-1193）也是 wait_fw_init → ENABLE_HCA，不发 QUERY_FW。fw 版本从 init segment 的 `fw_rev` 寄存器直接读（T02 已实现 `mlx5_prism_fw_rev_major/minor/sub`）。task.json T05/T06 step 中的"QUERY_FW"需相应改为 ENABLE_HCA 或其他真实存在的命令。
