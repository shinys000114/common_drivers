// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/hdmi.h>
#include <linux/regmap.h>
#include <sound/hdmi-codec.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>

#include "s7d-hdmi-audio.h"
#include "s7d-hdmi-io.h"
#include "s7d-hdmi-output.h"

#define ACR_CTRL	0x0a01
#define AUDIO_EN	0x0a13
#define AIP_RESET	0x0a2c
#define PACKET_CTRL	0x0a2f
#define TPI_AUDIO	0x0a62
#define INFO_SELECT	0x06bf
#define INFO_DATA	0x06c0
#define INFO_ENABLE	0x06df
#define INFO_TRANSMIT	(BIT(7) | BIT(6))

/* Two-channel L-PCM rates: N comes from the 32/44.1/48 kHz base times a multiple. */
static const struct s7d_audio_rate {
	u32 rate;
	u8 base;	/* 0: 32 kHz, 1: 44.1 kHz, 2: 48 kHz */
	u8 multiple;
	u8 sad_freq;
} audio_rates[] = {
	{ 32000, 0, 1, BIT(0) },
	{ 44100, 1, 1, BIT(1) },
	{ 48000, 2, 1, BIT(2) },
	{ 88200, 1, 2, BIT(3) },
	{ 96000, 2, 2, BIT(4) },
	{ 176400, 1, 4, BIT(5) },
	{ 192000, 2, 4, BIT(6) },
};

static const struct s7d_audio_rate *audio_rate(unsigned int rate)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(audio_rates); i++)
		if (audio_rates[i].rate == rate)
			return &audio_rates[i];
	return NULL;
}

bool s7d_hdmi_audio_supported(const u8 *eld, const struct hdmi_codec_params *params)
{
	bool compressed = params->iec.status[0] & IEC958_AES0_NONAUDIO;
	const struct s7d_audio_rate *r = audio_rate(params->sample_rate);
	struct cea_sad sad;
	u8 rate, width;
	int i;

	if (params->channels != 2 || !r)
		return false;
	rate = r->sad_freq;
	if (params->sample_width == 16)
		width = BIT(0);
	else if (params->sample_width == 24 || params->sample_width == 32)
		width = BIT(2);	/* 32-bit samples carry 24 bits */
	else
		return false;
	for (i = 0; i < drm_eld_sad_count(eld); i++) {
		if (drm_eld_sad_get(eld, i, &sad))
			return false;
		/* IEC 61937 needs some compressed format at this frame rate. */
		if (compressed && sad.format != HDMI_AUDIO_CODING_TYPE_PCM &&
		    (sad.freq & rate))
			return true;
		if (!compressed && sad.format == HDMI_AUDIO_CODING_TYPE_PCM &&
		    sad.channels >= 1 && (sad.freq & rate) && (sad.byte2 & width))
			return true;
	}
	return false;
}

static int audio_write(struct regmap *core, u16 reg, u8 value)
{
	return s7d_hdmi_update_checked(core, reg, 0xff, value);
}

int s7d_hdmi_audio_mute(struct regmap *core, bool mute)
{
	int ret;

	ret = s7d_hdmi_update_checked(core, AUDIO_EN, BIT(0), mute ? 0 : BIT(0));
	if (ret)
		return ret;
	ret = s7d_hdmi_update_checked(core, PACKET_CTRL, BIT(7), mute ? BIT(7) : 0);
	if (ret)
		return ret;
	return s7d_hdmi_update_checked(core, TPI_AUDIO, BIT(4), mute ? BIT(4) : 0);
}

int s7d_hdmi_audio_disable(struct regmap *core)
{
	int ret;

	ret = audio_write(core, AUDIO_EN, 0);
	if (ret)
		return ret;
	ret = s7d_hdmi_update_checked(core, ACR_CTRL, BIT(1), 0);
	if (ret)
		return ret;
	ret = audio_write(core, INFO_SELECT, 2);
	if (ret)
		return ret;
	return s7d_hdmi_update_checked(core, INFO_ENABLE, INFO_TRANSMIT, 0);
}

/*
 * Selected S7D deep-color N values for 32/44.1/48 kHz, indexed by nominal pixel
 * kHz, not TMDS. The vendor looks 4:2:0 up at half the pixel rate and scales
 * the base N for 88.2-192 kHz.
 */
static unsigned int audio_n(unsigned int rate,
			    const struct s7d_hdmi_output_state *output)
{
	static const u16 base_n[] = { 4096, 6272, 6144 };
	static const struct s7d_audio_n {
		u32 pixel_khz;
		u16 n[3];
	} rgb10[] = {
		{ 25175, { 9152, 14014, 9152 } },
		{ 27027, { 8192, 12544, 8192 } },
		{ 54054, { 8192, 6272, 8192 } },
		{ 74176, { 11648, 17836, 11648 } },
		{ 74250, { 8192, 6272, 12288 } },
		{ 148352, { 11648, 17836, 11648 } },
		{ 296703, { 5824, 8918, 11648 } },
		{ 297000, { 6144, 4704, 5120 } },
	}, rgb12[] = {
		{ 25175, { 9152, 7007, 9152 } },
		{ 27027, { 8192, 6272, 8192 } },
		{ 74176, { 11648, 17836, 11648 } },
		{ 148352, { 11648, 17836, 11648 } },
		{ 296703, { 5824, 8918, 5824 } },
		{ 297000, { 4096, 4704, 5120 } },
	};
	const struct s7d_audio_rate *r = audio_rate(rate);
	const struct s7d_audio_n *table;
	unsigned long pixel_khz;
	unsigned int n, i, count;

	if (!output || !r)
		return 0;
	n = base_n[r->base] * r->multiple;
	switch (output->encoding) {
	case S7D_HDMI_OUTPUT_RGB8:
	case S7D_HDMI_OUTPUT_601_Y4448:
	case S7D_HDMI_OUTPUT_709_Y4448:
	case S7D_HDMI_OUTPUT_601_Y4228:
	case S7D_HDMI_OUTPUT_709_Y4228:
		/* Preserve the existing 8-bpc hardware-CTS profile. */
		return output->bpc == 8 && output->rate.ratio_q == 40 ? n : 0;
	case S7D_HDMI_OUTPUT_601_Y4208:
	case S7D_HDMI_OUTPUT_709_Y4208:
		return output->bpc == 8 && output->rate.ratio_q == 20 ? n : 0;
	case S7D_HDMI_OUTPUT_RGB10:
	case S7D_HDMI_OUTPUT_601_Y44410:
	case S7D_HDMI_OUTPUT_709_Y44410:
	case S7D_HDMI_OUTPUT_601_Y42010:
	case S7D_HDMI_OUTPUT_709_Y42010:
		if (output->bpc != 10 ||
		    output->rate.ratio_q != (s7d_hdmi_output_is_y420(output) ? 25 : 50))
			return 0;
		table = rgb10;
		count = ARRAY_SIZE(rgb10);
		break;
	case S7D_HDMI_OUTPUT_RGB12:
	case S7D_HDMI_OUTPUT_601_Y44412:
	case S7D_HDMI_OUTPUT_709_Y44412:
	case S7D_HDMI_OUTPUT_601_Y42012:
	case S7D_HDMI_OUTPUT_709_Y42012:
		if (output->bpc != 12 ||
		    output->rate.ratio_q != (s7d_hdmi_output_is_y420(output) ? 30 : 60))
			return 0;
		table = rgb12;
		count = ARRAY_SIZE(rgb12);
		break;
	default:
		return 0;
	}
	pixel_khz = output->rate.nominal_pixel / 1000 / output->pixel_divider;
	for (i = 0; i < count; i++) {
		if (pixel_khz >= table[i].pixel_khz - 1 &&
		    pixel_khz <= table[i].pixel_khz + 1)
			return table[i].n[r->base] * r->multiple;
	}
	return n;
}

int s7d_hdmi_audio_configure(struct regmap *core, struct regmap *top,
			     const struct hdmi_codec_params *params,
			     const struct s7d_hdmi_output_state *output, bool spdif)
{
	static const struct reg_sequence config[] = {
		{ 0x0a2d, 0 }, /* HDMI, no MHL. */
		{ 0x0a25, 0 }, /* No sample-rate conversion. */
		{ PACKET_CTRL, BIT(7) }, /* Layout 0, initially muted. */
		{ ACR_CTRL, 2 }, /* Hardware CTS from the external MCLK. */
		{ 0x0a02, 0 }, /* MCLK = 128 Fs. */
		{ 0x0a1c, 0xe4 }, /* Identity I2S lane mapping. */
		{ 0x0a1d, 0x20 },
		{ 0x0a26, 0x0b },
		{ 0x0a1b, 0 },
	};
	struct hdmi_audio_infoframe frame = params->cea;
	u8 info[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AUDIO_INFOFRAME_SIZE];
	unsigned int n, i;
	int ret;

	if (params->channels != 2 || params->cea.channels != 2 ||
	    params->cea.channel_allocation ||
	    (!spdif && (params->iec.status[0] & IEC958_AES0_NONAUDIO)) ||
	    (params->sample_width != 16 && params->sample_width != 24 &&
	     params->sample_width != 32))
		return -EINVAL;
	n = audio_n(params->sample_rate, output);
	if (!n)
		return -EINVAL;
	ret = hdmi_audio_infoframe_pack(&frame, info, sizeof(info));
	if (ret < 0)
		return ret;
	ret = s7d_hdmi_audio_disable(core);
	if (ret)
		return ret;
	ret = s7d_hdmi_update_checked(core, AIP_RESET, BIT(0), BIT(0));
	if (ret)
		goto fail;
	/* ACR measures the I2S MCLK or the SPDIF clock. */
	ret = s7d_hdmi_update_checked(top, 0x004, BIT(13), spdif ? BIT(13) : 0);
	if (ret)
		goto fail;
	for (i = 0; i < ARRAY_SIZE(config); i++) {
		ret = audio_write(core, config[i].reg, config[i].def);
		if (ret)
			goto fail;
	}
	/* I2S lane 0 or the SPDIF input; no HBR or DSD. */
	ret = audio_write(core, 0x0a14, spdif ? BIT(1) : BIT(4));
	if (ret)
		goto fail;
	ret = s7d_hdmi_update_checked(core, 0x0245, BIT(1), BIT(1));
	if (ret)
		goto fail;
	ret = s7d_hdmi_update_checked(core, 0x0a0e, BIT(4), 0);
	if (ret)
		goto fail;
	for (i = 0; i < 3; i++) {
		ret = audio_write(core, 0x0a03 + i, n >> (i * 8));
		if (ret)
			goto fail;
	}
	/* SPDIF carries its own channel status. */
	for (i = 0; !spdif && i < 5; i++) {
		ret = audio_write(core, 0x0a1e + i, params->iec.status[i]);
		if (ret)
			goto fail;
	}
	ret = audio_write(core, TPI_AUDIO,
			  (params->sample_width == 16 && !spdif ? 0x40 : 0xc0) | BIT(4));
	if (ret)
		goto fail;
	ret = audio_write(core, INFO_SELECT, 2);
	if (ret)
		goto fail;
	for (i = 0; i < 31; i++) {
		ret = audio_write(core, INFO_DATA + i, i < sizeof(info) ? info[i] : 0);
		if (ret)
			goto fail;
	}
	ret = s7d_hdmi_update_checked(core, INFO_ENABLE, INFO_TRANSMIT, INFO_TRANSMIT);
	if (ret)
		goto fail;
	ret = audio_write(core, AUDIO_EN, 2);
	if (ret)
		goto fail;
	ret = s7d_hdmi_update_checked(core, AIP_RESET, BIT(0), 0);
	if (!ret)
		return 0;
fail:
	s7d_hdmi_audio_disable(core);
	s7d_hdmi_update_checked(core, AIP_RESET, BIT(0), 0);
	return ret;
}
