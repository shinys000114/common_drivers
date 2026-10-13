/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_PROTOCOL_H
#define S7D_H264_PROTOCOL_H

#include <linux/types.h>
#include "s7d-h264-layout.h"

#define S7D_H264_CONFIG_REQUEST	0x11U
#define S7D_H264_RPM_WORDS	0x400U

struct s7d_h264_config_snapshot {
	u32 status;
	u32 scratch1;
	u32 scratch2;
	u32 scratchb;
	const void *rpm;
	size_t rpm_bytes;
};

struct s7d_h264_sequence {
	struct s7d_h264_sps sps;
	struct s7d_codec_layout layout;
};

struct s7d_h264_capture_plane {
	dma_addr_t dma;
	size_t bytes;
};

struct s7d_h264_canvas_plan {
	u32 index;
	u32 data_low;
	u32 data_high;
	u32 block32;
};

struct s7d_h264_capture_descriptor {
	u32 slot;
	u32 canvas_cfg0;
	u32 anc_offset;
	u32 anc_value;
	struct s7d_h264_canvas_plan canvas[2];
};

/* The caller supplies a completed, immutable firmware/LMEM snapshot. */
int s7d_h264_parse_config(const struct s7d_h264_config_snapshot *snapshot,
			  enum s7d_codec_capture_format format, u32 extra_surfaces,
			  struct s7d_h264_sequence *sequence);
/* Calculation only; the caller retains DMA/DPB refs before programming it. */
int s7d_h264_build_capture_descriptor(const struct s7d_codec_layout *layout,
				      u32 slot,
				      const struct s7d_h264_capture_plane planes[2],
				      struct s7d_h264_capture_descriptor *descriptor);

#endif
