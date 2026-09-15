// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: ConnectX-4 Lx control-plane driver — PCI probe/remove.
 *
 * T02: bind 15b3:1015, map BAR0 via devres, read init segment to verify
 * the MMIO path is alive.
 * T03: set DMA mask, allocate CMDQ mailbox and EQ ring buffers.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/dma-mapping.h>
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

static int mlx5_prism_set_dma_caps(struct pci_dev *pdev)
{
	int err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		pci_warn(pdev, "64-bit DMA mask failed, trying 32-bit\n");
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			pci_err(pdev, "failed to set DMA mask: %d\n", err);
			return err;
		}
	}

	dma_set_max_seg_size(&pdev->dev, UINT_MAX);
	return 0;
}

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

	err = mlx5_prism_set_dma_caps(pdev);
	if (err)
		return err;

	pci_set_master(pdev);

	err = pcim_iomap_regions(pdev, BIT(MLX5_PRISM_BAR0), KBUILD_MODNAME);
	if (err) {
		dev_err(&pdev->dev,
			"pcim_iomap_regions(BAR0) failed: %d\n", err);
		return err;
	}

	dev->bar0 = pcim_iomap_table(pdev)[MLX5_PRISM_BAR0];
	dev->iseg = (struct mlx5_prism_iseg __iomem *)dev->bar0;
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

	err = mlx5_prism_cmdq_alloc(dev);
	if (err)
		return err;

	err = mlx5_prism_eq_alloc(dev);
	if (err)
		goto err_free_cmdq;

	err = mlx5_prism_irq_alloc(dev);
	if (err)
		goto err_free_eq;

	err = mlx5_prism_cmdq_init(dev);
	if (err)
		goto err_free_irq;

	/*
	 * T06a: prove the blocking poll round trip (ENABLE_HCA, QUERY_PAGES,
	 * bad opcode) before T07 starts mutating firmware state.
	 */
	err = mlx5_prism_cmdq_selftest(dev);
	if (err)
		goto err_free_irq;

	/* T06b: mailbox chain — QUERY_ISSI (1-block out) + SET_ISSI. */
	err = mlx5_prism_cmdq_mailbox_selftest(dev);
	if (err)
		goto err_free_irq;

	/* T07/T08: full bootstrap — ISSI, pages, caps, INIT_HCA (M1). */
	err = mlx5_prism_fw_bootstrap(dev);
	if (err) {
		/* Reclaim any pages already given (mlx5_core's failed-load path). */
		mlx5_prism_fw_teardown(dev);
		goto err_free_irq;
	}

	/*
	 * T09: EQ with interrupts masked — CREATE_EQ ladder, EQEs consumed
	 * by polling; /proc/interrupts must stay at 0.
	 */
	err = mlx5_prism_eq_setup(dev);
	if (err) {
		mlx5_prism_eq_teardown(dev);
		mlx5_prism_fw_teardown(dev);
		goto err_free_irq;
	}

	err = mlx5_prism_eq_selftest(dev);
	if (err)
		goto err_eq_teardown;

	return 0;

err_eq_teardown:
	mlx5_prism_eq_teardown(dev);
	mlx5_prism_fw_teardown(dev);

err_free_irq:
	mlx5_prism_irq_free(dev);
err_free_eq:
	mlx5_prism_eq_free(dev);
err_free_cmdq:
	mlx5_prism_cmdq_free(dev);
	return err;
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

	/* Teardown talks over the CMDQ — must run before the queues go away. */
	mlx5_prism_eq_teardown(dev);
	mlx5_prism_fw_teardown(dev);
	mlx5_prism_irq_free(dev);
	mlx5_prism_eq_free(dev);
	mlx5_prism_cmdq_free(dev);
	pci_clear_master(pdev);
	pci_set_drvdata(pdev, NULL);
}
