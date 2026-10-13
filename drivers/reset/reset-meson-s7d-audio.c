// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define S7D_AUDIO_SW_RESET0	0x28

struct s7d_audio_reset {
	struct reset_controller_dev rcdev;
	struct regmap *map;
};

static int s7d_audio_reset_xlate(struct reset_controller_dev *rcdev,
			       const struct of_phandle_args *spec)
{
	u32 id = spec->args[0];

	if (id > 32 && id != 40)
		return -EINVAL;
	return id;
}

static int s7d_audio_reset_status(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	struct s7d_audio_reset *rst = container_of(rcdev, struct s7d_audio_reset, rcdev);
	u32 val;
	int ret;

	ret = regmap_read(rst->map, S7D_AUDIO_SW_RESET0 + (id / 32) * 4, &val);
	return ret ? ret : !!(val & BIT(id % 32));
}

static int s7d_audio_reset_update(struct reset_controller_dev *rcdev,
				unsigned long id, bool assert)
{
	struct s7d_audio_reset *rst = container_of(rcdev, struct s7d_audio_reset, rcdev);
	u32 mask = BIT(id % 32);
	int ret;

	ret = regmap_update_bits(rst->map, S7D_AUDIO_SW_RESET0 + (id / 32) * 4,
				 mask, assert ? mask : 0);
	if (ret)
		return ret;
	ret = s7d_audio_reset_status(rcdev, id);
	if (ret < 0)
		return ret;
	return ret == assert ? 0 : -EIO;
}

static int s7d_audio_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return s7d_audio_reset_update(rcdev, id, true);
}

static int s7d_audio_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return s7d_audio_reset_update(rcdev, id, false);
}

static const struct reset_control_ops s7d_audio_reset_ops = {
	.assert = s7d_audio_reset_assert,
	.deassert = s7d_audio_reset_deassert,
	.status = s7d_audio_reset_status,
};

static int s7d_audio_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s7d_audio_reset *rst;

	rst = devm_kzalloc(dev, sizeof(*rst), GFP_KERNEL);
	if (!rst)
		return -ENOMEM;
	rst->map = dev_get_regmap(dev->parent, NULL);
	if (!rst->map)
		return -EPROBE_DEFER;

	rst->rcdev.owner = THIS_MODULE;
	rst->rcdev.dev = dev;
	rst->rcdev.of_node = dev->of_node;
	rst->rcdev.ops = &s7d_audio_reset_ops;
	rst->rcdev.nr_resets = 41;
	rst->rcdev.of_reset_n_cells = 1;
	rst->rcdev.of_xlate = s7d_audio_reset_xlate;
	return devm_reset_controller_register(dev, &rst->rcdev);
}

static const struct of_device_id s7d_audio_reset_of_match[] = {
	{ .compatible = "amlogic,s7d-audio-reset" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_audio_reset_of_match);

static struct platform_driver s7d_audio_reset_driver = {
	.probe = s7d_audio_reset_probe,
	.driver = {
		.name = "meson-s7d-audio-reset",
		.of_match_table = s7d_audio_reset_of_match,
	},
};
module_platform_driver(s7d_audio_reset_driver);

MODULE_DESCRIPTION("Amlogic S7D audio block reset controller");
MODULE_LICENSE("GPL");
