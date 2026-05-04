# CLAUDE.md — mlx5_prism 项目指引

## 项目

实现 **mlx5_prism.ko**——ConnectX-4 Lx 智能网卡的控制面内核驱动。从零完成 PCIe 探测、CMDQ 引擎、7 步固件引导、PHY 诊断和硬件冒烟测试。

完整技术设计见 **DESIGN.md**。先读 DESIGN.md，然后拆任务、写代码、逐个验证。

## 硬件环境

- **网卡**: Mellanox ConnectX-4 Lx, `15b3:1015`, PCI `08:00.0`, 双口 25GbE
- **BAR0**: 32MB, UAR doorbell 寄存器
- **内核源码**: `/home/zqc/code/linux` (自编译 7.0.0) 或 `/lib/modules/$(uname -r)/build` (当前运行内核 headers)
- **运行内核**: `5.15.0-174-generic`，headers 已安装

## 构建方式

```bash
# 方式一：使用当前运行内核的 headers（推荐，最快上手）
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# 方式二：使用自编译内核源码树（切换 7.0.0 内核后使用）
make -C /home/zqc/code/linux M=$(pwd) modules
```

自编译内核如未执行 `make modules_prepare`，需先在内核源码目录执行一次。

## 关键约束

1. **不注册 netdev**——不实现 `ndo_start_xmit` / `napi_gro_receive` / ethtool 回调
2. **不依赖 mlx5_core**——不 `#include` mlx5_core 的任何头文件，独立实现 CMDQ
3. **不导出用户态接口**——不创建 `/dev/` 设备，不使用 `mmap`
4. **与 mlx5_core 互斥**——加载前必须先 `rmmod mlx5_ib && rmmod mlx5_core`
5. **sysfs 挂在 `/sys/class/misc/`**——不注册 `struct net_device`
6. **代码风格**：Linux 内核 coding style (`scripts/checkpatch.pl --no-tree --strict`)

## 验证命令

```bash
# 卸载 mlx5_core
sudo modprobe -r mlx5_ib mlx5_core

# 编译
make -C /home/zqc/code/linux M=$(pwd) clean
make -C /home/zqc/code/linux M=$(pwd) modules

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
3. 每完成一个任务，编译验证、加载测试、git commit
4. 遇到设计文档与实际硬件行为不一致时，修正 DESIGN.md（commit 时注明原因）
