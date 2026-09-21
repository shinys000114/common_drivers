/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_PLL_RATE_H
#define __S7D_HDMI_PLL_RATE_H

#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/types.h>

/* TMDS, RGB/YCbCr444, eight bits per component; no FRL or deep colour. */
struct s7d_hdmi_pll_rate {
	u32 m;
	u32 frac;
	u32 od00;
	u32 od01;
	unsigned long rate;
};

/* Decode the analogue pixel output; usable for read-only boot validation. */
static inline unsigned long s7d_hdmi_pll_decode(u32 ctrl0, u32 ctrl3,
					       unsigned long parent)
{
	u32 od00 = (ctrl0 >> 20) & 7;
	u32 od01 = (ctrl0 >> 23) & 7;
	u32 od21 = (ctrl3 >> 24) & 7;
	u32 divider = (ctrl3 >> 22) & 3;
	u32 denominator;
	u64 numerator;

	if (parent != 24000000 || !(ctrl3 & (1U << 19)) || divider == 3 ||
	    od00 > 3 || od01 > 3 || od21 > 3 || !(ctrl0 & 0x1ff))
		return 0;
	/* Pixel divider 5 / 6.25 / 7.5 expressed as 20 / 25 / 30 over four. */
	denominator = (20 + 5 * divider) << (17 + od00 + od01 + od21);
	numerator = (((u64)(ctrl0 & 0x1ff) << 17) | (ctrl3 & 0x1ffff)) *
		    parent * 2;
	return div_u64(numerator + denominator / 2, denominator);
}

/* Pure calculation: safe to call from clk_round_rate()/DRM atomic_check(). */
static inline int s7d_hdmi_pll_calculate(unsigned long rate,
				       unsigned long parent,
				       struct s7d_hdmi_pll_rate *cfg)
{
	u64 vco, scaled;
	u32 shift = 0;

	/* The analogue characterisation below is for the board's 24 MHz XTAL. */
	if (parent != 24000000 || rate < 25175000 || rate > 594000000)
		return -EINVAL;

	vco = (u64)rate * 10;
	while (vco < 3000000000ULL) {
		vco *= 2;
		shift++;
	}
	if (vco >= 6000000000ULL || shift > 6)
		return -EINVAL;

	/* VCO = XTAL / 2 * (M + fraction / 2^17), rounded to nearest. */
	scaled = div_u64((vco << 17) + parent / 4, parent / 2);
	/* Rounding must not cross the exclusive 6 GHz analogue limit. */
	if (scaled >= (500ULL << 17))
		scaled = (500ULL << 17) - 1;
	cfg->m = scaled >> 17;
	cfg->frac = scaled & 0x1ffff;
	cfg->od00 = shift > 3 ? 3 : shift;
	cfg->od01 = shift - cfg->od00;
	/* OD20 = 0, OD21 = 1 and the analogue pixel divider is five. */
	cfg->rate = div_u64(scaled * (parent / 2) + (5ULL << (shift + 17)),
			    10U << (shift + 17));
	return 0;
}

#endif
