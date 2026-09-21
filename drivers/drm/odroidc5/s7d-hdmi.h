/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_HDMI_H
#define __S7D_HDMI_H

struct drm_bridge;
struct s7d_vpu_link;

/* Obtain the native bridge's VPU handoff/prepare callbacks before attaching it. */
int s7d_hdmi_bridge_link(struct drm_bridge *bridge, struct s7d_vpu_link *link);

#endif
