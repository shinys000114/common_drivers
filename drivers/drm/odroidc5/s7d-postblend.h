/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_POSTBLEND_H
#define __S7D_POSTBLEND_H

#include "s7d-rdma.h"

struct drm_rect;

#define S7D_POSTBLEND_REG_COUNT 15

struct s7d_postblend_state {
	struct s7d_rdma_entry regs[S7D_POSTBLEND_REG_COUNT];
};

/* OSD1 < opaque VD1 < opaque VD2 < OSD2. NULL destinations disable videos.
 * VD1 and VD2 together consume the current OSD2 postblend input.
 * Preblend covers the cropped input at origin zero; postblend sets its position.
 * Shared alpha/dummy setup is masked while stopped. No register access;
 * errors leave state unchanged.
 */
int s7d_postblend_build_state(u32 width, u32 height,
			     const struct drm_rect *video_dst,
			     u32 video_width, u32 video_height,
			     const struct drm_rect *video2_dst, bool osd2_enabled,
			     struct s7d_postblend_state *state);

#endif
