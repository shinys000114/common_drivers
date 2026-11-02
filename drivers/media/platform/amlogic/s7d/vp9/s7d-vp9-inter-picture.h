/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VP9_INTER_PICTURE_H
#define S7D_VP9_INTER_PICTURE_H

#include "s7d-vp9-context.h"
#include "s7d-vp9-header-prefix.h"

#define S7D_VP9_MAX_PHYSICAL 24U
#define S7D_VP9_PHYSICAL_INVALID (~0U)

struct regmap;

/*
 * Valid means allocated with fixed geometry; decoded/pinned state is external.
 * plane[] is the NV12 output; with compressed references, header and body
 * are the picture's Amlogic FBC (zero bytes while it has none).
 */
struct s7d_vp9_hw_surface {
	struct s7d_vp9_region plane[2];
	struct s7d_vp9_region header;
	struct s7d_vp9_region body;
	u32 width;
	u32 height;
	bool valid;
};

struct s7d_vp9_picture_frame {
	const struct s7d_vp9_workspace *workspace;
	const struct s7d_vp9_header_prefix *header;
	const struct s7d_vp9_rpm *rpm;
	const struct s7d_codec_layout *layout;
	const struct s7d_vp9_hw_surface *surface;
	/* FBC references through the SAO MMU, NV12 by double write; else linear 8-bit. */
	bool compressed;
	u32 physical_count;
	u32 current_physical;
	u32 logical_to_physical[S7D_VP9_REFERENCE_SLOTS];
	/* Admission must have split/rejected superframes before this writer. */
	u32 coded_frames;
	/* Capacities; hardware uses the calculated compressed-MV span. */
	struct s7d_vp9_region current_mv;
	struct s7d_vp9_region previous_mv;
	u32 previous_width;
	u32 previous_height;
	bool previous_valid;
	bool previous_keyframe;
	bool previous_intra_only;
	bool previous_show_frame;
};

struct s7d_vp9_hw_picture_plan {
	struct s7d_vp9_picture picture;
	u32 anc2axi[S7D_VP9_MAX_PHYSICAL * 2];
	u32 reference_canvas[S7D_VP9_FRAME_REFERENCES];
	u32 reference_info[S7D_VP9_FRAME_REFERENCES][5];
	u32 physical_count;
	u32 current_y;
	u32 current_uv;
	u32 current_header;
	u32 y_bytes;
	u32 uv_bytes;
	u32 mv_above;
	u32 current_mv;
	u32 previous_mv;
	u32 previous_mv_end;
	u32 mv_bytes;
	bool intra;
	bool use_prev_frame_mvs;
	bool compressed;
};

int s7d_vp9_compressed_mv_size(u32 width, u32 height, u32 *bytes);
/* Bytes the hardware writes per picture in FBC (vendor compute_losless_comp_*). */
u32 s7d_vp9_fbc_body_bytes(u32 width, u32 height, u32 bit_depth);
u32 s7d_vp9_fbc_header_bytes(u32 width, u32 height);
/* Scatter header the SAO writes at CM_HEADER_START_ADDR: 32 bytes per 32x32. */
u32 s7d_vp9_fbc_scatter_bytes(u32 width, u32 height);
/* Pure preflight: failure leaves plan unchanged. */
int s7d_vp9_inter_picture_plan(const struct s7d_vp9_picture_frame *frame,
			      struct s7d_vp9_hw_picture_plan *plan);
/*
 * Immutable normalized inputs, exclusive prepared VP9 HEVC lease, header pause.
 * The caller pins current/ref/display surfaces and both MV buffers through DMA.
 * Protocol/context initialization and all start/status/mailbox/PM/IRQ work are
 * external. No MMIO occurs before complete preflight. First MMIO error stops
 * immediately; partial programming requires owner-proven full halt before reuse.
 */
int s7d_vp9_inter_picture_configure(struct regmap *map,
				   const struct s7d_vp9_picture_frame *frame);

#endif
