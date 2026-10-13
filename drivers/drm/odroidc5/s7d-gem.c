// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/align.h>
#include <linux/mm.h>

#include <drm/drm_gem_dma_helper.h>

#include "s7d-gem.h"
#include "s7d-osd.h"

int s7d_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			struct drm_mode_create_dumb *args)
{
	struct drm_mode_create_dumb aligned = *args;
	int ret;

	if (!args->width || !args->height || args->flags ||
	    (args->bpp != 8 && args->bpp != 32) ||
	    args->width > S7D_OSD_MAX_SIZE || args->height > S7D_OSD_MAX_SIZE)
		return -EINVAL;

	aligned.pitch = ALIGN(args->width * (args->bpp / 8), S7D_OSD_PITCH_ALIGN);
	aligned.size = PAGE_ALIGN((u64)aligned.pitch * args->height);
	ret = drm_gem_dma_dumb_create_internal(file, drm, &aligned);
	if (ret)
		return ret;
	*args = aligned;
	return 0;
}
