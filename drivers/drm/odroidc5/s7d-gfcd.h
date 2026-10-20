/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_GFCD_H
#define __S7D_GFCD_H

#include <linux/bits.h>

#include "s7d-afbc.h"

struct regmap;
struct s7d_osd_scaler_state;

#define S7D_GFCD_HEAD 0x5904
#define S7D_GFCD_TOP 0x5955
#define S7D_GFCD_ERROR 0x595c
#define S7D_GFCD_ROUTE (BIT(14) | BIT(10) | BIT(4))

struct s7d_gfcd_state {
	struct s7d_osd_state osd;
	u32 head;
	u32 frame_end;
	u32 frame_size;
};

int s7d_gfcd_build_state(const struct drm_afbc_framebuffer *fb,
			 const struct drm_rect *source, u64 dma_mask,
			 bool premult, struct s7d_gfcd_state *state);
int s7d_gfcd_check_state(const struct s7d_gfcd_state *state);
int s7d_gfcd_check_scaler(const struct s7d_gfcd_state *state,
			  const struct s7d_osd_scaler_state *scaler);
bool s7d_gfcd_same_config(const struct s7d_gfcd_state *a,
			  const struct s7d_gfcd_state *b);
int s7d_gfcd_admit(struct regmap *map);
int s7d_gfcd_prepare(struct regmap *map, const struct s7d_gfcd_state *state);
int s7d_gfcd_close(struct regmap *map);

#endif
