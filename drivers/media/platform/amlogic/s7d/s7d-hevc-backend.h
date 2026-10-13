/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_BACKEND_H
#define S7D_HEVC_BACKEND_H

#include "s7d-codec-session.h"

struct device;
struct module;
struct s7d_codec_dos;
struct s7d_codec_firmware_owner;
struct s7d_hevc_decoder;

struct s7d_hevc_decode_result {
	struct s7d_codec_frame frame;
	u32 consumed_bytes;
};

/*
 * Caller keeps DOS, shared firmware owner and their module alive until destroy
 * succeeds. Join/exclude all other API callers before allowing destroy to
 * succeed; a successful destroy frees the decoder and permits no later calls.
 * Stop and put all frontend sessions before destroy. The shared firmware owner
 * has no borrower reference; its jobs count does not extend this loan.
 * Shared cache operations serialize through the device's exclusive DOS lease;
 * destroy both codec consumers before destroying the firmware owner.
 */
struct s7d_hevc_decoder *
s7d_hevc_decoder_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			struct module *owner);
struct s7d_codec_core *s7d_hevc_decoder_core(struct s7d_hevc_decoder *decoder);
/*
 * One VPS/SPS/PPS and one independent Main8 IDR; NV12M capture planes.
 * Session must be OPEN on decoder_core and surface already queued on it. Caller
 * joins execution before stopping that session, and owns capture exclusively;
 * this internal consumer neither waits nor publishes DMA-BUF fences.
 * A negative return leaves result untouched. Unproved halt retains DMA custody.
 * Frame callback runs under the core mutex and must not reenter core/backend.
 * Success proves full halt, not the caller's later session/DOS release.
 */
int s7d_hevc_decoder_decode_surface(struct s7d_hevc_decoder *decoder,
				    struct s7d_codec_session *session,
				    const void *annexb, size_t bytes,
				    struct s7d_codec_surface *surface,
				    u64 surface_id, u64 generation,
				    u64 timestamp_ns,
				    struct s7d_hevc_decode_result *result);
/* Latched until a successful new DOS hold; caller gates admission and joins work. */
void s7d_hevc_decoder_request_stop(struct s7d_hevc_decoder *decoder);
/* Failure retains the decoder, DOS dependency and DMA/capture custody. */
int s7d_hevc_decoder_destroy(struct s7d_hevc_decoder *decoder);

#endif
