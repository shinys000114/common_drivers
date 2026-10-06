/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_AFBC_H
#define __S7D_AFBC_H

#include <drm/drm_fourcc.h>

#include "s7d-osd.h"

struct drm_afbc_framebuffer;
struct drm_rect;

#define S7D_AFBC_WIDTH 250
#define S7D_AFBC_HEIGHT 250
#define S7D_AFBC_SURFACE 1
#define S7D_AFBC_SURFACE_REG_COUNT 13
#define S7D_AFBC_MODIFIER DRM_FORMAT_MOD_ARM_AFBC( \
	AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 | AFBC_FORMAT_MOD_YTR | \
	AFBC_FORMAT_MOD_SPLIT | AFBC_FORMAT_MOD_SPARSE | \
	AFBC_FORMAT_MOD_TILED | AFBC_FORMAT_MOD_SC)

struct s7d_afbc_reg_setting {
	u32 reg;
	u32 mask;
	u32 value;
};

struct s7d_afbc_reg_value {
	u32 reg;
	u32 value;
};

struct s7d_afbc_state {
	struct s7d_osd_state osd;
	/* Decoder-owned settings; never replay through display RDMA. */
	struct s7d_afbc_reg_value regs[S7D_AFBC_SURFACE_REG_COUNT];
	struct s7d_afbc_reg_setting route;
	struct s7d_afbc_reg_setting unpack;
	u64 header_addr;
	u64 body_addr;
	u64 last_byte_addr;
	u32 header_size;
	u32 body_size;
	u32 afbc_size;
	u32 aligned_width;
	u32 aligned_height;
	u32 buffer_height;
	u32 surface_mask;
};

/* Full 16.16 source; metadata comes from drm_gem_fb_afbc_init(). */
int s7d_afbc_build_state(const struct drm_afbc_framebuffer *afbc_fb,
			 const struct drm_rect *source, u64 dma_mask,
			 unsigned int surface, struct s7d_afbc_state *out);
int s7d_afbc_check_state(const struct s7d_afbc_state *state);

#endif
