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
 * ifc-numbered eqc fields (for reference and future experiments):
 * log_eq_size @99 (5 bits), uar_page @104 (24 bits), intr @140 (7 bits).
 */

/*
 * What the T09 QUERY_EQ readback actually reads: byte 12 bits[3:7] of the
 * eqc payload — NOT necessarily the ifc log_eq_size field (ifc dw-relative
 * bit numbering addresses different physical bytes; see progress.md R0.2).
 * The two are indistinguishable so far: the readback has only ever
 * returned 0 (the fw reports the 1-entry EQ it actually built).
 */
#define PRISM_FLD_EQC_LOGSZ_READBACK	(12 * 8 + 3)

/*
 * eqc dword at bytes[12:16] — the exact legacy T09 encoding, byte for byte.
 * prism_eqc_write_dw3() writes ((log_sz & 7) << 29) | uar as one be32.
 * Under ifc numbering that spells log_eq_size = (uar >> 3) & 0x1f = 0 with
 * uar = 4096 — i.e. THE 1-ENTRY EQ WAS LITERALLY REQUESTED, which would
 * re-explain the T09 "firmware built 1 entry despite log12 accepted"
 * finding without any firmware misbehaviour. DO NOT change these bytes
 * without a hardware experiment (queued for T10 prep, progress.md R0.2):
 * golden behavior depends on them.
 */
static inline void prism_eqc_write_dw3(u8 *in, u8 log_sz, u32 uar)
{
	mlx5_prism_put_be32(in + PRISM_OFF_EQC + 12,
			    ((u32)(log_sz & 7) << 29) | uar);
}

/*
 * eqc intr (vector binding) — exact legacy T09 encoding: be16 at eqc bytes
 * [16:18] carrying (vec << 4). Not the ifc intr bit range (140-146); the
 * line has never fired under this encoding (T09: events arrive with polling
 * semantics), so its correctness is untested. Revisit with T10.
 */
static inline void prism_eqc_set_intr(u8 *in, u32 vec)
{
	u8 *eqc = in + PRISM_OFF_EQC;

	eqc[16] = (u8)(vec >> 4);
	eqc[17] = (u8)((vec & 0xf) << 4);
}

#endif /* __MLX5_PRISM_FMT_H */
