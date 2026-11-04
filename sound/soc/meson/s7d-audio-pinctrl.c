// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amlogic S7D audio pad crossbar
 *
 * Copyright (c) 2026 Hardkernel Co., Ltd.
 *
 * The audio block has pads of its own, which the SoC pin controller then
 * muxes to GPIOs. Each TDMIN lane selects the data pad it reads, each data
 * pad selects the TDMOUT lane it drives or works as an input, and each bit
 * and frame clock pad selects a master clock or works as an input. Pads
 * TDM_D0-D8, TDM_SCLK0-2 and TDM_LRCLK0-2 reach the SoC pins.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

/* Data pad window (reg 0, audio bus offset 0xe40) */
#define S7D_DAT_PAD_IN(n)	((n) * 4)		/* TDMIN lane 4n-4n+3 input pads */
#define S7D_DAT_PAD_OUT(n)	(0x18 + (n) * 4)	/* pad 4n-4n+3 TDMOUT lanes */
#define S7D_DAT_PAD_OEN_SEL	0x38			/* pad direction from TDMOUT */
#define S7D_DAT_PAD_OEN		0x3c			/* pad direction: 1 = input */
#define S7D_DAT_PAD_SEL		GENMASK(4, 0)

/* Clock pad window (reg 1, audio bus offset 0xe88) */
#define S7D_SCLK_PAD		0x0
#define S7D_LRCLK_PAD		0x4
#define S7D_CLK_PAD_IN		BIT(3)
#define S7D_CLK_PAD_FIELD	GENMASK(3, 0)

#define S7D_AUDIO_DATA_PADS	9
#define S7D_AUDIO_CLK_PADS	3

static const struct pinctrl_pin_desc s7d_audio_pins[] = {
	PINCTRL_PIN(0, "TDM_D0"),
	PINCTRL_PIN(1, "TDM_D1"),
	PINCTRL_PIN(2, "TDM_D2"),
	PINCTRL_PIN(3, "TDM_D3"),
	PINCTRL_PIN(4, "TDM_D4"),
	PINCTRL_PIN(5, "TDM_D5"),
	PINCTRL_PIN(6, "TDM_D6"),
	PINCTRL_PIN(7, "TDM_D7"),
	PINCTRL_PIN(8, "TDM_D8"),
	PINCTRL_PIN(9, "TDM_SCLK0"),
	PINCTRL_PIN(10, "TDM_SCLK1"),
	PINCTRL_PIN(11, "TDM_SCLK2"),
	PINCTRL_PIN(12, "TDM_LRCLK0"),
	PINCTRL_PIN(13, "TDM_LRCLK1"),
	PINCTRL_PIN(14, "TDM_LRCLK2"),
};

/* One group per pad, in pin order: the data pads, then the clock pads. */
static const char * const s7d_audio_groups[] = {
	"tdm_d0", "tdm_d1", "tdm_d2", "tdm_d3", "tdm_d4",
	"tdm_d5", "tdm_d6", "tdm_d7", "tdm_d8",
	"tdm_sclk0", "tdm_sclk1", "tdm_sclk2",
	"tdm_lrclk0", "tdm_lrclk1", "tdm_lrclk2",
};

static const unsigned int s7d_audio_group_pins[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};

#define S7D_LANES(f)	f "_lane0", f "_lane1", f "_lane2", f "_lane3", \
			f "_lane4", f "_lane5", f "_lane6", f "_lane7"

/* The names the vendor kernel uses, which board files already know */
static const char * const s7d_audio_functions[] = {
	S7D_LANES("tdmina"), S7D_LANES("tdminb"), S7D_LANES("tdminc"),
	S7D_LANES("tdmouta"), S7D_LANES("tdmoutb"), S7D_LANES("tdmoutc"),
	"tdm_clk_outa", "tdm_clk_outb", "tdm_clk_outc",
	"tdm_clk_outd", "tdm_clk_oute", "tdm_clk_outf",
	"tdm_clk_in",
};

/* Function ranges: TDMIN and TDMOUT A-C lanes 0-7, MST A-F, clock input */
#define S7D_FUNC_TDMIN		0
#define S7D_FUNC_TDMOUT		24
#define S7D_FUNC_CLK_OUT	48
#define S7D_FUNC_CLK_IN		54

struct s7d_audio_pads {
	struct regmap *data;
	struct regmap *clk;
};

static int s7d_audio_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(s7d_audio_groups);
}

static const char *s7d_audio_get_group_name(struct pinctrl_dev *pctldev,
					    unsigned int selector)
{
	return s7d_audio_groups[selector];
}

static int s7d_audio_get_group_pins(struct pinctrl_dev *pctldev,
				    unsigned int selector,
				    const unsigned int **pins,
				    unsigned int *num_pins)
{
	*pins = &s7d_audio_group_pins[selector];
	*num_pins = 1;
	return 0;
}

static const struct pinctrl_ops s7d_audio_pctrl_ops = {
	.get_groups_count = s7d_audio_get_groups_count,
	.get_group_name = s7d_audio_get_group_name,
	.get_group_pins = s7d_audio_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int s7d_audio_get_functions_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(s7d_audio_functions);
}

static const char *s7d_audio_get_function_name(struct pinctrl_dev *pctldev,
					       unsigned int selector)
{
	return s7d_audio_functions[selector];
}

static int s7d_audio_get_function_groups(struct pinctrl_dev *pctldev,
					 unsigned int selector,
					 const char * const **groups,
					 unsigned int * const num_groups)
{
	if (selector < S7D_FUNC_CLK_OUT) {
		*groups = s7d_audio_groups;
		*num_groups = S7D_AUDIO_DATA_PADS;
	} else {
		*groups = &s7d_audio_groups[S7D_AUDIO_DATA_PADS];
		*num_groups = 2 * S7D_AUDIO_CLK_PADS;
	}
	return 0;
}

static int s7d_audio_set_data_pad(struct s7d_audio_pads *pads,
				  unsigned int func, unsigned int pad)
{
	unsigned int lane, shift;
	bool input;
	int ret;

	if (func < S7D_FUNC_TDMOUT) {
		/* The lane, numbered across TDMIN A-C, reads the pad. */
		lane = func - S7D_FUNC_TDMIN;
		shift = (lane % 4) * 8;
		ret = regmap_update_bits(pads->data, S7D_DAT_PAD_IN(lane / 4),
					 S7D_DAT_PAD_SEL << shift, pad << shift);
		input = true;
	} else {
		/* The pad drives the lane, numbered across TDMOUT A-C. */
		lane = func - S7D_FUNC_TDMOUT;
		shift = (pad % 4) * 8;
		ret = regmap_update_bits(pads->data, S7D_DAT_PAD_OUT(pad / 4),
					 S7D_DAT_PAD_SEL << shift, lane << shift);
		input = false;
	}
	if (ret)
		return ret;
	/* Take the pad direction from the OEN register, not from TDMOUT. */
	ret = regmap_update_bits(pads->data, S7D_DAT_PAD_OEN_SEL, BIT(pad), 0);
	if (ret)
		return ret;
	return regmap_update_bits(pads->data, S7D_DAT_PAD_OEN, BIT(pad),
				  input ? BIT(pad) : 0);
}

static int s7d_audio_set_mux(struct pinctrl_dev *pctldev, unsigned int func,
			     unsigned int group)
{
	struct s7d_audio_pads *pads = pinctrl_dev_get_drvdata(pctldev);
	unsigned int pad, reg, shift, val;

	if (group < S7D_AUDIO_DATA_PADS) {
		if (func >= S7D_FUNC_CLK_OUT)
			return -EINVAL;
		return s7d_audio_set_data_pad(pads, func, group);
	}

	if (func < S7D_FUNC_CLK_OUT)
		return -EINVAL;
	/* Bit clock pads 0-2 then frame clock pads 0-2, a nibble each */
	pad = group - S7D_AUDIO_DATA_PADS;
	reg = pad < S7D_AUDIO_CLK_PADS ? S7D_SCLK_PAD : S7D_LRCLK_PAD;
	shift = (pad % S7D_AUDIO_CLK_PADS) * 4;
	val = func == S7D_FUNC_CLK_IN ? S7D_CLK_PAD_IN : func - S7D_FUNC_CLK_OUT;
	return regmap_update_bits(pads->clk, reg, S7D_CLK_PAD_FIELD << shift,
				  val << shift);
}

static const struct pinmux_ops s7d_audio_pmx_ops = {
	.get_functions_count = s7d_audio_get_functions_count,
	.get_function_name = s7d_audio_get_function_name,
	.get_function_groups = s7d_audio_get_function_groups,
	.set_mux = s7d_audio_set_mux,
};

static struct pinctrl_desc s7d_audio_pctrl_desc = {
	.name = "s7d-audio-pads",
	.pins = s7d_audio_pins,
	.npins = ARRAY_SIZE(s7d_audio_pins),
	.pctlops = &s7d_audio_pctrl_ops,
	.pmxops = &s7d_audio_pmx_ops,
	.owner = THIS_MODULE,
};

static const struct regmap_config s7d_audio_pads_data_config = {
	.name = "data",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = S7D_DAT_PAD_OEN,
	.fast_io = true,
};

static const struct regmap_config s7d_audio_pads_clk_config = {
	.name = "clk",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = S7D_LRCLK_PAD,
	.fast_io = true,
};

static struct regmap *s7d_audio_pads_regmap(struct platform_device *pdev,
					    unsigned int index,
					    const struct regmap_config *config)
{
	struct resource *res;
	void __iomem *base;

	base = devm_platform_get_and_ioremap_resource(pdev, index, &res);
	if (IS_ERR(base))
		return ERR_CAST(base);
	if (resource_size(res) < config->max_register + 4)
		return ERR_PTR(-EINVAL);
	return devm_regmap_init_mmio(&pdev->dev, base, config);
}

static void s7d_audio_pads_pm_put(void *data)
{
	pm_runtime_put_sync(data);
}

static int s7d_audio_pads_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s7d_audio_pads *pads;
	struct pinctrl_dev *pctl;
	struct clk *pclk;
	int ret;

	pads = devm_kzalloc(dev, sizeof(*pads), GFP_KERNEL);
	if (!pads)
		return -ENOMEM;
	pads->data = s7d_audio_pads_regmap(pdev, 0, &s7d_audio_pads_data_config);
	if (IS_ERR(pads->data))
		return PTR_ERR(pads->data);
	pads->clk = s7d_audio_pads_regmap(pdev, 1, &s7d_audio_pads_clk_config);
	if (IS_ERR(pads->clk))
		return PTR_ERR(pads->clk);

	/* The registers need the audio power domain and bus clock. */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to power audio domain\n");
	ret = devm_add_action_or_reset(dev, s7d_audio_pads_pm_put, dev);
	if (ret)
		return ret;
	pclk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(pclk))
		return dev_err_probe(dev, PTR_ERR(pclk), "failed to enable bus clock\n");

	pctl = devm_pinctrl_register(dev, &s7d_audio_pctrl_desc, pads);
	if (IS_ERR(pctl))
		return dev_err_probe(dev, PTR_ERR(pctl), "failed to register pads\n");
	return 0;
}

static const struct of_device_id s7d_audio_pads_of_match[] = {
	{ .compatible = "amlogic,s7d-audio-pinctrl" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_audio_pads_of_match);

static struct platform_driver s7d_audio_pads_driver = {
	.probe = s7d_audio_pads_probe,
	.driver = {
		.name = "s7d-audio-pinctrl",
		.of_match_table = s7d_audio_pads_of_match,
	},
};
module_platform_driver(s7d_audio_pads_driver);

MODULE_DESCRIPTION("Amlogic S7D audio pad crossbar");
MODULE_LICENSE("GPL");
