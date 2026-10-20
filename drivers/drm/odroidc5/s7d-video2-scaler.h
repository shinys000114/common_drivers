/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VIDEO2_SCALER_H
#define __S7D_VIDEO2_SCALER_H

#include "s7d-video-scaler.h"

#define S7D_VIDEO2_PPS_SETUP_REG_COUNT 25
#define S7D_VIDEO2_SCALER_MAX_UPSCALE 1024
#define S7D_VIDEO2_SCALER_MAX_DOWNSCALE 16

struct s7d_video2_pps_state {
	struct s7d_video_scaler_reg setup[S7D_VIDEO2_PPS_SETUP_REG_COUNT];
	struct s7d_video_scaler_reg control;
	/* Immutable tables, loaded only while fetch and VENC are stopped. */
	const u32 *coefficients[2];
};

int s7d_video2_scaler_build(u32 width, u32 height, u32 output_w, u32 output_h,
			    struct s7d_video2_pps_state *out);

#endif
