/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_DMA_CONTEXT_H
#define S7D_AV1_DMA_CONTEXT_H

#include "s7d-av1-input.h"
#include "s7d-av1-workspace.h"

struct regmap;
struct s7d_codec_session;
struct s7d_av1_dma_context;

struct s7d_av1_dma_sizes {
	u32 workspace_bytes;
	u32 rpm_offset;
	u32 lmem_offset;
	u32 mmu_map_offset;
	u32 input_offset;
	u32 ring_bytes;
	u32 total_bytes;
	u32 allocation_bytes;
};

struct s7d_av1_input_plan {
	u32 start;
	u32 end;
	u32 read_pointer;
	u32 write_pointer;
	u32 decode_size;
	u32 max_shift_bytes;
	u64 token;
};

int s7d_av1_dma_sizes(u32 width, u32 height, size_t staging_capacity,
		      struct s7d_av1_dma_sizes *sizes);
/* Capacity is converted AMLV bytes, not OUTPUT bytesused or sizeimage. */
struct s7d_av1_dma_context *
s7d_av1_dma_context_create(struct s7d_codec_session *session,
			   u32 width, u32 height, size_t staging_capacity,
			   u64 lease_epoch);
/* Caller serializes calls and joins engine, IRQ, worker and DPB users. */
int s7d_av1_dma_context_destroy(struct s7d_av1_dma_context *context,
			       u64 lease_epoch);
const struct s7d_av1_workspace *
s7d_av1_dma_context_workspace(const struct s7d_av1_dma_context *context);
int s7d_av1_dma_context_mmu_region(const struct s7d_av1_dma_context *context,
				  u64 lease_epoch,
				  struct s7d_av1_region *region);
/* This does not admit sequence/tools; caller supplies a validated immutable TU. */
int s7d_av1_dma_context_stage(struct s7d_av1_dma_context *context,
			     const u8 *data, size_t bytes,
			     u64 lease_epoch, u64 token,
			     struct s7d_av1_input_plan *plan);
/* Before the first firmware or protocol MMIO attempt for this TU. */
int s7d_av1_dma_context_begin(struct s7d_av1_dma_context *context,
			     u64 lease_epoch, u64 token);
/* Caller proves CPU/fetch stopped; programs four stream addresses only. */
int s7d_av1_dma_context_program_input(struct s7d_av1_dma_context *context,
				     struct regmap *map,
				     u64 lease_epoch, u64 token);
/* Caller proves header pause and holds the body until verified full stop. */
int s7d_av1_dma_context_map_body(struct s7d_av1_dma_context *context,
				const struct s7d_av1_region *body, u32 pages,
				u64 lease_epoch, u64 token);
/* Caller proves the firmware is paused and cannot write RPM during snapshot. */
int s7d_av1_dma_context_snapshot_rpm(struct s7d_av1_dma_context *context,
				    u64 lease_epoch, u64 token, bool efficiency,
				    struct s7d_av1_rpm *rpm);
/* Actual DOS halt/join; completed describes the requested protocol operation. */
int s7d_av1_dma_context_mark_idle(struct s7d_av1_dma_context *context,
				 u64 lease_epoch, u64 token, int halt_status,
				 bool joined, bool completed);

#endif
