// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <drm/drm_fourcc.h>

#include "s7d-vdin.h"
#include "s7d-encp.h"
#include "s7d-caps.h"
#include "s7d-vpu.h"

/* VCBUS word indices; the VPU owns the mapping. */
#define VD_DMA_ALIGN		64U
#define VD_ADDR_SHIFT		4U
#define VD_STRIDE_SHIFT	4U

#define VD_COM_CTRL0		0x1302U
#define VD_COM_MUX_MASK	0x0000000fU
#define VD_COM_MUX_WB0		0x00000007U
#define VD_COM_INPUT_EN	0x00000010U
#define VD_COM_WINDOW_EN	0x00000020U
#define VD_COM_RGB_MANUAL_MAP	0x00000180U
#define VD_COM_IDENTITY_MAP	0x00000900U
#define VD_COM_CONFIG_MASK	0xffffffffU

#define VD_WIDTHM1I_WIDTHM1O	0x130aU
#define VD_WIDTH_MASK		0x1fff1fffU
#define VD_WIDTH_VALUE(in, out) ((((in) - 1U) << 16) | ((out) - 1U))
#define VD_SC_MISC_CTRL		0x130bU
#define VD_HSC_ENABLE_MASK	0x000000c0U
#define VD_HSC_CONFIG_MASK	0x000001ffU
/* hsc_en, short_line_o_en, nearest_en and a four-tap bank. */
#define VD_HSC_DOWNSCALE	0x00000074U
#define VD_SCALE_COEF_IDX	0x1300U
#define VD_SCALE_COEF		0x1301U
#define VD_HSC_PHASE_STEP	0x130cU
#define VD_HSC_INI_CTRL		0x130dU
#define VD_HSC_INI_VALUE	(BIT(29) | (4U << 24))
#define VD_ASFIFO_CTRL2		0x130fU
#define VD_DECIMATION_MASK	0x03ff0000U

#define VD_MATRIX_CTRL		0x1310U
#define VD_MATRIX_DISABLE_MASK	0x00000083U
/* Matrix enables, coefficient index, highlight and matrix1 bypass. */
#define VD_MATRIX_CONFIG_MASK	0x0000018fU
#define VD_MATRIX1_ENABLE	(BIT(1) | (1U << 2))
#define VD_MATRIX_COEF00_01	0x1311U
#define VD_MATRIX_COEF22	0x1315U
#define VD_MATRIX_OFFSET0_1	0x1316U
#define VD_MATRIX_OFFSET2	0x1317U
#define VD_MATRIX_PRE_OFFSET0_1	0x1318U
#define VD_MATRIX_PRE_OFFSET2	0x1319U
#define VD_LFIFO_CTRL		0x131aU
#define VD_LFIFO_CONFIG_MASK	0xd0073fffU
#define VD_LFIFO_DEPTH		1920U
#define VD_COM_GCLK_CTRL	0x131bU
#define VD_COM_GCLK_MASK	0x0000ffffU
#define VD_GCLK_AUTO		0x00000000U
#define VD_INTF_WIDTHM1		0x131cU
#define VD_INTF_WIDTH_MASK	0x00001fffU
#define VD_INTF_WIDTH_VALUE(width) ((width) - 1U)

#define VD_WR_CTRL2		0x131fU
#define VD_WR_CTRL2_CONFIG_MASK 0x000ff7ffU
#define VD_WR_CTRL2_RGB8	0x00011000U
#define VD_WR_CTRL		0x1320U
#define VD_WR_CTRL_CONFIG_MASK	0xf8ff3fffU
#define VD_WR_CTRL_RGB8		0x88c31600U
#define VD_WR_CTRL_FORMAT_MASK	0x00073000U
#define VD_WR_FMT(fmt)		((u32)(fmt) << 12)
#define VD_WR_VCONV(mode)	((u32)(mode) << 16)
#define VD_WR_SWAP_CBCR		0x00040000U
#define VD_WR_DISABLE_CTRL_PULSE 0x00000400U
#define VD_WR_REQUEST_EN	0x00000100U
#define VD_WR_H_START_END	0x1321U
#define VD_WR_H_MASK		0x7fff1fffU
#define VD_WR_H_VALUE(width)	(0x40000000U | ((width) - 1U))
#define VD_WR_V_START_END	0x1322U
#define VD_WR_V_MASK		0x3fff1fffU
#define VD_WR_V_VALUE(height)	((height) - 1U)
#define VD_VSC_PHASE_STEP	0x1323U
#define VD_VSC_INI_CTRL		0x1324U
#define VD_VSC_ENABLE		0x00800000U
#define VD_VSC_CONFIG_MASK	0x00bfffffU
#define VD_VSC_INI_PHASE	0x00008000U
#define VD_SCIN_HEIGHTM1	0x1325U
#define VD_DUMMY_DATA		0x1326U
/* VSC keeps one line in a buffer of this many pixels. */
#define VD_VSC_MAX_WIDTH	1920U
#define VD_CM_BRI_CON_CTRL	0x132dU
#define VD_CM_TOP_ENABLE	0x10000000U

#define VD_WIN_H_START_END	0x136dU
#define VD_WIN_V_START_END	0x136eU
#define VD_WIN_MASK		0x1fff1fffU
#define VD_WIN_H_VALUE(width)	((width) - 1U)
#define VD_WIN_V_VALUE(height)	((height) - 1U)
#define VD_ASFIFO_CTRL3		0x136fU
#define VD_ASFIFO_WB0_MASK	0x000000ffU
#define VD_ASFIFO_WB0_CONFIG	0x000000e4U
#define VD_ASFIFO_DE_ENABLE	0x00000080U
#define VD_ASFIFO_GO_ENABLE	0x00000060U
#define VD_ASFIFO_OVFL_CLEAR	0x00000002U
#define VD_ASFIFO_SOFT_RESET	0x00000001U
#define VD_COM_GCLK_CTRL2	0x1370U
#define VD_COM_GCLK2_MASK	0x0000000fU
#define VD_VSHRK_CTRL		0x1371U

#define VD_META_WR_CTRL		0x1379U
#define VD_META_WR_ENABLE	0x40000000U
#define VD_HDR2_CTRL		0x1380U
#define VD_HDR_DISABLE_MASK	0x001fe02cU
#define VD_HDR2_MATRIXI_EN_CTRL	0x13bbU
#define VD_HDR2_MATRIXO_EN_CTRL	0x13bcU
#define VD_HDR_MATRIX_ENABLE	0x00000001U
#define VD_LINE_INT		0x13cbU
#define VD_LINE_INT_ENABLE	0x00008000U
#define VD_WRITE_DONE_CLEAR	0x00010000U
#define VD_WR_BADDR_LUMA	0x13ccU
#define VD_WR_BADDR_CHROMA	0x13cdU
#define VD_WR_STRIDE_LUMA	0x13ceU
#define VD_WR_STRIDE_CHROMA	0x13cfU
#define VD_WR_BADDR_VALUE(dma)	((dma) >> VD_ADDR_SHIFT)
#define VD_WR_STRIDE_VALUE(pitch) ((pitch) >> VD_STRIDE_SHIFT)

/* Save and restore only these owned capture-routing fields. */
#define VD_WB_CTRL		0x1df9U
#define VD_WB_SOURCE_MASK	0x0000000fU
#define VD_WB_VPP		0x00000006U
#define VD_WB_VPP_HBLANK_MASK	0x00ff0000U
#define VD_WB_VPP_HBLANK	0x00ff0000U
#define VD_WB_FRAME_CTRL	0x1a51U
#define VD_WB_HSYNC_MASK	0x03000000U
#define VD_WB_HSYNC_ENABLE	0x03000000U
#define VD_WB_MISC_CTRL		0x1a0dU
#define VD_WB_MISC_MASK		0x00000003U
#define VD_WB_MISC_WB0		0x00000001U
#define VD_VIU2VDIN_HDN_CTRL	0x2780U
#define VD_IF_MUX_CTRL		0x2783U
#define VD_IF_WB0_MASK		0x00007f7fU
#define VD_IF_WB0_CONFIG	0x00000808U
#define VD_TOP_DOUBLE_CTRL	0x410bU
#define VD_MIF1_ROUTE_MASK	0x00000f00U
#define VD_MIF1_VDIN1_NORMAL	0x00000300U

/* Read-only observations; never write these fields to force an idle result. */
#define VD_ACTIVE_MAX_PIX	0x1303U
#define VD_LCNT_STATUS		0x1304U
#define VD_COM_STATUS0		0x1305U
#define VD_LCNT_SHADOW_STATUS	0x1307U
#define VD_LFIFO_COUNT_MASK	0x00001ff8U
#define VD_COM_STATUS2		0x130eU
#define VD_WB0_FIFO_OVERFLOW	0x00008000U
#define VD_WB0_FIFO_COUNT_MASK	0x00003f00U
#define VD_WR_ARB_STATUS	0x13c4U
#define VD_WR_ARB_BUSY		0x00000004U
#define VD_WRITE_DONE_FLAG	0x40000000U
#define VD_ASYNC_STAT		0x27adU
#define VD_ENCP_INFO_READ	0x271dU
#define VD_AXI_WRITE_IDLE_MASK	0x00030000U
#define VD_AXI_WRITE_IDLE	0x00030000U

#define VD_COUNTER_MASK 0x1fffU
#define VD_STOP_NS (120 * NSEC_PER_MSEC)
#define VD_QUIET_NS (10 * NSEC_PER_MSEC)
/* The first done may close a frame that began before the capture was armed. */
#define VD_DONE_COUNT 2U

struct vd_route {
	u16 reg;
	u32 mask;
	u32 value;
};

static const struct vd_route vd_routes[] = {
	{ VD_WB_FRAME_CTRL, VD_WB_HSYNC_MASK, VD_WB_HSYNC_ENABLE },
	{ VD_WB_MISC_CTRL, VD_WB_MISC_MASK, VD_WB_MISC_WB0 },
	{ VD_VIU2VDIN_HDN_CTRL, 0x000f3fff, 0 },
	{ VD_WB_CTRL, VD_WB_SOURCE_MASK | VD_WB_VPP_HBLANK_MASK,
		VD_WB_VPP | VD_WB_VPP_HBLANK },
	{ VD_IF_MUX_CTRL, VD_IF_WB0_MASK, VD_IF_WB0_CONFIG },
	{ VD_TOP_DOUBLE_CTRL, VD_MIF1_ROUTE_MASK, VD_MIF1_VDIN1_NORMAL },
};

struct s7d_vdin {
	struct s7d_vpu *vpu;
	struct reset_control_bulk_data resets[2];
	u32 saved_routes[ARRAY_SIZE(vd_routes)];
	u64 frame_ns;
	bool active;
	bool routes_owned;
	/* Protects the armed capture against the VSYNC interrupt. */
	spinlock_t irq_lock;
	struct completion done;
	struct drm_display_mode mode;
	unsigned int events;
	int status;
	bool armed;
	/* Interlaced capture weaves the fields into the frame buffer. */
	bool interlace;
	dma_addr_t base;
	u32 pitch;
	u8 fields;
	s8 field;
};

static const struct drm_display_mode vdin_modes[] = {
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED, 148500,
		1920, 2008, 2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000,
		3840, 4016, 4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000,
		3840, 4016, 4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
};

struct vdin_format {
	u32 format;
	u8 cpp;
	u8 wr_fmt;
	u8 vconv;
	bool swap_cbcr;
	bool yuv;
};

/*
 * The component map runs before the matrix; VPP delivers R, G, B and the
 * RGB map reverses that. WR_FMT 1 stores the three components last first,
 * 0 packs 4:2:2 as Cb Y0 Cr Y1 and 2 writes Y and an interleaved CbCr plane
 * from even lines.
 */
static const struct vdin_format vdin_format_table[] = {
	{ DRM_FORMAT_BGR888, 3, 1, 3 },
	{ DRM_FORMAT_XBGR2101010, 4, 1, 3 },
	{ DRM_FORMAT_VUY888, 3, 1, 3, .yuv = true },
	{ DRM_FORMAT_UYVY, 2, 0, 0, .yuv = true },
	{ DRM_FORMAT_NV12, 1, 2, 0, .swap_cbcr = true, .yuv = true },
	{ DRM_FORMAT_NV21, 1, 2, 0, .yuv = true },
};

static const u32 vdin_formats[] = {
	DRM_FORMAT_BGR888, DRM_FORMAT_XBGR2101010, DRM_FORMAT_VUY888,
	DRM_FORMAT_UYVY, DRM_FORMAT_NV12, DRM_FORMAT_NV21,
};

/* BT.709 limited range from full-range R, G, B (s2.10, vendor RGB_YUV709). */
static const s16 vdin_rgb_to_709[3][3] = {
	{ 0x00bb, 0x0275, 0x003f },
	{ -0x0067, -0x015a, 0x01c2 },
	{ 0x01c2, -0x0199, -0x0029 },
};

static const struct vdin_format *vdin_find_format(u32 format)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vdin_format_table); i++)
		if (vdin_format_table[i].format == format)
			return &vdin_format_table[i];
	return NULL;
}

/* VDIN's own bicubic horizontal filter. */
static const u32 vdin_hsc_coef[33] = {
	0x00800000, 0x007f0100, 0xff7f0200, 0xfe7f0300, 0xfd7e0500,
	0xfc7e0600, 0xfb7d0800, 0xfb7c0900, 0xfa7b0b00, 0xfa7a0dff,
	0xf9790fff, 0xf97711ff, 0xf87613ff, 0xf87416fe, 0xf87218fe,
	0xf8701afe, 0xf76f1dfd, 0xf76d1ffd, 0xf76b21fd, 0xf76824fd,
	0xf76627fc, 0xf76429fc, 0xf7612cfc, 0xf75f2ffb, 0xf75d31fb,
	0xf75a34fb, 0xf75837fa, 0xf7553afa, 0xf8523cfa, 0xf8503ff9,
	0xf84d42f9, 0xf84a45f9, 0xf84848f8,
};

const struct s7d_writeback_caps s7d_vdin_writeback_caps = {
	.formats = vdin_formats,
	.num_formats = ARRAY_SIZE(vdin_formats),
	.max_bpc = 10,
	.max_width = S7D_DISPLAY_MAX_WIDTH,
	.max_height = S7D_DISPLAY_MAX_HEIGHT,
	.modes = vdin_modes,
	.num_modes = ARRAY_SIZE(vdin_modes),
};

static enum drm_mode_status vdin_mode_valid(void *data, const struct drm_display_mode *mode)
{
	const u32 sync = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
			 DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC;
	struct s7d_encp_state encp;

	/* VDIN reads the VPP output, whatever polarity the encoder sends. */
	if (!mode || (mode->flags & ~(sync | DRM_MODE_FLAG_INTERLACE)))
		return MODE_BAD;
	return s7d_encp_build_state(mode, &encp);
}

static int vdin_frame_period(const struct drm_display_mode *mode,
			     unsigned long pixel_rate, u64 *frame_ns)
{
	u64 pixels, scaled;

	if (!pixel_rate || mode->htotal <= 0 || mode->vtotal <= 0 ||
	    check_mul_overflow((u64)mode->htotal, (u64)mode->vtotal, &pixels) ||
	    check_mul_overflow(pixels, (u64)NSEC_PER_SEC, &scaled))
		return -ERANGE;
	*frame_ns = DIV_ROUND_UP_ULL(scaled, pixel_rate);
	return *frame_ns ? 0 : -ERANGE;
}

static u32 vdin_read(struct s7d_vdin *v, u16 reg)
{
	return readl(v->vpu->regs + (reg << 2));
}

static int vdin_update(struct s7d_vdin *v, u16 reg, u32 mask, u32 value)
{
	u32 old = vdin_read(v, reg);

	writel((old & ~mask) | (value & mask), v->vpu->regs + (reg << 2));
	if ((vdin_read(v, reg) & mask) != (value & mask)) {
		v->vpu->failed_reg = reg;
		return -EIO;
	}
	return 0;
}

static int vdin_check(void *data, const struct s7d_crtc_state *state,
		      const struct s7d_writeback_buffer *buffer)
{
	struct s7d_vdin *v = data;
	const struct drm_display_mode *mode = &state->base.adjusted_mode;
	const struct vdin_format *format = vdin_find_format(buffer->format);
	u64 size, end, frame_ns;

	if (!format)
		return -EINVAL;
	if (!state->valid || !state->base.active || !state->base.enable ||
	    state->base.async_flip || state->base.vrr_enabled ||
	    state->base.self_refresh_active ||
	    vdin_mode_valid(data, mode) != MODE_OK ||
	    buffer->width > mode->hdisplay || buffer->height > mode->vdisplay ||
	    (buffer->height != mode->vdisplay && buffer->width > VD_VSC_MAX_WIDTH) ||
	    buffer->pitch < buffer->width * format->cpp ||
	    !IS_ALIGNED(buffer->dma, VD_DMA_ALIGN) ||
	    !IS_ALIGNED(buffer->pitch, VD_DMA_ALIGN) ||
	    !!buffer->chroma_dma != (format->wr_fmt == 2) ||
	    !IS_ALIGNED(buffer->chroma_dma, VD_DMA_ALIGN) ||
	    !IS_ALIGNED(buffer->chroma_pitch, VD_DMA_ALIGN))
		return -EINVAL;
	if (vdin_frame_period(mode, state->output.rate.pixel, &frame_ns) ||
	    check_mul_overflow((u64)buffer->pitch, (u64)buffer->height, &size) ||
	    buffer->size < size || check_add_overflow((u64)buffer->dma, size - 1, &end) ||
	    end > U32_MAX ||
	    clk_get_rate(v->vpu->clocks[0].clk) < state->output.rate.pixel)
		return -ERANGE;
	/* Fields are woven line by line at full size; 4:2:0 would mix them. */
	if (mode->flags & DRM_MODE_FLAG_INTERLACE &&
	    (format->wr_fmt == 2 || buffer->width != mode->hdisplay ||
	     buffer->height != mode->vdisplay))
		return -EINVAL;
	if (format->wr_fmt == 2 &&
	    (buffer->chroma_pitch < buffer->width ||
	     check_mul_overflow((u64)buffer->chroma_pitch, (u64)buffer->height / 2, &size) ||
	     buffer->chroma_size < size ||
	     check_add_overflow((u64)buffer->chroma_dma, size - 1, &end) || end > U32_MAX))
		return -ERANGE;
	return 0;
}

static bool vdin_writes_idle(struct s7d_vdin *v)
{
	return !(vdin_read(v, VD_COM_STATUS0) & VD_LFIFO_COUNT_MASK) &&
	       !(vdin_read(v, VD_WR_ARB_STATUS) & VD_WR_ARB_BUSY) &&
	       (vdin_read(v, VD_ASYNC_STAT) & VD_AXI_WRITE_IDLE_MASK) == VD_AXI_WRITE_IDLE;
}

static bool vdin_drained(struct s7d_vdin *v)
{
	return vdin_writes_idle(v) &&
	       !(vdin_read(v, VD_COM_STATUS2) &
		 (VD_WB0_FIFO_COUNT_MASK | VD_WB0_FIFO_OVERFLOW));
}

/* No input and no write requests: nothing can reach memory. */
static bool vdin_stopped(struct s7d_vdin *v)
{
	return !(vdin_read(v, VD_COM_CTRL0) & (VD_COM_INPUT_EN | VD_COM_MUX_MASK)) &&
	       !(vdin_read(v, VD_WR_CTRL) & VD_WR_REQUEST_EN) && vdin_writes_idle(v);
}

static bool vdin_quiet(struct s7d_vdin *v)
{
	return vdin_stopped(v) && vdin_drained(v);
}

static int vdin_wait_drained(struct s7d_vdin *v, u64 deadline, bool stable)
{
	u64 quiet_since = 0;

	do {
		if (vdin_read(v, VD_COM_STATUS2) & VD_WB0_FIFO_OVERFLOW)
			return -EOVERFLOW;
		if (vdin_drained(v)) {
			if (!stable)
				return 0;
			if (!quiet_since)
				quiet_since = ktime_get_ns();
			if (ktime_get_ns() - quiet_since >= VD_QUIET_NS)
				return 0;
		} else {
			quiet_since = 0;
		}
		usleep_range(500, 1000);
	} while (ktime_get_ns() < deadline);
	return -ETIMEDOUT;
}

/* The WB0 overflow status is sticky until this clear pulse. */
static int vdin_clear_overflow(struct s7d_vdin *v)
{
	int ret;

	ret = vdin_update(v, VD_ASFIFO_CTRL3, VD_ASFIFO_OVFL_CLEAR, VD_ASFIFO_OVFL_CLEAR);
	if (!ret)
		ret = vdin_update(v, VD_ASFIFO_CTRL3, VD_ASFIFO_OVFL_CLEAR, 0);
	if (!ret && (vdin_read(v, VD_COM_STATUS2) & VD_WB0_FIFO_OVERFLOW))
		ret = -EOVERFLOW;
	return ret;
}

static int vdin_restore_routes(struct s7d_vdin *v)
{
	unsigned int i;
	int ret, err;

	if (!v->routes_owned)
		return 0;
	ret = vdin_update(v, VD_IF_MUX_CTRL, VD_IF_WB0_MASK, 0);
	for (i = ARRAY_SIZE(vd_routes); i-- > 0;) {
		err = vdin_update(v, vd_routes[i].reg, vd_routes[i].mask,
				  v->saved_routes[i]);
		if (!ret)
			ret = err;
	}
	if (!ret)
		v->routes_owned = false;
	return ret;
}

static int vdin_park(struct s7d_vdin *v)
{
	u64 deadline = ktime_get_ns() + max_t(u64, VD_STOP_NS,
					     2 * v->frame_ns + 2 * VD_QUIET_NS);
	int ret, err;

	if (!v->active)
		return 0;
	if (!v->vpu->acquired)
		return -EHOSTDOWN;
	ret = vdin_update(v, VD_ASFIFO_CTRL3, VD_ASFIFO_DE_ENABLE, 0);
	/* The failed capture already reported the overflow; let the rest drain. */
	if (!ret && (vdin_read(v, VD_COM_STATUS2) & VD_WB0_FIFO_OVERFLOW))
		ret = vdin_clear_overflow(v);
	if (!ret)
		ret = vdin_wait_drained(v, deadline, false);
	err = vdin_update(v, VD_ASFIFO_CTRL3,
			  VD_ASFIFO_DE_ENABLE | VD_ASFIFO_GO_ENABLE, 0);
	if (!ret)
		ret = err;
	if (!ret)
		ret = vdin_wait_drained(v, deadline, true);
	if (v->routes_owned) {
		err = vdin_update(v, VD_IF_MUX_CTRL, VD_IF_WB0_MASK, 0);
		if (!ret)
			ret = err;
	}
	err = vdin_update(v, VD_COM_CTRL0, VD_COM_INPUT_EN | VD_COM_MUX_MASK, 0);
	if (!ret)
		ret = err;
	err = vdin_update(v, VD_WR_CTRL, VD_WR_REQUEST_EN, 0);
	if (!ret)
		ret = err;
	if (ret)
		return ret;
	ret = vdin_wait_drained(v, deadline, true);
	if (!ret && !vdin_quiet(v))
		ret = -EIO;
	if (!ret)
		ret = vdin_restore_routes(v);
	if (!ret && !vdin_quiet(v))
		ret = -EIO;
	if (!ret)
		v->active = false;
	return ret;
}

static bool vdin_foreign_writer(struct s7d_vdin *v)
{
	u32 route = vdin_read(v, VD_TOP_DOUBLE_CTRL);
	unsigned int shift;

	if (vdin_read(v, VD_META_WR_CTRL) & VD_META_WR_ENABLE)
		return true;
	for (shift = 0; shift <= 12; shift += 4) {
		u32 source = (route >> shift) & 0xf;

		if (shift != 8 && (source == 3 || source == 4))
			return true;
	}
	return (vdin_read(v, VD_IF_MUX_CTRL) & VD_IF_WB0_MASK) ||
	       ((route & VD_MIF1_ROUTE_MASK) != 0 &&
		(route & VD_MIF1_ROUTE_MASK) != VD_MIF1_VDIN1_NORMAL);
}

static int vdin_route(struct s7d_vdin *v, u32 width)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(vd_routes); i++)
		v->saved_routes[i] = vdin_read(v, vd_routes[i].reg);
	v->routes_owned = true;
	ret = vdin_update(v, VD_IF_MUX_CTRL, VD_IF_WB0_MASK, 0);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(vd_routes); i++) {
		u32 value = vd_routes[i].reg == VD_VIU2VDIN_HDN_CTRL ? width : vd_routes[i].value;

		ret = vdin_update(v, vd_routes[i].reg, vd_routes[i].mask, value);
		if (ret)
			return ret;
	}
	return 0;
}

static int vdin_reinitialize(struct s7d_vdin *v)
{
	unsigned int i;
	u32 value;
	int ret, err;

	ret = vdin_update(v, VD_COM_GCLK_CTRL, VD_COM_GCLK_MASK, VD_GCLK_AUTO);
	if (!ret)
		ret = vdin_update(v, VD_COM_GCLK_CTRL2, VD_COM_GCLK2_MASK, VD_GCLK_AUTO);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(v->resets); i++) {
		ret = reset_control_assert(v->resets[i].rstc);
		err = reset_control_deassert(v->resets[i].rstc);
		if (ret || err) {
			dev_err(v->vpu->dev, "VDIN reset %s failed: assert %d deassert %d\n",
				v->resets[i].id, ret, err);
			return ret ? ret : err;
		}
	}
	/*
	 * The block resets leave the WB0 async FIFO alone; data it latched
	 * after the last capture parked would otherwise block every capture.
	 */
	value = vdin_read(v, VD_ASFIFO_CTRL3) & ~VD_ASFIFO_SOFT_RESET;
	writel(value | VD_ASFIFO_SOFT_RESET, v->vpu->regs + (VD_ASFIFO_CTRL3 << 2));
	readl(v->vpu->regs + (VD_ASFIFO_CTRL3 << 2));
	ret = vdin_update(v, VD_ASFIFO_CTRL3, VD_ASFIFO_SOFT_RESET, 0);
	if (!ret)
		ret = vdin_clear_overflow(v);
	if (ret)
		return ret;
	return vdin_quiet(v) ? 0 : -EBUSY;
}

/* Program Y, Cb, Cr, or for WR_FMT 1 Cr, Cb, Y so that memory holds Y first. */
static void vdin_matrix(u32 regs[9], bool reverse)
{
	static const u16 offsets[3] = { 0x040, 0x200, 0x200 };
	u32 c[3][3], o[3];
	unsigned int r, i;

	for (r = 0; r < 3; r++) {
		unsigned int row = reverse ? 2 - r : r;

		for (i = 0; i < 3; i++)
			c[r][i] = (u16)vdin_rgb_to_709[row][i] & 0x1fff;
		o[r] = offsets[row];
	}
	regs[0] = c[0][0] << 16 | c[0][1];
	regs[1] = c[0][2] << 16 | c[1][0];
	regs[2] = c[1][1] << 16 | c[1][2];
	regs[3] = c[2][0] << 16 | c[2][1];
	regs[4] = c[2][2];
	/* Post offsets in the 10-bit domain, no pre offsets. */
	regs[5] = o[0] << 16 | o[1];
	regs[6] = o[2];
	regs[7] = 0;
	regs[8] = 0;
}

static int vdin_scale(struct s7d_vdin *v, const struct drm_display_mode *mode,
		      const struct s7d_writeback_buffer *buffer)
{
	bool interlace = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 in_w = mode->hdisplay, in_h = mode->vdisplay >> interlace;
	u32 out_w = buffer->width, out_h = buffer->height >> interlace;
	u32 step;
	unsigned int i;
	int ret;

	ret = vdin_update(v, VD_SC_MISC_CTRL, VD_HSC_ENABLE_MASK, 0);
	if (!ret)
		ret = vdin_update(v, VD_VSC_INI_CTRL, VD_VSC_ENABLE, 0);
	if (ret)
		return ret;
	if (out_w != in_w) {
		writel(0x0100, v->vpu->regs + (VD_SCALE_COEF_IDX << 2));
		for (i = 0; i < ARRAY_SIZE(vdin_hsc_coef); i++)
			writel(vdin_hsc_coef[i], v->vpu->regs + (VD_SCALE_COEF << 2));
		step = in_w >> 12 ? (u32)(((u64)in_w << 18) / out_w) << 6 :
				    (u32)(((u64)in_w << 20) / out_w) << 4;
		ret = vdin_update(v, VD_HSC_PHASE_STEP, U32_MAX, step);
		if (!ret)
			ret = vdin_update(v, VD_HSC_INI_CTRL, U32_MAX, VD_HSC_INI_VALUE);
		if (!ret)
			ret = vdin_update(v, VD_SC_MISC_CTRL, VD_HSC_CONFIG_MASK,
					  VD_HSC_DOWNSCALE);
		if (ret)
			return ret;
	}
	if (out_h == in_h)
		return 0;
	ret = vdin_update(v, VD_VSC_PHASE_STEP, U32_MAX,
			  (u32)(((u64)in_h << 20) / out_h));
	if (!ret)
		ret = vdin_update(v, VD_SCIN_HEIGHTM1, 0x1fff, in_h - 1);
	if (!ret)
		ret = vdin_update(v, VD_DUMMY_DATA, U32_MAX, 0x008080);
	if (!ret)
		ret = vdin_update(v, VD_VSC_INI_CTRL, VD_VSC_CONFIG_MASK,
				  VD_VSC_ENABLE | VD_VSC_INI_PHASE);
	return ret;
}

static int vdin_configure(struct s7d_vdin *v, const struct drm_display_mode *mode,
			  const struct s7d_writeback_buffer *buffer)
{
	const struct vdin_format *format = vdin_find_format(buffer->format);
	bool interlace = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 lines = mode->vdisplay >> interlace;
	u32 wr_ctrl = (VD_WR_CTRL_RGB8 & ~VD_WR_CTRL_FORMAT_MASK) |
		      VD_WR_FMT(format->wr_fmt) | VD_WR_VCONV(format->vconv) |
		      (format->swap_cbcr ? VD_WR_SWAP_CBCR : 0);
	u32 matrix[9];
	unsigned int i;
	int ret;
	const struct vd_route config[] = {
		{ VD_COM_CTRL0, VD_COM_CONFIG_MASK, VD_COM_WINDOW_EN |
			(format->yuv ? VD_COM_IDENTITY_MAP : VD_COM_RGB_MANUAL_MAP) },
		{ VD_WIDTHM1I_WIDTHM1O, VD_WIDTH_MASK,
			VD_WIDTH_VALUE(mode->hdisplay, buffer->width) },
		{ VD_ASFIFO_CTRL2, VD_DECIMATION_MASK, 0 },
		{ VD_VSHRK_CTRL, U32_MAX, 0 },
		{ VD_MATRIX_CTRL, VD_MATRIX_DISABLE_MASK, 0 },
		{ VD_CM_BRI_CON_CTRL, VD_CM_TOP_ENABLE, 0 },
		{ VD_HDR2_CTRL, VD_HDR_DISABLE_MASK, 0 },
		{ VD_HDR2_MATRIXI_EN_CTRL, VD_HDR_MATRIX_ENABLE, 0 },
		{ VD_HDR2_MATRIXO_EN_CTRL, VD_HDR_MATRIX_ENABLE, 0 },
		{ VD_LFIFO_CTRL, VD_LFIFO_CONFIG_MASK, 0xc0020000 | VD_LFIFO_DEPTH },
		{ VD_INTF_WIDTHM1, VD_INTF_WIDTH_MASK, VD_INTF_WIDTH_VALUE(mode->hdisplay) },
		{ VD_WIN_H_START_END, VD_WIN_MASK, VD_WIN_H_VALUE(mode->hdisplay) },
		{ VD_WIN_V_START_END, VD_WIN_MASK, VD_WIN_V_VALUE(lines) },
		{ VD_ASFIFO_CTRL3, VD_ASFIFO_WB0_MASK,
			VD_ASFIFO_WB0_CONFIG & ~(VD_ASFIFO_DE_ENABLE | VD_ASFIFO_GO_ENABLE) },
		{ VD_WR_CTRL2, VD_WR_CTRL2_CONFIG_MASK,
			VD_WR_CTRL2_RGB8 |
			(buffer->format == DRM_FORMAT_XBGR2101010 ? BIT(19) : 0) },
		{ VD_WR_H_START_END, VD_WR_H_MASK, VD_WR_H_VALUE(buffer->width) },
		{ VD_WR_V_START_END, VD_WR_V_MASK, VD_WR_V_VALUE(buffer->height >> interlace) },
		{ VD_WR_CTRL, VD_WR_CTRL_CONFIG_MASK,
			wr_ctrl & ~VD_WR_DISABLE_CTRL_PULSE },
		{ VD_WR_CTRL, VD_WR_CTRL_CONFIG_MASK,
			wr_ctrl & ~VD_WR_DISABLE_CTRL_PULSE },
		{ VD_WR_CTRL, VD_WR_CTRL_CONFIG_MASK, wr_ctrl },
		{ VD_WR_BADDR_LUMA, U32_MAX, VD_WR_BADDR_VALUE(buffer->dma) },
		{ VD_WR_STRIDE_LUMA, U32_MAX, VD_WR_STRIDE_VALUE(buffer->pitch << interlace) },
		{ VD_WR_BADDR_CHROMA, U32_MAX, VD_WR_BADDR_VALUE(buffer->chroma_dma) },
		{ VD_WR_STRIDE_CHROMA, U32_MAX, VD_WR_STRIDE_VALUE(buffer->chroma_pitch) },
		{ VD_LINE_INT, VD_LINE_INT_ENABLE, 0 },
	};

	for (i = 0; i < ARRAY_SIZE(config); i++) {
		ret = vdin_update(v, config[i].reg, config[i].mask, config[i].value);
		if (ret)
			return ret;
	}
	ret = vdin_scale(v, mode, buffer);
	if (ret)
		return ret;
	if (format->yuv) {
		vdin_matrix(matrix, format->wr_fmt == 1);
		ret = vdin_update(v, VD_MATRIX_CTRL, VD_MATRIX_CONFIG_MASK, 1U << 2);
		for (i = 0; !ret && i < 5; i++)
			ret = vdin_update(v, VD_MATRIX_COEF00_01 + i, 0x1fff1fff, matrix[i]);
		if (!ret)
			ret = vdin_update(v, VD_MATRIX_OFFSET0_1, 0x07ff07ff, matrix[5]);
		if (!ret)
			ret = vdin_update(v, VD_MATRIX_OFFSET2, 0x07ff, matrix[6]);
		if (!ret)
			ret = vdin_update(v, VD_MATRIX_PRE_OFFSET0_1, 0x07ff07ff, matrix[7]);
		if (!ret)
			ret = vdin_update(v, VD_MATRIX_PRE_OFFSET2, 0x07ff, matrix[8]);
		if (!ret)
			ret = vdin_update(v, VD_MATRIX_CTRL, VD_MATRIX_CONFIG_MASK,
					  VD_MATRIX1_ENABLE);
		if (ret)
			return ret;
	}
	return vdin_route(v, mode->hdisplay);
}

static int vdin_clear_done(struct s7d_vdin *v)
{
	u32 value = vdin_read(v, VD_LINE_INT) & ~VD_WRITE_DONE_CLEAR;

	writel(value | VD_WRITE_DONE_CLEAR, v->vpu->regs + (VD_LINE_INT << 2));
	readl(v->vpu->regs + (VD_LINE_INT << 2));
	writel(value, v->vpu->regs + (VD_LINE_INT << 2));
	return vdin_read(v, VD_LINE_INT) & VD_WRITE_DONE_FLAG ? -EIO : 0;
}

static bool vdin_geometry_matches(struct s7d_vdin *v, const struct drm_display_mode *mode)
{
	u32 lines = mode->vdisplay >> !!(mode->flags & DRM_MODE_FLAG_INTERLACE);

	return (vdin_read(v, VD_ACTIVE_MAX_PIX) & VD_COUNTER_MASK) == mode->hdisplay - 1 &&
	       (vdin_read(v, VD_LCNT_SHADOW_STATUS) & VD_COUNTER_MASK) == lines;
}

/*
 * At each field start, credit the field that just ended to the parity it was
 * written for, then aim the next field at its lines of the woven frame.
 */
static int vdin_field(struct s7d_vdin *v)
{
	u32 line = (vdin_read(v, VD_ENCP_INFO_READ) >> 16) & VD_COUNTER_MASK;
	bool bottom = line >= v->mode.vtotal / 2;

	if (vdin_read(v, VD_LINE_INT) & VD_WRITE_DONE_FLAG) {
		/* The first done may close a field that began before arming. */
		if (v->events++ && v->field >= 0)
			v->fields |= BIT(v->field);
		if (vdin_clear_done(v))
			return -EIO;
	}
	if (v->fields == (BIT(0) | BIT(1)))
		return vdin_geometry_matches(v, &v->mode) ? 0 : -EIO;
	writel(VD_WR_BADDR_VALUE(v->base + (bottom ? v->pitch : 0)),
	       v->vpu->regs + (VD_WR_BADDR_LUMA << 2));
	v->field = bottom;
	return 1;
}

/* Called from the VPU VSYNC interrupt; only the armed capture is observed. */
void s7d_vdin_vsync(struct s7d_vdin *v)
{
	int status = 1;

	spin_lock(&v->irq_lock);
	if (!v->armed)
		goto unlock;
	if (vdin_read(v, VD_COM_STATUS2) & VD_WB0_FIFO_OVERFLOW) {
		status = -EOVERFLOW;
	} else if (v->interlace) {
		status = vdin_field(v);
	} else if (vdin_read(v, VD_LINE_INT) & VD_WRITE_DONE_FLAG) {
		if (++v->events >= VD_DONE_COUNT)
			status = vdin_geometry_matches(v, &v->mode) ? 0 : -EIO;
		else if (vdin_clear_done(v))
			status = -EIO;
	}
	if (status <= 0) {
		/* Stop requesting writes; the parked teardown drains the rest. */
		writel(vdin_read(v, VD_WR_CTRL) & ~VD_WR_REQUEST_EN,
		       v->vpu->regs + (VD_WR_CTRL << 2));
		v->status = status;
		v->armed = false;
		complete(&v->done);
	}
unlock:
	spin_unlock(&v->irq_lock);
}

static void vdin_disarm(struct s7d_vdin *v, int status)
{
	unsigned long flags;

	spin_lock_irqsave(&v->irq_lock, flags);
	if (v->armed) {
		v->status = status;
		v->armed = false;
		complete(&v->done);
	}
	spin_unlock_irqrestore(&v->irq_lock, flags);
}

static int vdin_wait(void *data)
{
	struct s7d_vdin *v = data;
	unsigned long timeout = nsecs_to_jiffies(8 * v->frame_ns) + 1;

	if (!wait_for_completion_timeout(&v->done, timeout))
		vdin_disarm(v, -ETIMEDOUT);
	return v->status;
}

static struct s7d_writeback_result vdin_capture(void *data,
		const struct s7d_crtc_state *state, const struct s7d_writeback_buffer *buffer)
{
	struct s7d_vdin *v = data;
	unsigned long flags;
	int ret, stop;

	if (v->active)
		return (struct s7d_writeback_result) { -EBUSY, false };
	ret = vdin_check(data, state, buffer);
	if (ret)
		return (struct s7d_writeback_result) { ret, true };
	if (!v->vpu->acquired || !v->vpu->pixel_enabled)
		return (struct s7d_writeback_result) { -EHOSTDOWN, true };
	if (vdin_foreign_writer(v) || !vdin_stopped(v))
		return (struct s7d_writeback_result) { -EBUSY, true };
	ret = vdin_frame_period(&state->base.adjusted_mode, state->output.rate.pixel,
				 &v->frame_ns);
	if (!ret)
		ret = vdin_reinitialize(v);
	if (ret)
		return (struct s7d_writeback_result) { ret, true };
	v->active = true;
	ret = vdin_configure(v, &state->base.adjusted_mode, buffer);
	if (ret)
		goto park;
	ret = vdin_clear_overflow(v);
	if (!ret)
		ret = vdin_clear_done(v);
	if (ret)
		goto park;
	dma_wmb();
	ret = vdin_update(v, VD_WR_CTRL, VD_WR_REQUEST_EN, VD_WR_REQUEST_EN);
	if (!ret)
		ret = vdin_update(v, VD_COM_CTRL0, VD_COM_INPUT_EN | VD_COM_MUX_MASK,
				  VD_COM_INPUT_EN | VD_COM_MUX_WB0);
	if (!ret) {
		spin_lock_irqsave(&v->irq_lock, flags);
		drm_mode_copy(&v->mode, &state->base.adjusted_mode);
		v->interlace = v->mode.flags & DRM_MODE_FLAG_INTERLACE;
		v->base = buffer->dma;
		v->pitch = buffer->pitch;
		v->fields = 0;
		v->field = -1;
		v->events = 0;
		v->status = -EINPROGRESS;
		reinit_completion(&v->done);
		v->armed = true;
		spin_unlock_irqrestore(&v->irq_lock, flags);
	}
	if (!ret)
		ret = vdin_update(v, VD_ASFIFO_CTRL3, VD_ASFIFO_DE_ENABLE | VD_ASFIFO_GO_ENABLE,
				  VD_ASFIFO_DE_ENABLE | VD_ASFIFO_GO_ENABLE);
	if (!ret)
		return (struct s7d_writeback_result) { 0, false };
park:
	vdin_disarm(v, ret);
	stop = vdin_park(v);
	if (ret || stop)
		dev_err(v->vpu->dev, "VDIN capture failed: capture %d park %d afifo %#x lfifo %#x shadow %#x\n",
			ret, stop, vdin_read(v, VD_COM_STATUS2),
			vdin_read(v, VD_COM_STATUS0), vdin_read(v, VD_LCNT_SHADOW_STATUS));
	if (stop)
		return (struct s7d_writeback_result) { stop, false };
	dma_rmb();
	return (struct s7d_writeback_result) { ret, true };
}

static struct s7d_writeback_result vdin_finish(void *data, int status)
{
	struct s7d_vdin *v = data;
	int stop;

	vdin_disarm(v, -ECANCELED);
	stop = vdin_park(v);
	if (status || stop)
		dev_err(v->vpu->dev, "VDIN capture failed: capture %d park %d afifo %#x lfifo %#x shadow %#x\n",
			status, stop, vdin_read(v, VD_COM_STATUS2),
			vdin_read(v, VD_COM_STATUS0), vdin_read(v, VD_LCNT_SHADOW_STATUS));
	if (stop)
		return (struct s7d_writeback_result) { stop, false };
	dma_rmb();
	return (struct s7d_writeback_result) { status, true };
}

static struct s7d_writeback_result vdin_cancel(void *data)
{
	struct s7d_vdin *v = data;
	int ret;

	vdin_disarm(v, -ECANCELED);
	ret = vdin_park(v);
	return (struct s7d_writeback_result) { ret, !v->active };
}

const struct s7d_writeback_ops s7d_vdin_writeback_ops = {
	.mode_valid = vdin_mode_valid,
	.check = vdin_check,
	.capture = vdin_capture,
	.wait = vdin_wait,
	.finish = vdin_finish,
	.cancel = vdin_cancel,
};

int s7d_vdin_init(struct s7d_vpu *vpu, struct s7d_vdin **result)
{
	struct s7d_vdin *v;
	int ret;

	if (!vpu || !result)
		return -EINVAL;
	*result = NULL;
	v = devm_kzalloc(vpu->dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->vpu = vpu;
	spin_lock_init(&v->irq_lock);
	init_completion(&v->done);
	v->resets[0].id = "vdin1";
	v->resets[1].id = "wrmif1";
	ret = devm_reset_control_bulk_get_exclusive(vpu->dev, ARRAY_SIZE(v->resets),
						   v->resets);
	if (ret)
		return ret;
	*result = v;
	return 0;
}
