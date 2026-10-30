/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_V4L2_DEVICE_H
#define S7D_V4L2_DEVICE_H

#include "s7d-v4l2.h"

struct module;
struct s7d_v4l2_device;

struct s7d_v4l2_backend_factory {
	struct module *owner;
	struct s7d_v4l2_codec codec;
	const struct s7d_v4l2_backend_ops *ops;
	/* Ready must check a functional backend; create only allocates an instance. */
	int (*ready)(void *priv);
	void *(*create)(void *priv);
	/* Creation errors unwind themselves; destroy follows successful retirement. */
	void (*destroy)(void *priv, void *backend);
};

#define S7D_V4L2_MAX_CODECS	4

/* The caller serializes lifecycle calls and preserves core/provider ownership. */
/* Frontend retirement must gate and join all driver-owned producers. */
struct s7d_v4l2_device *s7d_v4l2_device_create(struct device *dev);
/* Before registration; the first codec added is the default of a new open. */
int s7d_v4l2_device_add_codec(struct s7d_v4l2_device *device,
			      struct s7d_codec_core *core,
			      const struct s7d_v4l2_backend_factory *factory, void *priv);
int s7d_v4l2_device_register(struct s7d_v4l2_device *device);
void s7d_v4l2_device_unregister(struct s7d_v4l2_device *device);
/* Unregister and join every client before backend/provider shutdown. */
int s7d_v4l2_device_shutdown(struct s7d_v4l2_device *device);
/* Busy clients retain the object; success waits for video release and consumes it. */
int s7d_v4l2_device_destroy(struct s7d_v4l2_device *device);

#endif
