// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-fence.h>
#include <linux/hdmi.h>
#include <linux/kernel.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

#include <drm/drm_blend.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_plane.h>
#include <drm/drm_property.h>

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

struct s7d_vendor_props {
	struct drm_device *drm;
	struct drm_connector *hdmi;
	/* Optional: vpu_clk reads 0 without it. */
	struct clk *vpu_clk;
	struct s7d_vendor_crtc_props crtc;
	struct s7d_vendor_osd_props osd;
	struct s7d_vendor_video_props video;
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

int s7d_vendor_props_attach(struct drm_device *drm, const struct s7d_vendor_objs *objs)
{
	struct s7d_vendor_props *vp;
	struct clk *clk;
	int ret;

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
	ret = s7d_vendor_crtc_attach(vp, objs->crtc);
	if (ret)
		return ret;
	return s7d_vendor_planes_attach(vp, objs);
}
