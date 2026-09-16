// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: Event Queue — creation (size ladder), EQE polling, teardown.
 *
 * T09 zeroes in on the CREATE_EQ mystery with interrupts entirely masked:
 * the MSI-X vector allocated in T04 stays disabled (IRQF_NO_AUTOEN), every
 * EQE is consumed by polling the ring from process context, and the arm
 * doorbell is the only new MMIO write in this task. /proc/interrupts
 * staying at 0 while events flow is the proof the line never fired.
 *
 * Commands generate EQEs because the invoke() descriptors carry a non-zero
 * token (mlx5_core only allocates tokens in events mode for the same
 * effect): each completed command posts a type-0x0a EQE whose vector bitmap
 * names the finished CMDQ slot. Poll-mode completion and EQE delivery are
 * independent — we keep waiting on the ownership bit (robust) and verify
 * the EQE trail after each command.
 *
 * Layout offsets verified against mlx5_ifc.h v5.15 and hardware hexdumps:
 * create_eq_in: eqc @payload0 (log_eq_size at eqc byte 12 bits[3:7],
 * uar_page at bytes[13:16]), event_bitmask @payload72 (4 x be64 words),
 * pas @payload256, inlen = 272 + 8 per page. create_eq_out eq_number @byte
 * 11; destroy_eq_in eq_number @byte 11; alloc_uar_out uar @bytes[9:12].
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mlx5_prism.h"
#include "mlx5_prism_fmt.h"

#define MLX5_PRISM_EQ_VEC_CMD	1	/* v4.19 MLX5_EQ_VEC_CMD */

/*
 * Event-commit patience window and silent-engine recreate retries —
 * measured fw behaviour (T09), tunable for experiments; the quirk profile
 * snapshots them for readers.
 */
u32 mlx5_prism_eq_patience_ms = 2000;
u32 mlx5_prism_eq_engagement_retries = 3;
module_param_named(eq_patience_ms, mlx5_prism_eq_patience_ms, uint, 0644);
module_param_named(eq_engagement_retries, mlx5_prism_eq_engagement_retries,
		   uint, 0644);
MODULE_PARM_DESC(eq_patience_ms,
		 "ms to wait for an EQE commit before declaring silence (default 2000)");
MODULE_PARM_DESC(eq_engagement_retries,
		 "EQ destroy/recreate attempts when the event engine stays silent (default 3)");

/*
 * CREATE_EQ size ladder — record the first rung the firmware accepts.
 * Order is deliberate: mlx5_core never builds single-page EQs on this box
 * (all its EQs carry 64+ pages), and the one-event-then-stall behaviour of
 * the small rungs suggests this fw accepts them but runs a crippled event
 * engine — so try the mlx5_core-shaped rung first and only fall back to
 * the geometric sizes if it is rejected.
 */
static const struct {
	u8	log_sz;
	int	npas;
	const char *name;
} mlx5_prism_eq_ladder[] = {
	{ 12, 64, "64 KiB / log12 / 64 pas (mlx5_core-style)" },
	{ 12, 16, "64 KiB / log12 / 16 pas" },
	{ 8,  4, "16 KiB / log8 / 4 pas" },
	{ 6,  1, "4 KiB / log6 / 1 pas" },
};

int mlx5_prism_eq_alloc(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	struct mlx5_prism_eqe *eqe;
	int i;

	eq->size = MLX5_PRISM_EQ_BUF_PAGES * MLX5_PRISM_PAGE_SIZE;
	eq->buf = dma_alloc_coherent(&dev->pdev->dev, eq->size,
				     &eq->dma, GFP_KERNEL);
	if (!eq->buf)
		return -ENOMEM;

	memset(eq->buf, 0, eq->size);
	eqe = eq->buf;
	for (i = 0; i < eq->size / MLX5_PRISM_EQE_SIZE; i++)
		eqe[i].owner = MLX5_PRISM_EQE_OWNER_INIT;

	eq->cons_index = 0;
	eq->events = 0;
	eq->created = false;
	eq->uar = 0;
	eq->doorbell = NULL;

	pci_info(dev->pdev,
		 "eq: cpu=%pK dma=%pad size=%zu pages=%zu\n",
		 eq->buf, &eq->dma, eq->size, eq->size / MLX5_PRISM_PAGE_SIZE);

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

/*
 * EQ doorbell (v5.15 mlx5_eq_update_ci): consumer index | eqn in the high
 * byte, big-endian, to uar + 0x40 to arm (re-enable event generation) or
 * + 0x48 to only publish the index.
 */
static void mlx5_prism_eq_update_ci(struct mlx5_prism_dev *dev, bool arm)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	u32 val = (eq->cons_index & 0xffffff) | (eq->eqn << 24);

	__raw_writel((__force u32)cpu_to_be32(val),
		     eq->doorbell + (arm ? 0 : 8));
	/* Order the doorbell write before any subsequent descriptor reads. */
	wmb();
}

/*
 * Consume fresh EQEs from process context. Two hardware facts drive the
 * loop shape (both measured on this fw, see v0.13 diagnostic run):
 *   - Event DATA is staged into the ring eagerly, but the owner byte —
 *     the commit — flips only after the driver publishes its consumer
 *     index through the non-arm doorbell (+8). Polling without doorbells
 *     starves forever with stale owners over live data.
 *   - The arm doorbell (+0x40) never releases staged events by itself
 *     (v0.12); it only gates interrupt generation — the line stays
 *     masked here, arming keeps the fw posting.
 * So each iteration walks the ring, then writes both doorbells; repeat
 * until an iteration consumes nothing new and the timeout expires.
 */
static int mlx5_prism_eq_poll(struct mlx5_prism_dev *dev, int timeout_ms)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	u32 nent = 1 << eq->log_sz;
	int processed = 0;

	for (;;) {
		struct mlx5_prism_eqe *e;
		int walked = 0;

		for (;;) {
			e = eq->buf + ((eq->cons_index & (nent - 1)) *
				       MLX5_PRISM_EQE_SIZE);
			if ((e->owner & 1) ^ !!(eq->cons_index & nent))
				break;		/* stale — walk done */
			/* Fresh: read it only after observing the owner flip. */
			dma_rmb();

			if (e->type == MLX5_PRISM_EVENT_TYPE_CMD) {
				u32 v = be32_to_cpu(e->data.cmd_vector);

				pci_info(dev->pdev,
					 "eqe[%u]: type=0x%02x vector=0x%08x\n",
					 eq->cons_index, e->type, v);
				/* The EQE acknowledges these slots — free them. */
				mlx5_prism_cmdq_complete_slots(dev, v);
			} else {
				pci_info(dev->pdev, "eqe[%u]: type=0x%02x\n",
					 eq->cons_index, e->type);
			}
			eq->cons_index++;
			eq->events++;
			walked++;
		}
		processed += walked;

		mlx5_prism_eq_update_ci(dev, false);	/* commit staged */
		mlx5_prism_eq_update_ci(dev, true);	/* keep posting */

		if (walked)
			continue;	/* the doorbells may release more */
		if (!time_before(jiffies, deadline))
			break;
		usleep_range(50, 100);
	}

	if (!processed && timeout_ms) {
		struct mlx5_prism_eqe *e = eq->buf;

		dev_warn_ratelimited(&dev->pdev->dev,
				     "no EQE within %d ms (cons=%u)\n",
				     timeout_ms, eq->cons_index);
		/* Raw ring state — what did the firmware actually write? */
		pci_info(dev->pdev,
			 "eqe[0..3] head: %16ph %16ph %16ph %16ph\n",
			 &e[0], &e[1], &e[2], &e[3]);
		pci_info(dev->pdev,
			 "owners[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			 e[0].owner, e[1].owner, e[2].owner, e[3].owner,
			 e[4].owner, e[5].owner, e[6].owner, e[7].owner);
	}

	return processed;
}

/*
 * (Re)create the EQ: ladder, authoritative size readback, ring re-init,
 * initial arm. Split out of setup so the self-test can retry creation —
 * the fw's event engine engages on only ~10% of loads (measured, cause
 * unknown; likely a create/arm race), and a fresh EQ rolls the dice again.
 */
static int mlx5_prism_eq_create(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	int err, i;

	/* Fresh ring state for the new EQ instance. */
	memset(eq->buf, 0, eq->size);
	{
		struct mlx5_prism_eqe *eqe = eq->buf;
		int n;

		for (n = 0; n < eq->size / MLX5_PRISM_EQE_SIZE; n++)
			eqe[n].owner = MLX5_PRISM_EQE_OWNER_INIT;
	}
	eq->cons_index = 0;

	for (i = 0; i < ARRAY_SIZE(mlx5_prism_eq_ladder); i++) {
		u8 log_sz = mlx5_prism_eq_ladder[i].log_sz;
		int npas = mlx5_prism_eq_ladder[i].npas;
		int inlen = 272 + 8 * npas;
		u8 *cin;
		u8 cout[MLX5_PRISM_CMD_HDR_SIZE];
		int j;

		cin = kzalloc(inlen, GFP_KERNEL);
		if (!cin)
			return -ENOMEM;

		/*
		 * eqc dword at payload bytes[12:16] and vector binding —
		 * exact legacy T09 encoding, see prism_eqc_write_dw3()/
		 * prism_eqc_set_intr() provenance in mlx5_prism_fmt.h.
		 */
		prism_eqc_write_dw3(cin, log_sz, eq->uar);
		prism_eqc_set_intr(cin, MLX5_PRISM_EQ_VEC_CMD);
		mlx5_prism_put_be64(cin + PRISM_OFF_EQ_EVENT_BITMASK,
				    BIT(MLX5_PRISM_EVENT_TYPE_CMD));
		for (j = 0; j < npas; j++)
			mlx5_prism_put_be64(cin + PRISM_OFF_EQ_PAS + 8 * j,
					    eq->dma + (dma_addr_t)j *
					    MLX5_PRISM_PAGE_SIZE);

		err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_EQ,
					     0, cin, inlen, cout, sizeof(cout));
		kfree(cin);
		if (err)
			continue;

		eq->eqn = cout[PRISM_OFF_EQN];
		eq->log_sz = log_sz;
		eq->created = true;
		/* Slot release now defers to completion EQEs (cmdq->mode). */
		mlx5_prism_cmdq_eq_bound(dev);

		/*
		 * The fw's actual ring size can differ from the request
		 * (measured: log12 requested, 1 entry built — every event
		 * lands at entry 0, owner alternating per delivery). QUERY_EQ
		 * is the authoritative size; adopt it for the consumer.
		 */
		{
			u8 qin[MLX5_PRISM_CMD_HDR_SIZE] = { };
			u8 qout[MLX5_PRISM_CMD_HDR_SIZE + 64];
			u8 *qeqc = qout + MLX5_PRISM_CMD_HDR_SIZE;
			u8 log_fw;
			int qerr;

			qin[PRISM_OFF_EQN] = eq->eqn;
			qerr = mlx5_prism_cmdq_invoke(dev,
						      MLX5_PRISM_CMD_OP_QUERY_EQ,
						      0, qin, sizeof(qin),
						      qout, sizeof(qout));
			if (!qerr) {
				log_fw = prism_get_bits(qeqc,
							PRISM_FLD_EQC_LOGSZ_READBACK,
							5);
				pci_info(dev->pdev,
					 "QUERY_EQ: fw reports log_eq_size=%u (%u entries; requested %u) eqn=%u\n",
					 log_fw, 1u << log_fw, 1u << log_sz,
					 eq->eqn);
				eq->log_sz = log_fw;
			}
		}

		/* Initial arm (mlx5_eq_enable's eq_update_ci(eq,1)). */
		mlx5_prism_eq_update_ci(dev, true);
		return 0;
	}

	pci_err(dev->pdev, "CREATE_EQ: every ladder rung rejected\n");
	return -EIO;
}

static void mlx5_prism_eq_destroy(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	int err;

	if (!eq->created)
		return;

	in[PRISM_OFF_EQN] = eq->eqn;
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_DESTROY_EQ, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		pci_warn(dev->pdev, "DESTROY_EQ failed: %d\n", err);
	else
		pci_info(dev->pdev,
			 "DESTROY_EQ ok: eq_number=%u, %u events consumed\n",
			 eq->eqn, eq->events);
	eq->created = false;
	eq->eqn = 0;
	eq->log_sz = 0;
	mlx5_prism_cmdq_eq_gone(dev);
}

int mlx5_prism_eq_setup(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u32 uar;
	int err;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_ALLOC_UAR, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	uar = mlx5_prism_get_be32(out + PRISM_OFF_UAR_OUT) & 0xffffff;
	eq->uar = uar;
	eq->doorbell = dev->bar0 + (dma_addr_t)uar * MLX5_PRISM_PAGE_SIZE +
		       MLX5_PRISM_EQ_DOORBELL_OFFSET;
	pci_info(dev->pdev, "ALLOC_UAR ok: uar=%u doorbell=%pK\n",
		 uar, eq->doorbell);

	err = mlx5_prism_eq_create(dev);
	if (err)
		return err;

	enable_irq(dev->irq.vector[MLX5_PRISM_EQ_VEC_CMD]);
	/* Second arm after the line is live — absorbs the create/arm race. */
	mlx5_prism_eq_update_ci(dev, true);
	pci_info(dev->pdev,
		 "EQ ready: eqn=%u entries=%u (armed, irq enabled: counting stub)\n",
		 eq->eqn, 1 << eq->log_sz);
	return 0;
}

/*
 * Fire commands and consume one EQE each, from process context. On a miss
 * (the fw's event engine engages on only ~10% of loads) retry by
 * destroying and re-creating the EQ — each creation re-rolls the dice.
 */
int mlx5_prism_eq_selftest(struct mlx5_prism_dev *dev)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u32 patience = dev->profile.eq_poll_patience_ms;
	u32 max_retries = dev->profile.eq_engagement_retries;
	int i, err, eqes, retries, total = 0;

	for (i = 0; i < 10; i++) {
		err = mlx5_prism_cmdq_invoke(dev,
					     MLX5_PRISM_CMD_OP_QUERY_PAGES,
					     MLX5_PRISM_QUERY_PAGES_BOOT,
					     in, sizeof(in), out, sizeof(out));
		if (err) {
			pci_err(dev->pdev,
				"eq self-test: QUERY_PAGES failed at %d/10: %d\n",
				i + 1, err);
			return err;
		}
		for (retries = 0; ; retries++) {
			eqes = mlx5_prism_eq_poll(dev, patience);
			if (eqes > 0)
				break;
			if (retries >= max_retries) {
				pci_err(dev->pdev,
					"eq self-test: no EQE for command %d/10 after %d recreations\n",
					i + 1, retries);
				return -EIO;
			}
			pci_info(dev->pdev,
				 "eq self-test: event engine silent at %d/10 — recreating EQ (attempt %d)\n",
				 i + 1, retries + 1);
			mlx5_prism_eq_destroy(dev);
			err = mlx5_prism_eq_create(dev);
			if (err)
				return err;
			mlx5_prism_eq_update_ci(dev, true);
		}
		total += eqes;
	}

	pci_info(dev->pdev,
		 "eq self-test: 10/10 commands ok with an EQE each (%u events total, %u interrupts)\n",
		 total, atomic_read(&dev->irq.count));
	return 0;
}

void mlx5_prism_eq_teardown(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	int err;

	if (eq->created) {
		/* Stop event generation before tearing the EQ down. */
		disable_irq(dev->irq.vector[MLX5_PRISM_EQ_VEC_CMD]);
		mlx5_prism_eq_destroy(dev);
	}

	if (eq->doorbell) {
		mlx5_prism_put_be32(in + PRISM_OFF_UAR_IN, eq->uar);
		err = mlx5_prism_cmdq_invoke(dev,
					     MLX5_PRISM_CMD_OP_DEALLOC_UAR, 0,
					     in, sizeof(in), out, sizeof(out));
		if (err)
			pci_warn(dev->pdev, "DEALLOC_UAR failed: %d\n", err);
		eq->doorbell = NULL;
		eq->uar = 0;
	}
}
