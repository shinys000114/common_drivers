// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include "s7d-hevc-sequence.h"

#define HEVC_MAX_SUB_LAYERS 7U
#define HEVC_MAX_DPB 16U
#define HEVC_MAX_RPS 64U

struct hevc_bits {
	const u8 *data;
	size_t bytes;
	size_t bit;
	int error;
};

static u32 hevc_get(struct hevc_bits *b, u32 count)
{
	u32 value = 0, i;

	if (b->error)
		return 0;
	if (count > 32 || b->bit > b->bytes * 8 ||
	    count > b->bytes * 8 - b->bit) {
		b->error = -EBADMSG;
		return 0;
	}
	for (i = 0; i < count; i++, b->bit++)
		value = (value << 1) | ((b->data[b->bit / 8] >>
					 (7 - b->bit % 8)) & 1);
	return value;
}

static void hevc_skip(struct hevc_bits *b, size_t count)
{
	if (b->error)
		return;
	if (b->bit > b->bytes * 8 || count > b->bytes * 8 - b->bit)
		b->error = -EBADMSG;
	else
		b->bit += count;
}

static u32 hevc_ue(struct hevc_bits *b, u32 maximum)
{
	u32 zeros = 0, suffix;
	u64 value;

	while (!hevc_get(b, 1)) {
		if (b->error)
			return 0;
		if (++zeros > 32) {
			b->error = -EBADMSG;
			return 0;
		}
	}
	suffix = hevc_get(b, zeros);
	value = ((1ULL << zeros) - 1) + suffix;
	if (value > maximum)
		b->error = -EBADMSG;
	return b->error ? 0 : value;
}

static s32 hevc_se(struct hevc_bits *b, u32 magnitude)
{
	u32 value = hevc_ue(b, magnitude * 2);

	return value & 1 ? (value + 1) / 2 : -(s32)(value / 2);
}

static void hevc_profile(struct hevc_bits *b, u32 *profile, bool *interlaced)
{
	u32 space = hevc_get(b, 2);
	u32 compatibility;

	hevc_skip(b, 1);
	*profile = hevc_get(b, 5);
	compatibility = hevc_get(b, 32);
	if (!*profile && (compatibility & BIT(30)))
		*profile = 1;
	hevc_skip(b, 1);
	*interlaced = hevc_get(b, 1);
	hevc_skip(b, 46);
	if (space && !b->error)
		b->error = -EOPNOTSUPP;
}

static void hevc_ptl(struct hevc_bits *b, struct s7d_hevc_sps *sps)
{
	bool profiles[HEVC_MAX_SUB_LAYERS] = { 0 };
	bool levels[HEVC_MAX_SUB_LAYERS] = { 0 };
	bool interlaced;
	u32 i, profile;

	hevc_profile(b, &sps->profile_idc, &sps->interlaced_source);
	sps->level_idc = hevc_get(b, 8);
	for (i = 0; i + 1 < sps->max_sub_layers; i++) {
		profiles[i] = hevc_get(b, 1);
		levels[i] = hevc_get(b, 1);
	}
	if (sps->max_sub_layers > 1)
		for (i = sps->max_sub_layers - 1; i < 8; i++)
			if (hevc_get(b, 2))
				b->error = -EBADMSG;
	for (i = 0; i + 1 < sps->max_sub_layers; i++) {
		if (profiles[i])
			hevc_profile(b, &profile, &interlaced);
		if (levels[i])
			hevc_skip(b, 8);
	}
}

static void hevc_scaling_list(struct hevc_bits *b)
{
	u32 size, matrix, i, count;
	s32 coefficient;

	for (size = 0; size < 4; size++)
		for (matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
			if (!hevc_get(b, 1)) {
				hevc_ue(b, matrix / (size == 3 ? 3 : 1));
				continue;
			}
			if (size > 1) {
				coefficient = hevc_se(b, 255);
				if (coefficient < -7 || coefficient > 247)
					b->error = -EBADMSG;
			}
			count = size ? 64 : 16;
			for (i = 0; i < count; i++)
				if (hevc_se(b, 128) > 127)
					b->error = -EBADMSG;
		}
}

static void hevc_rps_sort(struct s7d_hevc_st_rps *rps)
{
	u32 i, j, negative = 0;
	s32 value;
	bool used;

	for (i = 1; i < rps->count; i++) {
		value = rps->delta[i];
		used = rps->used[i];
		for (j = i; j && rps->delta[j - 1] > value; j--) {
			rps->delta[j] = rps->delta[j - 1];
			rps->used[j] = rps->used[j - 1];
		}
		rps->delta[j] = value;
		rps->used[j] = used;
	}
	while (negative < rps->count && rps->delta[negative] < 0)
		negative++;
	for (i = 0; i < negative / 2; i++) {
		value = rps->delta[i];
		used = rps->used[i];
		rps->delta[i] = rps->delta[negative - i - 1];
		rps->used[i] = rps->used[negative - i - 1];
		rps->delta[negative - i - 1] = value;
		rps->used[negative - i - 1] = used;
	}
}

static void hevc_short_term_rps(struct hevc_bits *b, u32 index,
			      const struct s7d_hevc_st_rps *previous,
			      struct s7d_hevc_st_rps *rps)
{
	u32 i, negative, positive, step;
	s32 delta, value;
	bool used, use_delta, sign;

	if (index && hevc_get(b, 1)) {
		sign = hevc_get(b, 1);
		step = hevc_ue(b, 32767) + 1;
		delta = sign ? -(s32)step : step;
		for (i = 0; i <= previous->count; i++) {
			used = hevc_get(b, 1);
			use_delta = used || hevc_get(b, 1);
			if (!use_delta)
				continue;
			value = delta + (i < previous->count ? previous->delta[i] : 0);
			if (!value)
				continue;
			if (rps->count == HEVC_MAX_DPB) {
				b->error = -EBADMSG;
				return;
			}
			rps->used[rps->count] = used;
			rps->delta[rps->count++] = value;
		}
		hevc_rps_sort(rps);
		return;
	}
	negative = hevc_ue(b, HEVC_MAX_DPB);
	positive = hevc_ue(b, HEVC_MAX_DPB - negative);
	value = 0;
	for (i = 0; i < negative; i++) {
		value -= (s32)hevc_ue(b, 32767) + 1;
		rps->used[rps->count] = hevc_get(b, 1);
		rps->delta[rps->count++] = value;
	}
	value = 0;
	for (i = 0; i < positive; i++) {
		value += (s32)hevc_ue(b, 32767) + 1;
		rps->used[rps->count] = hevc_get(b, 1);
		rps->delta[rps->count++] = value;
	}
}

static void hevc_sub_hrd(struct hevc_bits *b, u32 count, bool subpic)
{
	u32 i;

	for (i = 0; i < count; i++) {
		hevc_ue(b, U32_MAX - 1);
		hevc_ue(b, U32_MAX - 1);
		if (subpic) {
			hevc_ue(b, U32_MAX - 1);
			hevc_ue(b, U32_MAX - 1);
		}
		hevc_skip(b, 1);
	}
}

static void hevc_hrd(struct hevc_bits *b, u32 layers)
{
	bool nal = hevc_get(b, 1), vcl = hevc_get(b, 1);
	bool subpic = false, fixed, low_delay;
	u32 i, count;

	if (nal || vcl) {
		subpic = hevc_get(b, 1);
		if (subpic)
			hevc_skip(b, 19);
		hevc_skip(b, 8);
		if (subpic)
			hevc_skip(b, 4);
		hevc_skip(b, 15);
	}
	for (i = 0; i < layers; i++) {
		fixed = hevc_get(b, 1);
		if (!fixed)
			fixed = hevc_get(b, 1);
		low_delay = false;
		if (fixed)
			hevc_ue(b, 2047);
		else
			low_delay = hevc_get(b, 1);
		count = low_delay ? 1 : hevc_ue(b, 31) + 1;
		if (nal)
			hevc_sub_hrd(b, count, subpic);
		if (vcl)
			hevc_sub_hrd(b, count, subpic);
	}
}

static void hevc_vui(struct hevc_bits *b, struct s7d_hevc_sps *sps)
{
	u32 i, window = 0;

	if (hevc_get(b, 1) && hevc_get(b, 8) == 255)
		hevc_skip(b, 32);
	if (hevc_get(b, 1))
		hevc_skip(b, 1);
	if (hevc_get(b, 1)) {
		sps->colour.flags = S7D_CODEC_COLOUR_SIGNAL_PRESENT;
		hevc_skip(b, 3);
		if (hevc_get(b, 1))
			sps->colour.flags |= S7D_CODEC_COLOUR_FULL_RANGE;
		if (hevc_get(b, 1)) {
			sps->colour.flags |= S7D_CODEC_COLOUR_DESCRIPTION_PRESENT;
			sps->colour.primaries = hevc_get(b, 8);
			sps->colour.transfer = hevc_get(b, 8);
			sps->colour.matrix = hevc_get(b, 8);
		}
	}
	if (hevc_get(b, 1)) {
		hevc_ue(b, 5);
		hevc_ue(b, 5);
	}
	hevc_skip(b, 1);
	sps->field_seq = hevc_get(b, 1);
	hevc_skip(b, 1);
	if (hevc_get(b, 1))
		for (i = 0; i < 4; i++)
			window |= hevc_ue(b, U32_MAX);
	if (hevc_get(b, 1)) {
		hevc_skip(b, 64);
		if (hevc_get(b, 1))
			hevc_ue(b, U32_MAX - 1);
		if (hevc_get(b, 1))
			hevc_hrd(b, sps->max_sub_layers);
	}
	if (hevc_get(b, 1)) {
		hevc_skip(b, 3);
		hevc_ue(b, 4095);
		hevc_ue(b, 16);
		hevc_ue(b, 16);
		hevc_ue(b, 15);
		hevc_ue(b, 15);
	}
	if (!b->error && window)
		b->error = -EOPNOTSUPP;
}

static int hevc_sps_rbsp(struct hevc_bits *b, struct s7d_hevc_sps *sps,
			 struct s7d_hevc_sequence *sequence)
{
	struct s7d_hevc_st_rps previous = { 0 }, next_rps;
	u32 start, i, j, buffering[HEVC_MAX_SUB_LAYERS] = { 0 };
	u32 reorder[HEVC_MAX_SUB_LAYERS] = { 0 };
	u32 latency[HEVC_MAX_SUB_LAYERS] = { 0 };
	bool flag;
	u32 min_tb, max_tb, max_inter, max_intra, rps_count, luma, chroma;
	u32 min_pcm, max_pcm, last;

	sps->vps_id = hevc_get(b, 4);
	sps->max_sub_layers = hevc_get(b, 3) + 1;
	if (sps->max_sub_layers > HEVC_MAX_SUB_LAYERS)
		return -EBADMSG;
	last = sps->max_sub_layers - 1;
	if (!hevc_get(b, 1) && !last)
		return b->error ?: -EBADMSG;
	hevc_ptl(b, sps);
	sps->sps_id = hevc_ue(b, 15);
	sps->chroma_format_idc = hevc_ue(b, 3);
	if (sps->chroma_format_idc == 3)
		sps->separate_colour_plane = hevc_get(b, 1);
	sps->coded_width = hevc_ue(b, 8192);
	sps->coded_height = hevc_ue(b, 8192);
	if (hevc_get(b, 1)) {
		sps->crop_left = hevc_ue(b, U32_MAX);
		sps->crop_right = hevc_ue(b, U32_MAX);
		sps->crop_top = hevc_ue(b, U32_MAX);
		sps->crop_bottom = hevc_ue(b, U32_MAX);
	}
	sps->bit_depth_luma = hevc_ue(b, 8) + 8;
	sps->bit_depth_chroma = hevc_ue(b, 8) + 8;
	sps->log2_max_poc_lsb = hevc_ue(b, 12) + 4;
	start = hevc_get(b, 1) ? 0 : last;
	for (i = start; i <= last; i++) {
		buffering[i] = hevc_ue(b, HEVC_MAX_DPB - 1) + 1;
		reorder[i] = hevc_ue(b, buffering[i] - 1);
		latency[i] = hevc_ue(b, U32_MAX - 1);
		if (i > start && (buffering[i] < buffering[i - 1] ||
				  reorder[i] < reorder[i - 1]))
			b->error = -EBADMSG;
	}
	sps->max_dec_pic_buffering = buffering[last];
	sps->num_reorder_pics = reorder[last];
	sps->max_latency_increase_plus1 = latency[last];
	sps->log2_min_cb_size = hevc_ue(b, 3) + 3;
	sps->log2_ctb_size = sps->log2_min_cb_size + hevc_ue(b, 3);
	min_tb = hevc_ue(b, 3) + 2;
	max_tb = min_tb + hevc_ue(b, 3);
	max_inter = hevc_ue(b, 4);
	max_intra = hevc_ue(b, 4);
	if (sps->log2_ctb_size < 4 || sps->log2_ctb_size > 6 ||
	    min_tb >= sps->log2_min_cb_size || max_tb > min(sps->log2_ctb_size, 5U) ||
	    max_inter > sps->log2_ctb_size - min_tb ||
	    max_intra > sps->log2_ctb_size - min_tb)
		return b->error ?: -EBADMSG;
	flag = hevc_get(b, 1);
	if (sequence)
		sequence->scaling_list_enabled = flag;
	if (flag && hevc_get(b, 1))
		hevc_scaling_list(b);
	hevc_skip(b, 1);
	flag = hevc_get(b, 1);
	if (sequence)
		sequence->sao_enabled = flag;
	flag = hevc_get(b, 1);
	if (sequence)
		sequence->pcm_enabled = flag;
	if (flag) {
		luma = hevc_get(b, 4) + 1;
		chroma = hevc_get(b, 4) + 1;
		min_pcm = hevc_ue(b, 2) + 3;
		max_pcm = min_pcm + hevc_ue(b, 2);
		if (luma > sps->bit_depth_luma || chroma > sps->bit_depth_chroma ||
		    max_pcm > min(sps->log2_ctb_size, 5U))
			b->error = -EBADMSG;
		hevc_skip(b, 1);
	}
	rps_count = hevc_ue(b, HEVC_MAX_RPS);
	if (sequence)
		sequence->num_short_term_rps = rps_count;
	for (i = 0; i < rps_count; i++) {
		next_rps = (struct s7d_hevc_st_rps) { 0 };
		hevc_short_term_rps(b, i, &previous, &next_rps);
		if (sequence)
			sequence->rps[i] = next_rps;
		previous = next_rps;
	}
	flag = hevc_get(b, 1);
	if (sequence)
		sequence->long_term_refs_present = flag;
	if (flag) {
		j = hevc_ue(b, 32);
		for (i = 0; i < j; i++)
			hevc_skip(b, sps->log2_max_poc_lsb + 1);
	}
	flag = hevc_get(b, 1);
	if (sequence)
		sequence->temporal_mvp_enabled = flag;
	hevc_skip(b, 1);
	if (hevc_get(b, 1))
		hevc_vui(b, sps);
	if (hevc_get(b, 1) && hevc_get(b, 8))
		return b->error ?: -EOPNOTSUPP;
	if (!hevc_get(b, 1))
		return b->error ?: -EBADMSG;
	while (!b->error && b->bit % 8)
		if (hevc_get(b, 1))
			b->error = -EBADMSG;
	if (b->bit != b->bytes * 8)
		return b->error ?: -EBADMSG;
	return b->error;
}

static int hevc_parse_sps(const u8 *nal, size_t bytes,
			  struct s7d_hevc_sps *sps,
			  struct s7d_hevc_sequence *sequence)
{
	struct s7d_hevc_sps next = { 0 };
	struct hevc_bits bits = { 0 };
	u8 *rbsp;
	size_t i, count = 0;
	u32 zeros = 0;
	int ret;

	if (!nal || !sps || bytes < 3)
		return -EINVAL;
	if (bytes > S7D_HEVC_SPS_MAX_BYTES)
		return -E2BIG;
	if ((nal[0] & 0x80) || ((nal[0] >> 1) & 0x3f) != 33 ||
	    (nal[0] & 1) || nal[1] != 1)
		return -EOPNOTSUPP;
	rbsp = kmalloc(bytes - 2, GFP_KERNEL);
	if (!rbsp)
		return -ENOMEM;
	for (i = 2; i < bytes; i++) {
		if (zeros >= 2 && nal[i] <= 3) {
			if (nal[i] != 3 || i + 1 == bytes || nal[i + 1] > 3) {
				ret = -EBADMSG;
				goto out;
			}
			zeros = 0;
			continue;
		}
		rbsp[count++] = nal[i];
		zeros = nal[i] ? 0 : zeros + 1;
	}
	bits.data = rbsp;
	bits.bytes = count;
	ret = hevc_sps_rbsp(&bits, &next, sequence);
	if (!ret)
		*sps = next;
out:
	kfree(rbsp);
	return ret;
}

int s7d_hevc_parse_sequence(const u8 *nal, size_t bytes,
			    struct s7d_hevc_sequence *sequence)
{
	struct s7d_hevc_sequence *next;
	int ret;

	if (!sequence)
		return -EINVAL;
	next = kzalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	ret = hevc_parse_sps(nal, bytes, &next->sps, next);
	if (!ret)
		*sequence = *next;
	kfree(next);
	return ret;
}

static int hevc_dpb_limit(const struct s7d_hevc_sps *sps, u32 *limit)
{
	u32 max_luma, pixels;

	switch (sps->level_idc) {
	case 30:
		max_luma = 36864;
		break;
	case 60:
		max_luma = 122880;
		break;
	case 63:
		max_luma = 245760;
		break;
	case 90:
		max_luma = 552960;
		break;
	case 93:
		max_luma = 983040;
		break;
	case 120:
	case 123:
		max_luma = 2228224;
		break;
	case 150:
	case 153:
		max_luma = 8912896;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (check_mul_overflow(sps->coded_width, sps->coded_height, &pixels))
		return -EOVERFLOW;
	if (pixels > max_luma || (u64)sps->coded_width * sps->coded_width > 8U * max_luma ||
	    (u64)sps->coded_height * sps->coded_height > 8U * max_luma)
		return -ERANGE;
	if (sps->level_idc >= 150 && sps->log2_ctb_size < 5)
		return -EINVAL;
	if (pixels <= max_luma / 4)
		*limit = 16;
	else if (pixels <= max_luma / 2)
		*limit = 12;
	else if (pixels <= 3U * max_luma / 4)
		*limit = 8;
	else
		*limit = 6;
	return 0;
}

int s7d_hevc_build_layout(const struct s7d_hevc_sps *sps,
			  enum s7d_codec_capture_format format,
			  u32 extra_surfaces, struct s7d_codec_layout *layout)
{
	struct s7d_codec_layout next = { .format = format, .plane_count = 2 };
	u32 right, bottom, horizontal, vertical, area, uv_size;
	int ret;

	if (!sps || !layout)
		return -EINVAL;
	if (sps->profile_idc != 1 || sps->chroma_format_idc != 1 ||
	    sps->separate_colour_plane || sps->bit_depth_luma != 8 ||
	    sps->bit_depth_chroma != 8 || sps->interlaced_source || sps->field_seq)
		return -EOPNOTSUPP;
	if (format != S7D_CODEC_CAPTURE_NV12M && format != S7D_CODEC_CAPTURE_NV21M)
		return -EOPNOTSUPP;
	if (sps->coded_width < 64 || sps->coded_height < 64 ||
	    sps->coded_width > 1920 || sps->coded_height > 1088)
		return -ERANGE;
	if (!sps->max_sub_layers || sps->max_sub_layers > HEVC_MAX_SUB_LAYERS ||
	    sps->log2_min_cb_size < 3 || sps->log2_min_cb_size > 6 ||
	    sps->log2_ctb_size < 4 || sps->log2_ctb_size > 6 ||
	    sps->log2_ctb_size < sps->log2_min_cb_size ||
	    sps->log2_max_poc_lsb < 4 || sps->log2_max_poc_lsb > 16 ||
	    ((sps->coded_width | sps->coded_height) & ((1U << sps->log2_min_cb_size) - 1)))
		return -EINVAL;
	ret = hevc_dpb_limit(sps, &next.dpb_limit);
	if (ret)
		return ret;
	if (!sps->max_dec_pic_buffering || sps->max_dec_pic_buffering > next.dpb_limit ||
	    sps->num_reorder_pics >= sps->max_dec_pic_buffering)
		return -EINVAL;
	next.coded_width = sps->coded_width;
	next.coded_height = sps->coded_height;
	if (check_mul_overflow(sps->crop_left, 2U, &next.visible_left) ||
	    check_mul_overflow(sps->crop_right, 2U, &right) ||
	    check_mul_overflow(sps->crop_top, 2U, &next.visible_top) ||
	    check_mul_overflow(sps->crop_bottom, 2U, &bottom) ||
	    check_add_overflow(next.visible_left, right, &horizontal) ||
	    check_add_overflow(next.visible_top, bottom, &vertical))
		return -EOVERFLOW;
	if (horizontal >= next.coded_width || vertical >= next.coded_height)
		return -EINVAL;
	next.visible_width = next.coded_width - horizontal;
	next.visible_height = next.coded_height - vertical;
	if (next.visible_width > 1920 || next.visible_height > 1080)
		return -ERANGE;
	next.pitch[0] = ALIGN(next.coded_width, S7D_CODEC_STORAGE_ALIGNMENT);
	next.pitch[1] = next.pitch[0];
	next.storage_height = ALIGN(next.coded_height, S7D_CODEC_STORAGE_ALIGNMENT);
	if (check_mul_overflow(next.pitch[0], next.storage_height, &area))
		return -EOVERFLOW;
	/* SAO writes page-rounded chroma and twice that length for luma. */
	uv_size = ALIGN(area / 2, 4096U);
	if (check_mul_overflow(uv_size, 2U, &next.size[0]) ||
	    check_add_overflow(next.size[0], uv_size, &next.total_size))
		return -EOVERFLOW;
	next.size[1] = uv_size;
	next.dpb_slots = sps->max_dec_pic_buffering;
	next.current_slots = 2;
	next.extra_surfaces = extra_surfaces;
	if (check_add_overflow(next.dpb_slots, next.current_slots, &next.min_capture_buffers) ||
	    check_add_overflow(next.min_capture_buffers, extra_surfaces,
			       &next.min_capture_buffers))
		return -EOVERFLOW;
	if (next.min_capture_buffers > S7D_HEVC_MAX_CAPTURE_BUFFERS)
		return -ERANGE;
	ret = s7d_codec_resolve_colour(&sps->colour, next.coded_width,
				     next.coded_height, &next.colourimetry);
	if (!ret)
		ret = s7d_codec_validate_layout(&next);
	if (!ret)
		*layout = next;
	return ret;
}
