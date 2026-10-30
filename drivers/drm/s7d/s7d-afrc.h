/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_AFRC_H
#define __S7D_AFRC_H

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>

/* Current native KMS output profile. */
#define S7D_AFRC_MAX_WIDTH 4096
#define S7D_AFRC_MAX_HEIGHT 2160
/* A SCAN paging tile is 16x4 coding units of 4x4 RGBA pixels. */
#define S7D_AFRC_TILE_WIDTH 64
#define S7D_AFRC_TILE_HEIGHT 16
#define S7D_AFRC_MODIFIER(cu) DRM_FORMAT_MOD_ARM_AFRC( \
	AFRC_FORMAT_MOD_CU_SIZE_P0(AFRC_FORMAT_MOD_CU_SIZE_##cu) | \
	AFRC_FORMAT_MOD_LAYOUT_SCAN)
/* GFCD AFRC_CTRL reg_afrc_cu_bits values. */
#define S7D_AFRC_CU_16 0
#define S7D_AFRC_CU_24 1
#define S7D_AFRC_CU_32 2

struct s7d_afrc_framebuffer {
	struct drm_framebuffer base;
	u32 afrc_size;
};

struct s7d_afrc_layout {
	u32 cu_bytes;
	u32 align;
	u32 size;
};

struct s7d_afrc_buffer {
	struct s7d_afrc_layout layout;
	u64 addr;
	u64 last_byte_addr;
	u32 cu;
};

/* SCAN, single plane (P12 = 0) modifier to cu_bits, or -EINVAL. */
int s7d_afrc_cu(u64 modifier);
/* Header-less fixed-rate payload of 64x16-aligned RGBA dimensions. */
int s7d_afrc_layout(u32 width, u32 height, u32 cu, struct s7d_afrc_layout *out);
/* Whole-buffer RGBA8888 checks; errors leave the output unchanged. */
int s7d_afrc_build_buffer(const struct drm_framebuffer *fb, u64 dma_mask,
			  struct s7d_afrc_buffer *out);

#endif
