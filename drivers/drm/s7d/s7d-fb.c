// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#include "s7d-video.h"
#include "s7d-afbc.h"
#include "s7d-afrc.h"
#include "s7d-fb.h"

/*
 * The boot logo that a running firmware output scans out. Its memory is a
 * reservation, not a GEM object: no handle, no mapping, nothing that reads
 * the pixels. release() returns the reservation once armed.
 */
struct s7d_logo_fb {
	struct drm_framebuffer base;
	void (*release)(void *data, struct drm_framebuffer *fb);
	void *data;
	bool armed;
};

static void s7d_logo_fb_destroy(struct drm_framebuffer *fb)
{
	struct s7d_logo_fb *logo = container_of(fb, struct s7d_logo_fb, base);

	drm_framebuffer_cleanup(fb);
	if (logo->armed)
		logo->release(logo->data, fb);
	kfree(logo);
}

static const struct drm_framebuffer_funcs s7d_logo_fb_funcs = {
	.destroy = s7d_logo_fb_destroy,
};

struct drm_framebuffer *s7d_fb_create_logo(struct drm_device *dev, u32 format,
					   u32 width, u32 height, u32 pitch,
					   void (*release)(void *data, struct drm_framebuffer *fb),
					   void *data)
{
	struct s7d_logo_fb *logo;
	int ret;

	if (!release || !width || !height || !pitch)
		return ERR_PTR(-EINVAL);
	logo = kzalloc(sizeof(*logo), GFP_KERNEL);
	if (!logo)
		return ERR_PTR(-ENOMEM);
	logo->base.dev = dev;
	logo->base.format = drm_format_info(format);
	logo->base.width = width;
	logo->base.height = height;
	logo->base.pitches[0] = pitch;
	logo->base.modifier = DRM_FORMAT_MOD_LINEAR;
	logo->release = release;
	logo->data = data;
	ret = drm_framebuffer_init(dev, &logo->base, &s7d_logo_fb_funcs);
	if (ret) {
		kfree(logo);
		return ERR_PTR(ret);
	}
	return &logo->base;
}

bool s7d_fb_is_logo(const struct drm_framebuffer *fb)
{
	return fb && fb->funcs == &s7d_logo_fb_funcs;
}

void s7d_fb_arm_logo(struct drm_framebuffer *fb)
{
	if (s7d_fb_is_logo(fb))
		container_of(fb, struct s7d_logo_fb, base)->armed = true;
}

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

static const struct drm_framebuffer_funcs s7d_afrc_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

const struct s7d_afrc_framebuffer *
s7d_fb_afrc_metadata(const struct drm_framebuffer *fb)
{
	if (!fb || fb->funcs != &s7d_afrc_fb_funcs)
		return NULL;
	return container_of(fb, struct s7d_afrc_framebuffer, base);
}

/*
 * The fixed-rate payload is smaller than the linear bound that
 * drm_gem_fb_init_with_funcs() enforces from the pitch, so size it here.
 */
static struct drm_framebuffer *
s7d_fb_create_afrc(struct drm_device *dev, struct drm_file *file,
		   const struct drm_mode_fb_cmd2 *cmd)
{
	struct s7d_afrc_framebuffer *fb;
	struct s7d_afrc_buffer layout;
	struct drm_gem_object *obj;
	int ret;

	if (!IS_ENABLED(CONFIG_DRM_S7D_GFCD) ||
	    cmd->pixel_format != DRM_FORMAT_RGBA8888 ||
	    cmd->flags != DRM_MODE_FB_MODIFIERS || cmd->offsets[0] ||
	    !drm_any_plane_has_format(dev, cmd->pixel_format, cmd->modifier[0]))
		return ERR_PTR(-EINVAL);
	obj = drm_gem_object_lookup(file, cmd->handles[0]);
	if (!obj)
		return ERR_PTR(-ENOENT);
	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb) {
		ret = -ENOMEM;
		goto err_put;
	}
	drm_helper_mode_fill_fb_struct(dev, &fb->base, cmd);
	fb->base.obj[0] = obj;
	ret = s7d_afrc_build_buffer(&fb->base, dma_get_mask(dev->dev), &layout);
	if (ret)
		goto err_free;
	fb->afrc_size = layout.layout.size;
	ret = drm_framebuffer_init(dev, &fb->base, &s7d_afrc_fb_funcs);
	if (ret)
		goto err_free;
	drm_dbg_kms(dev, "AFRC fb %ux%u modifier 0x%llx pitch %u size %u\n",
		    cmd->width, cmd->height, cmd->modifier[0], cmd->pitches[0],
		    fb->afrc_size);
	return &fb->base;

err_free:
	kfree(fb);
err_put:
	drm_gem_object_put(obj);
	return ERR_PTR(ret);
}

struct drm_framebuffer *
s7d_fb_create(struct drm_device *dev, struct drm_file *file,
	      const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_afbc_framebuffer *fb;
	struct s7d_afbc_buffer layout;
	struct drm_rect source;
	int ret;

	if (cmd->modifier[0] == DRM_FORMAT_MOD_LINEAR ||
	    s7d_video_fbc_modifier(cmd->pixel_format, cmd->modifier[0]))
		return drm_gem_fb_create(dev, file, cmd);
	if (s7d_afrc_cu(cmd->modifier[0]) >= 0)
		return s7d_fb_create_afrc(dev, file, cmd);
	if ((!s7d_afbc_format(cmd->pixel_format, cmd->modifier[0]) &&
	     (!IS_ENABLED(CONFIG_DRM_S7D_GFCD) ||
	      cmd->modifier[0] != S7D_AFBC_MODIFIER ||
	      cmd->pixel_format != DRM_FORMAT_XBGR8888)) ||
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
	ret = s7d_afbc_build_buffer(fb, &source, dma_get_mask(dev->dev), &layout);
	if (ret)
		goto err_put;
	return &fb->base;

err_put:
	drm_framebuffer_put(&fb->base);
	return ERR_PTR(ret);
}
