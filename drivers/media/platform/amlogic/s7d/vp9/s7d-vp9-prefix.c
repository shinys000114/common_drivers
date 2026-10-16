// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/limits.h>

#include "s7d-vp9-prefix.h"

struct vp9_bits {
	const u8 *data;
	size_t bytes;
	u32 bit;
	int error;
};

static u32 vp9_read(struct vp9_bits *bits, u32 count)
{
	u32 value = 0;

	while (count--) {
		if (bits->error)
			return 0;
		if (bits->bit / 8 >= bits->bytes) {
			bits->error = -ENODATA;
			return 0;
		}
		value = (value << 1) |
			((bits->data[bits->bit / 8] >> (7 - bits->bit % 8)) & 1);
		bits->bit++;
	}
	return value;
}

int s7d_vp9_split_packet(const u8 *data, size_t bytes, struct s7d_vp9_packet *packet)
{
	struct s7d_vp9_packet next = { 0 };
	u32 marker, count, size_bytes, index_bytes, offset = 0;
	u32 i, j, pos, size, payload;

	if (!data || !packet || !bytes)
		return -EINVAL;
	if (bytes > U32_MAX)
		return -E2BIG;
	next.count = 1;
	next.payload_size = bytes;
	next.frame[0].size = bytes;
	marker = data[bytes - 1];
	if ((marker & 0xe0) != 0xc0)
		goto done;
	count = (marker & 7) + 1;
	size_bytes = ((marker >> 3) & 3) + 1;
	index_bytes = 2 + count * size_bytes;
	if (bytes < index_bytes || data[bytes - index_bytes] != marker)
		goto done;
	payload = bytes - index_bytes;
	pos = payload + 1;
	for (i = 0; i < count; i++) {
		size = 0;
		for (j = 0; j < size_bytes; j++)
			size |= (u32)data[pos++] << (8 * j);
		if (!size || size > payload - offset)
			return -EBADMSG;
		next.frame[i].offset = offset;
		next.frame[i].size = size;
		offset += size;
	}
	if (offset != payload)
		return -EBADMSG;
	next.count = count;
	next.payload_size = payload;
done:
	*packet = next;
	return 0;
}

int s7d_vp9_parse_keyframe_prefix(const u8 *data, size_t bytes,
				 struct s7d_vp9_keyframe_prefix *prefix)
{
	struct s7d_vp9_keyframe_prefix next = { 0 };
	struct vp9_bits bits = { .data = data, .bytes = bytes };
	u32 profile, marker, existing, inter, show, sync, render;

	if (!data || !prefix || !bytes)
		return -EINVAL;
	marker = vp9_read(&bits, 2);
	profile = vp9_read(&bits, 1);
	profile |= vp9_read(&bits, 1) << 1;
	if (bits.error)
		return bits.error;
	if (marker != 2)
		return -EBADMSG;
	if (profile)
		return -EOPNOTSUPP;
	existing = vp9_read(&bits, 1);
	if (bits.error)
		return bits.error;
	if (existing)
		return -EOPNOTSUPP;
	inter = vp9_read(&bits, 1);
	show = vp9_read(&bits, 1);
	next.error_resilient = vp9_read(&bits, 1);
	if (bits.error)
		return bits.error;
	if (inter || !show)
		return -EOPNOTSUPP;
	sync = vp9_read(&bits, 24);
	if (bits.error)
		return bits.error;
	if (sync != 0x498342)
		return -EBADMSG;
	next.color_space = vp9_read(&bits, 3);
	if (bits.error)
		return bits.error;
	if (next.color_space == 6 || next.color_space == 7)
		return -EBADMSG;
	next.full_range = vp9_read(&bits, 1);
	next.width = vp9_read(&bits, 16) + 1;
	next.height = vp9_read(&bits, 16) + 1;
	render = vp9_read(&bits, 1);
	if (render) {
		next.render_width = vp9_read(&bits, 16) + 1;
		next.render_height = vp9_read(&bits, 16) + 1;
	} else {
		next.render_width = next.width;
		next.render_height = next.height;
	}
	if (bits.error)
		return bits.error;
	next.prefix_bits = bits.bit;
	*prefix = next;
	return 0;
}
