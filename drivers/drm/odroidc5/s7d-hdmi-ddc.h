/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_DDC_H
#define __S7D_HDMI_DDC_H

#include <linux/i2c.h>
#include <linux/mutex.h>

struct regmap;

struct s7d_hdmi_ddc {
	struct i2c_adapter adapter;
	struct regmap *core;
	/* Whole FIFO transactions and recovery, including parent TX setup. */
	struct mutex lock;
	bool fault;
};

/*
 * Parent must retain DDC/APB clocks, runtime PM and accessible TX SRAM for the
 * entire adapter lifetime (200 MHz basic-clock profile), independently of
 * the pixel PLL/PHY. No TX reset may race a transfer. Parent stops users and deletes the adapter before release.
 * Setup preserves output/PHY; it only programs the DDC controller.
 */
int s7d_hdmi_ddc_register(struct device *dev, struct s7d_hdmi_ddc *ddc,
			struct regmap *core);
void s7d_hdmi_ddc_unregister(struct s7d_hdmi_ddc *ddc);

#endif
