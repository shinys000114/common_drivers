// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/*
 * The display mode for the Amlogic media stack. aml_media takes the output
 * mode from a video output (vout) server, which the vendor HDMI driver
 * provides; with the vendor display off there is none. Serve the S7D mode
 * instead, read-only: modes are set through DRM, and listeners hear of a
 * mode only while the output runs.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/gcd.h>
#include <linux/hdmi.h>
#include <linux/kmemleak.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/barrier.h>

#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/amlogic/meson_connector_dev.h>
#include <linux/amlogic/media/vout/vinfo.h>
#include <linux/amlogic/media/vout/vout_notify.h>
#include <vout/vout_serve/vout_func.h>

#include "s7d-ext.h"

/* The vout of VIU0, which feeds the HDMI encoder. */
#define S7D_AMLMEDIA_VOUT	1

/*
 * aml_media keeps pointers into the vinfo: amvideo caches it on every mode
 * change with a valid mode, and nothing points it elsewhere later. So once
 * the server was registered, this stays allocated after the module goes.
 */
struct s7d_amlmedia {
	struct vout_server_s server;
	struct vinfo_s vinfo;
	char name[DRM_DISPLAY_MODE_LEN];
	/* Bits of the vouts this server is current on, under vout_mutex. */
	int state;
};

static struct s7d_amlmedia *s7d_amlmedia;
/* The server is known to aml_media. */
static bool s7d_amlmedia_served;

static struct vinfo_s *s7d_amlmedia_get_vinfo(void *data)
{
	struct s7d_amlmedia *am = data;

	return &am->vinfo;
}

static int s7d_amlmedia_vmode_is_supported(enum vmode_e vmode, void *data)
{
	return (vmode & VMODE_MODE_BIT_MASK) == VMODE_HDMI;
}

static int s7d_amlmedia_set_state(int index, void *data)
{
	struct s7d_amlmedia *am = data;

	am->state |= BIT(index);
	return 0;
}

static int s7d_amlmedia_clr_state(int index, void *data)
{
	struct s7d_amlmedia *am = data;

	am->state &= ~BIT(index);
	return 0;
}

static int s7d_amlmedia_get_state(void *data)
{
	struct s7d_amlmedia *am = data;

	return am->state;
}

/*
 * No validate_vmode: a vout mode request (vout= replay, /sys/class/display
 * mode) never selects this server. No set_vmode or rate hint either, so no
 * vendor path sends a mode change for it.
 */
static const struct vout_op_s s7d_amlmedia_vout_ops = {
	.get_vinfo = s7d_amlmedia_get_vinfo,
	.vmode_is_supported = s7d_amlmedia_vmode_is_supported,
	.set_state = s7d_amlmedia_set_state,
	.clr_state = s7d_amlmedia_clr_state,
	.get_state = s7d_amlmedia_get_state,
};

/* Field rate in mHz, as the vendor HDMI driver computes it. */
static u32 s7d_amlmedia_rate(const struct drm_display_mode *m)
{
	u64 num = (u64)m->clock * 1000000;
	u64 den = (u64)m->htotal * m->vtotal;

	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (m->vscan > 1)
		den *= m->vscan;
	return den ? DIV64_U64_ROUND_CLOSEST(num, den) : 0;
}

static bool s7d_amlmedia_vendor_name(const char *name)
{
	size_t len = strlen(name);

	return (len > 2 && !strcmp(name + len - 2, "hz")) ||
	       (len > 3 && !strcmp(name + len - 3, "hzV"));
}

/*
 * The vendor-style name: the DRM name when the vendor interface already gave
 * the mode one (1080p60hz), else WxHpRRhz, where a 1000/1001 rate takes the
 * lower whole number as the vendor names it (1920x1080p59hz).
 */
static void s7d_amlmedia_name(char *name, size_t size,
			      const struct drm_display_mode *m, u32 rate)
{
	u32 hz = DIV_ROUND_CLOSEST(rate, 1000);

	if (s7d_amlmedia_vendor_name(m->name)) {
		strscpy(name, m->name, size);
		return;
	}
	if (hz && !(hz % 6) && abs((int)rate - (int)(hz * 1000000 / 1001)) <= 3)
		hz--;
	snprintf(name, size, "%ux%u%c%uhz", m->hdisplay, m->vdisplay,
		 m->flags & DRM_MODE_FLAG_INTERLACE ? 'i' : 'p', hz);
}

/*
 * The picture aspect the AVI InfoFrame signals: the mode's own, else that
 * of its CEA VIC (4:3 for 480p), else square pixels.
 */
static void s7d_amlmedia_aspect(struct vinfo_s *v, struct drm_device *dev,
				const struct drm_display_mode *m)
{
	static const u16 ratio[][2] = {
		[HDMI_PICTURE_ASPECT_4_3] = { 4, 3 },
		[HDMI_PICTURE_ASPECT_16_9] = { 16, 9 },
		[HDMI_PICTURE_ASPECT_64_27] = { 64, 27 },
		[HDMI_PICTURE_ASPECT_256_135] = { 256, 135 },
	};
	enum hdmi_picture_aspect pa = m->picture_aspect_ratio;
	struct drm_display_mode *cea;
	unsigned long g;
	u8 vic;

	if (pa == HDMI_PICTURE_ASPECT_NONE) {
		vic = drm_match_cea_mode(m);
		cea = vic ? drm_display_mode_from_cea_vic(dev, vic) : NULL;
		if (cea) {
			pa = cea->picture_aspect_ratio;
			drm_mode_destroy(dev, cea);
		}
	}
	g = gcd(m->hdisplay, m->vdisplay);
	if (pa < ARRAY_SIZE(ratio) && ratio[pa][0]) {
		v->aspect_ratio_num = ratio[pa][0];
		v->aspect_ratio_den = ratio[pa][1];
	} else if (g) {
		v->aspect_ratio_num = m->hdisplay / g;
		v->aspect_ratio_den = m->vdisplay / g;
	}
	/* No EDID size here: the vendor driver also falls back to the aspect. */
	v->screen_real_width = v->aspect_ratio_num;
	v->screen_real_height = v->aspect_ratio_den;
}

/* The vinfo of no output, as aml_media's own; nonzero rate for msync. */
static void s7d_amlmedia_reset(struct s7d_amlmedia *am)
{
	struct vinfo_s *v = &am->vinfo;

	strscpy(am->name, "invalid");
	v->name = am->name;
	v->mode = VMODE_INVALID;
	v->width = 1920;
	v->height = 1080;
	v->field_height = 1080;
	v->aspect_ratio_num = 16;
	v->aspect_ratio_den = 9;
	v->sync_duration_num = 60;
	v->sync_duration_den = 1;
	v->video_clk = 148500000;
	v->htotal = 2200;
	v->vtotal = 1125;
	v->connector_type = DRM_MODE_CONNECTOR_MESON_HDMIA_A;
	v->fr_adj_type = VOUT_FR_ADJ_HDMI;
	v->viu_color_fmt = COLOR_FMT_YUV444;
	v->viu_mux = VIU_MUX_ENCP;
	v->cur_enc_ppc = 1;
	v->cd = COLORDEPTH_24B;
	v->cs = HDMI_COLORSPACE_RGB;
}

/* Fields of a running @m; the caller then makes the mode valid. */
static void s7d_amlmedia_fill(struct s7d_amlmedia *am, struct drm_device *dev,
			      const struct drm_display_mode *m)
{
	struct vinfo_s *v = &am->vinfo;
	bool interlace = m->flags & DRM_MODE_FLAG_INTERLACE;
	/* 480i and 576i send every pixel twice. */
	u32 rep = m->flags & DRM_MODE_FLAG_DBLCLK ? 2 : 1;
	u32 rate = s7d_amlmedia_rate(m);

	if (!rate)
		rate = 60000;
	s7d_amlmedia_name(am->name, sizeof(am->name), m, rate);
	v->width = m->hdisplay;
	v->height = m->vdisplay;
	v->field_height = interlace ? m->vdisplay / 2 : m->vdisplay;
	s7d_amlmedia_aspect(v, dev, m);
	/* 60/1 or 59940/1000, as the vendor: amvideo needs den <= 47721. */
	if (rate % 1000) {
		v->sync_duration_num = rate;
		v->sync_duration_den = 1000;
	} else {
		v->sync_duration_num = rate / 1000;
		v->sync_duration_den = 1;
	}
	/*
	 * Horizontal timings in link pixels, as the vendor HDMI driver gives
	 * them (1716 for 480i), width in picture pixels. video_clk in Hz, as
	 * aml_media's invalid vinfo and its dummy encoder use it; the vendor
	 * HDMI driver stores kHz there, and nothing in this setup reads it.
	 */
	v->video_clk = m->clock * 1000 * rep;
	v->htotal = m->htotal * rep;
	v->vtotal = m->vtotal;
	v->hsw = (m->hsync_end - m->hsync_start) * rep;
	v->hbp = (m->htotal - m->hsync_end) * rep;
	v->hfp = (m->hsync_start - m->hdisplay) * rep;
	v->vsw = m->vsync_end - m->vsync_start;
	v->vbp = m->vtotal - m->vsync_end;
	v->vfp = m->vsync_start - m->vdisplay;
	/* 480i and 576i run on ENCI, as 720-pixel double-clocked modes. */
	v->viu_mux = interlace && (m->flags & DRM_MODE_FLAG_DBLCLK) ?
		     VIU_MUX_ENCI : VIU_MUX_ENCP;
}

static void s7d_amlmedia_mode_set(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct s7d_amlmedia *am = s7d_amlmedia;
	enum vmode_e vmode = VMODE_HDMI;

	/* Still VMODE_INVALID: readers skip the fields while they change. */
	s7d_amlmedia_fill(am, crtc->dev, mode);
	if (!s7d_amlmedia_served) {
		if (vout_register_server(&am->server)) {
			pr_err("cannot register the video output server\n");
			return;
		}
		s7d_amlmedia_served = true;
	}
	/* Fields before the valid mode; the vendor readers take no barrier. */
	smp_store_release(&am->vinfo.mode, VMODE_HDMI);
	update_curr_vout_server(S7D_AMLMEDIA_VOUT, &am->server);
	vout_func_set_state(S7D_AMLMEDIA_VOUT, VMODE_HDMI);
	/* Listeners may write VPU registers now: the output runs. */
	vout_notifier_call_chain(VOUT_EVENT_MODE_CHANGE, &vmode);
}

static void s7d_amlmedia_mode_clear(struct drm_crtc *crtc)
{
	struct s7d_amlmedia *am = s7d_amlmedia;

	/*
	 * No notification: listeners may touch VPU registers, which must not
	 * be accessed once the output stops. With the mode invalid, a later
	 * one from elsewhere leaves them alone.
	 */
	WRITE_ONCE(am->vinfo.mode, VMODE_INVALID);
	strscpy(am->name, "invalid");
}

static const struct s7d_ext_ops s7d_amlmedia_ops = {
	.mode_set = s7d_amlmedia_mode_set,
	.mode_clear = s7d_amlmedia_mode_clear,
};

static int __init s7d_amlmedia_init(void)
{
	struct s7d_amlmedia *am;
	int ret;

	am = kzalloc(sizeof(*am), GFP_KERNEL);
	if (!am)
		return -ENOMEM;
	am->server.name = "s7d_vout_server";
	am->server.connector_type = DRM_MODE_CONNECTOR_MESON_HDMIA_A;
	am->server.op = s7d_amlmedia_vout_ops;
	am->server.data = am;
	s7d_amlmedia_reset(am);
	s7d_amlmedia = am;
	/* Nothing reaches aml_media before the display runs a mode. */
	ret = s7d_ext_register(&s7d_amlmedia_ops);
	if (ret) {
		s7d_amlmedia = NULL;
		kfree(am);
	}
	return ret;
}
module_init(s7d_amlmedia_init);

static void __exit s7d_amlmedia_exit(void)
{
	struct s7d_amlmedia *am = s7d_amlmedia;

	s7d_ext_unregister(&s7d_amlmedia_ops);
	if (!s7d_amlmedia_served) {
		kfree(am);
		return;
	}
	vout_unregister_server(&am->server);
	/*
	 * get_current_vinfo() calls get_vinfo without a lock or RCU, and a
	 * reader may be preempted with the op loaded or inside it. Wait until
	 * every task has scheduled voluntarily, so none runs this module's
	 * code when it is freed; the loader's synchronize_rcu() covers readers
	 * in interrupts.
	 */
	synchronize_rcu_tasks();
	/* aml_media may still hold &am->vinfo, now invalid: keep it. */
	kmemleak_not_leak(am);
}
module_exit(s7d_amlmedia_exit);

MODULE_DESCRIPTION("Amlogic S7D display mode for the Amlogic media stack");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(S7D_DRM);
