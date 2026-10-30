/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_WRITEBACK_H
#define __S7D_WRITEBACK_H

#include <linux/types.h>
#include <drm/drm_modes.h>

struct drm_atomic_state;
struct drm_connector;
struct drm_crtc;
struct drm_device;
struct drm_encoder;
struct s7d_crtc_state;
struct s7d_writeback;

struct s7d_writeback_buffer {
	dma_addr_t dma;
	u64 size;
	u32 format;
	u32 width;
	u32 height;
	u32 pitch;
	/* Second plane of a semi-planar format, otherwise zero. */
	dma_addr_t chroma_dma;
	u64 chroma_size;
	u32 chroma_pitch;
};

struct s7d_writeback_caps {
	const u32 *formats;
	unsigned int num_formats;
	unsigned int max_bpc;
	u32 max_width;
	u32 max_height;
	const struct drm_display_mode *modes;
	unsigned int num_modes;
};

struct s7d_writeback_result {
	int status;
	bool target_retired;
};

/*
 * check is pure. capture arms the hardware with the native CRTC mutex held;
 * status 0 with the target not retired means armed. wait then sleeps without
 * locks until the frame is written, and finish parks the hardware. cancel may
 * run instead of finish and also wakes a sleeping wait.
 */
struct s7d_writeback_ops {
	enum drm_mode_status (*mode_valid)(void *data, const struct drm_display_mode *mode);
	int (*check)(void *data, const struct s7d_crtc_state *state,
		     const struct s7d_writeback_buffer *buffer);
	struct s7d_writeback_result (*capture)(void *data,
		const struct s7d_crtc_state *state, const struct s7d_writeback_buffer *buffer);
	int (*wait)(void *data);
	struct s7d_writeback_result (*finish)(void *data, int status);
	struct s7d_writeback_result (*cancel)(void *data);
};

int s7d_writeback_init(struct drm_device *drm, struct drm_crtc *crtc,
		      const struct s7d_writeback_caps *caps,
		      const struct s7d_writeback_ops *ops, void *data,
		      struct s7d_writeback **result);
int s7d_writeback_atomic_check(struct s7d_writeback *wb, struct drm_atomic_state *state);
bool s7d_writeback_has_job(struct s7d_writeback *wb, struct drm_atomic_state *state);
int s7d_writeback_capture(struct s7d_writeback *wb, const struct s7d_crtc_state *state);
/* Call without the native CRTC mutex after s7d_writeback_capture() armed. */
int s7d_writeback_wait(struct s7d_writeback *wb);
int s7d_writeback_cancel(struct s7d_writeback *wb, int error);
/* Call after stopping commit production and successful native CRTC shutdown. */
int s7d_writeback_shutdown(struct s7d_writeback *wb);
struct drm_connector *s7d_writeback_connector(struct s7d_writeback *wb);
struct drm_encoder *s7d_writeback_encoder(struct s7d_writeback *wb);

#endif
