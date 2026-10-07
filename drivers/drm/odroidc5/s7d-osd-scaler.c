// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/math64.h>

#include "s7d-osd-scaler.h"

int s7d_osd_scaler_build(unsigned int index, u32 input_w, u32 input_h,
			 u32 output_w, u32 output_h,
			 struct s7d_osd_scaler_state *state)
{
	if (!state || index > 1 || !input_w || !input_h ||
	    input_w > 8192 || input_h > 8192 ||
	    !output_w || !output_h || output_w > 4096 || output_h > 4096)
		return -EINVAL;
	if (input_w == output_w && input_h == output_h) {
		*state = (struct s7d_osd_scaler_state) {0};
		return 0;
	}
	if (input_w < 4 || input_h < 4 ||
	    input_w > S7D_OSD_SCALER_LINEBUFFER || input_h > 2160 ||
	    output_h > 2160 || input_w > output_w || input_h > output_h ||
	    output_w > input_w * S7D_OSD_SCALER_MAX_UPSCALE ||
	    output_h > input_h * S7D_OSD_SCALER_MAX_UPSCALE)
		return -ERANGE;
	*state = (struct s7d_osd_scaler_state) {
		.input_size = ((input_w - 1) << 16) | (input_h - 1),
		.output_h = output_w - 1,
		.output_v = output_h - 1,
		.h_phase_step = div_u64((u64)input_w << 18, output_w) << 6,
		.v_phase_step = div_u64((u64)input_h << 20, output_h) << 4,
		.h_control = BIT(22) | BIT(8) | (4 << 3) | 4,
		.v_control = BIT(25) | BIT(24) | BIT(8) | (4 << 3) | 4,
		.enabled = true,
	};
	return 0;
}

bool s7d_osd_scaler_same(const struct s7d_osd_scaler_state *a,
			const struct s7d_osd_scaler_state *b)
{
	return a->input_size == b->input_size && a->output_h == b->output_h &&
		a->output_v == b->output_v && a->h_phase_step == b->h_phase_step &&
		a->v_phase_step == b->v_phase_step && a->h_control == b->h_control &&
		a->v_control == b->v_control && a->enabled == b->enabled;
}
