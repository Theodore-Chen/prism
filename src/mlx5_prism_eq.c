// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: Event Queue — the firmware->driver event channel.
 *
 * Reading map: eq_alloc (ring, owner prefill 0x01) -> eq_setup (ALLOC_UAR,
 * doorbell = bar0 + uar*4K + 0x40, create ladder, authoritative QUERY_EQ
 * size readback, arm) -> eq_poll (process-context consumption) ->
 * eq_teardown. eq_selftest drives 10 commands each expecting one EQE.
 *
 * Measured fw behaviour (DESIGN.md §7; T09 + T10-prep): event data is
 * staged eagerly but the owner byte — the commit — flips only after the
 * non-arm doorbell (+0x48) publishes the consumer index; the arm doorbell
 * (+0x40) gates interrupt generation only. The T09-era "~10% engagement"
 * and "interrupts never fire" findings were artifacts of our own eqc
 * encoding — the corrected encodings give deterministic first-attempt
 * delivery and a live MSI-X line (progress.md T10-prep).
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
#include "mlx5_prism_trace.h"

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
 * T10: completion mode. Default "event" = MSI-X hardirq consumption with
 * invoke sleeping on the completion (mlx5_core shape); "poll" = the T09
 * behaviour verbatim (process-context eq_poll, irq handler counts only) —
 * the recovery fallback.
 */
static char *eq_mode = "event";
module_param(eq_mode, charp, 0444);
MODULE_PARM_DESC(eq_mode,
		 "EQ completion mode: event (default, MSI-X hardirq) or poll (T09 fallback)");

/*
 * CREATE_EQ size ladder — record the first rung the firmware accepts.
 * Rung 0 (mlx5_core-shaped log12/64pas) has been accepted on every load
 * since the eqc encoding fix (T10-prep: 10/10); the lower rungs are a
 * defensive fallback for a fw that ever rejects the big ring.
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

static void prism_eq_init_state(struct mlx5_prism_eq *eq)
{
	struct mlx5_prism_eqe *eqe = eq->buf;
	int i;

	memset(eq->buf, 0, eq->size);
	for (i = 0; i < eq->size / MLX5_PRISM_EQE_SIZE; i++)
		eqe[i].owner = MLX5_PRISM_EQE_OWNER_INIT;

	eq->cons_index = 0;
	eq->events = 0;
	atomic_set(&eq->irq_events, 0);
	eq->empty_irqs = 0;
	eq->storm_latched = false;
	eq->event_mode = strcmp(eq_mode, "poll") != 0;
	eq->created = false;
	eq->eqn = 0;
	eq->uar = 0;
	eq->doorbell = NULL;
}

static int prism_eq_alloc_buf(struct mlx5_prism_dev *dev,
			      struct mlx5_prism_eq *eq, const char *name)
{
	eq->size = MLX5_PRISM_EQ_BUF_PAGES * MLX5_PRISM_PAGE_SIZE;
	eq->buf = dma_alloc_coherent(&dev->pdev->dev, eq->size,
				     &eq->dma, GFP_KERNEL);
	if (!eq->buf)
		return -ENOMEM;

	prism_eq_init_state(eq);

	pci_info(dev->pdev,
		 "%s: cpu=%pK dma=%pad size=%zu pages=%zu\n",
		 name, eq->buf, &eq->dma, eq->size,
		 eq->size / MLX5_PRISM_PAGE_SIZE);
	return 0;
}

int mlx5_prism_eq_alloc(struct mlx5_prism_dev *dev)
{
	int err;

	err = prism_eq_alloc_buf(dev, &dev->eq, "eq");
	if (err)
		return err;
	/* The completion EQ (T15) has its own ring on vector 0. */
	err = prism_eq_alloc_buf(dev, &dev->comp_eq, "comp eq");
	if (err) {
		dma_free_coherent(&dev->pdev->dev, dev->eq.size,
				  dev->eq.buf, dev->eq.dma);
		dev->eq.buf = NULL;
		return err;
	}
	return 0;
}

static void prism_eq_free_buf(struct mlx5_prism_dev *dev,
			      struct mlx5_prism_eq *eq)
{
	if (!eq->buf)
		return;

	dma_free_coherent(&dev->pdev->dev, eq->size, eq->buf, eq->dma);
	eq->buf = NULL;
	eq->dma = 0;
	eq->size = 0;
	eq->cons_index = 0;
}

void mlx5_prism_eq_free(struct mlx5_prism_dev *dev)
{
	prism_eq_free_buf(dev, &dev->comp_eq);
	prism_eq_free_buf(dev, &dev->eq);
}

/*
 * EQ doorbell (v5.15 mlx5_eq_update_ci): consumer index | eqn in the high
 * byte, big-endian, to uar + 0x40 to arm (re-enable event generation) or
 * + 0x48 to only publish the index.
 */
static void mlx5_prism_eq_update_ci(struct mlx5_prism_dev *dev,
				     struct mlx5_prism_eq *eq, bool arm)
{
	u32 val = (eq->cons_index & 0xffffff) | (eq->eqn << 24);

	__raw_writel((__force u32)cpu_to_be32(val),
		     eq->doorbell + (arm ? 0 : 8));
	/* Order the doorbell write before any subsequent descriptor reads. */
	wmb();
}

/*
 * T10 hardirq consumption (mlx5_eq_async_int shape): walk fresh EQEs up
 * to a budget, dispatch CMD vectors to the cmdq completion backend, and
 * re-arm once on the way out. Deliberately no printk on the hot path —
 * silence is normal; diagnostics come from the tracepoints, the eq
 * counters, and the storm guard.
 */
int mlx5_prism_eq_irq_consume(struct mlx5_prism_dev *dev,
			      struct mlx5_prism_eq *eq)
{
	u32 nent = 1 << eq->log_sz;
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

			trace_prism_eq_event(eq->eqn, e->type, v,
					     eq->cons_index);
			mlx5_prism_cmdq_complete_slots(dev, v);
		} else if (e->type == MLX5_PRISM_EVENT_TYPE_COMP) {
			u32 raw = mlx5_prism_get_be32((u8 *)e +
					PRISM_EQE_OFF_COMP_CQN);
			u32 cqn = raw & 0xffffff;

			trace_prism_eq_event(eq->eqn, e->type, cqn,
					     eq->cons_index);
			/* trip-15: the events ride the hardirq branch — the
			 * parsed cqn has never been verified against a real
			 * comp EQE; one line per event until settled
			 */
			dev_info_ratelimited(&dev->pdev->dev,
					     "eqe[%u]: comp cqn_raw=0x%08x parsed=%u\n",
					     eq->cons_index, raw, cqn);
			mlx5_prism_net_comp_event(dev, cqn);
		} else {
			trace_prism_eq_event(eq->eqn, e->type, 0,
					     eq->cons_index);
		}
		eq->cons_index++;
		eq->events++;
		atomic_inc(&eq->irq_events);
		if (++walked >= MLX5_PRISM_EQ_IRQ_BUDGET)
			break;		/* remainder arrives with the next IRQ */
	}

	mlx5_prism_eq_update_ci(dev, eq, true);
	return walked;
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
static int mlx5_prism_eq_poll(struct mlx5_prism_dev *dev,
			      struct mlx5_prism_eq *eq, int timeout_ms)
{
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

				trace_prism_eq_event(eq->eqn, e->type, v,
						     eq->cons_index);
				pci_info(dev->pdev,
					 "eqe[%u]: type=0x%02x vector=0x%08x\n",
					 eq->cons_index, e->type, v);
				/* The EQE acknowledges these slots — free them. */
				mlx5_prism_cmdq_complete_slots(dev, v);
			} else if (e->type == MLX5_PRISM_EVENT_TYPE_COMP) {
				u32 cqn = mlx5_prism_get_be32((u8 *)e +
						PRISM_EQE_OFF_COMP_CQN) & 0xffffff;

				trace_prism_eq_event(eq->eqn, e->type, cqn,
						     eq->cons_index);
				pci_info(dev->pdev,
					 "eqe[%u]: comp cqn_raw=0x%08x parsed=%u\n",
					 eq->cons_index,
					 mlx5_prism_get_be32((u8 *)e +
							 PRISM_EQE_OFF_COMP_CQN),
					 cqn);
				print_hex_dump(KERN_INFO, "comp eqe: ",
					       DUMP_PREFIX_OFFSET, 16, 1, e,
					       MLX5_PRISM_EQE_SIZE, false);
				mlx5_prism_net_comp_event(dev, cqn);
			} else {
				trace_prism_eq_event(eq->eqn, e->type, 0,
						     eq->cons_index);
				pci_info(dev->pdev, "eqe[%u]: type=0x%02x\n",
					 eq->cons_index, e->type);
			}
			eq->cons_index++;
			eq->events++;
			walked++;
		}
		processed += walked;

		mlx5_prism_eq_update_ci(dev, eq, false);	/* commit staged */
		mlx5_prism_eq_update_ci(dev, eq, true);	/* keep posting */

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
static int mlx5_prism_eq_create(struct mlx5_prism_dev *dev,
				struct mlx5_prism_eq *eq, int vec, u64 mask)
{
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
		 * eqc dword3 (log_eq_size/uar_page) and dword5 (intr) —
		 * ifc-field encodings, hw-confirmed; geometry note in
		 * mlx5_prism_fmt.h.
		 */
		prism_eqc_write_dw3(cin, log_sz, eq->uar);
		prism_eqc_set_intr(cin, vec);
		/*
		 * Trip-8 refutation: adding BIT(COMP) here had CREATE_EQ
		 * rejected outright on every ladder rung (0x3 / 0x008fef) —
		 * COMP (type 0x00) is not a subscribable async class on this
		 * fw. The cmd EQ rides the CMD-only mask; the comp EQ rides
		 * a ZERO mask — the exact v4.14/v5.15 create_comp_eqs form
		 * (mlx5_create_map_eq(..., 0, ...)) that this fw accepts
		 * from stock mlx5_core every day.
		 */
		mlx5_prism_put_be64(cin + PRISM_OFF_EQ_EVENT_BITMASK, mask);
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

		/*
		 * QUERY_EQ is the authoritative ring size; adopt it for the
		 * consumer (was load-bearing in the T09 era, when the legacy
		 * encoding requested a 1-entry ring).
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
				log_fw = prism_eqc_logsz_readback(qeqc);
				pci_info(dev->pdev,
					 "QUERY_EQ: fw reports log_eq_size=%u (%u entries; requested %u) eqn=%u\n",
					 log_fw, 1u << log_fw, 1u << log_sz,
					 eq->eqn);
				eq->log_sz = log_fw;
			}
		}

		/* Initial arm (mlx5_eq_enable's eq_update_ci(eq,1)). */
		mlx5_prism_eq_update_ci(dev, eq, true);
		/*
		 * Drain-before-enable (event mode): the force-polled
		 * EQ-lifecycle commands release their slot inline, but their
		 * completion EQE is still in flight. If it gets consumed after
		 * the caller binds EVENT mode, its vector bit aliases the
		 * pending bit of whatever command next takes that slot — the
		 * woken issuer reads an unflipped owner and leaks the slot
		 * (rebaseline 2026-09-18: command 1/10 -110 right after every
		 * fresh CREATE_EQ/QUERY_EQ pair; recreate does not help, it
		 * mints a new backlog EQE). Consuming the backlog here, with
		 * the line still masked, closes the window deterministically
		 * and makes the backlog consumption a visible, ordered dmesg
		 * line instead of a race.
		 */
		if (eq->event_mode)
			mlx5_prism_eq_poll(dev, eq, 0);
		return 0;
	}

	pci_err(dev->pdev, "CREATE_EQ: every ladder rung rejected\n");
	return -EIO;
}

static void mlx5_prism_eq_destroy(struct mlx5_prism_dev *dev,
				  struct mlx5_prism_eq *eq)
{
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
	if (eq == &dev->eq)
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

	err = mlx5_prism_eq_create(dev, &dev->eq, MLX5_PRISM_EQ_VEC_CMD,
				   BIT(MLX5_PRISM_EVENT_TYPE_CMD));
	if (err)
		return err;

	/*
	 * T15: dedicated completion EQ — the v4.14/v5.15 create_comp_eqs
	 * form: vector 0, ZERO event bitmask, sharing the function UAR
	 * page (all mlx5_core EQs doorbell the same page; the value
	 * carries the eqn). The CQs bind c_eqn to THIS eq; the trip-7/8
	 * chain showed comp events do not ride the async/cmd EQ.
	 */
	dev->comp_eq.uar = uar;
	dev->comp_eq.doorbell = dev->bar0 +
				 (dma_addr_t)uar * MLX5_PRISM_PAGE_SIZE +
				 MLX5_PRISM_EQ_DOORBELL_OFFSET;
	err = mlx5_prism_eq_create(dev, &dev->comp_eq, 0, 0);
	if (err)
		goto err_destroy_eq;
	enable_irq(dev->irq.vector[0]);

	enable_irq(dev->irq.vector[MLX5_PRISM_EQ_VEC_CMD]);
	/* Second arm after the line is live — absorbs the create/arm race. */
	mlx5_prism_eq_update_ci(dev, &dev->eq, true);
	mlx5_prism_eq_update_ci(dev, &dev->comp_eq, true);
	/*
	 * T10: bind the cmdq completion mode only after the line is live.
	 * Backlog EQEs (QUERY_EQ's own completion, consumed by the irq in
	 * the window above) hit POLL_INLINE-mode complete_slots, which
	 * no-ops on slots the polled issuers already freed.
	 */
	mlx5_prism_cmdq_eq_bound(dev, eq->event_mode);
	if (eq->event_mode)
		pci_info(dev->pdev,
			 "EQ up (event mode): eqn=%u entries=%u budget=%d\n",
			 eq->eqn, 1 << eq->log_sz, MLX5_PRISM_EQ_IRQ_BUDGET);
	else
		pci_info(dev->pdev,
			 "EQ ready: eqn=%u entries=%u (armed, poll fallback: irq counts only)\n",
			 eq->eqn, 1 << eq->log_sz);
	pci_info(dev->pdev,
		 "comp EQ up: eqn=%u entries=%u (vector 0, zero mask)\n",
		 dev->comp_eq.eqn, 1 << dev->comp_eq.log_sz);
	return 0;

err_destroy_eq:
	mlx5_prism_eq_destroy(dev, &dev->eq);
	return err;
}

/*
 * Event-mode recreate (defensive recovery for a silent engine — none
 * observed since the eqc encoding fix, T10-prep: 10/10 first-attempt).
 * Keeps the irq-core disable depth balanced whether or not the storm
 * guard already self-disabled the line.
 */
static int mlx5_prism_eq_recreate(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	int err;

	if (!eq->storm_latched)
		disable_irq(dev->irq.vector[MLX5_PRISM_EQ_VEC_CMD]);
	eq->storm_latched = false;
	eq->empty_irqs = 0;

	mlx5_prism_eq_destroy(dev, &dev->eq);
	err = mlx5_prism_eq_create(dev, &dev->eq, MLX5_PRISM_EQ_VEC_CMD,
				   BIT(MLX5_PRISM_EVENT_TYPE_CMD));
	if (err)
		return err;

	enable_irq(dev->irq.vector[MLX5_PRISM_EQ_VEC_CMD]);
	mlx5_prism_eq_update_ci(dev, &dev->eq, true);
	mlx5_prism_cmdq_eq_bound(dev, eq->event_mode);
	return 0;
}

/*
 * Fire commands and consume one EQE each. Event mode: each invoke sleeps
 * until its own EQE wakes it, so this is round trips plus accounting; the
 * recreate ladder stays as defensive recovery. Poll mode keeps the T09
 * patience-window shape verbatim.
 */
int mlx5_prism_eq_selftest(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u32 patience = dev->profile.eq_poll_patience_ms;
	u32 max_retries = dev->profile.eq_engagement_retries;
	int i, err, eqes, retries, total = 0;

	if (eq->event_mode) {
		for (i = 0; i < 10; i++) {
			for (retries = 0; ; retries++) {
				err = mlx5_prism_cmdq_invoke(dev,
						MLX5_PRISM_CMD_OP_QUERY_PAGES,
						MLX5_PRISM_QUERY_PAGES_BOOT,
						in, sizeof(in), out, sizeof(out));
				if (!err)
					break;
				if (retries >= max_retries) {
					pci_err(dev->pdev,
						"eq self-test: command %d/10 failed after %d recreations: %d\n",
						i + 1, retries, err);
					return err;
				}
				pci_info(dev->pdev,
					 "eq self-test: command %d/10 failed (%d) — recreating EQ (attempt %d)\n",
					 i + 1, err, retries + 1);
				err = mlx5_prism_eq_recreate(dev);
				if (err)
					return err;
			}
		}
		pci_info(dev->pdev,
			 "eq self-test: 10/10 commands ok in event mode (%u events, %u consumed in hardirq, %u interrupts)\n",
			 eq->events, atomic_read(&eq->irq_events),
			 atomic_read(&dev->irq.count));
		return 0;
	}

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
			eqes = mlx5_prism_eq_poll(dev, &dev->eq, patience);
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
			mlx5_prism_eq_destroy(dev, &dev->eq);
			err = mlx5_prism_eq_create(dev, &dev->eq,
						   MLX5_PRISM_EQ_VEC_CMD,
						   BIT(MLX5_PRISM_EVENT_TYPE_CMD));
			if (err)
				return err;
			mlx5_prism_eq_update_ci(dev, &dev->eq, true);
		}
		total += eqes;
	}

	pci_info(dev->pdev,
		 "eq self-test: 10/10 commands ok with an EQE each (%u events total, %u interrupts)\n",
		 total, atomic_read(&dev->irq.count));
	return 0;
}

/*
 * Close-time forensic probe (T15 trip-9/10): walk the comp EQ with the
 * poll machinery. The ci-publish doorbell commits any staged-but-
 * uncommitted EQEs (the T09 phenomenon), and each consumed event prints
 * its own line — type 0x00 entries here mean comp events were generated
 * but never committed/raised; an empty walk (with the raw ring dump)
 * means the fw never generated them at all.
 */
/*
 * (Re)create the completion EQ — the netdev open path retries with a
 * fresh comp EQ when its event engine lands silent (the T09-era
 * stochastic engagement: a recreate re-rolls the dice; the caller owns
 * the CQ lifetime around it, as DESTROY_EQ refuses EQs still bound by
 * a CQ).
 */
int mlx5_prism_comp_eq_recreate(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->comp_eq;
	u32 uar = dev->eq.uar;
	int err;

	if (eq->created) {
		if (!eq->storm_latched)
			disable_irq(dev->irq.vector[0]);
		eq->storm_latched = false;
		mlx5_prism_eq_destroy(dev, eq);
	}

	eq->uar = uar;
	eq->doorbell = dev->bar0 + (dma_addr_t)uar * MLX5_PRISM_PAGE_SIZE +
		       MLX5_PRISM_EQ_DOORBELL_OFFSET;
	err = mlx5_prism_eq_create(dev, eq, 0, 0);
	if (err)
		return err;
	enable_irq(dev->irq.vector[0]);
	mlx5_prism_eq_update_ci(dev, eq, true);
	pci_info(dev->pdev, "comp EQ up: eqn=%u entries=%u (vector 0, zero mask)\n",
		 eq->eqn, 1 << eq->log_sz);
	return 0;
}

int mlx5_prism_comp_eq_probe(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->comp_eq;
	int n;

	if (!eq->created)
		return 0;
	n = mlx5_prism_eq_poll(dev, eq, 100);
	pci_info(dev->pdev,
		 "comp eq probe: %d events this probe, %u total\n",
		 n, eq->events);
	return n;
}

void mlx5_prism_eq_teardown(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_eq *eq = &dev->eq;
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	int err;

	if (dev->comp_eq.created) {
		if (!dev->comp_eq.storm_latched)
			disable_irq(dev->irq.vector[0]);
		dev->comp_eq.storm_latched = false;
		mlx5_prism_eq_destroy(dev, &dev->comp_eq);
	}

	if (eq->created) {
		/*
		 * Stop event generation before tearing the EQ down. Skip the
		 * disable when the storm guard already did it (depth balance;
		 * the latch means no handler can be running).
		 */
		if (!eq->storm_latched)
			disable_irq(dev->irq.vector[MLX5_PRISM_EQ_VEC_CMD]);
		eq->storm_latched = false;
		mlx5_prism_eq_destroy(dev, eq);
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
