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
#include <drm/drm_drv.h>
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
#include "s7d-hooks.h"
#include "s7d-vendor-props.h"
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
#define TOP_HS_INTR	0x084
#define TOP_ACTIVE	0x08c
#define TOP_I2C_BUSY	0x0a0
#define HPD_IRQS	(BIT(1) | BIT(2))
/* TOP_INTR_STAT[31:30] are read-only core IRQ shadows, not reserved bits. */
#define TOP_STATUS_VALID	(GENMASK(31, 30) | GENMASK(8, 0))
#define CORE_PWD_RESET	0x0110
#define CORE_SYS_STAT	0x0201
#define CORE_INTR2	0x0265
#define CORE_INTR2_MASK	0x026b
#define CORE_INTR5_MASK	0x026d
#define CORE_GCP_AUTO	0x06e3
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
	/* The vendor AV mute of the commit */
	bool avmute;
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
	/* The HPD handler masked its IRQ after a failed acknowledgment. */
	bool hpd_masked;
	/*
	 * Serializes hotplug reports, from the detection to the vendor hook.
	 * Taken before mode_config.mutex (then eld_mutex, then lock) and
	 * before the bridge's hpd_mutex (then mode_config.mutex).
	 */
	struct mutex hpd_lock;
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
	/* Between the PM prepare and complete phases of system sleep */
	bool sleeping;
	/* Sleep cut a stream off; accept it back until the link is up. */
	bool audio_resume;
	/* Serializes callback registration against notification. */
	struct mutex audio_callback_lock;
	hdmi_codec_plugged_cb audio_plugged;
	struct device *codec_dev;
	/* The codec is going away: report nothing more to its jack. */
	bool audio_detached;
	struct work_struct audio_notify;
	struct drm_connector *connector;
	/* The status last passed to the vendor hook for that connector; under lock. */
	enum drm_connector_status vendor_status;
	struct cec_notifier *cec_notifier;
	struct hdmi_spd_infoframe spd_frame;
	u8 spd[HDMI_INFOFRAME_HEADER_SIZE + HDMI_SPD_INFOFRAME_SIZE];
	unsigned int sink_generation;
	unsigned int edid_generation;
	/* What a fastboot readout found the firmware link sends */
	struct {
		bool valid;
		bool hdmi;
		/* TPI packet banks transmitted, and the contents of 0, 3 and 5 */
		u16 banks;
		u8 avi[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AVI_INFOFRAME_SIZE];
		u8 spd[HDMI_INFOFRAME_HEADER_SIZE + HDMI_SPD_INFOFRAME_SIZE];
		u8 vendor[HDMI_INFOFRAME_SIZE(MAX)];
		u8 vendor_len;
	} boot;
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
	h->connector = connector;
	h->vendor_status = connector_status_unknown;
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
	if (h->audio_plugged && !h->audio_detached) {
		plugged = !s7d_hdmi_audio_get_eld(h->dev, h, eld, sizeof(eld));
		h->audio_plugged(h->codec_dev, plugged);
	}
	mutex_unlock(&h->audio_callback_lock);
}

/*
 * hdmi-codec reports what the TX tells it to its card's jack until the
 * card unhooks it, which happens only after ALSA has started tearing the
 * jack down. Stop reporting, and wait for a report in flight, before the
 * codec goes away.
 */
static void s7d_hdmi_audio_detach(struct s7d_hdmi *h)
{
	mutex_lock(&h->audio_callback_lock);
	h->audio_detached = true;
	mutex_unlock(&h->audio_callback_lock);
	cancel_work_sync(&h->audio_notify);
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

/* Keep the stream's audio InfoFrame where DRM debugfs reads it, as upstream does. */
static void s7d_hdmi_audio_infoframe(struct s7d_hdmi *h,
				     const struct hdmi_audio_infoframe *frame)
{
	struct drm_connector_hdmi_infoframe *infoframe;

	lockdep_assert_held(&h->lock);
	if (!h->connector)
		return;
	infoframe = &h->connector->hdmi.infoframes.audio;
	mutex_lock(&h->connector->hdmi.infoframes.lock);
	if (frame)
		infoframe->data.audio = *frame;
	infoframe->set = !!frame;
	mutex_unlock(&h->connector->hdmi.infoframes.lock);
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
	bool resume;
	int ret;

	if ((fmt->fmt != HDMI_I2S && fmt->fmt != HDMI_SPDIF) ||
	    fmt->bit_clk_inv || fmt->frame_clk_inv ||
	    fmt->bit_clk_provider || fmt->frame_clk_provider ||
	    (fmt->bit_fmt != SNDRV_PCM_FORMAT_S16_LE &&
	     fmt->bit_fmt != SNDRV_PCM_FORMAT_S24_LE &&
	     fmt->bit_fmt != SNDRV_PCM_FORMAT_S32_LE))
		return -EINVAL;
	mutex_lock(&h->lock);
	/*
	 * The sound card resumes a stream that sleep cut off when its own
	 * turn comes, which may be before the display enables the link.
	 */
	resume = h->audio_resume;
	h->audio_resume = false;
	if (!h->eld_valid || (!resume && (!h->prepared || !h->phy_on))) {
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
	if (h->prepared && h->phy_on) {
		ret = s7d_hdmi_audio_apply(h);
		h->audio_valid = !ret;
	} else {
		/* The link enable applies them, as it does after a modeset. */
		h->audio_valid = false;
		ret = 0;
	}
	if (ret) {
		clk_disable_unprepare(h->audio_clk);
		h->audio_clock_on = false;
	} else {
		s7d_hdmi_audio_infoframe(h, &h->audio_params.cea);
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
	/* Without the link, the next link enable unmutes held parameters. */
	else if (!mute && !h->audio_clock_on)
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
	h->audio_resume = h->sleeping && h->audio_clock_on;
	h->audio_valid = false;
	h->audio_muted = true;
	s7d_hdmi_audio_infoframe(h, NULL);
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

/* The stages of s7d_hdmi_setup_tx() in which its register image is written. */
enum s7d_hdmi_tx_stage {
	S7D_HDMI_TX_PATTERN,
	S7D_HDMI_TX_LINK,
	S7D_HDMI_TX_DE,
	/* PCM, Dynamic HDR and EMP off: packet data rather than link setup */
	S7D_HDMI_TX_DATA,
	S7D_HDMI_TX_ACTIVE,
};

struct s7d_hdmi_tx_setting {
	u8 stage;
	bool top;
	u16 reg;
	u32 mask;
	u32 value;
};

#define S7D_HDMI_TX_SETTING_COUNT 25

/*
 * The masked TX writes of s7d_hdmi_setup_tx() for @state, in order, for
 * TOP or core registers; @image takes S7D_HDMI_TX_SETTING_COUNT entries.
 * The TMDS pattern load, the output mapping, the packets, the FIFO resets
 * and the TPI mode come between and after them. Pure. Returns the count or
 * -EINVAL for an output the TX cannot carry.
 */
static int s7d_hdmi_tx_image(const struct s7d_hdmi_state *state,
			     struct s7d_hdmi_tx_setting *image)
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
	unsigned int count = 0, i;
	u8 p2t, merge;

	static_assert(3 + 4 + ARRAY_SIZE(config) + 1 + 3 + 3 + 1 == S7D_HDMI_TX_SETTING_COUNT);
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
#define TX(st, t, r, m, v) (image[count++] = (struct s7d_hdmi_tx_setting) { \
	.stage = (st), .top = (t), .reg = (r), .mask = (m), .value = (v) })
	TX(S7D_HDMI_TX_PATTERN, true, TOP_CLK, 7, 7);
	TX(S7D_HDMI_TX_PATTERN, true, TOP_PATTERN0, U32_MAX,
	   state->high_tmds ? 0 : 0x001f001f);
	TX(S7D_HDMI_TX_PATTERN, true, TOP_PATTERN1, U32_MAX,
	   state->high_tmds ? 0x03ff03ff : 0x001f001f);
	TX(S7D_HDMI_TX_LINK, true, TOP_PATTERN_CTRL, BIT(1), state->high_tmds ? BIT(1) : 0);
	TX(S7D_HDMI_TX_LINK, false, 0x0900, 0xff, BIT(5) | state->high_tmds); /* SCRCTL */
	TX(S7D_HDMI_TX_LINK, true, TOP_BIST, GENMASK(14, 12) | GENMASK(11, 0), BIT(12));
	TX(S7D_HDMI_TX_LINK, false, 0x0236, 0xff, p2t);
	for (i = 0; i < ARRAY_SIZE(config); i++)
		TX(S7D_HDMI_TX_LINK, false, config[i].reg, 0xff,
		   config[i].reg == 0x0b46 ? state->output.tx_mask : config[i].def);
	TX(S7D_HDMI_TX_LINK, false, 0x06e1, 0xff, merge);
	TX(S7D_HDMI_TX_DE, false, 0x012a, 3, 0); /* Original DE */
	TX(S7D_HDMI_TX_DE, false, 0x023c, BIT(6), BIT(6));
	TX(S7D_HDMI_TX_DE, false, 0x023d, BIT(0), 0);
	/* PCM stays disabled until hdmi-codec configures audio explicitly. */
	TX(S7D_HDMI_TX_DATA, false, CORE_AUDIO_EN, 0xff, 0);
	TX(S7D_HDMI_TX_DATA, false, 0x0f00, BIT(0), 0); /* Dynamic HDR */
	TX(S7D_HDMI_TX_DATA, false, 0x0f15, BIT(0), 0); /* EMP insertion */
	/* The transmitter sees repeated pixels: 1440 per line for 480i/576i. */
	TX(S7D_HDMI_TX_ACTIVE, true, TOP_ACTIVE, 0x7fff7fff, (state->mode.vdisplay << 16) |
	   (state->mode.hdisplay << !!(state->mode.flags & DRM_MODE_FLAG_DBLCLK)));
#undef TX
	return count;
}

static int s7d_hdmi_tx_apply(struct s7d_hdmi *h, const struct s7d_hdmi_tx_setting *image,
			     unsigned int count, enum s7d_hdmi_tx_stage stage)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		if (image[i].stage != stage)
			continue;
		ret = s7d_hdmi_write_checked(h, image[i].top ? h->top : h->core, image[i].reg,
					     image[i].mask, image[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

static int s7d_hdmi_setup_tx(struct s7d_hdmi *h, const struct s7d_hdmi_state *state)
{
	struct s7d_hdmi_tx_setting image[S7D_HDMI_TX_SETTING_COUNT];
	const u8 mapping[] = {
		state->output.tx_mapping & 0xff, state->output.tx_mapping >> 8,
	};
	u8 actual[sizeof(mapping)];
	int ret, count;

	count = s7d_hdmi_tx_image(state, image);
	if (count < 0)
		return count;
	ret = s7d_hdmi_tx_apply(h, image, count, S7D_HDMI_TX_PATTERN);
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
	ret = s7d_hdmi_tx_apply(h, image, count, S7D_HDMI_TX_LINK);
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
	ret = s7d_hdmi_tx_apply(h, image, count, S7D_HDMI_TX_DE);
	if (!ret)
		ret = s7d_hdmi_tx_apply(h, image, count, S7D_HDMI_TX_DATA);
	if (ret)
		return ret;
	/* S7D has no accessible DSC packet bank; even a disable read can hang. */
	ret = s7d_hdmi_packets(h, state);
	if (!ret)
		ret = s7d_hdmi_tx_apply(h, image, count, S7D_HDMI_TX_ACTIVE);
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
	/* From both GCP bits clear: Clear_AVMUTE, or Set_AVMUTE for a vendor mute */
	if (state->avmute)
		return regmap_update_bits(h->core, CORE_TPI_SC, BIT(3), BIT(3));
	return s7d_hdmi_write_checked(h, h->core, CORE_TPI_SC, BIT(7), BIT(7));
}

static int s7d_hdmi_mismatch(struct s7d_fastboot_reason *r, const char *cond,
			     const char *what, u32 reg, u64 hw, u64 want)
{
	*r = (struct s7d_fastboot_reason) {
		.cond = cond, .what = what, .reg = reg, .hw = hw, .want = want,
	};
	return -EINVAL;
}

/* The TPI packet banks with their contents, through the bank selector. */
static int s7d_hdmi_read_banks(struct s7d_hdmi *h)
{
	unsigned int bank, i, value;
	u8 data[31];
	int ret;

	h->boot.banks = 0;
	for (bank = 0; bank < 11; bank++) {
		ret = regmap_write(h->core, CORE_INFO_SELECT, bank);
		if (!ret)
			ret = regmap_read(h->core, CORE_INFO_ENABLE, &value);
		if (ret)
			return ret;
		if ((value & CORE_INFO_TRANSMIT) != CORE_INFO_TRANSMIT)
			continue;
		h->boot.banks |= BIT(bank);
		if (bank != 0 && bank != 3 && bank != 5)
			continue;
		for (i = 0; i < sizeof(data); i++) {
			ret = regmap_read(h->core, CORE_INFO_DATA + i, &value);
			if (ret)
				return ret;
			data[i] = value;
		}
		if (bank == 0) {
			memcpy(h->boot.avi, data, sizeof(h->boot.avi));
		} else if (bank == 3) {
			memcpy(h->boot.spd, data, sizeof(h->boot.spd));
		} else {
			h->boot.vendor_len = min_t(unsigned int, sizeof(h->boot.vendor),
						   HDMI_INFOFRAME_HEADER_SIZE + data[2]);
			memcpy(h->boot.vendor, data, h->boot.vendor_len);
		}
	}
	return 0;
}

/*
 * Fastboot readout of the transmitter that the probe adopted: the link
 * setup of s7d_hdmi_setup_tx() and the clocks of s7d_hdmi_prepare() for
 * @output, no AV mute, no HDR metadata, and on HDMI above 340 MHz a sink
 * that scrambles. Records the packets the link sends for the first commit.
 */
static int s7d_hdmi_readout(void *data, const struct drm_display_mode *mode,
			    const struct s7d_hdmi_output_state *output,
			    struct s7d_fastboot_reason *r)
{
	struct s7d_hdmi *h = data;
	struct s7d_hdmi_tx_setting image[S7D_HDMI_TX_SETTING_COUNT];
	struct s7d_hdmi_state state = {
		.mode = *mode, .output = *output, .high_tmds = output->high_tmds,
	};
	unsigned int i, value;
	u8 mapping[2], config;
	int count, ret;

	mutex_lock(&h->lock);
	h->boot.valid = false;
	if (!h->phy_on || !h->phy_initialized || !h->video_on || h->prepared) {
		ret = s7d_hdmi_mismatch(r, "A6", "PHY", 0, h->phy_on, 1);
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(h->video); i++) {
		bool half = !i && output->pixel_divider == 2;
		unsigned long rate = output->rate.pixel / (half ? 2 : 1);

		if (clk_get_rate(h->video[i].clk) != rate ||
		    !clk_is_match(clk_get_parent(h->video_mux[i].clk),
				  half ? h->video_div2 : h->video_parent)) {
			ret = s7d_hdmi_mismatch(r, "A9", "TX clock", i,
						clk_get_rate(h->video[i].clk), rate);
			goto out;
		}
	}
	mutex_lock(&h->ddc.lock);
	ret = regmap_read(h->core, CORE_TPI_SC, &value);
	if (ret)
		goto unlock_ddc;
	/* HDMI or DVI, and on HDMI Clear_AVMUTE sent last */
	state.hdmi = value & BIT(0);
	if ((value & 0x89) != (state.hdmi ? 0x81 : 0)) {
		ret = s7d_hdmi_mismatch(r, "A6", "TX", CORE_TPI_SC, value & 0x89,
					state.hdmi ? 0x81 : 0);
		goto unlock_ddc;
	}
	count = s7d_hdmi_tx_image(&state, image);
	if (count < 0) {
		ret = s7d_hdmi_mismatch(r, "A11", "TX", 0, output->bpc, 8);
		goto unlock_ddc;
	}
	for (i = 0; i < count; i++) {
		/* The first commit stops the firmware's audio, EMP and HDR data. */
		if (image[i].stage == S7D_HDMI_TX_DATA)
			continue;
		ret = regmap_read(image[i].top ? h->top : h->core, image[i].reg, &value);
		if (ret)
			goto unlock_ddc;
		if ((value & image[i].mask) != image[i].value) {
			ret = s7d_hdmi_mismatch(r, "A11", image[i].top ? "TX top" : "TX",
						image[i].reg, value & image[i].mask,
						image[i].value);
			goto unlock_ddc;
		}
	}
	ret = regmap_bulk_read(h->core, 0x0b44, mapping, sizeof(mapping));
	if (ret)
		goto unlock_ddc;
	if ((mapping[0] | mapping[1] << 8) != output->tx_mapping) {
		ret = s7d_hdmi_mismatch(r, "A11", "TX", 0x0b44, mapping[0] | mapping[1] << 8,
					output->tx_mapping);
		goto unlock_ddc;
	}
	ret = s7d_hdmi_read_banks(h);
	if (ret)
		goto unlock_ddc;
	/* No HDR metadata; a DVI link sends no packets at all. */
	if ((h->boot.banks & BIT(6)) || (!state.hdmi && h->boot.banks)) {
		ret = s7d_hdmi_mismatch(r, "A13", "TX bank", CORE_INFO_ENABLE, h->boot.banks,
					state.hdmi ? h->boot.banks & ~BIT(6) : 0);
		goto unlock_ddc;
	}
unlock_ddc:
	mutex_unlock(&h->ddc.lock);
out:
	mutex_unlock(&h->lock);
	if (ret) {
		if (!r->cond)
			s7d_hdmi_mismatch(r, "A11", "TX read", 0, -ret, 0);
		return -EINVAL;
	}
	/*
	 * The sink must scramble at 1/40 the clock ratio the link runs. As
	 * in s7d_hdmi_scdc_check(), no DDC transfer runs under the lock.
	 */
	if (output->high_tmds) {
		const u8 scrambling = SCDC_SCRAMBLING_ENABLE | SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

		config = 0;
		ret = state.hdmi ? drm_scdc_readb(&h->ddc.adapter, SCDC_TMDS_CONFIG, &config) :
			-EINVAL;
		if (ret || (config & scrambling) != scrambling)
			return s7d_hdmi_mismatch(r, "A12", "SCDC", SCDC_TMDS_CONFIG, config,
						 scrambling);
	}
	mutex_lock(&h->lock);
	h->boot.hdmi = state.hdmi;
	h->boot.valid = true;
	mutex_unlock(&h->lock);
	return 0;
}

static int s7d_hdmi_adopt(void *data, const struct s7d_hdmi_output_state *output)
{
	struct s7d_hdmi *h = data;
	int ret = 0;

	mutex_lock(&h->lock);
	if (!h->boot.valid || !h->phy_on || !h->phy_initialized || !h->video_on ||
	    h->prepared) {
		ret = -EBUSY;
		goto out;
	}
	h->prepared_output = *output;
	h->prepared = true;
	/* As the enable leaves a link that scrambles: recheck the sink. */
	h->scdc_config = output->high_tmds ? SCDC_SCRAMBLING_ENABLE |
		SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 : 0;
	if (h->scdc_config) {
		h->sink_scrambled = true;
		mod_delayed_work(system_wq, &h->scdc_check, HZ);
	}
	h->audio_error = 0;
	h->audio_valid = false;
	h->audio_resume = false;
out:
	mutex_unlock(&h->lock);
	return ret;
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

static bool s7d_hdmi_ycc_709(enum s7d_hdmi_output_encoding encoding)
{
	switch (encoding) {
	case S7D_HDMI_OUTPUT_709_Y4448:
	case S7D_HDMI_OUTPUT_709_Y44410:
	case S7D_HDMI_OUTPUT_709_Y44412:
	case S7D_HDMI_OUTPUT_709_Y4228:
	case S7D_HDMI_OUTPUT_709_Y4208:
	case S7D_HDMI_OUTPUT_709_Y42010:
	case S7D_HDMI_OUTPUT_709_Y42012:
		return true;
	default:
		return false;
	}
}

enum drm_mode_status s7d_hdmi_link_valid(const struct drm_display_info *info,
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

/*
 * As the vendor driver, a color_depth write, even of the same value, takes a
 * modeset in a commit that allows one, and UPDATE sets the output again. The
 * format request waits for a modeset otherwise.
 */
static int s7d_hdmi_vendor_check(struct drm_atomic_state *atomic,
				 const struct drm_connector_state *state,
				 const struct s7d_vendor_conn_values *vendor)
{
	struct drm_crtc_state *crtc_state;

	if (!atomic->allow_modeset || (!vendor->update && !vendor->depth_written))
		return 0;
	crtc_state = drm_atomic_get_crtc_state(atomic, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	if (!crtc_state->active)
		return 0;
	if (vendor->update)
		crtc_state->connectors_changed = true;
	if (vendor->depth_written)
		crtc_state->mode_changed = true;
	return 0;
}

int s7d_hdmi_connector_atomic_check(struct drm_connector *connector,
				  struct drm_atomic_state *atomic)
{
	struct drm_connector_state *old = drm_atomic_get_old_connector_state(atomic, connector);
	struct drm_connector_state *state = drm_atomic_get_new_connector_state(atomic, connector);
	const struct s7d_vendor_conn_values *vendor = s7d_vendor_props_conn(state);
	struct drm_crtc_state *crtc_state, *old_crtc_state;
	enum s7d_hdmi_output_encoding encoding;
	bool content_changed = connector->display_info.is_hdmi &&
		old->content_type != state->content_type;
	bool range_changed = old->hdmi.broadcast_rgb != state->hdmi.broadcast_rgb;
	bool hdr_changed = !drm_connector_atomic_hdr_metadata_equal(old, state);
	bool y422;
	int ret;

	if (vendor && state->crtc) {
		ret = s7d_hdmi_vendor_check(atomic, state, vendor);
		if (ret)
			return ret;
	}
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
	/* A vendor format request takes its own sampling and falls back instead. */
	if (vendor && vendor->request) {
		if (range_changed)
			crtc_state->connectors_changed = true;
		return 0;
	}
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

/* A vendor output format: HDMI_COLORSPACE_* and bits per component */
struct s7d_hdmi_attr {
	u8 space;
	u8 depth;
};

/* The vendor order of preference: UHD at 50/60 Hz, then every other mode */
static const struct s7d_hdmi_attr s7d_hdmi_uhd_attrs[] = {
	{ HDMI_COLORSPACE_YUV420, 10 },
	{ HDMI_COLORSPACE_YUV422, 12 },
	{ HDMI_COLORSPACE_YUV420, 8 },
	{ HDMI_COLORSPACE_YUV444, 8 },
	{ HDMI_COLORSPACE_RGB, 8 },
};

static const struct s7d_hdmi_attr s7d_hdmi_other_attrs[] = {
	{ HDMI_COLORSPACE_YUV444, 10 },
	{ HDMI_COLORSPACE_YUV422, 12 },
	{ HDMI_COLORSPACE_RGB, 10 },
	{ HDMI_COLORSPACE_YUV444, 8 },
	{ HDMI_COLORSPACE_RGB, 8 },
};

/*
 * The encoding of a vendor format for this sink. YCbCr takes BT.601 below
 * 720 lines and BT.709 above, unless Colorspace names one; any 4:2:2 depth
 * is the 12-bit container. Deep colour needs the sink's deep colour modes
 * and stays within max bpc.
 */
static int s7d_hdmi_vendor_encoding(const struct drm_connector_state *conn_state,
				    const struct drm_display_mode *mode,
				    struct s7d_hdmi_attr attr,
				    enum s7d_hdmi_output_encoding *encoding)
{
	static const enum s7d_hdmi_output_encoding y444[3][2] = {
		{ S7D_HDMI_OUTPUT_601_Y4448, S7D_HDMI_OUTPUT_709_Y4448 },
		{ S7D_HDMI_OUTPUT_601_Y44410, S7D_HDMI_OUTPUT_709_Y44410 },
		{ S7D_HDMI_OUTPUT_601_Y44412, S7D_HDMI_OUTPUT_709_Y44412 },
	}, y420[3][2] = {
		{ S7D_HDMI_OUTPUT_601_Y4208, S7D_HDMI_OUTPUT_709_Y4208 },
		{ S7D_HDMI_OUTPUT_601_Y42010, S7D_HDMI_OUTPUT_709_Y42010 },
		{ S7D_HDMI_OUTPUT_601_Y42012, S7D_HDMI_OUTPUT_709_Y42012 },
	}, rgb[3] = {
		S7D_HDMI_OUTPUT_RGB8, S7D_HDMI_OUTPUT_RGB10, S7D_HDMI_OUTPUT_RGB12,
	};
	const struct drm_display_info *info = &conn_state->connector->display_info;
	unsigned int depth = attr.depth == 8 ? 0 : attr.depth == 10 ? 1 : 2;
	bool bt709;

	if ((attr.depth != 8 && attr.depth != 10 && attr.depth != 12) ||
	    (attr.space != HDMI_COLORSPACE_RGB && !info->is_hdmi) ||
	    (attr.space != HDMI_COLORSPACE_YUV420 && drm_mode_is_420_only(info, mode)))
		return -EINVAL;
	if (depth && attr.space != HDMI_COLORSPACE_YUV422 &&
	    (!info->is_hdmi || attr.depth > conn_state->max_bpc ||
	     (attr.space != HDMI_COLORSPACE_YUV420 && drm_match_cea_mode(mode) == 1)))
		return -EINVAL;
	switch (conn_state->colorspace) {
	case DRM_MODE_COLORIMETRY_SMPTE_170M_YCC:
		bt709 = false;
		break;
	case DRM_MODE_COLORIMETRY_BT709_YCC:
	case DRM_MODE_COLORIMETRY_BT2020_RGB:
	case DRM_MODE_COLORIMETRY_BT2020_YCC:
		bt709 = true;
		break;
	default:
		bt709 = mode->vdisplay >= 720;
		break;
	}
	switch (attr.space) {
	case HDMI_COLORSPACE_RGB:
		if (depth && !(info->edid_hdmi_rgb444_dc_modes &
			       (depth == 1 ? DRM_EDID_HDMI_DC_30 : DRM_EDID_HDMI_DC_36)))
			return -EINVAL;
		*encoding = rgb[depth];
		return 0;
	case HDMI_COLORSPACE_YUV444:
		if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR444) ||
		    (depth && !(info->edid_hdmi_ycbcr444_dc_modes &
				(depth == 1 ? DRM_EDID_HDMI_DC_30 : DRM_EDID_HDMI_DC_36))))
			return -EINVAL;
		*encoding = y444[depth][bt709];
		return 0;
	case HDMI_COLORSPACE_YUV422:
		if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR422))
			return -EINVAL;
		*encoding = bt709 ? S7D_HDMI_OUTPUT_709_Y4228 : S7D_HDMI_OUTPUT_601_Y4228;
		return 0;
	case HDMI_COLORSPACE_YUV420:
		if (!drm_mode_is_420(info, mode) ||
		    (depth && !(info->hdmi.y420_dc_modes &
				(depth == 1 ? DRM_EDID_YCBCR420_DC_30 : DRM_EDID_YCBCR420_DC_36))))
			return -EINVAL;
		*encoding = y420[depth][bt709];
		return 0;
	default:
		return -EINVAL;
	}
}

/* Build a vendor format with the range, BT.2020 and repetition of the commit. */
static int s7d_hdmi_vendor_output(const struct drm_connector_state *conn_state,
				  const struct drm_display_mode *mode, struct s7d_hdmi_attr attr,
				  struct s7d_hdmi_output_state *output)
{
	enum s7d_hdmi_output_encoding encoding;
	struct s7d_hdmi_output_state next;
	int ret;

	ret = s7d_hdmi_vendor_encoding(conn_state, mode, attr, &encoding);
	if (!ret)
		ret = s7d_hdmi_output_build(encoding, (unsigned long)s7d_mode_clock(mode) * 1000,
					    24000000, &next);
	if (!ret && !s7d_hdmi_sink_accepts_output(&conn_state->connector->display_info, &next))
		ret = -EINVAL;
	if (!ret)
		ret = s7d_hdmi_output_apply_bt2020(&next, s7d_hdmi_bt2020(conn_state));
	if (!ret && attr.space == HDMI_COLORSPACE_RGB)
		ret = s7d_hdmi_output_apply_rgb_range(&next,
						      s7d_hdmi_rgb_limited(conn_state, mode));
	if (!ret)
		ret = s7d_hdmi_output_apply_repeat(&next, mode->flags & DRM_MODE_FLAG_DBLCLK);
	if (!ret)
		*output = next;
	return ret;
}

/*
 * A vendor format request: the requested format when the sink and the link
 * take it, else the first that fits in the vendor order, as the vendor driver
 * picks one instead of failing the commit.
 */
static int s7d_hdmi_select_vendor(const struct drm_connector_state *conn_state,
				  struct s7d_hdmi_attr request,
				  const struct drm_display_mode *mode,
				  struct s7d_hdmi_output_state *output, struct s7d_hdmi_attr *attr)
{
	bool uhd = mode->hdisplay >= 3840 && mode->vdisplay == 2160 &&
		!(mode->flags & DRM_MODE_FLAG_INTERLACE) && drm_mode_vrefresh(mode) >= 50;
	const struct s7d_hdmi_attr *list = uhd ? s7d_hdmi_uhd_attrs : s7d_hdmi_other_attrs;
	unsigned int count = uhd ? ARRAY_SIZE(s7d_hdmi_uhd_attrs) :
		ARRAY_SIZE(s7d_hdmi_other_attrs);
	unsigned int i;

	if (!s7d_hdmi_vendor_output(conn_state, mode, request, output)) {
		*attr = request;
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (!s7d_hdmi_vendor_output(conn_state, mode, list[i], output)) {
			*attr = list[i];
			return 0;
		}
	}
	return -EINVAL;
}

/* The vendor format of an output; HDMI carries 4:2:2 in the 12-bit container. */
static struct s7d_hdmi_attr s7d_hdmi_output_attr(const struct s7d_hdmi_output_state *output)
{
	struct s7d_hdmi_attr attr = { .space = HDMI_COLORSPACE_YUV444, .depth = output->bpc };

	if (output->encoding == S7D_HDMI_OUTPUT_RGB8 || output->encoding == S7D_HDMI_OUTPUT_RGB10 ||
	    output->encoding == S7D_HDMI_OUTPUT_RGB12) {
		attr.space = HDMI_COLORSPACE_RGB;
	} else if (s7d_hdmi_output_is_y422(output)) {
		attr.space = HDMI_COLORSPACE_YUV422;
		attr.depth = 12;
	} else if (s7d_hdmi_output_is_y420(output)) {
		attr.space = HDMI_COLORSPACE_YUV420;
	}
	return attr;
}

/*
 * The vendor format request of a commit. As in the vendor driver, a modeset
 * takes the one that color_space and color_depth hold; a commit without a
 * modeset keeps the one the link runs, so that it leaves the output as it is.
 */
static bool s7d_hdmi_vendor_request(const struct drm_crtc_state *crtc_state,
				    const struct drm_connector_state *conn_state,
				    struct s7d_hdmi_attr *request)
{
	const struct s7d_vendor_conn_values *vendor = s7d_vendor_props_conn(conn_state);

	if (!vendor)
		return false;
	if (drm_atomic_crtc_needs_modeset(crtc_state)) {
		request->space = vendor->color_space;
		request->depth = vendor->color_depth;
		return vendor->request;
	}
	request->space = vendor->link_space;
	request->depth = vendor->link_depth;
	return vendor->link_request;
}

/*
 * Pixel repetition (480i/576i): no 4:2:2 and no dither, like the vendor. A
 * vendor format request that no format satisfies leaves the choice to the
 * display. @attr gets the vendor format of the output.
 */
static int s7d_hdmi_select_output(const struct drm_crtc_state *crtc_state,
				  const struct drm_connector_state *conn_state,
				  struct s7d_hdmi_output_state *output,
				  struct s7d_hdmi_attr *attr)
{
	const struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	struct s7d_hdmi_attr request;
	int ret;

	if (s7d_hdmi_vendor_request(crtc_state, conn_state, &request) &&
	    !s7d_hdmi_select_vendor(conn_state, request, mode, output, attr))
		return 0;
	ret = __s7d_hdmi_select_output(conn_state, mode, output);
	if (!ret)
		ret = s7d_hdmi_output_apply_repeat(output, mode->flags & DRM_MODE_FLAG_DBLCLK);
	if (!ret)
		*attr = s7d_hdmi_output_attr(output);
	return ret;
}

bool s7d_hdmi_vendor_format_valid(struct drm_connector *connector,
				  const struct drm_display_mode *mode, u8 space, u8 depth)
{
	const struct drm_connector_state state = {
		.connector = connector,
		.max_bpc = S7D_HDMI_MAX_BPC,
		.colorspace = DRM_MODE_COLORIMETRY_DEFAULT,
	};
	struct s7d_hdmi_attr attr = { .space = space, .depth = depth };
	struct s7d_hdmi_output_state output;
	struct s7d_encp_state encp;

	if (mode->clock > 594000 || mode->hdisplay > 4096 || mode->vdisplay > 2160 ||
	    s7d_encp_build_state(mode, &encp) != MODE_OK)
		return false;
	return !s7d_hdmi_vendor_output(&state, mode, attr, &output);
}

/* A CEA rate divided by 1.001, as 59.94 Hz, is what FRAC_RATE_POLICY reports. */
static bool s7d_hdmi_mode_fractional(const struct drm_display_mode *mode)
{
	u64 total = (u64)mode->htotal * mode->vtotal;
	u64 rate, whole;

	if (!total)
		return false;
	/* mHz, against the nearest whole rate times 1000/1001 */
	rate = div64_u64((u64)mode->clock * 1000000, total);
	whole = DIV_ROUND_CLOSEST_ULL(rate, 1000);
	return rate % 1000 && abs_diff(rate * 1001, whole * 1000000) <= 2002;
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
	struct s7d_hdmi_attr attr;
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
		if (s7d_hdmi_select_output(crtc_state, conn_state, &output, &attr))
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

/* @infoframes: the AVI and vendor InfoFrames may change on the running link. */
static bool s7d_hdmi_link_same(const struct s7d_hdmi_state *a,
			       const struct s7d_hdmi_state *b, bool infoframes)
{
	if (!a || !b || !a->valid || !b->valid || a->hdmi != b->hdmi ||
	    a->scdc != b->scdc || a->high_tmds != b->high_tmds)
		return false;
	if (!a->hdmi)
		return true;
	if (a->vendor_len > sizeof(a->vendor) || b->vendor_len > sizeof(b->vendor) ||
	    a->hdr_len > sizeof(a->hdr) || b->hdr_len > sizeof(b->hdr) ||
	    a->hdr_len != b->hdr_len || memcmp(a->hdr, b->hdr, a->hdr_len))
		return false;
	return infoframes || (a->vendor_len == b->vendor_len &&
			      !memcmp(a->avi, b->avi, sizeof(a->avi)) &&
			      !memcmp(a->vendor, b->vendor, a->vendor_len));
}

/* The HDMI Forum VSIF with ALLM set: header, checksum, OUI, version 1, flags */
static u8 s7d_hdmi_pack_allm(u8 *buffer)
{
	static const u8 frame[] = {
		HDMI_INFOFRAME_TYPE_VENDOR, 1, 5, 0,
		HDMI_FORUM_IEEE_OUI & 0xff, (HDMI_FORUM_IEEE_OUI >> 8) & 0xff,
		HDMI_FORUM_IEEE_OUI >> 16, 1, BIT(1),
	};
	unsigned int i;
	u8 sum = 0;

	memcpy(buffer, frame, sizeof(frame));
	for (i = 0; i < sizeof(frame); i++)
		sum += frame[i];
	buffer[3] = -sum;
	return sizeof(frame);
}

static int s7d_hdmi_atomic_check(struct drm_bridge *bridge, struct drm_bridge_state *base,
				 struct drm_crtc_state *crtc_state,
				 struct drm_connector_state *conn_state)
{
	struct s7d_hdmi_state *state = to_s7d_hdmi_state(base);
	struct drm_connector *connector = conn_state->connector;
	const struct s7d_vendor_conn_values *vendor_values, *old_values;
	struct hdmi_avi_infoframe avi;
	struct hdmi_vendor_infoframe vendor;
	struct hdmi_drm_infoframe hdr;
	struct s7d_hdmi_attr attr;
	bool allm, live;
	int ret;

	state->valid = false;
	/* DRM debugfs and the state dump read the link and InfoFrames from here. */
	memset(&conn_state->hdmi.infoframes, 0, sizeof(conn_state->hdmi.infoframes));
	conn_state->hdmi.is_limited_range = false;
	conn_state->hdmi.output_bpc = 0;
	conn_state->hdmi.output_format = HDMI_COLORSPACE_RGB;
	conn_state->hdmi.tmds_char_rate = 0;
	if (!crtc_state->active)
		return 0;
	if (s7d_hdmi_mode_valid(bridge, &connector->display_info,
				&crtc_state->adjusted_mode) != MODE_OK)
		return -EINVAL;
	ret = s7d_hdmi_select_output(crtc_state, conn_state, &state->output, &attr);
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
	if (drm_atomic_crtc_needs_modeset(crtc_state))
		s7d_vendor_props_take_request(conn_state);
	s7d_vendor_props_set_output(conn_state, attr.space, attr.depth,
				    s7d_hdmi_mode_fractional(&state->mode));
	vendor_values = s7d_vendor_props_conn(conn_state);
	state->avmute = vendor_values && vendor_values->avmute;
	state->hdmi = connector->display_info.is_hdmi;
	state->scdc = state->hdmi && connector->display_info.hdmi.scdc.supported;
	state->high_tmds = state->output.high_tmds;
	state->vendor_len = 0;
	state->hdr_len = 0;
	/* ALLM only to a sink that takes it, like vendor userspace checks allm_cap */
	allm = state->hdmi && vendor_values && vendor_values->allm == 1 &&
		connector->display_info.has_hdmi_infoframe &&
		s7d_vendor_props_sink_allm(connector);
	conn_state->hdmi.is_limited_range = state->output.rgb_limited;
	conn_state->hdmi.output_bpc = state->output.bpc;
	conn_state->hdmi.tmds_char_rate = div_u64(state->output.rate.tmds_numerator,
						  state->output.rate.tmds_denominator);
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
			/*
			 * An RGB request carried as 4:2:0, or a vendor YCbCr
			 * request, states the matrix selected for it.
			 */
			if (conn_state->colorspace == DRM_MODE_COLORIMETRY_DEFAULT)
				avi.colorimetry = s7d_hdmi_ycc_709(state->output.encoding) ?
					HDMI_COLORIMETRY_ITU_709 : HDMI_COLORIMETRY_ITU_601;
		}
		drm_hdmi_avi_infoframe_content_type(&avi, conn_state);
		/*
		 * With "future" the vendor driver picks from the EDID video
		 * capability block, starting from underscan. Keep the helper's
		 * underscan rather than send the reserved code.
		 */
		if (vendor_values && vendor_values->scan_info != HDMI_SCAN_MODE_RESERVED)
			avi.scan_mode = vendor_values->scan_info;
		if (allm) {
			/*
			 * The HDMI Forum VSIF replaces the HDMI 1.4 one, so the AVI
			 * carries the VIC of a 4K mode; ALLM replaces the content
			 * type, as the vendor sends them.
			 */
			if (!avi.video_code)
				avi.video_code = drm_match_cea_mode(&state->mode);
			avi.itc = false;
			avi.content_type = HDMI_CONTENT_TYPE_GRAPHICS;
		}
		ret = hdmi_avi_infoframe_pack(&avi, state->avi, sizeof(state->avi));
		if (ret < 0)
			return ret;
		conn_state->hdmi.output_format = avi.colorspace;
		conn_state->hdmi.infoframes.avi.data.avi = avi;
		conn_state->hdmi.infoframes.avi.set = true;
		conn_state->hdmi.infoframes.spd.data.spd = to_s7d_hdmi(bridge)->spd_frame;
		conn_state->hdmi.infoframes.spd.set = true;
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
			conn_state->hdmi.infoframes.hdr_drm.data.drm = hdr;
			conn_state->hdmi.infoframes.hdr_drm.set = true;
		}
	}
	if (allm) {
		state->vendor_len = s7d_hdmi_pack_allm(state->vendor);
	} else if (state->hdmi && connector->display_info.has_hdmi_infoframe) {
		ret = drm_hdmi_vendor_infoframe_from_display_mode(&vendor, connector, &state->mode);
		if (ret)
			return ret;
		ret = hdmi_vendor_infoframe_pack(&vendor, state->vendor, sizeof(state->vendor));
		if (ret < 0)
			return ret;
		state->vendor_len = ret;
		conn_state->hdmi.infoframes.hdmi.data.vendor.hdmi = vendor;
		conn_state->hdmi.infoframes.hdmi.set = true;
	}
	state->valid = true;
	/*
	 * As the vendor driver, a new ALLM or scan information goes out on the
	 * running link: the CRTC flush sends the InfoFrames of a commit without
	 * a modeset.
	 */
	old_values = s7d_vendor_props_conn(drm_atomic_get_old_connector_state(crtc_state->state,
									    connector));
	live = vendor_values && old_values && !drm_atomic_crtc_needs_modeset(crtc_state) &&
		(old_values->allm != vendor_values->allm ||
		 old_values->scan_info != vendor_values->scan_info);
	/* Copy the checked plan before prepare; VPU applies it before PHY/start. */
	{
		struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(crtc_state->state,
								       crtc_state->crtc);
		struct drm_bridge_state *old_bridge =
			drm_atomic_get_old_bridge_state(crtc_state->state, bridge);

		if (old && old->active &&
		    (!s7d_hdmi_output_same(&to_s7d_crtc_state(old)->output, &state->output) ||
		     !old_bridge ||
		     !s7d_hdmi_link_same(to_s7d_hdmi_state(old_bridge), state, live)))
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
		/* With the link up, a resuming stream needs no exception. */
		h->audio_resume = false;
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

/* As the vendor: both GCP bits clear, then Set_AVMUTE (3) or Clear_AVMUTE (7) */
static int s7d_hdmi_avmute(struct s7d_hdmi *h, bool mute)
{
	unsigned int bit = mute ? BIT(3) : BIT(7);
	int ret;

	ret = regmap_update_bits(h->core, CORE_TPI_SC, BIT(7) | BIT(3), 0);
	return ret ?: regmap_update_bits(h->core, CORE_TPI_SC, bit, bit);
}

/*
 * The vendor and AVI InfoFrames that a commit without a modeset changed, each
 * TPI bank rewritten on the running link as the vendor driver does.
 */
static int s7d_hdmi_update_infoframes(struct s7d_hdmi *h, const struct s7d_hdmi_state *old,
				      const struct s7d_hdmi_state *new)
{
	int ret = 0;

	if (!old->valid || !new->valid || !old->hdmi || !new->hdmi ||
	    old->vendor_len > sizeof(old->vendor) || new->vendor_len > sizeof(new->vendor))
		return 0;
	if (old->vendor_len != new->vendor_len ||
	    memcmp(old->vendor, new->vendor, new->vendor_len)) {
		if (new->vendor_len) {
			ret = s7d_hdmi_packet(h, 5, new->vendor, new->vendor_len);
		} else {
			ret = core_write(h, CORE_INFO_SELECT, 5);
			if (!ret)
				ret = s7d_hdmi_write_checked(h, h->core, CORE_INFO_ENABLE,
							     CORE_INFO_TRANSMIT, 0);
		}
	}
	if (!ret && memcmp(old->avi, new->avi, sizeof(new->avi)))
		ret = s7d_hdmi_packet(h, 0, new->avi, sizeof(new->avi));
	return ret;
}

void s7d_hdmi_commit_vendor(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	const struct s7d_vendor_conn_values *before, *after;
	struct drm_bridge_state *old_bridge, *new_bridge;
	struct drm_connector_state *old, *new;
	struct drm_connector *connector;
	struct drm_bridge *bridge;
	struct s7d_hdmi *h;
	int i;

	for_each_oldnew_connector_in_state(atomic, connector, old, new, i) {
		before = s7d_vendor_props_conn(old);
		after = s7d_vendor_props_conn(new);
		if (new->crtc != crtc || !new->best_encoder)
			continue;
		bridge = drm_bridge_chain_get_first_bridge(new->best_encoder);
		if (!bridge || bridge->funcs != &s7d_hdmi_bridge_funcs)
			continue;
		h = to_s7d_hdmi(bridge);
		old_bridge = drm_atomic_get_old_bridge_state(atomic, bridge);
		new_bridge = drm_atomic_get_new_bridge_state(atomic, bridge);
		/*
		 * Only on a configured link; the next one starts from the state.
		 * Without the vendor interface the InfoFrames differ only after
		 * a takeover at probe, whose first commit keeps the link.
		 */
		mutex_lock(&h->lock);
		if (h->prepared && before && after && before->avmute != after->avmute &&
		    s7d_hdmi_avmute(h, after->avmute))
			dev_warn(h->dev, "HDMI AV mute %d failed\n", after->avmute);
		if (h->prepared && old_bridge && new_bridge &&
		    s7d_hdmi_update_infoframes(h, to_s7d_hdmi_state(old_bridge),
					       to_s7d_hdmi_state(new_bridge)))
			dev_warn(h->dev, "HDMI InfoFrame update failed\n");
		mutex_unlock(&h->lock);
	}
}

int s7d_hdmi_bridge_inherit(struct drm_bridge *bridge, const struct drm_display_mode *mode,
			    const struct s7d_hdmi_output_state *output)
{
	struct s7d_hdmi *h = to_s7d_hdmi(bridge);
	struct s7d_hdmi_state *state;
	int ret = 0;

	if (bridge->funcs != &s7d_hdmi_bridge_funcs || !bridge->base.state)
		return -EINVAL;
	state = to_s7d_hdmi_state(drm_priv_to_bridge_state(bridge->base.state));
	mutex_lock(&h->lock);
	if (!h->boot.valid || state->valid) {
		ret = -EINVAL;
		goto out;
	}
	state->mode = *mode;
	state->output = *output;
	state->hdmi = h->boot.hdmi;
	/* The readout found the sink scrambling, as enable leaves an SCDC link. */
	state->scdc = output->high_tmds;
	state->high_tmds = output->high_tmds;
	/* The packets the link sends; the first commit rewrites what differs. */
	if (h->boot.banks & BIT(0))
		memcpy(state->avi, h->boot.avi, sizeof(state->avi));
	if (h->boot.banks & BIT(5)) {
		state->vendor_len = h->boot.vendor_len;
		memcpy(state->vendor, h->boot.vendor, state->vendor_len);
	}
	state->valid = true;
out:
	mutex_unlock(&h->lock);
	return ret;
}

void s7d_hdmi_bridge_uninherit(struct drm_bridge *bridge)
{
	struct s7d_hdmi_state *state;

	if (bridge->funcs != &s7d_hdmi_bridge_funcs || !bridge->base.state)
		return;
	state = to_s7d_hdmi_state(drm_priv_to_bridge_state(bridge->base.state));
	memset(&state->mode, 0, sizeof(*state) - offsetof(struct s7d_hdmi_state, mode));
}

const char *s7d_hdmi_bridge_fastset(struct drm_bridge *bridge, struct drm_atomic_state *atomic)
{
	const struct s7d_vendor_conn_values *vendor;
	const struct drm_bridge_state *old, *new;
	const struct s7d_hdmi_state *a, *b;
	struct s7d_hdmi *h;

	if (bridge->funcs != &s7d_hdmi_bridge_funcs)
		return "bridge";
	h = to_s7d_hdmi(bridge);
	old = drm_atomic_get_old_bridge_state(atomic, bridge);
	new = drm_atomic_get_new_bridge_state(atomic, bridge);
	if (!old || !new)
		return "bridge";
	a = container_of(old, const struct s7d_hdmi_state, base);
	b = container_of(new, const struct s7d_hdmi_state, base);
	if (!a->valid || !b->valid)
		return "bridge";
	if (a->hdmi != b->hdmi)
		return "hdmi";
	/* SCDC is set up only above 340 MHz, where the readout found it on. */
	if (a->high_tmds != b->high_tmds || (b->high_tmds && !b->scdc))
		return "scdc";
	if (a->hdr_len || b->hdr_len)
		return "hdr";
	/* As the vendor driver, a vendor format write takes a modeset. */
	vendor = h->connector ?
		s7d_vendor_props_conn(drm_atomic_get_new_connector_state(atomic, h->connector)) :
		NULL;
	if (vendor && (vendor->update || vendor->depth_written))
		return "vendor";
	return NULL;
}

int s7d_hdmi_bridge_link(struct drm_bridge *bridge, struct s7d_vpu_link *link)
{
	if (!bridge || bridge->funcs != &s7d_hdmi_bridge_funcs || !link)
		return -EINVAL;
	*link = (struct s7d_vpu_link) {
		.quiesce = s7d_hdmi_quiesce,
		.prepare = s7d_hdmi_prepare,
		.readout = s7d_hdmi_readout,
		.adopt = s7d_hdmi_adopt,
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

/*
 * Whether DRM must hear of @status. The bridge connector turns every report
 * into a hotplug event, and with no DRM master the fbdev client then commits
 * its mode again, over the boot logo or under a starting splash, for any HPD
 * pulse, as some sinks make when a signal starts. Like
 * drm_connector_helper_hpd_irq_event() with a detect that updates the EDID,
 * as later upstream HDMI connector helpers do, report a status other than the
 * connector's, or a connected sink whose EDID changed while HPD dropped and
 * came back. That needs the EDID read and compared under the mode config
 * mutex, as a probe does; with it unchanged, give back the audio and CEC
 * state that the HPD edge withdrew, as the probe would have.
 */
static bool s7d_hdmi_sink_changed(struct s7d_hdmi *h, struct drm_connector *connector,
				  enum drm_connector_status status)
{
	struct drm_device *drm = connector->dev;
	const struct drm_edid *edid;
	bool changed = true;
	u64 epoch;

	mutex_lock(&drm->mode_config.mutex);
	if (connector->status != status)
		goto out;
	changed = false;
	if (status != connector_status_connected)
		goto out;
	/* A sink gone or silent meanwhile is for the probe to find out. */
	edid = s7d_hdmi_get_edid(&h->bridge, connector);
	changed = !edid || !connector->edid_blob_ptr;
	if (!changed) {
		/* An update with a different EDID moves the epoch, as in a probe. */
		epoch = connector->epoch_counter;
		changed = drm_edid_connector_update(connector, edid) ||
			connector->epoch_counter != epoch;
	}
	drm_edid_free(edid);
	if (!changed)
		s7d_hdmi_bridge_eld_updated(&h->bridge, connector);
out:
	mutex_unlock(&drm->mode_config.mutex);
	return changed;
}

/*
 * Report the sink to DRM when it changed, or always with @force, then a
 * detected status to the vendor interfaces. @failed: the HPD acknowledgment
 * failed, so the status is unknown.
 *
 * Any probe rewrites connector->status from a fresh detection, also the
 * fbdev probe that a report itself runs, and can do so between the compare
 * and the report: after an HPD pulse, the falling edge is reported, the
 * probe in it finds the sink back, and the rising edge then matches the
 * connector. DRM has the right status then, but the vendor interfaces only
 * hear of reports. They get their own baseline, so that a detected status
 * other than the one they last heard reaches them as well.
 */
static void s7d_hdmi_report_hpd(struct s7d_hdmi *h, bool failed, bool force)
{
	enum drm_connector_status status;
	struct drm_connector *connector;
	struct drm_device *drm = NULL;
	bool report = true, live = true, vendor;
	int idx;

	/* Detect inside the lock, so that the last report has the last status. */
	mutex_lock(&h->hpd_lock);
	status = failed ? connector_status_unknown : s7d_hdmi_detect(&h->bridge);
	/* The display drops the connector before it releases its device. */
	mutex_lock(&h->lock);
	connector = h->connector;
	if (connector) {
		drm = connector->dev;
		drm_dev_get(drm);
	}
	mutex_unlock(&h->lock);
	if (drm && !force) {
		/* An unplugged display has nobody left to tell. */
		live = drm_dev_enter(drm, &idx);
		report = live && s7d_hdmi_sink_changed(h, connector, status);
		if (live)
			drm_dev_exit(idx);
	}
	if (report)
		drm_bridge_hpd_notify(&h->bridge, status);
	/* An I/O error is no unplug; keep the last detected status. */
	if (drm && live && status != connector_status_unknown) {
		mutex_lock(&h->lock);
		vendor = report || h->vendor_status != status;
		if (h->connector == connector)
			h->vendor_status = status;
		mutex_unlock(&h->lock);
		if (vendor)
			s7d_vendor_hpd(drm, status == connector_status_connected);
	}
	if (drm)
		drm_dev_put(drm);
	mutex_unlock(&h->hpd_lock);
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
	WRITE_ONCE(h->hpd_masked, false);
	enable_irq(h->irq);
	mutex_unlock(&h->lock);
	s7d_hdmi_audio_hpd(h);
	if (READ_ONCE(h->hpd_enabled))
		s7d_hdmi_report_hpd(h, false, false);
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
		WRITE_ONCE(h->hpd_masked, true);
		disable_irq_nosync(irq);
		dev_err_ratelimited(h->dev, "HPD acknowledgment failed: %d; retrying with IRQ masked\n",
				    ret);
		if (!READ_ONCE(h->stopping))
			mod_delayed_work(system_wq, &h->hpd_recovery, HZ);
	}
	if (READ_ONCE(h->hpd_enabled))
		s7d_hdmi_report_hpd(h, ret < 0, false);
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
	if (ret < 0)
		return ret;
	h->spd_frame = spd;
	return 0;
}

/* Deep suspend can power-cycle the domain, leaving the TX in soft reset. */
static int s7d_hdmi_tx_reset(struct s7d_hdmi *h)
{
	return top_write(h, TOP_RESET, U32_MAX, 0);
}

/* TX setup that lives in the VPU/HDMI domain and is lost with its power. */
static int s7d_hdmi_hw_init(struct s7d_hdmi *h)
{
	int ret;

	ret = top_write(h, TOP_MASK, 0x1ff, 0);
	if (ret)
		return ret;
	ret = top_write(h, TOP_SCRATCH, U32_MAX, 1);
	if (ret)
		return ret;
	ret = top_write(h, TOP_HPD_FILTER, U32_MAX, 0x80007000);
	if (ret)
		return ret;
	/* The firmware boot values below are lost when deep suspend cuts power. */
	ret = top_write(h, TOP_CLK, BIT(3), BIT(3)); /* I2S clock to audio input */
	if (ret)
		return ret;
	ret = top_write(h, TOP_I2C_BUSY, U32_MAX, 30);
	if (ret)
		return ret;
	ret = top_write(h, TOP_HS_INTR, U32_MAX, 0x10);
	if (ret)
		return ret;
	ret = s7d_hdmi_write_checked(h, h->core, CORE_GCP_AUTO, GENMASK(3, 2), BIT(3));
	if (ret)
		return ret;
	ret = top_write(h, TOP_INFILTER, 0x07ff0000, BIT(24));
	if (ret)
		return ret;
	/*
	 * The level TX line also carries the core INTR2/INTR5 shadows, which
	 * nothing here handles; mask any source left on before the IRQ starts.
	 * This is defensive, so a failed write only warns.
	 */
	if (regmap_write(h->core, CORE_INTR2_MASK, 0) ||
	    regmap_write(h->core, CORE_INTR5_MASK, 0))
		dev_warn(h->dev, "could not mask core TX interrupts\n");
	return 0;
}

static int s7d_hdmi_hpd_start(struct s7d_hdmi *h)
{
	int ret;

	ret = regmap_write(h->top, TOP_CLEAR, 0x1ff);
	if (ret)
		return ret;
	return top_write(h, TOP_MASK, 0x1ff, HPD_IRQS);
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
		.max_i2s_channels = 8,
	};
	unsigned int value, i;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	mutex_init(&h->lock);
	mutex_init(&h->hpd_lock);
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
	/* Without firmware setup the TX starts cold: release its resets. */
	for (i = 0; i < ARRAY_SIZE(h->resets); i++) {
		ret = reset_control_status(h->resets[i].rstc);
		if (ret < 0)
			return dev_err_probe(dev, ret, "TX reset state\n");
		if (ret) {
			ret = reset_control_bulk_deassert(ARRAY_SIZE(h->resets), h->resets);
			if (ret)
				return dev_err_probe(dev, ret, "TX reset release\n");
			dev_info(dev, "TX held in reset; starting it without firmware setup\n");
			break;
		}
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
	if (!ret && value)
		ret = s7d_hdmi_tx_reset(h);
	if (ret)
		goto disable_basic;
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
	ret = s7d_hdmi_hw_init(h);
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
	/* ENCP drives 1080i; s7d_encp_build_state() rejects the others. */
	h->bridge.interlace_allowed = true;
	h->bridge.ddc = &h->ddc.adapter;
	ret = s7d_hdmi_hpd_start(h);
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
	/* devres takes a registered codec away after this returns. */
	s7d_hdmi_audio_detach(h);
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

static int s7d_hdmi_suspend(struct device *dev)
{
	struct s7d_hdmi *h = dev_get_drvdata(dev);

	/* The display device, a consumer of this one, has stopped the link. */
	mutex_lock(&h->lock);
	if (h->prepared || h->phy_on || h->video_on) {
		mutex_unlock(&h->lock);
		return -EBUSY;
	}
	WRITE_ONCE(h->stopping, true);
	mutex_unlock(&h->lock);
	disable_irq(h->irq);
	/* Undo the handler's own mask so resume leaves a single depth. */
	if (READ_ONCE(h->hpd_masked))
		enable_irq(h->irq);
	cancel_delayed_work_sync(&h->hpd_recovery);
	cancel_delayed_work_sync(&h->scdc_check);
	s7d_hdmi_ddc_suspend(&h->ddc);
	return 0;
}

static int s7d_hdmi_resume(struct device *dev)
{
	struct s7d_hdmi *h = dev_get_drvdata(dev);
	int ret;

	/* Deep suspend can reset the TX like a cold boot; redo all setup. */
	ret = s7d_hdmi_tx_reset(h);
	if (!ret)
		ret = s7d_hdmi_hw_init(h);
	if (!ret)
		ret = s7d_hdmi_ddc_resume(&h->ddc);
	if (!ret)
		ret = s7d_hdmi_hpd_start(h);
	if (ret) {
		dev_err(dev, "TX restore failed: %d\n", ret);
		return ret;
	}
	mutex_lock(&h->lock);
	WRITE_ONCE(h->stopping, false);
	WRITE_ONCE(h->hpd_masked, false);
	mutex_unlock(&h->lock);
	enable_irq(h->irq);
	/* The sink may have changed while asleep: report it unconditionally. */
	if (READ_ONCE(h->hpd_enabled))
		s7d_hdmi_report_hpd(h, false, true);
	return 0;
}

/*
 * Prepare runs before any device suspends and complete after all have
 * resumed: a stream the sound card cuts off in between is cut off by
 * sleep, wherever the card is in the suspend order.
 */
static int s7d_hdmi_pm_prepare(struct device *dev)
{
	struct s7d_hdmi *h = dev_get_drvdata(dev);

	mutex_lock(&h->lock);
	h->sleeping = true;
	/* Only a stream that this sleep cuts off may come back early. */
	h->audio_resume = false;
	mutex_unlock(&h->lock);
	return 0;
}

static void s7d_hdmi_pm_complete(struct device *dev)
{
	struct s7d_hdmi *h = dev_get_drvdata(dev);

	mutex_lock(&h->lock);
	h->sleeping = false;
	mutex_unlock(&h->lock);
}

static const struct dev_pm_ops s7d_hdmi_pm_ops = {
	.prepare = pm_sleep_ptr(s7d_hdmi_pm_prepare),
	.complete = pm_sleep_ptr(s7d_hdmi_pm_complete),
	SYSTEM_SLEEP_PM_OPS(s7d_hdmi_suspend, s7d_hdmi_resume)
};

/* The display, a consumer through its device link, has already unbound. */
static void s7d_hdmi_remove(struct platform_device *pdev)
{
	struct s7d_hdmi *h = platform_get_drvdata(pdev);
	int ret;

	drm_bridge_remove(&h->bridge);
	mutex_lock(&h->lock);
	WRITE_ONCE(h->stopping, true);
	WRITE_ONCE(h->hpd_enabled, false);
	mutex_unlock(&h->lock);
	/* HPD queues jack reports: stop it before the last flush. */
	disable_irq(h->irq);
	cancel_delayed_work_sync(&h->hpd_recovery);
	cancel_delayed_work_sync(&h->scdc_check);
	s7d_hdmi_audio_detach(h);
	/* The audio codec still reaches the TX while it goes away. */
	devm_release_action(&pdev->dev, s7d_hdmi_codec_unregister, h);
	ret = s7d_hdmi_quiesce(h);
	if (ret)
		dev_err(&pdev->dev, "TX did not stop: %d\n", ret);
	s7d_hdmi_ddc_unregister(&h->ddc);
	clk_bulk_disable_unprepare(ARRAY_SIZE(h->basic), h->basic);
	pm_runtime_put(&pdev->dev);
}

static const struct of_device_id s7d_hdmi_match[] = {
	{ .compatible = "amlogic,s7d-hdmi-tx" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_hdmi_match);

struct platform_driver s7d_hdmi_driver = {
	.probe = s7d_hdmi_probe,
	.remove = s7d_hdmi_remove,
	.shutdown = s7d_hdmi_shutdown,
	.driver = {
		.name = "s7d-hdmi-tx",
		.of_match_table = s7d_hdmi_match,
		.pm = pm_sleep_ptr(&s7d_hdmi_pm_ops),
	},
};
