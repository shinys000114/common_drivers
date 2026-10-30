/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_OSD_H
#define __S7D_OSD_H

#include <linux/bits.h>
#include <linux/types.h>

struct drm_framebuffer;
struct drm_rect;

#define S7D_OSD_MAX_SIZE 8192
#define S7D_OSD_PITCH_ALIGN 64
#define S7D_OSD_REFLECT_X BIT(0)
#define S7D_OSD_REFLECT_Y BIT(1)

struct s7d_osd_state {
	u32 block_config;
	u32 frame_addr;
	u32 stride;
	u32 scope_x;
	u32 scope_y;
	u32 alpha_config;
};

/*
 * Pure calculation; leaves state unchanged on error.
 * source is a 16.16 DRM source rectangle; fb must be backed by GEM DMA.
 */
int s7d_osd_build_state(struct drm_framebuffer *fb, const struct drm_rect *source,
			u64 dma_mask, struct s7d_osd_state *state);

#endif
