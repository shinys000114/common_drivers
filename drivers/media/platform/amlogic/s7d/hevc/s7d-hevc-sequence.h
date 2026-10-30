/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_SEQUENCE_H
#define S7D_HEVC_SEQUENCE_H

#include "../common/s7d-codec-layout.h"

#define S7D_HEVC_DOUBLE_WRITE_MODE 0x10U
#define S7D_HEVC_MAX_CAPTURE_BUFFERS 24U
#define S7D_HEVC_SPS_MAX_BYTES (64U * 1024U)
#define S7D_HEVC_ST_RPS_MAX 64U
#define S7D_HEVC_ST_RPS_ENTRIES 16U

struct s7d_hevc_st_rps {
	u32 count;
	s32 delta[S7D_HEVC_ST_RPS_ENTRIES];
	bool used[S7D_HEVC_ST_RPS_ENTRIES];
};

struct s7d_hevc_sps {
	u32 vps_id;
	u32 sps_id;
	u32 profile_idc;
	u32 level_idc;
	u32 max_sub_layers;
	u32 coded_width;
	u32 coded_height;
	u32 crop_left;
	u32 crop_right;
	u32 crop_top;
	u32 crop_bottom;
	u32 chroma_format_idc;
	u32 bit_depth_luma;
	u32 bit_depth_chroma;
	u32 log2_min_cb_size;
	u32 log2_ctb_size;
	u32 log2_max_poc_lsb;
	u32 max_dec_pic_buffering;
	u32 num_reorder_pics;
	u32 max_latency_increase_plus1;
	struct s7d_codec_colour colour;
	bool separate_colour_plane;
	bool interlaced_source;
	bool field_seq;
};

struct s7d_hevc_sequence {
	struct s7d_hevc_sps sps;
	u32 num_short_term_rps;
	struct s7d_hevc_st_rps rps[S7D_HEVC_ST_RPS_MAX];
	bool sao_enabled;
	bool temporal_mvp_enabled;
	bool long_term_refs_present;
	bool scaling_list_enabled;
	bool pcm_enabled;
};

/* Heap-backed caller output; retains bounded RPS and slice-header flags. */
int s7d_hevc_parse_sequence(const u8 *nal, size_t bytes,
			    struct s7d_hevc_sequence *sequence);
/* Linear, full-size DW0x10 storage; conformance offsets remain SPS units. */
int s7d_hevc_build_layout(const struct s7d_hevc_sps *sps,
			  enum s7d_codec_capture_format format,
			  u32 extra_surfaces, struct s7d_codec_layout *layout);

#endif
