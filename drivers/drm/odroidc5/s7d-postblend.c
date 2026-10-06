// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>

#include <asm/byteorder.h>

#include <drm/drm_rect.h>

#include "s7d-postblend.h"

#define VPP_PREBLEND_VD1_H	0x1d1a
#define VPP_PREBLEND_VD1_V	0x1d1b
#define VPP_PREBLEND_SIZE		0x1d20
#define VPP_POSTBLEND_VD1_H	0x1d1c
#define VPP_POSTBLEND_VD1_V	0x1d1d
#define VPP_OSD1_SCOPE_H		0x1df5
#define VPP_OSD1_SCOPE_V		0x1df6
#define VPP_OSD2_SCOPE_H		0x1df7
#define VPP_OSD2_SCOPE_V		0x1df8
#define VD1_BLEND_SRC_CTRL	0x1dfb
#define VD2_BLEND_SRC_CTRL	0x1dfc
#define OSD1_BLEND_SRC_CTRL	0x1dfd
#define OSD2_BLEND_SRC_CTRL	0x1dfe

#define REG(r, v) { .reg = cpu_to_le32(r), .value = cpu_to_le32(v) }

int s7d_postblend_build_state(u32 width, u32 height,
			     const struct drm_rect *video_dst, bool osd2_enabled,
			     struct s7d_postblend_state *state)
{
	u32 video_h = 0x1fff0000, video_v = 0x1fff0000;
	u32 input_width = width, input_height = height;
	u32 video_src = 0, pre_src = 0;

	if (!state || !width || !height || width > 4096 || height > 4096)
		return -EINVAL;
	if (video_dst) {
		if (video_dst->x1 < 0 || video_dst->y1 < 0 ||
		    video_dst->x2 > (int)width || video_dst->y2 > (int)height ||
		    video_dst->x2 <= video_dst->x1 || video_dst->y2 <= video_dst->y1)
			return -EINVAL;
		video_h = (video_dst->x1 << 16) | (video_dst->x2 - 1);
		video_v = (video_dst->y1 << 16) | (video_dst->y2 - 1);
		input_width = video_dst->x2 - video_dst->x1;
		input_height = video_dst->y2 - video_dst->y1;
		video_src = 1 << 8;
		/* Vendor vpp_blend_update() keeps the VD1 preblend input selected. */
		pre_src = BIT(4) | BIT(0);
	}
	*state = (struct s7d_postblend_state) {
		.regs = {
			/* Scopes follow logical sources, independent of mux ports. */
			REG(VPP_OSD1_SCOPE_H, width - 1),
			REG(VPP_OSD1_SCOPE_V, height - 1),
			REG(VPP_OSD2_SCOPE_H, width - 1),
			REG(VPP_OSD2_SCOPE_V, height - 1),
			/* The cropped input starts at zero; postblend places its output. */
			REG(VPP_PREBLEND_VD1_H, input_width - 1),
			REG(VPP_PREBLEND_VD1_V, input_height - 1),
			REG(VPP_PREBLEND_SIZE, (input_height << 16) | input_width),
			REG(VPP_POSTBLEND_VD1_H, video_h),
			REG(VPP_POSTBLEND_VD1_V, video_v),
			/* Port 0 requires bit 16; keep straight OSD output off it. */
			REG(VD1_BLEND_SRC_CTRL, BIT(16) | pre_src),
			REG(VD2_BLEND_SRC_CTRL, 3 << 8),
			/* Bit 20 keeps logical OSD1 on the postblend path. */
			REG(OSD1_BLEND_SRC_CTRL, BIT(20) | video_src),
			REG(OSD2_BLEND_SRC_CTRL, BIT(20) | (osd2_enabled ? 4 << 8 : 0)),
		},
	};
	return 0;
}
