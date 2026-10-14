/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_INPUT_H
#define S7D_H264_INPUT_H

#include <linux/types.h>

#include "s7d-h264-layout.h"

struct s7d_codec_dos;
struct s7d_codec_session;
struct s7d_v4l2_input;
struct s7d_vdec_runtime;
struct s7d_h264_input;
struct s7d_h264_parameters;
struct s7d_h264_input_checkpoint;
struct s7d_codec_dma;
struct s7d_h264_slice;
struct s7d_v4l2_drain;

#define S7D_H264_AU_MAX_SLICES 128U
#define S7D_H264_PREFIX_MAX_BYTES (64U * 1024U)
#define S7D_H264_PARAMETERS_MAX_BYTES (64U * 1024U)
#define S7D_H264_ASSEMBLY_MAX_BYTES \
	(S7D_H264_PREFIX_MAX_BYTES + S7D_H264_PARAMETERS_MAX_BYTES)
#define S7D_H264_CHECKPOINT_MAX_BYTES \
	(16U * 1024U * 1024U + S7D_H264_ASSEMBLY_MAX_BYTES)

enum s7d_h264_input_kind {
	S7D_H264_INPUT_HEADERS,
	S7D_H264_INPUT_PICTURE,
	S7D_H264_INPUT_SKIP,
};

struct s7d_h264_preflight {
	enum s7d_h264_input_kind kind;
	bool sequence_ready;
	int skip_status;
	u32 picture_bytes;
	struct s7d_h264_sps sps;
};

struct s7d_h264_au_info {
	u32 pps_id;
	u32 sps_id;
	u32 frame_num;
	u32 idr_pic_id;
	u32 poc_type;
	u32 log2_frame_num;
	u32 log2_poc_lsb;
	u32 max_references;
	u32 poc_lsb;
	s32 delta_bottom;
	u32 coded_width;
	u32 coded_height;
	u32 slice_count;
	u32 last_vcl_end;
	struct s7d_codec_colour colour;
	struct s7d_h264_sps sps;
	u32 first_mb[S7D_H264_AU_MAX_SLICES];
	u8 slice_type[S7D_H264_AU_MAX_SLICES];
	u8 cavlc_tail_bits;
	bool idr;
	bool reference;
	bool no_output_of_prior_pics;
	bool long_term_reference;
};

struct s7d_h264_checkpoint_id {
	u64 input_token;
	u64 generation;
};

struct s7d_h264_input_plan {
	u32 start;
	u32 end;
	u32 write_pointer;
	u32 ring_bytes;
	u32 decode_bytes;
	u32 decode_bits;
	u64 token;
	u64 timestamp_ns;
};

struct s7d_h264_input_observation {
	u64 token;
	u64 timestamp_ns;
	u32 firmware_status;
	u32 remaining_bits;
	u32 read_pointer;
	u32 write_pointer;
	u32 fifo_control;
	u32 whole_bytes_consumed;
};

struct s7d_h264_input_return {
	u64 token;
	u64 timestamp_ns;
	int status;
};

int s7d_h264_input_build_plan(const struct s7d_v4l2_input *input,
			    struct s7d_h264_input_plan *plan);
/* CPU-only accepted state; serialized with its borrowing hardware epoch. */
struct s7d_h264_parameters *s7d_h264_parameters_create(void);
void s7d_h264_parameters_clear(struct s7d_h264_parameters *parameters);
void s7d_h264_parameters_destroy(struct s7d_h264_parameters *parameters);
/* The backend serializes calls and retains its original DOS/session/DMA refs. */
struct s7d_h264_input *
s7d_h264_input_create(struct s7d_codec_dos *dos, struct s7d_codec_session *session,
		      struct s7d_h264_parameters *parameters, bool hunting);
int s7d_h264_input_destroy(struct s7d_h264_input *input);
struct s7d_h264_input_checkpoint *s7d_h264_input_checkpoint_create(void);
/* CPU-only transfer; caller retains all old DMA and proves its physical stop. */
/* Successful take makes the old input terminal and eligible for destroy. */
int s7d_h264_input_checkpoint_take(struct s7d_h264_input_checkpoint *checkpoint,
				 struct s7d_h264_input *input,
				 const struct s7d_h264_checkpoint_id *id);
/* Fresh input, unchanged accepted bank, and a newer caller generation only. */
/* Restored buffer is s7d_v4l2_input; failure leaves state, DMA and outputs intact. */
int s7d_h264_input_checkpoint_restore(struct s7d_h264_input_checkpoint *checkpoint,
				    struct s7d_h264_input *input,
				    struct s7d_codec_dma *memory,
				    const struct s7d_h264_checkpoint_id *source,
				    u64 generation,
				    struct s7d_v4l2_input *buffer,
				    struct s7d_h264_preflight *preflight);
void s7d_h264_input_checkpoint_discard(struct s7d_h264_input_checkpoint **checkpoint);
/* CPU-only; retain this token and backing until acceptance, cancel or park. */
int s7d_h264_input_preflight(struct s7d_h264_input *input,
			    const struct s7d_v4l2_input *buffer,
			    struct s7d_h264_preflight *preflight);
/* The backend validates initial sequence metadata before acceptance. */
int s7d_h264_input_accept_headers(struct s7d_h264_input *input, u64 token,
				 struct s7d_h264_input_return *returned);
/* CPU-only HUNT completion; no firmware entry, park proof or terminal state. */
int s7d_h264_input_accept_skip(struct s7d_h264_input *input, u64 token,
			     struct s7d_h264_input_return *returned);
/* HUNT sequence rejection before hardware entry; discard all draft changes. */
int s7d_h264_input_reject_picture(struct s7d_h264_input *input, u64 token,
				int status,
				struct s7d_h264_input_return *returned);
/* Error-only CPU cancellation; any hardware-active input remains retained. */
int s7d_h264_input_cancel_preflight(struct s7d_h264_input *input, u64 token,
				   int status,
				   struct s7d_h264_input_return *returned);
/* After DOS prepare, before port_enable; any partial failure retains the input. */
int s7d_h264_input_prepare(struct s7d_h264_input *input, u64 token);
/* After firmware load acknowledgment, before IRQ/FIFO/CPU enable. */
int s7d_h264_input_configure(struct s7d_h264_input *input, u64 token);
int s7d_h264_input_enable(struct s7d_h264_input *input, u64 token);
/* A stable byte-count observation is not a DMA-idle or drain acknowledgment. */
int s7d_h264_input_observe(struct s7d_h264_input *input, u64 token,
			 struct s7d_h264_input_observation *observation);
/* Raw AU validation alone never proves entropy completion or DMA idleness. */
int s7d_h264_input_get_au(struct s7d_h264_input *input, u64 token,
			struct s7d_h264_au_info *au);
/* Only an actual SLICE01 with a completed immutable RPM snapshot qualifies. */
int s7d_h264_input_confirm_slice(struct s7d_h264_input *input, u64 token,
				 const struct s7d_h264_slice *slice,
				 u64 write_token);
/* After same CONFIG11/RPM and raw AU reconciliation, before F2. */
int s7d_h264_input_confirm_config(struct s7d_h264_input *input, u64 token,
				  u32 snapshot_sequence);
/* Reads PIC02/counter/MB/error; never halts, acknowledges or returns backing. */
int s7d_h264_input_confirm_picture(struct s7d_h264_input *input, u64 token,
				   u64 write_token);
/* Revalidates the saved completion before runtime park resets local blocks. */
int s7d_h264_input_get_picture(struct s7d_h264_input *input,
			     struct s7d_codec_session *session, u64 token,
			     u64 write_token,
			     struct s7d_h264_input_observation *picture);
/* Requires resumable park/context/IRQ/work proof, not final-stop proof. */
int s7d_h264_input_return_after_park(struct s7d_h264_input *input,
				  struct s7d_vdec_runtime *runtime,
				  u64 token, u64 write_token, u64 park_epoch,
				  struct s7d_h264_input_return *returned);
/* This tracks the input cut; it never publishes LAST or a drain notification. */
int s7d_h264_input_begin_drain(struct s7d_h264_input *input,
			     const struct s7d_v4l2_drain *request);
/* Logical empty/header-only cut with no hardware submission; no park proof. */
int s7d_h264_input_check_header_drain(struct s7d_h264_input *input,
				    const struct s7d_v4l2_drain *request);
int s7d_h264_input_check_drain(struct s7d_h264_input *input,
			     struct s7d_vdec_runtime *runtime,
			     const struct s7d_v4l2_drain *request);
/* After the frontend's matched START; retained parameter state is preserved. */
int s7d_h264_input_end_drain(struct s7d_h264_input *input,
			   const struct s7d_v4l2_drain *request);
/* Stop must succeed before the caller's final session put; error returns only. */
int s7d_h264_input_return_after_quiesce(struct s7d_h264_input *input,
				      struct s7d_vdec_runtime *runtime,
				      u64 token, int status,
				      struct s7d_h264_input_return *returned);

#endif
