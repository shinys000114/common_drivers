/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VDIN_H
#define __S7D_VDIN_H

#include "s7d-writeback.h"

struct s7d_vdin;
struct s7d_vpu;

extern const struct s7d_writeback_caps s7d_vdin_writeback_caps;
extern const struct s7d_writeback_ops s7d_vdin_writeback_ops;

/* State allocation only; the VPU owns the register mapping and PM reference. */
int s7d_vdin_init(struct s7d_vpu *vpu, struct s7d_vdin **result);
/* VPU VSYNC interrupt hook for an armed capture. */
void s7d_vdin_vsync(struct s7d_vdin *v);

#endif
