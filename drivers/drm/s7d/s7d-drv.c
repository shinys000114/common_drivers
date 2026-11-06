// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/types.h>
#include <linux/amlogic/cpu_info.h>
#include <linux/amlogic/cpu_version.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "s7d-fb.h"
#include "s7d-gem.h"
#include "s7d-hdmi.h"
#include "s7d-hdmi-output.h"
#include "s7d-hooks.h"
#include "s7d-plane.h"
#include "s7d-vendor-props.h"
#include "s7d-vpu.h"
#include "s7d-writeback.h"
#include "s7d-vdin.h"

/*
 * Keep a running firmware output that the first commit would set up the
 * same way, instead of stopping it for a full modeset, as i915 fastboot.
 */
static bool fastboot;
module_param(fastboot, bool, 0444);
MODULE_PARM_DESC(fastboot, "Take a matching firmware display over without a modeset (default: false)");

struct s7d_drm {
	struct drm_device drm;
	struct s7d_vpu vpu;
	struct drm_bridge *bridge;
	struct drm_connector_funcs connector_funcs;
	void (*connector_reset)(struct drm_connector *connector);
	struct drm_connector_helper_funcs connector_helpers;
	int (*get_modes)(struct drm_connector *connector);
};

static void s7d_connector_reset(struct drm_connector *connector)
{
	struct s7d_drm *display = container_of(connector->dev, struct s7d_drm, drm);

	display->connector_reset(connector);
	/* As the HDMI connector reset helper: default to the property maximum. */
	if (connector->state) {
		connector->state->max_requested_bpc = S7D_HDMI_MAX_BPC;
		connector->state->max_bpc = S7D_HDMI_MAX_BPC;
	}
}

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
	.desc = "Amlogic S7D display",
	.major = 1,
	.minor = 0,
};

static int s7d_output_route(u32 connector_mask, u32 writeback_mask, bool active,
			   bool *writeback_only)
{
	if (active && !connector_mask)
		return -EINVAL;
	*writeback_only = connector_mask && !(connector_mask & ~writeback_mask);
	return 0;
}

static int s7d_check_output_routes(struct drm_device *drm, struct drm_atomic_state *state)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	struct drm_crtc_state *old, *base;
	struct drm_crtc *crtc;
	u32 writeback_mask = 0;
	int i, ret;

	drm_connector_list_iter_begin(drm, &iter);
	drm_for_each_connector_iter(connector, &iter)
		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			writeback_mask |= drm_connector_mask(connector);
	drm_connector_list_iter_end(&iter);
	for_each_oldnew_crtc_in_state(state, crtc, old, base, i) {
		struct s7d_crtc_state *next = to_s7d_crtc_state(base);

		ret = s7d_output_route(base->connector_mask, writeback_mask,
				       base->active, &next->writeback_only);
		if (ret)
			return ret;
		if (old->active && to_s7d_crtc_state(old)->writeback_only != next->writeback_only)
			base->mode_changed = true;
	}
	return 0;
}

static int s7d_writeback_output(const struct drm_crtc_state *base, unsigned long reference,
			       struct s7d_hdmi_output_state *output)
{
	struct s7d_hdmi_output_state next;
	struct s7d_encp_state encp;
	int ret;

	if (s7d_encp_build_state(&base->adjusted_mode, &encp) != MODE_OK)
		return -EINVAL;
	/* Plan internal RGB8/full independently of HDMI sink capabilities. */
	ret = s7d_hdmi_output_build(S7D_HDMI_OUTPUT_RGB8,
		(unsigned long)s7d_mode_clock(&base->adjusted_mode) * 1000, reference, &next);
	if (!ret)
		ret = s7d_hdmi_output_apply_ctm(&next, base->ctm ? base->ctm->data : NULL);
	if (!ret)
		*output = next;
	return ret;
}

static bool s7d_writeback_output_reusable(const struct s7d_crtc_state *before,
					 const struct s7d_crtc_state *next)
{
	const struct drm_crtc_state *old = &before->base, *base = &next->base;

	return old->active && base->active && before->valid && before->writeback_only &&
		next->writeback_only && !drm_atomic_crtc_needs_modeset(base) &&
		!base->color_mgmt_changed && !base->vrr_enabled &&
		old->ctm == base->ctm && old->gamma_lut == base->gamma_lut &&
		old->degamma_lut == base->degamma_lut &&
		old->mode.clock == base->mode.clock && old->mode.flags == base->mode.flags &&
		old->adjusted_mode.clock == base->adjusted_mode.clock &&
		old->adjusted_mode.flags == base->adjusted_mode.flags &&
		drm_mode_equal(&old->mode, &base->mode) &&
		drm_mode_equal(&old->adjusted_mode, &base->adjusted_mode) &&
		before->output.encoding == S7D_HDMI_OUTPUT_RGB8 && before->output.bpc == 8 &&
		!before->output.rgb_limited && s7d_hdmi_output_same(&before->output, &next->output);
}

static int s7d_build_writeback_outputs(struct drm_device *drm, struct drm_atomic_state *state)
{
	struct s7d_drm *display = container_of(drm, struct s7d_drm, drm);
	struct drm_crtc_state *old, *base;
	struct drm_crtc *crtc;
	int i, ret;

	for_each_oldnew_crtc_in_state(state, crtc, old, base, i) {
		struct s7d_crtc_state *next = to_s7d_crtc_state(base);
		struct s7d_hdmi_output_state output, others;

		if (!base->active || !next->writeback_only)
			continue;
		if (s7d_writeback_output_reusable(to_s7d_crtc_state(old), next))
			continue;
		ret = s7d_writeback_output(base, clk_get_rate(display->vpu.xtal), &output);
		if (ret)
			return ret;
		/* As on HDMI, a CTM change alone is a POST2 update over RDMA. */
		others = output;
		others.post2 = to_s7d_crtc_state(old)->output.post2;
		if (old->active && !s7d_hdmi_output_same(&to_s7d_crtc_state(old)->output, &others))
			base->mode_changed = true;
		next->output = output;
	}
	return 0;
}

static int s7d_atomic_check(struct drm_device *drm, struct drm_atomic_state *state)
{
	struct s7d_drm *display = container_of(drm, struct s7d_drm, drm);
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_plane *plane;
	struct drm_plane_state *old_plane_state, *new_plane_state;
	unsigned int checked_modesets = 0;
	bool late_modeset = false;
	int i, ret;

	for_each_oldnew_plane_in_state(state, plane, old_plane_state, new_plane_state, i) {
		if (old_plane_state->crtc) {
			crtc_state = drm_atomic_get_crtc_state(state, old_plane_state->crtc);
			if (IS_ERR(crtc_state))
				return PTR_ERR(crtc_state);
		}
		if (new_plane_state->crtc) {
			crtc_state = drm_atomic_get_crtc_state(state, new_plane_state->crtc);
			if (IS_ERR(crtc_state))
				return PTR_ERR(crtc_state);
		}
	}
	for_each_new_crtc_in_state(state, crtc, crtc_state, i)
		s7d_crtc_capture_input(crtc, state);
	ret = s7d_check_output_routes(drm, state);
	if (ret)
		return ret;
	/* Cursor updates also require VSYNC RDMA completion and buffer retirement. */
	state->legacy_cursor_update = false;
	ret = drm_atomic_helper_check_modeset(drm, state);
	if (ret)
		return ret;
	for_each_new_crtc_in_state(state, crtc, crtc_state, i)
		if (crtc_state->mode_changed)
			checked_modesets |= drm_crtc_mask(crtc);
	ret = s7d_build_writeback_outputs(drm, state);
	if (ret)
		return ret;
	if (drm->mode_config.normalize_zpos) {
		ret = drm_atomic_normalize_zpos(drm, state);
		if (ret)
			return ret;
	}
	ret = drm_atomic_helper_check_planes(drm, state);
	if (ret)
		return ret;
	for_each_new_crtc_in_state(state, crtc, crtc_state, i)
		if (crtc_state->mode_changed &&
		    !(checked_modesets & drm_crtc_mask(crtc)))
			late_modeset = true;
	/* AFBC route/mixed-frame guards can require a modeset after plane check. */
	if (late_modeset) {
		ret = drm_atomic_helper_check_modeset(drm, state);
		if (ret)
			return ret;
		ret = s7d_build_writeback_outputs(drm, state);
		if (ret)
			return ret;
		ret = drm_atomic_helper_check_planes(drm, state);
		if (ret)
			return ret;
	}
	return s7d_writeback_atomic_check(display->vpu.writeback, state);
}

static const struct drm_mode_config_funcs s7d_mode_config_funcs = {
	.fb_create = s7d_fb_create,
	.atomic_check = s7d_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void s7d_atomic_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *drm = state->dev;
	struct s7d_drm *display = container_of(drm, struct s7d_drm, drm);
	bool capture = s7d_writeback_has_job(display->vpu.writeback, state);
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	int i;

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_planes(drm, state, 0);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	/* Before hw_done, while these states are still the current ones. */
	for_each_new_crtc_in_state(state, crtc, crtc_state, i)
		s7d_crtc_ext_mode_set(crtc, crtc_state);
	drm_atomic_helper_fake_vblank(state);
	if (capture) {
		drm_atomic_helper_wait_for_flip_done(drm, state);
		s7d_crtc_writeback_capture(display->vpu.crtc, state, display->vpu.writeback);
		drm_atomic_helper_commit_hw_done(state);
	} else {
		drm_atomic_helper_commit_hw_done(state);
		/* RDMA completion and buffer retirement can span several vblanks. */
		drm_atomic_helper_wait_for_flip_done(drm, state);
	}
	drm_atomic_helper_cleanup_planes(drm, state);
}

static const struct drm_mode_config_helper_funcs s7d_mode_config_helper_funcs = {
	.atomic_commit_tail = s7d_atomic_commit_tail,
};

/* GEM buffers can outlive the device binding until their last user closes. */
static void s7d_release_dma_pool(struct drm_device *drm, void *dev)
{
	of_reserved_mem_device_release(dev);
}

/*
 * Order system sleep around the transmitter that drives the display, and
 * unbind the display first when the transmitter goes away.
 */
static int s7d_drm_link_hdmi(struct device *dev, struct drm_bridge *bridge)
{
	struct platform_device *hdmi = of_find_device_by_node(bridge->of_node);
	struct device_link *link;

	if (!hdmi)
		return -EPROBE_DEFER;
	link = device_link_add(dev, &hdmi->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&hdmi->dev);
	return link ? 0 : dev_err_probe(dev, -EINVAL, "HDMI device link\n");
}

/* The VPU local reset controller is a child that this probe registers. */
static bool s7d_drm_local_reset(struct device_node *np, int index)
{
	struct of_phandle_args args;
	struct device_node *parent;
	bool local;

	if (of_parse_phandle_with_args(np, "resets", "#reset-cells", index, &args))
		return false;
	parent = of_get_parent(args.np);
	local = args.np == np || parent == np;
	of_node_put(parent);
	of_node_put(args.np);
	return local;
}

/*
 * drm_dev_init() reserves the card and render minors. A probe deferred
 * after devm_drm_dev_alloc() gives them up to whichever DRM device comes
 * next, so wait for every supplier that can defer before allocating: look
 * up and release the clocks, the resets of other controllers and the
 * interrupts of the node. s7d_vpu_init() takes them again and reports any
 * other error. The power domain is attached before probe, and the HDMI
 * transmitter publishes its bridge only after its own resources.
 */
static int s7d_drm_preflight(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct reset_control *rstc;
	const char *name;
	struct clk *clk;
	int count, i, ret;

	count = of_count_phandle_with_args(np, "clocks", "#clock-cells");
	for (i = 0; i < count; i++) {
		clk = of_clk_get(np, i);
		if (PTR_ERR(clk) == -EPROBE_DEFER)
			return dev_err_probe(dev, -EPROBE_DEFER, "clock %d\n", i);
		if (!IS_ERR(clk))
			clk_put(clk);
	}
	count = of_count_phandle_with_args(np, "resets", "#reset-cells");
	for (i = 0; i < count; i++) {
		if (of_property_read_string_index(np, "reset-names", i, &name) ||
		    s7d_drm_local_reset(np, i))
			continue;
		/* Released: claims nothing, even against a current holder. */
		rstc = reset_control_get_exclusive_released(dev, name);
		if (PTR_ERR(rstc) == -EPROBE_DEFER)
			return dev_err_probe(dev, -EPROBE_DEFER, "%s reset\n", name);
		if (!IS_ERR(rstc))
			reset_control_put(rstc);
	}
	ret = platform_irq_count(pdev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "interrupts\n");
	return 0;
}

static int s7d_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct drm_connector *connector;
	struct drm_encoder *encoder, *writeback_encoder;
	struct drm_bridge *bridge;
	struct s7d_vendor_objs objs;
	struct s7d_vpu_link link;
	struct s7d_vdin *vdin;
	struct drm_plane *primary, *cursor = NULL, *overlay, *video, *video2;
	struct drm_device *drm;
	struct s7d_drm *display;
	u8 major, revision;
	bool vendor_order;
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
	ret = s7d_drm_preflight(pdev);
	if (ret)
		return ret;
	ret = s7d_drm_link_hdmi(dev, bridge);
	if (ret)
		return ret;
	ret = dma_set_mask(dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;
	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	display = devm_drm_dev_alloc(dev, s7d_vendor_driver(&s7d_drm_driver),
				     struct s7d_drm, drm);
	if (IS_ERR(display))
		return PTR_ERR(display);
	drm = &display->drm;
	/* Do not compete with the firmware-sized default CMA or vendor GPU heap. */
	ret = of_reserved_mem_device_init_by_name(dev, dev->of_node, "scanout");
	if (ret)
		return dev_err_probe(dev, ret, "scanout DMA pool\n");
	ret = drmm_add_action_or_reset(drm, s7d_release_dma_pool, dev);
	if (ret)
		return ret;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &s7d_mode_config_funcs;
	drm->mode_config.helper_private = &s7d_mode_config_helper_funcs;
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	/* Framebuffers hold decoded video up to 4096x2304; modes stay within s7d_caps. */
	drm->mode_config.max_width = S7D_VIDEO_MAX_WIDTH;
	drm->mode_config.max_height = S7D_VIDEO_MAX_HEIGHT;
	drm->mode_config.cursor_width = 256;
	drm->mode_config.cursor_height = 256;
	drm->mode_config.normalize_zpos = true;
	ret = s7d_vpu_init(pdev, &display->vpu, &link);
	if (ret)
		return dev_err_probe(dev, ret, "VPU resources\n");
	display->vpu.fastboot = fastboot;
	/* Cache read-only retained-state admission before building IN_FORMATS. */
	ret = s7d_vpu_hold_boot(&display->vpu);
	if (ret)
		goto fini_vpu;
	s7d_vpu_debugfs_init(&display->vpu, drm);
	primary = s7d_plane_create(drm, BIT(0), DMA_BIT_MASK(36),
				   S7D_PLANE_PRIMARY,
				   IS_ENABLED(CONFIG_DRM_S7D_GFCD) ?
				   display->vpu.gfcd_available :
				   (display->vpu.afbc_available &&
				    (display->vpu.afbc_ready_mask & BIT(0))));
	if (IS_ERR(primary)) {
		ret = PTR_ERR(primary);
		goto fini_vpu;
	}
	/*
	 * Vendor userspace takes OSD and video planes by their order; the vendor
	 * driver has no cursor, so in vendor mode it comes after them, if at all.
	 */
	vendor_order = s7d_vendor_props_active(drm);
	if (!vendor_order) {
		cursor = s7d_plane_create(drm, BIT(0), DMA_BIT_MASK(36),
					  S7D_PLANE_CURSOR, false);
		if (IS_ERR(cursor)) {
			ret = PTR_ERR(cursor);
			goto fini_vpu;
		}
	}
	overlay = s7d_plane_create(drm, BIT(0), DMA_BIT_MASK(36), S7D_PLANE_RGB,
				   display->vpu.afbc_available &&
				   (display->vpu.afbc_ready_mask & BIT(1)));
	if (IS_ERR(overlay)) {
		ret = PTR_ERR(overlay);
		goto fini_vpu;
	}
	video = s7d_plane_create(drm, BIT(0), DMA_BIT_MASK(36),
				 S7D_PLANE_VIDEO, false);
	if (IS_ERR(video)) {
		ret = PTR_ERR(video);
		goto fini_vpu;
	}
	video2 = s7d_plane_create(drm, BIT(0), DMA_BIT_MASK(36),
				  S7D_PLANE_VIDEO2, false);
	if (IS_ERR(video2)) {
		ret = PTR_ERR(video2);
		goto fini_vpu;
	}
	if (vendor_order && s7d_plane_has_cursor(drm)) {
		cursor = s7d_plane_create(drm, BIT(0), DMA_BIT_MASK(36),
					  S7D_PLANE_CURSOR, false);
		if (IS_ERR(cursor)) {
			ret = PTR_ERR(cursor);
			goto fini_vpu;
		}
	}
	display->vpu.crtc = s7d_crtc_create(drm, primary, cursor, revision, &display->vpu.scanout,
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
	ret = drm_mode_create_content_type_property(drm);
	if (ret)
		goto fini_vpu;
	drm_connector_attach_content_type_property(connector);
	ret = drm_connector_attach_broadcast_rgb_property(connector);
	if (ret)
		goto fini_vpu;
	ret = drm_mode_create_hdmi_colorspace_property(connector,
		BIT(DRM_MODE_COLORIMETRY_SMPTE_170M_YCC) |
		BIT(DRM_MODE_COLORIMETRY_BT709_YCC) |
		BIT(DRM_MODE_COLORIMETRY_BT2020_RGB) |
		BIT(DRM_MODE_COLORIMETRY_BT2020_YCC));
	if (ret)
		goto fini_vpu;
	drm_object_attach_property(&connector->base, connector->colorspace_property,
				   DRM_MODE_COLORIMETRY_DEFAULT);
	ret = drm_connector_attach_hdr_output_metadata_property(connector);
	if (ret)
		goto fini_vpu;
	display->bridge = bridge;
	display->connector_funcs = *connector->funcs;
	display->connector_reset = display->connector_funcs.reset;
	display->connector_funcs.reset = s7d_connector_reset;
	connector->funcs = &display->connector_funcs;
	display->connector_helpers = *connector->helper_private;
	display->get_modes = display->connector_helpers.get_modes;
	display->connector_helpers.get_modes = s7d_connector_get_modes;
	display->connector_helpers.atomic_check = s7d_hdmi_connector_atomic_check;
	drm_connector_helper_add(connector, &display->connector_helpers);
	ret = drmm_add_action_or_reset(drm, s7d_connector_clear, bridge);
	if (ret)
		goto fini_vpu;
	ret = s7d_hdmi_bridge_set_connector(bridge, connector);
	if (ret)
		goto fini_vpu;
	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		goto fini_vpu;
	ret = s7d_vdin_init(&display->vpu, &vdin);
	if (ret)
		goto fini_vpu;
	display->vpu.vdin = vdin;
	encoder->possible_clones = drm_encoder_mask(encoder);
	/* Without VDIN1 everything else still works; there is no writeback. */
	if (vdin) {
		ret = s7d_writeback_init(drm, display->vpu.crtc,
			&s7d_vdin_writeback_caps, &s7d_vdin_writeback_ops, vdin,
			&display->vpu.writeback);
		if (ret)
			goto fini_vpu;
		writeback_encoder = s7d_writeback_encoder(display->vpu.writeback);
		encoder->possible_clones |= drm_encoder_mask(writeback_encoder);
		writeback_encoder->possible_clones = encoder->possible_clones;
	}
	drm_mode_config_reset(drm);
	if (!primary->state || !display->vpu.crtc->state || !connector->state ||
	    !bridge->base.state ||
	    (display->vpu.writeback &&
	     !s7d_writeback_connector(display->vpu.writeback)->state)) {
		ret = -ENOMEM;
		goto fini_vpu;
	}
	connector->ycbcr_420_allowed = true;
	ret = drm_connector_attach_max_bpc_property(connector, 8, S7D_HDMI_MAX_BPC);
	if (ret)
		goto fini_vpu;
	drm_crtc_vblank_off(display->vpu.crtc);
	platform_set_drvdata(pdev, display);
	objs = (struct s7d_vendor_objs) {
		.crtc = display->vpu.crtc,
		.primary = primary,
		.cursor = cursor,
		.overlay = overlay,
		.video = video,
		.video2 = video2,
		.hdmi = connector,
		.writeback = s7d_writeback_drm_connector(display->vpu.writeback),
		.bridge = bridge,
	};
	ret = s7d_vendor_init(drm, &objs);
	/* The minors are taken: a deferral could give card0 away. */
	if (WARN_ON(ret == -EPROBE_DEFER))
		ret = -EINVAL;
	if (ret)
		goto fini_vpu;
	/* No framebuffer/RDMA takeover occurs before the first atomic commit. */
	ret = drm_dev_register(drm, 0);
	if (ret)
		goto fini_vpu;
	drm_kms_helper_poll_init(drm);
	/* Userspace may already use the device; on failure keep plain KMS. */
	ret = s7d_vendor_register(drm);
	if (ret)
		drm_err(drm, "vendor interfaces unavailable: %d\n", ret);
	drm_fbdev_dma_setup(drm, 32);
	return 0;

fini_vpu:
	s7d_hdmi_bridge_set_connector(bridge, NULL);
	/*
	 * No commit can have started before a successful drm_dev_register();
	 * only a failed takeover restart can have kept the VPU.
	 */
	if (s7d_vpu_fini(&display->vpu))
		dev_err(dev, "keeping the VPU that the takeover left\n");
	return dev_err_probe(dev, ret, "DRM initialization\n");
}

static void s7d_drm_remove(struct platform_device *pdev)
{
	struct s7d_drm *display = platform_get_drvdata(pdev);
	struct drm_device *drm = &display->drm;
	int ret;

	s7d_vendor_unregister(drm);
	drm_dev_unplug(drm);
	drm_kms_helper_poll_fini(drm);
	drm_atomic_helper_shutdown(drm);
	ret = s7d_crtc_shutdown(display->vpu.crtc);
	if (!ret)
		ret = s7d_writeback_shutdown(display->vpu.writeback);
	s7d_hdmi_bridge_set_connector(display->bridge, NULL);
	if (!ret)
		ret = s7d_vpu_fini(&display->vpu);
	/* Free everything else; buffers the hardware may still read are leaked. */
	if (ret)
		dev_err(&pdev->dev, "display did not stop (%d); leaking its scanout buffers\n",
			ret);
}

static void s7d_drm_shutdown(struct platform_device *pdev)
{
	struct s7d_drm *display = platform_get_drvdata(pdev);
	int ret;

	drm_kms_helper_poll_fini(&display->drm);
	drm_atomic_helper_shutdown(&display->drm);
	ret = s7d_crtc_shutdown(display->vpu.crtc);
	if (!ret)
		ret = s7d_writeback_shutdown(display->vpu.writeback);
	if (ret)
		dev_err(&pdev->dev, "shutdown failed (%d); retaining DMA and resources\n", ret);
	/* No devres/DMA release here: failed stop is never permission to free. */
}

static int s7d_drm_suspend(struct device *dev)
{
	struct s7d_drm *display = dev_get_drvdata(dev);
	int ret, restore;

	ret = drm_mode_config_helper_suspend(&display->drm);
	if (ret)
		return ret;
	/* Atomic disable cannot return its hardware-stop error to the helper. */
	ret = s7d_crtc_last_error(display->vpu.crtc);
	/*
	 * Without a commit since boot, sleep may give up the firmware's
	 * display only when that output is off; a running one stays held.
	 * The CRTC lock keeps a hotplug-driven fbdev enable from racing it.
	 */
	if (!ret) {
		drm_modeset_lock(&display->vpu.crtc->mutex, NULL);
		ret = s7d_vpu_release_boot(&display->vpu);
		drm_modeset_unlock(&display->vpu.crtc->mutex);
	}
	if (!ret) {
		s7d_vendor_pm(&display->drm, false);
		return 0;
	}
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
	/* A sink swapped while asleep keeps HPD high; have clients reprobe. */
	drm_kms_helper_hotplug_event(&display->drm);
	s7d_vendor_pm(&display->drm, true);
	return ret ? ret : s7d_crtc_last_error(display->vpu.crtc);
}

static DEFINE_SIMPLE_DEV_PM_OPS(s7d_drm_pm_ops, s7d_drm_suspend, s7d_drm_resume);

static const struct of_device_id s7d_drm_match[] = {
	{ .compatible = "amlogic,s7d-vpu" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_drm_match);

static struct platform_driver s7d_drm_platform_driver = {
	.probe = s7d_drm_probe,
	.remove = s7d_drm_remove,
	.shutdown = s7d_drm_shutdown,
	.driver = {
		.name = "s7d-drm",
		.of_match_table = s7d_drm_match,
		.pm = pm_sleep_ptr(&s7d_drm_pm_ops),
	},
};

/* The transmitter registers its bridge before the display looks it up. */
static struct platform_driver * const s7d_drivers[] = {
	&s7d_hdmi_driver,
	&s7d_drm_platform_driver,
};

static int __init s7d_drm_init(void)
{
	return platform_register_drivers(s7d_drivers, ARRAY_SIZE(s7d_drivers));
}
module_init(s7d_drm_init);

static void __exit s7d_drm_exit(void)
{
	platform_unregister_drivers(s7d_drivers, ARRAY_SIZE(s7d_drivers));
}
module_exit(s7d_drm_exit);

MODULE_DESCRIPTION("Amlogic S7D DRM/KMS");
MODULE_LICENSE("GPL");
