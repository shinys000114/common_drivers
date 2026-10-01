// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>

#include "s7d-vpp.h"

#define ENCI_VIDEO_EN		0x1b57
#define ENCP_VIDEO_EN		0x1b80
#define ENCL_VIDEO_EN		0x1ca0
#define OSD1_CTRL_STAT		0x1a10
#define OSD2_CTRL_STAT		0x1a30

struct s7d_vpp_setting {
	u32 reg;
	u32 mask;
	u32 value;
};

int s7d_vpp_setup(void __iomem *vcbus, u32 width, u32 height, u32 *failed_reg)
{
	/*
	 * Only configuration fields are touched. In particular LUT3D and VE
	 * clock gates share words with enable bits: preserve their clock fields.
	 * Sources: S905X5M tables 9-1312/1314, 9-1463..1466, 9-1504..1513,
	 * 9-1526/1527, 9-1543..1546, 9-1609..1612, 9-2634 and 9-3124;
	 * vendor set_hdr2_v0.c, amve.c, bitdepth.c and meson_vpu_postblend.c.
	 */
	const struct s7d_vpp_setting settings[] = {
		/* Block the postblend input until its entire route is ready. */
		{ 0x1dfd, 0x00110f1f, 0 }, /* OSD1_BLEND_SRC_CTRL */
		{ 0x1dfb, 0x00010f1f, 0 }, /* VD1_BLEND_SRC_CTRL */
		{ 0x1dfc, 0x00110f1f, 0 }, /* VD2_BLEND_SRC_CTRL */
		{ 0x1dfe, 0x00110f1f, BIT(20) }, /* OSD2: no source */
		/* Direct OSD memory path, no GFCD, AFBC or output exchange. */
		{ 0x1a0e, BIT(14) | GENMASK(12, 9) | GENMASK(7, 0), 0 },
		/* Bypass OSD/video Dolby paths, with unsigned 12-bit extension. */
		{ 0x1a0c, GENMASK(7, 0), 0xf },
		/* No OSD scaling; discard inherited interlace/filter enables. */
		{ 0x1dc8, GENMASK(13, 12) | GENMASK(11, 2), 0 },
		{ 0x1dc2, BIT(24) | BIT(23), 0 }, /* VSC enable/interlace */
		{ 0x1dc5, BIT(22), 0 }, /* HSC enable */
		{ 0x1dc9, 0x1fff1fff, ((width - 1) << 16) | (height - 1) },
		{ 0x1dca, 0x0fff0fff, width - 1 },
		{ 0x1dcb, 0x0fff0fff, height - 1 },
		{ 0x3d08, GENMASK(13, 12) | GENMASK(11, 2), 0 },
		{ 0x3d02, BIT(24) | BIT(23), 0 },
		{ 0x3d05, BIT(22), 0 },
		{ 0x1df1, 0x1fff1fff, (height << 16) | width },
		/* Disable HDR and its independent matrix-only bypass path. */
		{ 0x38a0, GENMASK(20, 13) | GENMASK(7, 2), 0 },
		/* Clear enable_sync_sel too: no VSYNC is running at this point. */
		{ 0x38db, GENMASK(1, 0), 0 }, /* HDR input matrix */
		{ 0x38dc, GENMASK(1, 0), 0 }, /* HDR output matrix */
		/* S7D moves the OSD2 HDR block by 0x500 from the vendor base. */
		{ 0x6000, GENMASK(20, 13) | GENMASK(7, 2), 0 },
		{ 0x603b, GENMASK(1, 0), 0 },
		{ 0x603c, GENMASK(1, 0), 0 },
		{ 0x391d, GENMASK(1, 0), 0 }, /* OSD1 matrix */
		{ 0x392d, GENMASK(1, 0), 0 }, /* OSD2 matrix */
		{ 0x3d6d, GENMASK(1, 0), 0 }, /* OSD1 wrap RGB->YUV matrix */
		{ 0x3d7d, GENMASK(1, 0), 0 }, /* OSD2 wrap RGB->YUV matrix */
		{ 0x32bd, GENMASK(1, 0), 0 }, /* POST matrix */
		{ 0x39ad, GENMASK(1, 0), 0 }, /* POST2 matrix */
		/* No colour enhancement, highlight, super-resolution or LUTs. */
		{ 0x1d26, GENMASK(30, 28) | GENMASK(11, 6) | BIT(3) | BIT(1), BIT(7) },
		{ 0x1d5f, BIT(16), 0 }, /* Matrix probe highlight */
		{ 0x1d40, BIT(2) | BIT(0), 0 }, /* VADJ2/VADJ1 */
		{ 0x1d6a, GENMASK(31, 30), 0 }, /* Gain/offset, immediate disable */
		{ 0x1d91, GENMASK(3, 0), 0 }, /* SRSHARP0 */
		{ 0x1d92, GENMASK(3, 0), 0 }, /* SRSHARP1 */
		{ 0x1da1, GENMASK(20, 18) | GENMASK(4, 2) | BIT(0), 0 },
		{ 0x39d0, BIT(2) | BIT(0), 0 }, /* LUT3D, no VSYNC shadow */
		{ 0x39d4, GENMASK(1, 0), 0 }, /* Pre-gamma */
		/* Bypass unused stages/core3; disable inherited dithering. */
		{ 0x1d96, GENMASK(25, 24) | BIT(20) | BIT(14) | BIT(12) | GENMASK(10, 8) |
			    GENMASK(3, 0), 0 }, /* Immediate path changes */
		{ 0x1d93, GENMASK(25, 24) | BIT(20) | BIT(14) | BIT(12) | GENMASK(10, 8) |
			    GENMASK(3, 0), BIT(14) | 7 },
		{ 0x1d94, 0x3fff3fff, 0 }, /* Pre unsigned/signed conversion */
		{ 0x1d95, 0x3fff3fff, 0 }, /* Post unsigned/signed conversion */
		{ 0x1dd9, GENMASK(29, 0), GENMASK(29, 0) }, /* Full-range clip top */
		{ 0x1dda, GENMASK(29, 0), 0 }, /* Full-range clip bottom */
		/* Scope packing differs from the MIF/blend input scopes. */
		{ 0x1df5, 0x1fff1fff, width - 1 },
		{ 0x1df6, 0x1fff1fff, height - 1 },
		{ 0x1d21, 0x3fff3fff, (height << 16) | width },
		{ 0x1da5, 0x1fff1fff, (width << 16) | height },
		{ 0x1d22, GENMASK(15, 0), 0x0808 }, /* Pre/post hold lines */
		/* 4096-pixel FIFO, not active width. Never replay force-go pulses. */
		{ 0x1d27, U32_MAX, 0xfff01000 },
		/* Select OSD1 only, postblend, no premultiplication. */
		{ 0x1dfd, 0x00110f1f, BIT(20) | (3 << 8) },
	};
	unsigned int i;
	u32 value;

	if (failed_reg)
		*failed_reg = 0;
	if (!vcbus || !width || !height || width > 4096 || height > 4096)
		return -EINVAL;
	/* Refuse accidental reconfiguration of an active firmware/Linux frame. */
	if ((readl(vcbus + ENCP_VIDEO_EN * 4) |
	     readl(vcbus + ENCI_VIDEO_EN * 4) |
	     readl(vcbus + ENCL_VIDEO_EN * 4) |
	     readl(vcbus + OSD1_CTRL_STAT * 4) |
	     readl(vcbus + OSD2_CTRL_STAT * 4)) & BIT(0))
		return -EBUSY;

	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		const struct s7d_vpp_setting *s = &settings[i];
		void __iomem *addr = vcbus + s->reg * 4;

		value = (readl(addr) & ~s->mask) | s->value;
		writel(value, addr);
		if ((readl(addr) & s->mask) != s->value) {
			if (failed_reg)
				*failed_reg = s->reg;
			return -EIO;
		}
	}
	return 0;
}
