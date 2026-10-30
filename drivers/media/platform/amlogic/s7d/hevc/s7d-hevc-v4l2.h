/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_V4L2_H
#define S7D_HEVC_V4L2_H

struct device;
struct module;
struct s7d_codec_dos;
struct s7d_hevc_engine;
struct s7d_codec_firmware_owner;
struct s7d_codec_core;
struct s7d_v4l2_backend_factory;
struct s7d_hevc_v4l2;

struct s7d_hevc_v4l2 *
s7d_hevc_v4l2_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
		      struct s7d_codec_firmware_owner *borrowed_firmware,
		      struct s7d_hevc_engine *borrowed_engine, struct module *owner);
struct s7d_codec_core *s7d_hevc_v4l2_core(struct s7d_hevc_v4l2 *adapter);
const struct s7d_v4l2_backend_factory *
s7d_hevc_v4l2_factory(struct s7d_hevc_v4l2 *adapter);
int s7d_hevc_v4l2_suspend_check(struct s7d_hevc_v4l2 *adapter);
/* Join execution only; the frontend owner must stop sessions before teardown. */
int s7d_hevc_v4l2_shutdown(struct s7d_hevc_v4l2 *adapter);
/* Destroy this consumer before releasing DOS, firmware or the IRQ engine. */
int s7d_hevc_v4l2_destroy(struct s7d_hevc_v4l2 *adapter);

#endif
