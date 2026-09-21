// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_plane_helper.h>

#include "s7d-plane.h"

struct s7d_plane {
	struct drm_plane base;
	u64 dma_mask;
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
	int ret;

	state->osd_valid = false;
	if (!base->crtc) {
		base->visible = false;
		return base->fb ? -EINVAL : 0;
	}
	if (base->rotation != DRM_MODE_ROTATE_0 ||
	    base->alpha != DRM_BLEND_ALPHA_OPAQUE)
		return -EINVAL;

	ret = drm_atomic_helper_check_plane_state(base, crtc_state,
						 DRM_PLANE_NO_SCALING,
						 DRM_PLANE_NO_SCALING,
						 false, false);
	if (ret || !base->visible)
		return ret;
	ret = s7d_osd_build_state(base->fb, &base->src, plane->dma_mask, &state->osd);
	if (!ret)
		state->osd_valid = true;
	return ret;
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

static const struct drm_plane_funcs s7d_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = s7d_plane_reset,
	.atomic_duplicate_state = s7d_plane_duplicate_state,
	.atomic_destroy_state = s7d_plane_destroy_state,
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

static const struct drm_plane_helper_funcs s7d_plane_helper_funcs = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = s7d_plane_atomic_check,
	.atomic_update = s7d_plane_atomic_update,
};

bool s7d_plane_is_primary(const struct drm_plane *plane)
{
	return plane && plane->funcs == &s7d_plane_funcs &&
		plane->helper_private == &s7d_plane_helper_funcs;
}

struct drm_plane *s7d_plane_create(struct drm_device *drm,
				 unsigned int possible_crtcs, u64 dma_mask)
{
	static const u32 formats[] = {
		DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888,
		DRM_FORMAT_RGBX8888, DRM_FORMAT_BGRX8888,
	};
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct s7d_plane *plane;

	plane = drmm_universal_plane_alloc(drm, struct s7d_plane, base,
					  possible_crtcs, &s7d_plane_funcs,
					  formats, ARRAY_SIZE(formats), modifiers,
					  DRM_PLANE_TYPE_PRIMARY, "S7D OSD1");
	if (IS_ERR(plane))
		return ERR_CAST(plane);
	plane->dma_mask = dma_mask;
	drm_plane_helper_add(&plane->base, &s7d_plane_helper_funcs);
	return &plane->base;
}
