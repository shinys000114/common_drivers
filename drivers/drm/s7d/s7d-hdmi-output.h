/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_OUTPUT_H
#define __S7D_HDMI_OUTPUT_H

#include <linux/types.h>

struct drm_color_ctm;

#define S7D_HDMI_MATRIX_WORDS 9
#define S7D_HDMI_POST2_WORDS 13
#define S7D_HDMI_MAX_TMDS_RATE 594000000UL
/* RGB/YCbCr 4:4:4 and 4:2:0 reach 36-bit deep color. */
#define S7D_HDMI_MAX_BPC 12

enum s7d_hdmi_output_encoding {
	S7D_HDMI_OUTPUT_RGB8,
	S7D_HDMI_OUTPUT_601_Y4448,
	S7D_HDMI_OUTPUT_709_Y4448,
	S7D_HDMI_OUTPUT_RGB10,
	S7D_HDMI_OUTPUT_RGB12,
	S7D_HDMI_OUTPUT_601_Y44410,
	S7D_HDMI_OUTPUT_709_Y44410,
	S7D_HDMI_OUTPUT_601_Y44412,
	S7D_HDMI_OUTPUT_709_Y44412,
	S7D_HDMI_OUTPUT_601_Y4228,
	S7D_HDMI_OUTPUT_709_Y4228,
	S7D_HDMI_OUTPUT_601_Y4208,
	S7D_HDMI_OUTPUT_709_Y4208,
	S7D_HDMI_OUTPUT_601_Y42010,
	S7D_HDMI_OUTPUT_709_Y42010,
	S7D_HDMI_OUTPUT_601_Y42012,
	S7D_HDMI_OUTPUT_709_Y42012,
};

struct s7d_hdmi_output_rate {
	unsigned long nominal_pixel;
	unsigned long shared;
	unsigned long serial;
	unsigned long pixel;
	unsigned long reference;
	u64 tmds_numerator;
	u32 tmds_denominator;
	u32 ratio_q;
};

struct s7d_hdmi_post2_state {
	u32 matrix[S7D_HDMI_POST2_WORDS];
	bool enabled;
};

struct s7d_hdmi_output_state {
	enum s7d_hdmi_output_encoding encoding;
	u32 bus_format;
	u32 fmt;
	u32 dith;
	u32 matrix[S7D_HDMI_MATRIX_WORDS];
	struct s7d_hdmi_post2_state post2;
	struct s7d_hdmi_output_rate rate;
	u16 tx_mapping;
	u8 bpc;
	u8 encp_divider;
	u8 pixel_divider;
	u8 fe_divider;
	u8 map_pre;
	u8 map_post;
	u8 tx_mask;
	bool high_tmds;
	bool rgb_limited;
	/* Pixel repetition of a DBLCLK mode: no 4:2:2 and no dither. */
	bool repeat;
	bool bt2020;
};

static inline bool
s7d_hdmi_output_is_y422(const struct s7d_hdmi_output_state *state)
{
	return state->encoding == S7D_HDMI_OUTPUT_601_Y4228 ||
		state->encoding == S7D_HDMI_OUTPUT_709_Y4228;
}

static inline bool
s7d_hdmi_output_is_y420(const struct s7d_hdmi_output_state *state)
{
	return state->encoding >= S7D_HDMI_OUTPUT_601_Y4208 &&
		state->encoding <= S7D_HDMI_OUTPUT_709_Y42012;
}

int s7d_hdmi_output_build(enum s7d_hdmi_output_encoding encoding,
			  unsigned long pixel_rate, unsigned long reference,
			  struct s7d_hdmi_output_state *state);
int s7d_hdmi_output_apply_ctm(struct s7d_hdmi_output_state *state,
			      const struct drm_color_ctm *ctm);
int s7d_hdmi_output_apply_rgb_range(struct s7d_hdmi_output_state *state, bool limited);
int s7d_hdmi_output_apply_bt2020(struct s7d_hdmi_output_state *state, bool bt2020);
int s7d_hdmi_output_apply_repeat(struct s7d_hdmi_output_state *state, bool repeat);
int s7d_hdmi_output_rebuild(const struct s7d_hdmi_output_state *state,
			    unsigned long pixel_rate, unsigned long reference,
			    const struct drm_color_ctm *ctm,
			    struct s7d_hdmi_output_state *rebuilt);
int s7d_hdmi_output_check(const struct s7d_hdmi_output_state *state,
			  unsigned long pixel_rate, unsigned long reference,
			  const struct drm_color_ctm *ctm);
bool s7d_hdmi_output_same(const struct s7d_hdmi_output_state *a,
			  const struct s7d_hdmi_output_state *b);
bool s7d_hdmi_output_tmds_above(const struct s7d_hdmi_output_state *state,
				unsigned long rate);

#endif
