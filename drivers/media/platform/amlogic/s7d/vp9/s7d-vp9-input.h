/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VP9_INPUT_H
#define S7D_VP9_INPUT_H

#include "s7d-codec-layout.h"
#include "s7d-vp9-header-prefix.h"

#define S7D_VP9_MAX_WIDTH 4096U
#define S7D_VP9_MAX_HEIGHT 2304U
/*
 * One VP9 frame: 2 MiB up to 1920x1088, 8 MiB above (UHD key frames of
 * 5.6 MiB occur in real streams). An OUTPUT buffer holds one packet, its
 * superframe index included, so it is bounded by the largest frame capacity.
 */
#define S7D_VP9_INPUT_HD_BYTES (2U * 1024U * 1024U)
#define S7D_VP9_INPUT_MAX_BYTES (8U * 1024U * 1024U)
#define S7D_VP9_OUTPUT_MAX_BYTES S7D_VP9_INPUT_MAX_BYTES
#define S7D_VP9_CAPTURE_MINIMUM 10U

struct s7d_vp9_input_bank;
struct s7d_vp9_input;
struct s7d_vp9_rpm;

struct s7d_vp9_input_result {
	const u8 *data;
	size_t bytes;
	/* CPU staging includes AMLV; DMA ring allocation belongs to the backend. */
	const u8 *staging;
	u32 decode_bytes;
	struct s7d_vp9_header_prefix prefix;
	struct s7d_codec_layout layout;
};

/* Largest frame the decoder takes for a picture of this size. */
u32 s7d_vp9_input_capacity(u32 width, u32 height);
/* Caller serializes the bank and keeps it alive until all inputs are released. */
struct s7d_vp9_input_bank *s7d_vp9_input_bank_create(void);
void s7d_vp9_input_bank_destroy(struct s7d_vp9_input_bank *bank);
/*
 * Forget every reference: only a key frame or an intra-only frame decodes
 * next, the others fail to prepare with -ENODATA.
 */
void s7d_vp9_input_bank_resync(struct s7d_vp9_input_bank *bank);
/* The frame header starts a key frame (of any profile). */
bool s7d_vp9_input_keyframe(const u8 *data, size_t bytes);
/* Profile 0/2 single frame; failed prepare changes neither bank nor out. */
int s7d_vp9_input_prepare(const struct s7d_vp9_input_bank *bank,
			 const u8 *data, size_t bytes, struct s7d_vp9_input **out);
/* Exact subframe span from a validated packet; do not parse an index again. */
int s7d_vp9_input_prepare_frame(const struct s7d_vp9_input_bank *bank,
			       const u8 *data, size_t bytes, struct s7d_vp9_input **out);
const struct s7d_vp9_input_result *s7d_vp9_input_view(const struct s7d_vp9_input *input);
/* Check before hardware admission; the caller permits only one active transaction. */
int s7d_vp9_input_check_current(const struct s7d_vp9_input_bank *bank,
			       const struct s7d_vp9_input *input);
/* Snapshot at firmware header pause; compare only present/effective prefix fields. */
int s7d_vp9_input_check_rpm(struct s7d_vp9_input *input,
			   const struct s7d_vp9_rpm *rpm);
/* Commit after successful decode or existing-frame presentation; hardware stays idle. */
int s7d_vp9_input_commit_metadata(struct s7d_vp9_input_bank *bank,
				 struct s7d_vp9_input *input);
void s7d_vp9_input_release(struct s7d_vp9_input *input);

#endif
