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

#include "s7d-osd.h"

int s7d_osd_build_state(struct drm_framebuffer *fb, const struct drm_rect *src,
			u64 dma_mask, struct s7d_osd_state *state)
{
	struct drm_gem_dma_object *obj;
	u64 base, bytes, end, object_end;
	u32 x1, y1, x2, y2;

	if (!fb || !src || !state || !fb->format || !fb->obj[0] ||
	    fb->format->format != DRM_FORMAT_XRGB8888 ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
	if (!fb->width || !fb->height ||
	    fb->width > S7D_OSD_MAX_SIZE || fb->height > S7D_OSD_MAX_SIZE)
		return -EINVAL;
	if (src->x1 < 0 || src->y1 < 0 || src->x2 <= src->x1 || src->y2 <= src->y1 ||
	    ((src->x1 | src->y1 | src->x2 | src->y2) & 0xffff))
		return -EINVAL;
	x1 = src->x1 >> 16;
	y1 = src->y1 >> 16;
	x2 = src->x2 >> 16;
	y2 = src->y2 >> 16;
	if (x2 > fb->width || y2 > fb->height)
		return -EINVAL;

	/*
	 * The proven linear vendor path aligns rows to 64 bytes. Retain that
	 * restriction, but use the framebuffer's actual pitch, not its width.
	 * The vendor programs only the low 12 stride bits, in 16-byte units.
	 */
	if (!IS_ALIGNED(fb->pitches[0], S7D_OSD_PITCH_ALIGN) ||
	    fb->pitches[0] < fb->width * 4 ||
	    (fb->pitches[0] >> 4) > 0xfff)
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!obj)
		return -EINVAL;

	/* Include row padding so a burst cannot fetch past the GEM allocation. */
	bytes = (u64)fb->pitches[0] * fb->height;
	if (check_add_overflow((u64)fb->offsets[0], bytes, &object_end) ||
	    object_end > obj->base.size)
		return -EINVAL;
	if (check_add_overflow((u64)obj->dma_addr, (u64)fb->offsets[0], &base) ||
	    check_add_overflow(base, bytes - 1, &end))
		return -ERANGE;
	if (!IS_ALIGNED(base, 16) || end > dma_mask || end > GENMASK_ULL(35, 0))
		return -ERANGE;

	/* Base stays at the framebuffer origin; source cropping uses W1/W2. */
	*state = (struct s7d_osd_state) {
		.frame_addr = base >> 4,
		.stride = fb->pitches[0] >> 4,
		.scope_x = ((x2 - 1) << 16) | x1,
		.scope_y = ((y2 - 1) << 16) | y1,
	};
	return 0;
}
