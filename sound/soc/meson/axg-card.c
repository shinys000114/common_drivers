// SPDX-License-Identifier: (GPL-2.0 OR MIT)
//
// Copyright (c) 2018 BayLibre, SAS.
// Author: Jerome Brunet <jbrunet@baylibre.com>
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <drm/drm_eld.h>
#include <sound/control.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>
#include <sound/pcm_drm_eld.h>

#include "axg-tdm.h"
#include "meson-card.h"

struct axg_dai_link_tdm_mask {
	u32 tx;
	u32 rx;
};

struct axg_dai_link_tdm_data {
	unsigned int mclk_fs;
	unsigned int slots;
	unsigned int slot_width;
	u32 *tx_mask;
	u32 *rx_mask;
	struct axg_dai_link_tdm_mask *codec_masks;
};

/* A playback FE, which may play to the HDMI transmitter */
struct axg_card_fe {
	/* The sink's ELD; the FE's constraint rules read it again. */
	u8 eld[MESON_CARD_ELD_BYTES];
	/* Its channel maps, one per channel count, ended by an empty one */
	struct snd_pcm_chmap_elem chmaps[5];
};

/*
 * Base params for the codec to codec links
 * Those will be over-written by the CPU side of the link
 */
static const struct snd_soc_pcm_stream codec_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = 5525,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = 8,
};

static int axg_card_tdm_be_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct meson_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct axg_dai_link_tdm_data *be =
		(struct axg_dai_link_tdm_data *)priv->link_data[rtd->num];

	return meson_card_i2s_set_sysclk(substream, params, be->mclk_fs);
}

static const struct snd_soc_ops axg_card_tdm_be_ops = {
	.hw_params = axg_card_tdm_be_hw_params,
};

static int axg_card_tdm_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct meson_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct axg_dai_link_tdm_data *be =
		(struct axg_dai_link_tdm_data *)priv->link_data[rtd->num];
	struct snd_soc_dai *codec_dai;
	int ret, i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_set_tdm_slot(codec_dai,
					       be->codec_masks[i].tx,
					       be->codec_masks[i].rx,
					       be->slots, be->slot_width);
		if (ret && ret != -ENOTSUPP) {
			dev_err(codec_dai->dev,
				"setting tdm link slots failed\n");
			return ret;
		}
	}

	ret = axg_tdm_set_tdm_slots(snd_soc_rtd_to_cpu(rtd, 0), be->tx_mask, be->rx_mask,
				    be->slots, be->slot_width);
	if (ret) {
		dev_err(snd_soc_rtd_to_cpu(rtd, 0)->dev, "setting tdm link slots failed\n");
		return ret;
	}

	return 0;
}

static int axg_card_tdm_dai_lb_init(struct snd_soc_pcm_runtime *rtd)
{
	struct meson_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct axg_dai_link_tdm_data *be =
		(struct axg_dai_link_tdm_data *)priv->link_data[rtd->num];
	int ret;

	/* The loopback rx_mask is the pad tx_mask */
	ret = axg_tdm_set_tdm_slots(snd_soc_rtd_to_cpu(rtd, 0), NULL, be->tx_mask,
				    be->slots, be->slot_width);
	if (ret) {
		dev_err(snd_soc_rtd_to_cpu(rtd, 0)->dev, "setting tdm link slots failed\n");
		return ret;
	}

	return 0;
}

static int axg_card_add_tdm_loopback(struct snd_soc_card *card,
				     int *index)
{
	struct meson_card *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai_link *pad;
	struct snd_soc_dai_link *lb;
	struct snd_soc_dai_link_component *dlc;
	int ret;

	/* extend links */
	ret = meson_card_reallocate_links(card, card->num_links + 1);
	if (ret)
		return ret;

	pad = &card->dai_link[*index];
	lb = &card->dai_link[*index + 1];

	lb->name = devm_kasprintf(card->dev, GFP_KERNEL, "%s-lb", pad->name);
	if (!lb->name)
		return -ENOMEM;

	dlc = devm_kzalloc(card->dev, sizeof(*dlc), GFP_KERNEL);
	if (!dlc)
		return -ENOMEM;

	lb->cpus = dlc;
	lb->codecs = &snd_soc_dummy_dlc;
	lb->num_cpus = 1;
	lb->num_codecs = 1;

	lb->stream_name = lb->name;
	lb->cpus->of_node = pad->cpus->of_node;
	lb->cpus->dai_name = "TDM Loopback";
	lb->capture_only = 1;
	lb->no_pcm = 1;
	lb->ops = &axg_card_tdm_be_ops;
	lb->init = axg_card_tdm_dai_lb_init;

	/* Provide the same link data to the loopback */
	priv->link_data[*index + 1] = priv->link_data[*index];

	/*
	 * axg_card_clean_references() will iterate over this link,
	 * make sure the node count is balanced
	 */
	of_node_get(lb->cpus->of_node);

	/* Let add_links continue where it should */
	*index += 1;

	return 0;
}

static int axg_card_parse_cpu_tdm_slots(struct snd_soc_card *card,
					struct snd_soc_dai_link *link,
					struct device_node *node,
					struct axg_dai_link_tdm_data *be)
{
	char propname[32];
	u32 tx, rx;
	int i;

	be->tx_mask = devm_kcalloc(card->dev, AXG_TDM_NUM_LANES,
				   sizeof(*be->tx_mask), GFP_KERNEL);
	be->rx_mask = devm_kcalloc(card->dev, AXG_TDM_NUM_LANES,
				   sizeof(*be->rx_mask), GFP_KERNEL);
	if (!be->tx_mask || !be->rx_mask)
		return -ENOMEM;

	for (i = 0, tx = 0; i < AXG_TDM_NUM_LANES; i++) {
		snprintf(propname, 32, "dai-tdm-slot-tx-mask-%d", i);
		snd_soc_of_get_slot_mask(node, propname, &be->tx_mask[i]);
		tx = max(tx, be->tx_mask[i]);
	}

	/* Disable playback is the interface has no tx slots */
	if (!tx)
		link->capture_only = 1;

	for (i = 0, rx = 0; i < AXG_TDM_NUM_LANES; i++) {
		snprintf(propname, 32, "dai-tdm-slot-rx-mask-%d", i);
		snd_soc_of_get_slot_mask(node, propname, &be->rx_mask[i]);
		rx = max(rx, be->rx_mask[i]);
	}

	/* Disable capture is the interface has no rx slots */
	if (!rx)
		link->playback_only = 1;

	/* ... but the interface should at least have one direction */
	if (!tx && !rx) {
		dev_err(card->dev, "tdm link has no cpu slots\n");
		return -EINVAL;
	}

	of_property_read_u32(node, "dai-tdm-slot-num", &be->slots);
	if (!be->slots) {
		/*
		 * If the slot number is not provided, set it such as it
		 * accommodates the largest mask
		 */
		be->slots = fls(max(tx, rx));
	} else if (be->slots < fls(max(tx, rx)) || be->slots > 32) {
		/*
		 * Error if the slots can't accommodate the largest mask or
		 * if it is just too big
		 */
		dev_err(card->dev, "bad slot number\n");
		return -EINVAL;
	}

	of_property_read_u32(node, "dai-tdm-slot-width", &be->slot_width);

	return 0;
}

static int axg_card_parse_codecs_masks(struct snd_soc_card *card,
				       struct snd_soc_dai_link *link,
				       struct device_node *node,
				       struct axg_dai_link_tdm_data *be)
{
	struct axg_dai_link_tdm_mask *codec_mask;
	struct device_node *np;

	codec_mask = devm_kcalloc(card->dev, link->num_codecs,
				  sizeof(*codec_mask), GFP_KERNEL);
	if (!codec_mask)
		return -ENOMEM;

	be->codec_masks = codec_mask;

	for_each_child_of_node(node, np) {
		snd_soc_of_get_slot_mask(np, "dai-tdm-slot-rx-mask",
					 &codec_mask->rx);
		snd_soc_of_get_slot_mask(np, "dai-tdm-slot-tx-mask",
					 &codec_mask->tx);

		codec_mask++;
	}

	return 0;
}

static int axg_card_parse_tdm(struct snd_soc_card *card,
			      struct device_node *node,
			      int *index)
{
	struct meson_card *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai_link *link = &card->dai_link[*index];
	struct axg_dai_link_tdm_data *be;
	int ret;

	/* Allocate tdm link parameters */
	be = devm_kzalloc(card->dev, sizeof(*be), GFP_KERNEL);
	if (!be)
		return -ENOMEM;
	priv->link_data[*index] = be;

	/* Setup tdm link */
	link->ops = &axg_card_tdm_be_ops;
	link->init = axg_card_tdm_dai_init;
	link->dai_fmt = meson_card_parse_daifmt(node, link->cpus->of_node);

	of_property_read_u32(node, "mclk-fs", &be->mclk_fs);

	ret = axg_card_parse_cpu_tdm_slots(card, link, node, be);
	if (ret) {
		dev_err(card->dev, "error parsing tdm link slots\n");
		return ret;
	}

	ret = axg_card_parse_codecs_masks(card, link, node, be);
	if (ret)
		return ret;

	/* Add loopback if the pad dai has playback */
	if (!link->capture_only) {
		ret = axg_card_add_tdm_loopback(card, index);
		if (ret)
			return ret;
	}

	return 0;
}

static int axg_card_cpu_is_capture_fe(struct device_node *np)
{
	return of_device_is_compatible(np, DT_PREFIX "axg-toddr") ||
		of_device_is_compatible(np, DT_PREFIX "s7d-toddr");
}

static int axg_card_cpu_is_playback_fe(struct device_node *np)
{
	return of_device_is_compatible(np, DT_PREFIX "axg-frddr") ||
		of_device_is_compatible(np, DT_PREFIX "s7d-frddr");
}

/*
 * A PCM control of a codec DAI, which hdmi-codec adds for its link's PCM.
 * The FEs, whose codec is the dummy, carry the card's forwarded copies.
 */
static struct snd_kcontrol *axg_card_codec_ctl(struct snd_soc_card *card,
					       struct snd_soc_dai *dai,
					       const char *name)
{
	struct snd_ctl_elem_id id = {
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	};
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec;
	int i;

	strscpy(id.name, name, sizeof(id.name));
	for_each_card_rtds(card, rtd) {
		if (!rtd->pcm || rtd->dai_link->dynamic)
			continue;
		for_each_rtd_codec_dais(rtd, i, codec) {
			if (codec != dai)
				continue;
			id.device = rtd->pcm->device;
			return snd_ctl_find_id(card->snd_card, &id);
		}
	}
	return NULL;
}

static struct snd_kcontrol *axg_card_eld_ctl(struct snd_soc_card *card,
					     struct snd_soc_dai *dai)
{
	return axg_card_codec_ctl(card, dai, "ELD");
}

#define AXG_CARD_CHMAP4(a, b, c, d) \
	{ SNDRV_CHMAP_##a, SNDRV_CHMAP_##b, SNDRV_CHMAP_##c, SNDRV_CHMAP_##d }

/*
 * The speakers of HDMI audio slots 0-7 for a CEA-861 channel allocation:
 * FL and FR, LFE and FC by bits 0 and 1, then slots 4-7 by bits 2-4.
 */
static void axg_card_hdmi_ca_map(u8 ca, unsigned char *map)
{
	static const unsigned char rear[8][4] = {
		AXG_CARD_CHMAP4(NA, NA, NA, NA),
		AXG_CARD_CHMAP4(RC, NA, NA, NA),
		AXG_CARD_CHMAP4(RL, RR, NA, NA),
		AXG_CARD_CHMAP4(RL, RR, RC, NA),
		AXG_CARD_CHMAP4(RL, RR, RLC, RRC),
		AXG_CARD_CHMAP4(NA, NA, FLC, FRC),
		AXG_CARD_CHMAP4(RC, NA, FLC, FRC),
		AXG_CARD_CHMAP4(RL, RR, FLC, FRC),
	};

	map[0] = SNDRV_CHMAP_FL;
	map[1] = SNDRV_CHMAP_FR;
	map[2] = ca & BIT(0) ? SNDRV_CHMAP_LFE : SNDRV_CHMAP_NA;
	map[3] = ca & BIT(1) ? SNDRV_CHMAP_FC : SNDRV_CHMAP_NA;
	memcpy(&map[4], rear[ca >> 2], sizeof(rear[0]));
}

/* The speakers an ELD speaker allocation lists, as SNDRV_CHMAP_* bits */
static unsigned int axg_card_hdmi_spk_mask(u8 spk_alloc)
{
	static const unsigned int spk[] = {
		BIT(SNDRV_CHMAP_FL) | BIT(SNDRV_CHMAP_FR),
		BIT(SNDRV_CHMAP_LFE),
		BIT(SNDRV_CHMAP_FC),
		BIT(SNDRV_CHMAP_RL) | BIT(SNDRV_CHMAP_RR),
		BIT(SNDRV_CHMAP_RC),
		BIT(SNDRV_CHMAP_FLC) | BIT(SNDRV_CHMAP_FRC),
		BIT(SNDRV_CHMAP_RLC) | BIT(SNDRV_CHMAP_RRC),
	};
	unsigned int mask = BIT(SNDRV_CHMAP_NA), i;

	for (i = 0; i < ARRAY_SIZE(spk); i++)
		if (spk_alloc & BIT(i))
			mask |= spk[i];
	return mask;
}

/*
 * The channel allocation hdmi-codec sends for a channel count: stereo
 * when the sink lists no speakers, else the first of these, in
 * hdmi_codec_channel_alloc[] order, whose speakers the sink has.
 */
static int axg_card_hdmi_ca(u8 spk_alloc, unsigned int channels)
{
	static const u8 cas[][2] = {
		{ 2, 0x00 },
		{ 4, 0x03 }, { 4, 0x02 }, { 4, 0x01 },
		{ 6, 0x0b }, { 6, 0x0a }, { 6, 0x09 }, { 6, 0x08 },
		{ 6, 0x07 }, { 6, 0x06 }, { 6, 0x05 }, { 6, 0x04 },
		{ 8, 0x13 }, { 8, 0x1f }, { 8, 0x12 }, { 8, 0x1e },
		{ 8, 0x11 }, { 8, 0x1d }, { 8, 0x10 }, { 8, 0x1c },
		{ 8, 0x0f }, { 8, 0x1b }, { 8, 0x0e }, { 8, 0x1a },
		{ 8, 0x0d }, { 8, 0x19 }, { 8, 0x0c }, { 8, 0x18 },
		{ 8, 0x17 }, { 8, 0x16 }, { 8, 0x15 }, { 8, 0x14 },
		{ 8, 0x0b }, { 8, 0x0a }, { 8, 0x09 }, { 8, 0x08 },
		{ 8, 0x07 }, { 8, 0x06 }, { 8, 0x05 }, { 8, 0x04 },
		{ 8, 0x03 }, { 8, 0x02 }, { 8, 0x01 },
	};
	unsigned int sink = axg_card_hdmi_spk_mask(spk_alloc), mask, i, j;
	unsigned char map[8];

	if (!spk_alloc)
		return 0;
	for (i = 0; i < ARRAY_SIZE(cas); i++) {
		if (cas[i][0] != channels)
			continue;
		axg_card_hdmi_ca_map(cas[i][1], map);
		for (mask = 0, j = 0; j < ARRAY_SIZE(map); j++)
			mask |= BIT(map[j]);
		if (!(mask & ~sink))
			return cas[i][1];
	}
	return -EINVAL;
}

/*
 * The FE's channel maps for the sink in its ELD: the slot order of what
 * hdmi-codec sends at each channel count it can, stereo only when the
 * sink lists no speakers, as hdmi-codec's own map does.
 */
static void axg_card_fe_set_chmaps(struct axg_card_fe *fe,
				   unsigned int max_channels)
{
	struct snd_pcm_chmap_elem chmaps[ARRAY_SIZE(fe->chmaps) - 1] = {};
	u8 spk_alloc = drm_eld_get_spk_alloc(fe->eld);
	unsigned int ch, n = 0;
	int ca;

	if (!spk_alloc)
		max_channels = 2;
	for (ch = 2; ch <= max_channels && n < ARRAY_SIZE(chmaps); ch += 2) {
		ca = axg_card_hdmi_ca(spk_alloc, ch);
		if (ca < 0)
			continue;
		chmaps[n].channels = ch;
		axg_card_hdmi_ca_map(ca, chmaps[n].map);
		n++;
	}
	/* The last entry stays empty, so readers always find the end. */
	memcpy(fe->chmaps, chmaps, sizeof(chmaps));
}

/*
 * The HDMI codec sits behind a codec-to-codec link, which opens it from a
 * DAPM event: its constraints never reach the application's stream, and a
 * channel count or rate the codec or sink lacks then fails there unseen
 * while the stream plays silence. Apply them to the playback FE instead.
 */
static int axg_card_fe_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct meson_card *priv = snd_soc_card_get_drvdata(rtd->card);
	const struct snd_soc_pcm_stream *stream;
	struct axg_card_fe *fe = priv->link_data[rtd->num];
	struct snd_soc_dapm_widget_list *list;
	struct snd_soc_dapm_widget *w;
	struct snd_ctl_elem_value *value;
	struct snd_kcontrol *kctl = NULL;
	struct snd_soc_dai *dai = NULL;
	int i, ret;

	if (snd_soc_dapm_dai_get_connected_widgets(snd_soc_rtd_to_cpu(rtd, 0),
						   substream->stream, &list, NULL) < 0)
		return 0;
	for_each_dapm_widgets(list, i, w) {
		if (w->id != snd_soc_dapm_dai_in || !w->priv)
			continue;
		kctl = axg_card_eld_ctl(rtd->card, w->priv);
		if (kctl) {
			dai = w->priv;
			break;
		}
	}
	snd_soc_dapm_dai_free_widgets(&list);
	if (!kctl)
		return 0;

	/* hdmi-codec takes two channels at least, and up to its DAI's maximum. */
	stream = snd_soc_dai_get_pcm_stream(dai, SNDRV_PCM_STREAM_PLAYBACK);
	ret = snd_pcm_hw_constraint_minmax(substream->runtime,
					   SNDRV_PCM_HW_PARAM_CHANNELS,
					   stream->channels_min,
					   stream->channels_max);
	if (ret < 0)
		return ret;

	value = kzalloc(sizeof(*value), GFP_KERNEL);
	if (!value)
		return -ENOMEM;
	/*
	 * Not under controls_rwsem: a DAPM route write holds it while it
	 * waits for the pcm_mutex this open holds. hdmi-codec's get only
	 * copies its ELD.
	 */
	ret = kctl->get(kctl, value);
	if (!ret) {
		memcpy(fe->eld, value->value.bytes.data, MESON_CARD_ELD_BYTES);
		axg_card_fe_set_chmaps(fe, stream->channels_max);
	}
	kfree(value);
	/* No sink audio data (unplugged, DVI): leave the rest unconstrained. */
	if (ret || !drm_eld_sad_count(fe->eld))
		return 0;
	/* The rules read the ELD again at each refinement of this stream. */
	return snd_pcm_hw_constraint_eld(substream->runtime, fe->eld);
}

static const struct snd_soc_ops axg_card_fe_ops = {
	.startup = axg_card_fe_startup,
};

#define AXG_CARD_IEC958_DEFAULT SNDRV_CTL_NAME_IEC958("", PLAYBACK, DEFAULT)
#define AXG_CARD_IEC958_MASK SNDRV_CTL_NAME_IEC958("", PLAYBACK, MASK)

static bool axg_card_is_hdmi_dai(struct snd_soc_card *card,
				 struct snd_soc_dai *dai)
{
	return axg_card_eld_ctl(card, dai);
}

/* A PCM control a component adds under its own name, for no PCM. */
static struct snd_kcontrol *
axg_card_component_ctl(struct snd_soc_card *card,
		       struct snd_soc_component *component, const char *name)
{
	struct snd_ctl_elem_id id = {
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	};

	if (!component->name_prefix)
		return NULL;
	snprintf(id.name, sizeof(id.name), "%s %s", component->name_prefix,
		 name);
	return snd_ctl_find_id(card->snd_card, &id);
}

/* SPDIF out, which sends the channel status it is given itself */
static bool axg_card_is_spdif_dai(struct snd_soc_card *card,
				  struct snd_soc_dai *dai)
{
	return axg_card_component_ctl(card, dai->component,
				      AXG_CARD_IEC958_DEFAULT);
}

/*
 * The first DAI that @match accepts which a playback widget is wired to
 * within @depth widgets, whatever the mixer settings: the card binds
 * before they are applied.
 */
static struct snd_soc_dai *
axg_card_wired_dai(struct snd_soc_card *card, struct snd_soc_dapm_widget *w,
		   unsigned int depth,
		   bool (*match)(struct snd_soc_card *card,
				 struct snd_soc_dai *dai))
{
	struct snd_soc_dapm_path *p;
	struct snd_soc_dai *dai;

	if (w->id == snd_soc_dapm_dai_in && w->priv && match(card, w->priv))
		return w->priv;
	if (!depth)
		return NULL;

	snd_soc_dapm_widget_for_each_sink_path(w, p) {
		dai = axg_card_wired_dai(card, p->sink, depth - 1, match);
		if (dai)
			return dai;
	}
	return NULL;
}

/*
 * A forwarded control reads and writes the control in private_value, and
 * writes the one in private_data, if any, as well.
 */
static int axg_card_fwd_info(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_info *uinfo)
{
	struct snd_kcontrol *kctl = (struct snd_kcontrol *)kcontrol->private_value;

	return kctl->info(kctl, uinfo);
}

static int axg_card_fwd_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_kcontrol *kctl = (struct snd_kcontrol *)kcontrol->private_value;

	return kctl->get(kctl, ucontrol);
}

static int axg_card_fwd_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_kcontrol *kctl = (struct snd_kcontrol *)kcontrol->private_value;
	struct snd_kcontrol *also = snd_kcontrol_chip(kcontrol);
	int changed, ret;

	changed = kctl->put(kctl, ucontrol);
	if (changed < 0 || !also)
		return changed;
	ret = also->put(also, ucontrol);
	return ret < 0 ? ret : changed;
}

static int axg_card_add_fwd_ctl(struct snd_soc_card *card,
				struct snd_soc_pcm_runtime *rtd,
				const char *name, struct snd_kcontrol *kctl,
				struct snd_kcontrol *also)
{
	struct snd_kcontrol_new tmpl = {
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.device = rtd->pcm->device,
		.name = name,
		.info = axg_card_fwd_info,
		.get = axg_card_fwd_get,
	};

	if (!kctl)
		return 0;
	tmpl.access = kctl->vd[0].access &
		(SNDRV_CTL_ELEM_ACCESS_READWRITE |
		 SNDRV_CTL_ELEM_ACCESS_VOLATILE);
	tmpl.put = kctl->put ? axg_card_fwd_put : NULL;
	tmpl.private_value = (unsigned long)kctl;
	kctl = snd_ctl_new1(&tmpl, also && also->put ? also : NULL);
	if (!kctl)
		return -ENOMEM;
	return snd_ctl_add(card->snd_card, kctl);
}

/*
 * hdmi-codec's channel map is on the internal PCM as well, and there is
 * one per component: the SPDIF DAI's, made last, replaces the I2S DAI's.
 * Give the FE its own, which each open sets for the sink.
 */
static int axg_card_add_fe_chmap(struct snd_soc_card *card,
				 struct snd_soc_pcm_runtime *rtd,
				 struct snd_soc_dai *hdmi)
{
	struct meson_card *priv = snd_soc_card_get_drvdata(card);
	struct axg_card_fe *fe = priv->link_data[rtd->num];
	const struct snd_soc_pcm_stream *stream;

	stream = snd_soc_dai_get_pcm_stream(hdmi, SNDRV_PCM_STREAM_PLAYBACK);
	axg_card_fe_set_chmaps(fe, stream->channels_max);
	return snd_pcm_add_chmap_ctls(rtd->pcm, SNDRV_PCM_STREAM_PLAYBACK,
				      fe->chmaps, stream->channels_max, 0,
				      NULL);
}

/*
 * hdmi-codec adds its ELD and IEC958 controls for the internal PCM of the
 * codec-to-codec link, and SPDIF out adds its IEC958 controls under its
 * own name, so nobody finds them by the device they open. Give each
 * playback FE controls of the usual names that forward to those of the
 * HDMI codec or SPDIF out it is wired to.
 */
static int axg_card_add_fe_ctls(struct snd_soc_card *card,
				struct snd_soc_pcm_runtime *rtd)
{
	struct snd_kcontrol *dflt = NULL, *mask = NULL, *also = NULL;
	struct snd_soc_dapm_widget *w;
	struct snd_soc_dai *hdmi, *spdif;
	int ret;

	if (!rtd->dai_link->dynamic)
		return 0;
	w = snd_soc_dai_get_widget_playback(snd_soc_rtd_to_cpu(rtd, 0));
	if (!w)
		return 0;

	hdmi = axg_card_wired_dai(card, w, 32, axg_card_is_hdmi_dai);
	if (hdmi) {
		ret = axg_card_add_fwd_ctl(card, rtd, "ELD",
					   axg_card_eld_ctl(card, hdmi), NULL);
		if (ret)
			return ret;
		ret = axg_card_add_fe_chmap(card, rtd, hdmi);
		if (ret)
			return ret;
		dflt = axg_card_codec_ctl(card, hdmi, AXG_CARD_IEC958_DEFAULT);
		mask = axg_card_codec_ctl(card, hdmi, AXG_CARD_IEC958_MASK);
	}
	/*
	 * The channel status on the wire is SPDIF out's own. hdmi-codec gets
	 * a copy, as it sets up the InfoFrame and the transmitter from it.
	 */
	spdif = axg_card_wired_dai(card, w, 32, axg_card_is_spdif_dai);
	if (spdif) {
		also = dflt;
		dflt = axg_card_component_ctl(card, spdif->component,
					      AXG_CARD_IEC958_DEFAULT);
		mask = axg_card_component_ctl(card, spdif->component,
					      AXG_CARD_IEC958_MASK);
	}
	ret = axg_card_add_fwd_ctl(card, rtd, AXG_CARD_IEC958_DEFAULT, dflt,
				   also);
	if (ret)
		return ret;
	return axg_card_add_fwd_ctl(card, rtd, AXG_CARD_IEC958_MASK, mask,
				    NULL);
}

static void axg_card_hdmi_jack_free(struct snd_jack *jack)
{
	snd_soc_component_set_jack(jack->private_data, NULL, NULL);
}

/* Report whether the HDMI sink takes audio, as hdmi-codec learns it. */
static int axg_card_add_hdmi_jack(struct snd_soc_card *card,
				  struct snd_soc_component *component)
{
	struct meson_card *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_jack *jack = &priv->hdmi_jack;
	int ret;

	ret = snd_soc_card_jack_new(card, "HDMI Jack", SND_JACK_LINEOUT, jack);
	/* Without CONFIG_SND_JACK there is no jack to report to. */
	if (ret || !jack->jack)
		return ret;
	jack->jack->private_data = component;
	jack->jack->private_free = axg_card_hdmi_jack_free;

	ret = snd_soc_component_set_jack(component, jack, NULL);
	return ret == -ENOTSUPP ? 0 : ret;
}

static int axg_card_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *dai;
	int i, ret;

	for_each_card_rtds(card, rtd) {
		ret = axg_card_add_fe_ctls(card, rtd);
		if (ret)
			return ret;
	}

	/* One HDMI transmitter: its I2S and SPDIF DAIs share a component. */
	for_each_card_rtds(card, rtd) {
		for_each_rtd_codec_dais(rtd, i, dai) {
			if (axg_card_eld_ctl(card, dai))
				return axg_card_add_hdmi_jack(card, dai->component);
		}
	}
	return 0;
}

static int axg_card_cpu_is_tdm_iface(struct device_node *np)
{
	return of_device_is_compatible(np, DT_PREFIX "axg-tdm-iface");
}

static int axg_card_cpu_is_codec(struct device_node *np)
{
	return of_device_is_compatible(np, DT_PREFIX "g12a-tohdmitx") ||
		of_device_is_compatible(np, DT_PREFIX "g12a-toacodec") ||
		of_device_is_compatible(np, DT_PREFIX "s7d-tohdmitx") ||
		of_device_is_compatible(np, DT_PREFIX "s7d-toacodec");
}

static int axg_card_add_link(struct snd_soc_card *card, struct device_node *np,
			     int *index)
{
	struct meson_card *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai_link *dai_link = &card->dai_link[*index];
	struct snd_soc_dai_link_component *cpu;
	struct axg_card_fe *fe;
	int ret;

	cpu = devm_kzalloc(card->dev, sizeof(*cpu), GFP_KERNEL);
	if (!cpu)
		return -ENOMEM;

	dai_link->cpus = cpu;
	dai_link->num_cpus = 1;
	dai_link->nonatomic = true;

	ret = meson_card_parse_dai(card, np, dai_link->cpus);
	if (ret)
		return ret;

	if (axg_card_cpu_is_playback_fe(dai_link->cpus->of_node)) {
		fe = devm_kzalloc(card->dev, sizeof(*fe), GFP_KERNEL);
		if (!fe)
			return -ENOMEM;
		priv->link_data[*index] = fe;
		dai_link->ops = &axg_card_fe_ops;
		return meson_card_set_fe_link(card, dai_link, np, true);
	}
	else if (axg_card_cpu_is_capture_fe(dai_link->cpus->of_node))
		return meson_card_set_fe_link(card, dai_link, np, false);


	ret = meson_card_set_be_link(card, dai_link, np);
	if (ret)
		return ret;

	if (axg_card_cpu_is_codec(dai_link->cpus->of_node)) {
		dai_link->c2c_params = &codec_params;
		dai_link->num_c2c_params = 1;
	} else {
		dai_link->no_pcm = 1;
		if (axg_card_cpu_is_tdm_iface(dai_link->cpus->of_node))
			ret = axg_card_parse_tdm(card, np, index);
	}

	return ret;
}

static const struct meson_card_match_data axg_card_match_data = {
	.add_link = axg_card_add_link,
	.late_probe = axg_card_late_probe,
};

static const struct of_device_id axg_card_of_match[] = {
	{
		.compatible = "amlogic,axg-sound-card",
		.data = &axg_card_match_data,
	}, {
		.compatible = "amlogic,s7d-sound-card",
		.data = &axg_card_match_data,
	}, {}
};
MODULE_DEVICE_TABLE(of, axg_card_of_match);

static struct platform_driver axg_card_pdrv = {
	.probe = meson_card_probe,
	.remove = meson_card_remove,
	.driver = {
		.name = "axg-sound-card",
		.of_match_table = axg_card_of_match,
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(axg_card_pdrv);

MODULE_DESCRIPTION("Amlogic AXG ALSA machine driver");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL v2");
