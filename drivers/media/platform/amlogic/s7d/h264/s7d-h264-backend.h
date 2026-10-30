/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_BACKEND_H
#define S7D_H264_BACKEND_H

struct device;
struct module;
struct s7d_codec_dos;
struct s7d_codec_firmware_owner;
struct s7d_codec_core;
struct s7d_h264_decoder;
struct s7d_v4l2_backend_factory;

/* DOS and shared firmware owner outlive the decoder and its runtime. */
struct s7d_h264_decoder *
s7d_h264_decoder_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			struct module *owner);
struct s7d_codec_core *s7d_h264_decoder_core(struct s7d_h264_decoder *decoder);
const struct s7d_v4l2_backend_factory *
s7d_h264_decoder_factory(struct s7d_h264_decoder *decoder);
/* Errors retain the decoder and all borrowed-resource dependencies. */
int s7d_h264_decoder_destroy(struct s7d_h264_decoder *decoder);
int s7d_h264_decoder_suspend_check(struct s7d_h264_decoder *decoder);
/* Gate existing clients and join notifications before full quiesce. */
int s7d_h264_decoder_shutdown(struct s7d_h264_decoder *decoder);

#endif
