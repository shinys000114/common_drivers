/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_FB_H
#define __S7D_FB_H

struct drm_afbc_framebuffer;
struct drm_device;
struct drm_file;
struct drm_framebuffer;
struct drm_mode_fb_cmd2;

struct drm_framebuffer *
s7d_fb_create(struct drm_device *dev, struct drm_file *file,
	      const struct drm_mode_fb_cmd2 *cmd);
/* Borrowed metadata; the caller holds a framebuffer reference. */
const struct drm_afbc_framebuffer *
s7d_fb_afbc_metadata(const struct drm_framebuffer *fb);

#endif
