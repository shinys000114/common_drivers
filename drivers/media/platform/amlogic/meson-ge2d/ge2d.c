// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 * Copyright (c) 2026 Hardkernel Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/regmap.h>

#include <linux/platform_device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include "ge2d-regs.h"

#define GE2D_NAME	"meson-ge2d"

#define DEFAULT_WIDTH	128
#define DEFAULT_HEIGHT	128
#define DEFAULT_STRIDE	512

#define MAX_WIDTH	8191
#define MAX_HEIGHT	8191

/*
 * Missing features:
 * - Scaling, YUV and colorspace conversion before the S7D
 * - Simple 1/2 vertical scaling
 * - Source global alpha
 */

struct ge2d_fmt {
	u32 fourcc;
	bool alpha;
	bool le;
	/* Bits per pixel of the first plane */
	unsigned int depth;
	unsigned int hw_fmt;
	unsigned int hw_map;
	/* YUV formats keep their chroma planes after the luma plane */
	bool yuv;
	u8 planes;
	u8 hsub;
	u8 vsub;
	/* Only on the S7D, or only as a source */
	bool s7d;
	bool src_only;
	/* S7D: swapped chroma planes, DST2 colour map */
	bool swap_uv;
	u8 dst2_map;
};

struct ge2d_frame {
	struct vb2_v4l2_buffer *buf;

	/* Image Format */
	struct v4l2_pix_format pix_fmt;

	/* Crop */
	struct v4l2_rect crop;

	/* Image format */
	const struct ge2d_fmt *fmt;
};

struct ge2d_ctx {
	struct v4l2_fh fh;
	struct meson_ge2d *ge2d;
	struct ge2d_frame in;
	struct ge2d_frame out;
	struct v4l2_ctrl_handler ctrl_handler;

	unsigned long sequence_out, sequence_cap;

	/* Control values */
	u32 hflip;
	u32 vflip;
	u32 xy_swap;
};

struct meson_ge2d_data {
	/* Byte offset of the register block in the resource. */
	unsigned int reg_offset;
	unsigned int max_register;
	/* S7D: C1 address registers, STATUS2 busy and GEN_CTRL4 setup. */
	bool c1;
};

struct meson_ge2d {
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct video_device *vfd;

	struct device *dev;
	struct regmap *map;
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *rst;
	const struct meson_ge2d_data *data;

	/* vb2 queue lock */
	struct mutex mutex;

	struct ge2d_ctx *curr;
	/* Finishes a job whose completion interrupt never came */
	struct delayed_work timeout;
};

#define GE2D_TIMEOUT_MS	1000

#define YUV(_fourcc, _depth, _hw, _map, _planes, _hsub, _vsub, ...)	\
{									\
	.fourcc = _fourcc,						\
	.depth = (_depth),						\
	.hw_fmt = GE2D_FORMAT_ ## _hw ## BIT,				\
	.hw_map = GE2D_COLOR_MAP_ ## _map,				\
	.yuv = true,							\
	.planes = (_planes),						\
	.hsub = (_hsub),						\
	.vsub = (_vsub),						\
	.s7d = true,							\
	__VA_ARGS__							\
}

#define FMT(_fourcc, _alpha, _depth, _map)		\
{							\
	.fourcc = _fourcc,				\
	.alpha = (_alpha),				\
	.depth = (_depth),				\
	.hw_fmt = GE2D_FORMAT_ ## _depth ## BIT,	\
	.hw_map = GE2D_COLOR_MAP_ ## _map,		\
	.planes = 1,					\
	.hsub = 1,					\
	.vsub = 1,					\
}

/* TOFIX Handle the YUV input formats */
static const struct ge2d_fmt formats[] = {
	/*  FOURCC Alpha  HW FMT  HW MAP */
	FMT(V4L2_PIX_FMT_XRGB32, false, 32, BGRA8888),
	FMT(V4L2_PIX_FMT_RGB32, true, 32, BGRA8888),
	FMT(V4L2_PIX_FMT_ARGB32, true, 32, BGRA8888),
	FMT(V4L2_PIX_FMT_RGBX32, false, 32, ABGR8888),
	FMT(V4L2_PIX_FMT_RGBA32, true, 32, ABGR8888),
	FMT(V4L2_PIX_FMT_BGRX32, false, 32, RGBA8888),
	FMT(V4L2_PIX_FMT_BGRA32, true, 32, RGBA8888),
	FMT(V4L2_PIX_FMT_BGR32, true, 32, ARGB8888),
	FMT(V4L2_PIX_FMT_ABGR32, true, 32, ARGB8888),
	FMT(V4L2_PIX_FMT_XBGR32, false, 32, ARGB8888),

	FMT(V4L2_PIX_FMT_RGB24, false, 24, BGR888),
	FMT(V4L2_PIX_FMT_BGR24, false, 24, RGB888),

	FMT(V4L2_PIX_FMT_XRGB555X, false, 16, ARGB1555),
	FMT(V4L2_PIX_FMT_ARGB555X, true, 16, ARGB1555),
	FMT(V4L2_PIX_FMT_RGB565, false, 16, RGB565),
	FMT(V4L2_PIX_FMT_RGBX444, false, 16, RGBA4444),
	FMT(V4L2_PIX_FMT_RGBA444, true, 16, RGBA4444),
	FMT(V4L2_PIX_FMT_XRGB444, false, 16, ARGB4444),
	FMT(V4L2_PIX_FMT_ARGB444, true, 16, ARGB4444),

	/*
	 * S7D YCbCr: the source formatter rebuilds 4:4:4 from the planes.
	 * Packed 4:2:2 reads as UYVY; the big-endian mode swaps pixel pairs,
	 * so YUYV cannot be read.
	 */
	YUV(V4L2_PIX_FMT_NV12, 8, 24, NV12, 2, 2, 2, .dst2_map = GE2D_DST2_COLOR_MAP_NV12),
	YUV(V4L2_PIX_FMT_NV21, 8, 24, NV21, 2, 2, 2, .dst2_map = GE2D_DST2_COLOR_MAP_NV21),
	YUV(V4L2_PIX_FMT_NV16, 8, 24, NV12, 2, 2, 1, .dst2_map = GE2D_DST2_COLOR_MAP_NV12),
	YUV(V4L2_PIX_FMT_NV61, 8, 24, NV21, 2, 2, 1, .dst2_map = GE2D_DST2_COLOR_MAP_NV21),
	YUV(V4L2_PIX_FMT_YUV420, 8, 8, YUV422, 3, 2, 2, .src_only = true),
	YUV(V4L2_PIX_FMT_YVU420, 8, 8, YUV422, 3, 2, 2, .src_only = true, .swap_uv = true),
	YUV(V4L2_PIX_FMT_YUV422P, 8, 8, YUV422, 3, 2, 1, .src_only = true),
	YUV(V4L2_PIX_FMT_UYVY, 16, 16, YUV422, 1, 2, 1, .src_only = true),
	YUV(V4L2_PIX_FMT_YUV24, 24, 24, VUY888, 1, 1, 1),
};

#define NUM_FORMATS ARRAY_SIZE(formats)

static bool fmt_supported(struct ge2d_ctx *ctx, const struct ge2d_fmt *fmt,
			  enum v4l2_buf_type type)
{
	if (fmt->s7d && !ctx->ge2d->data->c1)
		return false;
	return !(fmt->src_only && type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
}

static const struct ge2d_fmt *find_fmt(struct ge2d_ctx *ctx, struct v4l2_format *f)
{
	unsigned int i;

	for (i = 0; i < NUM_FORMATS; i++) {
		if (formats[i].fourcc == f->fmt.pix.pixelformat &&
		    fmt_supported(ctx, &formats[i], f->type))
			return &formats[i];
	}

	/*
	 * TRY_FMT/S_FMT should never return an error when the requested format
	 * is not supported. Drivers should always return a valid format,
	 * preferably a format that is as widely supported by applications as
	 * possible.
	 */
	return &formats[0];
}

static struct ge2d_frame *get_frame(struct ge2d_ctx *ctx,
				    enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->in;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->out;
	default:
		/* This should never happen, warn and return OUTPUT frame */
		dev_warn(ctx->ge2d->dev, "%s: invalid buffer type\n", __func__);
		return &ctx->in;
	}
}

static void ge2d_set_addr(struct meson_ge2d *ge2d, unsigned int baddr,
			  unsigned int stride, struct ge2d_frame *frame)
{
	regmap_write(ge2d->map, baddr,
		     (vb2_dma_contig_plane_dma_addr(&frame->buf->vb2_buf, 0) + 7) >> 3);
	regmap_write(ge2d->map, stride, (frame->pix_fmt.bytesperline + 7) >> 3);
}

static void ge2d_hw_start(struct meson_ge2d *ge2d)
{
	struct ge2d_ctx *ctx = ge2d->curr;
	u32 reg;

	/* Reset */
	regmap_update_bits(ge2d->map, GE2D_GEN_CTRL1,
			   GE2D_SOFT_RST, GE2D_SOFT_RST);
	regmap_update_bits(ge2d->map, GE2D_GEN_CTRL1,
			   GE2D_SOFT_RST, 0);

	usleep_range(100, 200);

	/* Implement CANVAS for non-AXG */
	ge2d_set_addr(ge2d, GE2D_SRC1_BADDR_CTRL, GE2D_SRC1_STRIDE_CTRL, &ctx->in);
	ge2d_set_addr(ge2d, GE2D_SRC2_BADDR_CTRL, GE2D_SRC2_STRIDE_CTRL, &ctx->out);
	ge2d_set_addr(ge2d, GE2D_DST1_BADDR_CTRL, GE2D_DST1_STRIDE_CTRL, &ctx->out);

	regmap_write(ge2d->map, GE2D_GEN_CTRL0, 0);
	regmap_write(ge2d->map, GE2D_GEN_CTRL1,
		     FIELD_PREP(GE2D_INTERRUPT_CTRL, 2) |
		     FIELD_PREP(GE2D_SRC2_BURST_SIZE_CTRL, 3) |
		     FIELD_PREP(GE2D_SRC1_BURST_SIZE_CTRL, 0x3f));

	regmap_write(ge2d->map, GE2D_GEN_CTRL2,
		     GE2D_SRC1_LITTLE_ENDIAN |
		     GE2D_SRC2_LITTLE_ENDIAN |
		     GE2D_DST_LITTLE_ENDIAN |
		     FIELD_PREP(GE2D_DST1_COLOR_MAP, ctx->out.fmt->hw_map) |
		     FIELD_PREP(GE2D_DST1_FORMAT, ctx->out.fmt->hw_fmt) |
		     FIELD_PREP(GE2D_SRC2_COLOR_MAP, ctx->out.fmt->hw_map) |
		     FIELD_PREP(GE2D_SRC2_FORMAT, ctx->out.fmt->hw_fmt) |
		     FIELD_PREP(GE2D_SRC1_COLOR_MAP, ctx->in.fmt->hw_map) |
		     FIELD_PREP(GE2D_SRC1_FORMAT, ctx->in.fmt->hw_fmt));
	regmap_write(ge2d->map, GE2D_GEN_CTRL3,
		     GE2D_DST1_ENABLE);

	regmap_write(ge2d->map, GE2D_SRC1_CLIPY_START_END,
		     FIELD_PREP(GE2D_START, ctx->in.crop.top) |
		     FIELD_PREP(GE2D_END, ctx->in.crop.top + ctx->in.crop.height - 1));
	regmap_write(ge2d->map, GE2D_SRC1_CLIPX_START_END,
		     FIELD_PREP(GE2D_START, ctx->in.crop.left) |
		     FIELD_PREP(GE2D_END, ctx->in.crop.left + ctx->in.crop.width - 1));
	regmap_write(ge2d->map, GE2D_SRC2_CLIPY_START_END,
		     FIELD_PREP(GE2D_START, ctx->out.crop.top) |
		     FIELD_PREP(GE2D_END, ctx->out.crop.top + ctx->out.crop.height - 1));
	regmap_write(ge2d->map, GE2D_SRC2_CLIPX_START_END,
		     FIELD_PREP(GE2D_START, ctx->out.crop.left) |
		     FIELD_PREP(GE2D_END, ctx->out.crop.left + ctx->out.crop.width - 1));
	regmap_write(ge2d->map, GE2D_DST_CLIPY_START_END,
		     FIELD_PREP(GE2D_START, ctx->out.crop.top) |
		     FIELD_PREP(GE2D_END, ctx->out.crop.top + ctx->out.crop.height - 1));
	regmap_write(ge2d->map, GE2D_DST_CLIPX_START_END,
		     FIELD_PREP(GE2D_START, ctx->out.crop.left) |
		     FIELD_PREP(GE2D_END, ctx->out.crop.left + ctx->out.crop.width - 1));

	regmap_write(ge2d->map, GE2D_SRC1_Y_START_END,
		     FIELD_PREP(GE2D_END, ctx->in.pix_fmt.height - 1));
	regmap_write(ge2d->map, GE2D_SRC1_X_START_END,
		     FIELD_PREP(GE2D_END, ctx->in.pix_fmt.width - 1));
	regmap_write(ge2d->map, GE2D_SRC2_Y_START_END,
		     FIELD_PREP(GE2D_END, ctx->out.pix_fmt.height - 1));
	regmap_write(ge2d->map, GE2D_SRC2_X_START_END,
		     FIELD_PREP(GE2D_END, ctx->out.pix_fmt.width - 1));
	regmap_write(ge2d->map, GE2D_DST_Y_START_END,
		     FIELD_PREP(GE2D_END, ctx->out.pix_fmt.height - 1));
	regmap_write(ge2d->map, GE2D_DST_X_START_END,
		     FIELD_PREP(GE2D_END, ctx->out.pix_fmt.width - 1));

	/* Color, no blend, use source color */
	reg = GE2D_ALU_DO_COLOR_OPERATION_LOGIC(LOGIC_OPERATION_COPY,
						COLOR_FACTOR_SRC_COLOR);

	if (ctx->in.fmt->alpha && ctx->out.fmt->alpha)
		/* Take source alpha */
		reg |= GE2D_ALU_DO_ALPHA_OPERATION_LOGIC(LOGIC_OPERATION_COPY,
							 COLOR_FACTOR_SRC_ALPHA);
	else if (!ctx->out.fmt->alpha)
		/* Set alpha to 0 */
		reg |= GE2D_ALU_DO_ALPHA_OPERATION_LOGIC(LOGIC_OPERATION_SET,
							 COLOR_FACTOR_ZERO);
	else
		/* Keep original alpha */
		reg |= GE2D_ALU_DO_ALPHA_OPERATION_LOGIC(LOGIC_OPERATION_COPY,
							 COLOR_FACTOR_DST_ALPHA);

	regmap_write(ge2d->map, GE2D_ALU_OP_CTRL, reg);

	/* Start */
	regmap_write(ge2d->map, GE2D_CMD_CTRL,
		     (ctx->xy_swap ? GE2D_DST_XY_SWAP : 0) |
		     (ctx->hflip ? GE2D_SRC1_Y_REV : 0) |
		     (ctx->vflip ? GE2D_SRC1_X_REV : 0) |
		     GE2D_CBUS_CMD_WR);
}

static void ge2d_job_done(struct meson_ge2d *ge2d, struct ge2d_ctx *ctx,
			  enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src, *dst;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	src->sequence = ctx->sequence_out++;
	dst->sequence = ctx->sequence_cap++;

	dst->timecode = src->timecode;
	dst->vb2_buf.timestamp = src->vb2_buf.timestamp;
	dst->flags = src->flags;

	v4l2_m2m_buf_done(src, state);
	v4l2_m2m_buf_done(dst, state);
	v4l2_m2m_job_finish(ge2d->m2m_dev, ctx->fh.m2m_ctx);
}

/* Matrix rows give Y/R, Cb/G, Cr/B from Y/R, Cb/G, Cr/B; 0x400 is 1.0. */
struct ge2d_csc {
	u32 pre_offset;
	u32 coef00_01;
	u32 coef02_10;
	u32 coef11_12;
	u32 coef20_21;
	u32 coef22_ctrl;
	u32 offset;
};

/* BT.601 and BT.709, limited and full range, indexed by ge2d_csc_index() */
static const struct ge2d_csc ge2d_yuv2rgb[] = {
	{ 0x1f060180, 0x04a80000, 0x066204a8, 0x1e6f1cbf, 0x04a80811, 0x00000081, 0 },
	{ 0x1f060180, 0x04a80000, 0x072c04a8, 0x1f261dde, 0x04a80873, 0x00000081, 0 },
	{ 0x00060180, 0x04000000, 0x059c0400, 0x1ea01d25, 0x04000717, 0x00000001, 0 },
	{ 0x00060180, 0x04000000, 0x064d0400, 0x1f401e21, 0x0400076c, 0x00000001, 0 },
};

static const struct ge2d_csc ge2d_rgb2yuv[] = {
	{ 0, 0x01070204, 0x00641f68, 0x1ed601c2, 0x01c21e87, 0x1fb70001, 0x01020080 },
	{ 0, 0x00bb0275, 0x003f1f99, 0x1ea601c2, 0x01c21e67, 0x1fd70001, 0x01020080 },
	{ 0, 0x01320259, 0x00751f53, 0x1ead0200, 0x02001e53, 0x1fad0001, 0x00020080 },
	{ 0, 0x00da02dc, 0x004a1f8b, 0x1e750200, 0x02001e2f, 0x1fd10001, 0x00020080 },
};

static unsigned int ge2d_csc_index(const struct v4l2_pix_format *pix)
{
	enum v4l2_ycbcr_encoding enc = pix->ycbcr_enc;
	enum v4l2_quantization quant = pix->quantization;

	if (enc == V4L2_YCBCR_ENC_DEFAULT)
		enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix->colorspace);
	if (quant == V4L2_QUANTIZATION_DEFAULT)
		quant = V4L2_MAP_QUANTIZATION_DEFAULT(false, pix->colorspace, enc);

	return (enc == V4L2_YCBCR_ENC_709 ? 1 : 0) +
	       (quant == V4L2_QUANTIZATION_FULL_RANGE ? 2 : 0);
}

static void ge2d_set_csc(struct meson_ge2d *ge2d, struct ge2d_ctx *ctx)
{
	const struct ge2d_csc *csc = NULL;

	if (ctx->in.fmt->yuv && !ctx->out.fmt->yuv)
		csc = &ge2d_yuv2rgb[ge2d_csc_index(&ctx->in.pix_fmt)];
	else if (!ctx->in.fmt->yuv && ctx->out.fmt->yuv)
		csc = &ge2d_rgb2yuv[ge2d_csc_index(&ctx->out.pix_fmt)];

	if (!csc) {
		regmap_write(ge2d->map, GE2D_MATRIX_COEF22_CTRL, 0);
		return;
	}
	regmap_write(ge2d->map, GE2D_MATRIX_PRE_OFFSET, csc->pre_offset);
	regmap_write(ge2d->map, GE2D_MATRIX_COEF00_01, csc->coef00_01);
	regmap_write(ge2d->map, GE2D_MATRIX_COEF02_10, csc->coef02_10);
	regmap_write(ge2d->map, GE2D_MATRIX_COEF11_12, csc->coef11_12);
	regmap_write(ge2d->map, GE2D_MATRIX_COEF20_21, csc->coef20_21);
	regmap_write(ge2d->map, GE2D_MATRIX_OFFSET, csc->offset);
	regmap_write(ge2d->map, GE2D_MATRIX_COEF22_CTRL, csc->coef22_ctrl);
}

/* DMA address and stride of plane n of a contiguous multi-plane buffer */
static void ge2d_plane(struct ge2d_frame *f, unsigned int n, dma_addr_t *addr,
		       u32 *stride)
{
	const struct ge2d_fmt *fmt = f->fmt;
	u32 bpl = f->pix_fmt.bytesperline, h = f->pix_fmt.height;
	u32 cstride = fmt->planes == 3 ? bpl / fmt->hsub : bpl;
	u32 csize = cstride * (h / fmt->vsub);

	*addr = vb2_dma_contig_plane_dma_addr(&f->buf->vb2_buf, 0);
	*stride = n ? cstride : bpl;
	if (n)
		*addr += bpl * h + (n - 1) * csize;
}

static void ge2d_write_plane(struct meson_ge2d *ge2d, unsigned int baddr,
			     unsigned int stride, struct ge2d_frame *f,
			     unsigned int n)
{
	dma_addr_t addr;
	u32 bytes;

	ge2d_plane(f, n, &addr, &bytes);
	regmap_write(ge2d->map, baddr, addr >> 3);
	regmap_write(ge2d->map, stride, bytes >> 3);
}

/* Bilinear taps for 33 phases; each word holds four taps summing to 128. */
static void ge2d_write_coefs(struct meson_ge2d *ge2d)
{
	unsigned int bank, i;

	for (bank = 0; bank < 2; bank++) {
		regmap_write(ge2d->map, GE2D_SCALE_COEF_IDX, bank << 8);
		for (i = 0; i <= 32; i++)
			regmap_write(ge2d->map, GE2D_SCALE_COEF,
				     (0x80 - 2 * i) << 16 | (2 * i) << 8);
	}
}

static u32 ge2d_ini_phase(unsigned int in, unsigned int out, u32 *rpt)
{
	unsigned int rate = out * 10 / in;

	if (rate == 10) {
		*rpt = 1;
		return 0;
	}
	if (rate < 10) {
		*rpt = 1;
		return rate ? 0x5000000 / rate - 0x800000 : 0x5000000;
	}
	*rpt = 2;
	return 0x800000 + 0x5000000 / rate;
}

/*
 * Scale the source crop to the destination compose rectangle. The steps are
 * 5.24 fixed point; sources more than twice the output are halved first.
 */
static bool ge2d_set_scaler(struct meson_ge2d *ge2d, struct ge2d_ctx *ctx)
{
	unsigned int win = ctx->in.crop.width, hin = ctx->in.crop.height;
	unsigned int wout = ctx->xy_swap ? ctx->out.crop.height : ctx->out.crop.width;
	unsigned int hout = ctx->xy_swap ? ctx->out.crop.width : ctx->out.crop.height;
	bool pre_h = win > wout * 2, pre_v = hin > hout * 2;
	u32 hstep, vstep, div, multo, hrpt, vrpt, hini, vini, misc;

	if (win == wout && hin == hout) {
		/* The filter banks keep their length even without scaling. */
		regmap_write(ge2d->map, GE2D_SC_MISC_CTRL,
			     FIELD_PREP(GE2D_VSC_BANK_LENGTH, 4) |
			     FIELD_PREP(GE2D_HSC_BANK_LENGTH, 4));
		regmap_write(ge2d->map, GE2D_HSC_START_PHASE_STEP, 0);
		regmap_write(ge2d->map, GE2D_HSC_PHASE_SLOPE, 0);
		regmap_write(ge2d->map, GE2D_HSC_ADV_CTRL, 0);
		regmap_write(ge2d->map, GE2D_HSC_INI_CTRL, 0);
		regmap_write(ge2d->map, GE2D_VSC_START_PHASE_STEP, 0);
		regmap_write(ge2d->map, GE2D_VSC_PHASE_SLOPE, 0);
		regmap_write(ge2d->map, GE2D_VSC_INI_CTRL, 0);
		return false;
	}

	hstep = (((pre_h ? (win + 1) >> 1 : win) << 18) / wout) << 6;
	vstep = (((pre_v ? (hin + 1) >> 1 : hin) << 18) / hout) << 6;
	div = (124 << 24) / hstep;
	multo = hstep * div;
	hini = ge2d_ini_phase(win, wout, &hrpt);
	vini = ge2d_ini_phase(hin, hout, &vrpt);

	misc = GE2D_HSC_DIV_EN | FIELD_PREP(GE2D_HSC_DIV_LENGTH, div) |
	       (pre_h ? GE2D_PRE_HSC_EN : 0) | (pre_v ? GE2D_PRE_VSC_EN : 0) |
	       GE2D_VSC_EN | GE2D_HSC_EN |
	       FIELD_PREP(GE2D_VSC_BANK_LENGTH, 4) |
	       FIELD_PREP(GE2D_HSC_BANK_LENGTH, 4);
	/* The vendor repeats edge pixels only when both axes are scaled. */
	if (win != wout && hin != hout)
		misc |= GE2D_HSC_RPT_CTRL | GE2D_VSC_RPT_CTRL;
	regmap_write(ge2d->map, GE2D_SC_MISC_CTRL, misc);

	regmap_write(ge2d->map, GE2D_HSC_START_PHASE_STEP, hstep);
	regmap_write(ge2d->map, GE2D_HSC_PHASE_SLOPE, 0);
	regmap_write(ge2d->map, GE2D_HSC_ADV_CTRL,
		     (multo >> 24) << 24 | (multo & 0xffffff));
	regmap_write(ge2d->map, GE2D_HSC_INI_CTRL,
		     FIELD_PREP(GE2D_SC_RPT_NUM, hrpt) | (hini & GE2D_SC_INI_PHASE));
	regmap_write(ge2d->map, GE2D_VSC_START_PHASE_STEP, vstep);
	regmap_write(ge2d->map, GE2D_VSC_PHASE_SLOPE, 0);
	regmap_write(ge2d->map, GE2D_VSC_INI_CTRL,
		     FIELD_PREP(GE2D_SC_RPT_NUM, vrpt) | (vini & GE2D_SC_INI_PHASE));
	ge2d_write_coefs(ge2d);

	return true;
}

static u32 ge2d_alu(struct ge2d_ctx *ctx)
{
	/* Color, no blend, use source color */
	u32 reg = GE2D_ALU_DO_COLOR_OPERATION_LOGIC(LOGIC_OPERATION_COPY,
						    COLOR_FACTOR_SRC_COLOR);

	if (ctx->in.fmt->alpha && ctx->out.fmt->alpha)
		/* Take source alpha */
		reg |= GE2D_ALU_DO_ALPHA_OPERATION_LOGIC(LOGIC_OPERATION_COPY,
							 COLOR_FACTOR_SRC_ALPHA);
	else if (!ctx->out.fmt->alpha)
		/* Set alpha to 0 */
		reg |= GE2D_ALU_DO_ALPHA_OPERATION_LOGIC(LOGIC_OPERATION_SET,
							 COLOR_FACTOR_ZERO);
	else
		/* Keep original alpha */
		reg |= GE2D_ALU_DO_ALPHA_OPERATION_LOGIC(LOGIC_OPERATION_COPY,
							 COLOR_FACTOR_DST_ALPHA);
	return reg;
}

static u32 ge2d_window(const struct v4l2_rect *r, bool y)
{
	u32 start = y ? r->top : r->left;
	u32 size = y ? r->height : r->width;

	return FIELD_PREP(GE2D_START, start) | FIELD_PREP(GE2D_END, start + size - 1);
}

/*
 * The S7D programs the source window as the crop and the destination window
 * as the compose rectangle, which sets the scaling ratio. Subsampled chroma
 * is rebuilt by the source formatter, then converted by the matrix.
 */
/* The phase steps keep 5 integer bits, after the optional halving. */
#define GE2D_MAX_SCALE	16

static bool ge2d_scale_ok(struct ge2d_ctx *ctx)
{
	unsigned int win = ctx->in.crop.width, hin = ctx->in.crop.height;
	unsigned int wout = ctx->xy_swap ? ctx->out.crop.height : ctx->out.crop.width;
	unsigned int hout = ctx->xy_swap ? ctx->out.crop.width : ctx->out.crop.height;

	return win <= wout * GE2D_MAX_SCALE && wout <= win * GE2D_MAX_SCALE &&
	       hin <= hout * GE2D_MAX_SCALE && hout <= hin * GE2D_MAX_SCALE;
}

static void ge2d_hw_start_s7d(struct meson_ge2d *ge2d)
{
	struct ge2d_ctx *ctx = ge2d->curr;
	const struct ge2d_fmt *fi = ctx->in.fmt, *fo = ctx->out.fmt;
	bool xs = fi->hsub > 1, ys = fi->vsub > 1, flip_x, flip_y, scaled;
	u32 gen3 = GE2D_DST1_ENABLE, xwin, ywin, xph = 0, yph = 0;
	u32 xex_s = 0, xex_e = 0, yex_s = 0, yex_e = 0;

	regmap_update_bits(ge2d->map, GE2D_GEN_CTRL1, GE2D_SOFT_RST, GE2D_SOFT_RST);
	regmap_update_bits(ge2d->map, GE2D_GEN_CTRL1, GE2D_SOFT_RST, 0);
	usleep_range(100, 200);

	ge2d_write_plane(ge2d, GE2D_C1_SRC1_BADDR_CTRL, GE2D_C1_SRC1_STRIDE_CTRL,
			 &ctx->in, 0);
	if (fi->planes > 1)
		ge2d_write_plane(ge2d, GE2D_C1_SRC1_CB_BADDR_CTRL,
				 GE2D_C1_SRC1_CB_STRIDE_CTRL, &ctx->in,
				 fi->swap_uv ? 2 : 1);
	if (fi->planes > 2)
		ge2d_write_plane(ge2d, GE2D_C1_SRC1_CR_BADDR_CTRL,
				 GE2D_C1_SRC1_CR_STRIDE_CTRL, &ctx->in,
				 fi->swap_uv ? 1 : 2);
	ge2d_write_plane(ge2d, GE2D_C1_SRC2_BADDR_CTRL, GE2D_C1_SRC2_STRIDE_CTRL,
			 &ctx->out, 0);
	ge2d_write_plane(ge2d, GE2D_C1_DST1_BADDR_CTRL, GE2D_C1_DST1_STRIDE_CTRL,
			 &ctx->out, 0);
	if (fo->planes == 2) {
		ge2d_write_plane(ge2d, GE2D_C1_DST2_BADDR_CTRL,
				 GE2D_C1_DST2_STRIDE_CTRL, &ctx->out, 1);
		gen3 |= GE2D_DST2_ENABLE |
			FIELD_PREP(GE2D_DST2_FORMAT, GE2D_FORMAT_16BIT) |
			FIELD_PREP(GE2D_DST2_COLOR_MAP, fo->dst2_map) |
			FIELD_PREP(GE2D_DST2_X_DISCARD_MODE, fo->hsub > 1 ? 3 : 0) |
			FIELD_PREP(GE2D_DST2_Y_DISCARD_MODE, fo->vsub > 1 ? 3 : 0);
	}

	regmap_write(ge2d->map, GE2D_GEN_CTRL0,
		     (fi->planes > 1 ? GE2D_SRC1_SEP_EN_S7D : 0) |
		     (xs ? GE2D_SRC1_X_YC_RATIO_S7D : 0) |
		     (ys ? GE2D_SRC1_Y_YC_RATIO_S7D : 0));
	regmap_write(ge2d->map, GE2D_GEN_CTRL1,
		     FIELD_PREP(GE2D_INTERRUPT_CTRL, 2) |
		     FIELD_PREP(GE2D_SRC2_BURST_SIZE_CTRL, 3) |
		     FIELD_PREP(GE2D_SRC1_BURST_SIZE_CTRL, 0x3f));

	scaled = ge2d_set_scaler(ge2d, ctx);
	/* A two-plane destination writes its luma plane as 8-bit Y. */
	regmap_write(ge2d->map, GE2D_GEN_CTRL2,
		     (scaled ? GE2D_SRC2_COLOR_EXPAND_MODE |
			       GE2D_SRC1_COLOR_EXPAND_MODE : 0) |
		     GE2D_SRC1_LITTLE_ENDIAN | GE2D_SRC2_LITTLE_ENDIAN |
		     GE2D_DST_LITTLE_ENDIAN |
		     FIELD_PREP(GE2D_DST1_COLOR_MAP, fo->planes == 2 ? 0 : fo->hw_map) |
		     FIELD_PREP(GE2D_DST1_FORMAT, fo->planes == 2 ? 0 : fo->hw_fmt) |
		     FIELD_PREP(GE2D_SRC2_COLOR_MAP, fo->planes == 2 ? 0 : fo->hw_map) |
		     FIELD_PREP(GE2D_SRC2_FORMAT, fo->planes == 2 ? 0 : fo->hw_fmt) |
		     FIELD_PREP(GE2D_SRC1_COLOR_MAP, fi->hw_map) |
		     FIELD_PREP(GE2D_SRC1_FORMAT, fi->hw_fmt));
	regmap_write(ge2d->map, GE2D_GEN_CTRL3, gen3);
	/* 64-byte bursts without a canvas, and recover from a bus hang. */
	regmap_write(ge2d->map, GE2D_GEN_CTRL4, GE2D_BYTES_PER_BURST | GE2D_HANG_FLAG);

	ge2d_set_csc(ge2d, ctx);

	/* Chroma siting: co-sited horizontally, centred vertically. */
	flip_x = ctx->hflip;
	flip_y = ctx->vflip;
	if (xs) {
		xex_s = flip_x ? 3 : 2;
		xex_e = flip_x ? 2 : 3;
		xph = flip_x ? 0x0c : 0x08;
	}
	if (ys) {
		yex_s = flip_y ? 3 : 2;
		yex_e = flip_y ? 2 : 3;
		yph = 0x4c;
	}
	regmap_write(ge2d->map, GE2D_SRC1_FMT_CTRL,
		     (xs ? GE2D_SRC1_X_FORMATTER_EN : 0) |
		     (ys ? GE2D_SRC1_Y_FORMATTER_EN : 0) |
		     FIELD_PREP(GE2D_SRC1_X_CHR_PHASE, xph) |
		     FIELD_PREP(GE2D_SRC1_Y_CHR_PHASE, yph));

	xwin = ge2d_window(&ctx->in.crop, false);
	ywin = ge2d_window(&ctx->in.crop, true);
	regmap_write(ge2d->map, GE2D_SRC1_CLIPX_START_END, xwin);
	regmap_write(ge2d->map, GE2D_SRC1_CLIPY_START_END, ywin);
	regmap_write(ge2d->map, GE2D_SRC1_X_START_END,
		     xex_s << 30 | xwin | xex_e << 14);
	/* The vendor reads one more source line for the vertical filter. */
	regmap_write(ge2d->map, GE2D_SRC1_Y_START_END,
		     yex_s << 30 | (ywin + (scaled ? 1 : 0)) | yex_e << 14);

	xwin = ge2d_window(&ctx->out.crop, false);
	ywin = ge2d_window(&ctx->out.crop, true);
	regmap_write(ge2d->map, GE2D_SRC2_CLIPX_START_END, xwin);
	regmap_write(ge2d->map, GE2D_SRC2_CLIPY_START_END, ywin);
	regmap_write(ge2d->map, GE2D_SRC2_X_START_END, xwin);
	regmap_write(ge2d->map, GE2D_SRC2_Y_START_END, ywin);
	regmap_write(ge2d->map, GE2D_DST_CLIPX_START_END, xwin);
	regmap_write(ge2d->map, GE2D_DST_CLIPY_START_END, ywin);
	regmap_write(ge2d->map, GE2D_DST_X_START_END, xwin);
	regmap_write(ge2d->map, GE2D_DST_Y_START_END, ywin);

	regmap_write(ge2d->map, GE2D_ALU_OP_CTRL, ge2d_alu(ctx));

	/* Start. On the S7D X_REV mirrors columns and Y_REV mirrors rows. */
	regmap_write(ge2d->map, GE2D_CMD_CTRL,
		     (ctx->xy_swap ? GE2D_DST_XY_SWAP : 0) |
		     (ctx->hflip ? GE2D_SRC1_X_REV : 0) |
		     (ctx->vflip ? GE2D_SRC1_Y_REV : 0) |
		     GE2D_CBUS_CMD_WR);
}

static void device_run(void *priv)
{
	struct ge2d_ctx *ctx = priv;
	struct meson_ge2d *ge2d = ctx->ge2d;

	ge2d->curr = ctx;

	ctx->in.buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	ctx->out.buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	if (ge2d->data->c1 && !ge2d_scale_ok(ctx)) {
		ge2d->curr = NULL;
		ge2d_job_done(ge2d, ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	schedule_delayed_work(&ge2d->timeout, msecs_to_jiffies(GE2D_TIMEOUT_MS));
	if (ge2d->data->c1)
		ge2d_hw_start_s7d(ge2d);
	else
		ge2d_hw_start(ge2d);
}

static bool ge2d_busy(struct meson_ge2d *ge2d)
{
	u32 val;

	if (ge2d->data->c1) {
		regmap_read(ge2d->map, GE2D_STATUS2, &val);
		return val & GE2D_STATUS2_BUSY;
	}
	regmap_read(ge2d->map, GE2D_STATUS0, &val);
	return val & GE2D_GE2D_BUSY;
}

static void ge2d_timeout(struct work_struct *work)
{
	struct meson_ge2d *ge2d = container_of(work, struct meson_ge2d,
					       timeout.work);
	struct ge2d_ctx *ctx;

	/* The interrupt can come while STATUS2 still reports busy. */
	if (!ge2d_busy(ge2d)) {
		ctx = xchg(&ge2d->curr, NULL);
		if (ctx)
			ge2d_job_done(ge2d, ctx, VB2_BUF_STATE_DONE);
		return;
	}

	ctx = xchg(&ge2d->curr, NULL);
	if (!ctx)
		return;

	dev_err(ge2d->dev, "job timed out, resetting\n");
	regmap_update_bits(ge2d->map, GE2D_GEN_CTRL1, GE2D_SOFT_RST, GE2D_SOFT_RST);
	regmap_update_bits(ge2d->map, GE2D_GEN_CTRL1, GE2D_SOFT_RST, 0);
	ge2d_job_done(ge2d, ctx, VB2_BUF_STATE_ERROR);
}

static irqreturn_t ge2d_isr(int irq, void *priv)
{
	struct meson_ge2d *ge2d = priv;
	struct ge2d_ctx *ctx;
	u32 val;

	/* On the S7D, STATUS2 may still show busy right after the interrupt. */
	if (ge2d->data->c1)
		regmap_read_poll_timeout_atomic(ge2d->map, GE2D_STATUS2, val,
						!(val & GE2D_STATUS2_BUSY), 2, 100);

	if (!ge2d_busy(ge2d)) {
		ctx = xchg(&ge2d->curr, NULL);
		if (ctx) {
			cancel_delayed_work(&ge2d->timeout);
			ge2d_job_done(ge2d, ctx, VB2_BUF_STATE_DONE);
		}
	}

	return IRQ_HANDLED;
}

static const struct v4l2_m2m_ops ge2d_m2m_ops = {
	.device_run = device_run,
};

static int ge2d_queue_setup(struct vb2_queue *vq,
			    unsigned int *nbuffers, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct ge2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct ge2d_frame *f = get_frame(ctx, vq->type);

	if (*nplanes)
		return sizes[0] < f->pix_fmt.sizeimage ? -EINVAL : 0;

	sizes[0] = f->pix_fmt.sizeimage;
	*nplanes = 1;

	return 0;
}

static int ge2d_buf_prepare(struct vb2_buffer *vb)
{
	struct ge2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct ge2d_frame *f = get_frame(ctx, vb->vb2_queue->type);

	vbuf->field = V4L2_FIELD_NONE;

	vb2_set_plane_payload(vb, 0, f->pix_fmt.sizeimage);

	return 0;
}

static void ge2d_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct ge2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int ge2d_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct ge2d_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		ctx->sequence_out = 0;
	else
		ctx->sequence_cap = 0;

	return 0;
}

static void ge2d_stop_streaming(struct vb2_queue *vq)
{
	struct ge2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops ge2d_qops = {
	.queue_setup = ge2d_queue_setup,
	.buf_prepare = ge2d_buf_prepare,
	.buf_queue = ge2d_buf_queue,
	.start_streaming = ge2d_start_streaming,
	.stop_streaming = ge2d_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int
queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct ge2d_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &ge2d_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->ge2d->mutex;
	src_vq->dev = ctx->ge2d->v4l2_dev.dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &ge2d_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->ge2d->mutex;
	dst_vq->dev = ctx->ge2d->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static int
vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, GE2D_NAME, sizeof(cap->driver));
	strscpy(cap->card, GE2D_NAME, sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" GE2D_NAME, sizeof(cap->bus_info));

	return 0;
}

static int vidioc_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct ge2d_ctx *ctx = priv;
	unsigned int i, n = 0;

	for (i = 0; i < NUM_FORMATS; i++) {
		if (!fmt_supported(ctx, &formats[i], f->type))
			continue;
		if (n++ == f->index) {
			f->pixelformat = formats[i].fourcc;
			return 0;
		}
	}

	return -EINVAL;
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct ge2d_ctx *ctx = priv;
	struct ge2d_frame *f;
	bool use_frame = false;

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	    s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	f = get_frame(ctx, s->type);

	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_COMPOSE:
		if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		use_frame = true;
		break;
	case V4L2_SEL_TGT_CROP:
		if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		use_frame = true;
		break;
	default:
		return -EINVAL;
	}

	if (use_frame) {
		s->r = f->crop;
	} else {
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = f->pix_fmt.width;
		s->r.height = f->pix_fmt.height;
	}

	return 0;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct ge2d_ctx *ctx = priv;
	struct meson_ge2d *ge2d = ctx->ge2d;
	struct ge2d_frame *f;
	int ret = 0;

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	    s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	f = get_frame(ctx, s->type);

	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE:
		/*
		 * COMPOSE target is only valid for capture buffer type, return
		 * error for output buffer type
		 */
		if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_CROP:
		/*
		 * CROP target is only valid for output buffer type, return
		 * error for capture buffer type
		 */
		if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		break;
	/*
	 * bound and default crop/compose targets are invalid targets to
	 * try/set
	 */
	default:
		return -EINVAL;
	}

	if (s->r.top < 0 || s->r.left < 0) {
		v4l2_err(&ge2d->v4l2_dev,
			 "doesn't support negative values for top & left.\n");
		return -EINVAL;
	}

	/* Keep the rectangle on chroma sample boundaries. */
	s->r.left = round_down(s->r.left, f->fmt->hsub);
	s->r.top = round_down(s->r.top, f->fmt->vsub);
	s->r.width = max_t(u32, round_down(s->r.width, f->fmt->hsub), f->fmt->hsub);
	s->r.height = max_t(u32, round_down(s->r.height, f->fmt->vsub), f->fmt->vsub);

	if (s->r.left + s->r.width > f->pix_fmt.width ||
	    s->r.top + s->r.height > f->pix_fmt.height) {
		v4l2_err(&ge2d->v4l2_dev, "unsupported rectangle value.\n");
		return -EINVAL;
	}

	f->crop = s->r;

	return ret;
}

static void ge2d_fill_pix(const struct ge2d_fmt *fmt, struct v4l2_pix_format *pix)
{
	/* Chroma planes of 3-plane formats need 8-byte aligned strides too. */
	unsigned int align = fmt->planes == 3 ? 16 : 8;
	unsigned int bpl, size;

	pix->field = V4L2_FIELD_NONE;
	pix->pixelformat = fmt->fourcc;
	pix->width = clamp_t(u32, ALIGN(pix->width, fmt->hsub), fmt->hsub,
			     round_down(MAX_WIDTH, fmt->hsub));
	pix->height = clamp_t(u32, ALIGN(pix->height, fmt->vsub), fmt->vsub,
			      round_down(MAX_HEIGHT, fmt->vsub));

	/* The stride registers count 8-byte units in 17 bits. */
	bpl = ALIGN(DIV_ROUND_UP(pix->width * fmt->depth, 8), align);
	pix->bytesperline = ALIGN(clamp_t(u32, pix->bytesperline, bpl, SZ_64K), align);

	size = pix->bytesperline * pix->height;
	if (fmt->planes == 2)
		size += pix->bytesperline * pix->height / fmt->vsub;
	else if (fmt->planes == 3)
		size += 2 * (pix->bytesperline / fmt->hsub) * (pix->height / fmt->vsub);
	pix->sizeimage = size;
}

/* Default capture size: the output frame, transposed when rotating by 90. */
static void vidioc_setup_cap_fmt(struct ge2d_ctx *ctx, struct v4l2_pix_format *f)
{
	struct ge2d_frame *frm_out = get_frame(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);

	f->width = frm_out->pix_fmt.width;
	f->height = frm_out->pix_fmt.height;

	if (ctx->xy_swap) {
		f->width = frm_out->pix_fmt.height;
		f->height = frm_out->pix_fmt.width;
	}
}

/*
 * The capture keeps the colorimetry of the output. A YUV capture of an RGB
 * output may pick its own encoding and range for the conversion.
 */
static void ge2d_copy_colorimetry(struct ge2d_ctx *ctx, const struct ge2d_fmt *fmt,
				  struct v4l2_pix_format *f)
{
	const struct v4l2_pix_format *in = &ctx->in.pix_fmt;
	bool own = fmt->yuv && !ctx->in.fmt->yuv;

	f->colorspace = in->colorspace;
	f->xfer_func = in->xfer_func;
	if (!own || f->ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT)
		f->ycbcr_enc = in->ycbcr_enc;
	if (!own || f->quantization == V4L2_QUANTIZATION_DEFAULT)
		f->quantization = in->quantization;
}

static int vidioc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ge2d_ctx *ctx = priv;
	const struct ge2d_fmt *fmt = find_fmt(ctx, f);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	/* Only the S7D variant scales; elsewhere the size follows the output. */
	if (!ctx->ge2d->data->c1 || !pix->width || !pix->height)
		vidioc_setup_cap_fmt(ctx, pix);

	ge2d_fill_pix(fmt, pix);
	ge2d_copy_colorimetry(ctx, fmt, pix);

	return 0;
}

static int vidioc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ge2d_ctx *ctx = priv;
	struct meson_ge2d *ge2d = ctx->ge2d;
	struct vb2_queue *vq;
	struct ge2d_frame *frm;
	int ret = 0;

	/* Adjust all values accordingly to the hardware capabilities
	 * and chosen format.
	 */
	ret = vidioc_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq)) {
		v4l2_err(&ge2d->v4l2_dev, "queue (%d) bust\n", f->type);
		return -EBUSY;
	}

	frm = get_frame(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	frm->pix_fmt = f->fmt.pix;
	frm->fmt = find_fmt(ctx, f);
	f->fmt.pix.pixelformat = frm->fmt->fourcc;

	/* Reset crop settings */
	frm->crop.left = 0;
	frm->crop.top = 0;
	frm->crop.width = frm->pix_fmt.width;
	frm->crop.height = frm->pix_fmt.height;

	return 0;
}

static int vidioc_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ge2d_ctx *ctx = priv;
	struct vb2_queue *vq;
	struct ge2d_frame *frm;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	frm = get_frame(ctx, f->type);

	f->fmt.pix = frm->pix_fmt;
	f->fmt.pix.pixelformat = frm->fmt->fourcc;

	return 0;
}

static int vidioc_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ge2d_ctx *ctx = priv;
	const struct ge2d_fmt *fmt = find_fmt(ctx, f);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	ge2d_fill_pix(fmt, pix);
	if (pix->colorspace == V4L2_COLORSPACE_DEFAULT)
		pix->colorspace = fmt->yuv ? V4L2_COLORSPACE_REC709 :
					     V4L2_COLORSPACE_SRGB;

	return 0;
}

/* Recompute the capture frame from the output, keeping its pixel format. */
static void ge2d_reset_cap(struct ge2d_ctx *ctx)
{
	struct ge2d_frame *frm = &ctx->out;
	struct v4l2_pix_format pix = {};

	vidioc_setup_cap_fmt(ctx, &pix);
	ge2d_fill_pix(frm->fmt, &pix);
	ge2d_copy_colorimetry(ctx, frm->fmt, &pix);
	frm->pix_fmt = pix;
	frm->crop.left = 0;
	frm->crop.top = 0;
	frm->crop.width = pix.width;
	frm->crop.height = pix.height;
}

static int vidioc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ge2d_ctx *ctx = priv;
	struct meson_ge2d *ge2d = ctx->ge2d;
	struct vb2_queue *vq;
	struct ge2d_frame *frm;
	int ret = 0;

	/* Adjust all values accordingly to the hardware capabilities
	 * and chosen format.
	 */
	ret = vidioc_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq)) {
		v4l2_err(&ge2d->v4l2_dev, "queue (%d) bust\n", f->type);
		return -EBUSY;
	}

	frm = get_frame(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);

	frm->pix_fmt = f->fmt.pix;
	frm->fmt = find_fmt(ctx, f);
	f->fmt.pix.pixelformat = frm->fmt->fourcc;

	/* Reset crop settings */
	frm->crop.left = 0;
	frm->crop.top = 0;
	frm->crop.width = frm->pix_fmt.width;
	frm->crop.height = frm->pix_fmt.height;

	/* Propagate settings to capture; a later capture S_FMT may scale. */
	ge2d_reset_cap(ctx);

	return 0;
}

static const struct v4l2_ioctl_ops ge2d_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt,
	.vidioc_g_fmt_vid_cap = vidioc_g_fmt,
	.vidioc_try_fmt_vid_cap = vidioc_try_fmt_cap,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt_cap,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt,
	.vidioc_g_fmt_vid_out = vidioc_g_fmt,
	.vidioc_try_fmt_vid_out = vidioc_try_fmt_out,
	.vidioc_s_fmt_vid_out = vidioc_s_fmt_out,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_g_selection = vidioc_g_selection,
	.vidioc_s_selection = vidioc_s_selection,
};

static int ge2d_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ge2d_ctx *ctx = container_of(ctrl->handler, struct ge2d_ctx,
					   ctrl_handler);
	struct vb2_queue *vq;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		ctx->hflip = ctrl->val;
		break;
	case V4L2_CID_VFLIP:
		ctx->vflip = ctrl->val;
		break;
	case V4L2_CID_ROTATE:
		vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		if (vb2_is_busy(vq))
			return -EBUSY;

		if (ctrl->val == 90) {
			ctx->hflip = 0;
			ctx->vflip = 1;
			ctx->xy_swap = 1;
		} else if (ctrl->val == 180) {
			ctx->hflip = 1;
			ctx->vflip = 1;
			ctx->xy_swap = 0;
		} else if (ctrl->val == 270) {
			ctx->hflip = 1;
			ctx->vflip = 0;
			ctx->xy_swap = 1;
		} else {
			ctx->hflip = 0;
			ctx->vflip = 0;
			ctx->xy_swap = 0;
		}

		/*
		 * If the rotation parameter changes the OUTPUT frames
		 * parameters, take them in account
		 */
		ge2d_reset_cap(ctx);

		break;
	}

	return 0;
}

static const struct v4l2_ctrl_ops ge2d_ctrl_ops = {
	.s_ctrl = ge2d_s_ctrl,
};

static int ge2d_setup_ctrls(struct ge2d_ctx *ctx)
{
	struct meson_ge2d *ge2d = ctx->ge2d;

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 4);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &ge2d_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &ge2d_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &ge2d_ctrl_ops,
			  V4L2_CID_ROTATE, 0, 270, 90, 0);

	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;

		v4l2_err(&ge2d->v4l2_dev, "%s failed\n", __func__);
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return err;
	}

	return 0;
}

static const struct ge2d_frame def_frame = {
	.pix_fmt = {
		.width = DEFAULT_WIDTH,
		.height = DEFAULT_HEIGHT,
		.bytesperline = DEFAULT_STRIDE,
		.sizeimage = DEFAULT_STRIDE * DEFAULT_HEIGHT,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_SRGB,
	},
	.crop.width = DEFAULT_WIDTH,
	.crop.height = DEFAULT_HEIGHT,
	.fmt = &formats[0],
};

static int ge2d_open(struct file *file)
{
	struct meson_ge2d *ge2d = video_drvdata(file);
	struct ge2d_ctx *ctx = NULL;
	int ret = 0;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->ge2d = ge2d;

	/* Keep the unit powered while a context exists. */
	ret = pm_runtime_resume_and_get(ge2d->dev);
	if (ret) {
		kfree(ctx);
		return ret;
	}

	/* Set default formats */
	ctx->in = def_frame;
	ctx->out = def_frame;

	if (mutex_lock_interruptible(&ge2d->mutex)) {
		pm_runtime_put(ge2d->dev);
		kfree(ctx);
		return -ERESTARTSYS;
	}
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(ge2d->m2m_dev, ctx, &queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		mutex_unlock(&ge2d->mutex);
		pm_runtime_put(ge2d->dev);
		kfree(ctx);
		return ret;
	}
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	ge2d_setup_ctrls(ctx);

	/* Write the default values to the ctx struct */
	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	mutex_unlock(&ge2d->mutex);

	return 0;
}

static int ge2d_release(struct file *file)
{
	struct ge2d_ctx *ctx =
		container_of(file->private_data, struct ge2d_ctx, fh);
	struct meson_ge2d *ge2d = ctx->ge2d;

	mutex_lock(&ge2d->mutex);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	mutex_unlock(&ge2d->mutex);
	pm_runtime_put(ge2d->dev);

	return 0;
}

static const struct v4l2_file_operations ge2d_fops = {
	.owner = THIS_MODULE,
	.open = ge2d_open,
	.release = ge2d_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct video_device ge2d_videodev = {
	.name = "meson-ge2d",
	.fops = &ge2d_fops,
	.ioctl_ops = &ge2d_ioctl_ops,
	.minor = -1,
	.release = video_device_release,
	.vfl_dir = VFL_DIR_M2M,
	.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static const struct regmap_config meson_ge2d_regmap_conf = {
	.reg_bits = 8,
	.val_bits = 32,
	.reg_stride = 4,
};

static int ge2d_runtime_suspend(struct device *dev)
{
	struct meson_ge2d *ge2d = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(ge2d->num_clks, ge2d->clks);
	return 0;
}

static int ge2d_runtime_resume(struct device *dev)
{
	struct meson_ge2d *ge2d = dev_get_drvdata(dev);
	int ret;

	/* The power domain may have been off: reset the unit after power up. */
	reset_control_assert(ge2d->rst);
	ret = clk_bulk_prepare_enable(ge2d->num_clks, ge2d->clks);
	if (ret)
		return ret;
	udelay(1);
	reset_control_deassert(ge2d->rst);
	return 0;
}

static int ge2d_probe(struct platform_device *pdev)
{
	struct regmap_config conf = meson_ge2d_regmap_conf;
	struct video_device *vfd;
	struct meson_ge2d *ge2d;
	struct resource *res;
	void __iomem *regs;
	int ret = 0;
	int irq;

	if (!pdev->dev.of_node)
		return -ENODEV;

	ge2d = devm_kzalloc(&pdev->dev, sizeof(*ge2d), GFP_KERNEL);
	if (!ge2d)
		return -ENOMEM;

	ge2d->dev = &pdev->dev;
	ge2d->data = of_device_get_match_data(&pdev->dev);
	mutex_init(&ge2d->mutex);
	INIT_DELAYED_WORK(&ge2d->timeout, ge2d_timeout);

	regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	if (resource_size(res) < ge2d->data->reg_offset + ge2d->data->max_register + 4)
		return dev_err_probe(ge2d->dev, -EINVAL, "register window too small\n");

	conf.max_register = ge2d->data->max_register;
	ge2d->map = devm_regmap_init_mmio(ge2d->dev, regs + ge2d->data->reg_offset,
					  &conf);
	if (IS_ERR(ge2d->map))
		return PTR_ERR(ge2d->map);

	ge2d->rst = devm_reset_control_get(ge2d->dev, NULL);
	if (IS_ERR(ge2d->rst))
		return dev_err_probe(ge2d->dev, PTR_ERR(ge2d->rst),
				     "failed to get core reset controller\n");

	ge2d->num_clks = devm_clk_bulk_get_all(ge2d->dev, &ge2d->clks);
	if (ge2d->num_clks < 0)
		return dev_err_probe(ge2d->dev, ge2d->num_clks, "failed to get clocks\n");

	/* The default CMA area may be too small for full-size frames. */
	ret = of_reserved_mem_device_init(ge2d->dev);
	if (ret && ret != -ENODEV)
		return dev_err_probe(ge2d->dev, ret, "failed to assign memory\n");

	platform_set_drvdata(pdev, ge2d);
	ret = devm_pm_runtime_enable(ge2d->dev);
	if (ret)
		goto release_mem;

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(ge2d->dev, irq, ge2d_isr, 0,
			       dev_name(ge2d->dev), ge2d);
	if (ret < 0) {
		dev_err(ge2d->dev, "failed to request irq\n");
		goto release_mem;
	}

	ret = v4l2_device_register(&pdev->dev, &ge2d->v4l2_dev);
	if (ret)
		goto release_mem;

	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&ge2d->v4l2_dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto unreg_v4l2_dev;
	}

	*vfd = ge2d_videodev;
	vfd->lock = &ge2d->mutex;
	vfd->v4l2_dev = &ge2d->v4l2_dev;

	video_set_drvdata(vfd, ge2d);
	ge2d->vfd = vfd;

	ge2d->m2m_dev = v4l2_m2m_init(&ge2d_m2m_ops);
	if (IS_ERR(ge2d->m2m_dev)) {
		v4l2_err(&ge2d->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(ge2d->m2m_dev);
		goto rel_vdev;
	}

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&ge2d->v4l2_dev, "Failed to register video device\n");
		goto rel_m2m;
	}

	v4l2_info(&ge2d->v4l2_dev, "Registered %s as /dev/%s\n",
		  vfd->name, video_device_node_name(vfd));

	return 0;

rel_m2m:
	v4l2_m2m_release(ge2d->m2m_dev);
rel_vdev:
	video_device_release(ge2d->vfd);
unreg_v4l2_dev:
	v4l2_device_unregister(&ge2d->v4l2_dev);
release_mem:
	of_reserved_mem_device_release(ge2d->dev);

	return ret;
}

static void ge2d_remove(struct platform_device *pdev)
{
	struct meson_ge2d *ge2d = platform_get_drvdata(pdev);

	video_unregister_device(ge2d->vfd);
	cancel_delayed_work_sync(&ge2d->timeout);
	v4l2_m2m_release(ge2d->m2m_dev);
	v4l2_device_unregister(&ge2d->v4l2_dev);
	of_reserved_mem_device_release(ge2d->dev);
}

static const struct meson_ge2d_data axg_ge2d_data = {
	.reg_offset = 0x8a0 * 4,
	.max_register = GE2D_SRC2_STRIDE_CTRL,
};

static const struct meson_ge2d_data s7d_ge2d_data = {
	.reg_offset = 0xa0 * 4,
	.max_register = GE2D_C1_SRC2_STRIDE_CTRL,
	.c1 = true,
};

static const struct of_device_id meson_ge2d_match[] = {
	{
		.compatible = "amlogic,axg-ge2d",
		.data = &axg_ge2d_data,
	}, {
		.compatible = "amlogic,s7d-ge2d",
		.data = &s7d_ge2d_data,
	},
	{},
};

MODULE_DEVICE_TABLE(of, meson_ge2d_match);

static DEFINE_RUNTIME_DEV_PM_OPS(ge2d_pm_ops, ge2d_runtime_suspend,
				 ge2d_runtime_resume, NULL);

static struct platform_driver ge2d_drv = {
	.probe = ge2d_probe,
	.remove_new = ge2d_remove,
	.driver = {
		.name = "meson-ge2d",
		.of_match_table = meson_ge2d_match,
		.pm = pm_ptr(&ge2d_pm_ops),
	},
};

module_platform_driver(ge2d_drv);

MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Amlogic 2D Graphic Acceleration Unit");
MODULE_LICENSE("GPL");
