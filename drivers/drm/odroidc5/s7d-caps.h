/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_CAPS_H
#define __S7D_CAPS_H

#include <linux/bits.h>
#include <linux/types.h>
#include <drm/drm_modes.h>

#define S7D_DISPLAY_MAX_WIDTH 4096
#define S7D_DISPLAY_MAX_HEIGHT 2160
#define S7D_DISPLAY_MAX_TOTAL 8192
/* Pixel clocks use drm_display_mode's kHz units. */
#define S7D_DISPLAY_MIN_CLOCK 25175
#define S7D_DISPLAY_MAX_CLOCK 594000

enum s7d_display_input {
	S7D_INPUT_OSD1,
	S7D_INPUT_OSD2,
	S7D_INPUT_VD1,
	S7D_INPUT_VD2,
	S7D_INPUT_COUNT,
};

#define S7D_OSD_INPUTS (BIT(S7D_INPUT_OSD1) | BIT(S7D_INPUT_OSD2))
#define S7D_VIDEO_INPUTS (BIT(S7D_INPUT_VD1) | BIT(S7D_INPUT_VD2))

enum s7d_display_fetch {
	S7D_FETCH_LINEAR,
	S7D_FETCH_AFBC,
	S7D_FETCH_GFCD,
};

/* Physical inputs claimed by one atomic frame, not logical DRM planes. */
struct s7d_display_resources {
	u8 inputs;
	u8 afbc;
	u8 gfcd;
	u8 scalers;
};

enum drm_mode_status s7d_display_mode_valid(const struct drm_display_mode *mode);
int s7d_display_check_resources(const struct s7d_display_resources *resources);
int s7d_display_claim(struct s7d_display_resources *resources,
		      enum s7d_display_input input, enum s7d_display_fetch fetch);

#endif
