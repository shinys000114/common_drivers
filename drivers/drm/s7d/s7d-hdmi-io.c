// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/arm-smccc.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

#include "s7d-hdmi-io.h"

#define S7D_HDMI_READ	0x82000018
#define S7D_HDMI_WRITE	0x82000019
#define S7D_DSC_FIRST	0x0f80
#define S7D_DSC_LAST	0x0fa9

static bool s7d_hdmi_core_reg_valid(unsigned int reg)
{
	/* Vendor S7D dump code explicitly skips the absent DSC packet bank. */
	return reg < S7D_DSC_FIRST || reg > S7D_DSC_LAST;
}

struct s7d_hdmi_bus {
	phys_addr_t base;
	unsigned int width;
};

static int s7d_hdmi_reg_read(void *context, unsigned int reg, unsigned int *value)
{
	struct s7d_hdmi_bus *bus = context;
	struct arm_smccc_res res;

	if (bus->width == 8 && !s7d_hdmi_core_reg_valid(reg))
		return -EOPNOTSUPP;
	arm_smccc_smc(S7D_HDMI_READ, bus->base + reg, bus->width,
		      0, 0, 0, 0, 0, &res);
	/* An 8-bit read cannot legitimately return a firmware error word. */
	if (bus->width == 8 && res.a0 > U8_MAX)
		return -EIO;
	/*
	 * The 32-bit ABI returns raw data, not a separate status. Do not decode
	 * a negative-looking register value as errno. Callers validate fields.
	 */
	*value = res.a0;
	return 0;
}

static int s7d_hdmi_reg_write(void *context, unsigned int reg, unsigned int value)
{
	struct s7d_hdmi_bus *bus = context;
	struct arm_smccc_res res;

	if (bus->width == 8 && !s7d_hdmi_core_reg_valid(reg))
		return -EOPNOTSUPP;
	arm_smccc_smc(S7D_HDMI_WRITE, bus->base + reg, value, bus->width,
		      0, 0, 0, 0, &res);
	/*
	 * Vendor Linux and U-Boot specify no write-result ABI. A transport call
	 * is not proof that a write took effect: use checked configuration
	 * writes, or verify command completion/status for FIFO/W1C/pulses.
	 */
	return 0;
}

static const struct regmap_bus s7d_hdmi_regmap_bus = {
	.reg_read = s7d_hdmi_reg_read,
	.reg_write = s7d_hdmi_reg_write,
};

static const struct regmap_range s7d_hdmi_core_holes[] = {
	regmap_reg_range(S7D_DSC_FIRST, S7D_DSC_LAST),
};

static const struct regmap_access_table s7d_hdmi_core_access = {
	.no_ranges = s7d_hdmi_core_holes,
	.n_no_ranges = ARRAY_SIZE(s7d_hdmi_core_holes),
};

static struct regmap *s7d_hdmi_regmap(struct platform_device *pdev,
				    const char *name, unsigned int width)
{
	struct device *dev = &pdev->dev;
	const struct regmap_config config = {
		.name = name,
		.reg_bits = 16,
		.val_bits = width,
		.reg_stride = width / 8,
		.max_register = SZ_64K - width / 8,
		.cache_type = REGCACHE_NONE,
		.rd_table = width == 8 ? &s7d_hdmi_core_access : NULL,
		.wr_table = width == 8 ? &s7d_hdmi_core_access : NULL,
	};
	struct s7d_hdmi_bus *bus;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res || resource_size(res) != SZ_64K || !IS_ALIGNED(res->start, SZ_64K) ||
	    res->end > U32_MAX)
		return ERR_PTR(-EINVAL);
	if (!devm_request_mem_region(dev, res->start, resource_size(res), dev_name(dev)))
		return ERR_PTR(-EBUSY);
	bus = devm_kzalloc(dev, sizeof(*bus), GFP_KERNEL);
	if (!bus)
		return ERR_PTR(-ENOMEM);
	bus->base = res->start;
	bus->width = width;
	/* Secure registers are never ioremapped for non-secure MMIO accesses. */
	return devm_regmap_init(dev, &s7d_hdmi_regmap_bus, bus, &config);
}

int s7d_hdmi_init_regmaps(struct platform_device *pdev, struct regmap **core,
			struct regmap **top)
{
	*core = s7d_hdmi_regmap(pdev, "core", 8);
	if (IS_ERR(*core))
		return PTR_ERR(*core);
	*top = s7d_hdmi_regmap(pdev, "top", 32);
	return PTR_ERR_OR_ZERO(*top);
}

int s7d_hdmi_update_checked(struct regmap *map, unsigned int reg,
			   unsigned int mask, unsigned int value)
{
	unsigned int actual;
	int ret;

	if (value & ~mask)
		return -EINVAL;
	ret = regmap_update_bits(map, reg, mask, value);
	if (ret)
		return ret;
	ret = regmap_read(map, reg, &actual);
	if (ret)
		return ret;
	if ((actual & mask) != value) {
		dev_err(regmap_get_device(map),
			"TX readback reg %#x actual %#x mask %#x expected %#x\n",
			reg, actual, mask, value);
		return -EIO;
	}
	return 0;
}
