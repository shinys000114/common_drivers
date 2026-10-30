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

/* partial_decode keeps its reset value 3; pix_type 1, RGBA8888 pix_bits 0. */
#define AFRC_CTRL(cu) (GENMASK(29, 28) | BIT(16) | (cu) << 8)

int s7d_gfcd_check_state(const struct s7d_gfcd_state *s)
{
	u32 width, height, fb_width, fb_height, x1, y1, x2, y2, cu;
	struct s7d_afrc_layout afrc;
	u64 blocks, bytes;

	if (!s)
		return -EINVAL;
	width = (s->osd.scope_x >> 16) + 1;
	height = (s->osd.scope_y >> 16) + 1;
	fb_width = s->frame_size >> 16;
	fb_height = s->frame_size & 0xffff;
	x1 = s->frame_begin & 0xffff;
	y1 = s->frame_begin >> 16;
	x2 = s->frame_end & 0xffff;
	y2 = s->frame_end >> 16;
	if ((s->mif_mode & ~GENMASK(5, 4)) != 0x400 ||
	    width > 1920 || height > 1080 ||
	    !fb_width || !fb_height ||
	    fb_width > S7D_AFBC_MAX_WIDTH || fb_height > S7D_AFBC_MAX_HEIGHT ||
	    !IS_ALIGNED(fb_width, 32) || !IS_ALIGNED(fb_height, 8) ||
	    !s->head || (s->head & 3) ||
	    s->osd.block_config != (BIT(30) | (5 << 8)) ||
	    s->osd.frame_addr != 0x00100000 ||
	    (s->osd.alpha_config != 0x7fc2 && s->osd.alpha_config != (BIT(2) | BIT(1))) ||
	    ((s->osd.scope_x | s->osd.scope_y) & 0xffff) ||
	    x1 > x2 || y1 > y2 || x2 >= fb_width || y2 >= fb_height ||
	    width != x2 - x1 + 1 || height != y2 - y1 + 1 ||
	    s->osd.stride != ALIGN(width, 256) / 4)
		return -EINVAL;
	/* The cropped output never reduces the compressed allocation bound. */
	if (s->afrc_ctrl) {
		cu = (s->afrc_ctrl >> 8) & 3;
		if (s->afrc_ctrl != AFRC_CTRL(cu) || s->body != s->head ||
		    (s->mif_mode & GENMASK(5, 4)) ||
		    s7d_afrc_layout(fb_width, fb_height, cu, &afrc) ||
		    s->afrc_size != afrc.size ||
		    !IS_ALIGNED((u64)s->head << 4, afrc.align))
			return -EINVAL;
		bytes = afrc.size;
	} else {
		if (s->body || s->afrc_size)
			return -EINVAL;
		blocks = (u64)ALIGN(fb_width, 256) * ALIGN(fb_height, 64) / 256;
		bytes = ALIGN(blocks * 16, 4096) + blocks * 1024;
	}
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
	width = (s->osd.scope_x >> 16) + 1;
	height = (s->osd.scope_y >> 16) + 1;
	output_w = scaler->output_h + 1;
	output_h = scaler->output_v + 1;
	/* GFCD supplies the source window; the local OSD scaler changes size. */
	if (s->osd.alpha_config != 0x7fc2 ||
	    output_w > 1920 || output_h > 1080)
		return -EINVAL;
	ret = s7d_osd_scaler_build(0, width, height, output_w, output_h, false, &checked);
	if (ret)
		return ret;
	return s7d_osd_scaler_same(scaler, &checked) ? 0 : -EINVAL;
}

bool s7d_gfcd_same_config(const struct s7d_gfcd_state *a,
			  const struct s7d_gfcd_state *b)
{
	return !s7d_gfcd_check_state(a) && !s7d_gfcd_check_state(b) &&
		a->mif_mode == b->mif_mode &&
		a->frame_size == b->frame_size && a->frame_begin == b->frame_begin &&
		a->frame_end == b->frame_end &&
		a->afrc_ctrl == b->afrc_ctrl && a->afrc_size == b->afrc_size &&
		!memcmp(&a->osd, &b->osd, sizeof(a->osd));
}

static int gfcd_build_state(const struct drm_framebuffer *fb,
			    const struct drm_rect *source, bool premult,
			    struct s7d_gfcd_state *state)
{
	u32 width, height, x1, y1, x2, y2;

	width = fb->width;
	height = fb->height;
	if (!width || !height ||
	    width > S7D_AFBC_MAX_WIDTH || height > S7D_AFBC_MAX_HEIGHT ||
	    source->x1 < 0 || source->y1 < 0 ||
	    source->x2 <= source->x1 || source->y2 <= source->y1 ||
	    source->x2 > (int)(width << 16) || source->y2 > (int)(height << 16) ||
	    ((source->x1 | source->y1 | source->x2 | source->y2) & 0xffff))
		return -EINVAL;
	x1 = source->x1 >> 16;
	y1 = source->y1 >> 16;
	x2 = source->x2 >> 16;
	y2 = source->y2 >> 16;
	*state = (struct s7d_gfcd_state) {
		.osd = {
			.block_config = BIT(30) | (5 << 8),
			.frame_addr = 0x00100000,
			/* Logical decoded-output stride, not the compressed FB pitch. */
			.stride = ALIGN(x2 - x1, 256) / 4,
			.scope_x = (x2 - x1 - 1) << 16,
			.scope_y = (y2 - y1 - 1) << 16,
			.alpha_config = 0x7fc2,
		},
		.mif_mode = 0x400,
		.frame_begin = y1 << 16 | x1,
		.frame_end = (y2 - 1) << 16 | (x2 - 1),
		.frame_size = width << 16 | height,
	};
	/* X is an encoded fourth component, never pixel alpha. */
	if (premult && fb->format->format != DRM_FORMAT_XBGR8888)
		state->osd.alpha_config = BIT(2) | BIT(1);
	return 0;
}

int s7d_gfcd_build_state(const struct drm_afbc_framebuffer *fb,
			 const struct drm_rect *source, u64 dma_mask,
			 bool premult, struct s7d_gfcd_state *state)
{
	struct s7d_afbc_buffer buffer;
	struct s7d_gfcd_state candidate;
	struct drm_rect full_source;
	int ret;

	if (!fb || !source || !state)
		return -EINVAL;
	ret = gfcd_build_state(&fb->base, source, premult, &candidate);
	if (ret)
		return ret;
	/* Validate the original FB layout and full allocation, without rebasing. */
	full_source = (struct drm_rect) {0, 0, fb->base.width << 16, fb->base.height << 16};
	ret = s7d_afbc_build_buffer(fb, &full_source, dma_mask, &buffer);
	if (ret)
		return ret;
	candidate.head = buffer.header_addr >> 4;
	ret = s7d_gfcd_check_state(&candidate);
	if (!ret)
		*state = candidate;
	return ret;
}

int s7d_gfcd_build_afrc_state(const struct s7d_afrc_framebuffer *fb,
			      const struct drm_rect *source, u64 dma_mask,
			      bool premult, struct s7d_gfcd_state *state)
{
	struct s7d_afrc_buffer buffer;
	struct s7d_gfcd_state candidate;
	int ret;

	if (!fb || !source || !state)
		return -EINVAL;
	ret = gfcd_build_state(&fb->base, source, premult, &candidate);
	if (ret)
		return ret;
	ret = s7d_afrc_build_buffer(&fb->base, dma_mask, &buffer);
	if (ret)
		return ret;
	if (buffer.layout.size != fb->afrc_size)
		return -EINVAL;
	/* No header: both MIF pointers address the fixed-rate payload. */
	candidate.head = buffer.addr >> 4;
	candidate.body = candidate.head;
	candidate.afrc_ctrl = AFRC_CTRL(buffer.cu);
	candidate.afrc_size = buffer.layout.size;
	ret = s7d_gfcd_check_state(&candidate);
	if (!ret)
		*state = candidate;
	return ret;
}

int s7d_gfcd_set_reflection(struct s7d_gfcd_state *state, u32 reflection)
{
	int ret = s7d_gfcd_check_state(state);

	if (ret)
		return ret;
	/* The AFRC axis mapping is unverified. */
	if ((reflection & ~(S7D_OSD_REFLECT_X | S7D_OSD_REFLECT_Y)) ||
	    (reflection && state->afrc_ctrl))
		return -EINVAL;
	state->mif_mode = 0x400 |
		(reflection & S7D_OSD_REFLECT_X ? BIT(4) : 0) |
		(reflection & S7D_OSD_REFLECT_Y ? BIT(5) : 0);
	return 0;
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
	u32 value;
	int ret = s7d_gfcd_check_state(state);

	if (ret)
		return ret;
	setup[0].value = state->mif_mode;
	setup[2].value = state->head;
	setup[3].value = state->frame_begin;
	setup[4].value = state->frame_end;
	setup[6].value = state->frame_size;
	if (!ret)
		ret = s7d_gfcd_admit(map);
	for (i = 0; !ret && i < ARRAY_SIZE(setup); i++)
		ret = gfcd_write(map, setup[i].reg, setup[i].value);
	/* AFRC reads the payload through the body MIF; AFBC clears AFRC_CTRL[0]. */
	if (!ret && state->afrc_ctrl) {
		ret = gfcd_write(map, 0x5907, 0);
		if (!ret)
			ret = gfcd_write(map, S7D_GFCD_BODY, state->body);
		if (!ret)
			ret = gfcd_write(map, S7D_GFCD_AFRC, state->afrc_ctrl);
	} else if (!ret) {
		ret = regmap_update_bits(map, S7D_GFCD_AFRC * 4, BIT(0), 0);
		if (!ret)
			ret = regmap_read(map, S7D_GFCD_AFRC * 4, &value);
		if (!ret && (value & BIT(0)))
			ret = -EIO;
	}
	/* TOP bit 0 selects the AFRC decoder. */
	if (!ret)
		ret = gfcd_write(map, S7D_GFCD_TOP,
			(state->osd.alpha_config == 0x7fc2 ? 0xfff0e410 : 0x8000e410) |
			(state->afrc_ctrl ? BIT(0) : 0));
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
