// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/minmax.h>

#include <drm/drm_color_mgmt.h>

#include "s7d-gamma.h"
#include "s7d-rdma.h"

#define VPP_DOLBY_CTRL		0x1d93
#define POST2_TO_OUTBUF_BYPASS	BIT(2)
#define VPP_GAMMA_CTRL		0x39d4
#define VPP_GAMMA_BIN_ADDR	0x39d5
#define VPP_GAMMA_BIN_DATA	0x39d6

int s7d_gamma_check(const struct drm_property_blob *lut)
{
	if (lut && lut->length != S7D_GAMMA_LUT_SIZE * sizeof(struct drm_color_lut))
		return -EINVAL;
	return 0;
}

bool s7d_gamma_active(const struct drm_property_blob *lut)
{
	const struct drm_color_lut *table;
	unsigned int i;

	if (!lut || s7d_gamma_check(lut))
		return !!lut;
	table = lut->data;
	for (i = 0; i < S7D_GAMMA_LUT_SIZE; i++) {
		u32 identity = min(i * 16, 1023U);

		if (drm_color_lut_extract(table[i].red, 10) != identity ||
		    drm_color_lut_extract(table[i].green, 10) != identity ||
		    drm_color_lut_extract(table[i].blue, 10) != identity)
			return true;
	}
	return false;
}

static int s7d_gamma_control(void __iomem *vcbus, u32 value, u32 *failed_reg)
{
	void __iomem *addr = vcbus + VPP_GAMMA_CTRL * 4;

	writel((readl(addr) & ~GENMASK(1, 0)) | value, addr);
	/* The selected S7D vendor status reads only the stable enable bit. */
	if ((readl(addr) & BIT(0)) != (value & BIT(0))) {
		if (failed_reg)
			*failed_reg = VPP_GAMMA_CTRL;
		return -EIO;
	}
	return 0;
}

static int s7d_gamma_bypass(void __iomem *vcbus, bool bypass, u32 *failed_reg)
{
	void __iomem *addr = vcbus + VPP_DOLBY_CTRL * 4;
	u32 value = bypass ? POST2_TO_OUTBUF_BYPASS : 0;

	writel((readl(addr) & ~POST2_TO_OUTBUF_BYPASS) | value, addr);
	if ((readl(addr) & POST2_TO_OUTBUF_BYPASS) != value) {
		if (failed_reg)
			*failed_reg = VPP_DOLBY_CTRL;
		return -EIO;
	}
	return 0;
}

/* 10-bit LUT output, with limited RGB folded in as 64 + 877 / 1024, at 12 bits. */
static u32 s7d_gamma_code(u32 value, bool limited)
{
	if (limited)
		value = 64 + DIV_ROUND_CLOSEST(value * 877, 1024);
	return value << 2;
}

static u32 s7d_gamma_value(const struct drm_color_lut *table, unsigned int i,
			   unsigned int channel, bool limited)
{
	u32 value;

	if (!table)
		return s7d_gamma_code(min(i * 16, 1023U), limited);
	switch (channel) {
	case 0:
		value = table[i].red;
		break;
	case 1:
		value = table[i].green;
		break;
	default:
		value = table[i].blue;
		break;
	}
	return s7d_gamma_code(drm_color_lut_extract(value, 10), limited);
}

int s7d_gamma_build(const struct drm_property_blob *lut, bool limited,
		    struct s7d_rdma_entry *update)
{
	const struct drm_color_lut *table;
	unsigned int channel, i, count = 0;
	int ret;

	ret = s7d_gamma_check(lut);
	if (ret)
		return ret;
	if (!limited && !s7d_gamma_active(lut)) {
		update[count++] = (struct s7d_rdma_entry) {
			.reg = cpu_to_le32(VPP_GAMMA_CTRL), .value = 0,
		};
		return count;
	}
	/* Identity outputs when the LUT is absent: the range alone remains. */
	table = s7d_gamma_active(lut) ? lut->data : NULL;
	update[count++] = (struct s7d_rdma_entry) {
		.reg = cpu_to_le32(VPP_GAMMA_BIN_ADDR), .value = 0,
	};
	/* The selected S7D vendor writes R, G, B: 32 packed pairs and entry 64. */
	for (channel = 0; channel < 3; channel++) {
		for (i = 0; i < 32; i++) {
			u32 value = s7d_gamma_value(table, 2 * i, channel, limited) |
				s7d_gamma_value(table, 2 * i + 1, channel, limited) << 16;

			update[count++] = (struct s7d_rdma_entry) {
				.reg = cpu_to_le32(VPP_GAMMA_BIN_DATA),
				.value = cpu_to_le32(value),
			};
		}
		update[count++] = (struct s7d_rdma_entry) {
			.reg = cpu_to_le32(VPP_GAMMA_BIN_DATA),
			.value = cpu_to_le32(s7d_gamma_value(table, 64, channel, limited)),
		};
	}
	update[count++] = (struct s7d_rdma_entry) {
		.reg = cpu_to_le32(VPP_GAMMA_CTRL), .value = cpu_to_le32(3),
	};
	return count;
}

/* CTRL shares its word with fields the gamma writes leave alone. */
u32 s7d_gamma_reg_mask(u32 reg)
{
	return reg == VPP_GAMMA_CTRL ? GENMASK(1, 0) : 0;
}

bool s7d_gamma_update_valid(u32 reg, u32 value)
{
	switch (reg) {
	case VPP_GAMMA_CTRL:
		return value == 0 || value == 3;
	case VPP_GAMMA_BIN_ADDR:
		return !value;
	case VPP_GAMMA_BIN_DATA:
		return !(value & ~0x0ffc0ffcU);
	default:
		return false;
	}
}

int s7d_gamma_disable(void __iomem *vcbus, u32 *failed_reg)
{
	int ret;

	if (!vcbus)
		return -EINVAL;
	ret = s7d_gamma_control(vcbus, 0, failed_reg);
	if (ret)
		return ret;
	return s7d_gamma_bypass(vcbus, true, failed_reg);
}

/*
 * Un-bypassed with VPP_GAMMA_CTRL 0 passes pixels unchanged (writeback, 8/10
 * bit), as in the vendor; CTRL alone then switches the LUT, so later updates
 * need no bypass change.
 */
int s7d_gamma_program(void __iomem *vcbus, const struct drm_property_blob *lut,
		      bool limited, u32 *failed_reg)
{
	struct s7d_rdma_entry update[S7D_GAMMA_UPDATE_COUNT];
	int count, i, ret;

	if (!vcbus)
		return -EINVAL;
	count = s7d_gamma_build(lut, limited, update);
	if (count < 0)
		return count;
	ret = s7d_gamma_control(vcbus, 0, failed_reg);
	if (ret)
		return ret;
	ret = s7d_gamma_bypass(vcbus, false, failed_reg);
	if (ret)
		return ret;
	for (i = 0; i < count - 1; i++)
		writel(le32_to_cpu(update[i].value),
		       vcbus + le32_to_cpu(update[i].reg) * 4);
	return s7d_gamma_control(vcbus, le32_to_cpu(update[count - 1].value), failed_reg);
}
