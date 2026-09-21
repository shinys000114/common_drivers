/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_OSD_PIPELINE_H
#define __S7D_OSD_PIPELINE_H

#include "s7d-osd.h"
#include "s7d-rdma.h"

#define S7D_OSD_REV_B 0x0b
#define S7D_OSD_SETUP_REG_COUNT 26
#define S7D_OSD_UPDATE_REG_COUNT 4

struct s7d_osd_pipeline_state {
	struct s7d_rdma_entry setup[S7D_OSD_SETUP_REG_COUNT];
	struct s7d_rdma_entry update[S7D_OSD_UPDATE_REG_COUNT];
};

/*
 * Pure calculation for S7D Rev.B, an opaque XRGB8888 primary at (0, 0),
 * without scaling. layout must come from s7d_osd_build_state(): this does
 * not establish GEM ownership, fences or DMA bounds. Unknown revisions
 * fail; obtain the revision before entering atomic_check, without assuming
 * that a firmware version provider is already ready.
 *
 * setup configures only OSD1 MIF and the OSD blend block. Apply it with
 * VENC stopped and all other fetchers/triggers quiesced under exclusive VPU
 * ownership. It does NOT prepare SRAM, arbitration, GFCD/scaler bypass,
 * colour processing, postblend, ENCP, clocks or the HDMI bridge.
 *
 * update is for a frame change at unchanged output dimensions/format on an
 * already configured pipeline, via the CRTC's synchronized RDMA path.
 * Neither list proves completion or authorizes releasing the old framebuffer.
 * The destination is unchanged on error. No registers are read or written.
 */
int s7d_osd_build_pipeline(u8 revision, u32 width, u32 height,
			   const struct s7d_osd_state *layout,
			   struct s7d_osd_pipeline_state *state);

#endif
