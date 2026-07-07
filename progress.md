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
| T01-hello-module | 内核模块骨架 + Makefile | S0 | — | **completed** | (本 commit) | 2026-07-07 |
| T02-pci-probe | PCI probe + BAR0 + 设备结构体 | S1 | M0 | pending | — | — |
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

---

## 详细记录

### T01-hello-module

- **状态**: completed
- **开始时间**: 2026-07-07
- **完成时间**: 2026-07-07
- **Commit**: (本 commit)
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

### T02-pci-probe

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
