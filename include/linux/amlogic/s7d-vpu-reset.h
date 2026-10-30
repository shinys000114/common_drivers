/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef _LINUX_S7D_VPU_RESET_H
#define _LINUX_S7D_VPU_RESET_H

struct device;
struct device_node;
struct regmap;

/* Caller serializes access with VPU lifetime and holds PM/clock references. */
int devm_s7d_vpu_reset_register(struct device *dev, struct regmap *map,
			      struct device_node *node,
			      int (*check_access)(void *), void *data);

#endif
