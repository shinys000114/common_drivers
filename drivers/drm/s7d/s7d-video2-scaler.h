/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VIDEO2_SCALER_H
#define __S7D_VIDEO2_SCALER_H

#include <linux/bits.h>

#include "s7d-video-scaler.h"

#include "s7d-filter.h"

#define S7D_VIDEO2_PPS_SETUP_REG_COUNT 27
#define S7D_VIDEO2_SCALER_MAX_UPSCALE 1024
#define S7D_VIDEO2_SCALER_MAX_DOWNSCALE 32
#define S7D_VIDEO2_PPS_COEF_IDX 0x3943
#define S7D_VIDEO2_PPS_COEF_DATA 0x3944
/* Close, geometry, three filter tables and the control word. */
#define S7D_VIDEO2_SCALER_LIVE_COUNT \
	(S7D_VIDEO2_PPS_SETUP_REG_COUNT + 2 + 3 * (S7D_FILTER_PHASES + 1))

struct s7d_video2_pps_state {
	struct s7d_video_scaler_reg setup[S7D_VIDEO2_PPS_SETUP_REG_COUNT];
	struct s7d_video_scaler_reg control;
	/* Vertical, horizontal luma and horizontal chroma tables. */
	const u32 *coefficients[3];
};

/* Filter RAM index of a coefficients[] direction. */
static inline u32 s7d_video2_coef_index(unsigned int direction)
{
	return direction == 2 ? BIT(8) | BIT(7) : direction << 8;
}

int s7d_video2_scaler_build(u32 width, u32 height, u32 output_w, u32 output_h,
			    struct s7d_video2_pps_state *out);
/*
 * Add the masked writes, in the stopped preparation order, that move a
 * running VD2 PPS to pps: closed for a disabled VD2 (zero state), or the
 * geometry, filter tables and control of a built one.
 */
int s7d_video2_scaler_build_live(const struct s7d_video2_pps_state *pps,
				 struct s7d_rdma_live *live);

#endif
