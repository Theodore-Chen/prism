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
 * T06b: mailbox chains — payload beyond the 16-byte inline header travels
 *	through 1 KiB-aligned 576-byte blocks linked by DMA next pointers.
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
#include "mlx5_prism_fmt.h"

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
	cmdq->mode = MLX5_PRISM_CMDQ_POLL_INLINE;
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

/*
 * Mailbox chain helpers (PRM §4.2.2). Payload beyond the 16-byte inline
 * header travels in 576-byte blocks linked by DMA next pointers, numbered
 * from 0 at the head. Blocks are laid out at a 1 KiB stride — mlx5_core
 * serves them from a dma_pool with 1 KiB alignment and the firmware rejects
 * misaligned block pointers (delivery stat 0x4/0x5). sig/ctrl_sig stay zero:
 * this firmware defaults to mailbox checksums disabled.
 */
static struct mlx5_prism_cmd_block *
mlx5_prism_chain_alloc(struct mlx5_prism_dev *dev, int n, dma_addr_t *dma)
{
	struct mlx5_prism_cmd_block *blks;
	int i;

	blks = dma_alloc_coherent(&dev->pdev->dev,
				  n * MLX5_PRISM_CMD_BLOCK_STRIDE,
				  dma, GFP_KERNEL);
	if (!blks)
		return NULL;

	for (i = 0; i < n; i++) {
		struct mlx5_prism_cmd_block *b;

		b = (struct mlx5_prism_cmd_block *)
			((u8 *)blks + i * MLX5_PRISM_CMD_BLOCK_STRIDE);
		b->next = cpu_to_be64(i + 1 < n ?
				      *dma + (i + 1) *
				      MLX5_PRISM_CMD_BLOCK_STRIDE : 0);
		b->block_num = cpu_to_be32(i);
	}
	return blks;
}

static struct mlx5_prism_cmd_block *
mlx5_prism_chain_at(struct mlx5_prism_cmd_block *blks, int i)
{
	return (struct mlx5_prism_cmd_block *)
		((u8 *)blks + i * MLX5_PRISM_CMD_BLOCK_STRIDE);
}

static void mlx5_prism_chain_token(struct mlx5_prism_cmd_block *blks,
				   int n, u8 token)
{
	int i;

	for (i = 0; i < n; i++)
		mlx5_prism_chain_at(blks, i)->token = token;
}

static void mlx5_prism_chain_fill(struct mlx5_prism_cmd_block *blks, int n,
				  const u8 *src, size_t len)
{
	int i;

	for (i = 0; i < n && len; i++) {
		size_t copy = min_t(size_t, len,
				    MLX5_PRISM_CMD_DATA_BLOCK_SIZE);

		memcpy(mlx5_prism_chain_at(blks, i)->data, src, copy);
		src += copy;
		len -= copy;
	}
}

static void mlx5_prism_chain_drain(struct mlx5_prism_cmd_block *blks, int n,
				   u8 *dst, size_t len)
{
	int i;

	for (i = 0; i < n && len; i++) {
		size_t copy = min_t(size_t, len,
				    MLX5_PRISM_CMD_DATA_BLOCK_SIZE);

		memcpy(dst, mlx5_prism_chain_at(blks, i)->data, copy);
		dst += copy;
		len -= copy;
	}
}

static int mlx5_prism_cmd_exec(struct mlx5_prism_dev *dev,
			       u16 opcode, u16 op_mod,
			       u8 *in, u32 in_len, u8 *out, u32 out_len,
			       bool page_queue)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;
	struct mlx5_prism_cmd_layout *lay;
	struct mlx5_prism_cmd_block *in_blks = NULL, *out_blks = NULL;
	dma_addr_t in_dma = 0, out_dma = 0;
	size_t in_pay, out_pay, chain_max;
	unsigned long deadline, flags;
	u8 delivery, status, own = MLX5_PRISM_CMD_OWNER_HW;
	u32 syndrome;
	int err, idx, n_in, n_out;

	if (!mlx5_prism_fw_cmd_allowed(dev, opcode))
		return -EINVAL;

	chain_max = MLX5_PRISM_CMD_DATA_BLOCK_SIZE * MLX5_PRISM_CMD_MAX_CHAIN;
	if (in_len < MLX5_PRISM_CMD_HDR_SIZE ||
	    out_len < MLX5_PRISM_CMD_HDR_SIZE ||
	    in_len > MLX5_PRISM_CMD_HDR_SIZE + chain_max ||
	    out_len > MLX5_PRISM_CMD_HDR_SIZE + chain_max)
		return -EINVAL;

	in_pay = in_len - MLX5_PRISM_CMD_HDR_SIZE;
	out_pay = out_len - MLX5_PRISM_CMD_HDR_SIZE;
	n_in = DIV_ROUND_UP(in_pay, MLX5_PRISM_CMD_DATA_BLOCK_SIZE);
	n_out = DIV_ROUND_UP(out_pay, MLX5_PRISM_CMD_DATA_BLOCK_SIZE);

	if (n_in) {
		in_blks = mlx5_prism_chain_alloc(dev, n_in, &in_dma);
		if (!in_blks)
			return -ENOMEM;
		mlx5_prism_chain_fill(in_blks, n_in,
				      in + MLX5_PRISM_CMD_HDR_SIZE, in_pay);
	}
	if (n_out) {
		out_blks = mlx5_prism_chain_alloc(dev, n_out, &out_dma);
		if (!out_blks) {
			err = -ENOMEM;
			goto err_free_in;
		}
	}

	spin_lock_irqsave(&cmdq->lock, flags);

	/*
	 * The top slot is reserved for MANAGE_PAGES (mlx5_core's page_queue);
	 * regular commands share the slots below it.
	 */
	if (page_queue) {
		idx = cmdq->max_cmds - 1;
		if (test_and_set_bit(idx, cmdq->slots)) {
			spin_unlock_irqrestore(&cmdq->lock, flags);
			err = -EBUSY;
			goto err_free_out;
		}
	} else {
		idx = find_first_zero_bit(cmdq->slots, cmdq->max_cmds - 1);
		if (idx >= cmdq->max_cmds - 1) {
			spin_unlock_irqrestore(&cmdq->lock, flags);
			err = -EBUSY;
			goto err_free_out;
		}
		__set_bit(idx, cmdq->slots);
	}

	cmdq->token_next++;
	if (!cmdq->token_next)
		cmdq->token_next = 1;

	lay = cmdq->buf + (idx << cmdq->log_stride);
	memset(lay, 0, sizeof(*lay));
	lay->type = MLX5_PRISM_PCI_CMD_XPORT;
	lay->inlen = cpu_to_be32(in_len);
	lay->in_ptr = cpu_to_be64(in_dma);
	/* Inline 16-byte header: opcode at [0:2], op_mod at [6:8], args at [8:16]. */
	lay->in[0] = cpu_to_be32((u32)opcode << 16);
	lay->in[1] = cpu_to_be32((u32)op_mod);
	memcpy(&lay->in[2], in + 8, 8);
	lay->out_ptr = cpu_to_be64(out_dma);
	lay->outlen = cpu_to_be32(out_len);
	lay->token = cmdq->token_next;
	lay->status_own = MLX5_PRISM_CMD_OWNER_HW;
	lay->sig = ~mlx5_prism_xor8(lay, sizeof(*lay));
	if (in_blks)
		mlx5_prism_chain_token(in_blks, n_in, cmdq->token_next);
	if (out_blks)
		mlx5_prism_chain_token(out_blks, n_out, cmdq->token_next);

	/*
	 * Order the descriptor (and any mailbox writes) before the doorbell MMIO
	 * write. wmb() — not smp_wmb() — because the consumer is the firmware
	 * reading via PCIe DMA, not another CPU.
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
		 * be handed to a new command (mlx5_core leaks it too). The
		 * mailbox chains leak with it — the firmware still holds their
		 * DMA addresses, so freeing (and reusing) the memory could let
		 * a late completion corrupt an unrelated command.
		 */
		pci_err(dev->pdev,
			"opcode 0x%x: no ownership handover in %d ms, slot %d + %d mailbox blocks leaked\n",
			opcode, MLX5_PRISM_CMD_TIMEOUT_MSECS, idx,
			n_in + n_out);
		return -ETIMEDOUT;
	}

	/* Read the descriptor only after observing the ownership handover. */
	rmb();
	delivery = own >> 1;
	status = (be32_to_cpu(lay->out[0]) >> 24) & 0xff;
	/* Syndrome is the full be32 at out bytes[4:8] (fw writes e.g. 0x4d99ef). */
	syndrome = be32_to_cpu(lay->out[1]) & 0xffffff;
	memcpy(out, lay->out, MLX5_PRISM_CMD_HDR_SIZE);
	if (out_pay)
		mlx5_prism_chain_drain(out_blks, n_out,
				       out + MLX5_PRISM_CMD_HDR_SIZE, out_pay);

	spin_lock_irqsave(&cmdq->lock, flags);
	/*
	 * Slot lifecycle follows cmdq->mode: with an EQ alive the slot stays
	 * BUSY until the command's completion EQE (vector bit) is consumed —
	 * see enum mlx5_prism_cmdq_mode. Without an EQ there is nobody to
	 * release it — clear here.
	 */
	if (cmdq->mode == MLX5_PRISM_CMDQ_POLL_INLINE)
		__clear_bit(idx, cmdq->slots);
	spin_unlock_irqrestore(&cmdq->lock, flags);

	if (out_blks)
		dma_free_coherent(&dev->pdev->dev,
				  n_out * MLX5_PRISM_CMD_BLOCK_STRIDE,
				  out_blks, out_dma);
	if (in_blks)
		dma_free_coherent(&dev->pdev->dev,
				  n_in * MLX5_PRISM_CMD_BLOCK_STRIDE,
				  in_blks, in_dma);

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

err_free_out:
	if (out_blks)
		dma_free_coherent(&dev->pdev->dev,
				  n_out * MLX5_PRISM_CMD_BLOCK_STRIDE,
				  out_blks, out_dma);
err_free_in:
	if (in_blks)
		dma_free_coherent(&dev->pdev->dev,
				  n_in * MLX5_PRISM_CMD_BLOCK_STRIDE,
				  in_blks, in_dma);
	return err;
}

int mlx5_prism_cmdq_invoke(struct mlx5_prism_dev *dev,
			   u16 opcode, u16 op_mod,
			   u8 *in, u32 in_len, u8 *out, u32 out_len)
{
	return mlx5_prism_cmd_exec(dev, opcode, op_mod, in, in_len,
				   out, out_len, false);
}

int mlx5_prism_cmdq_invoke_pages(struct mlx5_prism_dev *dev,
				 u16 opcode, u16 op_mod,
				 u8 *in, u32 in_len, u8 *out, u32 out_len)
{
	return mlx5_prism_cmd_exec(dev, opcode, op_mod, in, in_len,
				   out, out_len, true);
}

/*
 * Release CMDQ slots named by a command-completion vector bitmap — the
 * EQ side's single entry point into slot state (DESIGN.md v2 §2.1 rule 2).
 */
int mlx5_prism_cmdq_complete_slots(struct mlx5_prism_dev *dev, u32 vector)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;
	unsigned long v = vector, flags;
	int i, n = 0;

	spin_lock_irqsave(&cmdq->lock, flags);
	for_each_set_bit(i, &v, cmdq->max_cmds) {
		if (test_and_clear_bit(i, cmdq->slots))
			n++;
	}
	spin_unlock_irqrestore(&cmdq->lock, flags);

	return n;
}

/* EQ created: hold each regular slot until its completion EQE arrives. */
void mlx5_prism_cmdq_eq_bound(struct mlx5_prism_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->cmdq.lock, flags);
	dev->cmdq.mode = MLX5_PRISM_CMDQ_EQ_DEFERRED;
	spin_unlock_irqrestore(&dev->cmdq.lock, flags);
}

/* EQ destroyed: pending EQEs will never arrive — forget every held slot. */
void mlx5_prism_cmdq_eq_gone(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_cmdq *cmdq = &dev->cmdq;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&cmdq->lock, flags);
	cmdq->mode = MLX5_PRISM_CMDQ_POLL_INLINE;
	for (i = 0; i < cmdq->max_cmds - 1; i++)
		__clear_bit(i, cmdq->slots);
	spin_unlock_irqrestore(&cmdq->lock, flags);
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
		boot_pages = mlx5_prism_get_be32(out + PRISM_OFF_QP_NUM_PAGES);
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

/*
 * T06b: mailbox-chain self-test. QUERY_ISSI (96 B payload out, single
 * block) is the only >16 B command this firmware accepts before boot pages
 * are given — QUERY_HCA_CAP draws status 0x4 / syndrome 0x2b69b6 at that
 * stage (measured on both ports), it only becomes legal afterwards, so its
 * 8-block chain is probed by the T07 bootstrap right after the boot-page
 * give. Negotiating ISSI 1 here is also the first step of the T07
 * bootstrap, and idempotent on this firmware.
 */
int mlx5_prism_cmdq_mailbox_selftest(struct mlx5_prism_dev *dev)
{
	u8 qin[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 qout[MLX5_PRISM_CMD_HDR_SIZE + 96];
	u8 sout[MLX5_PRISM_CMD_HDR_SIZE];
	u8 sin[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u32 supported;
	int err;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_ISSI, 0,
				     qin, sizeof(qin), qout, sizeof(qout));
	if (err)
		return err;
	supported = mlx5_prism_get_be32(qout + PRISM_OFF_ISSI_SUPPORTED);
	pci_info(dev->pdev,
		 "mailbox self-test: QUERY_ISSI ok (1 block): current=%u supported=0x%x\n",
		 mlx5_prism_get_be16(qout + PRISM_OFF_ISSI_CUR), supported);

	if (supported & BIT(1)) {
		/* current_issi: be16 at inline bytes[10:12]. */
		mlx5_prism_put_be16(sin + PRISM_OFF_ISSI_CUR, 1);
		err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_SET_ISSI, 0,
					     sin, sizeof(sin),
					     sout, sizeof(sout));
		if (err)
			return err;
		pci_info(dev->pdev, "mailbox self-test: ISSI set to 1\n");
	} else {
		pci_info(dev->pdev,
			 "mailbox self-test: ISSI 1 unsupported, staying at 0\n");
	}

	return 0;
}
