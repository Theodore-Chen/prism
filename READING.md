# READING.md — mlx5_prism 阅读指南

> 给第一次打开这个仓库的人（包括未来的你和面试官）。
> 目标：沿 ~600 行"协议脊柱"读完就能复述主机如何命令一块 ConnectX-4 Lx。

## 一、按什么顺序读（协议脊柱，~600 行核心）

| 顺序 | 文件 | 读什么 | 你会学到的关键不变量 |
|------|------|--------|---------------------|
| 1 | `mlx5_prism_pci.c` (~190) | `mlx5_prism_pci_probe`：PCI 探测 → BAR0/iseg → 启动编排；`mlx5_prism_teardown`：唯一的有序拆卸 | devres 免 goto 清理链；资源栈式获取、逆序释放 |
| 2 | `mlx5_prism_cmdq.c` (~570) | `mlx5_prism_cmd_exec`：**全项目的心脏**——slot 分配 → 填 64B 描述符 → `wmb()` → `iowrite32be(1<<idx, cmd_dbell)` → 轮询 owner 位 → 两层状态解析 | wmb 非 smp_wmb（消费者是经 PCIe DMA 的固件，不是 CPU）；超时故意泄露 slot+邮箱（防迟到完成写坏复用内存）；最高槽保留给页命令 |
| 3 | `mlx5_prism_fw.c` (~560) | `mlx5_prism_fw_bootstrap`：9 步引导（ISSI → boot 页 → 字节序 → caps → SET_HCA_CAP → init 页 → INIT_HCA）；`fw_teardown` 双形态 | 每命令有门禁状态（fw_state 门禁表）；known-good caps 是怪癖档案数据不是字段语义 |
| 4 | `mlx5_prism_eq.c` (~460) | `eq_poll`：owner 奇偶判定 → `dma_rmb` → vector → `complete_slots`；双 doorbell（+0x48 commit / +0x40 arm） | 数据先行、owner 后提交（长尾）；~10% 激活率 → 销毁重建重掷骰子 |

辅助：`mlx5_prism.h`（全部结构体与线格式定义）、`mlx5_prism_fmt.h`（**唯一**偏移表 + 每命令 ASCII 布局图）、`mlx5_prism_trace.h`（tracepoint）。

## 二、文件到层的映射（DESIGN.md v2 §2）

```
L0 硬件访问   mlx5_prism_pci.c (BAR0/iseg)
L1 传输       mlx5_prism_cmdq.c  mlx5_prism_eq.c  mlx5_prism_irq.c  mlx5_prism_fmt.h
L2 固件       mlx5_prism_fw.c (状态机/caps/怪癖档案 fw_profile)
L3 服务       (T12+ phy / T14 smoke / T15 netdev，未建)
横切          mlx5_prism_trace.{h,c}
```

依赖严格向下；eq 只能经 `complete_slots/eq_bound/eq_gone` 触碰命令槽；裸偏移只允许出现在 fmt.h。

## 三、动手路径

```bash
# 构建（无需 root）
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# 上机（需 root；先卸载官方驱动）
sudo modprobe -r mlx5_ib mlx5_core
sudo insmod mlx5_prism.ko selftest=1     # 自检 + 完整日志（含 caps hexdump）
dmesg | grep mlx5_prism                  # 一步一行的引导剧本
sudo rmmod mlx5_prism

# 行为保持门禁（重构/修改后必跑）
sudo bash scripts/t09_verify.sh 1 && sudo bash scripts/golden/diff_golden.sh

# 观测（R0.6 起内置）
sudo perf list | grep mlx5_prism
sudo perf record -a -e mlx5_prism:prism_cmdq_issue -e mlx5_prism:prism_cmdq_complete \
     -e mlx5_prism:prism_eq_event -- insmod mlx5_prism.ko selftest=1

# R0 硬件门禁一键跑（G1 金样/G2 生产路径/G3 tracepoint/G4 40 循环压测）
sudo bash scripts/r0_gate.sh
```

## 四、知识索引（深挖入口）

- **DESIGN.md v2**：四层架构 §2、命令引擎 §5（含错误分类学 §5.4）、EQ 与事件设计 §7（T09 实验全记录）、怪癖档案 §6
- **progress.md**：每任务的学习记录 + "历史问题归档"（字节偏移实证表、known-good 考古、syndrome 速查、两次死机复盘）
- **eqc dw3 翻案假设**（progress.md R0.2 节）：现存"1 条目 EQ"很可能是驱动编码请求的——T10 前置实验清单
- **mlx5_core 对照读法**：`/home/zqc/code/linux/drivers/net/ethernet/mellanox/mlx5/core/`（cmd.c 命令引擎 / eq.c 事件 / main.c 引导序列）
- **宏魔法专题**（为什么 MLX5_SET 长那样）：三世界（u8 量尺声明 / u32 移位掩码算术 / be32 落盘）、双坐标系（bit→字节 ÷8、bit→dword ÷32）、量尺结构体与 offsetof/sizeof 的"不求值"语义（C11 §6.5.3.4）

## 五、一句话定位

prism ≈ mlx5_core 底部三层的最小可读重写 + 一个 mlx5_core 没有的层（怪癖档案：把固件的实测行为当作数据管理），总量控制在 ~4500 行内。
