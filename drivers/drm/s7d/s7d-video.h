/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VIDEO_H
#define __S7D_VIDEO_H

#include <linux/types.h>
#include <linux/bits.h>

struct drm_framebuffer;
struct drm_rect;

#define S7D_VIDEO_MAX_WIDTH 4096
#define S7D_VIDEO_MAX_HEIGHT 2160
#define S7D_VIDEO_REFLECT_X BIT(0)
#define S7D_VIDEO_REFLECT_Y BIT(1)

/*
 * Amlogic FBC (scatter layout): frame_addr[0] is the header address >> 4,
 * scope_x/y[0] the 32-pixel/4-line block scopes and [1] the pixel scopes.
 */
struct s7d_video_fbc {
	bool enabled;
	bool ten_bit;
	bool mem_saving;
	u16 width;
	u16 height;
};

struct s7d_video_state {
	u32 frame_addr[2];
	u32 stride[2];
	u32 scope_x[2];
	u32 scope_y[2];
	u32 format;
	u8 reflection;
	struct s7d_video_fbc fbc;
};

/* Formats and modifiers the VD1 Amlogic FBC decoder reads. */
bool s7d_video_fbc_modifier(u32 format, u64 modifier);

/* GEM DMA layout and even 16.16 crop; errors leave state unchanged. */
int s7d_video_build_state(struct drm_framebuffer *fb, const struct drm_rect *source,
			  u64 dma_mask, struct s7d_video_state *state);

#endif
