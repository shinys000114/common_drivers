/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_CONTEXT_H
#define S7D_H264_CONTEXT_H

#include <linux/types.h>
#include "../common/s7d-codec-session.h"
#include "s7d-h264-layout.h"

struct regmap;
struct s7d_h264_context;
struct s7d_vdec_runtime;

struct s7d_h264_mv_layout {
	u32 mb_width;
	u32 mb_height;
	u32 slots;
	u32 slot_bytes;
	size_t bytes;
};

struct s7d_h264_mv_buffer {
	struct s7d_codec_dma *memory;
	dma_addr_t dma;
	struct s7d_h264_mv_layout layout;
};

/* The caller owns a live session; this allocates backing without MMIO. */
struct s7d_h264_context *
s7d_h264_context_create(struct s7d_codec_session *session);
/* Drain all users first; only the core can release or quarantine DMA backing. */
void s7d_h264_context_release(struct s7d_h264_context *context);
/* Borrowed 4 KiB backing; this is not a completed RPM snapshot. */
struct s7d_codec_dma *s7d_h264_context_lmem(struct s7d_h264_context *context);
/* Initial TEE frame context only, under a parked DOS lease after firmware ack. */
int s7d_h264_context_initialize(struct s7d_h264_context *context,
				struct regmap *map);

/* Backend transaction lock and live session/DOS lease cover every call. */
int s7d_h264_context_begin(struct s7d_h264_context *context, u64 input_token);
/* All CAPTURE descriptors and independent MV backing are ready before F2. */
int s7d_h264_context_set_sequence(struct s7d_h264_context *context,
				  struct regmap *map,
				  const struct s7d_h264_sps *sps,
				  const struct s7d_codec_layout *layout,
				  u32 config_scratch1, u32 mv_slots,
				  u32 capture_slots);
/* Actual SLICE01 pause; no action ACK or input completion is sent. */
int s7d_h264_context_save_slice(struct s7d_h264_context *context,
				struct regmap *map, u64 input_token,
				u64 write_token);
/* DOSpark and IRQ/work join precede this; local reset has not happened. */
int s7d_h264_context_save_completion(struct s7d_h264_context *context,
				     struct regmap *map, u64 input_token,
				     u64 write_token);
/* Matching DPB picture_done succeeded; count pictures, not publications. */
int s7d_h264_context_confirm_park(struct s7d_h264_context *context,
				  struct s7d_vdec_runtime *runtime,
				  u64 input_token, u64 write_token,
				  u64 park_epoch, u32 completed_pictures);
/* TEE ack, parked CPU and pinned DPB/backing precede config-only restore. */
int s7d_h264_context_restore(struct s7d_h264_context *context,
			     struct regmap *map, u64 previous_input,
			     u64 previous_write, u64 park_epoch,
			     u64 next_input);

/* The caller chooses an independent MV pool, not CAPTURE slot numbering. */
int s7d_h264_build_mv_layout(const struct s7d_codec_layout *layout, u32 slots,
			     struct s7d_h264_mv_layout *mv);
int s7d_h264_context_allocate_mv(struct s7d_h264_context *context,
				 const struct s7d_codec_layout *layout, u32 slots,
				 struct s7d_h264_mv_buffer *buffer);
/* Current and references must share the same direct_8x8 representation. */
int s7d_h264_mv_slot(const struct s7d_h264_mv_buffer *buffer, u32 slot,
		     bool direct_8x8, u32 *address, u32 *bytes);

#endif
