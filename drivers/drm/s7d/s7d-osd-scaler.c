// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>

#include "s7d-osd-scaler.h"
#include "s7d-filter.h"

/*
 * Vendor f2v_get_vertical_phase for a progressive input: the output line
 * sits at out_pos quarter lines, and a 2.0 field ratio gets the half phase.
 */
static void scaler_vphase(u64 ratio, unsigned int out_pos, u32 bank,
			  u32 *rcv, u32 *rpt, u32 *phase)
{
	u64 in = 0, out = (out_pos * ratio) >> 4;

	*rcv = bank;
	*rpt = bank >= 3;
	while (in + (4 << 16) <= out) {
		if (*rpt)
			*rpt = 0;
		else
			(*rcv)++;
		in += 4 << 16;
	}
	*phase = (out - in) >> 2;
	if ((ratio >> 20) == 2 && !(ratio & GENMASK(19, 0)))
		*phase |= 0x8000;
}

int s7d_osd_scaler_build(unsigned int index, u32 input_w, u32 input_h,
			 u32 output_w, u32 output_h, bool interlace,
			 struct s7d_osd_scaler_state *state)
{
	u32 v_control = BIT(25) | BIT(24), bank, rcv, rpt, top, bottom = 0;
	u64 ratio;

	if (!state || index > 1 || !input_w || !input_h ||
	    input_w > 8192 || input_h > 8192 ||
	    !output_w || !output_h || output_w > 4096 || output_h > 4096)
		return -EINVAL;
	if (!interlace && input_w == output_w && input_h == output_h) {
		*state = (struct s7d_osd_scaler_state) {0};
		return 0;
	}
	if (input_w < 4 || input_h < 4 ||
	    input_w > S7D_OSD_SCALER_MAX_INPUT_WIDTH || input_h > 2160 ||
	    output_h > 2160 ||
	    input_w > output_w * S7D_OSD_SCALER_MAX_DOWNSCALE ||
	    input_h > output_h * S7D_OSD_SCALER_MAX_DOWNSCALE ||
	    output_w > input_w * S7D_OSD_SCALER_MAX_UPSCALE ||
	    output_h > input_h * S7D_OSD_SCALER_MAX_UPSCALE)
		return -ERANGE;
	bank = input_w > S7D_OSD_SCALER_LINEBUFFER ? 2 : 4;
	if (bank == 2)
		v_control |= BIT(21);
	ratio = div_u64((u64)input_h << 20, output_h);
	if (interlace) {
		/* Fields from a progressive frame: top on line 0, bottom half a line on. */
		scaler_vphase(ratio, 0, bank, &rcv, &rpt, &top);
		v_control |= BIT(23) | (rpt << 8) | (rcv << 3);
		scaler_vphase(ratio, 2, bank, &rcv, &rpt, &bottom);
		v_control |= (rpt << 16) | (rcv << 11);
	} else {
		scaler_vphase(ratio, 0, bank, &rcv, &rpt, &top);
		top = 0;
		v_control |= (rpt << 8) | (rcv << 3);
	}
	*state = (struct s7d_osd_scaler_state) {
		.input_size = ((input_w - 1) << 16) | (input_h - 1),
		.output_h = output_w - 1,
		.output_v = output_h - 1,
		.h_phase_step = div_u64((u64)input_w << 18, output_w) << 6,
		.v_phase_step = (u32)ratio << 4,
		.h_control = BIT(22) | BIT(8) | (4 << 3) | 4,
		.v_control = v_control | bank,
		.v_init_phase = interlace ? (bottom << 16) | top : 0,
		.enabled = true,
	};
	return 0;
}

bool s7d_osd_scaler_same(const struct s7d_osd_scaler_state *a,
			const struct s7d_osd_scaler_state *b)
{
	return a->input_size == b->input_size && a->output_h == b->output_h &&
		a->output_v == b->output_v && a->h_phase_step == b->h_phase_step &&
		a->v_phase_step == b->v_phase_step && a->h_control == b->h_control &&
		a->v_control == b->v_control && a->v_init_phase == b->v_init_phase &&
		a->enabled == b->enabled;
}

int s7d_osd_scaler_check_mode(const struct s7d_osd_scaler_state states[2],
			      u32 htotal, unsigned long pixel_rate,
			      unsigned long core_rate)
{
	unsigned int index;

	if (!states || !htotal || !pixel_rate || !core_rate)
		return -EINVAL;
	if (pixel_rate > 594000000)
		return -ERANGE;
	for (index = 0; index < 2; index++) {
		const struct s7d_osd_scaler_state *s = &states[index];
		struct s7d_osd_scaler_state checked;
		u32 input_w = (s->input_size >> 16) + 1;
		u32 input_h = (s->input_size & 0xffff) + 1;
		u64 numerator, denominator;
		int ret;

		if (!s->enabled)
			continue;
		ret = s7d_osd_scaler_build(index, input_w, input_h,
					 s->output_h + 1, s->output_v + 1,
					 s->v_control & BIT(23), &checked);
		if (ret || !s7d_osd_scaler_same(s, &checked))
			return -EINVAL;
		if (input_w <= s->output_h + 1 && input_h <= s->output_v + 1)
			continue;
		numerator = (u64)input_w * input_h * pixel_rate * 105;
		denominator = (u64)htotal * (s->output_v + 1) * 100;
		if (div64_u64(numerator + denominator - 1, denominator) > core_rate)
			return -ERANGE;
	}
	return 0;
}

static int scaler_write(void __iomem *vcbus, u32 reg, u32 mask, u32 value,
			u32 *failed_reg)
{
	void __iomem *addr = vcbus + reg * 4;

	writel((readl(addr) & ~mask) | value, addr);
	if ((readl(addr) & mask) == value)
		return 0;
	if (failed_reg)
		*failed_reg = reg;
	return -EIO;
}

/* Rev.B has a local OSD1 scaler before the independent OSD blend outputs. */
const struct s7d_vpp_setting s7d_osd_scaler_route = { 0x1dff, GENMASK(2, 0), 1 };

static const u32 scaler_base[] = { 0x1dc0, 0x3d00 };
static const u32 scaler_coef_index[] = { 0x1dcc, 0x3d18 };
static const u32 scaler_div_alpha[] = { 0x1dbf, 0x3d38 };
#define SCALER_V_MASK	(GENMASK(25, 19) | GENMASK(17, 16) | GENMASK(14, 11) | \
			 GENMASK(9, 8) | GENMASK(6, 0))
#define SCALER_H_MASK	(GENMASK(22, 19) | GENMASK(17, 16) | GENMASK(14, 11) | \
			 GENMASK(9, 8) | GENMASK(6, 0))

struct scaler_reg {
	u32 reg, mask, value;
};

#define SCALER_SETTING_COUNT 10

/* Geometry, phases, dummy data and alpha division of an enabled scaler. */
static void scaler_settings(unsigned int index, const struct s7d_osd_scaler_state *s,
			    struct scaler_reg out[SCALER_SETTING_COUNT])
{
	const u32 r = scaler_base[index];
	const struct scaler_reg settings[SCALER_SETTING_COUNT] = {
		{ r + 9, 0x1fff1fff, s->input_size },
		{ r + 10, 0x0fff0fff, s->output_h },
		{ r + 11, 0x0fff0fff, s->output_v },
		{ r, GENMASK(27, 0), s->v_phase_step },
		{ r + 1, U32_MAX, s->v_init_phase },
		{ r + 3, GENMASK(27, 0), s->h_phase_step },
		{ r + 4, U32_MAX, 0 },
		{ r + 6, 0xff77, 0 },
		{ r + 7, U32_MAX, 0x80808080 },
		{ scaler_div_alpha[index], BIT(7) | GENMASK(5, 4) | GENMASK(2, 0), 0xb1 },
	};

	memcpy(out, settings, sizeof(settings));
}

/* The vertical and horizontal filter tables of an enabled scaler. */
static void scaler_tables(const struct s7d_osd_scaler_state *s, const u32 *tables[2])
{
	u32 input_w = (s->input_size >> 16) + 1;
	u32 input_h = (s->input_size & 0xffff) + 1;

	tables[0] = s7d_filter_bilinear;
	tables[1] = s7d_filter_bilinear;
	if (input_w > S7D_OSD_SCALER_LINEBUFFER) {
		tables[0] = s7d_filter_bilinear_2point;
	} else if (s->output_h < 720 &&
		   (input_w > s->output_h + 1 || input_h > s->output_v + 1)) {
		tables[0] = input_h > s->output_v + 1 ?
			s7d_filter_triangle : s7d_filter_bicubic;
		tables[1] = input_w > s->output_h + 1 ?
			s7d_filter_triangle : s7d_filter_bicubic;
	}
}

static void scaler_coefficients(void __iomem *vcbus, u32 index_reg, u32 data_reg,
				const struct s7d_osd_scaler_state *s)
{
	const u32 *tables[2];
	unsigned int direction, phase;

	scaler_tables(s, tables);
	for (direction = 0; direction < 2; direction++) {
		writel(direction << 8, vcbus + index_reg * 4);
		for (phase = 0; phase < S7D_FILTER_PHASES; phase++)
			writel(tables[direction][phase], vcbus + data_reg * 4);
	}
	writel(0, vcbus + index_reg * 4);
}

int s7d_osd_scaler_setup(void __iomem *vcbus,
			 const struct s7d_osd_scaler_state states[2], u32 *failed_reg)
{
	unsigned int index, i;
	int ret;

	if (failed_reg)
		*failed_reg = 0;
	if (!vcbus || !states)
		return -EINVAL;
	for (index = 0; index < 2; index++) {
		const struct s7d_osd_scaler_state *s = &states[index];
		struct s7d_osd_scaler_state checked = {0};

		if (s->enabled) {
			ret = s7d_osd_scaler_build(index, (s->input_size >> 16) + 1,
				(s->input_size & 0xffff) + 1, s->output_h + 1,
				s->output_v + 1, s->v_control & BIT(23), &checked);
			if (ret)
				return ret;
		}
		if (!s7d_osd_scaler_same(s, &checked))
			return -EINVAL;
	}
	if ((readl(vcbus + 0x1b57 * 4) | readl(vcbus + 0x1b80 * 4) |
	     readl(vcbus + 0x1ca0 * 4) | readl(vcbus + 0x1a10 * 4) |
	     readl(vcbus + 0x1a30 * 4)) & BIT(0))
		return -EBUSY;
	if (readl(vcbus + 0x4800 * 4) & (BIT(17) | BIT(0)))
		return -EBUSY;
	ret = scaler_write(vcbus, s7d_osd_scaler_route.reg, s7d_osd_scaler_route.mask,
			   s7d_osd_scaler_route.value, failed_reg);
	if (ret)
		return ret;
	for (index = 0; index < 2; index++) {
		const struct s7d_osd_scaler_state *s = &states[index];
		const u32 r = scaler_base[index];
		struct scaler_reg settings[SCALER_SETTING_COUNT];

		ret = scaler_write(vcbus, r + 8, GENMASK(13, 2), 0, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 2, BIT(24) | BIT(23), 0, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 5, BIT(22), 0, failed_reg);
		if (ret)
			return ret;
		if (!s->enabled)
			continue;
		scaler_settings(index, s, settings);
		for (i = 0; i < ARRAY_SIZE(settings); i++) {
			ret = scaler_write(vcbus, settings[i].reg, settings[i].mask,
					   settings[i].value, failed_reg);
			if (ret)
				return ret;
		}
		scaler_coefficients(vcbus, scaler_coef_index[index],
				    scaler_coef_index[index] + 1, s);
		ret = scaler_write(vcbus, r + 2, SCALER_V_MASK, s->v_control, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 5, SCALER_H_MASK, s->h_control, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 8, GENMASK(13, 2), BIT(3) | BIT(2),
					   failed_reg);
		if (ret)
			return ret;
	}
	return 0;
}

static_assert(S7D_OSD_SCALER_LIVE_COUNT ==
	      3 + SCALER_SETTING_COUNT + 2 * (S7D_FILTER_PHASES + 1) + 1 + 3);

int s7d_osd_scaler_build_live(unsigned int index, const struct s7d_osd_scaler_state *old,
			      const struct s7d_osd_scaler_state *state,
			      struct s7d_rdma_live *live)
{
	const u32 *before[2] = { NULL, NULL }, *after[2];
	struct scaler_reg settings[SCALER_SETTING_COUNT];
	unsigned int i, direction, phase;
	u32 r, coef;

	if (index > 1 || !old || !state || !live)
		return -EINVAL;
	r = scaler_base[index];
	coef = scaler_coef_index[index];
	/* The stopped preparation order: off, geometry, filters, then on. */
	s7d_rdma_live_add(live, r + 8, GENMASK(13, 2), 0);
	s7d_rdma_live_add(live, r + 2, BIT(24) | BIT(23), 0);
	s7d_rdma_live_add(live, r + 5, BIT(22), 0);
	if (!state->enabled)
		return 0;
	scaler_settings(index, state, settings);
	for (i = 0; i < ARRAY_SIZE(settings); i++)
		s7d_rdma_live_add(live, settings[i].reg, settings[i].mask, settings[i].value);
	scaler_tables(state, after);
	if (old->enabled)
		scaler_tables(old, before);
	/* The filter RAM keeps what a running scaler loaded; reload it for new tables. */
	if (!old->enabled || before[0] != after[0] || before[1] != after[1]) {
		for (direction = 0; direction < 2; direction++) {
			s7d_rdma_live_add(live, coef, U32_MAX, direction << 8);
			for (phase = 0; phase < S7D_FILTER_PHASES; phase++)
				s7d_rdma_live_add(live, coef + 1, U32_MAX,
						  after[direction][phase]);
		}
		s7d_rdma_live_add(live, coef, U32_MAX, 0);
	}
	s7d_rdma_live_add(live, r + 2, SCALER_V_MASK, state->v_control);
	s7d_rdma_live_add(live, r + 5, SCALER_H_MASK, state->h_control);
	s7d_rdma_live_add(live, r + 8, GENMASK(13, 2), BIT(3) | BIT(2));
	return 0;
}
