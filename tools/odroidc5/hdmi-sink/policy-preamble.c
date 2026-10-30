// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* Userspace API scaffolding only; hardware/DRM full-stack behavior is not modeled. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/bits.h>
#include <linux/media-bus-format.h>
#include <uapi/drm/drm_mode.h>
#include "s7d-hdmi-output.h"
#include "s7d-encp.h"
#include "s7d-caps.h"
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define DRM_COLOR_FORMAT_RGB444 (1 << 0)
#define DRM_COLOR_FORMAT_YCBCR444 (1 << 1)
#define DRM_COLOR_FORMAT_YCBCR422 (1 << 2)
#define DRM_COLOR_FORMAT_YCBCR420 (1 << 3)
#define DRM_EDID_HDMI_DC_48 (1 << 6)
#define DRM_EDID_HDMI_DC_36 (1 << 5)
#define DRM_EDID_HDMI_DC_30 (1 << 4)
#define DRM_EDID_HDMI_DC_Y444 (1 << 3)
#define DRM_EDID_MAX_FRL_RATE_MASK 0xf0
#define DRM_EDID_YCBCR420_DC_MASK 7
#define DRM_EDID_YCBCR420_DC_36 (1 << 1)
#define DRM_EDID_YCBCR420_DC_30 (1 << 0)
#define DRM_MODE_COLORIMETRY_DEFAULT 0
#define DRM_MODE_COLORIMETRY_SMPTE_170M_YCC 1
#define DRM_MODE_COLORIMETRY_BT709_YCC 2
#define DRM_MODE_COLORIMETRY_BT2020_RGB 9
#define DRM_MODE_COLORIMETRY_BT2020_YCC 10
#define DRM_HDMI_BROADCAST_RGB_AUTO 0
#define DRM_HDMI_BROADCAST_RGB_FULL 1
#define DRM_HDMI_BROADCAST_RGB_LIMITED 2
#define HDMI_EOTF_TRADITIONAL_GAMMA_SDR 0
#define HDMI_EOTF_TRADITIONAL_GAMMA_HDR 1
#define HDMI_EOTF_SMPTE_ST2084 2
#define HDMI_EOTF_BT_2100_HLG 3
#define HDMI_STATIC_METADATA_TYPE1 0
#define HDMI_INFOFRAME_HEADER_SIZE 4
#define HDMI_AVI_INFOFRAME_SIZE 13
#define HDMI_DRM_INFOFRAME_SIZE 26
#define HDMI_INFOFRAME_SIZE(t) 31
#define HDMI_COLORSPACE_RGB 0
#define HDMI_COLORSPACE_YUV422 1
#define HDMI_COLORSPACE_YUV444 2
#define HDMI_COLORSPACE_YUV420 3
#define HDMI_COLORIMETRY_ITU_601 1
#define HDMI_COLORIMETRY_ITU_709 2
#define HDMI_COLORIMETRY_EXTENDED 3
#define HDMI_EXTENDED_COLORIMETRY_BT2020 6
#define HDMI_YCC_QUANTIZATION_RANGE_LIMITED 0
#define drm_dbg_kms(...) ((void)0)
struct drm_scdc {
	bool supported, read_request;
	struct {
		bool supported, low_rates;
	} scrambling;
};
struct drm_hdmi_dsc_cap {
	int unused;
};
struct drm_hdmi_info {
	struct drm_scdc scdc;
	struct drm_hdmi_dsc_cap dsc_cap;
	u8 max_lanes, max_frl_rate_per_lane, y420_dc_modes;
	unsigned long y420_vdb_modes[4];
	unsigned long y420_cmdb_modes[4];
};
struct drm_display_info {
	bool is_hdmi, has_hdmi_infoframe, dvi_dual, rgb_quant_range_selectable;
	int max_tmds_clock, bpc;
	unsigned color_formats;
	u8 edid_hdmi_rgb444_dc_modes, edid_hdmi_ycbcr444_dc_modes;
	u16 source_physical_address;
	struct drm_hdmi_info hdmi;
};
struct drm_connector {
	struct drm_display_info display_info;
	struct {
		struct {
			u8 eotf, metadata_type, max_cll, max_fall, min_cll;
		} hdmi_type1;
	} hdr_sink_metadata;
};
struct drm_property_blob {
	void *data;
	size_t length;
};
struct hdr_metadata_infoframe {
	u8 eotf, metadata_type;
};
struct hdr_output_metadata {
	u32 metadata_type;
	union {
		struct hdr_metadata_infoframe hdmi_metadata_type1;
	};
};
struct drm_connector_state {
	struct drm_connector *connector;
	unsigned colorspace, max_bpc, max_requested_bpc, content_type;
	struct {
		unsigned broadcast_rgb;
	} hdmi;
	struct drm_property_blob *hdr_output_metadata;
};
struct drm_bridge {
	int unused;
};
struct drm_bridge_state {
	struct {
		u32 format;
	} input_bus_cfg, output_bus_cfg;
};
struct drm_crtc {
	int unused;
};
struct drm_atomic_state {
	int unused;
};
struct drm_crtc_state {
	bool active, mode_changed;
	struct drm_display_mode adjusted_mode;
	struct drm_property_blob *ctm;
	struct drm_property_blob *gamma_lut;
	struct drm_atomic_state *state;
	struct drm_crtc *crtc;
};
struct s7d_crtc_state {
	struct drm_crtc_state base;
	struct s7d_hdmi_output_state output;
};
static struct s7d_crtc_state *to_s7d_crtc_state(struct drm_crtc_state *s)
{
	return container_of(s, struct s7d_crtc_state, base);
}
static struct drm_crtc_state *
drm_atomic_get_old_crtc_state(struct drm_atomic_state *a, struct drm_crtc *c)
{
	(void)a;
	(void)c;
	return NULL;
}
static struct drm_bridge_state *
drm_atomic_get_old_bridge_state(struct drm_atomic_state *a,
				struct drm_bridge *b)
{
	(void)a;
	(void)b;
	return NULL;
}
/* VIC matching is supplied by the fixture, not tested by this host harness. */
static u8 drm_match_cea_mode(const struct drm_display_mode *m)
{
	return m->fixture_vic;
}
static bool test_bit(unsigned n, const unsigned long *p)
{
	return !!(p[n / 64] & (1UL << (n % 64)));
}
static void bitmap_set(unsigned long *p, unsigned n, unsigned count)
{
	assert(count == 1 && n < 256);
	p[n / 64] |= 1UL << (n % 64);
}
static bool drm_valid_cea_vic(u8 vic)
{
	return vic > 0 && vic <= 127;
}
static u8 svd_to_vic(u8 svd)
{
	return svd <= 192 ? svd & 127 : svd;
}
struct cea_db {
	u8 data[32];
};
#define cea_db_payload_len(p) (((const u8 *)(p))[0] & 31)
static const u8 *cea_db_data(const struct cea_db *p)
{
	return (const u8 *)p + 1;
}
static void drm_parse_dsc_info(struct drm_hdmi_dsc_cap *cap, const u8 *p)
{
	(void)cap;
	(void)p;
	assert(!"DSC is outside the fixture scope");
}
static void drm_calculate_luminance_range(struct drm_connector *c)
{
	(void)c;
	assert(!"Luminance values are outside the fixture scope");
}
enum hdmi_quantization_range {
	HDMI_QUANTIZATION_RANGE_DEFAULT,
	HDMI_QUANTIZATION_RANGE_LIMITED,
	HDMI_QUANTIZATION_RANGE_FULL
};
struct hdmi_avi_infoframe {
	unsigned colorspace, colorimetry, extended_colorimetry, quantization_range,
		ycc_quantization_range;
};
/* The last packed AVI fields, for policy assertions; bytes stay inert. */
static struct hdmi_avi_infoframe last_avi;
struct hdmi_vendor_infoframe {
	int unused;
};
/* Packet packing is external to this policy test; these are explicitly inert. */
static int
drm_hdmi_avi_infoframe_from_display_mode(struct hdmi_avi_infoframe *a,
					 struct drm_connector *c,
					 const struct drm_display_mode *m)
{
	(void)c;
	(void)m;
	memset(a, 0, sizeof(*a));
	return 0;
}
static void drm_hdmi_avi_infoframe_quant_range(struct hdmi_avi_infoframe *a,
					       struct drm_connector *c,
					       const struct drm_display_mode *m,
					       unsigned q)
{
	(void)a;
	(void)c;
	(void)m;
	(void)q;
}
/* Only the BT.2020 mapping is modelled; other requests leave the frame unchanged. */
static void
drm_hdmi_avi_infoframe_colorimetry(struct hdmi_avi_infoframe *a,
				   const struct drm_connector_state *s)
{
	if (s->colorspace == DRM_MODE_COLORIMETRY_BT2020_RGB ||
	    s->colorspace == DRM_MODE_COLORIMETRY_BT2020_YCC) {
		a->colorimetry = HDMI_COLORIMETRY_EXTENDED;
		a->extended_colorimetry = HDMI_EXTENDED_COLORIMETRY_BT2020;
	}
}
static void
drm_hdmi_avi_infoframe_content_type(struct hdmi_avi_infoframe *a,
				    const struct drm_connector_state *s)
{
	(void)a;
	(void)s;
}
static int hdmi_avi_infoframe_pack(struct hdmi_avi_infoframe *a, u8 *b,
				   size_t n)
{
	last_avi = *a;
	memset(b, 0, n);
	return (int)n;
}
static int
drm_hdmi_vendor_infoframe_from_display_mode(struct hdmi_vendor_infoframe *v,
					    struct drm_connector *c,
					    const struct drm_display_mode *m)
{
	(void)v;
	(void)c;
	(void)m;
	return 0;
}
static int hdmi_vendor_infoframe_pack(struct hdmi_vendor_infoframe *v, u8 *b,
				      size_t n)
{
	(void)v;
	memset(b, 0, n);
	return (int)n;
}
struct hdmi_drm_infoframe {
	unsigned eotf;
};
/* The last packed DRM InfoFrame EOTF; payload packing is the kernel helper's. */
static struct hdmi_drm_infoframe last_hdr;
static int drm_hdmi_infoframe_set_hdr_metadata(struct hdmi_drm_infoframe *f,
					       const struct drm_connector_state *s)
{
	const struct hdr_output_metadata *m = s->hdr_output_metadata->data;

	f->eotf = m->hdmi_metadata_type1.eotf;
	return 0;
}
static int hdmi_drm_infoframe_pack(struct hdmi_drm_infoframe *f, u8 *b, size_t n)
{
	last_hdr = *f;
	memset(b, 0, n);
	return (int)n;
}
