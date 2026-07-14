// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: CMDQ memory allocation.
 *
 * T03: allocate one 4 KiB command-queue page via dma_alloc_coherent.
 * The firmware reads command descriptors from this page; T05 fills them
 * in and rings the doorbell. Here we only hand the bus address to the
 * init segment later (T07) and verify the DMA path is sane.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include "mlx5_prism.h"

int mlx5_prism_cmdq_alloc(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;

	cmdq->size = MLX5_PRISM_PAGE_SIZE;
	cmdq->buf = dma_alloc_coherent(&dev->pdev->dev, cmdq->size,
				       &cmdq->dma, GFP_KERNEL);
	if (!cmdq->buf)
		return -ENOMEM;

	spin_lock_init(&cmdq->lock);

	pci_info(dev->pdev,
		 "cmdq: cpu=%pK dma=%pad size=%zu\n",
		 cmdq->buf, &cmdq->dma, cmdq->size);

	return 0;
}

void mlx5_prism_cmdq_free(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;

	if (!cmdq->buf)
		return;

	dma_free_coherent(&dev->pdev->dev, cmdq->size, cmdq->buf, cmdq->dma);
	cmdq->buf = NULL;
	cmdq->dma = 0;
	cmdq->size = 0;
}
