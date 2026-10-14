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
struct s7d_hevc_input_result;
struct s7d_hevc_sps;
struct s7d_codec_layout;
struct s7d_v4l2_capture_buffer;

struct s7d_hevc_decode_result {
	u32 published_frames;
	bool skipped;
	bool cancelled;
};

/* The caller keeps DOS and firmware owner alive until destroy succeeds. */
struct s7d_hevc_decoder *
s7d_hevc_decoder_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			struct module *owner);
struct s7d_codec_core *s7d_hevc_decoder_core(struct s7d_hevc_decoder *decoder);
/* Gate and join frontend requests before stopping the borrowed session. */
/* Capture bindings remain live through successful stream_close. */
int s7d_hevc_decoder_bind(struct s7d_hevc_decoder *decoder,
			 struct s7d_codec_session *session,
			 const struct s7d_hevc_sps *sps,
			 const struct s7d_codec_layout *layout,
			 u64 generation, u64 parameter_epoch,
			 const struct s7d_v4l2_capture_buffer *capture, u32 count);
/* Immutable input and its typed view live until this synchronous call returns. */
int s7d_hevc_decoder_decode(struct s7d_hevc_decoder *decoder,
			   struct s7d_codec_session *session,
			   const struct s7d_hevc_input_result *input,
			   u64 input_token, u64 timestamp_ns,
			   const struct s7d_codec_timestamp_metadata *timestamp_meta,
			   struct s7d_hevc_decode_result *result);
/* QBUF wake only; safe while the executor waits at a firmware header pause. */
int s7d_hevc_decoder_capture_queue(struct s7d_hevc_decoder *decoder,
				  struct s7d_codec_surface *surface,
				  u64 surface_id, u64 generation);
int s7d_hevc_decoder_drain(struct s7d_hevc_decoder *decoder,
			  struct s7d_codec_session *session, u32 *published, bool *cancelled);
/* Before any AU execution, or after successful session_stop and joined work. */
int s7d_hevc_decoder_stream_close(struct s7d_hevc_decoder *decoder,
				 struct s7d_codec_session *session);
void s7d_hevc_decoder_request_stop(struct s7d_hevc_decoder *decoder);
/* Failure retains decoder, DOS dependency and DMA/capture custody. */
int s7d_hevc_decoder_destroy(struct s7d_hevc_decoder *decoder);

#endif
