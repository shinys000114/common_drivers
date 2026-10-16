// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "s7d-vp9-inter-picture.h"

#define HREG(word)			((word) * 4U)
#define HEVC_PARSER_PICTURE_SIZE		HREG(0x3123)
#define HEVC_MPRED_CTRL0			HREG(0x3201)
#define HEVC_MPRED_ABOVE			HREG(0x3212)
#define HEVC_MPRED_MV_WR_START		HREG(0x3213)
#define HEVC_MPRED_MV_RD_START		HREG(0x3214)
#define HEVC_MPRED_MV_WPTR			HREG(0x3215)
#define HEVC_MPRED_MV_RPTR			HREG(0x3216)
#define HEVC_MPRED_CTRL3			HREG(0x321d)
#define HEVC_MPRED_CTRL4			HREG(0x324c)
#define HEVC_MPRED_MV_RD_END		HREG(0x3262)
#define VP9_REF_SCALE_ENABLE		HREG(0x3441)
#define VP9_REFINFO_ACCESS		HREG(0x3442)
#define VP9_REFINFO_DATA			HREG(0x3443)
#define HEVCD_IPP_AXIIF_CONFIG		HREG(0x340b)
#define HEVCD_ANC2AXI_CONFIG		HREG(0x3460)
#define HEVCD_ANC2AXI_DATA		HREG(0x3464)
#define HEVCD_ANC_CANVAS_ACCESS		HREG(0x34c0)
#define HEVCD_ANC_CANVAS_DATA		HREG(0x34c1)
#define HEVCD_DECOMP_CTRL1		HREG(0x34c2)
#define HEVCD_MCRCC_CTRL1			HREG(0x34f0)
#define HEVCD_MCRCC_CTRL2			HREG(0x34f1)
#define HEVCD_MCRCC_CTRL3			HREG(0x34f2)
#define HEVC_DBLK_CFG9			HREG(0x3509)
#define HEVC_DBLK_CFGA			HREG(0x350a)
#define HEVC_DBLK_CFGB			HREG(0x350b)
#define HEVC_SAO_CTRL1			HREG(0x3602)
#define HEVC_SAO_Y_START			HREG(0x360b)
#define HEVC_SAO_Y_LENGTH			HREG(0x360c)
#define HEVC_SAO_C_START			HREG(0x360d)
#define HEVC_SAO_C_LENGTH			HREG(0x360e)
#define HEVC_SAO_Y_WPTR			HREG(0x360f)
#define HEVC_SAO_C_WPTR			HREG(0x3610)
#define HEVC_SAO_CTRL5			HREG(0x3623)
#define HEVC_CM_BODY_LENGTH		HREG(0x3627)
#define HEVC_CM_HEADER_LENGTH		HREG(0x3629)
#define HEVC_CM_HEADER_OFFSET		HREG(0x362b)

static bool overlaps(const struct s7d_vp9_region *a,
		     const struct s7d_vp9_region *b)
{
	return (u64)a->address < (u64)b->address + b->bytes &&
	       (u64)b->address < (u64)a->address + a->bytes;
}

static int region_valid(const struct s7d_vp9_region *region, u32 minimum,
			 u32 alignment)
{
	if (!region->address || !IS_ALIGNED(region->address, alignment) ||
	    region->bytes < minimum ||
	    (u64)region->address + region->bytes > (1ULL << 32))
		return -EINVAL;
	return 0;
}

static bool overlaps_workspace(const struct s7d_vp9_workspace *ws,
			       const struct s7d_vp9_region *region)
{
	return overlaps(region, &ws->backing) || overlaps(region, &ws->rpm) ||
	       overlaps(region, &ws->lmem);
}

int s7d_vp9_compressed_mv_size(u32 width, u32 height, u32 *bytes)
{
	u32 x, y, size;

	if (!bytes || width < 64 || width > 1920 || height < 64 || height > 1080 ||
	    ((width | height) & 1))
		return -EOPNOTSUPP;
	x = DIV_ROUND_UP(width, 64);
	y = DIV_ROUND_UP(ALIGN(height, 32), 64);
	size = x * y * 36 * 16 + x * 16 * ((y >> 3) + (y & 7));
	*bytes = ALIGN(size, 65536);
	return 0;
}

static int validate_rpm(const struct s7d_vp9_picture_frame *f)
{
	const struct s7d_vp9_header_prefix *h = f->header;
	const struct s7d_vp9_rpm *rpm = f->rpm;
	u32 refs = 0, width = rpm->word[10], height = rpm->word[11], i;
	bool from_reference = false;

	if (rpm->word[0] || rpm->word[1] || rpm->word[3] != !h->keyframe ||
	    rpm->word[4] != h->show_frame || rpm->word[5] != h->error_resilient ||
	    rpm->word[25] != 8)
		return -EOPNOTSUPP;
	if (rpm->word[7] > 1 || rpm->word[16] > 1 || rpm->word[23] > 63 ||
	    rpm->word[24] > 7 || rpm->word[34] > 1 ||
	    (rpm->word[34] && rpm->word[35] > 1))
		return -EINVAL;
	if (rpm->word[16])
		for (i = 17; i < 23; i++)
			if ((s8)rpm->word[i] < -63 || (s8)rpm->word[i] > 63)
				return -EINVAL;
	if (!h->keyframe &&
	    ((!h->show_frame && rpm->word[6] != h->intra_only) ||
	     rpm->word[9] != h->refresh_flags ||
	     (!h->error_resilient && rpm->word[8] != h->reset_frame_context)))
		return -EBADMSG;
	if (!h->keyframe && !h->intra_only) {
		for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
			if (h->reference_index[i] >= S7D_VP9_REFERENCE_SLOTS)
				return -EINVAL;
			refs = (refs << 4) | (h->reference_index[i] << 1) | h->sign_bias[i];
		}
		if ((rpm->word[14] & 0xfff) != refs)
			return -EBADMSG;
		for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
			u32 slot = f->logical_to_physical[h->reference_index[i]];

			if (slot >= f->physical_count || !f->surface[slot].valid)
				return -ENODATA;
			if (!(rpm->word[15] & BIT(2 - i)) || from_reference)
				continue;
			width = f->surface[slot].width;
			height = f->surface[slot].height;
			from_reference = true;
		}
		if (from_reference != h->size_from_reference)
			return -EBADMSG;
	}
	if (width != h->width || height != h->height ||
	    (rpm->word[7] && (rpm->word[12] != h->render_width ||
			     rpm->word[13] != h->render_height)))
		return -EBADMSG;
	return 0;
}

static u32 threshold(u32 level, u32 sharpness)
{
	u32 limit = level >> ((sharpness > 0) + (sharpness > 4));

	if (sharpness)
		limit = min(limit, 9 - sharpness);
	limit = max(limit, 1U);
	return ((limit & 0x3f) << 8) | (2 * (level + 2) + limit);
}

static void filter_plan(const struct s7d_vp9_rpm *rpm, struct s7d_vp9_picture *p)
{
	u8 level[8][4][2] = { };
	u32 i, ref, mode;
	int base = rpm->word[23], scale = 1 << (base >> 5);

	for (i = 0; i < 32; i++)
		p->threshold[i] = (threshold(i * 2 + 1, rpm->word[24]) << 16) |
				  threshold(i * 2, rpm->word[24]);
	for (i = 0; i < 8; i++) {
		int segment_level = base;
		u16 feature = rpm->word[36 + i];

		if (rpm->word[34] && (feature & BIT(15))) {
			int delta = feature & 0x3f;

			if (feature & BIT(8))
				delta = -delta;
			segment_level = clamp(rpm->word[35] ? delta : base + delta, 0, 63);
		}
		if (!rpm->word[16]) {
			memset(level[i], segment_level, sizeof(level[i]));
			continue;
		}
		level[i][0][0] = clamp(segment_level + (s8)rpm->word[17] * scale, 0, 63);
		for (ref = 1; ref < 4; ref++)
			for (mode = 0; mode < 2; mode++)
				level[i][ref][mode] = clamp(segment_level +
					(s8)rpm->word[17 + ref] * scale +
					(s8)rpm->word[21 + mode] * scale, 0, 63);
	}
	for (i = 0; i < 16; i++) {
		u32 value = 0;

		for (ref = 0; ref < 4; ref++)
			value |= (u32)level[i >> 1][ref][i & 1] << (ref * 8);
		p->level[i] = base ? value : 0;
	}
}

int s7d_vp9_inter_picture_plan(const struct s7d_vp9_picture_frame *f,
			      struct s7d_vp9_hw_picture_plan *plan)
{
	struct s7d_vp9_hw_picture_plan next = { };
	const struct s7d_vp9_header_prefix *h;
	const struct s7d_codec_layout *l;
	u32 i, j, a, b, end;
	int ret;

	if (!f || !plan || !f->header || !f->rpm || !f->layout || !f->surface)
		return -EINVAL;
	h = f->header;
	l = f->layout;
	if (f->coded_frames != 1 || h->show_existing ||
	    h->render_width != h->width || h->render_height != h->height ||
	    (h->keyframe && (h->refresh_flags != 0xff || h->color_space > 3)))
		return -EOPNOTSUPP;
	ret = s7d_vp9_compressed_mv_size(h->width, h->height, &next.mv_bytes);
	if (ret)
		return ret;
	ret = s7d_vp9_workspace_validate(f->workspace);
	if (ret)
		return ret;
	ret = s7d_codec_validate_layout(l);
	if (ret)
		return ret;
	if (l->format != S7D_CODEC_CAPTURE_NV12M || l->coded_width != h->width ||
	    l->coded_height != h->height || l->visible_left || l->visible_top ||
	    l->visible_width != h->width || l->visible_height != h->height ||
	    l->pitch[0] != ALIGN(h->width, 64) || l->storage_height != ALIGN(h->height, 64) ||
	    l->size[0] != l->pitch[0] * l->storage_height || l->size[1] != l->size[0] / 2 ||
	    !f->physical_count || f->physical_count > S7D_VP9_MAX_PHYSICAL ||
	    f->current_physical >= f->physical_count ||
	    !f->surface[f->current_physical].valid)
		return -EINVAL;
	for (i = 0; i < f->physical_count; i++) {
		const struct s7d_vp9_hw_surface *s = &f->surface[i];

		if (!s->valid)
			continue;
		if (s->width != h->width || s->height != h->height)
			return -EOPNOTSUPP;
		for (a = 0; a < 2; a++) {
			if (region_valid(&s->plane[a], l->size[a], 32) ||
			    overlaps_workspace(f->workspace, &s->plane[a]) ||
			    overlaps(&s->plane[a], &f->current_mv) ||
			    (!h->keyframe && !h->intra_only &&
			     overlaps(&s->plane[a], &f->previous_mv)))
				return -EINVAL;
			for (j = 0; j <= i; j++) {
				if (!f->surface[j].valid)
					continue;
				for (b = 0; b < 2; b++) {
					if (j == i && b >= a)
						continue;
					if (overlaps(&s->plane[a], &f->surface[j].plane[b]))
						return -EINVAL;
				}
			}
		}
		next.anc2axi[i * 2] = s->plane[0].address >> 5;
		next.anc2axi[i * 2 + 1] = s->plane[1].address >> 5;
	}
	for (i = 0; i < S7D_VP9_REFERENCE_SLOTS; i++) {
		u32 slot = f->logical_to_physical[i];

		if (slot == S7D_VP9_PHYSICAL_INVALID)
			continue;
		if (slot >= f->physical_count || !f->surface[slot].valid ||
		    slot == f->current_physical)
			return -EINVAL;
	}
	ret = validate_rpm(f);
	if (ret)
		return ret;
	if (region_valid(&f->current_mv, next.mv_bytes, 4096) ||
	    overlaps_workspace(f->workspace, &f->current_mv) ||
	    check_add_overflow(f->current_mv.address, next.mv_bytes, &end))
		return -EINVAL;
	if (!h->keyframe && !h->intra_only) {
		if (!f->previous_valid || f->previous_width != h->width ||
		    f->previous_height != h->height ||
		    region_valid(&f->previous_mv, next.mv_bytes, 4096) ||
		    overlaps_workspace(f->workspace, &f->previous_mv) ||
		    overlaps(&f->current_mv, &f->previous_mv) ||
		    check_add_overflow(f->previous_mv.address, next.mv_bytes, &end))
			return -EINVAL;
		next.previous_mv_end = end;
		for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
			u32 slot = f->logical_to_physical[h->reference_index[i]];
			u32 y = slot * 2, uv = y + 1;

			next.reference_canvas[i] = (uv << 16) | (uv << 8) | y;
			next.reference_info[i][0] = h->width;
			next.reference_info[i][1] = h->height;
			next.reference_info[i][2] = 1 << 14;
			next.reference_info[i][3] = 1 << 14;
			next.reference_info[i][4] = DIV_ROUND_UP(h->width, 64) *
				DIV_ROUND_UP(h->height, 32) * 3200 / 32;
		}
	}
	next.picture.size = (h->height << 16) | h->width;
	next.picture.body_bytes = DIV_ROUND_UP(h->width, 64) *
		DIV_ROUND_UP(h->height, 32) * 3200;
	next.picture.header_bytes = DIV_ROUND_UP(h->width, 128) *
		DIV_ROUND_UP(h->height, 64) * 32;
	next.picture.sao_ctrl1 = (2 << 14) | 0xff0 | 1;
	next.picture.ipp_axiif = (2 << 8) | 0xf;
	next.picture.dblk_cfgb = 0xfe01 | (h->width >= 1280 ? BIT(4) : 0);
	filter_plan(f->rpm, &next.picture);
	next.physical_count = f->physical_count;
	next.current_y = f->surface[f->current_physical].plane[0].address;
	next.current_uv = f->surface[f->current_physical].plane[1].address;
	next.y_bytes = l->size[0];
	next.uv_bytes = l->size[1];
	next.mv_above = f->workspace->part[S7D_VP9_MPRED_ABOVE].address;
	next.current_mv = f->current_mv.address;
	next.previous_mv = f->previous_mv.address;
	next.intra = h->keyframe || h->intra_only;
	next.use_prev_frame_mvs = !next.intra && !h->error_resilient &&
		f->previous_valid && !f->previous_keyframe && !f->previous_intra_only &&
		f->previous_show_frame && f->previous_width == h->width &&
		f->previous_height == h->height;
	*plan = next;
	return 0;
}

static int write_many(struct regmap *map, const struct reg_sequence *seq, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = regmap_write(map, seq[i].reg, seq[i].def);
		if (ret)
			return ret;
	}
	return 0;
}

static int configure_references(struct regmap *map,
				const struct s7d_vp9_hw_picture_plan *p)
{
	u32 i, j;
	int ret;

	for (j = 0; j < 2; j++) {
		ret = regmap_write(map, HEVCD_ANC_CANVAS_ACCESS, (j * 16 << 8) | 1);
		if (ret)
			return ret;
		for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
			ret = regmap_write(map, HEVCD_ANC_CANVAS_DATA, p->reference_canvas[i]);
			if (ret)
				return ret;
		}
	}
	ret = regmap_write(map, VP9_REFINFO_ACCESS, BIT(2));
	if (ret)
		return ret;
	for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++)
		for (j = 0; j < 5; j++) {
			ret = regmap_write(map, VP9_REFINFO_DATA, p->reference_info[i][j]);
			if (ret)
				return ret;
		}
	{
		const struct reg_sequence seq[] = {
			{ HEVC_MPRED_CTRL3, 0x24122412 },
			{ HEVC_MPRED_ABOVE, p->mv_above },
		};

		ret = write_many(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(map, HEVC_MPRED_CTRL4, BIT(1) | BIT(6),
				BIT(1) | (p->use_prev_frame_mvs ? BIT(6) : 0));
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_MPRED_MV_WR_START, p->current_mv },
			{ HEVC_MPRED_MV_WPTR, p->current_mv },
			{ HEVC_MPRED_MV_RD_START, p->previous_mv },
			{ HEVC_MPRED_MV_RPTR, p->previous_mv },
			{ HEVC_MPRED_MV_RD_END, p->previous_mv_end },
		};

		return write_many(map, seq, ARRAY_SIZE(seq));
	}
}

static int configure_cache(struct regmap *map, const struct s7d_vp9_hw_picture_plan *p)
{
	u32 last = p->reference_canvas[0] & 0xffff;
	u32 golden = p->reference_canvas[1] & 0xffff;
	int ret;

	ret = regmap_write(map, HEVCD_MCRCC_CTRL1, 2);
	if (ret)
		return ret;
	if (p->intra)
		return regmap_write(map, HEVCD_MCRCC_CTRL1, 0);
	{
		/* Selected vendor's supported LAST/GOLDEN fallback cache policy. */
		const struct reg_sequence seq[] = {
			{ HEVCD_MCRCC_CTRL2, last | (last << 16) },
			{ HEVCD_MCRCC_CTRL3, golden | (golden << 16) },
			{ HEVCD_MCRCC_CTRL1, 0xff0 },
		};

		return write_many(map, seq, ARRAY_SIZE(seq));
	}
}

int s7d_vp9_inter_picture_configure(struct regmap *map,
				   const struct s7d_vp9_picture_frame *frame)
{
	struct s7d_vp9_hw_picture_plan p;
	u32 i;
	int ret;

	if (!map)
		return -EINVAL;
	ret = s7d_vp9_inter_picture_plan(frame, &p);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVCD_ANC2AXI_CONFIG, 6);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(p.anc2axi); i++) {
		ret = regmap_write(map, HEVCD_ANC2AXI_DATA, p.anc2axi[i]);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence seq[] = {
			{ HEVCD_ANC2AXI_CONFIG, 1 },
			{ HEVCD_ANC_CANVAS_ACCESS, 1 },
		};

		ret = write_many(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	for (i = 0; i < 32; i++) {
		ret = regmap_write(map, HEVCD_ANC_CANVAS_DATA, 0);
		if (ret)
			return ret;
	}
	ret = regmap_write(map, HEVC_PARSER_PICTURE_SIZE, p.picture.size);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, HEVC_SAO_CTRL5,
				BIT(9) | BIT(10) | (0xff << 16), BIT(9));
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVCD_DECOMP_CTRL1, BIT(31) },
			{ HEVC_CM_BODY_LENGTH, p.picture.body_bytes },
			{ HEVC_CM_HEADER_OFFSET, p.picture.body_bytes },
			{ HEVC_CM_HEADER_LENGTH, p.picture.header_bytes },
			{ VP9_REF_SCALE_ENABLE, 0 },
		};

		ret = write_many(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	if (p.intra) {
		ret = regmap_update_bits(map, HEVC_MPRED_CTRL4, BIT(1) | BIT(6), BIT(1));
		if (ret)
			return ret;
		ret = regmap_update_bits(map, HEVC_MPRED_CTRL0, 3 << 9, 0);
	} else {
		ret = configure_references(map, &p);
	}
	if (ret)
		return ret;
	ret = configure_cache(map, &p);
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SAO_Y_START, p.current_y },
			{ HEVC_SAO_C_START, p.current_uv },
			{ HEVC_SAO_Y_WPTR, p.current_y },
			{ HEVC_SAO_C_WPTR, p.current_uv },
			{ HEVC_SAO_Y_LENGTH, p.y_bytes },
			{ HEVC_SAO_C_LENGTH, p.uv_bytes },
			{ HEVC_DBLK_CFGB, p.picture.dblk_cfgb },
		};

		ret = write_many(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(map, HEVC_SAO_CTRL1, 0xfff3, p.picture.sao_ctrl1);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, HEVCD_IPP_AXIIF_CONFIG, 0x133f, p.picture.ipp_axiif);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(p.picture.threshold); i++) {
		ret = regmap_write(map, HEVC_DBLK_CFG9, p.picture.threshold[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(p.picture.level); i++) {
		ret = regmap_write(map, HEVC_DBLK_CFGA, p.picture.level[i]);
		if (ret)
			return ret;
	}
	return 0;
}
