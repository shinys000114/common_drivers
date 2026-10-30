// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2019 BayLibre, SAS.
// Author: Jerome Brunet <jbrunet@baylibre.com>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <sound/pcm_params.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include <dt-bindings/sound/meson-g12a-tohdmitx.h>
#include "meson-codec-glue.h"

#define G12A_TOHDMITX_DRV_NAME "g12a-tohdmitx"

#define TOHDMITX_CTRL0			0x0
#define  CTRL0_ENABLE_SHIFT		31
#define  CTRL0_SPDIF_DAT_EN_SHIFT_S7D	31
#define  CTRL0_SPDIF_CLK_EN_SHIFT_S7D	30
#define  CTRL0_I2S_DAT_EN_SHIFT_S7D	29
#define  CTRL0_I2S_CLK_EN_SHIFT_S7D	28
#define  CTRL0_MCLK_DIV_S7D		GENMASK(27, 20)
#define  CTRL0_MCLK_EN_S7D		BIT(19)
#define  CTRL0_MCLK_SEL_S7D		GENMASK(18, 16)
#define  CTRL0_I2S_DAT_SEL_SHIFT	12
#define  CTRL0_I2S_DAT_SEL		(0x3 << CTRL0_I2S_DAT_SEL_SHIFT)
#define  CTRL0_I2S_LRCLK_SEL		GENMASK(9, 8)
#define  CTRL0_I2S_BLK_CAP_INV		BIT(7)
#define  CTRL0_I2S_BCLK_O_INV		BIT(6)
#define  CTRL0_I2S_BCLK_SEL		GENMASK(5, 4)
#define  CTRL0_SPDIF_CLK_CAP_INV	BIT(3)
#define  CTRL0_SPDIF_CLK_O_INV		BIT(2)
#define  CTRL0_SPDIF_SEL_SHIFT		1
#define  CTRL0_SPDIF_SEL		(0x1 << CTRL0_SPDIF_SEL_SHIFT)
#define  CTRL0_SPDIF_CLK_SEL		BIT(0)

#define TOHDMITX_I2S_IN_NUM		(TOHDMITX_I2S_IN_C + 1)
#define TOHDMITX_MCLK_FS		128

struct g12a_tohdmitx {
	struct clk *mclk[TOHDMITX_I2S_IN_NUM];
};

struct g12a_tohdmitx_match_data {
	const struct snd_soc_component_driver *component_drv;
	struct snd_soc_dai_driver *dai_drv;
	unsigned int num_dai_drv;
	bool has_mclk;
};

static const char * const g12a_tohdmitx_i2s_mux_texts[] = {
	"I2S A", "I2S B", "I2S C",
};

static int g12a_tohdmitx_i2s_mux_put_enum(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_dapm_kcontrol_component(kcontrol);
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int mux, changed;

	if (ucontrol->value.enumerated.item[0] >= e->items)
		return -EINVAL;

	mux = snd_soc_enum_item_to_val(e, ucontrol->value.enumerated.item[0]);
	changed = snd_soc_component_test_bits(component, e->reg,
					      CTRL0_I2S_DAT_SEL,
					      FIELD_PREP(CTRL0_I2S_DAT_SEL,
							 mux));

	if (!changed)
		return 0;

	/* Force disconnect of the mux while updating */
	snd_soc_dapm_mux_update_power(dapm, kcontrol, 0, NULL, NULL);

	snd_soc_component_update_bits(component, e->reg,
				      CTRL0_I2S_DAT_SEL |
				      CTRL0_I2S_LRCLK_SEL |
				      CTRL0_I2S_BCLK_SEL,
				      FIELD_PREP(CTRL0_I2S_DAT_SEL, mux) |
				      FIELD_PREP(CTRL0_I2S_LRCLK_SEL, mux) |
				      FIELD_PREP(CTRL0_I2S_BCLK_SEL, mux));

	snd_soc_dapm_mux_update_power(dapm, kcontrol, mux, e, NULL);

	return 1;
}

static SOC_ENUM_SINGLE_DECL(g12a_tohdmitx_i2s_mux_enum, TOHDMITX_CTRL0,
			    CTRL0_I2S_DAT_SEL_SHIFT,
			    g12a_tohdmitx_i2s_mux_texts);

static const struct snd_kcontrol_new g12a_tohdmitx_i2s_mux =
	SOC_DAPM_ENUM_EXT("I2S Source", g12a_tohdmitx_i2s_mux_enum,
			  snd_soc_dapm_get_enum_double,
			  g12a_tohdmitx_i2s_mux_put_enum);

static const char * const g12a_tohdmitx_spdif_mux_texts[] = {
	"SPDIF A", "SPDIF B",
};

static int g12a_tohdmitx_spdif_mux_put_enum(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_dapm_kcontrol_component(kcontrol);
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int mux, changed;

	if (ucontrol->value.enumerated.item[0] >= e->items)
		return -EINVAL;

	mux = snd_soc_enum_item_to_val(e, ucontrol->value.enumerated.item[0]);
	changed = snd_soc_component_test_bits(component, TOHDMITX_CTRL0,
					      CTRL0_SPDIF_SEL,
					      FIELD_PREP(CTRL0_SPDIF_SEL, mux));

	if (!changed)
		return 0;

	/* Force disconnect of the mux while updating */
	snd_soc_dapm_mux_update_power(dapm, kcontrol, 0, NULL, NULL);

	snd_soc_component_update_bits(component, TOHDMITX_CTRL0,
				      CTRL0_SPDIF_SEL |
				      CTRL0_SPDIF_CLK_SEL,
				      FIELD_PREP(CTRL0_SPDIF_SEL, mux) |
				      FIELD_PREP(CTRL0_SPDIF_CLK_SEL, mux));

	snd_soc_dapm_mux_update_power(dapm, kcontrol, mux, e, NULL);

	return 1;
}

static SOC_ENUM_SINGLE_DECL(g12a_tohdmitx_spdif_mux_enum, TOHDMITX_CTRL0,
			    CTRL0_SPDIF_SEL_SHIFT,
			    g12a_tohdmitx_spdif_mux_texts);

static const struct snd_kcontrol_new g12a_tohdmitx_spdif_mux =
	SOC_DAPM_ENUM_EXT("SPDIF Source", g12a_tohdmitx_spdif_mux_enum,
			  snd_soc_dapm_get_enum_double,
			  g12a_tohdmitx_spdif_mux_put_enum);

static const struct snd_kcontrol_new g12a_tohdmitx_out_enable =
	SOC_DAPM_SINGLE_AUTODISABLE("Switch", TOHDMITX_CTRL0,
				    CTRL0_ENABLE_SHIFT, 1, 0);

static const struct snd_soc_dapm_widget g12a_tohdmitx_widgets[] = {
	SND_SOC_DAPM_MUX("I2S SRC", SND_SOC_NOPM, 0, 0,
			 &g12a_tohdmitx_i2s_mux),
	SND_SOC_DAPM_SWITCH("I2S OUT EN", SND_SOC_NOPM, 0, 0,
			    &g12a_tohdmitx_out_enable),
	SND_SOC_DAPM_MUX("SPDIF SRC", SND_SOC_NOPM, 0, 0,
			 &g12a_tohdmitx_spdif_mux),
	SND_SOC_DAPM_SWITCH("SPDIF OUT EN", SND_SOC_NOPM, 0, 0,
			    &g12a_tohdmitx_out_enable),
};

static const struct snd_kcontrol_new s7d_tohdmitx_i2s_out_enable =
	SOC_DAPM_SINGLE_AUTODISABLE("Switch", TOHDMITX_CTRL0,
				    CTRL0_I2S_DAT_EN_SHIFT_S7D, 1, 0);

static const struct snd_kcontrol_new s7d_tohdmitx_spdif_out_enable =
	SOC_DAPM_SINGLE_AUTODISABLE("Switch", TOHDMITX_CTRL0,
				    CTRL0_SPDIF_DAT_EN_SHIFT_S7D, 1, 0);

/*
 * On the s7d, the clock and data enables are split. The clocks are
 * supplies of the data switches so DAPM enables the clock before the
 * data and disables the data before the clock.
 */
static const struct snd_soc_dapm_widget s7d_tohdmitx_widgets[] = {
	SND_SOC_DAPM_MUX("I2S SRC", SND_SOC_NOPM, 0, 0,
			 &g12a_tohdmitx_i2s_mux),
	SND_SOC_DAPM_SUPPLY("I2S CLK EN", TOHDMITX_CTRL0,
			    CTRL0_I2S_CLK_EN_SHIFT_S7D, 0, NULL, 0),
	SND_SOC_DAPM_SWITCH("I2S OUT EN", SND_SOC_NOPM, 0, 0,
			    &s7d_tohdmitx_i2s_out_enable),
	SND_SOC_DAPM_MUX("SPDIF SRC", SND_SOC_NOPM, 0, 0,
			 &g12a_tohdmitx_spdif_mux),
	SND_SOC_DAPM_SUPPLY("SPDIF CLK EN", TOHDMITX_CTRL0,
			    CTRL0_SPDIF_CLK_EN_SHIFT_S7D, 0, NULL, 0),
	SND_SOC_DAPM_SWITCH("SPDIF OUT EN", SND_SOC_NOPM, 0, 0,
			    &s7d_tohdmitx_spdif_out_enable),
};

static const struct snd_soc_dai_ops g12a_tohdmitx_input_ops = {
	.probe		= meson_codec_glue_input_dai_probe,
	.remove		= meson_codec_glue_input_dai_remove,
	.hw_params	= meson_codec_glue_input_hw_params,
	.set_fmt	= meson_codec_glue_input_set_fmt,
};

static bool s7d_tohdmitx_i2s_selected(struct snd_soc_dai *dai)
{
	unsigned int val;

	val = snd_soc_component_read(dai->component, TOHDMITX_CTRL0);

	return FIELD_GET(CTRL0_I2S_DAT_SEL, val) == dai->id;
}

static int s7d_tohdmitx_i2s_input_prepare(struct snd_pcm_substream *substream,
					  struct snd_soc_dai *dai)
{
	struct g12a_tohdmitx *priv = snd_soc_dai_get_drvdata(dai);
	struct meson_codec_glue_input *data =
		meson_codec_glue_input_get_data(dai);
	struct clk *mclk = priv->mclk[dai->id];
	unsigned long rate, freq;
	unsigned int div;

	/* Nothing to do if this input is not the one routed to HDMI */
	if (!mclk || !s7d_tohdmitx_i2s_selected(dai))
		return 0;

	/*
	 * The HDMI TX needs a 128 x fs master clock. It is derived from
	 * the MCLK of the TDM interface feeding the selected input.
	 *
	 * FIXME:
	 * Here we assume interface A uses clock A, etc ... Same as the
	 * internal DAC glue, the glue should find out the clock used by
	 * the interface instead.
	 */
	rate = clk_get_rate(mclk);
	freq = data->params.rate_min * TOHDMITX_MCLK_FS;
	div = freq ? rate / freq : 0;
	if (!div || rate % freq || div > FIELD_MAX(CTRL0_MCLK_DIV_S7D) + 1) {
		dev_err(dai->dev, "can't derive %lu Hz from mclk %lu Hz\n",
			freq, rate);
		return -EINVAL;
	}

	/* Gate the clock while updating the source and divider */
	snd_soc_component_update_bits(dai->component, TOHDMITX_CTRL0,
				      CTRL0_MCLK_EN_S7D, 0);
	snd_soc_component_update_bits(dai->component, TOHDMITX_CTRL0,
				      CTRL0_MCLK_SEL_S7D | CTRL0_MCLK_DIV_S7D,
				      FIELD_PREP(CTRL0_MCLK_SEL_S7D, dai->id) |
				      FIELD_PREP(CTRL0_MCLK_DIV_S7D, div - 1));
	snd_soc_component_update_bits(dai->component, TOHDMITX_CTRL0,
				      CTRL0_MCLK_EN_S7D, CTRL0_MCLK_EN_S7D);

	return 0;
}

static void s7d_tohdmitx_i2s_input_shutdown(struct snd_pcm_substream *substream,
					    struct snd_soc_dai *dai)
{
	struct g12a_tohdmitx *priv = snd_soc_dai_get_drvdata(dai);

	if (!priv->mclk[dai->id] || !s7d_tohdmitx_i2s_selected(dai))
		return;

	snd_soc_component_update_bits(dai->component, TOHDMITX_CTRL0,
				      CTRL0_MCLK_EN_S7D, 0);
}

static const struct snd_soc_dai_ops s7d_tohdmitx_i2s_input_ops = {
	.probe		= meson_codec_glue_input_dai_probe,
	.remove		= meson_codec_glue_input_dai_remove,
	.hw_params	= meson_codec_glue_input_hw_params,
	.set_fmt	= meson_codec_glue_input_set_fmt,
	.prepare	= s7d_tohdmitx_i2s_input_prepare,
	.shutdown	= s7d_tohdmitx_i2s_input_shutdown,
};

static const struct snd_soc_dai_ops g12a_tohdmitx_output_ops = {
	.startup	= meson_codec_glue_output_startup,
};

#define TOHDMITX_SPDIF_FORMATS					\
	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |	\
	 SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S24_LE)

#define TOHDMITX_I2S_FORMATS					\
	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |	\
	 SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S24_LE |	\
	 SNDRV_PCM_FMTBIT_S32_LE)

#define TOHDMITX_STREAM(xname, xsuffix, xfmt, xchmax)		\
{								\
	.stream_name	= xname " " xsuffix,			\
	.channels_min	= 1,					\
	.channels_max	= (xchmax),				\
	.rate_min       = 8000,					\
	.rate_max	= 192000,				\
	.formats	= (xfmt),				\
}

#define TOHDMITX_IN(xname, xid, xfmt, xchmax, xops) {			\
	.name = xname,							\
	.id = (xid),							\
	.playback = TOHDMITX_STREAM(xname, "Playback", xfmt, xchmax),	\
	.ops = (xops),							\
}

#define TOHDMITX_OUT(xname, xid, xfmt, xchmax) {			\
	.name = xname,							\
	.id = (xid),							\
	.capture = TOHDMITX_STREAM(xname, "Capture", xfmt, xchmax),	\
	.ops = &g12a_tohdmitx_output_ops,				\
}

static struct snd_soc_dai_driver g12a_tohdmitx_dai_drv[] = {
	TOHDMITX_IN("I2S IN A", TOHDMITX_I2S_IN_A,
		    TOHDMITX_I2S_FORMATS, 8, &g12a_tohdmitx_input_ops),
	TOHDMITX_IN("I2S IN B", TOHDMITX_I2S_IN_B,
		    TOHDMITX_I2S_FORMATS, 8, &g12a_tohdmitx_input_ops),
	TOHDMITX_IN("I2S IN C", TOHDMITX_I2S_IN_C,
		    TOHDMITX_I2S_FORMATS, 8, &g12a_tohdmitx_input_ops),
	TOHDMITX_OUT("I2S OUT", TOHDMITX_I2S_OUT,
		     TOHDMITX_I2S_FORMATS, 8),
	TOHDMITX_IN("SPDIF IN A", TOHDMITX_SPDIF_IN_A,
		    TOHDMITX_SPDIF_FORMATS, 2, &g12a_tohdmitx_input_ops),
	TOHDMITX_IN("SPDIF IN B", TOHDMITX_SPDIF_IN_B,
		    TOHDMITX_SPDIF_FORMATS, 2, &g12a_tohdmitx_input_ops),
	TOHDMITX_OUT("SPDIF OUT", TOHDMITX_SPDIF_OUT,
		     TOHDMITX_SPDIF_FORMATS, 2),
};

static struct snd_soc_dai_driver s7d_tohdmitx_dai_drv[] = {
	TOHDMITX_IN("I2S IN A", TOHDMITX_I2S_IN_A,
		    TOHDMITX_I2S_FORMATS, 8, &s7d_tohdmitx_i2s_input_ops),
	TOHDMITX_IN("I2S IN B", TOHDMITX_I2S_IN_B,
		    TOHDMITX_I2S_FORMATS, 8, &s7d_tohdmitx_i2s_input_ops),
	TOHDMITX_IN("I2S IN C", TOHDMITX_I2S_IN_C,
		    TOHDMITX_I2S_FORMATS, 8, &s7d_tohdmitx_i2s_input_ops),
	TOHDMITX_OUT("I2S OUT", TOHDMITX_I2S_OUT,
		     TOHDMITX_I2S_FORMATS, 8),
	TOHDMITX_IN("SPDIF IN A", TOHDMITX_SPDIF_IN_A,
		    TOHDMITX_SPDIF_FORMATS, 2, &g12a_tohdmitx_input_ops),
	TOHDMITX_IN("SPDIF IN B", TOHDMITX_SPDIF_IN_B,
		    TOHDMITX_SPDIF_FORMATS, 2, &g12a_tohdmitx_input_ops),
	TOHDMITX_OUT("SPDIF OUT", TOHDMITX_SPDIF_OUT,
		     TOHDMITX_SPDIF_FORMATS, 2),
};

static int g12a_tohdmi_component_probe(struct snd_soc_component *c)
{
	/* Initialize the static clock parameters */
	return snd_soc_component_write(c, TOHDMITX_CTRL0,
		     CTRL0_I2S_BLK_CAP_INV | CTRL0_SPDIF_CLK_CAP_INV);
}

static int s7d_tohdmi_component_probe(struct snd_soc_component *c)
{
	int ret;

	/*
	 * Initialize the static clock parameters. The register also holds
	 * the HDMI MCLK generator settings which are left untouched.
	 */
	ret = snd_soc_component_update_bits(c, TOHDMITX_CTRL0,
					    GENMASK(31, 28) | GENMASK(15, 0),
					    CTRL0_I2S_BLK_CAP_INV |
					    CTRL0_SPDIF_CLK_CAP_INV);

	return ret < 0 ? ret : 0;
}

static const struct snd_soc_dapm_route g12a_tohdmitx_routes[] = {
	{ "I2S SRC", "I2S A", "I2S IN A Playback" },
	{ "I2S SRC", "I2S B", "I2S IN B Playback" },
	{ "I2S SRC", "I2S C", "I2S IN C Playback" },
	{ "I2S OUT EN", "Switch", "I2S SRC" },
	{ "I2S OUT Capture", NULL, "I2S OUT EN" },
	{ "SPDIF SRC", "SPDIF A", "SPDIF IN A Playback" },
	{ "SPDIF SRC", "SPDIF B", "SPDIF IN B Playback" },
	{ "SPDIF OUT EN", "Switch", "SPDIF SRC" },
	{ "SPDIF OUT Capture", NULL, "SPDIF OUT EN" },
};

static const struct snd_soc_dapm_route s7d_tohdmitx_routes[] = {
	{ "I2S SRC", "I2S A", "I2S IN A Playback" },
	{ "I2S SRC", "I2S B", "I2S IN B Playback" },
	{ "I2S SRC", "I2S C", "I2S IN C Playback" },
	{ "I2S OUT EN", "Switch", "I2S SRC" },
	{ "I2S OUT EN", NULL, "I2S CLK EN" },
	{ "I2S OUT Capture", NULL, "I2S OUT EN" },
	{ "SPDIF SRC", "SPDIF A", "SPDIF IN A Playback" },
	{ "SPDIF SRC", "SPDIF B", "SPDIF IN B Playback" },
	{ "SPDIF OUT EN", "Switch", "SPDIF SRC" },
	{ "SPDIF OUT EN", NULL, "SPDIF CLK EN" },
	{ "SPDIF OUT Capture", NULL, "SPDIF OUT EN" },
};

static const struct snd_soc_component_driver g12a_tohdmitx_component_drv = {
	.probe			= g12a_tohdmi_component_probe,
	.dapm_widgets		= g12a_tohdmitx_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(g12a_tohdmitx_widgets),
	.dapm_routes		= g12a_tohdmitx_routes,
	.num_dapm_routes	= ARRAY_SIZE(g12a_tohdmitx_routes),
	.endianness		= 1,
};

static const struct snd_soc_component_driver s7d_tohdmitx_component_drv = {
	.probe			= s7d_tohdmi_component_probe,
	.dapm_widgets		= s7d_tohdmitx_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(s7d_tohdmitx_widgets),
	.dapm_routes		= s7d_tohdmitx_routes,
	.num_dapm_routes	= ARRAY_SIZE(s7d_tohdmitx_routes),
	.endianness		= 1,
};

static const struct regmap_config g12a_tohdmitx_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
};

static const struct g12a_tohdmitx_match_data g12a_tohdmitx_match_data = {
	.component_drv	= &g12a_tohdmitx_component_drv,
	.dai_drv	= g12a_tohdmitx_dai_drv,
	.num_dai_drv	= ARRAY_SIZE(g12a_tohdmitx_dai_drv),
};

static const struct g12a_tohdmitx_match_data s7d_tohdmitx_match_data = {
	.component_drv	= &s7d_tohdmitx_component_drv,
	.dai_drv	= s7d_tohdmitx_dai_drv,
	.num_dai_drv	= ARRAY_SIZE(s7d_tohdmitx_dai_drv),
	.has_mclk	= true,
};

static const struct of_device_id g12a_tohdmitx_of_match[] = {
	{
		.compatible = "amlogic,g12a-tohdmitx",
		.data = &g12a_tohdmitx_match_data,
	}, {
		.compatible = "amlogic,s7d-tohdmitx",
		.data = &s7d_tohdmitx_match_data,
	}, {}
};
MODULE_DEVICE_TABLE(of, g12a_tohdmitx_of_match);

static int g12a_tohdmitx_probe(struct platform_device *pdev)
{
	static const char * const mclk_names[TOHDMITX_I2S_IN_NUM] = {
		"mclk-a", "mclk-b", "mclk-c",
	};
	const struct g12a_tohdmitx_match_data *data;
	struct device *dev = &pdev->dev;
	struct g12a_tohdmitx *priv;
	void __iomem *regs;
	struct regmap *map;
	int i, ret;

	data = device_get_match_data(dev);
	if (!data) {
		dev_err(dev, "failed to match device\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	if (data->has_mclk) {
		for (i = 0; i < TOHDMITX_I2S_IN_NUM; i++) {
			priv->mclk[i] = devm_clk_get_optional(dev,
							      mclk_names[i]);
			if (IS_ERR(priv->mclk[i]))
				return dev_err_probe(dev,
						     PTR_ERR(priv->mclk[i]),
						     "failed to get %s\n",
						     mclk_names[i]);
		}
	}

	ret = device_reset(dev);
	if (ret)
		return ret;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	map = devm_regmap_init_mmio(dev, regs, &g12a_tohdmitx_regmap_cfg);
	if (IS_ERR(map)) {
		dev_err(dev, "failed to init regmap: %ld\n",
			PTR_ERR(map));
		return PTR_ERR(map);
	}

	return devm_snd_soc_register_component(dev,
			data->component_drv, data->dai_drv, data->num_dai_drv);
}

static struct platform_driver g12a_tohdmitx_pdrv = {
	.driver = {
		.name = G12A_TOHDMITX_DRV_NAME,
		.of_match_table = g12a_tohdmitx_of_match,
	},
	.probe = g12a_tohdmitx_probe,
};
module_platform_driver(g12a_tohdmitx_pdrv);

MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_DESCRIPTION("Amlogic G12a To HDMI Tx Control Codec Driver");
MODULE_LICENSE("GPL v2");
