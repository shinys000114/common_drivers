/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_PLL_H
#define __S7D_HDMI_PLL_H

#include <linux/amlogic/clk/s7d-hdmi-pll-rate.h>

struct device;
struct regmap;
struct meson_clk_hw_data;

int s7d_hdmi_clocks_register(struct device *dev, struct regmap *pll, struct meson_clk_hw_data *data);

#endif
