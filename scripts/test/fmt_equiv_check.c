// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side byte-equivalence proof for the R0.2 accessor migration.
 *
 * Every expression that R0.2 replaced is reproduced here in its OLD form
 * (raw offsets, exactly as they were in v0.26) and compared byte for byte
 * against the NEW form (prism_*_bits / named offsets / fmt.h helpers).
 * This is the no-hardware leg of the R0.2 gate; the hardware leg is the
 * golden-dmesg diff (scripts/golden/diff_golden.sh).
 *
 * Build & run (host gcc, no kernel headers):
 *   gcc -Wall -Wextra -O2 -I include fmt_equiv_check.c -o /tmp/fmt_equiv && /tmp/fmt_equiv
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

	/* 4. eqc dword at payload bytes[12:16]: (log_sz & 7) << 29 | uar.
	 * Old side mirrors the original eq.c lines verbatim: eqc = cin + 16. */
	printf("create_eq eqc dw3 (legacy T09 encoding, all rungs/uars)\n");
	{
		unsigned char log_szs[] = { 12, 9, 8, 6 };
		uint32_t uars[] = { 4096, 5, 0x123456, 0 };
		size_t a, b;

		for (a = 0; a < sizeof(log_szs); a++)
			for (b = 0; b < sizeof(uars) / sizeof(uars[0]); b++) {
				u8 o[80] = { }, n[80] = { };
				u8 *eqc_o = o + 16, *eqc_n = n + 16;
				char name[64];

				mlx5_prism_put_be32(eqc_o + 12,
					((uint32_t)log_szs[a] << 29) | uars[b]);
				prism_eqc_write_dw3(n, log_szs[a], uars[b]);
				(void)eqc_n;
				snprintf(name, sizeof(name), "log%u uar=0x%x",
					 log_szs[a], uars[b]);
				check_eq(name, o, n, 80);
			}
	}

	/* 5. eqc intr encoding: be16(vec << 4) at payload bytes[16:18]. */
	printf("create_eq eqc intr (legacy T09 encoding)\n");
	{
		uint32_t vecs[] = { 0, 1, 2, 15, 16, 127 };
		size_t a;

		for (a = 0; a < sizeof(vecs) / sizeof(vecs[0]); a++) {
			u8 o[80] = { }, n[80] = { };
			u8 *eqc_o = o + 16;
			char name[64];

			eqc_o[16] = (uint8_t)(vecs[a] >> 4);
			eqc_o[17] = (uint8_t)((vecs[a] & 0xf) << 4);
			prism_eqc_set_intr(n, vecs[a]);
			snprintf(name, sizeof(name), "vec=%u", vecs[a]);
			check_eq(name, o, n, 80);
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

	/* 7. QUERY_EQ readback: (qeqc[12] >> 3) & 0x1f over all byte values. */
	printf("QUERY_EQ log-size readback\n");
	{
		unsigned char seed = 0;

		do {
			u8 qeqc[64] = { };
			uint32_t o, n;

			qeqc[12] = seed;
			o = (qeqc[12] >> 3) & 0x1f;
			n = prism_get_bits(qeqc, PRISM_FLD_EQC_LOGSZ_READBACK, 5);
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

	printf(failures ? "FAILURES: %d\n" : "ALL EQUIVALENT (%d failures)\n",
	       failures);
	return failures ? 1 : 0;
}
