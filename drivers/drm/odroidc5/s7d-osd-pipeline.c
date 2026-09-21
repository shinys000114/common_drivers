// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
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
#define OSD1_PROT_CTRL		0x1a2e
#define OSD1_DIMM_CTRL		0x1adf
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

int s7d_osd_build_pipeline(u8 revision, u32 width, u32 height,
			   const struct s7d_osd_state *layout,
			   struct s7d_osd_pipeline_state *state)
{
	u32 x_start, x_end, y_start, y_end, h_scope, v_scope, size;

	if (!layout || !state)
		return -EINVAL;
	if (revision != S7D_OSD_REV_B)
		return -EOPNOTSUPP;
	/* W3/W4 destination fields are 12 bits; source fields are 13 bits. */
	if (!width || !height || width > 4096 || height > 4096 ||
	    ((layout->scope_x | layout->scope_y) & 0xe000e000) ||
	    !layout->stride || layout->stride > 0xfff || (layout->stride & 3))
		return -EINVAL;
	x_start = layout->scope_x & 0x1fff;
	x_end = layout->scope_x >> 16;
	y_start = layout->scope_y & 0x1fff;
	y_end = layout->scope_y >> 16;
	if (x_end < x_start || y_end < y_start ||
	    x_end - x_start + 1 != width || y_end - y_start + 1 != height ||
	    (x_end + 1) * 4 > layout->stride * 16)
		return -EINVAL;

	h_scope = (width - 1) << 16;
	v_scope = (height - 1) << 16;
	size = (height << 16) | width;
	*state = (struct s7d_osd_pipeline_state) {
		.setup = {
			/* 512-entry FIFO, hold 8 lines; no reset/error-clear pulse. */
			REG(OSD1_FIFO_CTRL, 0x82840501),
			REG(OSD1_PROT_CTRL, 0x80620200),
			/* Replace X with opaque alpha; never copy read-only status. */
			REG(OSD1_CTRL_STAT2, 0x7fc0),
			/* Little endian, 32-bit block, ARGB8888, no canvas/AFBC. */
			REG(OSD1_BLK0_CFG_W0, 0x8504),
			REG(OSD1_FRAME_ADDR, layout->frame_addr),
			REG(OSD1_LINE_STRIDE, layout->stride),
			REG(OSD1_BLK0_CFG_W1, layout->scope_x),
			REG(OSD1_BLK0_CFG_W2, layout->scope_y),
			REG(OSD1_BLK0_CFG_W3, h_scope),
			REG(OSD1_BLK0_CFG_W4, v_scope),
			REG(OSD1_DIMM_CTRL, 0),
			/* Scope indices identify OSD inputs, not reordered DINs. */
			REG(OSD_BLEND_DIN0_H, h_scope),
			REG(OSD_BLEND_DIN0_V, v_scope),
			REG(OSD_BLEND_DIN1_H, 0x1fff1fff),
			REG(OSD_BLEND_DIN1_V, 0x043a0439),
			REG(OSD_BLEND_DIN2_H, 0x1fff1fff),
			REG(OSD_BLEND_DIN2_V, 0x0439043a),
			REG(OSD_BLEND_DIN3_H, 0x1fff1fff),
			REG(OSD_BLEND_DIN3_V, 0x0439043a),
			REG(OSD_BLEND_DUMMY_DATA, 0),
			/* Rev.B write encoding; readback would be 0x04020000. */
			REG(OSD_BLEND_DUMMY_ALPHA, 0x10080000),
			REG(OSD_BLEND0_SIZE, size),
			REG(OSD_BLEND1_SIZE, size),
			/* Both output dividers, 9-bit alpha. */
			REG(OSD_BLEND_CTRL1, 0x31031),
			/* Rev.B: DIN1=OSD1, DIN0=3, mask 7, premult 0xf. */
			REG(OSD_BLEND_CTRL, 0x807f4413),
			/* Global alpha 256, linear mode, enable only block 0. */
			REG(OSD1_CTRL_STAT, 0x00100005),
		},
		.update = {
			REG(OSD1_FRAME_ADDR, layout->frame_addr),
			REG(OSD1_LINE_STRIDE, layout->stride),
			REG(OSD1_BLK0_CFG_W1, layout->scope_x),
			REG(OSD1_BLK0_CFG_W2, layout->scope_y),
		},
	};
	return 0;
}
