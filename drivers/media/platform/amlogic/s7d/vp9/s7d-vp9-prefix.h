/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VP9_PREFIX_H
#define S7D_VP9_PREFIX_H

#include <linux/types.h>

#define S7D_VP9_MAX_SUPERFRAME_FRAMES 8U

struct s7d_vp9_frame_span {
	u32 offset;
	u32 size;
};

struct s7d_vp9_packet {
	u32 count;
	u32 payload_size;
	struct s7d_vp9_frame_span frame[S7D_VP9_MAX_SUPERFRAME_FRAMES];
};

struct s7d_vp9_keyframe_prefix {
	u32 width;
	u32 height;
	u32 render_width;
	u32 render_height;
	u32 prefix_bits;
	u8 color_space;
	bool full_range;
	bool error_resilient;
};

/* Raw standard VP9 packet; offsets exclude an optional superframe index. */
int s7d_vp9_split_packet(const u8 *data, size_t bytes, struct s7d_vp9_packet *packet);
/* Profile 0 visible keyframe through render_size, not full-header validation. */
int s7d_vp9_parse_keyframe_prefix(const u8 *data, size_t bytes,
				 struct s7d_vp9_keyframe_prefix *prefix);

#endif
