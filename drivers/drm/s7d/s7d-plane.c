// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/dma-resv.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_plane_helper.h>

#include "s7d-plane.h"
#include "s7d-crtc.h"
#include "s7d-fb.h"

struct s7d_plane {
	struct drm_plane base;
	u64 dma_mask;
	enum s7d_plane_slot slot;
	/* Latched at creation; atomic checks never read the parameter. */
	bool gfcd_primary;
};

/* -1 keeps the Kconfig policy, 0 selects Mali AFBCD, 1 the GFCD primary. */
static int afbc_policy = -1;
module_param(afbc_policy, int, 0444);
MODULE_PARM_DESC(afbc_policy,
		 "AFBC primary decoder: -1 Kconfig default, 0 Mali AFBCD, 1 GFCD within its envelope");

/*
 * GFCD decodes only what Mali AFBCD cannot read, XBGR8888 AFBC and AFRC,
 * unless the GFCD primary policy also takes ABGR8888 inside its FHD/148.5 MHz
 * envelope. With no fallback, the GFCD checks reject the first two outside it.
 */
static bool s7d_plane_gfcd(const struct s7d_plane *plane,
			   const struct drm_plane_state *base,
			   const struct drm_crtc_state *crtc_state)
{
	if (!IS_ENABLED(CONFIG_DRM_S7D_GFCD) ||
	    plane->slot != S7D_PLANE_PRIMARY || !base->fb ||
	    base->fb->modifier == DRM_FORMAT_MOD_LINEAR)
		return false;
	if (base->fb->format->format == DRM_FORMAT_XBGR8888 ||
	    s7d_fb_afrc_metadata(base->fb))
		return true;
	return plane->gfcd_primary && crtc_state->adjusted_mode.clock <= 148500 &&
		base->src_w <= (1920 << 16) && base->src_h <= (1080 << 16) &&
		base->crtc_w <= 1920 && base->crtc_h <= 1080;
}

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
	int max_scale = DRM_PLANE_NO_SCALING;
	bool can_scale, scaled, gfcd_primary;
	int ret;

	state->osd_valid = false;
	state->video_valid = false;
	state->afbc_valid = false;
	state->gfcd_valid = false;
	state->compressed_size = 0;
	state->layer.afbc = false;
	state->layer.enabled = false;
	if (!base->crtc) {
		base->visible = false;
		return base->fb ? -EINVAL : 0;
	}
	if (!(base->rotation & DRM_MODE_ROTATE_0) ||
	    (base->rotation & ~(DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)))
		return -EINVAL;
	if (plane->slot == S7D_PLANE_CURSOR && base->fb &&
	    (base->fb->width > 256 || base->fb->height > 256))
		return -EINVAL;

	can_scale = base->fb &&
		(base->fb->modifier == DRM_FORMAT_MOD_LINEAR ||
		 base->fb->modifier == S7D_AFBC_MODIFIER ||
		 s7d_fb_afrc_metadata(base->fb)) &&
		(plane->slot == S7D_PLANE_PRIMARY || plane->slot == S7D_PLANE_RGB);
	/* Otherwise ABGR AFBC uses Mali AFBCD S0 on OSD1 or S1 on OSD2. */
	gfcd_primary = s7d_plane_gfcd(plane, base, crtc_state);
	if (gfcd_primary)
		can_scale = can_scale && base->pixel_blend_mode == DRM_MODE_BLEND_PIXEL_NONE;
	if (can_scale) {
		min_scale /= S7D_OSD_SCALER_MAX_UPSCALE;
		max_scale *= S7D_OSD_SCALER_MAX_DOWNSCALE;
	} else if (plane->slot == S7D_PLANE_VIDEO) {
		min_scale /= S7D_VIDEO_SCALER_MAX_UPSCALE;
		max_scale *= S7D_VIDEO_SCALER_MAX_DOWNSCALE;
	} else if (plane->slot == S7D_PLANE_VIDEO2) {
		min_scale /= S7D_VIDEO2_SCALER_MAX_UPSCALE;
		max_scale = max_scale * S7D_VIDEO2_SCALER_MAX_DOWNSCALE - 1;
	}
	scaled = base->src_w != ((u64)base->crtc_w << 16) ||
		base->src_h != ((u64)base->crtc_h << 16);
	ret = drm_atomic_helper_check_plane_state(base, crtc_state,
						 min_scale,
						 max_scale,
						 true, false);
	if (ret || !base->visible)
		return ret;
	if (scaled && plane->slot != S7D_PLANE_VIDEO && plane->slot != S7D_PLANE_VIDEO2 &&
	    (base->src.x1 != base->src_x || base->src.y1 != base->src_y ||
	     drm_rect_width(&base->src) != base->src_w ||
	     drm_rect_height(&base->src) != base->src_h ||
	     base->dst.x1 != base->crtc_x || base->dst.y1 != base->crtc_y ||
	     drm_rect_width(&base->dst) != base->crtc_w ||
	     drm_rect_height(&base->dst) != base->crtc_h))
		return -EINVAL;
	if (plane->slot == S7D_PLANE_VIDEO || plane->slot == S7D_PLANE_VIDEO2) {
		if (plane->slot == S7D_PLANE_VIDEO2 &&
		    (base->fb->width > 2048 || base->fb->height > 1080))
			return -EINVAL;
		ret = s7d_video_build_state(base->fb, &base->src, plane->dma_mask,
					    &state->video);
		if (!ret) {
			state->video.reflection =
				(base->rotation & DRM_MODE_REFLECT_X ? S7D_VIDEO_REFLECT_X : 0) |
				(base->rotation & DRM_MODE_REFLECT_Y ? S7D_VIDEO_REFLECT_Y : 0);
			state->video_valid = true;
		}
		return ret;
	}
	if (base->fb->modifier != DRM_FORMAT_MOD_LINEAR) {
		const struct drm_afbc_framebuffer *fb = s7d_fb_afbc_metadata(base->fb);
		const struct s7d_afrc_framebuffer *afrc = s7d_fb_afrc_metadata(base->fb);
		bool premult = base->pixel_blend_mode == DRM_MODE_BLEND_PREMULTI;
		u32 reflection =
			(base->rotation & DRM_MODE_REFLECT_X ? S7D_OSD_REFLECT_X : 0) |
			(base->rotation & DRM_MODE_REFLECT_Y ? S7D_OSD_REFLECT_Y : 0);

		if ((plane->slot != S7D_PLANE_PRIMARY && plane->slot != S7D_PLANE_RGB) ||
		    (!fb && !(afrc && gfcd_primary)) || base->alpha != DRM_BLEND_ALPHA_OPAQUE ||
		    (base->pixel_blend_mode != DRM_MODE_BLEND_PIXEL_NONE && !premult))
			return -EINVAL;
		state->compressed_size = afrc ? afrc->afrc_size : fb->afbc_size;
		if (gfcd_primary) {
			ret = afrc ?
				s7d_gfcd_build_afrc_state(afrc, &base->src, plane->dma_mask,
							  premult, &state->gfcd) :
				s7d_gfcd_build_state(fb, &base->src, plane->dma_mask,
						     premult, &state->gfcd);
			if (!ret && reflection)
				ret = s7d_gfcd_set_reflection(&state->gfcd, reflection);
			if (ret)
				return ret;
			state->layer.layout = state->gfcd.osd;
			state->gfcd_valid = true;
		} else {
			ret = s7d_afbc_build_state(fb, &base->src, plane->dma_mask,
					 plane->slot == S7D_PLANE_PRIMARY ? 0 : 1,
					 &state->afbc);
			if (!ret && premult)
				ret = s7d_afbc_set_premult(&state->afbc);
			if (!ret && reflection)
				ret = s7d_afbc_set_reflection(&state->afbc, reflection);
			if (ret)
				return ret;
			state->layer.layout = state->afbc.osd;
			state->afbc_valid = true;
		}
		state->layer.afbc = true;
	} else {
		ret = s7d_osd_build_state(base->fb, &base->src, plane->dma_mask,
					&state->layer.layout);
		if (ret)
			return ret;
		if (base->rotation & DRM_MODE_REFLECT_X)
			state->layer.layout.block_config |= BIT(28);
		if (base->rotation & DRM_MODE_REFLECT_Y)
			state->layer.layout.block_config |= BIT(29);
		if (base->pixel_blend_mode == DRM_MODE_BLEND_PIXEL_NONE)
			state->layer.layout.alpha_config =
				(state->layer.layout.alpha_config & BIT(0)) | 0x7fc0;
		if (scaled && (base->alpha != DRM_BLEND_ALPHA_OPAQUE ||
			       (state->layer.layout.alpha_config & ~BIT(0)) != 0x7fc0))
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
	bool gfcd = IS_ENABLED(CONFIG_DRM_S7D_GFCD) &&
		plane->type == DRM_PLANE_TYPE_PRIMARY;

	return modifier == DRM_FORMAT_MOD_LINEAR ||
		(modifier == S7D_AFBC_MODIFIER &&
		 (format == DRM_FORMAT_ABGR8888 ||
		  (gfcd && format == DRM_FORMAT_XBGR8888))) ||
		(gfcd && s7d_afrc_cu(modifier) >= 0 && format == DRM_FORMAT_RGBA8888);
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

/* Whether prepare_fb will make the commit wait for this buffer. */
bool s7d_plane_fence_pending(const struct drm_plane_state *state)
{
	unsigned int i;

	if (!state->fb)
		return false;
	if (state->fence)
		return true;
	for (i = 0; i < state->fb->format->num_planes; i++) {
		struct drm_gem_object *obj = drm_gem_fb_get_obj(state->fb, i);

		if (obj && !dma_resv_test_signaled(obj->resv, DMA_RESV_USAGE_WRITE))
			return true;
	}
	return false;
}

static int s7d_plane_prepare_fb(struct drm_plane *plane, struct drm_plane_state *state)
{
	bool dependency = state->fence;
	int ret = drm_gem_plane_helper_prepare_fb(plane, state);

	if (s7d_plane_is_primary(plane) && state->crtc &&
	    (dependency || state->fence))
		s7d_crtc_primary_dependency(state->crtc, state->state);
	if (s7d_plane_is_native(plane) && s7d_plane_slot(plane) == S7D_PLANE_RGB &&
	    state->crtc && (dependency || state->fence))
		s7d_crtc_osd2_dependency(state->crtc, state->state);
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
		DRM_FORMAT_RGB565, DRM_FORMAT_RGB888, DRM_FORMAT_BGR888,
		DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR2101010,
	};
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	static const u64 rgb_modifiers[] = {
		DRM_FORMAT_MOD_LINEAR, S7D_AFBC_MODIFIER, DRM_FORMAT_MOD_INVALID,
	};
	/* GFCD reads AFRC on the primary only. */
	static const u64 gfcd_modifiers[] = {
		DRM_FORMAT_MOD_LINEAR, S7D_AFBC_MODIFIER,
		S7D_AFRC_MODIFIER(16), S7D_AFRC_MODIFIER(24), S7D_AFRC_MODIFIER(32),
		DRM_FORMAT_MOD_INVALID,
	};
	const u64 *plane_modifiers = modifiers;
	const struct drm_plane_funcs *funcs = &s7d_plane_funcs;
	static const u32 video_formats[] = {
		DRM_FORMAT_NV12, DRM_FORMAT_NV21, DRM_FORMAT_NV16, DRM_FORMAT_NV61,
		DRM_FORMAT_UYVY, DRM_FORMAT_VYUY,
		DRM_FORMAT_VUY888,
	};
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
			plane_modifiers = IS_ENABLED(CONFIG_DRM_S7D_GFCD) ?
				gfcd_modifiers : rgb_modifiers;
		}
		break;
	case S7D_PLANE_RGB:
		type = DRM_PLANE_TYPE_OVERLAY;
		name = "S7D OSD2 overlay";
		zpos = 3;
		if (afbc_supported) {
			funcs = &s7d_rgb_plane_funcs;
			plane_modifiers = rgb_modifiers;
		}
		break;
	case S7D_PLANE_CURSOR:
		type = DRM_PLANE_TYPE_CURSOR;
		name = "S7D OSD2 cursor";
		zpos = 4;
		break;
	case S7D_PLANE_VIDEO:
	case S7D_PLANE_VIDEO2:
		type = DRM_PLANE_TYPE_OVERLAY;
		name = slot == S7D_PLANE_VIDEO ? "S7D VD1" : "S7D VD2";
		zpos = slot == S7D_PLANE_VIDEO ? 1 : 2;
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
	plane->gfcd_primary = IS_ENABLED(CONFIG_DRM_S7D_GFCD) &&
		(afbc_policy < 0 ? IS_ENABLED(CONFIG_DRM_S7D_AFBC_POLICY_GFCD_PRIMARY) :
		 afbc_policy == 1);
	ret = drm_plane_create_zpos_property(&plane->base, zpos, 0, S7D_DISPLAY_MAX_ZPOS);
	if (ret)
		return ERR_PTR(ret);
	ret = drm_plane_create_alpha_property(&plane->base);
	if (ret)
		return ERR_PTR(ret);
	ret = drm_plane_create_rotation_property(&plane->base, DRM_MODE_ROTATE_0,
		DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y);
	if (ret)
		return ERR_PTR(ret);
	if (slot == S7D_PLANE_VIDEO || slot == S7D_PLANE_VIDEO2) {
		ret = drm_plane_create_color_properties(&plane->base,
			BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709),
			BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) | BIT(DRM_COLOR_YCBCR_FULL_RANGE),
			DRM_COLOR_YCBCR_BT709, DRM_COLOR_YCBCR_LIMITED_RANGE);
		if (ret)
			return ERR_PTR(ret);
		drm_plane_helper_add(&plane->base, &s7d_plane_helper_funcs);
		return &plane->base;
	}
	ret = drm_plane_create_blend_mode_property(&plane->base,
		BIT(DRM_MODE_BLEND_PIXEL_NONE) | BIT(DRM_MODE_BLEND_PREMULTI) |
		BIT(DRM_MODE_BLEND_COVERAGE));
	if (ret)
		return ERR_PTR(ret);
	drm_plane_helper_add(&plane->base, &s7d_plane_helper_funcs);
	return &plane->base;
}
