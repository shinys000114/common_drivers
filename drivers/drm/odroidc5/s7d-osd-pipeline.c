// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/align.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/types.h>

#include <asm/byteorder.h>

#include "s7d-osd-pipeline.h"

/* VCBUS word indices, as required by the display RDMA engine. */
#define OSD1_CTRL_STAT		0x1a10
#define OSD1_BLK0_CFG_W4		0x1a13
#define OSD1_FRAME_ADDR		0x1a14
#define OSD1_LINE_STRIDE		0x1a15
#define OSD1_BLK0_CFG_W0		0x1a1b
#define OSD1_BLK0_CFG_W1		0x1a1c
#define OSD1_BLK0_CFG_W2		0x1a1d
#define OSD1_BLK0_CFG_W3		0x1a1e
#define OSD1_FIFO_CTRL		0x1a2b
#define OSD1_CTRL_STAT2		0x1a2d
#define OSD1_MALI_UNPACK_CTRL	0x1a2f
#define OSD1_PROT_CTRL		0x1a2e
#define OSD1_DIMM_CTRL		0x1adf
#define OSD2_CTRL_STAT		0x1a30
#define OSD2_BLK0_CFG_W4		0x1a64
#define OSD2_FRAME_ADDR		0x1a65
#define OSD2_LINE_STRIDE		0x1a66
#define OSD2_BLK0_CFG_W0		0x1a3b
#define OSD2_BLK0_CFG_W1		0x1a3c
#define OSD2_BLK0_CFG_W2		0x1a3d
#define OSD2_BLK0_CFG_W3		0x1a3e
#define OSD2_FIFO_CTRL		0x1a4b
#define OSD2_CTRL_STAT2		0x1a4d
#define OSD2_MALI_UNPACK_CTRL	0x1abd
#define OSD2_PROT_CTRL		0x1a4e
#define OSD2_DIMM_CTRL		0x1acf
#define OSD1_NORMAL_SWAP		0x1abe
#define OSD2_NORMAL_SWAP		0x1abf
#define OSD_BLEND_CTRL		0x39b0
#define OSD_BLEND_DIN0_H		0x39b1
#define OSD_BLEND_DIN0_V		0x39b2
#define OSD_BLEND_DIN1_H		0x39b3
#define OSD_BLEND_DIN1_V		0x39b4
#define OSD_BLEND_DIN2_H		0x39b5
#define OSD_BLEND_DIN2_V		0x39b6
#define OSD_BLEND_DIN3_H		0x39b7
#define OSD_BLEND_DIN3_V		0x39b8
#define OSD_BLEND_DUMMY_DATA	0x39b9
#define OSD_BLEND_DUMMY_ALPHA	0x39ba
#define OSD_BLEND0_SIZE		0x39bb
#define OSD_BLEND1_SIZE		0x39bc
#define OSD_BLEND_CTRL1		0x39c0

#define REG(r, v) { .reg = cpu_to_le32(r), .value = cpu_to_le32(v) }

u32 s7d_osd_unpack_mask(u32 reg)
{
	if (reg == OSD1_MALI_UNPACK_CTRL || reg == OSD2_MALI_UNPACK_CTRL)
		return BIT(31) | BIT(28) | GENMASK(25, 24);
	return 0;
}

static u32 linear_unpack(const struct s7d_osd_layer *layer)
{
	if (layer->enabled && !layer->afbc && layer->premult &&
	    !(layer->layout.alpha_config & BIT(14)))
		return BIT(28);
	return 0;
}

static int check_layer(const struct s7d_osd_layer *layer, u32 width, u32 height,
		       bool secondary)
{
	const struct s7d_osd_state *layout = &layer->layout;
	const struct drm_rect *dst = &layer->dst;
	u32 x_start, x_end, y_start, y_end;

	if (!layer->enabled)
		return 0;
	if (layer->afbc) {
		if (!secondary || layer->alpha != 256 ||
		    layout->block_config != (BIT(30) | (5 << 8)) ||
		    layout->alpha_config != 0x7fc2 ||
		    layout->frame_addr != 0x00200000 ||
		    ((layout->scope_x | layout->scope_y) & 0xffff) ||
		    layout->stride != ALIGN((layout->scope_x >> 16) + 1, 256) / 4)
			return -EINVAL;
	} else if ((layout->block_config & ~0xcU) != 0x8500 ||
		   (layout->alpha_config != 0x7fc0 && layout->alpha_config != BIT(2))) {
		return -EINVAL;
	}
	if (((layout->scope_x | layout->scope_y) & 0xe000e000) ||
	    !layout->stride || layout->stride > 0xfff || (layout->stride & 3) ||
	    layer->alpha > 256 ||
	    dst->x1 < 0 || dst->y1 < 0 || dst->x2 > (int)width || dst->y2 > (int)height ||
	    dst->x2 <= dst->x1 || dst->y2 <= dst->y1)
		return -EINVAL;
	x_start = layout->scope_x & 0x1fff;
	x_end = layout->scope_x >> 16;
	y_start = layout->scope_y & 0x1fff;
	y_end = layout->scope_y >> 16;
	if (x_end < x_start || y_end < y_start ||
	    x_end - x_start + 1 != (u32)drm_rect_width(dst) ||
	    y_end - y_start + 1 != (u32)drm_rect_height(dst) ||
	    (x_end + 1) * 4 > layout->stride * 16)
		return -EINVAL;
	return 0;
}

int s7d_osd_build_pipeline(u8 revision, u32 width, u32 height,
			   const struct s7d_osd_layer layers[2],
			   struct s7d_osd_pipeline_state *state)
{
	struct s7d_osd_state secondary = {
		.block_config = 0x8504, .stride = 4, .alpha_config = 0x7fc0,
	};
	const struct s7d_osd_state *primary;
	u32 h_scope, v_scope, secondary_h = 0x1fff1fff, secondary_v = 0x043a0439;
	u32 size, blend = 0x807f4413 | BIT(25), alpha, secondary_ctrl = 0x00100004;

	if (!layers || !state)
		return -EINVAL;
	if (revision != S7D_OSD_REV_B)
		return -EOPNOTSUPP;
	if (!width || !height || width > 4096 || height > 4096 ||
	    !layers[0].enabled || layers[0].dst.x1 || layers[0].dst.y1 ||
	    layers[0].dst.x2 != (int)width || layers[0].dst.y2 != (int)height ||
	    check_layer(&layers[0], width, height, false) ||
	    check_layer(&layers[1], width, height, true))
		return -EINVAL;
	primary = &layers[0].layout;
	h_scope = (width - 1) << 16;
	v_scope = (height - 1) << 16;
	size = (height << 16) | width;
	alpha = (layers[0].alpha << 20) | (256 << 11);
	if (layers[1].enabled) {
		secondary = layers[1].layout;
		secondary_h = ((layers[1].dst.x2 - 1) << 16) | layers[1].dst.x1;
		secondary_v = ((layers[1].dst.y2 - 1) << 16) | layers[1].dst.y1;
		secondary_ctrl |= BIT(0);
		/* Rev.B routes OSD2 through DIN3, with OSD4 as its alpha input. */
		blend = (blend & ~GENMASK(15, 12)) | (2 << 12) | BIT(23);
		alpha = (layers[0].alpha << 20) | (layers[1].alpha << 11);
	}
	*state = (struct s7d_osd_pipeline_state) {
		.setup = {
			REG(OSD1_FIFO_CTRL, 0x82840501),
			REG(OSD1_PROT_CTRL, 0x80620200),
			REG(OSD1_CTRL_STAT2, primary->alpha_config),
			REG(OSD1_MALI_UNPACK_CTRL, linear_unpack(&layers[0])),
			REG(OSD1_BLK0_CFG_W0, primary->block_config),
			REG(OSD1_FRAME_ADDR, primary->frame_addr),
			REG(OSD1_LINE_STRIDE, primary->stride),
			REG(OSD1_BLK0_CFG_W1, primary->scope_x),
			REG(OSD1_BLK0_CFG_W2, primary->scope_y),
			REG(OSD1_BLK0_CFG_W3, h_scope),
			REG(OSD1_BLK0_CFG_W4, v_scope),
			REG(OSD1_DIMM_CTRL, 0),
			REG(OSD2_FIFO_CTRL, 0x82840501),
			REG(OSD2_PROT_CTRL, 0x80620200),
			REG(OSD2_CTRL_STAT2, secondary.alpha_config),
			REG(OSD2_MALI_UNPACK_CTRL, linear_unpack(&layers[1])),
			REG(OSD2_BLK0_CFG_W0, secondary.block_config),
			REG(OSD2_FRAME_ADDR, secondary.frame_addr),
			REG(OSD2_LINE_STRIDE, secondary.stride),
			REG(OSD2_BLK0_CFG_W1, secondary.scope_x),
			REG(OSD2_BLK0_CFG_W2, secondary.scope_y),
			REG(OSD2_BLK0_CFG_W3, layers[1].enabled ? secondary_h : 0),
			REG(OSD2_BLK0_CFG_W4, layers[1].enabled ? secondary_v : 0),
			REG(OSD2_DIMM_CTRL, 0),
			REG(OSD1_NORMAL_SWAP, 0x3210),
			REG(OSD2_NORMAL_SWAP, 0x3210),
			/* Scope indices identify OSD inputs, not reordered DINs. */
			REG(OSD_BLEND_DIN0_H, h_scope),
			REG(OSD_BLEND_DIN0_V, v_scope),
			REG(OSD_BLEND_DIN1_H, secondary_h),
			REG(OSD_BLEND_DIN1_V, secondary_v),
			REG(OSD_BLEND_DIN2_H, 0x1fff1fff),
			REG(OSD_BLEND_DIN2_V, 0x0439043a),
			REG(OSD_BLEND_DIN3_H, 0x1fff1fff),
			REG(OSD_BLEND_DIN3_V, 0x0439043a),
			REG(OSD_BLEND_DUMMY_DATA, 0),
			REG(OSD_BLEND_DUMMY_ALPHA, alpha),
			REG(OSD_BLEND0_SIZE, size),
			REG(OSD_BLEND1_SIZE, size),
			REG(OSD_BLEND_CTRL1, 0x31031),
			REG(OSD_BLEND_CTRL, blend),
			REG(OSD2_CTRL_STAT, secondary_ctrl),
			REG(OSD1_CTRL_STAT, 0x00100005),
		},
		.update = {
			REG(OSD1_BLK0_CFG_W0, primary->block_config),
			REG(OSD1_FRAME_ADDR, primary->frame_addr),
			REG(OSD1_LINE_STRIDE, primary->stride),
			REG(OSD1_BLK0_CFG_W1, primary->scope_x),
			REG(OSD1_BLK0_CFG_W2, primary->scope_y),
			REG(OSD1_CTRL_STAT2, primary->alpha_config),
			REG(OSD1_MALI_UNPACK_CTRL, linear_unpack(&layers[0])),
			REG(OSD2_BLK0_CFG_W0, secondary.block_config),
			REG(OSD2_FRAME_ADDR, secondary.frame_addr),
			REG(OSD2_LINE_STRIDE, secondary.stride),
			REG(OSD2_BLK0_CFG_W1, secondary.scope_x),
			REG(OSD2_BLK0_CFG_W2, secondary.scope_y),
			REG(OSD2_BLK0_CFG_W3, layers[1].enabled ? secondary_h : 0),
			REG(OSD2_BLK0_CFG_W4, layers[1].enabled ? secondary_v : 0),
			REG(OSD2_CTRL_STAT2, secondary.alpha_config),
			REG(OSD2_MALI_UNPACK_CTRL, linear_unpack(&layers[1])),
			REG(OSD_BLEND_DIN1_H, secondary_h),
			REG(OSD_BLEND_DIN1_V, secondary_v),
			REG(OSD_BLEND_DUMMY_ALPHA, alpha),
			REG(OSD_BLEND_CTRL, blend),
			REG(OSD2_CTRL_STAT, secondary_ctrl),
		},
	};
	return 0;
}
