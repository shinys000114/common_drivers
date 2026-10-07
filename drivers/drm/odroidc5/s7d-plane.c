// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_plane_helper.h>

#include "s7d-plane.h"
#include "s7d-crtc.h"
#include "s7d-fb.h"

struct s7d_plane {
	struct drm_plane base;
	u64 dma_mask;
	enum s7d_plane_slot slot;
};

static void s7d_plane_destroy_state(struct drm_plane *plane,
				    struct drm_plane_state *state)
{
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(to_s7d_plane_state(state));
}

static void s7d_plane_reset(struct drm_plane *plane)
{
	struct s7d_plane_state *state;

	if (plane->state)
		s7d_plane_destroy_state(plane, plane->state);
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	__drm_atomic_helper_plane_reset(plane, state ? &state->base : NULL);
}

static struct drm_plane_state *s7d_plane_duplicate_state(struct drm_plane *plane)
{
	struct s7d_plane_state *state;

	if (!plane->state)
		return NULL;
	state = kmemdup(to_s7d_plane_state(plane->state), sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	__drm_atomic_helper_plane_duplicate_state(plane, &state->base);
	return &state->base;
}

static int s7d_plane_check(struct s7d_plane *plane, struct drm_plane_state *base,
			   const struct drm_crtc_state *crtc_state)
{
	struct s7d_plane_state *state = to_s7d_plane_state(base);
	int min_scale = DRM_PLANE_NO_SCALING;
	bool can_scale, scaled;
	int ret;

	state->osd_valid = false;
	state->video_valid = false;
	state->afbc_valid = false;
	state->layer.afbc = false;
	state->layer.enabled = false;
	if (!base->crtc) {
		base->visible = false;
		return base->fb ? -EINVAL : 0;
	}
	if (base->rotation != DRM_MODE_ROTATE_0)
		return -EINVAL;
	if (plane->slot == S7D_PLANE_CURSOR && base->fb &&
	    (base->fb->width > 256 || base->fb->height > 256))
		return -EINVAL;

	can_scale = base->fb && base->fb->modifier == DRM_FORMAT_MOD_LINEAR &&
		(plane->slot == S7D_PLANE_PRIMARY || plane->slot == S7D_PLANE_RGB);
	if (can_scale)
		min_scale /= S7D_OSD_SCALER_MAX_UPSCALE;
	else if (plane->slot == S7D_PLANE_VIDEO)
		min_scale /= 4;
	scaled = base->src_w != ((u64)base->crtc_w << 16) ||
		base->src_h != ((u64)base->crtc_h << 16);
	ret = drm_atomic_helper_check_plane_state(base, crtc_state,
						 min_scale,
						 DRM_PLANE_NO_SCALING,
						 plane->slot != S7D_PLANE_PRIMARY, false);
	if (ret || !base->visible)
		return ret;
	if (scaled && plane->slot != S7D_PLANE_VIDEO &&
	    (base->src.x1 != base->src_x || base->src.y1 != base->src_y ||
	     drm_rect_width(&base->src) != base->src_w ||
	     drm_rect_height(&base->src) != base->src_h ||
	     base->dst.x1 != base->crtc_x || base->dst.y1 != base->crtc_y ||
	     drm_rect_width(&base->dst) != base->crtc_w ||
	     drm_rect_height(&base->dst) != base->crtc_h))
		return -EINVAL;
	if (plane->slot == S7D_PLANE_VIDEO) {
		ret = s7d_video_build_state(base->fb, &base->src, plane->dma_mask,
					    &state->video);
		if (!ret)
			state->video_valid = true;
		return ret;
	}
	if (base->fb->modifier != DRM_FORMAT_MOD_LINEAR) {
		const struct drm_afbc_framebuffer *fb = s7d_fb_afbc_metadata(base->fb);

		if ((plane->slot != S7D_PLANE_PRIMARY && plane->slot != S7D_PLANE_RGB) ||
		    !fb || base->alpha != DRM_BLEND_ALPHA_OPAQUE ||
		    (base->pixel_blend_mode != DRM_MODE_BLEND_PIXEL_NONE &&
		     (plane->slot != S7D_PLANE_PRIMARY ||
		      base->pixel_blend_mode != DRM_MODE_BLEND_PREMULTI)))
			return -EINVAL;
		ret = s7d_afbc_build_state(fb, &base->src, plane->dma_mask,
					 plane->slot == S7D_PLANE_PRIMARY ? 0 : 1,
					 &state->afbc);
		if (!ret && base->pixel_blend_mode == DRM_MODE_BLEND_PREMULTI)
			ret = s7d_afbc_set_premult(&state->afbc);
		if (ret)
			return ret;
		state->layer.layout = state->afbc.osd;
		state->layer.afbc = true;
		state->afbc_valid = true;
	} else {
		ret = s7d_osd_build_state(base->fb, &base->src, plane->dma_mask,
					&state->layer.layout);
		if (ret)
			return ret;
		if (base->pixel_blend_mode == DRM_MODE_BLEND_PIXEL_NONE)
			state->layer.layout.alpha_config = 0x7fc0;
		if (scaled && (base->alpha != DRM_BLEND_ALPHA_OPAQUE ||
			       state->layer.layout.alpha_config != 0x7fc0))
			return -EINVAL;
	}
	state->layer.dst = base->dst;
	state->layer.alpha = DIV_ROUND_CLOSEST(base->alpha, 256);
	state->layer.premult = base->pixel_blend_mode != DRM_MODE_BLEND_COVERAGE;
	state->layer.enabled = true;
	state->osd_valid = true;
	return 0;

}

static int s7d_plane_atomic_check(struct drm_plane *plane,
				  struct drm_atomic_state *atomic)
{
	struct drm_plane_state *state = drm_atomic_get_new_plane_state(atomic, plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (state->crtc) {
		crtc_state = drm_atomic_get_crtc_state(atomic, state->crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);
	}
	return s7d_plane_check(container_of(plane, struct s7d_plane, base),
			       state, crtc_state);
}

static bool s7d_plane_format_mod_supported(struct drm_plane *plane,
					   u32 format, u64 modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR;
}

static bool s7d_rgb_format_mod_supported(struct drm_plane *plane,
					 u32 format, u64 modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR ||
		(format == DRM_FORMAT_ABGR8888 && modifier == S7D_AFBC_MODIFIER);
}

static const struct drm_plane_funcs s7d_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = s7d_plane_reset,
	.atomic_duplicate_state = s7d_plane_duplicate_state,
	.atomic_destroy_state = s7d_plane_destroy_state,
	.format_mod_supported = s7d_plane_format_mod_supported,
};

static const struct drm_plane_funcs s7d_rgb_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = s7d_plane_reset,
	.atomic_duplicate_state = s7d_plane_duplicate_state,
	.atomic_destroy_state = s7d_plane_destroy_state,
	.format_mod_supported = s7d_rgb_format_mod_supported,
};

static void s7d_plane_atomic_update(struct drm_plane *plane,
				    struct drm_atomic_state *atomic)
{
	/*
	 * The commit helper calls this for both updates and disables. OSD writes
	 * belong to CRTC enable/flush/disable so timing and framebuffer lifetime
	 * remain one transaction. The helper still requires a callable hook.
	 */
}

static int s7d_plane_prepare_fb(struct drm_plane *plane, struct drm_plane_state *state)
{
	bool dependency = state->fence;
	int ret = drm_gem_plane_helper_prepare_fb(plane, state);

	if (s7d_plane_is_primary(plane) && state->crtc &&
	    (dependency || state->fence))
		s7d_crtc_primary_dependency(state->crtc, state->state);
	return ret;
}

static const struct drm_plane_helper_funcs s7d_plane_helper_funcs = {
	.prepare_fb = s7d_plane_prepare_fb,
	.atomic_check = s7d_plane_atomic_check,
	.atomic_update = s7d_plane_atomic_update,
};

bool s7d_plane_is_native(const struct drm_plane *plane)
{
	return plane && (plane->funcs == &s7d_plane_funcs ||
			 plane->funcs == &s7d_rgb_plane_funcs) &&
		plane->helper_private == &s7d_plane_helper_funcs;
}

enum s7d_plane_slot s7d_plane_slot(const struct drm_plane *plane)
{
	return container_of(plane, struct s7d_plane, base)->slot;
}

bool s7d_plane_is_primary(const struct drm_plane *plane)
{
	return s7d_plane_is_native(plane) && s7d_plane_slot(plane) == S7D_PLANE_PRIMARY;
}

struct drm_plane *s7d_plane_create(struct drm_device *drm,
				 unsigned int possible_crtcs, u64 dma_mask,
				 enum s7d_plane_slot slot, bool afbc_supported)
{
	static const u32 formats[] = {
		DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888,
		DRM_FORMAT_RGBX8888, DRM_FORMAT_BGRX8888,
		DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888,
		DRM_FORMAT_RGBA8888, DRM_FORMAT_BGRA8888,
	};
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	static const u64 rgb_modifiers[] = {
		DRM_FORMAT_MOD_LINEAR, S7D_AFBC_MODIFIER, DRM_FORMAT_MOD_INVALID,
	};
	const u64 *plane_modifiers = modifiers;
	const struct drm_plane_funcs *funcs = &s7d_plane_funcs;
	static const u32 video_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_NV21 };
	const u32 *plane_formats = formats;
	unsigned int format_count = ARRAY_SIZE(formats);
	struct s7d_plane *plane;
	enum drm_plane_type type;
	const char *name;
	unsigned int zpos;
	int ret;

	switch (slot) {
	case S7D_PLANE_PRIMARY:
		type = DRM_PLANE_TYPE_PRIMARY;
		name = "S7D OSD1";
		zpos = 0;
		if (afbc_supported) {
			funcs = &s7d_rgb_plane_funcs;
			plane_modifiers = rgb_modifiers;
		}
		break;
	case S7D_PLANE_RGB:
		type = DRM_PLANE_TYPE_OVERLAY;
		name = "S7D OSD2 overlay";
		zpos = 2;
		if (afbc_supported) {
			funcs = &s7d_rgb_plane_funcs;
			plane_modifiers = rgb_modifiers;
		}
		break;
	case S7D_PLANE_CURSOR:
		type = DRM_PLANE_TYPE_CURSOR;
		name = "S7D OSD2 cursor";
		zpos = 3;
		break;
	case S7D_PLANE_VIDEO:
		type = DRM_PLANE_TYPE_OVERLAY;
		name = "S7D VD1";
		zpos = 1;
		plane_formats = video_formats;
		format_count = ARRAY_SIZE(video_formats);
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	plane = drmm_universal_plane_alloc(drm, struct s7d_plane, base,
					  possible_crtcs, funcs,
					  plane_formats, format_count, plane_modifiers,
					  type, "%s", name);
	if (IS_ERR(plane))
		return ERR_CAST(plane);
	plane->dma_mask = dma_mask;
	plane->slot = slot;
	ret = drm_plane_create_zpos_immutable_property(&plane->base, zpos);
	if (ret)
		return ERR_PTR(ret);
	if (slot == S7D_PLANE_VIDEO) {
		ret = drm_plane_create_color_properties(&plane->base,
			BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709),
			BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) | BIT(DRM_COLOR_YCBCR_FULL_RANGE),
			DRM_COLOR_YCBCR_BT709, DRM_COLOR_YCBCR_LIMITED_RANGE);
		if (ret)
			return ERR_PTR(ret);
		drm_plane_helper_add(&plane->base, &s7d_plane_helper_funcs);
		return &plane->base;
	}
	ret = drm_plane_create_alpha_property(&plane->base);
	if (ret)
		return ERR_PTR(ret);
	ret = drm_plane_create_blend_mode_property(&plane->base,
		BIT(DRM_MODE_BLEND_PIXEL_NONE) | BIT(DRM_MODE_BLEND_PREMULTI) |
		BIT(DRM_MODE_BLEND_COVERAGE));
	if (ret)
		return ERR_PTR(ret);
	drm_plane_helper_add(&plane->base, &s7d_plane_helper_funcs);
	return &plane->base;
}
