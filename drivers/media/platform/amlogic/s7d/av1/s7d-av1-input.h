/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_INPUT_H
#define S7D_AV1_INPUT_H

#include <linux/types.h>

#define S7D_AV1_INPUT_MAX_BYTES (4U * 1024U * 1024U)
#define S7D_AV1_AMLV_BYTES 20U
#define S7D_AV1_TU_MAX_PICTURES 16U

struct s7d_av1_sequence;

enum s7d_av1_obu_type {
	S7D_AV1_OBU_SEQUENCE = 1,
	S7D_AV1_OBU_TEMPORAL_DELIMITER = 2,
	S7D_AV1_OBU_FRAME_HEADER = 3,
	S7D_AV1_OBU_TILE_GROUP = 4,
	S7D_AV1_OBU_METADATA = 5,
	S7D_AV1_OBU_FRAME = 6,
	S7D_AV1_OBU_REDUNDANT_HEADER = 7,
	S7D_AV1_OBU_TILE_LIST = 8,
	S7D_AV1_OBU_PADDING = 15,
};

struct s7d_av1_obu {
	u32 offset;
	u32 bytes;
	u32 payload_offset;
	u32 payload_bytes;
	u8 type;
	u8 temporal_id;
	u8 spatial_id;
};

struct s7d_av1_picture_span {
	u32 offset;
	u32 bytes;
};

struct s7d_av1_tu {
	u32 picture_count;
	struct s7d_av1_picture_span picture[S7D_AV1_TU_MAX_PICTURES];
};

struct s7d_av1_transport {
	u32 source_bytes;
	u32 staged_bytes;
	u32 obu_count;
	u32 sequence_count;
	u32 frame_header_count;
};

/* Complete TU boundaries; frame syntax remains subject to RPM validation. */
/* Immutable input; every byte is assigned once and failure preserves output. */
int s7d_av1_split_tu(const u8 *data, size_t bytes,
		     const struct s7d_av1_sequence *sequence, struct s7d_av1_tu *tu);

int s7d_av1_obu_next(const u8 *data, size_t bytes, u32 offset,
		      struct s7d_av1_obu *obu);
/* OBU boundaries and transport only; sequence/frame admission is separate. */
int s7d_av1_transport_size(const u8 *data, size_t bytes,
			   struct s7d_av1_transport *transport);
/* Input is immutable, non-overlapping memory; failure leaves output untouched. */
int s7d_av1_transport_write(const u8 *data, size_t bytes, u8 *output,
			    size_t capacity,
			    struct s7d_av1_transport *transport);

#endif
