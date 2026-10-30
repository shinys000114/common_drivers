// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/amlogic/clk/s7d-hdmi-pll-rate.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/media-bus-format.h>
#include <linux/math64.h>
#include <linux/string.h>

#include <uapi/drm/drm_mode.h>

#include "s7d-hdmi-output.h"

/*
 * The gamma LUT follows POST2 and carries limited range in its outputs, with
 * or without a user LUT, so CTM, gamma and range combine at any RGB depth.
 */
int s7d_hdmi_output_apply_rgb_range(struct s7d_hdmi_output_state *state, bool limited)
{
	if (!limited)
		return 0;
	if (state->encoding != S7D_HDMI_OUTPUT_RGB8 &&
	    state->encoding != S7D_HDMI_OUTPUT_RGB10 &&
	    state->encoding != S7D_HDMI_OUTPUT_RGB12)
		return -EINVAL;
	state->rgb_limited = true;
	return 0;
}

/*
 * BT.2020 non-constant-luminance RGB to limited YCbCr, Q10, in the formatter
 * layout of the BT.709 table below: rows Cr/Y/Cb, then post offsets.
 */
int s7d_hdmi_output_apply_bt2020(struct s7d_hdmi_output_state *state, bool bt2020)
{
	static const u32 matrix2020[7] = {
		0x01c21e62, 0x1fdc00e7, 0x02540034, 0x1f821ebc, 0x000001c2,
		0x02000040, 0x00000200,
	};

	if (!bt2020)
		return 0;
	switch (state->encoding) {
	case S7D_HDMI_OUTPUT_RGB8:
	case S7D_HDMI_OUTPUT_RGB10:
	case S7D_HDMI_OUTPUT_RGB12:
		break;
	case S7D_HDMI_OUTPUT_709_Y4448:
	case S7D_HDMI_OUTPUT_709_Y44410:
	case S7D_HDMI_OUTPUT_709_Y44412:
	case S7D_HDMI_OUTPUT_709_Y4228:
	case S7D_HDMI_OUTPUT_709_Y4208:
	case S7D_HDMI_OUTPUT_709_Y42010:
	case S7D_HDMI_OUTPUT_709_Y42012:
		memcpy(state->matrix + 2, matrix2020, sizeof(matrix2020));
		break;
	default:
		return -EINVAL;
	}
	state->bt2020 = true;
	return 0;
}

/* Reproduce a checked plan from its encoding and recorded adjustments. */
int s7d_hdmi_output_rebuild(const struct s7d_hdmi_output_state *state,
			    unsigned long pixel_rate, unsigned long reference,
			    const struct drm_color_ctm *ctm,
			    struct s7d_hdmi_output_state *rebuilt)
{
	int ret;

	if (!state || !rebuilt)
		return -EINVAL;
	ret = s7d_hdmi_output_build(state->encoding, pixel_rate, reference, rebuilt);
	if (!ret)
		ret = s7d_hdmi_output_apply_bt2020(rebuilt, state->bt2020);
	if (!ret)
		ret = s7d_hdmi_output_apply_rgb_range(rebuilt, state->rgb_limited);
	if (!ret)
		ret = s7d_hdmi_output_apply_repeat(rebuilt, state->repeat);
	if (!ret)
		ret = s7d_hdmi_output_apply_ctm(rebuilt, ctm);
	return ret;
}

/* Like the vendor: repeated pixels carry no 4:2:2 and are not dithered. */
int s7d_hdmi_output_apply_repeat(struct s7d_hdmi_output_state *state, bool repeat)
{
	if (!repeat)
		return 0;
	if (state->encoding == S7D_HDMI_OUTPUT_601_Y4228 ||
	    state->encoding == S7D_HDMI_OUTPUT_709_Y4228)
		return -EINVAL;
	state->repeat = true;
	state->dith &= ~BIT(4);
	return 0;
}

/* Round one exact (Q10 * S31.32) dot product to signed 3.10. */
static int s7d_hdmi_ctm_coefficient(const s16 base[3], const struct drm_color_ctm *ctm,
				    unsigned int column, s16 *coefficient)
{
	s64 whole = 0, fraction = 0, quotient;
	unsigned int k;

	/*
	 * Split the user value before multiplying, preserving cancellation over
	 * the full S31.32 range. Base row L1 is at most 1024, so both signed
	 * accumulators stay below 2^42, even for the largest user magnitudes.
	 */
	for (k = 0; k < 3; k++) {
		u64 value = ctm->matrix[3 * k + column];
		u64 magnitude = value & ~BIT_ULL(63);
		s64 weight = value & BIT_ULL(63) ? -base[k] : base[k];

		whole += weight * (s64)(magnitude >> 32);
		fraction += weight * (s64)(magnitude & U32_MAX);
	}
	quotient = div64_s64(fraction, (s64)BIT_ULL(32));
	whole += quotient;
	fraction -= quotient * (s64)BIT_ULL(32);
	/* Nearest, with exact half-way values rounded away from zero. */
	if (fraction > (s64)BIT_ULL(31) || (fraction == (s64)BIT_ULL(31) && whole >= 0))
		whole++;
	else if (fraction < -(s64)BIT_ULL(31) ||
		 (fraction == -(s64)BIT_ULL(31) && whole <= 0))
		whole--;
	if (whole < -4096 || whole > 4095)
		return -ERANGE;
	*coefficient = whole;
	return 0;
}

/* Apply once to a freshly built canonical plan; never to a live register image. */
int s7d_hdmi_output_apply_ctm(struct s7d_hdmi_output_state *state,
			      const struct drm_color_ctm *ctm)
{
	struct s7d_hdmi_post2_state next;
	s16 base[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
	s16 coefficient[9];
	unsigned int i;
	int ret;

	if (!state)
		return -EINVAL;
	if (!ctm)
		return 0;
	next = state->post2;
	if (next.enabled) {
		/* Preserve the output transform and its post offsets when fusing CTM. */
		for (i = 0; i < 9; i++) {
			u32 value = i < 8 ? next.matrix[i / 2] >> (i & 1 ? 0 : 16) :
				next.matrix[4];

			base[i] = value & 0x1fff;
			if (base[i] & BIT(12))
				base[i] -= BIT(13);
		}
	}
	/* POST2 is RGB-domain; YCbCr conversion follows gamma in the HDMI formatter. */
	for (i = 0; i < 9; i++) {
		ret = s7d_hdmi_ctm_coefficient(base + 3 * (i / 3), ctm, i % 3,
					       &coefficient[i]);
		if (ret)
			return ret;
	}
	/* NULL/identity, including negative zero, retains the exact baseline. */
	if (!memcmp(coefficient, base, sizeof(base)))
		return 0;
	for (i = 0; i < 4; i++)
		next.matrix[i] = (((u32)coefficient[2 * i] & 0x1fff) << 16) |
			((u32)coefficient[2 * i + 1] & 0x1fff);
	next.matrix[4] = (u32)coefficient[8] & 0x1fff;
	next.enabled = true;
	state->post2 = next;
	return 0;
}

int s7d_hdmi_output_build(enum s7d_hdmi_output_encoding encoding,
			  unsigned long pixel_rate, unsigned long reference,
			  struct s7d_hdmi_output_state *state)
{
	/* Actual S7D RGB2YCC601 helper: pre0, Q10 rows Cr/Y/Cb, post512/64/512. */
	static const u32 matrix601[S7D_HDMI_MATRIX_WORDS] = {
		0, 0,
		0x01c21e87, 0x1fb70107, 0x02040064, 0x1f681ed6, 0x000001c2,
		0x02000040, 0x00000200,
	};
	/* S7D OSD SDR matrix709, with rows reordered from Y/Cb/Cr to Cr/Y/Cb. */
	static const u32 matrix709[S7D_HDMI_MATRIX_WORDS] = {
		0, 0,
		0x01c21e67, 0x1fd700bb, 0x0275003f, 0x1f991ea6, 0x000001c2,
		0x02000040, 0x00000200,
	};
	struct s7d_hdmi_output_state next;
	struct s7d_hdmi_link_rate rate;
	const u32 *matrix = NULL;
	bool deep = false;
	u32 ratio_q = 40;
	int ret;

	if (!state || pixel_rate < 25175000 || pixel_rate > 594000000)
		return -EINVAL;
	switch (encoding) {
	case S7D_HDMI_OUTPUT_RGB10:
	case S7D_HDMI_OUTPUT_601_Y44410:
	case S7D_HDMI_OUTPUT_709_Y44410:
		ratio_q = 50;
		break;
	case S7D_HDMI_OUTPUT_RGB12:
	case S7D_HDMI_OUTPUT_601_Y44412:
	case S7D_HDMI_OUTPUT_709_Y44412:
		ratio_q = 60;
		break;
	/* 4:2:0 carries two pixels per TMDS character period. */
	case S7D_HDMI_OUTPUT_601_Y4208:
	case S7D_HDMI_OUTPUT_709_Y4208:
		ratio_q = 20;
		break;
	case S7D_HDMI_OUTPUT_601_Y42010:
	case S7D_HDMI_OUTPUT_709_Y42010:
		ratio_q = 25;
		break;
	case S7D_HDMI_OUTPUT_601_Y42012:
	case S7D_HDMI_OUTPUT_709_Y42012:
		ratio_q = 30;
		break;
	default:
		break;
	}
	ret = s7d_hdmi_link_calculate(pixel_rate, ratio_q, reference, &rate);
	if (ret)
		return ret;
	next = (struct s7d_hdmi_output_state) {
		.encoding = encoding,
		.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
		.fmt = (2 << 22) | BIT(10) | (2 << 2),
		/* As the vendor at 24 bits: round and dither the last 10->8 step. */
		.dith = BIT(10) | BIT(4),
		.rate = {
			.nominal_pixel = pixel_rate,
			.shared = rate.shared.pll.rate,
			.serial = rate.serial.rate,
			.pixel = rate.pixel.rate,
			.reference = reference,
			.tmds_numerator = rate.tmds_numerator,
			.tmds_denominator = rate.tmds_denominator,
			.ratio_q = rate.ratio_q,
		},
		.bpc = 8,
		.encp_divider = 1,
		.pixel_divider = 1,
		.fe_divider = 1,
		.map_pre = 1,
		.tx_mapping = 0x0440,
	};
	switch (encoding) {
	case S7D_HDMI_OUTPUT_RGB8:
		break;
	case S7D_HDMI_OUTPUT_RGB10:
	case S7D_HDMI_OUTPUT_RGB12:
		next.bus_format = encoding == S7D_HDMI_OUTPUT_RGB12 ?
			MEDIA_BUS_FMT_RGB121212_1X36 : MEDIA_BUS_FMT_RGB101010_1X30;
		next.fmt &= ~BIT(10);
		next.dith = 0;
		next.bpc = encoding == S7D_HDMI_OUTPUT_RGB12 ? 12 : 10;
		if (s7d_hdmi_output_tmds_above(&next, S7D_HDMI_MAX_TMDS_RATE))
			return -ERANGE;
		break;
	case S7D_HDMI_OUTPUT_601_Y4448:
		matrix = matrix601;
		break;
	case S7D_HDMI_OUTPUT_709_Y4448:
		matrix = matrix709;
		break;
	case S7D_HDMI_OUTPUT_601_Y44410:
	case S7D_HDMI_OUTPUT_601_Y44412:
	case S7D_HDMI_OUTPUT_601_Y4228:
	case S7D_HDMI_OUTPUT_601_Y4208:
	case S7D_HDMI_OUTPUT_601_Y42010:
	case S7D_HDMI_OUTPUT_601_Y42012:
		matrix = matrix601;
		deep = true;
		break;
	case S7D_HDMI_OUTPUT_709_Y44410:
	case S7D_HDMI_OUTPUT_709_Y44412:
	case S7D_HDMI_OUTPUT_709_Y4228:
	case S7D_HDMI_OUTPUT_709_Y4208:
	case S7D_HDMI_OUTPUT_709_Y42010:
	case S7D_HDMI_OUTPUT_709_Y42012:
		matrix = matrix709;
		deep = true;
		break;
	default:
		return -EINVAL;
	}
	if (deep) {
		if (s7d_hdmi_output_is_y422(&next)) {
			/*
			 * The 4:2:2 link uses the fixed 12-bit container, but the
			 * TX mask keeps eight bits; round to them in the formatter.
			 */
			next.bus_format = MEDIA_BUS_FMT_UYVY8_1X16;
			next.fmt |= 1;
			next.dith = BIT(10);
			next.tx_mapping = 0x0500;
			next.tx_mask = 0x2a;
		} else if (s7d_hdmi_output_is_y420(&next)) {
			/*
			 * The formatter pairs ENCP pixels into Y1Y0C words; the TX
			 * pixel clock and post-FIFO writes run at half the ENCP rate.
			 */
			next.fmt |= 2;
			next.map_post = 4;
			next.pixel_divider = 2;
			if (ratio_q == 20) {
				next.bus_format = MEDIA_BUS_FMT_UYYVYY8_0_5X24;
			} else {
				next.bus_format = ratio_q == 30 ?
					MEDIA_BUS_FMT_UYYVYY12_0_5X36 :
					MEDIA_BUS_FMT_UYYVYY10_0_5X30;
				next.fmt &= ~BIT(10);
				next.dith = 0;
				next.bpc = ratio_q == 30 ? 12 : 10;
			}
		} else {
			next.bus_format = ratio_q == 60 ? MEDIA_BUS_FMT_YUV12_1X36 :
				MEDIA_BUS_FMT_YUV10_1X30;
			next.fmt &= ~BIT(10);
			next.dith = 0;
			next.bpc = ratio_q == 60 ? 12 : 10;
		}
		if (s7d_hdmi_output_tmds_above(&next, S7D_HDMI_MAX_TMDS_RATE))
			return -ERANGE;
	} else if (matrix) {
		next.bus_format = MEDIA_BUS_FMT_YUV8_1X24;
	}
	/*
	 * RGB reaches the gamma LUT; the formatter matrix converts to YCbCr in
	 * Cr/Y/Cb order (map_pre 1) before the 4:2:2/4:2:0 downsampler.
	 */
	if (matrix) {
		next.fmt |= BIT(31);
		memcpy(next.matrix, matrix, sizeof(next.matrix));
	}
	next.high_tmds = s7d_hdmi_output_tmds_above(&next, 340000000);
	*state = next;
	return 0;
}

bool s7d_hdmi_output_tmds_above(const struct s7d_hdmi_output_state *state,
				unsigned long rate)
{
	return !state->rate.tmds_denominator ||
		state->rate.tmds_numerator > (u64)rate * state->rate.tmds_denominator;
}

bool s7d_hdmi_output_same(const struct s7d_hdmi_output_state *a,
			  const struct s7d_hdmi_output_state *b)
{
	return a->encoding == b->encoding && a->bus_format == b->bus_format &&
		a->fmt == b->fmt && a->dith == b->dith &&
		!memcmp(a->matrix, b->matrix, sizeof(a->matrix)) &&
		a->post2.enabled == b->post2.enabled &&
		!memcmp(a->post2.matrix, b->post2.matrix, sizeof(a->post2.matrix)) &&
		a->rate.nominal_pixel == b->rate.nominal_pixel &&
		a->rate.shared == b->rate.shared && a->rate.serial == b->rate.serial &&
		a->rate.pixel == b->rate.pixel && a->rate.reference == b->rate.reference &&
		a->rate.tmds_numerator == b->rate.tmds_numerator &&
		a->rate.tmds_denominator == b->rate.tmds_denominator &&
		a->rate.ratio_q == b->rate.ratio_q && a->bpc == b->bpc &&
		a->encp_divider == b->encp_divider &&
		a->pixel_divider == b->pixel_divider && a->fe_divider == b->fe_divider &&
		a->map_pre == b->map_pre && a->map_post == b->map_post &&
		a->tx_mapping == b->tx_mapping && a->tx_mask == b->tx_mask &&
		a->high_tmds == b->high_tmds && a->rgb_limited == b->rgb_limited &&
		a->bt2020 == b->bt2020;
}

int s7d_hdmi_output_check(const struct s7d_hdmi_output_state *state,
			  unsigned long pixel_rate, unsigned long reference,
			  const struct drm_color_ctm *ctm)
{
	struct s7d_hdmi_output_state checked;
	int ret;

	ret = s7d_hdmi_output_rebuild(state, pixel_rate, reference, ctm, &checked);
	if (ret)
		return ret;
	return s7d_hdmi_output_same(state, &checked) ? 0 : -EINVAL;
}
