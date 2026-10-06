/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_POSTBLEND_H
#define __S7D_POSTBLEND_H

#include "s7d-rdma.h"

struct drm_rect;

#define S7D_POSTBLEND_REG_COUNT 13

struct s7d_postblend_state {
	struct s7d_rdma_entry regs[S7D_POSTBLEND_REG_COUNT];
};

/* OSD1 < opaque VD1 < OSD2; NULL video_dst disables VD1.
 * Preblend covers the cropped input at origin zero; postblend sets its position.
 * Shared alpha/dummy setup is masked while stopped. No register access;
 * errors leave state unchanged.
 */
int s7d_postblend_build_state(u32 width, u32 height,
			     const struct drm_rect *video_dst, bool osd2_enabled,
			     struct s7d_postblend_state *state);

#endif
