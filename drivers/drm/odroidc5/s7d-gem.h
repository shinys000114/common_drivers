/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_GEM_H
#define __S7D_GEM_H

struct drm_device;
struct drm_file;
struct drm_mode_create_dumb;

int s7d_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			struct drm_mode_create_dumb *args);

#endif
