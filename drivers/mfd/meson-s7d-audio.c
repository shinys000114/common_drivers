// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

static const struct regmap_config s7d_audio_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xffc,
	.fast_io = true,
};

static void s7d_audio_pm_put(void *data)
{
	pm_runtime_put_sync(data);
}

static int s7d_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *reset;
	struct regmap *map;
	struct clk *pclk;
	struct resource *res;
	void __iomem *base;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != 0x1000)
		return -EINVAL;
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	reset = devm_reset_control_get_exclusive(dev, "audio");
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get bus reset\n");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to power audio domain\n");
	ret = devm_add_action_or_reset(dev, s7d_audio_pm_put, dev);
	if (ret)
		return ret;

	/* Child clock and reset callbacks require a live register interface. */
	pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(pclk))
		return dev_err_probe(dev, PTR_ERR(pclk), "failed to enable bus clock\n");

	ret = reset_control_deassert(reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to release bus reset\n");

	map = devm_regmap_init_mmio(dev, base, &s7d_audio_regmap_config);
	if (IS_ERR(map))
		return PTR_ERR(map);

	return devm_of_platform_populate(dev);
}

static const struct of_device_id s7d_audio_of_match[] = {
	{ .compatible = "amlogic,s7d-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_audio_of_match);

static struct platform_driver s7d_audio_driver = {
	.probe = s7d_audio_probe,
	.driver = {
		.name = "meson-s7d-audio",
		.of_match_table = s7d_audio_of_match,
	},
};
module_platform_driver(s7d_audio_driver);

MODULE_DESCRIPTION("Amlogic S7D audio resource controller");
MODULE_LICENSE("GPL");
