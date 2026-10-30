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

#define S7D_ENCP_REG_COUNT 48

struct s7d_enci_reg {
	u16 reg;
	u16 value[2];
};

/* Despite the name this also carries the ENCI timing of 480i and 576i. */
struct s7d_encp_state {
	struct s7d_reg_value regs[S7D_ENCP_REG_COUNT];
	u8 count;
	bool interlace;
	bool enci;
	bool pal;
	u16 flip_start;
	u16 flip_end;
	u8 fifo_hold_lines;
	bool hsync_positive;
	bool vsync_positive;
};

/* Pure calculations: leave the destination unchanged on error. */
enum drm_mode_status
s7d_encp_build_state(const struct drm_display_mode *mode,
		    struct s7d_encp_state *state);

/* ENCI image written before the checked regs[]; ENCI states only. */
unsigned int s7d_enci_image_size(void);
void s7d_enci_image_entry(const struct s7d_encp_state *state, unsigned int i,
			  u32 *reg, u32 *value);

#endif
