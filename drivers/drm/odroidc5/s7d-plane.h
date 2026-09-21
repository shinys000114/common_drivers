/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_PLANE_H
#define __S7D_PLANE_H

#include <drm/drm_plane.h>

#include "s7d-osd.h"

struct s7d_plane_state {
	struct drm_plane_state base;
	struct s7d_osd_state osd;
	bool osd_valid;
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
				 unsigned int possible_crtcs, u64 dma_mask);
bool s7d_plane_is_primary(const struct drm_plane *plane);

#endif
