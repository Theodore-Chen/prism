// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: ConnectX-4 Lx control-plane kernel driver.
 *
 * T01: skeleton module. Loads/unloads cleanly and emits a dmesg marker.
 * Subsequent tasks attach PCI probe, CMDQ engine, firmware bootstrap,
 * PHY diagnostics and a minimal netdev skeleton here.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>

#define MLX5_PRISM_DRV_VERSION	"0.1"

static int __init mlx5_prism_init(void)
{
	pr_info("loaded (v%s)\n", MLX5_PRISM_DRV_VERSION);
	return 0;
}

static void __exit mlx5_prism_exit(void)
{
	pr_info("unloaded\n");
}

module_init(mlx5_prism_init);
module_exit(mlx5_prism_exit);

MODULE_VERSION(MLX5_PRISM_DRV_VERSION);
MODULE_AUTHOR("mlx5_prism project");
MODULE_DESCRIPTION("ConnectX-4 Lx control-plane driver (learning project)");
MODULE_LICENSE("GPL");
