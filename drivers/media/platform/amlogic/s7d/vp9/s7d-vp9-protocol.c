// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/regmap.h>

#include "s7d-vp9-protocol.h"

#define HREG(word)			((word) * 4U)
#define HEVC_ASSIST_MMU_MAP_ADDR		HREG(0x3009)
#define HEVC_SCRATCH(n)			HREG(0x30c0 + (n))
#define HEVC_SHIFT_CONTROL		HREG(0x3108)
#define HEVC_PARSER_CMD_WRITE		HREG(0x3112)
#define HEVC_PARSER_CMD_SKIP0		HREG(0x3128)
#define HEVC_PARSER_CMD_SKIP1		HREG(0x3129)
#define HEVC_PARSER_CMD_SKIP2		HREG(0x312a)
#define HEVC_MPRED_CTRL3			HREG(0x321d)
#define HEVC_MPRED_CTRL4			HREG(0x324c)
#define HEVCD_IPP_LINEBUFFER		HREG(0x3409)
#define HEVCD_ANC2AXI_CONFIG		HREG(0x3460)
#define HEVCD_ANC2AXI_DATA		HREG(0x3464)
#define HEVCD_ANC_CANVAS_ACCESS		HREG(0x34c0)
#define HEVCD_ANC_CANVAS_DATA		HREG(0x34c1)
#define HEVCD_DECOMP_CTRL1		HREG(0x34c2)
#define HEVCD_DECOMP_CTRL2		HREG(0x34c3)
#define HEVC_DBLK_CFG3			HREG(0x3503)
#define HEVC_DBLK_CFG4			HREG(0x3504)
#define HEVC_DBLK_CFG5			HREG(0x3505)
#define HEVC_DBLK_CFG9			HREG(0x3509)
#define HEVC_DBLK_CFGB			HREG(0x350b)
#define HEVC_DBLK_CFGE			HREG(0x350e)
#define HEVC_CM_BODY_LENGTH		HREG(0x3627)
#define HEVC_CM_HEADER_LENGTH		HREG(0x3629)
#define HEVC_SAO_CTRL5			HREG(0x3623)
#define HEVC_CM_HEADER_OFFSET		HREG(0x362b)
#define HEVC_SAO_MMU_VH0_ADDR		HREG(0x363a)
#define HEVC_SAO_MMU_VH1_ADDR		HREG(0x363b)
#define HEVC_PSCALE_CONTROL		HREG(0x3911)

static bool protocol_overlap(const struct s7d_vp9_region *a,
			     const struct s7d_vp9_region *b)
{
	return (u64)a->address < (u64)b->address + b->bytes &&
	       (u64)b->address < (u64)a->address + a->bytes;
}

/*
 * A surface's memory: NV12 Y and UV (none if fbc_only), then its FBC header
 * and body, if any.
 */
static u32 protocol_regions(const struct s7d_vp9_hw_surface *s, bool compressed,
			    struct s7d_vp9_region region[4])
{
	u32 parts = 0;

	if (!s->fbc_only) {
		region[parts++] = s->plane[0];
		region[parts++] = s->plane[1];
	}
	if (compressed && s->header.bytes) {
		region[parts++] = s->header;
		region[parts++] = s->body;
	}
	return parts;
}

static int protocol_validate(const struct s7d_vp9_protocol_frame *f)
{
	struct s7d_vp9_region mine[4], theirs[4];
	const struct s7d_codec_layout *l;
	u32 i, j, a, b, unused, parts, other_parts, nv12;
	int ret;

	if (!f || !f->layout || !f->surface || !f->physical_count ||
	    f->physical_count > S7D_VP9_MAX_PHYSICAL ||
	    (f->bit_depth != 8 && f->bit_depth != 10) ||
	    (!f->compressed && f->bit_depth != 8))
		return -EINVAL;
	l = f->layout;
	ret = s7d_vp9_workspace_validate(f->workspace);
	if (ret)
		return ret;
	ret = s7d_codec_validate_layout(l);
	if (ret)
		return ret;
	ret = s7d_vp9_compressed_mv_size(l->coded_width, l->coded_height, &unused);
	if (ret)
		return ret;
	if (f->compressed &&
	    (!f->workspace->mmu_vbh.bytes || !f->workspace->mmu_map.bytes ||
	     !IS_ALIGNED(f->workspace->mmu_map.address, 4096)))
		return -EINVAL;
	if (l->format != S7D_CODEC_CAPTURE_NV12M || l->visible_left || l->visible_top ||
	    l->visible_width != l->coded_width || l->visible_height != l->coded_height ||
	    l->pitch[0] != ALIGN(l->coded_width, 64) ||
	    l->storage_height != ALIGN(l->coded_height, 64) ||
	    l->size[0] != l->pitch[0] * l->storage_height || l->size[1] != l->size[0] / 2)
		return -EINVAL;
	for (i = 0; i < f->physical_count; i++) {
		const struct s7d_vp9_hw_surface *s = &f->surface[i];

		if (!s->valid)
			continue;
		if (s->width != l->coded_width || s->height != l->coded_height)
			return -EOPNOTSUPP;
		/* Without NV12 a surface is its own FBC, written compressed only. */
		if (s->fbc_only && (!f->compressed || !s->header.bytes))
			return -EINVAL;
		parts = protocol_regions(s, f->compressed, mine);
		nv12 = s->fbc_only ? 0 : 2;
		for (a = 0; a < parts; a++) {
			const struct s7d_vp9_region *p = &mine[a];

			if (!p->address || !IS_ALIGNED(p->address, a < nv12 ? 32 : 4096) ||
			    p->bytes < (a < nv12 ? l->size[a] : 1) ||
			    (u64)p->address + p->bytes > (1ULL << 32) ||
			    protocol_overlap(p, &f->workspace->backing) ||
			    protocol_overlap(p, &f->workspace->rpm) ||
			    protocol_overlap(p, &f->workspace->lmem) ||
			    protocol_overlap(p, &f->workspace->mmu_vbh) ||
			    protocol_overlap(p, &f->workspace->mmu_map))
				return -EINVAL;
			for (j = 0; j <= i; j++) {
				if (!f->surface[j].valid)
					continue;
				other_parts = protocol_regions(&f->surface[j], f->compressed,
							       theirs);
				for (b = 0; b < other_parts; b++) {
					if (j == i && b >= a)
						continue;
					if (protocol_overlap(p, &theirs[b]))
						return -EINVAL;
				}
			}
		}
	}
	return 0;
}

static int protocol_writes(struct regmap *map, const struct reg_sequence *seq, size_t n)
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

int s7d_vp9_protocol_init(struct regmap *map, const struct s7d_vp9_protocol_frame *f)
{
	static const u16 parser_cmd[] = {
		0x0401, 0x8401, 0x0800, 0x0402, 0x9002, 0x1423,
		0x8cc3, 0x1423, 0x8804, 0x9825, 0x0800, 0x04fe,
		0x8406, 0x8411, 0x1800, 0x8408, 0x8409, 0x8c2a,
		0x9c2b, 0x1c00, 0x840f, 0x8407, 0x8000, 0x8408,
		0x2000, 0xa800, 0x8410, 0x04de, 0x840c, 0x840d,
		0xac00, 0xa000, 0x08c0, 0x08e0, 0xa40e, 0xfc00,
		0x7c00,
	};
	const struct s7d_vp9_workspace *ws;
	u32 body, header, i;
	int ret;

	if (!map)
		return -EINVAL;
	ret = protocol_validate(f);
	if (ret)
		return ret;
	ws = f->workspace;
	body = s7d_vp9_fbc_body_bytes(f->layout->coded_width, f->layout->coded_height,
				      f->bit_depth);
	header = s7d_vp9_fbc_header_bytes(f->layout->coded_width, f->layout->coded_height);
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SCRATCH(1), ws->rpm.address },
			{ HEVC_SCRATCH(2), ws->part[S7D_VP9_RPS].address },
			{ HEVC_SCRATCH(5), ws->part[S7D_VP9_PPS].address },
			{ HEVC_SCRATCH(7), ws->part[S7D_VP9_SWAP1].address },
			{ HEVC_SCRATCH(8), ws->part[S7D_VP9_SWAP2].address },
			{ HEVC_SCRATCH(15), ws->lmem.address },
			{ HEVCD_IPP_LINEBUFFER, ws->part[S7D_VP9_IPP].address },
			{ HEVC_DBLK_CFGE, ws->part[S7D_VP9_DBLK_PARAMETERS].address },
			{ HEVC_DBLK_CFG4, ws->part[S7D_VP9_DBLK_PARAMETERS].address },
			{ HEVC_DBLK_CFG5, ws->part[S7D_VP9_DBLK_DATA].address },
			{ HEVC_DBLK_CFG3, 0x808020 },
			/* The body size only without the MMU (vp9_config_work_space_hw). */
			{ HEVCD_DECOMP_CTRL2, f->compressed ? 0 : body >> 5 },
			{ HEVC_CM_BODY_LENGTH, body },
			{ HEVC_CM_HEADER_OFFSET, body },
			{ HEVC_CM_HEADER_LENGTH, header },
			{ HEVC_MPRED_CTRL3, 0x24122412 },
		};

		ret = protocol_writes(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	if (f->compressed) {
		/* Vendor vp9_config_work_space_hw with mmu_enable. */
		const struct reg_sequence seq[] = {
			{ HEVC_SAO_MMU_VH0_ADDR, ws->mmu_vbh.address },
			{ HEVC_SAO_MMU_VH1_ADDR, ws->mmu_vbh.address + ws->mmu_vbh.bytes / 2 },
			{ HEVC_ASSIST_MMU_MAP_ADDR, ws->mmu_map.address },
		};

		ret = protocol_writes(map, seq, ARRAY_SIZE(seq));
		if (!ret)
			ret = regmap_update_bits(map, HEVC_SAO_CTRL5, BIT(10), BIT(10));
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(map, HEVC_MPRED_CTRL4, BIT(1), BIT(1));
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SCRATCH(11), ws->part[S7D_VP9_SEGMENT_MAP].address },
			{ HEVC_SCRATCH(9), ws->part[S7D_VP9_PROBABILITY].address },
			{ HEVC_SCRATCH(10), ws->part[S7D_VP9_COUNTS].address },
			{ HEVCD_ANC2AXI_CONFIG, 6 },
		};

		ret = protocol_writes(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	for (i = 0; i < S7D_VP9_MAX_PHYSICAL * 2; i++) {
		u32 value = 0;

		/* Compressed: one entry per picture, its FBC header (init_pic_list_hw). */
		if (f->compressed && i < f->physical_count && f->surface[i].valid)
			value = f->surface[i].header.address >> 5;
		else if (!f->compressed && i / 2 < f->physical_count && f->surface[i / 2].valid)
			value = f->surface[i / 2].plane[i % 2].address >> 5;
		ret = regmap_write(map, HEVCD_ANC2AXI_DATA, value);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence seq[] = {
			{ HEVCD_ANC2AXI_CONFIG, 1 },
			{ HEVCD_ANC_CANVAS_ACCESS, 1 },
		};

		ret = protocol_writes(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	for (i = 0; i < 32; i++) {
		ret = regmap_write(map, HEVCD_ANC_CANVAS_DATA, 0);
		if (ret)
			return ret;
	}
	ret = regmap_write(map, HEVC_SHIFT_CONTROL, 0x6e7);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVC_SCRATCH(0), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, HEVC_SCRATCH(21), BIT(0) | BIT(1), BIT(0) | BIT(1));
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SCRATCH(19), 0x80000002 },
			{ HEVC_SCRATCH(23), 0 },
			{ HEVC_SCRATCH(22), f->decode_count },
			{ HEVC_PARSER_CMD_WRITE, BIT(16) },
		};

		ret = protocol_writes(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(parser_cmd); i++) {
		ret = regmap_write(map, HEVC_PARSER_CMD_WRITE, parser_cmd[i]);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence seq[] = {
			{ HEVC_PARSER_CMD_SKIP0, 0x0000090b },
			{ HEVC_PARSER_CMD_SKIP1, 0x1b14140f },
			{ HEVC_PARSER_CMD_SKIP2, 0x001b1910 },
			/* MMU: paged reference reads; else NV21 reference read mode. */
			{ HEVCD_DECOMP_CTRL1, f->compressed ? BIT(4) : BIT(31) },
		};

		ret = protocol_writes(map, seq, ARRAY_SIZE(seq));
		if (ret)
			return ret;
	}
	for (i = 0; i < 32; i++) {
		u32 even = i * 2, odd = even + 1;
		u32 low = (max(even, 1U) << 8) | (2 * (even + 2) + max(even, 1U));
		u32 high = (odd << 8) | (2 * (odd + 2) + odd);

		ret = regmap_write(map, HEVC_DBLK_CFG9, (high << 16) | low);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence seq[] = {
			/* [8] compressed write, [9] double write. */
			{ HEVC_DBLK_CFGB, f->compressed ? 0xff01 : 0xfe01 },
			{ HEVC_SCRATCH(14), 1 },
			{ HEVC_PSCALE_CONTROL, 0 },
			{ HEVC_SCRATCH(16), 0 },
			{ HEVC_SCRATCH(18), 8 },
			{ HEVC_SCRATCH(20), 0 },
		};

		return protocol_writes(map, seq, ARRAY_SIZE(seq));
	}
}
