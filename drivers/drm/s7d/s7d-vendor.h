/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VENDOR_H
#define __S7D_VENDOR_H

#include <linux/list.h>
#include <linux/types.h>

struct dentry;
struct drm_connector;
struct drm_crtc;
struct drm_device;

/*
 * Vendor DRM interface state of one display in vendor mode. Allocated with
 * drmm in s7d_vendor_init(), so it lives as long as the DRM device.
 */
struct s7d_vendor {
	struct drm_device *drm;
	struct list_head node;
	struct drm_crtc *crtc;
	struct drm_connector *hdmi;
	struct dentry *debugfs;
	int card_minor;
	int render_minor;
	bool node_order_ok;
};

#if IS_ENABLED(CONFIG_DRM_S7D_VENDOR_UAPI)
/* Whether @drm was allocated as the vendor DRM device; valid from the alloc on. */
bool s7d_vendor_enabled(const struct drm_device *drm);
#else
static inline bool s7d_vendor_enabled(const struct drm_device *drm)
{
	return false;
}
#endif

/* The vendor state of @drm, or NULL outside vendor mode. */
struct s7d_vendor *s7d_vendor_find(const struct drm_device *drm);

#endif
