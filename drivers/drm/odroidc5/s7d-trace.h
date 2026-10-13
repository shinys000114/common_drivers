/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM s7d_display

#if !defined(_S7D_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _S7D_TRACE_H

#include <linux/tracepoint.h>

/* kind: 0=VIU vsync entry, 1=RDMA done, 2=threaded drain complete. */
TRACE_EVENT(s7d_frame,
	TP_PROTO(unsigned int kind, u32 encp, unsigned int sequence, int result),
	TP_ARGS(kind, encp, sequence, result),
	TP_STRUCT__entry(
		__field(unsigned int, kind)
		__field(u32, encp)
		__field(unsigned int, sequence)
		__field(int, result)
	),
	TP_fast_assign(
		__entry->kind = kind;
		__entry->encp = encp;
		__entry->sequence = sequence;
		__entry->result = result;
	),
	TP_printk("kind=%u line=%u pixel=%u field=%u sequence=%u result=%d",
		  __entry->kind, (__entry->encp >> 16) & 0x1fff,
		  __entry->encp & 0x1fff, __entry->encp >> 29,
		  __entry->sequence, __entry->result)
);

TRACE_EVENT(s7d_frame_idle,
	TP_PROTO(u32 before, u32 after, u32 fifo, u32 fifo2, u32 arbiter, bool vblank),
	TP_ARGS(before, after, fifo, fifo2, arbiter, vblank),
	TP_STRUCT__entry(
		__field(u32, before)
		__field(u32, after)
		__field(u32, fifo)
		__field(u32, fifo2)
		__field(u32, arbiter)
		__field(bool, vblank)
	),
	TP_fast_assign(
		__entry->before = before;
		__entry->after = after;
		__entry->fifo = fifo;
		__entry->fifo2 = fifo2;
		__entry->arbiter = arbiter;
		__entry->vblank = vblank;
	),
	TP_printk("before=%#x after=%#x fifo=%#x fifo2=%#x arbiter=%#x vblank=%u",
		  __entry->before, __entry->after, __entry->fifo,
		  __entry->fifo2, __entry->arbiter, __entry->vblank)
);
#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE s7d-trace
#include <trace/define_trace.h>
