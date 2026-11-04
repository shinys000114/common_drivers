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

struct drm_connector;
struct drm_connector_state;
struct drm_crtc;
struct drm_crtc_state;
struct drm_plane;
struct drm_plane_state;
struct drm_property;
struct s7d_vendor_props;

/*
 * Values of the vendor HDMI connector properties, carried by the connector
 * state in vendor mode only.
 */
struct s7d_vendor_conn_values {
	u32 hdr_priority;
	/* Last color_space (HDMI_COLORSPACE_*) and color_depth (bits) written */
	u8 color_space;
	u8 color_depth;
	/* What the last modeset sent, as color_space and color_depth read it */
	u8 output_space;
	u8 output_depth;
	u8 allm;
	u8 scan_info;
	/* color_space or color_depth asked for an output format */
	bool request;
	/* The output runs a CEA rate divided by 1.001. */
	bool fractional;
	/* A mode was set since the connector reset. */
	bool used;
	/* As in the vendor driver, these hold for the commit that sets them. */
	bool avmute;
	bool depth_written;
	bool update;
};

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
/* Atomic property hooks of the planes; @vp is NULL without vendor properties. */
int s7d_vendor_props_plane_get(const struct s7d_vendor_props *vp, struct drm_plane *plane,
			       const struct drm_plane_state *state,
			       struct drm_property *property, u64 *val);
int s7d_vendor_props_plane_set(const struct s7d_vendor_props *vp, struct drm_plane *plane,
			       struct drm_plane_state *state,
			       struct drm_property *property, u64 val);

/* The vendor values of an HDMI connector state, or NULL outside vendor mode. */
const struct s7d_vendor_conn_values *
s7d_vendor_props_conn(const struct drm_connector_state *state);
/*
 * HDMI atomic check: record the format (HDMI_COLORSPACE_*, bits per component)
 * and rate an active output takes in @state, for the vendor properties.
 */
void s7d_vendor_props_set_output(struct drm_connector_state *state, u8 color_space,
				 u8 depth, bool fractional);
/* color_space and color_depth as read from @state: RGB 8-bit by default */
void s7d_vendor_props_attr(const struct drm_connector_state *state, u8 *color_space,
			   u8 *depth);
/* meson.crtc.hdr_policy of @state */
u8 s7d_vendor_props_hdr_policy(const struct drm_crtc_state *state);
/* The sink accepts ALLM in the HDMI Forum VSIF. */
bool s7d_vendor_props_sink_allm(struct drm_connector *connector);
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

static inline int s7d_vendor_props_plane_get(const struct s7d_vendor_props *vp,
					     struct drm_plane *plane,
					     const struct drm_plane_state *state,
					     struct drm_property *property, u64 *val)
{
	return -EINVAL;
}

static inline int s7d_vendor_props_plane_set(const struct s7d_vendor_props *vp,
					     struct drm_plane *plane,
					     struct drm_plane_state *state,
					     struct drm_property *property, u64 val)
{
	return -EINVAL;
}

static inline const struct s7d_vendor_conn_values *
s7d_vendor_props_conn(const struct drm_connector_state *state)
{
	return NULL;
}

static inline void s7d_vendor_props_set_output(struct drm_connector_state *state,
					       u8 color_space, u8 depth, bool fractional)
{
}

static inline void s7d_vendor_props_attr(const struct drm_connector_state *state,
					 u8 *color_space, u8 *depth)
{
	*color_space = 0;
	*depth = 8;
}

static inline u8 s7d_vendor_props_hdr_policy(const struct drm_crtc_state *state)
{
	return 0;
}

static inline bool s7d_vendor_props_sink_allm(struct drm_connector *connector)
{
	return false;
}
#endif

#endif
