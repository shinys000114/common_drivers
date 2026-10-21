// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/align.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_rect.h>

#include "s7d-gfcd.h"
#include "s7d-osd-scaler.h"

int s7d_gfcd_check_state(const struct s7d_gfcd_state *s)
{
	u32 width, height;
	u64 blocks, bytes;

	if (!s)
		return -EINVAL;
	width = (s->osd.scope_x >> 16) + 1;
	height = (s->osd.scope_y >> 16) + 1;
	if (!width || !height || width > 1920 || height > 1080 || !IS_ALIGNED(width, 32) ||
	    !IS_ALIGNED(height, 8) || !s->head || (s->head & 3) ||
	    s->osd.block_config != (BIT(30) | (5 << 8)) ||
	    s->osd.frame_addr != 0x00100000 ||
	    (s->osd.alpha_config != 0x7fc2 && s->osd.alpha_config != (BIT(2) | BIT(1))) ||
	    ((s->osd.scope_x | s->osd.scope_y) & 0xffff) ||
	    s->osd.stride != ALIGN(width, 256) / 4 ||
	    s->frame_end != ((height - 1) << 16 | (width - 1)) ||
	    s->frame_size != (width << 16 | height))
		return -EINVAL;
	blocks = (u64)ALIGN(width, 256) * ALIGN(height, 64) / 256;
	bytes = ALIGN(blocks * 16, 4096) + blocks * 1024;
	if (((u64)s->head << 4) > GENMASK_ULL(35, 0) - (bytes - 1))
		return -ERANGE;
	return 0;
}

int s7d_gfcd_check_scaler(const struct s7d_gfcd_state *s,
			  const struct s7d_osd_scaler_state *scaler)
{
	struct s7d_osd_scaler_state checked = { };
	u32 width, height, output_w, output_h;
	int ret = s7d_gfcd_check_state(s);

	if (ret)
		return ret;
	if (!scaler)
		return -EINVAL;
	if (!scaler->enabled)
		return s7d_osd_scaler_same(scaler, &checked) ? 0 : -EINVAL;
	width = s->frame_size >> 16;
	height = s->frame_size & 0xffff;
	output_w = scaler->output_h + 1;
	output_h = scaler->output_v + 1;
	/* Opaque enlargement does not skip source pixels. */
	if (s->osd.alpha_config != 0x7fc2 || output_w < width ||
	    output_h < height || output_w > 1920 || output_h > 1080)
		return -EINVAL;
	ret = s7d_osd_scaler_build(0, width, height, output_w, output_h, &checked);
	if (ret)
		return ret;
	return s7d_osd_scaler_same(scaler, &checked) ? 0 : -EINVAL;
}

bool s7d_gfcd_same_config(const struct s7d_gfcd_state *a,
			  const struct s7d_gfcd_state *b)
{
	return !s7d_gfcd_check_state(a) && !s7d_gfcd_check_state(b) &&
		a->frame_size == b->frame_size && a->frame_end == b->frame_end &&
		!memcmp(&a->osd, &b->osd, sizeof(a->osd));
}

int s7d_gfcd_build_state(const struct drm_afbc_framebuffer *fb,
			 const struct drm_rect *source, u64 dma_mask,
			 bool premult, struct s7d_gfcd_state *state)
{
	struct s7d_afbc_buffer buffer;
	struct s7d_gfcd_state candidate;
	u32 width, height;
	int ret;

	if (!state)
		return -EINVAL;
	ret = s7d_afbc_build_buffer(fb, source, dma_mask, &buffer);
	if (ret)
		return ret;
	width = fb->base.width;
	height = fb->base.height;
	candidate = (struct s7d_gfcd_state) {
		.osd = {
			.block_config = BIT(30) | (5 << 8),
			.frame_addr = 0x00100000,
			.stride = buffer.pitch >> 4,
			.scope_x = (width - 1) << 16,
			.scope_y = (height - 1) << 16,
			.alpha_config = 0x7fc2,
		},
		.head = buffer.header_addr >> 4,
		.frame_end = (height - 1) << 16 | (width - 1),
		.frame_size = width << 16 | height,
	};
	/* X is an encoded fourth component, never pixel alpha. */
	if (premult && fb->base.format->format == DRM_FORMAT_ABGR8888)
		candidate.osd.alpha_config = BIT(2) | BIT(1);
	ret = s7d_gfcd_check_state(&candidate);
	if (!ret)
		*state = candidate;
	return ret;
}

static int gfcd_write(struct regmap *map, u32 reg, u32 value)
{
	u32 actual;
	int ret = regmap_write(map, reg * 4, value);

	if (!ret)
		ret = regmap_read(map, reg * 4, &actual);
	return ret ? ret : (actual == value ? 0 : -EIO);
}

int s7d_gfcd_admit(struct regmap *map)
{
	static const struct { u32 reg, mask, value; } state[] = {
		{ 0x5900, U32_MAX, 0 }, { 0x5901, U32_MAX, 0 },
		{ 0x5952, U32_MAX, 0 }, { 0x5953, U32_MAX, 0 },
		{ 0x59c2, U32_MAX, 0 }, { 0x59c3, U32_MAX, 0 },
		{ 0x59f0, U32_MAX, BIT(31) }, { 0x59f1, U32_MAX, 0 },
		{ 0x5955, BIT(4), 0 }, { 0x59c5, BIT(4), 0 },
		{ 0x595c, GENMASK(31, 28), 0 },
	};
	u32 value;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(state); i++) {
		ret = regmap_read(map, state[i].reg * 4, &value);
		if (ret)
			return ret;
		if ((value & state[i].mask) != state[i].value)
			return -EOPNOTSUPP;
	}
	return 0;
}

int s7d_gfcd_prepare(struct regmap *map, const struct s7d_gfcd_state *state)
{
	struct { u32 reg, value; } setup[] = {
		{ 0x5902, 0x400 }, { 0x5903, 0x11 },
		{ S7D_GFCD_HEAD, 0 }, { 0x5950, 0 }, { 0x5951, 0 },
		{ 0x5958, 0x7010 }, { 0x5959, 0 },
	};
	unsigned int i;
	int ret = s7d_gfcd_check_state(state);

	if (ret)
		return ret;
	setup[2].value = state->head;
	setup[4].value = state->frame_end;
	setup[6].value = state->frame_size;
	if (!ret)
		ret = s7d_gfcd_admit(map);
	for (i = 0; !ret && i < ARRAY_SIZE(setup); i++)
		ret = gfcd_write(map, setup[i].reg, setup[i].value);
	if (!ret)
		ret = gfcd_write(map, S7D_GFCD_TOP,
			state->osd.alpha_config == 0x7fc2 ? 0xfff0e410 : 0x8000e410);
	return ret;
}

int s7d_gfcd_close(struct regmap *map)
{
	u32 value;
	int ret = regmap_update_bits(map, S7D_GFCD_TOP * 4, BIT(4), 0);

	if (!ret)
		ret = regmap_read(map, S7D_GFCD_TOP * 4, &value);
	return ret ? ret : (value & BIT(4) ? -EIO : 0);
}
