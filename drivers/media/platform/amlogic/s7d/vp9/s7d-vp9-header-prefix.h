/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VP9_HEADER_PREFIX_H
#define S7D_VP9_HEADER_PREFIX_H

#include <linux/types.h>

#define S7D_VP9_REFERENCE_SLOTS 8U
#define S7D_VP9_FRAME_REFERENCES 3U

struct s7d_vp9_reference_metadata {
	u32 width;
	u32 height;
	u32 render_width;
	u32 render_height;
	bool valid;
};

struct s7d_vp9_header_prefix {
	u32 width;
	u32 height;
	u32 render_width;
	u32 render_height;
	u32 prefix_bits;
	u8 refresh_flags;
	u8 reference_index[S7D_VP9_FRAME_REFERENCES];
	u8 existing_index;
	u8 reset_frame_context;
	/* Effective intra color; other picture color is outside this helper. */
	u8 color_space;
	bool sign_bias[S7D_VP9_FRAME_REFERENCES];
	bool keyframe;
	bool intra_only;
	bool show_frame;
	bool show_existing;
	bool error_resilient;
	bool size_from_reference;
	bool color_config_present;
	bool full_range;
};

/* Profile 0 through render_size only; never validates the complete frame. */
int s7d_vp9_parse_header_prefix(const u8 *data, size_t bytes,
		const struct s7d_vp9_reference_metadata references[8],
		struct s7d_vp9_header_prefix *prefix);

#endif
