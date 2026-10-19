// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "s7d-av1-input.h"
#include "s7d-av1-sequence.h"

int s7d_av1_obu_next(const u8 *data, size_t bytes, u32 offset,
		      struct s7d_av1_obu *obu)
{
	struct s7d_av1_obu next = { 0 };
	u64 payload = 0;
	u32 cursor, i;
	u8 header;

	if (!data || !obu || bytes > U32_MAX || offset >= bytes)
		return -EINVAL;
	header = data[offset];
	if ((header & 0x81) || !(header & 0x02))
		return -EINVAL;
	next.type = (header >> 3) & 0xf;
	if (!((next.type >= 1 && next.type <= 8) || next.type == 15))
		return -EOPNOTSUPP;
	next.offset = offset;
	cursor = offset + 1;
	if (header & 0x04) {
		u8 extension;

		if (cursor >= bytes)
			return -EINVAL;
		extension = data[cursor++];
		if (extension & 0x07)
			return -EINVAL;
		next.temporal_id = extension >> 5;
		next.spatial_id = (extension >> 3) & 3;
	}
	for (i = 0; i < 8; i++) {
		u8 byte;

		if (cursor >= bytes)
			return -EINVAL;
		byte = data[cursor++];
		payload |= (u64)(byte & 0x7f) << (i * 7);
		if (!(byte & 0x80))
			break;
	}
	if (i == 8)
		return -EINVAL;
	if (payload > U32_MAX)
		return -EOVERFLOW;
	if (payload > bytes - cursor)
		return -EINVAL;
	next.payload_offset = cursor;
	next.payload_bytes = payload;
	next.bytes = cursor - offset + payload;
	*obu = next;
	return 0;
}


int s7d_av1_split_tu(const u8 *data, size_t bytes,
		     const struct s7d_av1_sequence *sequence, struct s7d_av1_tu *tu)
{
	struct s7d_av1_tu next = { 0 };
	u32 offset = 0, start = 0;
	u8 frame = 0;
	bool show_existing = false, tiles = false, sequence_seen = false;
	int ret;

	if (!data || !sequence || !tu || !bytes)
		return -EINVAL;
	if (bytes > S7D_AV1_INPUT_MAX_BYTES)
		return -E2BIG;
	while (offset < bytes) {
		struct s7d_av1_obu obu;

		ret = s7d_av1_obu_next(data, bytes, offset, &obu);
		if (ret)
			return ret;
		if (obu.temporal_id || obu.spatial_id)
			return -EOPNOTSUPP;
		switch (obu.type) {
		case S7D_AV1_OBU_SEQUENCE:
			if (frame || sequence_seen || !obu.payload_bytes)
				return -EBADMSG;
			sequence_seen = true;
			break;
		case S7D_AV1_OBU_TEMPORAL_DELIMITER:
			if (offset || obu.payload_bytes)
				return -EBADMSG;
			break;
		case S7D_AV1_OBU_FRAME_HEADER:
		case S7D_AV1_OBU_FRAME:
			if (!obu.payload_bytes || (frame == S7D_AV1_OBU_FRAME_HEADER &&
			    !show_existing && !tiles))
				return -EBADMSG;
			if (next.picture_count == S7D_AV1_TU_MAX_PICTURES)
				return -E2BIG;
			if (frame) {
				next.picture[next.picture_count - 1].bytes = offset - start;
				start = offset;
			}
			next.picture[next.picture_count++].offset = start;
			frame = obu.type;
			show_existing = !sequence->reduced_still_picture_header &&
				(data[obu.payload_offset] & 0x80);
			if (show_existing && sequence->frame_id_numbers_present)
				return -EOPNOTSUPP;
			tiles = false;
			break;
		case S7D_AV1_OBU_TILE_GROUP:
			if (frame != S7D_AV1_OBU_FRAME_HEADER || show_existing ||
			    !obu.payload_bytes)
				return -EBADMSG;
			tiles = true;
			break;
		case S7D_AV1_OBU_METADATA:
			if (!obu.payload_bytes)
				return -EBADMSG;
			break;
		case S7D_AV1_OBU_PADDING:
			break;
		default:
			return -EOPNOTSUPP;
		}
		offset += obu.bytes;
	}
	if (!frame || (frame == S7D_AV1_OBU_FRAME_HEADER && !show_existing && !tiles))
		return -EBADMSG;
	next.picture[next.picture_count - 1].bytes = bytes - start;
	*tu = next;
	return 0;
}

int s7d_av1_transport_size(const u8 *data, size_t bytes,
			   struct s7d_av1_transport *transport)
{
	struct s7d_av1_transport next = { 0 };
	u32 offset = 0;
	int ret;

	if (!data || !transport || !bytes)
		return -EINVAL;
	if (bytes > S7D_AV1_INPUT_MAX_BYTES)
		return -E2BIG;
	while (offset < bytes) {
		struct s7d_av1_obu obu;
		u32 record;

		ret = s7d_av1_obu_next(data, bytes, offset, &obu);
		if (ret)
			return ret;
		if (obu.temporal_id || obu.spatial_id ||
		    obu.type == S7D_AV1_OBU_TILE_LIST)
			return -EOPNOTSUPP;
		if (obu.bytes > 0x0fffffffU - 4)
			return -EOVERFLOW;
		if (check_add_overflow(obu.bytes, S7D_AV1_AMLV_BYTES, &record) ||
		    check_add_overflow(next.staged_bytes, record, &next.staged_bytes))
			return -EOVERFLOW;
		next.obu_count++;
		if (obu.type == S7D_AV1_OBU_SEQUENCE)
			next.sequence_count++;
		if (obu.type == S7D_AV1_OBU_FRAME_HEADER ||
		    obu.type == S7D_AV1_OBU_FRAME)
			next.frame_header_count++;
		offset += obu.bytes;
	}
	next.source_bytes = bytes;
	*transport = next;
	return 0;
}

int s7d_av1_transport_write(const u8 *data, size_t bytes, u8 *output,
			    size_t capacity,
			    struct s7d_av1_transport *transport)
{
	struct s7d_av1_transport next;
	unsigned long input_end, output_end;
	u32 offset = 0, written = 0;
	int ret;

	if (!output || !transport)
		return -EINVAL;
	ret = s7d_av1_transport_size(data, bytes, &next);
	if (ret)
		return ret;
	if (capacity < next.staged_bytes)
		return -ENOSPC;
	if (check_add_overflow((unsigned long)data, bytes, &input_end) ||
	    check_add_overflow((unsigned long)output, (size_t)next.staged_bytes,
			       &output_end))
		return -EOVERFLOW;
	if ((unsigned long)data < output_end && (unsigned long)output < input_end)
		return -EINVAL;
	while (offset < bytes) {
		struct s7d_av1_obu obu;
		u8 *record = output + written;
		u32 size, i;

		/* The caller holds the same immutable input throughout both passes. */
		ret = s7d_av1_obu_next(data, bytes, offset, &obu);
		if (ret)
			return ret;
		size = obu.bytes + 4;
		put_unaligned_be32(size + 4, record);
		for (i = 0; i < 4; i++)
			record[4 + i] = ~record[i];
		put_unaligned_be32(1, record + 8);
		memcpy(record + 12, "AMLV", 4);
		record[16] = (size & 0x7f) | 0x80;
		record[17] = ((size >> 7) & 0x7f) | 0x80;
		record[18] = ((size >> 14) & 0x7f) | 0x80;
		record[19] = (size >> 21) & 0x7f;
		memcpy(record + S7D_AV1_AMLV_BYTES, data + offset, obu.bytes);
		offset += obu.bytes;
		written += obu.bytes + S7D_AV1_AMLV_BYTES;
	}
	*transport = next;
	return 0;
}
