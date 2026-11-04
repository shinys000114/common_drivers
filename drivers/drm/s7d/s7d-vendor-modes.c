// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bug.h>
#include <linux/hdmi.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>

#include <uapi/amlogic/drm/meson_drm.h>

#include "s7d-caps.h"
#include "s7d-encp.h"
#include "s7d-hdmi.h"
#include "s7d-hdmi-output.h"
#include "s7d-vendor.h"

/*
 * The vendor HDMI timing table (hdmitx_mode.c) by CEA VIC: the short name
 * where it has one, else the full one, and the clock in kHz of the timing
 * it names, counting repeated pixels.
 */
static const struct {
	u32 clock;
	const char *name;
} s7d_vendor_cea_names[] = {
	[1] = { 25175, "640x480p60hz" },
	[2] = { 27027, "480p60hz" },
	[3] = { 27027, "480p60hz" },
	[4] = { 74250, "720p60hz" },
	[5] = { 74250, "1080i60hz" },
	[6] = { 27027, "480i60hz" },
	[7] = { 27027, "480i60hz" },
	[8] = { 27000, "720x240p60hz" },
	[9] = { 27000, "720x240p60hz" },
	[10] = { 54000, "2880x480i60hz" },
	[11] = { 54000, "2880x480i60hz" },
	[12] = { 54000, "2880x240p60hz" },
	[13] = { 54000, "2880x240p60hz" },
	[14] = { 54000, "1440x480p60hz" },
	[15] = { 54000, "1440x480p60hz" },
	[16] = { 148500, "1080p60hz" },
	[17] = { 27000, "576p50hz" },
	[18] = { 27000, "576p50hz" },
	[19] = { 74250, "720p50hz" },
	[20] = { 74250, "1080i50hz" },
	[21] = { 27000, "576i50hz" },
	[22] = { 27000, "576i50hz" },
	[23] = { 27000, "720x288p50hz" },
	[24] = { 27000, "720x288p50hz" },
	[25] = { 54000, "2880x576i50hz" },
	[26] = { 54000, "2880x576i50hz" },
	[27] = { 54000, "2880x288p50hz" },
	[28] = { 54000, "2880x288p50hz" },
	[29] = { 54000, "1440x576p50hz" },
	[30] = { 54000, "1440x576p50hz" },
	[31] = { 148500, "1080p50hz" },
	[32] = { 74250, "1080p24hz" },
	[33] = { 74250, "1080p25hz" },
	[34] = { 74250, "1080p30hz" },
	[35] = { 108000, "2880x480p60hz" },
	[36] = { 108000, "2880x480p60hz" },
	[37] = { 108000, "2880x576p50hz" },
	[38] = { 108000, "2880x576p50hz" },
	[39] = { 72000, "1920x1080i50hz" },
	[40] = { 148500, "1920x1080i100hz" },
	[41] = { 148500, "1280x720p100hz" },
	[42] = { 54000, "720x576p100hz" },
	[43] = { 54000, "720x576p100hz" },
	[44] = { 54000, "720x576i100hz" },
	[45] = { 54000, "720x576i100hz" },
	[46] = { 148500, "1920x1080i120hz" },
	[47] = { 148500, "1280x720p120hz" },
	[48] = { 54000, "720x480p120hz" },
	[49] = { 54000, "720x480p120hz" },
	[50] = { 54000, "720x480i120hz" },
	[51] = { 54000, "720x480i120hz" },
	[52] = { 108000, "720x576p200hz" },
	[53] = { 108000, "720x576p200hz" },
	[54] = { 108000, "720x576i200hz" },
	[55] = { 108000, "720x576i200hz" },
	[56] = { 108000, "720x480p240hz" },
	[57] = { 108000, "720x480p240hz" },
	[58] = { 108000, "720x480i240hz" },
	[59] = { 108000, "720x480i240hz" },
	[60] = { 59400, "720p24hz" },
	[61] = { 74250, "720p25hz" },
	[62] = { 74250, "720p30hz" },
	[63] = { 297000, "1920x1080p120hz" },
	[64] = { 297000, "1920x1080p100hz" },
	[65] = { 59400, "1280x720p24hz" },
	[66] = { 74250, "1280x720p25hz" },
	[67] = { 74250, "1280x720p30hz" },
	[68] = { 74250, "1280x720p50hz" },
	[69] = { 74250, "1280x720p60hz" },
	[70] = { 148500, "1280x720p100hz" },
	[71] = { 148500, "1280x720p120hz" },
	[72] = { 74250, "1920x1080p24hz" },
	[73] = { 74250, "1920x1080p25hz" },
	[74] = { 74250, "1920x1080p30hz" },
	[75] = { 148500, "1920x1080p50hz" },
	[76] = { 148500, "1920x1080p60hz" },
	[77] = { 297000, "1920x1080p100hz" },
	[78] = { 297000, "1920x1080p120hz" },
	[79] = { 59400, "1680x720p24hz" },
	[80] = { 59400, "1680x720p25hz" },
	[81] = { 59400, "1680x720p30hz" },
	[82] = { 82500, "1680x720p50hz" },
	[83] = { 99000, "1680x720p60hz" },
	[84] = { 165000, "1680x720p100hz" },
	[85] = { 198000, "1680x720p120hz" },
	[86] = { 99000, "2560x1080p24hz" },
	[87] = { 90000, "2560x1080p25hz" },
	[88] = { 118800, "2560x1080p30hz" },
	[89] = { 185625, "2560x1080p50hz" },
	[90] = { 198000, "2560x1080p60hz" },
	[91] = { 371250, "2560x1080p100hz" },
	[92] = { 495000, "2560x1080p120hz" },
	[93] = { 297000, "2160p24hz" },
	[94] = { 297000, "2160p25hz" },
	[95] = { 297000, "2160p30hz" },
	[96] = { 594000, "2160p50hz" },
	[97] = { 594000, "2160p60hz" },
	[98] = { 297000, "smpte24hz" },
	[99] = { 297000, "smpte25hz" },
	[100] = { 297000, "smpte30hz" },
	[101] = { 594000, "smpte50hz" },
	[102] = { 594000, "smpte60hz" },
	[103] = { 297000, "3840x2160p24hz" },
	[104] = { 297000, "3840x2160p25hz" },
	[105] = { 297000, "3840x2160p30hz" },
	[106] = { 594000, "3840x2160p50hz" },
	[107] = { 594000, "3840x2160p60hz" },
	[108] = { 90000, "720p48hz" },
	[109] = { 90000, "1280x720p48hz" },
	[110] = { 99000, "1680x720p48hz" },
	[111] = { 148500, "1080p48hz" },
	[112] = { 148500, "1920x1080p48hz" },
	[113] = { 198000, "2560x1080p48hz" },
	[114] = { 594000, "2160p48hz" },
	[115] = { 594000, "smpte48hz" },
	[116] = { 594000, "3840x2160p48hz" },
	[117] = { 1188000, "3840x2160p100hz" },
	[118] = { 1188000, "3840x2160p120hz" },
	[119] = { 1188000, "3840x2160p100hz" },
	[120] = { 1188000, "3840x2160p120hz" },
	[121] = { 396000, "5120x2160p24hz" },
	[122] = { 396000, "5120x2160p25hz" },
	[123] = { 396000, "5120x2160p30hz" },
	[124] = { 742500, "5120x2160p48hz" },
	[125] = { 742500, "5120x2160p50hz" },
	[126] = { 742500, "5120x2160p60hz" },
	[127] = { 1485000, "5120x2160p100hz" },
	[193] = { 1485000, "5120x2160p120hz" },
	[194] = { 1188000, "7680x4320p24hz" },
	[195] = { 1188000, "7680x4320p25hz" },
	[196] = { 1188000, "7680x4320p30hz" },
	[197] = { 2376000, "7680x4320p48hz" },
	[198] = { 2376000, "7680x4320p50hz" },
	[199] = { 2376000, "7680x4320p60hz" },
	[200] = { 4752000, "7680x4320p100hz" },
	[201] = { 4752000, "7680x4320p120hz" },
	[202] = { 1188000, "7680x4320p24hz" },
	[203] = { 1188000, "7680x4320p25hz" },
	[204] = { 1188000, "7680x4320p30hz" },
	[205] = { 2376000, "7680x4320p48hz" },
	[206] = { 2376000, "7680x4320p50hz" },
	[207] = { 2376000, "7680x4320p60hz" },
	[208] = { 4752000, "7680x4320p100hz" },
	[209] = { 4752000, "7680x4320p120hz" },
	[210] = { 1485000, "10240x4320p24hz" },
	[211] = { 1485000, "10240x4320p25hz" },
	[212] = { 1485000, "10240x4320p30hz" },
	[213] = { 2970000, "10240x4320p48hz" },
	[214] = { 2970000, "10240x4320p50hz" },
	[215] = { 2970000, "10240x4320p60hz" },
	[216] = { 5940000, "10240x4320p100hz" },
	[217] = { 5940000, "10240x4320p120hz" },
	[218] = { 1188000, "smpte100hz" },
	[219] = { 1188000, "smpte120hz" },
};

/* The vendor VESA timings, which the vendor matches by size and refresh rate. */
static const struct {
	u16 hdisplay;
	u16 vdisplay;
	u16 refresh;
	const char *name;
} s7d_vendor_vesa_names[] = {
	{ 640, 480, 60, "640x480p60hzV" },
	{ 800, 480, 60, "800x480p60hzV" },
	{ 800, 600, 101, "800x600p60hzV" },
	{ 852, 480, 60, "852x480p60hzV" },
	{ 854, 480, 60, "854x480p60hzV" },
	{ 1024, 600, 60, "1024x600p60hzV" },
	{ 1024, 768, 73, "1024x768p60hzV" },
	{ 1152, 864, 75, "1152x864p75hzV" },
	{ 1280, 768, 60, "1280x768p60hzV" },
	{ 1280, 800, 70, "1280x800p60hzV" },
	{ 1280, 960, 60, "1280x960p60hzV" },
	{ 1280, 1024, 60, "1280x1024p60hzV" },
	{ 1360, 768, 60, "1360x768p60hzV" },
	{ 1366, 768, 60, "1366x768p60hzV" },
	{ 1400, 1050, 60, "1400x1050p60hzV" },
	{ 1440, 900, 60, "1440x900p60hzV" },
	{ 1440, 2560, 60, "1440x2560p60hzV" },
	{ 1600, 900, 60, "1600x900p60hzV" },
	{ 1600, 1200, 60, "1600x1200p60hzV" },
	{ 1680, 1050, 60, "1680x1050p60hzV" },
	{ 1920, 1200, 60, "1920x1200p60hzV" },
	{ 2048, 1080, 24, "2048x1080p24hzV" },
	{ 2160, 1200, 90, "2160x1200p90hzV" },
	{ 2560, 1600, 60, "2560x1600p60hzV" },
	{ 3440, 1440, 60, "3440x1440p60hzV" },
	{ 2400, 1200, 90, "2400x1200p90hzV" },
	{ 3840, 1080, 60, "3840x1080p60hzV" },
};

/* Whether @mode runs at 1000/1001 of @refresh, within 100 ppm. */
static bool s7d_vendor_mode_frac(const struct drm_display_mode *mode, int refresh)
{
	u64 num = (u64)mode->clock * 1000000, den = (u64)mode->htotal * mode->vtotal;
	u64 rate, frac;

	if (refresh <= 0 || refresh % 6 || !den)
		return false;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (mode->vscan > 1)
		den *= mode->vscan;
	rate = div64_u64(num + den / 2, den);
	frac = DIV_ROUND_CLOSEST_ULL((u64)refresh * 1000000, 1001);
	return abs_diff(rate, frac) * 10000 <= frac;
}

/*
 * Name @mode as the vendor driver names its timings; @name may be
 * @mode->name. A CEA mode takes the vendor name of its VIC. Its other clock,
 * the 1000/1001 one of a rate divisible by 6, is named one hertz lower, as
 * the vendor names the alternate modes it adds: 1080p59hz, smpte59hz. A VESA
 * size and rate of the vendor table takes its name, 1920x1200p60hzV. Any
 * other mode is WxHpRRhz, also one hertz lower at 1000/1001 of the rate.
 */
void s7d_vendor_mode_name(const struct drm_display_mode *mode, char *name)
{
	char scan = mode->flags & DRM_MODE_FLAG_INTERLACE ? 'i' : 'p';
	int refresh = drm_mode_vrefresh(mode);
	u8 vic = drm_match_cea_mode(mode);
	unsigned int i;
	char *end;
	u32 clock;

	if (vic < ARRAY_SIZE(s7d_vendor_cea_names) && s7d_vendor_cea_names[vic].name) {
		strscpy(name, s7d_vendor_cea_names[vic].name, DRM_DISPLAY_MODE_LEN);
		clock = s7d_vendor_cea_names[vic].clock;
		/* The vendor timing itself, within 500 ppm, or a VIC without a 1000/1001 rate. */
		if (abs_diff((u32)s7d_mode_clock(mode), clock) * 2000 <= clock || refresh % 6)
			return;
		if (mode->hdisplay == 4096 && mode->vdisplay == 2160) {
			snprintf(name, DRM_DISPLAY_MODE_LEN, "smpte%dhz", refresh - 1);
			return;
		}
		end = strrchr(name, 'p') ?: strrchr(name, 'i');
		if (end)
			snprintf(end + 1, DRM_DISPLAY_MODE_LEN - (end + 1 - name), "%dhz",
				 refresh - 1);
		return;
	}
	for (i = 0; scan == 'p' && i < ARRAY_SIZE(s7d_vendor_vesa_names); i++) {
		if (mode->hdisplay == s7d_vendor_vesa_names[i].hdisplay &&
		    mode->vdisplay == s7d_vendor_vesa_names[i].vdisplay &&
		    refresh == s7d_vendor_vesa_names[i].refresh) {
			strscpy(name, s7d_vendor_vesa_names[i].name, DRM_DISPLAY_MODE_LEN);
			return;
		}
	}
	snprintf(name, DRM_DISPLAY_MODE_LEN, "%ux%u%c%dhz", mode->hdisplay, mode->vdisplay,
		 scan, s7d_vendor_mode_frac(mode, refresh) ? refresh - 1 : refresh);
}

/* Vendor userspace finds modes by their vendor names; the timings stay. */
static int s7d_vendor_get_modes(struct drm_connector *connector)
{
	struct s7d_vendor *v = s7d_vendor_find(connector->dev);
	struct drm_display_mode *mode;
	int count;

	if (WARN_ON(!v))
		return 0;
	count = v->get_modes(connector);
	list_for_each_entry(mode, &connector->probed_modes, head)
		s7d_vendor_mode_name(mode, mode->name);
	return count;
}

void s7d_vendor_modes_init(struct s7d_vendor *v)
{
	struct drm_connector *connector = v->hdmi;

	v->helpers = *connector->helper_private;
	v->get_modes = v->helpers.get_modes;
	if (!v->get_modes)
		return;
	v->helpers.get_modes = s7d_vendor_get_modes;
	drm_connector_helper_add(connector, &v->helpers);
}

/*
 * Parse a vendor attribute string such as "444,10bit" in the vendor order:
 * the first depth and the first colour space found, 8-bit 4:4:4 without
 * them. The quantization range is ignored, as by the vendor mode test.
 */
void s7d_vendor_parse_attr(const char *text, struct s7d_vendor_attr *attr)
{
	static const struct {
		const char *name;
		unsigned int bpc;
	} depths[] = {
		{ "8bit", 8 }, { "10bit", 10 }, { "12bit", 12 }, { "16bit", 16 },
	};
	static const struct {
		const char *name;
		enum hdmi_colorspace colorspace;
	} spaces[] = {
		{ "rgb", HDMI_COLORSPACE_RGB },
		{ "422", HDMI_COLORSPACE_YUV422 },
		{ "444", HDMI_COLORSPACE_YUV444 },
		{ "420", HDMI_COLORSPACE_YUV420 },
	};
	unsigned int i;

	attr->bpc = 8;
	for (i = 0; i < ARRAY_SIZE(depths); i++) {
		if (strstr(text, depths[i].name)) {
			attr->bpc = depths[i].bpc;
			break;
		}
	}
	attr->colorspace = HDMI_COLORSPACE_YUV444;
	for (i = 0; i < ARRAY_SIZE(spaces); i++) {
		if (strstr(text, spaces[i].name)) {
			attr->colorspace = spaces[i].colorspace;
			break;
		}
	}
}

/*
 * Whether the output can send @mode with @attr: the sink takes the colour
 * format and depth, and the transmitter and the sink carry the link. As
 * the vendor transmitter does, refuse 640x480p60 in any format and 4:2:2
 * at any depth but 12 bits; 4:2:2 has no pixel repetition either. Only
 * sink information is read.
 */
bool s7d_vendor_attr_valid(const struct drm_display_info *info,
			   const struct drm_display_mode *mode,
			   const struct s7d_vendor_attr *attr)
{
	static const enum s7d_hdmi_output_encoding rgb[] = {
		S7D_HDMI_OUTPUT_RGB8, S7D_HDMI_OUTPUT_RGB10, S7D_HDMI_OUTPUT_RGB12,
	};
	static const enum s7d_hdmi_output_encoding y444[2][3] = {
		{ S7D_HDMI_OUTPUT_601_Y4448, S7D_HDMI_OUTPUT_601_Y44410,
		  S7D_HDMI_OUTPUT_601_Y44412 },
		{ S7D_HDMI_OUTPUT_709_Y4448, S7D_HDMI_OUTPUT_709_Y44410,
		  S7D_HDMI_OUTPUT_709_Y44412 },
	};
	static const enum s7d_hdmi_output_encoding y420[2][3] = {
		{ S7D_HDMI_OUTPUT_601_Y4208, S7D_HDMI_OUTPUT_601_Y42010,
		  S7D_HDMI_OUTPUT_601_Y42012 },
		{ S7D_HDMI_OUTPUT_709_Y4208, S7D_HDMI_OUTPUT_709_Y42010,
		  S7D_HDMI_OUTPUT_709_Y42012 },
	};
	static const u8 hdmi_dc[] = { 0, DRM_EDID_HDMI_DC_30, DRM_EDID_HDMI_DC_36 };
	static const u8 y420_dc[] = { 0, DRM_EDID_YCBCR420_DC_30, DRM_EDID_YCBCR420_DC_36 };
	unsigned int depth, family = mode->vdisplay >= 720;
	enum s7d_hdmi_output_encoding encoding;
	struct s7d_encp_state encp;

	if (attr->bpc != 8 && attr->bpc != 10 && attr->bpc != 12)
		return false;
	depth = (attr->bpc - 8) / 2;
	/* A DVI sink takes 8-bit RGB only. */
	if (!info->is_hdmi && (attr->colorspace != HDMI_COLORSPACE_RGB || depth))
		return false;
	if (attr->colorspace != HDMI_COLORSPACE_YUV420 && drm_mode_is_420_only(info, mode))
		return false;
	if (drm_match_cea_mode(mode) == 1)
		return false;
	switch (attr->colorspace) {
	case HDMI_COLORSPACE_RGB:
		if (depth && !(info->edid_hdmi_rgb444_dc_modes & hdmi_dc[depth]))
			return false;
		encoding = rgb[depth];
		break;
	case HDMI_COLORSPACE_YUV444:
		if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR444) ||
		    (depth && !(info->edid_hdmi_ycbcr444_dc_modes & hdmi_dc[depth])))
			return false;
		encoding = y444[family][depth];
		break;
	case HDMI_COLORSPACE_YUV422:
		/* 4:2:2 always carries 12 bits, and the vendor offers no other depth. */
		if (attr->bpc != 12 || !(info->color_formats & DRM_COLOR_FORMAT_YCBCR422) ||
		    (mode->flags & DRM_MODE_FLAG_DBLCLK))
			return false;
		encoding = family ? S7D_HDMI_OUTPUT_709_Y4228 : S7D_HDMI_OUTPUT_601_Y4228;
		break;
	case HDMI_COLORSPACE_YUV420:
		if (!drm_mode_is_420(info, mode) ||
		    (depth && !(info->hdmi.y420_dc_modes & y420_dc[depth])))
			return false;
		encoding = y420[family][depth];
		break;
	default:
		return false;
	}
	return s7d_hdmi_link_valid(info, encoding, mode) == MODE_OK &&
		s7d_encp_build_state(mode, &encp) == MODE_OK;
}

/*
 * Whether the HDMI connector can send its probed mode @modename with
 * @attr; a name it has not probed is not valid. -EINVAL only for an
 * unknown connector, as in the vendor driver; nothing is read from the
 * hardware.
 */
int s7d_vendor_testattr_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_mode_test_attr *test = data;
	char name[sizeof(test->modename)], text[sizeof(test->attr)];
	struct drm_connector *connector;
	struct drm_display_mode *mode;
	struct s7d_vendor_attr attr;
	bool valid = false;

	connector = drm_connector_lookup(drm, file, test->conn_id);
	if (!connector)
		return -EINVAL;
	if (connector->connector_type != DRM_MODE_CONNECTOR_HDMIA) {
		drm_connector_put(connector);
		return -EINVAL;
	}
	strscpy(name, test->modename, sizeof(name));
	strscpy(text, test->attr, sizeof(text));
	s7d_vendor_parse_attr(text, &attr);
	mutex_lock(&drm->mode_config.mutex);
	list_for_each_entry(mode, &connector->modes, head) {
		if (!strcmp(mode->name, name)) {
			valid = s7d_vendor_attr_valid(&connector->display_info, mode, &attr);
			break;
		}
	}
	mutex_unlock(&drm->mode_config.mutex);
	drm_connector_put(connector);
	test->valid = valid;
	return 0;
}
