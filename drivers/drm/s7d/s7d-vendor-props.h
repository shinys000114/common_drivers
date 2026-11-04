/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VENDOR_PROPS_H
#define __S7D_VENDOR_PROPS_H

struct drm_device;
struct s7d_vendor_objs;

int s7d_vendor_props_attach(struct drm_device *drm, const struct s7d_vendor_objs *objs);

#endif
