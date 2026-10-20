/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_POSTBLEND_H
#define __S7D_POSTBLEND_H

#include "s7d-rdma.h"
#include "s7d-caps.h"

struct drm_rect;

#define S7D_POSTBLEND_REG_COUNT 15

struct s7d_postblend_state {
	struct s7d_rdma_entry regs[S7D_POSTBLEND_REG_COUNT];
};

/* Order up to three physical inputs using DRM's normalized zpos values.
 * OSD2 cursor and overlay share an input. NULL destinations disable videos.
 * Preblend covers the cropped input at origin zero; postblend sets its position.
 * Shared alpha/dummy setup is masked while stopped. No register access;
 * errors leave state unchanged.
 */
int s7d_postblend_build_state(u32 width, u32 height,
			     const struct s7d_display_resources *resources,
			     const u32 zpos[S7D_INPUT_COUNT],
			     const struct drm_rect *video_dst,
			     u32 video_width, u32 video_height,
			     const struct drm_rect *video2_dst,
			     struct s7d_postblend_state *state);

#endif
