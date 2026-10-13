// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "s7d-codec-layout.h"
#include "s7d-hevc-context.h"
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

static int hevc_workspace_check(const struct s7d_hevc_workspace *ws)
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
		0x800, 0x800, 0x6000, 0x40000,
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
	ret = hevc_workspace_check(ws);
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

static int hevc_picture_check(const struct s7d_hevc_workspace *ws,
			      const struct s7d_hevc_rpm *rpm,
			      const struct s7d_hevc_sps *host,
			      const struct s7d_codec_layout *layout,
			      const struct s7d_hevc_surface *surface,
			      struct s7d_hevc_idr_plan *plan)
{
	struct s7d_codec_layout expected;
	const struct s7d_hevc_dma_region *region[] = {
		&ws->ipp, &ws->short_term_rps, &ws->vps, &ws->sps, &ws->pps,
		&ws->scalelut, &ws->dblk_para, &ws->dblk_data, &ws->dblk_data2,
		&ws->mpred_above, &ws->rpm, &ws->lmem, &ws->swap1, &ws->swap2,
		&ws->aux, &ws->mv,
	};
	u64 end[2];
	u32 i, j, mv_unit;
	int ret;

	ret = s7d_hevc_rpm_plan_idr(rpm, host, plan);
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
	ret = hevc_workspace_check(ws);
	if (ret)
		return ret;
	mv_unit = plan->ctb_size == 64 ? 0x200 :
		  plan->ctb_size == 32 ? 0x80 : 0x20;
	ret = hevc_region_check(&ws->mv, plan->ctb_count * mv_unit);
	if (ret || (u64)ws->mv.address + ws->mv.bytes >= (1ULL << 32))
		return ret ? ret : -ERANGE;
	for (i = 0; i < 2; i++) {
		end[i] = (u64)surface->address[i] + surface->bytes[i];
		if (!surface->address[i] ||
		    !IS_ALIGNED(surface->address[i], 64) ||
		    surface->bytes[i] < layout->size[i] || end[i] > (1ULL << 32))
			return -EINVAL;
		for (j = 0; j < ARRAY_SIZE(region); j++)
			if ((u64)surface->address[i] <
			    (u64)region[j]->address + region[j]->bytes &&
			    (u64)region[j]->address < end[i])
				return -EINVAL;
	}
	if ((u64)surface->address[0] < end[1] &&
	    (u64)surface->address[1] < end[0])
		return -EINVAL;
	if (rpm->word[RPM_COL_FROM_L0] > 1 ||
	    rpm->word[RPM_MERGE_LEVEL] > 7 ||
	    rpm->word[RPM_FIVE_MINUS_MERGE] > 4 ||
	    plan->misc_flags & ~GENMASK(10, 0))
		return -EPROTO;
	return 0;
}

int s7d_hevc_picture_configure_idr(struct regmap *map,
				 const struct s7d_hevc_workspace *ws,
				 const struct s7d_hevc_rpm *rpm,
				 const struct s7d_hevc_sps *host,
				 const struct s7d_codec_layout *layout,
				 const struct s7d_hevc_surface *surface)
{
	struct s7d_hevc_idr_plan plan;
	u32 log2_ctb, pic_size, lcu_size, tile_size, sao_unit;
	u32 mpred0, mpred1, dblk1, dblk9 = 0, sao_flags = 0;
	u32 flags, disabled, beta, tc, across;
	u32 i;
	int ret;

	if (!map || !ws || !rpm || !host || !layout || !surface)
		return -EINVAL;
	ret = hevc_picture_check(ws, rpm, host, layout, surface, &plan);
	if (ret)
		return ret;
	log2_ctb = host->log2_ctb_size;
	pic_size = plan.coded_width | (plan.coded_height << 16);
	lcu_size = (plan.ctb_columns - 1) | ((plan.ctb_rows - 1) << 16);
	tile_size = plan.ctb_columns | (plan.ctb_rows << 16);
	sao_unit = (plan.ctb_size == 16 ? 9 : plan.ctb_size == 32 ? 14 : 24) << 4;
	flags = plan.misc_flags;
	mpred0 = 2 | BIT(2) | BIT(3) | BIT(9) | BIT(10) | BIT(12) |
		 BIT(13) | (rpm->word[RPM_COL_FROM_L0] << 7) |
		 (log2_ctb << 16) | (3 << 20) |
		 (rpm->word[RPM_MERGE_LEVEL] << 24);
	mpred1 = (5 - rpm->word[RPM_FIVE_MINUS_MERGE]) |
		 (2 << 4) | (3 << 8) | (5 << 12) | (36 << 16);
	/* Vendor consumes raw low QP/beta/tc bits; high-word ABI is undocumented. */
	dblk1 = (plan.ctb_size == 64 ? 0 : plan.ctb_size == 32 ? 1 : 2) |
		 ((flags & BIT(1) ? flags & BIT(0) : 0) << 3) |
		 ((rpm->word[RPM_PPS_CB_QP] & 0x1f) << 4) |
		 ((rpm->word[RPM_PPS_CR_QP] & 0x1f) << 9) |
		 (plan.coded_width <= 64 ? BIT(20) : 0);
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
	{
		const struct reg_sequence seq[] = {
			/* One current picture, canvas Y=0 and UV=1. */
			{ HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 6 },
			{ HEVCD_MPP_ANC2AXI_TBL_DATA, surface->address[0] >> 5 },
			{ HEVCD_MPP_ANC2AXI_TBL_DATA, surface->address[1] >> 5 },
			{ HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 1 },
			{ HEVCD_MPP_ANC_CANVAS_ACCESS, 1 },
		};

		ret = hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
	if (ret)
		return ret;
	for (i = 0; i < 32; i++) {
		ret = regmap_write(map, HEVCD_MPP_ANC_CANVAS_DATA, 0);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence seq[] = {
			{ HEVC_SCRATCH(9), sao_unit },
			{ HEVC_SCRATCH(10), ws->sao_abv.address },
			{ HEVC_SCRATCH(11), (sao_unit + 32) * plan.ctb_rows },
			{ HEVC_SCRATCH(12), ws->sao_vb.address },
			{ HEVCD_MCRCC_CTL1, 2 },
			{ HEVCD_MCRCC_CTL1, 0 },
			{ HEVC_MPRED_MV_WR_START_ADDR, ws->mv.address },
			{ HEVC_MPRED_MV_RD_START_ADDR, ws->mv.address },
			{ HEVC_MPRED_MV_WR_ROW_JUMP, 0 },
			{ HEVC_MPRED_MV_RD_ROW_JUMP, 0 },
			{ HEVC_MPRED_CTRL0, mpred0 },
			{ HEVC_MPRED_CTRL1, mpred1 },
			{ HEVC_MPRED_PIC_SIZE, pic_size },
			{ HEVC_MPRED_PIC_SIZE_LCU, lcu_size },
			{ HEVC_MPRED_TILE_START, 0 },
			{ HEVC_MPRED_TILE_SIZE_LCU, tile_size },
			{ HEVC_MPRED_REF_NUM, 0 },
			{ HEVC_MPRED_LT_REF, 0 },
			{ HEVC_MPRED_REF_EN_L0, 0 },
			{ HEVC_MPRED_REF_EN_L1, 0 },
			{ HEVC_MPRED_CUR_POC, 0 },
		};

		ret = hevc_write_many(map, seq, ARRAY_SIZE(seq));
	}
	if (ret)
		return ret;
	/* Hardware requires L0[n], L1[n] pairs even when both lists are disabled. */
	for (i = 0; i < 16; i++) {
		ret = regmap_write(map, HEVC_MPRED_L0_REF_POC(i), 0);
		if (ret)
			return ret;
		ret = regmap_write(map, HEVC_MPRED_L1_REF_POC(i), 0);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence seq[] = {
			{ HEVC_MPRED_ABV_START_ADDR, ws->mpred_above.address },
			{ HEVC_MPRED_MV_WPTR, ws->mv.address },
			{ HEVC_MPRED_MV_RPTR, ws->mv.address },
			{ HEVC_MPRED_MV_RD_END_ADDR, ws->mv.address + ws->mv.bytes },
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
				(plan.coded_width >= 1280 ? BIT(4) : 0) },
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
	ret = regmap_update_bits(map, HEVCD_IPP_AXIIF_CONFIG, 0x33f, 0x20f);
	if (ret)
		return ret;
	ret = regmap_write(map, HEVC_DBLK_CFG9, dblk9);
	if (ret)
		return ret;
	return regmap_update_bits(map, HEVC_SAO_CTRL0, GENMASK(9, 8), sao_flags);
}
