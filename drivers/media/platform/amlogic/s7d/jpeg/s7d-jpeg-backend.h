/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_JPEG_BACKEND_H
#define S7D_JPEG_BACKEND_H

#include <linux/types.h>

struct device;
struct module;
struct s7d_codec_core;
struct s7d_codec_dos;
struct s7d_codec_firmware_owner;
struct s7d_codec_session;
struct s7d_jpeg_encoder;

struct s7d_jpeg_source {
	u32 format;
	u32 width;
	u32 height;
	u32 stride;
	const void *data;
	size_t bytes;
};

struct s7d_jpeg_encode_result {
	u32 bytesused;
	u32 scan_bytes;
	u64 generation;
};

/* Borrowed providers and module outlive every client, session and encoder. */
struct s7d_jpeg_encoder *
s7d_jpeg_encoder_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			struct module *owner);
struct s7d_codec_core *s7d_jpeg_encoder_core(struct s7d_jpeg_encoder *encoder);

/*
 * Synchronous worker call; session is OPEN HCODEC on encoder_core. Initial
 * NV12/NV21 is contiguous, even, at most 1920x1080, stride ALIGN(width, 32).
 * Caller excludes session_stop and retains CPU buffers until return. Neither
 * CPU pointer is retained afterwards, including when private DMA is retained
 * after an unproved halt. Negative return leaves capture and result untouched.
 */
int s7d_jpeg_encoder_encode(struct s7d_jpeg_encoder *encoder,
			    struct s7d_codec_session *session,
			    const struct s7d_jpeg_source *source,
			    void *capture, size_t capture_capacity,
			    unsigned int quality,
			    struct s7d_jpeg_encode_result *result);
void s7d_jpeg_encoder_request_stop(struct s7d_jpeg_encoder *encoder);
/* Gate/join frontend work first. Retry this before session_stop on failure. */
int s7d_jpeg_encoder_stop(struct s7d_jpeg_encoder *encoder,
			  struct s7d_codec_session *session);
/* Permanent admission gate; frontend sessions must still be stopped and put. */
int s7d_jpeg_encoder_shutdown(struct s7d_jpeg_encoder *encoder);
/* Caller joins/excludes API users and has stopped and put every session. */
int s7d_jpeg_encoder_destroy(struct s7d_jpeg_encoder *encoder);

#endif
