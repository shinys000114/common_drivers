// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/hdmi.h>
#include <linux/kernel.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_property.h>

#include "s7d-crtc.h"
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

struct s7d_vendor_props {
	struct drm_device *drm;
	struct drm_connector *hdmi;
	/* Optional: vpu_clk reads 0 without it. */
	struct clk *vpu_clk;
	struct s7d_vendor_crtc_props crtc;
};

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

/* Creates properties and attaches each to one object; any failure fails the whole. */
struct s7d_vendor_builder {
	struct drm_device *drm;
	struct drm_mode_object *obj;
	bool failed;
};

static struct drm_property *s7d_vendor_add(struct s7d_vendor_builder *b,
					   struct drm_property *prop, u64 value)
{
	if (prop)
		drm_object_attach_property(b->obj, prop, value);
	else
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

int s7d_vendor_props_attach(struct drm_device *drm, const struct s7d_vendor_objs *objs)
{
	struct s7d_vendor_props *vp;
	struct clk *clk;

	if (!objs->crtc || !s7d_crtc_is_native(objs->crtc))
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
	return s7d_vendor_crtc_attach(vp, objs->crtc);
}
