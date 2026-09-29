// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/amlogic,s7d-audio-clkc.h>

#include "clk-regmap.h"
#include "clk-phase.h"
#include "sclk-div.h"

static const struct clk_parent_data s7d_mclk_parents[] = {
	{ .fw_name = "hifi1" },
	{ .fw_name = "hifi0" },
	{ .fw_name = "fclk-div3" },
	{ .fw_name = "fclk-div4" },
	{ .fw_name = "xtal" },
};

/* S7D input 7 is xtal, unlike the vendor TXHD2 audio table. */
static u32 s7d_mclk_mux_table[] = { 0, 4, 5, 6, 7 };

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
		const char *name, const struct clk_hw *parent, bool lrclk)
{
	struct clk_parent_data p = { .hw = parent };
	struct meson_sclk_div_data data = {
		.div = { .reg_off = 0x48, .shift = lrclk ? 0 : 20, .width = 10 },
		.hi = { .reg_off = 0x48, .shift = 10, .width = lrclk ? 10 : 0 },
	};

	return s7d_audio_register(dev, map, name, &meson_sclk_div_ops,
			&data, sizeof(data), &p, 1, 0);
}

static struct clk_hw *s7d_audio_phase(struct device *dev, struct regmap *map,
		const char *name, const struct clk_hw *parent, u8 shift)
{
	struct clk_parent_data p = { .hw = parent };
	struct meson_clk_phase_data data = {
		.ph = { .reg_off = 0x4c, .shift = shift, .width = 1 },
	};

	return s7d_audio_register(dev, map, name, &meson_clk_phase_ops,
			&data, sizeof(data), &p, 1,
			CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT);
}

static int s7d_audio_master_b(struct device *dev, struct regmap *map,
		struct clk_hw_onecell_data *clks)
{
	struct clk_parent_data p = { .hw = clks->hws[CLKID_AUDIO_MCLK_B] };
	struct clk_hw *hw;

	p.hw = s7d_audio_gate(dev, map, "mst_b_pre", &p, 0x48, 31, 0);
	p.hw = s7d_audio_sample_div(dev, map, "mst_b_sclk_div", p.hw, false);
	p.hw = s7d_audio_gate(dev, map, "mst_b_post", &p, 0x48, 30, CLK_SET_RATE_PARENT);
	hw = s7d_audio_phase(dev, map, "mst_b_sclk", p.hw, 4);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_MST_B_SCLK] = hw;
	hw = s7d_audio_sample_div(dev, map, "mst_b_lrclk_div", p.hw, true);
	hw = s7d_audio_phase(dev, map, "mst_b_lrclk", hw, 5);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_MST_B_LRCLK] = hw;
	return 0;
}

static int s7d_audio_hdmi_mclk(struct device *dev, struct regmap *map,
		struct clk_hw_onecell_data *clks)
{
	struct clk_regmap_mux_data mux = { .offset = 0x744, .shift = 16, .mask = 7 };
	struct clk_parent_data p[6];
	struct clk_hw *hw;
	int i;

	for (i = 0; i < ARRAY_SIZE(p); i++)
		p[i] = (struct clk_parent_data) { .hw = clks->hws[CLKID_AUDIO_MCLK_A + i] };
	hw = s7d_audio_register(dev, map, "hdmi_mclk_sel", &clk_regmap_mux_ops,
			&mux, sizeof(mux), p, ARRAY_SIZE(p), CLK_SET_RATE_NO_REPARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_HDMI_MCLK_SEL] = hw;
	p[0].hw = s7d_audio_div(dev, map, "hdmi_mclk_div", hw, 0x744, 20, 8, 0);
	hw = s7d_audio_gate(dev, map, "hdmi_mclk", &p[0], 0x744, 19, CLK_SET_RATE_PARENT);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clks->hws[S7D_AUDIO_CLK_HDMI_MCLK] = hw;
	return 0;
}

static int s7d_audio_clkc_probe(struct platform_device *pdev)
{
	static const struct {
		unsigned int id;
		const char *name;
	} gates[] = {
		{ CLKID_AUDIO_GATE_DDR_ARB, "ddr_arb" },
		{ CLKID_AUDIO_GATE_TDMOUTA, "tdmout_a" },
		{ CLKID_AUDIO_GATE_TDMOUTB, "tdmout_b" },
		{ CLKID_AUDIO_GATE_TDMOUTC, "tdmout_c" },
		{ CLKID_AUDIO_GATE_FRDDRA, "frddr_a" },
		{ CLKID_AUDIO_GATE_FRDDRB, "frddr_b" },
		{ CLKID_AUDIO_GATE_FRDDRC, "frddr_c" },
		{ CLKID_AUDIO_GATE_FRDDRD, "frddr_d" },
	};
	struct clk_parent_data p = { .fw_name = "pclk" };
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clks;
	struct regmap *map;
	struct clk_hw *hw;
	unsigned int id, i;
	int ret;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -EPROBE_DEFER;
	clks = devm_kzalloc(dev, struct_size(clks, hws, S7D_AUDIO_CLK_COUNT), GFP_KERNEL);
	if (!clks)
		return -ENOMEM;
	clks->num = S7D_AUDIO_CLK_COUNT;
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
	ret = s7d_audio_master_b(dev, map, clks);
	if (ret)
		return ret;
	ret = s7d_audio_hdmi_mclk(dev, map, clks);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clks);
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
