// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/*
 * ENCP timing for the S7D DRM atomic state. Progressive timing derives from
 * Amlogic's config_tv_enc_calc (2019); 1080i50/60 uses the vendor's fixed
 * tvregs_1080i tables and hdmi_tvenc1080i_set DE/sync placement. S7D does
 * not implement VENC_DVI_SETTING, VENC_VIDEO_PROG_MODE, the YFP timings or
 * SYNC_MODE bit 9 from those tables; they read back as zero.
 * No MMIO, clock or power operation belongs in this calculation.
 */
#include <linux/kernel.h>
#include <linux/string.h>

#include "s7d-encp.h"
#include "s7d-caps.h"

/*
 * Use the S7D Linux/U-Boot VCBUS indices. Datasheet 01's ENCP HAVON and
 * VAVON table offsets differ from those headers and the running C5.
 */
#define ENCP_VIDEO_SYNC_MODE	0x1b81
#define ENCP_VIDEO_HSPULS_BEGIN	0x1b98
#define ENCP_VIDEO_HSPULS_END	0x1b99
#define ENCP_VIDEO_HSPULS_SWITCH 0x1b9a
#define ENCP_VIDEO_VSPULS_BEGIN	0x1b9b
#define ENCP_VIDEO_VSPULS_END	0x1b9c
#define ENCP_VIDEO_VSPULS_BLINE	0x1b9d
#define ENCP_VIDEO_VSPULS_ELINE	0x1b9e
#define ENCP_VIDEO_EQPULS_BEGIN	0x1b9f
#define ENCP_VIDEO_EQPULS_END	0x1ba0
#define ENCP_VIDEO_EQPULS_BLINE	0x1ba1
#define ENCP_VIDEO_EQPULS_ELINE	0x1ba2
#define ENCP_VIDEO_OFLD_VOAV_OFST 0x1bba
#define ENCP_VIDEO_MODE		0x1b8d
#define ENCP_VIDEO_MODE_ADV	0x1b8e
#define ENCP_VIDEO_MAX_PXCNT	0x1b97
#define ENCP_VIDEO_HAVON_END	0x1ba3
#define ENCP_VIDEO_HAVON_BEGIN	0x1ba4
#define ENCP_VIDEO_VAVON_BLINE	0x1ba6
#define ENCP_VIDEO_HSO_BEGIN	0x1ba7
#define ENCP_VIDEO_HSO_END	0x1ba8
#define ENCP_VIDEO_VSO_BEGIN	0x1ba9
#define ENCP_VIDEO_VSO_END	0x1baa
#define ENCP_VIDEO_VSO_BLINE	0x1bab
#define ENCP_VIDEO_VSO_ELINE	0x1bac
#define ENCP_VIDEO_MAX_LNCNT	0x1bae
#define ENCP_VIDEO_VAVON_ELINE	0x1baf
#define ENCP_DVI_HSO_BEGIN	0x1c30
#define ENCP_DVI_HSO_END		0x1c31
#define ENCP_DVI_VSO_BLINE_EVN	0x1c32
#define ENCP_DVI_VSO_BLINE_ODD	0x1c33
#define ENCP_DVI_VSO_ELINE_EVN	0x1c34
#define ENCP_DVI_VSO_ELINE_ODD	0x1c35
#define ENCP_DVI_VSO_BEGIN_EVN	0x1c36
#define ENCP_DVI_VSO_BEGIN_ODD	0x1c37
#define ENCP_DVI_VSO_END_EVN	0x1c38
#define ENCP_DVI_VSO_END_ODD	0x1c39
#define ENCP_DE_H_BEGIN		0x1c3a
#define ENCP_DE_H_END		0x1c3b
#define ENCP_DE_V_BEGIN_EVEN	0x1c3c
#define ENCP_DE_V_END_EVEN	0x1c3d
#define ENCP_DE_V_BEGIN_ODD	0x1c3e
#define ENCP_DE_V_END_ODD	0x1c3f

#define REG(r, v) { .reg = (r), .value = (v) }

/* 1080i59.94/60 (VIC 5) and 1080i50 (VIC 20): 1920x1080, 1125 frame lines. */
static bool s7d_encp_is_1080i(const struct drm_display_mode *m)
{
	return m->hdisplay == 1920 && m->vdisplay == 1080 && m->vtotal == 1125 &&
	       (m->htotal == 2200 || m->htotal == 2640) &&
	       (m->clock == 74250 || (m->htotal == 2200 && m->clock == 74176)) &&
	       m->hsync_end - m->hsync_start == 44 && m->htotal - m->hsync_end == 148 &&
	       m->flags == (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
			    DRM_MODE_FLAG_INTERLACE);
}

static enum drm_mode_status
s7d_encp_build_1080i(const struct drm_display_mode *m, struct s7d_encp_state *state)
{
	/* DE_V_EVEN begins on line 20; the vendor's FIFO hold and flip window. */
	const u32 vs = 1, de_v_begin = 20, hold = de_v_begin - vs - 8;
	const struct s7d_reg_value regs[] = {
		REG(ENCP_VIDEO_MAX_PXCNT, m->htotal - 1),
		REG(ENCP_VIDEO_MAX_LNCNT, 1124),
		REG(ENCP_VIDEO_HSPULS_BEGIN, 88),
		REG(ENCP_VIDEO_HSPULS_END, 264),
		REG(ENCP_VIDEO_HSPULS_SWITCH, 88),
		REG(ENCP_VIDEO_HAVON_BEGIN, 192),
		REG(ENCP_VIDEO_HAVON_END, 2111),
		REG(ENCP_VIDEO_HSO_BEGIN, 0),
		REG(ENCP_VIDEO_HSO_END, 44),
		REG(ENCP_VIDEO_EQPULS_BEGIN, 2288),
		REG(ENCP_VIDEO_EQPULS_END, 2464),
		REG(ENCP_VIDEO_EQPULS_BLINE, 0),
		REG(ENCP_VIDEO_EQPULS_ELINE, 4),
		REG(ENCP_VIDEO_VSPULS_BEGIN, 440),
		REG(ENCP_VIDEO_VSPULS_END, 2200),
		REG(ENCP_VIDEO_VSPULS_BLINE, 0),
		REG(ENCP_VIDEO_VSPULS_ELINE, 4),
		REG(ENCP_VIDEO_VAVON_BLINE, 20),
		REG(ENCP_VIDEO_VAVON_ELINE, 559),
		REG(ENCP_VIDEO_VSO_BEGIN, 30),
		REG(ENCP_VIDEO_VSO_END, 50),
		REG(ENCP_VIDEO_VSO_BLINE, 0),
		REG(ENCP_VIDEO_VSO_ELINE, 5),
		REG(ENCP_VIDEO_OFLD_VOAV_OFST, 0x11),
		REG(ENCP_VIDEO_MODE, 0x5ffc),
		REG(ENCP_VIDEO_MODE_ADV, 0x18),
		REG(ENCP_VIDEO_SYNC_MODE, 0x7),
		REG(ENCP_DE_H_BEGIN, 194),
		REG(ENCP_DE_H_END, 2114),
		REG(ENCP_DE_V_BEGIN_EVEN, 20),
		REG(ENCP_DE_V_END_EVEN, 560),
		REG(ENCP_DE_V_BEGIN_ODD, 583),
		REG(ENCP_DE_V_END_ODD, 1123),
		REG(ENCP_DVI_HSO_BEGIN, 2),
		REG(ENCP_DVI_HSO_END, 46),
		REG(ENCP_DVI_VSO_BLINE_EVN, 0),
		REG(ENCP_DVI_VSO_ELINE_EVN, 5),
		REG(ENCP_DVI_VSO_BEGIN_EVN, 2),
		REG(ENCP_DVI_VSO_END_EVN, 2),
		REG(ENCP_DVI_VSO_BLINE_ODD, 562),
		REG(ENCP_DVI_VSO_ELINE_ODD, 567),
		/* The odd field's vertical sync starts half a line later. */
		REG(ENCP_DVI_VSO_BEGIN_ODD, 2 + m->htotal / 2),
		REG(ENCP_DVI_VSO_END_ODD, 2 + m->htotal / 2),
	};

	*state = (struct s7d_encp_state) {
		.count = ARRAY_SIZE(regs),
		.interlace = true,
		.flip_start = vs,
		.flip_end = vs + hold - 2,
		.fifo_hold_lines = hold,
		.hsync_positive = true,
		.vsync_positive = true,
	};
	memcpy(state->regs, regs, sizeof(regs));
	return MODE_OK;
}

/*
 * ENCI image the vendor kernel runs for 480i60 and 576i50 over HDMI, read
 * back from the running encoder (config_tv_enci and adjust_enci_for_hdmi,
 * pixel repetition 2). ENCI fetches 720 VPP pixels over 1440 counts.
 */
static const struct s7d_enci_reg s7d_enci_image[] = {
	{ 0x1b00, { 0x8, 0x13 } },
	{ 0x1b01, { 0x26, 0x26 } },
	{ 0x1b07, { 0x0, 0x28 } },
	{ 0x1b08, { 0x7, 0x7 } },
	{ 0x1b09, { 0x184, 0x184 } },
	{ 0x1b0a, { 0x9, 0x1 } },
	{ 0x1b0b, { 0x1d, 0x7f } },
	{ 0x1b0e, { 0x3, 0x3 } },
	{ 0x1b0f, { 0x3, 0x3 } },
	{ 0x1b10, { 0x0, 0x16 } },
	{ 0x1b11, { 0x6, 0x6 } },
	{ 0x1b13, { 0x2, 0x2 } },
	{ 0x1b16, { 0xea, 0x10c } },
	{ 0x1b17, { 0x68a, 0x6ac } },
	{ 0x1b18, { 0x11, 0x15 } },
	{ 0x1b19, { 0x101, 0x135 } },
	{ 0x1b1a, { 0x12, 0x16 } },
	{ 0x1b1b, { 0x102, 0x136 } },
	{ 0x1b23, { 0x1211, 0x1211 } },
	{ 0x1b24, { 0x1615, 0x1615 } },
	{ 0x1b26, { 0x16, 0x16 } },
	{ 0x1b29, { 0x1110, 0x1110 } },
	{ 0x1b2a, { 0xb535, 0xb535 } },
	{ 0x1b2b, { 0x1415, 0x1415 } },
	{ 0x1b30, { 0x1, 0x1 } },
	{ 0x1b31, { 0x17, 0x17 } },
	{ 0x1b32, { 0x15, 0x15 } },
	{ 0x1b33, { 0x21, 0x21 } },
	{ 0x1b34, { 0x15, 0x15 } },
	{ 0x1b35, { 0x5, 0x5 } },
	{ 0x1b36, { 0x5, 0x5 } },
	{ 0x1b37, { 0x2, 0x2 } },
	{ 0x1b38, { 0x1b, 0x1b } },
	{ 0x1b39, { 0x1b, 0x1b } },
	{ 0x1b3a, { 0x24, 0x24 } },
	{ 0x1b3b, { 0x7f8, 0x7f8 } },
	{ 0x1b3d, { 0xf, 0xf } },
	{ 0x1b3e, { 0xf, 0xf } },
	{ 0x1b3f, { 0x60, 0x60 } },
	{ 0x1b40, { 0x1, 0x1 } },
	{ 0x1b41, { 0xa, 0xa } },
	{ 0x1b43, { 0x5, 0x5 } },
	{ 0x1b44, { 0x4, 0x4 } },
	{ 0x1b45, { 0x3ff, 0x3ff } },
	{ 0x1b48, { 0x0, 0x0 } },
	{ 0x1b50, { 0x0, 0x8107 } },
	{ 0x1b54, { 0x2, 0x800 } },
	{ 0x1b55, { 0x4d53, 0x4d53 } },
	{ 0x1b56, { 0x22, 0x341 } },
	{ 0x1b60, { 0x0, 0x0 } },
	{ 0x1c00, { 0x6b0, 0x4 } },
	{ 0x1c01, { 0x78, 0x82 } },
	{ 0x1c02, { 0x105, 0x137 } },
	{ 0x1c03, { 0x105, 0x138 } },
	{ 0x1c04, { 0x1, 0x2 } },
	{ 0x1c05, { 0x2, 0x2 } },
	{ 0x1c06, { 0x356, 0x364 } },
	{ 0x1c07, { 0x6b0, 0x4 } },
	{ 0x1c08, { 0x6b0, 0x4 } },
	{ 0x1c09, { 0x356, 0x364 } },
	{ 0x1c0a, { 0x0, 0x10 } },
	{ 0x1c0b, { 0x321, 0x321 } },
	{ 0x1c0c, { 0x54, 0x54 } },
	{ 0x1c0d, { 0x3210, 0x3210 } },
	{ 0x1c0e, { 0x54, 0x54 } },
	{ 0x1c0f, { 0x1fff, 0x1fff } },
	{ 0x1c11, { 0x1, 0x1 } },
	{ 0x1c12, { 0x200, 0x200 } },
	{ 0x1c13, { 0x200, 0x200 } },
	{ 0x1c14, { 0x200, 0x200 } },
	{ 0x1c15, { 0x113, 0x113 } },
	{ 0x1c16, { 0x168, 0x168 } },
	{ 0x1c18, { 0x4e01, 0x4e01 } },
	{ 0x1c19, { 0xe9, 0x10b } },
	{ 0x1c1a, { 0x689, 0x6ab } },
	{ 0x1c1b, { 0x11, 0x15 } },
	{ 0x1c1c, { 0x101, 0x135 } },
	{ 0x1c1d, { 0x12, 0x16 } },
	{ 0x1c1e, { 0x102, 0x136 } },
};

/* Registers the vendor programs explicitly; these must read back. */
static const u16 s7d_enci_checked[] = {
	0x1b00, 0x1b01, 0x1b07, 0x1b0a, 0x1b0b, 0x1b0e, 0x1b0f, 0x1b10,
	0x1b16, 0x1b17, 0x1b18, 0x1b19, 0x1b1a, 0x1b1b, 0x1b50, 0x1b54,
	0x1b56, 0x1c00, 0x1c01, 0x1c02, 0x1c03, 0x1c04, 0x1c05, 0x1c06,
	0x1c07, 0x1c08, 0x1c09, 0x1c0a, 0x1c18, 0x1c19, 0x1c1a, 0x1c1b,
	0x1c1c, 0x1c1d, 0x1c1e,
};

unsigned int s7d_enci_image_size(void)
{
	return ARRAY_SIZE(s7d_enci_image);
}

void s7d_enci_image_entry(const struct s7d_encp_state *state, unsigned int i,
			  u32 *reg, u32 *value)
{
	*reg = s7d_enci_image[i].reg;
	*value = s7d_enci_image[i].value[state->pal];
}

/* 480i59.94/60 (VIC 6/7) and 576i50 (VIC 21/22) as 720-pixel DBLCLK modes. */
static bool s7d_encp_is_enci(const struct drm_display_mode *m)
{
	if (m->flags != (DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
			 DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK) ||
	    m->hdisplay != 720)
		return false;
	if (m->vdisplay == 480)
		return m->htotal == 858 && m->vtotal == 525 &&
		       m->clock >= 13500 && m->clock <= 13514;
	return m->vdisplay == 576 && m->htotal == 864 && m->vtotal == 625 &&
	       m->clock == 13500;
}

static enum drm_mode_status
s7d_encp_build_enci(const struct drm_display_mode *m, struct s7d_encp_state *state)
{
	bool pal = m->vdisplay == 576;
	/* VFIFO2VD_LINE_TOP_START: the field's first fetched line. */
	const u32 vs = 1, top = pal ? 21 : 17, hold = top - vs - 8;
	unsigned int i, j;

	*state = (struct s7d_encp_state) {
		.interlace = true,
		.enci = true,
		.pal = pal,
		.flip_start = vs,
		.flip_end = vs + hold - 2,
		.fifo_hold_lines = hold,
	};
	for (i = 0; i < ARRAY_SIZE(s7d_enci_checked); i++)
		for (j = 0; j < ARRAY_SIZE(s7d_enci_image); j++)
			if (s7d_enci_image[j].reg == s7d_enci_checked[i])
				state->regs[state->count++] = (struct s7d_reg_value) {
					s7d_enci_image[j].reg, s7d_enci_image[j].value[pal] };
	return MODE_OK;
}

static enum drm_mode_status s7d_encp_check_mode(const struct drm_display_mode *m)
{
	u32 sync_flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
			 DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC;
	enum drm_mode_status status;

	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		return s7d_encp_is_1080i(m) || s7d_encp_is_enci(m) ?
			s7d_display_mode_valid(m) : MODE_NO_INTERLACE;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;
	if (m->vscan > 1)
		return MODE_NO_VSCAN;
	if (m->flags & DRM_MODE_FLAG_3D_MASK)
		return MODE_NO_STEREO;
	if (m->flags & ~sync_flags || m->hskew)
		return MODE_BAD;
	if ((m->flags & (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC)) ==
	    (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC) ||
	    (m->flags & (DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC)) ==
	    (DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC))
		return MODE_BAD;
	status = s7d_display_mode_valid(m);
	if (status != MODE_OK)
		return status;

	if (m->hdisplay >= m->hsync_start || m->hsync_start >= m->hsync_end ||
	    m->hsync_end >= m->htotal)
		return MODE_BAD_HVALUE;
	if (m->vdisplay >= m->vsync_start || m->vsync_start >= m->vsync_end ||
	    m->vsync_end >= m->vtotal)
		return MODE_BAD_VVALUE;

	/*
	 * Keep DE inside a frame and the request two pixels ahead of output.
	 * Wrapping the fetch/sync intervals is deliberately not implemented.
	 */
	if (m->hsync_start - m->hdisplay < 3)
		return MODE_HBLANK_NARROW;
	if (m->vsync_start - m->vdisplay < 2)
		return MODE_VBLANK_NARROW;
	return MODE_OK;
}

enum drm_mode_status
s7d_encp_build_state(const struct drm_display_mode *m, struct s7d_encp_state *state)
{
	const u32 latency = 2, vs = 1;
	u32 hs, de_h_begin, de_h_end, de_v_begin, de_v_end, hsync, vsync, hold = 8;
	enum drm_mode_status status;

	if (!m || !state)
		return MODE_ERROR;
	status = s7d_encp_check_mode(m);
	if (status != MODE_OK)
		return status;
	if (m->flags & DRM_MODE_FLAG_DBLCLK)
		return s7d_encp_build_enci(m, state);
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		return s7d_encp_build_1080i(m, state);

	hs = min_t(u32, 5, m->hsync_start - m->hdisplay - 1);
	hsync = m->hsync_end - m->hsync_start;
	vsync = m->vsync_end - m->vsync_start;
	de_h_end = m->htotal - (m->hsync_start - m->hdisplay) + hs;
	de_h_begin = de_h_end - m->hdisplay;
	de_v_end = m->vtotal - (m->vsync_start - m->vdisplay) + vs;
	de_v_begin = de_v_end - m->vdisplay;
	/* Leave eight lines to fill the FIFO before the active display window. */
	if (de_v_begin >= vs + 16)
		hold = min_t(u32, 31, de_v_begin - vs - 8);

	*state = (struct s7d_encp_state) {
		.regs = {
			{ ENCP_DVI_HSO_BEGIN, hs },
			{ ENCP_DVI_HSO_END, hs + hsync },
			{ ENCP_DVI_VSO_BLINE_EVN, vs },
			{ ENCP_DVI_VSO_ELINE_EVN, vs + vsync },
			{ ENCP_DVI_VSO_BEGIN_EVN, hs },
			{ ENCP_DVI_VSO_END_EVN, hs },
			{ ENCP_DE_H_BEGIN, de_h_begin },
			{ ENCP_DE_H_END, de_h_end },
			{ ENCP_DE_V_BEGIN_EVEN, de_v_begin },
			{ ENCP_DE_V_END_EVEN, de_v_end },
			{ ENCP_VIDEO_MODE, 0x4040 },
			{ ENCP_VIDEO_MODE_ADV, 0x18 },
			{ ENCP_VIDEO_HAVON_BEGIN, de_h_begin - latency },
			{ ENCP_VIDEO_HAVON_END, de_h_end - latency - 1 },
			{ ENCP_VIDEO_VAVON_BLINE, de_v_begin },
			{ ENCP_VIDEO_VAVON_ELINE, de_v_end - 1 },
			{ ENCP_VIDEO_HSO_BEGIN, hs - latency },
			{ ENCP_VIDEO_HSO_END, hs + hsync - latency },
			{ ENCP_VIDEO_VSO_BEGIN, 0 },
			{ ENCP_VIDEO_VSO_END, 0 },
			{ ENCP_VIDEO_VSO_BLINE, vs },
			{ ENCP_VIDEO_VSO_ELINE, vs + vsync },
			{ ENCP_VIDEO_MAX_PXCNT, m->htotal - 1 },
			{ ENCP_VIDEO_MAX_LNCNT, m->vtotal - 1 },
			/* Undo an interlaced timing; the values a progressive ENCP runs with. */
			{ ENCP_VIDEO_OFLD_VOAV_OFST, 0x111 },
			{ ENCP_VIDEO_SYNC_MODE, 0x7 },
		},
		.count = 26,
		.flip_start = vs,
		.flip_end = de_v_begin >= vs + hold + 8 ? vs + hold - 2 : 0,
		.fifo_hold_lines = hold,
		.hsync_positive = m->flags & DRM_MODE_FLAG_PHSYNC,
		.vsync_positive = m->flags & DRM_MODE_FLAG_PVSYNC,
	};
	return MODE_OK;
}
