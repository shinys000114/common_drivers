/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VP9_DMA_CONTEXT_H
#define S7D_VP9_DMA_CONTEXT_H

#include "s7d-vp9-context.h"

#define S7D_VP9_MV_BANKS 2U
#define S7D_VP9_WORKSPACE_BYTES 0x1a2000U
/* SAO MMU line buffers (vendor 8K spec) and the 4K frame map (0x1200 pages). */
#define S7D_VP9_MMU_VBH_BYTES 0xa000U
#define S7D_VP9_MMU_MAP_BYTES (0x1200U * 4U)
/*
 * Internal FBC buffers for NV12 CAPTURE with compressed references: one per
 * live picture, the eight reference slots and the picture being decoded.
 */
#define S7D_VP9_FBC_SLOTS 9U

struct regmap;
struct s7d_codec_session;
struct s7d_codec_surface;
struct s7d_vp9_input_result;
struct s7d_vp9_dma_context;

struct s7d_vp9_dma_sizes {
	u32 rpm_offset;
	u32 lmem_offset;
	u32 vbh_offset;
	u32 map_offset;
	u32 mv_offset;
	u32 mv_used_bytes;
	u32 mv_stride;
	u32 input_offset;
	u32 ring_bytes;
	u32 total_bytes;
	u32 allocation_bytes;
};

struct s7d_vp9_input_plan {
	u32 start;
	u32 end;
	u32 read_pointer;
	u32 write_pointer;
	u32 decode_size;
	u32 max_shift_bytes;
	u64 token;
};

int s7d_vp9_dma_sizes(u32 width, u32 height, size_t input_capacity,
		      struct s7d_vp9_dma_sizes *sizes);
/*
 * Owner serializes calls and joins executor, IRQ and DPB users before destroy.
 * A nonzero fbc_count allocates that many internal FBC buffers of fbc_bytes
 * (header, then body at fbc_header_bytes, 4 KiB aligned).
 */
struct s7d_vp9_dma_context *
s7d_vp9_dma_context_create_stream(struct s7d_codec_session *session,
				u32 width, u32 height, size_t input_capacity,
				u32 fbc_count, u32 fbc_header_bytes, u32 fbc_bytes,
				u64 lease_epoch);
int s7d_vp9_dma_context_destroy_stream(struct s7d_vp9_dma_context *context,
				     u64 lease_epoch);
const struct s7d_vp9_workspace *
s7d_vp9_dma_context_workspace(const struct s7d_vp9_dma_context *context);
int s7d_vp9_dma_context_mv_slot(const struct s7d_vp9_dma_context *context,
			      u64 lease_epoch, u32 bank,
			      struct s7d_vp9_region *region);
/* Internal FBC buffer @index: header and body; -ENOENT without them. */
int s7d_vp9_dma_context_fbc_slot(const struct s7d_vp9_dma_context *context,
				 u64 lease_epoch, u32 index,
				 struct s7d_vp9_region *header,
				 struct s7d_vp9_region *body);
/* Firmware header pause: map the current picture's contiguous body 1:1. */
int s7d_vp9_dma_context_map_body(struct s7d_vp9_dma_context *context,
				 u64 lease_epoch, u64 token, u32 body_address,
				 u32 pages);
/* Caller holds a live CAPTURE pin from this session and generation. */
int s7d_vp9_dma_context_bind_surface(const struct s7d_vp9_dma_context *context,
				   u64 lease_epoch,
				   struct s7d_codec_surface *surface,
				   const struct s7d_codec_layout *layout,
				   struct s7d_vp9_region region[2]);
int s7d_vp9_dma_context_stage(struct s7d_vp9_dma_context *context,
			     const struct s7d_vp9_input_result *input,
			     u64 lease_epoch, u64 token,
			     struct s7d_vp9_input_plan *plan);
/* Call before the first firmware or protocol MMIO attempt for this input. */
int s7d_vp9_dma_context_begin(struct s7d_vp9_dma_context *context,
			     u64 lease_epoch, u64 token);
/* The caller proves the CPU and fetch are stopped; four addresses only. */
int s7d_vp9_dma_context_program_input(struct s7d_vp9_dma_context *context,
				     struct regmap *map,
				     u64 lease_epoch, u64 token);
/* The caller proves firmware header pause and excludes an RPM writer. */
int s7d_vp9_dma_context_snapshot_rpm(struct s7d_vp9_dma_context *context,
				    u64 lease_epoch, u64 token,
				    struct s7d_vp9_rpm *rpm);
/* Initial token0, or this input's actual full-halt result and joined IRQ/work. */
int s7d_vp9_dma_context_mark_idle(struct s7d_vp9_dma_context *context,
				 u64 lease_epoch, u64 token, int halt_status,
				 bool irq_joined, bool decoded);

#endif
