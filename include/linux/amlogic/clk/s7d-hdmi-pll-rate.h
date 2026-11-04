/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_PLL_RATE_H
#define __S7D_HDMI_PLL_RATE_H

#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/types.h>

struct s7d_hdmi_pll_rate {
	u32 m;
	u32 frac;
	u32 od00;
	u32 od01;
	unsigned long rate;
};

struct s7d_hdmi_shared_rate {
	struct s7d_hdmi_pll_rate pll;
	u64 numerator;
	u32 denominator;
};

struct s7d_hdmi_pll_divider {
	u32 od;
	u32 selector;
	unsigned long rate;
};

struct s7d_hdmi_link_rate {
	struct s7d_hdmi_shared_rate shared;
	struct s7d_hdmi_pll_divider serial;
	struct s7d_hdmi_pll_divider pixel;
	unsigned long pixel_requested;
	unsigned long serial_requested;
	u64 tmds_numerator;
	u32 tmds_denominator;
	u32 ratio_q;
};

static inline unsigned long s7d_hdmi_shared_decode(u32 ctrl0, u32 ctrl3,
						  unsigned long parent)
{
	u32 od00 = (ctrl0 >> 20) & 7;
	u32 od01 = (ctrl0 >> 23) & 7;
	u32 denominator;
	u64 numerator;

	if (parent != 24000000 || !(ctrl3 & (1U << 19)) ||
	    od00 > 3 || od01 > 3 || !(ctrl0 & 0x1ff))
		return 0;
	denominator = 1U << (17 + od00 + od01);
	numerator = (((u64)(ctrl0 & 0x1ff) << 17) | (ctrl3 & 0x1ffff)) *
		    (parent / 2);
	return div_u64(numerator + denominator / 2, denominator);
}

static inline unsigned long s7d_hdmi_serial_decode(unsigned long parent, u32 ctrl3)
{
	u32 od = (ctrl3 >> 27) & 7;

	if (!parent || parent >= 6144000000ULL || od > 3)
		return 0;
	return div_u64((u64)parent + (1U << od) / 2, 1U << od);
}

static inline unsigned long s7d_hdmi_pixel_decode(unsigned long parent, u32 ctrl3)
{
	u32 od = (ctrl3 >> 24) & 7;
	u32 selector = (ctrl3 >> 22) & 3;
	u32 q;

	if (!parent || parent >= 6144000000ULL || od > 3 || selector == 3)
		return 0;
	q = (20 + 5 * selector) << od;
	return div_u64((u64)parent * 4 + q / 2, q);
}

/* The shared output is before OD20/OD21; this is not a PHY operating range. */
static inline int s7d_hdmi_shared_calculate(unsigned long rate,
					 unsigned long parent,
					 struct s7d_hdmi_shared_rate *cfg)
{
	struct s7d_hdmi_shared_rate next = { 0 };
	u64 vco, scaled;
	u32 shift = 0;

	if (parent != 24000000 || rate < 46875000 || rate >= 6000000000ULL)
		return -EINVAL;
	vco = rate;
	while (vco < 3000000000ULL) {
		vco *= 2;
		shift++;
	}
	if (shift > 6)
		return -EINVAL;
	scaled = div_u64((vco << 17) + parent / 4, parent / 2);
	if (scaled >= (500ULL << 17))
		scaled = (500ULL << 17) - 1;
	next.pll.m = scaled >> 17;
	next.pll.frac = scaled & 0x1ffff;
	next.pll.od00 = shift > 3 ? 3 : shift;
	next.pll.od01 = shift - next.pll.od00;
	next.numerator = scaled * (parent / 2);
	next.denominator = 1U << (17 + shift);
	next.pll.rate = div_u64(next.numerator + next.denominator / 2,
			       next.denominator);
	*cfg = next;
	return 0;
}

static inline int s7d_hdmi_serial_calculate(unsigned long rate,
					 unsigned long parent,
					 struct s7d_hdmi_pll_divider *cfg)
{
	struct s7d_hdmi_pll_divider next = { 0 };
	u32 od;

	if (!rate || !parent)
		return -EINVAL;
	for (od = 0; od <= 3; od++) {
		next.rate = s7d_hdmi_serial_decode(parent, od << 27);
		if (next.rate != rate)
			continue;
		next.od = od;
		next.selector = 0;
		*cfg = next;
		return 0;
	}
	return -EINVAL;
}

static inline int s7d_hdmi_pixel_calculate(unsigned long rate,
					unsigned long parent,
					struct s7d_hdmi_pll_divider *cfg)
{
	struct s7d_hdmi_pll_divider next = { 0 };
	u32 od, selector;

	if (!rate || !parent)
		return -EINVAL;
	for (od = 0; od <= 1; od++) {
		for (selector = 0; selector <= 2; selector++) {
			next.rate = s7d_hdmi_pixel_decode(parent,
					 (od << 24) | (selector << 22));
			if (next.rate != rate)
				continue;
			next.od = od;
			next.selector = selector;
			*cfg = next;
			return 0;
		}
	}
	return -EINVAL;
}

/* ratio_q = 4*S/P: RGB/4448/422=40; 10=50; 12=60; 420=20/25/30. */
static inline int s7d_hdmi_link_calculate(unsigned long pixel, u32 ratio_q,
					unsigned long parent,
					struct s7d_hdmi_link_rate *cfg)
{
	struct s7d_hdmi_link_rate next = { 0 };
	u64 serial;
	u32 od, selector;
	int ret;

	switch (ratio_q) {
	case 20: case 25: case 30:
		od = 0;
		break;
	case 40: case 50: case 60:
		od = 1;
		break;
	default:
		return -EINVAL;
	}
	if (!pixel || pixel > 1200000000UL)
		return -EINVAL;
	serial = div_u64((u64)pixel * ratio_q + 2, 4);
	if (serial >= 6000000000ULL)
		return -EINVAL;
	ret = s7d_hdmi_shared_calculate(serial, parent, &next.shared);
	if (ret)
		return ret;
	selector = ((ratio_q >> od) - 20) / 5;
	next.serial.od = 0;
	next.serial.selector = 0;
	next.serial.rate = next.shared.pll.rate;
	next.pixel.od = od;
	next.pixel.selector = selector;
	next.pixel.rate = s7d_hdmi_pixel_decode(next.shared.pll.rate,
					 (od << 24) | (selector << 22));
	next.pixel_requested = pixel;
	next.serial_requested = serial;
	next.tmds_numerator = next.shared.numerator;
	next.tmds_denominator = next.shared.denominator * 10;
	next.ratio_q = ratio_q;
	*cfg = next;
	return 0;
}

/* Legacy RGB/4448 calculation and direct boot-pixel decode. */
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
	denominator = (20 + 5 * divider) << (17 + od00 + od01 + od21);
	numerator = (((u64)(ctrl0 & 0x1ff) << 17) | (ctrl3 & 0x1ffff)) *
		    parent * 2;
	return div_u64(numerator + denominator / 2, denominator);
}

static inline int s7d_hdmi_pll_calculate(unsigned long rate,
				       unsigned long parent,
				       struct s7d_hdmi_pll_rate *cfg)
{
	struct s7d_hdmi_shared_rate shared;
	struct s7d_hdmi_pll_rate next;
	int ret;

	if (rate < 25175000 || rate > 594000000)
		return -EINVAL;
	ret = s7d_hdmi_shared_calculate((u64)rate * 10, parent, &shared);
	if (ret)
		return ret;
	next = shared.pll;
	next.rate = div_u64(shared.numerator + shared.denominator * 5,
			    shared.denominator * 10);
	*cfg = next;
	return 0;
}

#endif
