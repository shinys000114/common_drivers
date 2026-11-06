/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_OSD_SCALER_H
#define __S7D_OSD_SCALER_H

#include <linux/types.h>

#include "s7d-filter.h"
#include "s7d-rdma.h"
#include "s7d-vpp.h"

#define S7D_OSD_SCALER_MAX_UPSCALE 4
#define S7D_OSD_SCALER_MAX_DOWNSCALE 2
#define S7D_OSD_SCALER_LINEBUFFER 1920
/* Selected S7D normal-pipeline input limit, separate from the bank threshold. */
#define S7D_OSD_SCALER_MAX_INPUT_WIDTH 3840

struct s7d_osd_scaler_state {
	u32 input_size;
	u32 output_h;
	u32 output_v;
	u32 h_phase_step;
	u32 v_phase_step;
	u32 h_control;
	u32 v_control;
	u32 v_init_phase;
	bool enabled;
};

/* With interlace, output_h is the field height and the scaler always runs. */
int s7d_osd_scaler_build(unsigned int index, u32 input_w, u32 input_h,
			 u32 output_w, u32 output_h, bool interlace,
			 struct s7d_osd_scaler_state *state);
bool s7d_osd_scaler_same(const struct s7d_osd_scaler_state *a,
			const struct s7d_osd_scaler_state *b);
int s7d_osd_scaler_check_mode(const struct s7d_osd_scaler_state states[2],
			      u32 htotal, unsigned long pixel_rate,
			      unsigned long core_rate);
/* The scaler route that s7d_osd_scaler_setup() selects first. */
extern const struct s7d_vpp_setting s7d_osd_scaler_route;

/* Caller owns the VPU and has stopped all fetchers, VENCs and RDMA. */
int s7d_osd_scaler_setup(void __iomem *vcbus,
			 const struct s7d_osd_scaler_state states[2], u32 *failed_reg);

/* Off, ten settings, both filter tables with their index writes, then on. */
#define S7D_OSD_SCALER_LIVE_COUNT (3 + 10 + 2 * (S7D_FILTER_PHASES + 1) + 1 + 3)

/*
 * Add the masked writes, in the stopped preparation order, that move scaler
 * index of a running progressive output from old to state. The filter
 * tables are reloaded only when old did not use the same ones. The setup
 * has owned the scaler route since the output was prepared.
 */
int s7d_osd_scaler_build_live(unsigned int index, const struct s7d_osd_scaler_state *old,
			      const struct s7d_osd_scaler_state *state,
			      struct s7d_rdma_live *live);

#endif
