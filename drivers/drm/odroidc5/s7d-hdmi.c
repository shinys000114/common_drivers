// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
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
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/workqueue.h>
#include <sound/hdmi-codec.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>
#include <drm/drm_probe_helper.h>
#include <drm/display/drm_scdc_helper.h>

#include "s7d-hdmi.h"
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
#define CORE_TPI_SC	0x061a
#define CORE_INFO_SELECT 0x06bf
#define CORE_INFO_DATA	0x06c0
#define CORE_INFO_ENABLE 0x06df
#define CORE_INFO_TRANSMIT (BIT(7) | BIT(6)) /* enable and per-frame repeat */
#define CORE_HDCP1	0x062a
#define CORE_HDCP2	0x08bd
#define CORE_AUDIO_EN	0x0a13

static bool trace_tx;
module_param(trace_tx, bool, 0600);
MODULE_PARM_DESC(trace_tx, "Log TX configuration entry/return to diagnose stalled secure accesses");

struct s7d_hdmi_state {
	struct drm_bridge_state base;
	struct drm_display_mode mode;
	u8 avi[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AVI_INFOFRAME_SIZE];
	u8 vendor[HDMI_INFOFRAME_SIZE(MAX)];
	u8 vendor_len;
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
	struct reset_control_bulk_data resets[2];
	struct phy *phy;
	int irq;
	bool phy_on;
	bool video_on;
	bool prepared;
	bool hpd_enabled;
	bool stopping;
	struct delayed_work hpd_recovery;
	u8 eld[MAX_ELD_BYTES];
	struct platform_device *codec;
	struct clk *audio_clk;
	struct hdmi_codec_params audio_params;
	bool audio_clock_on;
	bool audio_valid;
	bool audio_muted;
	bool eld_valid;
	int audio_error;
	/* Serializes callback registration against notification. */
	struct mutex audio_callback_lock;
	hdmi_codec_plugged_cb audio_plugged;
	struct device *codec_dev;
	struct work_struct audio_notify;
	struct mutex connector_lock;
	struct drm_connector *connector;
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
	/* 100 ppm still keeps the 75 kHz DDC profile below the 100 kHz limit. */
	if (!rate)
		return -EPROBE_DEFER;
	return rate >= 199980000 && rate <= 200020000 ? 0 : -ERANGE;
}

static int s7d_hdmi_audio_apply(struct s7d_hdmi *h);

static void s7d_hdmi_refresh_eld(struct s7d_hdmi *h)
{
	struct drm_connector *connector;
	struct drm_device *drm;
	bool valid;

	mutex_lock(&h->lock);
	valid = h->eld_valid || h->stopping;
	mutex_unlock(&h->lock);
	if (valid)
		return;
	mutex_lock(&h->connector_lock);
	connector = h->connector;
	if (connector) {
		drm = connector->dev;
		mutex_lock(&drm->mode_config.mutex);
		drm_helper_probe_single_connector_modes(connector, drm->mode_config.max_width,
						      drm->mode_config.max_height);
		mutex_unlock(&drm->mode_config.mutex);
	}
	mutex_unlock(&h->connector_lock);
}

void s7d_hdmi_bridge_set_connector(struct drm_bridge *bridge, struct drm_connector *connector)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);

	/* DRM clears this pointer before its managed connector cleanup. */
	mutex_lock(&h->connector_lock);
	h->connector = connector;
	mutex_unlock(&h->connector_lock);
}

static int s7d_hdmi_audio_get_eld(struct device *dev, void *data, u8 *buf, size_t len)
{
	struct s7d_hdmi *h = data;
	int ret = -ENODEV;

	s7d_hdmi_refresh_eld(h);
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

	ret = s7d_hdmi_audio_configure(h->core, h->top, &h->audio_params);
	if (!ret)
		ret = s7d_hdmi_audio_mute(h->core, h->audio_muted);
	if (ret) {
		s7d_hdmi_audio_disable(h->core);
		h->audio_error = ret;
		dev_err(h->dev, "HDMI audio setup failed: %d\n", ret);
	}
	return ret;
}

static int s7d_hdmi_audio_prepare(struct device *dev, void *data,
				struct hdmi_codec_daifmt *fmt,
				struct hdmi_codec_params *params)
{
	struct s7d_hdmi *h = data;
	int ret;

	if (fmt->fmt != HDMI_I2S || fmt->bit_clk_inv || fmt->frame_clk_inv ||
	    fmt->bit_clk_provider || fmt->frame_clk_provider ||
	    (fmt->bit_fmt != SNDRV_PCM_FORMAT_S16_LE &&
	     fmt->bit_fmt != SNDRV_PCM_FORMAT_S24_LE))
		return -EINVAL;
	s7d_hdmi_refresh_eld(h);
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
	if (!mute)
		s7d_hdmi_refresh_eld(h);
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
	.prepare = s7d_hdmi_audio_prepare,
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
	bool trace = READ_ONCE(trace_tx);

	/* Log before access too: firmware/MMIO can stall without returning errno. */
	if (trace)
		dev_info(h->dev, "TX %s reg %#x mask %#x value %#x begin\n",
			 map == h->core ? "core" : "top", reg, mask, value);
	ret = s7d_hdmi_update_checked(map, reg, mask, value);
	if (trace)
		dev_info(h->dev, "TX %s reg %#x end: %d\n",
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

static int s7d_hdmi_quiesce(void *data)
{
	struct s7d_hdmi *h = data;
	int ret = 0;

	mutex_lock(&h->lock);
	h->prepared = false;
	if (h->audio_valid) {
		ret = s7d_hdmi_audio_disable(h->core);
		if (ret) {
			h->audio_error = ret;
			dev_err(h->dev, "HDMI audio stop before modeset failed: %d\n", ret);
			goto out;
		}
	}
	if (h->phy_on) {
		ret = phy_power_off(h->phy);
		if (ret)
			goto out;
		h->phy_on = false;
	}
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
	if (ret || !state->vendor_len)
		return ret;
	return s7d_hdmi_packet(h, 5, state->vendor, state->vendor_len);
}

static int s7d_hdmi_setup_tx(struct s7d_hdmi *h, const struct s7d_hdmi_state *state)
{
	/* S7D branch of config_hdmi21_tx: progressive RGB8, no FRL/deep colour. */
	static const struct reg_sequence config[] = {
		{ 0x0236, 0 }, /* P2T_CTRL: 8 bpc, TMDS */
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
		{ 0x06e1, 0 }, /* GCP non-merge for 8 bpc */
	};
	const u8 mapping[] = { 0x40, 0x04 };
	u8 actual[sizeof(mapping)];
	unsigned int i;
	int ret;

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
	if (READ_ONCE(trace_tx))
		dev_info(h->dev, "TX TMDS pattern load begin\n");
	ret = regmap_write(h->top, TOP_PATTERN_CTRL, BIT(0));
	if (READ_ONCE(trace_tx))
		dev_info(h->dev, "TX TMDS pattern load end: %d\n", ret);
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
	for (i = 0; i < ARRAY_SIZE(config); i++) {
		ret = core_write(h, config[i].reg, config[i].def);
		if (ret)
			return ret;
	}
	/* Write the complete two-byte mapping before checking its readback. */
	ret = regmap_bulk_write(h->core, 0x0b44, mapping, sizeof(mapping));
	if (ret)
		return ret;
	ret = regmap_bulk_read(h->core, 0x0b44, actual, sizeof(actual));
	if (ret)
		return ret;
	if (memcmp(actual, mapping, sizeof(mapping))) {
		dev_err(h->dev, "HDMI output mapping readback %#x%02x, expected 0x0440\n",
			actual[1], actual[0]);
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
	ret = top_write(h, TOP_ACTIVE, 0x7fff7fff,
			(state->mode.vdisplay << 16) | state->mode.hdisplay);
	if (ret)
		return ret;
	/* Video FIFO reset only; no shared TX/DDC/controller/PHY reset. */
	ret = s7d_hdmi_write_checked(h, h->core, CORE_PWD_RESET, 6, 6);
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, CORE_PWD_RESET, 6, 0);
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, CORE_TPI_SC, 0x89, state->hdmi ? 1 : 0);
	if (ret || !state->hdmi)
		return ret;
	return s7d_hdmi_write_checked(h, h->core, CORE_TPI_SC, BIT(7), BIT(7));
}

static int s7d_hdmi_prepare(void *data, unsigned long pixel_rate)
{
	struct s7d_hdmi *h = data;
	struct s7d_hdmi_state *state;
	unsigned int i;
	int ret;

	if (!h->bridge.base.state)
		return -EINVAL;
	state = to_s7d_hdmi_state(drm_priv_to_bridge_state(h->bridge.base.state));
	if (!state->valid)
		return -EINVAL;
	mutex_lock(&h->lock);
	ret = -EBUSY;
	if (h->phy_on || h->video_on)
		goto out;
	/* VPU protects the ENCP parent rate while we prepare sibling branches. */
	for (i = 0; i < ARRAY_SIZE(h->video); i++) {
		ret = clk_set_rate(h->video[i].clk, pixel_rate);
		if (ret)
			goto out;
		if (clk_get_rate(h->video[i].clk) != pixel_rate) {
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
	if (!ret)
		h->prepared = true;
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

	mutex_lock(&h->lock);
	h->edid_generation = h->sink_generation;
	mutex_unlock(&h->lock);
	if (s7d_hdmi_detect(bridge) != connector_status_connected)
		return NULL;
	return drm_edid_read_ddc(connector, &h->ddc.adapter);
}

static enum drm_mode_status s7d_hdmi_mode_valid(struct drm_bridge *bridge,
					      const struct drm_display_info *info,
					      const struct drm_display_mode *mode)
{
	struct s7d_encp_state state;

	if (mode->clock > 594000 || mode->hdisplay > 4096 || mode->vdisplay > 2160)
		return MODE_CLOCK_HIGH;
	if (drm_mode_is_420_only(info, mode))
		return MODE_NO_420;
	if (!info->is_hdmi && mode->clock > 165000)
		return MODE_CLOCK_HIGH;
	if (mode->clock > 340000 &&
	    (!info->is_hdmi || !info->hdmi.scdc.supported ||
	     !info->hdmi.scdc.scrambling.supported))
		return MODE_CLOCK_HIGH;
	if (info->max_tmds_clock && mode->clock > info->max_tmds_clock)
		return MODE_CLOCK_HIGH;
	return s7d_encp_build_state(mode, &state);
}

static int s7d_hdmi_atomic_check(struct drm_bridge *bridge, struct drm_bridge_state *base,
				 struct drm_crtc_state *crtc_state,
				 struct drm_connector_state *conn_state)
{
	struct s7d_hdmi_state *state = to_s7d_hdmi_state(base);
	struct drm_connector *connector = conn_state->connector;
	struct hdmi_avi_infoframe avi;
	struct hdmi_vendor_infoframe vendor;
	int ret;

	state->valid = false;
	if (!crtc_state->active)
		return 0;
	if (s7d_hdmi_mode_valid(bridge, &connector->display_info,
				&crtc_state->adjusted_mode) != MODE_OK ||
	    conn_state->hdr_output_metadata ||
	    conn_state->colorspace != DRM_MODE_COLORIMETRY_DEFAULT)
		return -EINVAL;
	state->mode = crtc_state->adjusted_mode;
	state->hdmi = connector->display_info.is_hdmi;
	state->scdc = state->hdmi && connector->display_info.hdmi.scdc.supported;
	state->high_tmds = state->mode.clock > 340000;
	state->vendor_len = 0;
	if (state->hdmi) {
		ret = drm_hdmi_avi_infoframe_from_display_mode(&avi, connector, &state->mode);
		if (ret)
			return ret;
		avi.colorspace = HDMI_COLORSPACE_RGB;
		avi.quantization_range = HDMI_QUANTIZATION_RANGE_FULL;
		ret = hdmi_avi_infoframe_pack(&avi, state->avi, sizeof(state->avi));
		if (ret < 0)
			return ret;
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
	base->input_bus_cfg.format = MEDIA_BUS_FMT_RGB888_1X24;
	state->valid = true;
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

	if (!state->scdc)
		return state->high_tmds ? -EINVAL : 0;
	ret = drm_scdc_readb(&h->ddc.adapter, SCDC_SINK_VERSION, &value);
	if (ret)
		return ret;
	if (!value)
		return -EINVAL;
	ret = drm_scdc_writeb(&h->ddc.adapter, SCDC_SOURCE_VERSION, 1);
	if (ret)
		return ret;
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
	ret = s7d_hdmi_setup_scdc(h, state);
	if (ret)
		goto out;
	ret = phy_power_on(h->phy);
	if (!ret) {
		h->phy_on = true;
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

static irqreturn_t s7d_hdmi_irq(int irq, void *data)
{
	struct s7d_hdmi *h = data;
	int ret = s7d_hdmi_ack_hpd(h);

	if (!ret)
		return IRQ_NONE;
	s7d_hdmi_audio_hpd(h);
	if (ret < 0) {
		disable_irq_nosync(irq);
		dev_err_ratelimited(h->dev, "HPD acknowledgement failed: %d; retrying with IRQ masked\n",
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
	WRITE_ONCE(h->hpd_enabled, false);
	disable_irq_nosync(h->irq);
	mutex_unlock(&h->lock);
	synchronize_irq(h->irq);
	cancel_delayed_work_sync(&h->hpd_recovery);
	cancel_work_sync(&h->audio_notify);
	s7d_hdmi_audio_shutdown(h->dev, h);
}

static int s7d_hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s7d_hdmi *h;
	struct hdmi_codec_pdata audio = {
		.ops = &s7d_hdmi_codec_ops,
		.i2s = 1,
		.no_i2s_capture = 1,
		.max_i2s_channels = 2,
	};
	unsigned int value, i;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	mutex_init(&h->lock);
	mutex_init(&h->connector_lock);
	mutex_init(&h->audio_callback_lock);
	INIT_WORK(&h->audio_notify, s7d_hdmi_audio_notify);
	h->audio_muted = true;
	INIT_DELAYED_WORK(&h->hpd_recovery, s7d_hdmi_hpd_recovery);
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
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(h->video), h->video);
	if (ret)
		goto disable_basic;
	h->video_on = true;
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
	/* Last fallible steps: no probe unwind may free an adopted live PHY. */
	ret = phy_init(h->phy);
	if (ret)
		goto remove_ddc;
	ret = phy_power_on(h->phy);
	if (ret) {
		phy_exit(h->phy);
		goto remove_ddc;
	}
	h->phy_on = true;
	platform_set_drvdata(pdev, h);
	drm_bridge_add(&h->bridge);
	enable_irq(h->irq);
	return 0;
remove_ddc:
	cancel_work_sync(&h->audio_notify);
	s7d_hdmi_ddc_unregister(&h->ddc);
disable_video:
	clk_bulk_disable_unprepare(ARRAY_SIZE(h->video), h->video);
disable_basic:
	clk_bulk_disable_unprepare(ARRAY_SIZE(h->basic), h->basic);
put_pm:
	pm_runtime_put(dev);
	return dev_err_probe(dev, ret, "HDMI initialization\n");
}

/* Static DT and built-in driver only until failed-stop removal can retain devres. */
static const struct of_device_id s7d_hdmi_match[] = {
	{ .compatible = "amlogic,s7d-hdmi-native" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_hdmi_match);

static struct platform_driver s7d_hdmi_driver = {
	.probe = s7d_hdmi_probe,
	.shutdown = s7d_hdmi_shutdown,
	.driver = {
		.name = "s7d-hdmi-native",
		.of_match_table = s7d_hdmi_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(s7d_hdmi_driver);

MODULE_DESCRIPTION("Amlogic S7D native TMDS bridge");
MODULE_LICENSE("GPL");
