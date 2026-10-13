/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_AUDIO_H__
#define __S7D_HDMI_AUDIO_H__

#include <linux/types.h>

struct regmap;
struct hdmi_codec_params;

bool s7d_hdmi_audio_supported(const u8 *eld, const struct hdmi_codec_params *params);
int s7d_hdmi_audio_configure(struct regmap *core, struct regmap *top,
			     const struct hdmi_codec_params *params);
int s7d_hdmi_audio_mute(struct regmap *core, bool mute);
int s7d_hdmi_audio_disable(struct regmap *core);

#endif
