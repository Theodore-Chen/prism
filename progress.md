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
| T02-pci-probe | PCI probe + BAR0 + 设备结构体 | S1 | **M0** | **completed** | (本 commit) | 2026-07-07 |
| T03-dma-eq-alloc | DMA mailbox + EQ 分配 | S2 | — | pending | — | — |
| T04-msix-irq | MSI-X 中断 + request_irq | S2 | — | pending | — | — |
| T05-cmdq-submit | CMDQ 命令提交（doorbell） | S3 | — | pending | — | — |
| T06-cmdq-complete | CMDQ 响应处理（completion） | S3 | — | pending | — | — |
| T07-fw-poll-enable | 固件引导 1：poll + ENABLE_HCA | S4 | — | pending | — | — |
| T08-fw-pages | 固件引导 2：QUERY/MANAGE_PAGES | S4 | — | pending | — | — |
| T09-fw-caps | 固件引导 3：QUERY_HCA_CAP 对比 | S4 | — | pending | — | — |
| T10-fw-init-hca | 固件引导 4：INIT_HCA → OPERATIONAL | S4 | **M1** | pending | — | — |
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
- **Commit**: (本 commit)
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

### T04-msix-irq

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

### T05-cmdq-submit

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

### T06-cmdq-complete

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

- _（待记录）_

## 修改 DESIGN.md 的记录

> CLAUDE.md 要求：设计与实际硬件行为不一致时必须修正 DESIGN.md，在这里登记。

- _（待记录）_
