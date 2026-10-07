// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>

#include "s7d-osd-scaler.h"

int s7d_osd_scaler_build(unsigned int index, u32 input_w, u32 input_h,
			 u32 output_w, u32 output_h,
			 struct s7d_osd_scaler_state *state)
{
	if (!state || index > 1 || !input_w || !input_h ||
	    input_w > 8192 || input_h > 8192 ||
	    !output_w || !output_h || output_w > 4096 || output_h > 4096)
		return -EINVAL;
	if (input_w == output_w && input_h == output_h) {
		*state = (struct s7d_osd_scaler_state) {0};
		return 0;
	}
	if (input_w < 4 || input_h < 4 ||
	    input_w > S7D_OSD_SCALER_LINEBUFFER || input_h > 2160 ||
	    output_h > 2160 || input_w > output_w || input_h > output_h ||
	    output_w > input_w * S7D_OSD_SCALER_MAX_UPSCALE ||
	    output_h > input_h * S7D_OSD_SCALER_MAX_UPSCALE)
		return -ERANGE;
	*state = (struct s7d_osd_scaler_state) {
		.input_size = ((input_w - 1) << 16) | (input_h - 1),
		.output_h = output_w - 1,
		.output_v = output_h - 1,
		.h_phase_step = div_u64((u64)input_w << 18, output_w) << 6,
		.v_phase_step = div_u64((u64)input_h << 20, output_h) << 4,
		.h_control = BIT(22) | BIT(8) | (4 << 3) | 4,
		.v_control = BIT(25) | BIT(24) | BIT(8) | (4 << 3) | 4,
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
		a->v_control == b->v_control && a->enabled == b->enabled;
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

static u32 bilinear_coef(unsigned int phase)
{
	return ((128 - 2 * phase) << 16) | ((2 * phase) << 8);
}

static void scaler_coefficients(void __iomem *vcbus, u32 index_reg, u32 data_reg)
{
	unsigned int direction, phase;

	for (direction = 0; direction < 2; direction++) {
		writel(direction << 8, vcbus + index_reg * 4);
		for (phase = 0; phase < 33; phase++)
			writel(bilinear_coef(phase), vcbus + data_reg * 4);
	}
	writel(0, vcbus + index_reg * 4);
}

int s7d_osd_scaler_setup(void __iomem *vcbus,
			 const struct s7d_osd_scaler_state states[2], u32 *failed_reg)
{
	const u32 base[] = { 0x1dc0, 0x3d00 };
	const u32 coef_index[] = { 0x1dcc, 0x3d18 };
	const u32 div_alpha[] = { 0x1dbf, 0x3d38 };
	const u32 v_mask = GENMASK(25, 19) | GENMASK(17, 16) |
		GENMASK(14, 11) | GENMASK(9, 8) | GENMASK(6, 0);
	const u32 h_mask = GENMASK(22, 19) | GENMASK(17, 16) |
		GENMASK(14, 11) | GENMASK(9, 8) | GENMASK(6, 0);
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
				s->output_v + 1, &checked);
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
	/* Rev.B has a local OSD1 scaler before the independent OSD blend outputs. */
	ret = scaler_write(vcbus, 0x1dff, GENMASK(2, 0), 1, failed_reg);
	if (ret)
		return ret;
	for (index = 0; index < 2; index++) {
		const struct s7d_osd_scaler_state *s = &states[index];
		const u32 r = base[index];
		const struct {
			u32 reg, mask, value;
		} settings[] = {
			{ r + 9, 0x1fff1fff, s->input_size },
			{ r + 10, 0x0fff0fff, s->output_h },
			{ r + 11, 0x0fff0fff, s->output_v },
			{ r, GENMASK(27, 0), s->v_phase_step },
			{ r + 1, U32_MAX, 0 },
			{ r + 3, GENMASK(27, 0), s->h_phase_step },
			{ r + 4, U32_MAX, 0 },
			{ r + 6, 0xff77, 0 },
			{ r + 7, U32_MAX, 0x80808080 },
			{ div_alpha[index], BIT(7) | GENMASK(5, 4) | GENMASK(2, 0), 0xb1 },
		};

		ret = scaler_write(vcbus, r + 8, GENMASK(13, 2), 0, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 2, BIT(24) | BIT(23), 0, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 5, BIT(22), 0, failed_reg);
		if (ret)
			return ret;
		if (!s->enabled)
			continue;
		for (i = 0; i < ARRAY_SIZE(settings); i++) {
			ret = scaler_write(vcbus, settings[i].reg, settings[i].mask,
					   settings[i].value, failed_reg);
			if (ret)
				return ret;
		}
		scaler_coefficients(vcbus, coef_index[index], coef_index[index] + 1);
		ret = scaler_write(vcbus, r + 2, v_mask, s->v_control, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 5, h_mask, s->h_control, failed_reg);
		if (!ret)
			ret = scaler_write(vcbus, r + 8, GENMASK(13, 2), BIT(3) | BIT(2),
					   failed_reg);
		if (ret)
			return ret;
	}
	return 0;
}
