// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* S7D HDMI clock ownership; analogue sequencing derived from hdmitx_hw_s7d.c. */
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <dt-bindings/clock/amlogic,s7d-clkc.h>

#include "meson-clkc-utils.h"
#include "s7d-hdmi-pll.h"

#define PLL_CTRL0	0x1c0
#define PLL_CTRL1	0x1c4
#define PLL_CTRL2	0x1c8
#define PLL_CTRL3	0x1cc
#define PLL_BIAS	BIT(28)
#define PLL_LOCK_EN	BIT(29)
#define PLL_RESET_N	BIT(30)
#define PLL_LOCK	BIT(31)
#define PLL_FREE_RUN	BIT(18)

struct s7d_hdmi_pll {
	struct clk_hw hw;
	struct regmap *map;
	struct device *dev;
	/* Last requested configuration survives unprepare()/prepare(). */
	u32 saved[4];
	bool saved_valid;
};

#define to_s7d_pll(h) container_of(h, struct s7d_hdmi_pll, hw)

static int s7d_pll_start(struct s7d_hdmi_pll *pll, const u32 *cfg)
{
	u32 val;
	int ret, i;

	for (i = 0; i < 4; i++) {
		val = cfg[i];
		if (!i)
			val &= ~(PLL_BIAS | PLL_LOCK_EN | PLL_RESET_N |
				 PLL_LOCK | PLL_FREE_RUN);
		ret = regmap_write(pll->map, PLL_CTRL0 + 4 * i, val);
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_BIAS, PLL_BIAS);
	if (ret)
		return ret;
	usleep_range(10, 20);
	ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_FREE_RUN, PLL_FREE_RUN);
	if (ret)
		return ret;
	usleep_range(10, 20);
	ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_RESET_N, PLL_RESET_N);
	if (ret)
		return ret;
	ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_FREE_RUN, 0);
	if (ret)
		return ret;
	usleep_range(80, 90);
	ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_LOCK_EN, PLL_LOCK_EN);
	if (ret)
		return ret;
	return regmap_read_poll_timeout(pll->map, PLL_CTRL0, val,
				       val & PLL_LOCK, 50, 2000);
}

static int s7d_pll_is_prepared(struct clk_hw *hw)
{
	u32 val;

	if (regmap_read(to_s7d_pll(hw)->map, PLL_CTRL0, &val))
		return 0;
	return (val & (PLL_LOCK | PLL_BIAS | PLL_RESET_N)) ==
		(PLL_LOCK | PLL_BIAS | PLL_RESET_N);
}

static int s7d_pll_prepare(struct clk_hw *hw)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);
	int ret;

	/* Acquiring the firmware's live clock must not pulse its reset. */
	if (s7d_pll_is_prepared(hw))
		return 0;
	if (!pll->saved_valid)
		return -EINVAL;
	ret = s7d_pll_start(pll, pll->saved);
	if (ret)
		regmap_update_bits(pll->map, PLL_CTRL0,
				   PLL_BIAS | PLL_LOCK_EN | PLL_RESET_N, 0);
	return ret;
}

static void s7d_pll_unprepare(struct clk_hw *hw)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);

	regmap_update_bits(pll->map, PLL_CTRL0,
			   PLL_BIAS | PLL_LOCK_EN | PLL_RESET_N, 0);
}

static unsigned long s7d_pll_recalc_rate(struct clk_hw *hw, unsigned long parent)
{
	u32 c0, c3;
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);

	if (regmap_read(pll->map, PLL_CTRL0, &c0) ||
	    regmap_read(pll->map, PLL_CTRL3, &c3))
		return 0;
	return s7d_hdmi_pll_decode(c0, c3, parent);
}

static long s7d_pll_round_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long *parent)
{
	struct s7d_hdmi_pll_rate cfg;
	int ret = s7d_hdmi_pll_calculate(rate, *parent, &cfg);

	return ret ? ret : cfg.rate;
}

static int s7d_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			  unsigned long parent)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);
	struct s7d_hdmi_pll_rate cfg;
	u32 old[4], next[4];
	bool running;
	int ret, restore;

	ret = s7d_hdmi_pll_calculate(rate, parent, &cfg);
	if (ret)
		return ret;
	ret = regmap_bulk_read(pll->map, PLL_CTRL0, old, ARRAY_SIZE(old));
	if (ret)
		return ret;
	running = s7d_pll_is_prepared(hw);
	next[0] = 0x00017000 | cfg.m | (cfg.od00 << 20) | (cfg.od01 << 23);
	next[1] = 0x9040137d;
	next[2] = 0x04000000;
	next[3] = 0x011e0000 | cfg.frac; /* OD21=1, analogue /5, SSC off */
	ret = s7d_pll_start(pll, next);
	if (ret) {
		/* Keep CCF's unchanged cached rate consistent with the hardware. */
		if (running)
			restore = s7d_pll_start(pll, old);
		else
			restore = regmap_bulk_write(pll->map, PLL_CTRL0, old, 4);
		if (restore) {
			s7d_pll_unprepare(hw);
			dev_err(pll->dev, "HDMI PLL restore failed: %d (rate error %d)\n",
				restore, ret);
		}
		return ret;
	}
	memcpy(pll->saved, next, sizeof(next));
	pll->saved_valid = true;
	if (!running)
		s7d_pll_unprepare(hw);
	return 0;
}

static const struct clk_ops s7d_pll_ops = {
	.prepare = s7d_pll_prepare,
	.unprepare = s7d_pll_unprepare,
	.is_prepared = s7d_pll_is_prepared,
	.is_enabled = s7d_pll_is_prepared,
	.recalc_rate = s7d_pll_recalc_rate,
	.round_rate = s7d_pll_round_rate,
	.set_rate = s7d_pll_set_rate,
};

int s7d_hdmi_clocks_register(struct device *dev, struct regmap *map,
			    struct meson_clk_hw_data *data)
{
	struct clk_parent_data parent = { .fw_name = "xtal" };
	struct clk_init_data init = {
		.name = "s7d_hdmi_pll",
		.ops = &s7d_pll_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct s7d_hdmi_pll *pll;
	int ret;

	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;
	pll->dev = dev;
	pll->map = map;
	pll->hw.init = &init;
	ret = regmap_bulk_read(map, PLL_CTRL0, pll->saved, 4);
	if (ret)
		return ret;
	pll->saved_valid = !!(pll->saved[0] & 0x1ff);
	ret = devm_clk_hw_register(dev, &pll->hw);
	if (ret)
		return ret;
	data->hws[CLKID_HDMI_PLL] = &pll->hw;
	return 0;
}
