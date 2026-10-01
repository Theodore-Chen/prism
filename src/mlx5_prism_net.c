// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: minimal netdev skeleton (T15) — register_netdev +
 * ndo_start_xmit + NAPI, one queue pair per PF.
 *
 * The datapath reuses the T14 winner form verbatim (DESIGN.md v2.4 §5.7):
 * ALLOC_PD -> CREATE_MKEY(PA) -> CREATE_CQ -> ALLOC_TD -> CREATE_TIS ->
 * CREATE_SQ(tis_lst_sz=1) -> MODIFY_SQ RST->RDY, plus the receive leg the
 * smoke test never built: CREATE_RQ -> MODIFY_RQ RST->RDY -> CREATE_TIR
 * (direct dispatch, inline_rqn) -> CREATE_FLOW_TABLE -> CREATE_FLOW_GROUP
 * (catch-all) -> SET_FTE (fwd to TIR) -> SET_FLOW_TABLE_ROOT. RQ WQEs are
 * a pure dseg array posted by writing the doorbell record only (no UAR
 * MMIO — en_rx.c:597 / fpga/conn.c:126); TX WQEs are the v5.15 skb form,
 * header inline in the eth segment and the linear payload behind a
 * pointer dseg under the global PA mkey (en_tx.c:509/292), with the T14
 * all-inline form kept behind netdev_inline_all=1 as the fallback.
 *
 * One CQ is shared by the SQ and RQ; CQEs dispatch by opcode (REQ -> TX
 * reclaim, RESP_SEND -> RX). CQ completion events arrive as EQE type
 * 0x00 on the existing command EQ and schedule the NAPI instance from
 * hardirq (mlx5_eq_comp_int -> napi_schedule shape); the CQ is armed on
 * napi_complete (cq.h mlx5_cq_arm: sn<<28 | 0<<24 | ci to dbr+4, then
 * write64{value, cqn} to UAR+0x20). netdev_stage grades ignition like
 * the smoke test's smoke_stage: 1 = objects only, 2 = +root/arm, 3 =
 * +xmit (default).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mlx5_prism.h"
#include "mlx5_prism_fmt.h"
#include "mlx5_prism_trace.h"

static uint netdev_stage = 3;
module_param(netdev_stage, uint, 0644);
MODULE_PARM_DESC(netdev_stage,
		 "netdev bring-up stage: 1=objects only, 2=+RX armed/root, 3=+xmit (default)");

static uint netdev_arm_ms = 10;
module_param(netdev_arm_ms, uint, 0644);
MODULE_PARM_DESC(netdev_arm_ms,
		 "keep-fresh CQ re-arm tick in ms (0 disables; the fw drops arms with a stale sequence number)");

static uint netdev_inline_all;
module_param(netdev_inline_all, uint, 0644);
MODULE_PARM_DESC(netdev_inline_all,
		 "force the T14 all-inline TX WQE form (1) instead of hdr-inline + pointer dseg (0, default)");

/* Ring geometry: 16 KiB send WQ (256 WQEBBs, 4 pages), 64-entry shared CQ,
 * 16-entry RQ of 2 KiB buffers. A 1500-byte frame is ~25 WQEBBs inline or
 * ~5 with a pointer dseg; stop_room covers the worst form plus a full page
 * of NOP padding.
 */
#define NET_LOG_SQ_SZ		8
#define NET_SQ_BBS		BIT(NET_LOG_SQ_SZ)
#define NET_SQ_BYTES		(NET_SQ_BBS * PRISM_WQE_BB_SIZE)
#define NET_BBS_PER_PAGE	(MLX5_PRISM_PAGE_SIZE / PRISM_WQE_BB_SIZE)
#define NET_SQ_STOP_ROOM	(NET_BBS_PER_PAGE + 32)
#define NET_LOG_CQ_SZ		6
#define NET_CQES		BIT(NET_LOG_CQ_SZ)
#define NET_CQ_BYTES		(NET_CQES * PRISM_CQE_SIZE)
#define NET_LOG_RQ_SZ		4
#define NET_RQ_WQES		BIT(NET_LOG_RQ_SZ)
#define NET_RX_BUF_SZ		2048
#define NET_RX_BYTES		(NET_RQ_WQES * NET_RX_BUF_SZ)
#define NET_CQE_OPCODE_RESP_SEND	0x2
#define NET_CQE_OPCODE_RESP_ERR	0xe

/* ---- command builders ---- */

/* destroy_something_in shape: the 24-bit id rides as a be32 @ [8:12]. */
static int net_destroy_id(struct mlx5_prism_dev *dev, u16 opcode, u32 id)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];

	mlx5_prism_put_be32(in + PRISM_OFF_PD_IN, id);
	return mlx5_prism_cmdq_invoke(dev, opcode, 0, in, sizeof(in),
				      out, sizeof(out));
}

static int net_flow_destroy(struct mlx5_prism_dev *dev, u16 opcode,
			    u32 table_id, u32 group_id)
{
	u8 in[64] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];

	prism_ifc_set(in, PRISM_FTCMD_BIT_TABLE_TYPE, 8,
		      PRISM_FT_TYPE_NIC_RX);
	prism_ifc_set(in, PRISM_FTCMD_BIT_TABLE_ID, 24, table_id);
	if (opcode == MLX5_PRISM_CMD_OP_DESTROY_FLOW_GROUP)
		mlx5_prism_put_be32(in + PRISM_OFF_GROUP_ID_IN, group_id);
	return mlx5_prism_cmdq_invoke(dev, opcode, 0, in, sizeof(in),
				      out, sizeof(out));
}

static int net_alloc_pd(struct mlx5_prism_net *net)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	int err;

	err = mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_ALLOC_PD,
				     0, in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	net->pdn = mlx5_prism_get_be32(out + PRISM_OFF_PD_OUT) & 0xffffff;
	net->have_pd = true;
	return 0;
}

static int net_create_mkey(struct mlx5_prism_net *net)
{
	u8 *in = kzalloc(PRISM_CREATE_MKEY_IN_LEN, GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *mkc;
	int err;

	if (!in)
		return -ENOMEM;

	/* The mlx5e hw-obj mkey form (en_common.c:61-68): PA access, lw/lr,
	 * length64, unbound qpn — no length field; TX pointer dsegs and RX
	 * buffers share it.
	 */
	mkc = in + PRISM_OFF_MKC;
	prism_ifc_set(mkc, PRISM_MKC_BIT_ACCESS_MODE, 2, 0);
	prism_ifc_set(mkc, PRISM_MKC_BIT_LW, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_LR, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_LENGTH64, 1, 1);
	prism_ifc_set(mkc, PRISM_MKC_BIT_QPN, 24, 0xffffff);
	prism_ifc_set(mkc, PRISM_MKC_BIT_PD, 24, net->pdn);

	err = mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_CREATE_MKEY,
				     0, in, PRISM_CREATE_MKEY_IN_LEN,
				     out, sizeof(out));
	kfree(in);
	if (err)
		return err;
	net->mkey = (mlx5_prism_get_be32(out + PRISM_OFF_MKEY_IDX_OUT) &
		     0xffffff) << 8;
	net->have_mkey = true;
	return 0;
}

static int net_create_cq(struct mlx5_prism_net *net)
{
	u8 *in = kzalloc(PRISM_CREATE_CQ_IN_LEN(1), GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *cqc;
	int err;

	if (!in)
		return -ENOMEM;

	cqc = in + PRISM_OFF_CQC;
	prism_ifc_set(cqc, PRISM_CQC_BIT_LOG_CQ_SIZE, 5, NET_LOG_CQ_SZ);
	/* Completion events ride the dedicated comp EQ (vector 0, zero
	 * mask); uar_page backs the arm doorbell at +0x20.
	 */
	prism_ifc_set(cqc, PRISM_CQC_BIT_C_EQN, 32, net->dev->comp_eq.eqn);
	prism_ifc_set(cqc, PRISM_CQC_BIT_UAR_PAGE, 24, net->dev->eq.uar);
	prism_ifc_set(cqc, PRISM_CQC_BIT_LOG_PAGE_SIZE, 5, 0);
	mlx5_prism_put_be64(cqc + PRISM_CQC_OFF_DBR_ADDR, net->cq_dbr_dma);
	mlx5_prism_put_be64(in + PRISM_OFF_CQ_PAS, net->cq_dma);

	err = mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_CREATE_CQ, 0,
				     in, PRISM_CREATE_CQ_IN_LEN(1),
				     out, sizeof(out));
	kfree(in);
	if (err)
		return err;
	net->cqn = mlx5_prism_get_be32(out + PRISM_OFF_CQN_OUT) & 0xffffff;
	net->have_cq = true;
	return 0;
}

static int net_alloc_td_tis(struct mlx5_prism_net *net)
{
	u8 in[PRISM_CREATE_TIS_IN_LEN] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	struct mlx5_prism_dev *dev = net->dev;
	int err;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_ALLOC_TD, 0,
				     in, MLX5_PRISM_CMD_HDR_SIZE,
				     out, sizeof(out));
	if (err)
		return err;
	net->tdn = mlx5_prism_get_be32(out + PRISM_OFF_TDN_OUT) & 0xffffff;
	net->have_td = true;

	/* tisc carries the transport domain only (T14 tier-3 form). */
	prism_ifc_set(in + PRISM_OFF_TISC, PRISM_TISC_BIT_TRANSPORT_DOMAIN, 24,
		      net->tdn);
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_TIS, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	net->tisn = mlx5_prism_get_be32(out + PRISM_OFF_TISN_OUT) & 0xffffff;
	net->have_tis = true;
	return 0;
}

static int net_create_sq(struct mlx5_prism_net *net)
{
	u8 *in = kzalloc(PRISM_CREATE_SQ_IN_LEN(4), GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *sqc, *wq;
	int err, i;

	if (!in)
		return -ENOMEM;

	/* T14 winner: tier-3 field set, ts_format 0, no min_wqe_inline_mode
	 * (v0 variant). The WQ spans 4 pages.
	 */
	sqc = in + PRISM_OFF_SQC;
	wq = sqc + PRISM_OFF_SQC_WQ;
	prism_ifc_set(sqc, PRISM_SQC_BIT_CQN, 24, net->cqn);
	prism_ifc_set(sqc, PRISM_SQC_BIT_STATE, 4, PRISM_SQC_STATE_RST);
	prism_ifc_set(sqc, PRISM_SQC_BIT_FLUSH_IN_ERROR_EN, 1, 1);
	prism_ifc_set(sqc, PRISM_SQC_BIT_TIS_LST_SZ, 16, 1);
	prism_ifc_set(sqc, PRISM_SQC_BIT_TIS_NUM_0, 24, net->tisn);
	prism_ifc_set(wq, PRISM_WQ_BIT_WQ_TYPE, 4, PRISM_WQ_TYPE_CYCLIC);
	prism_ifc_set(wq, PRISM_WQ_BIT_PD, 24, net->pdn);
	prism_ifc_set(wq, PRISM_WQ_BIT_UAR_PAGE, 24, net->dev->eq.uar);
	mlx5_prism_put_be64(wq + PRISM_WQ_OFF_DBR_ADDR, net->sq_dbr_dma);
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_STRIDE, 4,
		      ilog2(PRISM_WQE_BB_SIZE));
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_PG_SZ, 5, 0);
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_SZ, 5, NET_LOG_SQ_SZ);
	for (i = 0; i < 4; i++)
		mlx5_prism_put_be64(in + PRISM_OFF_SQ_PAS + 8 * i,
				    net->wq_dma + i * MLX5_PRISM_PAGE_SIZE);

	err = mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_CREATE_SQ, 0,
				     in, PRISM_CREATE_SQ_IN_LEN(4),
				     out, sizeof(out));
	kfree(in);
	if (err)
		return err;
	net->sqn = mlx5_prism_get_be32(out + PRISM_OFF_SQN_OUT) & 0xffffff;
	net->have_sq = true;
	return 0;
}

static int net_modify_sq_rdy(struct mlx5_prism_net *net)
{
	u8 in[PRISM_MODIFY_SQ_IN_LEN] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];

	mlx5_prism_put_be32(in + PRISM_OFF_SQN_IN, net->sqn);
	prism_ifc_set(in, PRISM_MODSQ_BIT_SQ_STATE, 4, PRISM_SQC_STATE_RST);
	prism_ifc_set(in + PRISM_OFF_SQC, PRISM_SQC_BIT_STATE, 4,
		      PRISM_SQC_STATE_RDY);
	return mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_MODIFY_SQ, 0,
				      in, sizeof(in), out, sizeof(out));
}

static int net_create_rq(struct mlx5_prism_net *net)
{
	u8 *in = kzalloc(PRISM_CREATE_RQ_IN_LEN(1), GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *rqc, *wq;
	int err;

	if (!in)
		return -ENOMEM;

	rqc = in + PRISM_OFF_RQC;
	wq = rqc + PRISM_OFF_RQC_WQ;
	prism_ifc_set(rqc, PRISM_RQC_BIT_VSD, 1, 1); /* keep vlan on the frame */
	prism_ifc_set(rqc, PRISM_RQC_BIT_STATE, 4, PRISM_RQC_STATE_RST);
	prism_ifc_set(rqc, PRISM_RQC_BIT_CQN, 24, net->cqn);
	prism_ifc_set(wq, PRISM_WQ_BIT_WQ_TYPE, 4, PRISM_WQ_TYPE_CYCLIC);
	prism_ifc_set(wq, PRISM_WQ_BIT_PD, 24, net->pdn);
	mlx5_prism_put_be64(wq + PRISM_WQ_OFF_DBR_ADDR, net->rq_dbr_dma);
	/* one 16-byte dseg per WQE (en/params.c:431-444, ndsegs = 1); RQ wqs
	 * carry no uar_page — posting never rings the UAR
	 */
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_STRIDE, 4, 4);
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_PG_SZ, 5, 0);
	prism_ifc_set(wq, PRISM_WQ_BIT_LOG_WQ_SZ, 5, NET_LOG_RQ_SZ);
	mlx5_prism_put_be64(in + PRISM_OFF_RQ_PAS, net->rxwq_dma);

	err = mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_CREATE_RQ, 0,
				     in, PRISM_CREATE_RQ_IN_LEN(1),
				     out, sizeof(out));
	kfree(in);
	if (err)
		return err;
	net->rqn = mlx5_prism_get_be32(out + PRISM_OFF_RQN_OUT) & 0xffffff;
	net->have_rq = true;
	return 0;
}

static int net_modify_rq_rdy(struct mlx5_prism_net *net)
{
	u8 in[PRISM_MODIFY_RQ_IN_LEN] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];

	/* modify_rq_in shares the modify_sq_in top-level geometry: current
	 * state at bit 0x40, id at [8:12], ctx at byte 32; the bitmask stays
	 * zero (en_main.c:694-695 shape).
	 */
	mlx5_prism_put_be32(in + PRISM_OFF_RQN_IN, net->rqn);
	prism_ifc_set(in, PRISM_MODSQ_BIT_SQ_STATE, 4, PRISM_RQC_STATE_RST);
	prism_ifc_set(in + PRISM_OFF_RQC, PRISM_RQC_BIT_STATE, 4,
		      PRISM_RQC_STATE_RDY);
	return mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_MODIFY_RQ, 0,
				      in, sizeof(in), out, sizeof(out));
}

static int net_create_tir(struct mlx5_prism_net *net)
{
	u8 in[PRISM_CREATE_TIR_IN_LEN] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u8 *tirc;
	int err;

	tirc = in + PRISM_OFF_TIRC;
	prism_ifc_set(tirc, PRISM_TIRC_BIT_DISP_TYPE, 4,
		      PRISM_TIRC_DISP_TYPE_DIRECT);
	prism_ifc_set(tirc, PRISM_TIRC_BIT_INLINE_RQN, 24, net->rqn);
	prism_ifc_set(tirc, PRISM_TIRC_BIT_RX_HASH_FN, 4, 0);
	prism_ifc_set(tirc, PRISM_TIRC_BIT_TRANSPORT_DOMAIN, 24, net->tdn);

	err = mlx5_prism_cmdq_invoke(net->dev, MLX5_PRISM_CMD_OP_CREATE_TIR, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	net->tirn = mlx5_prism_get_be32(out + PRISM_OFF_TIRN_OUT) & 0xffffff;
	net->have_tir = true;
	return 0;
}

static int net_create_flow(struct mlx5_prism_net *net)
{
	u8 *ftin = kzalloc(PRISM_CREATE_FT_IN_LEN, GFP_KERNEL);
	u8 *fgin = kzalloc(PRISM_CREATE_FG_IN_LEN, GFP_KERNEL);
	u8 *fein = kzalloc(PRISM_SET_FTE_IN_LEN(1), GFP_KERNEL);
	u8 out[MLX5_PRISM_CMD_HDR_SIZE] = { };
	struct mlx5_prism_dev *dev = net->dev;
	u8 *ftc, *fc, *dest;
	int err = -ENOMEM;

	if (!ftin || !fgin || !fein)
		goto out;

	/* level-0 table, one entry; the single catch-all rule forwards to the
	 * TIR so nothing can miss.
	 */
	prism_ifc_set(ftin, PRISM_FTCMD_BIT_TABLE_TYPE, 8,
		      PRISM_FT_TYPE_NIC_RX);
	ftc = ftin + 24;
	prism_ifc_set(ftc, PRISM_FTC_BIT_LEVEL, 8, 0);
	prism_ifc_set(ftc, PRISM_FTC_BIT_LOG_SIZE, 8, 0);
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_FLOW_TABLE,
				     0, ftin, PRISM_CREATE_FT_IN_LEN,
				     out, sizeof(out));
	if (err)
		goto out;
	net->table_id = mlx5_prism_get_be32(out + PRISM_OFF_TABLE_ID_OUT) &
			0xffffff;
	net->have_table = true;

	prism_ifc_set(fgin, PRISM_FTCMD_BIT_TABLE_TYPE, 8,
		      PRISM_FT_TYPE_NIC_RX);
	prism_ifc_set(fgin, PRISM_FTCMD_BIT_TABLE_ID, 24, net->table_id);
	prism_ifc_set(fgin, PRISM_FG_BIT_START, 32, 0);
	prism_ifc_set(fgin, PRISM_FG_BIT_END, 32, 0);
	/* match_criteria_enable 0 = match nothing = catch-all (en_fs.c:334
	 * trap-rule shape); match_criteria stays all zero.
	 */
	prism_ifc_set(fgin, PRISM_FG_BIT_MATCH_EN, 8, 0);
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_CREATE_FLOW_GROUP,
				     0, fgin, PRISM_CREATE_FG_IN_LEN,
				     out, sizeof(out));
	if (err)
		goto out;
	net->group_id = mlx5_prism_get_be32(out + PRISM_OFF_TABLE_ID_OUT) &
			0xffffff;
	net->have_group = true;

	prism_ifc_set(fein, PRISM_FTCMD_BIT_TABLE_TYPE, 8,
		      PRISM_FT_TYPE_NIC_RX);
	prism_ifc_set(fein, PRISM_FTCMD_BIT_TABLE_ID, 24, net->table_id);
	prism_ifc_set(fein, PRISM_FTE_BIT_FLOW_INDEX, 32, 0);
	fc = fein + 64;
	prism_ifc_set(fc, PRISM_FC_BIT_GROUP_ID, 32, net->group_id);
	prism_ifc_set(fc, PRISM_FC_BIT_ACTION, 16, PRISM_FC_ACTION_FWD_DEST);
	prism_ifc_set(fc, PRISM_FC_BIT_DEST_LIST_SZ, 24, 1);
	dest = fc + PRISM_FC_OFF_DEST;
	prism_ifc_set(dest, PRISM_DEST_BIT_TYPE, 8, PRISM_DEST_TYPE_TIR);
	prism_ifc_set(dest, PRISM_DEST_BIT_ID, 24, net->tirn);
	err = mlx5_prism_cmdq_invoke(dev,
				     MLX5_PRISM_CMD_OP_SET_FLOW_TABLE_ENTRY,
				     0, fein, PRISM_SET_FTE_IN_LEN(1),
				     out, sizeof(out));
	if (err)
		goto out;
	net->have_fte = true;

out:
	kfree(fein);
	kfree(fgin);
	kfree(ftin);
	return err;
}

static int net_set_root(struct mlx5_prism_net *net, bool connect)
{
	u8 in[PRISM_SET_ROOT_IN_LEN] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];

	prism_ifc_set(in, PRISM_FTCMD_BIT_TABLE_TYPE, 8, PRISM_FT_TYPE_NIC_RX);
	if (connect)
		prism_ifc_set(in, PRISM_FTCMD_BIT_TABLE_ID, 24, net->table_id);
	else
		prism_ifc_set(in, PRISM_FTCMD_BIT_OP_MOD, 16, 1);
	return mlx5_prism_cmdq_invoke(net->dev,
				      MLX5_PRISM_CMD_OP_SET_FLOW_TABLE_ROOT,
				      0, in, sizeof(in), out, sizeof(out));
}

/* Fill every RQ WQE with its buffer and publish the producer index —
 * the whole posting protocol for RQs is this dbr write (dma_wmb first).
 */
static void net_rq_post_all(struct mlx5_prism_net *net)
{
	u8 *dseg;
	int i;

	for (i = 0; i < NET_RQ_WQES; i++) {
		dseg = net->rxwq + i * PRISM_WQE_DS_SIZE;
		mlx5_prism_put_be32(dseg + PRISM_WQE_SEG_OFF_BYTE_CNT,
				    NET_RX_BUF_SZ);
		mlx5_prism_put_be32(dseg + PRISM_WQE_SEG_OFF_LKEY, net->mkey);
		mlx5_prism_put_be64(dseg + PRISM_WQE_SEG_OFF_ADDR,
				    net->rxbuf_dma + i * NET_RX_BUF_SZ);
	}
	net->rq_pc = NET_RQ_WQES;
	dma_wmb();
	mlx5_prism_put_be32(net->rq_dbr, net->rq_pc);
}

/*
 * Teardown, strictly reverse of creation. A failed DESTROY means the fw
 * may still hold the buffer addresses, so the DMA allocations leak (the
 * T14 discipline) and the caller keeps the error for the log.
 */
static int prism_net_destroy_objects(struct mlx5_prism_net *net)
{
	struct mlx5_prism_dev *dev = net->dev;
	int err = 0, ret;

	if (net->root_set) {
		/* best effort — refuse to chain-break on a disconnect that
		 * the fw may judge redundant
		 */
		net_set_root(net, false);
		net->root_set = false;
	}
	if (net->have_fte) {
		u8 in[PRISM_DELETE_FTE_IN_LEN] = { };
		u8 out[MLX5_PRISM_CMD_HDR_SIZE];

		/* the fw refuses DESTROY_FLOW_GROUP with 0x9 (BAD_RES_STATE)
		 * while the group still holds a live entry — DELETE_FTE
		 * (0x938) first, fs_cmd.c mlx5_cmd_delete_fte form
		 */
		prism_ifc_set(in, PRISM_FTCMD_BIT_TABLE_TYPE, 8,
			      PRISM_FT_TYPE_NIC_RX);
		prism_ifc_set(in, PRISM_FTCMD_BIT_TABLE_ID, 24, net->table_id);
		prism_ifc_set(in, PRISM_FTE_BIT_FLOW_INDEX, 32, 0);
		ret = mlx5_prism_cmdq_invoke(dev,
					     MLX5_PRISM_CMD_OP_DELETE_FLOW_TABLE_ENTRY,
					     0, in, sizeof(in),
					     out, sizeof(out));
		if (ret) {
			netdev_err(net->netdev,
				   "DELETE_FTE failed (%d)\n", ret);
			err = ret;
			goto out;
		}
		net->have_fte = false;
	}
	if (net->have_group) {
		ret = net_flow_destroy(dev, MLX5_PRISM_CMD_OP_DESTROY_FLOW_GROUP,
				       net->table_id, net->group_id);
		if (ret) {
			netdev_err(net->netdev,
				   "DESTROY_FLOW_GROUP failed (%d)\n", ret);
			err = ret;
			goto out;
		}
		net->have_group = false;
		net->group_id = 0;
	}
	if (net->have_table) {
		ret = net_flow_destroy(dev, MLX5_PRISM_CMD_OP_DESTROY_FLOW_TABLE,
				       net->table_id, 0);
		if (ret) {
			netdev_err(net->netdev,
				   "DESTROY_FLOW_TABLE failed (%d)\n", ret);
			err = ret;
			goto out;
		}
		net->have_table = false;
		net->table_id = 0;
	}
	if (net->have_tir) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_TIR,
				     net->tirn);
		if (ret) {
			err = ret;
			goto out;
		}
		net->have_tir = false;
		net->tirn = 0;
	}
	if (net->have_rq) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_RQ,
				     net->rqn);
		if (ret) {
			err = ret;
			goto out;
		}
		net->have_rq = false;
		net->rqn = 0;
	}
	if (net->have_sq) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_SQ,
				     net->sqn);
		if (ret) {
			err = ret;
			goto out;
		}
		net->have_sq = false;
		net->sqn = 0;
	}
	if (net->have_tis) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_TIS,
				     net->tisn);
		if (ret) {
			err = ret;
			goto out;
		}
		net->have_tis = false;
		net->tisn = 0;
	}
	if (net->have_td) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DEALLOC_TD,
				     net->tdn);
		if (ret) {
			err = ret;
			goto out;
		}
		net->have_td = false;
		net->tdn = 0;
	}
	if (net->have_cq) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_CQ,
				     net->cqn);
		if (ret) {
			err = ret;
			goto out;
		}
		net->have_cq = false;
		net->cqn = 0;
	}
	if (net->have_mkey) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DESTROY_MKEY,
				     net->mkey >> 8);
		if (ret) {
			err = ret;
			goto out;
		}
		net->have_mkey = false;
		net->mkey = 0;
	}
	if (net->have_pd) {
		ret = net_destroy_id(dev, MLX5_PRISM_CMD_OP_DEALLOC_PD,
				     net->pdn);
		if (ret)
			err = ret;
		net->have_pd = false;
		net->pdn = 0;
	}
out:
	return err;
}

static void prism_net_free_dma(struct mlx5_prism_net *net)
{
	struct device *dma = &net->dev->pdev->dev;

	kfree(net->txwi);
	net->txwi = NULL;
	if (net->rq_dbr)
		dma_free_coherent(dma, 64, net->rq_dbr, net->rq_dbr_dma);
	if (net->cq_dbr)
		dma_free_coherent(dma, 64, net->cq_dbr, net->cq_dbr_dma);
	if (net->sq_dbr)
		dma_free_coherent(dma, 64, net->sq_dbr, net->sq_dbr_dma);
	if (net->rxbuf)
		dma_free_coherent(dma, NET_RX_BYTES, net->rxbuf, net->rxbuf_dma);
	if (net->rxwq)
		dma_free_coherent(dma, MLX5_PRISM_PAGE_SIZE, net->rxwq,
				  net->rxwq_dma);
	if (net->cq)
		dma_free_coherent(dma, NET_CQ_BYTES, net->cq, net->cq_dma);
	if (net->wq)
		dma_free_coherent(dma, NET_SQ_BYTES, net->wq, net->wq_dma);
	net->rq_dbr = NULL;
	net->cq_dbr = NULL;
	net->sq_dbr = NULL;
	net->rxbuf = NULL;
	net->rxwq = NULL;
	net->cq = NULL;
	net->wq = NULL;
}

/* Free everything still pending on the TX ring (close path — en_tx.c
 * mlx5e_free_txqsq_descs shape): unmap + free skbs from cc to pc.
 */
static void net_tx_flush(struct mlx5_prism_net *net)
{
	while (net->sq_cc != net->sq_pc) {
		struct mlx5_prism_net_txwi *wi =
			&net->txwi[net->sq_cc & (NET_SQ_BBS - 1)];

		net->sq_cc += wi->num_wqebbs;
		if (wi->skb) {
			if (wi->dma_addr)
				dma_unmap_single(&net->dev->pdev->dev,
						 wi->dma_addr, wi->dma_len,
						 DMA_TO_DEVICE);
			dev_kfree_skb_any(wi->skb);
		}
		memset(wi, 0, sizeof(*wi));
	}
}

/* ---- doorbells ---- */

static void net_sq_notify(struct mlx5_prism_net *net, u8 *ctrl)
{
	u64 doorbell;

	/* WQE visible before the doorbell record... (mlx5e_notify_hw) */
	dma_wmb();
	mlx5_prism_put_be32(net->sq_dbr + 4, net->sq_pc);
	/* ...and the record visible before the MMIO doorbell */
	wmb();
	memcpy(&doorbell, ctrl, sizeof(doorbell));
	__raw_writeq(doorbell, net->bf);
}

static void net_cq_set_ci(struct mlx5_prism_net *net)
{
	mlx5_prism_put_be32(net->cq_dbr, net->cq_ci & 0xffffff);
}

/* The per-CQ arm sync (trip-12 model): the fw tracks an expected sn
 * starting at 0 per CQ — a sn==expected, all-zero arm ADVANCES the
 * expectation without requesting notification; a sn==expected nonzero
 * arm arms the CQ; anything else is dropped as stale. A fresh CQ
 * therefore needs this zero sync first, then the real sn=1 arm.
 */
static void net_cq_arm_sync(struct mlx5_prism_net *net)
{
	u32 be_cqn = mlx5_prism_get_be32((u8 *)&net->cqn);
	u64 mmio;

	mlx5_prism_put_be32(net->cq_dbr + PRISM_CQ_DBR_ARM_OFF, 0);
	/* sync record visible before the MMIO doorbell */
	wmb();
	mmio = (u64)be_cqn << 32;
	__raw_writeq(mmio, net->cq_arm);
}

static void prism_net_arm_work(struct work_struct *w);

static void net_cq_arm(struct mlx5_prism_net *net)
{
	u32 val, be_val, be_cqn;
	u64 mmio;

	spin_lock(&net->arm_lock);

	/*
	 * Trip-19 model (closes the 19-trip chain): the fw's expected arm
	 * sequence number cycles mod 4 INCLUDING zero — 0,1,2,3,0,1... The
	 * trip-11 "skip zero" fix conflated sn=0 with the all-zero VALUE:
	 * sn=0 with a nonzero ci/cmd is a legal notifying arm; only the
	 * fully-zero doorbell is special (first arm on a fresh CQ — the
	 * sync write covers it). Skipping 0 desynchronised the counters at
	 * the first wrap, after which every arm was dropped as stale —
	 * explaining the partial-then-total failures of trips 11-18.
	 */
	net->arm_sn = (net->arm_sn + 1) & 3;
	val = ((u32)net->arm_sn << 28) | (net->cq_ci & 0xffffff);
	be_val = mlx5_prism_get_be32((u8 *)&val);
	be_cqn = mlx5_prism_get_be32((u8 *)&net->cqn);

	mlx5_prism_put_be32(net->cq_dbr + PRISM_CQ_DBR_ARM_OFF, val);
	/* arm record visible before the MMIO doorbell (mlx5_cq_arm) */
	wmb();
	/* mlx5_write64 word order: doorbell[0] = the arm VALUE at +0x20,
	 * doorbell[1] = cqn at +0x24 — on LE the u64's LOW word lands at
	 * the lower address, so the value must sit in the low half. (The
	 * trip-5 verdict: both halves swapped made every arm malformed —
	 * zero EQEs, five traffic judgments failing on a healthy datapath.)
	 */
	mmio = ((u64)be_cqn << 32) | be_val;
	__raw_writeq(mmio, net->cq_arm);
	spin_unlock(&net->arm_lock);
}

/* ---- NAPI poll: one shared CQ, opcode dispatch ---- */

static void net_cqe_tx(struct mlx5_prism_net *net, const u8 *cqe, bool error)
{
	struct net_device *ndev = net->netdev;
	u16 wqe_counter = mlx5_prism_get_be16(cqe + PRISM_CQE_OFF_WQE_COUNTER);
	u32 sqcc = net->sq_cc;

	/* en_tx.c:846-855 — walk wqe_info from cc to the CQE's WQE (NOPs and
	 * any skipped CQEs in between land here too).
	 */
	do {
		bool last = (sqcc & 0xffff) == wqe_counter;
		struct mlx5_prism_net_txwi *wi =
			&net->txwi[sqcc & (NET_SQ_BBS - 1)];

		if (!wi->num_wqebbs) {
			/* accounting desync — bail out rather than spin */
			netdev_err(ndev, "tx reclaim desync at bb %u\n", sqcc);
			break;
		}
		sqcc += wi->num_wqebbs;
		if (wi->skb) {
			net->tx_packets++;
			net->tx_bytes += wi->num_bytes;
			if (wi->dma_addr) {
				dma_unmap_single(&net->dev->pdev->dev,
						 wi->dma_addr, wi->dma_len,
						 DMA_TO_DEVICE);
				wi->dma_addr = 0;
			}
			if (error)
				dev_kfree_skb_any(wi->skb);
			else
				napi_consume_skb(wi->skb, NAPI_POLL_WEIGHT);
			wi->skb = NULL;
		}
		if (last)
			break;
	} while (1);
	net->sq_cc = sqcc;
	if (error) {
		net->tx_errors++;
		netdev_err(ndev, "TX error CQE: syndrome=0x%02x wqe_counter=%u\n",
			   cqe[PRISM_CQE_OFF_ERR_SYNDROME], wqe_counter);
	}
}

static void net_cqe_rx(struct mlx5_prism_net *net, const u8 *cqe)
{
	struct net_device *ndev = net->netdev;
	u16 wqe_id = mlx5_prism_get_be16(cqe + PRISM_CQE_OFF_WQE_COUNTER) &
		     (NET_RQ_WQES - 1);
	u32 byte_cnt = mlx5_prism_get_be32(cqe + PRISM_CQE_OFF_BYTE_CNT);
	struct sk_buff *skb;

	trace_prism_rx_cqe(byte_cnt, wqe_id);

	if (byte_cnt > NET_RX_BUF_SZ) {
		net->rx_errors++;
	} else {
		skb = napi_alloc_skb(&net->napi, byte_cnt);
		if (skb) {
			skb_put_data(skb, net->rxbuf + wqe_id * NET_RX_BUF_SZ,
				     byte_cnt);
			skb->protocol = eth_type_trans(skb, ndev);
			skb->ip_summed = CHECKSUM_NONE;
			napi_gro_receive(&net->napi, skb);
			net->rx_packets++;
			net->rx_bytes += byte_cnt;
		} else {
			net->rx_errors++;
		}
	}
	/* repost: the dseg is already in place (same buffer address), so
	 * publishing a new producer index hands the WQE back
	 */
	net->rq_pc++;
	dma_wmb();
	mlx5_prism_put_be32(net->rq_dbr, net->rq_pc);
}

static int prism_napi_poll(struct napi_struct *napi, int budget)
{
	struct mlx5_prism_net *net =
		container_of(napi, struct mlx5_prism_net, napi);
	int work = 0;

	for (;;) {
		const u8 *cqe = net->cq + (net->cq_ci & (NET_CQES - 1)) *
				PRISM_CQE_SIZE;
		u8 op_own = READ_ONCE(cqe[PRISM_CQE_OFF_OP_OWN]);
		u8 opcode;

		/*
		 * T14 smoke_poll_cqe semantics: owner bit EQUAL to the lap
		 * parity means FRESH — consume. (Trip-16 verdict: this was
		 * ported inverted from the EQ consumer's XOR shape, so the
		 * poll ran forever consuming nothing — cc/cq_ci stuck at 0
		 * while events and CQEs both flowed.)
		 */
		if ((op_own & 1) != ((net->cq_ci >> NET_LOG_CQ_SZ) & 1))
			break;		/* stale — ring drained */
		dma_rmb();

		opcode = op_own >> 4;
		if (opcode == NET_CQE_OPCODE_RESP_SEND) {
			net_cqe_rx(net, cqe);
			if (++work >= budget)
				break;
		} else if (opcode == PRISM_CQE_OPCODE_REQ ||
			   opcode == PRISM_CQE_OPCODE_REQ_ERR) {
			net_cqe_tx(net, cqe,
				   opcode == PRISM_CQE_OPCODE_REQ_ERR);
		} else if (opcode == NET_CQE_OPCODE_RESP_ERR) {
			/* an RX error CQE still consumed a posted WQE */
			net->rx_errors++;
			net->rq_pc++;
			dma_wmb();
			mlx5_prism_put_be32(net->rq_dbr, net->rq_pc);
		} else {
			netdev_dbg(net->netdev,
				   "unexpected CQE opcode %u ignored\n", opcode);
		}
		net->cq_ci++;
	}

	/* publish consumption before arming / leaving (CQ overrun rule) */
	net_cq_set_ci(net);
	/* consumption published before the NAPI state change */
	wmb();

	dev_info_ratelimited(&net->netdev->dev,
			     "napi poll: ci=%u work=%d pc=%u cc=%u\n",
			     net->cq_ci, work, net->sq_pc, net->sq_cc);

	if (work < budget && napi_complete_done(napi, work))
		net_cq_arm(net);

	if (netif_queue_stopped(net->netdev)) {
		u32 used = (net->sq_pc - net->sq_cc) & (NET_SQ_BBS - 1);

		if (NET_SQ_BBS - used >= NET_SQ_STOP_ROOM)
			netif_tx_wake_queue(netdev_get_tx_queue(net->netdev, 0));
	}
	return work;
}

/* ---- ndo_start_xmit ---- */

static netdev_tx_t prism_ndo_start_xmit(struct sk_buff *skb,
					struct net_device *ndev)
{
	struct mlx5_prism_net *net = mlx5_prism_net_of(ndev);
	u32 used = (net->sq_pc - net->sq_cc) & (NET_SQ_BBS - 1);
	u32 len, ihs, payload, ds, num_bbs, pc;
	struct mlx5_prism_net_txwi *wi;
	u8 *ctrl, *eseg, *dseg;
	bool all_inline;
	dma_addr_t dma = 0;

	if (!net->opened || !net->xmit_on) {
		net->tx_errors++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (skb_linearize(skb)) {
		net->tx_errors++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	len = skb->len;
	/* L2-inline floor (vport min_wqe_inline_mode=1): at least the eth
	 * header rides in eseg.inline_hdr (en_tx.c:166-171 form, T14 proved
	 * 14 bytes suffice).
	 */
	ihs = min_t(u32, skb_network_offset(skb), len);
	ihs = max_t(u32, ihs, ETH_HLEN);
	ihs = min_t(u32, ihs, len);
	payload = len - ihs;

	all_inline = net->tx_inline;
	ds = 2 + DIV_ROUND_UP(max_t(int, (int)ihs - 2, 0), PRISM_WQE_DS_SIZE) +
	     (all_inline ? DIV_ROUND_UP(4 + payload, PRISM_WQE_DS_SIZE) : 1);
	num_bbs = DIV_ROUND_UP(ds, 4);
	if (num_bbs > 32) {
		/* oversize for the ring's stop_room budget */
		net->tx_errors++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (NET_SQ_BBS - used < num_bbs + NET_BBS_PER_PAGE) {
		/* leave stop_room; the queue is stopped below and the poll
		 * wakes it once the CQEs land
		 */
		net->tx_errors++;
		dev_kfree_skb_any(skb);
		goto stop_maybe;
	}

	pc = net->sq_pc;
	/* NOP-pad so the WQE never crosses a 4-KiB page boundary (en_tx.c
	 * get_next_pi: WQEs are page-contained; a NOP is 1 ctrl-only BB) —
	 * and so it never wraps the ring either, keeping the linear build
	 * below inside the buffer.
	 */
	while (((pc & (NET_BBS_PER_PAGE - 1)) + num_bbs) > NET_BBS_PER_PAGE ||
	       ((pc & (NET_SQ_BBS - 1)) + num_bbs) > NET_SQ_BBS) {
		ctrl = net->wq + (pc & (NET_SQ_BBS - 1)) * PRISM_WQE_BB_SIZE;
		mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP,
				    (pc << 8) | PRISM_WQE_OPCODE_NOP);
		mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_QPN_DS,
				    (net->sqn << 8) | PRISM_WQE_CTRL_DS);
		net->txwi[pc & (NET_SQ_BBS - 1)] = (struct mlx5_prism_net_txwi){ };
		net->txwi[pc & (NET_SQ_BBS - 1)].num_wqebbs = 1;
		pc++;
	}

	ctrl = net->wq + (pc & (NET_SQ_BBS - 1)) * PRISM_WQE_BB_SIZE;
	memset(ctrl, 0, num_bbs * PRISM_WQE_BB_SIZE);
	eseg = ctrl + PRISM_WQE_OFF_ETH_SEG;
	mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP,
			    (pc << 8) | PRISM_WQE_OPCODE_SEND);
	mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_QPN_DS,
			    (net->sqn << 8) | ds);
	ctrl[PRISM_WQE_OFF_CTRL_FM_CE_SE] = PRISM_WQE_CTRL_CQ_UPDATE;

	/* inline header: 2 bytes live inside eseg, the rest spills into the
	 * DS units that follow the 2-DS head (mlx5e attr.ihs geometry)
	 */
	mlx5_prism_put_be16(eseg + PRISM_WQE_ESEG_OFF_INLINE_SZ, ihs);
	memcpy(eseg + PRISM_WQE_ESEG_OFF_INLINE_START, skb->data,
	       min_t(u32, ihs, 2));
	if (ihs > 2)
		memcpy(ctrl + PRISM_WQE_OFF_DATA_SEG, skb->data + 2, ihs - 2);

	dseg = ctrl + PRISM_WQE_OFF_DATA_SEG +
	       DIV_ROUND_UP(max_t(int, (int)ihs - 2, 0), PRISM_WQE_DS_SIZE) *
	       PRISM_WQE_DS_SIZE;
	if (all_inline) {
		mlx5_prism_put_be32(dseg + PRISM_WQE_SEG_OFF_BYTE_CNT,
				    payload | PRISM_WQE_INLINE_FLAG);
		memcpy(dseg + 4, skb->data + ihs, payload);
	} else {
		dma = dma_map_single(&net->dev->pdev->dev, skb->data + ihs,
				     payload, DMA_TO_DEVICE);
		if (dma_mapping_error(&net->dev->pdev->dev, dma)) {
			net->tx_errors++;
			dev_kfree_skb_any(skb);
			goto stop_maybe;
		}
		mlx5_prism_put_be32(dseg + PRISM_WQE_SEG_OFF_BYTE_CNT, payload);
		mlx5_prism_put_be32(dseg + PRISM_WQE_SEG_OFF_LKEY, net->mkey);
		mlx5_prism_put_be64(dseg + PRISM_WQE_SEG_OFF_ADDR, dma);
	}

	wi = &net->txwi[pc & (NET_SQ_BBS - 1)];
	wi->skb = skb;
	wi->dma_addr = dma;
	wi->dma_len = payload;
	wi->num_bytes = len;
	wi->num_wqebbs = num_bbs;
	net->sq_pc = pc + num_bbs;

	/*
	 * Trip-17 verdict: completions generate events only when a fresh
	 * arm precedes them by microseconds (verify/probe NOP+SEND all
	 * event; traffic completions ~100ms after the last arm never do,
	 * across 4 trips, both WQE opcodes, both EQ types). Arm here, at
	 * post time, so every TX completion rides a fresh arm; the poll
	 * re-arms on its way out, and request/reply pairs keep the RX
	 * side inside the same window.
	 */
	net_cq_arm(net);
	net_sq_notify(net, ctrl);
	trace_prism_sq_xmit(len, ihs, all_inline, pc);

stop_maybe:
	used = (net->sq_pc - net->sq_cc) & (NET_SQ_BBS - 1);
	if (NET_SQ_BBS - used < NET_SQ_STOP_ROOM)
		netif_tx_stop_queue(netdev_get_tx_queue(ndev, 0));
	return NETDEV_TX_OK;
}

/*
 * Trip-21 verdict closed the arm story: an arrival on an idle CQ never
 * raises an event on this fw — no matter how fresh the tick's re-arm is.
 * Every consumption ever observed rode an arm->doorbell->us-completion
 * triple (the xmit path) or piggybacked on a poll such an event woke;
 * prism1's whole traffic window (11 TX completions plus RX CQEs, all
 * fresh in the ring at close) was missed while its verify-NOP and
 * close-probe arms evented deterministically. Freshness alone does not
 * arm; only completion-right-after-arm notifies. The tick therefore
 * schedules the poll itself as a watchdog — consumption latency is
 * bounded by netdev_arm_ms, not by the event engine. sn races between
 * the arm writers stay confined by arm_lock.
 */
static void prism_net_arm_work(struct work_struct *w)
{
	struct mlx5_prism_net *net =
		container_of(w, struct mlx5_prism_net, arm_dw.work);

	if (!READ_ONCE(net->opened))
		return;
	net_cq_arm(net);
	if (net->napi_enabled)
		napi_schedule(&net->napi);
	if (netdev_arm_ms)
		queue_delayed_work(system_wq, &net->arm_dw,
				   msecs_to_jiffies(netdev_arm_ms));
}

/* PPCNT 802.3 counters are wire-level truth, independent of our steering.
 * They are also clear-on-read on this fw (trip-21: stress-cycle reads came
 * back zero after each lifecycle read drained them) — the open-time read
 * zeroes the window, so the close-time read is the wire delta of exactly
 * this open: rx_ok>0 with an empty RQ = a steering miss; rx_ok=0 = the
 * frames never crossed the wire.
 */
static void prism_net_wire_counters(struct mlx5_prism_net *net,
				    const char *when)
{
	u8 ppcnt[PRISM_PPCNT_REG_SIZE] = { };

	prism_ppcnt_set_grp(ppcnt, PRISM_PPCNT_GRP_IEEE_802_3);
	if (!mlx5_prism_access_reg(net->dev, MLX5_PRISM_REG_PPCNT, false,
				   0, ppcnt, ppcnt, PRISM_PPCNT_REG_SIZE))
		netdev_info(net->netdev,
			    "%s: wire counters tx_ok=%llu rx_ok=%llu\n", when,
			    prism_ppcnt_ctr64(ppcnt, PRISM_OFF_PPCNT0_TX_OK),
			    prism_ppcnt_ctr64(ppcnt, PRISM_OFF_PPCNT0_RX_OK));
}

/* ---- ndo open/stop ---- */

/* Reset the software/DMA ring state between open attempts (fresh
 * attempt = fresh CQ, so the rings restart virgin too).
 */
static void net_reset_rings(struct mlx5_prism_net *net)
{
	int i;

	memset(net->wq, 0, NET_SQ_BYTES);
	memset(net->txwi, 0, NET_SQ_BBS * sizeof(*net->txwi));
	memset(net->sq_dbr, 0, 64);
	memset(net->cq_dbr, 0, 64);
	memset(net->rq_dbr, 0, 64);
	for (i = 0; i < NET_CQES; i++)
		net->cq[i * PRISM_CQE_SIZE + PRISM_CQE_OFF_OP_OWN] =
			PRISM_CQE_OWNER_INIT;
	net->sq_pc = 0;
	net->sq_cc = 0;
	net->cq_ci = 0;
	net->rq_pc = 0;
}

/* Post one NOP WQE (CQ_UPDATE) and ring — the verify probe's signal.
 * Returns 0 when posted, -ENOSPC when the next BB sits on a wrap edge.
 */
static int net_fire_probe_nop(struct mlx5_prism_net *net)
{
	u32 pc = net->sq_pc;

	if ((pc & (NET_BBS_PER_PAGE - 1)) == NET_BBS_PER_PAGE - 1 ||
	    (pc & (NET_SQ_BBS - 1)) == NET_SQ_BBS - 1)
		return -ENOSPC;

	{
		u8 *ctrl = net->wq + (pc & (NET_SQ_BBS - 1)) *
				      PRISM_WQE_BB_SIZE;

		mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP,
				    (pc << 8) | PRISM_WQE_OPCODE_NOP);
		mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_QPN_DS,
				    (net->sqn << 8) | PRISM_WQE_CTRL_DS);
		ctrl[PRISM_WQE_OFF_CTRL_FM_CE_SE] = PRISM_WQE_CTRL_CQ_UPDATE;
		net->txwi[pc & (NET_SQ_BBS - 1)].num_wqebbs = 1;
		net->sq_pc = pc + 1;
		net_sq_notify(net, ctrl);
	}
	return 0;
}

static int prism_ndo_open(struct net_device *ndev)
{
	struct mlx5_prism_net *net = mlx5_prism_net_of(ndev);
	struct mlx5_prism_dev *dev = net->dev;
	struct pci_dev *pdev = dev->pdev;
	u8 paos[PRISM_PAOS_REG_SIZE];
	u32 stage = netdev_stage ?: 3;
	int attempt;
	int err;

	if (!dev->eq.event_mode || !dev->eq.created) {
		netdev_err(ndev,
			   "datapath needs the EQ in event mode (eq_mode=poll forbids it)\n");
		return -EOPNOTSUPP;
	}
	if (stage > 3)
		stage = 3;

	net->wq = dma_alloc_coherent(&pdev->dev, NET_SQ_BYTES,
				     &net->wq_dma, GFP_KERNEL);
	net->cq = dma_alloc_coherent(&pdev->dev, NET_CQ_BYTES,
				     &net->cq_dma, GFP_KERNEL);
	net->rxwq = dma_alloc_coherent(&pdev->dev, MLX5_PRISM_PAGE_SIZE,
				       &net->rxwq_dma, GFP_KERNEL);
	net->rxbuf = dma_alloc_coherent(&pdev->dev, NET_RX_BYTES,
					&net->rxbuf_dma, GFP_KERNEL);
	net->sq_dbr = dma_alloc_coherent(&pdev->dev, 64,
					 &net->sq_dbr_dma, GFP_KERNEL);
	net->cq_dbr = dma_alloc_coherent(&pdev->dev, 64,
					 &net->cq_dbr_dma, GFP_KERNEL);
	net->rq_dbr = dma_alloc_coherent(&pdev->dev, 64,
					 &net->rq_dbr_dma, GFP_KERNEL);
	net->txwi = kcalloc(NET_SQ_BBS, sizeof(*net->txwi), GFP_KERNEL);
	if (!net->wq || !net->cq || !net->rxwq || !net->rxbuf || !net->sq_dbr ||
	    !net->cq_dbr || !net->rq_dbr || !net->txwi) {
		err = -ENOMEM;
		goto err_free;
	}
	spin_lock_init(&net->arm_lock);
	INIT_DELAYED_WORK(&net->arm_dw, prism_net_arm_work);
	net->bf = dev->bar0 + (dma_addr_t)dev->eq.uar * MLX5_PRISM_PAGE_SIZE +
		  PRISM_SEND_DOORBELL_OFFSET;
	net->cq_arm = dev->bar0 +
		      (dma_addr_t)dev->eq.uar * MLX5_PRISM_PAGE_SIZE +
		      PRISM_CQ_ARM_DOORBELL_OFFSET;

	/*
	 * Trip-10/11: the comp EQ's event engine engages stochastically per
	 * CREATE_EQ (the T09-era lottery — trip-10's load was engaged,
	 * trip-11's was silent end to end). A silent engine is verified
	 * here with a fresh arm + a NOP whose CQE is guaranteed, and the
	 * whole chain is retried on a fresh comp EQ (a recreate re-rolls
	 * the dice; the new CQ of each attempt re-binds c_eqn and restarts
	 * the arm sequence).
	 */
	for (attempt = 0; attempt < 4; attempt++) {
		u32 ev0;

		if (attempt) {
			err = mlx5_prism_comp_eq_recreate(dev);
			if (err)
				goto err_free;
			netdev_info(ndev,
				    "comp eq event engine silent — retry with fresh eq (attempt %d, eqn=%u)\n",
				    attempt + 1, dev->comp_eq.eqn);
		}
		net_reset_rings(net);
		net->arm_sn = 0;	/* fresh CQ: sn space restarts */

		err = net_alloc_pd(net);
		if (err)
			goto err_free;
		err = net_create_mkey(net);
		if (err)
			goto err_free;
		err = net_create_cq(net);
		if (err)
			goto err_free;
		err = net_alloc_td_tis(net);
		if (err)
			goto err_free;
		err = net_create_sq(net);
		if (err)
			goto err_free;
		err = net_modify_sq_rdy(net);
		if (err)
			goto err_free;
		err = net_create_rq(net);
		if (err)
			goto err_free;
		net_rq_post_all(net);
		err = net_modify_rq_rdy(net);
		if (err)
			goto err_free;
		err = net_create_tir(net);
		if (err)
			goto err_free;
		err = net_create_flow(net);
		if (err)
			goto err_free;

		netif_napi_add(ndev, &net->napi, prism_napi_poll,
			       NAPI_POLL_WEIGHT);
		net->napi_added = true;

		if (stage < 2) {
			net->opened = true;
			/* opened visible to the xmit gate */
			wmb();
			goto armed;
		}

		err = net_set_root(net, true);
		if (err) {
			netdev_err(ndev, "SET_FLOW_TABLE_ROOT failed (%d)\n",
				   err);
			goto err_napi_del;
		}
		net->root_set = true;
		napi_enable(&net->napi);
		net->napi_enabled = true;
		/* the EQE dispatch gate opens before the first arm so the
		 * first completion event cannot be dropped
		 */
		net->opened = true;
		/* opened visible to the hardirq dispatch first */
		wmb();
		net_cq_set_ci(net);
		/* set_ci visible before the first arm */
		wmb();
		net_cq_arm_sync(net);
		net_cq_arm(net);

		/* engagement verify: the NOP's CQE is T14-proven; a silent
		 * comp EQ consumes nothing in the 100 ms probe window
		 */
		ev0 = dev->comp_eq.events;
		if (!net_fire_probe_nop(net))
			mlx5_prism_comp_eq_probe(dev);
		if ((int)(dev->comp_eq.events - ev0) > 0)
			goto armed;

		/* silent — unwind and re-roll with a fresh comp EQ */
		net->opened = false;
		/* opened=false visible to the hardirq dispatch before unwind */
		wmb();
		cancel_delayed_work_sync(&net->arm_dw);
		napi_disable(&net->napi);
		net->napi_enabled = false;
		netif_napi_del(&net->napi);
		net->napi_added = false;
		net_tx_flush(net);
		err = prism_net_destroy_objects(net);
		if (err) {
			netdev_err(ndev,
				   "destroy chain broken during retry (%d)\n",
				   err);
			goto err_free;
		}
	}

	netdev_err(ndev, "comp eq event engine silent after 4 attempts\n");
	err = -EIO;
	goto err_free;

armed:
	/* the verify NOP consumed the one-shot arm — re-arm for traffic
	 * (trip-14 verdict: pc=5 with cc=0/cq_ci=0 — completions landed on
	 * a DISARMED cq because nobody rearmed after the verify)
	 */
	net_cq_arm(net);

	/*
	 * Trip-21: after port churn the PHY takes ~250 ms to re-report
	 * oper=UP (prism1's opens consistently lagged ~235 ms while prism0's
	 * read instant). The old single read raced it and prism1 spent the
	 * whole traffic phase carrier-less. Bounded re-read instead —
	 * mlx5_core gets this from async port events this skeleton does not
	 * handle.
	 */
	{
		unsigned int wait_ms;
		bool link_up = false;

		for (wait_ms = 0; wait_ms <= 2000; wait_ms += 50) {
			if (wait_ms)
				msleep(50);
			memset(paos, 0, sizeof(paos));
			prism_paos_set_port(paos);
			if (!mlx5_prism_access_reg(dev, MLX5_PRISM_REG_PAOS,
						   false, 0, paos, paos,
						   PRISM_PAOS_REG_SIZE) &&
			    prism_paos_oper_readback(paos) == PRISM_PAOS_OPER_UP) {
				link_up = true;
				break;
			}
		}
		if (link_up)
			netif_carrier_on(ndev);
		netdev_info(ndev, "link oper %s after %u ms\n",
			    link_up ? "up" : "down", wait_ms);
	}

	/* clears the PPCNT window — the close-time read is the traffic delta */
	prism_net_wire_counters(net, "open");

	if (stage >= 3) {
		/* the stack may deliver traffic regardless of queue state —
		 * the xmit gate is the authoritative stage check
		 */
		net->xmit_on = true;
		netif_tx_start_all_queues(ndev);
	}
	if (netdev_arm_ms)
		queue_delayed_work(system_wq, &net->arm_dw,
				   msecs_to_jiffies(netdev_arm_ms));

	netdev_info(ndev,
		    "up (stage %u): pd=%u mkey=0x%x cqn=%u tdn=%u tisn=%u sqn=%u rqn=%u tirn=%u ft=%u.%u form=%s\n",
		    stage, net->pdn, net->mkey, net->cqn, net->tdn, net->tisn,
		    net->sqn, net->rqn, net->tirn, net->table_id, net->group_id,
		    net->tx_inline ? "all-inline" : "hdr-inline+ptr");
	return 0;

err_napi_del:
	netif_napi_del(&net->napi);
	net->napi_added = false;
err_free:
	prism_net_destroy_objects(net);
	prism_net_free_dma(net);
	netdev_err(ndev, "open failed (%d)\n", err);
	return err;
}

static void prism_net_close_dump(struct mlx5_prism_net *net)
{
	const u8 *cqe = net->cq ? net->cq + (net->cq_ci & (NET_CQES - 1)) *
				  PRISM_CQE_SIZE : NULL;

	/*
	 * Trip-9/10 discriminator: fresh arm, then one NOP WQE with
	 * CQ_UPDATE (its CQE is T14-proven guaranteed), then a 100 ms
	 * comp-EQ drain probe. If the probe reports type 0x00 events,
	 * generation works and the break was in commit/raise; an empty
	 * probe (raw ring dump follows) means the fw never generated
	 * the event for this CQ at all.
	 */
	if (net->have_sq && net->wq) {
		u32 pc = net->sq_pc;
		u32 n = min_t(u32, net->sq_pc, 4);
		u32 k;

		/* the traffic SENDs' CQEs have never been seen — dump the
		 * first four entries before anything else disturbs them
		 */
		for (k = 0; k < n; k++) {
			const u8 *c = net->cq + k * PRISM_CQE_SIZE;

			netdev_info(net->netdev,
				    "close: cqe[%u] op_own=0x%02x opcode=%u wqe_counter=%u byte_cnt=%u\n",
				    k, c[PRISM_CQE_OFF_OP_OWN],
				    c[PRISM_CQE_OFF_OP_OWN] >> 4,
				    mlx5_prism_get_be16(c + PRISM_CQE_OFF_WQE_COUNTER),
				    mlx5_prism_get_be32(c + PRISM_CQE_OFF_BYTE_CNT));
		}

		/* opcode-vs-timing discriminator (trip-15/16): a NOP
		 * (known eventer) then a tiny all-inline SEND, each right
		 * after a fresh arm with its own probe — which one
		 * events?
		 */
		if ((pc & (NET_SQ_BBS - 1)) < NET_SQ_BBS - 2) {
			u8 *ctrl = net->wq + (pc & (NET_SQ_BBS - 1)) *
					      PRISM_WQE_BB_SIZE;

			net_cq_arm(net);
			mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP,
					    (pc << 8) | PRISM_WQE_OPCODE_NOP);
			mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_QPN_DS,
					    (net->sqn << 8) | PRISM_WQE_CTRL_DS);
			ctrl[PRISM_WQE_OFF_CTRL_FM_CE_SE] =
				PRISM_WQE_CTRL_CQ_UPDATE;
			memset(&net->txwi[pc & (NET_SQ_BBS - 1)], 0,
			       sizeof(net->txwi[0]));
			net->txwi[pc & (NET_SQ_BBS - 1)].num_wqebbs = 1;
			net->sq_pc = pc + 1;
			net_sq_notify(net, ctrl);
			mlx5_prism_comp_eq_probe(net->dev);

			/* tiny all-inline SEND (T14 winner form, 14 B hdr +
			 * 4 B payload = 4 DS = 1 WQEBB)
			 */
			pc = net->sq_pc;
			ctrl = net->wq + (pc & (NET_SQ_BBS - 1)) *
					  PRISM_WQE_BB_SIZE;
			net_cq_arm(net);
			memset(ctrl, 0, PRISM_WQE_BB_SIZE);
			mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP,
					    (pc << 8) | PRISM_WQE_OPCODE_SEND);
			mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_CTRL_QPN_DS,
					    (net->sqn << 8) | 4);
			ctrl[PRISM_WQE_OFF_CTRL_FM_CE_SE] =
				PRISM_WQE_CTRL_CQ_UPDATE;
			mlx5_prism_put_be16(ctrl + PRISM_WQE_OFF_ETH_SEG +
					    PRISM_WQE_ESEG_OFF_INLINE_SZ, 14);
			ctrl[PRISM_WQE_OFF_ETH_SEG +
			     PRISM_WQE_ESEG_OFF_INLINE_START] = 0xff;
			mlx5_prism_put_be32(ctrl + PRISM_WQE_OFF_DATA_SEG +
					    PRISM_WQE_DS_SIZE +
					    PRISM_WQE_SEG_OFF_BYTE_CNT,
					    4 | PRISM_WQE_INLINE_FLAG);
			memset(&net->txwi[pc & (NET_SQ_BBS - 1)], 0,
			       sizeof(net->txwi[0]));
			net->txwi[pc & (NET_SQ_BBS - 1)].num_wqebbs = 1;
			net->sq_pc = pc + 1;
			net_sq_notify(net, ctrl);
			mlx5_prism_comp_eq_probe(net->dev);
		}
	}

	/* Trip-6 discriminator (pre-registered): the event-driven poll gives
	 * no visibility into whether completions ever landed — dump the
	 * ring state plus the first unconsumed CQE and the first posted
	 * WQE at close. cqe@ci fresh (opcode 0/13/14) => completions land,
	 * the event chain is broken; cqe@ci == 0xf1 => the WQEs never
	 * executed. The WQE@0 bytes diff against the T14 winner form.
	 */
	netdev_info(net->netdev,
		    "close: pc=%u cc=%u cq_ci=%u rq_pc=%u tx_pkts=%llu tx_err=%llu rx_pkts=%llu rx_err=%llu\n",
		    net->sq_pc, net->sq_cc, net->cq_ci, net->rq_pc,
		    net->tx_packets, net->tx_errors, net->rx_packets,
		    net->rx_errors);
	if (net->cq && cqe) {
		netdev_info(net->netdev,
			    "close: cqe@%u op_own=0x%02x opcode=%u byte_cnt=%u wqe_counter=%u\n",
			    (unsigned int)(net->cq_ci & (NET_CQES - 1)),
			    cqe[PRISM_CQE_OFF_OP_OWN],
			    cqe[PRISM_CQE_OFF_OP_OWN] >> 4,
			    mlx5_prism_get_be32(cqe + PRISM_CQE_OFF_BYTE_CNT),
			    mlx5_prism_get_be16(cqe + PRISM_CQE_OFF_WQE_COUNTER));
		print_hex_dump(KERN_INFO, "wqe@0: ", DUMP_PREFIX_OFFSET, 16, 1,
			       net->wq, 64, false);
	}
}

static int prism_ndo_stop(struct net_device *ndev)
{
	struct mlx5_prism_net *net = mlx5_prism_net_of(ndev);
	int err;

	net->opened = false;
	net->xmit_on = false;
	/* opened=false visible to the hardirq dispatch before teardown */
	wmb();
	cancel_delayed_work_sync(&net->arm_dw);

	netif_tx_stop_all_queues(ndev);
	netif_tx_disable(ndev);
	/* post-traffic wire delta for exactly this open window (the open-time
	 * read cleared the counters) — the trip-22 steering/wire discriminator
	 */
	prism_net_wire_counters(net, "close");
	if (net->napi_enabled) {
		napi_disable(&net->napi);
		net->napi_enabled = false;
	}
	if (net->napi_added) {
		netif_napi_del(&net->napi);
		net->napi_added = false;
	}
	netif_carrier_off(ndev);

	prism_net_close_dump(net);
	net_tx_flush(net);
	err = prism_net_destroy_objects(net);
	if (err) {
		/* the fw may still hold buffer addresses — leak them */
		netdev_err(ndev, "destroy chain broken (%d), leaking DMA\n",
			   err);
		return -EIO;
	}
	prism_net_free_dma(net);
	netdev_info(ndev, "down\n");
	return 0;
}

static void prism_ndo_get_stats64(struct net_device *ndev,
				  struct rtnl_link_stats64 *stats)
{
	struct mlx5_prism_net *net = mlx5_prism_net_of(ndev);

	stats->tx_packets = net->tx_packets;
	stats->tx_bytes = net->tx_bytes;
	stats->tx_errors = net->tx_errors;
	stats->tx_dropped = net->tx_errors;
	stats->rx_packets = net->rx_packets;
	stats->rx_bytes = net->rx_bytes;
	stats->rx_errors = net->rx_errors;
	stats->rx_dropped = net->rx_errors;
}

static const struct net_device_ops prism_netdev_ops = {
	.ndo_open		= prism_ndo_open,
	.ndo_stop		= prism_ndo_stop,
	.ndo_start_xmit	= prism_ndo_start_xmit,
	.ndo_get_stats64	= prism_ndo_get_stats64,
};

/* ---- register/unregister + EQE dispatch ---- */

static int net_query_mac(struct mlx5_prism_dev *dev, u8 *mac)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *out;
	const u8 *ctx;
	int err;

	out = kzalloc(512, GFP_KERNEL);
	if (!out)
		return -ENOMEM;
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_NIC_VPORT_CTX,
				     0, in, sizeof(in), out, 512);
	if (err) {
		kfree(out);
		return err;
	}
	ctx = out + PRISM_OFF_NVC_OUT;
	memcpy(mac, ctx + PRISM_OFF_NVC_PERM_MAC, ETH_ALEN);
	kfree(out);
	return 0;
}

int mlx5_prism_net_register(struct mlx5_prism_dev *dev)
{
	struct net_device *ndev;
	struct mlx5_prism_net *net;
	u8 mac[ETH_ALEN];
	int err;

	ndev = alloc_etherdev_mqs(sizeof(struct mlx5_prism_net), 1, 1);
	if (!ndev)
		return -ENOMEM;
	net = netdev_priv(ndev);
	net->netdev = ndev;
	net->dev = dev;
	net->tx_inline = !!netdev_inline_all;

	snprintf(ndev->name, IFNAMSIZ, "prism%%d");
	ndev->netdev_ops = &prism_netdev_ops;
	netif_carrier_off(ndev);

	memset(mac, 0, sizeof(mac));
	if (!net_query_mac(dev, mac) && is_valid_ether_addr(mac))
		eth_hw_addr_set(ndev, mac);
	else
		eth_hw_addr_random(ndev);

	err = register_netdev(ndev);
	if (err) {
		free_netdev(ndev);
		return err;
	}
	dev->ndev = ndev;
	pci_info(dev->pdev, "netdev %s registered (mac %pM)\n", ndev->name,
		 ndev->dev_addr);
	return 0;
}

void mlx5_prism_net_unregister(struct mlx5_prism_dev *dev)
{
	struct net_device *ndev = dev->ndev;

	if (!ndev)
		return;
	dev->ndev = NULL;
	/* ndev=NULL visible to the hardirq dispatch before the netdev dies */
	wmb();
	/* runs ndo_stop synchronously when the link is up */
	unregister_netdev(ndev);
	free_netdev(ndev);
}

/* EQE type 0x00 dispatch (hardirq context): match the shared CQ and
 * schedule the poll. `opened` gates the race with ndo_stop, which clears
 * it before disabling the NAPI instance.
 */
void mlx5_prism_net_comp_event(struct mlx5_prism_dev *dev, u32 cqn)
{
	struct mlx5_prism_net *net;
	struct net_device *ndev = READ_ONCE(dev->ndev);

	if (!ndev)
		return;
	net = netdev_priv(ndev);
	if (!READ_ONCE(net->opened))
		return;
	if (cqn != net->cqn) {
		dev_info_ratelimited(&ndev->dev,
				     "comp cqn mismatch: eqe=%u cq=%u (dispatch skipped)\n",
				     cqn, net->cqn);
		return;
	}
	dev_info_ratelimited(&ndev->dev, "comp event dispatched: cqn=%u\n", cqn);
	napi_schedule(&net->napi);
}
