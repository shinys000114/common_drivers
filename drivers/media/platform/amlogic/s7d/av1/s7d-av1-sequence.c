// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/minmax.h>

#include "s7d-av1-sequence.h"

struct s7d_av1_bits {
	const u8 *data;
	u32 bits;
	u32 position;
	int error;
};

static u32 s7d_av1_read(struct s7d_av1_bits *bits, u32 count)
{
	u32 value = 0;

	if (bits->error)
		return 0;
	if (count > 32 || count > bits->bits - bits->position) {
		bits->error = -EINVAL;
		return 0;
	}
	while (count--) {
		u32 position = bits->position++;

		value = (value << 1) |
			((bits->data[position >> 3] >> (7 - (position & 7))) & 1);
	}
	return value;
}

static u32 s7d_av1_uvlc(struct s7d_av1_bits *bits)
{
	u32 zeros = 0;

	while (!bits->error && !s7d_av1_read(bits, 1)) {
		if (++zeros == 32) {
			bits->error = -EINVAL;
			return 0;
		}
	}
	return ((1U << zeros) - 1) + s7d_av1_read(bits, zeros);
}

static void s7d_av1_timing(struct s7d_av1_bits *bits,
			   struct s7d_av1_sequence *sequence)
{
	sequence->num_units_in_display_tick = s7d_av1_read(bits, 32);
	sequence->time_scale = s7d_av1_read(bits, 32);
	sequence->equal_picture_interval = s7d_av1_read(bits, 1);
	if (!sequence->num_units_in_display_tick || !sequence->time_scale) {
		bits->error = -EINVAL;
		return;
	}
	if (sequence->equal_picture_interval)
		sequence->num_ticks_per_picture = s7d_av1_uvlc(bits) + 1;
}

static void s7d_av1_decoder_model(struct s7d_av1_bits *bits,
				  struct s7d_av1_sequence *sequence)
{
	sequence->buffer_delay_bits = s7d_av1_read(bits, 5) + 1;
	sequence->num_units_in_decoding_tick = s7d_av1_read(bits, 32);
	sequence->buffer_removal_time_bits = s7d_av1_read(bits, 5) + 1;
	sequence->frame_presentation_time_bits = s7d_av1_read(bits, 5) + 1;
}

static void s7d_av1_operating_points(struct s7d_av1_bits *bits,
				     struct s7d_av1_sequence *sequence)
{
	u32 i;

	if (sequence->reduced_still_picture_header) {
		sequence->operating_points = 1;
		sequence->op[0].level_idx = s7d_av1_read(bits, 5);
		return;
	}
	sequence->timing_info_present = s7d_av1_read(bits, 1);
	if (sequence->timing_info_present) {
		s7d_av1_timing(bits, sequence);
		sequence->decoder_model_info_present = s7d_av1_read(bits, 1);
		if (sequence->decoder_model_info_present)
			s7d_av1_decoder_model(bits, sequence);
	}
	sequence->initial_display_delay_present = s7d_av1_read(bits, 1);
	sequence->operating_points = s7d_av1_read(bits, 5) + 1;
	for (i = 0; i < sequence->operating_points && !bits->error; i++) {
		struct s7d_av1_operating_point *op = &sequence->op[i];

		op->idc = s7d_av1_read(bits, 12);
		op->level_idx = s7d_av1_read(bits, 5);
		if (op->level_idx > 7)
			op->tier = s7d_av1_read(bits, 1);
		if (sequence->decoder_model_info_present) {
			op->decoder_model_present = s7d_av1_read(bits, 1);
			if (op->decoder_model_present) {
				op->decoder_buffer_delay =
					s7d_av1_read(bits, sequence->buffer_delay_bits);
				op->encoder_buffer_delay =
					s7d_av1_read(bits, sequence->buffer_delay_bits);
				op->low_delay_mode = s7d_av1_read(bits, 1);
			}
		}
		if (sequence->initial_display_delay_present) {
			op->initial_display_delay_present = s7d_av1_read(bits, 1);
			if (op->initial_display_delay_present)
				op->initial_display_delay = s7d_av1_read(bits, 4) + 1;
		}
	}
}

static void s7d_av1_geometry(struct s7d_av1_bits *bits,
			     struct s7d_av1_sequence *sequence)
{
	sequence->frame_width_bits = s7d_av1_read(bits, 4) + 1;
	sequence->frame_height_bits = s7d_av1_read(bits, 4) + 1;
	sequence->max_frame_width =
		s7d_av1_read(bits, sequence->frame_width_bits) + 1;
	sequence->max_frame_height =
		s7d_av1_read(bits, sequence->frame_height_bits) + 1;
	if (!sequence->reduced_still_picture_header)
		sequence->frame_id_numbers_present = s7d_av1_read(bits, 1);
	if (sequence->frame_id_numbers_present) {
		sequence->delta_frame_id_bits = s7d_av1_read(bits, 4) + 2;
		sequence->frame_id_bits = s7d_av1_read(bits, 3) + 1 +
			sequence->delta_frame_id_bits;
	}
}

static void s7d_av1_tools(struct s7d_av1_bits *bits,
			  struct s7d_av1_sequence *sequence)
{
	sequence->use_128x128_superblock = s7d_av1_read(bits, 1);
	sequence->enable_filter_intra = s7d_av1_read(bits, 1);
	sequence->enable_intra_edge_filter = s7d_av1_read(bits, 1);
	sequence->force_screen_content_tools = S7D_AV1_SELECT_TOOLS;
	sequence->force_integer_mv = S7D_AV1_SELECT_TOOLS;
	if (!sequence->reduced_still_picture_header) {
		sequence->enable_interintra_compound = s7d_av1_read(bits, 1);
		sequence->enable_masked_compound = s7d_av1_read(bits, 1);
		sequence->enable_warped_motion = s7d_av1_read(bits, 1);
		sequence->enable_dual_filter = s7d_av1_read(bits, 1);
		sequence->enable_order_hint = s7d_av1_read(bits, 1);
		if (sequence->enable_order_hint) {
			sequence->enable_jnt_comp = s7d_av1_read(bits, 1);
			sequence->enable_ref_frame_mvs = s7d_av1_read(bits, 1);
		}
		if (!s7d_av1_read(bits, 1))
			sequence->force_screen_content_tools = s7d_av1_read(bits, 1);
		if (sequence->force_screen_content_tools &&
		    !s7d_av1_read(bits, 1))
			sequence->force_integer_mv = s7d_av1_read(bits, 1);
		if (sequence->enable_order_hint)
			sequence->order_hint_bits = s7d_av1_read(bits, 3) + 1;
	}
	sequence->enable_superres = s7d_av1_read(bits, 1);
	sequence->enable_cdef = s7d_av1_read(bits, 1);
	sequence->enable_restoration = s7d_av1_read(bits, 1);
}

static void s7d_av1_color(struct s7d_av1_bits *bits,
			  struct s7d_av1_sequence *sequence)
{
	sequence->high_bitdepth = s7d_av1_read(bits, 1);
	if (sequence->profile == 2 && sequence->high_bitdepth)
		sequence->twelve_bit = s7d_av1_read(bits, 1);
	sequence->bit_depth = sequence->twelve_bit ? 12 :
		sequence->high_bitdepth ? 10 : 8;
	if (sequence->profile != 1)
		sequence->monochrome = s7d_av1_read(bits, 1);
	sequence->color_description_present = s7d_av1_read(bits, 1);
	sequence->color_primaries = 2;
	sequence->transfer_characteristics = 2;
	sequence->matrix_coefficients = 2;
	if (sequence->color_description_present) {
		sequence->color_primaries = s7d_av1_read(bits, 8);
		sequence->transfer_characteristics = s7d_av1_read(bits, 8);
		sequence->matrix_coefficients = s7d_av1_read(bits, 8);
	}
	if (sequence->monochrome) {
		sequence->color_range = s7d_av1_read(bits, 1);
		sequence->subsampling_x = true;
		sequence->subsampling_y = true;
		return;
	}
	if (sequence->color_primaries == 1 &&
	    sequence->transfer_characteristics == 13 &&
	    sequence->matrix_coefficients == 0) {
		sequence->color_range = true;
	} else {
		sequence->color_range = s7d_av1_read(bits, 1);
		if (sequence->profile == 0) {
			sequence->subsampling_x = true;
			sequence->subsampling_y = true;
		} else if (sequence->profile == 2) {
			if (sequence->bit_depth == 12) {
				sequence->subsampling_x = s7d_av1_read(bits, 1);
				if (sequence->subsampling_x)
					sequence->subsampling_y = s7d_av1_read(bits, 1);
			} else {
				sequence->subsampling_x = true;
			}
		}
		if (sequence->subsampling_x && sequence->subsampling_y)
			sequence->chroma_sample_position = s7d_av1_read(bits, 2);
	}
	sequence->separate_uv_delta_q = s7d_av1_read(bits, 1);
}

static int s7d_av1_sequence_valid(const struct s7d_av1_sequence *sequence)
{
	u32 i;

	if (sequence->frame_id_bits > 16 ||
	    (sequence->decoder_model_info_present &&
	     !sequence->num_units_in_decoding_tick))
		return -EINVAL;
	for (i = 0; i < sequence->operating_points; i++) {
		const struct s7d_av1_operating_point *op = &sequence->op[i];

		if ((op->level_idx >= 24 && op->level_idx != 31) ||
		    op->initial_display_delay > 10)
			return -EINVAL;
		if (op->idc && (!(op->idc & 0xff) || !(op->idc & 0xf00)))
			return -EINVAL;
	}
	if (!sequence->monochrome && sequence->matrix_coefficients == 0 &&
	    (sequence->subsampling_x || sequence->subsampling_y))
		return -EINVAL;
	if (sequence->profile == 0 && !sequence->monochrome &&
	    (!sequence->subsampling_x || !sequence->subsampling_y))
		return -EINVAL;
	if (sequence->profile == 2 && sequence->bit_depth != 12 &&
	    !sequence->monochrome && !sequence->subsampling_x)
		return -EINVAL;
	if (sequence->chroma_sample_position == 3)
		return -EINVAL;
	return 0;
}

int s7d_av1_parse_sequence(const u8 *payload, size_t bytes,
			   struct s7d_av1_sequence *sequence)
{
	struct s7d_av1_sequence next = { 0 };
	struct s7d_av1_bits bits = { .data = payload };
	int ret;

	if (!payload || !sequence || !bytes ||
	    bytes > S7D_AV1_SEQUENCE_MAX_BYTES)
		return -EINVAL;
	bits.bits = bytes * 8;
	next.profile = s7d_av1_read(&bits, 3);
	next.still_picture = s7d_av1_read(&bits, 1);
	next.reduced_still_picture_header = s7d_av1_read(&bits, 1);
	if (next.profile > 2 ||
	    (next.reduced_still_picture_header && !next.still_picture))
		return -EINVAL;
	s7d_av1_operating_points(&bits, &next);
	s7d_av1_geometry(&bits, &next);
	s7d_av1_tools(&bits, &next);
	s7d_av1_color(&bits, &next);
	next.film_grain_params_present = s7d_av1_read(&bits, 1);
	if (s7d_av1_read(&bits, 1) != 1)
		return -EINVAL;
	while (!bits.error && bits.position < bits.bits) {
		if (s7d_av1_read(&bits, 1))
			return -EINVAL;
	}
	if (bits.error)
		return bits.error;
	ret = s7d_av1_sequence_valid(&next);
	if (ret)
		return ret;
	*sequence = next;
	return 0;
}

int s7d_av1_admit_sequence(const struct s7d_av1_sequence *sequence)
{
	if (!sequence || !sequence->operating_points ||
	    sequence->operating_points > S7D_AV1_OPERATING_POINTS ||
	    !sequence->max_frame_width || !sequence->max_frame_height)
		return -EINVAL;
	/* Main profile 4:2:0, 8 or 10 bits: profile 0 cannot carry 12 bits. */
	if (sequence->profile ||
	    (sequence->bit_depth != 8 && sequence->bit_depth != 10) ||
	    sequence->monochrome || !sequence->subsampling_x ||
	    !sequence->subsampling_y || sequence->enable_superres ||
	    sequence->max_frame_width > 4096 ||
	    sequence->max_frame_height > 2304 ||
	    (sequence->op[0].idc && sequence->op[0].idc != 0x101) ||
	    /* 4096x2304 exceeds every level up to 5.3; 6.0 is the first to hold it. */
	    (sequence->op[0].level_idx > 16 && sequence->op[0].level_idx != 31))
		return -EOPNOTSUPP;
	return 0;
}

bool s7d_av1_sequence_same_coding(const struct s7d_av1_sequence *a,
				  const struct s7d_av1_sequence *b)
{
	u32 i;

	if (a->profile != b->profile || a->still_picture != b->still_picture ||
	    a->reduced_still_picture_header != b->reduced_still_picture_header ||
	    a->operating_points != b->operating_points ||
	    a->decoder_model_info_present != b->decoder_model_info_present ||
	    a->equal_picture_interval != b->equal_picture_interval ||
	    a->buffer_removal_time_bits != b->buffer_removal_time_bits ||
	    a->frame_presentation_time_bits != b->frame_presentation_time_bits ||
	    a->max_frame_width != b->max_frame_width ||
	    a->max_frame_height != b->max_frame_height ||
	    a->frame_width_bits != b->frame_width_bits ||
	    a->frame_height_bits != b->frame_height_bits ||
	    a->frame_id_numbers_present != b->frame_id_numbers_present ||
	    a->frame_id_bits != b->frame_id_bits ||
	    a->delta_frame_id_bits != b->delta_frame_id_bits ||
	    a->use_128x128_superblock != b->use_128x128_superblock ||
	    a->enable_filter_intra != b->enable_filter_intra ||
	    a->enable_intra_edge_filter != b->enable_intra_edge_filter ||
	    a->enable_interintra_compound != b->enable_interintra_compound ||
	    a->enable_masked_compound != b->enable_masked_compound ||
	    a->enable_warped_motion != b->enable_warped_motion ||
	    a->enable_dual_filter != b->enable_dual_filter ||
	    a->enable_order_hint != b->enable_order_hint ||
	    a->enable_jnt_comp != b->enable_jnt_comp ||
	    a->enable_ref_frame_mvs != b->enable_ref_frame_mvs ||
	    a->force_screen_content_tools != b->force_screen_content_tools ||
	    a->force_integer_mv != b->force_integer_mv ||
	    a->order_hint_bits != b->order_hint_bits ||
	    a->enable_superres != b->enable_superres || a->enable_cdef != b->enable_cdef ||
	    a->enable_restoration != b->enable_restoration ||
	    a->bit_depth != b->bit_depth || a->monochrome != b->monochrome ||
	    a->color_description_present != b->color_description_present ||
	    a->color_primaries != b->color_primaries ||
	    a->transfer_characteristics != b->transfer_characteristics ||
	    a->matrix_coefficients != b->matrix_coefficients ||
	    a->color_range != b->color_range || a->subsampling_x != b->subsampling_x ||
	    a->subsampling_y != b->subsampling_y ||
	    a->chroma_sample_position != b->chroma_sample_position ||
	    a->separate_uv_delta_q != b->separate_uv_delta_q ||
	    a->film_grain_params_present != b->film_grain_params_present)
		return false;
	for (i = 0; i < a->operating_points && i < S7D_AV1_OPERATING_POINTS; i++)
		if (a->op[i].idc != b->op[i].idc ||
		    a->op[i].decoder_model_present != b->op[i].decoder_model_present)
			return false;
	return true;
}

int s7d_av1_parse_intra_size(const u8 *payload, size_t bytes,
			     const struct s7d_av1_sequence *sequence,
			     u32 *width, u32 *height)
{
	struct s7d_av1_bits bits = { .data = payload };
	u32 frame_type, show_frame, refresh, tools, w, h, i;
	bool resilient, override;

	if (!payload || !sequence || !width || !height || !bytes)
		return -EINVAL;
	/* The size comes within the first bytes of the frame header. */
	bits.bits = min_t(size_t, bytes, 256) * 8;
	if (sequence->reduced_still_picture_header) {
		*width = sequence->max_frame_width;
		*height = sequence->max_frame_height;
		return 0;
	}
	if (s7d_av1_read(&bits, 1))
		return -EOPNOTSUPP;
	frame_type = s7d_av1_read(&bits, 2);
	if (frame_type != 0 && frame_type != 2)
		return -EOPNOTSUPP;
	show_frame = s7d_av1_read(&bits, 1);
	if (show_frame && sequence->decoder_model_info_present &&
	    !sequence->equal_picture_interval)
		s7d_av1_read(&bits, sequence->frame_presentation_time_bits);
	if (!show_frame)
		s7d_av1_read(&bits, 1);
	resilient = (!frame_type && show_frame) || s7d_av1_read(&bits, 1);
	/* disable_cdf_update, then the screen content and integer MV choices. */
	s7d_av1_read(&bits, 1);
	tools = sequence->force_screen_content_tools == S7D_AV1_SELECT_TOOLS ?
		s7d_av1_read(&bits, 1) : sequence->force_screen_content_tools;
	if (tools && sequence->force_integer_mv == S7D_AV1_SELECT_TOOLS)
		s7d_av1_read(&bits, 1);
	if (sequence->frame_id_numbers_present)
		s7d_av1_read(&bits, sequence->frame_id_bits);
	override = s7d_av1_read(&bits, 1);
	s7d_av1_read(&bits, sequence->order_hint_bits);
	if (sequence->decoder_model_info_present && s7d_av1_read(&bits, 1)) {
		/* Layered OBUs are refused: temporal and spatial id are 0. */
		for (i = 0; i < sequence->operating_points; i++) {
			const struct s7d_av1_operating_point *op = &sequence->op[i];

			if (op->decoder_model_present &&
			    (!op->idc || ((op->idc & BIT(0)) && (op->idc & BIT(8)))))
				s7d_av1_read(&bits, sequence->buffer_removal_time_bits);
		}
	}
	refresh = !frame_type && show_frame ? 0xff : s7d_av1_read(&bits, 8);
	if (refresh != 0xff && resilient && sequence->enable_order_hint)
		for (i = 0; i < 8; i++)
			s7d_av1_read(&bits, sequence->order_hint_bits);
	if (override) {
		w = s7d_av1_read(&bits, sequence->frame_width_bits) + 1;
		h = s7d_av1_read(&bits, sequence->frame_height_bits) + 1;
	} else {
		w = sequence->max_frame_width;
		h = sequence->max_frame_height;
	}
	if (bits.error)
		return bits.error;
	if (w > sequence->max_frame_width || h > sequence->max_frame_height)
		return -EBADMSG;
	*width = w;
	*height = h;
	return 0;
}
