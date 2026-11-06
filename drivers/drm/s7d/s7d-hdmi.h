/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_H
#define __S7D_HDMI_H

#include <drm/drm_modes.h>

#include "s7d-hdmi-output.h"

struct drm_atomic_state;
struct drm_bridge;
struct drm_connector;
struct drm_crtc;
struct drm_display_info;
struct drm_display_mode;
struct platform_driver;
struct s7d_vpu_link;

int s7d_hdmi_connector_atomic_check(struct drm_connector *connector,
				  struct drm_atomic_state *atomic);
/*
 * Whether @mode runs on @connector in the vendor output format @space
 * (HDMI_COLORSPACE_*) and @depth bits, at the default max bpc and Colorspace,
 * as the vendor TESTATTR answers. Reads the sink's display info, so call with
 * the mode config mutex held; touches no hardware.
 */
bool s7d_hdmi_vendor_format_valid(struct drm_connector *connector,
				  const struct drm_display_mode *mode, u8 space, u8 depth);
/*
 * Commit tail, from the flush of @crtc without a modeset, with no driver lock
 * held: apply what the commit changes in the vendor settings of the HDMI
 * connectors on @crtc, the AV mute as GCP Set_AVMUTE or Clear_AVMUTE and
 * ALLM and scan information in the InfoFrames. A modeset sends them with the
 * link setup.
 */
void s7d_hdmi_commit_vendor(struct drm_crtc *crtc, struct drm_atomic_state *atomic);

/* Whether the transmitter and the sink carry @mode with @encoding; no hardware access. */
enum drm_mode_status s7d_hdmi_link_valid(const struct drm_display_info *info,
					 enum s7d_hdmi_output_encoding encoding,
					 const struct drm_display_mode *mode);

/* Obtain the native bridge's VPU handoff/prepare callbacks before attaching it. */
extern struct platform_driver s7d_hdmi_driver;

int s7d_hdmi_bridge_link(struct drm_bridge *bridge, struct s7d_vpu_link *link);
/*
 * Fastboot, at probe: describe the link that the link readout matched, for
 * @mode and @output, in the bridge's reset state; uninherit resets it.
 */
int s7d_hdmi_bridge_inherit(struct drm_bridge *bridge, const struct drm_display_mode *mode,
			    const struct s7d_hdmi_output_state *output);
void s7d_hdmi_bridge_uninherit(struct drm_bridge *bridge);
void s7d_hdmi_bridge_eld_updated(struct drm_bridge *bridge, struct drm_connector *connector);
int s7d_hdmi_bridge_set_connector(struct drm_bridge *bridge, struct drm_connector *connector);

#endif
