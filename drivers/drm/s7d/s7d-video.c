// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/align.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/overflow.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_rect.h>

#include "s7d-video.h"

bool s7d_video_fbc_modifier(u32 format, u64 modifier)
{
	u64 mem_saving = DRM_FORMAT_MOD_AMLOGIC_FBC(AMLOGIC_FBC_LAYOUT_SCATTER,
						   AMLOGIC_FBC_OPTION_MEM_SAVING);

	return (format == DRM_FORMAT_YUV420_8BIT && modifier == mem_saving) ||
		((format == DRM_FORMAT_YUV420_8BIT || format == DRM_FORMAT_YUV420_10BIT) &&
		 modifier == DRM_FORMAT_MOD_AMLOGIC_FBC(AMLOGIC_FBC_LAYOUT_SCATTER, 0));
}

/* The scatter header holds the body addresses: 32 bytes per 32x32 block. */
static int s7d_video_build_fbc(struct drm_framebuffer *fb, const struct drm_rect *src,
			       u64 dma_mask, struct s7d_video_state *state)
{
	struct s7d_video_state next = {0};
	struct drm_gem_dma_object *obj = drm_fb_dma_get_gem_obj(fb, 0);
	u32 x1 = src->x1 >> 16, y1 = src->y1 >> 16;
	u32 x2 = src->x2 >> 16, y2 = src->y2 >> 16;
	u64 header, end, bytes;

	if (!obj || !s7d_video_fbc_modifier(fb->format->format, fb->modifier) ||
	    !fb->width || !fb->height ||
	    fb->width > S7D_VIDEO_MAX_WIDTH || fb->height > S7D_VIDEO_MAX_HEIGHT ||
	    src->x1 < 0 || src->y1 < 0 || src->x2 <= src->x1 || src->y2 <= src->y1 ||
	    ((src->x1 | src->y1 | src->x2 | src->y2) & 0x1ffff) ||
	    x2 > fb->width || y2 > fb->height)
		return -EINVAL;
	bytes = (u64)DIV_ROUND_UP(fb->width, 32) * DIV_ROUND_UP(fb->height, 32) * 32;
	if (!IS_ALIGNED(fb->offsets[0], 32) ||
	    check_add_overflow((u64)fb->offsets[0], bytes, &end) || end > obj->base.size)
		return -EINVAL;
	header = (u64)obj->dma_addr + fb->offsets[0];
	if (header + bytes - 1 > dma_mask || header + bytes > (1ULL << 36))
		return -ERANGE;
	next.format = fb->format->format;
	next.frame_addr[0] = header >> 4;
	next.scope_x[0] = ((x1 / 32) << 16) | ((x2 - 1) / 32);
	next.scope_x[1] = ((x1 % 32) << 16) | (x1 % 32 + x2 - x1 - 1);
	next.scope_y[0] = ((y1 / 4) << 16) | ((y2 - 1) / 4);
	next.scope_y[1] = ((y1 % 4) << 16) | (y1 % 4 + y2 - y1 - 1);
	next.fbc = (struct s7d_video_fbc) {
		.enabled = true,
		.ten_bit = fb->format->format == DRM_FORMAT_YUV420_10BIT,
		.mem_saving = fb->modifier ==
			DRM_FORMAT_MOD_AMLOGIC_FBC(AMLOGIC_FBC_LAYOUT_SCATTER,
						   AMLOGIC_FBC_OPTION_MEM_SAVING),
		.width = fb->width,
		.height = fb->height,
	};
	*state = next;
	return 0;
}

int s7d_video_build_state(struct drm_framebuffer *fb, const struct drm_rect *src,
			  u64 dma_mask, struct s7d_video_state *state)
{
	struct s7d_video_state next = {0};
	u32 x1, y1, x2, y2;
	unsigned int i, planes = 2, cpp = 1;
	bool full_chroma = false;

	if (!fb || !src || !state || !fb->format)
		return -EINVAL;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return s7d_video_build_fbc(fb, src, dma_mask, state);
	switch (fb->format->format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
		break;
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
		planes = 1;
		cpp = 2;
		break;
	case DRM_FORMAT_VUY888:
	case DRM_FORMAT_XVYU2101010:
		planes = 1;
		cpp = fb->format->cpp[0];
		full_chroma = true;
		break;
	default:
		return -EINVAL;
	}
	next.format = fb->format->format;
	if (!fb->width || !fb->height || ((fb->width | fb->height) & 1) ||
	    fb->width > S7D_VIDEO_MAX_WIDTH || fb->height > S7D_VIDEO_MAX_HEIGHT ||
	    src->x1 < 0 || src->y1 < 0 || src->x2 <= src->x1 || src->y2 <= src->y1 ||
	    ((src->x1 | src->y1 | src->x2 | src->y2) & 0x1ffff))
		return -EINVAL;
	x1 = src->x1 >> 16;
	y1 = src->y1 >> 16;
	x2 = src->x2 >> 16;
	y2 = src->y2 >> 16;
	if (x2 > fb->width || y2 > fb->height)
		return -EINVAL;

	for (i = 0; i < planes; i++) {
		struct drm_gem_dma_object *obj;
		u64 base, bytes, end, object_end;

		if (!fb->obj[i] || !IS_ALIGNED(fb->pitches[i], 16) ||
		    fb->pitches[i] < fb->width * cpp ||
		    (fb->pitches[i] >> 4) > 0x1fff)
			return -EINVAL;
		obj = drm_fb_dma_get_gem_obj(fb, i);
		/* Include the complete padded rows fetched by the MIF bursts. */
		bytes = (u64)fb->pitches[i] *
			(i ? fb->height / fb->format->vsub : fb->height);
		if (check_add_overflow((u64)fb->offsets[i], bytes, &object_end) ||
		    object_end > obj->base.size)
			return -EINVAL;
		if (check_add_overflow((u64)obj->dma_addr, (u64)fb->offsets[i], &base) ||
		    check_add_overflow(base, bytes - 1, &end))
			return -ERANGE;
		if (!IS_ALIGNED(base, 16) || end > dma_mask || end > GENMASK_ULL(35, 0))
			return -ERANGE;
		next.frame_addr[i] = base >> 4;
		next.stride[i] = fb->pitches[i] >> 4;
	}
	next.scope_x[0] = ((x2 - 1) << 16) | x1;
	next.scope_y[0] = ((y2 - 1) << 16) | y1;
	next.scope_x[1] = full_chroma ? next.scope_x[0] :
		(((x2 >> 1) - 1) << 16) | (x1 >> 1);
	next.scope_y[1] = fb->format->vsub == 1 ? next.scope_y[0] :
		(((y2 >> 1) - 1) << 16) | (y1 >> 1);
	*state = next;
	return 0;
}
