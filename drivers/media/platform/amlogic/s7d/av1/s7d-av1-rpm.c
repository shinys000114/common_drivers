// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>

#include "s7d-av1-rpm.h"

static s8 signed7(u16 value)
{
	return (value & 0x3f) - (value & 0x40);
}

int s7d_av1_rpm_check_sequence(const struct s7d_av1_rpm *rpm,
			      const struct s7d_av1_sequence *sequence)
{
	const u16 *w;
	int ret;

	if (!rpm || !sequence)
		return -EINVAL;
	ret = s7d_av1_admit_sequence(sequence);
	if (ret)
		return ret;
	w = rpm->word;
	if (w[0] != sequence->profile || w[1] != sequence->still_picture ||
	    w[2] != sequence->reduced_still_picture_header ||
	    w[3] != sequence->decoder_model_info_present ||
	    w[4] != sequence->max_frame_width || w[5] != sequence->max_frame_height ||
	    w[6] != sequence->frame_id_numbers_present ||
	    w[10] != sequence->enable_order_hint ||
	    w[56] != sequence->bit_depth ||
	    !!(w[57] & BIT(6)) != sequence->use_128x128_superblock ||
	    !!(w[57] & BIT(7)) != sequence->enable_ref_frame_mvs ||
	    !!(w[57] & BIT(15)) != sequence->enable_superres)
		return -EPROTO;
	if (sequence->frame_id_numbers_present &&
	    (w[7] != sequence->delta_frame_id_bits ||
	     (u32)w[8] + w[7] + 1 != sequence->frame_id_bits))
		return -EPROTO;
	if (sequence->enable_order_hint && w[9] + 1 != sequence->order_hint_bits)
		return -EPROTO;
	if (!!(w[93] & BIT(9)) != sequence->color_range ||
	    !!(w[93] & BIT(8)) != sequence->color_description_present)
		return -EPROTO;
	if (sequence->color_description_present &&
	    ((w[93] & 0xff) != sequence->color_primaries ||
	     (w[94] >> 8) != sequence->transfer_characteristics ||
	     (w[94] & 0xff) != sequence->matrix_coefficients))
		return -EPROTO;
	if (w[98] || w[99])
		return -EOPNOTSUPP;
	return 0;
}

int s7d_av1_rpm_read_frame(const struct s7d_av1_rpm *rpm,
			  const struct s7d_av1_sequence *sequence,
			  struct s7d_av1_header *header)
{
	struct s7d_av1_header next = { 0 };
	struct s7d_av1_dpb_frame *f = &next.frame;
	const u16 *w;
	u32 i;
	int ret;

	if (!header)
		return -EINVAL;
	ret = s7d_av1_rpm_check_sequence(rpm, sequence);
	if (ret)
		return ret;
	w = rpm->word;
	if (w[13] > 1)
		return -EPROTO;
	f->show_existing = w[13];
	f->existing_slot = w[33];
	if (f->show_existing) {
		if (f->existing_slot >= S7D_AV1_DPB_REFERENCES)
			return -EPROTO;
		/* display_frame_id is outside the firmware's valid 101 words. */
		if (sequence->frame_id_numbers_present)
			return -EOPNOTSUPP;
		f->show_frame = true;
		goto snapshot;
	}
	if (w[14] > S7D_AV1_SWITCH_FRAME || w[15] > 1 || w[16] > 1 ||
	    w[17] > 0xff || w[18] > 1 || w[20] > 1 || w[22] > 7 ||
	    w[23] > 1 || w[28] > 1 || w[29] > 1 || w[60] > 7 || (w[61] & ~7U) ||
	    w[72] > 63 || w[73] > 63 || w[74] > 63 || w[75] > 63 ||
	    !w[25] || w[25] != w[26] || !w[27] ||
	    w[26] > sequence->max_frame_width || w[27] > sequence->max_frame_height ||
	    (w[30] != 0 && w[30] != 8))
		return -EOPNOTSUPP;
	f->frame_type = w[14];
	f->show_frame = w[15];
	f->error_resilient = w[16];
	f->refresh_flags = w[17];
	f->showable_frame = w[18];
	f->current_frame_id = w[19];
	f->order_hint = w[21];
	f->primary_ref = w[22];
	f->short_signaling = w[23];
	f->width = w[26];
	f->decode_width = w[25];
	f->height = w[27];
	f->last_ref = w[31];
	f->golden_ref = w[32];
	f->bit_depth = w[56];
	for (i = 0; i < 7; i++) {
		f->remap[i] = w[34 + i];
		f->delta_frame_id_minus_1[i] = w[41 + i];
	}
	for (i = 0; i < 8; i++) {
		u16 packed = w[62 + i / 2] >> ((i & 1) * 8);

		f->ref_order_hint[i] = w[48 + i];
		next.filter_ref_delta[i] = signed7(packed);
		if (packed & BIT(7))
			next.filter_ref_update_mask |= BIT(i);
		next.segment_y[i] = w[77 + i];
		next.segment_c[i] = w[85 + i];
	}
	next.sequence_flags = w[57];
	next.sequence_flags2 = w[97];
	next.allow_ref_frame_mvs = w[29];
	next.sharpness = w[60];
	next.filter_delta_enabled = w[61] & BIT(0);
	next.filter_delta_update = w[61] & BIT(1);
	next.filter_delta_reset = w[61] & BIT(2);
	for (i = 0; i < 2; i++) {
		u16 packed = w[70] >> (i * 8);

		next.filter_mode_delta[i] = signed7(packed);
		if (packed & BIT(7))
			next.filter_mode_update_mask |= BIT(i);
	}
	for (i = 0; i < 4; i++)
		next.filter_level[i] = w[72 + i];
	next.segmentation = w[76];
snapshot:
	next.snapshot = *rpm;
	*header = next;
	return 0;
}
