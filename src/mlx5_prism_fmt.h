/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mlx5_prism: L1 wire-format accessors and THE offset table.
 *
 * Layer rule (DESIGN.md v2 §2.1): raw byte/bit offsets live ONLY in this
 * header. Every constant below was verified against mlx5_ifc.h bit layouts
 * and/or hardware hexdumps on FW 14.32.1908 — provenance in the per-field
 * comments and progress.md 历史问题归档. Two conventions, clearly separated:
 *
 *   PRISM_OFF_*  plain byte offsets for byte-aligned BE fields, used with
 *                the mlx5_prism_{get,put}_be16/32/64 helpers.
 *   PRISM_FLD_*  bit offsets for sub-byte fields (byte * 8 + bit-in-byte,
 *                LSB-first within the byte), used with prism_set_bits()/
 *                prism_get_bits(). Every field prism touches so far fits
 *                within a single byte; multi-byte BE fields use the OFF
 *                convention above, never straddling bit-field accessors.
 */

#ifndef __MLX5_PRISM_FMT_H
#define __MLX5_PRISM_FMT_H

#include <linux/bits.h>
#include <linux/types.h>

#include "mlx5_prism.h"

/* ---- sub-byte accessor: byte-LSB-first bit numbering ---- */

static inline u32 prism_get_bits(const u8 *buf, u16 bit_off, u8 nbits)
{
	u32 v = 0;
	u8 k;

	for (k = 0; k < nbits; k++) {
		u16 n = bit_off + k;

		if (buf[n >> 3] & BIT(n & 7))
			v |= BIT(k);
	}
	return v;
}

static inline void prism_set_bits(u8 *buf, u16 bit_off, u8 nbits, u32 val)
{
	u8 k;

	for (k = 0; k < nbits; k++) {
		u16 n = bit_off + k;

		if (val & BIT(k))
			buf[n >> 3] |= BIT(n & 7);
		else
			buf[n >> 3] &= ~BIT(n & 7);
	}
}

/*
 * ---- command inline header (16 B), byte offsets into in/out buffers ----
 * All verified on FW 14.32.1908 (progress.md 历史问题归档 ①).
 *
 * Command layout maps (byte ranges; the 16 B header's opcode/op_mod at
 * bytes[0:8] are filled by the engine, not by callers):
 *
 *	query_pages_in  (16 B)		manage_pages_in  (16 + 8*n B)
 *	  [0:8]   header			  [0:8]    header
 *	  [8:16]  op_mod (boot/init)	  [8:12]   function_id
 *					  [12:16]  num_entries
 *	query_pages_out (16 B)		  [16+8i]  pas[i]
 *	  [8:12]  function_id (lo 16)
 *	  [12:16] num_pages		manage_pages_out (16 B)
 *					  [8:12]   out_num_entries
 *	query_issi_out (112 B exactly — a shorter outlen is rejected 0x51)
 *	  [10:12] current_issi (be16)
 *	  [108:112] supported_issi_dw0 (be32, one bit per ISSI)
 *
 *	access_reg_in (16 + reg_size B)	create_eq_in (272 + 8*npas B)
 *	  [10:12] register_id (be16)	  [16:80]   eqc (64 B) — see the
 *	  [16:]   register payload		    dw3/intr encoding notes below
 *	access_reg_out (16 + reg_size B)	  [88:120]  event_bitmask (be64)
 *	  [16:]   register data		  [272+8i]  pas[i]
 *					  out[11]   eq_number
 *	alloc_uar_out: uar (24-bit) at [8:12]
 *	dealloc_uar_in: uar (be32) at [8:12]
 *	query_eq_in / destroy_eq_in: eq_number at [11]
 */
#define PRISM_OFF_MP_FUNC_ID		8	/* manage_pages_in: be32, low 16 bits valid */
#define PRISM_OFF_MP_NUM_ENTRIES	12	/* manage_pages_in: be32 (give/take count) */
#define PRISM_OFF_MP_PAS		16	/* manage_pages_in: pas[i] be64 @ +8i */
#define PRISM_OFF_MP_OUT_ENTRIES	8	/* manage_pages_out: be32 */
#define PRISM_OFF_QP_FUNC_ID		8	/* query_pages_out: be32, low 16 bits valid */
#define PRISM_OFF_QP_NUM_PAGES		12	/* query_pages_out: be32 */
#define PRISM_OFF_ISSI_CUR		10	/* query_issi_out: current be16 @ [10:12] */

/* query_issi_out: supported be32; the out buffer must be 112 B —
 * a shorter outlen is rejected with fw status 0x51.
 */
#define PRISM_OFF_ISSI_SUPPORTED	108

#define PRISM_OFF_REG_ID		10	/* access_reg_in: register_id be16 @ [10:12] */
#define PRISM_OFF_REG_ARG		12	/* access_reg_in: argument be32 @ [12:16] */

/* query_eq_in/destroy_eq_in: eq_number byte; create_eq_out: eq_number */
#define PRISM_OFF_EQN			11

#define PRISM_OFF_UAR_IN		8	/* dealloc_uar_in: be32 (24-bit value) */
#define PRISM_OFF_UAR_OUT		8	/* alloc_uar_out: be32, low 24 bits valid */

/*
 * ---- HCA capability payload (byte/bit offsets relative to payload start,
 * i.e. buffer + MLX5_PRISM_CMD_HDR_SIZE) ----
 */
#define PRISM_FLD_CAPS_LOG_MAX_SRQ_SZ	(16 * 8)	/* byte: log_max_srq_sz */
#define PRISM_FLD_CAPS_LOG_MAX_QP_SZ	(17 * 8)	/* byte: log_max_qp_sz */
#define PRISM_FLD_CAPS_LOG_MAX_QP	(19 * 8 + 3)	/* 5 bits: log_max_qp */
#define PRISM_FLD_CAPS_LOG_MAX_MSG	(56 * 8 + 3)	/* 5 bits: log_max_msg (ifc 0x1c3) */
#define PRISM_FLD_CAPS_LOG_MAX_KLM	(35 * 8 + 3)	/* 5 bits: log_max_klm_list_size */
#define PRISM_FLD_CAPS_NUM_PORTS	(55 * 8)	/* byte: num_ports */
#define PRISM_OFF_CAPS_CMDIF_CHKSUM	66	/* known-good: 2 bytes cleared */

/* caps byte 82 = low byte of max_wqe_sz_sq (v4.14 AND v5.15 ifc both map
 * bytes 82-83 there; this era fw carries the log2 value in the low byte —
 * trip-11 identification). The T05 known-good clears bits 0-1 (SET is
 * rejected with them set), turning the fw-reported 0x0? into 0x04; the
 * caps82 module param can force another value to test the trip-12
 * hypothesis that log 4 (16 B) < the SQ WQEBB stride 6 (64 B) is what
 * makes CREATE_SQ a content-invariant bad parameter.
 */
#define PRISM_OFF_CAPS_UNIDENT_82	82

/* known-good: 2 bytes cleared (field unidentified, effect verified) */
#define PRISM_OFF_CAPS_UNIDENT_162	162

/*
 * ---- create_eq_in (byte offsets into the full command buffer) ----
 */
#define PRISM_OFF_EQC			16	/* eqc 64 B @ payload0 */
#define PRISM_OFF_EQ_EVENT_BITMASK	88	/* payload+72: 4 x be64 words */
#define PRISM_OFF_EQ_PAS		272	/* payload+256: pas[i] be64 @ +8i */

/*
 * eqc field geometry (mlx5_ifc.h struct mlx5_ifc_eqc_bits, PRM numbering
 * is MSB-first within each big-endian dword): log_eq_size @99 (5 bits),
 * uar_page @104 (24 bits), intr @180 (12 bits).
 *
 * CAUTION — two bit-numbering conventions meet here and diverge WITHIN a
 * byte: MLX5_SET algebra (dw_bit_off = 32 - sz - (bit_off & 31), stored
 * big-endian) places ifc bit 99 at eqc[12] bit 4, so log_eq_size occupies
 * eqc[12] bits 0-4; prism_get_bits counts LSB-first, so its "99" would
 * read eqc[12] bits 3-7. The windows overlap on bits 3-4 — that overlap
 * is how the T09-era flipped-window readback parser stayed invisible
 * while the field was always 0. Everything eqc uses plain BE arithmetic
 * so exactly one convention is in play.
 *
 * CONFIRMED ON HARDWARE (T10-prep, 2026-09-17, progress.md): the fw
 * decodes eqc per ifc semantics. The legacy T09 encodings — dw3 as
 * ((log&7)<<29)|uar, intr as be16(vec<<4) @eqc[16:18] — put log_eq_size
 * into reserved bits (the fw read eqc[12]&0x1f = uar&0x1f = 0: THE
 * 1-ENTRY EQ WAS LITERALLY REQUESTED) and never touched the intr field
 * (the MSI-X line never fired). With the encodings below the fw stores
 * log12 verbatim (QUERY_EQ dw3 readback 0x0c001000), events land on
 * successive entries of a real 4096-entry ring, and the MSI-X line fires
 * about once per event. Byte-equivalence with MLX5_SET is proven
 * host-side (scripts/test/fmt_equiv_check.c §4).
 */
static inline void prism_eqc_write_dw3(u8 *in, u8 log_sz, u32 uar)
{
	mlx5_prism_put_be32(in + PRISM_OFF_EQC + 12,
			    ((u32)(log_sz & 0x1f) << 24) | (uar & 0xffffff));
}

static inline void prism_eqc_set_intr(u8 *in, u32 vec)
{
	mlx5_prism_put_be32(in + PRISM_OFF_EQC + 20, vec & 0xfff);
}

/*
 * Decoders for the same three fields (QUERY_EQ readback; eqc points at
 * the 64B eqc payload): log_eq_size [99,103] = eqc[12] bits 0-4,
 * uar_page [104,127] = be24 at eqc[13:16], intr [180,191] = low 12 bits
 * of the be32 at eqc[20:24]. Matches MLX5_GET algebra exactly;
 * prism_get_bits would read the flipped LSB-first windows.
 */
static inline u32 prism_eqc_logsz_readback(const u8 *eqc)
{
	return eqc[12] & 0x1f;
}

static inline u32 prism_eqc_uar_readback(const u8 *eqc)
{
	return mlx5_prism_get_be32(eqc + 12) & 0xffffff;
}

static inline u32 prism_eqc_intr_readback(const u8 *eqc)
{
	return mlx5_prism_get_be32(eqc + 20) & 0xfff;
}

/*
 * ---- ppcnt_reg (T12): all offsets below are relative to the 256-byte
 * register IMAGE, i.e. the access_reg payload at command buffer + 16
 * (same "payload-relative" convention as the caps block above). Layout
 * per mlx5_ifc struct mlx5_ifc_ppcnt_reg_bits (v5.15 :8905, unchanged in
 * v7.0): the counter set union starts at image byte 8; every counter is
 * a big-endian _high/_low 32-bit pair. Counter-group offsets follow the
 * ifc group layouts (mlx5_ifc_eth_802_3_cntrs_grp_data_layout_bits,
 * _phys_layer_cntrs_bits, _phys_layer_statistical_cntrs_bits) —
 * byte-equivalence with MLX5_SET/MLX5_GET algebra is proven host-side
 * (scripts/test/fmt_equiv_check.c §ppcnt).
 *
 * The card is dual-PF (08:00.0/.1, one port per function), so
 * local_port is always 1 on each dev instance — mlx5_core likewise
 * hardcodes local_port=1 (en_stats.c).
 */
#define PRISM_PPCNT_REG_SIZE		256
#define PRISM_PPCNT_LOCAL_PORT		1

#define PRISM_OFF_PPCNT_LOCAL_PORT	1	/* byte: local_port */
#define PRISM_FLD_PPCNT_GRP		(3 * 8)	/* 6 bits: counter group select */
#define PRISM_FLD_PPCNT_CLR		(4 * 8 + 7)	/* 1 bit: clear-on-read */

/* clr MUST stay 0: prism reads must never zero the fw counters the
 * ethtool cross-check (T12 J3) and T13's monitor depend on.
 */

#define PRISM_PPCNT_GRP_IEEE_802_3	0x0	/* a_frames_* / symbol errors */
#define PRISM_PPCNT_GRP_PHY_LAYER	0x12	/* FEC + link error counters */
#define PRISM_PPCNT_GRP_PHY_STATISTICAL	0x16	/* CX5+ (PCAM-gated); probed once */

/* grp 0x12 (physical layer counters) — image-relative be64 pairs. */
#define PRISM_OFF_PPCNT12_TSLC		0x08	/* time_since_last_clear */
#define PRISM_OFF_PPCNT12_SYM_ERR	0x10	/* symbol_errors */
#define PRISM_OFF_PPCNT12_SYNC_HDR	0x18	/* sync_headers_errors */
#define PRISM_OFF_PPCNT12_EDPL_L0	0x20	/* edpl_bip_errors_lane[i] @ +8i */
#define PRISM_OFF_PPCNT12_FC_CORR_L0	0x40	/* fc_fec_corrected_blocks_lane[i] */
#define PRISM_OFF_PPCNT12_FC_UNC_L0	0x60	/* fc_fec_uncorrectable_blocks_lane[i] */
#define PRISM_OFF_PPCNT12_RS_CORR	0x80	/* rs_fec_corrected_blocks */
#define PRISM_OFF_PPCNT12_RS_UNC	0x88	/* rs_fec_uncorrectable_blocks */
#define PRISM_OFF_PPCNT12_LINK_DOWN	0xC8	/* be32: link_down_events */

/* grp 0x0 (IEEE 802.3) — the ethtool -S anchors rx/tx_packets_phy and
 * rx_symbol_err_phy read these (ethtool name in the comment).
 */
#define PRISM_OFF_PPCNT0_TX_OK		0x08	/* a_frames_transmitted_ok (tx_packets_phy) */
#define PRISM_OFF_PPCNT0_RX_OK		0x10	/* a_frames_received_ok (rx_packets_phy) */
/*
 * a_symbol_error_during_carrier — the ethtool -S anchor rx_symbol_err_phy.
 */
#define PRISM_OFF_PPCNT0_SYM_ERR	0x70

/* grp 0x16 (statistical; pre-FEC BER numerator/denominator — expected
 * unavailable on pre-PCAM silicon, probed once for the record).
 */
#define PRISM_OFF_PPCNT16_RX_BITS	0x10	/* phy_received_bits */
#define PRISM_OFF_PPCNT16_SYM_ERR	0x18	/* phy_symbol_errors */
#define PRISM_OFF_PPCNT16_CORR_BITS	0x20	/* phy_corrected_bits */

static inline u64 prism_ppcnt_ctr64(const u8 *reg, u16 off)
{
	return ((u64)mlx5_prism_get_be32(reg + off) << 32) |
	       mlx5_prism_get_be32(reg + off + 4);
}

static inline void prism_ppcnt_set_grp(u8 *reg, u8 grp)
{
	prism_set_bits(reg, PRISM_FLD_PPCNT_GRP, 6, grp);
	reg[PRISM_OFF_PPCNT_LOCAL_PORT] = PRISM_PPCNT_LOCAL_PORT;
}

/*
 * ---- paos_reg (T13): 16-byte register image, image-relative like ppcnt.
 * Layout per mlx5_ifc struct mlx5_ifc_paos_reg_bits: local_port @0x08
 * (8 bits) = byte 1; oper_status @0x1c (4 bits) lands in the low nibble
 * of byte 3 under the MLX5_SET dw algebra (same shift rule as ppcnt grp
 * @0x1a → PRISM bit 24, LSB-first). Values 1=up 2=down (mlx5_core enum
 * mlx5_port_status; port.c's admin-state query is the write-side sibling
 * of this read). admin_status (@0x14 → byte 2 low nibble) is never
 * touched: prism never changes port admin state.
 */
#define PRISM_PAOS_REG_SIZE		16
#define PRISM_OFF_PAOS_LOCAL_PORT	1	/* byte: local_port */
#define PRISM_FLD_PAOS_OPER		(3 * 8)	/* 4 bits: oper_status */

#define PRISM_PAOS_OPER_UP		1
#define PRISM_PAOS_OPER_DOWN		2

static inline void prism_paos_set_port(u8 *reg)
{
	reg[PRISM_OFF_PAOS_LOCAL_PORT] = PRISM_PPCNT_LOCAL_PORT;
}

static inline u32 prism_paos_oper_readback(const u8 *reg)
{
	return prism_get_bits(reg, PRISM_FLD_PAOS_OPER, 4);
}

/*
 * ---- mtmp_reg (T13): temperature. sensor_index @0x14 (12 bits)
 * straddles bytes 1-2 under the MLX5_SET algebra, but the only sensor
 * prism queries is the ASIC sensor 0 — an all-zero request image IS
 * sensor_index=0, so no bit-packing helper is needed. temperature @0x30
 * (16 bits) = be16 at image bytes 6:8, 0.125 °C per LSB (mlx5_core
 * hwmon.c mtmp_temp_to_mdeg: LSB × 125 = millidegrees). Two lengths are
 * probed at monitor start: 8 B (CX4-era PRM minimal layout, ends right
 * after temperature) then 32 B (modern layout with thresholds and sensor
 * names) — the fw's answer is a quirk-profile fact either way.
 */
#define PRISM_MTMP_LEN_MIN		8
#define PRISM_MTMP_LEN_FULL		32
#define PRISM_OFF_MTMP_TEMPERATURE	6	/* be16: 0.125 °C/LSB */

static inline int prism_mtmp_to_mdeg(u16 raw)
{
	return (int)raw * 125;
}

/*
 * ---- ifc bit-field arithmetic (T14): MLX5_SET algebra on plain BE dwords.
 *
 * sqc/cqc/mkc fields sit at arbitrary within-dword positions, so the
 * byte-LSB-first prism_set_bits() convention cannot be used for them — its
 * window is flipped relative to ifc/PRM numbering (the eqc CAUTION above is
 * the archived incident). prism_ifc_set()/prism_ifc_get() implement the
 * MLX5_SET shift rule directly: field value bit 0 sits at dword LSB offset
 * (32 - nbits - (bit & 31)); the dword itself is stored big-endian. Fields
 * must not straddle a dword boundary (none below do; 64-bit fields are
 * byte-aligned and handled as be64 puts). Byte-equivalence with MLX5_SET
 * is proven host-side (scripts/test/fmt_equiv_check.c §T14).
 *
 * The PRISM_*_BIT_* constants carry ifc bit numbers relative to the named
 * structure's start — they are ONLY valid inputs to prism_ifc_set/get.
 */
static inline void prism_ifc_set(u8 *buf, u16 bit, u8 nbits, u32 val)
{
	u32 dw = bit / 32;
	u32 off = 32 - nbits - (bit & 31);
	u32 mask = (nbits == 32) ? 0xffffffffu : (BIT(nbits) - 1);
	u32 v = mlx5_prism_get_be32(buf + dw * 4);

	mlx5_prism_put_be32(buf + dw * 4,
			    (v & ~(mask << off)) | ((val & mask) << off));
}

static inline u32 prism_ifc_get(const u8 *buf, u16 bit, u8 nbits)
{
	u32 dw = bit / 32;
	u32 off = 32 - nbits - (bit & 31);
	u32 mask = (nbits == 32) ? 0xffffffffu : (BIT(nbits) - 1);

	return (mlx5_prism_get_be32(buf + dw * 4) >> off) & mask;
}

/*
 * ---- T14 smoke-test commands (byte offsets into the full command buffer;
 * layouts per mlx5_ifc v5.15 — the running kernel's tree — ABI-crossed
 * against v4.9/v7.0 during the route-A survey, progress.md T14):
 *
 *	alloc_pd_in (16 B)			create_mkey_in (272 B, PA mode:
 *	dealloc_pd_in: pd be32 @ [8:12]	  no klm tail needed — dr_send.c)
 *	alloc_pd_out: pd (24-bit) @ [8:12]	  mkc (64 B) @ [16:80]
 *	create_mkey_out: mkey_index	  destroy_mkey_in: mkey_index be32
 *	  (24-bit) @ [8:12]		    @ [8:12]; mkey = index << 8
 *
 *	create_cq_in (280 B with 1 pas)	create_sq_in (280 B with 1 pas)
 *	  cqc (64 B) @ [16:80]			  sqc hdr (48 B) @ [32:80]
 *	  pas[i] be64 @ [272+8i]			  wq hdr (192 B) @ [80:272]
 *	create_cq_out: cqn (24-bit) @ [8:12]	  pas[i] be64 @ [272+8i]
 *	destroy_cq_in: cqn be32 @ [8:12]	create_sq_out: sqn (24-bit) @ [8:12]
 *	destroy_sq_in (16 B): sqn be32 @ [8:12]
 *
 *	modify_sq_in (272 B): sq_state @ ifc bit 0x40 (top nibble of byte 8),
 *	  sqn be32 @ [8:12], modify_bitmask be64 @ [16:24] (stays 0 — a state
 *	  transition needs no field mask, wc.c form), sqc ctx @ [32:]
 *
 *	alloc_td_in (16 B)  alloc_td_out: tdn (24-bit) @ [8:12]
 *	dealloc_td_in (16 B): tdn be32 @ [8:12]
 *	create_tis_in (32 B hdr + 160 B tisc): tisc @ [32:]
 *	create_tis_out: tisn (24-bit) @ [8:12]
 *	destroy_tis_in (16 B): tisn be32 @ [8:12]
 */
#define PRISM_OFF_PD_IN			8	/* alloc/dealloc_pd */
#define PRISM_OFF_PD_OUT		8
#define PRISM_OFF_MKEY_IDX_IN		8	/* destroy_mkey: mkey index */
#define PRISM_OFF_MKEY_IDX_OUT		8	/* create_mkey_out */
#define PRISM_OFF_CQN_IN		8	/* create/destroy_cq */
#define PRISM_OFF_CQN_OUT		8
#define PRISM_OFF_SQN_IN		8	/* destroy_sq_in */
#define PRISM_OFF_SQN_OUT		8	/* create_sq_out */
#define PRISM_OFF_TDN_IN		8	/* dealloc_td_in */
#define PRISM_OFF_TDN_OUT		8	/* alloc_td_out */
#define PRISM_OFF_TISN_IN		8	/* destroy_tis_in */
#define PRISM_OFF_TISN_OUT		8	/* create_tis_out */
#define PRISM_OFF_MKC			16	/* create_mkey_in */
#define PRISM_OFF_CQC			16	/* create_cq_in */
#define PRISM_OFF_CQ_PAS		272	/* pas[i] be64 @ +8i */
#define PRISM_OFF_SQC			32	/* create_sq_in / modify_sq_in ctx */
#define PRISM_OFF_SQC_WQ		48	/* wq embedded at sqc byte 0x30 */
#define PRISM_OFF_SQ_PAS		272
#define PRISM_OFF_TISC			32	/* create_tis_in ctx */

#define PRISM_CREATE_CQ_IN_LEN(npas)	(272 + 8 * (npas))
#define PRISM_CREATE_SQ_IN_LEN(npas)	(272 + 8 * (npas))
#define PRISM_MODIFY_SQ_IN_LEN		272
#define PRISM_CREATE_TIS_IN_LEN		192	/* 32 B hdr + 160 B tisc */
#define PRISM_CREATE_MKEY_IN_LEN	272

/* mkc field bits (mlx5_ifc_mkc_bits). */
#define PRISM_MKC_BIT_A			0x11	/* atomic */
#define PRISM_MKC_BIT_RW		0x12	/* remote write */
#define PRISM_MKC_BIT_RR		0x13	/* remote read */
#define PRISM_MKC_BIT_LW		0x14	/* local write */
#define PRISM_MKC_BIT_LR		0x15	/* local read */
#define PRISM_MKC_BIT_ACCESS_MODE	0x16	/* 2 bits; 0 = PA */
#define PRISM_MKC_BIT_QPN		0x20	/* 24 bits; 0xffffff = unbound */
#define PRISM_MKC_BIT_LENGTH64		0x60
#define PRISM_MKC_BIT_PD		0x68	/* 24 bits */

/* cqc field bits (mlx5_ifc_cqc_bits). */
#define PRISM_CQC_BIT_LOG_CQ_SIZE	0x63	/* 5 bits */
#define PRISM_CQC_BIT_UAR_PAGE		0x68	/* 24 bits */
#define PRISM_CQC_BIT_C_EQN		0xa0	/* events only when armed (never) */
#define PRISM_CQC_BIT_LOG_PAGE_SIZE	0xc3	/* 5 bits; 0 = 4 KiB pages */
#define PRISM_CQC_OFF_DBR_ADDR		56	/* be64: [0]=set_ci [4]=arm */

/* sqc field bits (mlx5_ifc_sqc_bits; identical across v4.9/5.15/7.0 except
 * non_wire, which 6.x inserted at 0xf — 5.15 carries that bit as reserved
 * and this 2016 fw is expected to reject it (tier 2 of the ladder).
 */
#define PRISM_SQC_BIT_FLUSH_IN_ERROR_EN	0x3	/* 1 bit */
#define PRISM_SQC_BIT_MIN_WQE_INLINE_MODE	0x5	/* 3 bits; 0 = none */
#define PRISM_SQC_BIT_STATE		0x8	/* 4 bits: 0 RST 1 RDY */
#define PRISM_SQC_BIT_NON_WIRE		0xf	/* 1 bit; 7.0-only (tier 2) */
#define PRISM_SQC_BIT_TS_FORMAT		0x1a	/* 2 bits; stays 0 on this fw */
#define PRISM_SQC_BIT_CQN		0x48	/* 24 bits */
#define PRISM_SQC_BIT_TIS_LST_SZ	0x100	/* 16 bits */
#define PRISM_SQC_BIT_TIS_NUM_0		0x168	/* 24 bits */
#define PRISM_SQC_STATE_RST		0x0
#define PRISM_SQC_STATE_RDY		0x1

/* wq field bits (mlx5_ifc_wq_bits, identical across v4.14/5.15/7.0; the
 * sqc embeds it at byte 0x30). log_wq_stride carries ilog2(WQEBB) = 6
 * verbatim — en/params.c:579 form, no 16-byte unit conversion. NOTE
 * (trip-12 lesson): dbr_addr is a 64-bit field at wq bit 0x80 = byte 16 —
 * an early survey note mis-recorded it as 0xa0/byte 20, which made the fw
 * read the bus address's HIGH dword as the whole address and reject
 * CREATE_SQ with 0x17a194 "dbr_address must be dword aligned" on every
 * field variant (the variant ladder never touched dbr).
 */
#define PRISM_WQ_BIT_WQ_TYPE		0x0	/* 4 bits; 1 = CYCLIC */
#define PRISM_WQ_BIT_PD			0x48	/* 24 bits */
#define PRISM_WQ_BIT_UAR_PAGE		0x68	/* 24 bits */
#define PRISM_WQ_BIT_LOG_WQ_STRIDE	0x10c	/* 4 bits; 6 = ilog2(64) */
#define PRISM_WQ_BIT_LOG_WQ_PG_SZ	0x113	/* 5 bits; 0 = 4 KiB pages */
#define PRISM_WQ_BIT_LOG_WQ_SZ		0x11b	/* 5 bits */
#define PRISM_WQ_OFF_DBR_ADDR		16	/* be64: [4]=sq counter */
#define PRISM_WQ_TYPE_CYCLIC		0x1

/* modify_sq_in fields (bit 0x40 = top nibble of byte 8). */
#define PRISM_MODSQ_BIT_SQ_STATE	0x40	/* 4 bits: the CURRENT state */

/* tisc field bits (mlx5_ifc_tisc_bits) — tier 3 writes only the domain. */
#define PRISM_TISC_BIT_TRANSPORT_DOMAIN	0x128	/* 24 bits */

/*
 * Trip-11 env probes (trip-10 verdict: CREATE_SQ 0x3/0x17a194 with a
 * wc.c-identical field set — the blocker is environmental):
 *
 * nic_vport_context (query_nic_vport_out, context @ byte 16):
 * min_wqe_inline_mode @0x5 (3 bits) — the value mlx5e_create_sq forwards
 * when the eth-offload cap wqe_inline_mode == 1 (vport.h
 * MLX5_CAP_INLINE_MODE_VPORT_CONTEXT).
 *
 * per_protocol_networking_offload caps (QUERY_HCA_CAP group 1, payload @
 * byte 16): wqe_inline_mode @0x12 (2 bits).
 */
#define PRISM_OFF_NVC_OUT			16
#define PRISM_NVC_BIT_MIN_WQE_INLINE_MODE	0x5	/* 3 bits */
#define PRISM_ETHCAP_BIT_WQE_INLINE_MODE	0x12	/* 2 bits */

/*
 * ---- WQE / CQE wire geometry (T14): hand-packed big-endian segments
 * inside 64-byte WQEBBs (ctrl 16 B + eth 16 B + data 16 B each = one DS);
 * CQEs are 64-byte entries. Offsets below are relative to their entry.
 *
 *	ctrl seg: opmod_idx_opcode be32 @0 ([31:24] opmod, [23:8] wqe_index
 *	  in WQEBBs, [7:0] opcode), qpn_ds be32 @4 ([31:8] the SQN this WQE
 *	  is posted to — en_tx.c cseg->qpn_ds = (sqn << 8) | ds, [7:0] DS
 *	  count incl. the ctrl seg), signature @8 (0 — wqe_signature off),
 *	  fm_ce_se @11 (bit3 CQ_UPDATE = request a CQE), imm @12
 *	eth seg: 16 B @16 — with a vport min_wqe_inline_mode floor the L2
 *	  header rides INLINE here (mlx5e attr.ihs form): inline_hdr.sz be16
 *	  @eseg+12, header bytes from eseg+14 spilling into the following DS
 *	  units; with no floor a plain unoffloaded send leaves it all zero
 *	data seg: byte_cnt be32 @0 — bit31 set marks the payload as inline,
 *	  following the counter inside the WQE (the smoke SEND's form); a
 *	  pointer dseg (lkey @4, addr be64 @8) returns with T15's receive
 *	  side
 *
 *	CQE: opcode/owner byte @0x3f (opcode bits[7:4], owner bit0 — HW
 *	  writes owner 0 on the first lap, SW pre-fills 0xf1), byte_cnt be32
 *	  @0x2c, wqe_counter be16 @0x3c. Opcodes: 0 REQ (send ok — the TX
 *	  verdict; RX completions move to T15 with the receive side). Error
 *	  CQEs (13/14) follow mlx5_err_cqe: srqn @0x20, vendor_err_synd
 *	  @0x36, syndrome @0x37, s_wqe_opcode_qpn be32 @0x38 (the failed
 *	  WQE opcode at +1).
 */
#define PRISM_WQE_BB_SIZE		64
#define PRISM_WQE_DS_SIZE		16
#define PRISM_WQE_CTRL_DS		1	/* NOP: ctrl only */
#define PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP	0
#define PRISM_WQE_OFF_CTRL_QPN_DS	4
#define PRISM_WQE_OFF_CTRL_FM_CE_SE	11
#define PRISM_WQE_OFF_ETH_SEG		16
#define PRISM_WQE_ESEG_OFF_INLINE_SZ	12	/* be16: inline header size */
#define PRISM_WQE_ESEG_OFF_INLINE_START	14	/* header bytes, spill past eseg */
#define PRISM_WQE_OFF_DATA_SEG		32
#define PRISM_WQE_SEG_OFF_BYTE_CNT	0

#define PRISM_WQE_OPCODE_NOP		0x00
#define PRISM_WQE_OPCODE_SEND		0x0a
#define PRISM_WQE_CTRL_CQ_UPDATE	0x08	/* fm_ce_se bit3 */

/* data-segment byte_count bit31 marks inlined data (MLX5_INLINE_SEG): the
 * payload follows the 4-byte counter inside the WQE instead of a pointer
 * — legal regardless of the vport's min_wqe_inline_mode and independent
 * of any mkey (mlx5e's small-packet form).
 */
#define PRISM_WQE_INLINE_FLAG		0x80000000u

#define PRISM_CQE_SIZE			64
#define PRISM_CQE_OFF_BYTE_CNT		0x2c
#define PRISM_CQE_OFF_WQE_COUNTER	0x3c
#define PRISM_CQE_OFF_OP_OWN		0x3f
/* error CQEs (opcode 13/14) follow struct mlx5_err_cqe (v4.14 == v5.15). */
#define PRISM_CQE_OFF_ERR_VENDOR	0x36
#define PRISM_CQE_OFF_ERR_SYNDROME	0x37
#define PRISM_CQE_OFF_ERR_S_WQE_OP_QPN	0x38	/* be32; WQE opcode at +1 */
#define PRISM_CQE_OWNER_INIT		0xf1	/* invalid opcode, owner = SW-opposite */
#define PRISM_CQE_OPCODE_REQ		0x0
#define PRISM_CQE_OPCODE_REQ_ERR	0xd

/* Send doorbell: first 8 ctrl-seg bytes written raw (they are already
 * big-endian) to bfreg 0 of the SQ's UAR page — plain doorbell semantics,
 * the HW DMA-reads the WQE from host memory (mlx5_write64 / doorbell.h).
 */
#define PRISM_SEND_DOORBELL_OFFSET	0x800

#endif /* __MLX5_PRISM_FMT_H */
