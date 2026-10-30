/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_SEQUENCE_H
#define S7D_AV1_SEQUENCE_H

#include <linux/types.h>

#define S7D_AV1_OPERATING_POINTS 32U
#define S7D_AV1_SEQUENCE_MAX_BYTES 4096U
#define S7D_AV1_SELECT_TOOLS 2U

struct s7d_av1_operating_point {
	u32 decoder_buffer_delay;
	u32 encoder_buffer_delay;
	u16 idc;
	u8 level_idx;
	u8 tier;
	u8 initial_display_delay;
	bool decoder_model_present;
	bool low_delay_mode;
	bool initial_display_delay_present;
};

struct s7d_av1_sequence {
	struct s7d_av1_operating_point op[S7D_AV1_OPERATING_POINTS];
	u32 max_frame_width;
	u32 max_frame_height;
	u32 num_units_in_display_tick;
	u32 time_scale;
	u32 num_ticks_per_picture;
	u32 num_units_in_decoding_tick;
	u8 profile;
	u8 operating_points;
	u8 frame_width_bits;
	u8 frame_height_bits;
	u8 frame_id_bits;
	u8 delta_frame_id_bits;
	u8 order_hint_bits;
	u8 buffer_delay_bits;
	u8 buffer_removal_time_bits;
	u8 frame_presentation_time_bits;
	u8 force_screen_content_tools;
	u8 force_integer_mv;
	u8 bit_depth;
	u8 color_primaries;
	u8 transfer_characteristics;
	u8 matrix_coefficients;
	u8 chroma_sample_position;
	bool still_picture;
	bool reduced_still_picture_header;
	bool timing_info_present;
	bool equal_picture_interval;
	bool decoder_model_info_present;
	bool initial_display_delay_present;
	bool frame_id_numbers_present;
	bool use_128x128_superblock;
	bool enable_filter_intra;
	bool enable_intra_edge_filter;
	bool enable_interintra_compound;
	bool enable_masked_compound;
	bool enable_warped_motion;
	bool enable_dual_filter;
	bool enable_order_hint;
	bool enable_jnt_comp;
	bool enable_ref_frame_mvs;
	bool enable_superres;
	bool enable_cdef;
	bool enable_restoration;
	bool high_bitdepth;
	bool twelve_bit;
	bool monochrome;
	bool color_description_present;
	bool color_range;
	bool subsampling_x;
	bool subsampling_y;
	bool separate_uv_delta_q;
	bool film_grain_params_present;
};

/* Sequence OBU payload, including trailing bits; failure preserves output. */
int s7d_av1_parse_sequence(const u8 *payload, size_t bytes,
			   struct s7d_av1_sequence *sequence);
/* Initial NV12, Main profile, layer-zero admission; frame syntax is separate. */
int s7d_av1_admit_sequence(const struct s7d_av1_sequence *sequence);

#endif
