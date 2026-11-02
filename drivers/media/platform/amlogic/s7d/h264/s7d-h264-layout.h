/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_LAYOUT_H
#define S7D_H264_LAYOUT_H

#include "s7d-codec-layout.h"

struct s7d_h264_sps {
	u32 mb_width;
	/* Frame macroblock rows; crop offsets below are in SPS crop units. */
	u32 mb_height;
	u32 crop_left;
	u32 crop_right;
	u32 crop_top;
	u32 crop_bottom;
	u32 profile_idc;
	u32 level_idc;
	u32 max_num_ref_frames;
	u32 num_reorder_frames;
	u32 max_dec_frame_buffering;
	u32 chroma_format_idc;
	u32 bit_depth_luma;
	u32 bit_depth_chroma;
	struct s7d_codec_colour colour;
	bool frame_mbs_only;
	bool separate_colour_plane;
	bool constraint_set3;
	bool bitstream_restriction;
};

int s7d_h264_build_layout(const struct s7d_h264_sps *sps,
			 enum s7d_codec_capture_format format,
			 u32 extra_surfaces, struct s7d_codec_layout *layout);

#endif
