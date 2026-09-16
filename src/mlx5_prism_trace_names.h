/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mlx5_prism: opcode name table for TP_printk — separate header because
 * the trace header is re-read by define_trace.h (TRACE_HEADER_MULTI_READ):
 * this file's ordinary include guard gives exactly one definition per TU
 * while staying visible to both expansion passes.
 */

#ifndef __MLX5_PRISM_TRACE_NAMES_H
#define __MLX5_PRISM_TRACE_NAMES_H

#include <linux/types.h>

/* Only the opcodes prism actually issues. */
static inline const char *prism_opcode_name(u16 opcode)
{
	switch (opcode) {
	case 0x100: return "QUERY_HCA_CAP";
	case 0x101: return "SET_HCA_CAP";
	case 0x102: return "INIT_HCA";
	case 0x103: return "TEARDOWN_HCA";
	case 0x104: return "ENABLE_HCA";
	case 0x105: return "DISABLE_HCA";
	case 0x107: return "QUERY_PAGES";
	case 0x108: return "MANAGE_PAGES";
	case 0x109: return "SET_HCA_CAP2";
	case 0x10a: return "QUERY_ISSI";
	case 0x10b: return "SET_ISSI";
	case 0x301: return "CREATE_EQ";
	case 0x302: return "DESTROY_EQ";
	case 0x303: return "QUERY_EQ";
	case 0x304: return "GEN_EQE";
	case 0x802: return "ALLOC_UAR";
	case 0x803: return "DEALLOC_UAR";
	case 0x805: return "ACCESS_REG";
	case 0x80d: return "NOP";
	default: return "?";
	}
}

#endif /* __MLX5_PRISM_TRACE_NAMES_H */
