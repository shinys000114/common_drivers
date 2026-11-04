/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HOOKS_H
#define __S7D_HOOKS_H

#include <linux/types.h>

struct drm_bridge;
struct drm_connector;
struct drm_crtc;
struct drm_device;
struct drm_display_mode;
struct drm_driver;
struct drm_plane;
struct drm_writeback_connector;

/*
 * The only calls from the display core into the optional interfaces built
 * on it. Each group below is provided by one of them; until it is, its
 * hooks do nothing and the core is the plain KMS driver.
 */

/*
 * KMS objects of one display. All but @bridge live as long as its DRM
 * device. @bridge belongs to the HDMI transmitter and is valid only while
 * the display is bound: do not use it from s7d_vendor_unregister() on, as
 * open files can keep the DRM device after the unbind.
 */
struct s7d_vendor_objs {
	struct drm_crtc *crtc;
	struct drm_plane *primary, *cursor, *overlay, *video, *video2;
	struct drm_connector *hdmi;
	struct drm_writeback_connector *writeback; /* may be NULL */
	struct drm_bridge *bridge;
};

/*
 * Vendor DRM interface (s7d_vendor_*), provided by s7d-vendor.c with
 * CONFIG_DRM_S7D_VENDOR_UAPI. All hooks run in process context and may
 * sleep.
 */

#if IS_ENABLED(CONFIG_DRM_S7D_VENDOR_UAPI)

/*
 * s7d_drm_probe() (s7d-drv.c): the driver argument of devm_drm_dev_alloc(),
 * once every supplier is ready. Returns @native, or the driver to register
 * in its place. No locks held. Cannot fail.
 */
const struct drm_driver *s7d_vendor_driver(const struct drm_driver *native);

/*
 * s7d_drm_probe(): after the max bpc property attach and
 * platform_set_drvdata(), before drm_dev_register(). Every KMS object exists
 * and drm_mode_config_reset() has created its state; userspace cannot see
 * the device yet. No locks held. @objs itself is valid for the call only.
 * There is no matching teardown: allocate with drmm. An error fails the
 * probe. The DRM minors are already taken, so -EPROBE_DEFER is refused
 * with a warning and fails the probe as -EINVAL.
 */
int s7d_vendor_init(struct drm_device *drm, const struct s7d_vendor_objs *objs);

/*
 * s7d_drm_probe(): after drm_dev_register() and drm_kms_helper_poll_init(),
 * before drm_fbdev_dma_setup(). Userspace may already use the device. No
 * locks held. An error is logged and the display stays up without the
 * vendor interfaces, so unregister must cope with a failed or partial
 * register.
 */
int s7d_vendor_register(struct drm_device *drm);

/*
 * s7d_drm_remove(): first call, before drm_dev_unplug(); the output may
 * still run. No locks held. Not called on shutdown or on a failed probe.
 */
void s7d_vendor_unregister(struct drm_device *drm);

/*
 * s7d_hdmi_report_hpd() (s7d-hdmi.c): with a detected sink status, after
 * drm_bridge_hpd_notify() has reported it to DRM, or when DRM was not told
 * but it differs from the last value passed here (a probe may have updated
 * the connector first). A same-status re-plug with the same EDID is not
 * passed on; @connected may repeat the previous value otherwise. An
 * unknown status (failed HPD acknowledge or pin read) is not passed on:
 * the last value stands until the next report, which after a failed
 * acknowledge comes from the HPD recovery work. Runs from the HPD
 * interrupt thread and that work, one call at a time under the HDMI
 * driver's report mutex, which nests outside mode_config.mutex and the
 * bridge's hpd_mutex; no DRM locks are held, and the hook must not wait
 * for HPD processing. The caller holds a reference on @drm for the call.
 *
 * Runs only while connector HPD is enabled (never with
 * drm_kms_helper.poll=0): from drm_kms_helper_poll_init() in probe, so it
 * can come before s7d_vendor_register() returns, until the poll helper
 * stops in remove, after s7d_vendor_unregister() and drm_dev_unplug().
 * System suspend turns HPD off before s7d_vendor_pm(false) and
 * drm_mode_config_helper_resume() turns it on before s7d_vendor_pm(true),
 * so no report starts in between; the transmitter resumes first and finds
 * HPD off. A report already under way when suspend turns HPD off can still
 * overlap s7d_vendor_pm(false). A sink changed while asleep is not
 * reported here; see s7d_vendor_pm().
 */
void s7d_vendor_hpd(struct drm_device *drm, bool connected);

/*
 * s7d_drm_suspend() and s7d_drm_resume() (s7d-drv.c): last call of a
 * successful system suspend, with every output and connector HPD off
 * (@resume false), and of every system resume, after
 * drm_mode_config_helper_resume() has restored the outputs and turned HPD
 * back on and after the hotplug event, even when the restore failed
 * (@resume true). An aborted suspend calls neither. No locks held; HPD
 * reports can run during the resume call. No s7d_vendor_hpd() reports
 * a sink changed while asleep, and the hotplug event only asks clients to
 * reprobe without refreshing the connector status, so on resume the vendor
 * layer must detect the sink again rather than trust any cached status.
 */
void s7d_vendor_pm(struct drm_device *drm, bool resume);

#else

static inline const struct drm_driver *s7d_vendor_driver(const struct drm_driver *native)
{
	return native;
}

static inline int s7d_vendor_init(struct drm_device *drm, const struct s7d_vendor_objs *objs)
{
	return 0;
}

static inline int s7d_vendor_register(struct drm_device *drm)
{
	return 0;
}

static inline void s7d_vendor_unregister(struct drm_device *drm)
{
}

static inline void s7d_vendor_hpd(struct drm_device *drm, bool connected)
{
}

static inline void s7d_vendor_pm(struct drm_device *drm, bool resume)
{
}

#endif

/*
 * Media glue (s7d_ext_*), provided by the glue registry in s7d-ext.c. Both
 * hooks run with the CRTC's internal mutex held, which nests inside the
 * connection mutex: they may sleep but must not take DRM modeset locks,
 * use the atomic API or wait for a commit or vblank. The CRTC cannot stop
 * during either call.
 */

/*
 * s7d_atomic_commit_tail() (s7d-drv.c), through s7d_crtc_ext_mode_set():
 * after drm_atomic_helper_commit_modeset_enables(), before the commit is
 * marked done, for a CRTC that took a modeset to active and started without
 * error. The output runs @mode, the adjusted mode, and the VPU clock and
 * power stay on for the whole call: VPU registers may be accessed. Modeset
 * locks may or may not be held, as nonblocking commits run the tail from a
 * worker. @mode points into the new CRTC state and is valid for the call
 * only: copy what is kept. Not called again before s7d_ext_mode_clear().
 */
void s7d_ext_mode_set(struct drm_crtc *crtc, const struct drm_display_mode *mode);

/*
 * s7d_crtc_stop() (s7d-crtc.c): first step, before vblank is turned off,
 * the hardware stops and the VPU clock is released, once for every
 * s7d_ext_mode_set(). Covers disable and modeset commits, system suspend,
 * the error recovery work (system workqueue) after a fault or timeout, and
 * unbind or shutdown. The output may have faulted; the VPU is still clocked.
 */
void s7d_ext_mode_clear(struct drm_crtc *crtc);

#endif
