// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: Event Queue memory allocation.
 *
 * T03: allocate one 4 KiB EQ ring buffer (64 EQEs of 64 B each) via
 * dma_alloc_coherent. The firmware writes Event Queue Entries here and
 * raises an MSI-X interrupt (T04 wires the IRQ; T06 consumes EQEs).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include "mlx5_prism.h"

int mlx5_prism_eq_alloc(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;

	eq->size = MLX5_PRISM_PAGE_SIZE;
	eq->buf = dma_alloc_coherent(&dev->pdev->dev, eq->size,
				     &eq->dma, GFP_KERNEL);
	if (!eq->buf)
		return -ENOMEM;

	eq->cons_index = 0;

	pci_info(dev->pdev,
		 "eq: cpu=%pK dma=%pad size=%zu entries=%d\n",
		 eq->buf, &eq->dma, eq->size, MLX5_PRISM_EQ_ENTRIES);

	return 0;
}

void mlx5_prism_eq_free(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;

	if (!eq->buf)
		return;

	dma_free_coherent(&dev->pdev->dev, eq->size, eq->buf, eq->dma);
	eq->buf = NULL;
	eq->dma = 0;
	eq->size = 0;
	eq->cons_index = 0;
}
