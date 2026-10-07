/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_OSD_SCALER_H
#define __S7D_OSD_SCALER_H

#include <linux/types.h>

#define S7D_OSD_SCALER_MAX_UPSCALE 4
#define S7D_OSD_SCALER_LINEBUFFER 1920

struct s7d_osd_scaler_state {
	u32 input_size;
	u32 output_h;
	u32 output_v;
	u32 h_phase_step;
	u32 v_phase_step;
	u32 h_control;
	u32 v_control;
	bool enabled;
};

int s7d_osd_scaler_build(unsigned int index, u32 input_w, u32 input_h,
			 u32 output_w, u32 output_h,
			 struct s7d_osd_scaler_state *state);
bool s7d_osd_scaler_same(const struct s7d_osd_scaler_state *a,
			const struct s7d_osd_scaler_state *b);
/* Caller owns the VPU and has stopped all fetchers, VENCs and RDMA. */
int s7d_osd_scaler_setup(void __iomem *vcbus,
			 const struct s7d_osd_scaler_state states[2], u32 *failed_reg);

#endif
