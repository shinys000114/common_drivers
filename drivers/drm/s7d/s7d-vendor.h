/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VENDOR_H
#define __S7D_VENDOR_H

#include <linux/atomic.h>
#include <linux/hdmi.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_modeset_helper_vtables.h>

struct dentry;
struct device;
struct dma_fence;
struct drm_connector;
struct drm_crtc;
struct drm_device;
struct drm_display_info;
struct drm_display_mode;
struct drm_file;

/*
 * The present fence of the CRTC (CREAT_PRESENT_FENCE): at most one at a
 * time, signalled from the vblank wait queue at the next vblank, or when the
 * CRTC stops, and holding a vblank reference meanwhile.
 */
struct s7d_vendor_present {
	struct drm_crtc *crtc;
	wait_queue_head_t *vblank;
	struct wait_queue_entry wait;
	struct work_struct put_work;
	u64 context;
	atomic64_t seqno;
	/* Under the vblank wait queue lock. */
	struct dma_fence *fence;
	unsigned int puts;
	bool closed;
};

/*
 * Vendor DRM interface state of one display in vendor mode. Allocated with
 * drmm in s7d_vendor_init(), so it lives as long as the DRM device.
 */
struct s7d_vendor {
	struct drm_device *drm;
	struct list_head node;
	struct drm_crtc *crtc;
	struct drm_connector *hdmi;
	/* The HDMI connector helpers, renaming the modes the native ones probe. */
	struct drm_connector_helper_funcs helpers;
	int (*get_modes)(struct drm_connector *connector);
	struct dentry *debugfs;
	struct s7d_vendor_present present;
	int card_minor;
	int render_minor;
	bool node_order_ok;
	/* MUTE_PLANE on video: kept for the whole device until unmuted. */
	bool video_mute;
	bool display_class;
	/* HPD and vendor uevent state, and the amhdmitx0 device that sends them. */
	struct mutex lock;
	struct device *amhdmitx;
	bool registered;
	/* HPD reports so far, and the last one; received even before register. */
	unsigned int hpd_reports;
	bool hpd_report;
	/* The last hdmitx_hpd and hdmitx_hdcppwr sent, -1 before the first. */
	int hpd;
	int hdcppwr;
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

/* An HDMI output format of a vendor attribute string. */
struct s7d_vendor_attr {
	enum hdmi_colorspace colorspace;
	unsigned int bpc;
};

/* The vendor state of @drm, or NULL outside vendor mode. */
struct s7d_vendor *s7d_vendor_find(const struct drm_device *drm);

/* Vendor ioctls (s7d-vendor-ioctl.c). */
int s7d_vendor_gem_create_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int s7d_vendor_rmfb_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int s7d_vendor_export_sync_file_ioctl(struct drm_device *drm, void *data,
				      struct drm_file *file);
int s7d_vendor_prime_handle_to_fd_ioctl(struct drm_device *drm, void *data,
					struct drm_file *file);
int s7d_vendor_present_fence_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int s7d_vendor_mute_plane_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int s7d_vendor_vrr_range_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int s7d_vendor_connector_force_ioctl(struct drm_device *drm, void *data, struct drm_file *file);

/* Vendor mode names and the mode test (s7d-vendor-modes.c). */
void s7d_vendor_mode_name(const struct drm_display_mode *mode, char *name);
void s7d_vendor_modes_init(struct s7d_vendor *v);
void s7d_vendor_parse_attr(const char *text, struct s7d_vendor_attr *attr);
bool s7d_vendor_attr_valid(const struct drm_display_info *info,
			   const struct drm_display_mode *mode,
			   const struct s7d_vendor_attr *attr);
int s7d_vendor_testattr_ioctl(struct drm_device *drm, void *data, struct drm_file *file);

/* Vendor sysfs and uevents (s7d-vendor-sysfs.c). */
void s7d_vendor_sysfs_register(struct s7d_vendor *v);
void s7d_vendor_sysfs_unregister(struct s7d_vendor *v);
void s7d_vendor_sysfs_hpd(struct s7d_vendor *v, bool connected, bool force);
void s7d_vendor_sysfs_power(struct s7d_vendor *v, bool on);
void s7d_vendor_present_init(struct s7d_vendor *v);
void s7d_vendor_present_fini(struct s7d_vendor *v);

#endif
