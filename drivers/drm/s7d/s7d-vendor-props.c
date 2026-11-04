// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/cec.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-fence.h>
#include <linux/hdmi.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_plane.h>
#include <drm/drm_property.h>
#include <drm/drm_writeback.h>

#include "s7d-crtc.h"
#include "s7d-plane.h"
#include "s7d-vendor-props.h"

/*
 * The properties of the vendor DRM driver (aml_drm), with its names, types,
 * ranges, enum lists and initial values, so that vendor userspace finds what
 * it looks up by name. Objects keep their native standard properties.
 */

/* meson.crtc.hdr_policy */
#define S7D_VENDOR_HDR_FOLLOW_SINK	0
#define S7D_VENDOR_HDR_FOLLOW_SOURCE	1
#define S7D_VENDOR_HDR_FOLLOW_FORCE	2

/* osd_pixelformat bits RGBA_8888, RGBX_8888, RGB_888, RGB_565, BGRA_8888 */
#define S7D_VENDOR_OSD_PIXELFORMATS	0x1f
/* video_pixelformat bits YCBCR_422_SP, YCBCR_422_I */
#define S7D_VENDOR_VIDEO_PIXELFORMATS	0x3
/* drm_policy_mask of S7D revision B: HDR_BEFORE_BLEND only */
#define S7D_VENDOR_POLICY_MASK		BIT(2)
/* max_fb_size without a device tree limit: 1920x1080 as height << 16 | width */
#define S7D_VENDOR_MAX_FB_SIZE		(1080 << 16 | 1920)
/* osd_capability of the S7D OSD blocks in the vendor device tree */
#define S7D_VENDOR_OSD_CAPABILITY	0x80000c0fULL
/*
 * video_cap layer bits (avail, scaler, AFBC, alpha): VD1 and VD2 with scaler
 * and alpha, only VD1 with the Amlogic FBC decoder. The vendor reports the
 * mask of both layers on each plane.
 */
#define S7D_VENDOR_VIDEO_CAP		0x1317
/* video_min_size and video_max_size: width << 16 | height of the source */
#define S7D_VENDOR_VIDEO_MIN_SIZE	(64 << 16 | 64)
#define S7D_VENDOR_VD1_MAX_SIZE		(4096 << 16 | 2160)
#define S7D_VENDOR_VD2_MAX_SIZE		(2048 << 16 | 1080)
/* meson.connector_type of the vendor HDMI A connector */
#define S7D_VENDOR_CONNECTOR_HDMIA	160
/* hdmi_hdr_status */
#define S7D_VENDOR_HDR_STATUS_ST2084	3
#define S7D_VENDOR_HDR_STATUS_OTHERS	4
#define S7D_VENDOR_HDR_STATUS_HLG	5
#define S7D_VENDOR_HDR_STATUS_SDR	6
/* hdr_cap bits: SDR, traditional HDR, SMPTE ST 2084, HLG (the EDID EOTFs), HDR10+ */
#define S7D_VENDOR_HDR_CAP_ST2084	BIT(2)
#define S7D_VENDOR_HDR_CAP_HLG		BIT(3)
#define S7D_VENDOR_HDR_CAP_HDR10PLUS	BIT(4)
/* dc_cap bits, one per format and depth */
#define S7D_VENDOR_DC_Y420_12		BIT(0)
#define S7D_VENDOR_DC_Y420_10		BIT(1)
#define S7D_VENDOR_DC_Y420_8		BIT(2)
#define S7D_VENDOR_DC_Y444_12		BIT(3)
#define S7D_VENDOR_DC_Y444_10		BIT(4)
#define S7D_VENDOR_DC_Y444_8		BIT(5)
#define S7D_VENDOR_DC_Y422_12		BIT(6)
#define S7D_VENDOR_DC_RGB_12		BIT(7)
#define S7D_VENDOR_DC_RGB_10		BIT(8)
#define S7D_VENDOR_DC_RGB_8		BIT(9)
#define S7D_VENDOR_DC_LIMIT		BIT(10)
/* sink_type */
#define S7D_VENDOR_SINK_NONE		BIT(0)
#define S7D_VENDOR_SINK_REPEATER	BIT(1)
#define S7D_VENDOR_SINK_SINK		BIT(2)
/* EDID data blocks the DRM EDID parser leaves out */
#define S7D_VENDOR_HDR10PLUS_OUI	0x90848b
#define S7D_VENDOR_CTA_EXTENSION	0x02
#define S7D_VENDOR_CTA_VENDOR		3
#define S7D_VENDOR_CTA_EXTENDED		7
#define S7D_VENDOR_CTA_EXT_VSVDB	0x01
#define S7D_VENDOR_CTA_EXT_HF_SCDB	0x79
/* CAPTURE_PORT: the TVIN ports of the vendor writeback */
#define S7D_VENDOR_CAPTURE_VIDEO	0xa002
#define S7D_VENDOR_CAPTURE_OSD		0xa005
#define S7D_VENDOR_CAPTURE_VPP		0xa008
/* hdr_conversion_ctrl output codes */
#define S7D_VENDOR_HDR_OUTPUT_HDR10	1
#define S7D_VENDOR_HDR_OUTPUT_HLG	2
#define S7D_VENDOR_HDR_OUTPUT_SDR	6

struct s7d_vendor_crtc_props {
	struct drm_property *hdr_policy;
	struct drm_property *dv_enable;
	struct drm_property *brr_update;
	struct drm_property *dv_mode;
	struct drm_property *background_color;
	struct drm_property *osd_pixelformat;
	struct drm_property *video_pixelformat;
	struct drm_property *vpu_clk;
	struct drm_property *hdr_conversion_cap;
	struct drm_property *hdr_conversion_ctrl;
	struct drm_property *force_output;
	struct drm_property *drm_policy_mask;
	struct drm_property *nonblock_by_vblank;
	struct drm_property *dv_support_info;
	struct drm_property *vrr_type;
	struct drm_property *game_rate;
	struct drm_property *crtc_max_out_size;
};

struct s7d_vendor_osd_props {
	struct drm_property *scaling_filter;
	struct drm_property *occupied;
	struct drm_property *max_fb_size;
	struct drm_property *osd_capability;
	struct drm_property *rotation_reflect;
	struct drm_property *sec_en;
	struct drm_property *palette;
	struct drm_property *unsupport_nonafbc;
	struct drm_property *dimming;
};

struct s7d_vendor_video_props {
	struct drm_property *async_in_fence;
	struct drm_property *present_fence;
	struct drm_property *release_fence;
	struct drm_property *signal_format;
	struct drm_property *video_cap;
	struct drm_property *min_size;
	struct drm_property *max_size;
};

struct s7d_vendor_conn_props {
	struct drm_property *color_depth;
	struct drm_property *color_space;
	struct drm_property *hdr_status;
	struct drm_property *static_meta;
	struct drm_property *hdr_priority;
	/* The range properties of the vendor table, in its order */
	struct drm_property *hdcp_user;
	struct drm_property *hdcp_topo;
	struct drm_property *hdcp_ver;
	struct drm_property *hdcp_mode;
	struct drm_property *hdr_cap;
	struct drm_property *hdr_cap_rx;
	struct drm_property *dv_cap;
	struct drm_property *dv_cap_rx;
	struct drm_property *dc_cap;
	struct drm_property *contenttype_cap;
	struct drm_property *allm_cap;
	struct drm_property *allm;
	struct drm_property *update;
	struct drm_property *avmute;
	struct drm_property *ready;
	struct drm_property *frac_rate_policy;
	struct drm_property *hdmi_used;
	struct drm_property *sink_type;
	struct drm_property *edid_valid;
	struct drm_property *vrr_capable_type;
	struct drm_property *sink_device_type;
	struct drm_property *scan_info;
	struct drm_property *connector_type;
};

struct s7d_vendor_props {
	struct drm_device *drm;
	struct drm_connector *hdmi;
	/* Optional: vpu_clk reads 0 without it. */
	struct clk *vpu_clk;
	struct s7d_vendor_crtc_props crtc;
	struct s7d_vendor_osd_props osd;
	struct s7d_vendor_video_props video;
	struct s7d_vendor_conn_props conn;
	/* HDR_STATIC_META of the current EDID; under the connection mutex */
	struct drm_property_blob *static_meta;
	struct drm_connector *writeback;
	struct drm_property *capture_port;
	/*
	 * The display's HDMI and writeback connector functions, and copies with
	 * the vendor ones that the connectors use until the device goes.
	 */
	const struct drm_connector_funcs *conn_base, *wb_base;
	struct drm_connector_funcs conn_funcs, wb_funcs;
};

struct s7d_vendor_conn_state {
	struct drm_connector_state base;
	struct s7d_vendor_conn_values v;
};

static_assert(offsetof(struct s7d_vendor_conn_state, base) == 0);

/* Raw EDID capabilities that struct drm_display_info does not carry */
struct s7d_vendor_sink_ext {
	/* CNC0..CNC3 of the HDMI VSDB */
	u8 content_types;
	bool allm;
	bool hdr10plus;
};

/* The vendor hdr_priority default comes from its HDMI boot parameter. */
static unsigned int hdr_priority;
module_param(hdr_priority, uint, 0444);
MODULE_PARM_DESC(hdr_priority,
		 "Initial vendor hdr_priority: bits 3:0 1 hide Dolby Vision, 2 also HDR (default: 0)");

static const struct drm_prop_enum_list s7d_vendor_hdr_policies[] = {
	{ S7D_VENDOR_HDR_FOLLOW_SINK, "HDR_POLICY_FOLLOW_SINK" },
	{ S7D_VENDOR_HDR_FOLLOW_SOURCE, "HDR_POLICY_FOLLOW_SOURCE" },
	{ S7D_VENDOR_HDR_FOLLOW_FORCE, "HDR_POLICY_FOLLOW_FORCE_MODE" },
};

/* Output format codes of the Amlogic colour management (amcsc). */
static const struct drm_prop_enum_list s7d_vendor_force_outputs[] = {
	{ 0, "UNKNOWN_FMT" },
	{ 1, "BT709" },
	{ 2, "BT709_HDR" },
	{ 3, "BT2020" },
	{ 4, "BT2020_PQ" },
	{ 5, "BT2020_PQ_DYNAMIC" },
	{ 6, "BT2020_HLG" },
	{ 7, "BT2100_IPT" },
	{ 8, "BT2020YUV_BT2020RGB_CUVA" },
	{ 11, "BT_BYPASS" },
};

static const struct drm_prop_enum_list s7d_vendor_vrr_types[] = {
	{ 0, "QMS-VRR" },
	{ 1, "GAME-VRR" },
};

/* The vendor superset of the standard enum, under its own name. */
static const struct drm_prop_enum_list s7d_vendor_scaling_filters[] = {
	{ 0, "Default" },
	{ 1, "Nearest Neighbor" },
	{ 2, "Bicubic_Sharp" },
	{ 3, "Bicubic" },
	{ 4, "Bilinear" },
	{ 5, "2Point_Bilinear" },
	{ 6, "3Point_Triangle_Sharp" },
	{ 7, "3Point_Triangle" },
	{ 8, "4Point_Triangle" },
	{ 9, "4Point_BSPline" },
	{ 10, "3Point_BSPline" },
	{ 11, "Repeate" },
};

static const struct drm_prop_enum_list s7d_vendor_reflections[] = {
	{ DRM_MODE_ROTATE_0, "reflect-0" },
	{ DRM_MODE_REFLECT_X, "reflect-x" },
	{ DRM_MODE_REFLECT_Y, "reflect-y" },
	{ DRM_MODE_REFLECT_MASK, "reflect-all" },
};

static const struct drm_prop_enum_list s7d_vendor_signal_formats[] = {
	{ 0, "sdr" },
	{ 1, "hdr10" },
	{ 2, "hdr10prime" },
	{ 3, "cuva_hdr" },
};

static const struct drm_prop_enum_list s7d_vendor_color_spaces[] = {
	{ HDMI_COLORSPACE_RGB, "RGB" },
	{ HDMI_COLORSPACE_YUV422, "422" },
	{ HDMI_COLORSPACE_YUV444, "444" },
	{ HDMI_COLORSPACE_YUV420, "420" },
	{ HDMI_COLORSPACE_RESERVED6, "HDMI_COLORSPACE_RESERVED6" },
};

static const struct drm_prop_enum_list s7d_vendor_hdr_statuses[] = {
	{ 0, "HDR10Plus-VSIF" },
	{ 1, "DolbyVision-Std" },
	{ 2, "DolbyVision-Lowlatency" },
	{ S7D_VENDOR_HDR_STATUS_ST2084, "HDR10-GAMMA_ST2084" },
	{ S7D_VENDOR_HDR_STATUS_OTHERS, "HDR10-others" },
	{ S7D_VENDOR_HDR_STATUS_HLG, "HDR10-GAMMA_HLG" },
	{ S7D_VENDOR_HDR_STATUS_SDR, "SDR" },
};

static const struct drm_prop_enum_list s7d_vendor_capture_ports[] = {
	{ S7D_VENDOR_CAPTURE_VIDEO, "video" },
	{ S7D_VENDOR_CAPTURE_OSD, "osd" },
	{ S7D_VENDOR_CAPTURE_VPP, "video&osd" },
};

static const struct drm_prop_enum_list s7d_vendor_scan_infos[] = {
	{ HDMI_SCAN_MODE_NONE, "no data" },
	{ HDMI_SCAN_MODE_OVERSCAN, "overscan" },
	{ HDMI_SCAN_MODE_UNDERSCAN, "underscan" },
	{ HDMI_SCAN_MODE_RESERVED, "future" },
};

/* The vendor range table: all start at 0 and are attached with 0. */
static const struct {
	const char *name;
	u64 max;
	size_t offset;
} s7d_vendor_conn_ranges[] = {
#define S7D_VENDOR_CONN_RANGE(field, name, max) \
	{ name, max, offsetof(struct s7d_vendor_conn_props, field) }
	S7D_VENDOR_CONN_RANGE(hdcp_user, "hdcp_user", 1),
	S7D_VENDOR_CONN_RANGE(hdcp_topo, "hdcp_topo", 1),
	S7D_VENDOR_CONN_RANGE(hdcp_ver, "hdcp_ver", 36),
	S7D_VENDOR_CONN_RANGE(hdcp_mode, "hdcp_mode", 36),
	S7D_VENDOR_CONN_RANGE(hdr_cap, "hdr_cap", 64),
	S7D_VENDOR_CONN_RANGE(hdr_cap_rx, "hdr_cap_rx", 64),
	S7D_VENDOR_CONN_RANGE(dv_cap, "dv_cap", 512),
	S7D_VENDOR_CONN_RANGE(dv_cap_rx, "dv_cap_rx", 512),
	S7D_VENDOR_CONN_RANGE(dc_cap, "dc_cap", S7D_VENDOR_DC_LIMIT),
	S7D_VENDOR_CONN_RANGE(contenttype_cap, "contenttype_cap", 1023),
	S7D_VENDOR_CONN_RANGE(allm_cap, "allm_cap", 1),
	S7D_VENDOR_CONN_RANGE(allm, "allm", 3),
	S7D_VENDOR_CONN_RANGE(update, "UPDATE", 1),
	S7D_VENDOR_CONN_RANGE(avmute, "MESON_DRM_HDMITX_PROP_AVMUTE", 1),
	S7D_VENDOR_CONN_RANGE(ready, "ready", 1),
	S7D_VENDOR_CONN_RANGE(frac_rate_policy, "FRAC_RATE_POLICY", 1),
	S7D_VENDOR_CONN_RANGE(hdmi_used, "hdmi_used", 1),
	S7D_VENDOR_CONN_RANGE(sink_type, "sink_type", 8),
	S7D_VENDOR_CONN_RANGE(edid_valid, "edid_valid", 1),
	S7D_VENDOR_CONN_RANGE(vrr_capable_type, "vrr_capable_type", 3),
	S7D_VENDOR_CONN_RANGE(sink_device_type, "sink_device_type", 1),
#undef S7D_VENDOR_CONN_RANGE
};

/* Creates properties and attaches each to one object; any failure fails the whole. */
struct s7d_vendor_builder {
	struct drm_device *drm;
	struct drm_mode_object *obj;
	bool failed;
};

/* Without an object, the properties are only created. */
static struct drm_property *s7d_vendor_add(struct s7d_vendor_builder *b,
					   struct drm_property *prop, u64 value)
{
	if (prop && b->obj)
		drm_object_attach_property(b->obj, prop, value);
	else if (!prop)
		b->failed = true;
	return prop;
}

static struct drm_property *s7d_vendor_add_range(struct s7d_vendor_builder *b, u32 flags,
						 const char *name, u64 min, u64 max, u64 value)
{
	return s7d_vendor_add(b, drm_property_create_range(b->drm, flags, name, min, max), value);
}

static struct drm_property *s7d_vendor_add_bool(struct s7d_vendor_builder *b, const char *name)
{
	return s7d_vendor_add(b, drm_property_create_bool(b->drm, 0, name), 0);
}

static struct drm_property *s7d_vendor_add_enum(struct s7d_vendor_builder *b, const char *name,
						const struct drm_prop_enum_list *list,
						unsigned int count, u64 value)
{
	return s7d_vendor_add(b, drm_property_create_enum(b->drm, 0, name, list, count), value);
}

/* The DRM InfoFrame on the HDMI link of @crtc, as hdr_conversion_ctrl reports it. */
static u64 s7d_vendor_hdr_output(const struct s7d_vendor_props *vp, const struct drm_crtc *crtc)
{
	const struct drm_connector_state *state = vp->hdmi ? vp->hdmi->state : NULL;

	if (!state || state->crtc != crtc || !state->hdmi.infoframes.hdr_drm.set)
		return S7D_VENDOR_HDR_OUTPUT_SDR;
	switch (state->hdmi.infoframes.hdr_drm.data.drm.eotf) {
	case HDMI_EOTF_SMPTE_ST2084:
		return S7D_VENDOR_HDR_OUTPUT_HDR10;
	case HDMI_EOTF_BT_2100_HLG:
		return S7D_VENDOR_HDR_OUTPUT_HLG;
	default:
		return S7D_VENDOR_HDR_OUTPUT_SDR;
	}
}

int s7d_vendor_props_crtc_get(const struct s7d_vendor_props *vp, struct drm_crtc *crtc,
			      const struct drm_crtc_state *base,
			      struct drm_property *property, u64 *val)
{
	const struct s7d_crtc_vendor *v =
		&container_of(base, const struct s7d_crtc_state, base)->vendor;
	const struct s7d_vendor_crtc_props *p;

	if (!vp)
		return -EINVAL;
	p = &vp->crtc;
	if (property == p->hdr_policy)
		*val = v->hdr_policy;
	else if (property == p->dv_enable)
		*val = v->dv_enable;
	else if (property == p->brr_update)
		*val = v->brr_update;
	else if (property == p->dv_mode)
		*val = v->dv_mode;
	else if (property == p->background_color)
		*val = v->background_color;
	else if (property == p->osd_pixelformat)
		*val = S7D_VENDOR_OSD_PIXELFORMATS;
	else if (property == p->video_pixelformat)
		*val = S7D_VENDOR_VIDEO_PIXELFORMATS;
	else if (property == p->vpu_clk)
		/* In Hz, beyond the declared range, as the vendor reports it. */
		*val = vp->vpu_clk ? clk_get_rate(vp->vpu_clk) : 0;
	else if (property == p->hdr_conversion_cap)
		/* No SDR/HDR conversion engine. */
		*val = 0;
	else if (property == p->hdr_conversion_ctrl)
		*val = s7d_vendor_hdr_output(vp, crtc);
	else if (property == p->force_output)
		*val = v->force_output;
	else if (property == p->drm_policy_mask)
		*val = S7D_VENDOR_POLICY_MASK;
	else if (property == p->nonblock_by_vblank)
		*val = v->nonblock_by_vblank;
	else if (property == p->dv_support_info)
		/* No Dolby Vision. */
		*val = 0;
	else if (property == p->vrr_type)
		*val = v->vrr_type;
	else if (property == p->game_rate)
		*val = v->game_rate;
	else
		return -EINVAL;
	return 0;
}

int s7d_vendor_props_crtc_set(const struct s7d_vendor_props *vp, struct drm_crtc *crtc,
			      struct drm_crtc_state *base,
			      struct drm_property *property, u64 val)
{
	struct s7d_crtc_vendor *v = &to_s7d_crtc_state(base)->vendor;
	const struct s7d_vendor_crtc_props *p;

	if (!vp)
		return -EINVAL;
	p = &vp->crtc;
	if (property == p->hdr_policy)
		v->hdr_policy = val;
	else if (property == p->dv_enable)
		v->dv_enable = val;
	else if (property == p->brr_update)
		v->brr_update = val;
	else if (property == p->dv_mode)
		v->dv_mode = val;
	else if (property == p->background_color)
		v->background_color = val;
	else if (property == p->force_output)
		v->force_output = val;
	else if (property == p->nonblock_by_vblank)
		v->nonblock_by_vblank = val;
	else if (property == p->vrr_type)
		v->vrr_type = val;
	else if (property == p->game_rate)
		v->game_rate = val;
	/* The vendor driver accepts and drops writes to its read-only ones. */
	else if (property != p->osd_pixelformat && property != p->video_pixelformat &&
		 property != p->vpu_clk && property != p->hdr_conversion_cap &&
		 property != p->hdr_conversion_ctrl && property != p->drm_policy_mask &&
		 property != p->dv_support_info)
		return -EINVAL;
	return 0;
}

/*
 * In the vendor attach order. The vendor driver creates VRR_TYPE and
 * GAME_RATE twice; one of each serves the same lookups by name.
 */
static int s7d_vendor_crtc_attach(struct s7d_vendor_props *vp, struct drm_crtc *crtc)
{
	struct s7d_vendor_builder b = { .drm = vp->drm, .obj = &crtc->base };
	struct s7d_vendor_crtc_props *p = &vp->crtc;

	p->hdr_policy = s7d_vendor_add_enum(&b, "meson.crtc.hdr_policy", s7d_vendor_hdr_policies,
					    ARRAY_SIZE(s7d_vendor_hdr_policies),
					    S7D_VENDOR_HDR_FOLLOW_SINK);
	p->dv_enable = s7d_vendor_add_bool(&b, "dv_enable");
	p->brr_update = s7d_vendor_add_bool(&b, "brr_update");
	p->dv_mode = s7d_vendor_add_bool(&b, "dv_mode");
	p->background_color = s7d_vendor_add_range(&b, 0, "BACKGROUND_COLOR", 0, U64_MAX, 0);
	p->osd_pixelformat = s7d_vendor_add_range(&b, 0, "osd_pixelformat", 0, 65535, 0);
	p->video_pixelformat = s7d_vendor_add_range(&b, 0, "video_pixelformat", 0, 65535, 0);
	p->vpu_clk = s7d_vendor_add_range(&b, 0, "vpu_clk", 0, 65535, 0);
	p->hdr_conversion_cap = s7d_vendor_add_range(&b, 0, "hdr_conversion_cap", 0, U32_MAX, 0);
	p->hdr_conversion_ctrl = s7d_vendor_add_range(&b, 0, "hdr_conversion_ctrl", 0, 36, 0);
	p->force_output = s7d_vendor_add_enum(&b, "force_output", s7d_vendor_force_outputs,
					      ARRAY_SIZE(s7d_vendor_force_outputs), 0);
	p->drm_policy_mask = s7d_vendor_add_range(&b, 0, "drm_policy_mask", 0, U64_MAX, 0);
	p->nonblock_by_vblank = s7d_vendor_add_bool(&b, "nonblock_by_vblank");
	p->dv_support_info = s7d_vendor_add_range(&b, 0, "dv_support_info", 0, 15, 0);
	p->vrr_type = s7d_vendor_add_enum(&b, "VRR_TYPE", s7d_vendor_vrr_types,
					  ARRAY_SIZE(s7d_vendor_vrr_types), 0);
	p->game_rate = s7d_vendor_add_range(&b, 0, "GAME_RATE", 0, U16_MAX, 0);
	/* No output size limit in the device tree: 0, as on the vendor S7D. */
	p->crtc_max_out_size = s7d_vendor_add_range(&b, DRM_MODE_PROP_IMMUTABLE,
						    "crtc_max_out_size", 0, U32_MAX, 0);
	if (b.failed)
		return -ENOMEM;
	s7d_crtc_set_vendor_props(crtc, vp);
	return 0;
}

int s7d_vendor_props_plane_get(const struct s7d_vendor_props *vp, struct drm_plane *plane,
			       const struct drm_plane_state *base,
			       struct drm_property *property, u64 *val)
{
	const struct s7d_plane_vendor *v =
		&container_of(base, const struct s7d_plane_state, base)->vendor;
	const struct s7d_vendor_video_props *vd;
	const struct s7d_vendor_osd_props *o;

	if (!vp)
		return -EINVAL;
	o = &vp->osd;
	vd = &vp->video;
	if (property == o->occupied)
		*val = v->occupied;
	else if (property == o->osd_capability)
		*val = S7D_VENDOR_OSD_CAPABILITY;
	else if (property == o->rotation_reflect)
		/* One view of the reflection in the standard rotation property */
		*val = (base->rotation & DRM_MODE_REFLECT_MASK) ?: DRM_MODE_ROTATE_0;
	else if (property == o->sec_en || property == o->unsupport_nonafbc)
		*val = 0;
	else if (property == o->palette)
		*val = v->palette ? v->palette->base.id : 0;
	else if (property == o->dimming)
		*val = v->dimming;
	else if (property == vd->async_in_fence)
		*val = I642U64(-1);
	else if (property == vd->present_fence || property == vd->release_fence)
		*val = 0;
	else if (property == vd->signal_format)
		*val = v->signal_format;
	else if (property == vd->video_cap)
		*val = S7D_VENDOR_VIDEO_CAP;
	else if (property == vd->min_size)
		*val = S7D_VENDOR_VIDEO_MIN_SIZE;
	else if (property == vd->max_size)
		*val = s7d_plane_slot(plane) == S7D_PLANE_VIDEO2 ?
			S7D_VENDOR_VD2_MAX_SIZE : S7D_VENDOR_VD1_MAX_SIZE;
	else
		return -EINVAL;
	return 0;
}

int s7d_vendor_props_plane_set(const struct s7d_vendor_props *vp, struct drm_plane *plane,
			       struct drm_plane_state *base,
			       struct drm_property *property, u64 val)
{
	struct s7d_plane_vendor *v = &to_s7d_plane_state(base)->vendor;
	const struct s7d_vendor_video_props *vd;
	const struct s7d_vendor_osd_props *o;
	bool replaced;

	if (!vp)
		return -EINVAL;
	o = &vp->osd;
	vd = &vp->video;
	if (property == o->occupied) {
		v->occupied = val;
	} else if (property == o->rotation_reflect) {
		base->rotation = DRM_MODE_ROTATE_0 | (val & DRM_MODE_REFLECT_MASK);
	} else if (property == o->sec_en) {
		/* No secure OSD path */
		if (val)
			return -EINVAL;
	} else if (property == o->palette) {
		return drm_property_replace_blob_from_id(plane->dev, &v->palette, val,
							 -1, -1, &replaced);
	} else if (property == o->dimming) {
		v->dimming = val;
	} else if (property == vd->async_in_fence) {
		if (v->async_in_fence)
			return -EINVAL;
		if (U642I64(val) == -1)
			return 0;
		v->async_in_fence = sync_file_get_fence(U642I64(val));
		if (!v->async_in_fence)
			return -EINVAL;
	} else if (property == vd->present_fence || property == vd->release_fence) {
		s32 __user *fence = u64_to_user_ptr(val);

		/* No video fences yet: report none, as before the vendor fills one in. */
		if (fence && put_user(-1, fence))
			return -EFAULT;
	} else if (property == vd->signal_format) {
		v->signal_format = val;
	} else if (property != o->osd_capability && property != o->unsupport_nonafbc &&
		   property != vd->video_cap && property != vd->min_size &&
		   property != vd->max_size) {
		/* The vendor driver accepts and drops writes to the read-only ones. */
		return -EINVAL;
	}
	return 0;
}

static int s7d_vendor_osd_create(struct s7d_vendor_props *vp)
{
	struct s7d_vendor_builder b = { .drm = vp->drm };
	struct s7d_vendor_osd_props *p = &vp->osd;

	p->scaling_filter = s7d_vendor_add_enum(&b, "SCALING_FILTER", s7d_vendor_scaling_filters,
						ARRAY_SIZE(s7d_vendor_scaling_filters), 0);
	p->occupied = s7d_vendor_add_bool(&b, "meson.plane.occupied");
	p->max_fb_size = s7d_vendor_add_range(&b, DRM_MODE_PROP_IMMUTABLE, "max_fb_size",
					      0, U32_MAX, 0);
	p->osd_capability = s7d_vendor_add_range(&b, 0, "osd_capability", 0, U32_MAX, 0);
	p->rotation_reflect = s7d_vendor_add_enum(&b, "rotation_reflect", s7d_vendor_reflections,
						  ARRAY_SIZE(s7d_vendor_reflections), 0);
	p->sec_en = s7d_vendor_add_bool(&b, "SEC_EN");
	p->palette = s7d_vendor_add(&b, drm_property_create(vp->drm, DRM_MODE_PROP_BLOB,
							     "palette", 0), 0);
	p->unsupport_nonafbc = s7d_vendor_add_bool(&b, "unsupport_nonafbc");
	p->dimming = s7d_vendor_add_range(&b, 0, "OSD_DIMMING_CTRL", 0, U32_MAX, 0);
	return b.failed ? -ENOMEM : 0;
}

/*
 * The 9 OSD plane properties, in the vendor attach order. SCALING_FILTER is
 * the plane's scaling filter property, kept in the standard state field.
 */
static void s7d_vendor_osd_attach(struct s7d_vendor_props *vp, struct drm_plane *plane)
{
	const struct s7d_vendor_osd_props *p = &vp->osd;
	struct drm_mode_object *obj = &plane->base;

	drm_object_attach_property(obj, p->scaling_filter, 0);
	plane->scaling_filter_property = p->scaling_filter;
	drm_object_attach_property(obj, p->occupied, 0);
	drm_object_attach_property(obj, p->max_fb_size, S7D_VENDOR_MAX_FB_SIZE);
	drm_object_attach_property(obj, p->osd_capability, 0);
	drm_object_attach_property(obj, p->rotation_reflect, DRM_MODE_ROTATE_0);
	drm_object_attach_property(obj, p->sec_en, 0);
	drm_object_attach_property(obj, p->palette, 0);
	drm_object_attach_property(obj, p->unsupport_nonafbc, 0);
	drm_object_attach_property(obj, p->dimming, 0);
	s7d_plane_set_vendor_props(plane, vp);
}

static int s7d_vendor_video_create(struct s7d_vendor_props *vp)
{
	struct s7d_vendor_builder b = { .drm = vp->drm };
	struct s7d_vendor_video_props *p = &vp->video;

	p->async_in_fence = s7d_vendor_add(&b, drm_property_create_signed_range(vp->drm,
						DRM_MODE_PROP_ATOMIC, "ASYNC_IN_FENCE_FD",
						-1, INT_MAX), 0);
	p->present_fence = s7d_vendor_add_range(&b, 0, "video_present_fence", 0, U64_MAX, 0);
	p->release_fence = s7d_vendor_add_range(&b, 0, "video_release_fence", 0, U64_MAX, 0);
	p->signal_format = s7d_vendor_add_enum(&b, "signal_format", s7d_vendor_signal_formats,
					       ARRAY_SIZE(s7d_vendor_signal_formats), 0);
	p->video_cap = s7d_vendor_add_range(&b, 0, "video_cap", 0, 0, 0);
	p->min_size = s7d_vendor_add_range(&b, 0, "video_min_size", 0, 0, 0);
	p->max_size = s7d_vendor_add_range(&b, 0, "video_max_size", 0, 0, 0);
	return b.failed ? -ENOMEM : 0;
}

/* The 7 video plane properties, in the vendor attach order. */
static void s7d_vendor_video_attach(struct s7d_vendor_props *vp, struct drm_plane *plane)
{
	const struct s7d_vendor_video_props *p = &vp->video;
	struct drm_mode_object *obj = &plane->base;

	drm_object_attach_property(obj, p->async_in_fence, I642U64(-1));
	drm_object_attach_property(obj, p->present_fence, 0);
	drm_object_attach_property(obj, p->release_fence, 0);
	drm_object_attach_property(obj, p->signal_format, 0);
	drm_object_attach_property(obj, p->video_cap, 0);
	drm_object_attach_property(obj, p->min_size, 0);
	drm_object_attach_property(obj, p->max_size, 0);
	s7d_plane_set_vendor_props(plane, vp);
}

/* OSD1, OSD2, VD1 and VD2 carry the vendor plane properties; the cursor none. */
static int s7d_vendor_planes_attach(struct s7d_vendor_props *vp,
				    const struct s7d_vendor_objs *objs)
{
	int ret;

	if (!s7d_plane_is_primary(objs->primary) || !s7d_plane_is_native(objs->overlay) ||
	    !s7d_plane_is_native(objs->video) || !s7d_plane_is_native(objs->video2))
		return -EINVAL;
	ret = s7d_vendor_osd_create(vp);
	if (!ret)
		ret = s7d_vendor_video_create(vp);
	if (ret)
		return ret;
	s7d_vendor_osd_attach(vp, objs->primary);
	s7d_vendor_osd_attach(vp, objs->overlay);
	s7d_vendor_video_attach(vp, objs->video);
	s7d_vendor_video_attach(vp, objs->video2);
	return 0;
}

static struct s7d_vendor_props *s7d_vendor_conn_props(const struct drm_connector *connector)
{
	return container_of(connector->funcs, struct s7d_vendor_props, conn_funcs);
}

static struct drm_connector_state *s7d_vendor_conn_duplicate(struct drm_connector *connector)
{
	const struct s7d_vendor_conn_state *current_state;
	struct s7d_vendor_conn_state *state;

	if (!connector->state)
		return NULL;
	current_state = container_of(connector->state, struct s7d_vendor_conn_state, base);
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	state->v = current_state->v;
	/* An output ran in the current state: the HDMI atomic check sets output_bpc. */
	state->v.used |= current_state->base.crtc && current_state->base.hdmi.output_bpc;
	state->v.avmute = false;
	state->v.depth_written = false;
	state->v.update = false;
	return &state->base;
}

static void s7d_vendor_conn_destroy(struct drm_connector *connector,
				    struct drm_connector_state *state)
{
	__drm_atomic_helper_connector_destroy_state(state);
	kfree(container_of(state, struct s7d_vendor_conn_state, base));
}

/*
 * Let the display reset the connector state, then move it into a vendor one;
 * it holds no references yet. The vendor defaults are RGB 8-bit, no request.
 * On failure the connector has no state, like the reset helpers leave it.
 */
static void s7d_vendor_conn_reset(struct drm_connector *connector)
{
	const struct s7d_vendor_props *vp = s7d_vendor_conn_props(connector);
	struct s7d_vendor_conn_state *state;

	vp->conn_base->reset(connector);
	if (!connector->state)
		return;
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		state->base = *connector->state;
		state->v.color_space = HDMI_COLORSPACE_RGB;
		state->v.color_depth = 8;
		state->v.hdr_priority = hdr_priority;
		/* What the vendor driver and the DRM helper send by default */
		state->v.scan_info = HDMI_SCAN_MODE_UNDERSCAN;
	}
	kfree(connector->state);
	connector->state = state ? &state->base : NULL;
}

const struct s7d_vendor_conn_values *
s7d_vendor_props_conn(const struct drm_connector_state *state)
{
	if (!state || !state->connector ||
	    state->connector->funcs->atomic_duplicate_state != s7d_vendor_conn_duplicate)
		return NULL;
	return &container_of(state, const struct s7d_vendor_conn_state, base)->v;
}

void s7d_vendor_props_set_output(struct drm_connector_state *state, u8 color_space,
				 u8 depth, bool fractional)
{
	struct s7d_vendor_conn_values *v;

	if (!s7d_vendor_props_conn(state))
		return;
	v = &container_of(state, struct s7d_vendor_conn_state, base)->v;
	v->output_space = color_space;
	v->output_depth = depth;
	v->fractional = fractional;
}

void s7d_vendor_props_take_request(struct drm_connector_state *state)
{
	struct s7d_vendor_conn_values *v;

	if (!s7d_vendor_props_conn(state))
		return;
	v = &container_of(state, struct s7d_vendor_conn_state, base)->v;
	v->link_request = v->request;
	v->link_space = v->color_space;
	v->link_depth = v->color_depth;
}

/* An output runs on @state: the HDMI atomic check sets output_bpc for it. */
static bool s7d_vendor_conn_active(const struct drm_connector_state *state)
{
	return state->crtc && state->hdmi.output_bpc;
}

void s7d_vendor_props_attr(const struct drm_connector_state *state, u8 *color_space,
			   u8 *depth)
{
	const struct s7d_vendor_conn_values *v = s7d_vendor_props_conn(state);

	*color_space = HDMI_COLORSPACE_RGB;
	*depth = 8;
	if (state && s7d_vendor_conn_active(state)) {
		if (v && v->output_depth) {
			*color_space = v->output_space;
			*depth = v->output_depth;
		} else {
			/* HDMI 4:2:2 always takes the 12-bit container. */
			*color_space = state->hdmi.output_format;
			*depth = state->hdmi.output_format == HDMI_COLORSPACE_YUV422 ?
				12 : state->hdmi.output_bpc;
		}
	} else if (v && v->request) {
		*color_space = v->color_space;
		*depth = v->color_depth;
	}
}

u8 s7d_vendor_props_hdr_policy(const struct drm_crtc_state *state)
{
	return container_of(state, const struct s7d_crtc_state, base)->vendor.hdr_policy;
}

/*
 * The blob id of the connector's EDID property, 0 for none. The property is
 * immutable, so the stored value is the current one;
 * drm_object_property_get_value() is meant for non-atomic drivers.
 */
static u64 s7d_vendor_edid_id(struct drm_connector *connector)
{
	struct drm_property *prop = connector->dev->mode_config.edid_property;
	u64 id;

	if (!prop || drm_object_property_get_default_value(&connector->base, prop, &id))
		return 0;
	return id;
}

/*
 * Walk the CTA data blocks of the current EDID, taken by its property blob:
 * the connector may get a new EDID meanwhile, and its blob stays valid here.
 */
static void s7d_vendor_sink_ext(struct drm_connector *connector, struct s7d_vendor_sink_ext *ext)
{
	struct drm_device *drm = connector->dev;
	struct drm_property_blob *blob;
	unsigned int block, pos, len;
	const u8 *data, *db;
	u64 id;

	memset(ext, 0, sizeof(*ext));
	id = s7d_vendor_edid_id(connector);
	if (!id)
		return;
	blob = drm_property_lookup_blob(drm, id);
	if (!blob)
		return;
	data = blob->data;
	for (block = 1; (block + 1) * EDID_LENGTH <= blob->length; block++) {
		const u8 *ext_block = data + block * EDID_LENGTH;
		unsigned int end = ext_block[2];

		if (ext_block[0] != S7D_VENDOR_CTA_EXTENSION || end < 4 || end >= EDID_LENGTH)
			continue;
		for (pos = 4; pos < end; pos += len + 1) {
			u32 oui;

			db = ext_block + pos;
			len = db[0] & 0x1f;
			if (pos + len >= end)
				break;
			if (db[0] >> 5 == S7D_VENDOR_CTA_VENDOR && len >= 3) {
				oui = db[3] << 16 | db[2] << 8 | db[1];
				/* HDMI VSDB byte 8: CNC3..0; HF-VSDB byte 8 bit 1: ALLM */
				if (oui == HDMI_IEEE_OUI && len >= 8)
					ext->content_types = db[8] & 0xf;
				else if (oui == HDMI_FORUM_IEEE_OUI && len >= 8)
					ext->allm = db[8] & BIT(1);
			} else if (db[0] >> 5 == S7D_VENDOR_CTA_EXTENDED && len >= 5 &&
				   db[1] == S7D_VENDOR_CTA_EXT_VSVDB) {
				oui = db[4] << 16 | db[3] << 8 | db[2];
				if (oui == S7D_VENDOR_HDR10PLUS_OUI)
					ext->hdr10plus = true;
			} else if (db[0] >> 5 == S7D_VENDOR_CTA_EXTENDED && len >= 8 &&
				   db[1] == S7D_VENDOR_CTA_EXT_HF_SCDB) {
				/* The HF-SCDB lays out its fields as the HF-VSDB. */
				ext->allm = db[8] & BIT(1);
			}
		}
	}
	drm_property_blob_put(blob);
}

bool s7d_vendor_props_sink_allm(struct drm_connector *connector)
{
	struct s7d_vendor_sink_ext ext;

	s7d_vendor_sink_ext(connector, &ext);
	return ext.allm;
}

/* hdr_priority: bits 31:28 pick the Linux (0) or Android (1) strategy of the vendor. */
static u32 s7d_vendor_hdr_mask(u32 cap, u32 priority)
{
	switch (priority >> 28) {
	case 0:
		/* 1 hides Dolby Vision only, 2 also HDR10, HDR10+ and HLG. */
		if ((priority & 0xf) == 2)
			cap &= ~(S7D_VENDOR_HDR_CAP_ST2084 | S7D_VENDOR_HDR_CAP_HLG |
				 S7D_VENDOR_HDR_CAP_HDR10PLUS);
		break;
	case 1:
		if (priority & BIT(5))
			cap &= ~(S7D_VENDOR_HDR_CAP_ST2084 | S7D_VENDOR_HDR_CAP_HDR10PLUS);
		if (priority & BIT(6))
			cap &= ~S7D_VENDOR_HDR_CAP_HLG;
		break;
	}
	return cap;
}

static u32 s7d_vendor_hdr_cap(struct drm_connector *connector)
{
	struct s7d_vendor_sink_ext ext;
	u32 cap;

	s7d_vendor_sink_ext(connector, &ext);
	cap = connector->hdr_sink_metadata.hdmi_type1.eotf & 0xf;
	return ext.hdr10plus ? cap | S7D_VENDOR_HDR_CAP_HDR10PLUS : cap;
}

static u32 s7d_vendor_dc_cap(const struct drm_display_info *info)
{
	u32 cap = S7D_VENDOR_DC_RGB_8;

	if (!info->is_hdmi)
		return cap;
	if (info->hdmi.y420_dc_modes & DRM_EDID_YCBCR420_DC_36)
		cap |= S7D_VENDOR_DC_Y420_12;
	if (info->hdmi.y420_dc_modes & DRM_EDID_YCBCR420_DC_30)
		cap |= S7D_VENDOR_DC_Y420_10;
	if (!bitmap_empty(info->hdmi.y420_vdb_modes, 256) ||
	    !bitmap_empty(info->hdmi.y420_cmdb_modes, 256))
		cap |= S7D_VENDOR_DC_Y420_8;
	if (info->color_formats & DRM_COLOR_FORMAT_YCBCR444) {
		if (info->edid_hdmi_ycbcr444_dc_modes & DRM_EDID_HDMI_DC_36)
			cap |= S7D_VENDOR_DC_Y444_12;
		if (info->edid_hdmi_ycbcr444_dc_modes & DRM_EDID_HDMI_DC_30)
			cap |= S7D_VENDOR_DC_Y444_10;
		cap |= S7D_VENDOR_DC_Y444_8;
	}
	if (info->color_formats & DRM_COLOR_FORMAT_YCBCR422)
		cap |= S7D_VENDOR_DC_Y422_12;
	if (info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_36)
		cap |= S7D_VENDOR_DC_RGB_12;
	if (info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_30)
		cap |= S7D_VENDOR_DC_RGB_10;
	return cap;
}

/* The CEA physical address names a repeater when its second digit is set. */
static u32 s7d_vendor_sink_type(const struct drm_connector *connector)
{
	u16 address = connector->display_info.source_physical_address;

	if (connector->status != connector_status_connected)
		return S7D_VENDOR_SINK_NONE;
	if (address != CEC_PHYS_ADDR_INVALID && (address & 0x0f00))
		return S7D_VENDOR_SINK_REPEATER;
	return S7D_VENDOR_SINK_SINK;
}

static bool s7d_vendor_edid_valid(struct drm_connector *connector)
{
	return s7d_vendor_edid_id(connector);
}

/* One blob per EDID content instead of one per read, as the vendor makes. */
static u64 s7d_vendor_static_meta(struct s7d_vendor_props *vp, struct drm_connector *connector)
{
	const struct hdr_static_metadata *hdr = &connector->hdr_sink_metadata.hdmi_type1;
	const u8 meta[3] = { hdr->max_cll, hdr->min_cll, hdr->max_fall };
	struct drm_property_blob *blob;

	if (!vp->static_meta || memcmp(vp->static_meta->data, meta, sizeof(meta))) {
		blob = drm_property_create_blob(connector->dev, sizeof(meta), meta);
		if (!IS_ERR(blob)) {
			drm_property_blob_put(vp->static_meta);
			vp->static_meta = blob;
		}
	}
	return vp->static_meta ? vp->static_meta->base.id : 0;
}

static u64 s7d_vendor_hdr_status(const struct drm_connector_state *state)
{
	if (!state->hdmi.infoframes.hdr_drm.set)
		return S7D_VENDOR_HDR_STATUS_SDR;
	switch (state->hdmi.infoframes.hdr_drm.data.drm.eotf) {
	case HDMI_EOTF_TRADITIONAL_GAMMA_SDR:
		return S7D_VENDOR_HDR_STATUS_SDR;
	case HDMI_EOTF_SMPTE_ST2084:
		return S7D_VENDOR_HDR_STATUS_ST2084;
	case HDMI_EOTF_BT_2100_HLG:
		return S7D_VENDOR_HDR_STATUS_HLG;
	default:
		return S7D_VENDOR_HDR_STATUS_OTHERS;
	}
}

/*
 * Callers hold the connection mutex. Sink values come from the EDID parsed
 * into the connector, read without the mode config mutex as the vendor does.
 */
static int s7d_vendor_conn_get(struct drm_connector *connector,
			       const struct drm_connector_state *state,
			       struct drm_property *property, uint64_t *val)
{
	struct s7d_vendor_props *vp = s7d_vendor_conn_props(connector);
	const struct s7d_vendor_conn_values *v = s7d_vendor_props_conn(state);
	const struct s7d_vendor_conn_props *p = &vp->conn;
	bool active = s7d_vendor_conn_active(state);
	struct s7d_vendor_sink_ext ext;
	u8 space, depth;

	if (!v)
		return -EINVAL;
	if (property == p->color_space || property == p->color_depth) {
		s7d_vendor_props_attr(state, &space, &depth);
		*val = property == p->color_space ? space : depth;
	} else if (property == p->hdr_status) {
		*val = s7d_vendor_hdr_status(state);
	} else if (property == p->static_meta) {
		*val = s7d_vendor_static_meta(vp, connector);
	} else if (property == p->hdr_priority) {
		*val = v->hdr_priority;
	} else if (property == p->hdr_cap) {
		*val = s7d_vendor_hdr_mask(s7d_vendor_hdr_cap(connector), v->hdr_priority);
	} else if (property == p->hdr_cap_rx) {
		*val = s7d_vendor_hdr_cap(connector);
	} else if (property == p->dc_cap) {
		*val = s7d_vendor_dc_cap(&connector->display_info);
	} else if (property == p->contenttype_cap) {
		s7d_vendor_sink_ext(connector, &ext);
		/* No Data, then CNC0..3 as the Graphics, Photo, Cinema and Game types */
		*val = BIT(DRM_MODE_CONTENT_TYPE_NO_DATA) |
			(u32)ext.content_types << DRM_MODE_CONTENT_TYPE_GRAPHICS;
	} else if (property == p->allm_cap) {
		*val = s7d_vendor_props_sink_allm(connector);
	} else if (property == p->allm) {
		*val = v->allm;
	} else if (property == p->avmute) {
		*val = v->avmute;
	} else if (property == p->ready) {
		*val = active && state->link_status == DRM_LINK_STATUS_GOOD;
	} else if (property == p->frac_rate_policy) {
		*val = active && v->fractional;
	} else if (property == p->hdmi_used) {
		*val = v->used || active;
	} else if (property == p->sink_type) {
		*val = s7d_vendor_sink_type(connector);
	} else if (property == p->edid_valid) {
		*val = s7d_vendor_edid_valid(connector);
	} else if (property == p->sink_device_type) {
		*val = !connector->display_info.is_hdmi;
	} else if (property == p->scan_info) {
		*val = v->scan_info;
	} else if (property == p->update || property == p->hdcp_user ||
		   property == p->hdcp_topo || property == p->hdcp_ver ||
		   property == p->hdcp_mode || property == p->dv_cap ||
		   property == p->dv_cap_rx || property == p->vrr_capable_type) {
		/* No HDCP, Dolby Vision or VRR; UPDATE reads 0. */
		*val = 0;
	} else {
		return -EINVAL;
	}
	return 0;
}

/*
 * A color_space other than RESERVED6 or a color_depth other than 0 asks for an
 * output format; those values withdraw the request. A color_depth write, even
 * of the same value, and UPDATE force a modeset in a commit that allows one.
 */
static int s7d_vendor_conn_set(struct drm_connector *connector,
			       struct drm_connector_state *state,
			       struct drm_property *property, uint64_t val)
{
	const struct s7d_vendor_conn_props *p = &s7d_vendor_conn_props(connector)->conn;
	struct s7d_vendor_conn_values *v;

	if (!s7d_vendor_props_conn(state))
		return -EINVAL;
	v = &container_of(state, struct s7d_vendor_conn_state, base)->v;
	if (property == p->color_space) {
		v->request = val != HDMI_COLORSPACE_RESERVED6;
		if (v->request)
			v->color_space = val;
	} else if (property == p->color_depth) {
		v->depth_written = true;
		v->request = val;
		if (val)
			v->color_depth = val;
	} else if (property == p->update) {
		v->update = true;
	} else if (property == p->avmute) {
		v->avmute = val;
	} else if (property == p->allm) {
		v->allm = val;
	} else if (property == p->hdr_priority) {
		v->hdr_priority = val;
	} else if (property == p->scan_info) {
		v->scan_info = val;
	} else if (property != p->ready && property != p->frac_rate_policy) {
		/* The vendor driver takes and drops these two and refuses the others. */
		return -EINVAL;
	}
	return 0;
}

/* The connectors are cleaned up later, with their own functions. */
static void s7d_vendor_release(struct drm_device *drm, void *data)
{
	struct s7d_vendor_props *vp = data;

	if (vp->conn_base)
		vp->hdmi->funcs = vp->conn_base;
	if (vp->wb_base)
		vp->writeback->funcs = vp->wb_base;
	drm_property_blob_put(vp->static_meta);
	vp->static_meta = NULL;
}

/* The 28 connector properties, in the vendor attach order, after vrr_capable. */
static int s7d_vendor_conn_attach(struct s7d_vendor_props *vp, struct drm_connector *connector)
{
	struct s7d_vendor_builder b = { .drm = vp->drm, .obj = &connector->base };
	struct s7d_vendor_conn_props *p = &vp->conn;
	unsigned int i;
	int ret;

	ret = drm_connector_attach_vrr_capable_property(connector);
	if (ret)
		return ret;
	p->color_depth = s7d_vendor_add_range(&b, 0, "color_depth", 0, 16, 8);
	p->color_space = s7d_vendor_add_enum(&b, "color_space", s7d_vendor_color_spaces,
					     ARRAY_SIZE(s7d_vendor_color_spaces),
					     HDMI_COLORSPACE_RGB);
	p->hdr_status = s7d_vendor_add_enum(&b, "hdmi_hdr_status", s7d_vendor_hdr_statuses,
					    ARRAY_SIZE(s7d_vendor_hdr_statuses), 0);
	p->static_meta = s7d_vendor_add(&b, drm_property_create(vp->drm, DRM_MODE_PROP_BLOB,
								 "HDR_STATIC_META", 0), 0);
	p->hdr_priority = s7d_vendor_add_range(&b, 0, "hdr_priority", 0, U32_MAX, hdr_priority);
	for (i = 0; i < ARRAY_SIZE(s7d_vendor_conn_ranges); i++)
		*(struct drm_property **)((u8 *)p + s7d_vendor_conn_ranges[i].offset) =
			s7d_vendor_add_range(&b, 0, s7d_vendor_conn_ranges[i].name, 0,
					     s7d_vendor_conn_ranges[i].max, 0);
	p->scan_info = s7d_vendor_add_enum(&b, "scan_info", s7d_vendor_scan_infos,
					   ARRAY_SIZE(s7d_vendor_scan_infos),
					   HDMI_SCAN_MODE_UNDERSCAN);
	p->connector_type = s7d_vendor_add_range(&b, DRM_MODE_PROP_IMMUTABLE,
						 "meson.connector_type", 0, INT_MAX,
						 S7D_VENDOR_CONNECTOR_HDMIA);
	if (b.failed)
		return -ENOMEM;

	/* The vendor values live in a connector state of its own. */
	vp->conn_base = connector->funcs;
	vp->conn_funcs = *connector->funcs;
	vp->conn_funcs.reset = s7d_vendor_conn_reset;
	vp->conn_funcs.atomic_duplicate_state = s7d_vendor_conn_duplicate;
	vp->conn_funcs.atomic_destroy_state = s7d_vendor_conn_destroy;
	vp->conn_funcs.atomic_get_property = s7d_vendor_conn_get;
	vp->conn_funcs.atomic_set_property = s7d_vendor_conn_set;
	connector->funcs = &vp->conn_funcs;
	s7d_vendor_conn_reset(connector);
	return connector->state ? 0 : -ENOMEM;
}

static struct s7d_vendor_props *s7d_vendor_wb_props(const struct drm_connector *connector)
{
	return container_of(connector->funcs, struct s7d_vendor_props, wb_funcs);
}

/* VDIN1 takes the VPP output, video and OSD blended: the vendor default. */
static int s7d_vendor_wb_get(struct drm_connector *connector,
			     const struct drm_connector_state *state,
			     struct drm_property *property, uint64_t *val)
{
	if (property != s7d_vendor_wb_props(connector)->capture_port)
		return -EINVAL;
	*val = S7D_VENDOR_CAPTURE_VPP;
	return 0;
}

static int s7d_vendor_wb_set(struct drm_connector *connector, struct drm_connector_state *state,
			     struct drm_property *property, uint64_t val)
{
	if (property != s7d_vendor_wb_props(connector)->capture_port ||
	    val != S7D_VENDOR_CAPTURE_VPP)
		return -EINVAL;
	return 0;
}

static int s7d_vendor_wb_attach(struct s7d_vendor_props *vp, struct drm_connector *connector)
{
	struct s7d_vendor_builder b = { .drm = vp->drm, .obj = &connector->base };

	vp->capture_port = s7d_vendor_add_enum(&b, "CAPTURE_PORT", s7d_vendor_capture_ports,
					       ARRAY_SIZE(s7d_vendor_capture_ports),
					       S7D_VENDOR_CAPTURE_VPP);
	if (b.failed)
		return -ENOMEM;
	vp->writeback = connector;
	vp->wb_base = connector->funcs;
	vp->wb_funcs = *connector->funcs;
	vp->wb_funcs.atomic_get_property = s7d_vendor_wb_get;
	vp->wb_funcs.atomic_set_property = s7d_vendor_wb_set;
	connector->funcs = &vp->wb_funcs;
	return 0;
}

int s7d_vendor_props_attach(struct drm_device *drm, const struct s7d_vendor_objs *objs)
{
	struct s7d_vendor_props *vp;
	struct clk *clk;
	int ret;

	if (!objs->crtc || !s7d_crtc_is_native(objs->crtc) || !objs->hdmi)
		return -EINVAL;
	/* The planes took their vendor profile only in vendor mode. */
	if (!s7d_vendor_props_active(drm))
		return 0;
	vp = drmm_kzalloc(drm, sizeof(*vp), GFP_KERNEL);
	if (!vp)
		return -ENOMEM;
	vp->drm = drm;
	vp->hdmi = objs->hdmi;
	/* The CRTC holds the VPU clock; this reference only reads its rate. */
	clk = devm_clk_get_optional(drm->dev, "vpu");
	vp->vpu_clk = IS_ERR(clk) ? NULL : clk;
	ret = drmm_add_action_or_reset(drm, s7d_vendor_release, vp);
	if (!ret)
		ret = s7d_vendor_crtc_attach(vp, objs->crtc);
	if (!ret)
		ret = s7d_vendor_planes_attach(vp, objs);
	if (!ret)
		ret = s7d_vendor_conn_attach(vp, objs->hdmi);
	/* The writeback connector exists only with the VDIN1 resets. */
	if (!ret && objs->writeback)
		ret = s7d_vendor_wb_attach(vp, &objs->writeback->base);
	return ret;
}
