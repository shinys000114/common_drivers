// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>

#include "s7d-csc.h"

#define CSC_COEFF_PAIR_MASK (GENMASK(28, 16) | GENMASK(12, 0))
#define CSC_OFFSET_PAIR_MASK (GENMASK(27, 16) | GENMASK(11, 0))

static u32 csc_pair(int a, int b, u32 mask)
{
	return (((u32)a & mask) << 16) | ((u32)b & mask);
}

u32 s7d_csc_reg_mask(u32 reg)
{
	switch (reg) {
	case 0x3290:
	case 0x3291:
	case 0x3292:
	case 0x3293:
	case 0x3295:
	case 0x3296:
	case 0x3297:
	case 0x3852:
	case 0x3853:
	case 0x3854:
	case 0x3855:
	case 0x3857:
	case 0x3858:
	case 0x3859:
		return CSC_COEFF_PAIR_MASK;
	case 0x3294:
		return GENMASK(18, 16) | GENMASK(12, 0);
	case 0x3298:
		return GENMASK(7, 3);
	case 0x3299:
	case 0x329b:
		return CSC_OFFSET_PAIR_MASK;
	case 0x329a:
	case 0x329c:
		return GENMASK(11, 0);
	case 0x3856:
		return GENMASK(12, 0);
	case 0x386a:
		return GENMASK(19, 3);
	case 0x385a:
	case 0x385c:
		return GENMASK(26, 16) | GENMASK(10, 0);
	case 0x385b:
	case 0x385d:
		return GENMASK(10, 0);
	default:
		return 0;
	}
}

int s7d_csc_build_state(enum drm_color_encoding encoding,
			enum drm_color_range range,
			enum s7d_csc_input_order input_order,
			struct s7d_csc_state *state)
{
	/* Q10 coefficients; rows are R, G, B and columns are Y, Cb, Cr. */
	static const s16 coeffs[2][2][3][3] = {
		{
			{{1192, 0, 1634}, {1192, -401, -832}, {1192, 2066, 0}},
			{{1024, 0, 1436}, {1024, -352, -731}, {1024, 1815, 0}},
		}, {
			{{1192, 0, 1836}, {1192, -218, -546}, {1192, 2163, 0}},
			{{1024, 0, 1613}, {1024, -192, -479}, {1024, 1900, 0}},
		},
	};
	static const u8 columns[][3] = {
		[S7D_CSC_INPUT_YCBCR] = {0, 1, 2},
		[S7D_CSC_INPUT_YCRCB] = {0, 2, 1},
		[S7D_CSC_INPUT_CRCBY] = {2, 1, 0},
	};
	struct s7d_csc_state next;
	const s16 (*coeff)[3];
	const u8 *column;
	unsigned int enc, rng;
	unsigned int i;
	int pre[3] = {0, -512, -512};

	if (!state || (unsigned int)input_order > S7D_CSC_INPUT_CRCBY)
		return -EINVAL;
	switch (encoding) {
	case DRM_COLOR_YCBCR_BT601:
		enc = 0;
		break;
	case DRM_COLOR_YCBCR_BT709:
		enc = 1;
		break;
	default:
		return -EINVAL;
	}
	switch (range) {
	case DRM_COLOR_YCBCR_LIMITED_RANGE:
		rng = 0;
		pre[0] = -64;
		break;
	case DRM_COLOR_YCBCR_FULL_RANGE:
		rng = 1;
		break;
	default:
		return -EINVAL;
	}
	coeff = coeffs[enc][rng];
	column = columns[input_order];
	next = (struct s7d_csc_state) {
		.matrix = {
			{0x3290, 0,
			 csc_pair(coeff[0][column[0]], coeff[0][column[1]], 0x1fff)},
			{0x3291, 0,
			 csc_pair(coeff[0][column[2]], coeff[1][column[0]], 0x1fff)},
			{0x3292, 0,
			 csc_pair(coeff[1][column[1]], coeff[1][column[2]], 0x1fff)},
			{0x3293, 0,
			 csc_pair(coeff[2][column[0]], coeff[2][column[1]], 0x1fff)},
			{0x3294, 0,
			 (u32)coeff[2][column[2]] & 0x1fff},
			{0x3295, 0, 0},
			{0x3296, 0, 0},
			{0x3297, 0, 0},
			{0x3298, 0, 0},
			{0x3299, 0, csc_pair(2, 2, 0xfff)},
			{0x329a, 0, 2},
			{0x329b, 0, csc_pair(pre[column[0]], pre[column[1]], 0xfff)},
			{0x329c, 0, (u32)pre[column[2]] & 0xfff},
		},
		.control = {
			{0x1a0c, BIT(4) | BIT(0), BIT(0)},
			{0x3800, GENMASK(20, 13) | GENMASK(7, 2), 0},
			{0x383b, GENMASK(1, 0), 0},
			{0x383c, GENMASK(1, 0), 0},
			{0x329d, GENMASK(1, 0), BIT(0)},
		},
	};
	for (i = 0; i < S7D_CSC_MATRIX_REG_COUNT; i++)
		next.matrix[i].mask = s7d_csc_reg_mask(next.matrix[i].reg);
	*state = next;
	return 0;
}

int s7d_csc_build_video2_state(enum drm_color_encoding encoding,
			enum drm_color_range range,
			enum s7d_csc_input_order input_order,
			struct s7d_csc_state *state)
{
	static const u32 matrix_regs[S7D_CSC_MATRIX_REG_COUNT] = {
		0x3852, 0x3853, 0x3854, 0x3855, 0x3856, 0x3857, 0x3858,
		0x3859, 0x386a, 0x385a, 0x385b, 0x385c, 0x385d,
	};
	static const struct s7d_csc_reg controls[S7D_CSC_CONTROL_REG_COUNT] = {
		{0x3850, GENMASK(20, 13) | GENMASK(7, 2), 0},
		{0x1a0c, BIT(5) | BIT(1), BIT(1)},
		{0x388c, GENMASK(1, 0), 0},
		{0x388b, GENMASK(1, 0), BIT(0)},
		{0x3850, BIT(16) | BIT(14), BIT(16) | BIT(14)},
	};
	struct s7d_csc_state next;
	unsigned int i;
	int ret;

	if (!state)
		return -EINVAL;
	ret = s7d_csc_build_state(encoding, range, input_order, &next);
	if (ret)
		return ret;
	for (i = 0; i < S7D_CSC_MATRIX_REG_COUNT; i++) {
		next.matrix[i].reg = matrix_regs[i];
		next.matrix[i].mask = s7d_csc_reg_mask(matrix_regs[i]);
		next.matrix[i].value &= next.matrix[i].mask;
	}
	for (i = 0; i < S7D_CSC_CONTROL_REG_COUNT; i++)
		next.control[i] = controls[i];
	*state = next;
	return 0;
}
