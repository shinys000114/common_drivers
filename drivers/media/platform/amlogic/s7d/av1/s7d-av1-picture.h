/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_PICTURE_H
#define S7D_AV1_PICTURE_H

#include "s7d-av1-rpm.h"

struct regmap;
struct s7d_av1_reference_backing;
struct s7d_av1_reference_state;

struct s7d_av1_picture_input {
	const struct s7d_av1_workspace *workspace;
	const struct s7d_av1_sequence *sequence;
	const struct s7d_av1_header *header;
	const struct s7d_av1_dpb_plan *dpb;
	const struct s7d_av1_reference_backing *backing;
	const struct s7d_av1_reference_state *state;
	const struct s7d_av1_reference_state *primary;
	struct s7d_av1_region mmu_map;
	u32 backing_count;
};

struct s7d_av1_hw_picture_plan {
	u32 anc2axi[S7D_AV1_FRAME_SLOTS];
	u32 canvas[8];
	u32 reference_info[8][5];
	u32 mv_references[3];
	u32 mv_offsets[6];
	u32 mv_read[3];
	u32 cache_canvas[2];
	u32 body_bytes;
	u32 header_bytes;
	u32 y_address;
	u32 uv_address;
	u32 y_bytes;
	u32 uv_bytes;
	u32 current_mv;
	u32 header_address;
	u32 cdf_write;
	u32 cdf_read;
	u32 segment_write;
	u32 segment_read;
	u32 scale_enable;
	u32 mv_order;
	u32 sign_bias;
	u32 skip_mode;
	u32 grain_table;
	u32 lcu_total;
	u32 mv_count;
	u32 current_slot;
	u32 backing_count;
	bool inter;
	bool read_motion;
	bool restore_segment_features;
};

/* Pure preflight; immutable inputs and caller-pinned backing, output preserved. */
int s7d_av1_picture_plan(const struct s7d_av1_picture_input *input,
			 struct s7d_av1_hw_picture_plan *plan);
/*
 * Exclusive AV1M/core2/no-swap lease at a validated frame-header pause.
 * Partial MMIO failure requires full owner halt/join before backing reuse.
 * No DECODE/status, clock, reset, IRQ, allocation or input completion operation.
 */
int s7d_av1_picture_configure(struct regmap *map,
			      const struct s7d_av1_picture_input *input,
			      struct s7d_av1_reference_state *configured);

#endif
