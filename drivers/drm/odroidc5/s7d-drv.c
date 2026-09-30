// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/types.h>
#include <linux/amlogic/cpu_info.h>
#include <linux/amlogic/cpu_version.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "s7d-gem.h"
#include "s7d-hdmi.h"
#include "s7d-plane.h"
#include "s7d-vpu.h"

struct s7d_drm {
	struct drm_device drm;
	struct s7d_vpu vpu;
	struct drm_bridge *bridge;
	struct drm_connector_helper_funcs connector_helpers;
	int (*get_modes)(struct drm_connector *connector);
};

static int s7d_connector_get_modes(struct drm_connector *connector)
{
	struct s7d_drm *display = container_of(connector->dev, struct s7d_drm, drm);
	int count;

	count = display->get_modes(connector);
	s7d_hdmi_bridge_eld_updated(display->bridge, connector);
	return count;
}

static void s7d_connector_clear(struct drm_device *drm, void *data)
{
	s7d_hdmi_bridge_set_connector(data, NULL);
}

DEFINE_DRM_GEM_DMA_FOPS(s7d_drm_fops);

static const struct drm_driver s7d_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &s7d_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(s7d_gem_dumb_create),
	.name = "s7d-drm",
	.desc = "Amlogic S7D native display",
	.major = 1,
	.minor = 0,
};

static const struct drm_mode_config_funcs s7d_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void s7d_atomic_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *drm = state->dev;

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_planes(drm, state, 0);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	/* RDMA completion and buffer retirement can span several vblanks. */
	drm_atomic_helper_wait_for_flip_done(drm, state);
	drm_atomic_helper_cleanup_planes(drm, state);
}

static const struct drm_mode_config_helper_funcs s7d_mode_config_helper_funcs = {
	.atomic_commit_tail = s7d_atomic_commit_tail,
};

static void s7d_release_dma_pool(void *dev)
{
	of_reserved_mem_device_release(dev);
}

static int s7d_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_bridge *bridge;
	struct s7d_vpu_link link;
	struct drm_plane *primary;
	struct drm_device *drm;
	struct s7d_drm *display;
	u8 major, revision;
	int ret;

	ret = meson_cpu_version_read(MESON_CPU_VERSION_LVL_MAJOR, &major);
	if (ret)
		return dev_err_probe(dev, ret, "SoC identity\n");
	ret = meson_cpu_version_read(MESON_CPU_VERSION_LVL_MINOR, &revision);
	if (ret)
		return dev_err_probe(dev, ret, "SoC revision\n");
	if (major != MESON_CPU_MAJOR_ID_S7D || revision != S7D_OSD_REV_B)
		return dev_err_probe(dev, -ENODEV, "unsupported SoC %#x revision %#x\n",
				     major, revision);
	ret = drm_of_find_panel_or_bridge(dev->of_node, 0, 0, NULL, &bridge);
	if (ret)
		return dev_err_probe(dev, ret, "HDMI bridge\n");
	ret = s7d_hdmi_bridge_link(bridge, &link);
	if (ret)
		return dev_err_probe(dev, ret, "incompatible HDMI bridge\n");
	ret = dma_set_mask(dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;
	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	/* Do not compete with the firmware-sized default CMA or vendor GPU heap. */
	ret = of_reserved_mem_device_init_by_name(dev, dev->of_node, "scanout");
	if (ret)
		return dev_err_probe(dev, ret, "scanout DMA pool\n");
	ret = devm_add_action_or_reset(dev, s7d_release_dma_pool, dev);
	if (ret)
		return ret;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	display = devm_drm_dev_alloc(dev, &s7d_drm_driver, struct s7d_drm, drm);
	if (IS_ERR(display))
		return PTR_ERR(display);
	drm = &display->drm;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &s7d_mode_config_funcs;
	drm->mode_config.helper_private = &s7d_mode_config_helper_funcs;
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = 1920;
	drm->mode_config.max_height = 1080;
	ret = s7d_vpu_init(pdev, &display->vpu, &link);
	if (ret)
		return dev_err_probe(dev, ret, "VPU resources\n");
	primary = s7d_plane_create(drm, BIT(0), DMA_BIT_MASK(36));
	if (IS_ERR(primary)) {
		ret = PTR_ERR(primary);
		goto fini_vpu;
	}
	display->vpu.crtc = s7d_crtc_create(drm, primary, revision, &display->vpu.scanout,
					    &s7d_vpu_crtc_ops, &display->vpu);
	if (IS_ERR(display->vpu.crtc)) {
		ret = PTR_ERR(display->vpu.crtc);
		goto fini_vpu;
	}
	ret = drm_vblank_init(drm, 1);
	if (ret)
		goto fini_vpu;
	encoder = drmm_plain_encoder_alloc(drm, NULL, DRM_MODE_ENCODER_TMDS, "S7D HDMI");
	if (IS_ERR(encoder)) {
		ret = PTR_ERR(encoder);
		goto fini_vpu;
	}
	encoder->possible_crtcs = BIT(0);
	ret = drm_bridge_attach(encoder, bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		goto fini_vpu;
	connector = drm_bridge_connector_init(drm, encoder);
	if (IS_ERR(connector)) {
		ret = PTR_ERR(connector);
		goto fini_vpu;
	}
	display->bridge = bridge;
	display->connector_helpers = *connector->helper_private;
	display->get_modes = display->connector_helpers.get_modes;
	display->connector_helpers.get_modes = s7d_connector_get_modes;
	drm_connector_helper_add(connector, &display->connector_helpers);
	ret = drmm_add_action_or_reset(drm, s7d_connector_clear, bridge);
	if (ret)
		goto fini_vpu;
	s7d_hdmi_bridge_set_connector(bridge, connector);
	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		goto fini_vpu;
	drm_mode_config_reset(drm);
	if (!primary->state || !display->vpu.crtc->state || !connector->state ||
	    !bridge->base.state) {
		ret = -ENOMEM;
		goto fini_vpu;
	}
	drm_crtc_vblank_off(display->vpu.crtc);
	ret = s7d_vpu_hold_boot(&display->vpu);
	if (ret)
		goto fini_vpu;
	platform_set_drvdata(pdev, display);
	/* No framebuffer/RDMA takeover occurs before the first atomic commit. */
	ret = drm_dev_register(drm, 0);
	if (ret)
		goto fini_vpu;
	drm_kms_helper_poll_init(drm);
	drm_fbdev_dma_setup(drm, 32);
	return 0;

fini_vpu:
	s7d_hdmi_bridge_set_connector(bridge, NULL);
	/* No commit can have started before a successful drm_dev_register(). */
	WARN_ON(s7d_vpu_fini(&display->vpu));
	return dev_err_probe(dev, ret, "DRM initialization\n");
}

static void s7d_drm_shutdown(struct platform_device *pdev)
{
	struct s7d_drm *display = platform_get_drvdata(pdev);
	int ret;

	drm_kms_helper_poll_fini(&display->drm);
	drm_atomic_helper_shutdown(&display->drm);
	ret = s7d_crtc_shutdown(display->vpu.crtc);
	if (ret)
		dev_err(&pdev->dev, "shutdown failed (%d); retaining DMA and resources\n", ret);
	/* No devres/DMA release here: failed stop is never permission to free. */
}

static int s7d_drm_suspend(struct device *dev)
{
	struct s7d_drm *display = dev_get_drvdata(dev);
	int ret, restore;

	/* Only s2idle retains the shared domain and DDC/HPD clock context. */
	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE || display->vpu.boot_held)
		return -EBUSY;
	ret = drm_mode_config_helper_suspend(&display->drm);
	if (ret)
		return ret;
	/* Atomic disable cannot return its hardware-stop error to the helper. */
	ret = s7d_crtc_last_error(display->vpu.crtc);
	if (!ret)
		return 0;
	restore = drm_mode_config_helper_resume(&display->drm);
	if (!restore)
		restore = s7d_crtc_last_error(display->vpu.crtc);
	if (restore)
		dev_err(dev, "failed to restore display after aborted suspend: %d\n", restore);
	return ret;
}

static int s7d_drm_resume(struct device *dev)
{
	struct s7d_drm *display = dev_get_drvdata(dev);
	int ret;

	ret = drm_mode_config_helper_resume(&display->drm);
	return ret ? ret : s7d_crtc_last_error(display->vpu.crtc);
}

static int s7d_drm_freeze(struct device *dev)
{
	/* Hibernation needs restoration of context lost across power removal. */
	return -EBUSY;
}

static const struct dev_pm_ops s7d_drm_pm_ops = {
	.suspend = s7d_drm_suspend,
	.resume = s7d_drm_resume,
	.freeze = s7d_drm_freeze,
	.poweroff = s7d_drm_freeze,
};

static const struct of_device_id s7d_drm_match[] = {
	{ .compatible = "amlogic,s7d-vpu-native" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_drm_match);

/* Built-in/static DT: forbid unbind until failed-stop devres retention is supported. */
static struct platform_driver s7d_drm_platform_driver = {
	.probe = s7d_drm_probe,
	.shutdown = s7d_drm_shutdown,
	.driver = {
		.name = "s7d-drm",
		.of_match_table = s7d_drm_match,
		.suppress_bind_attrs = true,
		.pm = &s7d_drm_pm_ops,
	},
};
builtin_platform_driver(s7d_drm_platform_driver);

MODULE_DESCRIPTION("Amlogic S7D native DRM/KMS");
MODULE_LICENSE("GPL");
