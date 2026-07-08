// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: ConnectX-4 Lx control-plane driver — PCI probe/remove.
 *
 * T02: bind 15b3:1015, map BAR0 via devres, read init segment to verify
 * the MMIO path is alive. No CMDQ, no firmware bootstrap yet.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "mlx5_prism.h"

const struct pci_device_id mlx5_prism_pci_ids[] = {
	{ PCI_VDEVICE(MELLANOX, MLX5_PRISM_DEVICE_ID_CX4_LX) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, mlx5_prism_pci_ids);

int mlx5_prism_pci_probe(struct pci_dev *pdev,
			 const struct pci_device_id *id)
{
	struct mlx5_prism_dev *dev;
	u16 rev_major, rev_minor, rev_sub, cmdif_rev;
	int err;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->pdev = pdev;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "pcim_enable_device failed: %d\n", err);
		return err;
	}

	pci_set_master(pdev);

	err = pcim_iomap_regions(pdev, BIT(MLX5_PRISM_BAR0), KBUILD_MODNAME);
	if (err) {
		dev_err(&pdev->dev,
			"pcim_iomap_regions(BAR0) failed: %d\n", err);
		return err;
	}

	dev->bar0 = pcim_iomap_table(pdev)[MLX5_PRISM_BAR0];
	dev->iseg = (const struct mlx5_prism_iseg __iomem *)dev->bar0;
	pci_set_drvdata(pdev, dev);

	rev_major = mlx5_prism_fw_rev_major(dev);
	rev_minor = mlx5_prism_fw_rev_minor(dev);
	rev_sub = mlx5_prism_fw_rev_sub(dev);
	cmdif_rev = mlx5_prism_cmdif_rev(dev);
	if (!rev_major && !rev_minor && !rev_sub) {
		dev_err(&pdev->dev,
			"BAR0 reads all-zero — init segment unreadable\n");
		return -EIO;
	}

	pci_info(pdev, "BAR0 mapped at %pR, fw_rev %u.%u.%04u, cmdif_rev %u\n",
		 &pdev->resource[MLX5_PRISM_BAR0],
		 rev_major, rev_minor, rev_sub, cmdif_rev);

	return 0;
}

void mlx5_prism_pci_remove(struct pci_dev *pdev)
{
	struct mlx5_prism_dev *dev = pci_get_drvdata(pdev);

	if (!dev) {
		pci_warn(pdev, "remove with no drvdata\n");
		return;
	}

	pci_info(pdev, "removing (fw_rev %u.%u.%04u)\n",
		 mlx5_prism_fw_rev_major(dev),
		 mlx5_prism_fw_rev_minor(dev),
		 mlx5_prism_fw_rev_sub(dev));

	pci_clear_master(pdev);
	pci_set_drvdata(pdev, NULL);
}
