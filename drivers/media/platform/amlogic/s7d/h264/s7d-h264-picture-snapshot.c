/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/unaligned.h>
#include "s7d-h264-dpb.h"
#include "s7d-h264-picture-snapshot.h"

#define H264_RPM_BYTES S7D_H264_PICTURE_RPM_BYTES
#define H264_MAX_REFERENCES 16U

static u16 rpm_word(const void *rpm, u32 word)
{
	u32 index = (word & ~3U) + 3U - (word & 3U);

	return get_unaligned_le16((const u8 *)rpm + index * 2U);
}

static int parse_reorder(const void *rpm, u32 base,
			 struct s7d_h264_reorder *commands, u32 *count)
{
	u32 pos = 0, operation;

	while (pos < 66U) {
		operation = rpm_word(rpm, base + pos++);
		if (operation == 3)
			return 0;
		if (operation > 2 || pos == 66U ||
		    *count == S7D_H264_REF_LIST_SIZE)
			return -EBADMSG;
		commands[*count].operation = operation;
		commands[(*count)++].argument = rpm_word(rpm, base + pos++);
	}
	return -EBADMSG;
}

static int parse_mmco(const void *rpm, struct s7d_h264_slice *slice)
{
	u32 pos = 0, operation;
	struct s7d_h264_mmco *command;

	while (pos < 44U) {
		operation = rpm_word(rpm, 0x384U + pos++);
		if (!operation)
			return 0;
		if (operation > 6 || slice->mmco_count == S7D_H264_MMCO_SIZE)
			return -EBADMSG;
		command = &slice->mmco[slice->mmco_count++];
		command->operation = operation;
		if (operation == 1 || operation == 3) {
			if (pos == 44U)
				return -EBADMSG;
			command->difference_minus1 = rpm_word(rpm, 0x384U + pos++);
		}
		if (operation == 2 || operation == 3 ||
		    operation == 4 || operation == 6) {
			if (pos == 44U)
				return -EBADMSG;
			command->long_term_index = rpm_word(rpm, 0x384U + pos++);
		}
	}
	return -EBADMSG;
}

int s7d_h264_parse_slice(const void *rpm, size_t bytes,
			 struct s7d_h264_slice *slice)
{
	struct s7d_h264_slice parsed = { 0 };
	u32 nal, flags, structure;
	int ret;

	if (!rpm || !slice || bytes < H264_RPM_BYTES)
		return -EINVAL;
	if (rpm_word(rpm, 0x40) || rpm_word(rpm, 0x41))
		return -EOPNOTSUPP;
	structure = rpm_word(rpm, 0x7c);
	if (rpm_word(rpm, 0x84) != 1 || (structure != 0 && structure != 3) ||
	    rpm_word(rpm, 0x1cc) != 2 || rpm_word(rpm, 0x8b))
		return -EOPNOTSUPP;
	parsed.poc_type = rpm_word(rpm, 0x85);
	if (parsed.poc_type != 0 && parsed.poc_type != 2)
		return -EOPNOTSUPP;
	parsed.log2_frame_num = rpm_word(rpm, 0x83);
	parsed.log2_poc_lsb = rpm_word(rpm, 0x86);
	if (parsed.log2_frame_num < 4 || parsed.log2_frame_num > 16 ||
	    (parsed.poc_type == 0 && (parsed.log2_poc_lsb < 4 ||
				     parsed.log2_poc_lsb > 16)))
		return -EBADMSG;
	switch (rpm_word(rpm, 0x82)) {
	case 2:
		parsed.type = S7D_H264_SLICE_I;
		break;
	case 5:
		parsed.type = S7D_H264_SLICE_P;
		break;
	case 6:
		parsed.type = S7D_H264_SLICE_B;
		break;
	default:
		return -EOPNOTSUPP;
	}
	nal = rpm_word(rpm, 0x1cb);
	if ((nal & 0x1fU) != 1 && (nal & 0x1fU) != 5)
		return -EOPNOTSUPP;
	parsed.idr = (nal & 0x1fU) == 5;
	parsed.reference = !!(nal & 0x60U);
	parsed.frame_num = rpm_word(rpm, 0x1cd);
	parsed.idr_pic_id = rpm_word(rpm, 0x7f);
	parsed.first_mb = rpm_word(rpm, 0xf0);
	parsed.poc_lsb = rpm_word(rpm, 0x1ce);
	parsed.delta_bottom = (s32)((u32)rpm_word(rpm, 0x1d3) |
				 ((u32)rpm_word(rpm, 0x1d4) << 16));
	parsed.max_references = rpm_word(rpm, 0xe4);
	parsed.mode_8x8_flags = rpm_word(rpm, 0x8c);
	if (parsed.frame_num >= (1U << parsed.log2_frame_num) ||
	    (parsed.poc_type == 0 &&
	     parsed.poc_lsb >= (1U << parsed.log2_poc_lsb)) ||
	    parsed.max_references > H264_MAX_REFERENCES ||
	    (parsed.idr && (!parsed.reference || parsed.frame_num ||
			   parsed.type != S7D_H264_SLICE_I)))
		return -EBADMSG;
	if (parsed.type != S7D_H264_SLICE_I) {
		parsed.active[0] = (u32)rpm_word(rpm, 0x1cf) + 1U;
		if (parsed.active[0] > S7D_H264_REF_LIST_SIZE)
			return -EBADMSG;
		ret = parse_reorder(rpm, 0x300, parsed.reorder[0],
				    &parsed.reorder_count[0]);
		if (ret)
			return ret;
		if (parsed.reorder_count[0] > parsed.active[0])
			return -EBADMSG;
	}
	if (parsed.type == S7D_H264_SLICE_B) {
		parsed.active[1] = (u32)rpm_word(rpm, 0x1d0) + 1U;
		if (parsed.active[1] > S7D_H264_REF_LIST_SIZE)
			return -EBADMSG;
		ret = parse_reorder(rpm, 0x342, parsed.reorder[1],
				    &parsed.reorder_count[1]);
		if (ret)
			return ret;
		if (parsed.reorder_count[1] > parsed.active[1])
			return -EBADMSG;
	}
	if (parsed.idr) {
		flags = rpm_word(rpm, 0x384);
		parsed.long_term_reference = flags & 1U;
		parsed.no_output_of_prior_pics = flags & 2U;
	} else if (parsed.reference) {
		ret = parse_mmco(rpm, &parsed);
		if (ret)
			return ret;
	}
	*slice = parsed;
	return 0;
}
