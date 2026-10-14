// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "s7d-hevc-dpb.h"

static bool hevc_metadata_equal(const struct s7d_hevc_picture_metadata *a,
				const struct s7d_hevc_picture_metadata *b)
{
	return a->input_token == b->input_token && a->timestamp_ns == b->timestamp_ns &&
		a->timestamp_meta.flags == b->timestamp_meta.flags &&
		!memcmp(&a->timestamp_meta.timecode, &b->timestamp_meta.timecode,
			sizeof(a->timestamp_meta.timecode));
}

static bool hevc_poc_equal(const struct s7d_hevc_poc_state *a,
			  const struct s7d_hevc_poc_state *b)
{
	return a->prev_tid0_poc == b->prev_tid0_poc && a->irap_poc == b->irap_poc &&
		a->irap_nal_type == b->irap_nal_type &&
		a->prev_tid0_valid == b->prev_tid0_valid && a->irap_valid == b->irap_valid &&
		a->no_rasl_output == b->no_rasl_output && a->started == b->started;
}

static bool hevc_actions_equal(const struct s7d_hevc_dpb_actions *a,
			      const struct s7d_hevc_dpb_actions *b)
{
	return a->generation == b->generation && a->revision == b->revision &&
		a->surface_release_mask == b->surface_release_mask &&
		a->mv_release_mask == b->mv_release_mask && a->discard_mask == b->discard_mask &&
		!memcmp(a->discard_write_token, b->discard_write_token,
			sizeof(a->discard_write_token));
}

static bool hevc_idr(u32 nal)
{
	return nal == 19 || nal == 20;
}

static bool hevc_bla(u32 nal)
{
	return nal >= 16 && nal <= 18;
}

static bool hevc_irap(u32 nal)
{
	return nal >= 16 && nal <= 21;
}

static bool hevc_leading(u32 nal)
{
	return nal >= 6 && nal <= 9;
}

static bool hevc_rasl(u32 nal)
{
	return nal == 8 || nal == 9;
}

static bool hevc_reference(u32 nal)
{
	return hevc_irap(nal) || (nal <= 9 && (nal & 1));
}

static u32 hevc_slot_mask(const struct s7d_hevc_dpb *dpb)
{
	return (1U << dpb->slot_count) - 1;
}

static u32 hevc_valid_mask(const struct s7d_hevc_dpb *dpb)
{
	u32 i, mask = 0;

	for (i = 0; i < dpb->slot_count; i++)
		if (dpb->picture[i].valid)
			mask |= BIT(i);
	return mask;
}

static u32 hevc_output_mask(const struct s7d_hevc_dpb *dpb)
{
	u32 i, mask = 0;

	for (i = 0; i < dpb->slot_count; i++)
		if (dpb->picture[i].valid && dpb->picture[i].output_needed)
			mask |= BIT(i);
	return mask;
}

static int hevc_state_check(const struct s7d_hevc_dpb *dpb)
{
	u32 i;

	if (!dpb || !dpb->generation || !dpb->parameter_epoch ||
	    !dpb->revision || !dpb->max_dec_pic_buffering ||
	    dpb->max_dec_pic_buffering > 16 ||
	    dpb->num_reorder_pics >= dpb->max_dec_pic_buffering ||
	    dpb->slot_count < dpb->max_dec_pic_buffering + 2 ||
	    dpb->slot_count > S7D_HEVC_DPB_SLOTS ||
	    dpb->log2_max_poc_lsb < 4 || dpb->log2_max_poc_lsb > 16)
		return -EINVAL;
	if (dpb->retiring_mask & ~hevc_slot_mask(dpb))
		return -EUCLEAN;
	if (dpb->retiring_mask &&
	    (dpb->pending_actions.generation != dpb->generation ||
	     dpb->pending_actions.revision != dpb->revision ||
	     (dpb->pending_actions.surface_release_mask |
	      dpb->pending_actions.discard_mask) != dpb->retiring_mask ||
	     (dpb->pending_actions.surface_release_mask & dpb->pending_actions.discard_mask) ||
	     dpb->pending_actions.mv_release_mask != dpb->retiring_mask))
		return -EUCLEAN;
	if (dpb->active &&
	    (dpb->retiring_mask || !dpb->active_write_token ||
	     dpb->active_plan.plan_id != dpb->revision ||
	     dpb->active_plan.current_slot >= dpb->slot_count))
		return -EUCLEAN;
	for (i = 0; i < dpb->slot_count; i++) {
		const struct s7d_hevc_dpb_picture *pic = &dpb->picture[i];

		if (!pic->valid) {
			if (pic->decoded || pic->reference || pic->output_needed ||
			    pic->published ||
			    (dpb->retiring_mask & BIT(i)))
				return -EUCLEAN;
			continue;
		}
		if (!pic->metadata.input_token || !pic->write_token ||
		    pic->slice_type > 2 ||
		    (pic->published && (!pic->decoded || pic->output_needed)) ||
		    (!pic->decoded && !(dpb->retiring_mask & BIT(i)) &&
		     (!dpb->active || dpb->active_plan.current_slot != i)))
			return -EUCLEAN;
	}
	return 0;
}

int s7d_hevc_dpb_init(struct s7d_hevc_dpb *dpb,
		      const struct s7d_hevc_sps *sps, u64 generation,
		      u64 parameter_epoch, u32 slot_count)
{
	u64 latency;

	if (!dpb || !sps || !generation || !parameter_epoch ||
	    !sps->max_dec_pic_buffering || sps->max_dec_pic_buffering > 16 ||
	    sps->num_reorder_pics >= sps->max_dec_pic_buffering ||
	    sps->log2_max_poc_lsb < 4 || sps->log2_max_poc_lsb > 16 ||
	    slot_count < sps->max_dec_pic_buffering + 2 ||
	    slot_count > S7D_HEVC_DPB_SLOTS)
		return -EINVAL;
	if (sps->max_sub_layers != 1)
		return -EOPNOTSUPP;
	latency = sps->max_latency_increase_plus1 ?
		(u64)sps->num_reorder_pics + sps->max_latency_increase_plus1 - 1 : 0;
	if (latency > U32_MAX)
		return -EOVERFLOW;
	memset(dpb, 0, sizeof(*dpb));
	dpb->generation = generation;
	dpb->parameter_epoch = parameter_epoch;
	dpb->revision = 1;
	dpb->slot_count = slot_count;
	dpb->max_dec_pic_buffering = sps->max_dec_pic_buffering;
	dpb->num_reorder_pics = sps->num_reorder_pics;
	dpb->max_latency_pictures = latency;
	dpb->log2_max_poc_lsb = sps->log2_max_poc_lsb;
	return 0;
}

static int hevc_slice_check(const struct s7d_hevc_dpb *dpb,
			   const struct s7d_hevc_slice_info *slice)
{
	s32 last_negative = 0, last_positive = 0;
	bool positive = false;
	u32 i;

	if (!slice || slice->slice_type > 2 ||
	    !slice->temporal_id_plus1 || slice->temporal_id_plus1 > 7 ||
	    slice->rps_count > S7D_HEVC_RPS_ENTRIES ||
	    slice->active[0] > S7D_HEVC_RPS_ENTRIES ||
	    slice->active[1] > S7D_HEVC_RPS_ENTRIES ||
	    slice->poc_lsb >= (1U << dpb->log2_max_poc_lsb))
		return -EINVAL;
	if (slice->parameter_epoch != dpb->parameter_epoch)
		return -ESTALE;
	if (slice->temporal_id_plus1 != 1 || !slice->first_slice ||
	    slice->dependent_slice || slice->slice_address ||
	    (slice->nal_type > 9 && !hevc_irap(slice->nal_type)))
		return -EOPNOTSUPP;
	if (slice->rps_count >= dpb->max_dec_pic_buffering)
		return -EINVAL;
	if (hevc_irap(slice->nal_type) && slice->slice_type != 2)
		return -EBADMSG;
	if (!hevc_irap(slice->nal_type) && slice->no_output_of_prior_pics)
		return -EBADMSG;
	if (slice->slice_type == 2) {
		if (slice->active[0] || slice->active[1] ||
		    slice->modification[0] || slice->modification[1])
			return -EBADMSG;
	} else if (!slice->active[0] ||
		   (slice->slice_type == 0 && !slice->active[1]) ||
		   (slice->slice_type == 1 &&
		    (slice->active[1] || slice->modification[1]))) {
		return -EBADMSG;
	}
	if (hevc_idr(slice->nal_type) &&
	    (slice->rps_count || slice->poc_lsb || slice->temporal_mvp))
		return -EBADMSG;
	for (i = 0; i < slice->rps_count; i++) {
		const struct s7d_hevc_rps_entry *ref = &slice->rps[i];

		if (ref->long_term)
			return -EOPNOTSUPP;
		if (!ref->delta_poc || ref->delta_poc < -4096 ||
		    ref->delta_poc > 4095)
			return -ERANGE;
		if (hevc_irap(slice->nal_type) && ref->used)
			return -EBADMSG;
		if (ref->delta_poc < 0) {
			if (positive || ref->delta_poc >= last_negative)
				return -EBADMSG;
			last_negative = ref->delta_poc;
		} else {
			if (ref->delta_poc <= last_positive)
				return -EBADMSG;
			positive = true;
			last_positive = ref->delta_poc;
		}
	}
	return 0;
}

static int hevc_poc_plan(const struct s7d_hevc_dpb *dpb,
			 const struct s7d_hevc_slice_info *slice,
			 struct s7d_hevc_picture_plan *plan)
{
	struct s7d_hevc_poc_state *next = &plan->next_poc;
	s64 poc, msb = 0;
	u32 max_lsb = 1U << dpb->log2_max_poc_lsb;
	u32 previous_lsb;
	bool no_rasl = hevc_idr(slice->nal_type) || hevc_bla(slice->nal_type) ||
		(!dpb->poc.started && slice->nal_type == 21);

	*next = dpb->poc;
	if (!dpb->poc.started && !hevc_irap(slice->nal_type))
		return -ENODATA;
	if (!no_rasl) {
		if (!dpb->poc.prev_tid0_valid)
			return -ENODATA;
		previous_lsb = (u32)dpb->poc.prev_tid0_poc & (max_lsb - 1);
		msb = (s64)dpb->poc.prev_tid0_poc - previous_lsb;
		if (slice->poc_lsb < previous_lsb &&
		    previous_lsb - slice->poc_lsb >= max_lsb / 2)
			msb += max_lsb;
		else if (slice->poc_lsb > previous_lsb &&
			 slice->poc_lsb - previous_lsb > max_lsb / 2)
			msb -= max_lsb;
	}
	poc = hevc_idr(slice->nal_type) ? 0 : msb + slice->poc_lsb;
	if (poc < S32_MIN || poc > S32_MAX)
		return -EOVERFLOW;
	plan->poc = poc;
	plan->clear_dpb = no_rasl;
	if (hevc_irap(slice->nal_type)) {
		next->irap_poc = poc;
		next->irap_nal_type = slice->nal_type;
		next->irap_valid = true;
		next->no_rasl_output = no_rasl;
	}
	if (hevc_leading(slice->nal_type)) {
		if (!dpb->poc.irap_valid || poc >= dpb->poc.irap_poc)
			return -EBADMSG;
		if (dpb->poc.irap_nal_type == 18 || dpb->poc.irap_nal_type == 20 ||
		    (hevc_rasl(slice->nal_type) &&
		     (dpb->poc.irap_nal_type == 17 || dpb->poc.irap_nal_type == 19)))
			return -EBADMSG;
		if (hevc_rasl(slice->nal_type) && dpb->poc.no_rasl_output) {
			plan->kind = S7D_HEVC_PLAN_SKIP_RASL;
			return 0;
		}
	}
	if (!hevc_leading(slice->nal_type) && hevc_reference(slice->nal_type)) {
		next->prev_tid0_poc = poc;
		next->prev_tid0_valid = true;
	}
	next->started = true;
	return 0;
}

static int hevc_find_reference(const struct s7d_hevc_dpb *dpb, s32 poc)
{
	u32 i;
	int found = -ENOENT;

	for (i = 0; i < dpb->slot_count; i++) {
		const struct s7d_hevc_dpb_picture *pic = &dpb->picture[i];

		if (!pic->valid || !pic->decoded || !pic->reference || pic->poc != poc)
			continue;
		if (found >= 0)
			return -EUCLEAN;
		found = i;
	}
	return found;
}

static int hevc_reference_plan(const struct s7d_hevc_dpb *dpb,
			       const struct s7d_hevc_slice_info *slice,
			       struct s7d_hevc_picture_plan *plan)
{
	struct s7d_hevc_picture_ref before[S7D_HEVC_RPS_ENTRIES];
	struct s7d_hevc_picture_ref after[S7D_HEVC_RPS_ENTRIES];
	u32 nb = 0, na = 0, total, i, list;

	for (i = 0; i < slice->rps_count; i++) {
		const struct s7d_hevc_rps_entry *rps = &slice->rps[i];
		struct s7d_hevc_picture_ref ref;
		s64 poc = (s64)plan->poc + rps->delta_poc;
		int slot;

		if (poc < S32_MIN || poc > S32_MAX)
			return -EOVERFLOW;
		if (plan->clear_dpb)
			continue;
		slot = hevc_find_reference(dpb, poc);
		if (slot < 0)
			return slot;
		plan->retain_mask |= BIT(slot);
		if (!rps->used)
			continue;
		ref.poc = poc;
		ref.slot = slot;
		if (rps->delta_poc < 0)
			before[nb++] = ref;
		else
			after[na++] = ref;
	}
	total = nb + na;
	if (slice->slice_type == 2)
		return 0;
	if (!total)
		return -ENOENT;
	plan->low_delay = true;
	for (list = 0; list < 2; list++) {
		plan->list_count[list] = slice->active[list];
		for (i = 0; i < slice->active[list]; i++) {
			struct s7d_hevc_picture_ref ref;
			u32 index = slice->modification[list] ?
				slice->modification_index[list][i] : i % total;

			if (index >= total || (slice->modification[list] && total <= 1))
				return -EBADMSG;
			if (list == 0)
				ref = index < nb ? before[index] : after[index - nb];
			else
				ref = index < na ? after[index] : before[index - na];
			plan->list[list][i] = ref;
			if (ref.poc > plan->poc)
				plan->low_delay = false;
		}
	}
	list = slice->slice_type == 1 || slice->collocated_from_l0 ? 0 : 1;
	if (slice->collocated_ref_idx >= plan->list_count[list])
		return -EBADMSG;
	plan->collocated = plan->list[list][slice->collocated_ref_idx];
	plan->collocated_valid = true;
	return 0;
}

int s7d_hevc_dpb_plan(const struct s7d_hevc_dpb *dpb,
		      const struct s7d_hevc_slice_info *slice,
		      const struct s7d_hevc_picture_metadata *metadata,
		      u32 queued_slot_mask, struct s7d_hevc_picture_plan *plan)
{
	struct s7d_hevc_picture_plan next = { };
	u32 output_mask, valid_mask, available, i;
	int ret;

	if (!plan || !metadata || !metadata->input_token)
		return -EINVAL;
	ret = hevc_state_check(dpb);
	if (ret)
		return ret;
	if (dpb->active || dpb->retiring_mask || dpb->pending_reset_generation)
		return -EBUSY;
	if (metadata->input_token <= dpb->last_input_token)
		return -ESTALE;
	if (dpb->revision > U64_MAX - 3)
		return -EOVERFLOW;
	if (queued_slot_mask & ~hevc_slot_mask(dpb))
		return -EINVAL;
	ret = hevc_slice_check(dpb, slice);
	if (ret)
		return ret;
	next.generation = dpb->generation;
	next.parameter_epoch = dpb->parameter_epoch;
	next.base_revision = dpb->revision;
	next.plan_id = dpb->revision + 1;
	next.slot_count = dpb->slot_count;
	next.current_slot = S7D_HEVC_NO_SLOT;
	next.collocated.slot = S7D_HEVC_NO_SLOT;
	next.metadata = *metadata;
	next.slice_type = slice->slice_type;
	next.reference = hevc_reference(slice->nal_type);
	next.output_needed = slice->pic_output_flag;
	next.temporal_mvp = slice->temporal_mvp;
	ret = hevc_poc_plan(dpb, slice, &next);
	if (ret)
		return ret;
	if (next.kind == S7D_HEVC_PLAN_SKIP_RASL) {
		*plan = next;
		return 0;
	}
	ret = hevc_reference_plan(dpb, slice, &next);
	if (ret)
		return ret;
	valid_mask = hevc_valid_mask(dpb);
	output_mask = hevc_output_mask(dpb);
	for (i = 0; i < dpb->slot_count; i++) {
		const struct s7d_hevc_dpb_picture *pic = &dpb->picture[i];

		if (!(valid_mask & BIT(i)))
			continue;
		if (pic->metadata.input_token == metadata->input_token ||
		    (!next.clear_dpb && pic->poc == next.poc))
			return -EBADMSG;
	}
	if (next.clear_dpb) {
		if (slice->no_output_of_prior_pics)
			next.drop_output_mask = output_mask;
		else if (output_mask)
			next.kind = S7D_HEVC_PLAN_BUMP;
	} else {
		if (dpb->last_output_valid && next.output_needed &&
		    next.poc <= dpb->last_output_poc)
			return -EBADMSG;
		if (hweight32(next.retain_mask | output_mask) >=
		    dpb->max_dec_pic_buffering)
			next.kind = S7D_HEVC_PLAN_BUMP;
	}
	if (next.kind == S7D_HEVC_PLAN_BUMP) {
		if (!output_mask)
			return -EUCLEAN;
		*plan = next;
		return 0;
	}
	available = queued_slot_mask & ~valid_mask;
	if (!available)
		return -EAGAIN;
	next.current_slot = __ffs(available);
	*plan = next;
	return 0;
}

static int hevc_plan_check(const struct s7d_hevc_dpb *dpb,
			  const struct s7d_hevc_picture_plan *plan)
{
	int ret = hevc_state_check(dpb);

	if (ret)
		return ret;
	if (!plan || plan->generation != dpb->generation ||
	    plan->parameter_epoch != dpb->parameter_epoch ||
	    plan->base_revision != dpb->revision ||
	    dpb->revision == U64_MAX || plan->plan_id != dpb->revision + 1 ||
	    plan->slot_count != dpb->slot_count)
		return -ESTALE;
	if (dpb->active || dpb->retiring_mask || dpb->pending_reset_generation)
		return -EBUSY;
	return 0;
}

int s7d_hevc_dpb_begin(struct s7d_hevc_dpb *dpb,
		       const struct s7d_hevc_picture_plan *plan, u64 write_token)
{
	struct s7d_hevc_dpb_picture *pic;
	u32 mask, i, list;
	int ret = hevc_plan_check(dpb, plan);

	if (ret)
		return ret;
	if (plan->kind != S7D_HEVC_PLAN_PICTURE || !write_token ||
	    !plan->metadata.input_token || plan->current_slot >= dpb->slot_count ||
	    plan->slice_type > 2 || dpb->picture[plan->current_slot].valid)
		return -EINVAL;
	mask = hevc_valid_mask(dpb);
	if ((plan->retain_mask | plan->drop_output_mask) & ~mask)
		return -ESTALE;
	for (list = 0; list < 2; list++) {
		if (plan->list_count[list] > S7D_HEVC_RPS_ENTRIES)
			return -EINVAL;
		for (i = 0; i < plan->list_count[list]; i++) {
			const struct s7d_hevc_picture_ref *ref = &plan->list[list][i];

			if (ref->slot >= dpb->slot_count ||
			    !(plan->retain_mask & BIT(ref->slot)) ||
			    !dpb->picture[ref->slot].decoded ||
			    !dpb->picture[ref->slot].reference ||
			    dpb->picture[ref->slot].poc != ref->poc)
				return -ESTALE;
		}
	}
	pic = &dpb->picture[plan->current_slot];
	pic->metadata = plan->metadata;
	pic->write_token = write_token;
	pic->slice_type = plan->slice_type;
	pic->poc = plan->poc;
	pic->valid = true;
	dpb->active_plan = *plan;
	dpb->active_write_token = write_token;
	dpb->active = true;
	dpb->revision = plan->plan_id;
	return 0;
}

int s7d_hevc_dpb_skip_committed(struct s7d_hevc_dpb *dpb,
				const struct s7d_hevc_picture_plan *plan)
{
	int ret = hevc_plan_check(dpb, plan);

	if (ret)
		return ret;
	if (plan->kind != S7D_HEVC_PLAN_SKIP_RASL ||
	    plan->current_slot != S7D_HEVC_NO_SLOT ||
	    !dpb->poc.irap_valid || !dpb->poc.no_rasl_output ||
	    plan->poc >= dpb->poc.irap_poc ||
	    !hevc_poc_equal(&plan->next_poc, &dpb->poc))
		return -EINVAL;
	dpb->revision = plan->plan_id;
	dpb->last_input_token = plan->metadata.input_token;
	return 0;
}

static int hevc_completion_check(const struct s7d_hevc_dpb *dpb, u64 plan_id,
				 u32 slot, u64 write_token, bool physical_idle)
{
	int ret = hevc_state_check(dpb);

	if (ret)
		return ret;
	if (!dpb->active || plan_id != dpb->active_plan.plan_id ||
	    slot != dpb->active_plan.current_slot ||
	    !write_token || write_token != dpb->active_write_token)
		return -ESTALE;
	if (!physical_idle)
		return -EBUSY;
	if (dpb->revision == U64_MAX)
		return -EOVERFLOW;
	return 0;
}

static void hevc_actions_make(const struct s7d_hevc_dpb *dpb, u32 mask,
			      struct s7d_hevc_dpb_actions *actions)
{
	u32 i;

	memset(actions, 0, sizeof(*actions));
	actions->generation = dpb->generation;
	actions->revision = dpb->revision + 1;
	actions->mv_release_mask = mask;
	for (i = 0; i < dpb->slot_count; i++) {
		if (!(mask & BIT(i)))
			continue;
		if (dpb->picture[i].published)
			actions->surface_release_mask |= BIT(i);
		else {
			actions->discard_mask |= BIT(i);
			actions->discard_write_token[i] = dpb->picture[i].write_token;
		}
	}
}

static void hevc_actions_commit(struct s7d_hevc_dpb *dpb,
				const struct s7d_hevc_dpb_actions *actions)
{
	dpb->revision = actions->revision;
	dpb->retiring_mask = actions->mv_release_mask;
	dpb->pending_actions = *actions;
}

int s7d_hevc_dpb_complete(struct s7d_hevc_dpb *dpb, u64 plan_id,
			  u32 slot, u64 write_token, bool picture_done,
			  bool physical_idle, struct s7d_hevc_dpb_actions *actions)
{
	struct s7d_hevc_dpb_actions next;
	const struct s7d_hevc_picture_plan *plan;
	u32 i, retire = 0;
	int ret;

	if (!actions)
		return -EINVAL;
	ret = hevc_completion_check(dpb, plan_id, slot, write_token, physical_idle);
	if (ret)
		return ret;
	if (!picture_done)
		return -EAGAIN;
	plan = &dpb->active_plan;
	for (i = 0; i < dpb->slot_count; i++) {
		const struct s7d_hevc_dpb_picture *pic = &dpb->picture[i];
		bool output = pic->output_needed && !(plan->drop_output_mask & BIT(i));

		if (!pic->valid || i == slot)
			continue;
		if (plan->output_needed && output && pic->latency == U32_MAX)
			return -EOVERFLOW;
		if (!(plan->retain_mask & BIT(i)) && !output)
			retire |= BIT(i);
	}
	if (!plan->reference && !plan->output_needed)
		retire |= BIT(slot);
	hevc_actions_make(dpb, retire, &next);
	for (i = 0; i < dpb->slot_count; i++) {
		struct s7d_hevc_dpb_picture *pic = &dpb->picture[i];

		if (!pic->valid || i == slot)
			continue;
		pic->reference = plan->retain_mask & BIT(i);
		if (plan->drop_output_mask & BIT(i))
			pic->output_needed = false;
		if (plan->output_needed && pic->output_needed)
			pic->latency++;
	}
	dpb->picture[slot].decoded = true;
	dpb->picture[slot].reference = plan->reference;
	dpb->picture[slot].output_needed = plan->output_needed;
	dpb->poc = plan->next_poc;
	dpb->last_input_token = plan->metadata.input_token;
	if (plan->clear_dpb)
		dpb->last_output_valid = false;
	dpb->active = false;
	dpb->active_write_token = 0;
	hevc_actions_commit(dpb, &next);
	*actions = next;
	return 0;
}

int s7d_hevc_dpb_peek_output(const struct s7d_hevc_dpb *dpb, bool drain,
			     struct s7d_hevc_output_plan *out)
{
	struct s7d_hevc_output_plan next = { };
	u32 i, pending = 0;
	bool needed = drain;
	int slot = -1;
	int ret = hevc_state_check(dpb);

	if (ret)
		return ret;
	if (!out)
		return -EINVAL;
	if (dpb->active || dpb->retiring_mask || dpb->pending_reset_generation)
		return -EBUSY;
	if (dpb->revision > U64_MAX - 2)
		return -EOVERFLOW;
	for (i = 0; i < dpb->slot_count; i++) {
		const struct s7d_hevc_dpb_picture *pic = &dpb->picture[i];

		if (!pic->valid || !pic->decoded || !pic->output_needed)
			continue;
		pending++;
		if (slot < 0 || pic->poc < dpb->picture[slot].poc)
			slot = i;
		if (dpb->max_latency_pictures && pic->latency >= dpb->max_latency_pictures)
			needed = true;
	}
	if (slot < 0 || (!needed && pending <= dpb->num_reorder_pics))
		return -EAGAIN;
	if (dpb->last_output_valid && dpb->picture[slot].poc <= dpb->last_output_poc)
		return -EBADMSG;
	next.generation = dpb->generation;
	next.revision = dpb->revision;
	next.slot = slot;
	next.write_token = dpb->picture[slot].write_token;
	next.poc = dpb->picture[slot].poc;
	next.metadata = dpb->picture[slot].metadata;
	next.flags = dpb->picture[slot].slice_type == 2 ? S7D_CODEC_FRAME_KEY :
		dpb->picture[slot].slice_type == 1 ? S7D_CODEC_FRAME_P : S7D_CODEC_FRAME_B;
	*out = next;
	return 0;
}

int s7d_hevc_dpb_output_committed(struct s7d_hevc_dpb *dpb,
				  const struct s7d_hevc_output_plan *out,
				  struct s7d_hevc_dpb_actions *actions)
{
	struct s7d_hevc_dpb_actions next;
	const struct s7d_hevc_dpb_picture *pic;
	u32 mask;
	int ret = hevc_state_check(dpb);

	if (ret)
		return ret;
	if (!out || !actions)
		return -EINVAL;
	if (dpb->active || dpb->retiring_mask || dpb->pending_reset_generation)
		return -EBUSY;
	if (out->generation != dpb->generation || out->revision != dpb->revision ||
	    out->slot >= dpb->slot_count)
		return -ESTALE;
	if (dpb->revision > U64_MAX - 2)
		return -EOVERFLOW;
	pic = &dpb->picture[out->slot];
	if (!pic->valid || !pic->decoded || !pic->output_needed ||
	    pic->poc != out->poc || pic->write_token != out->write_token ||
	    !hevc_metadata_equal(&pic->metadata, &out->metadata))
		return -ESTALE;
	mask = pic->reference ? 0 : BIT(out->slot);
	hevc_actions_make(dpb, mask, &next);
	next.surface_release_mask = mask;
	next.discard_mask = 0;
	memset(next.discard_write_token, 0, sizeof(next.discard_write_token));
	dpb->picture[out->slot].output_needed = false;
	dpb->picture[out->slot].published = true;
	dpb->last_output_poc = out->poc;
	dpb->last_output_valid = true;
	hevc_actions_commit(dpb, &next);
	*actions = next;
	return 0;
}

int s7d_hevc_dpb_abort(struct s7d_hevc_dpb *dpb, u64 plan_id,
		       u32 slot, u64 write_token, bool physical_idle,
		       struct s7d_hevc_dpb_actions *actions)
{
	struct s7d_hevc_dpb_actions next;
	int ret;

	if (!actions)
		return -EINVAL;
	ret = hevc_completion_check(dpb, plan_id, slot, write_token, physical_idle);
	if (ret)
		return ret;
	hevc_actions_make(dpb, BIT(slot), &next);
	dpb->active = false;
	dpb->active_write_token = 0;
	hevc_actions_commit(dpb, &next);
	*actions = next;
	return 0;
}

static void hevc_reset_poc(struct s7d_hevc_dpb *dpb, u64 generation)
{
	dpb->generation = generation;
	dpb->pending_reset_generation = 0;
	dpb->last_output_valid = false;
	dpb->last_input_token = 0;
	memset(&dpb->poc, 0, sizeof(dpb->poc));
	memset(&dpb->active_plan, 0, sizeof(dpb->active_plan));
	memset(&dpb->pending_actions, 0, sizeof(dpb->pending_actions));
}

int s7d_hevc_dpb_reset(struct s7d_hevc_dpb *dpb, u64 new_generation,
		       bool physical_idle, struct s7d_hevc_dpb_actions *actions)
{
	struct s7d_hevc_dpb_actions next;
	u32 mask, i;
	int ret = hevc_state_check(dpb);

	if (ret)
		return ret;
	if (!actions || new_generation <= dpb->generation)
		return -EINVAL;
	if (!physical_idle || dpb->retiring_mask || dpb->pending_reset_generation)
		return -EBUSY;
	if (dpb->revision > U64_MAX - 2)
		return -EOVERFLOW;
	mask = hevc_valid_mask(dpb);
	hevc_actions_make(dpb, mask, &next);
	for (i = 0; i < dpb->slot_count; i++) {
		dpb->picture[i].reference = false;
		dpb->picture[i].output_needed = false;
	}
	dpb->active = false;
	dpb->active_write_token = 0;
	dpb->pending_reset_generation = new_generation;
	hevc_actions_commit(dpb, &next);
	*actions = next;
	return 0;
}

int s7d_hevc_dpb_release_committed(struct s7d_hevc_dpb *dpb,
				   const struct s7d_hevc_dpb_actions *actions)
{
	u32 i;
	int ret = hevc_state_check(dpb);

	if (ret)
		return ret;
	if (!actions ||
	    !hevc_actions_equal(actions, &dpb->pending_actions))
		return -ESTALE;
	if (dpb->revision == U64_MAX)
		return -EOVERFLOW;
	if (!dpb->retiring_mask && !dpb->pending_reset_generation)
		return 0;
	for (i = 0; i < dpb->slot_count; i++)
		if (dpb->retiring_mask & BIT(i))
			memset(&dpb->picture[i], 0, sizeof(dpb->picture[i]));
	dpb->retiring_mask = 0;
	memset(&dpb->pending_actions, 0, sizeof(dpb->pending_actions));
	dpb->revision++;
	if (dpb->pending_reset_generation)
		hevc_reset_poc(dpb, dpb->pending_reset_generation);
	return 0;
}
