/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef S7D_VP9_BACKEND_H
#define S7D_VP9_BACKEND_H

#include "../common/s7d-codec-session.h"
#include "s7d-vp9-dpb.h"

struct device;
struct module;
struct s7d_codec_dos;
struct s7d_codec_firmware_owner;
struct s7d_hevc_engine;
struct s7d_codec_layout;
struct s7d_v4l2_capture_buffer;
struct s7d_vp9_input_bank;
struct s7d_vp9_input;
struct s7d_vp9_decoder;

struct s7d_vp9_decode_result {
	u32 published_frames;
	bool cancelled;
};

/* Borrowed owners remain live until destroy succeeds. */
struct s7d_vp9_decoder *
s7d_vp9_decoder_create(struct device *dev, struct s7d_codec_dos *dos,
			struct s7d_codec_firmware_owner *firmware,
			struct s7d_hevc_engine *engine, struct module *owner);
struct s7d_codec_core *s7d_vp9_decoder_core(struct s7d_vp9_decoder *decoder);
int s7d_vp9_decoder_bind(struct s7d_vp9_decoder *decoder,
			 struct s7d_codec_session *session,
			 const struct s7d_codec_layout *layout, u64 generation,
			 const struct s7d_v4l2_capture_buffer *capture, u32 count);
/* CPU input is no longer referenced on return, even when DMA custody is retained. */
int s7d_vp9_decoder_decode(struct s7d_vp9_decoder *decoder,
			   struct s7d_codec_session *session,
			   struct s7d_vp9_input_bank *bank, struct s7d_vp9_input *input,
			   u64 token, u64 timestamp_ns,
			   const struct s7d_codec_timestamp_metadata *timestamp_meta,
			   struct s7d_vp9_decode_result *result);
int s7d_vp9_decoder_capture_queue(struct s7d_vp9_decoder *decoder,
				  struct s7d_codec_surface *surface,
				  u64 surface_id, u64 generation);
int s7d_vp9_decoder_drain(struct s7d_vp9_decoder *decoder,
			  struct s7d_codec_session *session, u32 *published, bool *cancelled);
/* Session stop and frontend work join precede close. */
int s7d_vp9_decoder_stream_close(struct s7d_vp9_decoder *decoder,
				 struct s7d_codec_session *session);
void s7d_vp9_decoder_request_stop(struct s7d_vp9_decoder *decoder);
int s7d_vp9_decoder_destroy(struct s7d_vp9_decoder *decoder);

#endif
