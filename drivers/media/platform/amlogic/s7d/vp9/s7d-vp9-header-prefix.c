// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>

#include "s7d-vp9-header-prefix.h"

struct vp9_bits {
	const u8 *data;
	size_t bytes;
	u32 bit;
	int error;
};

static u32 read_bits(struct vp9_bits *bits, u32 count)
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

static int reference_valid(const struct s7d_vp9_reference_metadata *reference)
{
	if (!reference || !reference->valid)
		return -ENODATA;
	if (!reference->width || reference->width > 65536 ||
	    !reference->height || reference->height > 65536 ||
	    !reference->render_width || reference->render_width > 65536 ||
	    !reference->render_height || reference->render_height > 65536)
		return -EINVAL;
	return 0;
}

static void frame_size(struct vp9_bits *bits,
		       struct s7d_vp9_header_prefix *prefix)
{
	prefix->width = read_bits(bits, 16) + 1;
	prefix->height = read_bits(bits, 16) + 1;
}

static void render_size(struct vp9_bits *bits,
			struct s7d_vp9_header_prefix *prefix)
{
	if (read_bits(bits, 1)) {
		prefix->render_width = read_bits(bits, 16) + 1;
		prefix->render_height = read_bits(bits, 16) + 1;
	} else {
		prefix->render_width = prefix->width;
		prefix->render_height = prefix->height;
	}
}

static int intra_prefix(struct vp9_bits *bits,
			struct s7d_vp9_header_prefix *prefix)
{
	u32 sync = read_bits(bits, 24);

	if (bits->error)
		return bits->error;
	if (sync != 0x498342)
		return -EBADMSG;
	if (prefix->keyframe) {
		prefix->color_space = read_bits(bits, 3);
		prefix->full_range = read_bits(bits, 1);
		if (bits->error)
			return bits->error;
		if (prefix->color_space >= 6)
			return -EBADMSG;
		prefix->color_config_present = true;
		prefix->refresh_flags = 0xff;
	} else {
		prefix->color_space = 1;
		prefix->refresh_flags = read_bits(bits, 8);
	}
	frame_size(bits, prefix);
	render_size(bits, prefix);
	return bits->error;
}

static int inter_prefix(struct vp9_bits *bits,
			const struct s7d_vp9_reference_metadata references[8],
			struct s7d_vp9_header_prefix *prefix)
{
	int ret;
	u32 i;

	prefix->refresh_flags = read_bits(bits, 8);
	for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
		prefix->reference_index[i] = read_bits(bits, 3);
		prefix->sign_bias[i] = read_bits(bits, 1);
	}
	if (bits->error)
		return bits->error;
	if (!references)
		return -ENODATA;
	for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
		ret = reference_valid(&references[prefix->reference_index[i]]);
		if (ret)
			return ret;
	}
	for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
		if (read_bits(bits, 1)) {
			const struct s7d_vp9_reference_metadata *reference =
				&references[prefix->reference_index[i]];

			prefix->width = reference->width;
			prefix->height = reference->height;
			prefix->size_from_reference = true;
			break;
		}
	}
	if (!prefix->size_from_reference)
		frame_size(bits, prefix);
	render_size(bits, prefix);
	return bits->error;
}

int s7d_vp9_parse_header_prefix(const u8 *data, size_t bytes,
		const struct s7d_vp9_reference_metadata references[8],
		struct s7d_vp9_header_prefix *prefix)
{
	struct s7d_vp9_header_prefix next = { 0 };
	struct vp9_bits bits = { .data = data, .bytes = bytes };
	u32 marker, profile;
	int ret;

	if (!data || !prefix || !bytes)
		return -EINVAL;
	marker = read_bits(&bits, 2);
	profile = read_bits(&bits, 1);
	profile |= read_bits(&bits, 1) << 1;
	if (bits.error)
		return bits.error;
	if (marker != 2)
		return -EBADMSG;
	if (profile)
		return -EOPNOTSUPP;
	next.show_existing = read_bits(&bits, 1);
	if (next.show_existing) {
		const struct s7d_vp9_reference_metadata *reference;

		next.existing_index = read_bits(&bits, 3);
		if (bits.error)
			return bits.error;
		if (!references)
			return -ENODATA;
		reference = &references[next.existing_index];
		ret = reference_valid(reference);
		if (ret)
			return ret;
		next.width = reference->width;
		next.height = reference->height;
		next.render_width = reference->render_width;
		next.render_height = reference->render_height;
		next.show_frame = true;
		goto done;
	}
	next.keyframe = !read_bits(&bits, 1);
	next.show_frame = read_bits(&bits, 1);
	next.error_resilient = read_bits(&bits, 1);
	if (bits.error)
		return bits.error;
	if (!next.keyframe) {
		if (!next.show_frame)
			next.intra_only = read_bits(&bits, 1);
		if (!next.error_resilient)
			next.reset_frame_context = read_bits(&bits, 2);
		if (bits.error)
			return bits.error;
	}
	if (next.keyframe || next.intra_only)
		ret = intra_prefix(&bits, &next);
	else
		ret = inter_prefix(&bits, references, &next);
	if (ret)
		return ret;
done:
	next.prefix_bits = bits.bit;
	*prefix = next;
	return 0;
}
