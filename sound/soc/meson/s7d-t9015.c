// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define T9015_BLOCK_ENABLE	0x00
#define T9015_GAIN		0x04
#define T9015_VOLUME		0x08
#define T9015_LINEOUT		0x0c
#define T9015_POWER		0x10

struct s7d_t9015 {
	struct regmap *map;
	u32 volume;
	bool initialized;
};

static int s7d_t9015_power_on(struct snd_soc_component *component)
{
	struct s7d_t9015 *codec = snd_soc_component_get_drvdata(component);
	struct regmap *map = codec->map;
	const struct reg_sequence defaults[] = {
		{ T9015_BLOCK_ENABLE, 0xb03f },
		{ T9015_GAIN, 0 },
		{ T9015_VOLUME, (codec->initialized ? codec->volume : 0xfbfb0000) | BIT(15) },
		{ T9015_LINEOUT, 0x1111 },
		{ T9015_POWER, 0x10000 },
	};
	int ret;

	ret = regmap_write(map, T9015_BLOCK_ENABLE, 0xc000);
	if (ret)
		return ret;
	msleep(200);
	ret = regmap_write(map, T9015_BLOCK_ENABLE, 0xf000);
	if (ret)
		return ret;
	msleep(50);
	ret = regmap_write(map, T9015_BLOCK_ENABLE, 0xb000);
	if (ret)
		return ret;
	usleep_range(1000, 1050);
	ret = regmap_multi_reg_write(map, defaults, ARRAY_SIZE(defaults));
	if (!ret)
		codec->initialized = true;
	return ret;
}

static int s7d_t9015_power_off(struct snd_soc_component *component)
{
	struct s7d_t9015 *codec = snd_soc_component_get_drvdata(component);
	struct regmap *map = codec->map;
	int ret;

	ret = regmap_read(map, T9015_VOLUME, &codec->volume);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, T9015_VOLUME, BIT(15), BIT(15));
	if (ret)
		return ret;
	ret = regmap_write(map, T9015_BLOCK_ENABLE, 0xb000);
	if (ret)
		return ret;
	usleep_range(10000, 10050);
	ret = regmap_write(map, T9015_BLOCK_ENABLE, 0xc000);
	if (ret)
		return ret;
	msleep(200);
	return regmap_write(map, T9015_BLOCK_ENABLE, 0);
}

static int s7d_t9015_bias(struct snd_soc_component *component,
			 enum snd_soc_bias_level level)
{
	if (level == SND_SOC_BIAS_STANDBY &&
	    snd_soc_component_get_bias_level(component) == SND_SOC_BIAS_OFF)
		return s7d_t9015_power_on(component);
	if (level == SND_SOC_BIAS_OFF)
		return s7d_t9015_power_off(component);
	return 0;
}

static int s7d_t9015_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BC_FC)
		return -EINVAL;
	return regmap_update_bits(dev_get_regmap(dai->component->dev, NULL),
				 T9015_BLOCK_ENABLE, BIT(30), 0);
}

static int s7d_t9015_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	return regmap_update_bits(dev_get_regmap(dai->component->dev, NULL),
				 T9015_VOLUME, BIT(15), mute ? BIT(15) : 0);
}

static const struct snd_soc_dai_ops s7d_t9015_ops = {
	.set_fmt = s7d_t9015_set_fmt,
	.mute_stream = s7d_t9015_mute,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver s7d_t9015_dai = {
	.name = "s7d-t9015-hifi",
	.playback = {
		.stream_name = "Analog Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
	.ops = &s7d_t9015_ops,
};

static const struct snd_kcontrol_new s7d_t9015_controls[] = {
	SOC_DOUBLE("Analog Playback Volume", T9015_VOLUME, 24, 16, 0xff, 0),
};

static const struct snd_soc_dapm_widget s7d_t9015_widgets[] = {
	SND_SOC_DAPM_DAC("Left DAC", "Analog Playback", T9015_BLOCK_ENABLE, 5, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Analog Playback", T9015_BLOCK_ENABLE, 4, 0),
	SND_SOC_DAPM_OUT_DRV("Left Out P", T9015_BLOCK_ENABLE, 3, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Left Out N", T9015_BLOCK_ENABLE, 2, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Right Out P", T9015_BLOCK_ENABLE, 1, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Right Out N", T9015_BLOCK_ENABLE, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUTPUT("Lineout Left"),
	SND_SOC_DAPM_OUTPUT("Lineout Right"),
};

static const struct snd_soc_dapm_route s7d_t9015_routes[] = {
	{ "Left Out P", NULL, "Left DAC" },
	{ "Left Out N", NULL, "Left DAC" },
	{ "Right Out P", NULL, "Right DAC" },
	{ "Right Out N", NULL, "Right DAC" },
	{ "Lineout Left", NULL, "Left Out P" },
	{ "Lineout Left", NULL, "Left Out N" },
	{ "Lineout Right", NULL, "Right Out P" },
	{ "Lineout Right", NULL, "Right Out N" },
};

static const struct snd_soc_component_driver s7d_t9015_component = {
	.set_bias_level = s7d_t9015_bias,
	.controls = s7d_t9015_controls,
	.num_controls = ARRAY_SIZE(s7d_t9015_controls),
	.dapm_widgets = s7d_t9015_widgets,
	.num_dapm_widgets = ARRAY_SIZE(s7d_t9015_widgets),
	.dapm_routes = s7d_t9015_routes,
	.num_dapm_routes = ARRAY_SIZE(s7d_t9015_routes),
	.use_pmdown_time = 1,
};

static const struct regmap_config s7d_t9015_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = T9015_POWER,
};

static int s7d_t9015_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *reset;
	struct s7d_t9015 *codec;
	struct regmap *map;
	struct clk *pclk;
	void __iomem *base;
	int ret;

	codec = devm_kzalloc(dev, sizeof(*codec), GFP_KERNEL);
	if (!codec)
		return -ENOMEM;
	platform_set_drvdata(pdev, codec);
	pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(pclk))
		return dev_err_probe(dev, PTR_ERR(pclk), "codec clock\n");
	reset = devm_reset_control_get_exclusive(dev, "acodec");
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "codec reset\n");
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	map = devm_regmap_init_mmio(dev, base, &s7d_t9015_regmap);
	if (IS_ERR(map))
		return PTR_ERR(map);
	codec->map = map;
	return devm_snd_soc_register_component(dev, &s7d_t9015_component,
					    &s7d_t9015_dai, 1);
}

static const struct of_device_id s7d_t9015_match[] = {
	{ .compatible = "amlogic,s7d-t9015" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_t9015_match);

static struct platform_driver s7d_t9015_driver = {
	.probe = s7d_t9015_probe,
	.driver = {
		.name = "s7d-t9015",
		.of_match_table = s7d_t9015_match,
	},
};
module_platform_driver(s7d_t9015_driver);

MODULE_DESCRIPTION("Amlogic S7D T9015 stereo DAC");
MODULE_LICENSE("GPL");
