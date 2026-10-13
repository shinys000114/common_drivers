// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>
#include <sound/hdmi-codec.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

struct c5_card {
	struct snd_soc_card card;
	struct snd_soc_dai_link links[3];
	struct snd_soc_dai_link_component cpus[3], codecs[3], platforms[3];
	struct snd_soc_jack jack;
	u8 eld[MAX_ELD_BYTES];
};

static const unsigned int c5_hdmi_rates[] = { 44100, 48000 };

static int c5_hdmi_format_rule(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct c5_card *priv = rule->private;
	const struct snd_interval *rate = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_mask allowed;
	struct cea_sad sad;
	unsigned int i, j;

	snd_mask_none(&allowed);
	for (i = 0; i < drm_eld_sad_count(priv->eld); i++) {
		if (drm_eld_sad_get(priv->eld, i, &sad) || sad.format != 1 || sad.channels < 1)
			continue;
		for (j = 0; j < ARRAY_SIZE(c5_hdmi_rates); j++) {
			if (!(sad.freq & BIT(j + 1)) ||
			    !snd_interval_test(rate, c5_hdmi_rates[j]))
				continue;
			if (sad.byte2 & BIT(0))
				snd_mask_set(&allowed, SNDRV_PCM_FORMAT_S16_LE);
			if (sad.byte2 & BIT(2))
				snd_mask_set(&allowed, SNDRV_PCM_FORMAT_S24_LE);
		}
	}
	return snd_mask_refine(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT), &allowed);
}

static int c5_hdmi_rate_rule(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct c5_card *priv = rule->private;
	const struct snd_mask *formats = hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_FORMAT);
	struct cea_sad sad;
	unsigned int i, mask = 0;
	u8 widths = 0;

	if (snd_mask_test(formats, SNDRV_PCM_FORMAT_S16_LE))
		widths |= BIT(0);
	if (snd_mask_test(formats, SNDRV_PCM_FORMAT_S24_LE))
		widths |= BIT(2);
	for (i = 0; i < drm_eld_sad_count(priv->eld); i++) {
		if (drm_eld_sad_get(priv->eld, i, &sad) || sad.format != 1 ||
		    sad.channels < 1 || !(sad.byte2 & widths))
			continue;
		mask |= (sad.freq >> 1) & 3;
	}
	if (!mask)
		return -EINVAL;
	return snd_interval_list(hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE),
				 ARRAY_SIZE(c5_hdmi_rates), c5_hdmi_rates, mask);
}

static int c5_hdmi_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct c5_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct device *dev = snd_soc_rtd_to_codec(rtd, 0)->dev;
	const struct hdmi_codec_pdata *data = dev_get_platdata(dev);
	int ret;

	if (!data || !data->ops || !data->ops->get_eld)
		return -ENODEV;
	ret = data->ops->get_eld(dev->parent, data->data, priv->eld, sizeof(priv->eld));
	if (ret)
		return ret;
	ret = snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_FORMAT,
				  c5_hdmi_format_rule, priv, SNDRV_PCM_HW_PARAM_RATE, -1);
	if (ret < 0)
		return ret;
	return snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
				   c5_hdmi_rate_rule, priv, SNDRV_PCM_HW_PARAM_FORMAT, -1);
}

static const struct snd_soc_ops c5_hdmi_ops = {
	.startup = c5_hdmi_startup,
};

static int c5_hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct c5_card *priv = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = snd_soc_card_jack_new(rtd->card, "HDMI Jack", SND_JACK_LINEOUT, &priv->jack);
	if (ret)
		return ret;
	return snd_soc_component_set_jack(snd_soc_rtd_to_codec(rtd, 0)->component,
					 &priv->jack, NULL);
}

static void c5_hdmi_exit(struct snd_soc_pcm_runtime *rtd)
{
	snd_soc_component_set_jack(snd_soc_rtd_to_codec(rtd, 0)->component, NULL, NULL);
}

static void c5_put_node(void *data)
{
	of_node_put(data);
}

static int c5_card_dai(struct device *dev, struct device_node *link, const char *name,
		       struct snd_soc_dai_link_component *dlc)
{
	struct device_node *node;
	struct of_phandle_args args;
	int ret;

	node = of_get_child_by_name(link, name);
	if (!node)
		return -EINVAL;
	ret = of_parse_phandle_with_args(node, "sound-dai", "#sound-dai-cells", 0, &args);
	of_node_put(node);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, c5_put_node, args.np);
	if (ret)
		return ret;
	dlc->of_node = args.np;
	return snd_soc_get_dai_name(&args, &dlc->dai_name);
}

static void c5_card_unlink_display(void *data)
{
	device_link_del(data);
}

static int c5_card_link_display(struct device *dev, struct device_node *hdmi)
{
	struct platform_device *display;
	struct device_node *node;
	struct device_link *link;

	node = of_graph_get_remote_node(hdmi, 0, 0);
	if (!node)
		return -EINVAL;
	display = of_find_device_by_node(node);
	of_node_put(node);
	if (!display)
		return -EPROBE_DEFER;
	/* The DRM resume callback restores the HDMI video clocks and PHY. */
	link = device_link_add(dev, &display->dev, DL_FLAG_STATELESS);
	put_device(&display->dev);
	if (!link)
		return -EINVAL;
	return devm_add_action_or_reset(dev, c5_card_unlink_display, link);
}

static const struct snd_soc_dapm_widget c5_card_widgets[] = {
	SND_SOC_DAPM_LINE("Analog Line Out", NULL),
};

static const struct snd_soc_dapm_route c5_card_routes[] = {
	{ "Analog Line Out", NULL, "Lineout Left" },
	{ "Analog Line Out", NULL, "Lineout Right" },
};

static int c5_card_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "hdmi", "analog", "spdif" };
	static const char * const labels[] = { "HDMI", "Analog", "SPDIF" };
	struct device *dev = &pdev->dev;
	struct device_node *link;
	struct c5_card *priv;
	unsigned int i, count = 0;
	bool analog = false;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		link = of_get_child_by_name(dev->of_node, names[i]);
		if (!link) {
			if (!i)
				return -EINVAL;
			continue;
		}
		if (!of_device_is_available(link)) {
			of_node_put(link);
			continue;
		}
		ret = c5_card_dai(dev, link, "cpu", &priv->cpus[count]);
		if (!ret)
			ret = c5_card_dai(dev, link, "codec", &priv->codecs[count]);
		of_node_put(link);
		if (ret)
			return dev_err_probe(dev, ret, "%s DAI link\n", names[i]);
		priv->platforms[count].of_node = priv->cpus[count].of_node;
		priv->links[count] = (struct snd_soc_dai_link) {
			.name = labels[i], .stream_name = labels[i],
			.cpus = &priv->cpus[count], .num_cpus = 1,
			.codecs = &priv->codecs[count], .num_codecs = 1,
			.platforms = &priv->platforms[count], .num_platforms = 1,
			.playback_only = 1,
			.ignore_pmdown_time = 1,
		};
		if (i < 2)
			priv->links[count].dai_fmt = SND_SOC_DAIFMT_I2S |
				SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
		if (!i) {
			ret = c5_card_link_display(dev, priv->codecs[count].of_node);
			if (ret)
				return dev_err_probe(dev, ret, "HDMI display dependency\n");
			priv->links[count].stream_name = "HDMI PCM";
			priv->links[count].init = c5_hdmi_init;
			priv->links[count].exit = c5_hdmi_exit;
			priv->links[count].ops = &c5_hdmi_ops;
		} else if (i == 1) {
			analog = true;
		}
		count++;
	}
	if (!count)
		return -EINVAL;
	priv->card = (struct snd_soc_card) {
		.owner = THIS_MODULE, .dev = dev, .name = "ODROID-C5",
		.dai_link = priv->links, .num_links = count,
	};
	if (analog) {
		priv->card.dapm_widgets = c5_card_widgets;
		priv->card.num_dapm_widgets = ARRAY_SIZE(c5_card_widgets);
		priv->card.dapm_routes = c5_card_routes;
		priv->card.num_dapm_routes = ARRAY_SIZE(c5_card_routes);
	}
	snd_soc_card_set_drvdata(&priv->card, priv);
	return devm_snd_soc_register_card(dev, &priv->card);
}

static const struct of_device_id c5_card_match[] = {
	{ .compatible = "hardkernel,odroidc5-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, c5_card_match);

static struct platform_driver c5_card_driver = {
	.probe = c5_card_probe,
	.driver = {
		.name = "odroidc5-audio",
		.of_match_table = c5_card_match,
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(c5_card_driver);

MODULE_DESCRIPTION("ODROID-C5 ASoC card");
MODULE_LICENSE("GPL");
