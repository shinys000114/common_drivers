// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/amlogic/s7d-hdmi-phy.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/hdmi.h>
#include <linux/interrupt.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/workqueue.h>
#include <media/cec-notifier.h>
#include <sound/hdmi-codec.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>
#include <drm/drm_probe_helper.h>
#include <drm/display/drm_scdc_helper.h>
#include <drm/display/drm_hdmi_helper.h>

#include "s7d-caps.h"
#include "s7d-hdmi.h"
#include "s7d-hdmi-output.h"
#include "s7d-hdmi-io.h"
#include "s7d-hdmi-ddc.h"
#include "s7d-hdmi-audio.h"
#include "s7d-vpu.h"

/* TOP offsets are bytes; core offsets address 8-bit registers. */
#define TOP_RESET	0x000
#define TOP_CLK		0x004
#define TOP_HPD_FILTER	0x008
#define TOP_MASK	0x00c
#define TOP_STATUS	0x010
#define TOP_CLEAR	0x014
#define TOP_BIST	0x018
#define TOP_PATTERN0	0x028
#define TOP_PATTERN1	0x02c
#define TOP_PATTERN_CTRL 0x030
#define TOP_PIN_STATUS	0x038
#define TOP_INFILTER	0x074
#define TOP_SCRATCH	0x07c
#define TOP_ACTIVE	0x08c
#define HPD_IRQS	(BIT(1) | BIT(2))
/* TOP_INTR_STAT[31:30] are read-only core IRQ shadows, not reserved bits. */
#define TOP_STATUS_VALID	(GENMASK(31, 30) | GENMASK(8, 0))
#define CORE_PWD_RESET	0x0110
#define CORE_SYS_STAT	0x0201
#define CORE_INTR2	0x0265
#define CORE_INTR2_MASK	0x026b
#define CORE_INTR5_MASK	0x026d
#define CORE_GCP_STATUS	0x06e5
#define CORE_TPI_SC	0x061a
#define CORE_INFO_SELECT 0x06bf
#define CORE_INFO_DATA	0x06c0
#define CORE_INFO_ENABLE 0x06df
#define CORE_INFO_TRANSMIT (BIT(7) | BIT(6)) /* enable and per-frame repeat */
#define CORE_HDCP1	0x062a
#define CORE_HDCP2	0x08bd
#define CORE_AUDIO_EN	0x0a13

struct s7d_hdmi_state {
	struct drm_bridge_state base;
	struct drm_display_mode mode;
	struct s7d_hdmi_output_state output;
	u8 avi[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AVI_INFOFRAME_SIZE];
	u8 vendor[HDMI_INFOFRAME_SIZE(MAX)];
	u8 vendor_len;
	u8 hdr[HDMI_INFOFRAME_HEADER_SIZE + HDMI_DRM_INFOFRAME_SIZE];
	u8 hdr_len;
	bool hdmi;
	bool scdc;
	bool high_tmds;
	bool valid;
};

struct s7d_hdmi {
	struct device *dev;
	struct drm_bridge bridge;
	struct drm_crtc *crtc;
	struct regmap *core;
	struct regmap *top;
	struct s7d_hdmi_ddc ddc;
	/* Serializes TX/packet/PHY lifecycle; DDC retains its own FIFO lock. */
	struct mutex lock;
	struct clk_bulk_data basic[4];
	struct clk_bulk_data video[2];
	struct clk_bulk_data video_mux[2];
	struct clk *video_parent;
	struct clk *video_div2;
	struct reset_control_bulk_data resets[2];
	struct phy *phy;
	int irq;
	bool phy_on;
	bool phy_initialized;
	bool video_on;
	bool prepared;
	struct s7d_hdmi_output_state prepared_output;
	bool hpd_enabled;
	bool stopping;
	struct delayed_work hpd_recovery;
	/* Sink TMDS_Config while scrambling, rechecked as vc4 does; else 0. */
	struct delayed_work scdc_check;
	u8 scdc_config;
	/* This driver left the sink scrambling; clear it even if the next EDID has no SCDC. */
	bool sink_scrambled;
	u8 eld[MAX_ELD_BYTES];
	struct platform_device *codec;
	struct clk *audio_clk;
	struct hdmi_codec_params audio_params;
	bool audio_clock_on;
	bool audio_valid;
	bool audio_muted;
	bool audio_spdif;
	bool eld_valid;
	int audio_error;
	/* Serializes callback registration against notification. */
	struct mutex audio_callback_lock;
	hdmi_codec_plugged_cb audio_plugged;
	struct device *codec_dev;
	struct work_struct audio_notify;
	struct cec_notifier *cec_notifier;
	u8 spd[HDMI_INFOFRAME_HEADER_SIZE + HDMI_SPD_INFOFRAME_SIZE];
	unsigned int sink_generation;
	unsigned int edid_generation;
};

static struct s7d_hdmi *to_s7d_hdmi(struct drm_bridge *bridge)
{
	return container_of(bridge, struct s7d_hdmi, bridge);
}

static struct s7d_hdmi_state *to_s7d_hdmi_state(struct drm_bridge_state *state)
{
	return container_of(state, struct s7d_hdmi_state, base);
}

/* Fixed-PLL/SCMI integer recalc can report 199999997 for nominal 200 MHz. */
static int s7d_hdmi_ddc_clock_check(unsigned long rate)
{
	/* 100 ppm still keeps the 50 kHz DDC profile below the 100 kHz limit. */
	if (!rate)
		return -EPROBE_DEFER;
	return rate >= 199980000 && rate <= 200020000 ? 0 : -ERANGE;
}

static int s7d_hdmi_audio_apply(struct s7d_hdmi *h);

int s7d_hdmi_bridge_set_connector(struct drm_bridge *bridge, struct drm_connector *connector)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);
	struct cec_connector_info info;
	int ret = 0;

	/* DRM detaches the connector before its managed cleanup. */
	mutex_lock(&h->lock);
	if (connector) {
		cec_fill_conn_info_from_drm(&info, connector);
		h->cec_notifier = cec_notifier_conn_register(h->dev, NULL, &info);
		if (!h->cec_notifier) {
			ret = -ENOMEM;
			goto out;
		}
	} else {
		cec_notifier_conn_unregister(h->cec_notifier);
		h->cec_notifier = NULL;
	}
out:
	mutex_unlock(&h->lock);
	return ret;
}

static int s7d_hdmi_audio_get_eld(struct device *dev, void *data, u8 *buf, size_t len)
{
	struct s7d_hdmi *h = data;
	int ret = -ENODEV;

	memset(buf, 0, len);
	mutex_lock(&h->lock);
	if (!h->eld_valid)
		goto out;
	memcpy(buf, h->eld, min(sizeof(h->eld), len));
	ret = 0;
out:
	mutex_unlock(&h->lock);
	return ret;
}

void s7d_hdmi_bridge_eld_updated(struct drm_bridge *bridge, struct drm_connector *connector)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);
	u8 eld[MAX_ELD_BYTES];
	bool valid, changed;

	mutex_lock(&connector->eld_mutex);
	memcpy(eld, connector->eld, sizeof(eld));
	mutex_unlock(&connector->eld_mutex);
	valid = connector->display_info.is_hdmi && connector->display_info.has_audio &&
		eld[0] && drm_eld_sad_count(eld) && drm_eld_size(eld) <= sizeof(eld);
	mutex_lock(&h->lock);
	if (!h->stopping && h->edid_generation == h->sink_generation &&
	    connector->display_info.is_hdmi)
		cec_notifier_set_phys_addr(h->cec_notifier,
			connector->display_info.source_physical_address);
	else
		cec_notifier_phys_addr_invalidate(h->cec_notifier);
	valid = valid && h->edid_generation == h->sink_generation;
	changed = h->eld_valid != valid || memcmp(h->eld, eld, sizeof(eld));
	memcpy(h->eld, eld, sizeof(eld));
	h->eld_valid = valid;
	if (valid && !h->audio_valid && h->audio_clock_on && h->prepared && h->phy_on &&
	    !h->audio_error) {
		if (s7d_hdmi_audio_supported(eld, &h->audio_params))
			h->audio_valid = !s7d_hdmi_audio_apply(h);
		else
			h->audio_error = -EINVAL;
	}
	mutex_unlock(&h->lock);
	if (changed)
		schedule_work(&h->audio_notify);
}

static void s7d_hdmi_audio_notify(struct work_struct *work)
{
	struct s7d_hdmi *h = container_of(work, struct s7d_hdmi, audio_notify);
	u8 eld[MAX_ELD_BYTES];
	bool plugged;

	mutex_lock(&h->audio_callback_lock);
	plugged = !s7d_hdmi_audio_get_eld(h->dev, h, eld, sizeof(eld));
	if (h->audio_plugged)
		h->audio_plugged(h->codec_dev, plugged);
	mutex_unlock(&h->audio_callback_lock);
}

static int s7d_hdmi_audio_hook(struct device *dev, void *data,
			     hdmi_codec_plugged_cb fn, struct device *codec_dev)
{
	struct s7d_hdmi *h = data;

	mutex_lock(&h->audio_callback_lock);
	h->audio_plugged = fn;
	h->codec_dev = codec_dev;
	mutex_unlock(&h->audio_callback_lock);
	if (fn)
		schedule_work(&h->audio_notify);
	return 0;
}

static int s7d_hdmi_audio_apply(struct s7d_hdmi *h)
{
	int ret;

	ret = s7d_hdmi_audio_configure(h->core, h->top, &h->audio_params,
					 &h->prepared_output, h->audio_spdif);
	if (!ret)
		ret = s7d_hdmi_audio_mute(h->core, h->audio_muted);
	if (ret) {
		s7d_hdmi_audio_disable(h->core);
		h->audio_error = ret;
		dev_err(h->dev, "HDMI audio setup failed: %d\n", ret);
	}
	return ret;
}

static int s7d_hdmi_audio_hw_params(struct device *dev, void *data,
				  struct hdmi_codec_daifmt *fmt,
				  struct hdmi_codec_params *params)
{
	struct s7d_hdmi *h = data;
	int ret;

	if ((fmt->fmt != HDMI_I2S && fmt->fmt != HDMI_SPDIF) ||
	    fmt->bit_clk_inv || fmt->frame_clk_inv ||
	    fmt->bit_clk_provider || fmt->frame_clk_provider ||
	    (fmt->bit_fmt != SNDRV_PCM_FORMAT_S16_LE &&
	     fmt->bit_fmt != SNDRV_PCM_FORMAT_S24_LE &&
	     fmt->bit_fmt != SNDRV_PCM_FORMAT_S32_LE))
		return -EINVAL;
	mutex_lock(&h->lock);
	if (!h->eld_valid || !h->prepared || !h->phy_on) {
		ret = -ENODEV;
		goto out;
	}
	if (!s7d_hdmi_audio_supported(h->eld, params)) {
		ret = -EINVAL;
		goto out;
	}
	if (!h->audio_clock_on) {
		ret = clk_set_rate(h->audio_clk, 200000000);
		if (ret)
			goto out;
		ret = s7d_hdmi_ddc_clock_check(clk_get_rate(h->audio_clk));
		if (ret)
			goto out;
		ret = clk_prepare_enable(h->audio_clk);
		if (ret)
			goto out;
		h->audio_clock_on = true;
	}
	h->audio_params = *params;
	h->audio_spdif = fmt->fmt == HDMI_SPDIF;
	h->audio_error = 0;
	h->audio_muted = true;
	ret = s7d_hdmi_audio_apply(h);
	h->audio_valid = !ret;
	if (ret) {
		clk_disable_unprepare(h->audio_clk);
		h->audio_clock_on = false;
	}
out:
	mutex_unlock(&h->lock);
	return ret;
}

static int s7d_hdmi_audio_set_mute(struct device *dev, void *data, bool mute, int direction)
{
	struct s7d_hdmi *h = data;
	int ret = 0;

	if (direction != SNDRV_PCM_STREAM_PLAYBACK)
		return -EINVAL;
	mutex_lock(&h->lock);
	h->audio_muted = mute;
	if (!mute && h->audio_error)
		ret = h->audio_error;
	else if (h->prepared && h->audio_valid)
		ret = s7d_hdmi_audio_mute(h->core, mute);
	else if (!mute)
		ret = -ENODEV;
	if (ret && h->audio_valid) {
		h->audio_error = ret;
		s7d_hdmi_audio_disable(h->core);
		schedule_work(&h->audio_notify);
	}
	mutex_unlock(&h->lock);
	return ret;
}

static void s7d_hdmi_audio_shutdown(struct device *dev, void *data)
{
	struct s7d_hdmi *h = data;
	int ret;

	mutex_lock(&h->lock);
	h->audio_valid = false;
	h->audio_muted = true;
	ret = s7d_hdmi_audio_disable(h->core);
	if (ret)
		dev_err(h->dev, "HDMI audio shutdown failed: %d\n", ret);
	if (h->audio_clock_on) {
		clk_disable_unprepare(h->audio_clk);
		h->audio_clock_on = false;
	}
	mutex_unlock(&h->lock);
}

static const struct hdmi_codec_ops s7d_hdmi_codec_ops = {
	.hw_params = s7d_hdmi_audio_hw_params,
	.audio_shutdown = s7d_hdmi_audio_shutdown,
	.mute_stream = s7d_hdmi_audio_set_mute,
	.get_eld = s7d_hdmi_audio_get_eld,
	.hook_plugged_cb = s7d_hdmi_audio_hook,
	.no_capture_mute = 1,
};

static void s7d_hdmi_codec_unregister(void *data)
{
	struct s7d_hdmi *h = data;

	platform_device_unregister(h->codec);
	cancel_work_sync(&h->audio_notify);
}

static int s7d_hdmi_write_checked(struct s7d_hdmi *h, struct regmap *map,
				  unsigned int reg, unsigned int mask,
				  unsigned int value)
{
	int ret;

	/* Log before access too: firmware/MMIO can stall without returning errno. */
	dev_dbg(h->dev, "TX %s reg %#x mask %#x value %#x begin\n",
		map == h->core ? "core" : "top", reg, mask, value);
	ret = s7d_hdmi_update_checked(map, reg, mask, value);
	dev_dbg(h->dev, "TX %s reg %#x end: %d\n",
		map == h->core ? "core" : "top", reg, ret);

	if (ret)
		dev_err(h->dev, "TX %s reg %#x mask %#x value %#x failed: %d\n",
			map == h->core ? "core" : "top", reg, mask, value, ret);
	return ret;
}

static int core_write(struct s7d_hdmi *h, unsigned int reg, unsigned int value)
{
	return s7d_hdmi_write_checked(h, h->core, reg, 0xff, value);
}

static int top_write(struct s7d_hdmi *h, unsigned int reg, unsigned int mask,
		     unsigned int value)
{
	return s7d_hdmi_write_checked(h, h->top, reg, mask, value);
}

static int s7d_hdmi_phy_exit(struct s7d_hdmi *h)
{
	int ret;

	if (!h->phy_initialized)
		return 0;
	ret = phy_exit(h->phy);
	if (!ret)
		h->phy_initialized = false;
	return ret;
}

static int s7d_hdmi_quiesce(void *data)
{
	struct s7d_hdmi *h = data;
	int ret = 0;

	mutex_lock(&h->lock);
	h->prepared = false;
	h->scdc_config = 0;
	if (h->audio_valid) {
		/*
		 * Report a stuck audio block to the audio path only; stopping the
		 * link still has to release the PHY and clocks for the next mode.
		 */
		ret = s7d_hdmi_audio_disable(h->core);
		h->audio_valid = false;
		if (ret) {
			h->audio_error = ret;
			dev_err(h->dev, "HDMI audio stop before modeset failed: %d\n", ret);
			ret = 0;
		}
	}
	if (h->phy_on) {
		ret = phy_power_off(h->phy);
		if (ret)
			goto out;
		h->phy_on = false;
	}
	ret = s7d_hdmi_phy_exit(h);
	if (ret)
		goto out;
	/* Keep basic clocks and the power domain for DDC/HPD while disconnected. */
	if (h->video_on) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(h->video), h->video);
		h->video_on = false;
	}
out:
	mutex_unlock(&h->lock);
	return ret;
}

static int s7d_hdmi_packet(struct s7d_hdmi *h, unsigned int bank,
			   const u8 *packet, unsigned int len)
{
	unsigned int i;
	int ret;

	ret = core_write(h, CORE_INFO_SELECT, bank);
	if (ret)
		return ret;
	for (i = 0; i < 31; i++) {
		ret = core_write(h, CORE_INFO_DATA + i, i < len ? packet[i] : 0);
		if (ret)
			return ret;
	}
	return s7d_hdmi_write_checked(h, h->core, CORE_INFO_ENABLE,
				      CORE_INFO_TRANSMIT, CORE_INFO_TRANSMIT);
}

static int s7d_hdmi_packets(struct s7d_hdmi *h, const struct s7d_hdmi_state *state)
{
	unsigned int bank;
	int ret;

	/* All eleven TPI banks: discard inherited audio/vendor/HDR/EMP packets. */
	for (bank = 0; bank < 11; bank++) {
		ret = core_write(h, CORE_INFO_SELECT, bank);
		if (ret)
			return ret;
		/* Bit 5 can read as one with transmission off; it is not our enable. */
		ret = s7d_hdmi_write_checked(h, h->core, CORE_INFO_ENABLE,
					     CORE_INFO_TRANSMIT, 0);
		if (ret)
			return ret;
	}
	if (!state->hdmi)
		return 0;
	ret = s7d_hdmi_packet(h, 0, state->avi, sizeof(state->avi));
	if (!ret)
		ret = s7d_hdmi_packet(h, 3, h->spd, sizeof(h->spd));
	if (!ret && state->hdr_len)
		ret = s7d_hdmi_packet(h, 6, state->hdr, state->hdr_len);
	if (ret || !state->vendor_len)
		return ret;
	return s7d_hdmi_packet(h, 5, state->vendor, state->vendor_len);
}

static int s7d_hdmi_fifo_reset(struct s7d_hdmi *h, u8 bit)
{
	int ret;

	ret = s7d_hdmi_write_checked(h, h->core, CORE_PWD_RESET, bit, bit);
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, CORE_PWD_RESET, bit, 0);
	if (ret || bit != BIT(1))
		return ret;
	return regmap_write(h->core, CORE_INTR2, BIT(1));
}

static int s7d_hdmi_check_phase(struct s7d_hdmi *h,
				const struct s7d_hdmi_state *state)
{
	unsigned int value, attempt, period, phase;
	int ret;

	if (s7d_hdmi_output_is_y422(&state->output)) {
		period = 1;
		phase = 0;
	} else if (s7d_hdmi_output_is_y420(&state->output)) {
		/* Half-rate characters: the selected vendor checks 2/8/4-pixel periods. */
		period = state->output.bpc == 8 ? 2 : state->output.bpc == 10 ? 8 : 4;
		phase = state->output.bpc == 12 ? 2 : 0;
	} else if (state->output.bpc == 8) {
		return 0;
	} else if (state->output.bpc == 10 &&
	    (state->output.encoding == S7D_HDMI_OUTPUT_RGB10 ||
	     state->output.encoding == S7D_HDMI_OUTPUT_601_Y44410 ||
	     state->output.encoding == S7D_HDMI_OUTPUT_709_Y44410)) {
		period = 4;
		phase = 0;
	} else if (state->output.bpc == 12 &&
		   (state->output.encoding == S7D_HDMI_OUTPUT_RGB12 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_601_Y44412 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_709_Y44412)) {
		period = 2;
		phase = 2;
	} else {
		return -EINVAL;
	}
	if (!state->mode.htotal)
		return -EINVAL;
	if (state->mode.htotal % period)
		return 0;
	for (attempt = 0; attempt < 20; attempt++) {
		ret = regmap_read_poll_timeout(h->core, CORE_SYS_STAT, value,
					      value & BIT(0), 10, 10000);
		if (ret) {
			dev_warn(h->dev, "TX clock not stable for the deep-color phase check: %#x\n",
				 value);
			return ret;
		}
		ret = regmap_read(h->core, CORE_GCP_STATUS, &value);
		if (ret)
			return ret;
		if (((value >> 5) & 3) == phase)
			return 0;
		if (attempt == 19)
			break;
		ret = s7d_hdmi_fifo_reset(h, BIT(1));
		if (ret)
			return ret;
	}
	/* As the vendor does, keep the output: failing the modeset only blanks it. */
	dev_warn(h->dev, "deep-color phase %u, expected %u\n", (value >> 5) & 3, phase);
	return 0;
}

static int s7d_hdmi_setup_tx(struct s7d_hdmi *h, const struct s7d_hdmi_state *state)
{
	static const struct reg_sequence config[] = {
		{ 0x029d, 0 }, /* FRL_LINK_RATE_CONFIG */
		{ 0x0319, 0 }, /* SW_RST */
		{ 0x012f, 1 }, /* CLK_DIV_CNTRL: TMDS */
		{ 0x010d, 0xf4 }, /* CLKPWD: vendor TMDS core profile */
		{ 0x010b, 1 }, /* SOC_FUNC_SEL */
		{ 0x02ca, 2 }, /* TEST_TXCTRL */
		{ 0x0235, 0x8a }, /* CLKRATIO */
		{ 0x0b46, 0 }, /* VP_OUTPUT_MASK */
		{ 0x06ed, 4 }, /* VTEM disabled */
		{ 0x06f7, 4 }, /* GEN5 disabled */
	};
	const u8 mapping[] = {
		state->output.tx_mapping & 0xff, state->output.tx_mapping >> 8,
	};
	u8 actual[sizeof(mapping)], p2t, merge;
	unsigned int i;
	int ret;

	if (state->output.bpc == 8) {
		p2t = 0;
		merge = 0;
	} else if (state->output.bpc == 10 &&
		   (state->output.encoding == S7D_HDMI_OUTPUT_RGB10 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_601_Y44410 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_709_Y44410 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_601_Y42010 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_709_Y42010)) {
		p2t = 0x81;
		merge = 1;
	} else if (state->output.bpc == 12 &&
		   (state->output.encoding == S7D_HDMI_OUTPUT_RGB12 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_601_Y44412 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_709_Y44412 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_601_Y42012 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_709_Y42012)) {
		p2t = 0x82;
		merge = 1;
	} else {
		return -EINVAL;
	}
	ret = top_write(h, TOP_CLK, 7, 7);
	if (ret)
		return ret;
	ret = top_write(h, TOP_PATTERN0, U32_MAX,
			state->high_tmds ? 0 : 0x001f001f);
	if (ret)
		return ret;
	ret = top_write(h, TOP_PATTERN1, U32_MAX,
			state->high_tmds ? 0x03ff03ff : 0x001f001f);
	if (ret)
		return ret;
	ret = top_write(h, TOP_PATTERN_CTRL, BIT(1), 0);
	if (ret)
		return ret;
	/* load_tmds_clk_pttn is a write-only pulse and always reads back zero. */
	dev_dbg(h->dev, "TX TMDS pattern load begin\n");
	ret = regmap_write(h->top, TOP_PATTERN_CTRL, BIT(0));
	dev_dbg(h->dev, "TX TMDS pattern load end: %d\n", ret);
	if (ret)
		return ret;
	ret = top_write(h, TOP_PATTERN_CTRL, BIT(1), state->high_tmds ? BIT(1) : 0);
	if (ret)
		return ret;
	ret = core_write(h, 0x0900, BIT(5) | state->high_tmds); /* SCRCTL */
	if (ret)
		return ret;
	ret = top_write(h, TOP_BIST, GENMASK(14, 12) | GENMASK(11, 0), BIT(12));
	if (ret)
		return ret;
	ret = core_write(h, 0x0236, p2t);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(config); i++) {
		ret = core_write(h, config[i].reg, config[i].reg == 0x0b46 ?
				 state->output.tx_mask : config[i].def);
		if (ret)
			return ret;
	}
	ret = core_write(h, 0x06e1, merge);
	if (ret)
		return ret;
	/* Write the complete two-byte mapping before checking its readback. */
	ret = regmap_bulk_write(h->core, 0x0b44, mapping, sizeof(mapping));
	if (ret)
		return ret;
	ret = regmap_bulk_read(h->core, 0x0b44, actual, sizeof(actual));
	if (ret)
		return ret;
	if (memcmp(actual, mapping, sizeof(mapping))) {
		dev_err(h->dev, "HDMI output mapping readback %#x%02x, expected %#06x\n",
			actual[1], actual[0], state->output.tx_mapping);
		return -EIO;
	}
	ret = s7d_hdmi_write_checked(h, h->core, 0x012a, 3, 0); /* Original DE */
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, 0x023c, BIT(6), BIT(6));
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, 0x023d, BIT(0), 0);
	if (ret)
		return ret;
	/* PCM stays disabled until hdmi-codec configures audio explicitly. */
	ret = core_write(h, CORE_AUDIO_EN, 0);
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, 0x0f00, BIT(0), 0); /* Dynamic HDR */
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, 0x0f15, BIT(0), 0); /* EMP insertion */
	if (ret)
		return ret;
	/* S7D has no accessible DSC packet bank; even a disable read can hang. */
	ret = s7d_hdmi_packets(h, state);
	if (ret)
		return ret;
	/* The transmitter sees repeated pixels: 1440 per line for 480i/576i. */
	ret = top_write(h, TOP_ACTIVE, 0x7fff7fff, (state->mode.vdisplay << 16) |
			(state->mode.hdisplay << !!(state->mode.flags & DRM_MODE_FLAG_DBLCLK)));
	if (ret)
		return ret;
	ret = s7d_hdmi_fifo_reset(h, BIT(2));
	if (ret)
		return ret;
	ret = s7d_hdmi_fifo_reset(h, BIT(1));
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, CORE_TPI_SC, 0x89, state->hdmi ? 1 : 0);
	if (ret || !state->hdmi)
		return ret;
	return s7d_hdmi_write_checked(h, h->core, CORE_TPI_SC, BIT(7), BIT(7));
}

static int s7d_hdmi_prepare(void *data,
			    const struct s7d_hdmi_output_state *output)
{
	struct s7d_hdmi *h = data;
	struct s7d_hdmi_state *state;
	unsigned int i;
	int ret;

	if (!output || !h->bridge.base.state)
		return -EINVAL;
	state = to_s7d_hdmi_state(drm_priv_to_bridge_state(h->bridge.base.state));
	if (!state->valid || !s7d_hdmi_output_same(&state->output, output))
		return -EINVAL;
	mutex_lock(&h->lock);
	ret = -EBUSY;
	if (h->phy_on || h->phy_initialized || h->video_on)
		goto out;
	/*
	 * VPU protects the ENCP parent rate while we prepare sibling branches.
	 * The TX pixel branch [0] uses the div2 output for 4:2:0; FE stays at the
	 * ENCP rate.
	 */
	for (i = 0; i < ARRAY_SIZE(h->video); i++) {
		bool half = !i && output->pixel_divider == 2;
		unsigned long rate = output->rate.pixel / (half ? 2 : 1);

		ret = clk_set_parent(h->video_mux[i].clk,
				     half ? h->video_div2 : h->video_parent);
		if (ret)
			goto out;
		ret = clk_set_rate(h->video[i].clk, rate);
		if (ret)
			goto out;
		if (clk_get_rate(h->video[i].clk) != rate) {
			ret = -ERANGE;
			goto out;
		}
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(h->video), h->video);
	if (ret)
		goto out;
	h->video_on = true;
	/* Serialize with FIFO transfers; no whole-controller reset in this path. */
	mutex_lock(&h->ddc.lock);
	ret = s7d_hdmi_setup_tx(h, state);
	mutex_unlock(&h->ddc.lock);
	if (!ret) {
		h->prepared_output = *output;
		h->prepared = true;
	}
	/* On failure the VPU calls quiesce before releasing its own references. */
out:
	mutex_unlock(&h->lock);
	return ret;
}

static enum drm_connector_status s7d_hdmi_detect(struct drm_bridge *bridge)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);
	unsigned int value;

	if (regmap_read(h->top, TOP_PIN_STATUS, &value) || (value & ~3U))
		return connector_status_unknown;
	return value & 1 ? connector_status_connected : connector_status_disconnected;
}

static const struct drm_edid *s7d_hdmi_get_edid(struct drm_bridge *bridge,
				    struct drm_connector *connector)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);
	const struct drm_edid *edid;
	unsigned int attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		/* Some sinks assert HPD before their EDID responder is ready. */
		if (attempt)
			msleep(500);
		mutex_lock(&h->lock);
		if (h->stopping) {
			mutex_unlock(&h->lock);
			return NULL;
		}
		h->edid_generation = h->sink_generation;
		mutex_unlock(&h->lock);
		if (s7d_hdmi_detect(bridge) != connector_status_connected)
			return NULL;
		edid = drm_edid_read_ddc(connector, &h->ddc.adapter);
		if (edid)
			return edid;
	}
	return NULL;
}

static bool s7d_hdmi_y420_709(enum s7d_hdmi_output_encoding encoding)
{
	return encoding == S7D_HDMI_OUTPUT_709_Y4208 ||
		encoding == S7D_HDMI_OUTPUT_709_Y42010 ||
		encoding == S7D_HDMI_OUTPUT_709_Y42012;
}

static enum drm_mode_status s7d_hdmi_link_valid(const struct drm_display_info *info,
						enum s7d_hdmi_output_encoding encoding,
						const struct drm_display_mode *mode)
{
	struct s7d_hdmi_output_state output;

	if (s7d_hdmi_output_build(encoding, (unsigned long)s7d_mode_clock(mode) * 1000,
				  24000000, &output))
		return MODE_CLOCK_RANGE;
	if (s7d_hdmi_output_is_y420(&output) && !info->is_hdmi)
		return MODE_NO_420;
	if (!info->is_hdmi && s7d_hdmi_output_tmds_above(&output, 165000000))
		return MODE_CLOCK_HIGH;
	if (output.high_tmds &&
	    (!info->is_hdmi || !info->hdmi.scdc.supported ||
	     !info->hdmi.scdc.scrambling.supported))
		return MODE_CLOCK_HIGH;
	if (info->max_tmds_clock &&
	    s7d_hdmi_output_tmds_above(&output, (unsigned long)info->max_tmds_clock * 1000))
		return MODE_CLOCK_HIGH;
	return MODE_OK;
}

static enum drm_mode_status s7d_hdmi_mode_valid(struct drm_bridge *bridge,
					      const struct drm_display_info *info,
					      const struct drm_display_mode *mode)
{
	struct s7d_encp_state state;
	enum drm_mode_status status;

	if (mode->clock > 594000 || mode->hdisplay > 4096 || mode->vdisplay > 2160)
		return MODE_CLOCK_HIGH;
	/* 4:2:0 halves the TMDS rate; ENCP still runs at the full pixel rate. */
	if (drm_mode_is_420_only(info, mode)) {
		status = s7d_hdmi_link_valid(info, S7D_HDMI_OUTPUT_709_Y4208, mode);
	} else {
		status = s7d_hdmi_link_valid(info, S7D_HDMI_OUTPUT_RGB8, mode);
		if (status != MODE_OK && drm_mode_is_420_also(info, mode))
			status = s7d_hdmi_link_valid(info, S7D_HDMI_OUTPUT_709_Y4208, mode);
	}
	if (status != MODE_OK)
		return status;
	return s7d_encp_build_state(mode, &state);
}

/* Colorimetry intent selects the matching implemented 8-bit encoding. */
static int s7d_hdmi_requested_output(const struct drm_connector_state *conn_state,
				    enum s7d_hdmi_output_encoding *encoding)
{
	switch (conn_state->colorspace) {
	case DRM_MODE_COLORIMETRY_DEFAULT:
	case DRM_MODE_COLORIMETRY_BT2020_RGB:
		*encoding = S7D_HDMI_OUTPUT_RGB8;
		return 0;
	case DRM_MODE_COLORIMETRY_SMPTE_170M_YCC:
		*encoding = S7D_HDMI_OUTPUT_601_Y4448;
		return 0;
	case DRM_MODE_COLORIMETRY_BT709_YCC:
	case DRM_MODE_COLORIMETRY_BT2020_YCC:
		*encoding = S7D_HDMI_OUTPUT_709_Y4448;
		return 0;
	default:
		return -EINVAL;
	}
}

static int s7d_hdmi_requested_sampling(const struct drm_connector_state *conn_state,
				      enum s7d_hdmi_output_encoding *encoding)
{
	const struct drm_display_info *info = &conn_state->connector->display_info;
	int ret;

	ret = s7d_hdmi_requested_output(conn_state, encoding);
	if (ret || *encoding == S7D_HDMI_OUTPUT_RGB8)
		return ret;
	if (!info->is_hdmi)
		return -EINVAL;
	if (info->color_formats & DRM_COLOR_FORMAT_YCBCR444)
		return 0;
	if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR422))
		return -EINVAL;
	*encoding = *encoding == S7D_HDMI_OUTPUT_601_Y4448 ?
		S7D_HDMI_OUTPUT_601_Y4228 : S7D_HDMI_OUTPUT_709_Y4228;
	return 0;
}

static bool s7d_hdmi_bt2020(const struct drm_connector_state *state)
{
	return state->colorspace == DRM_MODE_COLORIMETRY_BT2020_RGB ||
		state->colorspace == DRM_MODE_COLORIMETRY_BT2020_YCC;
}

/* A PQ or HLG transfer asks for at least ten bits on the wire. */
static bool s7d_hdmi_hdr_eotf(const struct drm_connector_state *state)
{
	const struct hdr_output_metadata *metadata;

	if (!state->hdr_output_metadata ||
	    state->hdr_output_metadata->length < sizeof(*metadata))
		return false;
	metadata = state->hdr_output_metadata->data;
	return metadata->metadata_type == HDMI_STATIC_METADATA_TYPE1 &&
		metadata->hdmi_metadata_type1.eotf != HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
}

static bool s7d_hdmi_rgb_limited(const struct drm_connector_state *state,
				 const struct drm_display_mode *mode)
{
	if (state->colorspace != DRM_MODE_COLORIMETRY_DEFAULT &&
	    state->colorspace != DRM_MODE_COLORIMETRY_BT2020_RGB)
		return false;
	switch (state->hdmi.broadcast_rgb) {
	case DRM_HDMI_BROADCAST_RGB_FULL:
		return false;
	case DRM_HDMI_BROADCAST_RGB_LIMITED:
		return true;
	default:
		return state->connector->display_info.is_hdmi &&
			drm_default_rgb_quant_range(mode) == HDMI_QUANTIZATION_RANGE_LIMITED;
	}
}

int s7d_hdmi_connector_atomic_check(struct drm_connector *connector,
				  struct drm_atomic_state *atomic)
{
	struct drm_connector_state *old = drm_atomic_get_old_connector_state(atomic, connector);
	struct drm_connector_state *state = drm_atomic_get_new_connector_state(atomic, connector);
	struct drm_crtc_state *crtc_state, *old_crtc_state;
	enum s7d_hdmi_output_encoding encoding;
	bool content_changed = connector->display_info.is_hdmi &&
		old->content_type != state->content_type;
	bool range_changed = old->hdmi.broadcast_rgb != state->hdmi.broadcast_rgb;
	bool hdr_changed = !drm_connector_atomic_hdr_metadata_equal(old, state);
	bool y422;
	int ret;

	if (!state->crtc || (!content_changed && !range_changed && !hdr_changed &&
			     old->colorspace == state->colorspace &&
			     state->colorspace == DRM_MODE_COLORIMETRY_DEFAULT))
		return 0;
	crtc_state = drm_atomic_get_crtc_state(atomic, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	if (old->colorspace != state->colorspace || hdr_changed ||
	    (content_changed && crtc_state->active))
		crtc_state->connectors_changed = true;
	if (!crtc_state->active)
		return 0;
	ret = s7d_hdmi_requested_sampling(state, &encoding);
	if (ret) {
		/* Without 4:4:4/4:2:2, the bridge may still carry the intent as 4:2:0. */
		if (!drm_mode_is_420(&connector->display_info, &crtc_state->mode))
			return ret;
		crtc_state->connectors_changed = true;
		return 0;
	}
	old_crtc_state = drm_atomic_get_old_crtc_state(atomic, state->crtc);
	if (old_crtc_state && old_crtc_state->active) {
		y422 = encoding == S7D_HDMI_OUTPUT_601_Y4228 ||
			encoding == S7D_HDMI_OUTPUT_709_Y4228;
		/* Expand affected planes before the bridge checks bus formats. */
		if (y422 != s7d_hdmi_output_is_y422(&to_s7d_crtc_state(old_crtc_state)->output) ||
		    s7d_hdmi_output_is_y420(&to_s7d_crtc_state(old_crtc_state)->output) ||
		    s7d_hdmi_rgb_limited(state, &crtc_state->mode) !=
		    to_s7d_crtc_state(old_crtc_state)->output.rgb_limited)
			crtc_state->connectors_changed = true;
	}
	return 0;
}

static bool s7d_hdmi_sink_accepts_output(const struct drm_display_info *info,
					 const struct s7d_hdmi_output_state *output)
{
	if (s7d_hdmi_output_tmds_above(output, S7D_HDMI_MAX_TMDS_RATE))
		return false;
	if (!info->is_hdmi && s7d_hdmi_output_tmds_above(output, 165000000))
		return false;
	if (info->max_tmds_clock &&
	    s7d_hdmi_output_tmds_above(output, (unsigned long)info->max_tmds_clock * 1000))
		return false;
	return !output->high_tmds ||
		(info->is_hdmi && info->hdmi.scdc.supported &&
		 info->hdmi.scdc.scrambling.supported);
}

/*
 * RGB requests use BT.709 for HD rasters and BT.601 below 720 lines, and
 * BT.2020 requests the BT.709 family with BT.2020 coefficients.
 */
static int s7d_hdmi_select_y420(const struct drm_connector_state *conn_state,
				enum s7d_hdmi_output_encoding requested,
				const struct drm_display_mode *mode, u8 min_bpc,
				struct s7d_hdmi_output_state *output)
{
	static const struct {
		enum s7d_hdmi_output_encoding encoding[2];
		u8 bpc;
		u8 dc;
	} y420[] = {
		{ { S7D_HDMI_OUTPUT_601_Y42012, S7D_HDMI_OUTPUT_709_Y42012 }, 12,
		  DRM_EDID_YCBCR420_DC_36 },
		{ { S7D_HDMI_OUTPUT_601_Y42010, S7D_HDMI_OUTPUT_709_Y42010 }, 10,
		  DRM_EDID_YCBCR420_DC_30 },
		{ { S7D_HDMI_OUTPUT_601_Y4208, S7D_HDMI_OUTPUT_709_Y4208 }, 8, 0 },
	};
	const struct drm_display_info *info = &conn_state->connector->display_info;
	struct s7d_hdmi_output_state next;
	unsigned int family, i;
	int ret = -EINVAL;

	if (!info->is_hdmi)
		return -EINVAL;
	if (s7d_hdmi_bt2020(conn_state))
		family = 1;
	else if (requested == S7D_HDMI_OUTPUT_601_Y4448 ||
		 requested == S7D_HDMI_OUTPUT_601_Y4228)
		family = 0;
	else if (requested == S7D_HDMI_OUTPUT_709_Y4448 ||
		 requested == S7D_HDMI_OUTPUT_709_Y4228)
		family = 1;
	else
		family = mode->vdisplay >= 720;
	for (i = 0; i < ARRAY_SIZE(y420); i++) {
		if (y420[i].bpc < min_bpc ||
		    (y420[i].dc && (conn_state->max_bpc < y420[i].bpc ||
				    !(info->hdmi.y420_dc_modes & y420[i].dc))))
			continue;
		ret = s7d_hdmi_output_build(y420[i].encoding[family],
					    (unsigned long)s7d_mode_clock(mode) * 1000, 24000000, &next);
		if (!ret)
			ret = s7d_hdmi_output_apply_bt2020(&next, s7d_hdmi_bt2020(conn_state));
		if (!ret && s7d_hdmi_sink_accepts_output(info, &next)) {
			*output = next;
			return 0;
		}
	}
	return ret ?: -EINVAL;
}

static int __s7d_hdmi_select_output(const struct drm_connector_state *conn_state,
				    const struct drm_display_mode *mode,
				    struct s7d_hdmi_output_state *output)
{
	static const struct {
		enum s7d_hdmi_output_encoding encoding[3];
		u8 bpc;
		u8 dc;
	} deep[] = {
		{ { S7D_HDMI_OUTPUT_RGB12, S7D_HDMI_OUTPUT_601_Y44412,
		    S7D_HDMI_OUTPUT_709_Y44412 }, 12, DRM_EDID_HDMI_DC_36 },
		{ { S7D_HDMI_OUTPUT_RGB10, S7D_HDMI_OUTPUT_601_Y44410,
		    S7D_HDMI_OUTPUT_709_Y44410 }, 10, DRM_EDID_HDMI_DC_30 },
	};
	const struct drm_display_info *info = &conn_state->connector->display_info;
	struct s7d_hdmi_output_state next;
	enum s7d_hdmi_output_encoding encoding;
	unsigned long pixel_rate = (unsigned long)s7d_mode_clock(mode) * 1000;
	unsigned int family, i;
	u8 dc_modes;
	bool limited = s7d_hdmi_rgb_limited(conn_state, mode);
	bool bt2020 = s7d_hdmi_bt2020(conn_state);
	bool y420_also = drm_mode_is_420_also(info, mode);
	int ret;

	ret = s7d_hdmi_requested_output(conn_state, &encoding);
	if (ret)
		return ret;
	if (drm_mode_is_420_only(info, mode))
		return s7d_hdmi_select_y420(conn_state, encoding, mode, 8, output);
	ret = s7d_hdmi_requested_sampling(conn_state, &encoding);
	if (ret)
		return y420_also ?
			s7d_hdmi_select_y420(conn_state, encoding, mode, 8, output) : ret;
	if (encoding == S7D_HDMI_OUTPUT_601_Y4228 ||
	    encoding == S7D_HDMI_OUTPUT_709_Y4228)
		goto build;
	if (encoding == S7D_HDMI_OUTPUT_RGB8) {
		family = 0;
		dc_modes = info->edid_hdmi_rgb444_dc_modes;
	} else {
		family = encoding == S7D_HDMI_OUTPUT_601_Y4448 ? 1 : 2;
		dc_modes = info->edid_hdmi_ycbcr444_dc_modes;
	}
	if (conn_state->max_bpc >= 10 && info->is_hdmi && drm_match_cea_mode(mode) != 1) {
		for (i = 0; i < ARRAY_SIZE(deep); i++) {
			if (conn_state->max_bpc < deep[i].bpc || !(dc_modes & deep[i].dc))
				continue;
			ret = s7d_hdmi_output_build(deep[i].encoding[family], pixel_rate,
						    24000000, &next);
			if (!ret && s7d_hdmi_sink_accepts_output(info, &next))
				goto adjust;
		}
		/* HDR prefers deep 4:2:0 to an 8-bit full-rate link (UHD60). */
		if (y420_also && s7d_hdmi_hdr_eotf(conn_state) &&
		    !s7d_hdmi_select_y420(conn_state, encoding, mode, 10, output))
			return 0;
	}
build:
	ret = s7d_hdmi_output_build(encoding, pixel_rate, 24000000, &next);
	if (ret || !s7d_hdmi_sink_accepts_output(info, &next)) {
		/* A 4:2:0-capable mode falls back when the full-rate link does not fit. */
		if (y420_also)
			return s7d_hdmi_select_y420(conn_state, encoding, mode, 8, output);
		return ret ?: -EINVAL;
	}
adjust:
	ret = s7d_hdmi_output_apply_bt2020(&next, bt2020);
	if (ret)
		return ret;
	ret = s7d_hdmi_output_apply_rgb_range(&next, limited);
	if (ret)
		return ret;
	*output = next;
	return 0;
}

/* Pixel repetition (480i/576i): no 4:2:2 and no dither, like the vendor. */
static int s7d_hdmi_select_output(const struct drm_connector_state *conn_state,
				  const struct drm_display_mode *mode,
				  struct s7d_hdmi_output_state *output)
{
	int ret = __s7d_hdmi_select_output(conn_state, mode, output);

	return ret ?: s7d_hdmi_output_apply_repeat(output,
						 mode->flags & DRM_MODE_FLAG_DBLCLK);
}

static u32 *s7d_hdmi_output_bus_fmts(struct drm_bridge *bridge,
				      struct drm_bridge_state *bridge_state,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state,
				      unsigned int *num_output_fmts)
{
	u32 *formats;

	/* HDMI is serialized; the negotiated input describes the VPU output. */
	*num_output_fmts = 1;
	formats = kmalloc(sizeof(*formats), GFP_KERNEL);
	if (formats)
		formats[0] = MEDIA_BUS_FMT_FIXED;
	return formats;
}

static u32 *s7d_hdmi_input_bus_fmts(struct drm_bridge *bridge,
				     struct drm_bridge_state *bridge_state,
				     struct drm_crtc_state *crtc_state,
				     struct drm_connector_state *conn_state,
				     u32 output_fmt, unsigned int *num_input_fmts)
{
	struct s7d_hdmi_output_state output;
	enum s7d_hdmi_output_encoding encoding;
	u32 format, *formats;

	*num_input_fmts = 0;
	if (output_fmt != MEDIA_BUS_FMT_FIXED)
		return NULL;
	if (!crtc_state->active) {
		if (s7d_hdmi_requested_output(conn_state, &encoding))
			return NULL;
		format = encoding == S7D_HDMI_OUTPUT_RGB8 ?
			MEDIA_BUS_FMT_RGB888_1X24 : MEDIA_BUS_FMT_YUV8_1X24;
	} else {
		if (s7d_hdmi_select_output(conn_state,
					   &crtc_state->adjusted_mode, &output))
			return NULL;
		format = output.bus_format;
	}
	formats = kmalloc(sizeof(*formats), GFP_KERNEL);
	if (formats) {
		formats[0] = format;
		*num_input_fmts = 1;
	}
	return formats;
}

static bool s7d_hdmi_link_same(const struct s7d_hdmi_state *a,
			       const struct s7d_hdmi_state *b)
{
	if (!a || !b || !a->valid || !b->valid || a->hdmi != b->hdmi ||
	    a->scdc != b->scdc || a->high_tmds != b->high_tmds)
		return false;
	if (!a->hdmi)
		return true;
	if (a->vendor_len > sizeof(a->vendor) || b->vendor_len > sizeof(b->vendor) ||
	    a->vendor_len != b->vendor_len || a->hdr_len > sizeof(a->hdr) ||
	    b->hdr_len > sizeof(b->hdr) || a->hdr_len != b->hdr_len)
		return false;
	return !memcmp(a->avi, b->avi, sizeof(a->avi)) &&
		!memcmp(a->vendor, b->vendor, a->vendor_len) &&
		!memcmp(a->hdr, b->hdr, a->hdr_len);
}

static int s7d_hdmi_atomic_check(struct drm_bridge *bridge, struct drm_bridge_state *base,
				 struct drm_crtc_state *crtc_state,
				 struct drm_connector_state *conn_state)
{
	struct s7d_hdmi_state *state = to_s7d_hdmi_state(base);
	struct drm_connector *connector = conn_state->connector;
	struct hdmi_avi_infoframe avi;
	struct hdmi_vendor_infoframe vendor;
	struct hdmi_drm_infoframe hdr;
	int ret;

	state->valid = false;
	if (!crtc_state->active)
		return 0;
	if (s7d_hdmi_mode_valid(bridge, &connector->display_info,
				&crtc_state->adjusted_mode) != MODE_OK)
		return -EINVAL;
	ret = s7d_hdmi_select_output(conn_state,
				     &crtc_state->adjusted_mode, &state->output);
	if (ret)
		return ret;
	ret = s7d_hdmi_output_apply_ctm(&state->output,
		crtc_state->ctm ? crtc_state->ctm->data : NULL);
	if (ret)
		return ret;
	if (base->input_bus_cfg.format != state->output.bus_format ||
	    base->output_bus_cfg.format != MEDIA_BUS_FMT_FIXED)
		return -EINVAL;
	state->mode = crtc_state->adjusted_mode;
	state->hdmi = connector->display_info.is_hdmi;
	state->scdc = state->hdmi && connector->display_info.hdmi.scdc.supported;
	state->high_tmds = state->output.high_tmds;
	state->vendor_len = 0;
	state->hdr_len = 0;
	if (state->hdmi) {
		ret = drm_hdmi_avi_infoframe_from_display_mode(&avi, connector, &state->mode);
		if (ret)
			return ret;
		if (state->output.encoding == S7D_HDMI_OUTPUT_RGB8 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_RGB10 ||
		    state->output.encoding == S7D_HDMI_OUTPUT_RGB12) {
			avi.colorspace = HDMI_COLORSPACE_RGB;
			drm_hdmi_avi_infoframe_quant_range(&avi, connector, &state->mode,
				state->output.rgb_limited ? HDMI_QUANTIZATION_RANGE_LIMITED :
				HDMI_QUANTIZATION_RANGE_FULL);
			drm_hdmi_avi_infoframe_colorimetry(&avi, conn_state);
		} else {
			avi.colorspace = s7d_hdmi_output_is_y422(&state->output) ?
				HDMI_COLORSPACE_YUV422 :
				s7d_hdmi_output_is_y420(&state->output) ?
				HDMI_COLORSPACE_YUV420 : HDMI_COLORSPACE_YUV444;
			avi.quantization_range = HDMI_QUANTIZATION_RANGE_DEFAULT;
			avi.ycc_quantization_range = HDMI_YCC_QUANTIZATION_RANGE_LIMITED;
			drm_hdmi_avi_infoframe_colorimetry(&avi, conn_state);
			/* An RGB request carried as 4:2:0 states the matrix selected for it. */
			if (conn_state->colorspace == DRM_MODE_COLORIMETRY_DEFAULT)
				avi.colorimetry = s7d_hdmi_y420_709(state->output.encoding) ?
					HDMI_COLORIMETRY_ITU_709 : HDMI_COLORIMETRY_ITU_601;
		}
		drm_hdmi_avi_infoframe_content_type(&avi, conn_state);
		ret = hdmi_avi_infoframe_pack(&avi, state->avi, sizeof(state->avi));
		if (ret < 0)
			return ret;
		/*
		 * Like the upstream HDMI state helper, send the metadata as given and
		 * leave DVI sinks without InfoFrames instead of failing the commit.
		 */
		if (conn_state->hdr_output_metadata) {
			ret = drm_hdmi_infoframe_set_hdr_metadata(&hdr, conn_state);
			if (ret)
				return ret;
			ret = hdmi_drm_infoframe_pack(&hdr, state->hdr, sizeof(state->hdr));
			if (ret < 0)
				return ret;
			state->hdr_len = ret;
		}
	}
	if (state->hdmi && connector->display_info.has_hdmi_infoframe) {
		ret = drm_hdmi_vendor_infoframe_from_display_mode(&vendor, connector, &state->mode);
		if (ret)
			return ret;
		ret = hdmi_vendor_infoframe_pack(&vendor, state->vendor, sizeof(state->vendor));
		if (ret < 0)
			return ret;
		state->vendor_len = ret;
	}
	state->valid = true;
	/* Copy the checked plan before prepare; VPU applies it before PHY/start. */
	{
		struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(crtc_state->state,
								       crtc_state->crtc);
		struct drm_bridge_state *old_bridge =
			drm_atomic_get_old_bridge_state(crtc_state->state, bridge);

		if (old && old->active &&
		    (!s7d_hdmi_output_same(&to_s7d_crtc_state(old)->output, &state->output) ||
		     !old_bridge || !s7d_hdmi_link_same(to_s7d_hdmi_state(old_bridge), state)))
			crtc_state->mode_changed = true;
	}
	to_s7d_crtc_state(crtc_state)->output = state->output;
	return 0;
}

static struct drm_bridge_state *s7d_hdmi_reset(struct drm_bridge *bridge)
{
	struct s7d_hdmi_state *state = kzalloc(sizeof(*state), GFP_KERNEL);

	if (!state)
		return ERR_PTR(-ENOMEM);
	__drm_atomic_helper_bridge_reset(bridge, &state->base);
	return &state->base;
}

static struct drm_bridge_state *s7d_hdmi_duplicate(struct drm_bridge *bridge)
{
	struct s7d_hdmi_state *state;

	state = kmemdup(to_s7d_hdmi_state(drm_priv_to_bridge_state(bridge->base.state)),
			sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	__drm_atomic_helper_bridge_duplicate_state(bridge, &state->base);
	return &state->base;
}

static void s7d_hdmi_destroy_state(struct drm_bridge *bridge, struct drm_bridge_state *state)
{
	kfree(to_s7d_hdmi_state(state));
}

static int s7d_hdmi_attach(struct drm_bridge *bridge, enum drm_bridge_attach_flags flags)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR) ||
	    bridge->encoder->possible_crtcs != BIT(0))
		return -EINVAL;
	h->crtc = drm_crtc_from_index(bridge->dev, 0);
	if (!h->crtc || !s7d_crtc_is_native(h->crtc)) {
		h->crtc = NULL;
		return -ENODEV;
	}
	return 0;
}

static void s7d_hdmi_detach(struct drm_bridge *bridge)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);

	WRITE_ONCE(h->hpd_enabled, false);
	mutex_lock(&h->lock);
	h->eld_valid = false;
	h->crtc = NULL;
	mutex_unlock(&h->lock);
	schedule_work(&h->audio_notify);
}

static int s7d_hdmi_setup_scdc(struct s7d_hdmi *h, const struct s7d_hdmi_state *state)
{
	u8 config = state->high_tmds ? SCDC_SCRAMBLING_ENABLE |
		SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 : 0;
	u8 value;
	int ret;

	if (!state->high_tmds) {
		/*
		 * Clear a previous mode's setting; the link needs no SCDC. Like vc4,
		 * undo scrambling this driver enabled even when the sink's new EDID
		 * no longer advertises SCDC.
		 */
		if (!state->scdc && !h->sink_scrambled)
			return 0;
		if (drm_scdc_writeb(&h->ddc.adapter, SCDC_TMDS_CONFIG, 0))
			dev_dbg(h->dev, "SCDC clear below 340 MHz failed\n");
		else
			h->sink_scrambled = false;
		return 0;
	}
	if (!state->scdc)
		return -EINVAL;
	ret = drm_scdc_readb(&h->ddc.adapter, SCDC_SINK_VERSION, &value);
	if (ret)
		return ret;
	if (!value)
		return -EINVAL;
	ret = drm_scdc_writeb(&h->ddc.adapter, SCDC_SOURCE_VERSION, 1);
	if (ret)
		return ret;
	h->sink_scrambled = true;
	ret = drm_scdc_writeb(&h->ddc.adapter, SCDC_TMDS_CONFIG, config);
	if (ret)
		return ret;
	ret = drm_scdc_readb(&h->ddc.adapter, SCDC_TMDS_CONFIG, &value);
	if (ret)
		return ret;
	if ((value & (SCDC_SCRAMBLING_ENABLE | SCDC_TMDS_BIT_CLOCK_RATIO_BY_40)) != config)
		return -EIO;
	/* Change the sink ratio with PHY off, at least 1 ms before transmission. */
	usleep_range(1000, 2000);
	return 0;
}

static void s7d_hdmi_enable(struct drm_bridge *bridge, struct drm_bridge_state *old_state)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);
	struct s7d_hdmi_state *state;
	int ret;

	mutex_lock(&h->lock);
	ret = s7d_crtc_last_error(h->crtc);
	if (ret)
		goto out;
	ret = -EIO;
	if (!h->prepared)
		goto out;
	state = to_s7d_hdmi_state(drm_priv_to_bridge_state(h->bridge.base.state));
	if (!state->valid || !s7d_hdmi_output_same(&h->prepared_output, &state->output))
		goto out;
	ret = s7d_hdmi_setup_scdc(h, state);
	if (ret)
		goto out;
	ret = phy_init(h->phy);
	if (ret)
		goto out;
	h->phy_initialized = true;
	ret = s7d_hdmi_check_phase(h, state);
	if (ret)
		goto out;
	ret = s7d_crtc_last_error(h->crtc);
	if (ret)
		goto out;
	ret = phy_power_on(h->phy);
	if (!ret) {
		h->phy_on = true;
		h->scdc_config = state->high_tmds ? SCDC_SCRAMBLING_ENABLE |
			SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 : 0;
		if (h->scdc_config)
			mod_delayed_work(system_wq, &h->scdc_check, HZ);
		h->audio_error = 0;
		h->audio_valid = false;
		if (h->audio_clock_on && h->eld_valid) {
			if (s7d_hdmi_audio_supported(h->eld, &h->audio_params))
				h->audio_valid = !s7d_hdmi_audio_apply(h);
			else
				h->audio_error = -EINVAL;
		}
		s7d_crtc_link_ready(h->crtc);
		schedule_work(&h->audio_notify);
	}
out:
	mutex_unlock(&h->lock);
	if (ret)
		s7d_crtc_link_error(h->crtc, ret);
}

static void s7d_hdmi_disable(struct drm_bridge *bridge, struct drm_bridge_state *old_state)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);
	int ret = s7d_hdmi_quiesce(h);

	if (ret)
		s7d_crtc_link_error(h->crtc, ret);
}

static void s7d_hdmi_hpd_enable(struct drm_bridge *bridge)
{
	WRITE_ONCE(to_s7d_hdmi(bridge)->hpd_enabled, true);
}

static void s7d_hdmi_hpd_disable(struct drm_bridge *bridge)
{
	WRITE_ONCE(to_s7d_hdmi(bridge)->hpd_enabled, false);
}

static const struct drm_bridge_funcs s7d_hdmi_bridge_funcs = {
	.attach = s7d_hdmi_attach,
	.detach = s7d_hdmi_detach,
	.detect = s7d_hdmi_detect,
	.edid_read = s7d_hdmi_get_edid,
	.mode_valid = s7d_hdmi_mode_valid,
	.atomic_get_output_bus_fmts = s7d_hdmi_output_bus_fmts,
	.atomic_get_input_bus_fmts = s7d_hdmi_input_bus_fmts,
	.atomic_check = s7d_hdmi_atomic_check,
	.atomic_reset = s7d_hdmi_reset,
	.atomic_duplicate_state = s7d_hdmi_duplicate,
	.atomic_destroy_state = s7d_hdmi_destroy_state,
	.atomic_enable = s7d_hdmi_enable,
	.atomic_disable = s7d_hdmi_disable,
	.hpd_enable = s7d_hdmi_hpd_enable,
	.hpd_disable = s7d_hdmi_hpd_disable,
};

int s7d_hdmi_bridge_link(struct drm_bridge *bridge, struct s7d_vpu_link *link)
{
	if (!bridge || bridge->funcs != &s7d_hdmi_bridge_funcs || !link)
		return -EINVAL;
	*link = (struct s7d_vpu_link) {
		.quiesce = s7d_hdmi_quiesce,
		.prepare = s7d_hdmi_prepare,
		.data = to_s7d_hdmi(bridge),
	};
	return 0;
}

static int s7d_hdmi_ack_hpd(struct s7d_hdmi *h)
{
	unsigned int status;
	bool pending = false;
	int ret;

	for (unsigned int retry = 0; retry < 4; retry++) {
		ret = regmap_read(h->top, TOP_STATUS, &status);
		if (ret)
			return ret;
		if (status & ~TOP_STATUS_VALID)
			return -EIO;
		if (!(status & HPD_IRQS))
			return pending;
		pending = true;
		ret = regmap_write(h->top, TOP_CLEAR, status & HPD_IRQS);
		if (ret)
			return ret;
	}
	return -EIO;
}

static void s7d_hdmi_audio_hpd(struct s7d_hdmi *h)
{
	int ret;

	mutex_lock(&h->lock);
	h->sink_generation++;
	cec_notifier_phys_addr_invalidate(h->cec_notifier);
	h->eld_valid = false;
	h->audio_valid = false;
	if (h->audio_clock_on) {
		ret = s7d_hdmi_audio_disable(h->core);
		if (ret) {
			h->audio_error = ret;
			dev_err_ratelimited(h->dev, "HDMI audio stop on HPD failed: %d\n", ret);
		}
	}
	mutex_unlock(&h->lock);
	schedule_work(&h->audio_notify);
}

static void s7d_hdmi_hpd_recovery(struct work_struct *work)
{
	struct s7d_hdmi *h = container_of(to_delayed_work(work),
					struct s7d_hdmi, hpd_recovery);
	int ret;

	mutex_lock(&h->lock);
	if (h->stopping) {
		mutex_unlock(&h->lock);
		return;
	}
	ret = s7d_hdmi_ack_hpd(h);
	if (ret < 0) {
		mod_delayed_work(system_wq, &h->hpd_recovery, HZ);
		mutex_unlock(&h->lock);
		return;
	}
	enable_irq(h->irq);
	mutex_unlock(&h->lock);
	s7d_hdmi_audio_hpd(h);
	if (READ_ONCE(h->hpd_enabled))
		drm_bridge_hpd_notify(&h->bridge, s7d_hdmi_detect(&h->bridge));
}

static bool s7d_hdmi_scdc_active(struct s7d_hdmi *h, u8 config)
{
	lockdep_assert_held(&h->lock);
	return !h->stopping && h->phy_on && h->scdc_config && h->scdc_config == config;
}

/*
 * A sink that power cycles or leaves standby with HPD kept high forgets the
 * scrambling setting; restore it like vc4 instead of leaving no picture.
 * The status read runs without h->lock so a silent or unplugged sink cannot
 * hold up HPD, audio or commits behind DDC timeouts.
 */
static void s7d_hdmi_scdc_check(struct work_struct *work)
{
	struct s7d_hdmi *h = container_of(to_delayed_work(work),
					struct s7d_hdmi, scdc_check);
	u8 config, status;

	mutex_lock(&h->lock);
	config = h->scdc_config;
	if (!s7d_hdmi_scdc_active(h, config)) {
		mutex_unlock(&h->lock);
		return;
	}
	mutex_unlock(&h->lock);

	if (s7d_hdmi_detect(&h->bridge) == connector_status_connected &&
	    (drm_scdc_readb(&h->ddc.adapter, SCDC_SCRAMBLER_STATUS, &status) ||
	     !(status & SCDC_SCRAMBLING_STATUS))) {
		/* Quiesce or a new mode may have changed the link meanwhile. */
		mutex_lock(&h->lock);
		if (s7d_hdmi_scdc_active(h, config) &&
		    !drm_scdc_writeb(&h->ddc.adapter, SCDC_SOURCE_VERSION, 1) &&
		    !drm_scdc_writeb(&h->ddc.adapter, SCDC_TMDS_CONFIG, config))
			dev_info_ratelimited(h->dev, "sink lost SCDC scrambling; restored\n");
		mutex_unlock(&h->lock);
	}

	mutex_lock(&h->lock);
	if (s7d_hdmi_scdc_active(h, config))
		mod_delayed_work(system_wq, &h->scdc_check, HZ);
	mutex_unlock(&h->lock);
}

static irqreturn_t s7d_hdmi_irq(int irq, void *data)
{
	struct s7d_hdmi *h = data;
	int ret = s7d_hdmi_ack_hpd(h);

	if (!ret)
		return IRQ_NONE;
	s7d_hdmi_audio_hpd(h);
	if (ret < 0) {
		disable_irq_nosync(irq);
		dev_err_ratelimited(h->dev, "HPD acknowledgment failed: %d; retrying with IRQ masked\n",
				    ret);
		if (!READ_ONCE(h->stopping))
			mod_delayed_work(system_wq, &h->hpd_recovery, HZ);
	}
	if (READ_ONCE(h->hpd_enabled))
		drm_bridge_hpd_notify(&h->bridge, ret < 0 ? connector_status_unknown :
				      s7d_hdmi_detect(&h->bridge));
	return IRQ_HANDLED;
}

static void s7d_hdmi_shutdown(struct platform_device *pdev)
{
	struct s7d_hdmi *h = platform_get_drvdata(pdev);

	mutex_lock(&h->lock);
	WRITE_ONCE(h->stopping, true);
	cec_notifier_phys_addr_invalidate(h->cec_notifier);
	WRITE_ONCE(h->hpd_enabled, false);
	disable_irq_nosync(h->irq);
	mutex_unlock(&h->lock);
	synchronize_irq(h->irq);
	cancel_delayed_work_sync(&h->hpd_recovery);
	cancel_delayed_work_sync(&h->scdc_check);
	cancel_work_sync(&h->audio_notify);
	s7d_hdmi_audio_shutdown(h->dev, h);
	s7d_hdmi_bridge_set_connector(&h->bridge, NULL);
}

static const char * const s7d_hdmi_spd_types[] = {
	[HDMI_SPD_SDI_UNKNOWN] = "unknown",
	[HDMI_SPD_SDI_DSTB] = "stb",
	[HDMI_SPD_SDI_DVDP] = "dvd",
	[HDMI_SPD_SDI_DVHS] = "d-vhs",
	[HDMI_SPD_SDI_HDDVR] = "hdd",
	[HDMI_SPD_SDI_DVC] = "dvc",
	[HDMI_SPD_SDI_DSC] = "dsc",
	[HDMI_SPD_SDI_VCD] = "vcd",
	[HDMI_SPD_SDI_GAME] = "game",
	[HDMI_SPD_SDI_PC] = "pc",
	[HDMI_SPD_SDI_BD] = "bd",
	[HDMI_SPD_SDI_SACD] = "sacd",
	[HDMI_SPD_SDI_HDDVD] = "hd-dvd",
	[HDMI_SPD_SDI_PMP] = "pmp",
};

static int s7d_hdmi_init_spd(struct s7d_hdmi *h)
{
	const char *vendor = "Amlogic", *product = "S7D";
	struct hdmi_spd_infoframe spd;
	int sdi, ret;

	device_property_read_string(h->dev, "amlogic,spd-vendor", &vendor);
	device_property_read_string(h->dev, "amlogic,spd-product", &product);
	/* Like drmm_connector_hdmi_init(), refuse names the frame would cut. */
	if (strlen(vendor) > sizeof(spd.vendor) ||
	    strlen(product) > sizeof(spd.product))
		return dev_err_probe(h->dev, -EINVAL,
				     "SPD vendor or product name too long\n");
	sdi = device_property_match_property_string(h->dev,
						    "amlogic,spd-source-type",
						    s7d_hdmi_spd_types,
						    ARRAY_SIZE(s7d_hdmi_spd_types));
	if (sdi == -EINVAL)
		sdi = HDMI_SPD_SDI_PC;
	else if (sdi < 0)
		return dev_err_probe(h->dev, sdi, "SPD source type\n");

	ret = hdmi_spd_infoframe_init(&spd, vendor, product);
	if (ret)
		return ret;
	spd.sdi = sdi;
	ret = hdmi_spd_infoframe_pack(&spd, h->spd, sizeof(h->spd));
	return ret < 0 ? ret : 0;
}

static int s7d_hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s7d_hdmi *h;
	struct hdmi_codec_pdata audio = {
		.ops = &s7d_hdmi_codec_ops,
		.i2s = 1,
		.spdif = 1,
		.no_i2s_capture = 1,
		.no_spdif_capture = 1,
		.max_i2s_channels = 2,
	};
	unsigned int value, i;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	mutex_init(&h->lock);
	mutex_init(&h->audio_callback_lock);
	INIT_WORK(&h->audio_notify, s7d_hdmi_audio_notify);
	h->audio_muted = true;
	INIT_DELAYED_WORK(&h->hpd_recovery, s7d_hdmi_hpd_recovery);
	INIT_DELAYED_WORK(&h->scdc_check, s7d_hdmi_scdc_check);
	ret = s7d_hdmi_init_spd(h);
	if (ret)
		return ret;
	ret = s7d_hdmi_init_regmaps(pdev, &h->core, &h->top);
	if (ret)
		return dev_err_probe(dev, ret, "TX register resources\n");
	h->basic[0].id = "apb";
	h->basic[1].id = "sys";
	h->basic[2].id = "prif";
	h->basic[3].id = "200m";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(h->basic), h->basic);
	if (ret)
		return dev_err_probe(dev, ret, "DDC/HPD clocks\n");
	h->audio_clk = devm_clk_get(dev, "aud");
	if (IS_ERR(h->audio_clk))
		return dev_err_probe(dev, PTR_ERR(h->audio_clk), "audio clock\n");
	h->video[0].id = "pixel";
	h->video[1].id = "fe";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(h->video), h->video);
	if (ret)
		return dev_err_probe(dev, ret, "video clocks\n");
	h->video_mux[0].id = "pixel-mux";
	h->video_mux[1].id = "fe-mux";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(h->video_mux), h->video_mux);
	if (ret)
		return dev_err_probe(dev, ret, "video mux clocks\n");
	h->video_parent = devm_clk_get(dev, "video-parent");
	if (IS_ERR(h->video_parent))
		return dev_err_probe(dev, PTR_ERR(h->video_parent), "video parent clock\n");
	h->video_div2 = devm_clk_get(dev, "video-div2");
	if (IS_ERR(h->video_div2))
		return dev_err_probe(dev, PTR_ERR(h->video_div2), "video div2 clock\n");
	h->resets[0].id = "tx";
	h->resets[1].id = "apb";
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(h->resets), h->resets);
	if (ret)
		return dev_err_probe(dev, ret, "TX resets\n");
	/* Initial retained-state profile: never deassert a cold block speculatively. */
	for (i = 0; i < ARRAY_SIZE(h->resets); i++) {
		ret = reset_control_status(h->resets[i].rstc);
		if (ret)
			return dev_err_probe(dev, ret < 0 ? ret : -EHOSTDOWN, "TX held in reset\n");
	}
	h->phy = devm_phy_get(dev, "hdmi");
	if (IS_ERR(h->phy))
		return dev_err_probe(dev, PTR_ERR(h->phy), "HDMI PHY\n");
	h->irq = platform_get_irq(pdev, 0);
	if (h->irq < 0)
		return h->irq;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	/* A published mux can remain orphaned until its SCMI parent registers. */
	for (i = 1; i < ARRAY_SIZE(h->basic); i++) {
		unsigned long rate = clk_get_rate(h->basic[i].clk);

		ret = s7d_hdmi_ddc_clock_check(rate);
		if (ret) {
			dev_err_probe(dev, ret, "%s clock not ready for 200 MHz profile: %lu Hz\n",
				      h->basic[i].id, rate);
			goto put_pm;
		}
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(h->basic), h->basic);
	if (ret)
		goto put_pm;
	ret = regmap_read(h->top, TOP_RESET, &value);
	if (ret || value) {
		ret = ret ? ret : -EHOSTDOWN;
		goto disable_basic;
	}
	/* Refuse firmware protection; this driver never owns HDCP authentication. */
	ret = regmap_read(h->core, CORE_HDCP1, &value);
	if (ret || (value & (BIT(7) | BIT(0)))) {
		ret = ret ? ret : -EOPNOTSUPP;
		goto disable_basic;
	}
	ret = regmap_read(h->core, CORE_HDCP2, &value);
	if (ret || (value & (BIT(7) | BIT(0)))) {
		ret = ret ? ret : -EOPNOTSUPP;
		goto disable_basic;
	}
	ret = s7d_hdmi_phy_is_enabled(h->phy);
	if (ret < 0)
		goto disable_basic;
	if (ret) {
		ret = clk_bulk_prepare_enable(ARRAY_SIZE(h->video), h->video);
		if (ret)
			goto disable_basic;
		h->video_on = true;
	}
	ret = top_write(h, TOP_MASK, 0x1ff, 0);
	if (ret)
		goto disable_video;
	ret = top_write(h, TOP_SCRATCH, U32_MAX, 1);
	if (ret)
		goto disable_video;
	ret = top_write(h, TOP_HPD_FILTER, U32_MAX, 0x80007000);
	if (ret)
		goto disable_video;
	ret = top_write(h, TOP_INFILTER, 0x07ff0000, BIT(24));
	if (ret)
		goto disable_video;
	/*
	 * The level TX line also carries the core INTR2/INTR5 shadows, which
	 * nothing here handles; mask any source left on before the IRQ starts.
	 * This is defensive, so a failed write only warns.
	 */
	if (regmap_write(h->core, CORE_INTR2_MASK, 0) ||
	    regmap_write(h->core, CORE_INTR5_MASK, 0))
		dev_warn(dev, "could not mask core TX interrupts\n");
	ret = s7d_hdmi_ddc_register(dev, &h->ddc, h->core);
	if (ret)
		goto disable_video;
	ret = devm_request_threaded_irq(dev, h->irq, NULL, s7d_hdmi_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(dev), h);
	if (ret)
		goto remove_ddc;
	h->bridge.funcs = &s7d_hdmi_bridge_funcs;
	h->bridge.of_node = dev->of_node;
	h->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	h->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;
	/* ENCP drives 1080i; s7d_encp_build_state() rejects the others. */
	h->bridge.interlace_allowed = true;
	h->bridge.ddc = &h->ddc.adapter;
	ret = regmap_write(h->top, TOP_CLEAR, 0x1ff);
	if (ret)
		goto remove_ddc;
	ret = top_write(h, TOP_MASK, 0x1ff, HPD_IRQS);
	if (ret)
		goto remove_ddc;
	audio.data = h;
	h->codec = platform_device_register_data(dev, HDMI_CODEC_DRV_NAME,
					      PLATFORM_DEVID_AUTO, &audio, sizeof(audio));
	if (IS_ERR(h->codec)) {
		ret = PTR_ERR(h->codec);
		goto remove_ddc;
	}
	ret = devm_add_action_or_reset(dev, s7d_hdmi_codec_unregister, h);
	if (ret)
		goto remove_ddc;
	/* Last fallible steps: preserve an adopted live PHY. */
	if (h->video_on) {
		ret = phy_init(h->phy);
		if (ret)
			goto remove_ddc;
		h->phy_initialized = true;
		ret = phy_power_on(h->phy);
		if (ret) {
			int cleanup = s7d_hdmi_phy_exit(h);

			if (cleanup)
				dev_err(dev, "HDMI PHY adoption exit failed: %d\n", cleanup);
			goto remove_ddc;
		}
		h->phy_on = true;
	}
	platform_set_drvdata(pdev, h);
	drm_bridge_add(&h->bridge);
	enable_irq(h->irq);
	return 0;
remove_ddc:
	cancel_work_sync(&h->audio_notify);
	s7d_hdmi_ddc_unregister(&h->ddc);
disable_video:
	if (h->video_on)
		clk_bulk_disable_unprepare(ARRAY_SIZE(h->video), h->video);
disable_basic:
	clk_bulk_disable_unprepare(ARRAY_SIZE(h->basic), h->basic);
put_pm:
	pm_runtime_put(dev);
	return dev_err_probe(dev, ret, "HDMI initialization\n");
}

/* Static DT and built-in driver only until failed-stop removal can retain devres. */
static const struct of_device_id s7d_hdmi_match[] = {
	{ .compatible = "amlogic,s7d-hdmi-tx" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_hdmi_match);

static struct platform_driver s7d_hdmi_driver = {
	.probe = s7d_hdmi_probe,
	.shutdown = s7d_hdmi_shutdown,
	.driver = {
		.name = "s7d-hdmi-tx",
		.of_match_table = s7d_hdmi_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(s7d_hdmi_driver);

MODULE_DESCRIPTION("Amlogic S7D HDMI transmitter");
MODULE_LICENSE("GPL");
