/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_CSC_H
#define __S7D_CSC_H

#include <linux/types.h>

#include <drm/drm_color_mgmt.h>

#define S7D_CSC_MATRIX_REG_COUNT 13
#define S7D_CSC_CONTROL_REG_COUNT 5

struct s7d_csc_reg {
	u32 reg;
	u32 mask;
	u32 value;
};

struct s7d_csc_state {
	struct s7d_csc_reg matrix[S7D_CSC_MATRIX_REG_COUNT];
	struct s7d_csc_reg control[S7D_CSC_CONTROL_REG_COUNT];
};

/*
 * Initial matrix/control setup requires VENC and VD1 quiesced. Preserve bits
 * outside each mask. Later matrix updates may use validated RDMA; controls
 * stay outside its replay list. This does not prepare fetch/scaler/blend.
 * Errors leave state unchanged; no registers are read or written.
 */
int s7d_csc_build_state(enum drm_color_encoding encoding,
			enum drm_color_range range, struct s7d_csc_state *state);

/* Matrix masks only; controls and unknown registers return zero. */
u32 s7d_csc_reg_mask(u32 reg);

#endif
