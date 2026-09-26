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
#define PRISM_FLD_CAPS_LOG_MAX_KLM	(35 * 8 + 3)	/* 5 bits: log_max_klm_list_size */
#define PRISM_FLD_CAPS_NUM_PORTS	(55 * 8)	/* byte: num_ports */
#define PRISM_OFF_CAPS_CMDIF_CHKSUM	66	/* known-good: 2 bytes cleared */

/* known-good: bits 0-1 cleared (field unidentified, effect verified) */
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

#endif /* __MLX5_PRISM_FMT_H */
