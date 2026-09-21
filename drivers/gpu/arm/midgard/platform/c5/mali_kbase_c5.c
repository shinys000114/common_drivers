// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <mali_kbase.h>
#include <mali_kbase_config.h>
#include "mali_kbase_config_platform.h"

struct c5_gpu {
	struct reset_control *reset;
	struct clk_bulk_data clocks[BASE_MAX_NR_CLOCKS_REGULATORS];
	unsigned int num_clocks;
	bool clocked;
	bool state_lost;
};

static int c5_runtime_on(struct kbase_device *kbdev)
{
	struct c5_gpu *gpu = kbdev->platform_context;
	int ret;

	if (gpu->clocked)
		return 0;

	ret = clk_bulk_enable(gpu->num_clocks, gpu->clocks);
	if (ret)
		return ret;
	gpu->clocked = true;
	return 0;
}

static void c5_runtime_off(struct kbase_device *kbdev)
{
	struct c5_gpu *gpu = kbdev->platform_context;

	if (!gpu->clocked)
		return;

	/* The CSF backend has stopped the GPU before this callback. */
	clk_bulk_disable(gpu->num_clocks, gpu->clocks);
	gpu->clocked = false;
}

static int c5_platform_init(struct kbase_device *kbdev)
{
	struct c5_gpu *gpu;
	unsigned int i;
	int ret;

	if (!of_device_is_compatible(kbdev->dev->of_node, "amlogic,s7d-mali"))
		return dev_err_probe(kbdev->dev, -EINVAL,
				     "C5 backend requires the resource-based GPU DT\n");
	if (kbdev->nr_clocks != 2 || kbdev->nr_regulators)
		return dev_err_probe(kbdev->dev, -EINVAL,
				     "C5 requires core/stacks clocks and a fixed GPU supply\n");
	ret = dev_pm_opp_get_opp_count(kbdev->dev);
	if (ret <= 0)
		return dev_err_probe(kbdev->dev, ret < 0 ? ret : -EINVAL,
				     "C5 requires a valid OPP table\n");

	gpu = devm_kzalloc(kbdev->dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;
	gpu->reset = devm_reset_control_array_get_exclusive(kbdev->dev);
	if (IS_ERR(gpu->reset))
		return dev_err_probe(kbdev->dev, PTR_ERR(gpu->reset),
				     "failed to acquire GPU resets\n");
	if (!gpu->reset)
		return -EINVAL;
	gpu->num_clocks = kbdev->nr_clocks;
	for (i = 0; i < gpu->num_clocks; i++)
		gpu->clocks[i].clk = kbdev->clocks[i];
	kbdev->platform_context = gpu;

	ret = c5_runtime_on(kbdev);
	if (ret)
		return ret;
	ret = reset_control_assert(gpu->reset);
	if (ret)
		goto err_clocks;
	udelay(10);
	ret = reset_control_deassert(gpu->reset);
	if (ret)
		goto err_clocks;
	udelay(10);

	/*
	 * Same S7D power override as the vendor bring-up sequence. The GPU
	 * register map is initialized later, so use the mapped GPU aperture.
	 */
	writel(0x2968a819, kbdev->reg + 0x50);
	writel(0x00200fff, kbdev->reg + 0x58);
	return 0;

err_clocks:
	c5_runtime_off(kbdev);
	return ret;
}

static void c5_platform_term(struct kbase_device *kbdev)
{
	c5_runtime_off(kbdev);
}

static int c5_runtime_init(struct kbase_device *kbdev)
{
	int ret;

	ret = pm_runtime_set_active(kbdev->dev);
	if (ret)
		return ret;
	pm_runtime_set_autosuspend_delay(kbdev->dev, 100);
	pm_runtime_use_autosuspend(kbdev->dev);
	pm_runtime_enable(kbdev->dev);
	return 0;
}

static void c5_runtime_term(struct kbase_device *kbdev)
{
	pm_runtime_barrier(kbdev->dev);
	pm_runtime_disable(kbdev->dev);
	pm_runtime_dont_use_autosuspend(kbdev->dev);
}

static int c5_power_on(struct kbase_device *kbdev)
{
	struct c5_gpu *gpu = kbdev->platform_context;
	int ret = pm_runtime_resume_and_get(kbdev->dev);

	if (ret < 0) {
		dev_err(kbdev->dev, "GPU runtime resume failed: %d\n", ret);
		return ret;
	}
	if (gpu->state_lost || readl(kbdev->reg + 0x58) != 0x00200fff) {
		writel(0x2968a819, kbdev->reg + 0x50);
		writel(0x00200fff, kbdev->reg + 0x58);
		gpu->state_lost = false;
		return 1; /* GPU_STATE_LOST: request CSF reinitialization. */
	}
	/*
	 * The GPU has no switchable supply in the C5 DT. Runtime suspend
	 * gates clocks only, without resetting retained CSF state.
	 */
	return 0;
}

static void c5_power_off(struct kbase_device *kbdev)
{
	pm_runtime_mark_last_busy(kbdev->dev);
	pm_runtime_put_autosuspend(kbdev->dev);
}

static void c5_system_suspend(struct kbase_device *kbdev)
{
	struct c5_gpu *gpu = kbdev->platform_context;

	/* System firmware may reset the GPU even though the supply is fixed. */
	gpu->state_lost = true;
}

struct kbase_platform_funcs_conf c5_platform_funcs = {
	.platform_init_func = c5_platform_init,
	.platform_term_func = c5_platform_term,
};

struct kbase_pm_callback_conf c5_pm_callbacks = {
	.power_on_callback = c5_power_on,
	.power_off_callback = c5_power_off,
	.power_suspend_callback = c5_system_suspend,
	.power_runtime_init_callback = c5_runtime_init,
	.power_runtime_term_callback = c5_runtime_term,
	.power_runtime_on_callback = c5_runtime_on,
	.power_runtime_off_callback = c5_runtime_off,
};

static struct kbase_platform_config c5_platform_config;

struct kbase_platform_config *kbase_get_platform_config(void)
{
	return &c5_platform_config;
}
