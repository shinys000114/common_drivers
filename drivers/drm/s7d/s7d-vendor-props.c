// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include "s7d-hooks.h"
#include "s7d-vendor-props.h"

/* No vendor property is attached yet. */
int s7d_vendor_props_attach(struct drm_device *drm, const struct s7d_vendor_objs *objs)
{
	return 0;
}
