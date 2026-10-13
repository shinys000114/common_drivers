/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VIDEO_PIPELINE_H
#define __S7D_VIDEO_PIPELINE_H

#include <linux/bits.h>
#include <drm/drm_rect.h>

#include "s7d-rdma.h"
#include "s7d-video.h"
#include "s7d-video-scaler.h"

#define S7D_VIDEO_SETUP_REG_COUNT 14
#define S7D_VIDEO_UPDATE_REG_COUNT 12

/* The only inherited GEN field that may enter a replay descriptor. */
#define S7D_VIDEO_GEN_REPLAY_PRESERVE_MASK BIT(31)

struct s7d_video_reg_setting {
	u32 reg;
	u32 mask;
	u32 value;
};

struct s7d_video_pipeline_state {
	struct s7d_video_reg_setting setup[S7D_VIDEO_SETUP_REG_COUNT];
	struct s7d_rdma_entry update[S7D_VIDEO_UPDATE_REG_COUNT];
	unsigned int update_count;
	struct s7d_video_reg_setting control;
	struct s7d_video_scaler_state scaler;
};

/*
 * Pure progressive 8-bit NV12/NV21 state; NULL layout disables VD1.
 * GEM bounds/fences belong to the plane and scanout owners. dst is clipped,
 * even, with identity or checked SAFA enlargement. Errors leave out unchanged.
 * Apply masked setup with VENC/VD1 stopped. Preserve clock/reset fields.
 * Append control.value with captured GEN[31] after replay updates/postblend;
 * never replay GEN busy, software-reset or manual-start bits.
 */
int s7d_video_build_pipeline(const struct s7d_video_state *layout,
			     const struct drm_rect *dst,
			     struct s7d_video_pipeline_state *out);

#endif
