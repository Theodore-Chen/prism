/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mlx5_prism: tracepoints — the primary observability interface
 * (DESIGN.md v2 §9). Landed before T10's event mode on purpose: new
 * interrupt paths get instrumented before they get enabled.
 *
 * Zero behaviour change: tracepoints are no-ops until a tracer attaches.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mlx5_prism

#include "mlx5_prism_trace_names.h"

#if !defined(__MLX5_PRISM_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define __MLX5_PRISM_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(prism_cmdq_issue,
	TP_PROTO(u16 opcode, u16 op_mod, u8 slot, u16 token, bool page_queue),
	TP_ARGS(opcode, op_mod, slot, token, page_queue),
	TP_STRUCT__entry(
		__field(u16, opcode)
		__field(u16, op_mod)
		__field(u8, slot)
		__field(u16, token)
		__field(bool, page_queue)
	),
	TP_fast_assign(
		__entry->opcode = opcode;
		__entry->op_mod = op_mod;
		__entry->slot = slot;
		__entry->token = token;
		__entry->page_queue = page_queue;
	),
	TP_printk("opcode=0x%04x[%s] op_mod=0x%x slot=%u token=%u%s",
		  __entry->opcode, prism_opcode_name(__entry->opcode),
		  __entry->op_mod, __entry->slot, __entry->token,
		  __entry->page_queue ? " (page)" : "")
);

TRACE_EVENT(prism_cmdq_complete,
	TP_PROTO(u16 opcode, u8 slot, u8 delivery, u8 status,
		 u32 syndrome, u64 latency_ns, int err),
	TP_ARGS(opcode, slot, delivery, status, syndrome, latency_ns, err),
	TP_STRUCT__entry(
		__field(u16, opcode)
		__field(u8, slot)
		__field(u8, delivery)
		__field(u8, status)
		__field(u32, syndrome)
		__field(u64, latency_ns)
		__field(int, err)
	),
	TP_fast_assign(
		__entry->opcode = opcode;
		__entry->slot = slot;
		__entry->delivery = delivery;
		__entry->status = status;
		__entry->syndrome = syndrome;
		__entry->latency_ns = latency_ns;
		__entry->err = err;
	),
	TP_printk("opcode=0x%04x[%s] slot=%u delivery=0x%x status=0x%x syndrome=0x%06x lat=%llu ns err=%d",
		  __entry->opcode, prism_opcode_name(__entry->opcode),
		  __entry->slot, __entry->delivery, __entry->status,
		  __entry->syndrome, __entry->latency_ns, __entry->err)
);

TRACE_EVENT(prism_eq_event,
	TP_PROTO(u32 eqn, u8 type, u32 vector, u32 cons_index),
	TP_ARGS(eqn, type, vector, cons_index),
	TP_STRUCT__entry(
		__field(u32, eqn)
		__field(u8, type)
		__field(u32, vector)
		__field(u32, cons_index)
	),
	TP_fast_assign(
		__entry->eqn = eqn;
		__entry->type = type;
		__entry->vector = vector;
		__entry->cons_index = cons_index;
	),
	TP_printk("eqn=%u type=0x%02x vector=0x%08x cons=%u",
		  __entry->eqn, __entry->type, __entry->vector,
		  __entry->cons_index)
);

TRACE_EVENT(prism_sq_xmit,
	TP_PROTO(u32 len, u16 ihs, u8 inline_all, u32 pc),
	TP_ARGS(len, ihs, inline_all, pc),
	TP_STRUCT__entry(
		__field(u32, len)
		__field(u16, ihs)
		__field(u8, inline_all)
		__field(u32, pc)
	),
	TP_fast_assign(
		__entry->len = len;
		__entry->ihs = ihs;
		__entry->inline_all = inline_all;
		__entry->pc = pc;
	),
	TP_printk("len=%u ihs=%u form=%s pc=%u",
		  __entry->len, __entry->ihs,
		  __entry->inline_all ? "all-inline" : "hdr-inline+ptr",
		  __entry->pc)
);

TRACE_EVENT(prism_rx_cqe,
	TP_PROTO(u32 byte_cnt, u16 wqe_id),
	TP_ARGS(byte_cnt, wqe_id),
	TP_STRUCT__entry(
		__field(u32, byte_cnt)
		__field(u16, wqe_id)
	),
	TP_fast_assign(
		__entry->byte_cnt = byte_cnt;
		__entry->wqe_id = wqe_id;
	),
	TP_printk("byte_cnt=%u wqe_id=%u", __entry->byte_cnt, __entry->wqe_id)
);

#endif /* __MLX5_PRISM_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE mlx5_prism_trace
#include <trace/define_trace.h>
