// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/overflow.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>

#include "s7d-afrc.h"

int s7d_afrc_cu(u64 modifier)
{
	switch (modifier) {
	case S7D_AFRC_MODIFIER(16):
		return S7D_AFRC_CU_16;
	case S7D_AFRC_MODIFIER(24):
		return S7D_AFRC_CU_24;
	case S7D_AFRC_MODIFIER(32):
		return S7D_AFRC_CU_32;
	default:
		return -EINVAL;
	}
}

int s7d_afrc_layout(u32 width, u32 height, u32 cu, struct s7d_afrc_layout *out)
{
	/* drm_fourcc.h: CU 16/24/32 bytes, plane alignment 1024/512/2048. */
	static const struct { u32 bytes, align; } units[] = {
		[S7D_AFRC_CU_16] = { 16, 1024 },
		[S7D_AFRC_CU_24] = { 24, 512 },
		[S7D_AFRC_CU_32] = { 32, 2048 },
	};

	if (!out || cu >= ARRAY_SIZE(units) || !width || !height ||
	    width > S7D_AFRC_MAX_WIDTH || height > S7D_AFRC_MAX_HEIGHT ||
	    !IS_ALIGNED(width, S7D_AFRC_TILE_WIDTH) ||
	    !IS_ALIGNED(height, S7D_AFRC_TILE_HEIGHT))
		return -EINVAL;
	/* GFCD takes no pitch: tiles are packed, 64 CUs of 4x4 pixels each. */
	*out = (struct s7d_afrc_layout) {
		.cu_bytes = units[cu].bytes,
		.align = units[cu].align,
		.size = width * height / 16 * units[cu].bytes,
	};
	return 0;
}

int s7d_afrc_build_buffer(const struct drm_framebuffer *fb, u64 dma_mask,
			  struct s7d_afrc_buffer *out)
{
	const struct drm_gem_dma_object *obj;
	struct s7d_afrc_buffer buffer;
	int cu, ret;

	if (!fb || !out || !fb->format ||
	    fb->format->format != DRM_FORMAT_RGBA8888 ||
	    fb->format->num_planes != 1 || !fb->obj[0] ||
	    fb->flags != DRM_MODE_FB_MODIFIERS || fb->offsets[0] ||
	    fb->pitches[0] < fb->width * 4)
		return -EINVAL;
	cu = s7d_afrc_cu(fb->modifier);
	if (cu < 0)
		return cu;
	ret = s7d_afrc_layout(fb->width, fb->height, cu, &buffer.layout);
	if (ret)
		return ret;
	obj = to_drm_gem_dma_obj(fb->obj[0]);
	if (buffer.layout.size > obj->base.size)
		return -EINVAL;
	buffer.addr = obj->dma_addr;
	buffer.cu = cu;
	if (check_add_overflow(buffer.addr, (u64)buffer.layout.size - 1,
			       &buffer.last_byte_addr))
		return -ERANGE;
	if (!IS_ALIGNED(buffer.addr, buffer.layout.align) ||
	    buffer.last_byte_addr > dma_mask ||
	    buffer.last_byte_addr > GENMASK_ULL(35, 0))
		return -ERANGE;
	*out = buffer;
	return 0;
}
