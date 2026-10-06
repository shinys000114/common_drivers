/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_CRTC_H
#define __S7D_CRTC_H

#include <drm/drm_crtc.h>

#include "s7d-encp.h"
#include "s7d-csc.h"
#include "s7d-osd-pipeline.h"
#include "s7d-postblend.h"
#include "s7d-scanout.h"
#include "s7d-video-pipeline.h"

#define S7D_CRTC_UPDATE_REG_COUNT (S7D_OSD_UPDATE_REG_COUNT + \
	S7D_VIDEO_UPDATE_REG_COUNT + S7D_CSC_MATRIX_REG_COUNT + \
	S7D_POSTBLEND_REG_COUNT + 1)

struct s7d_crtc_state {
	struct drm_crtc_state base;
	struct s7d_encp_state encp;
	struct s7d_osd_pipeline_state osd;
	struct s7d_video_pipeline_state video;
	struct s7d_csc_state csc;
	struct s7d_postblend_state postblend;
	struct s7d_rdma_entry update[S7D_CRTC_UPDATE_REG_COUNT];
	unsigned int update_count;
	/* Borrowed from the checked atomic plane states until scanout takes refs. */
	struct s7d_scanout_buffers buffers;
	unsigned long pixel_rate;
	bool valid;
};

static inline struct s7d_crtc_state *
to_s7d_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct s7d_crtc_state, base);
}

/*
 * Mandatory VPU backend, owned by the platform driver, not vendor globals.
 * check is pure: validate clock/PLL/TMDS/bandwidth feasibility, with no clock,
 * PM, MMIO writes or references. Bridge checks may further restrict the mode.
 *
 * acquire obtains clock/PM references without changing the running output.
 * On failure it unwinds its references; prepare/stop will not be called.
 * prepare runs with those references held, takes over/drains firmware DMA, prepares
 * RDMA and programs VPP/ENCP/OSD, leaving VENC STOPPED. The CRTC already holds
 * the new framebuffer. Even on error keep acquired resources for stop.
 * Successful prepare also masks, acknowledges and synchronizes old display
 * IRQs; no IRQ may be delivered until enable_vblank/start. This is the boundary
 * at which a new start can clear a previous error without losing a new fault.
 * start enables VENC; return an error on failed readback. stop disables and
 * drains ALL fetchers and masks/synchronizes display IRQs, retaining resources
 * for the CRTC's RDMA reset check. release runs only after successful stop AND
 * scanout quiesce. Failed stop must keep resources alive for a later retry.
 *
 * vblank hooks cannot sleep and must not access unpowered hardware. report_error
 * runs in work context without the CRTC mutex; report diagnostics/hotplug, do
 * not free buffers or resources. Recheck last_error before changing link status
 * since a recovery modeset may already have succeeded.
 */
struct s7d_crtc_ops {
	/* Defer initial completion until bridge reports link_ready or failure. */
	bool wait_for_link;
	int (*check)(void *data, const struct s7d_crtc_state *state);
	int (*acquire)(void *data);
	int (*prepare)(void *data, const struct s7d_crtc_state *state);
	int (*start)(void *data);
	int (*stop)(void *data);
	void (*release)(void *data);
	int (*enable_vblank)(void *data);
	void (*disable_vblank)(void *data);
	void (*report_error)(void *data, int error);
};

struct drm_crtc *s7d_crtc_create(struct drm_device *drm, struct drm_plane *primary,
			       struct drm_plane *cursor,
			       u8 revision, struct s7d_scanout *scanout,
			       const struct s7d_crtc_ops *ops, void *data);

/* Account each real acknowledged vblank/RDMA result exactly once. */
void s7d_crtc_irq(struct drm_crtc *crtc, bool vblank, enum s7d_rdma_result result,
		  const struct s7d_frame_state *frame);
bool s7d_crtc_is_native(const struct drm_crtc *crtc);
int s7d_crtc_last_error(struct drm_crtc *crtc);
/* Bridge completion/error, after atomic_enable; never used from atomic_check. */
void s7d_crtc_link_ready(struct drm_crtc *crtc);
void s7d_crtc_link_error(struct drm_crtc *crtc, int error);

/*
 * Before freeing backend resources, stop new commits and call shutdown. It
 * may sleep. A failure is NOT permission to unmap/power down/free scanout.
 * After success prevent further IRQ calls before freeing the DRM device.
 * Creation/reset/check do not take over or disable a firmware display.
 */
int s7d_crtc_shutdown(struct drm_crtc *crtc);

#endif
