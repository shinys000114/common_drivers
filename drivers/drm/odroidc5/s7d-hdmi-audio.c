// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/hdmi.h>
#include <linux/regmap.h>
#include <sound/hdmi-codec.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>

#include "s7d-hdmi-audio.h"
#include "s7d-hdmi-io.h"

#define ACR_CTRL	0x0a01
#define AUDIO_EN	0x0a13
#define AIP_RESET	0x0a2c
#define PACKET_CTRL	0x0a2f
#define TPI_AUDIO	0x0a62
#define INFO_SELECT	0x06bf
#define INFO_DATA	0x06c0
#define INFO_ENABLE	0x06df
#define INFO_TRANSMIT	(BIT(7) | BIT(6))

bool s7d_hdmi_audio_supported(const u8 *eld, const struct hdmi_codec_params *params)
{
	struct cea_sad sad;
	u8 rate, width;
	int i;

	if (params->channels != 2)
		return false;
	if (params->sample_rate == 44100)
		rate = BIT(1);
	else if (params->sample_rate == 48000)
		rate = BIT(2);
	else
		return false;
	if (params->sample_width == 16)
		width = BIT(0);
	else if (params->sample_width == 24)
		width = BIT(2);
	else
		return false;
	for (i = 0; i < drm_eld_sad_count(eld); i++) {
		if (drm_eld_sad_get(eld, i, &sad))
			return false;
		if (sad.format == HDMI_AUDIO_CODING_TYPE_PCM && sad.channels >= 1 &&
		    (sad.freq & rate) && (sad.byte2 & width))
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

int s7d_hdmi_audio_configure(struct regmap *core, struct regmap *top,
			     const struct hdmi_codec_params *params)
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
		{ 0x0a14, 0x10 }, /* I2S lane 0, PCM, no SPDIF/HBR/DSD. */
	};
	struct hdmi_audio_infoframe frame = params->cea;
	u8 info[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AUDIO_INFOFRAME_SIZE];
	unsigned int n, i;
	int ret;

	if (params->channels != 2 || params->cea.channels != 2 ||
	    params->cea.channel_allocation ||
	    (params->iec.status[0] & IEC958_AES0_NONAUDIO) ||
	    (params->sample_width != 16 && params->sample_width != 24))
		return -EINVAL;
	switch (params->sample_rate) {
	case 44100:
		n = 6272;
		break;
	case 48000:
		n = 6144;
		break;
	default:
		return -EINVAL;
	}
	ret = hdmi_audio_infoframe_pack(&frame, info, sizeof(info));
	if (ret < 0)
		return ret;
	ret = s7d_hdmi_audio_disable(core);
	if (ret)
		return ret;
	ret = s7d_hdmi_update_checked(core, AIP_RESET, BIT(0), BIT(0));
	if (ret)
		goto fail;
	ret = s7d_hdmi_update_checked(top, 0x004, BIT(13), 0);
	if (ret)
		goto fail;
	for (i = 0; i < ARRAY_SIZE(config); i++) {
		ret = audio_write(core, config[i].reg, config[i].def);
		if (ret)
			goto fail;
	}
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
	for (i = 0; i < 5; i++) {
		ret = audio_write(core, 0x0a1e + i, params->iec.status[i]);
		if (ret)
			goto fail;
	}
	ret = audio_write(core, TPI_AUDIO,
			  (params->sample_width == 16 ? 0x40 : 0xc0) | BIT(4));
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
