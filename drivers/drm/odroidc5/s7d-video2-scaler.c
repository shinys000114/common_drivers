// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/math64.h>

#include "s7d-video2-scaler.h"
#include "s7d-filter.h"

#define SET(r, m, v) { .reg = (r), .mask = (m), .value = (v) }

static const u32 *s7d_video2_filter(u64 step, u32 *bank)
{
	*bank = 4;
	if (step >= (3 << 24))
		return s7d_filter_triangle;
	if (step >= (2 << 24)) {
		*bank = 3;
		return s7d_filter_bspline;
	}
	return s7d_filter_bicubic;
}

int s7d_video2_scaler_build(u32 width, u32 height, u32 output_w, u32 output_h,
			  struct s7d_video2_pps_state *out)
{
	/* Prepare geometry before enabling the independent classic PPS. */
	struct s7d_video2_pps_state next = {
		.setup = {
			SET(0x395c, GENMASK(15, 0), 0),
			SET(0x3945, 0x1fff1fff, 0),
			SET(0x3946, 0x1fff1fff, 0),
			SET(0x3947, GENMASK(12, 0), 0),
			SET(0x3948, GENMASK(27, 0), BIT(24)),
			SET(0x3949, GENMASK(24, 0), 0),
			SET(0x394a, GENMASK(24, 0), 0),
			SET(0x394b, GENMASK(24, 0), 0),
			SET(0x394c, GENMASK(24, 0), 0),
			/* Preserve the read-only progressive/interlace status bit. */
			SET(0x394d, GENMASK(18, 17) | GENMASK(15, 13) |
			    GENMASK(11, 8) | GENMASK(7, 5) | GENMASK(3, 0), 0x2424),
			SET(0x394e, U32_MAX, 0),
			SET(0x394f, 0x1fff1fff, 0),
			SET(0x3950, 0x1fff1fff, 0),
			SET(0x3951, GENMASK(12, 0), 0),
			SET(0x3952, GENMASK(27, 0), BIT(24)),
			SET(0x3953, GENMASK(24, 0), 0),
			SET(0x3954, GENMASK(24, 0), 0),
			SET(0x3955, GENMASK(24, 0), 0),
			SET(0x3956, GENMASK(24, 0), 0),
			SET(0x3957, GENMASK(23, 0), (3 << 20) | (8 << 16)),
			SET(0x395a, GENMASK(27, 0), (3 << 20) | (8 << 16)),
			SET(0x395b, 0xff77ff77, 0),
			SET(0x395e, GENMASK(10, 0), (2 << 7) | (2 << 4)),
			SET(0x3959, GENMASK(27, 16) | GENMASK(11, 1), 0x0fff0200),
			SET(0x1df0, 0x1fff1fff, 0),
			SET(0x395d, U32_MAX, 0x48f80000),
			SET(0x395f, GENMASK(15, 0), 0x48f8),
		},
		.control = SET(0x3958, BIT(24) | GENMASK(22, 15) | BIT(12) |
			       GENMASK(11, 8) | GENMASK(5, 4) | GENMASK(2, 0),
			       BIT(24) | BIT(16) | BIT(15) | 4),
	};
	u32 h_end, v_end, ratio, offset, skip, phase, bank;
	u64 h_ratio, v_ratio, h_step, v_step;
	bool pre_h, pre_v;
	static const u8 skip_code[] = { 0x24, 0x04, 0x68, 0x48, 0x28, 0x08 };

	if (!out || !width || !height || width > 2048 || height > 1080 ||
	    !output_w || !output_h || output_w > 4096 || output_h > 2160 ||
	    ((width | height | output_w | output_h) & 1))
		return -EINVAL;
	h_ratio = div_u64((u64)width << 24, output_w);
	v_ratio = div_u64((u64)height << 24, output_h);
	pre_h = h_ratio >= BIT(25) && width >= 8;
	pre_v = h_ratio >= BIT(25) && v_ratio >= BIT(25) && height >= 8;
	h_step = h_ratio >> pre_h;
	v_step = v_ratio >> pre_v;
	if (h_step > GENMASK(27, 0) || v_step > GENMASK(27, 0))
		return -ERANGE;
	if ((width != output_w && width < 4) || (height != output_h && height < 4))
		return -EINVAL;
	h_end = output_w - 1;
	v_end = output_h - 1;
	next.setup[2].value = (v_end << 16) | v_end;
	next.setup[3].value = v_end;
	next.setup[12].value = (h_end << 16) | h_end;
	next.setup[13].value = h_end;
	next.setup[24].value = (height << 16) | width;
	if (pre_h) {
		next.setup[22].value &= ~GENMASK(10, 7);
		next.setup[22].value |= (4 << 7) | BIT(2);
		next.control.value |= BIT(20);
	}
	if (pre_v) {
		next.setup[22].value &= ~GENMASK(6, 4);
		next.setup[22].value |= (4 << 4) | BIT(0);
		next.control.value |= BIT(19) | BIT(21);
	}
	if (width != output_w) {
		next.setup[14].value = h_step;
		if ((width >> pre_h) >= 8) {
			next.coefficients[1] = h_ratio <= BIT(24) ?
				s7d_filter_lanczos4_8tap : s7d_filter_lanczos2_8tap;
			next.coefficients[2] = next.coefficients[1] + S7D_FILTER_PHASES;
			next.setup[19].value = (3 << 20) | (8 << 16);
			bank = 8;
		} else {
			next.coefficients[1] = s7d_video2_filter(h_ratio, &bank);
			next.setup[19].value = (1 << 20) | (4 << 16);
		}
		next.setup[20].value = next.setup[19].value;
		next.control.value |= BIT(17) | (bank << 8);
	}
	if (height != output_h) {
		ratio = div_u64((u64)height << 18, output_h) >> pre_v;
		offset = ratio > BIT(18) ? ratio >> 10 : 0;
		skip = offset >> 10;
		phase = ((offset & 0x3ff) >> 2) & 0xff;
		next.setup[4].value = v_step;
		next.setup[9].value = (skip_code[skip] << 8) | skip_code[skip];
		next.setup[10].value = (phase << 24) | (phase << 8);
		next.control.value |= BIT(18);
		next.coefficients[0] = s7d_video2_filter(v_ratio, &bank);
		next.control.value = (next.control.value & ~GENMASK(2, 0)) | bank;
	}
	*out = next;
	return 0;
}
