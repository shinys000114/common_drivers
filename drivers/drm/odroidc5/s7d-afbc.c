// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/align.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/wordpart.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_rect.h>

#include "s7d-afbc.h"

#define REG(r, v) { .reg = (r), .value = (v) }
/* AFBC_EN is a software flag, not a decoder FORMAT_SPECIFIER field. */
#define AFBC_FORMAT (BIT(18) | BIT(16) | BIT(9) | BIT(8) | 5)

const struct s7d_afbc_surface *s7d_afbc_surface_get(u32 mask)
{
	static const struct s7d_afbc_surface surfaces[] = {
		{
			.mask = BIT(0), .bank = 0x3a10, .alias = 0x01000000,
			.ctrl = 0x1a10, .fifo = 0x1a2b, .alpha = 0x1a2d,
			.unpack = 0x1a2f, .route_mask = BIT(4),
		},
		{
			.mask = BIT(1), .bank = 0x3a30, .alias = 0x02000000,
			.ctrl = 0x1a30, .fifo = 0x1a4b, .alpha = 0x1a4d,
			.unpack = 0x1abd, .route_mask = BIT(5),
		},
	};

	if (mask == BIT(0))
		return &surfaces[0];
	if (mask == BIT(1))
		return &surfaces[1];
	return NULL;
}

struct s7d_afbc_layout {
	u32 aligned_width;
	u32 aligned_height;
	u32 buffer_height;
	u32 pitch;
	u32 header_size;
	u32 body_size;
	u32 size;
};

static int afbc_layout(u32 width, u32 height, struct s7d_afbc_layout *layout)
{
	u64 n_blocks, header_size, body_size, size;
	u32 aligned_width, aligned_height, pitch;

	if (!width || !height || width > S7D_AFBC_MAX_WIDTH ||
	    height > S7D_AFBC_MAX_HEIGHT)
		return -EINVAL;
	aligned_width = ALIGN(width, 256);
	aligned_height = ALIGN(height, 64);
	if (check_mul_overflow(aligned_width, 4U, &pitch) ||
	    !IS_ALIGNED(pitch, 128) || (pitch >> 4) > 0xfff || pitch > 0xffff)
		return -EINVAL;
	n_blocks = (u64)aligned_width * aligned_height / 256;
	header_size = ALIGN(n_blocks * 16, 4096);
	body_size = n_blocks * 1024;
	if (check_add_overflow(header_size, body_size, &size) || size > U32_MAX)
		return -ERANGE;
	*layout = (struct s7d_afbc_layout) {
		.aligned_width = aligned_width,
		.aligned_height = aligned_height,
		.buffer_height = ALIGN(height, 8),
		.pitch = pitch,
		.header_size = header_size,
		.body_size = body_size,
		.size = size,
	};
	return 0;
}

int s7d_afbc_check_state(const struct s7d_afbc_state *p)
{
	const struct s7d_afbc_surface *s = p ?
		s7d_afbc_surface_get(p->surface_mask) : NULL;
	struct s7d_afbc_layout layout;
	u32 settings[11], width, height, unpack_mask, unpack_value;
	unsigned int i;

	if (!s || (p->osd.scope_x & 0xffff) || (p->osd.scope_y & 0xffff))
		return -EINVAL;
	width = (p->osd.scope_x >> 16) + 1;
	height = (p->osd.scope_y >> 16) + 1;
	if (afbc_layout(width, height, &layout) ||
	    p->header_size != layout.header_size || p->body_size != layout.body_size ||
	    p->afbc_size != layout.size || p->aligned_width != layout.aligned_width ||
	    p->aligned_height != layout.aligned_height ||
	    p->buffer_height != layout.buffer_height ||
	    !IS_ALIGNED(p->header_addr, 64) ||
	    p->header_addr > GENMASK_ULL(35, 0) - (layout.size - 1) ||
	    p->body_addr != p->header_addr + p->header_size ||
	    p->last_byte_addr != p->body_addr + p->body_size - 1)
		return -EINVAL;
	unpack_mask = BIT(31) | GENMASK(15, 0);
	unpack_value = BIT(31) | 0x1234;
	if (p->osd.alpha_config == (BIT(2) | BIT(1)) && s->mask == BIT(0)) {
		unpack_mask |= BIT(28) | GENMASK(25, 24);
		unpack_value |= BIT(28);
	} else if (p->osd.alpha_config != 0x7fc2) {
		return -EINVAL;
	}
	if (p->osd.block_config != (BIT(30) | (5 << 8)) ||
	    p->osd.frame_addr != (s->alias >> 4) ||
	    p->osd.stride != layout.pitch >> 4 ||
	    p->route.reg != 0x1a0e || p->route.mask != s->route_mask ||
	    p->route.value != s->route_mask || p->unpack.reg != s->unpack ||
	    p->unpack.mask != unpack_mask ||
	    p->unpack.value != unpack_value)
		return -EINVAL;
	for (i = 0; i < S7D_AFBC_SURFACE_REG_COUNT; i++)
		if (p->regs[i].reg != s->bank + i)
			return -EINVAL;
	if (p->regs[0].value != lower_32_bits(p->header_addr) ||
	    p->regs[1].value != upper_32_bits(p->header_addr))
		return -EINVAL;
	memcpy(settings, (u32[]) {
		AFBC_FORMAT, layout.aligned_width, layout.buffer_height,
		0, width - 1, 0, height - 1, s->alias, 0, layout.pitch, 0,
	}, sizeof(settings));
	for (i = 2; i < S7D_AFBC_SURFACE_REG_COUNT; i++)
		if (p->regs[i].value != settings[i - 2])
			return -EINVAL;
	return 0;
}

int s7d_afbc_build_state(const struct drm_afbc_framebuffer *afbc_fb,
			 const struct drm_rect *src, u64 dma_mask,
			 unsigned int surface, struct s7d_afbc_state *out)
{
	const struct drm_gem_dma_object *obj;
	const struct drm_framebuffer *fb;
	const struct s7d_afbc_surface *s;
	struct s7d_afbc_layout layout;
	u64 header_addr, dma_end;
	int ret;

	if (!afbc_fb || !src || !out || surface > 1)
		return -EINVAL;
	s = s7d_afbc_surface_get(BIT(surface));
	fb = &afbc_fb->base;
	if (!fb->format || fb->format->format != DRM_FORMAT_ABGR8888 ||
	    fb->format->num_planes != 1 || !fb->obj[0] ||
	    fb->modifier != S7D_AFBC_MODIFIER ||
	    fb->flags != DRM_MODE_FB_MODIFIERS ||
	    fb->offsets[0] || afbc_fb->offset != fb->offsets[0] ||
	    afbc_fb->block_width != 32 || afbc_fb->block_height != 8)
		return -EINVAL;
	ret = afbc_layout(fb->width, fb->height, &layout);
	if (ret)
		return ret;
	if (afbc_fb->aligned_width != layout.aligned_width ||
	    afbc_fb->aligned_height != layout.aligned_height ||
	    afbc_fb->afbc_size != layout.size || fb->pitches[0] != layout.pitch)
		return -EINVAL;
	if (src->x1 || src->y1 || src->x2 != (int)(fb->width << 16) ||
	    src->y2 != (int)(fb->height << 16))
		return -EINVAL;
	obj = to_drm_gem_dma_obj(fb->obj[0]);
	if (layout.size > obj->base.size)
		return -EINVAL;
	header_addr = obj->dma_addr;
	if (check_add_overflow(header_addr, layout.size - 1, &dma_end))
		return -ERANGE;
	if (!IS_ALIGNED(header_addr, 64) || dma_end > dma_mask ||
	    dma_end > GENMASK_ULL(35, 0))
		return -ERANGE;

	*out = (struct s7d_afbc_state) {
		.osd = {
			.block_config = BIT(30) | (5 << 8),
			.frame_addr = s->alias >> 4,
			.stride = layout.pitch >> 4,
			.scope_x = (fb->width - 1) << 16,
			.scope_y = (fb->height - 1) << 16,
			.alpha_config = 0x7fc2,
		},
		.regs = {
			REG(s->bank, lower_32_bits(header_addr)),
			REG(s->bank + 1, upper_32_bits(header_addr)),
			REG(s->bank + 2, AFBC_FORMAT),
			REG(s->bank + 3, layout.aligned_width),
			REG(s->bank + 4, layout.buffer_height),
			REG(s->bank + 5, 0),
			REG(s->bank + 6, fb->width - 1),
			REG(s->bank + 7, 0),
			REG(s->bank + 8, fb->height - 1),
			REG(s->bank + 9, s->alias),
			REG(s->bank + 10, 0),
			REG(s->bank + 11, layout.pitch),
			REG(s->bank + 12, 0),
		},
		.route = {
			.reg = 0x1a0e,
			.mask = s->route_mask,
			.value = s->route_mask,
		},
		.unpack = {
			.reg = s->unpack,
			.mask = BIT(31) | GENMASK(15, 0),
			.value = BIT(31) | 0x1234,
		},
		.header_addr = header_addr,
		.body_addr = header_addr + layout.header_size,
		.last_byte_addr = dma_end,
		.header_size = layout.header_size,
		.body_size = layout.body_size,
		.afbc_size = layout.size,
		.aligned_width = layout.aligned_width,
		.aligned_height = layout.aligned_height,
		.buffer_height = layout.buffer_height,
		.surface_mask = s->mask,
	};
	return 0;
}

int s7d_afbc_set_premult(struct s7d_afbc_state *state)
{
	int ret = s7d_afbc_check_state(state);

	if (ret)
		return ret;
	if (state->surface_mask != BIT(0))
		return -EOPNOTSUPP;
	state->osd.alpha_config = BIT(2) | BIT(1);
	state->unpack.mask |= BIT(28) | GENMASK(25, 24);
	state->unpack.value |= BIT(28);
	return 0;
}
