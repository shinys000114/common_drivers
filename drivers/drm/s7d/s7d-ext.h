/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_EXT_H
#define __S7D_EXT_H

struct drm_crtc;
struct drm_display_mode;

/*
 * Media glue registry: lets one module that links the vendor media stack
 * follow the display without the core linking that stack. The functions
 * below are exported in the S7D_DRM symbol namespace. The registry holds no
 * reference on the glue module: the glue registers from its module init and
 * unregisters from its module exit, which ends every call into it.
 *
 * The ops are called one at a time, also against s7d_ext_register() and
 * s7d_ext_unregister(), in process context. They may sleep, but must not
 * take DRM modeset locks, use the atomic API, wait for a commit or vblank,
 * or call the registry: the CRTC stop waits for them.
 *
 * @mode_set: @crtc started @mode, which stays valid for the call only. The
 *	output runs and the VPU is clocked and powered for the whole call, so
 *	listeners may access VPU registers. Called from the commit tail, or
 *	from s7d_ext_register() when a mode already runs; that replay holds no
 *	CRTC lock, but the CRTC cannot stop before it returns. Not called again
 *	before @mode_clear.
 * @mode_clear: the output of @crtc is about to stop, once for every
 *	@mode_set. The VPU is still clocked; the output may have faulted.
 *	Called from the CRTC stop, or from s7d_ext_unregister() when a mode
 *	still runs.
 */
struct s7d_ext_ops {
	void (*mode_set)(struct drm_crtc *crtc, const struct drm_display_mode *mode);
	void (*mode_clear)(struct drm_crtc *crtc);
};

/*
 * One glue at a time; -EBUSY when another one is registered. A running mode
 * is passed to @ops->mode_set before this returns.
 */
int s7d_ext_register(const struct s7d_ext_ops *ops);
/*
 * Ends the calls into @ops, after @ops->mode_clear when a mode still runs.
 * No op of @ops runs or starts once this returns.
 */
void s7d_ext_unregister(const struct s7d_ext_ops *ops);

#endif
