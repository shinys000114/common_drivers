// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "s7d-hevc-input.h"

#define HEVC_VPS_COUNT 16U
#define HEVC_SPS_COUNT 8U
#define HEVC_PPS_COUNT 64U
#define HEVC_NAL_COUNT 64U

struct hevc_bits {
	const u8 *data;
	size_t bytes;
	size_t bit;
	int error;
};

struct hevc_vps {
	u32 id;
	u32 max_sub_layers;
	u32 max_dec_pic_buffering;
	u32 num_reorder_pics;
};

struct hevc_parameter {
	u64 epoch;
	u8 *raw;
	size_t bytes;
	void *typed;
};

struct s7d_hevc_input_bank {
	struct hevc_parameter *vps[HEVC_VPS_COUNT];
	struct hevc_parameter *sps[HEVC_SPS_COUNT];
	struct hevc_parameter *pps[HEVC_PPS_COUNT];
	size_t bytes;
	u64 revision;
};

struct s7d_hevc_input {
	struct s7d_hevc_input_result view;
	struct s7d_hevc_sequence sequence;
	struct s7d_hevc_pps pps;
	struct hevc_parameter *vps_update[HEVC_VPS_COUNT];
	struct hevc_parameter *sps_update[HEVC_SPS_COUNT];
	struct hevc_parameter *pps_update[HEVC_PPS_COUNT];
	u8 *parameter[3];
	size_t parameter_bytes[3];
	u8 *data;
	size_t bank_bytes;
	u64 bank_revision;
	u64 selected_epoch;
	const struct s7d_hevc_input_bank *bank;
	bool changed;
	bool committed;
};

static u32 hevc_get(struct hevc_bits *b, u32 count)
{
	u32 i, value = 0;

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

static s32 hevc_se(struct hevc_bits *b, u32 maximum)
{
	u32 value = hevc_ue(b, 2U * maximum);

	return value & 1 ? (value + 1) / 2 : -(s32)(value / 2);
}

static int hevc_stop_bits(struct hevc_bits *b)
{
	if (!hevc_get(b, 1))
		return b->error ?: -EBADMSG;
	while (!b->error && b->bit % 8)
		if (hevc_get(b, 1))
			b->error = -EBADMSG;
	return b->error;
}

static int hevc_trailing(struct hevc_bits *b)
{
	int ret = hevc_stop_bits(b);

	return ret ?: (b->bit == b->bytes * 8 ? 0 : -EBADMSG);
}

/*
 * Some muxers leave the next NAL glued to a VPS or PPS. As libavcodec does,
 * ignore data after the parameter set's trailing bits.
 */
static int hevc_parameter_trailing(struct hevc_bits *b)
{
	return hevc_stop_bits(b);
}

static int hevc_rbsp(const u8 *nal, size_t bytes, u8 **out, size_t *written)
{
	u8 *rbsp;
	size_t i, count = 0;
	u32 zeros = 0;

	rbsp = kvmalloc(bytes - 2, GFP_KERNEL);
	if (!rbsp)
		return -ENOMEM;
	for (i = 2; i < bytes; i++) {
		if (zeros >= 2 && nal[i] <= 3) {
			if (nal[i] != 3 || i + 1 == bytes || nal[i + 1] > 3) {
				kvfree(rbsp);
				return -EBADMSG;
			}
			zeros = 0;
			continue;
		}
		rbsp[count++] = nal[i];
		zeros = nal[i] ? 0 : zeros + 1;
	}
	*out = rbsp;
	*written = count;
	return 0;
}

static int hevc_vps_parse(struct hevc_bits *b, struct hevc_vps *vps)
{
	u32 profile, compatibility, i, constraint;
	bool interlaced;

	vps->id = hevc_get(b, 4);
	if (!hevc_get(b, 1) || !hevc_get(b, 1) || hevc_get(b, 6))
		return b->error ?: -EOPNOTSUPP;
	vps->max_sub_layers = hevc_get(b, 3) + 1;
	if (vps->max_sub_layers != 1 || !hevc_get(b, 1))
		return b->error ?: -EOPNOTSUPP;
	if (hevc_get(b, 16) != 0xffff)
		return b->error ?: -EBADMSG;
	if (hevc_get(b, 2))
		return b->error ?: -EOPNOTSUPP;
	hevc_get(b, 1);
	profile = hevc_get(b, 5);
	compatibility = hevc_get(b, 32);
	hevc_get(b, 1);
	interlaced = hevc_get(b, 1);
	hevc_get(b, 2);
	constraint = hevc_get(b, 32);
	constraint |= hevc_get(b, 12);
	hevc_get(b, 8);
	if ((profile != 1 && (profile || !(compatibility & BIT(30)))) ||
	    interlaced || constraint)
		return b->error ?: -EOPNOTSUPP;
	hevc_get(b, 1);
	vps->max_dec_pic_buffering = hevc_ue(b, 15) + 1;
	vps->num_reorder_pics = hevc_ue(b, vps->max_dec_pic_buffering - 1);
	hevc_ue(b, U32_MAX - 1);
	if (hevc_get(b, 6) || hevc_ue(b, 1023))
		return b->error ?: -EOPNOTSUPP;
	if (hevc_get(b, 1)) {
		u32 tick = hevc_get(b, 32), scale = hevc_get(b, 32);

		if (!tick || !scale)
			return b->error ?: -EBADMSG;
		if (hevc_get(b, 1))
			hevc_ue(b, U32_MAX - 1);
		i = hevc_ue(b, 1024);
		if (i)
			return b->error ?: -EOPNOTSUPP;
	}
	if (hevc_get(b, 1))
		return b->error ?: -EOPNOTSUPP;
	return hevc_parameter_trailing(b);
}

static void hevc_scaling_list(struct hevc_bits *b)
{
	u32 size, matrix, i, count;
	s32 value;

	for (size = 0; size < 4; size++)
		for (matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
			if (!hevc_get(b, 1)) {
				hevc_ue(b, matrix / (size == 3 ? 3 : 1));
				continue;
			}
			if (size > 1) {
				value = hevc_se(b, 255);
				if (value < -7 || value > 247)
					b->error = -EBADMSG;
			}
			count = size ? 64 : 16;
			for (i = 0; i < count; i++)
				if (hevc_se(b, 128) > 127)
					b->error = -EBADMSG;
		}
}

static int hevc_pps_parse(struct hevc_bits *b, struct s7d_hevc_pps *pps)
{
	pps->pps_id = hevc_ue(b, HEVC_PPS_COUNT - 1);
	pps->sps_id = hevc_ue(b, HEVC_SPS_COUNT - 1);
	pps->dependent_slices = hevc_get(b, 1);
	pps->output_flag_present = hevc_get(b, 1);
	pps->extra_slice_header_bits = hevc_get(b, 3);
	hevc_get(b, 1);
	pps->cabac_init_present = hevc_get(b, 1);
	pps->default_active[0] = hevc_ue(b, 14) + 1;
	pps->default_active[1] = hevc_ue(b, 14) + 1;
	pps->init_qp_minus26 = hevc_se(b, 26);
	if (pps->init_qp_minus26 > 25)
		return b->error ?: -EBADMSG;
	hevc_get(b, 1);
	hevc_get(b, 1);
	pps->cu_qp_delta_enabled = hevc_get(b, 1);
	if (pps->cu_qp_delta_enabled)
		pps->diff_cu_qp_delta_depth = hevc_ue(b, 3);
	pps->cb_qp_offset = hevc_se(b, 12);
	pps->cr_qp_offset = hevc_se(b, 12);
	pps->slice_chroma_qp_offsets_present = hevc_get(b, 1);
	pps->weighted_pred = hevc_get(b, 1);
	pps->weighted_bipred = hevc_get(b, 1);
	hevc_get(b, 1);
	pps->tiles_enabled = hevc_get(b, 1);
	pps->entropy_coding_sync = hevc_get(b, 1);
	if (pps->dependent_slices || pps->tiles_enabled)
		return b->error ?: -EOPNOTSUPP;
	pps->loop_filter_across_slices = hevc_get(b, 1);
	pps->deblocking_control_present = hevc_get(b, 1);
	if (pps->deblocking_control_present) {
		pps->deblocking_override_enabled = hevc_get(b, 1);
		pps->deblocking_disabled = hevc_get(b, 1);
		if (!pps->deblocking_disabled) {
			pps->beta_offset_div2 = hevc_se(b, 6);
			pps->tc_offset_div2 = hevc_se(b, 6);
		}
	}
	pps->scaling_list_data_present = hevc_get(b, 1);
	if (pps->scaling_list_data_present)
		hevc_scaling_list(b);
	pps->lists_modification_present = hevc_get(b, 1);
	pps->log2_parallel_merge_level = hevc_ue(b, 4) + 2;
	pps->slice_header_extension_present = hevc_get(b, 1);
	if (hevc_get(b, 1)) {
		if (hevc_get(b, 8))
			return b->error ?: -EOPNOTSUPP;
	}
	return hevc_parameter_trailing(b);
}

static void hevc_rps_sort(struct s7d_hevc_st_rps *rps)
{
	u32 i, j;

	for (i = 1; i < rps->count; i++) {
		s32 delta = rps->delta[i];
		bool used = rps->used[i];

		for (j = i; j; j--) {
			s32 old = rps->delta[j - 1];
			bool before = delta < 0 ? old >= 0 || old < delta :
					old > 0 && old > delta;

			if (!before)
				break;
			rps->delta[j] = old;
			rps->used[j] = rps->used[j - 1];
		}
		rps->delta[j] = delta;
		rps->used[j] = used;
	}
}

static void hevc_slice_rps(struct hevc_bits *b,
			   const struct s7d_hevc_sequence *sequence,
			   struct s7d_hevc_st_rps *rps)
{
	u32 index = sequence->num_short_term_rps, i, count, distance;
	s32 value = 0;

	if (index && hevc_get(b, 1)) {
		const struct s7d_hevc_st_rps *previous;
		bool sign, used, use_delta;
		u32 step;
		s32 delta;

		distance = hevc_ue(b, index - 1) + 1;
		sign = hevc_get(b, 1);
		step = hevc_ue(b, 32767) + 1;
		delta = sign ? -(s32)step : step;
		previous = &sequence->rps[index - distance];
		for (i = 0; i <= previous->count; i++) {
			used = hevc_get(b, 1);
			use_delta = used || hevc_get(b, 1);
			if (!use_delta)
				continue;
			value = delta + (i < previous->count ? previous->delta[i] : 0);
			if (!value)
				continue;
			if (rps->count == S7D_HEVC_RPS_ENTRIES) {
				b->error = -EBADMSG;
				return;
			}
			rps->delta[rps->count] = value;
			rps->used[rps->count++] = used;
		}
		hevc_rps_sort(rps);
		return;
	}
	count = hevc_ue(b, S7D_HEVC_RPS_ENTRIES);
	distance = hevc_ue(b, S7D_HEVC_RPS_ENTRIES - count);
	for (i = 0; i < count; i++) {
		value -= (s32)hevc_ue(b, 32767) + 1;
		rps->delta[rps->count] = value;
		rps->used[rps->count++] = hevc_get(b, 1);
	}
	value = 0;
	for (i = 0; i < distance; i++) {
		value += (s32)hevc_ue(b, 32767) + 1;
		rps->delta[rps->count] = value;
		rps->used[rps->count++] = hevc_get(b, 1);
	}
}

static void hevc_weights(struct hevc_bits *b,
			 const struct s7d_hevc_slice_info *slice)
{
	u32 list, i, component, weights = 0, luma_denom = hevc_ue(b, 7);
	s32 chroma_denom = (s32)luma_denom + hevc_se(b, 7);
	bool luma[S7D_HEVC_RPS_ENTRIES], chroma[S7D_HEVC_RPS_ENTRIES];

	if (chroma_denom < 0 || chroma_denom > 7)
		b->error = -EBADMSG;
	for (list = 0; list < (slice->slice_type == 0 ? 2U : 1U); list++) {
		for (i = 0; i < slice->active[list]; i++)
			luma[i] = hevc_get(b, 1);
		for (i = 0; i < slice->active[list]; i++) {
			chroma[i] = hevc_get(b, 1);
			weights += luma[i] + 2 * chroma[i];
		}
		if (weights > 24)
			b->error = -EBADMSG;
		for (i = 0; i < slice->active[list]; i++) {
			if (luma[i]) {
				if (hevc_se(b, 128) > 127 || hevc_se(b, 128) > 127)
					b->error = -EBADMSG;
			}
			if (chroma[i])
				for (component = 0; component < 2; component++) {
					if (hevc_se(b, 128) > 127)
						b->error = -EBADMSG;
					if (hevc_se(b, 512) > 508)
						b->error = -EBADMSG;
				}
		}
	}
}

static size_t hevc_escaped_header_bytes(const u8 *nal, size_t bytes,
				       size_t rbsp_bytes)
{
	size_t i, count = 0;
	u32 zeros = 0;

	for (i = 2; i < bytes && count < rbsp_bytes; i++) {
		if (zeros >= 2 && nal[i] == 3) {
			zeros = 0;
			continue;
		}
		count++;
		zeros = nal[i] ? 0 : zeros + 1;
	}
	return i;
}

static int hevc_slice_parse(struct hevc_bits *b,
			    const u8 *nal, size_t bytes,
			    struct s7d_hevc_input *input)
{
	struct s7d_hevc_slice_info *slice = &input->view.slice;
	const struct s7d_hevc_sequence *sequence = &input->sequence;
	const struct s7d_hevc_sps *sps = &sequence->sps;
	const struct s7d_hevc_pps *pps = &input->pps;
	struct s7d_hevc_st_rps rps = { };
	u32 i, list, index, total = 0, bits = 0;
	u32 rows = DIV_ROUND_UP(sps->coded_height, 1U << sps->log2_ctb_size);
	s32 qp;
	bool sao_luma = false, sao_chroma = false, disabled = pps->deblocking_disabled;
	u64 offsets = 0;
	size_t escaped;

	slice->first_slice = hevc_get(b, 1);
	if (slice->nal_type >= 16 && slice->nal_type <= 23)
		slice->no_output_of_prior_pics = hevc_get(b, 1);
	if (hevc_ue(b, HEVC_PPS_COUNT - 1) != pps->pps_id)
		return b->error ?: -EBADMSG;
	if (!slice->first_slice)
		return b->error ?: -EOPNOTSUPP;
	for (i = 0; i < pps->extra_slice_header_bits; i++)
		if (hevc_get(b, 1))
			return b->error ?: -EOPNOTSUPP;
	slice->slice_type = hevc_ue(b, 2);
	if (slice->nal_type >= 16 && slice->slice_type != 2)
		return b->error ?: -EBADMSG;
	slice->pic_output_flag = pps->output_flag_present ? hevc_get(b, 1) : true;
	if (slice->nal_type != 19 && slice->nal_type != 20) {
		slice->poc_lsb = hevc_get(b, sps->log2_max_poc_lsb);
		if (!hevc_get(b, 1)) {
			hevc_slice_rps(b, sequence, &rps);
		} else {
			if (!sequence->num_short_term_rps)
				return b->error ?: -EBADMSG;
			while ((1U << bits) < sequence->num_short_term_rps)
				bits++;
			index = hevc_get(b, bits);
			if (index >= sequence->num_short_term_rps)
				return b->error ?: -EBADMSG;
			rps = sequence->rps[index];
		}
		if (sequence->temporal_mvp_enabled)
			slice->temporal_mvp = hevc_get(b, 1);
	}
	if (rps.count >= sps->max_dec_pic_buffering)
		return b->error ?: -EBADMSG;
	for (i = 0; i < rps.count; i++) {
		if (!rps.delta[i] || rps.delta[i] < -4096 || rps.delta[i] > 4095)
			return b->error ?: -EOPNOTSUPP;
		slice->rps[i].delta_poc = rps.delta[i];
		slice->rps[i].used = rps.used[i];
		total += rps.used[i];
	}
	slice->rps_count = rps.count;
	if (sequence->sao_enabled) {
		sao_luma = hevc_get(b, 1);
		sao_chroma = hevc_get(b, 1);
	}
	if (slice->slice_type != 2) {
		slice->active[0] = pps->default_active[0];
		slice->active[1] = slice->slice_type == 0 ? pps->default_active[1] : 0;
		if (hevc_get(b, 1)) {
			slice->active[0] = hevc_ue(b, 14) + 1;
			if (slice->slice_type == 0)
				slice->active[1] = hevc_ue(b, 14) + 1;
		}
		if (!total)
			return b->error ?: -EBADMSG;
		if (pps->lists_modification_present && total > 1) {
			bits = 0;
			while ((1U << bits) < total)
				bits++;
			for (list = 0; list < (slice->slice_type == 0 ? 2U : 1U); list++) {
				slice->modification[list] = hevc_get(b, 1);
				if (!slice->modification[list])
					continue;
				for (i = 0; i < slice->active[list]; i++) {
					index = hevc_get(b, bits);
					if (index >= total)
						return b->error ?: -EBADMSG;
					slice->modification_index[list][i] = index;
				}
			}
		}
		if (slice->slice_type == 0)
			hevc_get(b, 1);
		if (pps->cabac_init_present)
			hevc_get(b, 1);
		/* Non-coded collocated fields still need a safe vendor MV address. */
		slice->collocated_from_l0 = slice->slice_type == 1;
		if (slice->temporal_mvp) {
			slice->collocated_from_l0 = slice->slice_type == 0 ? hevc_get(b, 1) : true;
			list = !slice->collocated_from_l0;
			if (slice->active[list] > 1)
				slice->collocated_ref_idx = hevc_ue(b, slice->active[list] - 1);
		}
		if ((slice->slice_type == 1 && pps->weighted_pred) ||
		    (slice->slice_type == 0 && pps->weighted_bipred))
			hevc_weights(b, slice);
		hevc_ue(b, 4);
	}
	qp = hevc_se(b, 51);
	qp += 26 + pps->init_qp_minus26;
	if (qp < 0 || qp > 51)
		return b->error ?: -EBADMSG;
	if (pps->slice_chroma_qp_offsets_present) {
		s32 cb = hevc_se(b, 12), cr = hevc_se(b, 12);

		if (abs(cb + pps->cb_qp_offset) > 12 || abs(cr + pps->cr_qp_offset) > 12)
			return b->error ?: -EBADMSG;
	}
	if (pps->deblocking_override_enabled && hevc_get(b, 1)) {
		disabled = hevc_get(b, 1);
		if (!disabled) {
			hevc_se(b, 6);
			hevc_se(b, 6);
		}
	}
	if (pps->loop_filter_across_slices && (sao_luma || sao_chroma || !disabled))
		hevc_get(b, 1);
	if (pps->entropy_coding_sync) {
		input->view.entry_points = hevc_ue(b, rows - 1);
		if (input->view.entry_points) {
			bits = hevc_ue(b, 31) + 1;
			for (i = 0; i < input->view.entry_points; i++)
				offsets += (u64)hevc_get(b, bits) + 1;
		}
	}
	if (pps->slice_header_extension_present) {
		index = hevc_ue(b, S7D_HEVC_PARAMETER_BYTES);
		for (i = 0; i < index; i++)
			hevc_get(b, 8);
	}
	if (!hevc_get(b, 1))
		return b->error ?: -EBADMSG;
	while (!b->error && b->bit % 8)
		if (hevc_get(b, 1))
			b->error = -EBADMSG;
	if (b->error)
		return b->error;
	input->view.header_rbsp_bytes = b->bit / 8;
	input->view.cabac_rbsp_bytes = b->bytes - b->bit / 8;
	escaped = hevc_escaped_header_bytes(nal, bytes, b->bit / 8);
	if (!input->view.cabac_rbsp_bytes || escaped >= bytes || offsets >= bytes - escaped)
		return -EBADMSG;
	/* CABAC payload and firmware weight execution are not decoded here. */
	return 0;
}

static void hevc_parameter_free(struct hevc_parameter *parameter)
{
	if (!parameter)
		return;
	kvfree(parameter->raw);
	kvfree(parameter->typed);
	kfree(parameter);
}

static struct hevc_parameter *
hevc_parameter_get(const struct s7d_hevc_input_bank *bank,
		   const struct s7d_hevc_input *input, u32 type, u32 id)
{
	if (type == 32)
		return input->vps_update[id] ?: bank->vps[id];
	if (type == 33)
		return input->sps_update[id] ?: bank->sps[id];
	return input->pps_update[id] ?: bank->pps[id];
}

static int hevc_parameter_parse(const struct s7d_hevc_input_bank *bank,
				struct s7d_hevc_input *input,
				const u8 *nal, size_t bytes, u32 type,
				struct hevc_bits *bits)
{
	struct hevc_parameter *next, *old, **slot;
	size_t typed_size, total;
	u32 id;
	int ret;

	if (bytes > S7D_HEVC_PARAMETER_BYTES)
		return -E2BIG;
	next = kzalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	typed_size = type == 32 ? sizeof(struct hevc_vps) :
		     type == 33 ? sizeof(struct s7d_hevc_sequence) : sizeof(struct s7d_hevc_pps);
	next->typed = kvzalloc(typed_size, GFP_KERNEL);
	if (!next->typed) {
		ret = -ENOMEM;
		goto failed;
	}
	if (type == 32) {
		struct hevc_vps *vps = next->typed;

		ret = hevc_vps_parse(bits, vps);
		id = vps->id;
		slot = &input->vps_update[id];
	} else if (type == 33) {
		struct s7d_hevc_sequence *sequence = next->typed;
		struct s7d_codec_layout layout;

		ret = s7d_hevc_parse_sequence(nal, bytes, sequence);
		id = sequence->sps.sps_id;
		if (!ret && (id >= HEVC_SPS_COUNT || sequence->sps.max_sub_layers != 1 ||
			     sequence->long_term_refs_present))
			ret = -EOPNOTSUPP;
		if (!ret)
			ret = s7d_hevc_build_layout(&sequence->sps, S7D_CODEC_CAPTURE_NV12M, 0, &layout);
		slot = id < HEVC_SPS_COUNT ? &input->sps_update[id] : NULL;
	} else {
		struct s7d_hevc_pps *pps = next->typed;

		ret = hevc_pps_parse(bits, pps);
		id = pps->pps_id;
		slot = &input->pps_update[id];
	}
	if (ret)
		goto failed;
	old = hevc_parameter_get(bank, input, type, id);
	if (old && old->bytes == bytes && !memcmp(old->raw, nal, bytes)) {
		hevc_parameter_free(next);
		goto snapshot;
	}
	if (check_add_overflow(input->bank_bytes - (old ? old->bytes : 0), bytes, &total) ||
	    total > S7D_HEVC_PARAMETER_BYTES) {
		ret = -E2BIG;
		goto failed;
	}
	{
		struct hevc_parameter *original = type == 32 ? bank->vps[id] :
						  type == 33 ? bank->sps[id] : bank->pps[id];

		if (original && original->bytes == bytes && !memcmp(original->raw, nal, bytes)) {
			hevc_parameter_free(*slot);
			*slot = NULL;
			input->bank_bytes = total;
			hevc_parameter_free(next);
			goto snapshot;
		}
	}
	next->raw = kvmalloc(bytes, GFP_KERNEL);
	if (!next->raw) {
		ret = -ENOMEM;
		goto failed;
	}
	memcpy(next->raw, nal, bytes);
	next->bytes = bytes;
	next->epoch = bank->revision + 1;
	hevc_parameter_free(*slot);
	*slot = next;
	input->bank_bytes = total;
snapshot:
	old = hevc_parameter_get(bank, input, type, id);
	if (type == 33) {
		input->sequence = *(struct s7d_hevc_sequence *)old->typed;
		input->view.sequence = &input->sequence;
	} else if (type == 34) {
		input->pps = *(struct s7d_hevc_pps *)old->typed;
		input->view.pps = &input->pps;
	}
	input->selected_epoch = max(input->selected_epoch, old->epoch);
	return 0;
failed:
	hevc_parameter_free(next);
	return ret;
}

static int hevc_select_parameters(const struct s7d_hevc_input_bank *bank,
				  struct s7d_hevc_input *input,
				  const u8 *nal, size_t bytes, struct hevc_bits *bits)
{
	struct hevc_parameter *parameter[3];
	struct hevc_bits prefix = *bits;
	const struct hevc_vps *vps;
	u32 id, i;

	hevc_get(&prefix, 1);
	if (((nal[0] >> 1) & 63) >= 16 && ((nal[0] >> 1) & 63) <= 23)
		hevc_get(&prefix, 1);
	id = hevc_ue(&prefix, HEVC_PPS_COUNT - 1);
	if (prefix.error)
		return prefix.error;
	parameter[2] = hevc_parameter_get(bank, input, 34, id);
	if (!parameter[2])
		return -ENODATA;
	input->pps = *(struct s7d_hevc_pps *)parameter[2]->typed;
	parameter[1] = hevc_parameter_get(bank, input, 33, input->pps.sps_id);
	if (!parameter[1])
		return -ENODATA;
	input->sequence = *(struct s7d_hevc_sequence *)parameter[1]->typed;
	parameter[0] = hevc_parameter_get(bank, input, 32, input->sequence.sps.vps_id);
	if (!parameter[0])
		return -ENODATA;
	vps = parameter[0]->typed;
	if (vps->max_sub_layers != input->sequence.sps.max_sub_layers ||
	    input->pps.log2_parallel_merge_level > input->sequence.sps.log2_ctb_size ||
	    input->pps.diff_cu_qp_delta_depth > input->sequence.sps.log2_ctb_size -
					     input->sequence.sps.log2_min_cb_size ||
	    (input->pps.scaling_list_data_present && !input->sequence.scaling_list_enabled))
		return -EBADMSG;
	input->selected_epoch = 0;
	for (i = 0; i < 3; i++) {
		input->selected_epoch = max(input->selected_epoch, parameter[i]->epoch);
		input->parameter[i] = kvmalloc(parameter[i]->bytes, GFP_KERNEL);
		if (!input->parameter[i])
			return -ENOMEM;
		memcpy(input->parameter[i], parameter[i]->raw, parameter[i]->bytes);
		input->parameter_bytes[i] = parameter[i]->bytes;
	}
	input->view.sequence = &input->sequence;
	input->view.pps = &input->pps;
	return hevc_slice_parse(bits, nal, bytes, input);
}

static size_t hevc_start_code(const u8 *data, size_t bytes, size_t pos)
{
	if (bytes - pos < 3 || data[pos] || data[pos + 1])
		return 0;
	if (data[pos + 2] == 1)
		return 3;
	return bytes - pos >= 4 && !data[pos + 2] && data[pos + 3] == 1 ? 4 : 0;
}

static int hevc_auxiliary(struct hevc_bits *b, u32 type)
{
	u32 count;

	if (type == 35) {
		if (hevc_get(b, 3) > 2)
			return b->error ?: -EBADMSG;
		return hevc_trailing(b);
	}
	if (type == 38) {
		while (b->bit / 8 < b->bytes && b->data[b->bit / 8] == 0xff)
			hevc_get(b, 8);
		return hevc_trailing(b);
	}
	while (b->bytes - b->bit / 8 > 1) {
		u64 payload = 0;

		do {
			count = hevc_get(b, 8);
		} while (!b->error && count == 255);
		do {
			count = hevc_get(b, 8);
			payload += count;
		} while (!b->error && count == 255);
		if (b->error || payload > b->bytes - b->bit / 8)
			return -EBADMSG;
		b->bit += payload * 8;
	}
	return hevc_trailing(b);
}

struct s7d_hevc_input_bank *s7d_hevc_input_bank_create(void)
{
	struct s7d_hevc_input_bank *bank = kzalloc(sizeof(*bank), GFP_KERNEL);

	return bank ?: ERR_PTR(-ENOMEM);
}

void s7d_hevc_input_bank_destroy(struct s7d_hevc_input_bank *bank)
{
	u32 i;

	if (IS_ERR_OR_NULL(bank))
		return;
	for (i = 0; i < HEVC_VPS_COUNT; i++)
		hevc_parameter_free(bank->vps[i]);
	for (i = 0; i < HEVC_SPS_COUNT; i++)
		hevc_parameter_free(bank->sps[i]);
	for (i = 0; i < HEVC_PPS_COUNT; i++)
		hevc_parameter_free(bank->pps[i]);
	kfree(bank);
}

int s7d_hevc_input_prepare(const struct s7d_hevc_input_bank *bank,
			   const u8 *annexb, size_t bytes, struct s7d_hevc_input **out)
{
	struct s7d_hevc_input *input;
	size_t pos = 0;
	u32 nals = 0;
	bool picture = false;
	int ret = 0;

	if (IS_ERR_OR_NULL(bank) || !annexb || !bytes || !out)
		return -EINVAL;
	if (bytes > S7D_HEVC_AU_MAX_BYTES)
		return -E2BIG;
	input = kvzalloc(sizeof(*input), GFP_KERNEL);
	if (!input)
		return -ENOMEM;
	input->data = kvmalloc(bytes, GFP_KERNEL);
	if (!input->data) {
		ret = -ENOMEM;
		goto failed;
	}
	memcpy(input->data, annexb, bytes);
	input->bank = bank;
	input->bank_revision = bank->revision;
	input->bank_bytes = bank->bytes;
	while (pos < bytes && !hevc_start_code(input->data, bytes, pos) && !input->data[pos])
		pos++;
	while (pos < bytes) {
		struct hevc_bits bits = { };
		u8 *rbsp = NULL;
		const u8 *nal;
		size_t code = hevc_start_code(input->data, bytes, pos), start = pos + code, end;
		u32 type;

		if (!code || ++nals > HEVC_NAL_COUNT) {
			ret = -EBADMSG;
			goto failed;
		}
		for (pos = start; pos < bytes; pos++)
			if (hevc_start_code(input->data, bytes, pos))
				break;
		end = pos;
		while (end > start && !input->data[end - 1])
			end--;
		nal = input->data + start;
		if (end - start < 3 || (nal[0] & 0x80)) {
			ret = -EBADMSG;
			goto failed;
		}
		if ((nal[0] & 1) || nal[1] != 1) {
			ret = -EOPNOTSUPP;
			goto failed;
		}
		type = (nal[0] >> 1) & 63;
		ret = hevc_rbsp(nal, end - start, &rbsp, &bits.bytes);
		if (ret)
			goto failed;
		bits.data = rbsp;
		if (type >= 32 && type <= 34) {
			ret = picture ? -EOPNOTSUPP :
				hevc_parameter_parse(bank, input, nal, end - start, type, &bits);
		} else if (type == 0 || type == 1 || (type >= 6 && type <= 9) ||
			   (type >= 16 && type <= 21)) {
			if (picture) {
				ret = -EOPNOTSUPP;
			} else {
				input->view.slice.nal_type = type;
				input->view.slice.temporal_id_plus1 = nal[1] & 7;
				ret = hevc_select_parameters(bank, input, nal, end - start, &bits);
				picture = !ret;
			}
		} else if (type == 35 || type == 38 || type == 39 || type == 40) {
			if ((type == 35 || type == 39) && picture)
				ret = -EBADMSG;
			else if (type == 40 && !picture)
				ret = -EBADMSG;
			else
				ret = hevc_auxiliary(&bits, type);
			/*
			 * Decoding never reads SEI. A damaged one, such as a
			 * truncated hvcC copy that h265parse inserts, is skipped.
			 */
			if (ret == -EBADMSG && (type == 39 || type == 40))
				ret = 0;
		} else {
			ret = -EOPNOTSUPP;
		}
		kvfree(rbsp);
		if (ret)
			goto failed;
	}
	if (!nals) {
		ret = -EBADMSG;
		goto failed;
	}
	for (nals = 0; nals < HEVC_VPS_COUNT; nals++)
		input->changed |= !!input->vps_update[nals];
	for (nals = 0; nals < HEVC_SPS_COUNT; nals++)
		input->changed |= !!input->sps_update[nals];
	for (nals = 0; nals < HEVC_PPS_COUNT; nals++)
		input->changed |= !!input->pps_update[nals];
	if (input->changed && bank->revision == U64_MAX) {
		ret = -EOVERFLOW;
		goto failed;
	}
	input->view.data = input->data;
	input->view.bytes = bytes;
	input->view.kind = picture ? S7D_HEVC_INPUT_PICTURE : S7D_HEVC_INPUT_HEADERS;
	input->view.slice.parameter_epoch = input->selected_epoch;
	*out = input;
	return 0;
failed:
	s7d_hevc_input_release(input);
	return ret;
}

int s7d_hevc_input_commit(struct s7d_hevc_input_bank *bank,
			  struct s7d_hevc_input *input)
{
	u32 i;

	if (IS_ERR_OR_NULL(bank) || !input)
		return -EINVAL;
	if (input->committed || input->bank != bank || input->bank_revision != bank->revision)
		return -ESTALE;
	for (i = 0; i < HEVC_VPS_COUNT; i++)
		if (input->vps_update[i]) {
			hevc_parameter_free(bank->vps[i]);
			bank->vps[i] = input->vps_update[i];
			input->vps_update[i] = NULL;
		}
	for (i = 0; i < HEVC_SPS_COUNT; i++)
		if (input->sps_update[i]) {
			hevc_parameter_free(bank->sps[i]);
			bank->sps[i] = input->sps_update[i];
			input->sps_update[i] = NULL;
		}
	for (i = 0; i < HEVC_PPS_COUNT; i++)
		if (input->pps_update[i]) {
			hevc_parameter_free(bank->pps[i]);
			bank->pps[i] = input->pps_update[i];
			input->pps_update[i] = NULL;
		}
	bank->bytes = input->bank_bytes;
	bank->revision += input->changed;
	input->committed = true;
	return 0;
}

const struct s7d_hevc_input_result *
s7d_hevc_input_view(const struct s7d_hevc_input *input)
{
	return input ? &input->view : NULL;
}

size_t s7d_hevc_input_prefix_bytes(const struct s7d_hevc_input *input)
{
	if (!input || input->view.kind != S7D_HEVC_INPUT_PICTURE)
		return 0;
	return input->parameter_bytes[0] + input->parameter_bytes[1] +
	       input->parameter_bytes[2] + 12;
}

int s7d_hevc_input_copy_prefix(const struct s7d_hevc_input *input,
			       u8 *destination, size_t capacity)
{
	size_t bytes = s7d_hevc_input_prefix_bytes(input), offset = 0;
	u32 i;

	if (!bytes || !destination)
		return -EINVAL;
	if (capacity < bytes)
		return -ENOSPC;
	for (i = 0; i < 3; i++) {
		memcpy(destination + offset, "\0\0\0\1", 4);
		offset += 4;
		memcpy(destination + offset, input->parameter[i], input->parameter_bytes[i]);
		offset += input->parameter_bytes[i];
	}
	return 0;
}

void s7d_hevc_input_release(struct s7d_hevc_input *input)
{
	u32 i;

	if (!input)
		return;
	for (i = 0; i < HEVC_VPS_COUNT; i++)
		hevc_parameter_free(input->vps_update[i]);
	for (i = 0; i < HEVC_SPS_COUNT; i++)
		hevc_parameter_free(input->sps_update[i]);
	for (i = 0; i < HEVC_PPS_COUNT; i++)
		hevc_parameter_free(input->pps_update[i]);
	for (i = 0; i < 3; i++)
		kvfree(input->parameter[i]);
	kvfree(input->data);
	kvfree(input);
}
