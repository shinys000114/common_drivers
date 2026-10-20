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

static int s7d_postblend_sources(const struct s7d_display_resources *r,
				const u32 zpos[S7D_INPUT_COUNT], u32 sources[3])
{
	static const u8 source_id[] = {
		[S7D_INPUT_OSD1] = 3,
		[S7D_INPUT_OSD2] = 4,
		[S7D_INPUT_VD1] = 1,
		[S7D_INPUT_VD2] = 2,
	};
	unsigned int i, j, rank, port;

	for (i = 0; i < S7D_INPUT_COUNT; i++) {
		if (!(r->inputs & BIT(i)))
			continue;
		if (zpos[i] > S7D_DISPLAY_MAX_ZPOS)
			return -EINVAL;
		rank = 0;
		for (j = 0; j < S7D_INPUT_COUNT; j++) {
			if (!(r->inputs & BIT(j)) || i == j)
				continue;
			if (zpos[i] == zpos[j])
				return -EINVAL;
			if (zpos[j] < zpos[i])
				rank++;
		}
		/* Preserve the established OSD-only port gap. */
		port = rank == 1 && !(r->inputs & S7D_VIDEO_INPUTS) ? 2 : rank;
		sources[port] = source_id[i];
	}
	return 0;
}

int s7d_postblend_build_state(u32 width, u32 height,
			     const struct s7d_display_resources *resources,
			     const u32 zpos[S7D_INPUT_COUNT],
			     const struct drm_rect *video_dst,
			     u32 video_width, u32 video_height,
			     const struct drm_rect *video2_dst,
			     struct s7d_postblend_state *state)
{
	u32 video_h = 0x1fff0000, video_v = 0x1fff0000;
	u32 video2_h = 0x1fff0000, video2_v = 0x1fff0000;
	u32 input_width = width, input_height = height;
	u32 sources[3] = {0}, pre_src = 0;
	int ret;

	if (!state || !resources || !zpos || !width || !height ||
	    width > S7D_DISPLAY_MAX_WIDTH || height > S7D_DISPLAY_MAX_HEIGHT ||
	    !(resources->inputs & BIT(S7D_INPUT_OSD1)) ||
	    !!video_dst != !!(resources->inputs & BIT(S7D_INPUT_VD1)) ||
	    !!video2_dst != !!(resources->inputs & BIT(S7D_INPUT_VD2)))
		return -EINVAL;
	ret = s7d_display_check_resources(resources);
	if (ret)
		return ret;
	ret = s7d_postblend_sources(resources, zpos, sources);
	if (ret)
		return ret;
	if (video_dst) {
		if (video_dst->x1 < 0 || video_dst->y1 < 0 ||
		    video_dst->x2 > (int)width || video_dst->y2 > (int)height ||
		    video_dst->x2 <= video_dst->x1 || video_dst->y2 <= video_dst->y1 ||
		    !video_width || !video_height || video_width > 4096 || video_height > 2160)
			return -EINVAL;
		video_h = (video_dst->x1 << 16) | (video_dst->x2 - 1);
		video_v = (video_dst->y1 << 16) | (video_dst->y2 - 1);
		input_width = video_width;
		input_height = video_height;
		/* Vendor vpp_blend_update() keeps the VD1 preblend input selected. */
		pre_src = BIT(4) | BIT(0);
	}
	if (video2_dst) {
		if (video2_dst->x1 < 0 || video2_dst->y1 < 0 ||
		    video2_dst->x2 > (int)width || video2_dst->y2 > (int)height ||
		    video2_dst->x2 <= video2_dst->x1 || video2_dst->y2 <= video2_dst->y1)
			return -EINVAL;
		video2_h = (video2_dst->x1 << 16) | (video2_dst->x2 - 1);
		video2_v = (video2_dst->y1 << 16) | (video2_dst->y2 - 1);
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
			REG(0x1d1e, video2_h),
			REG(0x1d1f, video2_v),
			/* Port 0 requires bit 16; keep straight OSD output off it. */
			REG(VD1_BLEND_SRC_CTRL, BIT(16) | pre_src),
			REG(VD2_BLEND_SRC_CTRL, (sources[0] << 8) | (video2_dst ? BIT(20) : 0)),
			/* Bit 20 keeps logical OSD1 on the postblend path. */
			REG(OSD1_BLEND_SRC_CTRL, BIT(20) | (sources[1] << 8)),
			REG(OSD2_BLEND_SRC_CTRL, BIT(20) | (sources[2] << 8)),
		},
	};
	return 0;
}
