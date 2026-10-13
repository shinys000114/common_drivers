/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_PLANE_H
#define __S7D_PLANE_H

#include <drm/drm_plane.h>

#include "s7d-osd.h"
#include "s7d-afbc.h"
#include "s7d-osd-pipeline.h"
#include "s7d-video.h"

enum s7d_plane_slot {
	S7D_PLANE_PRIMARY,
	S7D_PLANE_RGB,
	S7D_PLANE_VIDEO,
	S7D_PLANE_CURSOR,
};

struct s7d_plane_state {
	struct drm_plane_state base;
	struct s7d_osd_layer layer;
	struct s7d_video_state video;
	struct s7d_afbc_state afbc;
	bool osd_valid;
	bool video_valid;
	bool afbc_valid;
};

static inline struct s7d_plane_state *
to_s7d_plane_state(struct drm_plane_state *state)
{
	return container_of(state, struct s7d_plane_state, base);
}

/*
 * The CRTC consumes checked osd state in its commit path and owns RDMA,
 * completion and scanout retirement. Plane callbacks never access hardware.
 */
struct drm_plane *s7d_plane_create(struct drm_device *drm,
				 unsigned int possible_crtcs, u64 dma_mask,
				 enum s7d_plane_slot slot, bool afbc_supported);
bool s7d_plane_is_native(const struct drm_plane *plane);
enum s7d_plane_slot s7d_plane_slot(const struct drm_plane *plane);
bool s7d_plane_is_primary(const struct drm_plane *plane);

#endif
