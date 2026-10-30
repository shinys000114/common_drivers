// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <dt-bindings/clock/amlogic,s7d-clkc.h>

#include "meson-clkc-utils.h"
#include "s7d-hdmi-pll.h"

#define PLL_CTRL0	0x1c0
#define PLL_CTRL3	0x1cc
#define PLL_BIAS	BIT(28)
#define PLL_LOCK_EN	BIT(29)
#define PLL_RESET_N	BIT(30)
#define PLL_LOCK	BIT(31)
#define PLL_FREE_RUN	BIT(18)
#define PLL_POWER	(PLL_BIAS | PLL_LOCK_EN | PLL_RESET_N | PLL_FREE_RUN)
#define PLL_SERIAL_OD	GENMASK(29, 27)
#define PLL_PIXEL_OD	GENMASK(26, 24)
#define PLL_PIXEL_DIV	GENMASK(23, 22)
#define PLL_CHILDREN	(PLL_SERIAL_OD | PLL_PIXEL_OD | PLL_PIXEL_DIV)
#define PLL_CFG0	(GENMASK(25, 20) | GENMASK(17, 16) | GENMASK(14, 0))
#define PLL_CFG1	(GENMASK(31, 25) | GENMASK(22, 20) | GENMASK(14, 6) | \
			 GENMASK(3, 2) | BIT(0))
#define PLL_CFG2	(GENMASK(31, 24) | GENMASK(19, 16) | GENMASK(13, 0))
#define PLL_CFG3	GENMASK(29, 0)

struct s7d_hdmi_pll;

struct s7d_hdmi_pll_output {
	struct clk_hw hw;
	struct s7d_hdmi_pll *pll;
	bool serial;
};

struct s7d_hdmi_pll {
	struct clk_hw hw;
	struct s7d_hdmi_pll_output serial;
	struct s7d_hdmi_pll_output pixel;
	struct regmap *map;
	struct device *dev;
	u32 saved[4];
	bool saved_valid;
	bool image_valid;
	bool fault;
};

#define to_s7d_pll(h) container_of(h, struct s7d_hdmi_pll, hw)
#define to_s7d_output(h) container_of(h, struct s7d_hdmi_pll_output, hw)

static bool s7d_pll_dividers_valid(u32 ctrl3)
{
	return FIELD_GET(PLL_SERIAL_OD, ctrl3) <= 3 &&
	       FIELD_GET(PLL_PIXEL_OD, ctrl3) <= 3 &&
	       FIELD_GET(PLL_PIXEL_DIV, ctrl3) != 3;
}

static bool s7d_pll_same_image(const u32 *a, const u32 *b)
{
	return !((a[0] ^ b[0]) & PLL_CFG0) &&
	       !((a[1] ^ b[1]) & PLL_CFG1) &&
	       !((a[2] ^ b[2]) & PLL_CFG2) &&
	       !((a[3] ^ b[3]) & PLL_CFG3);
}

static int s7d_pll_read(struct s7d_hdmi_pll *pll, u32 *cfg)
{
	return regmap_bulk_read(pll->map, PLL_CTRL0, cfg, 4);
}

static int s7d_pll_verify(struct s7d_hdmi_pll *pll, const u32 *cfg)
{
	u32 actual[4];
	int ret = s7d_pll_read(pll, actual);

	return ret ? ret : s7d_pll_same_image(actual, cfg) ? 0 : -EIO;
}

static bool s7d_pll_running(u32 ctrl0)
{
	return (ctrl0 & (PLL_LOCK | PLL_BIAS | PLL_RESET_N)) ==
		(PLL_LOCK | PLL_BIAS | PLL_RESET_N);
}

/* CCF continues child setters after a failed root update. */
static void s7d_pll_fault(struct s7d_hdmi_pll *pll, bool image_valid)
{
	pll->image_valid = image_valid;
	pll->fault = true;
}

static int s7d_pll_park(struct s7d_hdmi_pll *pll)
{
	u32 val;
	int ret;

	ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_POWER, 0);
	if (ret)
		return ret;
	ret = regmap_read(pll->map, PLL_CTRL0, &val);
	return ret ? ret : val & PLL_POWER ? -EIO : 0;
}

static int s7d_pll_start(struct s7d_hdmi_pll *pll, const u32 *cfg)
{
	u32 val;
	int ret;
	unsigned int i;

	for (i = 0; i < 4; i++) {
		val = cfg[i];
		if (!i)
			val &= ~(PLL_POWER | PLL_LOCK);
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
	/* Like the vendor, pulse the PLL reset while it fails to lock. */
	for (i = 0; i < 10; i++) {
		ret = regmap_read_poll_timeout(pll->map, PLL_CTRL0, val,
					       val & PLL_LOCK, 50, 200);
		if (ret != -ETIMEDOUT)
			break;
		ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_RESET_N, 0);
		if (!ret)
			ret = regmap_update_bits(pll->map, PLL_CTRL0, PLL_RESET_N,
						 PLL_RESET_N);
		if (ret)
			return ret;
		ret = -ETIMEDOUT;
	}
	if (i)
		dev_info(pll->dev, "HDMI PLL locked after %u resets\n", i);
	return ret ? ret : s7d_pll_verify(pll, cfg);
}

static int s7d_pll_restore(struct s7d_hdmi_pll *pll, const u32 *old, bool running);

/*
 * The vendor and upstream program the whole PLL on every rate change. A
 * parked PLL holding an image other than ours gets the saved image back;
 * a running PLL changed behind us is still refused.
 */
static int s7d_pll_reclaim(struct s7d_hdmi_pll *pll, u32 *actual)
{
	u32 image[4];
	int ret = s7d_pll_read(pll, actual);

	if (ret)
		return ret;
	if (s7d_pll_same_image(actual, pll->saved)) {
		pll->image_valid = true;
		return 0;
	}
	if (!pll->saved_valid || s7d_pll_running(actual[0]))
		return -EIO;
	/* An image adopted from a running firmware PLL carries its power bits. */
	memcpy(image, pll->saved, sizeof(image));
	image[0] &= ~(PLL_POWER | PLL_LOCK);
	ret = s7d_pll_restore(pll, image, false);
	if (!ret)
		ret = s7d_pll_read(pll, actual);
	if (ret)
		return ret;
	dev_info(pll->dev, "HDMI PLL image restored\n");
	pll->image_valid = true;
	pll->fault = false;
	return 0;
}

/* Rate planning only reads: an unowned parked image is reclaimable. */
static bool s7d_pll_usable(struct s7d_hdmi_pll *pll)
{
	u32 ctrl0;

	if (!pll->fault && pll->image_valid)
		return true;
	return pll->saved_valid && !regmap_read(pll->map, PLL_CTRL0, &ctrl0) &&
	       !s7d_pll_running(ctrl0);
}

static int s7d_pll_is_prepared(struct clk_hw *hw)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);
	u32 val;

	if (READ_ONCE(pll->fault) || regmap_read(pll->map, PLL_CTRL0, &val))
		return 0;
	return s7d_pll_running(val);
}

static int s7d_pll_prepare(struct clk_hw *hw)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);
	u32 actual[4];
	int ret;

	if (!pll->saved_valid)
		return -EINVAL;
	ret = s7d_pll_reclaim(pll, actual);
	if (ret) {
		s7d_pll_fault(pll, false);
		return ret;
	}
	if (s7d_pll_running(actual[0])) {
		pll->fault = false;
		return 0;
	}
	ret = s7d_pll_start(pll, pll->saved);
	if (ret) {
		int parked = s7d_pll_park(pll);

		s7d_pll_fault(pll, !parked && !s7d_pll_verify(pll, pll->saved));
	} else {
		pll->fault = false;
	}
	return ret;
}

static void s7d_pll_unprepare(struct clk_hw *hw)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);

	if (s7d_pll_park(pll) || s7d_pll_verify(pll, pll->saved))
		s7d_pll_fault(pll, false);
}

static unsigned long s7d_pll_recalc_rate(struct clk_hw *hw, unsigned long parent)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);
	u32 actual[4];

	/* Report only; prepare and set_rate act on a changed image. */
	if (s7d_pll_read(pll, actual))
		return 0;
	return s7d_hdmi_shared_decode(actual[0], actual[3], parent);
}

static long s7d_pll_round_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long *parent)
{
	struct s7d_hdmi_shared_rate cfg;
	int ret;

	ret = s7d_hdmi_shared_calculate(rate, *parent, &cfg);
	if (ret)
		return ret;
	return cfg.pll.rate;
}

static int s7d_pll_restore(struct s7d_hdmi_pll *pll, const u32 *old, bool running)
{
	u32 val;
	int ret;

	if (running)
		return s7d_pll_start(pll, old);
	ret = regmap_bulk_write(pll->map, PLL_CTRL0, old, 4);
	if (ret)
		return ret;
	ret = s7d_pll_verify(pll, old);
	if (ret)
		return ret;
	ret = regmap_read(pll->map, PLL_CTRL0, &val);
	return ret ? ret : (val ^ old[0]) & PLL_POWER ? -EIO : 0;
}

static int s7d_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			  unsigned long parent)
{
	struct s7d_hdmi_pll *pll = to_s7d_pll(hw);
	struct s7d_hdmi_shared_rate cfg;
	u32 old[4], next[4];
	bool running;
	int ret, restore;

	ret = s7d_hdmi_shared_calculate(rate, parent, &cfg);
	if (ret)
		return ret;
	ret = s7d_pll_read(pll, old);
	if (ret)
		return ret;
	running = s7d_pll_running(old[0]);
	/*
	 * A whole new image replaces whatever a parked PLL holds; refuse only
	 * a running PLL changed behind us.
	 */
	if (running && !s7d_pll_same_image(old, pll->saved)) {
		s7d_pll_fault(pll, false);
		return -EIO;
	}
	next[0] = 0x00017000 | cfg.pll.m | (cfg.pll.od00 << 20) |
		  (cfg.pll.od01 << 23);
	next[1] = 0x9040137d;
	next[2] = 0x04000000;
	next[3] = 0x001e0000 | cfg.pll.frac | (old[3] & PLL_CHILDREN);
	ret = s7d_pll_start(pll, next);
	if (!ret && !running) {
		ret = s7d_pll_park(pll);
		if (!ret)
			ret = s7d_pll_verify(pll, next);
	}
	if (ret) {
		restore = s7d_pll_restore(pll, old, running);
		s7d_pll_fault(pll, !restore);
		if (restore) {
			s7d_pll_park(pll);
			dev_err(pll->dev, "HDMI PLL restore failed: %d (rate error %d)\n",
				restore, ret);
		}
		return ret;
	}
	memcpy(pll->saved, next, sizeof(next));
	pll->saved_valid = true;
	pll->image_valid = true;
	pll->fault = false;
	return 0;
}

/* CCF also calls these with hypothetical parents while planning a root change. */
static unsigned long s7d_output_recalc_rate(struct clk_hw *hw, unsigned long parent)
{
	struct s7d_hdmi_pll_output *out = to_s7d_output(hw);
	u32 ctrl3 = out->pll->saved[3];

	return out->serial ? s7d_hdmi_serial_decode(parent, ctrl3) :
			     s7d_hdmi_pixel_decode(parent, ctrl3);
}

static int s7d_output_prepare(struct clk_hw *hw)
{
	struct s7d_hdmi_pll *pll = to_s7d_output(hw)->pll;
	u32 actual[4];
	int ret;

	if (!s7d_pll_usable(pll))
		return -EIO;
	ret = s7d_pll_read(pll, actual);
	if (ret || !s7d_pll_same_image(actual, pll->saved)) {
		s7d_pll_fault(pll, false);
		return ret ? ret : -EIO;
	}
	if (!s7d_pll_running(actual[0])) {
		s7d_pll_fault(pll, true);
		return -EIO;
	}
	return 0;
}

static long s7d_output_round_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long *parent)
{
	struct s7d_hdmi_pll_output *out = to_s7d_output(hw);
	struct s7d_hdmi_pll_divider cfg;
	int ret;

	if (!s7d_pll_usable(out->pll))
		return -EIO;
	if (rate && rate == s7d_output_recalc_rate(hw, *parent))
		return rate;
	ret = out->serial ? s7d_hdmi_serial_calculate(rate, *parent, &cfg) :
			    s7d_hdmi_pixel_calculate(rate, *parent, &cfg);
	if (ret)
		return ret;
	return cfg.rate;
}

static int s7d_output_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent)
{
	struct s7d_hdmi_pll_output *out = to_s7d_output(hw);
	struct s7d_hdmi_pll *pll = out->pll;
	struct s7d_hdmi_pll_divider cfg;
	u32 old[4], mask, value;
	int ret, restore;

	if (!s7d_pll_usable(pll))
		return -EIO;
	if (!rate || !parent)
		return -EINVAL;
	if (rate == s7d_output_recalc_rate(hw, parent))
		return 0;
	ret = out->serial ? s7d_hdmi_serial_calculate(rate, parent, &cfg) :
			    s7d_hdmi_pixel_calculate(rate, parent, &cfg);
	if (ret)
		return ret;
	ret = s7d_pll_reclaim(pll, old);
	if (ret) {
		s7d_pll_fault(pll, false);
		return ret;
	}
	if (out->serial) {
		mask = PLL_SERIAL_OD;
		value = FIELD_PREP(PLL_SERIAL_OD, cfg.od);
	} else {
		mask = PLL_PIXEL_OD | PLL_PIXEL_DIV;
		value = FIELD_PREP(PLL_PIXEL_OD, cfg.od) |
			FIELD_PREP(PLL_PIXEL_DIV, cfg.selector);
	}
	ret = regmap_update_bits(pll->map, PLL_CTRL3, mask, value);
	if (!ret) {
		u32 next[4];

		memcpy(next, old, sizeof(next));
		next[3] = (old[3] & ~mask) | value;
		ret = s7d_pll_verify(pll, next);
		if (!ret) {
			pll->saved[3] = (pll->saved[3] & ~mask) | value;
			return 0;
		}
	}
	restore = regmap_update_bits(pll->map, PLL_CTRL3, mask, old[3] & mask);
	if (!restore)
		restore = s7d_pll_verify(pll, old);
	s7d_pll_fault(pll, !restore);
	return ret;
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

static const struct clk_ops s7d_output_ops = {
	.prepare = s7d_output_prepare,
	.recalc_rate = s7d_output_recalc_rate,
	.round_rate = s7d_output_round_rate,
	.set_rate = s7d_output_set_rate,
};

int s7d_hdmi_clocks_register(struct device *dev, struct regmap *map,
			    struct meson_clk_hw_data *data)
{
	struct clk_parent_data parent = { .fw_name = "xtal" };
	struct clk_init_data init = {
		.name = "s7d_hdmi_shared",
		.ops = &s7d_pll_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_GATE |
			 (IS_ENABLED(CONFIG_DRM_S7D) ?
			  CLK_DONT_HOLD_STATE | CLK_IGNORE_UNUSED : 0),
	};
	struct s7d_hdmi_pll *pll;
	const struct clk_hw *shared;
	int ret;

	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;
	pll->dev = dev;
	pll->map = map;
	ret = s7d_pll_read(pll, pll->saved);
	if (ret)
		return ret;
	pll->image_valid = s7d_pll_dividers_valid(pll->saved[3]);
	pll->saved_valid = pll->image_valid && (pll->saved[0] & 0x1ff);
	pll->hw.init = &init;
	ret = devm_clk_hw_register(dev, &pll->hw);
	if (ret)
		return ret;
	data->hws[CLKID_HDMI_SHARED] = &pll->hw;

	shared = &pll->hw;
	init = (struct clk_init_data) {
		.name = "s7d_hdmi_serial",
		.ops = &s7d_output_ops,
		.parent_hws = &shared,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_GATE,
	};
	pll->serial.pll = pll;
	pll->serial.serial = true;
	pll->serial.hw.init = &init;
	ret = devm_clk_hw_register(dev, &pll->serial.hw);
	if (ret)
		return ret;
	data->hws[CLKID_HDMI_SERIAL] = &pll->serial.hw;

	init.name = "s7d_hdmi_pll";
	pll->pixel.pll = pll;
	pll->pixel.hw.init = &init;
	ret = devm_clk_hw_register(dev, &pll->pixel.hw);
	if (ret)
		return ret;
	data->hws[CLKID_HDMI_PLL] = &pll->pixel.hw;
	return 0;
}
