// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../common/s7d-codec-session.h"
#include "s7d-av1-reference.h"

#define AV1_CDF_BANK_BYTES 0x8000U

struct reference_layout {
	struct s7d_av1_reference_layout compressed;
	u32 header_offset;
	u32 mv_offset;
	u32 linear_offset;
	u32 y_bytes;
	u32 uv_bytes;
	u32 used_bytes;
	u32 allocation_bytes;
};

struct s7d_av1_references {
	struct s7d_codec_session *session;
	struct s7d_av1_workspace workspace;
	struct reference_layout layout;
	struct s7d_av1_dpb dpb;
	struct s7d_av1_reference_backing backing[S7D_AV1_DPB_MAX_SLOTS];
	struct s7d_av1_reference_state state[S7D_AV1_DPB_MAX_SLOTS];
	struct s7d_av1_reference_state pending_state;
	struct s7d_codec_dma *memory[S7D_AV1_DPB_MAX_SLOTS];
	u8 *cpu[S7D_AV1_DPB_MAX_SLOTS];
	u64 lease_epoch;
	u64 input_token;
	u64 last_token;
	u32 reserved_slot;
	u32 max_width;
	u32 max_height;
	bool idle;
	bool running;
	bool prepared;
	bool configured;
};

static int stopped(int halt_status, bool joined)
{
	if (halt_status || !joined)
		return halt_status < 0 ? halt_status : halt_status ? -EIO : -EBUSY;
	return 0;
}

static int reference_layout(const struct s7d_av1_sequence *sequence,
			    struct reference_layout *layout)
{
	struct reference_layout next = { 0 };
	u32 superblock = sequence->use_128x128_superblock ? 128 : 64;
	u32 pixels;
	int ret;

	ret = s7d_av1_reference_layout(sequence->max_frame_width,
		sequence->max_frame_height, 8, superblock, &next.compressed);
	if (ret)
		return ret;
	pixels = ALIGN(sequence->max_frame_width, superblock) *
		ALIGN(sequence->max_frame_height, superblock);
	next.uv_bytes = ALIGN(pixels / 2, SZ_64K);
	next.y_bytes = next.uv_bytes * 2;
	next.header_offset = next.compressed.body_allocation;
	next.mv_offset = ALIGN(next.header_offset + next.compressed.header_capacity, SZ_64K);
	next.linear_offset = next.mv_offset + next.compressed.mv_allocation;
	next.used_bytes = next.linear_offset + next.y_bytes + next.uv_bytes;
	next.allocation_bytes = next.used_bytes + SZ_64K - 1;
	*layout = next;
	return 0;
}

struct s7d_av1_references *
s7d_av1_references_create(struct s7d_codec_session *session,
			  const struct s7d_av1_workspace *workspace,
			  const struct s7d_av1_sequence *sequence,
			  u64 lease_epoch, int halt_status, bool joined)
{
	struct s7d_av1_references *references;
	struct s7d_av1_dpb_sequence seq;
	struct reference_layout layout;
	int ret = stopped(halt_status, joined);

	if (ret || IS_ERR_OR_NULL(session) || !lease_epoch)
		return ERR_PTR(ret ?: -EINVAL);
	ret = s7d_av1_admit_sequence(sequence);
	if (ret)
		return ERR_PTR(ret);
	if (sequence->profile || sequence->bit_depth != 8 || sequence->monochrome ||
	    !sequence->subsampling_x || !sequence->subsampling_y)
		return ERR_PTR(-EOPNOTSUPP);
	ret = s7d_av1_workspace_validate(workspace);
	if (ret)
		return ERR_PTR(ret);
	if (workspace->max_width < sequence->max_frame_width ||
	    workspace->max_height < sequence->max_frame_height ||
	    workspace->part[S7D_AV1_CDF].bytes !=
	    AV1_CDF_BANK_BYTES * S7D_AV1_DPB_MAX_SLOTS ||
	    workspace->part[S7D_AV1_SEGMENT_MAP].bytes % S7D_AV1_DPB_MAX_SLOTS)
		return ERR_PTR(-EINVAL);
	ret = reference_layout(sequence, &layout);
	if (ret)
		return ERR_PTR(ret);
	references = kzalloc(sizeof(*references), GFP_KERNEL);
	if (!references)
		return ERR_PTR(-ENOMEM);
	seq.order_hint_bits = sequence->enable_order_hint ? sequence->order_hint_bits : 0;
	seq.frame_id_bits = sequence->frame_id_numbers_present ? sequence->frame_id_bits : 0;
	seq.delta_frame_id_bits = sequence->frame_id_numbers_present ?
		sequence->delta_frame_id_bits : 0;
	ret = s7d_av1_dpb_init(&references->dpb, &seq, lease_epoch, S7D_AV1_DPB_MAX_SLOTS);
	if (ret) {
		kfree(references);
		return ERR_PTR(ret);
	}
	s7d_codec_session_get(session);
	references->session = session;
	references->workspace = *workspace;
	references->layout = layout;
	references->lease_epoch = lease_epoch;
	references->reserved_slot = S7D_AV1_DPB_NO_SLOT;
	references->max_width = sequence->max_frame_width;
	references->max_height = sequence->max_frame_height;
	references->idle = true;
	return references;
}

static int allocate_slot(struct s7d_av1_references *references, u32 slot)
{
	struct s7d_av1_reference_backing next = { 0 };
	const struct reference_layout *layout = &references->layout;
	const struct s7d_av1_workspace *ws = &references->workspace;
	struct s7d_codec_dma *memory;
	dma_addr_t dma;
	u64 base;
	u8 *cpu;
	int ret;

	if (references->backing[slot].id.backing_cookie)
		return 0;
	if (references->memory[slot])
		return -EIO;
	memory = s7d_codec_dma_alloc(references->session, layout->allocation_bytes);
	if (IS_ERR(memory))
		return PTR_ERR(memory);
	references->memory[slot] = memory;
	dma = s7d_codec_dma_addr(memory);
	base = ALIGN((u64)dma, (u64)SZ_64K);
	if (!base || base > U32_MAX || base + layout->used_bytes > (1ULL << 32) ||
	    base < dma || base - dma + layout->used_bytes > s7d_codec_dma_size(memory))
		return -ERANGE;
	cpu = (u8 *)s7d_codec_dma_cpu(memory) + (base - dma);
	next.id.slot = slot;
	next.id.backing_cookie = base;
	next.body.address = base;
	next.body.bytes = layout->compressed.body_allocation;
	next.header.address = base + layout->header_offset;
	next.header.bytes = layout->compressed.header_capacity;
	next.mv.address = base + layout->mv_offset;
	next.mv.bytes = layout->compressed.mv_allocation;
	next.linear.address = base + layout->linear_offset;
	next.linear.bytes = layout->y_bytes + layout->uv_bytes;
	next.y_bytes = layout->y_bytes;
	next.uv_bytes = layout->uv_bytes;
	next.cdf.address = ws->part[S7D_AV1_CDF].address + slot * AV1_CDF_BANK_BYTES;
	next.cdf.bytes = AV1_CDF_BANK_BYTES;
	next.segment_map.bytes = ws->part[S7D_AV1_SEGMENT_MAP].bytes / S7D_AV1_DPB_MAX_SLOTS;
	next.segment_map.address = ws->part[S7D_AV1_SEGMENT_MAP].address +
		slot * next.segment_map.bytes;
	memset(cpu, 0, layout->used_bytes);
	dma_wmb();
	ret = s7d_av1_dpb_bind(&references->dpb, slot, next.id.backing_cookie);
	if (ret)
		return ret;
	references->cpu[slot] = cpu;
	references->backing[slot] = next;
	return 0;
}

int s7d_av1_references_run_begin(struct s7d_av1_references *references,
				 u64 lease_epoch, u64 input_token)
{
	u32 owned, slot;
	int ret;

	if (IS_ERR_OR_NULL(references) || !input_token)
		return -EINVAL;
	if (references->lease_epoch != lease_epoch || input_token <= references->last_token)
		return -ESTALE;
	if (!references->idle || references->running || references->dpb.active ||
	    references->dpb.output_pending)
		return -EBUSY;
	ret = s7d_av1_dpb_owned_mask(&references->dpb, &owned);
	if (ret)
		return ret;
	for (slot = 0; slot < S7D_AV1_DPB_MAX_SLOTS; slot++)
		if (!(owned & BIT(slot)))
			break;
	if (slot == S7D_AV1_DPB_MAX_SLOTS)
		return -ENOSPC;
	ret = allocate_slot(references, slot);
	if (ret)
		return ret;
	references->reserved_slot = slot;
	references->input_token = input_token;
	references->last_token = input_token;
	references->running = true;
	references->idle = false;
	return 0;
}

static void resolve_state(const struct s7d_av1_header *header,
			  const struct s7d_av1_reference_state *primary,
			  struct s7d_av1_reference_state *state)
{
	static const s8 defaults[8] = { 1, 0, 0, 0, -1, 0, -1, -1 };
	u32 i;

	memset(state, 0, sizeof(*state));
	state->mi_cols = ALIGN(header->frame.decode_width, 8) / 4;
	state->mi_rows = ALIGN(header->frame.height, 8) / 4;
	state->seg_mi_cols = state->mi_cols;
	state->seg_mi_rows = state->mi_rows;
	state->segmentation_enabled = header->segmentation & BIT(0);
	state->segmentation_update_map = header->segmentation & BIT(3);
	state->intra_only = header->segmentation & BIT(2);
	if (primary) {
		memcpy(state->ref_delta, primary->ref_delta, sizeof(state->ref_delta));
		memcpy(state->mode_delta, primary->mode_delta, sizeof(state->mode_delta));
		state->prev_segmentation_enabled = primary->segmentation_enabled &&
			(primary->segmentation_update_map || primary->prev_segmentation_enabled) &&
			state->mi_rows == primary->mi_rows && state->mi_cols == primary->mi_cols;
		if (state->segmentation_enabled && !(header->segmentation & BIT(1))) {
			memcpy(state->seg_lf_y, primary->seg_lf_y, sizeof(state->seg_lf_y));
			memcpy(state->seg_lf_c, primary->seg_lf_c, sizeof(state->seg_lf_c));
			memcpy(state->segment_feature, primary->segment_feature,
			       sizeof(state->segment_feature));
		}
		if (state->segmentation_enabled && !state->segmentation_update_map) {
			state->seg_mi_cols = primary->seg_mi_cols;
			state->seg_mi_rows = primary->seg_mi_rows;
		}
	}
	if (header->filter_delta_enabled && header->filter_delta_update) {
		for (i = 0; i < 8; i++)
			if (!primary || (header->filter_ref_update_mask & BIT(i)))
				state->ref_delta[i] = header->filter_ref_delta[i];
		for (i = 0; i < 2; i++)
			if (!primary || (header->filter_mode_update_mask & BIT(i)))
				state->mode_delta[i] = header->filter_mode_delta[i];
	} else if (!primary || header->filter_delta_reset) {
		memcpy(state->ref_delta, defaults, sizeof(defaults));
		memset(state->mode_delta, 0, sizeof(state->mode_delta));
	}
	if (state->segmentation_enabled && (header->segmentation & BIT(1))) {
		memcpy(state->seg_lf_y, header->segment_y, sizeof(state->seg_lf_y));
		memcpy(state->seg_lf_c, header->segment_c, sizeof(state->seg_lf_c));
	}
	if (!state->segmentation_enabled)
		for (i = 0; i < 8; i++)
			state->segment_feature[i] = 0x80000000U | (i << 22);
}

int s7d_av1_references_prepare(struct s7d_av1_references *references,
			       u64 lease_epoch, u64 input_token,
			       const struct s7d_av1_header *header,
			       const struct s7d_av1_dpb_metadata *metadata,
			       struct s7d_av1_reference_picture *picture)
{
	const struct s7d_av1_reference_state *primary = NULL;
	struct s7d_av1_dpb_plan plan;
	u32 slot;
	int ret;

	if (IS_ERR_OR_NULL(references) || !header || !metadata || !picture)
		return -EINVAL;
	if (references->lease_epoch != lease_epoch || references->input_token != input_token ||
	    metadata->input_token != input_token)
		return -ESTALE;
	if (!references->running || references->prepared || references->idle)
		return -EBUSY;
	if (!header->frame.show_existing &&
	    (header->frame.bit_depth != 8 ||
	     header->frame.width > references->max_width ||
	     header->frame.height > references->max_height))
		return -EOPNOTSUPP;
	slot = header->frame.show_existing ? S7D_AV1_DPB_NO_SLOT : references->reserved_slot;
	ret = s7d_av1_dpb_begin(&references->dpb, &header->frame, slot, metadata, &plan);
	if (ret)
		return ret;
	if (plan.decode) {
		if (plan.primary_ref != S7D_AV1_DPB_PRIMARY_NONE)
			primary = &references->state[plan.reference[plan.primary_ref].id.slot];
		resolve_state(header, primary, &references->pending_state);
		references->backing[slot].pitch = ALIGN(header->frame.width, 64);
	} else {
		references->pending_state = references->state[plan.current_picture.id.slot];
	}
	references->prepared = true;
	*picture = (struct s7d_av1_reference_picture) {
		.plan = plan,
		.backing = references->backing,
		.count = S7D_AV1_DPB_MAX_SLOTS,
		.primary = primary,
		.state = &references->pending_state,
	};
	return 0;
}

int s7d_av1_references_configured(struct s7d_av1_references *references,
				  u64 lease_epoch, u64 input_token)
{
	if (IS_ERR_OR_NULL(references))
		return -EINVAL;
	if (references->lease_epoch != lease_epoch || references->input_token != input_token)
		return -ESTALE;
	if (!references->running || !references->prepared || references->configured)
		return -EBUSY;
	references->configured = true;
	return 0;
}

int s7d_av1_references_finish(struct s7d_av1_references *references,
			      u64 lease_epoch, u64 input_token,
			      int halt_status, bool joined, bool completed)
{
	u32 slot;
	int ret;

	if (IS_ERR_OR_NULL(references))
		return -EINVAL;
	if (references->lease_epoch != lease_epoch || references->input_token != input_token)
		return -ESTALE;
	if (!references->running)
		return -EBUSY;
	ret = stopped(halt_status, joined);
	if (ret)
		return ret;
	if (completed && (!references->prepared ||
	    (references->dpb.pending.decode && !references->configured)))
		return -EINVAL;
	if (references->prepared) {
		slot = references->dpb.pending.current_picture.id.slot;
		if (completed)
			ret = s7d_av1_dpb_complete(&references->dpb, references->dpb.pending.serial,
				input_token, true, true);
		else
			ret = s7d_av1_dpb_abort(&references->dpb, references->dpb.pending.serial,
				input_token, true);
		if (ret)
			return ret;
		if (completed)
			references->state[slot] = references->pending_state;
	}
	references->prepared = false;
	references->configured = false;
	references->running = false;
	references->idle = true;
	references->reserved_slot = S7D_AV1_DPB_NO_SLOT;
	memset(&references->pending_state, 0, sizeof(references->pending_state));
	return 0;
}

int s7d_av1_references_output(struct s7d_av1_references *references,
			      struct s7d_av1_reference_output *output)
{
	struct s7d_av1_reference_output next = { 0 };
	const struct s7d_av1_reference_backing *backing;
	u32 slot;
	int ret;

	if (IS_ERR_OR_NULL(references) || !output)
		return -EINVAL;
	if (!references->idle || references->running)
		return -EBUSY;
	ret = s7d_av1_dpb_peek_output(&references->dpb, &next.output);
	if (ret)
		return ret;
	slot = next.output.picture.id.slot;
	backing = &references->backing[slot];
	if (!backing->id.backing_cookie ||
	    backing->id.backing_cookie != next.output.picture.id.backing_cookie)
		return -ESTALE;
	dma_rmb();
	next.memory = references->memory[slot];
	next.source_offset[0] = (u64)backing->linear.address -
		s7d_codec_dma_addr(next.memory);
	next.source_offset[1] = next.source_offset[0] + backing->y_bytes;
	next.linear = backing->linear;
	next.y = references->cpu[slot] + references->layout.linear_offset;
	next.uv = next.y + backing->y_bytes;
	next.pitch = backing->pitch;
	next.y_bytes = backing->y_bytes;
	next.uv_bytes = backing->uv_bytes;
	*output = next;
	return 0;
}

int s7d_av1_references_output_committed(struct s7d_av1_references *references,
					const struct s7d_av1_reference_output *output)
{
	if (IS_ERR_OR_NULL(references) || !output)
		return -EINVAL;
	if (!references->idle || references->running)
		return -EBUSY;
	return s7d_av1_dpb_output_committed(&references->dpb, &output->output);
}

int s7d_av1_references_parser_words(const struct s7d_av1_references *references,
				    u64 lease_epoch,
				    u32 words[S7D_AV1_PARSER_REFERENCE_WORDS])
{
	u32 next[S7D_AV1_PARSER_REFERENCE_WORDS] = { 0 };
	u32 i, owned;
	int ret;

	if (IS_ERR_OR_NULL(references) || !words)
		return -EINVAL;
	if (references->lease_epoch != lease_epoch)
		return -ESTALE;
	if (references->dpb.active)
		return -EBUSY;
	ret = s7d_av1_dpb_owned_mask(&references->dpb, &owned);
	if (ret)
		return ret;
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
		const struct s7d_av1_dpb_picture *picture;
		u32 slot = references->dpb.map[i];

		if (slot == S7D_AV1_DPB_NO_SLOT)
			continue;
		picture = &references->dpb.picture[slot];
		next[i / 4] |= picture->order_hint << ((i % 4) * 8);
		next[2 + i] = (picture->width << 16) | picture->height;
		next[10 + i / 4] |= slot << ((i % 4) * 8);
	}
	memcpy(words, next, sizeof(next));
	return 0;
}

int s7d_av1_references_destroy(struct s7d_av1_references *references,
			       u64 lease_epoch, int halt_status, bool joined)
{
	u32 unbound;
	int ret;

	if (IS_ERR_OR_NULL(references))
		return -EINVAL;
	if (references->lease_epoch != lease_epoch)
		return -ESTALE;
	ret = stopped(halt_status, joined);
	if (ret)
		return ret;
	if (references->dpb.output_pending)
		return -EBUSY;
	ret = s7d_av1_dpb_release(&references->dpb, true, &unbound);
	if (ret)
		return ret;
	s7d_codec_session_put(references->session);
	kfree(references);
	return 0;
}
