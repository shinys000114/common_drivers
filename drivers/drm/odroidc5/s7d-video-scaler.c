// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* Coefficient tables Copyright (c) 2025 Amlogic, Inc. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/math64.h>

#include "s7d-video-scaler.h"

#define SET(r, m, v) { .reg = (r), .mask = (m), .value = (v) }

/* S7D video_safa.c: signed coefficient pairs, nine-bit normalization. */
static const s16 tap4[33][4] = {
	{0, 512, 0, 0},
	{-5, 512, 5, 0},
	{-10, 511, 11, 0},
	{-14, 510, 17, -1},
	{-18, 508, 23, -1},
	{-22, 506, 29, -1},
	{-25, 503, 36, -2},
	{-28, 500, 43, -3},
	{-32, 496, 51, -3},
	{-34, 491, 59, -4},
	{-37, 487, 67, -5},
	{-39, 482, 75, -6},
	{-41, 476, 84, -7},
	{-42, 470, 92, -8},
	{-44, 463, 102, -9},
	{-45, 456, 111, -10},
	{-45, 449, 120, -12},
	{-47, 442, 130, -13},
	{-47, 434, 140, -15},
	{-47, 425, 151, -17},
	{-47, 416, 161, -18},
	{-47, 407, 172, -20},
	{-47, 398, 182, -21},
	{-47, 389, 193, -23},
	{-46, 379, 204, -25},
	{-45, 369, 215, -27},
	{-44, 358, 226, -28},
	{-43, 348, 237, -30},
	{-43, 337, 249, -31},
	{-41, 326, 260, -33},
	{-40, 316, 271, -35},
	{-39, 305, 282, -36},
	{-37, 293, 293, -37},
};

static const s16 tap6[33][6] = {
	{0, 0, 512, 0, 0, 0},
	{2, -6, 512, 7, -2, -1},
	{3, -13, 511, 14, -3, 0},
	{5, -19, 510, 21, -5, 0},
	{6, -24, 508, 29, -7, 0},
	{7, -29, 507, 37, -9, -1},
	{8, -34, 504, 45, -11, 0},
	{9, -39, 501, 53, -13, 1},
	{10, -44, 498, 62, -16, 2},
	{11, -48, 494, 71, -18, 2},
	{12, -51, 490, 80, -20, 1},
	{13, -55, 486, 89, -23, 2},
	{14, -58, 481, 99, -25, 1},
	{14, -61, 475, 108, -27, 3},
	{15, -64, 470, 118, -30, 3},
	{15, -66, 464, 128, -32, 3},
	{15, -68, 457, 139, -35, 4},
	{16, -70, 450, 149, -37, 4},
	{16, -72, 443, 160, -40, 5},
	{16, -73, 436, 170, -42, 5},
	{16, -74, 428, 181, -45, 6},
	{16, -75, 419, 192, -47, 7},
	{16, -75, 411, 203, -50, 7},
	{16, -76, 402, 214, -52, 8},
	{16, -76, 393, 225, -54, 8},
	{15, -76, 384, 236, -56, 9},
	{15, -75, 374, 247, -59, 10},
	{15, -75, 365, 258, -61, 10},
	{14, -74, 355, 269, -63, 11},
	{14, -73, 344, 281, -65, 11},
	{13, -72, 334, 291, -66, 12},
	{13, -71, 324, 302, -68, 12},
	{13, -70, 313, 313, -70, 13},
};

static const s16 tap8[33][8] = {
	{0, 0, 0, 512, 0, 0, 0, 0},
	{-1, 3, -7, 512, 7, -3, 1, 0},
	{-2, 5, -14, 511, 15, -5, 2, 0},
	{-2, 7, -20, 510, 23, -8, 3, -1},
	{-3, 10, -27, 509, 31, -11, 3, 0},
	{-4, 12, -32, 507, 39, -14, 4, 0},
	{-4, 14, -38, 504, 48, -17, 5, 0},
	{-5, 16, -43, 501, 57, -19, 6, -1},
	{-5, 18, -49, 498, 66, -22, 7, -1},
	{-6, 19, -53, 495, 75, -26, 8, 0},
	{-6, 21, -58, 490, 85, -29, 10, -1},
	{-6, 22, -62, 486, 94, -32, 11, -1},
	{-7, 24, -66, 481, 104, -35, 12, -1},
	{-7, 25, -69, 476, 114, -38, 13, -2},
	{-7, 26, -72, 470, 124, -41, 14, -2},
	{-8, 27, -75, 464, 134, -44, 15, -1},
	{-8, 28, -78, 458, 145, -47, 16, -2},
	{-8, 29, -80, 451, 155, -50, 17, -2},
	{-8, 30, -83, 444, 166, -53, 18, -2},
	{-8, 31, -84, 437, 177, -56, 19, -4},
	{-8, 31, -86, 429, 188, -59, 20, -3},
	{-9, 32, -87, 421, 199, -62, 22, -4},
	{-8, 32, -88, 413, 209, -64, 23, -5},
	{-8, 32, -89, 405, 220, -67, 24, -5},
	{-9, 33, -89, 396, 231, -69, 25, -6},
	{-8, 33, -90, 387, 242, -72, 25, -5},
	{-8, 33, -90, 377, 253, -74, 26, -5},
	{-8, 32, -89, 368, 264, -76, 27, -6},
	{-8, 32, -89, 358, 275, -78, 28, -6},
	{-8, 32, -88, 348, 286, -80, 29, -7},
	{-7, 32, -88, 338, 297, -82, 29, -7},
	{-7, 31, -86, 328, 307, -84, 30, -7},
	{-8, 31, -85, 318, 318, -85, 31, -8},
};

static u32 initial_phase(u32 src, u32 dst)
{
	if (src == dst)
		return 0;
	return (0x1f << 16) | ((div_u64((u64)src << 16, dst) + 65536) / 2);
}

int s7d_video_scaler_build(u32 src_width, u32 src_height,
			 u32 dst_width, u32 dst_height,
			 struct s7d_video_scaler_state *out)
{
	struct s7d_video_scaler_state next = {
		.src_width = src_width,
		.src_height = src_height,
		.dst_width = dst_width,
		.dst_height = dst_height,
	};

	if (!out || !src_width || !src_height || !dst_width || !dst_height ||
	    src_width > 4096 || src_height > 2160 ||
	    ((src_width | src_height | dst_width | dst_height) & 1))
		return -EINVAL;
	if (src_width == dst_width && src_height == dst_height) {
		*out = next;
		return 0;
	}
	if (src_width < 16 || src_height < 16 || dst_width > 1920 || dst_height > 1080 ||
	    src_width > dst_width || src_height > dst_height ||
	    dst_width > src_width * 4 || dst_height > src_height * 4)
		return -EINVAL;
	next.setup[0] = (struct s7d_video_scaler_reg)SET(0x5001, U32_MAX, 0);
	next.setup[1] = (struct s7d_video_scaler_reg)SET(0x5008, 0x77, BIT(6) | BIT(2));
	next.setup[2] = (struct s7d_video_scaler_reg)SET(0x500a, 0xff, 0);
	next.setup[3] = (struct s7d_video_scaler_reg)SET(0x5048, BIT(4), 0);
	next.setup[4] = (struct s7d_video_scaler_reg)SET(0x5041, U32_MAX, 0x55555555);
	next.setup[5] = (struct s7d_video_scaler_reg)SET(0x5100, BIT(4) | BIT(0), BIT(0));
	next.setup[6] = (struct s7d_video_scaler_reg)SET(0x5103,
		BIT(12) | BIT(8) | BIT(4) | GENMASK(3, 0), BIT(12));
	next.setup[7] = (struct s7d_video_scaler_reg)SET(0x5101, GENMASK(27, 0),
		div_u64((u64)src_height << 24, dst_height));
	next.setup[8] = (struct s7d_video_scaler_reg)SET(0x5102, GENMASK(27, 0),
		div_u64((u64)src_width << 24, dst_width));
	next.setup[9] = (struct s7d_video_scaler_reg)SET(0x510b, GENMASK(20, 0),
		initial_phase(src_height, dst_height));
	next.setup[10] = (struct s7d_video_scaler_reg)SET(0x510c, GENMASK(20, 0),
		initial_phase(src_width, dst_width));
	next.setup[11] = (struct s7d_video_scaler_reg)SET(0x510d,
		GENMASK(26, 24) | BIT(20) | BIT(16) | GENMASK(13, 12) |
		GENMASK(9, 8) | BIT(4) | BIT(0), (3 << 12) | (3 << 8));
	next.setup[12] = (struct s7d_video_scaler_reg)SET(0x510e, BIT(8) | BIT(4) | BIT(0), 0);
	next.setup[13] = (struct s7d_video_scaler_reg)SET(0x510f,
		GENMASK(17, 12) | GENMASK(11, 8) | GENMASK(5, 0), 0);
	next.setup[14] = (struct s7d_video_scaler_reg)SET(0x5110, GENMASK(3, 0), 9);
	next.setup[15] = (struct s7d_video_scaler_reg)SET(0x5116, BIT(4), 0);
	next.setup[16] = (struct s7d_video_scaler_reg)SET(0x511e, BIT(24), 0);
	next.setup[17] = (struct s7d_video_scaler_reg)SET(0x5190,
		GENMASK(28, 19) | GENMASK(10, 0),
		BIT(20) | (2 << 9) | BIT(8) | BIT(2) | BIT(0));
	next.setup[18] = (struct s7d_video_scaler_reg)SET(0x5191, GENMASK(27, 0),
		BIT(10) | BIT(8));
	next.setup[19] = (struct s7d_video_scaler_reg)SET(0x1da6, 0x1fff1fff,
		(src_width << 16) | src_height);
	next.control = (struct s7d_video_scaler_reg)SET(0x5000,
		GENMASK(31, 16) | GENMASK(2, 0), 7);
	next.enabled = true;
	*out = next;
	return 0;
}

int s7d_video_scaler_check_mode(const struct s7d_video_scaler_state *state,
			      u32 width, u32 height, unsigned long pixel_rate,
			      unsigned long core_rate)
{
	if (!state)
		return -EINVAL;
	if (!state->enabled)
		return 0;
	if (!width || !height || width > 1920 || height > 1080 ||
	    state->dst_width > width || state->dst_height > height ||
	    !pixel_rate || pixel_rate > 148500000 || core_rate / 2 < pixel_rate)
		return -ERANGE;
	return 0;
}

bool s7d_video_scaler_same_config(const struct s7d_video_scaler_state *a,
				const struct s7d_video_scaler_state *b)
{
	if (!a || !b || a->enabled != b->enabled)
		return false;
	return !a->enabled || (a->src_width == b->src_width &&
		a->src_height == b->src_height && a->dst_width == b->dst_width &&
		a->dst_height == b->dst_height);
}

int s7d_video_scaler_coefficient(u32 bank, u32 phase, u32 pair, u32 *out)
{
	const s16 *coeff;

	if (!out || bank >= S7D_VIDEO_SCALER_COEF_BANKS ||
	    phase >= S7D_VIDEO_SCALER_COEF_PHASES || pair >= S7D_VIDEO_SCALER_COEF_PAIRS)
		return -EINVAL;
	switch (bank) {
	case 1:
		coeff = &tap8[phase][pair * 2];
		break;
	case 2:
		coeff = &tap8[phase][4 + pair * 2];
		break;
	case 4:
		coeff = &tap6[phase][pair * 2];
		break;
	case 5:
		/* The selected S7D loader repeats the last vertical pair. */
		coeff = &tap6[phase][4];
		break;
	default:
		coeff = &tap4[phase][pair * 2];
		break;
	}
	*out = ((u32)(u16)coeff[0] << 16) | (u16)coeff[1];
	return 0;
}
