/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_IO_H
#define __S7D_HDMI_IO_H

struct platform_device;
struct regmap;

/* DT core/top resources only; no VPU, clock, reset or analogue mapping. */
int s7d_hdmi_init_regmaps(struct platform_device *pdev, struct regmap **core,
			struct regmap **top);
/* Use only on ordinary R/W configuration fields, never FIFO/W1C/pulse words. */
int s7d_hdmi_update_checked(struct regmap *map, unsigned int reg,
			   unsigned int mask, unsigned int value);

#endif
