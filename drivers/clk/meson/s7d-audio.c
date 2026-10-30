// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/reset-controller.h>
#include <dt-bindings/clock/amlogic,s7d-audio-clkc.h>
#include <dt-bindings/reset/amlogic,s7d-audio-reset.h>

#include "clk-regmap.h"
#include "clk-phase.h"
#include "sclk-div.h"

/* Offsets in the clock window (reg 0, audio bus offset 0x000). */
#define S7D_AUDIO_SW_RESET0	0x28
#define S7D_AUDIO_MST_SCLK(i)	(0x40 + (i) * 8)
#define S7D_AUDIO_TDMIN_CLK(i)	(0x80 + (i) * 4)
#define S7D_AUDIO_TDMOUT_CLK(i)	(0x90 + (i) * 4)
#define S7D_AUDIO_SPDIFOUT_CLK	0xa0
#define S7D_AUDIO_SPDIFOUT_B_CLK	0xb4

/* Offsets in the pad window (reg 1, audio bus offset 0xe80). */
#define S7D_AUDIO_MCLK_PAD_CTRL0	0x0

#define S7D_AUDIO_NUM_MST	6

struct s7d_audio_clkc {
	struct reset_controller_dev rcdev;
	struct regmap *map;
};

static const struct clk_parent_data s7d_mclk_parents[] = {
	{ .fw_name = "hifi1" },
	{ .fw_name = "hifi0" },
	{ .fw_name = "fclk-div3" },
	{ .fw_name = "fclk-div4" },
	{ .fw_name = "xtal" },
};

/* S7D input 7 is xtal, unlike the vendor TXHD2 audio table. */
static u32 s7d_mclk_mux_table[] = { 0, 4, 5, 6, 7 };

static const unsigned int s7d_mst_ids[S7D_AUDIO_NUM_MST][2] = {
	{ S7D_AUDIO_CLK_MST_A_SCLK, S7D_AUDIO_CLK_MST_A_LRCLK },
	{ S7D_AUDIO_CLK_MST_B_SCLK, S7D_AUDIO_CLK_MST_B_LRCLK },
	{ S7D_AUDIO_CLK_MST_C_SCLK, S7D_AUDIO_CLK_MST_C_LRCLK },
	{ S7D_AUDIO_CLK_MST_D_SCLK, S7D_AUDIO_CLK_MST_D_LRCLK },
	{ S7D_AUDIO_CLK_MST_E_SCLK, S7D_AUDIO_CLK_MST_E_LRCLK },
	{ S7D_AUDIO_CLK_MST_F_SCLK, S7D_AUDIO_CLK_MST_F_LRCLK },
};

struct s7d_audio_tdm {
	const char *name;
	unsigned int reg;
	bool out;
	unsigned int sclk_sel;
	unsigned int sclk;
	unsigned int lrclk;
};

static const struct s7d_audio_tdm s7d_audio_tdms[] = {
	{ "tdmin_a", S7D_AUDIO_TDMIN_CLK(0), false, S7D_AUDIO_CLK_TDMIN_A_SCLK_SEL,
	  S7D_AUDIO_CLK_TDMIN_A_SCLK, S7D_AUDIO_CLK_TDMIN_A_LRCLK },
	{ "tdmin_b", S7D_AUDIO_TDMIN_CLK(1), false, S7D_AUDIO_CLK_TDMIN_B_SCLK_SEL,
	  S7D_AUDIO_CLK_TDMIN_B_SCLK, S7D_AUDIO_CLK_TDMIN_B_LRCLK },
	{ "tdmin_c", S7D_AUDIO_TDMIN_CLK(2), false, S7D_AUDIO_CLK_TDMIN_C_SCLK_SEL,
	  S7D_AUDIO_CLK_TDMIN_C_SCLK, S7D_AUDIO_CLK_TDMIN_C_LRCLK },
	{ "tdmin_lb", S7D_AUDIO_TDMIN_CLK(3), false, S7D_AUDIO_CLK_TDMIN_LB_SCLK_SEL,
	  S7D_AUDIO_CLK_TDMIN_LB_SCLK, S7D_AUDIO_CLK_TDMIN_LB_LRCLK },
	{ "tdmout_a", S7D_AUDIO_TDMOUT_CLK(0), true, S7D_AUDIO_CLK_TDMOUT_A_SCLK_SEL,
	  S7D_AUDIO_CLK_TDMOUT_A_SCLK, S7D_AUDIO_CLK_TDMOUT_A_LRCLK },
	{ "tdmout_b", S7D_AUDIO_TDMOUT_CLK(1), true, S7D_AUDIO_CLK_TDMOUT_B_SCLK_SEL,
	  S7D_AUDIO_CLK_TDMOUT_B_SCLK, S7D_AUDIO_CLK_TDMOUT_B_LRCLK },
	{ "tdmout_c", S7D_AUDIO_TDMOUT_CLK(2), true, S7D_AUDIO_CLK_TDMOUT_C_SCLK_SEL,
	  S7D_AUDIO_CLK_TDMOUT_C_SCLK, S7D_AUDIO_CLK_TDMOUT_C_LRCLK },
};

static struct clk_hw *s7d_audio_register(struct device *dev, struct regmap *map,
		const char *name, const struct clk_ops *ops, const void *data,
		size_t size, const struct clk_parent_data *parents, unsigned int count,
		unsigned long flags)
{
	struct clk_init_data init = {
		.ops = ops,
		.parent_data = parents,
		.num_parents = count,
		.flags = flags,
	};
	struct clk_regmap *clk;
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++)
		if (IS_ERR(parents[i].hw))
			return ERR_CAST(parents[i].hw);
	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);
	clk->data = devm_kmemdup(dev, data, size, GFP_KERNEL);
	init.name = devm_kasprintf(dev, GFP_KERNEL, "%s.%s", dev_name(dev), name);
	if (!clk->data || !init.name)
		return ERR_PTR(-ENOMEM);
	clk->map = map;
	clk->hw.init = &init;
	ret = devm_clk_hw_register(dev, &clk->hw);
	return ret ? ERR_PTR(ret) : &clk->hw;
}

static struct clk_hw *s7d_audio_gate(struct device *dev, struct regmap *map,
		const char *name, const struct clk_parent_data *parent,
		u32 reg, u8 bit, unsigned long flags)
{
	struct clk_regmap_gate_data data = { .offset = reg, .bit_idx = bit };

	return s7d_audio_register(dev, map, name, &clk_regmap_gate_ops,
			&data, sizeof(data), parent, 1, flags);
}

static struct clk_hw *s7d_audio_div(struct device *dev, struct regmap *map,
		const char *name, const struct clk_hw *parent, u32 reg,
		u8 shift, u8 width, unsigned long flags)
{
	struct clk_parent_data p = { .hw = parent };
	struct clk_regmap_div_data data = {
		.offset = reg, .shift = shift, .width = width,
		.flags = CLK_DIVIDER_ROUND_CLOSEST,
	};

	return s7d_audio_register(dev, map, name, &clk_regmap_divider_ops,
			&data, sizeof(data), &p, 1, flags);
}

static struct clk_hw *s7d_audio_mclk(struct device *dev, struct regmap *map, int index)
{
	struct clk_regmap_mux_data src = {
		.offset = 0x8 + index * 4, .shift = 24, .mask = 7,
		.table = s7d_mclk_mux_table, .flags = CLK_MUX_ROUND_CLOSEST,
	};
	struct clk_regmap_mux_data bypass = {
		.offset = src.offset, .shift = 30, .mask = 1,
		.flags = CLK_MUX_ROUND_CLOSEST,
	};
	struct clk_parent_data p[2] = { {}, { .fw_name = "xtal" } };
	struct clk_hw *hw;
	char name[24];

	snprintf(name, sizeof(name), "mclk_%c_sel", 'a' + index);
	hw = s7d_audio_register(dev, map, name, &clk_regmap_mux_ops,
			&src, sizeof(src), s7d_mclk_parents,
			ARRAY_SIZE(s7d_mclk_parents), CLK_SET_RATE_PARENT);
	snprintf(name, sizeof(name), "mclk_%c_div", 'a' + index);
	hw = s7d_audio_div(dev, map, name, hw, src.offset, 0, 16, CLK_SET_RATE_PARENT);
	snprintf(name, sizeof(name), "mclk_%c_gate", 'a' + index);
	p[0].hw = hw;
	p[0].hw = s7d_audio_gate(dev, map, name, &p[0], src.offset, 31, CLK_SET_RATE_PARENT);

	/* Force-osc bypasses both the divider and its gate. */
	snprintf(name, sizeof(name), "mclk_%c", 'a' + index);
	return s7d_audio_register(dev, map, name, &clk_regmap_mux_ops,
			&bypass, sizeof(bypass), p, ARRAY_SIZE(p), CLK_SET_RATE_PARENT);
}

static struct clk_hw *s7d_audio_sample_div(struct device *dev, struct regmap *map,
		const char *name, const struct clk_hw *parent, unsigned int reg, bool lrclk)
{
	struct clk_parent_data p = { .hw = parent };
	struct meson_sclk_div_data data = {
		.div = { .reg_off = reg, .shift = lrclk ? 0 : 20, .width = 10 },
		.hi = { .reg_off = reg, .shift = 10, .width = lrclk ? 10 : 0 },
	};

	return s7d_audio_register(dev, map, name, &meson_sclk_div_ops,
			&data, sizeof(data), &p, 1, 0);
}

static struct clk_hw *s7d_audio_phase(struct device *dev, struct regmap *map,
		const char *name, const struct clk_hw *parent, unsigned int reg, u8 shift)
{
	struct clk_parent_data p = { .hw = parent };
	struct meson_clk_phase_data data = {
		.ph = { .reg_off = reg, .shift = shift, .width = 1 },
	};

	return s7d_audio_register(dev, map, name, &meson_clk_phase_ops,
			&data, sizeof(data), &p, 1,
			CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT);
}

/*
 * Master clock inverters: one bit each for the pad, TDMIN and TDMOUT
 * copies of the clock, spaced two bits apart (sclk 0/2/4, lrclk 1/3/5).
 */
static struct clk_hw *s7d_audio_triphase(struct device *dev, struct regmap *map,
		const char *name, const struct clk_hw *parent, unsigned int reg, u8 shift)
{
	struct clk_parent_data p = { .hw = parent };
	struct meson_clk_triphase_data data = {
		.ph0 = { .reg_off = reg, .shift = shift, .width = 1 },
		.ph1 = { .reg_off = reg, .shift = shift + 2, .width = 1 },
		.ph2 = { .reg_off = reg, .shift = shift + 4, .width = 1 },
	};

	return s7d_audio_register(dev, map, name, &meson_clk_triphase_ops,
			&data, sizeof(data), &p, 1,
			CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT);
}

/* TDMOUT bit clock inverter; the ws bit must hold the opposite value. */
static struct clk_hw *s7d_audio_sclk_ws(struct device *dev, struct regmap *map,
		const char *name, const struct clk_hw *parent, unsigned int reg)
{
	struct clk_parent_data p = { .hw = parent };
	struct meson_sclk_ws_inv_data data = {
		.ph = { .reg_off = reg, .shift = 29, .width = 1 },
		.ws = { .reg_off = reg, .shift = 28, .width = 1 },
	};

	return s7d_audio_register(dev, map, name, &meson_sclk_ws_inv_ops,
			&data, sizeof(data), &p, 1,
			CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT);
}

static struct clk_hw *s7d_audio_mux(struct device *dev, struct regmap *map,
		const char *name, const struct clk_parent_data *parents,
		unsigned int count, u32 reg, u8 shift, u32 mask, u8 mux_flags,
		unsigned long flags)
{
	struct clk_regmap_mux_data data = {
		.offset = reg, .shift = shift, .mask = mask, .flags = mux_flags,
	};

	return s7d_audio_register(dev, map, name, &clk_regmap_mux_ops,
			&data, sizeof(data), parents, count, flags);
}

static int s7d_audio_master(struct device *dev, struct regmap *map,
		struct clk_hw_onecell_data *clks, unsigned int index)
{
	struct clk_parent_data p = { .hw = clks->hws[CLKID_AUDIO_MCLK_A + index] };
	unsigned int reg = S7D_AUDIO_MST_SCLK(index);
	struct clk_hw *hw;
	char name[24];

	snprintf(name, sizeof(name), "mst_%c_pre", 'a' + index);
	p.hw = s7d_audio_gate(dev, map, name, &p, reg, 31, 0);
	snprintf(name, sizeof(name), "mst_%c_sclk_div", 'a' + index);
	p.hw = s7d_audio_sample_div(dev, map, name, p.hw, reg, false);
	snprintf(name, sizeof(name), "mst_%c_post", 'a' + index);
	p.hw = s7d_audio_gate(dev, map, name, &p, reg, 30, CLK_SET_RATE_PARENT);
	snprintf(name, sizeof(name), "mst_%c_sclk", 'a' + index);
	hw = s7d_audio_triphase(dev, map, name, p.hw, reg + 4, 0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[s7d_mst_ids[index][0]] = hw;
	snprintf(name, sizeof(name), "mst_%c_lrclk_div", 'a' + index);
	hw = s7d_audio_sample_div(dev, map, name, p.hw, reg, true);
	snprintf(name, sizeof(name), "mst_%c_lrclk", 'a' + index);
	hw = s7d_audio_triphase(dev, map, name, hw, reg + 4, 1);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[s7d_mst_ids[index][1]] = hw;
	return 0;
}

/*
 * Formatter bit and sample clock selection. Values 0-5 select MST A-F;
 * the slave inputs (6-15) are not wired on ODROID-C5 and are left out.
 */
static int s7d_audio_tdm(struct device *dev, struct regmap *map,
		struct clk_hw_onecell_data *clks, const struct s7d_audio_tdm *tdm)
{
	struct clk_parent_data sclk[S7D_AUDIO_NUM_MST], lrclk[S7D_AUDIO_NUM_MST];
	struct clk_parent_data p = {};
	struct clk_hw *hw;
	char name[32];
	unsigned int i;

	for (i = 0; i < S7D_AUDIO_NUM_MST; i++) {
		sclk[i] = (struct clk_parent_data) { .hw = clks->hws[s7d_mst_ids[i][0]] };
		lrclk[i] = (struct clk_parent_data) { .hw = clks->hws[s7d_mst_ids[i][1]] };
	}

	snprintf(name, sizeof(name), "%s_sclk_sel", tdm->name);
	hw = s7d_audio_mux(dev, map, name, sclk, ARRAY_SIZE(sclk), tdm->reg,
			   24, 0xf, CLK_MUX_ROUND_CLOSEST, CLK_DUTY_CYCLE_PARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[tdm->sclk_sel] = hw;
	snprintf(name, sizeof(name), "%s_sclk_pre_en", tdm->name);
	p.hw = hw;
	p.hw = s7d_audio_gate(dev, map, name, &p, tdm->reg, 31,
			      CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT);
	snprintf(name, sizeof(name), "%s_sclk_post_en", tdm->name);
	p.hw = s7d_audio_gate(dev, map, name, &p, tdm->reg, 30,
			      CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT);
	snprintf(name, sizeof(name), "%s_sclk", tdm->name);
	if (tdm->out)
		hw = s7d_audio_sclk_ws(dev, map, name, p.hw, tdm->reg);
	else
		hw = s7d_audio_phase(dev, map, name, p.hw, tdm->reg, 29);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[tdm->sclk] = hw;

	snprintf(name, sizeof(name), "%s_lrclk", tdm->name);
	hw = s7d_audio_mux(dev, map, name, lrclk, ARRAY_SIZE(lrclk), tdm->reg,
			   20, 0xf, CLK_MUX_ROUND_CLOSEST, CLK_DUTY_CYCLE_PARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[tdm->lrclk] = hw;
	return 0;
}

static int s7d_audio_spdif_a(struct device *dev, struct regmap *map,
		struct clk_hw_onecell_data *clks)
{
	struct clk_regmap_mux_data mux = {
		.offset = S7D_AUDIO_SPDIFOUT_CLK, .shift = 24, .mask = 0xf,
		.table = s7d_mclk_mux_table,
	};
	struct clk_regmap_mux_data bypass = {
		.offset = S7D_AUDIO_SPDIFOUT_CLK, .shift = 30, .mask = 1,
		.flags = CLK_MUX_ROUND_CLOSEST,
	};
	struct clk_parent_data p = {}, bypass_parents[2] = { {}, { .fw_name = "xtal" } };
	struct clk_hw *hw;

	hw = s7d_audio_register(dev, map, "spdif_a_sel", &clk_regmap_mux_ops,
			&mux, sizeof(mux), s7d_mclk_parents,
			ARRAY_SIZE(s7d_mclk_parents),
			CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_SPDIF_A_SEL] = hw;
	p.hw = s7d_audio_div(dev, map, "spdif_a_div", hw, S7D_AUDIO_SPDIFOUT_CLK,
			     0, 10, CLK_SET_RATE_PARENT);
	hw = s7d_audio_gate(dev, map, "spdif_a", &p, S7D_AUDIO_SPDIFOUT_CLK, 31,
			    CLK_SET_RATE_PARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	bypass_parents[0].hw = hw;
	hw = s7d_audio_register(dev, map, "spdif_a_output", &clk_regmap_mux_ops,
			&bypass, sizeof(bypass), bypass_parents, ARRAY_SIZE(bypass_parents),
			CLK_SET_RATE_PARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[CLKID_AUDIO_SPDIFOUT_A] = hw;
	return 0;
}

/* SPDIF B only feeds the HDMI transmitter. */
static int s7d_audio_spdif_b(struct device *dev, struct regmap *map,
		struct clk_hw_onecell_data *clks)
{
	struct clk_regmap_mux_data mux = {
		.offset = S7D_AUDIO_SPDIFOUT_B_CLK, .shift = 24, .mask = 0xf,
		.table = s7d_mclk_mux_table,
	};
	struct clk_parent_data p = {};
	struct clk_hw *hw;

	hw = s7d_audio_register(dev, map, "spdif_b_sel", &clk_regmap_mux_ops,
			&mux, sizeof(mux), s7d_mclk_parents,
			ARRAY_SIZE(s7d_mclk_parents),
			CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_SPDIF_B_SEL] = hw;
	p.hw = s7d_audio_div(dev, map, "spdif_b_div", hw, S7D_AUDIO_SPDIFOUT_B_CLK,
			     0, 10, CLK_SET_RATE_PARENT);
	hw = s7d_audio_gate(dev, map, "spdif_b", &p, S7D_AUDIO_SPDIFOUT_B_CLK, 31,
			    CLK_SET_RATE_PARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[CLKID_AUDIO_SPDIFOUT_B] = hw;
	return 0;
}

/* MCLK pad 1 drives GPIOD_4 from one of the internal MCLK generators. */
static int s7d_audio_mclk_pad1(struct device *dev, struct regmap *map,
		struct clk_hw_onecell_data *clks)
{
	struct clk_regmap_mux_data mux = {
		.offset = S7D_AUDIO_MCLK_PAD_CTRL0, .shift = 24, .mask = 7,
	};
	struct clk_parent_data p[6];
	struct clk_hw *hw;
	int i;

	for (i = 0; i < ARRAY_SIZE(p); i++)
		p[i] = (struct clk_parent_data) { .hw = clks->hws[CLKID_AUDIO_MCLK_A + i] };
	hw = s7d_audio_register(dev, map, "mclk_pad1_sel", &clk_regmap_mux_ops,
			&mux, sizeof(mux), p, ARRAY_SIZE(p), CLK_SET_RATE_NO_REPARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_MCLK_PAD1_SEL] = hw;
	p[0].hw = s7d_audio_div(dev, map, "mclk_pad1_div", hw, S7D_AUDIO_MCLK_PAD_CTRL0,
				16, 8, 0);
	hw = s7d_audio_gate(dev, map, "mclk_pad1", &p[0], S7D_AUDIO_MCLK_PAD_CTRL0, 31,
			    CLK_SET_RATE_PARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_MCLK_PAD1] = hw;
	return 0;
}

static int s7d_audio_reset_xlate(struct reset_controller_dev *rcdev,
		const struct of_phandle_args *spec)
{
	u32 id = spec->args[0];

	if (id > S7D_AUDIO_RESET_FRDDRD && id != S7D_AUDIO_RESET_LOCKERB)
		return -EINVAL;
	return id;
}

static int s7d_audio_reset_status(struct reset_controller_dev *rcdev,
		unsigned long id)
{
	struct s7d_audio_clkc *c = container_of(rcdev, struct s7d_audio_clkc, rcdev);
	u32 val;
	int ret;

	ret = regmap_read(c->map, S7D_AUDIO_SW_RESET0 + (id / 32) * 4, &val);
	return ret ? ret : !!(val & BIT(id % 32));
}

static int s7d_audio_reset_update(struct reset_controller_dev *rcdev,
		unsigned long id, bool assert)
{
	struct s7d_audio_clkc *c = container_of(rcdev, struct s7d_audio_clkc, rcdev);
	u32 mask = BIT(id % 32);
	int ret;

	ret = regmap_update_bits(c->map, S7D_AUDIO_SW_RESET0 + (id / 32) * 4,
				 mask, assert ? mask : 0);
	if (ret)
		return ret;
	ret = s7d_audio_reset_status(rcdev, id);
	if (ret < 0)
		return ret;
	return ret == assert ? 0 : -EIO;
}

static int s7d_audio_reset_assert(struct reset_controller_dev *rcdev,
		unsigned long id)
{
	return s7d_audio_reset_update(rcdev, id, true);
}

static int s7d_audio_reset_deassert(struct reset_controller_dev *rcdev,
		unsigned long id)
{
	return s7d_audio_reset_update(rcdev, id, false);
}

static int s7d_audio_reset_toggle(struct reset_controller_dev *rcdev,
		unsigned long id)
{
	int ret;

	ret = s7d_audio_reset_assert(rcdev, id);
	if (ret)
		return ret;
	return s7d_audio_reset_deassert(rcdev, id);
}

static const struct reset_control_ops s7d_audio_reset_ops = {
	.assert = s7d_audio_reset_assert,
	.deassert = s7d_audio_reset_deassert,
	.reset = s7d_audio_reset_toggle,
	.status = s7d_audio_reset_status,
};

static struct regmap *s7d_audio_regmap(struct platform_device *pdev,
		unsigned int index, const char *name)
{
	struct regmap_config config = {
		.name = name,
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.fast_io = true,
	};
	struct resource *res;
	void __iomem *base;

	base = devm_platform_get_and_ioremap_resource(pdev, index, &res);
	if (IS_ERR(base))
		return ERR_CAST(base);
	config.max_register = resource_size(res) - 4;
	return devm_regmap_init_mmio(&pdev->dev, base, &config);
}

static void s7d_audio_pm_put(void *data)
{
	pm_runtime_put_sync(data);
}

static int s7d_audio_clkc_probe(struct platform_device *pdev)
{
	static const struct {
		unsigned int id;
		const char *name;
	} gates[] = {
		{ CLKID_AUDIO_GATE_DDR_ARB, "ddr_arb" },
		{ CLKID_AUDIO_GATE_TDMINA, "tdmin_a" },
		{ CLKID_AUDIO_GATE_TDMINB, "tdmin_b" },
		{ CLKID_AUDIO_GATE_TDMINC, "tdmin_c" },
		{ CLKID_AUDIO_GATE_TDMINLB, "tdmin_lb" },
		{ CLKID_AUDIO_GATE_TDMOUTA, "tdmout_a" },
		{ CLKID_AUDIO_GATE_TDMOUTB, "tdmout_b" },
		{ CLKID_AUDIO_GATE_TDMOUTC, "tdmout_c" },
		{ CLKID_AUDIO_GATE_FRDDRA, "frddr_a" },
		{ CLKID_AUDIO_GATE_FRDDRB, "frddr_b" },
		{ CLKID_AUDIO_GATE_FRDDRC, "frddr_c" },
		{ CLKID_AUDIO_GATE_FRDDRD, "frddr_d" },
		{ CLKID_AUDIO_GATE_TODDRA, "toddr_a" },
		{ CLKID_AUDIO_GATE_TODDRB, "toddr_b" },
		{ CLKID_AUDIO_GATE_TODDRC, "toddr_c" },
		{ CLKID_AUDIO_GATE_TODDRD, "toddr_d" },
		{ CLKID_AUDIO_GATE_SPDIFOUT_A, "spdifout_a" },
		{ CLKID_AUDIO_GATE_SPDIFOUT_B, "spdifout_b" },
	};
	struct clk_parent_data p = { .fw_name = "pclk" };
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clks;
	struct reset_control *reset;
	struct regmap *map, *pad_map;
	struct s7d_audio_clkc *c;
	struct clk_hw *hw;
	struct clk *pclk;
	unsigned int id, i;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	map = s7d_audio_regmap(pdev, 0, "clk");
	if (IS_ERR(map))
		return PTR_ERR(map);
	pad_map = s7d_audio_regmap(pdev, 1, "pad");
	if (IS_ERR(pad_map))
		return PTR_ERR(pad_map);

	reset = devm_reset_control_get_exclusive(dev, "audio");
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get bus reset\n");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to power audio domain\n");
	ret = devm_add_action_or_reset(dev, s7d_audio_pm_put, dev);
	if (ret)
		return ret;

	/* Clock and reset callbacks require a live register interface. */
	pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(pclk))
		return dev_err_probe(dev, PTR_ERR(pclk), "failed to enable bus clock\n");

	ret = reset_control_deassert(reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to release bus reset\n");

	clks = devm_kzalloc(dev, struct_size(clks, hws, S7D_AUDIO_CLK_COUNT), GFP_KERNEL);
	if (!clks)
		return -ENOMEM;
	clks->num = S7D_AUDIO_CLK_COUNT;
	/* Make unimplemented IDs fail lookups instead of returning NULL. */
	for (i = 0; i < S7D_AUDIO_CLK_COUNT; i++)
		clks->hws[i] = ERR_PTR(-ENOENT);
	for (i = 0; i < ARRAY_SIZE(gates); i++) {
		id = gates[i].id;
		hw = s7d_audio_gate(dev, map, gates[i].name, &p, (id / 32) * 4, id % 32, 0);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		clks->hws[id] = hw;
	}
	for (i = 0; i < 6; i++) {
		hw = s7d_audio_mclk(dev, map, i);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		clks->hws[CLKID_AUDIO_MCLK_A + i] = hw;
	}
	for (i = 0; i < S7D_AUDIO_NUM_MST; i++) {
		ret = s7d_audio_master(dev, map, clks, i);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(s7d_audio_tdms); i++) {
		ret = s7d_audio_tdm(dev, map, clks, &s7d_audio_tdms[i]);
		if (ret)
			return ret;
	}
	ret = s7d_audio_spdif_a(dev, map, clks);
	if (ret)
		return ret;
	ret = s7d_audio_spdif_b(dev, map, clks);
	if (ret)
		return ret;
	ret = s7d_audio_mclk_pad1(dev, pad_map, clks);
	if (ret)
		return ret;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clks);
	if (ret)
		return ret;

	c->map = map;
	c->rcdev.owner = THIS_MODULE;
	c->rcdev.dev = dev;
	c->rcdev.of_node = dev->of_node;
	c->rcdev.ops = &s7d_audio_reset_ops;
	c->rcdev.nr_resets = S7D_AUDIO_RESET_LOCKERB + 1;
	c->rcdev.of_reset_n_cells = 1;
	c->rcdev.of_xlate = s7d_audio_reset_xlate;
	return devm_reset_controller_register(dev, &c->rcdev);
}

static const struct of_device_id s7d_audio_clkc_of_match[] = {
	{ .compatible = "amlogic,s7d-audio-clkc" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_audio_clkc_of_match);

static struct platform_driver s7d_audio_clkc_driver = {
	.probe = s7d_audio_clkc_probe,
	.driver = {
		.name = "meson-s7d-audio-clkc",
		.of_match_table = s7d_audio_clkc_of_match,
	},
};
module_platform_driver(s7d_audio_clkc_driver);

MODULE_DESCRIPTION("Amlogic S7D audio clocks");
MODULE_LICENSE("GPL");
