// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/byteorder.h>

#include <drm/drm_fourcc.h>

#include "s7d-video-pipeline.h"

/* VCBUS word indices selected for S7D by vendor video_hw.c. */
#define VD1_GEN_REG		0x4800
#define VD1_LUMA_X0		0x4803
#define VD1_LUMA_Y0		0x4804
#define VD1_CHROMA_X0		0x4805
#define VD1_CHROMA_Y0		0x4806
#define VD1_RPT_LOOP		0x480b
#define VD1_LUMA0_RPT_PAT		0x480c
#define VD1_CHROMA0_RPT_PAT	0x480d
#define VD1_LUMA_PSEL		0x4810
#define VD1_CHROMA_PSEL		0x4811
#define VD1_DUMMY_PIXEL		0x4812
#define VD1_RANGE_MAP_Y		0x4816
#define VD1_RANGE_MAP_CB		0x4817
#define VD1_RANGE_MAP_CR		0x4818
#define VD1_GEN_REG2		0x4819
#define VD1_GEN_REG3		0x481c
#define VD1_FMT_CTRL		0x481d
#define VD1_FMT_W		0x481e
#define VD1_BADDR_Y		0x4820
#define VD1_BADDR_CB		0x4821
#define VD1_BADDR_CR		0x4822
#define VD1_STRIDE_0		0x4823
#define VD1_STRIDE_1		0x4824
#define VPP_LINE_IN_LENGTH	0x1d01
#define VPP_PIC_IN_HEIGHT		0x1d02
#define VPP_SC_MISC		0x1d19
#define VD1_PATH		0x1a0a
#define VD1_PATH_AFBC		(BIT(22) | BIT(12) | BIT(10))
/* VD1 Amlogic FBC decoder, SC2 register array */
#define AFBC_ENABLE		0x4840
#define AFBC_ENABLE_RESET	0x00241602
#define AFBC_MODE		0x4841
#define AFBC_SIZE_IN		0x4842
#define AFBC_DEC_DEF_COLOR	0x4843
#define AFBC_CONV_CTRL		0x4844
#define AFBC_HEAD_BADDR		0x4846
#define AFBC_BODY_BADDR		0x4847
#define AFBC_VD_CFMT_CTRL	0x484b
#define AFBC_MIF_HOR_SCOPE	0x484d
#define AFBC_MIF_VER_SCOPE	0x484e
#define AFBC_PIXEL_HOR_SCOPE	0x484f
#define AFBC_PIXEL_VER_SCOPE	0x4850
#define AFBCDEC_IQUANT_ENABLE	0x4852

#define VD1_GEN_MASK		(GENMASK(29, 18) | \
				 (GENMASK(16, 0) & ~BIT(5)))
#define VD1_GEN_VALUE		(BIT(29) | (3 << 27) | (8 << 19) | \
				 BIT(6) | BIT(4) | BIT(1))

#define SET(r, m, v)	{ .reg = (r), .mask = (m), .value = (v) }
#define REG(r, v)	{ .reg = cpu_to_le32(r), .value = cpu_to_le32(v) }

static int check_layout(const struct s7d_video_state *layout,
			const struct drm_rect *dst, u32 *width, u32 *height)
{
	u32 x_start, x_end, y_start, y_end;
	u32 chroma_x, chroma_y;
	bool semi422 = layout->format == DRM_FORMAT_NV16 ||
		layout->format == DRM_FORMAT_NV61;
	bool deep = layout->format == DRM_FORMAT_XVYU2101010;
	bool full_chroma = deep || layout->format == DRM_FORMAT_VUY888;
	bool packed = full_chroma || layout->format == DRM_FORMAT_UYVY ||
		layout->format == DRM_FORMAT_VYUY;
	u32 cpp = deep ? 4 : full_chroma ? 3 : packed ? 2 : 1;

	if (!dst || (layout->reflection & ~(S7D_VIDEO_REFLECT_X | S7D_VIDEO_REFLECT_Y)) ||
	    (layout->format != DRM_FORMAT_NV12 &&
		     layout->format != DRM_FORMAT_NV21 && !semi422 && !packed) ||
	    !layout->stride[0] || layout->stride[0] > 0x1fff ||
	    (packed ? layout->stride[1] || layout->frame_addr[1] :
	     !layout->stride[1] || layout->stride[1] > 0x1fff) ||
	    ((layout->scope_x[0] | layout->scope_x[1] |
	      layout->scope_y[0] | layout->scope_y[1]) & ~0x1fff1fffU) ||
	    dst->x1 < 0 || dst->y1 < 0 || dst->x2 > 8192 || dst->y2 > 8192 ||
	    dst->x2 <= dst->x1 || dst->y2 <= dst->y1 ||
	    ((dst->x1 | dst->y1 | dst->x2 | dst->y2) & 1))
		return -EINVAL;
	x_start = layout->scope_x[0] & 0x1fff;
	x_end = layout->scope_x[0] >> 16;
	y_start = layout->scope_y[0] & 0x1fff;
	y_end = layout->scope_y[0] >> 16;
	if (x_end < x_start || y_end < y_start ||
	    ((x_start | y_start | (x_end + 1) | (y_end + 1)) & 1) ||
	    x_end >= S7D_VIDEO_MAX_WIDTH || y_end >= S7D_VIDEO_MAX_HEIGHT ||
	    (x_end + 1) * cpp > layout->stride[0] * 16 ||
	    (!packed && x_end + 1 > layout->stride[1] * 16))
		return -EINVAL;
	chroma_x = full_chroma ? layout->scope_x[0] :
		((x_end >> 1) << 16) | (x_start >> 1);
	chroma_y = packed || semi422 ? layout->scope_y[0] :
		((y_end >> 1) << 16) | (y_start >> 1);
	*width = x_end - x_start + 1;
	*height = y_end - y_start + 1;
	if (layout->scope_x[1] != chroma_x || layout->scope_y[1] != chroma_y)
		return -EINVAL;
	return 0;
}

/*
 * Vendor vd1_set_dcu() compressed branch and vd_afbc_setting_tl1() for S7D:
 * the decoder replaces the MIF, which stays disabled, and the control word
 * becomes AFBC_ENABLE (dec_enable is bit 8, dos_uncomp_mode bit 19).
 */
static int build_fbc(const struct s7d_video_state *layout, const struct drm_rect *dst,
		     unsigned int hold_lines, struct s7d_video_pipeline_state *next,
		     struct s7d_video_pipeline_state *out)
{
	const struct s7d_video_fbc *fbc = &layout->fbc;
	u32 width = (layout->scope_x[1] & 0xffff) - (layout->scope_x[1] >> 16) + 1;
	u32 height = (layout->scope_y[1] & 0xffff) - (layout->scope_y[1] >> 16) + 1;
	/* The decoder's own hold lines, AFBC_MODE[21:16], as the MIF's. */
	u32 mode = (3 << 24) | (hold_lines << 16) | (2 << 14) | BIT(29) |
		(fbc->ten_bit ? 0x2a00 : 0) | (fbc->mem_saving ? BIT(28) : 0);

	if (!dst || dst->x1 < 0 || dst->y1 < 0 || dst->x2 > 8192 || dst->y2 > 8192 ||
	    dst->x2 <= dst->x1 || dst->y2 <= dst->y1 ||
	    ((dst->x1 | dst->y1 | dst->x2 | dst->y2 | width | height) & 1) ||
	    !fbc->width || !fbc->height || fbc->width > S7D_VIDEO_MAX_WIDTH ||
	    fbc->height > S7D_VIDEO_MAX_HEIGHT || layout->reflection ||
	    s7d_video_scaler_build(width, height, drm_rect_width(dst),
				   drm_rect_height(dst), false, &next->scaler))
		return -EINVAL;
	next->setup[S7D_VIDEO_SETUP_REG_COUNT - 5] =
		(struct s7d_video_reg_setting)SET(AFBC_MODE, U32_MAX, mode);
	next->setup[S7D_VIDEO_SETUP_REG_COUNT - 4] =
		(struct s7d_video_reg_setting)SET(AFBC_DEC_DEF_COLOR, U32_MAX,
						  fbc->ten_bit ? 0x3ff80200 : 0x3ff20080);
	next->setup[S7D_VIDEO_SETUP_REG_COUNT - 3] =
		(struct s7d_video_reg_setting)SET(AFBC_VD_CFMT_CTRL, U32_MAX, 0x10310c11);
	next->setup[S7D_VIDEO_SETUP_REG_COUNT - 2] =
		(struct s7d_video_reg_setting)SET(AFBCDEC_IQUANT_ENABLE, U32_MAX, 0);
	next->setup[S7D_VIDEO_SETUP_REG_COUNT - 1] =
		(struct s7d_video_reg_setting)SET(VD1_PATH, VD1_PATH_AFBC | BIT(9) | BIT(8),
						  VD1_PATH_AFBC);
	next->control = (struct s7d_video_reg_setting)SET(AFBC_ENABLE, U32_MAX,
							   BIT(19) | 0x1700);
	next->update[0] = (struct s7d_rdma_entry)REG(AFBC_HEAD_BADDR, layout->frame_addr[0]);
	next->update[1] = (struct s7d_rdma_entry)REG(AFBC_BODY_BADDR, 0);
	next->update[2] = (struct s7d_rdma_entry)REG(AFBC_SIZE_IN,
		(ALIGN(fbc->width, 32) << 16) | ALIGN(fbc->height, 4));
	next->update[3] = (struct s7d_rdma_entry)REG(AFBC_MIF_HOR_SCOPE, layout->scope_x[0]);
	next->update[4] = (struct s7d_rdma_entry)REG(AFBC_MIF_VER_SCOPE, layout->scope_y[0]);
	next->update[5] = (struct s7d_rdma_entry)REG(AFBC_PIXEL_HOR_SCOPE, layout->scope_x[1]);
	next->update[6] = (struct s7d_rdma_entry)REG(AFBC_PIXEL_VER_SCOPE, layout->scope_y[1]);
	/* lbuf_len 0x100 for VD1, 4:2:0 */
	next->update[7] = (struct s7d_rdma_entry)REG(AFBC_CONV_CTRL, (2 << 12) | 0x100);
	next->update[8] = (struct s7d_rdma_entry)REG(VD1_GEN_REG2, 0);
	next->update[9] = (struct s7d_rdma_entry)REG(VD1_FMT_W, (width << 16) | (width >> 1));
	next->update[10] = (struct s7d_rdma_entry)REG(VPP_LINE_IN_LENGTH, width);
	next->update[11] = (struct s7d_rdma_entry)REG(VPP_PIC_IN_HEIGHT, height);
	next->update_count = S7D_VIDEO_UPDATE_REG_COUNT;
	*out = *next;
	return 0;
}

static int s7d_video_build_pipeline_common(const struct s7d_video_state *layout,
					   const struct drm_rect *dst,
					   unsigned int hold_lines, bool vd2,
					   struct s7d_video_pipeline_state *out)
{
	/*
	 * Sources: video_hw.c S7D selection, vd1_set_dcu(), vd_set_blk_mode(),
	 * vd_mif_setting() and video_safa.c;
	 * S905X5M tables 9-1326..1353, 9-1454..1459 and 9-1678.
	 * The PDF's GEN2/GEN3/FMT addresses and NV selectors disagree with the
	 * selected S7D source; use its SC2 MIF/T7 linear register arrays.
	 */
	struct s7d_video_pipeline_state next = {
		.setup = {
			/* Disabled while the remaining immediate setup is applied. */
			SET(VD1_GEN_REG, VD1_GEN_MASK, VD1_GEN_VALUE),
			/* 16-byte Y bursts; preserve inherited block length [6:4]. */
			SET(VD1_GEN_REG3, GENMASK(26, 18) | GENMASK(15, 8) |
			    GENMASK(2, 0), (2 << 14) | (2 << 12)),
			/* Default 4:2:0; preserve formatter clock/reset fields. */
			SET(VD1_FMT_CTRL, GENMASK(29, 0), 0x30310c11),
			SET(VD1_RANGE_MAP_Y, BIT(0), 0),
			SET(VD1_RANGE_MAP_CB, BIT(0), 0),
			SET(VD1_RANGE_MAP_CR, BIT(0), 0),
			/* Top bypass also bypasses coefficient/phase/region state. */
			SET(VPP_SC_MISC, GENMASK(25, 15), 0),
			SET(VD1_RPT_LOOP, 0x77777777, 0),
			SET(VD1_LUMA0_RPT_PAT, U32_MAX, 0),
			SET(VD1_CHROMA0_RPT_PAT, U32_MAX, 0),
			/* Single picture0; picture1 scopes/bases are ignored. */
			SET(VD1_LUMA_PSEL, 0x0fffffff, 0),
			SET(VD1_CHROMA_PSEL, 0x0fffffff, 0),
			SET(VD1_DUMMY_PIXEL, 0xffffff00, 0x00808000),
			/* The interleaved UV plane does not use a separate CR base. */
			SET(VD1_BADDR_CR, U32_MAX, 0),
			/*
			 * The FBC decoder stays at its reset value, which the MIF path
			 * also needs; the VPP setup selects that path.
			 */
			SET(AFBC_ENABLE, U32_MAX, AFBC_ENABLE_RESET),
		},
		.control = SET(VD1_GEN_REG, VD1_GEN_MASK, VD1_GEN_VALUE),
	};
	u32 width, height, color;
	bool deep, full_chroma;

	if (!out || !hold_lines || hold_lines > 63)
		return -EINVAL;
	next.setup[0].value = (VD1_GEN_VALUE & ~GENMASK(24, 19)) |
		(hold_lines << 19);
	next.control.value = next.setup[0].value;
	if (!layout) {
		*out = next;
		return 0;
	}
	if (layout->fbc.enabled)
		return vd2 ? -EINVAL : build_fbc(layout, dst, hold_lines, &next, out);
	if (check_layout(layout, dst, &width, &height))
		return -EINVAL;
	deep = layout->format == DRM_FORMAT_XVYU2101010;
	full_chroma = deep || layout->format == DRM_FORMAT_VUY888;
	if (vd2) {
		next.scaler.src_width = width;
		next.scaler.src_height = height;
		next.scaler.dst_width = drm_rect_width(dst);
		next.scaler.dst_height = drm_rect_height(dst);
		next.scaler.enabled = width != next.scaler.dst_width ||
			height != next.scaler.dst_height;
	} else if (s7d_video_scaler_build(width, height, drm_rect_width(dst),
					  drm_rect_height(dst),
					  full_chroma, &next.scaler)) {
		return -EINVAL;
	}
	if (full_chroma) {
		/* Bytes per pixel minus one; 10-bit 4:4:4 also needs bits mode 2. */
		next.setup[0].value = (next.setup[0].value & ~BIT(1)) | BIT(16) |
			((deep ? 3 : 2) << 14);
		if (deep)
			next.setup[1].value |= 2 << 8;
		next.control.value = next.setup[0].value;
		next.setup[2].value = BIT(29);
		color = 0;
	} else if (layout->format == DRM_FORMAT_UYVY || layout->format == DRM_FORMAT_VYUY) {
		next.setup[0].value = (next.setup[0].value & ~BIT(1)) | BIT(14);
		next.control.value = next.setup[0].value;
		next.setup[2].value = 0x30310c20;
		color = 0;
	} else {
		color = layout->format == DRM_FORMAT_NV12 ||
			layout->format == DRM_FORMAT_NV16 ? 2 : 1;
		if (layout->format == DRM_FORMAT_NV16 || layout->format == DRM_FORMAT_NV61)
			next.setup[2].value = 0x30310c21;
	}
	/* SC2/T7 linear access requires STRIDE1[16], outside the stride. */
	next.update[0] = (struct s7d_rdma_entry)REG(VD1_BADDR_Y, layout->frame_addr[0]);
	next.update[1] = (struct s7d_rdma_entry)REG(VD1_BADDR_CB, layout->frame_addr[1]);
	next.update[2] = (struct s7d_rdma_entry)REG(VD1_STRIDE_0,
						layout->stride[0] | (layout->stride[1] << 16));
	next.update[3] = (struct s7d_rdma_entry)REG(VD1_STRIDE_1, BIT(16) | layout->stride[1]);
	next.update[4] = (struct s7d_rdma_entry)REG(VD1_LUMA_X0, layout->scope_x[0]);
	next.update[5] = (struct s7d_rdma_entry)REG(VD1_LUMA_Y0, layout->scope_y[0]);
	next.update[6] = (struct s7d_rdma_entry)REG(VD1_CHROMA_X0, layout->scope_x[1]);
	next.update[7] = (struct s7d_rdma_entry)REG(VD1_CHROMA_Y0, layout->scope_y[1]);
	if (layout->reflection & S7D_VIDEO_REFLECT_X)
		color |= BIT(2) | BIT(4);
	if (layout->reflection & S7D_VIDEO_REFLECT_Y)
		color |= BIT(3) | BIT(5);
	next.update[8] = (struct s7d_rdma_entry)REG(VD1_GEN_REG2, color);
	/* Vendor width4096 uses FMT_W bit28; qualify that PDF conflict on board. */
	next.update[9] = (struct s7d_rdma_entry)REG(VD1_FMT_W, (width << 16) | (width >> 1));
	next.update[10] = (struct s7d_rdma_entry)REG(VPP_LINE_IN_LENGTH, width);
	next.update[11] = (struct s7d_rdma_entry)REG(VPP_PIC_IN_HEIGHT, height);
	next.update_count = S7D_VIDEO_UPDATE_REG_COUNT;
	next.control.value |= BIT(0);
	*out = next;
	return 0;
}

int s7d_video_build_pipeline(const struct s7d_video_state *layout,
			     const struct drm_rect *dst, unsigned int hold_lines,
			     struct s7d_video_pipeline_state *out)
{
	return s7d_video_build_pipeline_common(layout, dst, hold_lines, false, out);
}

int s7d_video_build_vd2_pipeline(const struct s7d_video_state *layout,
			       const struct drm_rect *dst, unsigned int hold_lines,
			       struct s7d_video_pipeline_state *out)
{
	struct s7d_video_pipeline_state next;
	unsigned int i;
	int ret;

	if (!out)
		return -EINVAL;
	ret = s7d_video_build_pipeline_common(layout, dst, hold_lines, true, &next);
	if (ret)
		return ret;
	if (layout && (next.scaler.src_width > 2048 ||
		       next.scaler.src_height > 1080))
		return -EINVAL;
	/* S7D selects the same MIF layout at 0x4880 and a separate VD2 PPS. */
	for (i = 0; i < S7D_VIDEO_SETUP_REG_COUNT; i++) {
		if (next.setup[i].reg == AFBC_ENABLE) {
			/* VD1 only; later entries are FBC setup and stay unused. */
			memset(&next.setup[i], 0, (S7D_VIDEO_SETUP_REG_COUNT - i) *
			       sizeof(next.setup[i]));
			break;
		}
		if (next.setup[i].reg == VPP_SC_MISC) {
			next.setup[i].reg = 0x3958;
			next.setup[i].mask = BIT(24) | GENMASK(22, 15);
		} else {
			next.setup[i].reg += 0x80;
		}
	}
	next.control.reg += 0x80;
	for (i = 0; i < 10 && i < next.update_count; i++)
		next.update[i].reg = cpu_to_le32(le32_to_cpu(next.update[i].reg) + 0x80);
	if (layout) {
		next.update[10] = (struct s7d_rdma_entry)REG(0x1df0,
			(next.scaler.src_height << 16) | next.scaler.src_width);
		next.update_count = S7D_VIDEO2_UPDATE_REG_COUNT;
	}
	*out = next;
	return 0;
}
