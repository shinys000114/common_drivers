/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_JPEG_V4L2_H
#define S7D_JPEG_V4L2_H

struct device;
struct s7d_jpeg_encoder;
struct s7d_jpeg_v4l2;

/* The parent preserves the encoder, its core and providers until destroy. */
struct s7d_jpeg_v4l2 *
s7d_jpeg_v4l2_create(struct device *dev, struct s7d_jpeg_encoder *encoder);
int s7d_jpeg_v4l2_register(struct s7d_jpeg_v4l2 *device);
void s7d_jpeg_v4l2_unregister(struct s7d_jpeg_v4l2 *device);
/* Unregister, join clients and retry retained private hardware jobs. */
int s7d_jpeg_v4l2_shutdown(struct s7d_jpeg_v4l2 *device);
/* Open clients or unproved hardware retirement return -EBUSY. */
int s7d_jpeg_v4l2_destroy(struct s7d_jpeg_v4l2 *device);

#endif
