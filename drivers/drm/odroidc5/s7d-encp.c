// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/*
 * Progressive RGB8 ENCP timing for the S7D DRM atomic state.
 * Timing sequence derived from Amlogic's config_tv_enc_calc (2019).
 * No VIC table, MMIO, clock or power operation belongs in this calculation.
 */
#include <linux/kernel.h>

#include "s7d-encp.h"

/*
 * Use the S7D Linux/U-Boot VCBUS indices. Datasheet 01's ENCP HAVON and
 * VAVON table offsets differ from those headers and the running C5.
 */
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
#define ENCP_DVI_VSO_ELINE_EVN	0x1c34
#define ENCP_DVI_VSO_BEGIN_EVN	0x1c36
#define ENCP_DVI_VSO_END_EVN	0x1c38
#define ENCP_DE_H_BEGIN		0x1c3a
#define ENCP_DE_H_END		0x1c3b
#define ENCP_DE_V_BEGIN_EVEN	0x1c3c
#define ENCP_DE_V_END_EVEN	0x1c3d

static enum drm_mode_status s7d_encp_check_mode(const struct drm_display_mode *m)
{
	u32 sync_flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
			 DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC;

	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
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
	if (m->clock < 25175)
		return MODE_CLOCK_LOW;
	if (m->clock > 594000)
		return MODE_CLOCK_HIGH;

	/* The initial primary plane has 12-bit display coordinates. */
	if (!m->hdisplay || m->hdisplay > 4096 ||
	    m->hdisplay >= m->hsync_start || m->hsync_start >= m->hsync_end ||
	    m->hsync_end >= m->htotal || m->htotal > 8192)
		return MODE_BAD_HVALUE;
	if (!m->vdisplay || m->vdisplay > 4096 ||
	    m->vdisplay >= m->vsync_start || m->vsync_start >= m->vsync_end ||
	    m->vsync_end >= m->vtotal || m->vtotal > 8192)
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
		},
		.flip_start = vs,
		.flip_end = de_v_begin >= vs + hold + 8 ? vs + hold - 2 : 0,
		.fifo_hold_lines = hold,
		.hsync_positive = m->flags & DRM_MODE_FLAG_PHSYNC,
		.vsync_positive = m->flags & DRM_MODE_FLAG_PVSYNC,
	};
	return MODE_OK;
}
