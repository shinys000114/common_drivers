/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_FB_H
#define __S7D_FB_H

struct drm_afbc_framebuffer;
struct drm_device;
struct drm_file;
struct drm_framebuffer;
struct drm_mode_fb_cmd2;
struct s7d_afrc_framebuffer;

struct drm_framebuffer *
s7d_fb_create(struct drm_device *dev, struct drm_file *file,
	      const struct drm_mode_fb_cmd2 *cmd);
/*
 * A linear framebuffer around the boot logo that a running firmware output
 * scans out, for the state that the probe reads out of it. It has no GEM
 * object, so no file sees, maps or gets a handle of it, and no new plane
 * state may take it. Once armed, the last reference calls @release with
 * @data and the framebuffer, as it goes: arm it only when every reference
 * left is a state or the scanout owner, whose release proves that the
 * hardware no longer reads it.
 */
struct drm_framebuffer *s7d_fb_create_logo(struct drm_device *dev, u32 format,
					   u32 width, u32 height, u32 pitch,
					   void (*release)(void *data, struct drm_framebuffer *fb),
					   void *data);
bool s7d_fb_is_logo(const struct drm_framebuffer *fb);
void s7d_fb_arm_logo(struct drm_framebuffer *fb);
/* Borrowed metadata; the caller holds a framebuffer reference. */
const struct drm_afbc_framebuffer *
s7d_fb_afbc_metadata(const struct drm_framebuffer *fb);
const struct s7d_afrc_framebuffer *
s7d_fb_afrc_metadata(const struct drm_framebuffer *fb);

#endif
