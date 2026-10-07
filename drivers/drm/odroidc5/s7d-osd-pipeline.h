/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_OSD_PIPELINE_H
#define __S7D_OSD_PIPELINE_H

#include <drm/drm_rect.h>

#include "s7d-osd.h"
#include "s7d-osd-scaler.h"
#include "s7d-rdma.h"

#define S7D_OSD_REV_B 0x0b
#define S7D_OSD_SETUP_REG_COUNT 42
#define S7D_OSD_UPDATE_REG_COUNT 21

struct s7d_osd_layer {
	struct s7d_osd_state layout;
	struct drm_rect dst;
	u16 alpha;
	bool premult;
	bool enabled;
	bool afbc;
};

struct s7d_osd_pipeline_state {
	struct s7d_osd_scaler_state scalers[2];
	struct s7d_rdma_entry setup[S7D_OSD_SETUP_REG_COUNT];
	struct s7d_rdma_entry update[S7D_OSD_UPDATE_REG_COUNT];
};

u32 s7d_osd_unpack_mask(u32 reg);

/*
 * Pure calculation for S7D Rev.B, an RGB primary and optional OSD2,
 * without scaling, with separate OSD outputs to postblend. Layouts must come
 * from the checked linear or AFBC builders: this does not establish GEM ownership, fences
 * or DMA bounds. Unknown revisions
 * fail; obtain the revision before entering atomic_check, without assuming
 * that a firmware version provider is already ready.
 *
 * setup configures the OSD MIFs and blend block. Apply it with
 * VENC stopped and all other fetchers/triggers quiesced under exclusive VPU
 * ownership. It does NOT prepare SRAM, arbitration, GFCD/scaler bypass,
 * colour processing, the primary postblend route, ENCP, clocks or HDMI.
 *
 * update is for a frame change at unchanged output dimensions on an
 * already configured pipeline, via the CRTC's synchronized RDMA path.
 * Neither list proves completion or authorizes releasing the old framebuffer.
 * The destination is unchanged on error. No registers are read or written.
 */
int s7d_osd_build_pipeline(u8 revision, u32 width, u32 height,
			   const struct s7d_osd_layer layers[2],
			   struct s7d_osd_pipeline_state *state);

#endif
