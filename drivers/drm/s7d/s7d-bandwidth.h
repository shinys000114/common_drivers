/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_BANDWIDTH_H
#define __S7D_BANDWIDTH_H

#include <drm/drm_rect.h>

#include "s7d-caps.h"

struct drm_framebuffer;

struct s7d_bandwidth_input {
	const struct drm_framebuffer *fb;
	struct drm_rect src;
	struct drm_rect dst;
	u32 compressed_size;
};

struct s7d_bandwidth_fetch {
	u64 payload_bytes;
	u64 read_bytes;
	u64 average_bps;
	u64 window_bps;
};

struct s7d_bandwidth_state {
	struct s7d_bandwidth_fetch input[S7D_INPUT_COUNT];
	u64 payload_bytes;
	u64 read_bytes;
	u64 average_bps;
	u64 window_bps;
	u64 rdma_bytes;
};

/* Display-fetch demand, not a guarantee of shared DDR service. */
int s7d_bandwidth_build(const struct drm_display_mode *mode,
			const struct s7d_bandwidth_input inputs[S7D_INPUT_COUNT],
			unsigned int rdma_entries, struct s7d_bandwidth_state *state);

#endif
