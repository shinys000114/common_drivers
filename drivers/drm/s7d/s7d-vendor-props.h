/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VENDOR_PROPS_H
#define __S7D_VENDOR_PROPS_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/string.h>
#include <linux/types.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>

#include "s7d-hooks.h"

struct drm_crtc;
struct drm_crtc_state;
struct drm_property;
struct s7d_vendor_props;

/*
 * Create the properties of the vendor DRM interface on @objs and attach them,
 * before the device registers. Called in vendor mode only.
 */
int s7d_vendor_props_attach(struct drm_device *drm, const struct s7d_vendor_objs *objs);

/*
 * Vendor mode: the device registered as the vendor "meson" driver. Objects
 * created before the vendor interface attaches its properties take their
 * vendor profile only then; otherwise the display behaves as without it.
 */
static inline bool s7d_vendor_props_active(const struct drm_device *drm)
{
	return IS_ENABLED(CONFIG_DRM_S7D_VENDOR_UAPI) && drm->driver->name &&
		!strcmp(drm->driver->name, "meson");
}

#if IS_ENABLED(CONFIG_DRM_S7D_VENDOR_UAPI)
/* Atomic property hooks of the CRTC; @vp is NULL without vendor properties. */
int s7d_vendor_props_crtc_get(const struct s7d_vendor_props *vp, struct drm_crtc *crtc,
			      const struct drm_crtc_state *state,
			      struct drm_property *property, u64 *val);
int s7d_vendor_props_crtc_set(const struct s7d_vendor_props *vp, struct drm_crtc *crtc,
			      struct drm_crtc_state *state,
			      struct drm_property *property, u64 val);
#else
static inline int s7d_vendor_props_crtc_get(const struct s7d_vendor_props *vp,
					    struct drm_crtc *crtc,
					    const struct drm_crtc_state *state,
					    struct drm_property *property, u64 *val)
{
	return -EINVAL;
}

static inline int s7d_vendor_props_crtc_set(const struct s7d_vendor_props *vp,
					    struct drm_crtc *crtc,
					    struct drm_crtc_state *state,
					    struct drm_property *property, u64 val)
{
	return -EINVAL;
}
#endif

#endif
