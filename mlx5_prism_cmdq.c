// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: CMDQ engine.
 *
 * T03: allocate the 4 KiB command-descriptor ring via dma_alloc_coherent.
 * T05: publish the ring DMA address to the init segment, build command
 *	descriptors and ring the doorbell. A boot self-test fires ENABLE_HCA
 *	and polls the ownership bit to confirm the firmware processed the
 *	doorbell. CMDQ boot completions are poll-driven (matching mlx5_core's
 *	CMD_MODE_POLLING); the EQ/interrupt completion path arrives in T06
 *	once a real Event Queue exists.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

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
		 "cmdq ring: cpu=%pK dma=%pad size=%zu\n",
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

static u8 mlx5_prism_xor8(const void *buf, size_t len)
{
	const u8 *p = buf;
	u8 sum = 0;
	size_t i;

	for (i = 0; i < len; i++)
		sum ^= p[i];

	return sum;
}

int mlx5_prism_cmdq_init(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;
	struct pci_dev *pdev = dev->pdev;
	u32 geom, addr_l;

	/*
	 * The low byte of cmdq_addr_l_sz advertises the queue geometry until
	 * we overwrite the register with our DMA address: bits[7:4] = log_sz,
	 * bits[3:0] = log_stride.
	 */
	geom = ioread32be(&dev->iseg->cmdq_addr_l_sz) & 0xff;
	cmdq->log_sz = (geom >> 4) & 0xf;
	cmdq->log_stride = geom & 0xf;
	cmdq->max_cmds = 1 << cmdq->log_sz;

	if (!cmdq->log_sz || !cmdq->log_stride) {
		pci_err(pdev, "invalid cmdq geometry: log_sz=%u log_stride=%u\n",
			cmdq->log_sz, cmdq->log_stride);
		return -ENODEV;
	}
	if (cmdq->max_cmds > MLX5_PRISM_MAX_COMMANDS) {
		pci_err(pdev, "firmware reports %d cmd slots (max %d)\n",
			cmdq->max_cmds, MLX5_PRISM_MAX_COMMANDS);
		return -ENODEV;
	}

	addr_l = (u32)cmdq->dma;
	if (addr_l & 0xfff) {
		pci_err(pdev, "cmdq dma %pad not 4 KiB aligned\n", &cmdq->dma);
		return -ENOMEM;
	}

	/* Hand the descriptor ring address to the firmware. */
	iowrite32be((u32)(cmdq->dma >> 32), &dev->iseg->cmdq_addr_h);
	iowrite32be(addr_l, &dev->iseg->cmdq_addr_l_sz);
	/* Let the firmware observe the full address before any doorbell. */
	wmb();

	pci_info(pdev,
		 "cmdq ready: log_sz=%u (%d slots) log_stride=%u (%d bytes/slot)\n",
		 cmdq->log_sz, cmdq->max_cmds, cmdq->log_stride,
		 1 << cmdq->log_stride);

	return 0;
}

void mlx5_prism_cmdq_submit(struct mlx5_prism_dev *dev, int idx,
			    u16 opcode, u16 op_mod,
			    dma_addr_t in_ptr, u32 in_len,
			    dma_addr_t out_ptr, u32 out_len)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;
	struct mlx5_prism_cmd_layout *lay;
	unsigned long flags;

	lay = cmdq->buf + (idx << cmdq->log_stride);

	spin_lock_irqsave(&cmdq->lock, flags);

	memset(lay, 0, sizeof(*lay));
	lay->type = MLX5_PRISM_PCI_CMD_XPORT;
	lay->inlen = cpu_to_be32(in_len);
	lay->in_ptr = cpu_to_be64(in_ptr);
	/* Inline 16-byte command header: opcode at bytes[0:1], op_mod at[6:7]. */
	lay->in[0] = cpu_to_be32((u32)opcode << 16);
	lay->in[1] = cpu_to_be32((u32)op_mod);
	lay->out_ptr = cpu_to_be64(out_ptr);
	lay->outlen = cpu_to_be32(out_len);
	lay->status_own = MLX5_PRISM_CMD_OWNER_HW;
	lay->sig = ~mlx5_prism_xor8(lay, sizeof(*lay));

	/*
	 * Order the descriptor (and any mailbox writes) before the doorbell MMIO
	 * write. wmb() — not smp_wmb() — because the consumer is the firmware
	 * reading via PCIe DMA, not another CPU.
	 */
	wmb();
	iowrite32be(1 << idx, &dev->iseg->cmd_dbell);

	spin_unlock_irqrestore(&cmdq->lock, flags);
}

int mlx5_prism_cmdq_selftest(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;
	struct mlx5_prism_cmd_layout *lay;
	unsigned long deadline;
	u8 own, status;
	const int idx = 0;

	lay = cmdq->buf + (idx << cmdq->log_stride);

	if (ioread32be(&dev->iseg->initializing) >> 31) {
		pci_warn(dev->pdev, "cmdq self-test: firmware still initializing\n");
		return -EAGAIN;
	}

	/*
	 * ENABLE_HCA is the canonical first command the firmware accepts (the
	 * fw revision lives in the init segment, so there is no QUERY_FW on
	 * ConnectX-4 Lx). Its input/output are both a 16-byte header, so no
	 * mailbox is needed.
	 */
	pci_info(dev->pdev, "cmdq self-test: submitting ENABLE_HCA\n");
	mlx5_prism_cmdq_submit(dev, idx, MLX5_PRISM_CMD_OP_ENABLE_HCA, 0,
			       0, MLX5_PRISM_CMD_HDR_SIZE,
			       0, MLX5_PRISM_CMD_HDR_SIZE);

	deadline = jiffies + msecs_to_jiffies(2000);
	do {
		own = READ_ONCE(lay->status_own);
		if (!(own & MLX5_PRISM_CMD_OWNER_HW))
			break;
		usleep_range(10, 20);
	} while (time_before(jiffies, deadline));

	if (own & MLX5_PRISM_CMD_OWNER_HW) {
		pci_err(dev->pdev,
			"cmdq self-test: doorbell not honoured within 2s\n");
		return -ETIMEDOUT;
	}

	/* Read descriptor fields after observing the ownership handover. */
	rmb();
	status = own >> 1;
	pci_info(dev->pdev,
		 "cmdq self-test: doorbell honoured, desc_status=%u out_status=%u\n",
		 status, (be32_to_cpu(lay->out[0]) >> 24) & 0xff);

	return 0;
}
