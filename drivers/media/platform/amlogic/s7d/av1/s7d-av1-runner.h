/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_RUNNER_H
#define S7D_AV1_RUNNER_H

#include "s7d-av1-protocol.h"
#include "s7d-av1-reference.h"

struct device;
struct s7d_codec_dos;
struct s7d_codec_firmware_owner;
struct s7d_codec_session;
struct s7d_av1_runner;

struct s7d_av1_header_result {
	struct s7d_av1_sequence sequence;
	struct s7d_av1_header header;
	u32 status;
	u32 staged_bytes;
	u32 shift_bytes;
	bool firmware_loaded;
	bool sequence_seen;
	bool header_ready;
	bool input_consumed;
};

struct s7d_av1_decode_result {
	struct s7d_av1_reference_output output;
	u32 shift_bytes;
	bool picture_decoded;
	bool show_existing;
	bool input_consumed;
	bool output_ready;
};

/* Parent owns the powered session, DOS, provider, engine and module lifetime. */
struct s7d_av1_runner *
s7d_av1_runner_create(struct device *dev, struct s7d_codec_dos *dos,
		      struct s7d_codec_firmware_owner *firmware,
		      struct s7d_hevc_engine *engine, u32 staging_capacity);
/* Exclusive HEVC lease; epochs increase after close. Returns a header, not a frame. */
int s7d_av1_runner_headers(struct s7d_av1_runner *runner,
			   struct s7d_codec_session *session, u64 lease_epoch,
			   u64 input_token, const u8 *data, size_t bytes,
			   struct s7d_av1_header_result *result);
/* One complete frame per TU; the first TU includes its sequence header. */
int s7d_av1_runner_decode(struct s7d_av1_runner *runner,
			  struct s7d_codec_session *session, u64 lease_epoch,
			  u64 input_token, const u8 *data, size_t bytes,
			  u64 timestamp_ns, struct s7d_av1_decode_result *result);
/* The linear shadow remains pinned until every output reader has finished. */
int s7d_av1_runner_output_committed(struct s7d_av1_runner *runner,
				    const struct s7d_av1_reference_output *output);
void s7d_av1_runner_request_stop(struct s7d_av1_runner *runner);
/* Stop/join submissions before close, then stop/release the session and DOS lease. */
int s7d_av1_runner_close(struct s7d_av1_runner *runner,
			 struct s7d_codec_session *session, u64 lease_epoch);
int s7d_av1_runner_destroy(struct s7d_av1_runner *runner);

#endif
