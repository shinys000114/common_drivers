/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VIDEO_SCALER_H
#define __S7D_VIDEO_SCALER_H

#include <linux/types.h>

#define S7D_VIDEO_SCALER_SETUP_COUNT 20
#define S7D_VIDEO_SCALER_COEF_BANKS 7
#define S7D_VIDEO_SCALER_COEF_PHASES 33
#define S7D_VIDEO_SCALER_COEF_PAIRS 2
#define S7D_VIDEO_SCALER_COEF_IDX_LUMA 0x5197
#define S7D_VIDEO_SCALER_COEF_DATA_LUMA 0x5198
#define S7D_VIDEO_SCALER_COEF_IDX_CHROMA 0x5199
#define S7D_VIDEO_SCALER_COEF_DATA_CHROMA 0x519a

struct s7d_video_scaler_reg {
	u32 reg;
	u32 mask;
	u32 value;
};

struct s7d_video_scaler_state {
	struct s7d_video_scaler_reg setup[S7D_VIDEO_SCALER_SETUP_COUNT];
	struct s7d_video_scaler_reg control;
	u32 src_width;
	u32 src_height;
	u32 dst_width;
	u32 dst_height;
	u32 enabled;
};

/* Pure even NV geometry; apply SAFA state and LUTs only with VENC/VD1 stopped. */
int s7d_video_scaler_build(u32 src_width, u32 src_height,
			 u32 dst_width, u32 dst_height,
			 struct s7d_video_scaler_state *out);
int s7d_video_scaler_check_mode(const struct s7d_video_scaler_state *state,
			      u32 width, u32 height, unsigned long pixel_rate,
			      unsigned long core_rate);
bool s7d_video_scaler_same_config(const struct s7d_video_scaler_state *a,
				const struct s7d_video_scaler_state *b);
int s7d_video_scaler_coefficient(u32 bank, u32 phase, u32 pair, u32 *out);

#endif
