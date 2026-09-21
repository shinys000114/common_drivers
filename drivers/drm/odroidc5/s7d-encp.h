/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_ENCP_H
#define __S7D_ENCP_H

#include <linux/types.h>
#include <drm/drm_modes.h>

/* VCBUS word indices, not byte offsets or physical addresses. */
struct s7d_reg_value {
	u32 reg;
	u32 value;
};

#define S7D_ENCP_REG_COUNT 24

struct s7d_encp_state {
	struct s7d_reg_value regs[S7D_ENCP_REG_COUNT];
	bool hsync_positive;
	bool vsync_positive;
};

/* Pure calculations: leave the destination unchanged on error. */
enum drm_mode_status
s7d_encp_build_state(const struct drm_display_mode *mode,
		    struct s7d_encp_state *state);

#endif
