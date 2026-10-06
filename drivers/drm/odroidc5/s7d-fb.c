// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_rect.h>

#include "s7d-afbc.h"
#include "s7d-fb.h"

static const struct drm_framebuffer_funcs s7d_afbc_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

const struct drm_afbc_framebuffer *
s7d_fb_afbc_metadata(const struct drm_framebuffer *fb)
{
	if (!fb || fb->funcs != &s7d_afbc_fb_funcs)
		return NULL;
	return container_of(fb, struct drm_afbc_framebuffer, base);
}

struct drm_framebuffer *
s7d_fb_create(struct drm_device *dev, struct drm_file *file,
	      const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_afbc_framebuffer *fb;
	struct s7d_afbc_state layout;
	struct drm_rect source;
	int ret;

	if (cmd->modifier[0] == DRM_FORMAT_MOD_LINEAR)
		return drm_gem_fb_create(dev, file, cmd);
	if (cmd->modifier[0] != S7D_AFBC_MODIFIER ||
	    cmd->pixel_format != DRM_FORMAT_ABGR8888 ||
	    cmd->flags != DRM_MODE_FB_MODIFIERS ||
	    !cmd->width || !cmd->height || cmd->width > S7D_AFBC_MAX_WIDTH ||
	    cmd->height > S7D_AFBC_MAX_HEIGHT || cmd->offsets[0])
		return ERR_PTR(-EINVAL);

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return ERR_PTR(-ENOMEM);
	ret = drm_gem_fb_init_with_funcs(dev, &fb->base, file, cmd, &s7d_afbc_fb_funcs);
	if (ret) {
		kfree(fb);
		return ERR_PTR(ret);
	}
	ret = drm_gem_fb_afbc_init(dev, cmd, fb);
	if (ret)
		goto err_put;
	source = DRM_RECT_INIT(0, 0, cmd->width << 16, cmd->height << 16);
	ret = s7d_afbc_build_state(fb, &source, dma_get_mask(dev->dev),
				   S7D_AFBC_SURFACE, &layout);
	if (ret)
		goto err_put;
	return &fb->base;

err_put:
	drm_framebuffer_put(&fb->base);
	return ERR_PTR(ret);
}
