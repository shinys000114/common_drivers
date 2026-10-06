// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>

#include <dt-bindings/reset/amlogic,s7d-vpu-reset.h>
#include <linux/amlogic/s7d-vpu-reset.h>

#define VIU_SW_RESET (0x1a01 * 4)
#define AFBCD_TOP (0x1a0f * 4)

struct s7d_vpu_reset {
	struct reset_controller_dev rcdev;
	struct regmap *map;
	int (*check_access)(void *);
	void *data;
};

static const u8 reset_bits[] = { 22, 21, 20, 19 };

static int s7d_vpu_reset_level(struct reset_controller_dev *rcdev,
			     unsigned long id, bool assert)
{
	struct s7d_vpu_reset *r = container_of(rcdev, struct s7d_vpu_reset, rcdev);
	u32 value, mask;
	int ret;

	if (id >= ARRAY_SIZE(reset_bits))
		return -EINVAL;
	ret = r->check_access(r->data);
	if (ret)
		return ret;
	if (assert && id == S7D_VPU_RESET_AFBCD_LOGIC) {
		ret = regmap_read(r->map, AFBCD_TOP, &value);
		if (ret)
			return ret;
		if (!(value & BIT(23)))
			return -EOPNOTSUPP;
	}
	mask = BIT(reset_bits[id]);
	ret = regmap_update_bits(r->map, VIU_SW_RESET, mask, assert ? mask : 0);
	if (!ret)
		ret = regmap_read(r->map, VIU_SW_RESET, &value);
	if (ret)
		return ret;
	return (value & mask) == (assert ? mask : 0) ? 0 : -EIO;
}

static int s7d_vpu_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return s7d_vpu_reset_level(rcdev, id, true);
}

static int s7d_vpu_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return s7d_vpu_reset_level(rcdev, id, false);
}

static int s7d_vpu_reset_status(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct s7d_vpu_reset *r = container_of(rcdev, struct s7d_vpu_reset, rcdev);
	u32 value;
	int ret;

	if (id >= ARRAY_SIZE(reset_bits))
		return -EINVAL;
	ret = r->check_access(r->data);
	if (!ret)
		ret = regmap_read(r->map, VIU_SW_RESET, &value);
	return ret ? ret : !!(value & BIT(reset_bits[id]));
}

static const struct reset_control_ops s7d_vpu_reset_ops = {
	.assert = s7d_vpu_reset_assert,
	.deassert = s7d_vpu_reset_deassert,
	.status = s7d_vpu_reset_status,
};

static void s7d_vpu_reset_node_put(void *node)
{
	of_node_put(node);
}

int devm_s7d_vpu_reset_register(struct device *dev, struct regmap *map,
			      struct device_node *node,
			      int (*check_access)(void *), void *data)
{
	struct s7d_vpu_reset *r;
	int ret;

	if (!node || !map || !check_access)
		return -EINVAL;
	r = devm_kzalloc(dev, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->map = map;
	r->check_access = check_access;
	r->data = data;
	r->rcdev = (struct reset_controller_dev) {
		.ops = &s7d_vpu_reset_ops,
		.owner = THIS_MODULE,
		.dev = dev,
		.of_node = of_node_get(node),
		.of_reset_n_cells = 1,
		.nr_resets = ARRAY_SIZE(reset_bits),
	};
	ret = devm_add_action_or_reset(dev, s7d_vpu_reset_node_put, r->rcdev.of_node);
	return ret ? ret : devm_reset_controller_register(dev, &r->rcdev);
}
EXPORT_SYMBOL_GPL(devm_s7d_vpu_reset_register);
MODULE_LICENSE("GPL");
