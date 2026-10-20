/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_FILTER_H
#define __S7D_FILTER_H

#include <linux/types.h>

#define S7D_FILTER_PHASES 33

extern const u32 s7d_filter_bilinear[S7D_FILTER_PHASES];
extern const u32 s7d_filter_bicubic[S7D_FILTER_PHASES];
extern const u32 s7d_filter_triangle[S7D_FILTER_PHASES];

#endif
