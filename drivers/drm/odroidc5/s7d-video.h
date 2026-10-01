/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VIDEO_H
#define __S7D_VIDEO_H

#include <linux/types.h>

struct drm_framebuffer;
struct drm_rect;

#define S7D_VIDEO_MAX_WIDTH 4096
#define S7D_VIDEO_MAX_HEIGHT 2160

struct s7d_video_state {
	u32 frame_addr[2];
	u32 stride[2];
	u32 scope_x[2];
	u32 scope_y[2];
	u32 format;
};

/* GEM DMA layout and even 16.16 crop; errors leave state unchanged. */
int s7d_video_build_state(struct drm_framebuffer *fb, const struct drm_rect *source,
			  u64 dma_mask, struct s7d_video_state *state);

#endif
