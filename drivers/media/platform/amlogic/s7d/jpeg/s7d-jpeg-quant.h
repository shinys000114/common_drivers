/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_JPEG_QUANT_H
#define S7D_JPEG_QUANT_H

#include <linux/types.h>

#define S7D_JPEG_QUANT_TABLES	2
#define S7D_JPEG_QUANT_COEFFICIENTS	64

struct s7d_jpeg_quantization {
	/* Table 0 is Y, table 1 is Cb/Cr; DQT bytes are in zig-zag order. */
	u8 dqt[S7D_JPEG_QUANT_TABLES][S7D_JPEG_QUANT_COEFFICIENTS];
	/* Low/high halves contain consecutive reciprocals in natural order. */
	u32 lut[S7D_JPEG_QUANT_TABLES][S7D_JPEG_QUANT_COEFFICIENTS / 2];
};

/* Quality is 1..100; errors leave the caller's result unchanged. */
int s7d_jpeg_build_quantization(u32 quality,
				struct s7d_jpeg_quantization *quant);

#endif
