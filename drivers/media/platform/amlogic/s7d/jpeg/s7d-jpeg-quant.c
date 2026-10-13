// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/minmax.h>

#include "s7d-jpeg-quant.h"

static const u8 jpeg_quant[S7D_JPEG_QUANT_TABLES][S7D_JPEG_QUANT_COEFFICIENTS] = {
	{
		6, 6, 8, 10, 10, 16, 21, 25,
		6, 10, 10, 14, 18, 31, 41, 41,
		8, 10, 14, 18, 33, 41, 41, 41,
		10, 14, 18, 20, 35, 41, 41, 41,
		10, 18, 33, 35, 39, 41, 41, 41,
		16, 31, 41, 41, 41, 41, 41, 41,
		21, 41, 41, 41, 41, 41, 41, 41,
		25, 41, 41, 41, 41, 41, 41, 41,
	}, {
		10, 14, 16, 20, 21, 29, 43, 53,
		14, 18, 20, 29, 37, 62, 84, 84,
		16, 20, 25, 37, 64, 84, 84, 84,
		20, 29, 37, 39, 72, 84, 84, 84,
		21, 37, 64, 72, 78, 84, 84, 84,
		29, 62, 84, 84, 84, 84, 84, 84,
		43, 84, 84, 84, 84, 84, 84, 84,
		53, 84, 84, 84, 84, 84, 84, 84,
	},
};

static const u8 zigzag[S7D_JPEG_QUANT_COEFFICIENTS] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

static u16 jpeg_reciprocal(u8 value)
{
	return min_t(u32, (65536U + value / 2) / value, 65535);
}

int s7d_jpeg_build_quantization(u32 quality,
				struct s7d_jpeg_quantization *quant)
{
	struct s7d_jpeg_quantization result;
	u8 natural[S7D_JPEG_QUANT_COEFFICIENTS];
	u32 scale, value;
	unsigned int table, i;

	if (!quant)
		return -EINVAL;
	if (!quality || quality > 100)
		return -ERANGE;

	scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for (table = 0; table < S7D_JPEG_QUANT_TABLES; table++) {
		for (i = 0; i < S7D_JPEG_QUANT_COEFFICIENTS; i++) {
			value = (jpeg_quant[table][i] * scale + 50) / 100;
			natural[i] = clamp_t(u32, value, 1, 255);
		}
		for (i = 0; i < S7D_JPEG_QUANT_COEFFICIENTS; i += 2)
			result.lut[table][i / 2] = jpeg_reciprocal(natural[i]) |
				((u32)jpeg_reciprocal(natural[i + 1]) << 16);
		for (i = 0; i < S7D_JPEG_QUANT_COEFFICIENTS; i++)
			result.dqt[table][i] = natural[zigzag[i]];
	}
	*quant = result;
	return 0;
}
