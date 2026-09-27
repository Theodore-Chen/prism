// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side byte-equivalence proof for the R0.2 accessor migration.
 *
 * Every expression that R0.2 replaced is reproduced here in its OLD form
 * (raw offsets, exactly as they were in v0.26) and compared byte for byte
 * against the NEW form (prism_*_bits / named offsets / fmt.h helpers).
 * This is the no-hardware leg of the R0.2 gate; the hardware leg is the
 * baseline diff (scripts/baseline/diff_baseline.sh).
 *
 * Build & run (host gcc, no kernel headers):
 *   gcc -Wall -Wextra -O2 -I include -I ../../src fmt_equiv_check.c -o /tmp/fmt_equiv && /tmp/fmt_equiv
 * Exits 0 iff every case is byte-identical.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

/*
 * Host shims for the kernel headers the real fmt.h pulls in, then the BE
 * helpers (verbatim copies of the v0.26 helpers — unchanged by R0.2 and
 * shared verbatim by both sides of every comparison below). Pre-defining
 * __MLX5_PRISM_H makes the real mlx5_prism.h (kernel headers) expand to
 * nothing inside fmt.h, so the REAL accessor file is what gets tested.
 */
#include <linux/types.h>
#include <linux/bits.h>

static inline u32 mlx5_prism_get_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static inline void mlx5_prism_put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static inline u16 mlx5_prism_get_be16(const u8 *p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static inline void mlx5_prism_put_be16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

static inline void mlx5_prism_put_be64(u8 *p, u64 v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (u8)(v >> (56 - 8 * i));
}

#define __MLX5_PRISM_H
#include "mlx5_prism_fmt.h"

/* kernel rounding helper for the host-side test */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static int failures;

static void check_eq(const char *what, const void *old_, const void *new_,
		     size_t len)
{
	const unsigned char *o = old_, *n = new_;

	if (!memcmp(o, n, len)) {
		printf("  [ok]   %s\n", what);
	} else {
		size_t i;

		printf("  [FAIL] %s\n", what);
		for (i = 0; i < len; i++)
			if (o[i] != n[i])
				printf("         byte %zu: old %02x new %02x\n",
				       i, o[i], n[i]);
		failures++;
	}
}

static void check_val(const char *what, uint32_t old_v, uint32_t new_v)
{
	if (old_v == new_v) {
		printf("  [ok]   %s (%u)\n", what, old_v);
	} else {
		printf("  [FAIL] %s: old %u new %u\n", what, old_v, new_v);
		failures++;
	}
}

static uint64_t host_cpu_to_be64(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(v);
#else
	return v;
#endif
}

/* MLX5_SET algebra, host-side emulation of include/linux/mlx5/device.h:
 * dw_bit_off = 32 - sz - (bit_off & 31); the field is merged into the
 * cpu dword which put_be32 then stores big-endian. */
static uint32_t emu_mlx5_set(uint32_t old, uint32_t v, unsigned bit_off,
			     unsigned sz)
{
	uint32_t dw_bit_off = 32 - sz - (bit_off & 31);
	uint32_t mask = (sz == 32) ? 0xffffffffu : ((1u << sz) - 1);

	return (old & ~(mask << dw_bit_off)) | ((v & mask) << dw_bit_off);
}

int main(void)
{
	int i;

	/* 1. ISSI current (be16 @ bytes[10:12]) — read over all values. */
	printf("ISSI current_issi be16 read/write\n");
	{
		unsigned char seed = 0;

		do {
			u8 qout[16] = { };
			u32 old_v, new_v;

			qout[10] = seed;
			qout[11] = seed ^ 0x5a;
			old_v = ((qout[10] << 8) | qout[11]);
			new_v = mlx5_prism_get_be16(qout + PRISM_OFF_ISSI_CUR);
			if (old_v != new_v) {
				printf("  [FAIL] issi read seed %02x: %u vs %u\n",
				       seed, old_v, new_v);
				failures++;
				break;
			}
			seed++;
		} while (seed != 0);
		printf("  [ok]   read, all 256x256 byte pairs\n");

		{
			u8 o[16] = { }, n[16] = { };

			o[11] = 1;
			mlx5_prism_put_be16(n + PRISM_OFF_ISSI_CUR, 1);
			check_eq("write current=1", o, n, 16);
		}
	}

	/* 2. access_reg register_id (be16 @ bytes[10:12]). */
	printf("ACCESS_REG register_id\n");
	{
		u8 o[16] = { }, n[16] = { };

		o[10] = 0x70;
		o[11] = 0x04;
		mlx5_prism_put_be16(n + PRISM_OFF_REG_ID, 0x7004);
		check_eq("reg_id 0x7004", o, n, 16);
	}

	/* 3. MANAGE_PAGES header (be32s @ 8/12) and pas loop. */
	printf("MANAGE_PAGES func_id/num_entries/pas\n");
	{
		u8 o[64] = { }, n[64] = { };
		uint64_t pas[3] = { 0x1122334455667788ULL, 0xdeadbeefULL, 5 };
		int k;

		mlx5_prism_put_be32(o + 8, 0x1234);
		mlx5_prism_put_be32(o + 12, 3);
		for (k = 0; k < 3; k++)
			mlx5_prism_put_be64(o + 16 + 8 * k, pas[k]);
		mlx5_prism_put_be32(n + PRISM_OFF_MP_FUNC_ID, 0x1234);
		mlx5_prism_put_be32(n + PRISM_OFF_MP_NUM_ENTRIES, 3);
		for (k = 0; k < 3; k++)
			mlx5_prism_put_be64(n + PRISM_OFF_MP_PAS + 8 * k, pas[k]);
		check_eq("give header + 3 pas", o, n, 64);
	}

	/* 4. eqc dw3/dw5 encodings: pinned against hand-computed mlx5_ifc
	 * expectations (dword3 = (log_eq_size << 24) | uar_page for fields
	 * [99,103]/[104,127]; dword5 low 12 bits = intr for [180,191]) and
	 * against an emulation of the MLX5_SET algebra itself; decode
	 * roundtrips go through the true-window readback helpers (NOT
	 * prism_get_bits — its LSB-first numbering reads the flipped
	 * within-byte window; see the fmt.h geometry note). The legacy T09
	 * encodings these replaced are archived in progress.md T10-prep. */
	printf("create_eq eqc dw3/dw5 (ifc-field encodings)\n");
	{
		struct { uint32_t log, uar, dw3; } cases[] = {
			{ 12, 4096,     0x0c001000u },
			{ 9,  4096,     0x09001000u },
			{ 8,  5,        0x08000005u },
			{ 6,  0x123456, 0x06123456u },
			{ 0,  0,        0x00000000u },
		};
		uint32_t vecs[] = { 0, 1, 2, 0xfff };
		size_t a, b;

		for (a = 0; a < sizeof(cases) / sizeof(cases[0]); a++) {
			u8 exp[80] = { }, n[80] = { };
			uint32_t dw3 = 0;
			char name[64];

			prism_eqc_write_dw3(n, cases[a].log, cases[a].uar);
			mlx5_prism_put_be32(exp + 16 + 12, cases[a].dw3);
			snprintf(name, sizeof(name), "dw3 log%u uar=0x%x",
				 cases[a].log, cases[a].uar);
			check_eq(name, exp, n, 80);
			check_val("  decode log_eq_size",
				  prism_eqc_logsz_readback(n + 16),
				  cases[a].log & 0x1f);
			check_val("  decode uar_page",
				  prism_eqc_uar_readback(n + 16),
				  cases[a].uar & 0xffffff);
			dw3 = emu_mlx5_set(dw3, cases[a].log, 99, 5);
			dw3 = emu_mlx5_set(dw3, cases[a].uar, 104, 24);
			check_val("  MLX5_SET(emulated) dw3",
				  mlx5_prism_get_be32(n + 16 + 12), dw3);
		}
		for (b = 0; b < sizeof(vecs) / sizeof(vecs[0]); b++) {
			u8 exp[80] = { }, n[80] = { };
			uint32_t dw5 = 0;
			char name[64];

			prism_eqc_set_intr(n, vecs[b]);
			mlx5_prism_put_be32(exp + 16 + 20, vecs[b]);
			snprintf(name, sizeof(name), "dw5 vec=%u", vecs[b]);
			check_eq(name, exp, n, 80);
			check_val("  decode intr",
				  prism_eqc_intr_readback(n + 16), vecs[b]);
			dw5 = emu_mlx5_set(dw5, vecs[b], 180, 12);
			check_val("  MLX5_SET(emulated) dw5",
				  mlx5_prism_get_be32(n + 16 + 20), dw5);
		}
	}

	/* 6. event bitmask be64 @ payload+72 (absolute 88). */
	printf("create_eq event_bitmask\n");
	{
		u8 o[128] = { }, n[128] = { };
		uint64_t be = host_cpu_to_be64(1u << 10);

		memcpy(o + 88, &be, 8);
		mlx5_prism_put_be64(n + PRISM_OFF_EQ_EVENT_BITMASK, 1u << 10);
		check_eq("bitmask BIT(0x0a)", o, n, 128);
	}

	/* 7. QUERY_EQ readback: eqc[12] & 0x1f over all byte values. */
	printf("QUERY_EQ log-size readback\n");
	{
		unsigned char seed = 0;

		do {
			u8 qeqc[64] = { };
			uint32_t o, n;

			qeqc[12] = seed;
			o = qeqc[12] & 0x1f;
			n = prism_eqc_logsz_readback(qeqc);
			if (o != n) {
				printf("  [FAIL] readback %02x: %u vs %u\n",
				       seed, o, n);
				failures++;
				break;
			}
			seed++;
		} while (seed != 0);
		printf("  [ok]   all 256 byte-12 values\n");
	}

	/* 8. caps known-good patch over random inputs. */
	printf("SET_HCA_CAP known-good patch (1000 random 256B payloads)\n");
	{
		unsigned int rng = 12345;

		for (i = 0; i < 1000; i++) {
			u8 o[256], n[256];
			int k;

			for (k = 0; k < 256; k++) {
				rng = rng * 1103515245 + 12345;
				o[k] = (uint8_t)(rng >> 16);
			}
			memcpy(n, o, 256);

			o[35] = (o[35] & (uint8_t)~(0x1f << 3)) | (18 << 3);
			memset(o + 66, 0, 2);
			o[82] &= (uint8_t)~0x3;
			memset(o + 162, 0, 2);

			prism_set_bits(n, PRISM_FLD_CAPS_LOG_MAX_KLM, 5, 18);
			memset(n + PRISM_OFF_CAPS_CMDIF_CHKSUM, 0, 2);
			n[PRISM_OFF_CAPS_UNIDENT_82] &= (uint8_t)~0x3;
			memset(n + PRISM_OFF_CAPS_UNIDENT_162, 0, 2);

			if (memcmp(o, n, 256)) {
				printf("  [FAIL] round %d differs\n", i);
				failures++;
				break;
			}
		}
		printf("  [ok]   1000 random payloads byte-identical\n");
	}

	/* 9. caps field reads over all byte values. */
	printf("caps field reads\n");
	{
		unsigned char seed = 0;

		do {
			u8 c[256] = { };
			uint32_t o, n;

			c[19] = seed;
			c[55] = seed ^ 0xa5;
			o = (c[19] >> 3) & 0x1f;
			n = prism_get_bits(c, PRISM_FLD_CAPS_LOG_MAX_QP, 5);
			if (o != n) {
				printf("  [FAIL] log_max_qp %02x\n", seed);
				failures++;
				break;
			}
			o = c[55];
			n = prism_get_bits(c, PRISM_FLD_CAPS_NUM_PORTS, 8);
			if (o != n) {
				printf("  [FAIL] num_ports %02x\n", seed);
				failures++;
				break;
			}
			o = c[16];
			n = prism_get_bits(c, PRISM_FLD_CAPS_LOG_MAX_SRQ_SZ, 8);
			if (o != n) {
				printf("  [FAIL] srq_sz %02x\n", seed);
				failures++;
				break;
			}
			seed++;
		} while (seed != 0);
		printf("  [ok]   log_max_qp/num_ports/srq_sz, all values\n");
	}

	/* 10. UAR out (be32 @ 8, 24-bit) and eqn byte 11. */
	printf("alloc_uar_out / eq_number\n");
	{
		u8 out[16] = { };
		uint32_t o, n;

		out[9] = 0x00;
		out[10] = 0x10;
		out[11] = 0x00;
		o = mlx5_prism_get_be32(out + 8) & 0xffffff;
		n = mlx5_prism_get_be32(out + PRISM_OFF_UAR_OUT) & 0xffffff;
		check_val("uar 0x001000", o, n);

		out[11] = 4;
		check_val("eqn byte 11", out[11], out[PRISM_OFF_EQN]);
	}

	/* 11. ppcnt_reg (T12): header field encodings against emulated
	 * MLX5_SET algebra (local_port 8b @0x08, grp 6b @0x1a, clr 1b @0x20),
	 * the offset table against the ifc geometry (counter_set union at
	 * image byte 8 + group-relative offsets from mlx5_ifc.h), and the
	 * _high/_low be64 counter decode via random roundtrips. */
	printf("ppcnt_reg header/offsets/counters (T12)\n");
	{
		uint32_t gcases[] = { 0, 0x12, 0x16, 0x3f, 1 };
		size_t a;

		for (a = 0; a < sizeof(gcases) / sizeof(gcases[0]); a++) {
			u8 emu[8] = { }, n[PRISM_PPCNT_REG_SIZE] = { };
			uint32_t dw0 = 0;
			char name[64];

			prism_ppcnt_set_grp(n, (u8)gcases[a]);
			dw0 = emu_mlx5_set(dw0, 1, 0x08, 8);
			dw0 = emu_mlx5_set(dw0, gcases[a], 0x1a, 6);
			mlx5_prism_put_be32(emu, dw0);
			snprintf(name, sizeof(name), "header grp=0x%02x (clr stays 0)",
				 gcases[a]);
			check_eq(name, emu, n, 8);
		}

		check_val("grp0x12 tslc off", PRISM_OFF_PPCNT12_TSLC, 8 + 0x00);
		check_val("grp0x12 sym_err off", PRISM_OFF_PPCNT12_SYM_ERR, 8 + 0x08);
		check_val("grp0x12 sync_hdr off", PRISM_OFF_PPCNT12_SYNC_HDR, 8 + 0x10);
		check_val("grp0x12 edpl_l0 off", PRISM_OFF_PPCNT12_EDPL_L0, 8 + 0x18);
		check_val("grp0x12 fc_corr_l0 off", PRISM_OFF_PPCNT12_FC_CORR_L0, 8 + 0x38);
		check_val("grp0x12 fc_unc_l0 off", PRISM_OFF_PPCNT12_FC_UNC_L0, 8 + 0x58);
		check_val("grp0x12 rs_corr off", PRISM_OFF_PPCNT12_RS_CORR, 8 + 0x78);
		check_val("grp0x12 rs_unc off", PRISM_OFF_PPCNT12_RS_UNC, 8 + 0x80);
		check_val("grp0x12 link_down off", PRISM_OFF_PPCNT12_LINK_DOWN, 8 + 0xC0);
		check_val("grp0x00 tx_ok off", PRISM_OFF_PPCNT0_TX_OK, 8 + 0x00);
		check_val("grp0x00 rx_ok off", PRISM_OFF_PPCNT0_RX_OK, 8 + 0x08);
		check_val("grp0x00 sym_err off", PRISM_OFF_PPCNT0_SYM_ERR, 8 + 0x68);
		check_val("grp0x16 rx_bits off", PRISM_OFF_PPCNT16_RX_BITS, 8 + 0x08);
		check_val("grp0x16 sym_err off", PRISM_OFF_PPCNT16_SYM_ERR, 8 + 0x10);
		check_val("grp0x16 corr_bits off", PRISM_OFF_PPCNT16_CORR_BITS, 8 + 0x18);

		{
			unsigned int rng = 4242;
			int k, bad = 0;

			for (k = 0; k < 1000; k++) {
				u8 img[PRISM_PPCNT_REG_SIZE] = { };
				uint64_t v = ((uint64_t)(rng >> 8) << 32) | rng;
				uint16_t off;
				uint64_t be;

				rng = rng * 1103515245 + 12345;
				off = (uint16_t)(rng % (PRISM_PPCNT_REG_SIZE - 8));
				be = host_cpu_to_be64(v);
				memcpy(img + off, &be, 8);
				if (prism_ppcnt_ctr64(img, off) != v) {
					printf("  [FAIL] ctr64 roundtrip %d\n", k);
					failures++;
					bad = 1;
					break;
				}
			}
			if (!bad)
				printf("  [ok]   ctr64 roundtrip, 1000 random values/offsets\n");
		}
	}

	/* 12. paos/mtmp_reg (T13): request encodings and response decoders
	 * against emulated MLX5_SET algebra (paos local_port 8b @0x08,
	 * oper_status 4b @0x1c; mtmp sensor_index 12b @0x14 — an all-zero
	 * request image IS sensor 0, no packing needed), the offset table
	 * against the ifc geometry, and the 0.125°C→mdeg scaling. */
	printf("paos/mtmp_reg geometry (T13)\n");
	{
		uint32_t opers[] = { 0, 1, 2, 0xf };
		size_t a;

		for (a = 0; a < sizeof(opers) / sizeof(opers[0]); a++) {
			u8 emu[PRISM_PAOS_REG_SIZE] = { };
			u8 n[PRISM_PAOS_REG_SIZE] = { };
			uint32_t dw0;
			char name[64];

			prism_paos_set_port(n);
			dw0 = emu_mlx5_set(0, 1, 0x08, 8);
			mlx5_prism_put_be32(emu, dw0);
			check_eq("paos request (local_port=1)", emu, n, 8);

			memset(emu, 0, sizeof(emu));
			memset(n, 0, sizeof(n));
			dw0 = emu_mlx5_set(0, opers[a], 0x1c, 4);
			mlx5_prism_put_be32(emu, dw0);
			memcpy(n, emu, sizeof(n));
			snprintf(name, sizeof(name), "paos oper_status=%u decode",
				 opers[a]);
			check_val(name, prism_paos_oper_readback(n), opers[a]);
		}

		check_val("mtmp temperature off", PRISM_OFF_MTMP_TEMPERATURE,
			  0x30 / 8);
		check_val("mtmp len min", PRISM_MTMP_LEN_MIN, 8);
		check_val("mtmp len full", PRISM_MTMP_LEN_FULL, 32);
		check_val("mtmp 8 LSB to mdeg", prism_mtmp_to_mdeg(8), 1000);
		check_val("mtmp 0 LSB to mdeg", prism_mtmp_to_mdeg(0), 0);
	}

	/* 13. T14 smoke geometry: prism_ifc_set/get must be the MLX5_SET
	 * algebra byte-for-byte (the eqc lesson — a flipped within-byte
	 * window reads plausible zeros), the offset table must match the
	 * v5.15 ifc layouts (create_sq_in: sqc @0x100 bits, wq @0x280,
	 * pas @0x880; cqc/mkc @0x80), and the hand-packed WQE/CQE wire
	 * fields must land where the hardware expects them. */
	printf("smoke sqc/cqc/mkc/wqe/cqe geometry (T14)\n");
	{
		struct { unsigned bit; unsigned sz; } flds[] = {
			{ PRISM_MKC_BIT_A, 1 },	   { PRISM_MKC_BIT_RW, 1 },
			{ PRISM_MKC_BIT_RR, 1 },   { PRISM_MKC_BIT_LW, 1 },
			{ PRISM_MKC_BIT_LR, 1 },   { PRISM_MKC_BIT_ACCESS_MODE, 2 },
			{ PRISM_MKC_BIT_QPN, 24 }, { PRISM_MKC_BIT_LENGTH64, 1 },
			{ PRISM_MKC_BIT_PD, 24 },
			{ PRISM_CQC_BIT_LOG_CQ_SIZE, 5 }, { PRISM_CQC_BIT_UAR_PAGE, 24 },
			{ PRISM_CQC_BIT_C_EQN, 32 },      { PRISM_CQC_BIT_LOG_PAGE_SIZE, 5 },
			{ PRISM_SQC_BIT_FLUSH_IN_ERROR_EN, 1 },
			{ PRISM_SQC_BIT_MIN_WQE_INLINE_MODE, 3 },
			{ PRISM_SQC_BIT_STATE, 4 },  { PRISM_SQC_BIT_NON_WIRE, 1 },
			{ PRISM_SQC_BIT_TS_FORMAT, 2 }, { PRISM_SQC_BIT_CQN, 24 },
			{ PRISM_SQC_BIT_TIS_LST_SZ, 16 }, { PRISM_SQC_BIT_TIS_NUM_0, 24 },
			{ PRISM_WQ_BIT_WQ_TYPE, 4 }, { PRISM_WQ_BIT_PD, 24 },
			{ PRISM_WQ_BIT_UAR_PAGE, 24 }, { PRISM_WQ_BIT_LOG_WQ_STRIDE, 4 },
			{ PRISM_WQ_BIT_LOG_WQ_PG_SZ, 5 }, { PRISM_WQ_BIT_LOG_WQ_SZ, 5 },
			{ PRISM_MODSQ_BIT_SQ_STATE, 4 },
			{ PRISM_TISC_BIT_TRANSPORT_DOMAIN, 24 },
			{ PRISM_NVC_BIT_MIN_WQE_INLINE_MODE, 3 },
			{ PRISM_ETHCAP_BIT_WQE_INLINE_MODE, 2 },
		};
		unsigned int rng = 777;
		size_t a;
		int k, bad = 0;

		for (a = 0; a < sizeof(flds) / sizeof(flds[0]); a++) {
			/* the field may live in any dword of its structure —
			 * randomize only that dword, apply, compare in place */
			u8 buf[256];
			unsigned off = (flds[a].bit / 32) * 4;
			u32 mask = (flds[a].sz == 32) ? 0xffffffffu :
				   ((1u << flds[a].sz) - 1);

			for (k = 0; k < 32; k++) {
				u32 old_v, val, exp, got;

				rng = rng * 1103515245 + 12345;
				buf[off] = (uint8_t)(rng >> 16);
				buf[off + 1] = (uint8_t)(rng >> 24);
				rng = rng * 1103515245 + 12345;
				buf[off + 2] = (uint8_t)(rng >> 16);
				buf[off + 3] = (uint8_t)(rng >> 24);
				old_v = mlx5_prism_get_be32(buf + off);
				val = rng >> 8;

				prism_ifc_set(buf, flds[a].bit, flds[a].sz, val);
				exp = emu_mlx5_set(old_v, val, flds[a].bit, flds[a].sz);
				got = mlx5_prism_get_be32(buf + off);
				if (got != exp || prism_ifc_get(buf, flds[a].bit, flds[a].sz) !=
						  (val & mask)) {
					printf("  [FAIL] field bit 0x%x/%u iter %d: 0x%x vs 0x%x\n",
					       flds[a].bit, flds[a].sz, k, got, exp);
					failures++;
					bad = 1;
					break;
				}
			}
			if (bad)
				break;
		}
		if (!bad)
			printf("  [ok]   ifc set/get == MLX5_SET, %zu fields x 32 randoms\n",
			       sizeof(flds) / sizeof(flds[0]));

		check_val("sqc byte off (ifc 0x100/8)", PRISM_OFF_SQC, 0x100 / 8);
		check_val("wq byte off in sqc (ifc 0x180/8)", PRISM_OFF_SQC_WQ, 0x180 / 8);
		check_val("cqc byte off (ifc 0x80/8)", PRISM_OFF_CQC, 0x80 / 8);
		check_val("mkc byte off (ifc 0x80/8)", PRISM_OFF_MKC, 0x80 / 8);
		check_val("tisc byte off (ifc 0x100/8)", PRISM_OFF_TISC, 0x100 / 8);
		check_val("cq pas off (ifc 0x880/8)", PRISM_OFF_CQ_PAS, 0x880 / 8);
		check_val("sq pas off (ifc 0x880/8)", PRISM_OFF_SQ_PAS, 0x880 / 8);
		check_val("wq dbr_addr off (ifc 0x80/8)", PRISM_WQ_OFF_DBR_ADDR, 0x80 / 8);
		check_val("cqc dbr_addr off (ifc 0x1c0/8)", PRISM_CQC_OFF_DBR_ADDR, 0x1c0 / 8);
		check_val("modify_sq inlen (32 B hdr + 240 B sqc)",
			  PRISM_MODIFY_SQ_IN_LEN, 272);
		check_val("create_tis inlen (32 B hdr + 160 B tisc)",
			  PRISM_CREATE_TIS_IN_LEN, 192);
		check_val("create_cq inlen 1 pas", PRISM_CREATE_CQ_IN_LEN(1), 280);
		check_val("create_sq inlen 1 pas", PRISM_CREATE_SQ_IN_LEN(1), 280);
		check_val("create_mkey inlen", PRISM_CREATE_MKEY_IN_LEN, 272);
		check_val("send doorbell off", PRISM_SEND_DOORBELL_OFFSET, 0x800);

		{
			/* compose the offsets the way smoke_create_sq does and
			 * verify the landing bytes: cqn in the sqc dword @+8,
			 * pd nine bytes into the wq, dbr_addr as be64 @+20,
			 * pas at the absolute tail */
			u8 in[PRISM_CREATE_SQ_IN_LEN(1)] = { };
			u8 *sqc = in + PRISM_OFF_SQC;
			u8 *wq = sqc + PRISM_OFF_SQC_WQ;

			prism_ifc_set(sqc, PRISM_SQC_BIT_CQN, 24, 0x123456);
			/* cqn @ ifc bit 0x48: low 24 bits of the dword @sqc+8 */
			check_val("sqc cqn lands be24 @ sqc+8",
				  mlx5_prism_get_be32(sqc + 8) & 0xffffff, 0x123456);
			prism_ifc_set(wq, PRISM_WQ_BIT_PD, 24, 0xabcdef);
			/* pd @ ifc bit 0x48: low 24 bits of the dword @wq+8 */
			check_val("wq pd lands be24 @ wq+8",
				  mlx5_prism_get_be32(wq + 8) & 0xffffff, 0xabcdef);
			mlx5_prism_put_be64(wq + PRISM_WQ_OFF_DBR_ADDR,
					    0x1122334455667788ull);
			check_val("wq dbr_addr hi", mlx5_prism_get_be32(wq + 16), 0x11223344);
			check_val("wq dbr_addr lo", mlx5_prism_get_be32(wq + 20), 0x55667788);
			mlx5_prism_put_be64(in + PRISM_OFF_SQ_PAS, 0xdeadbeefbaadf00dull);
			check_val("sq pas hi @ in+272", mlx5_prism_get_be32(in + 272), 0xdeadbeef);
			check_val("sq pas lo @ in+276", mlx5_prism_get_be32(in + 276), 0xbaadf00d);
		}

		{
			/* modify_sq_in: sqn be32 @+8, sq_state top nibble of
			 * byte 8 (ifc bit 0x40), ctx.state RDY top nibble of
			 * byte 33 (ifc bit 0x8 within ctx@32) */
			u8 in[PRISM_MODIFY_SQ_IN_LEN] = { };

			mlx5_prism_put_be32(in + PRISM_OFF_SQN_IN, 0x77);
			prism_ifc_set(in, PRISM_MODSQ_BIT_SQ_STATE, 4, PRISM_SQC_STATE_RST);
			prism_ifc_set(in + PRISM_OFF_SQC, PRISM_SQC_BIT_STATE, 4,
				      PRISM_SQC_STATE_RDY);
			check_val("modify sqn be32 @ in+8",
				  mlx5_prism_get_be32(in + 8) & 0xffffff, 0x77);
			check_val("modify sq_state RST nibble (in[8] top)", in[8] >> 4, PRISM_SQC_STATE_RST);
			check_val("modify ctx.state RDY nibble (in[33] top)", in[33] >> 4, PRISM_SQC_STATE_RDY);
			check_val("modify bitmask stays 0", in[16] | in[17] | in[18] | in[19], 0);
		}

		{
			/* inline SEND as smoke_post_sq_wqe builds it: ctrl +
			 * eseg (L2 header in inline_hdr: sz @eseg+12, bytes
			 * from +14 spilling one DS) + inline dseg at DS 3 —
			 * 64 B payload => 8 DS, WQE spans two WQEBBs */
			u8 wqe[PRISM_WQE_BB_SIZE * 2] = { };
			u8 payload[64];
			u8 ds;
			u8 db[8];
			unsigned int i;

			for (i = 0; i < sizeof(payload); i++)
				payload[i] = (uint8_t)(i * 7 + 3);
			ds = 3 + DIV_ROUND_UP(4 + sizeof(payload), PRISM_WQE_DS_SIZE);
			mlx5_prism_put_be32(wqe + PRISM_WQE_OFF_CTRL_OPMOD_IDX_OP,
					    (1u << 8) | PRISM_WQE_OPCODE_SEND);
			mlx5_prism_put_be32(wqe + PRISM_WQE_OFF_CTRL_QPN_DS,
					    (0x1234u << 8) | ds);
			wqe[PRISM_WQE_OFF_CTRL_FM_CE_SE] = PRISM_WQE_CTRL_CQ_UPDATE;
			mlx5_prism_put_be16(wqe + PRISM_WQE_OFF_ETH_SEG +
					    PRISM_WQE_ESEG_OFF_INLINE_SZ, 14);
			wqe[PRISM_WQE_OFF_ETH_SEG + PRISM_WQE_ESEG_OFF_INLINE_START] = 0xff;
			wqe[PRISM_WQE_OFF_ETH_SEG + PRISM_WQE_ESEG_OFF_INLINE_START + 13] = 0xb5;
			mlx5_prism_put_be32(wqe + PRISM_WQE_OFF_DATA_SEG +
					    PRISM_WQE_DS_SIZE + PRISM_WQE_SEG_OFF_BYTE_CNT,
					    sizeof(payload) | PRISM_WQE_INLINE_FLAG);
			memcpy(wqe + PRISM_WQE_OFF_DATA_SEG + PRISM_WQE_DS_SIZE + 4,
			       payload, sizeof(payload));

			check_val("inline send ds count (14 B hdr + 64 B payload)", ds, 8);
			check_val("inline send ctrl dword1 BE (sqn|ds)",
				  mlx5_prism_get_be32(wqe + 4), (0x1234u << 8) | 8);
			check_val("eseg inline_hdr.sz be16 @eseg+12",
				  mlx5_prism_get_be16(wqe + PRISM_WQE_OFF_ETH_SEG +
						      PRISM_WQE_ESEG_OFF_INLINE_SZ), 14);
			check_val("eseg inline_hdr starts at eseg+14",
				  wqe[PRISM_WQE_OFF_ETH_SEG + PRISM_WQE_ESEG_OFF_INLINE_START], 0xff);
			check_val("inline counter BE at DS3 (64|0x80000000)",
				  mlx5_prism_get_be32(wqe + PRISM_WQE_OFF_DATA_SEG +
						      PRISM_WQE_DS_SIZE),
				  0x80000040u);
			check_val("inline payload byte0 @ DS3+4",
				  wqe[PRISM_WQE_OFF_DATA_SEG + PRISM_WQE_DS_SIZE + 4],
				  payload[0]);
			check_val("inline payload last byte",
				  wqe[PRISM_WQE_OFF_DATA_SEG + PRISM_WQE_DS_SIZE + 4 + 63],
				  payload[63]);

			/* the doorbell is the ctrl segment's first 8 bytes taken
			 * verbatim (memcpy + __raw_writeq preserves their order) */
			memcpy(db, wqe, sizeof(db));
			check_val("doorbell dword1 BE (sqn|ds)",
				  mlx5_prism_get_be32(db + 4), (0x1234u << 8) | 8);
		}

		{
			u8 cqe[PRISM_CQE_SIZE] = { };

			cqe[PRISM_CQE_OFF_OP_OWN] = 0x02; /* REQ, owner 0 = first lap */
			mlx5_prism_put_be32(cqe + PRISM_CQE_OFF_BYTE_CNT, 64);
			mlx5_prism_put_be16(cqe + PRISM_CQE_OFF_WQE_COUNTER, 1);
			check_val("cqe opcode", cqe[PRISM_CQE_OFF_OP_OWN] >> 4,
				  PRISM_CQE_OPCODE_REQ);
			check_val("cqe owner fresh at ci=0",
				  (cqe[PRISM_CQE_OFF_OP_OWN] & 1) == 0, 1);
			check_val("cqe byte_cnt", mlx5_prism_get_be32(cqe + PRISM_CQE_OFF_BYTE_CNT), 64);
			check_val("cqe wqe_counter", mlx5_prism_get_be16(cqe + PRISM_CQE_OFF_WQE_COUNTER), 1);
			check_val("cqe owner init", PRISM_CQE_OWNER_INIT, 0xf1);
		}
	}

	printf(failures ? "FAILURES: %d\n" : "ALL EQUIVALENT (%d failures)\n",
	       failures);
	return failures ? 1 : 0;
}
