// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "s7d-vp9-context.h"
#include "s7d-vp9-input.h"

struct s7d_vp9_input_bank {
	struct s7d_vp9_reference_metadata reference[S7D_VP9_REFERENCE_SLOTS];
	struct s7d_codec_colour colour;
	struct s7d_codec_layout layout;
	u64 revision;
	bool valid;
};

struct s7d_vp9_input {
	const struct s7d_vp9_input_bank *bank;
	struct s7d_vp9_reference_metadata reference[S7D_VP9_REFERENCE_SLOTS];
	struct s7d_codec_colour colour;
	struct s7d_vp9_input_result view;
	u8 *data;
	u64 revision;
	bool rpm_checked;
	bool committed;
};

static int vp9_colour(const struct s7d_vp9_header_prefix *prefix,
		      struct s7d_codec_colour *colour)
{
	struct s7d_codec_colour next = {
		.flags = S7D_CODEC_COLOUR_SIGNAL_PRESENT,
	};

	if (prefix->full_range)
		next.flags |= S7D_CODEC_COLOUR_FULL_RANGE;
	switch (prefix->color_space) {
	case 0:
		break;
	case 1:
	case 3:
		next.primaries = 6;
		next.transfer = 6;
		next.matrix = 6;
		next.flags |= S7D_CODEC_COLOUR_DESCRIPTION_PRESENT;
		break;
	case 2:
		next.primaries = 1;
		next.transfer = 1;
		next.matrix = 1;
		next.flags |= S7D_CODEC_COLOUR_DESCRIPTION_PRESENT;
		break;
	default:
		return -EOPNOTSUPP;
	}
	*colour = next;
	return 0;
}

static int vp9_layout(const struct s7d_vp9_header_prefix *prefix,
		      const struct s7d_codec_colour *colour,
		      struct s7d_codec_layout *layout)
{
	struct s7d_codec_layout next = {
		.format = S7D_CODEC_CAPTURE_NV12M, .plane_count = 2,
		.coded_width = prefix->width, .coded_height = prefix->height,
		.visible_width = prefix->width, .visible_height = prefix->height,
		.dpb_limit = 8, .dpb_slots = 8, .current_slots = 1,
		.extra_surfaces = 1, .min_capture_buffers = S7D_VP9_CAPTURE_MINIMUM,
	};
	int ret;

	if (prefix->width < 64 || prefix->width > 1920 ||
	    prefix->height < 64 || prefix->height > 1080 ||
	    ((prefix->width | prefix->height) & 1) ||
	    prefix->width != prefix->render_width ||
	    prefix->height != prefix->render_height)
		return -EOPNOTSUPP;
	next.pitch[0] = ALIGN(prefix->width, S7D_CODEC_STORAGE_ALIGNMENT);
	next.pitch[1] = next.pitch[0];
	next.storage_height = ALIGN(prefix->height, S7D_CODEC_STORAGE_ALIGNMENT);
	if (check_mul_overflow(next.pitch[0], next.storage_height, &next.size[0]))
		return -EOVERFLOW;
	next.size[1] = next.size[0] / 2;
	if (check_add_overflow(next.size[0], next.size[1], &next.total_size))
		return -EOVERFLOW;
	ret = s7d_codec_resolve_colour(colour, prefix->width, prefix->height,
				     &next.colourimetry);
	if (!ret)
		ret = s7d_codec_validate_layout(&next);
	if (!ret)
		*layout = next;
	return ret;
}

static bool same_sequence(const struct s7d_codec_layout *a,
			  const struct s7d_codec_layout *b)
{
	return a->coded_width == b->coded_width && a->coded_height == b->coded_height &&
		a->colourimetry.colorspace == b->colourimetry.colorspace &&
		a->colourimetry.xfer_func == b->colourimetry.xfer_func &&
		a->colourimetry.ycbcr_enc == b->colourimetry.ycbcr_enc &&
		a->colourimetry.quantization == b->colourimetry.quantization;
}

struct s7d_vp9_input_bank *s7d_vp9_input_bank_create(void)
{
	struct s7d_vp9_input_bank *bank = kzalloc(sizeof(*bank), GFP_KERNEL);

	return bank ?: ERR_PTR(-ENOMEM);
}

void s7d_vp9_input_bank_destroy(struct s7d_vp9_input_bank *bank)
{
	if (!IS_ERR_OR_NULL(bank))
		kfree(bank);
}

int s7d_vp9_input_prepare(const struct s7d_vp9_input_bank *bank,
			 const u8 *data, size_t bytes, struct s7d_vp9_input **out)
{
	struct s7d_vp9_packet packet;
	int ret;

	if (IS_ERR_OR_NULL(bank) || !data || !bytes || !out)
		return -EINVAL;
	if (bytes > S7D_VP9_INPUT_MAX_BYTES)
		return -E2BIG;
	if (bank->revision == U64_MAX)
		return -EOVERFLOW;
	ret = s7d_vp9_split_packet(data, bytes, &packet);
	if (ret)
		return ret;
	if (packet.count != 1)
		return -EOPNOTSUPP;
	return s7d_vp9_input_prepare_frame(bank, data + packet.frame[0].offset,
					 packet.frame[0].size, out);
}

int s7d_vp9_input_prepare_frame(const struct s7d_vp9_input_bank *bank,
			 const u8 *data, size_t bytes, struct s7d_vp9_input **out)
{
	struct s7d_vp9_input_result view = { };
	struct s7d_codec_colour colour;
	struct s7d_vp9_input *input;
	u32 i;
	int ret;

	if (IS_ERR_OR_NULL(bank) || !data || !bytes || !out)
		return -EINVAL;
	if (bytes > S7D_VP9_INPUT_MAX_BYTES)
		return -E2BIG;
	if (bank->revision == U64_MAX)
		return -EOVERFLOW;
	view.bytes = bytes;
	ret = s7d_vp9_parse_header_prefix(data, view.bytes, bank->reference, &view.prefix);
	if (ret)
		return ret;
	colour = bank->colour;
	if (view.prefix.keyframe || view.prefix.intra_only) {
		ret = vp9_colour(&view.prefix, &colour);
		if (ret)
			return ret;
	} else {
		if (!bank->valid)
			return -ENODATA;
		for (i = 0; !view.prefix.show_existing && i < S7D_VP9_FRAME_REFERENCES; i++) {
			const struct s7d_vp9_reference_metadata *ref =
				&bank->reference[view.prefix.reference_index[i]];

			if (ref->width != view.prefix.width || ref->height != view.prefix.height)
				return -EOPNOTSUPP;
		}
	}
	ret = vp9_layout(&view.prefix, &colour, &view.layout);
	if (ret)
		return ret;
	if (bank->valid && !same_sequence(&bank->layout, &view.layout))
		return -EOPNOTSUPP;
	if (check_add_overflow(view.bytes, 16U, &view.decode_bytes))
		return -EOVERFLOW;
	input = kzalloc(sizeof(*input), GFP_KERNEL);
	if (!input)
		return -ENOMEM;
	input->data = kvmalloc(view.decode_bytes, GFP_KERNEL);
	if (!input->data) {
		kfree(input);
		return -ENOMEM;
	}
	put_unaligned_be32(view.decode_bytes, input->data);
	for (i = 0; i < 4; i++)
		input->data[i + 4] = ~input->data[i];
	put_unaligned_be32(1, input->data + 8);
	memcpy(input->data + 12, "AMLV", 4);
	memcpy(input->data + 16, data, view.bytes);
	view.staging = input->data;
	view.data = input->data + 16;
	input->view = view;
	input->colour = colour;
	input->bank = bank;
	input->revision = bank->revision;
	memcpy(input->reference, bank->reference, sizeof(input->reference));
	*out = input;
	return 0;
}

const struct s7d_vp9_input_result *s7d_vp9_input_view(const struct s7d_vp9_input *input)
{
	return input ? &input->view : NULL;
}

int s7d_vp9_input_check_current(const struct s7d_vp9_input_bank *bank,
			       const struct s7d_vp9_input *input)
{
	if (IS_ERR_OR_NULL(bank) || !input)
		return -EINVAL;
	if (input->committed || input->bank != bank || input->revision != bank->revision)
		return -ESTALE;
	return 0;
}

int s7d_vp9_input_check_rpm(struct s7d_vp9_input *input, const struct s7d_vp9_rpm *rpm)
{
	const struct s7d_vp9_header_prefix *prefix;
	u32 width, height, i, refs = 0;
	bool from_reference = false;

	if (!input || !rpm)
		return -EINVAL;
	if (input->committed)
		return -ESTALE;
	input->rpm_checked = false;
	prefix = &input->view.prefix;
	if (rpm->word[0] || rpm->word[1] || rpm->word[3] != !prefix->keyframe ||
	    rpm->word[4] != prefix->show_frame || rpm->word[5] != prefix->error_resilient ||
	    rpm->word[25] != 8 || rpm->word[7] > 1)
		return -EBADMSG;
	width = rpm->word[10];
	height = rpm->word[11];
	if (!prefix->keyframe &&
	    ((!prefix->show_frame && rpm->word[6] != prefix->intra_only) ||
	     rpm->word[9] != prefix->refresh_flags ||
	     (!prefix->error_resilient && rpm->word[8] != prefix->reset_frame_context)))
		return -EBADMSG;
	if (!prefix->keyframe && !prefix->intra_only) {
		for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++)
			refs = (refs << 4) | (prefix->reference_index[i] << 1) |
				prefix->sign_bias[i];
		if ((rpm->word[14] & 0xfff) != refs)
			return -EBADMSG;
		for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
			const struct s7d_vp9_reference_metadata *ref =
				&input->reference[prefix->reference_index[i]];

			if (!(rpm->word[15] & BIT(2 - i)))
				continue;
			width = ref->width;
			height = ref->height;
			from_reference = true;
			break;
		}
		if (from_reference != prefix->size_from_reference)
			return -EBADMSG;
	}
	if (width != prefix->width || height != prefix->height ||
	    (rpm->word[7] && (rpm->word[12] != prefix->render_width ||
			     rpm->word[13] != prefix->render_height)))
		return -EBADMSG;
	input->rpm_checked = true;
	return 0;
}

int s7d_vp9_input_commit_metadata(struct s7d_vp9_input_bank *bank,
				 struct s7d_vp9_input *input)
{
	const struct s7d_vp9_header_prefix *prefix;
	u32 i;
	int ret;

	ret = s7d_vp9_input_check_current(bank, input);
	if (ret)
		return ret;
	if (input->view.prefix.show_existing) {
		input->committed = true;
		return 0;
	}
	if (!input->rpm_checked)
		return -EPROTO;
	if (bank->revision == U64_MAX)
		return -EOVERFLOW;
	prefix = &input->view.prefix;
	for (i = 0; i < S7D_VP9_REFERENCE_SLOTS; i++) {
		if (!(prefix->refresh_flags & BIT(i)))
			continue;
		bank->reference[i] = (struct s7d_vp9_reference_metadata) {
			.width = prefix->width, .height = prefix->height,
			.render_width = prefix->render_width,
			.render_height = prefix->render_height, .valid = true,
		};
	}
	bank->colour = input->colour;
	bank->layout = input->view.layout;
	bank->valid = true;
	bank->revision++;
	input->committed = true;
	return 0;
}

void s7d_vp9_input_release(struct s7d_vp9_input *input)
{
	if (!input)
		return;
	kvfree(input->data);
	kfree(input);
}
