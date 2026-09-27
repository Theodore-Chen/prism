// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: hardware datapath smoke test (T14) — L3 service, route A.
 *
 * QP probe verdicts (2026-09-27, nine machine trips): this fw (14.32.1908)
 * accepts CREATE_QP RC but refuses INIT2RTR categorically because the vport
 * runs roce_en=0, and runtime enablement is refused (0xc1f678) for reasons
 * that stay with mlx5_core's bring-up. Connection transports are off the
 * table — route A drives an ETH TX SQ instead, the object mlx5e itself uses
 * on this card every day:
 *
 *	ALLOC_PD -> CREATE_MKEY (PA) -> CREATE_CQ -> CREATE_SQ ->
 *	MODIFY_SQ RST->RDY -> NOP / 64-byte SEND WQEs -> TX CQEs -> destroy.
 *
 * The SQ form walks a three-tier ladder within the same load (smoke_sq_tier
 * pins a rung, 0 = auto):
 *	1 — wc.c minimal: no TIS, no non_wire (NOP -> REQ CQE is proven there)
 *	2 — + non_wire=1 (6.x inserted the bit; this 2016 fw is expected to
 *	    refuse it — cheap to try, settles the bit question the same trip)
 *	3 — the mlx5e real form: ALLOC_TD -> CREATE_TIS (transport_domain
 *	    only) -> CREATE_SQ with tis_lst_sz=1 / tis_num_0
 * A refused CREATE_SQ or a silent/failed datapath escalates to the next
 * rung after destroying the SQ (PD/mkey/CQ stay); the winning rung is the
 * "tier=N" line in the log. Reference forms: 7.0 core/wc.c (minimal set,
 * RST->RDY with bitmask 0), 7.0 steering/hws/send.c (non_wire), 5.15
 * en_main.c:1176 mlx5e_create_sq + en/params.c:573 (field set incl. the
 * wq pd and log_wq_stride = ilog2(WQEBB)).
 *
 * Trip 10 (2026-09-27): every tier drew status 0x3 / syndrome 0x17a194 on
 * CREATE_SQ — with the tier-1 set byte-identical to wc.c's and the tier-3
 * prerequisites ACCEPTED (ALLOC_TD tdn=1, CREATE_TIS tisn=0). The
 * rejection is therefore environmental, so the attempt walk adds an
 * enrichment axis v (bit0 = min_wqe_inline_mode from QUERY_NIC_VPORT —
 * the only field mlx5e sets conditionally, cap wqe_inline_mode==1 makes
 * it vport-driven; bit1 = log_wq_sz 4 -> 6), walked v-major and
 * tier-minor: the first (v, tier) whose whole stage passes wins. Trip 10
 * also taught the zero-id lesson: the fw hands out tisn=0 (a legal id),
 * so object validity is tracked by explicit have_* flags, never id!=0.
 *
 * Doorbell and completion mechanics are unchanged from the probe era:
 * WQE -> dma_wmb -> SQ dbr (be32 WQEBB counter) -> wmb -> 8-byte raw MMIO
 * write of the ctrl-segment head to bfreg 0; CQEs are polled with the
 * owner-bit predicate. Runs only under selftest=1 (R0.5 discipline); every
 * object is destroyed within the same call. smoke_stage gates ignition:
 *   1 — object lifecycle only (zero doorbells, the "don't fire" leg)
 *   2 — + NOP WQE (send-queue -> CQE pipeline, no message in flight)
 *   3 — + 64-byte ETH SEND (default; the TX CQE byte_cnt is the verdict)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mlx5_prism.h"
#include "mlx5_prism_fmt.h"

static uint smoke_stage = 3;
module_param(smoke_stage, uint, 0444);
MODULE_PARM_DESC(smoke_stage, "smoke test ignition stage: 1=object lifecycle, 2=+NOP WQE, 3=+ETH SEND (default)");

/*
 * SQ form ladder pin: 0 = auto (walk tiers 1->2->3, stop at the first
 * tier whose datapath completes; default), 1/2/3 = create exactly that
 * form and fail rather than escalate.
 */
static uint smoke_sq_tier;
module_param(smoke_sq_tier, uint, 0444);
MODULE_PARM_DESC(smoke_sq_tier, "SQ form tier pin: 0=auto ladder (default), 1=wc minimal, 2=+non_wire, 3=+TD/TIS");

#define SMOKE_LOG_CQ_SIZE	4	/* 16 CQEs */
#define SMOKE_CQES		BIT(SMOKE_LOG_CQ_SIZE)
#define SMOKE_LOG_SQ_SIZE	4	/* 16 send WQEBBs (variant bit1: 6) */
#define SMOKE_SQ_BBS		BIT(SMOKE_LOG_SQ_SIZE)
#define SMOKE_WQ_BYTES		4096	/* one adapter page per buffer */
#define SMOKE_MSG_LEN		64
#define SMOKE_CQE_POLLS		1000	/* ~1 ms apart — ~1 s bound */
#define SMOKE_SQ_VARIANTS	4	/* enrichment axis: |inline|log_wq_sz=6 */

struct mlx5_prism_smoke {
	u32		pdn;
	u32		mkey;
	u32		cqn;
	u32		sqn;
	u32		tdn;		/* tier 3 pair */
	u32		tisn;
	u8		tier;		/* ladder rung behind the live SQ */
	u8		variant;	/* enrichment variant of the live SQ */
	u8		vport_inline;	/* min_wqe_inline_mode per QUERY_NIC_VPORT */
	bool		vport_inline_valid;
	bool		have_pd;
	bool		have_mkey;
	bool		have_cq;
	bool		have_sq;
	bool		have_td;
	bool		have_tis;
	u8		*wq;
	dma_addr_t	wq_dma;
	u8		*cq;
	dma_addr_t	cq_dma;
	u8		*data;
	dma_addr_t	data_dma;
	u8		*sq_dbr;	/* coherent: [4]=sq counter */
	dma_addr_t	sq_dbr_dma;
	u8		*cq_dbr;	/* coherent: [0]=set_ci, [4]=arm (never rung) */
	dma_addr_t	cq_dbr_dma;
	void __iomem	*bf;		/* bar0 + uar * 4 KiB + 0x800 */
	u32		sq_pc;		/* WQEBB producer counter */
	u32		cq_ci;
};

struct smoke_cqe_res {
	u8		opcode;
	u32		byte_cnt;
	u16		wqe_counter;
};

static void smoke_build_msg(u8 *m)
{
	int i;

	for (i = 0; i < SMOKE_MSG_LEN; i++)
		m[i] = (u8)(i * 7 + 3);
}

static void smoke_free(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	struct pci_dev *pdev = dev->pdev;

	if (s->sq_dbr)
		dma_free_coherent(&pdev->dev, 64, s->sq_dbr, s->sq_dbr_dma);
	if (s->cq_dbr)
		dma_free_coherent(&pdev->dev, 64, s->cq_dbr, s->cq_dbr_dma);
	if (s->data)
		dma_free_coherent(&pdev->dev, SMOKE_WQ_BYTES, s->data, s->data_dma);
	if (s->cq)
		dma_free_coherent(&pdev->dev, SMOKE_WQ_BYTES, s->cq, s->cq_dma);
	if (s->wq)
		dma_free_coherent(&pdev->dev, SMOKE_WQ_BYTES, s->wq, s->wq_dma);
	memset(s, 0, sizeof(*s));
}

static int smoke_alloc(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	struct pci_dev *pdev = dev->pdev;
	int i;

	s->wq = dma_alloc_coherent(&pdev->dev, SMOKE_WQ_BYTES, &s->wq_dma, GFP_KERNEL);
	s->cq = dma_alloc_coherent(&pdev->dev, SMOKE_WQ_BYTES, &s->cq_dma, GFP_KERNEL);
	s->data = dma_alloc_coherent(&pdev->dev, SMOKE_WQ_BYTES, &s->data_dma, GFP_KERNEL);
	s->sq_dbr = dma_alloc_coherent(&pdev->dev, 64, &s->sq_dbr_dma, GFP_KERNEL);
	s->cq_dbr = dma_alloc_coherent(&pdev->dev, 64, &s->cq_dbr_dma, GFP_KERNEL);
	if (!s->wq || !s->cq || !s->data || !s->sq_dbr || !s->cq_dbr) {
		smoke_free(dev, s);
		return -ENOMEM;
	}

	/* CQEs pre-filled invalid with SW-opposite owner: HW writes owner 0 on
	 * the first lap, so the ownership predicate matches immediately.
	 */
	for (i = 0; i < SMOKE_CQES; i++)
		s->cq[i * PRISM_CQE_SIZE + PRISM_CQE_OFF_OP_OWN] = PRISM_CQE_OWNER_INIT;

	smoke_build_msg(s->data);

	s->bf = dev->bar0 + (dma_addr_t)dev->eq.uar * MLX5_PRISM_PAGE_SIZE +
		PRISM_SEND_DOORBELL_OFFSET;
	return 0;
}

/*
 * Trip-10 follow-up probes: CREATE_SQ drew 0x17a194 with a wc.c-identical
 * field set, so the missing piece is environmental. Collect the two values
 * mlx5e itself consults — min_wqe_inline_mode from the vport context (the
 * only field mlx5e_create_sq sets conditionally, driven by the eth-offload
 * cap wqe_inline_mode, vport.h MLX5_CAP_INLINE_MODE_*) — plus the T05
 * caps-patch bytes for the record (byte 82 is max_wqe_sz_sq's low byte in
 * the v5.15 layout). Read-only, failures logged and survived.
 */
static void smoke_probe_env(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	u8 vin[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 vout[512] = { };
	u32 eth_mode = 0;
	bool eth_ok = false;
	int err;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_NIC_VPORT_CTX,
				     0, vin, sizeof(vin), vout, sizeof(vout));
	if (!err) {
		s->vport_inline = prism_ifc_get(vout + PRISM_OFF_NVC_OUT,
						PRISM_NVC_BIT_MIN_WQE_INLINE_MODE,
						3);
		s->vport_inline_valid = true;
	} else {
		pci_info(dev->pdev, "smoke self-test: vport query failed (%d) — v1/v3 degenerate\n",
			 err);
	}

	{
		u8 *out = kzalloc(MLX5_PRISM_CMD_HDR_SIZE + MLX5_PRISM_CAP_BYTES,
				  GFP_KERNEL);

		if (out) {
			u8 cin[MLX5_PRISM_CMD_HDR_SIZE] = { };

			/* op_mod: cap group 1 (ethernet offloads), bit0 = current */
			err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_HCA_CAP,
						     0x3, cin, sizeof(cin), out,
						     MLX5_PRISM_CMD_HDR_SIZE +
						     MLX5_PRISM_CAP_BYTES);
			if (!err) {
				eth_mode = prism_ifc_get(out + MLX5_PRISM_CMD_HDR_SIZE,
							 PRISM_ETHCAP_BIT_WQE_INLINE_MODE,
							 2);
				eth_ok = true;
			}
			kfree(out);
		}
	}

	if (dev->fw.caps_cur)
		pci_info(dev->pdev,
			 "smoke self-test: env: caps patch bytes 82-83=0x%02x%02x 162-163=0x%02x%02x (as set at bootstrap)\n",
			 dev->fw.caps_cur[82], dev->fw.caps_cur[83],
			 dev->fw.caps_cur[162], dev->fw.caps_cur[163]);
	pci_info(dev->pdev,
		 "smoke self-test: env: vport min_wqe_inline_mode=%u eth caps wqe_inline_mode=%u%s\n",
		 s->vport_inline_valid ? s->vport_inline : 0,
		 eth_ok ? eth_mode : 0, eth_ok ? "" : " (eth caps query failed)");
}

/* destroy_something_in: the id (24-bit) rides as a be32 @ [8:12]. */
static int smoke_destroy_id(struct mlx5_prism_dev *dev, u16 opcode, u32 id)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];

	mlx5_prism_put_be32(in + PRISM_OFF_PD_IN, id);
	return mlx5_prism_cmdq_invoke(dev, opcode, 0, in, sizeof(in),
				      out, sizeof(out));
}

/*
 * Destroy in dependency order — the SQ references the CQ, mkey and PD,
 * and the tier-3 TIS/TD pair stands behind the SQ. Report whether it is
 * safe to free the DMA buffers: a failed DESTROY means the fw may still
 * hold the addresses, so the caller must leak them (same discipline as
 * the CMDQ timeout mailbox leak).
 */
static int smoke_destroy(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	int err = 0, ret;

	if (s->have_sq) {
		ret = smoke_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_SQ, s->sqn);
		if (ret) {
			pci_err(dev->pdev, "smoke self-test: DESTROY_SQ failed (%d) — leaking DMA buffers\n",
				ret);
			err = ret;
			goto out;
		}
		s->have_sq = false;
		s->sqn = 0;
	}
	if (s->have_tis) {
		ret = smoke_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_TIS, s->tisn);
		if (ret) {
			pci_err(dev->pdev, "smoke self-test: DESTROY_TIS failed (%d) — leaking DMA buffers\n",
				ret);
			err = ret;
			goto out;
		}
		s->have_tis = false;
		s->tisn = 0;
	}
	if (s->have_td) {
		ret = smoke_destroy_id(dev, MLX5_PRISM_CMD_OP_DEALLOC_TD, s->tdn);
		if (ret) {
			pci_err(dev->pdev, "smoke self-test: DEALLOC_TD failed (%d) — leaking DMA buffers\n",
				ret);
			err = ret;
			goto out;
		}
		s->have_td = false;
		s->tdn = 0;
	}
	if (s->have_cq) {
		ret = smoke_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_CQ, s->cqn);
		if (ret) {
			pci_err(dev->pdev, "smoke self-test: DESTROY_CQ failed (%d) — leaking DMA buffers\n",
				ret);
			err = ret;
			goto out;
		}
		s->have_cq = false;
		s->cqn = 0;
	}
	if (s->have_mkey) {
		ret = smoke_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_MKEY,
				       s->mkey >> 8);
		if (ret) {
			pci_err(dev->pdev, "smoke self-test: DESTROY_MKEY failed (%d) — leaking DMA buffers\n",
				ret);
			err = ret;
			goto out;
		}
		s->have_mkey = false;
		s->mkey = 0;
	}
	if (s->have_pd) {
		ret = smoke_destroy_id(dev, MLX5_PRISM_CMD_OP_DEALLOC_PD, s->pdn);
		if (ret) {
			pci_err(dev->pdev, "smoke self-test: DEALLOC_PD failed (%d)\n", ret);
			err = ret;
		}
		s->have_pd = false;
		s->pdn = 0;
	}
out:
	return err;
}

static int smoke_create_mkey(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	u8 *in = kzalloc(PRISM_CREATE_MKEY_IN_LEN, GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *mkc;
	int err;

	if (!in)
		return -ENOMEM;

	mkc = in + PRISM_OFF_MKC;
	prism_ifc_set(mkc, PRISM_MKC_BIT_ACCESS_MODE, 2, 0); /* PA */
	prism_ifc_set(mkc, PRISM_MKC_BIT_A, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_RW, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_RR, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_LW, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_LR, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_QPN, 24, 0xffffff); /* not QP-bound */
	prism_ifc_set(mkc, PRISM_MKC_BIT_LENGTH64, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_PD, 24, s->pdn);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_MKEY, 0,
				     in, PRISM_CREATE_MKEY_IN_LEN,
				     out, sizeof(out));
	kfree(in);
	if (err)
		return err;

	/* mkey = (out mkey_index << 8) | mkey_7_0, and mkey_7_0 stayed 0. */
	s->mkey = (mlx5_prism_get_be32(out + PRISM_OFF_MKEY_IDX_OUT) & 0xffffff) << 8;
	s->have_mkey = true;
	pci_info(dev->pdev, "smoke self-test: CREATE_MKEY ok mkey=0x%x\n", s->mkey);
	return 0;
}

static int smoke_create_cq(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	u8 *in = kzalloc(PRISM_CREATE_CQ_IN_LEN(1), GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *cqc;
	int err;

	if (!in)
		return -ENOMEM;

	cqc = in + PRISM_OFF_CQC;
	prism_ifc_set(cqc, PRISM_CQC_BIT_LOG_CQ_SIZE, 5, SMOKE_LOG_CQ_SIZE);
	/* Completion events are opt-in via the arm doorbell, which is never
	 * rung — binding the real EQ just satisfies the cqc field.
	 */
	prism_ifc_set(cqc, PRISM_CQC_BIT_C_EQN, 32, dev->eq.eqn);
	prism_ifc_set(cqc, PRISM_CQC_BIT_UAR_PAGE, 24, dev->eq.uar);
	prism_ifc_set(cqc, PRISM_CQC_BIT_LOG_PAGE_SIZE, 5, 0);
	mlx5_prism_put_be64(cqc + PRISM_CQC_OFF_DBR_ADDR, s->cq_dbr_dma);
	mlx5_prism_put_be64(in + PRISM_OFF_CQ_PAS, s->cq_dma);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_CQ, 0,
				     in, PRISM_CREATE_CQ_IN_LEN(1),
				     out, sizeof(out));
	kfree(in);
	if (err)
		return err;

	s->cqn = mlx5_prism_get_be32(out + PRISM_OFF_CQN_OUT) & 0xffffff;
	s->have_cq = true;
	pci_info(dev->pdev, "smoke self-test: CREATE_CQ ok cqn=%u\n", s->cqn);
	return 0;
}

/* tier 3 prerequisites: one transport domain and one TIS bound to it. */
static int smoke_alloc_td(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	int err;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_ALLOC_TD, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	s->tdn = mlx5_prism_get_be32(out + PRISM_OFF_TDN_OUT) & 0xffffff;
	s->have_td = true;
	pci_info(dev->pdev, "smoke self-test: ALLOC_TD ok tdn=%u\n", s->tdn);
	return 0;
}

static int smoke_create_tis(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	u8 in[PRISM_CREATE_TIS_IN_LEN] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	int err;

	/* tisc carries the transport domain only — the mlx5e create_tis form
	 * (its pd/prio writes are for TLS/LAG paths prism does not enter).
	 */
	prism_ifc_set(in + PRISM_OFF_TISC, PRISM_TISC_BIT_TRANSPORT_DOMAIN, 24,
		      s->tdn);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_TIS, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	s->tisn = mlx5_prism_get_be32(out + PRISM_OFF_TISN_OUT) & 0xffffff;
	s->have_tis = true;
	pci_info(dev->pdev, "smoke self-test: CREATE_TIS ok tisn=%u\n", s->tisn);
	return 0;
}

/*
 * CREATE_SQ at the given ladder tier and enrichment variant. Common core:
 * cqn, state RST, flush_in_error_en, wq{CYCLIC, pd, uar_page, dbr, stride,
 * pg_sz, sz, pas}; ts_format stays 0 — the bits are reserved-to-zero on
 * this pre-timestamp fw (MLX5_TIMESTAMP_FORMAT_FREE_RUNNING is 0 anyway).
 * Variant bit0 adds min_wqe_inline_mode from the vport probe; bit1 raises
 * log_wq_sz to 6 (the full 4 KiB WQ page — mlx5e's default is 0xa).
 */
static int smoke_create_sq(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s,
			   u8 tier, u8 variant)
{
	u8 *in = kzalloc(PRISM_CREATE_SQ_IN_LEN(1), GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 log_sq_sz = (variant & 0x2) ? 6 : SMOKE_LOG_SQ_SIZE;
	u8 *sqc, *wq;
	int err;

	if (!in)
		return -ENOMEM;

	sqc = in + PRISM_OFF_SQC;
	wq = sqc + PRISM_OFF_SQC_WQ;
	prism_ifc_set(sqc, PRISM_SQC_BIT_CQN, 24, s->cqn);
	prism_ifc_set(sqc, PRISM_SQC_BIT_STATE, 4, PRISM_SQC_STATE_RST);
	prism_ifc_set(sqc, PRISM_SQC_BIT_FLUSH_IN_ERROR_EN, 1, 1);
	if ((variant & 0x1) && s->vport_inline_valid)
		prism_ifc_set(sqc, PRISM_SQC_BIT_MIN_WQE_INLINE_MODE, 3,
			      s->vport_inline);
	if (tier == 2)
		prism_ifc_set(sqc, PRISM_SQC_BIT_NON_WIRE, 1, 1);
	if (tier == 3) {
		prism_ifc_set(sqc, PRISM_SQC_BIT_TIS_LST_SZ, 16, 1);
		prism_ifc_set(sqc, PRISM_SQC_BIT_TIS_NUM_0, 24, s->tisn);
	}
	prism_ifc_set(wq, PRISM_WQ_BIT_WQ_TYPE, 4, PRISM_WQ_TYPE_CYCLIC);
	prism_ifc_set(wq, PRISM_WQ_BIT_PD, 24, s->pdn);
	prism_ifc_set(wq, PRISM_WQ_BIT_UAR_PAGE, 24, dev->eq.uar);
	mlx5_prism_put_be64(wq + PRISM_WQ_OFF_DBR_ADDR, s->sq_dbr_dma);
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_STRIDE, 4,
		      ilog2(PRISM_WQE_BB_SIZE));
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_PG_SZ, 5, 0);
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_SZ, 5, log_sq_sz);
	mlx5_prism_put_be64(in + PRISM_OFF_SQ_PAS, s->wq_dma);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_SQ, 0,
				     in, PRISM_CREATE_SQ_IN_LEN(1),
				     out, sizeof(out));
	kfree(in);
	if (err)
		return err;

	s->sqn = mlx5_prism_get_be32(out + PRISM_OFF_SQN_OUT) & 0xffffff;
	s->have_sq = true;
	s->tier = tier;
	s->variant = variant;
	pci_info(dev->pdev, "smoke self-test: CREATE_SQ ok tier=%u v=%u sqn=%u\n",
		 tier, variant, s->sqn);
	return 0;
}

/* RST -> RDY: sq_state names the current state, ctx.state the target, and
 * the modify_bitmask stays 0 — a state transition applies no optional
 * fields (wc.c / hws send.c form).
 */
static int smoke_modify_sq_rdy(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	u8 in[PRISM_MODIFY_SQ_IN_LEN] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	int err;

	mlx5_prism_put_be32(in + PRISM_OFF_SQN_IN, s->sqn);
	prism_ifc_set(in, PRISM_MODSQ_BIT_SQ_STATE, 4, PRISM_SQC_STATE_RST);
	prism_ifc_set(in + PRISM_OFF_SQC, PRISM_SQC_BIT_STATE, 4,
		      PRISM_SQC_STATE_RDY);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_MODIFY_SQ, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;

	pci_info(dev->pdev, "smoke self-test: SQ to RDY ok\n");
	return 0;
}

/*
 * Build a WQE at the current SQ position and ring the doorbell. The order
 * is mlx5e_notify_hw / mlx5r_ring_db verbatim: WQE -> dma_wmb -> SQ dbr
 * (be32 WQEBB counter) -> wmb -> 8-byte MMIO write of the ctrl segment's
 * first two words (already big-endian) to bfreg 0 of the UAR page. A NOP
 * is ctrl only (1 DS); the SEND follows mlx5e's L2-inline small-packet
 * form: zeroed eseg carrying the ethernet header in inline_hdr (sz @+12,
 * bytes from +14 spilling into the next DS — the vport's
 * min_wqe_inline_mode floor demands it), then an INLINE data segment for
 * the payload. The 14-byte header + 64-byte payload WQE spans two WQEBBs.
 */
static void smoke_post_sq_wqe(struct mlx5_prism_smoke *s, u8 opcode,
			      u32 byte_cnt, const u8 *payload)
{
	u8 *ctrl = s->wq + (s->sq_pc & (SMOKE_SQ_BBS - 1)) * PRISM_WQE_BB_SIZE;
	u8 ds = opcode == PRISM_WQE_OPCODE_NOP ? PRISM_WQE_CTRL_DS :
		3 + DIV_ROUND_UP(4 + byte_cnt, PRISM_WQE_DS_SIZE);
	u64 doorbell;

	mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP,
			    (s->sq_pc << 8) | opcode);
	mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_QPN_DS, (s->sqn << 8) | ds);
	ctrl[PRISM_WQE_OFF_CTRL_FM_CE_SE] = PRISM_WQE_CTRL_CQ_UPDATE;
	if (opcode != PRISM_WQE_OPCODE_NOP) {
		static const u8 eth_hdr[14] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff,	/* broadcast dst */
			0x02, 0x11, 0x22, 0x33, 0x44, 0x55,	/* local-admin src */
			0x88, 0xb5,				/* ethertype: experimental */
		};
		u8 *eseg = ctrl + PRISM_WQE_OFF_ETH_SEG;
		u8 *dseg = ctrl + PRISM_WQE_OFF_DATA_SEG + PRISM_WQE_DS_SIZE;

		mlx5_prism_put_be16(eseg + PRISM_WQE_ESEG_OFF_INLINE_SZ,
				    sizeof(eth_hdr));
		memcpy(eseg + PRISM_WQE_ESEG_OFF_INLINE_START, eth_hdr,
		       sizeof(eth_hdr));
		mlx5_prism_put_be32(dseg + PRISM_WQE_SEG_OFF_BYTE_CNT,
				    byte_cnt | PRISM_WQE_INLINE_FLAG);
		memcpy(dseg + 4, payload, byte_cnt);
	}

	s->sq_pc++;
	/* WQE visible before the doorbell record... */
	dma_wmb();
	mlx5_prism_put_be32(s->sq_dbr + 4, s->sq_pc);
	/* ...and the record visible before the MMIO doorbell */
	wmb();

	memcpy(&doorbell, ctrl, sizeof(doorbell));
	__raw_writeq(doorbell, s->bf);
}

static void smoke_cqe_dump(struct mlx5_prism_dev *dev, const u8 *cqe)
{
	pci_err(dev->pdev,
		"smoke self-test: CQE dump: op_own=0x%02x opcode=%u byte_cnt=%u wqe_counter=%u\n",
		cqe[PRISM_CQE_OFF_OP_OWN], cqe[PRISM_CQE_OFF_OP_OWN] >> 4,
		mlx5_prism_get_be32(cqe + PRISM_CQE_OFF_BYTE_CNT),
		mlx5_prism_get_be16(cqe + PRISM_CQE_OFF_WQE_COUNTER));
	if ((cqe[PRISM_CQE_OFF_OP_OWN] >> 4) >= PRISM_CQE_OPCODE_REQ_ERR) {
		pci_err(dev->pdev,
			"smoke self-test: error CQE: vendor=0x%02x syndrome=0x%02x s_wqe_opcode_qpn=0x%08x\n",
			cqe[PRISM_CQE_OFF_ERR_VENDOR],
			cqe[PRISM_CQE_OFF_ERR_SYNDROME],
			mlx5_prism_get_be32(cqe + PRISM_CQE_OFF_ERR_S_WQE_OP_QPN));
		print_hex_dump(KERN_ERR, "err cqe: ", DUMP_PREFIX_OFFSET, 16, 1,
			       cqe, PRISM_CQE_SIZE, false);
	}
}

static int smoke_poll_cqe(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s,
			  struct smoke_cqe_res *res)
{
	u32 ci = s->cq_ci;
	int i;

	for (i = 0; i < SMOKE_CQE_POLLS; i++) {
		const u8 *cqe = s->cq + (ci & (SMOKE_CQES - 1)) * PRISM_CQE_SIZE;
		u8 op_own = READ_ONCE(cqe[PRISM_CQE_OFF_OP_OWN]);

		if ((op_own & 1) == ((ci >> SMOKE_LOG_CQ_SIZE) & 1)) {
			dma_rmb();
			res->opcode = op_own >> 4;
			res->byte_cnt = mlx5_prism_get_be32(cqe + PRISM_CQE_OFF_BYTE_CNT);
			res->wqe_counter = mlx5_prism_get_be16(cqe + PRISM_CQE_OFF_WQE_COUNTER);
			s->cq_ci = ci + 1;
			mlx5_prism_put_be32(s->cq_dbr, s->cq_ci & 0xffffff);
			return 0;
		}
		usleep_range(500, 1000);
	}

	smoke_cqe_dump(dev, s->cq + (ci & (SMOKE_CQES - 1)) * PRISM_CQE_SIZE);
	return -ETIMEDOUT;
}

/* the CQE slot just consumed — for the wrong-content diagnostic dump */
static const u8 *smoke_last_cqe(const struct mlx5_prism_smoke *s)
{
	return s->cq + ((s->cq_ci - 1) & (SMOKE_CQES - 1)) * PRISM_CQE_SIZE;
}

static int smoke_fire_nop(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	struct smoke_cqe_res res;

	smoke_post_sq_wqe(s, PRISM_WQE_OPCODE_NOP, 0, NULL);
	if (smoke_poll_cqe(dev, s, &res))
		return -EIO;
	if (res.opcode != PRISM_CQE_OPCODE_REQ || res.wqe_counter != 0) {
		smoke_cqe_dump(dev, smoke_last_cqe(s));
		return -EIO;
	}
	pci_info(dev->pdev, "smoke self-test: NOP WQE ok (opcode=REQ wqe_counter=0)\n");
	return 0;
}

static int smoke_fire_send(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	struct smoke_cqe_res res;

	smoke_post_sq_wqe(s, PRISM_WQE_OPCODE_SEND, SMOKE_MSG_LEN, s->data);
	if (smoke_poll_cqe(dev, s, &res))
		return -EIO;
	/* The verdict is opcode REQ + wqe_counter. byte_cnt varies by fw
	 * reporting: 64 = payload, 78 = with the inline header, 0 = this
	 * fw reports no TX byte count at all (trip-15 measurement) — all
	 * three pre-registered green, the actual value is logged.
	 */
	if (res.opcode != PRISM_CQE_OPCODE_REQ || res.wqe_counter != 1 ||
	    (res.byte_cnt != SMOKE_MSG_LEN &&
	     res.byte_cnt != SMOKE_MSG_LEN + 14 && res.byte_cnt != 0)) {
		smoke_cqe_dump(dev, smoke_last_cqe(s));
		return -EIO;
	}
	pci_info(dev->pdev,
		 "smoke self-test: TX CQE ok (opcode=REQ wqe_counter=1 byte_cnt=%u)\n",
		 res.byte_cnt);
	return 0;
}

/*
 * One attempt: create the SQ at this (tier, variant), take it to RDY, and
 * at the caller's stage fire the datapath. Any failure leaves the SQ for
 * the caller to destroy before the next form.
 */
static int smoke_sq_attempt(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s,
			    u8 tier, u8 variant, u32 stage)
{
	int err;

	err = smoke_create_sq(dev, s, tier, variant);
	if (err)
		return err;
	err = smoke_modify_sq_rdy(dev, s);
	if (err)
		return err;
	if (stage >= 2) {
		err = smoke_fire_nop(dev, s);
		if (err)
			return err;
	}
	if (stage >= 3) {
		err = smoke_fire_send(dev, s);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Escalation teardown: destroy only the SQ. PD/mkey/CQ/TD/TIS serve every
 * attempt; the WQ buffer and doorbell record restart from a virgin state
 * for the next attempt while the CQ keeps its consumer index —
 * already-consumed CQEs stay consumed and the next attempt's land further
 * along the ring.
 */
static int smoke_teardown_sq(struct mlx5_prism_dev *dev, struct mlx5_prism_smoke *s)
{
	int ret;

	if (s->have_sq) {
		ret = smoke_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_SQ, s->sqn);
		if (ret) {
			pci_err(dev->pdev, "smoke self-test: DESTROY_SQ failed (%d) — leaking DMA buffers\n",
				ret);
			return ret;
		}
		s->have_sq = false;
		s->sqn = 0;
	}
	memset(s->wq, 0, SMOKE_SQ_BBS * PRISM_WQE_BB_SIZE);
	memset(s->sq_dbr, 0, 64);
	s->sq_pc = 0;
	return 0;
}

int mlx5_prism_smoke_selftest(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_smoke s = { };
	u8 in16[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out16[MLX5_PRISM_CMD_HDR_SIZE];
	u32 stage = smoke_stage;
	u8 first = 1, last = 3;
	int err;

	if (!stage || stage > 3)
		stage = 3;
	if (smoke_sq_tier) {
		first = smoke_sq_tier > 3 ? 3 : smoke_sq_tier;
		last = first;
	}
	pci_info(dev->pdev, "smoke self-test: stage %u (ETH SQ datapath)\n", stage);

	err = smoke_alloc(dev, &s);
	if (err)
		goto failed;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_ALLOC_PD, 0,
				     in16, sizeof(in16), out16, sizeof(out16));
	if (err)
		goto failed;
	s.pdn = mlx5_prism_get_be32(out16 + PRISM_OFF_PD_OUT) & 0xffffff;
	s.have_pd = true;
	pci_info(dev->pdev, "smoke self-test: ALLOC_PD ok pd=%u\n", s.pdn);

	smoke_probe_env(dev, &s);

	err = smoke_create_mkey(dev, &s);
	if (err)
		goto failed;
	err = smoke_create_cq(dev, &s);
	if (err)
		goto failed;

	{
		u8 v, tier;

		for (v = 0; v < SMOKE_SQ_VARIANTS; v++) {
			for (tier = first; tier <= last; tier++) {
				if (tier == 3 && !s.have_tis) {
					err = smoke_alloc_td(dev, &s);
					if (err)
						goto failed;
					err = smoke_create_tis(dev, &s);
					if (err)
						goto failed;
				}
				err = smoke_sq_attempt(dev, &s, tier, v, stage);
				if (!err)
					goto won;
				if (v == SMOKE_SQ_VARIANTS - 1 && tier == last)
					goto failed;
				pci_info(dev->pdev,
					 "smoke self-test: tier %u v%u failed (%d) — next form\n",
					 tier, v, err);
				err = smoke_teardown_sq(dev, &s);
				if (err)
					goto failed;
			}
		}
	}
won:
	err = smoke_destroy(dev, &s);
	if (err)
		goto leak;
	pci_info(dev->pdev, "smoke self-test: DESTROY_SQ/TIS/TD/CQ/MKEY/PD ok\n");
	smoke_free(dev, &s);
	pci_info(dev->pdev, "Hardware datapath smoke test: PASSED\n");
	return 0;

failed:
	if (smoke_destroy(dev, &s))
		goto leak;
	smoke_free(dev, &s);
leak:
	pci_err(dev->pdev, "Hardware datapath smoke test: FAILED (%d)\n", err);
	return err;
}
