// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "../common/s7d-codec-layout.h"
#include "s7d-hevc-context.h"
#include "s7d-hevc-dpb.h"
#include "s7d-hevc-input.h"
#include "s7d-hevc-picture.h"
#include "s7d-hevc-rpm.h"
#include "s7d-hevc-sequence.h"

/* S7D v1 DOS word addresses; s7 compatibility does not remap these registers. */
#define HREG(word)			((word) * 4U)
#define HEVC_SCRATCH(n)			HREG(0x30c0 + (n))
#define HEVC_CABAC_CONTROL		HREG(0x3110)
#define HEVC_PARSER_CMD_WRITE		HREG(0x3112)
#define HEVC_PARSER_CORE_CONTROL		HREG(0x3113)
#define HEVC_PARSER_CMD_SKIP_0		HREG(0x3128)
#define HEVC_PARSER_CMD_SKIP_1		HREG(0x3129)
#define HEVC_PARSER_CMD_SKIP_2		HREG(0x312a)
#define HEVC_MPRED_CTRL0			HREG(0x3201)
#define HEVC_MPRED_CTRL1			HREG(0x3202)
#define HEVC_MPRED_PIC_SIZE		HREG(0x3205)
#define HEVC_MPRED_PIC_SIZE_LCU		HREG(0x3206)
#define HEVC_MPRED_TILE_START		HREG(0x3207)
#define HEVC_MPRED_TILE_SIZE_LCU		HREG(0x3208)
#define HEVC_MPRED_REF_NUM		HREG(0x3209)
#define HEVC_MPRED_LT_REF			HREG(0x320a)
#define HEVC_MPRED_REF_EN_L0		HREG(0x320c)
#define HEVC_MPRED_REF_EN_L1		HREG(0x320d)
#define HEVC_MPRED_ABV_START_ADDR		HREG(0x3212)
#define HEVC_MPRED_MV_WR_START_ADDR	HREG(0x3213)
#define HEVC_MPRED_MV_RD_START_ADDR	HREG(0x3214)
#define HEVC_MPRED_MV_WPTR		HREG(0x3215)
#define HEVC_MPRED_MV_RPTR		HREG(0x3216)
#define HEVC_MPRED_MV_WR_ROW_JUMP		HREG(0x3217)
#define HEVC_MPRED_MV_RD_ROW_JUMP		HREG(0x3218)
#define HEVC_MPRED_L0_REF_POC(n)		HREG(0x3220 + (n))
#define HEVC_MPRED_L1_REF_POC(n)		HREG(0x3230 + (n))
#define HEVC_MPRED_CUR_POC		HREG(0x3260)
#define HEVC_MPRED_COL_POC		HREG(0x3261)
#define HEVC_MPRED_MV_RD_END_ADDR		HREG(0x3262)
#define HEVCD_IPP_TOP_CNTL		HREG(0x3400)
#define HEVCD_IPP_LINEBUFF_BASE		HREG(0x3409)
#define HEVCD_IPP_AXIIF_CONFIG		HREG(0x340b)
#define HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR	HREG(0x3460)
#define HEVCD_MPP_ANC2AXI_TBL_DATA		HREG(0x3464)
#define HEVCD_MPP_ANC_CANVAS_ACCESS	HREG(0x34c0)
#define HEVCD_MPP_ANC_CANVAS_DATA		HREG(0x34c1)
#define HEVCD_MPP_DECOMP_CTL1		HREG(0x34c2)
#define HEVCD_MCRCC_CTL1			HREG(0x34f0)
#define HEVCD_MCRCC_CTL2			HREG(0x34f1)
#define HEVCD_MCRCC_CTL3			HREG(0x34f2)
#define HEVC_DBLK_CFG1			HREG(0x3501)
#define HEVC_DBLK_CFG2			HREG(0x3502)
#define HEVC_DBLK_CFG3			HREG(0x3503)
#define HEVC_DBLK_CFG4			HREG(0x3504)
#define HEVC_DBLK_CFG5			HREG(0x3505)
#define HEVC_DBLK_CFG9			HREG(0x3509)
#define HEVC_DBLK_CFGB			HREG(0x350b)
#define HEVC_DBLK_CFGE			HREG(0x350e)
#define HEVC_DBLK_STS1_SELECT		HREG(0x3510)
#define HEVC_SAO_CTRL0			HREG(0x3601)
#define HEVC_SAO_CTRL1			HREG(0x3602)
#define HEVC_SAO_PIC_SIZE			HREG(0x3605)
#define HEVC_SAO_PIC_SIZE_LCU		HREG(0x3606)
#define HEVC_SAO_Y_START_ADDR		HREG(0x360b)
#define HEVC_SAO_Y_LENGTH			HREG(0x360c)
#define HEVC_SAO_C_START_ADDR		HREG(0x360d)
#define HEVC_SAO_C_LENGTH			HREG(0x360e)
#define HEVC_SAO_Y_WPTR			HREG(0x360f)
#define HEVC_SAO_C_WPTR			HREG(0x3610)
#define HEVC_SAO_CTRL5			HREG(0x3623)

enum picture_rpm_word {
	RPM_COL_FROM_L0 = 0x1d,
	RPM_MISC_FLAGS = 0x3a,
	RPM_MERGE_LEVEL = 0x1f,
	RPM_FIVE_MINUS_MERGE = 0x20,
	RPM_PPS_BETA = 0x3b,
	RPM_PPS_TC = 0x3c,
	RPM_SLICE_BETA = 0x3d,
	RPM_SLICE_TC = 0x3e,
	RPM_PPS_CB_QP = 0x3f,
	RPM_PPS_CR_QP = 0x40,
};

/* Return the first write error and never continue programming after it. */
static int hevc_write_many(struct regmap *map, const struct reg_sequence *seq,
			   size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = regmap_write(map, seq[i].reg, seq[i].def);
		if (ret)
			return ret;
	}
	return 0;
}

static int hevc_region_check(const struct s7d_hevc_dma_region *region,
			     u32 minimum)
{
	if (!region->address || !IS_ALIGNED(region->address, 4096) ||
	    region->bytes < minimum ||
	    (u64)region->address + region->bytes > (1ULL << 32))
		return -EINVAL;
	return 0;
}

static int hevc_workspace_check(const struct s7d_hevc_workspace *ws, u32 mv_bytes)
{
	const struct s7d_hevc_dma_region *region[] = {
		&ws->ipp, &ws->short_term_rps, &ws->vps, &ws->sps, &ws->pps,
		&ws->scalelut, &ws->dblk_para, &ws->dblk_data,
		&ws->dblk_data2, &ws->mpred_above, &ws->rpm, &ws->lmem,
		&ws->swap1, &ws->swap2, &ws->aux, &ws->mv,
	};
	const u32 minimum[] = {
		0x8000, 0x800, 0x800, 0x800, 0x2000, 0x8000,
		0x40100, 0x140800, 0x100800, 0x8000, 0x100, 0xc00,
		0x800, 0x800, 0x6000, mv_bytes,
	};
	u32 i, j;
	int ret;

	for (i = 0; i < ARRAY_SIZE(region); i++) {
		ret = hevc_region_check(region[i], minimum[i]);
		if (ret)
			return ret;
		for (j = 0; j < i; j++)
			if ((u64)region[i]->address <
			    (u64)region[j]->address + region[j]->bytes &&
			    (u64)region[j]->address <
			    (u64)region[i]->address + region[i]->bytes)
				return -EINVAL;
	}
	if (ws->sao_up.bytes || ws->sao_abv.bytes || ws->sao_vb.bytes ||
	    !ws->sao_up.address || !ws->sao_abv.address || !ws->sao_vb.address ||
	    !IS_ALIGNED(ws->sao_up.address | ws->sao_abv.address |
			ws->sao_vb.address, 4096))
		return -EINVAL;
	return 0;
}

int s7d_hevc_workspace_program(struct regmap *map,
			       const struct s7d_hevc_workspace *ws)
{
	int ret;

	if (!map || !ws)
		return -EINVAL;
	ret = hevc_workspace_check(ws, 1);
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVCD_IPP_LINEBUFF_BASE, ws->ipp.address },
			{ HEVC_SCRATCH(1), ws->rpm.address },
			{ HEVC_SCRATCH(2), ws->short_term_rps.address },
			{ HEVC_SCRATCH(3), ws->vps.address },
			{ HEVC_SCRATCH(4), ws->sps.address },
			{ HEVC_SCRATCH(5), ws->pps.address },
			{ HEVC_SCRATCH(6), ws->sao_up.address },
			{ HEVC_SCRATCH(13), ws->scalelut.address },
			{ HEVC_DBLK_CFG3, 0x8020 },
			{ HEVC_DBLK_CFG4, ws->dblk_para.address },
			{ HEVC_DBLK_CFG5, ws->dblk_data.address },
			{ HEVC_DBLK_CFGE, ws->dblk_data2.address },
			{ HEVC_SCRATCH(15), ws->lmem.address },
		};

		return hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
}

int s7d_hevc_protocol_configure(struct regmap *map)
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
	static const struct reg_sequence seq[] = {
		{ HEVC_PARSER_CMD_SKIP_0, 0x0000090b },
		{ HEVC_PARSER_CMD_SKIP_1, 0x1b14140f },
		{ HEVC_PARSER_CMD_SKIP_2, 0x001b1910 },
		{ HEVCD_IPP_TOP_CNTL, 1 },
		{ HEVCD_IPP_TOP_CNTL, 2 },
		{ HEVCD_MPP_DECOMP_CTL1, BIT(31) },
	};
	u32 i;
	int ret;

	if (!map)
		return -EINVAL;
	ret = regmap_update_bits(map, HEVC_CABAC_CONTROL, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, HEVC_PARSER_CORE_CONTROL, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVC_PARSER_CORE_CONTROL, 1);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVC_PARSER_CMD_WRITE, BIT(16));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(parser_cmd); i++) {
		ret = regmap_write(map, HEVC_PARSER_CMD_WRITE, parser_cmd[i]);
		if (ret)
			return ret;
	}
	return hevc_write_many(map, seq, ARRAY_SIZE(seq));
}

static u32 hevc_canvas(u32 slot)
{
	u32 y = slot * 2;
	u32 uv = y + 1;

	return (uv << 16) | (uv << 8) | y;
}

static int hevc_mc_program(struct regmap *map,
			   const struct s7d_hevc_picture_plan *refs)
{
	u32 list, i;
	int ret;

	if (refs->slice_type == 2)
		return 0;
	for (list = 0; list < (refs->slice_type == 0 ? 2 : 1); list++) {
		ret = regmap_write(map, HEVCD_MPP_ANC_CANVAS_ACCESS,
				   (list * 16 << 8) | 1);
		if (ret)
			return ret;
		for (i = 0; i < refs->list_count[list]; i++) {
			ret = regmap_write(map, HEVCD_MPP_ANC_CANVAS_DATA,
					   hevc_canvas(refs->list[list][i].slot));
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int hevc_mcrcc_canvas(struct regmap *map, u32 slot, u32 *canvas)
{
	u32 value;
	int ret;

	ret = regmap_read(map, HEVCD_MPP_ANC_CANVAS_DATA, &value);
	if (ret)
		return ret;
	value &= 0xffff;
	if (value != (hevc_canvas(slot) & 0xffff))
		return -EIO;
	*canvas = value | (value << 16);
	return 0;
}

static int hevc_mcrcc_program(struct regmap *map,
			      const struct s7d_hevc_picture_plan *refs)
{
	u32 first, second;
	int ret;

	ret = regmap_write(map, HEVCD_MCRCC_CTL1, 2);
	if (ret)
		return ret;
	if (refs->slice_type == 2)
		return regmap_write(map, HEVCD_MCRCC_CTL1, 0);
	ret = regmap_write(map, HEVCD_MPP_ANC_CANVAS_ACCESS,
			   refs->slice_type == 0 ? 0 : BIT(1));
	if (ret)
		return ret;
	ret = hevc_mcrcc_canvas(map, refs->list[0][0].slot, &first);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVCD_MCRCC_CTL2, first);
	if (ret)
		return ret;
	if (refs->slice_type == 0) {
		ret = regmap_write(map, HEVCD_MPP_ANC_CANVAS_ACCESS,
				   (16 << 8) | BIT(1));
		if (ret)
			return ret;
		ret = hevc_mcrcc_canvas(map, refs->list[1][0].slot, &second);
		if (ret)
			return ret;
		if (first == second && refs->list_count[1] > 1) {
			ret = hevc_mcrcc_canvas(map, refs->list[1][1].slot, &second);
			if (ret)
				return ret;
		}
	} else if (refs->list_count[0] == 1) {
		second = first;
	} else {
		ret = hevc_mcrcc_canvas(map, refs->list[0][1].slot, &second);
		if (ret)
			return ret;
	}
	ret = regmap_write(map, HEVCD_MCRCC_CTL3, second);
	if (ret)
		return ret;
	return regmap_write(map, HEVCD_MCRCC_CTL1, 0xff0);
}

static int hevc_picture_program(struct regmap *map,
				const struct s7d_hevc_workspace *ws,
				const struct s7d_hevc_rpm *rpm,
				const struct s7d_hevc_sps *host,
				const struct s7d_codec_layout *layout,
				const struct s7d_hevc_geometry *geometry,
				const struct s7d_hevc_picture_buffers *buffers,
				const struct s7d_hevc_slice_info *slice,
				const struct s7d_hevc_picture_plan *refs)
{
	u32 slot = refs->current_slot;
	const struct s7d_hevc_surface *surface = &buffers->surface[slot];
	const struct s7d_hevc_dma_region *write_mv = &buffers->mv[slot];
	bool col_valid = refs->collocated_valid;
	const struct s7d_hevc_dma_region *read_mv = col_valid ?
		&buffers->mv[refs->collocated.slot] : write_mv;
	u32 slice_type = refs->slice_type;
	u32 col_from_l0 = slice->collocated_from_l0;
	u32 l0_count = refs->list_count[0];
	u32 l1_count = refs->list_count[1];
	u32 log2_ctb, pic_size, lcu_size, tile_size, sao_unit;
	u32 mpred0, mpred1, dblk1, dblk9 = 0, sao_flags = 0;
	u32 flags, disabled, beta, tc, across;
	u32 i;
	int ret;

	log2_ctb = host->log2_ctb_size;
	pic_size = geometry->coded_width | (geometry->coded_height << 16);
	lcu_size = (geometry->ctb_columns - 1) | ((geometry->ctb_rows - 1) << 16);
	tile_size = geometry->ctb_columns | (geometry->ctb_rows << 16);
	sao_unit = (geometry->ctb_size == 16 ? 9 : geometry->ctb_size == 32 ? 14 : 24) << 4;
	flags = rpm->word[RPM_MISC_FLAGS];
	mpred0 = slice_type | BIT(2) | BIT(3) | BIT(9) | BIT(10) |
		 BIT(13) | (col_from_l0 << 7) |
		 (col_valid ? 0 : BIT(12)) |
		 (slice_type == 2 ? 0 : BIT(11)) |
		 (refs->temporal_mvp ? BIT(5) : 0) |
		 (refs->low_delay ? BIT(6) : 0) |
		 (log2_ctb << 16) | (3 << 20) |
		 (rpm->word[RPM_MERGE_LEVEL] << 24);
	mpred1 = (5 - rpm->word[RPM_FIVE_MINUS_MERGE]) |
		 (2 << 4) | (3 << 8) | (5 << 12) | (36 << 16);
	/* Vendor consumes raw low QP/beta/tc bits; high-word ABI is undocumented. */
	dblk1 = (geometry->ctb_size == 64 ? 0 : geometry->ctb_size == 32 ? 1 : 2) |
		 ((flags & BIT(1) ? flags & BIT(0) : 0) << 3) |
		 ((rpm->word[RPM_PPS_CB_QP] & 0x1f) << 4) |
		 ((rpm->word[RPM_PPS_CR_QP] & 0x1f) << 9) |
		 (geometry->coded_width <= 64 ? BIT(20) : 0);
	disabled = !!(flags & BIT(7));
	if ((flags & (BIT(4) | BIT(6))) == (BIT(4) | BIT(6))) {
		beta = rpm->word[RPM_SLICE_BETA];
		tc = rpm->word[RPM_SLICE_TC];
	} else {
		dblk9 = !!(flags & BIT(5)) << 2;
		beta = rpm->word[RPM_PPS_BETA];
		tc = rpm->word[RPM_PPS_TC];
	}
	if ((flags & (BIT(4) | BIT(6))) == (BIT(4) | BIT(6)))
		dblk9 = disabled << 2;
	if (!(dblk9 & BIT(2)))
		dblk9 |= (beta & 0xf) << 3 | (tc & 0xf) << 7;
	across = flags & BIT(3) && ((flags & (BIT(8) | BIT(9))) || !disabled) ?
		 !!(flags & BIT(10)) : !!(flags & BIT(3));
	dblk9 |= across << 1;
	sao_flags = across << 9;
	ret = regmap_write(map, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 6);
	if (ret)
		return ret;
	for (i = 0; i < buffers->count; i++) {
		ret = regmap_write(map, HEVCD_MPP_ANC2AXI_TBL_DATA,
				   buffers->surface[i].address[0] >> 5);
		if (ret)
			return ret;
		ret = regmap_write(map, HEVCD_MPP_ANC2AXI_TBL_DATA,
				   buffers->surface[i].address[1] >> 5);
		if (ret)
			return ret;
	}
	ret = regmap_write(map, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 1);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVCD_MPP_ANC_CANVAS_ACCESS, 1);
	if (ret)
		return ret;
	for (i = 0; i < 32; i++) {
		ret = regmap_write(map, HEVCD_MPP_ANC_CANVAS_DATA, 0);
		if (ret)
			return ret;
	}
	ret = hevc_mc_program(map, refs);
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SCRATCH(9), sao_unit },
			{ HEVC_SCRATCH(10), ws->sao_abv.address },
			{ HEVC_SCRATCH(11), (sao_unit + 32) * geometry->ctb_rows },
			{ HEVC_SCRATCH(12), ws->sao_vb.address },
		};

		ret = hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
	if (ret)
		return ret;
	ret = hevc_mcrcc_program(map, refs);
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_MPRED_MV_WR_START_ADDR, write_mv->address },
			{ HEVC_MPRED_MV_RD_START_ADDR, read_mv->address },
			{ HEVC_MPRED_MV_WR_ROW_JUMP, 0 },
			{ HEVC_MPRED_MV_RD_ROW_JUMP, 0 },
			{ HEVC_MPRED_CTRL0, mpred0 },
			{ HEVC_MPRED_CTRL1, mpred1 },
			{ HEVC_MPRED_PIC_SIZE, pic_size },
			{ HEVC_MPRED_PIC_SIZE_LCU, lcu_size },
			{ HEVC_MPRED_TILE_START, 0 },
			{ HEVC_MPRED_TILE_SIZE_LCU, tile_size },
			{ HEVC_MPRED_REF_NUM, l0_count | (l1_count << 8) },
			{ HEVC_MPRED_LT_REF, 0 },
			{ HEVC_MPRED_REF_EN_L0, (1U << l0_count) - 1 },
			{ HEVC_MPRED_REF_EN_L1, (1U << l1_count) - 1 },
			{ HEVC_MPRED_CUR_POC, (u32)refs->poc },
		};

		ret = hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
	if (ret)
		return ret;
	if (col_valid) {
		ret = regmap_write(map, HEVC_MPRED_COL_POC,
				   (u32)refs->collocated.poc);
		if (ret)
			return ret;
	}
	/* Hardware requires L0[n], L1[n] pairs even when both lists are disabled. */
	for (i = 0; i < 16; i++) {
		ret = regmap_write(map, HEVC_MPRED_L0_REF_POC(i),
				   i < l0_count ? (u32)refs->list[0][i].poc : 0);
		if (ret)
			return ret;
		ret = regmap_write(map, HEVC_MPRED_L1_REF_POC(i),
				   i < l1_count ? (u32)refs->list[1][i].poc : 0);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence seq[] = {
			{ HEVC_MPRED_ABV_START_ADDR, ws->mpred_above.address },
			{ HEVC_MPRED_MV_WPTR, write_mv->address },
			{ HEVC_MPRED_MV_RPTR, read_mv->address },
			{ HEVC_MPRED_MV_RD_END_ADDR, read_mv->address + read_mv->bytes },
		};

		ret = hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
	if (ret)
		return ret;
	ret = regmap_update_bits(map, HEVC_SAO_CTRL0, 0xf, log2_ctb);
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SAO_PIC_SIZE, pic_size },
			{ HEVC_SAO_PIC_SIZE_LCU, lcu_size },
			{ HEVC_SAO_Y_START_ADDR, U32_MAX },
		};

		ret = hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
	if (ret)
		return ret;
	ret = regmap_update_bits(map, HEVC_SAO_CTRL5, GENMASK(23, 16), 0);
	if (ret)
		return ret;
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SAO_Y_START_ADDR, surface->address[0] },
			{ HEVC_SAO_Y_LENGTH, layout->size[0] },
			{ HEVC_SAO_C_START_ADDR, surface->address[1] },
			{ HEVC_SAO_C_LENGTH, layout->size[1] },
			{ HEVC_SAO_Y_WPTR, surface->address[0] },
			{ HEVC_SAO_C_WPTR, surface->address[1] },
			{ HEVC_DBLK_CFGB, 0xfe00 |
				(geometry->coded_width >= 1280 ? BIT(4) : 0) },
			{ HEVC_DBLK_CFG2, pic_size },
			{ HEVC_DBLK_CFG1, dblk1 },
			{ HEVC_DBLK_STS1_SELECT, BIT(28) },
		};

		ret = hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
	if (ret)
		return ret;
	/* Linear, little-endian NV12, 64-byte lines, compressed output disabled. */
	ret = regmap_update_bits(map, HEVC_SAO_CTRL1, 0xfff3, 0x8ff1);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, HEVCD_IPP_AXIIF_CONFIG,
				 0x133f, 0x20f);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVC_DBLK_CFG9, dblk9);
	if (ret)
		return ret;
	return regmap_update_bits(map, HEVC_SAO_CTRL0, GENMASK(9, 8), sao_flags);
}

static bool hevc_overlaps(u32 address, u32 bytes, u32 other, u32 other_bytes)
{
	return (u64)address < (u64)other + other_bytes &&
	       (u64)other < (u64)address + bytes;
}

static int hevc_buffers_check(const struct s7d_hevc_workspace *ws,
			      const struct s7d_codec_layout *layout,
			      const struct s7d_hevc_geometry *geometry,
			      const struct s7d_hevc_picture_buffers *buffers)
{
	const struct s7d_hevc_dma_region *region[] = {
		&ws->ipp, &ws->short_term_rps, &ws->vps, &ws->sps, &ws->pps,
		&ws->scalelut, &ws->dblk_para, &ws->dblk_data, &ws->dblk_data2,
		&ws->mpred_above, &ws->rpm, &ws->lmem, &ws->swap1, &ws->swap2,
		&ws->aux, &ws->mv,
	};
	u32 unit = geometry->ctb_size == 64 ? 0x200 :
		   geometry->ctb_size == 32 ? 0x80 : 0x20;
	u32 used = geometry->ctb_count * unit;
	u32 stride = ALIGN(used, 0x10000);
	u32 count = buffers->count;
	u32 i, j, plane, other;
	int ret;

	if (count < layout->min_capture_buffers ||
	    count > S7D_HEVC_MAX_CAPTURE_BUFFERS)
		return -EINVAL;
	ret = hevc_workspace_check(ws, count * stride);
	if (ret)
		return ret;
	if (!IS_ALIGNED(ws->mv.address, 0x10000) ||
	    ws->mv.bytes != count * stride)
		return -ESTALE;
	for (i = 0; i < count; i++) {
		const struct s7d_hevc_dma_region *mv = &buffers->mv[i];

		if ((u64)mv->address != (u64)ws->mv.address + i * stride ||
		    mv->bytes != used)
			return -ESTALE;
		if ((u64)mv->address + mv->bytes > U32_MAX)
			return -ERANGE;
		for (plane = 0; plane < 2; plane++) {
			const struct s7d_hevc_surface *surface = &buffers->surface[i];
			u32 address = surface->address[plane];
			u32 bytes = surface->bytes[plane];

			if (!address || !IS_ALIGNED(address, 64) ||
			    bytes < layout->size[plane] ||
			    (u64)address + bytes > (1ULL << 32))
				return -EINVAL;
			for (j = 0; j < ARRAY_SIZE(region); j++)
				if (hevc_overlaps(address, bytes, region[j]->address,
						  region[j]->bytes))
					return -EINVAL;
			for (j = 0; j <= i; j++)
				for (other = 0; other < 2; other++) {
					if (j == i && other >= plane)
						continue;
					if (hevc_overlaps(address, bytes,
						buffers->surface[j].address[other],
						buffers->surface[j].bytes[other]))
						return -EINVAL;
				}
		}
	}
	return 0;
}

static int hevc_references_check(const struct s7d_hevc_slice_info *slice,
				 const struct s7d_hevc_picture_plan *plan,
				 u32 count)
{
	const struct s7d_hevc_picture_ref *col;
	bool low_delay = plan->slice_type != 2;
	u32 list, i, other_list, j;

	if (plan->kind != S7D_HEVC_PLAN_PICTURE || !plan->generation ||
	    !plan->plan_id || !plan->parameter_epoch ||
	    plan->parameter_epoch != slice->parameter_epoch ||
	    plan->slot_count != count || plan->current_slot >= count ||
	    plan->slice_type != slice->slice_type ||
	    plan->temporal_mvp != slice->temporal_mvp ||
	    slice->rps_count > S7D_HEVC_RPS_ENTRIES ||
	    plan->retain_mask & ~((1U << count) - 1) ||
	    plan->retain_mask & BIT(plan->current_slot))
		return -ESTALE;
	for (i = 0; i < slice->rps_count; i++)
		if (slice->rps[i].long_term)
			return -EOPNOTSUPP;
	if (plan->list_count[0] != slice->active[0] ||
	    plan->list_count[1] != slice->active[1] ||
	    plan->list_count[0] > S7D_HEVC_RPS_ENTRIES ||
	    plan->list_count[1] > S7D_HEVC_RPS_ENTRIES)
		return -EPROTO;
	if (plan->slice_type == 2) {
		if (plan->list_count[0] || plan->list_count[1] ||
		    plan->collocated_valid || plan->low_delay)
			return -EPROTO;
		return 0;
	}
	if (!plan->list_count[0] || !plan->collocated_valid ||
	    (plan->slice_type == 0 && !plan->list_count[1]) ||
	    (plan->slice_type == 1 && plan->list_count[1]))
		return -EPROTO;
	for (list = 0; list < 2; list++)
		for (i = 0; i < plan->list_count[list]; i++) {
			const struct s7d_hevc_picture_ref *ref = &plan->list[list][i];

			if (ref->slot >= count || ref->slot == plan->current_slot ||
			    ref->poc == plan->poc ||
			    !(plan->retain_mask & BIT(ref->slot)))
				return -EPROTO;
			if (ref->poc > plan->poc)
				low_delay = false;
			for (other_list = 0; other_list < 2; other_list++)
				for (j = 0; j < plan->list_count[other_list]; j++) {
					const struct s7d_hevc_picture_ref *other =
						&plan->list[other_list][j];

					if ((ref->slot == other->slot) !=
					    (ref->poc == other->poc))
						return -EPROTO;
				}
		}
	if (low_delay != plan->low_delay)
		return -EPROTO;
	list = plan->slice_type == 1 || slice->collocated_from_l0 ? 0 : 1;
	if (slice->collocated_ref_idx >= plan->list_count[list])
		return -EPROTO;
	col = &plan->list[list][slice->collocated_ref_idx];
	if (plan->collocated.slot != col->slot || plan->collocated.poc != col->poc)
		return -EPROTO;
	return 0;
}

int s7d_hevc_picture_configure(struct regmap *map,
			       const struct s7d_hevc_workspace *ws,
			       const struct s7d_hevc_rpm *rpm,
			       const struct s7d_hevc_sps *host,
			       const struct s7d_codec_layout *layout,
			       const struct s7d_hevc_picture_buffers *buffers,
			       const struct s7d_hevc_slice_info *slice,
			       const struct s7d_hevc_picture_plan *plan)
{
	struct s7d_codec_layout expected;
	struct s7d_hevc_geometry geometry;
	struct s7d_hevc_slice_info checked;
	int ret;

	if (!map || !ws || !rpm || !host || !layout || !buffers ||
	    !buffers->surface || !buffers->mv || !slice || !plan)
		return -EINVAL;
	ret = s7d_hevc_rpm_check_sequence(rpm, host, &geometry);
	if (ret)
		return ret;
	ret = s7d_hevc_rpm_decode_slice(rpm, host, slice, &checked);
	if (ret)
		return ret;
	if (layout->format != S7D_CODEC_CAPTURE_NV12M)
		return -EOPNOTSUPP;
	ret = s7d_hevc_build_layout(host, layout->format, layout->extra_surfaces,
				    &expected);
	if (ret)
		return ret;
	if (memcmp(layout, &expected, sizeof(expected)))
		return -ESTALE;
	if (rpm->word[RPM_MERGE_LEVEL] > 7 ||
	    rpm->word[RPM_FIVE_MINUS_MERGE] > 4 ||
	    rpm->word[RPM_MISC_FLAGS] & ~GENMASK(10, 0))
		return -EPROTO;
	ret = hevc_buffers_check(ws, layout, &geometry, buffers);
	if (ret)
		return ret;
	ret = hevc_references_check(&checked, plan, buffers->count);
	if (ret)
		return ret;
	return hevc_picture_program(map, ws, rpm, host, layout, &geometry,
				    buffers, &checked, plan);
}
