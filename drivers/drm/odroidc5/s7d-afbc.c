// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/align.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/wordpart.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_rect.h>

#include "s7d-afbc.h"

#define REG(r, v) { .reg = (r), .value = (v) }
/* AFBC_EN is a software flag, not a decoder FORMAT_SPECIFIER field. */
#define AFBC_FORMAT (BIT(18) | BIT(16) | BIT(9) | BIT(8) | 5)
#define AFBC_BANK 0x3a30
#define AFBC_ALIAS 0x02000000

int s7d_afbc_check_state(const struct s7d_afbc_state *p)
{
	static const u32 settings[] = {
		AFBC_FORMAT, 256, 256, 0, 249, 0, 249, AFBC_ALIAS, 0, 1024, 0,
	};
	unsigned int i;

	if (!p || p->surface_mask != BIT(S7D_AFBC_SURFACE) ||
	    p->header_size != 4096 || p->body_size != 262144 ||
	    p->afbc_size != 266240 || p->aligned_width != 256 ||
	    p->aligned_height != 256 || p->buffer_height != 256 ||
	    !IS_ALIGNED(p->header_addr, 64) ||
	    p->header_addr > GENMASK_ULL(35, 0) - (266240 - 1) ||
	    p->body_addr != p->header_addr + p->header_size ||
	    p->last_byte_addr != p->body_addr + p->body_size - 1)
		return -EINVAL;
	if (p->osd.block_config != (BIT(30) | (5 << 8)) ||
	    p->osd.frame_addr != (AFBC_ALIAS >> 4) || p->osd.stride != 64 ||
	    p->osd.scope_x != (249 << 16) || p->osd.scope_y != (249 << 16) ||
	    p->osd.alpha_config != 0x7fc2 ||
	    p->route.reg != 0x1a0e || p->route.mask != BIT(5) ||
	    p->route.value != BIT(5) || p->unpack.reg != 0x1abd ||
	    p->unpack.mask != (BIT(31) | GENMASK(15, 0)) ||
	    p->unpack.value != (BIT(31) | 0x1234))
		return -EINVAL;
	for (i = 0; i < S7D_AFBC_SURFACE_REG_COUNT; i++)
		if (p->regs[i].reg != AFBC_BANK + i)
			return -EINVAL;
	if (p->regs[0].value != lower_32_bits(p->header_addr) ||
	    p->regs[1].value != upper_32_bits(p->header_addr))
		return -EINVAL;
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
	u64 header_addr, dma_end, n_blocks, header_size, body_size, required_size;
	u32 pitch;

	if (!afbc_fb || !src || !out || surface != S7D_AFBC_SURFACE)
		return -EINVAL;
	fb = &afbc_fb->base;
	if (!fb->format || fb->format->format != DRM_FORMAT_ABGR8888 ||
	    fb->format->num_planes != 1 || !fb->obj[0] ||
	    fb->modifier != S7D_AFBC_MODIFIER ||
	    fb->flags != DRM_MODE_FB_MODIFIERS ||
	    fb->width != S7D_AFBC_WIDTH || fb->height != S7D_AFBC_HEIGHT ||
	    fb->offsets[0] || afbc_fb->offset != fb->offsets[0] ||
	    afbc_fb->block_width != 32 || afbc_fb->block_height != 8 ||
	    afbc_fb->aligned_width != ALIGN(fb->width, 256) ||
	    afbc_fb->aligned_height != ALIGN(fb->height, 64) ||
	    !afbc_fb->afbc_size)
		return -EINVAL;
	if (src->x1 || src->y1 || src->x2 != (int)(fb->width << 16) ||
	    src->y2 != (int)(fb->height << 16))
		return -EINVAL;
	if (check_mul_overflow(afbc_fb->aligned_width, 4U, &pitch) ||
	    fb->pitches[0] != pitch || !IS_ALIGNED(pitch, 128) ||
	    (pitch >> 4) > 0xfff || pitch > 0xffff)
		return -EINVAL;
	n_blocks = (u64)afbc_fb->aligned_width * afbc_fb->aligned_height / 256;
	header_size = ALIGN(n_blocks * 16, 4096);
	body_size = n_blocks * 1024;
	if (check_add_overflow(header_size, body_size, &required_size) ||
	    required_size != afbc_fb->afbc_size)
		return -EINVAL;
	obj = to_drm_gem_dma_obj(fb->obj[0]);
	if (required_size > obj->base.size)
		return -EINVAL;
	header_addr = obj->dma_addr;
	if (check_add_overflow(header_addr, required_size - 1, &dma_end))
		return -ERANGE;
	if (!IS_ALIGNED(header_addr, 64) || dma_end > dma_mask ||
	    dma_end > GENMASK_ULL(35, 0))
		return -ERANGE;

	*out = (struct s7d_afbc_state) {
		.osd = {
			.block_config = BIT(30) | (5 << 8),
			.frame_addr = AFBC_ALIAS >> 4,
			.stride = pitch >> 4,
			.scope_x = (fb->width - 1) << 16,
			.scope_y = (fb->height - 1) << 16,
			.alpha_config = 0x7fc2,
		},
		.regs = {
			REG(AFBC_BANK, lower_32_bits(header_addr)),
			REG(AFBC_BANK + 1, upper_32_bits(header_addr)),
			REG(AFBC_BANK + 2, AFBC_FORMAT),
			REG(AFBC_BANK + 3, afbc_fb->aligned_width),
			REG(AFBC_BANK + 4, ALIGN(fb->height, 8)),
			REG(AFBC_BANK + 5, 0),
			REG(AFBC_BANK + 6, fb->width - 1),
			REG(AFBC_BANK + 7, 0),
			REG(AFBC_BANK + 8, fb->height - 1),
			REG(AFBC_BANK + 9, AFBC_ALIAS),
			REG(AFBC_BANK + 10, 0),
			REG(AFBC_BANK + 11, pitch),
			REG(AFBC_BANK + 12, 0),
		},
		.route = {
			.reg = 0x1a0e,
			.mask = BIT(5),
			.value = BIT(5),
		},
		.unpack = {
			.reg = 0x1abd,
			.mask = BIT(31) | GENMASK(15, 0),
			.value = BIT(31) | 0x1234,
		},
		.header_addr = header_addr,
		.body_addr = header_addr + header_size,
		.last_byte_addr = dma_end,
		.header_size = header_size,
		.body_size = body_size,
		.afbc_size = afbc_fb->afbc_size,
		.aligned_width = afbc_fb->aligned_width,
		.aligned_height = afbc_fb->aligned_height,
		.buffer_height = ALIGN(fb->height, 8),
		.surface_mask = BIT(S7D_AFBC_SURFACE),
	};
	return 0;
}
