// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: CMDQ engine.
 *
 * T03: allocate the 4 KiB command-descriptor ring via dma_alloc_coherent.
 * T05: publish the ring DMA address to the init segment, build command
 *	descriptors and ring the doorbell.
 * T06a: full poll-mode round trip — slot registry (bitmap + rolling token),
 *	blocking invoke() that polls the ownership bit, two-layer status
 *	parsing (delivery status vs command status/syndrome) and clean
 *	timeout handling. This matches mlx5_core's CMD_MODE_POLLING, which is
 *	how the whole firmware bootstrap runs; the EQ/interrupt completion
 *	path arrives in T09/T10.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "mlx5_prism.h"

#define MLX5_PRISM_CMD_TIMEOUT_MSECS	2000

int mlx5_prism_cmdq_alloc(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;

	cmdq->size = MLX5_PRISM_PAGE_SIZE;
	cmdq->buf = dma_alloc_coherent(&dev->pdev->dev, cmdq->size,
				       &cmdq->dma, GFP_KERNEL);
	if (!cmdq->buf)
		return -ENOMEM;

	spin_lock_init(&cmdq->lock);
	cmdq->token_next = 0;
	bitmap_zero(cmdq->slots, MLX5_PRISM_MAX_COMMANDS);

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

int mlx5_prism_cmdq_invoke(struct mlx5_prism_dev *dev,
			   u16 opcode, u16 op_mod,
			   u8 *in, u32 in_len, u8 *out, u32 out_len)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;
	struct mlx5_prism_cmd_layout *lay;
	unsigned long deadline, flags;
	u8 delivery, status, own = MLX5_PRISM_CMD_OWNER_HW;
	u32 syndrome;
	int idx;

	if (in_len != MLX5_PRISM_CMD_HDR_SIZE ||
	    out_len != MLX5_PRISM_CMD_HDR_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&cmdq->lock, flags);

	/*
	 * All slots are equal for now; the top slot becomes the reserved
	 * MANAGE_PAGES queue when T07 introduces page commands.
	 */
	idx = find_first_zero_bit(cmdq->slots, cmdq->max_cmds);
	if (idx >= cmdq->max_cmds) {
		spin_unlock_irqrestore(&cmdq->lock, flags);
		return -EBUSY;
	}
	__set_bit(idx, cmdq->slots);

	cmdq->token_next++;
	if (!cmdq->token_next)
		cmdq->token_next = 1;

	lay = cmdq->buf + (idx << cmdq->log_stride);
	memset(lay, 0, sizeof(*lay));
	lay->type = MLX5_PRISM_PCI_CMD_XPORT;
	lay->inlen = cpu_to_be32(in_len);
	lay->in_ptr = 0;
	/* Inline 16-byte header: opcode at [0:2], op_mod at [6:8], args at [8:16]. */
	lay->in[0] = cpu_to_be32((u32)opcode << 16);
	lay->in[1] = cpu_to_be32((u32)op_mod);
	memcpy(&lay->in[2], in + 8, 8);
	lay->out_ptr = 0;
	lay->outlen = cpu_to_be32(out_len);
	lay->token = cmdq->token_next;
	lay->status_own = MLX5_PRISM_CMD_OWNER_HW;
	lay->sig = ~mlx5_prism_xor8(lay, sizeof(*lay));

	/*
	 * Order the descriptor writes before the doorbell MMIO write. wmb() —
	 * not smp_wmb() — because the consumer is the firmware reading via
	 * PCIe DMA, not another CPU.
	 */
	wmb();
	iowrite32be(1 << idx, &dev->iseg->cmd_dbell);

	spin_unlock_irqrestore(&cmdq->lock, flags);

	deadline = jiffies + msecs_to_jiffies(MLX5_PRISM_CMD_TIMEOUT_MSECS);
	while (time_before(jiffies, deadline)) {
		own = READ_ONCE(lay->status_own);
		if (!(own & MLX5_PRISM_CMD_OWNER_HW))
			break;
		usleep_range(10, 20);
	}

	if (own & MLX5_PRISM_CMD_OWNER_HW) {
		/*
		 * The slot bit stays set: the firmware may still complete the
		 * descriptor long after we gave up, and a stale slot must not
		 * be handed to a new command (mlx5_core leaks it too).
		 */
		pci_err(dev->pdev,
			"opcode 0x%x: no ownership handover in %d ms, slot %d leaked\n",
			opcode, MLX5_PRISM_CMD_TIMEOUT_MSECS, idx);
		return -ETIMEDOUT;
	}

	/* Read the descriptor only after observing the ownership handover. */
	rmb();
	delivery = own >> 1;
	status = (be32_to_cpu(lay->out[0]) >> 24) & 0xff;
	/* Syndrome is the full be32 at out bytes[4:8] (fw writes e.g. 0x4d99ef). */
	syndrome = be32_to_cpu(lay->out[1]) & 0xffffff;
	memcpy(out, lay->out, MLX5_PRISM_CMD_HDR_SIZE);

	spin_lock_irqsave(&cmdq->lock, flags);
	__clear_bit(idx, cmdq->slots);
	spin_unlock_irqrestore(&cmdq->lock, flags);

	if (delivery) {
		pci_err(dev->pdev, "opcode 0x%x: delivery status 0x%x\n",
			opcode, delivery);
		return -EIO;
	}
	if (status) {
		pci_err(dev->pdev,
			"opcode 0x%x: command status 0x%x, syndrome 0x%06x\n",
			opcode, status, syndrome);
		return -EIO;
	}

	return 0;
}

int mlx5_prism_cmdq_selftest(struct mlx5_prism_dev *dev)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u32 boot_pages = 0;
	int i, err;

	if (ioread32be(&dev->iseg->initializing) >> 31) {
		pci_warn(dev->pdev, "cmdq self-test: firmware still initializing\n");
		return -EAGAIN;
	}

	/*
	 * ENABLE_HCA is the canonical first command the firmware accepts (the
	 * fw revision lives in the init segment, so there is no QUERY_FW on
	 * ConnectX-4 Lx). Header-only command — exercises the full poll path.
	 */
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_ENABLE_HCA, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	pci_info(dev->pdev, "cmdq poll self-test: ENABLE_HCA ok\n");

	/*
	 * QUERY_PAGES(boot): side-effect-free probe that is also the first
	 * real bootstrap command (T07 gives these pages to the firmware).
	 * num_pages rides in the inline out bytes[12:16].
	 */
	for (i = 0; i < 10; i++) {
		err = mlx5_prism_cmdq_invoke(dev,
					     MLX5_PRISM_CMD_OP_QUERY_PAGES,
					     MLX5_PRISM_QUERY_PAGES_BOOT,
					     in, sizeof(in), out, sizeof(out));
		if (err) {
			pci_err(dev->pdev,
				"cmdq poll self-test: QUERY_PAGES(boot) failed at %d/10: %d\n",
				i + 1, err);
			return err;
		}
		boot_pages = mlx5_prism_get_be32(out + 12);
	}
	pci_info(dev->pdev,
		 "cmdq poll self-test: QUERY_PAGES(boot) x10 ok, boot pages=%u\n",
		 boot_pages);

	/* Bad opcode: the firmware must answer with a status, not silence. */
	err = mlx5_prism_cmdq_invoke(dev, 0xdead, 0,
				     in, sizeof(in), out, sizeof(out));
	if (!err) {
		pci_err(dev->pdev,
			"cmdq self-test: bad opcode 0xdead unexpectedly ok\n");
		return -EINVAL;
	}
	if (err == -ETIMEDOUT) {
		pci_err(dev->pdev, "cmdq self-test: bad opcode 0xdead timed out\n");
		return err;
	}
	pci_info(dev->pdev,
		 "cmdq poll self-test: bad opcode 0xdead rejected fast (%d)\n",
		 err);

	return 0;
}
