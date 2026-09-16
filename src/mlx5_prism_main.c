// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: ConnectX-4 Lx control-plane kernel driver.
 *
 * Module entry: registers the PCI driver. The actual probe/remove logic
 * lives in mlx5_prism_pci.c; firmware bootstrap, CMDQ engine, PHY
 * diagnostics and the netdev skeleton attach later.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "mlx5_prism.h"

static struct pci_driver mlx5_prism_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= mlx5_prism_pci_ids,
	.probe		= mlx5_prism_pci_probe,
	.remove		= mlx5_prism_pci_remove,
};

static int __init mlx5_prism_init(void)
{
	int err;

	err = pci_register_driver(&mlx5_prism_driver);
	if (err)
		return err;

	pr_info("loaded (v%s)\n", MLX5_PRISM_DRV_VERSION);
	return 0;
}

static void __exit mlx5_prism_exit(void)
{
	pci_unregister_driver(&mlx5_prism_driver);
	pr_info("unloaded\n");
}

module_init(mlx5_prism_init);
module_exit(mlx5_prism_exit);

MODULE_VERSION(MLX5_PRISM_DRV_VERSION);
MODULE_AUTHOR("mlx5_prism project");
MODULE_DESCRIPTION("ConnectX-4 Lx control-plane driver (learning project)");
MODULE_LICENSE("GPL");
